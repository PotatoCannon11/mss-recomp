/* Host runtime for the recompiled game: guest memory, boot, CPU services. */
#define _GNU_SOURCE
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "cpu.h"
#include "runtime.h"
#include "gpu.h"

u8 *MEM;

extern const u32 g_fn_count, g_entry;
extern const u32 g_fn_addr[];
extern const GuestFn g_fn_ptr[];

/* ---------------------------------------------------------------- memory */

static void map_shared(int fd, u32 guest, size_t size) {
    if (mmap(MEM + guest, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0) == MAP_FAILED) {
        perror("mmap guest");
        exit(1);
    }
}

static void map_anon(u32 guest, size_t size) {
    if (mmap(MEM + guest, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == MAP_FAILED) {
        perror("mmap anon");
        exit(1);
    }
}

/* MEM1 (24 MiB) at 0x80000000 with its uncached mirror at 0xC0000000 and physical alias at 0;
 * MEM2 (64 MiB) at 0x90000000 / 0xD0000000 / 0x10000000. Hardware registers and the locked
 * cache are plain memory for now; device emulation hooks them by address. */
static void mem_init(void) {
    MEM = mmap(NULL, 1ull << 32, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (MEM == MAP_FAILED) {
        perror("reserve 4 GiB");
        exit(1);
    }
    int m1 = memfd_create("mem1", 0), m2 = memfd_create("mem2", 0);
    if (ftruncate(m1, MEM1_SIZE) || ftruncate(m2, MEM2_SIZE)) {
        perror("memfd");
        exit(1);
    }
    map_shared(m1, 0x80000000u, MEM1_SIZE);
    map_shared(m1, 0xC0000000u, MEM1_SIZE);
    map_shared(m1, 0x00000000u, MEM1_SIZE);
    map_shared(m2, 0x90000000u, MEM2_SIZE);
    map_shared(m2, 0xD0000000u, MEM2_SIZE);
    map_shared(m2, 0x10000000u, MEM2_SIZE);
    map_anon(0xCC000000u, 0x02000000);   /* Flipper-era (0xCC00xxxx) and Hollywood (0xCD00xxxx, 0xCD80xxxx) registers */
    map_anon(0xE0000000u, 0x00004000);   /* locked L1 data cache */
}

/* ---------------------------------------------------------------- boot */

static void load_dol(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        perror(path);
        exit(1);
    }
    u8 hdr[0x100];
    if (fread(hdr, 1, sizeof hdr, f) != sizeof hdr) {
        fprintf(stderr, "%s: short header\n", path);
        exit(1);
    }
#define BE32(p) ((u32)(p)[0] << 24 | (u32)(p)[1] << 16 | (u32)(p)[2] << 8 | (p)[3])
    /* the bss range spans the small-data sections too: clear it before loading them */
    u32 bss = BE32(hdr + 0xD8), bss_size = BE32(hdr + 0xDC);
    memset(MEM + bss, 0, bss_size);
    for (int i = 0; i < 18; i++) {
        u32 off = BE32(hdr + 4 * i), addr = BE32(hdr + 0x48 + 4 * i), size = BE32(hdr + 0x90 + 4 * i);
        if (!size)
            continue;
        fseek(f, off, SEEK_SET);
        if (fread(MEM + addr, 1, size, f) != size) {
            fprintf(stderr, "%s: short section %d\n", path, i);
            exit(1);
        }
    }
    fclose(f);
}

/* What the apploader / IOS leave in low memory before the game starts (values as on a retail
 * Wii with an NTSC disc, matching what Dolphin's HLE boot writes). */
static void lowmem_init(const char *game_id) {
    memcpy(MEM + 0x80000000u, game_id, 6);   /* disc ID, maker */
    mw32(0x80000018u, 0x5D1C9EA3);           /* Wii disc magic */
    mw32(0x80000020u, 0x0D15EA5E);           /* boot magic */
    mw32(0x80000024u, 1);
    mw32(0x80000028u, MEM1_SIZE);
    mw32(0x8000002Cu, 0x00000023);           /* console type: retail Wii */
    mw32(0x80000030u, 0);                    /* arena lo/hi: let OSInit use the linker's */
    mw32(0x80000034u, 0x817FEC60);
    mw32(0x800000CCu, 0);                    /* video mode NTSC */
    mw32(0x800000F0u, MEM1_SIZE);
    mw32(0x800000F8u, BUS_CLOCK);
    mw32(0x800000FCu, CPU_CLOCK);
    mw32(0x80003100u, MEM1_SIZE);
    mw32(0x80003104u, MEM1_SIZE);
    mw32(0x80003108u, 0x81800000);           /* MEM1 end */
    mw32(0x8000310Cu, 0);                    /* usable MEM1 start: 0 = use the linker arena */
    mw32(0x80003110u, 0x817FEC60);
    mw32(0x80003118u, MEM2_SIZE);
    mw32(0x8000311Cu, MEM2_SIZE);
    mw32(0x80003120u, 0x93400000);
    mw32(0x80003124u, 0x90000800);
    mw32(0x80003128u, 0x933E0000);
    mw32(0x80003130u, 0x933E0000);           /* IOS IPC buffer */
    mw32(0x80003134u, 0x93400000);
    mw32(0x80003138u, 0x00000011);           /* Hollywood revision */
    mw32(0x80003140u, 0x00240001);           /* IOS36 */
    mw32(0x80003188u, 0x00240001);
}

/* ---------------------------------------------------------------- functions */

GuestFn lookup_fn(u32 addr) {
    u32 lo = 0, hi = g_fn_count;
    while (lo < hi) {
        u32 mid = (lo + hi) / 2;
        if (g_fn_addr[mid] < addr)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo < g_fn_count && g_fn_addr[lo] == addr ? g_fn_ptr[lo] : NULL;
}

void call_indirect(CPU *c, u32 addr) {
    GuestFn f = lookup_fn(addr);
    if (!f) {
        fprintf(stderr, "call_indirect: no function at %08X (lr %08X)\n", addr, c->lr);
        rt_backtrace();
        abort();
    }
    f(c);
}

const char *fn_name(u32 addr) {
    static char *names[1 << 16];
    static int loaded;
    static u32 addrs[1 << 16];
    static int n;
    if (!loaded) {
        loaded = 1;
        FILE *f = fopen(NAMES_PATH, "r");
        char line[512];
        while (f && n < (1 << 16) && fgets(line, sizeof line, f)) {
            char nm[480] = "";
            if (sscanf(line, "%x %479s", &addrs[n], nm) >= 1) {
                names[n] = strdup(nm);
                n++;
            }
        }
        if (f)
            fclose(f);
    }
    int best = -1;
    for (int i = 0; i < n; i++)
        if (addrs[i] <= addr && (best < 0 || addrs[i] > addrs[best]))
            best = i;
    return best >= 0 ? names[best] : "?";
}

void ppc_illegal(CPU *c, u32 pc, u32 word) {
    fprintf(stderr, "illegal instruction %08X at %08X (%s)\n", word, pc, fn_name(pc));
    (void)c;
    abort();
}
void ppc_trap(CPU *c, u32 pc) {
    fprintf(stderr, "trap at %08X (%s), lr %08X\n", pc, fn_name(pc), c->lr);
    rt_backtrace();
    abort();
}
void ppc_sc(CPU *c, u32 pc) { (void)c; (void)pc; }
void ppc_rfi(CPU *c, u32 pc) { (void)c; (void)pc; }

u64 g_virtual_tb;   /* deterministic mode: time advances with guest progress */
int g_deterministic;
u32 ppc_mftb(CPU *c, int upper) {
    (void)c;
    if (g_deterministic) {
        g_virtual_tb += upper ? 0 : 64;
        return upper ? (u32)(g_virtual_tb >> 32) : (u32)g_virtual_tb;
    }
    /* the time base counts from 2000-01-01 (the Wii's epoch), so OSGetTime gives the real date */
    static u64 epoch_offset;
    if (!epoch_offset) {
        struct timespec rt, mono;
        clock_gettime(CLOCK_REALTIME, &rt);
        clock_gettime(CLOCK_MONOTONIC, &mono);
        epoch_offset = ((u64)rt.tv_sec - 946684800ull - (u64)mono.tv_sec) * TB_CLOCK;
    }
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    u64 tb = epoch_offset + (u64)ts.tv_sec * TB_CLOCK + (u64)ts.tv_nsec * TB_CLOCK / 1000000000ull;
    return upper ? (u32)(tb >> 32) : (u32)tb;
}

/* locked-cache DMA (DMAU 922 / DMAL 923): copies run immediately, so the queue is always empty */
static void lc_dma(CPU *c) {
    u32 u = c->spr[922], l = c->spr[923];
    u32 mem = u & 0xFFFFFFE0u, lc = l & 0xFFFFFFE0u;
    u32 lines = ((u & 0x1F) << 2) | ((l >> 2) & 3);
    if (!lines) lines = 128;
    if (l & 0x10) memmove(MEM + lc, MEM + mem, lines * 32);   /* LD: memory -> locked cache */
    else memmove(MEM + mem, MEM + lc, lines * 32);           /* store: locked cache -> memory */
    c->spr[923] = l & ~2u;   /* T (trigger) clears when done */
}

void ppc_mtspr(CPU *c, int spr, u32 v) {
    if (spr == 920 && ENV("RT_HID2LOG")) fprintf(stderr, "mtspr HID2 = %08X (lr %08X)\n", v, c->lr);
    c->spr[spr] = v;
    if (spr == 22)
        rt_set_dec(v);
    else if (spr == 923 && (v & 2))
        lc_dma(c);
}
u32 ppc_mfspr(CPU *c, int spr) {
    if (spr == 920 && ENV("RT_HID2LOG")) fprintf(stderr, "mfspr HID2 = %08X (lr %08X)\n", c->spr[920], c->lr);
    if (spr == 22)
        return rt_get_dec();
    if (spr == 287)
        return 0x00087102;   /* PVR: Broadway */
    return c->spr[spr];
}
void ppc_mtmsr(CPU *c, u32 v) { c->msr = v; }

void ppc_dcbz(CPU *c, u32 ea) { (void)c; memset(MEM + (ea & ~31u), 0, 32); }
void ppc_icbi(CPU *c, u32 ea) { (void)c; (void)ea; }

void ppc_lswi(CPU *c, int rd, u32 ea, int n) {
    int r = rd - 1, i = 0;
    while (n > 0) {
        if (i == 0) {
            r = (r + 1) & 31;
            c->r[r] = 0;
        }
        c->r[r] |= (u32)mr8(ea) << (24 - i);
        i = (i + 8) & 31;
        ea++;
        n--;
    }
}
void ppc_stswi(CPU *c, int rs, u32 ea, int n) {
    int r = rs - 1, i = 0;
    while (n > 0) {
        if (i == 0)
            r = (r + 1) & 31;
        mw8(ea, (u8)(c->r[r] >> (24 - i)));
        i = (i + 8) & 31;
        ea++;
        n--;
    }
}

/* paired-single quantised load/store, per GQR type/scale */
static double pow2_tab[65];   /* 2^n for n = -32..32 (GQR scales are 6-bit signed, used either way) */
static inline double pow2i(int n) { return pow2_tab[n + 32]; }
__attribute__((constructor)) static void pow2_init(void) { for (int n = -32; n <= 32; n++) pow2_tab[n + 32] = ldexp(1.0, n); }
static double dequant(u32 raw, int type, int scale) {
    double s = pow2i(-scale);
    switch (type) {
    case 4: return (u8)raw * s;
    case 5: return (u16)raw * s;
    case 6: return (s8)raw * s;
    case 7: return (s16)raw * s;
    default: return u2f(raw);
    }
}
static u32 quant(double v, int type, int scale) {
    double x = v * pow2i(scale);
    switch (type) {
    case 4: return x <= 0 ? 0 : x >= 255 ? 255 : (u32)x;
    case 5: return x <= 0 ? 0 : x >= 65535 ? 65535 : (u32)x;
    case 6: return (u8)(x <= -128 ? -128 : x >= 127 ? 127 : (s32)x);
    case 7: return (u16)(x <= -32768 ? -32768 : x >= 32767 ? 32767 : (s32)x);
    default: return f2u((float)v);
    }
}
static int sext6(u32 v) { return (v & 0x20) ? (int)v - 64 : (int)v; }

void psq_load(CPU *c, int fd, u32 ea, int w, int i) {
    u32 g = c->gqr[i];
    int type = (g >> 16) & 7, scale = sext6((g >> 24) & 0x3F);
    int sz = type == 4 || type == 6 ? 1 : type == 5 || type == 7 ? 2 : 4;
    u32 a = sz == 1 ? mr8(ea) : sz == 2 ? mr16(ea) : mr32(ea);
    c->f[fd] = dequant(a, type, scale);
    if (w)
        c->ps1[fd] = 1.0;
    else {
        u32 b = sz == 1 ? mr8(ea + 1) : sz == 2 ? mr16(ea + 2) : mr32(ea + 4);
        c->ps1[fd] = dequant(b, type, scale);
    }
}
void psq_store(CPU *c, int fs, u32 ea, int w, int i) {
    u32 g = c->gqr[i];
    int type = g & 7, scale = sext6((g >> 8) & 0x3F);
    int sz = type == 4 || type == 6 ? 1 : type == 5 || type == 7 ? 2 : 4;
    u32 a = quant(c->f[fs], type, scale);
    if (sz == 1) mw8(ea, (u8)a); else if (sz == 2) mw16(ea, (u16)a); else mw32(ea, a);
    if (!w) {
        u32 b = quant(c->ps1[fs], type, scale);
        if (sz == 1) mw8(ea + 1, (u8)b); else if (sz == 2) mw16(ea + 2, (u16)b); else mw32(ea + 4, b);
    }
}

/* ---------------------------------------------------------------- tracing */

#define RING 64
static u32 ring[RING];
static unsigned ring_pos;
static int trace_on = -1;
/* call profile (RT_PROFILE=<from s>-<to s>): call counts per function over a window */
static u32 *prof_cnt;
volatile int g_prof_on;
static int prof_slot(u32 addr) { return (int)(((addr - 0x80000000u) >> 2) & 0x3FFFFF); }
void prof_dump(void) {
    if (!prof_cnt) return;
    fprintf(stderr, "---- hottest functions\n");
    int top = ENV("RT_PROFILE_TOP") ? atoi(ENV("RT_PROFILE_TOP")) : 40;
    for (int k = 0; k < top; k++) {
        int best = -1;
        for (int i = 0; i < (1 << 22); i++) if (prof_cnt[i] && (best < 0 || prof_cnt[i] > prof_cnt[best])) best = i;
        if (best < 0) break;
        u32 a = 0x80000000u + ((u32)best << 2);
        fprintf(stderr, "%10u  %08X %s\n", prof_cnt[best], a, fn_name(a));
        prof_cnt[best] = 0;
    }
}
static u32 callers_of = 1;
static u32 logcall[32];
static int nlogcall = -1;
static void logcall_init(void) {
    nlogcall = 0;
    const char *s = ENV("RT_LOGCALL");
    while (s && *s && nlogcall < 32) {
        logcall[nlogcall++] = (u32)strtoul(s, (char **)&s, 16);
        if (*s == ',') s++;
    }
}
static void print_arg(u32 v) {
    if (v >= 0x80000000u && v < 0x81800000u && MEM[v] >= 32 && MEM[v] < 127 && MEM[v + 1] >= 32 && MEM[v + 1] < 127)
        fprintf(stderr, " \"%.40s\"", (char *)MEM + v);
    else
        fprintf(stderr, " %08X", v);
}
static u8 *coverage;
void coverage_dump(void) {
    if (!coverage) return;
    FILE *f = fopen(ENV("RT_COVERAGE"), "w");
    for (u32 i = 0; f && i < (1u << 22); i++)
        if (coverage[i]) fprintf(f, "%08X\n", 0x80000000u + (i << 2));
    if (f) fclose(f);
}
void trace_enter(u32 addr, u32 lr) {
    ring[ring_pos++ % RING] = addr;
    static int cov = -1;
    if (cov < 0) { cov = ENV("RT_COVERAGE") != NULL; if (cov) coverage = calloc(1 << 22, 1); }
    if (cov) coverage[((addr - 0x80000000u) >> 2) & 0x3FFFFF] = 1;
    if (nlogcall < 0) logcall_init();
    for (int i = 0; i < nlogcall; i++)
        if (logcall[i] == addr) {
            fprintf(stderr, "call %s from %08X (%s):", fn_name(addr), lr - 4, fn_name(lr - 4));
            for (int k = 3; k <= 6; k++) print_arg(g_cpu.r[k]);
            fprintf(stderr, "\n");
        }
    if (callers_of == 1) callers_of = ENV("RT_CALLERS") ? (u32)strtoul(ENV("RT_CALLERS"), NULL, 16) : 0;
    static u32 cal_lo = 1, cal_hi;
    if (cal_lo == 1) {
        const char *e = ENV("RT_CALLEES_OF");   /* lo-hi: count functions called from code in this range */
        cal_lo = 0;
        if (e) { cal_lo = (u32)strtoul(e, (char **)&e, 16); cal_hi = *e == '-' ? (u32)strtoul(e + 1, NULL, 16) : cal_lo + 4; }
    }
    if (cal_lo) {
        if (g_prof_on && lr - 4 >= cal_lo && lr - 4 < cal_hi) {
            if (!prof_cnt) prof_cnt = calloc(1 << 22, 4);
            prof_cnt[prof_slot(addr)]++;
        }
        return;
    }
    if (callers_of && addr == callers_of && g_prof_on) {
        if (!prof_cnt) prof_cnt = calloc(1 << 22, 4);
        prof_cnt[prof_slot(lr - 4)]++;   /* count call sites instead of callees */
        return;
    }
    static u32 prof_thread = 1;
    if (prof_thread == 1) prof_thread = ENV("RT_PROFILE_THREAD") ? (u32)strtoul(ENV("RT_PROFILE_THREAD"), NULL, 16) : 0;
    if (!callers_of && g_prof_on && (!prof_thread || mr32(0x800000E4u) == prof_thread)) {
        if (!prof_cnt) prof_cnt = calloc(1 << 22, 4);
        prof_cnt[prof_slot(addr)]++;
    }
    if (trace_on < 0)
        trace_on = ENV("RT_TRACE") != NULL;
    if (trace_on)
        fprintf(stderr, "-> %08X %s\n", addr, fn_name(addr));
}
void rt_backtrace(void) {
    CPU *c = &g_cpu;
    fprintf(stderr, "guest regs:");
    for (int i = 0; i < 32; i++)
        fprintf(stderr, "%s r%d=%08X", i % 8 ? "" : "\n ", i, c->r[i]);
    fprintf(stderr, "\n lr=%08X (%s) ctr=%08X cr=%08X msr=%08X\nguest stack:\n", c->lr, fn_name(c->lr), c->ctr, c->cr, c->msr);
    for (u32 sp = c->r[1], k = 0; k < 16 && sp >= 0x80000000u && sp < 0x94000000u; k++) {
        u32 back = mr32(sp);
        if (!back || back < 0x80000000u || back >= 0x94000000u) break;
        u32 lr = mr32(back + 4);
        fprintf(stderr, "  %08X %s\n", lr, fn_name(lr));
        sp = back;
    }
    fprintf(stderr, "last functions entered:\n");
    for (unsigned i = 0; i < RING; i++) {
        u32 a = ring[(ring_pos + i) % RING];
        if (a)
            fprintf(stderr, "  %08X %s\n", a, fn_name(a));
    }
}

/* ---------------------------------------------------------------- main */

CPU g_cpu;

static void on_segv(int sig, siginfo_t *si, void *uc) {
    (void)uc;
    u8 *a = (u8 *)si->si_addr;
    if (a >= MEM && a < MEM + (1ull << 32))
        fprintf(stderr, "guest memory fault at %08X\n", (u32)(a - MEM));
    else
        fprintf(stderr, "host fault (signal %d) at %p\n", sig, si->si_addr);
    rt_backtrace();
    signal(sig, SIG_DFL);
    raise(sig);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <main.dol> [disc root]\n", argv[0]);
        return 1;
    }
    struct sigaction sa = {0};
    sa.sa_sigaction = on_segv;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    mem_init();
    load_dol(argv[1]);
    lowmem_init("RMBE01");
    hle_init(argc > 2 ? argv[2] : NULL);
    gpu_init();
    gfx_init();
    CPU *c = &g_cpu;
    c->msr = 0x00002032;   /* FP, IR/DR on, interrupts off as after the apploader */
    c->lr = 0;
    g_deterministic = ENV("RT_DETERMINISTIC") != NULL;
    rt_timer_start();
    void hostprof_start(void);
    hostprof_start();
    fprintf(stderr, "boot: entry %08X (%s)\n", g_entry, fn_name(g_entry));
    call_indirect(c, g_entry);
    fprintf(stderr, "entry returned\n");
    return 0;
}
