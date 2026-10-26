/* GPU state shared by the command processor (gpu.c) and the renderer (gfx.c). */
#pragma once
#include "cpu.h"

typedef struct {
    u32 bp[256], cp[256];
    u32 xfmem[0x1000];   /* matrices, normal matrices, post-transform matrices, lights */
    u32 xfreg[0x100];    /* XF registers 0x1000.. */
    u32 bpmask;
    u32 token;
    volatile int irq_finish, irq_token;
    u64 prims, frames;
    int bad_ops, cur_vat;
    u64 ndl, last_frame_draws, preloads, efb_gen;
    int udirty;            /* XF / TEV register state changed since the last uniform fill */
    u32 xf_lo, xf_hi;      /* xfmem words changed since then (none: xf_lo = ~0, xf_hi = 0) */
    u32 state_epoch;       /* bumped when GL state was changed outside apply_state */
    float creg[4][4], kreg[4][4];   /* TEV colour registers (PREV, C0-C2) and konst colours, -4..4 */
} GPUState;
extern GPUState g_gpu;

typedef struct {
    float pos[3], nrm[3], col[2][4], tex[8][2];
    int pnmtx, texmtx[8], ntex, has_nrm, has_col;
} GXVertex;

void gpu_init(void);
void gpu_start(void);     /* renderer on its own thread (or inline when deterministic) */
void gpu_publish(void);   /* game thread: hand staged FIFO bytes to the GPU thread */
extern int gpu_threaded;
extern int g_deterministic;
void gpu_sync(void);
void gp_write(u32 v, int size);

/* renderer */
void gfx_init(void);
void gfx_draw(int prim, const GXVertex *v, u32 n);
void gfx_efb_copy(u32 cmd);
void gfx_flush(void);
void gfx_load_tlut(void);
void gfx_present(int sx, int sy, int sw, int sh);

/* window / frontend (window.c) */
int gfx_window_init(int w, int h);
void window_present(unsigned efb_tex, int sx, int sy, int sw, int sh, int efb_w, int efb_h);
void window_poll(void);
