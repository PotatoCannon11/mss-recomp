/* Example mod: rainbow mode. Lit material and ambient colours (GXSetChanMatColor / GXSetChanAmbColor)
 * are tinted with a hue that cycles over time, so lit models shimmer through the rainbow.
 *   cc -O2 -shared -fPIC -I runtime -o build/rainbow.so mods/rainbow.c
 *   RT_MODS=build/rainbow.so ./build/mss ... */
#include <math.h>
#include "mod_api.h"

static const MssApi *api;
static float tint[3] = {1, 1, 1};

static void every_frame(void *user) {
    (void)user;
    float h = (float)(api->frame() % 240) / 240.0f * 6.2831853f;
    for (int i = 0; i < 3; i++) tint[i] = 0.5f + 0.5f * cosf(h - i * 2.0943951f);
}

/* GXSetChanMatColor / GXSetChanAmbColor(GXChannelID chan, const GXColor *color): r4 -> RGBA bytes */
static int on_color(CPU *c, void *user) {
    u32 a = *(u32 *)user;
    u32 p = c->r[4], orig = api->r32(p);
    u32 rgb = 0;
    for (int i = 0; i < 3; i++) {
        u32 ch = (orig >> (24 - 8 * i)) & 255;
        ch = (u32)(ch * tint[i] + 0.5f);
        rgb |= (ch > 255 ? 255 : ch) << (24 - 8 * i);
    }
    api->w32(p, rgb | (orig & 255));
    api->call_original(c, a);
    api->w32(p, orig);   /* the caller's colour stays as it was */
    return 1;
}

int mss_mod_init(const MssApi *a) {
    static u32 addr[2];
    static const char *fns[2] = {"GXSetChanMatColor", "GXSetChanAmbColor"};
    api = a;
    for (int i = 0; i < 2; i++) {
        addr[i] = api->sym(fns[i]);
        if (!addr[i] || api->hook(addr[i], on_color, &addr[i])) { api->log("rainbow: %s not found\n", fns[i]); return 1; }
    }
    api->on_frame(every_frame, 0);
    api->log("rainbow: hooked material and ambient colours\n");
    return 0;
}
