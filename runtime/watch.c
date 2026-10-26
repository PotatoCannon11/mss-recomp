/* Debug data watchpoint (RT_WATCH=addr:len:tick): from retrace <tick> on, the guest page holding
 * [addr, addr+len) is made inaccessible; every access to the watched range is recorded (host
 * instruction pointer, guest address) by faulting, unprotecting and single-stepping, then the page is
 * protected again. Results go to RT_WATCH_OUT (default watch.txt) at exit, resolved with
 * tools/hostprof.py-style nm lookup by tools/watch.py. */
#define _GNU_SOURCE
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <ucontext.h>
#include "runtime.h"

static u32 w_addr, w_len;
static u8 *w_page;
static int w_armed;
#define MAXHIT 4096
static struct { unsigned long rip; u32 addr; u32 n; } hits[MAXHIT];
static int nhits;

static void record(unsigned long rip, u32 a) {
    for (int i = 0; i < nhits; i++)
        if (hits[i].rip == rip && hits[i].addr == a) { hits[i].n++; return; }
    if (nhits < MAXHIT) hits[nhits++] = (typeof(hits[0])){rip, a, 1};
}

static void on_segv(int sig, siginfo_t *si, void *ctx) {
    ucontext_t *uc = ctx;
    u8 *p = si->si_addr;
    if (!w_page || p < w_page || p >= w_page + 4096) {   /* not ours: default action */
        signal(sig, SIG_DFL);
        return;
    }
    u32 a = (u32)(p - MEM);
    if (a >= w_addr && a < w_addr + w_len) record((unsigned long)uc->uc_mcontext.gregs[REG_RIP], a);
    mprotect(w_page, 4096, PROT_READ | PROT_WRITE);
    uc->uc_mcontext.gregs[REG_EFL] |= 0x100;   /* trap after this one instruction */
}

static void on_trap(int sig, siginfo_t *si, void *ctx) {
    (void)sig; (void)si;
    ucontext_t *uc = ctx;
    uc->uc_mcontext.gregs[REG_EFL] &= ~0x100UL;
    if (w_armed) mprotect(w_page, 4096, PROT_NONE);
}

extern char __executable_start;
void watch_dump(void) {
    if (!w_len) return;
    FILE *f = fopen(ENV("RT_WATCH_OUT") ? ENV("RT_WATCH_OUT") : "watch.txt", "w");
    if (!f) return;
    fprintf(f, "base %lx\n", (unsigned long)&__executable_start);
    for (int i = 0; i < nhits; i++) fprintf(f, "%lx %08X %u\n", hits[i].rip, hits[i].addr, hits[i].n);
    fclose(f);
}

/* called every retrace */
void watch_tick(u64 ticks) {
    static int init;
    static u64 from;
    if (!init) {
        init = 1;
        const char *e = ENV("RT_WATCH");
        if (!e) return;
        unsigned long long t = 0;
        if (sscanf(e, "%x:%x:%llu", &w_addr, &w_len, &t) < 2) return;
        from = t;
        struct sigaction sa = {0};
        sa.sa_flags = SA_SIGINFO | SA_NODEFER;
        sa.sa_sigaction = on_segv;
        sigaction(SIGSEGV, &sa, NULL);
        sa.sa_sigaction = on_trap;
        sigaction(SIGTRAP, &sa, NULL);
    }
    if (!w_len || w_armed || ticks < from) return;
    w_page = (u8 *)(((unsigned long)(MEM + w_addr)) & ~4095UL);
    w_armed = 1;
    mprotect(w_page, 4096, PROT_NONE);
    fprintf(stderr, "watch: armed %08X+%X at tick %llu\n", w_addr, w_len, (unsigned long long)ticks);
}
