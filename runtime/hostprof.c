/* Host sampling profiler (RT_HOSTPROF=file): SIGPROF every 1 ms of CPU time records the interrupted
 * instruction pointer; the raw addresses are written out at the RT_DUMP_AFTER dump (or on demand)
 * and resolved offline with tools/hostprof.py (nm). */
#define _GNU_SOURCE
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <ucontext.h>
#include "runtime.h"
#include "gpu.h"

#define MAXS (1 << 20)
static unsigned long *samples;
static volatile int nsamples;

static void on_prof(int sig, siginfo_t *si, void *ctx) {
    (void)sig; (void)si;
    ucontext_t *uc = ctx;
    static long from = -1;
    if (from < 0) from = getenv("RT_HOSTPROF_FROM") ? atol(getenv("RT_HOSTPROF_FROM")) : 0;
    if ((long)g_gpu.frames < from) return;
    if (nsamples < MAXS) samples[nsamples++] = (unsigned long)uc->uc_mcontext.gregs[REG_RIP];
}

void hostprof_start(void) {
    if (!getenv("RT_HOSTPROF")) return;
    samples = calloc(MAXS, sizeof *samples);
    struct sigaction sa = {0};
    sa.sa_sigaction = on_prof;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigaction(SIGPROF, &sa, NULL);
    struct itimerval it = {{0, 1000}, {0, 1000}};
    setitimer(ITIMER_PROF, &it, NULL);
}

extern char __executable_start;
void hostprof_dump(void) {
    const char *p = getenv("RT_HOSTPROF");
    if (!p || !samples) return;
    FILE *f = fopen(p, "w");
    if (!f) return;
    fprintf(f, "base %lx\n", (unsigned long)&__executable_start);
    for (int i = 0; i < nsamples; i++) fprintf(f, "%lx\n", samples[i]);
    fclose(f);
}
