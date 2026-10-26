/* Window, presentation and input through SDL2, loaded at run time (no SDL headers needed).
 * Without a display (or without libSDL2) the renderer runs headless instead.
 *
 * Controls (Wii Remote on channel 0):
 *   mouse            pointer           left / right button   A / B
 *   arrow keys       d-pad             Z / X                 1 / 2
 *   Enter            +                 Backspace             -
 *   H                HOME              Space                 swing / pitch wind-up (pull up)
 *   C                pitch: throw (swing down)
 *   Esc              quit */
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glext.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include "runtime.h"
#include "gpu.h"

static void *sdl, *win;
static int (*SDL_Init_)(u32);
static void *(*SDL_CreateWindow_)(const char *, int, int, int, int, u32);
static void *(*SDL_GL_CreateContext_)(void *);
static int (*SDL_GL_SetAttribute_)(int, int);
static int (*SDL_GL_SetSwapInterval_)(int);
static void (*SDL_GL_SwapWindow_)(void *);
static int (*SDL_PollEvent_)(void *);
static const u8 *(*SDL_GetKeyboardState_)(int *);
static u32 (*SDL_GetMouseState_)(int *, int *);
static void (*SDL_GetWindowSize_)(void *, int *, int *);
static const char *(*SDL_GetError_)(void);

#define SYM(n) (*(void **)&n##_ = dlsym(sdl, #n))

int gfx_window_init(int w, int h) {
    if (ENV("RT_HEADLESS") || (!ENV("DISPLAY") && !ENV("WAYLAND_DISPLAY")))
        return 0;
    sdl = dlopen("libSDL2-2.0.so.0", RTLD_NOW);
    if (!sdl) { fprintf(stderr, "window: libSDL2 not found, running headless\n"); return 0; }
    if (!SYM(SDL_Init) || !SYM(SDL_CreateWindow) || !SYM(SDL_GL_CreateContext) || !SYM(SDL_GL_SetAttribute) ||
        !SYM(SDL_GL_SetSwapInterval) || !SYM(SDL_GL_SwapWindow) || !SYM(SDL_PollEvent) || !SYM(SDL_GetKeyboardState) ||
        !SYM(SDL_GetMouseState) || !SYM(SDL_GetWindowSize) || !SYM(SDL_GetError))
        return 0;
    if (SDL_Init_(0x20 | 0x2000) != 0) { fprintf(stderr, "window: %s\n", SDL_GetError_()); return 0; }
    SDL_GL_SetAttribute_(17, 3);   /* SDL_GL_CONTEXT_MAJOR_VERSION */
    SDL_GL_SetAttribute_(18, 3);   /* MINOR */
    SDL_GL_SetAttribute_(21, 1);   /* PROFILE_MASK: core */
    SDL_GL_SetAttribute_(5, 1);    /* DOUBLEBUFFER */
    win = SDL_CreateWindow_("Mario Super Sluggers", 0x2FFF0000, 0x2FFF0000, ENV("RT_43") ? 960 : 1280, 720, 0x2 | 0x20);   /* before the NAND sets g_widescreen */
    if (!win || !SDL_GL_CreateContext_(win)) { fprintf(stderr, "window: %s\n", SDL_GetError_()); return 0; }
    SDL_GL_SetSwapInterval_(0);   /* pacing comes from the emulated VI */
    return 1;
}


void window_poll(void) {
    if (!win) return;
    u8 ev[64];
    while (SDL_PollEvent_(ev)) {
        u32 type = *(u32 *)ev;
        if (type == 0x100) exit(0);   /* SDL_QUIT */
    }
    const u8 *k = SDL_GetKeyboardState_(NULL);
    int mx, my, ww, wh;
    u32 mb = SDL_GetMouseState_(&mx, &my);
    SDL_GetWindowSize_(win, &ww, &wh);
    if (k[41]) exit(0);   /* Esc */
    u16 b = 0;
    if (k[80]) b |= 0x0001;   /* left */
    if (k[79]) b |= 0x0002;   /* right */
    if (k[81]) b |= 0x0004;   /* down */
    if (k[82]) b |= 0x0008;   /* up */
    if (k[40]) b |= 0x0010;   /* Enter: + */
    if (k[27]) b |= 0x0100;   /* X: 2 */
    if (k[29]) b |= 0x0200;   /* Z: 1 */
    if (mb & 4) b |= 0x0400;  /* right mouse: B */
    if (mb & 1) b |= 0x0800;  /* left mouse: A */
    if (k[42]) b |= 0x1000;   /* Backspace: - */
    if (k[11]) b |= 0x8000;   /* H: HOME */
    g_pad.buttons = b;
    g_pad.ir_x = ww ? 2.0f * mx / ww - 1.0f : 0;
    g_pad.ir_y = wh ? 2.0f * my / wh - 1.0f : 0;
    g_pad.ir_valid = 1;
    if (ENV("RT_PADLOG")) { static int n; if (++n % 60 == 0) fprintf(stderr, "window: mouse %d,%d buttons %x win %dx%d\n", mx, my, mb, ww, wh); }
    static int space_was, c_was;
    if (k[44] && !space_was && !g_pad.swing_t) wpad_start_swing();   /* Space: swing */
    if (k[6] && !c_was && !g_pad.swing_t) wpad_start_gesture(1);     /* C: pitch throw */
    space_was = k[44];
    c_was = k[6];
}

void window_present(unsigned efb_tex, int sx, int sy, int sw, int sh, int efb_w, int efb_h) {
    if (!win) return;
    static GLuint rfbo;
    if (!rfbo) glGenFramebuffers(1, &rfbo);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, rfbo);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, efb_tex, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    int ww, wh;
    SDL_GetWindowSize_(win, &ww, &wh);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    /* keep the game's aspect (16:9 or 4:3), letterboxed */
    int an = g_widescreen ? 16 : 4, ad = g_widescreen ? 9 : 3;
    int dw = ww, dh = ww * ad / an;
    if (dh > wh) { dh = wh; dw = wh * an / ad; }
    int dx = (ww - dw) / 2, dy = (wh - dh) / 2;
    glBlitFramebuffer(sx, efb_h - sy, sx + sw, efb_h - (sy + sh), dx, dy + dh, dx + dw, dy, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    (void)efb_w;
    SDL_GL_SwapWindow_(win);
}
