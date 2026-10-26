/* Host sampling profiler (RT_HOSTPROF=file): every 1 ms of the guest thread's CPU time records the
 * interrupted instruction pointer; the raw addresses (plus the library map) are written out at the
 * RT_DUMP_AFTER dump (or on demand) and resolved offline with tools/hostprof.py. */
#define _GNU_SOURCE
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <ucontext.h>
#include "runtime.h"
#include "gpu.h"

#define MAXS (1 << 20)
static unsigned long *samples, *callers;   /* callers: word at the stack top (a leaf's return address) */
static volatile int nsamples;

static void on_prof(int sig, siginfo_t *si, void *ctx) {
    (void)sig; (void)si;
    ucontext_t *uc = ctx;
    static long from = -1;
    if (from < 0) from = ENV("RT_HOSTPROF_FROM") ? atol(ENV("RT_HOSTPROF_FROM")) : 0;
    if ((long)g_gpu.frames < from) return;
    static long min_draws = -1;   /* RT_HOSTPROF_DRAWS=n: only sample while the previous frame had >= n draws */
    if (min_draws < 0) min_draws = ENV("RT_HOSTPROF_DRAWS") ? atol(ENV("RT_HOSTPROF_DRAWS")) : 0;
    if ((long)g_gpu.last_frame_draws < min_draws) return;
    if (nsamples < MAXS) {
        callers[nsamples] = *(unsigned long *)uc->uc_mcontext.gregs[REG_RSP];
        samples[nsamples++] = (unsigned long)uc->uc_mcontext.gregs[REG_RIP];
    }
}

void hostprof_dump(void);

void hostprof_start(void) {
    if (!ENV("RT_HOSTPROF")) return;
    samples = calloc(MAXS, sizeof *samples);
    callers = calloc(MAXS, sizeof *callers);
    struct sigaction sa = {0};
    sa.sa_sigaction = on_prof;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigaction(SIGPROF, &sa, NULL);
    /* the calling (guest) thread's own CPU clock, signalled to it alone: GL driver and timer threads
     * would otherwise take samples too */
    struct sigevent ev = {0};
    ev.sigev_notify = SIGEV_THREAD_ID;
    ev.sigev_signo = SIGPROF;
    ev._sigev_un._tid = (pid_t)syscall(SYS_gettid);
    timer_t t;
    if (timer_create(CLOCK_THREAD_CPUTIME_ID, &ev, &t) == 0) {
        struct itimerspec it = {{0, 1000000}, {0, 1000000}};
        timer_settime(t, 0, &it, NULL);
    }
}

extern char __executable_start;
void hostprof_dump(void) {
    const char *p = ENV("RT_HOSTPROF");
    if (!p || !samples) return;
    static int done;
    if (done++) return;
    FILE *f = fopen(p, "w");
    if (!f) return;
    fprintf(f, "base %lx\n", (unsigned long)&__executable_start);
    for (int i = 0; i < nsamples; i++) fprintf(f, "%lx %lx\n", samples[i], callers[i]);
    FILE *m = fopen("/proc/self/maps", "r");   /* for attributing library samples */
    char line[512];
    while (m && fgets(line, sizeof line, m))
        if (strstr(line, " r-xp ") || strstr(line, " r-xs ")) fprintf(f, "map %s", line);
    if (m) fclose(m);
    fclose(f);
}
