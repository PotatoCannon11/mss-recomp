/* Example mod (present hook): a frame-time bar in the top-left corner, drawn with plain GL after the
 * game's frame and before it is shown. Green = 60 fps, growing red as frames take longer.
 *   make mods && RT_MODS=build/mods/fpsbar.so ./build/mss ... */
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <time.h>
#include "mod_api.h"

static void present(unsigned long long frame, int w, int h, void *user) {
    (void)frame; (void)w; (void)user;
    static struct timespec last;
    static double avg = 1.0 / 60;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (last.tv_sec) {
        double dt = (now.tv_sec - last.tv_sec) + (now.tv_nsec - last.tv_nsec) / 1e9;
        avg += (dt - avg) * 0.1;
    }
    last = now;
    double slow = avg * 60.0 - 1.0;   /* 0 at 60 fps */
    if (slow < 0) slow = 0;
    if (slow > 1) slow = 1;
    int len = (int)(120 + 240 * slow), hgt = 10;
    glEnable(GL_SCISSOR_TEST);
    glScissor(10, h - 10 - hgt, len, hgt);
    glClearColor((float)slow, (float)(1.0 - slow), 0.1f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_SCISSOR_TEST);
}

int mss_mod_init(const MssApi *a) {
    if (a->version < 2) return 1;
    a->on_present(present, 0);
    a->log("fpsbar: frame-time bar enabled\n");
    return 0;
}
