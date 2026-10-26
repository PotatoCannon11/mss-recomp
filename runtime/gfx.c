/* Renderer (placeholder): counts work until the OpenGL backend lands. */
#include <stdio.h>
#include <stdlib.h>
#include "runtime.h"
#include "gpu.h"

static u64 draws, verts;
void gfx_init(void) {}
void gfx_draw(int prim, const GXVertex *v, u32 n) { (void)prim; (void)v; draws++; verts += n; }
void gfx_flush(void) {}
void gfx_load_tlut(void) {}
void gfx_efb_copy(u32 cmd) {
    if (cmd & (1u << 14)) {   /* to XFB: a finished frame */
        g_gpu.frames++;
        if (getenv("RT_GFXLOG"))
            fprintf(stderr, "frame %llu: %llu draws, %llu vertices\n", (unsigned long long)g_gpu.frames,
                    (unsigned long long)draws, (unsigned long long)verts);
        draws = verts = 0;
    }
}
