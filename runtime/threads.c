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
    memset(c, 0, sizeof *c);
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
    g_cpu = o->cpu;   /* resumed */
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
        wpad_tick(c);
        any = 1;
    }
    mw32(0x800000D4u, irq_ctx);
    in_irq = 0;
    *c = save;
    return any;
}

static void wait_next_event(void) {
    u64 now = now_tb(), next = vi_next;
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
static void dump_threads(void) {
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

static void *timer_thread(void *arg) {
    (void)arg;
    const char *d = getenv("RT_DUMP_AFTER");
    long dump_ms = d ? atol(d) * 1000 : -1, ms = 0;
    for (;;) {
        struct timespec ts = {0, 1000000};   /* 1 ms */
        nanosleep(&ts, NULL);
        g_irq_pending = 1;
        if (++ms == dump_ms)
            dump_threads();
    }
    return NULL;
}

void rt_timer_start(void) {
    if (g_deterministic) {
        g_irq_pending = 1;   /* every back-edge advances virtual time and checks for interrupts */
        return;
    }
    if (getenv("RT_NO_ASYNC"))
        return;   /* debugging: interrupts only at OS safe points */
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
    if (!(now >= vi_next || (dec_armed && now >= dec_deadline) || g_gpu.irq_finish || g_gpu.irq_token))
        return;
    CPU save = *c;
    if (poll_irqs(c))
        FN___OSReschedule(c);
    *c = save;
}
