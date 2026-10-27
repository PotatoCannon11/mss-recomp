/* Guest CPU state and memory access for statically recompiled Gekko (PowerPC 750CL) code. */
#pragma once
#include <emmintrin.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;

typedef struct CPU {
    u32 r[32];
    double f[32];   /* ps0 */
    double ps1[32]; /* paired-single second slot */
    u32 cr;         /* CR0 in the top nibble, as on the hardware */
    u32 lr, ctr;
    u32 xer_so, xer_ov, xer_ca, xer_bc;
    u32 msr, fpscr;
    u32 gqr[8];
    u32 spr[1024];
    u32 reserve;    /* lwarx address */
} CPU;

/* Base of the 4 GiB host region the guest address space is mapped into. */
extern u8 *MEM;

static inline u8 mr8(u32 a) { return MEM[a]; }
static inline u16 mr16(u32 a) { u16 v; memcpy(&v, MEM + a, 2); return __builtin_bswap16(v); }
static inline u32 mr32(u32 a) { u32 v; memcpy(&v, MEM + a, 4); return __builtin_bswap32(v); }
static inline u64 mr64(u32 a) { u64 v; memcpy(&v, MEM + a, 8); return __builtin_bswap64(v); }
/* the GX write-gather pipe: every store to 0xCC008000..0xCC008FFF feeds the GPU */
void gp_write(u32 v, int size);
#define IS_WGPIPE(a) __builtin_expect(((a) & 0xFFFFF000u) == 0xCC008000u, 0)
static inline void mw8(u32 a, u8 v) { if (IS_WGPIPE(a)) { gp_write(v, 1); return; } MEM[a] = v; }
static inline void mw16(u32 a, u16 v) { if (IS_WGPIPE(a)) { gp_write(v, 2); return; } v = __builtin_bswap16(v); memcpy(MEM + a, &v, 2); }
static inline void mw32(u32 a, u32 v) { if (IS_WGPIPE(a)) { gp_write(v, 4); return; } v = __builtin_bswap32(v); memcpy(MEM + a, &v, 4); }
static inline void mw64(u32 a, u64 v) { if (IS_WGPIPE(a)) { gp_write((u32)(v >> 32), 4); gp_write((u32)v, 4); return; } v = __builtin_bswap64(v); memcpy(MEM + a, &v, 8); }

static inline float u2f(u32 v) { float f; memcpy(&f, &v, 4); return f; }
static inline u32 f2u(float f) { u32 v; memcpy(&v, &f, 4); return v; }
static inline double u2d(u64 v) { double d; memcpy(&d, &v, 8); return d; }
static inline u64 d2u(double d) { u64 v; memcpy(&v, &d, 8); return v; }

static inline double lfs(u32 a) { return (double)u2f(mr32(a)); }
static inline void stfs(u32 a, double d) { mw32(a, f2u((float)d)); }
static inline double lfd(u32 a) { return u2d(mr64(a)); }
static inline void stfd(u32 a, double d) { mw64(a, d2u(d)); }
static inline double rs(double d) { return (double)(float)d; } /* round to single */

/* CR helpers: field n occupies bits (28 - 4n)..(31 - 4n). */
static inline void setcr(CPU *c, int n, u32 v) { c->cr = (c->cr & ~(0xFu << (28 - 4 * n))) | (v << (28 - 4 * n)); }
static inline u32 crbit(CPU *c, int b) { return (c->cr >> (31 - b)) & 1; }
static inline void setcrbit(CPU *c, int b, u32 v) { c->cr = (c->cr & ~(1u << (31 - b))) | ((v & 1) << (31 - b)); }
static inline void cmps(CPU *c, int n, s32 a, s32 b) { setcr(c, n, (a < b ? 8 : a > b ? 4 : 2) | c->xer_so); }
static inline void cmpu(CPU *c, int n, u32 a, u32 b) { setcr(c, n, (a < b ? 8 : a > b ? 4 : 2) | c->xer_so); }
static inline void rc0(CPU *c, u32 v) { cmps(c, 0, (s32)v, 0); }
static inline void fcmp(CPU *c, int n, double a, double b) {
    u32 v = isnan(a) || isnan(b) ? 1 : a < b ? 8 : a > b ? 4 : 2;
    setcr(c, n, v);
    c->fpscr = (c->fpscr & ~0xF000u) | (v << 12);
}

static inline u32 rotl(u32 v, u32 n) { n &= 31; return n ? (v << n) | (v >> (32 - n)) : v; }

static inline u32 addc(CPU *c, u32 a, u32 b, u32 ci) {
    u64 s = (u64)a + b + ci;
    c->xer_ca = (u32)(s >> 32);
    return (u32)s;
}

static inline u32 sraw(CPU *c, u32 v, u32 n) {
    n &= 63;
    if (n >= 32) { c->xer_ca = (s32)v < 0; return (s32)v < 0 ? 0xFFFFFFFF : 0; }
    u32 r = (u32)((s32)v >> n);
    c->xer_ca = (s32)v < 0 && n && (v << (32 - n)) != 0;
    return r;
}

static inline u32 divw(s32 a, s32 b) { return (b == 0 || (a == INT32_MIN && b == -1)) ? (a < 0 ? 0xFFFFFFFF : 0) : (u32)(a / b); }
static inline u32 divwu(u32 a, u32 b) { return b ? a / b : 0; }

static inline double fctiwz(double d) {
    s32 i;
    if (isnan(d)) i = INT32_MIN;
    else if (d >= 2147483647.0) i = INT32_MAX;
    else if (d <= -2147483648.0) i = INT32_MIN;
    else i = (s32)d;
    return u2d(0xFFF8000000000000ull | (u32)i);
}
/* environment switches are read once per call site: getenv is a linear scan and some of these sit
 * on per-draw / per-instruction paths */
#define ENV(name) ({ static const char *env_v_; static int env_i_; if (!env_i_) { env_v_ = getenv(name); env_i_ = 1; } env_v_; })

static inline double fctiw(double d) {
    s32 i;
    if (isnan(d)) i = INT32_MIN;
    else if (d >= 2147483647.0) i = INT32_MAX;
    else if (d <= -2147483648.0) i = INT32_MIN;
    else i = _mm_cvtsd_si32(_mm_set_sd(d));   /* current rounding mode (nearest), inline */
    return u2d(0xFFF8000000000000ull | (u32)i);
}
static inline double fres(double d) { return rs(1.0 / d); }
static inline double frsqrte(double d) { return 1.0 / sqrt(d); }

/* runtime services (runtime.c) */
typedef void (*GuestFn)(CPU *);
GuestFn lookup_fn(u32 addr);          /* NULL if addr is not a known function entry */
void call_indirect(CPU *c, u32 addr);  /* bctrl / blrl / indirect tail call */
void ppc_illegal(CPU *c, u32 pc, u32 word);
void ppc_trap(CPU *c, u32 pc);
void ppc_sc(CPU *c, u32 pc);
void ppc_rfi(CPU *c, u32 pc);
u32 ppc_mftb(CPU *c, int upper);
void ppc_mtspr(CPU *c, int spr, u32 v);
u32 ppc_mfspr(CPU *c, int spr);
void ppc_mtmsr(CPU *c, u32 v);
void psq_load(CPU *c, int fd, u32 ea, int w, int i);
void psq_store(CPU *c, int fs, u32 ea, int w, int i);
void ppc_dcbz(CPU *c, u32 ea);
void ppc_icbi(CPU *c, u32 ea);
void ppc_lswi(CPU *c, int rd, u32 ea, int n);
void ppc_stswi(CPU *c, int rs, u32 ea, int n);

#define WEAK __attribute__((weak))
#ifdef TRACE
void trace_enter(u32 addr, u32 lr);
#define FN_ENTER(a, lr) trace_enter(a, lr)
#else
/* mod hooks: one bit per function entry; only hooked functions call into the mod layer */
extern u8 g_hookbits[1 << 19];
int mod_enter(CPU *c, u32 addr);
#define FN_ENTER(a, lr) do { if (__builtin_expect((g_hookbits[((a) >> 5) & 0x7FFFF] >> (((a) >> 2) & 7)) & 1, 0) && mod_enter(c, (a))) return; } while (0)
#endif

/* set by the host timer thread; checked on loop back-edges so long-running guest loops can be
 * interrupted (and preempted) like on hardware */
extern volatile int g_irq_pending;
void rt_async_irq(CPU *c);
#define IRQ_POINT(c) do { if (__builtin_expect(g_irq_pending, 0)) rt_async_irq(c); } while (0)
