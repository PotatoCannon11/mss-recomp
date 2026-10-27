/* Example mod (input hook): "auto swing" assist. Pressing 2 swings the bat for you - a one-button
 * swing for players without motion controls. Also shows on_present (logs the frame size once).
 *   make mods && RT_MODS=build/mods/autoswing.so ./build/mss ... */
#include "mod_api.h"

static const MssApi *api;
static int t;   /* > 0 while the injected swing plays */
static const float sx[] = {0.5f, 1.5f, 3.0f, 4.0f, 4.0f, 3.2f, 2.0f, 0.5f, -1.5f, -2.8f, -2.0f, -0.8f, 0.0f};

static void input(MssPad *p, void *user) {
    (void)user;
    static int was;
    int down = (p->buttons & 0x0100) != 0;   /* 2 */
    if (down && !was && !t) t = 1;
    was = down;
    p->buttons &= (u16)~0x0100;               /* the game doesn't see the 2 press */
    if (t) {
        p->acc_x = sx[t - 1];
        p->acc_z = 1.0f + (t < 8 ? 1.0f : 0.0f);
        if (++t > (int)(sizeof sx / sizeof *sx)) t = 0;
    }
}

static void present(unsigned long long frame, int w, int h, void *user) {
    (void)user;
    static int shown;
    if (!shown++) api->log("autoswing: first frame %llu at %dx%d internal\n", frame, w, h);
}

int mss_mod_init(const MssApi *a) {
    api = a;
    if (a->version < 2) return 1;
    a->on_input(input, 0);
    a->on_present(present, 0);
    a->log("autoswing: press 2 to swing\n");
    return 0;
}
