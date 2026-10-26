/* GPU front end: the recompiled GX library writes its command stream to the write-gather pipe
 * (0xCC008000). Those bytes are appended to the CPU FIFO in guest memory (so display lists built
 * with GXBeginDisplayList land where GX expects them) and, while the GP is linked to the CPU FIFO,
 * decoded here: CP/XF/BP register loads, display-list calls and primitives. Primitives are decoded
 * into host vertices and handed to the renderer (gfx.c). */
#include <stdio.h>
#include <stdlib.h>
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
static u32 last_pi_base;

static void parse_stream(void);

static int gp_linked(void) { return (mr16(CP_CR) & 0x11) == 0x11; }

void gp_write(u32 v, int size) {
    /* the CPU FIFO in memory (physical addresses) */
    u32 base = mr32(PI_BASE) & 0x3FFFFFE0u, end = mr32(PI_END) & 0x3FFFFFE0u;
    u32 wp = mr32(PI_WP) & 0x3FFFFFFFu;
    if (base != last_pi_base) {
        last_pi_base = base;
        stream_len = 0;
    }
    for (int i = size - 1; i >= 0; i--) {
        u8 b = (u8)(v >> (8 * i));
        if (base && end > base) {
            MEM[wp & 0x3FFFFFFFu] = b;
            wp++;
            if (wp >= end)   /* the write pointer wraps once it reaches the end line */
                wp = base;
        }
        if (gp_linked() && stream_len < sizeof stream)
            stream[stream_len++] = b;
    }
    if (base && end > base)
        mw32(PI_WP, wp);
    if (stream_len >= 32)
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

static void rd_color(const u8 *p, int fmt, float out[4]) {
    switch (fmt) {
    case 0: { u16 v = p[0] << 8 | p[1]; out[0] = ((v >> 11) & 31) / 31.f; out[1] = ((v >> 5) & 63) / 63.f; out[2] = (v & 31) / 31.f; out[3] = 1; break; }
    case 1: case 2: out[0] = p[0] / 255.f; out[1] = p[1] / 255.f; out[2] = p[2] / 255.f; out[3] = 1; break;
    case 3: { u16 v = p[0] << 8 | p[1]; out[0] = (v >> 12) / 15.f; out[1] = ((v >> 8) & 15) / 15.f; out[2] = ((v >> 4) & 15) / 15.f; out[3] = (v & 15) / 15.f; break; }
    case 4: { u32 v = p[0] << 16 | p[1] << 8 | p[2]; out[0] = (v >> 18) / 63.f; out[1] = ((v >> 12) & 63) / 63.f; out[2] = ((v >> 6) & 63) / 63.f; out[3] = (v & 63) / 63.f; break; }
    default: out[0] = p[0] / 255.f; out[1] = p[1] / 255.f; out[2] = p[2] / 255.f; out[3] = p[3] / 255.f; break;
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

static void decode_vertex(const u8 *p, const VtxFmt *f, GXVertex *v) {
    memset(v, 0, sizeof *v);
    v->pnmtx = (g_gpu.cp[0x30] & 63);   /* MATINDEX_A default */
    if (f->pnmtx) v->pnmtx = *p++;
    for (int i = 0; i < 8; i++) {
        int dflt = i < 4 ? (g_gpu.cp[0x30] >> (6 + 6 * i)) & 63 : (g_gpu.cp[0x40] >> (6 * (i - 4))) & 63;
        v->texmtx[i] = f->texmtx[i] ? *p++ : dflt;
    }
    if (f->pos) {
        const u8 *q = attr(&p, f->pos, 0);
        float sc = 1.0f / (1 << f->pos_shift);
        int cs = comp_size[f->pos_fmt];
        for (int k = 0; k < f->pos_cnt; k++) v->pos[k] = rd_comp(q + k * cs, f->pos_fmt, sc);
        if (f->pos == 1) p += cs * f->pos_cnt;
    }
    if (f->nrm) {
        const u8 *q = attr(&p, f->nrm, 1);
        int cs = comp_size[f->nrm_fmt];
        float sc = f->nrm_fmt == 1 ? 1.0f / 64 : f->nrm_fmt == 3 ? 1.0f / 16384 : 1.0f;
        for (int k = 0; k < 3; k++) v->nrm[k] = rd_comp(q + k * cs, f->nrm_fmt, sc);
        if (f->nrm == 1) p += cs * (f->nrm_nbt ? 9 : 3);
        else if (f->nrm_nbt && f->nrm_idx3) p += (f->nrm == 2 ? 1 : 2) * 2;
    }
    static const int col_size[8] = {2, 3, 4, 2, 3, 4, 0, 0};
    for (int i = 0; i < 2; i++) {
        if (!f->col[i]) { v->col[i][0] = v->col[i][1] = v->col[i][2] = v->col[i][3] = 1; continue; }
        const u8 *q = attr(&p, f->col[i], 2 + i);
        rd_color(q, f->col_fmt[i], v->col[i]);
        if (f->col[i] == 1) p += col_size[f->col_fmt[i]];
    }
    for (int i = 0; i < 8; i++) {
        if (!f->tex[i]) continue;
        const u8 *q = attr(&p, f->tex[i], 4 + i);
        int cs = comp_size[f->tex_fmt[i]], n = f->tex_cnt[i] ? 2 : 1;
        float sc = 1.0f / (1 << f->tex_shift[i]);
        for (int k = 0; k < n; k++) v->tex[i][k] = rd_comp(q + k * cs, f->tex_fmt[i], sc);
        if (f->tex[i] == 1) p += cs * n;
    }
    v->ntex = 0;
    for (int i = 0; i < 8; i++) if (f->tex[i]) v->ntex = i + 1;
    v->has_nrm = f->nrm != 0;
    v->has_col = (f->col[0] ? 1 : 0) | (f->col[1] ? 2 : 0);
}

/* ---------------------------------------------------------------- registers */

static void load_cp(u8 reg, u32 v) {
    g_gpu.cp[reg] = v;
}

static void load_xf(u32 addr, u32 n, const u8 *p) {
    for (u32 i = 0; i < n; i++) {
        u32 a = addr + i, v = (u32)p[4 * i] << 24 | p[4 * i + 1] << 16 | p[4 * i + 2] << 8 | p[4 * i + 3];
        if (a < 0x1000) g_gpu.xfmem[a] = v;
        else if (a < 0x1100) g_gpu.xfreg[a - 0x1000] = v;
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
    g_gpu.bp[reg] = (g_gpu.bp[reg] & ~mask) | (v & mask & 0xFFFFFF);
    switch (reg) {
    case 0x45:   /* PE_DONE: draw done -> finish interrupt */
        if (v & 2) gfx_flush();
        g_gpu.irq_finish = 1;
        break;
    case 0x47:   /* PE_TOKEN */
        g_gpu.token = v & 0xFFFF;
        break;
    case 0x48:   /* PE_TOKEN_INT */
        g_gpu.token = v & 0xFFFF;
        mw16(0xCC00100Eu, (u16)g_gpu.token);
        g_gpu.irq_token = 1;
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
            if (i + 6 > len) break;
            load_cp(p[i + 1], (u32)p[i + 2] << 24 | p[i + 3] << 16 | p[i + 4] << 8 | p[i + 5]);
            i += 6; continue;
        }
        if (op == 0x10) {
            if (i + 5 > len) break;
            u32 h = (u32)p[i + 1] << 24 | p[i + 2] << 16 | p[i + 3] << 8 | p[i + 4];
            u32 n = (h >> 16) + 1;
            if (i + 5 + 4 * n > len) break;
            load_xf(h & 0xFFFF, n, p + i + 5);
            i += 5 + 4 * n; continue;
        }
        if (op == 0x20 || op == 0x28 || op == 0x30 || op == 0x38) {
            if (i + 5 > len) break;
            load_indexed((op - 0x20) >> 3, (u32)p[i + 1] << 24 | p[i + 2] << 16 | p[i + 3] << 8 | p[i + 4]);
            i += 5; continue;
        }
        if (op == 0x40) {
            if (i + 9 > len) break;
            u32 a = ((u32)p[i + 1] << 24 | p[i + 2] << 16 | p[i + 3] << 8 | p[i + 4]) & 0x3FFFFFFFu;
            u32 n = (u32)p[i + 5] << 24 | p[i + 6] << 16 | p[i + 7] << 8 | p[i + 8];
            if (depth < 4 && n < 0x01000000u)
                run(MEM + a, n, depth + 1);
            i += 9; continue;
        }
        if (op == 0x61) {
            if (i + 5 > len) break;
            load_bp((u32)p[i + 1] << 24 | p[i + 2] << 16 | p[i + 3] << 8 | p[i + 4]);
            i += 5; continue;
        }
        if (op >= 0x80 && op < 0xC0) {
            if (i + 3 > len) break;
            int vat = op & 7, prim = (op >> 3) & 7;
            u32 n = (u32)p[i + 1] << 8 | p[i + 2];
            VtxFmt f;
            vtx_fmt(vat, &f);
            if (i + 3 + n * f.size > len) break;
            static GXVertex verts[65536];
            const u8 *q = p + i + 3;
            for (u32 k = 0; k < n; k++, q += f.size)
                decode_vertex(q, &f, &verts[k]);
            gfx_draw(prim, verts, n);
            g_gpu.prims++;
            i += 3 + n * f.size; continue;
        }
        if (g_gpu.bad_ops++ < 20)
            fprintf(stderr, "gpu: unknown opcode %02X\n", op);
        i++;
    }
    return i;
}

static void parse_stream(void) {
    u32 used = run(stream, stream_len, 0);
    memmove(stream, stream + used, stream_len - used);
    stream_len -= used;
}

/* GX entry points that need the stream drained first */
void gpu_sync(void) { parse_stream(); }

void gpu_init(void) {
    g_gpu.bpmask = 0xFFFFFF;
}
