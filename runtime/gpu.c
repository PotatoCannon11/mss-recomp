/* GPU front end: the recompiled GX library writes its command stream to the write-gather pipe
 * (0xCC008000). Those bytes are appended to the CPU FIFO in guest memory (so display lists built
 * with GXBeginDisplayList land where GX expects them) and, while the GP is linked to the CPU FIFO,
 * decoded here: CP/XF/BP register loads, display-list calls and primitives. Primitives are decoded
 * into host vertices and handed to the renderer (gfx.c). */
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "runtime.h"
#include "gpu.h"

GPUState g_gpu;

/* ---------------------------------------------------------------- FIFO */

#define PI_BASE 0xCC00300Cu
#define PI_END 0xCC003010u
#define PI_WP 0xCC003014u
#define CP_CR 0xCC000002u

static u8 stream[1 << 22];
static u32 stream_len;
static u32 stream_need;   /* bytes the next (incomplete) command needs before it is worth parsing again */
static u32 run_need;      /* set by run(): bytes from its start that the command it stopped at needs */
static u32 last_pi_base;

static void parse_stream(void);

/* ---------------------------------------------------------------- GPU thread
 * The game thread stages write-gather bytes and publishes them in chunks ([u32 len | reset << 31]
 * then the bytes) through a single-producer/single-consumer ring; the GPU thread owns the GL context
 * and the window, parses and draws, and raises the token/finish interrupt flags. RT_DETERMINISTIC
 * (or RT_GPU_INLINE) keeps everything on the game thread. */
int gpu_threaded;
u64 gpu_stat_verts[2][2];   /* [from display list][indexed positions] */
u64 gpu_busy_ns;   /* GPU thread time spent parsing/drawing (RT_FPS reports it per second) */
#define RING (32u << 20)
static u8 ring[RING];
static _Atomic u32 ring_w, ring_r;   /* monotonic byte counters */
static _Atomic int gpu_sleeping;
static pthread_mutex_t gpu_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gpu_cv = PTHREAD_COND_INITIALIZER;
static u8 stage[8192];
static u32 stage_len;
static int stage_reset;

static void ring_put(u32 w, const void *p, u32 n) {
    u32 o = w % RING, k = n < RING - o ? n : RING - o;
    memcpy(ring + o, p, k);
    memcpy(ring, (const u8 *)p + k, n - k);
}
static void ring_get(u32 r, void *p, u32 n) {
    u32 o = r % RING, k = n < RING - o ? n : RING - o;
    memcpy(p, ring + o, k);
    memcpy((u8 *)p + k, ring, n - k);
}

/* hand the staged bytes to the GPU thread (game thread) */
void gpu_publish(void) {
    if (!gpu_threaded || (!stage_len && !stage_reset)) return;
    u32 w = atomic_load_explicit(&ring_w, memory_order_relaxed), need = stage_len + 4;
    while (RING - (w - atomic_load_explicit(&ring_r, memory_order_acquire)) < need) sched_yield();   /* GPU behind: wait */
    u32 hdr = stage_len | (u32)stage_reset << 31;
    ring_put(w, &hdr, 4);
    ring_put(w + 4, stage, stage_len);
    atomic_store_explicit(&ring_w, w + need, memory_order_release);
    stage_len = 0;
    stage_reset = 0;
    if (atomic_load_explicit(&gpu_sleeping, memory_order_acquire)) {
        pthread_mutex_lock(&gpu_mu);
        pthread_cond_signal(&gpu_cv);
        pthread_mutex_unlock(&gpu_mu);
    }
}

static void *gpu_main(void *arg) {
    (void)arg;
    void hostprof_start(void);
    if (ENV("RT_HOSTPROF_GPU")) hostprof_start();   /* profile this thread instead of the game's */
    gfx_init();
    struct timespec last_poll = {0};
    for (;;) {
        u32 r = atomic_load_explicit(&ring_r, memory_order_relaxed);
        u32 w = atomic_load_explicit(&ring_w, memory_order_acquire);
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if ((now.tv_sec - last_poll.tv_sec) * 1000000000L + (now.tv_nsec - last_poll.tv_nsec) > 8000000L) {
            window_poll();   /* input and window events live on this thread with the window */
            last_poll = now;
        }
        if (r == w) {
            atomic_store_explicit(&gpu_sleeping, 1, memory_order_release);
            pthread_mutex_lock(&gpu_mu);
            if (atomic_load_explicit(&ring_w, memory_order_acquire) == r) {
                struct timespec until = now;
                until.tv_nsec += 4000000;
                if (until.tv_nsec >= 1000000000L) { until.tv_sec++; until.tv_nsec -= 1000000000L; }
                pthread_cond_timedwait(&gpu_cv, &gpu_mu, &until);   /* also wakes for window polling */
            }
            pthread_mutex_unlock(&gpu_mu);
            atomic_store_explicit(&gpu_sleeping, 0, memory_order_release);
            continue;
        }
        struct timespec b0;
        clock_gettime(CLOCK_MONOTONIC, &b0);
        while (r != w) {
            u32 hdr, n;
            ring_get(r, &hdr, 4);
            n = hdr & 0x7FFFFFFF;
            if (hdr >> 31) stream_len = stream_need = 0;
            if (stream_len + n > sizeof stream) stream_len = stream_need = 0;   /* runaway: drop */
            ring_get(r + 4, stream + stream_len, n);
            stream_len += n;
            r += 4 + n;
            if (stream_len >= stream_need) parse_stream();
        }
        atomic_store_explicit(&ring_r, r, memory_order_release);
        struct timespec b1;
        clock_gettime(CLOCK_MONOTONIC, &b1);
        gpu_busy_ns += (u64)((b1.tv_sec - b0.tv_sec) * 1000000000L + (b1.tv_nsec - b0.tv_nsec));
    }
    return NULL;
}

/* start rendering: on its own thread, or inline for deterministic runs */
void gpu_start(void) {
    gpu_threaded = !g_deterministic && !ENV("RT_GPU_INLINE");
    if (!gpu_threaded) { gfx_init(); return; }
    pthread_t t;
    pthread_create(&t, NULL, gpu_main, NULL);
}

static int gp_linked(void) { return (mr16(CP_CR) & 0x11) == 0x11; }

static void log_fifo_state(void) {
    if (!ENV("RT_GFXLOG")) return;
    static u32 last_cr = 0xFFFF, last_base;
    u32 cr = mr16(CP_CR), base = mr32(PI_BASE);
    if ((cr != last_cr || base != last_pi_base) && ENV("RT_GFXLOG")) {
        fprintf(stderr, "gpu: CP_CR %04X PI fifo %08X-%08X wp %08X | CP fifo base %04X%04X rp %04X%04X wp %04X%04X\n", cr, base,
                mr32(PI_END), mr32(PI_WP), mr16(0xCC000022u), mr16(0xCC000020u), mr16(0xCC00003Au), mr16(0xCC000038u),
                mr16(0xCC000036u), mr16(0xCC000034u));
        last_cr = cr;
        last_base = base;
    }
    (void)last_base;
}

void gp_write(u32 v, int size) {
    log_fifo_state();
    /* the CPU FIFO in memory (physical addresses) */
    u32 base = mr32(PI_BASE) & 0x3FFFFFE0u, end = mr32(PI_END) & 0x3FFFFFE0u;
    u32 wp = mr32(PI_WP) & 0x3FFFFFFFu;
    if (base != last_pi_base) {
        last_pi_base = base;
        if (gpu_threaded) { gpu_publish(); stage_reset = 1; }
        else stream_len = stream_need = 0;
    }
    int linked = gp_linked(), fifo = base && end > base;
    if (gpu_threaded) {   /* the GPU thread does the parsing: just stage the bytes */
        for (int i = size - 1; i >= 0; i--) {
            u8 b = (u8)(v >> (8 * i));
            if (fifo) {
                MEM[wp & 0x3FFFFFFFu] = b;
                if (++wp >= end) wp = base;
            }
            if (linked) stage[stage_len++] = b;
        }
        if (fifo) mw32(PI_WP, wp);
        if (stage_len >= sizeof stage - 8) gpu_publish();
        return;
    }
    for (int i = size - 1; i >= 0; i--) {
        u8 b = (u8)(v >> (8 * i));
        if (fifo) {
            MEM[wp & 0x3FFFFFFFu] = b;
            if (++wp >= end)   /* the write pointer wraps once it reaches the end line */
                wp = base;
        }
        if (linked && stream_len < sizeof stream)
            stream[stream_len++] = b;
    }
    if (fifo)
        mw32(PI_WP, wp);
    if (stream_len >= 32 && stream_len >= stream_need)
        parse_stream();
}

/* ---------------------------------------------------------------- vertex formats */

static const int comp_size[8] = {1, 1, 2, 2, 4, 0, 0, 0};

typedef struct {
    int pnmtx, texmtx[8];
    int pos, nrm, col[2], tex[8];   /* 0 none, 1 direct, 2 index8, 3 index16 */
    int pos_cnt, pos_fmt, pos_shift;
    int nrm_nbt, nrm_fmt, nrm_idx3;
    int col_fmt[2];
    int tex_cnt[8], tex_fmt[8], tex_shift[8];
    int size;
} VtxFmt;

static void vtx_fmt(int vat, VtxFmt *f) {
    u32 lo = g_gpu.cp[0x50], hi = g_gpu.cp[0x60];
    u32 a = g_gpu.cp[0x70 + vat], b = g_gpu.cp[0x80 + vat], cc = g_gpu.cp[0x90 + vat];
    memset(f, 0, sizeof *f);
    f->pnmtx = lo & 1;
    for (int i = 0; i < 8; i++) f->texmtx[i] = (lo >> (1 + i)) & 1;
    f->pos = (lo >> 9) & 3;
    f->nrm = (lo >> 11) & 3;
    f->col[0] = (lo >> 13) & 3;
    f->col[1] = (lo >> 15) & 3;
    for (int i = 0; i < 8; i++) f->tex[i] = (hi >> (2 * i)) & 3;
    f->pos_cnt = (a & 1) ? 3 : 2; f->pos_fmt = (a >> 1) & 7; f->pos_shift = (a >> 4) & 31;
    f->nrm_nbt = (a >> 9) & 1; f->nrm_fmt = (a >> 10) & 7; f->nrm_idx3 = (a >> 31) & 1;
    f->col_fmt[0] = (a >> 14) & 7; f->col_fmt[1] = (a >> 18) & 7;
    f->tex_cnt[0] = (a >> 21) & 1; f->tex_fmt[0] = (a >> 22) & 7; f->tex_shift[0] = (a >> 25) & 31;
    f->tex_cnt[1] = b & 1; f->tex_fmt[1] = (b >> 1) & 7; f->tex_shift[1] = (b >> 4) & 31;
    f->tex_cnt[2] = (b >> 9) & 1; f->tex_fmt[2] = (b >> 10) & 7; f->tex_shift[2] = (b >> 13) & 31;
    f->tex_cnt[3] = (b >> 18) & 1; f->tex_fmt[3] = (b >> 19) & 7; f->tex_shift[3] = (b >> 22) & 31;
    f->tex_cnt[4] = (b >> 27) & 1; f->tex_fmt[4] = (b >> 28) & 7; f->tex_shift[4] = cc & 31;
    f->tex_cnt[5] = (cc >> 5) & 1; f->tex_fmt[5] = (cc >> 6) & 7; f->tex_shift[5] = (cc >> 9) & 31;
    f->tex_cnt[6] = (cc >> 14) & 1; f->tex_fmt[6] = (cc >> 15) & 7; f->tex_shift[6] = (cc >> 18) & 31;
    f->tex_cnt[7] = (cc >> 23) & 1; f->tex_fmt[7] = (cc >> 24) & 7; f->tex_shift[7] = (cc >> 27) & 31;
    int s = f->pnmtx;
    for (int i = 0; i < 8; i++) s += f->texmtx[i];
    s += f->pos == 1 ? comp_size[f->pos_fmt] * f->pos_cnt : f->pos == 2 ? 1 : f->pos == 3 ? 2 : 0;
    int ncomp = f->nrm_nbt ? 9 : 3;
    if (f->nrm == 1) s += comp_size[f->nrm_fmt] * ncomp;
    else if (f->nrm >= 2) s += (f->nrm == 2 ? 1 : 2) * (f->nrm_nbt && f->nrm_idx3 ? 3 : 1);
    static const int col_size[8] = {2, 3, 4, 2, 3, 4, 0, 0};
    for (int i = 0; i < 2; i++)
        s += f->col[i] == 1 ? col_size[f->col_fmt[i]] : f->col[i] == 2 ? 1 : f->col[i] == 3 ? 2 : 0;
    for (int i = 0; i < 8; i++)
        s += f->tex[i] == 1 ? comp_size[f->tex_fmt[i]] * (f->tex_cnt[i] ? 2 : 1) : f->tex[i] == 2 ? 1 : f->tex[i] == 3 ? 2 : 0;
    f->size = s;
}

/* read one component of format fmt from guest memory or the stream */
static float rd_comp(const u8 *p, int fmt, float scale) {
    switch (fmt) {
    case 0: return p[0] * scale;
    case 1: return (s8)p[0] * scale;
    case 2: return (u16)(p[0] << 8 | p[1]) * scale;
    case 3: return (s16)(p[0] << 8 | p[1]) * scale;
    default: { u32 v = (u32)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; return u2f(v); }
    }
}

/* GX colour formats -> RGBA8 (bit replication, as the hardware expands them) */
static void rd_color8(const u8 *p, int fmt, u8 out[4]) {
    switch (fmt) {
    case 0: { u16 v = p[0] << 8 | p[1]; u8 r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
              out[0] = (u8)(r << 3 | r >> 2); out[1] = (u8)(g << 2 | g >> 4); out[2] = (u8)(b << 3 | b >> 2); out[3] = 255; break; }
    case 1: case 2: out[0] = p[0]; out[1] = p[1]; out[2] = p[2]; out[3] = 255; break;
    case 3: { u16 v = p[0] << 8 | p[1];
              out[0] = (u8)((v >> 12) * 17); out[1] = (u8)(((v >> 8) & 15) * 17); out[2] = (u8)(((v >> 4) & 15) * 17); out[3] = (u8)((v & 15) * 17); break; }
    case 4: { u32 v = p[0] << 16 | p[1] << 8 | p[2]; u8 c[4] = {(u8)(v >> 18), (u8)((v >> 12) & 63), (u8)((v >> 6) & 63), (u8)(v & 63)};
              for (int i = 0; i < 4; i++) out[i] = (u8)(c[i] << 2 | c[i] >> 4); break; }
    default: memcpy(out, p, 4); break;
    }
}

/* pointer to attribute data: direct (in the vertex) or indexed (array base/stride in CP regs) */
static const u8 *attr(const u8 **pp, int mode, int array) {
    const u8 *p = *pp;
    if (mode == 2 || mode == 3) {
        u32 idx = mode == 2 ? p[0] : (u32)(p[0] << 8 | p[1]);
        *pp += mode == 2 ? 1 : 2;
        u32 base = g_gpu.cp[0xA0 + array] & 0x3FFFFFFFu, stride = g_gpu.cp[0xB0 + array] & 0xFF;
        return MEM + base + idx * stride;
    }
    return p;
}

/* decode one GX vertex straight into the renderer's vertex layout */
static void decode_vertex(const u8 *p, const VtxFmt *f, GLVertex *v) {
    u32 m30 = g_gpu.cp[0x30], m40 = g_gpu.cp[0x40];
    v->mtx[0] = f->pnmtx ? *p++ : (u8)(m30 & 63);   /* MATINDEX_A default */
    for (int i = 0; i < 8; i++) {
        u8 dflt = (u8)(i < 4 ? (m30 >> (6 + 6 * i)) & 63 : (m40 >> (6 * (i - 4))) & 63);
        v->mtx[4 + i] = f->texmtx[i] ? *p++ : dflt;
    }
    v->pos[2] = 0;
    if (f->pos) {
        const u8 *q = attr(&p, f->pos, 0);
        float sc = 1.0f / (1 << f->pos_shift);
        int cs = comp_size[f->pos_fmt];
        for (int k = 0; k < f->pos_cnt; k++) v->pos[k] = rd_comp(q + k * cs, f->pos_fmt, sc);
        if (f->pos == 1) p += cs * f->pos_cnt;
    } else
        v->pos[0] = v->pos[1] = 0;
    if (f->nrm) {
        const u8 *q = attr(&p, f->nrm, 1);
        int cs = comp_size[f->nrm_fmt];
        float sc = f->nrm_fmt == 1 ? 1.0f / 64 : f->nrm_fmt == 3 ? 1.0f / 16384 : 1.0f;
        for (int k = 0; k < 3; k++) v->nrm[k] = rd_comp(q + k * cs, f->nrm_fmt, sc);
        if (f->nrm == 1) p += cs * (f->nrm_nbt ? 9 : 3);
        else if (f->nrm_nbt && f->nrm_idx3) p += (f->nrm == 2 ? 1 : 2) * 2;
    } else
        v->nrm[0] = v->nrm[1] = v->nrm[2] = 0;
    static const int col_size[8] = {2, 3, 4, 2, 3, 4, 0, 0};
    for (int i = 0; i < 2; i++) {
        u8 *out = i ? v->col1 : v->col0;
        if (!f->col[i]) { memset(out, 255, 4); continue; }
        const u8 *q = attr(&p, f->col[i], 2 + i);
        rd_color8(q, f->col_fmt[i], out);
        if (f->col[i] == 1) p += col_size[f->col_fmt[i]];
    }
    for (int i = 0; i < 8; i++) {
        if (!f->tex[i]) { v->tex[i][0] = v->tex[i][1] = 0; continue; }
        const u8 *q = attr(&p, f->tex[i], 4 + i);
        int cs = comp_size[f->tex_fmt[i]];
        float sc = 1.0f / (1 << f->tex_shift[i]);
        v->tex[i][0] = rd_comp(q, f->tex_fmt[i], sc);
        v->tex[i][1] = f->tex_cnt[i] ? rd_comp(q + cs, f->tex_fmt[i], sc) : 0;
        if (f->tex[i] == 1) p += cs * (f->tex_cnt[i] ? 2 : 1);
    }
}

struct FillCtx { const u8 *p; const VtxFmt *f; };
static void fill_vertices(GLVertex *out, u32 n, void *ctx) {
    struct FillCtx *fc = ctx;
    const u8 *q = fc->p;
    for (u32 k = 0; k < n; k++, q += fc->f->size) decode_vertex(q, fc->f, &out[k]);
}

/* ---------------------------------------------------------------- registers */

static void load_cp(u8 reg, u32 v) {
    g_gpu.cp[reg] = v;
}

/* only real changes mark uniforms dirty (games reload identical matrices all the time, and every
 * dirty mark breaks the current draw batch) */
static void load_xf(u32 addr, u32 n, const u8 *p) {
    for (u32 i = 0; i < n; i++) {
        u32 a = addr + i, v = (u32)p[4 * i] << 24 | p[4 * i + 1] << 16 | p[4 * i + 2] << 8 | p[4 * i + 3];
        if (a < 0x1000) {
            if (g_gpu.xfmem[a] == v) continue;
            g_gpu.xfmem[a] = v;
            if (a < g_gpu.xf_lo) g_gpu.xf_lo = a;
            if (a > g_gpu.xf_hi) g_gpu.xf_hi = a;
            g_gpu.udirty = 1;
        } else if (a < 0x1100) {
            if (g_gpu.xfreg[a - 0x1000] == v) continue;
            g_gpu.xfreg[a - 0x1000] = v;
            g_gpu.udirty = 1;
        }
    }
}

static void load_indexed(int which, u32 v) {
    /* LOAD_INDX_A..D: copy n words from array 12..15 entry `idx` into XF memory */
    u32 idx = v >> 16, n = ((v >> 12) & 15) + 1, addr = v & 0xFFF;
    u32 base = g_gpu.cp[0xA0 + 12 + which] & 0x3FFFFFFFu, stride = g_gpu.cp[0xB0 + 12 + which] & 0xFF;
    load_xf(addr, n, MEM + base + idx * stride);
}

static void load_bp(u32 v) {
    u8 reg = v >> 24;
    u32 mask = g_gpu.bpmask;
    g_gpu.bpmask = 0xFFFFFF;
    if (reg == 0xFE) {   /* BP mask for the next write */
        g_gpu.bpmask = v & 0xFFFFFF;
        return;
    }
    u32 old = g_gpu.bp[reg];
    g_gpu.bp[reg] = (old & ~mask) | (v & mask & 0xFFFFFF);
    if ((reg >= 0xE0 || reg == 0x43) && g_gpu.bp[reg] != old) g_gpu.udirty = 1;   /* TEV colours, fog, alpha refs */
    switch (reg) {
    case 0x45:   /* PE_DONE: draw done -> finish interrupt */
        if (v & 2) gfx_flush();
        g_gpu.irq_finish = 1;
        g_irq_pending = 1;   /* deliver at the next back-edge, not the next timer tick */
        break;
    case 0x47:   /* PE_TOKEN: GXReadDrawSync reads it back from the PE token register */
        g_gpu.token = v & 0xFFFF;
        mw16(0xCC00100Eu, (u16)g_gpu.token);
        break;
    case 0x48:   /* PE_TOKEN_INT */
        g_gpu.token = v & 0xFFFF;
        mw16(0xCC00100Eu, (u16)g_gpu.token);
        g_gpu.irq_token = 1;
        g_irq_pending = 1;
        break;
    case 0x52:   /* PE_COPY_EXECUTE: EFB -> texture or XFB, optional clear */
        gfx_efb_copy(v & 0xFFFFFF);
        break;
    case 0x64: case 0x65:   /* TLUT load */
        if (reg == 0x65) gfx_load_tlut();
        break;
    case 0xE0: case 0xE1: case 0xE2: case 0xE3: case 0xE4: case 0xE5: case 0xE6: case 0xE7: {
        /* TEV register pair: RA (even) or BG (odd); bit 23 selects konst vs colour register */
        int i = (reg - 0xE0) >> 1, konst = (v >> 23) & 1;
        float *d = konst ? g_gpu.kreg[i] : g_gpu.creg[i];
        int lo = v & 0x7FF, hi = (v >> 12) & 0x7FF;
        if (lo & 0x400) lo -= 0x800;
        if (hi & 0x400) hi -= 0x800;
        if (reg & 1) { d[2] = lo / 255.f; d[1] = hi / 255.f; }   /* B, G */
        else { d[0] = lo / 255.f; d[3] = hi / 255.f; }           /* R, A */
        break;
    }
    default:
        break;
    }
}

/* ---------------------------------------------------------------- command processor */

/* run commands in p[0..len): returns bytes consumed (a trailing partial command is left) */
static u32 run(const u8 *p, u32 len, int depth) {
    u32 i = 0;
    while (i < len) {
        u8 op = p[i];
        if (op == 0x00 || op == 0x48) { i++; continue; }
        if (op == 0x08) {
            if (i + 6 > len) { run_need = i + 6; break; }
            load_cp(p[i + 1], (u32)p[i + 2] << 24 | p[i + 3] << 16 | p[i + 4] << 8 | p[i + 5]);
            i += 6; continue;
        }
        if (op == 0x10) {
            if (i + 5 > len) { run_need = i + 5; break; }
            u32 h = (u32)p[i + 1] << 24 | p[i + 2] << 16 | p[i + 3] << 8 | p[i + 4];
            u32 n = (h >> 16) + 1;
            if (i + 5 + 4 * n > len) { run_need = i + 5 + 4 * n; break; }
            load_xf(h & 0xFFFF, n, p + i + 5);
            i += 5 + 4 * n; continue;
        }
        if (op == 0x20 || op == 0x28 || op == 0x30 || op == 0x38) {
            if (i + 5 > len) { run_need = i + 5; break; }
            load_indexed((op - 0x20) >> 3, (u32)p[i + 1] << 24 | p[i + 2] << 16 | p[i + 3] << 8 | p[i + 4]);
            i += 5; continue;
        }
        if (op == 0x40) {
            if (i + 9 > len) { run_need = i + 9; break; }
            u32 a = ((u32)p[i + 1] << 24 | p[i + 2] << 16 | p[i + 3] << 8 | p[i + 4]) & 0x3FFFFFFFu;
            u32 n = (u32)p[i + 5] << 24 | p[i + 6] << 16 | p[i + 7] << 8 | p[i + 8];
            g_gpu.ndl++;
            if (ENV("RT_DLLOG") && g_gpu.ndl < 40) fprintf(stderr, "gpu: CALL_DL %08X +%X\n", a, n);
            if (depth < 4 && n < 0x01000000u) {
                u32 saved = run_need;   /* a display list's own end doesn't concern the FIFO stream */
                run(MEM + a, n, depth + 1);
                run_need = saved;
            }
            i += 9; continue;
        }
        if (op == 0x61) {
            if (i + 5 > len) { run_need = i + 5; break; }
            load_bp((u32)p[i + 1] << 24 | p[i + 2] << 16 | p[i + 3] << 8 | p[i + 4]);
            i += 5; continue;
        }
        if (op >= 0x80 && op < 0xC0) {
            if (i + 3 > len) { run_need = i + 3; break; }
            int vat = op & 7, prim = (op >> 3) & 7;
            u32 n = (u32)p[i + 1] << 8 | p[i + 2];
            VtxFmt f;
            vtx_fmt(vat, &f);
            g_gpu.cur_vat = vat;
            if (i + 3 + n * f.size > len) { run_need = i + 3 + n * f.size; break; }
            struct FillCtx fc = {p + i + 3, &f};
            gpu_stat_verts[depth > 0][f.pos >= 2] += n;
            gfx_draw(prim, n, fill_vertices, &fc);
            g_gpu.prims++;
            i += 3 + n * f.size; continue;
        }
        if (g_gpu.bad_ops++ < 20)
            fprintf(stderr, "gpu: unknown opcode %02X\n", op);
        i++;
    }
    if (i >= len) run_need = 0;
    return i;
}

static void parse_stream(void) {
    run_need = 0;
    u32 used = run(stream, stream_len, 0);
    memmove(stream, stream + used, stream_len - used);
    stream_len -= used;
    stream_need = run_need > used ? run_need - used : 0;
}

/* GX entry points that need the stream drained first */
void gpu_sync(void) { parse_stream(); }

void gpu_init(void) {
    g_gpu.bpmask = 0xFFFFFF;
    g_gpu.udirty = 1;
    g_gpu.xf_lo = 0; g_gpu.xf_hi = 0xFFF;   /* first fill converts everything */
}
