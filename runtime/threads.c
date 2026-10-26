/* Guest threads and interrupts.
 *
 * The SDK's scheduler (thread queues, priorities, OSSleepThread/OSWakeupThread, SelectThread) runs
 * as recompiled code. Only the context switch is ours: each guest thread context is a host
 * coroutine with its own host stack and saved register file. OSSaveContext binds the running
 * coroutine to a context; OSLoadContext switches to the coroutine bound to the target context
 * (starting a new one from the OSContext in guest memory for a thread that never ran).
 *
 * Interrupts are delivered at safe points - when the guest enables interrupts and while the
 * scheduler idles - by calling the SDK's own handlers (decrementer -> alarms, VI retrace). */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <pthread.h>
#include <signal.h>
#include <ucontext.h>
#include "runtime.h"
#include "names.h"
#include "gpu.h"

#define HLE(name) void FN_##name(CPU *c)

typedef struct Co {
    u32 ctx;          /* guest OSContext this coroutine runs */
    int dead;
    ucontext_t uc;
    CPU cpu;
    void *stack;
} Co;

#define MAX_CO 256
#define CO_STACK (8u << 20)
static Co main_co, *coro[MAX_CO], *cur = &main_co;
static int nco;

static Co *find(u32 ctx) {
    if (main_co.ctx == ctx && !main_co.dead)
        return &main_co;
    for (int i = 0; i < nco; i++)
        if (coro[i]->ctx == ctx && !coro[i]->dead)
            return coro[i];
    return NULL;
}

/* entry of a thread that never ran: registers from its OSContext, then run to OSExitThread */
static void co_entry(void) {
    CPU *c = &g_cpu;
    u32 x = cur->ctx;
    static u32 spr[1024];
    memcpy(spr, c->spr, sizeof spr);   /* special registers are the processor's, not the thread's */
    memset(c, 0, sizeof *c);
    memcpy(c->spr, spr, sizeof spr);
    for (int i = 0; i < 32; i++) {
        c->r[i] = mr32(x + 4 * i);
        c->f[i] = lfd(x + 0x90 + 8 * i);
        c->ps1[i] = lfd(x + 0x1C8 + 8 * i);
    }
    c->cr = mr32(x + 0x80);
    c->lr = mr32(x + 0x84);
    c->ctr = mr32(x + 0x88);
    c->fpscr = mr32(x + 0x194);
    for (int i = 0; i < 8; i++)
        c->gqr[i] = mr32(x + 0x1A4 + 4 * i);
    c->msr = 0x0000B032;   /* EE on, as after rfi into a new thread */
    u32 entry = mr32(x + 0x198), ret = c->lr;
    if (ENV("RT_THREADLOG"))
        fprintf(stderr, "thread %08X starts at %08X (%s) r3=%08X\n", x, entry, fn_name(entry), c->r[3]);
    call_indirect(c, entry);
    call_indirect(c, ret);   /* returning from the thread function enters OSExitThread */
    fprintf(stderr, "thread %08X: OSExitThread returned\n", x);
    abort();
}

static void switch_to(Co *n) {
    Co *o = cur;
    o->cpu = g_cpu;
    /* a thread that is exiting never runs again: its coroutine can be reused */
    if (o != &main_co && o->ctx && (mr16(o->ctx + 0x2C8) == 8 || mr16(o->ctx + 0x2C8) == 0))
        o->dead = 1;
    cur = n;
    if (swapcontext(&o->uc, &n->uc)) {
        perror("swapcontext");
        abort();
    }
    static u32 spr[1024];
    memcpy(spr, g_cpu.spr, sizeof spr);   /* SPRs (HID2, DMA, ...) belong to the processor */
    g_cpu = o->cpu;   /* resumed */
    memcpy(g_cpu.spr, spr, sizeof spr);
}

static Co *new_co(u32 ctx) {
    Co *n = NULL;
    for (int i = 0; i < nco; i++)
        if (coro[i]->dead && coro[i] != cur) { n = coro[i]; break; }
    if (!n) {
        if (nco == MAX_CO) { fprintf(stderr, "too many guest threads\n"); abort(); }
        n = calloc(1, sizeof *n);
        n->stack = malloc(CO_STACK);
        coro[nco++] = n;
    }
    n->ctx = ctx;
    n->dead = 0;
    getcontext(&n->uc);
    n->uc.uc_stack.ss_sp = n->stack;
    n->uc.uc_stack.ss_size = CO_STACK;
    n->uc.uc_link = NULL;
    makecontext(&n->uc, co_entry, 0);
    return n;
}

extern u64 g_virtual_tb;
extern int g_deterministic;
static int in_irq;

/* return from an interrupt: the interrupted register file comes back, but special registers are
 * processor state that handlers (or threads run meanwhile) may have changed */
static void restore_regs(CPU *c, const CPU *saved) {
    static u32 spr[1024];
    memcpy(spr, c->spr, sizeof spr);
    *c = *saved;
    memcpy(c->spr, spr, sizeof spr);
}
#define IRQ_CONTEXT 0x93F00000u   /* guest scratch OSContext for interrupt handling (IOS-reserved MEM2) */
static u32 irq_ctx;

HLE(OSSaveContext) {
    u32 ctx = c->r[3];
    Co *o = find(ctx);
    if (o && o != cur)
        o->dead = 1;
    cur->ctx = ctx;
    c->r[3] = 0;
}

HLE(OSLoadContext) {
    u32 ctx = c->r[3];
    /* loading a context leaves it "not saved by an exception" (OS_CONTEXT_STATE_EXC cleared) */
    mw16(ctx + 0x1A2, mr16(ctx + 0x1A2) & ~2u);
    if (in_irq && ctx == irq_ctx)
        return;   /* return from an interrupt handler to the interrupted thread */
    if (ctx == cur->ctx)
        return;
    if (in_irq) {
        fprintf(stderr, "OSLoadContext(%08X) inside an interrupt handler (lr %08X %s)\n", ctx, c->lr, fn_name(c->lr));
        rt_backtrace();
        abort();
    }
    Co *n = find(ctx);
    switch_to(n ? n : new_co(ctx));
}

/* ---------------------------------------------------------------- interrupts */

static u64 now_tb(void) {
    u32 lo = ppc_mftb(NULL, 0);
    return ((u64)ppc_mftb(NULL, 1) << 32) | lo;
}

static u64 dec_deadline;
static int dec_armed;
static u64 vi_next;
#define VI_PERIOD ((u64)TB_CLOCK * 1001 / 60000)

void rt_set_dec(u32 v) {
    dec_armed = (s32)v >= 0;
    dec_deadline = now_tb() + (s32)v;
}
u32 rt_get_dec(void) { return (u32)(dec_deadline - now_tb()); }

void FN_DecrementerExceptionHandler(CPU *c);
void FN___VIRetraceHandler(CPU *c);
void FN___OSReschedule(CPU *c);
void FN_GXFinishInterruptHandler(CPU *c);
void FN___AIDHandler(CPU *c);
extern int g_ax_started;
static u64 ax_next;
u64 ai_block_ticks(void);
void ai_block_done(void);
void FN_GXTokenInterruptHandler(CPU *c);

static int poll_irqs(CPU *c) {
    if (in_irq)
        return 0;
    u64 now = now_tb();
    int any = 0;
    if (!vi_next)
        vi_next = now + VI_PERIOD;
    /* handlers end in OSLoadContext, which on hardware restores every register (the stack pointer
     * included): the interrupted register file is restored here instead */
    static CPU save;
    save = *c;
    in_irq = 1;
    irq_ctx = mr32(0x800000D4u);   /* OSCurrentContext */
    /* like an exception: while handlers run the current context is not a thread's, so the
     * scheduler defers any switch they cause until we reschedule afterwards */
    mw32(0x800000D4u, IRQ_CONTEXT);
    if (dec_armed && now >= dec_deadline) {
        dec_armed = 0;
        c->r[3] = 8;   /* __OS_EXCEPTION_DECREMENTER */
        c->r[4] = irq_ctx;
        mw32(0x800000D4u, IRQ_CONTEXT);
        FN_DecrementerExceptionHandler(c);
        any = 1;
    }
    if (g_gpu.irq_token) {
        g_gpu.irq_token = 0;
        mw16(0xCC00100Au, mr16(0xCC00100Au) | 4);
        c->r[3] = 18; c->r[4] = irq_ctx;   /* __OS_INTERRUPT_PI_PE_TOKEN */
        mw32(0x800000D4u, IRQ_CONTEXT);
        FN_GXTokenInterruptHandler(c);
        any = 1;
    }
    if (g_gpu.irq_finish) {
        g_gpu.irq_finish = 0;
        mw16(0xCC00100Au, mr16(0xCC00100Au) | 8);
        c->r[3] = 19; c->r[4] = irq_ctx;   /* __OS_INTERRUPT_PI_PE_FINISH */
        mw32(0x800000D4u, IRQ_CONTEXT);
        FN_GXFinishInterruptHandler(c);
        any = 1;
    }
    if (g_ax_started && now >= ax_next) {   /* AI DMA block finished: the DSP-AI interrupt */
        u64 period = ai_block_ticks();
        if (!ax_next || now - ax_next > 8 * period)
            ax_next = now;
        ax_next += period;
        ai_block_done();   /* plays the block that just finished and latches the next */
        mw16(0xCC00500Au, mr16(0xCC00500Au) | 0x8);   /* AIDINT */
        mw32(0x800000D4u, IRQ_CONTEXT);
        c->r[3] = 5; c->r[4] = irq_ctx;   /* __OS_INTERRUPT_DSP_AI */
        FN___AIDHandler(c);
        any = 1;
    }
    mw32(0x800000D4u, IRQ_CONTEXT);
    if (dvd_deliver(c))   /* drive interrupts */
        any = 1;
    mw32(0x800000D4u, IRQ_CONTEXT);
    if (ios_deliver(c))   /* IPC replies */
        any = 1;
    if (now >= vi_next) {
        vi_next += VI_PERIOD;
        if (vi_next < now)
            vi_next = now + VI_PERIOD;
        mw16(0xCC002030u, mr16(0xCC002030u) | 0x8000);   /* display interrupt 0 fired */
        c->r[3] = 24;  /* __OS_INTERRUPT_PI_VI */
        c->r[4] = irq_ctx;
        mw32(0x800000D4u, IRQ_CONTEXT);
        FN___VIRetraceHandler(c);
        mw32(0x800000D4u, IRQ_CONTEXT);
        window_poll();   /* keep the window responsive even while the game presents no frames */
        wpad_tick(c);
        any = 1;
    }
    mw32(0x800000D4u, irq_ctx);
    if (irq_ctx) mw16(irq_ctx + 0x1A2, mr16(irq_ctx + 0x1A2) & ~2u);   /* as the exception return would */
    in_irq = 0;
    restore_regs(c, &save);
    return any;
}

u64 dvd_next_due(void);
static void wait_next_event(void) {
    u64 now = now_tb(), next = vi_next;
    if (dvd_next_due() < next)
        next = dvd_next_due();
    if (g_ax_started && ax_next < next)
        next = ax_next;
    if (dec_armed && dec_deadline < next)
        next = dec_deadline;
    if (g_deterministic) {
        if (next > now)
            g_virtual_tb += next - now;
        return;
    }
    if (next > now) {
        u64 ns = (next - now) * 1000000000ull / TB_CLOCK;
        struct timespec ts = {(time_t)(ns / 1000000000ull), (long)(ns % 1000000000ull)};
        nanosleep(&ts, NULL);
    }
}

/* safe point: interrupts were just enabled */
static void irq_point(CPU *c) {
    if (in_irq)
        return;   /* handlers re-enable interrupts: nothing nests */
    u32 r3 = c->r[3];
    if (mr32(0x800000E4u) == 0) {
        /* scheduler idle loop (no current thread): wait for an interrupt to make a thread ready */
        while (mr32(DATA_RunQueueBits) == 0) {
            if (!poll_irqs(c))
                wait_next_event();
        }
    } else if (poll_irqs(c)) {
        FN___OSReschedule(c);   /* a woken thread may preempt this one */
    }
    c->r[3] = r3;
}

HLE(OSDisableInterrupts) {
    c->r[3] = (c->msr >> 15) & 1;
    c->msr &= ~0x8000u;
}
HLE(OSEnableInterrupts) {
    u32 old = (c->msr >> 15) & 1;
    c->msr |= 0x8000u;
    irq_point(c);
    c->r[3] = old;
}
HLE(OSRestoreInterrupts) {
    u32 lvl = c->r[3], old = (c->msr >> 15) & 1;
    if (lvl) {
        c->msr |= 0x8000u;
        if (!old)
            irq_point(c);
    } else
        c->msr &= ~0x8000u;
    c->r[3] = old;
}

/* ---------------------------------------------------------------- asynchronous interrupts */

volatile int g_irq_pending;

/* debug: every guest thread with its state and guest call stack (RT_DUMP_AFTER=<seconds>) */
static void peek(void) {
    const char *e = ENV("RT_PEEK");   /* addr:len[,addr:len...] */
    while (e && *e) {
        u32 a = (u32)strtoul(e, (char **)&e, 16), n = 64;
        if (*e == ':') n = (u32)strtoul(e + 1, (char **)&e, 16);
        if (*e == ',') e++;
        for (u32 i = 0; i < n; i += 16) {
            fprintf(stderr, "%08X:", a + i);
            for (u32 k = 0; k < 16 && i + k < n; k += 4) fprintf(stderr, " %08X", mr32(a + i + k));
            fprintf(stderr, "\n");
        }
    }
}

void ax_dump(void);
void coverage_dump(void);
void hostprof_dump(void);
static void dump_threads(void) {
    coverage_dump();
    hostprof_dump();
    peek();
    ax_dump();
    static const char *st[] = {"?", "READY", "RUNNING", "?", "WAITING", "?", "?", "?", "MORIBUND"};
    fprintf(stderr, "---- guest threads (current %08X)\n", mr32(0x800000E4u));
    for (u32 t = mr32(0x800000DCu), n = 0; t && n < 64; t = mr32(t + 0x2FC), n++) {
        u16 s = mr16(t + 0x2C8);
        fprintf(stderr, "thread %08X %-8s prio %2d queue %08X\n", t, s < 9 ? st[s] : "?", (s32)mr32(t + 0x2D0), mr32(t + 0x2DC));
        Co *co = find(t);
        u32 sp = co ? (co == cur ? g_cpu.r[1] : co->cpu.r[1]) : mr32(t + 4);
        for (int k = 0; k < 10 && sp >= 0x80000000u && sp < 0x94000000u; k++) {
            u32 back = mr32(sp);
            if (!back) break;
            u32 lr = mr32(back + 4);
            fprintf(stderr, "    %08X %s\n", lr, fn_name(lr));
            sp = back;
        }
    }
}

extern volatile int g_prof_on;
void prof_dump(void);

/* kill -USR1 <pid>: thread dump from the timer thread (works on a live, detached run) */
static volatile int dump_requested;
static void on_usr1(int sig) { (void)sig; dump_requested = 1; }

static void *timer_thread(void *arg) {
    (void)arg;
    const char *d = ENV("RT_DUMP_AFTER"), *pr = ENV("RT_PROFILE");
    long dump_ms = d ? atol(d) * 1000 : -1, ms = 0, p0 = -1, p1 = -1;
    if (pr) { p0 = atol(pr) * 1000; const char *dash = strchr(pr, '-'); p1 = dash ? atol(dash + 1) * 1000 : p0 + 1000; }
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int dumped = 0, pstate = 0;
    for (;;) {
        struct timespec ts = {0, 1000000};   /* 1 ms */
        nanosleep(&ts, NULL);
        g_irq_pending = 1;
        struct timespec t;
        clock_gettime(CLOCK_MONOTONIC, &t);
        ms = (t.tv_sec - t0.tv_sec) * 1000 + (t.tv_nsec - t0.tv_nsec) / 1000000;
        if (dump_ms >= 0 && ms >= dump_ms && !dumped) { dumped = 1; dump_threads(); }
        if (dump_requested) { dump_requested = 0; dump_threads(); rt_backtrace(); }
        if (ENV("RT_THREADPROF")) {   /* which guest thread holds the CPU, sampled every ms */
            static u32 tid[64], cnt[64];
            static int nt, total;
            u32 t = mr32(0x800000E4u);
            int i;
            for (i = 0; i < nt && tid[i] != t; i++) ;
            if (i == nt && nt < 64) tid[nt++] = t;
            if (i < 64) cnt[i]++;
            if (++total % 10000 == 0) {
                fprintf(stderr, "threadprof:");
                for (int k = 0; k < nt; k++) { fprintf(stderr, " %08X:%d%%", tid[k], cnt[k] * 100 / 10000); cnt[k] = 0; }
                fprintf(stderr, " (frame %llu)\n", (unsigned long long)g_gpu.frames);
            }
        }
        if (p0 >= 0 && ms >= p0 && pstate == 0) { pstate = 1; g_prof_on = 1; }
        if (p1 >= 0 && ms >= p1 && pstate == 1) { pstate = 2; g_prof_on = 0; prof_dump(); }
    }
    return NULL;
}

void rt_timer_start(void) {
    if (g_deterministic) {
        g_irq_pending = 1;   /* every back-edge advances virtual time and checks for interrupts */
        return;
    }
    if (ENV("RT_NO_ASYNC"))
        return;   /* debugging: interrupts only at OS safe points */
    signal(SIGUSR1, on_usr1);
    pthread_t t;
    pthread_create(&t, NULL, timer_thread, NULL);
}

/* at a loop back-edge with an interrupt possibly due: deliver it as an exception would, with the
 * interrupted code's registers fully preserved */
void rt_async_irq(CPU *c) {
    if (g_deterministic)
        g_virtual_tb += 16;
    else
        g_irq_pending = 0;
    if (in_irq || !(c->msr & 0x8000u) || mr32(0x800000E4u) == 0)
        return;
    u64 now = now_tb();
    if (!(now >= vi_next || (dec_armed && now >= dec_deadline) || g_gpu.irq_finish || g_gpu.irq_token || ios_pending() || dvd_pending() ||
          (g_ax_started && now >= ax_next)))
        return;
    CPU save = *c;
    if (poll_irqs(c))
        FN___OSReschedule(c);
    restore_regs(c, &save);
}
