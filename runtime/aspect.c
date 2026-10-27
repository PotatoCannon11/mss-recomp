/* Dynamic aspect: the game keeps rendering its 16:9 (or 4:3) picture into a 640-wide framebuffer;
 * the renderer makes that framebuffer as wide as the window (anamorphic, like the console's own
 * widescreen mode) and adapts screen projections (gfx.c, projection_adapt) so nothing is stretched
 * or cropped. Also: the MTX projection builders natively (exact SDK math), RT_ASPECTLOG=1 logs
 * every call with its caller (to see which cameras the game builds). */
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "runtime.h"
#include "names.h"

#define HLE(name) void FN_##name(CPU *c)

/* presentation surface aspect relative to the game's reference (16:9, or 4:3 with RT_43):
 * > 1 wider (Hor+), < 1 taller (Vert+) */
float g_aspect_k = 1.0f;
void aspect_set_surface(int w, int h) {
    const char *e = ENV("RT_ASPECT");   /* W:H overrides the window (headless tests) */
    float aw = (float)w, ah = (float)h;
    if (e) sscanf(e, "%f:%f", &aw, &ah);
    if (aw <= 0 || ah <= 0) return;
    float ref = ENV("RT_43") ? 4.0f / 3.0f : 16.0f / 9.0f;
    float k = (aw / ah) / ref;
    if (k < 0.25f) k = 0.25f;
    if (k > 4.0f) k = 4.0f;
    g_aspect_k = k;
}

static void wf(u32 a, float v) { u32 u; memcpy(&u, &v, 4); mw32(a, u); }

static void put44(u32 m, const float v[16]) {
    for (int i = 0; i < 16; i++) wf(m + 4 * i, v[i]);
}

static void logcall(CPU *c, const char *what, int nargs) {
    if (!ENV("RT_ASPECTLOG")) return;
    static int n;
    if (n++ > 4000) return;
    fprintf(stderr, "aspect: %s from %08X (%s):", what, c->lr - 4, fn_name(c->lr - 4));
    for (int i = 1; i <= nargs; i++) fprintf(stderr, " %g", c->f[i]);
    fprintf(stderr, "\n");
}

HLE(C_MTXFrustum) {
    logcall(c, "frustum", 6);
    float t = (float)c->f[1], b = (float)c->f[2], l = (float)c->f[3], r = (float)c->f[4], n = (float)c->f[5], f = (float)c->f[6];
    float ir = 1.0f / (r - l), it = 1.0f / (t - b), id = 1.0f / (f - n);
    float m[16] = {2.0f * n * ir, 0, ir * (r + l), 0,
                   0, 2.0f * n * it, it * (t + b), 0,
                   0, 0, -n * id, id * -(f * n),
                   0, 0, -1.0f, 0};
    put44(c->r[3], m);
}

HLE(C_MTXPerspective) {
    logcall(c, "perspective", 4);
    float fovy = (float)c->f[1], aspect = (float)c->f[2], n = (float)c->f[3], f = (float)c->f[4];
    float cot = 1.0f / tanf((float)M_PI / 180.0f * (0.5f * fovy)), id = 1.0f / (f - n);
    float m[16] = {cot / aspect, 0, 0, 0,
                   0, cot, 0, 0,
                   0, 0, -n * id, id * -(f * n),
                   0, 0, -1.0f, 0};
    put44(c->r[3], m);
}

HLE(C_MTXOrtho) {
    logcall(c, "ortho", 6);
    float t = (float)c->f[1], b = (float)c->f[2], l = (float)c->f[3], r = (float)c->f[4], n = (float)c->f[5], f = (float)c->f[6];
    float ir = 1.0f / (r - l), it = 1.0f / (t - b), id = 1.0f / (f - n);
    float m[16] = {2.0f * ir, 0, 0, ir * -(r + l),
                   0, 2.0f * it, 0, it * -(t + b),
                   0, 0, -1.0f * id, -f * id,
                   0, 0, 0, 1.0f};
    put44(c->r[3], m);
}
