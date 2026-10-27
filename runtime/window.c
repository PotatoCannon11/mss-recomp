/* Window, presentation and input through SDL2, loaded at run time (no SDL headers needed).
 * Without a display (or without libSDL2) the renderer runs headless instead.
 *
 * Controls (Wii Remote on channel 0):
 *   mouse            pointer           left / right button   A / B
 *   arrow keys       d-pad             Z / X                 1 / 2
 *   Enter            +                 Backspace             -
 *   H                HOME              Space                 swing / pitch wind-up (pull up)
 *   C                pitch: throw (swing down)
 * Gamepads: A/B = A/B, X/Y = 1/2, Start/Back/Guide = +/-/HOME, d-pad or left stick = d-pad,
 * right stick = pointer, RT/RB = swing / pull up, LT = throw
 *   Esc              quit */
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glext.h>
#include <dlfcn.h>
#include <pthread.h>
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
/* game controllers (optional: resolved separately so a missing symbol doesn't disable the window) */
static int (*SDL_NumJoysticks_)(void);
static int (*SDL_IsGameController_)(int);
static void *(*SDL_GameControllerOpen_)(int);
static u8 (*SDL_GameControllerGetAttached_)(void *);
static u8 (*SDL_GameControllerGetButton_)(void *, int);
static short (*SDL_GameControllerGetAxis_)(void *, int);
static const char *(*SDL_GameControllerName_)(void *);
static void (*SDL_GameControllerUpdate_)(void);

#define SYM(n) (*(void **)&n##_ = dlsym(sdl, #n))

/* SDL's global init isn't safe to run concurrently: video (GPU thread) and audio (game thread)
 * initialise under this lock */
pthread_mutex_t g_sdl_lock = PTHREAD_MUTEX_INITIALIZER;
static int window_init_locked(int w, int h);
void window_size(int *w, int *h) { *w = *h = 0; if (win) SDL_GetWindowSize_(win, w, h); }
int gfx_window_init(int w, int h) {
    pthread_mutex_lock(&g_sdl_lock);
    int r = window_init_locked(w, h);
    pthread_mutex_unlock(&g_sdl_lock);
    return r;
}
static int window_init_locked(int w, int h) {
    if (ENV("RT_HEADLESS") || (!ENV("DISPLAY") && !ENV("WAYLAND_DISPLAY")))
        return 0;
    sdl = dlopen("libSDL2-2.0.so.0", RTLD_NOW);
    if (!sdl) { fprintf(stderr, "window: libSDL2 not found, running headless\n"); return 0; }
    if (!SYM(SDL_Init) || !SYM(SDL_CreateWindow) || !SYM(SDL_GL_CreateContext) || !SYM(SDL_GL_SetAttribute) ||
        !SYM(SDL_GL_SetSwapInterval) || !SYM(SDL_GL_SwapWindow) || !SYM(SDL_PollEvent) || !SYM(SDL_GetKeyboardState) ||
        !SYM(SDL_GetMouseState) || !SYM(SDL_GetWindowSize) || !SYM(SDL_GetError))
        return 0;
    if (SDL_Init_(0x20 | 0x2000) != 0) { fprintf(stderr, "window: %s\n", SDL_GetError_()); return 0; }
    SYM(SDL_NumJoysticks); SYM(SDL_IsGameController); SYM(SDL_GameControllerOpen); SYM(SDL_GameControllerGetAttached);
    SYM(SDL_GameControllerGetButton); SYM(SDL_GameControllerGetAxis); SYM(SDL_GameControllerName); SYM(SDL_GameControllerUpdate);
    SDL_GL_SetAttribute_(17, 3);   /* SDL_GL_CONTEXT_MAJOR_VERSION */
    SDL_GL_SetAttribute_(18, 3);   /* MINOR */
    SDL_GL_SetAttribute_(21, 1);   /* PROFILE_MASK: core */
    SDL_GL_SetAttribute_(5, 1);    /* DOUBLEBUFFER */
    u32 flags = 0x2 | 0x20;   /* OPENGL | RESIZABLE */
    if (ENV("RT_FULLSCREEN")) flags |= 0x1001;   /* FULLSCREEN_DESKTOP: the monitor's own resolution and shape */
    win = SDL_CreateWindow_("Mario Super Sluggers", 0x2FFF0000, 0x2FFF0000, ENV("RT_43") ? 960 : 1280, 720, flags);
    if (!win || !SDL_GL_CreateContext_(win)) { fprintf(stderr, "window: %s\n", SDL_GetError_()); return 0; }
    SDL_GL_SetSwapInterval_(0);   /* pacing comes from the emulated VI */
    return 1;
}


/* gamepad -> Wii Remote: A/B = A/B, X/Y = 1/2, Start/Back/Guide = +/-/HOME, d-pad or left stick =
 * d-pad, right stick moves the pointer, RT/RB = swing (and pitching pull-up), LT = throw */
static void *pad;
static float pad_px, pad_py;   /* pointer driven by the right stick, -1..1 */
static int pad_ptr_active;
static void gamepad_poll(u16 *b) {
    if (!SDL_GameControllerGetButton_) return;
    if (pad && !SDL_GameControllerGetAttached_(pad)) pad = NULL;
    if (!pad) {
        static int tick;
        if (tick++ % 60) return;   /* look for a controller about once a second */
        for (int i = 0; i < SDL_NumJoysticks_(); i++)
            if (SDL_IsGameController_(i) && (pad = SDL_GameControllerOpen_(i))) {
                fprintf(stderr, "window: controller %s\n", SDL_GameControllerName_(pad));
                break;
            }
        if (!pad) return;
    }
    SDL_GameControllerUpdate_();
    static const struct { int sdl; u16 wii; } map[] = {
        {0, 0x0800}, {1, 0x0400}, {2, 0x0200}, {3, 0x0100}, {4, 0x1000}, {5, 0x8000}, {6, 0x0010},
        {11, 0x0008}, {12, 0x0004}, {13, 0x0001}, {14, 0x0002},
    };
    for (unsigned i = 0; i < sizeof map / sizeof *map; i++)
        if (SDL_GameControllerGetButton_(pad, map[i].sdl)) *b |= map[i].wii;
    int lx = SDL_GameControllerGetAxis_(pad, 0), ly = SDL_GameControllerGetAxis_(pad, 1);
    if (ENV("RT_NUNCHUK")) {   /* left stick is the Nunchuk stick, LB = Z, left-stick click = C */
        if (lx * lx + ly * ly > 3000 * 3000) { g_pad.nc_x = lx / 32767.0f; g_pad.nc_y = ly / 32767.0f; }
        if (SDL_GameControllerGetButton_(pad, 9)) g_pad.nc_z = 1;
        if (SDL_GameControllerGetButton_(pad, 7)) g_pad.nc_c = 1;
    } else {
        if (lx < -16000) *b |= 0x0001;
        if (lx > 16000) *b |= 0x0002;
        if (ly > 16000) *b |= 0x0004;
        if (ly < -16000) *b |= 0x0008;
    }
    float rx = SDL_GameControllerGetAxis_(pad, 2) / 32767.0f, ry = SDL_GameControllerGetAxis_(pad, 3) / 32767.0f;
    if (ENV("RT_PADLOG") && (rx != 0 || ry != 0 || SDL_GameControllerGetAxis_(pad, 4) || SDL_GameControllerGetAxis_(pad, 5)))
        fprintf(stderr, "window: pad axes rx %.2f ry %.2f lt %d rt %d ptr %d %.2f %.2f\n", rx, ry, SDL_GameControllerGetAxis_(pad, 4), SDL_GameControllerGetAxis_(pad, 5), pad_ptr_active, pad_px, pad_py);
    if (rx * rx + ry * ry > 0.04f) {   /* outside the dead zone: the stick drives the pointer */
        pad_ptr_active = 1;
        pad_px += rx * 0.035f;
        pad_py += ry * 0.035f;
        if (pad_px < -1) pad_px = -1; if (pad_px > 1) pad_px = 1;
        if (pad_py < -1) pad_py = -1; if (pad_py > 1) pad_py = 1;
    }
    static int swing_was, throw_was;
    int swing = SDL_GameControllerGetAxis_(pad, 5) > 16000 || SDL_GameControllerGetButton_(pad, 10);
    int thr = SDL_GameControllerGetAxis_(pad, 4) > 16000;
    if (swing && !swing_was && !g_pad.swing_t) wpad_start_swing();
    if (thr && !throw_was && !g_pad.swing_t) wpad_start_gesture(1);
    swing_was = swing;
    throw_was = thr;
}

void window_poll(void) {
    if (!win) return;
    u8 ev[64];
    while (SDL_PollEvent_(ev)) {
        u32 type = *(u32 *)ev;
        if (type == 0x100) rt_quit(0);   /* SDL_QUIT */
    }
    const u8 *k = SDL_GetKeyboardState_(NULL);
    int mx, my, ww, wh;
    u32 mb = SDL_GetMouseState_(&mx, &my);
    SDL_GetWindowSize_(win, &ww, &wh);
    if (k[41]) rt_quit(0);   /* Esc */
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
    /* Nunchuk (RT_NUNCHUK=1): WASD stick, Q = Z, E = C */
    g_pad.nc_x = (k[7] ? 1.0f : 0) - (k[4] ? 1.0f : 0);
    g_pad.nc_y = (k[22] ? 1.0f : 0) - (k[26] ? 1.0f : 0);
    g_pad.nc_z = k[20];
    g_pad.nc_c = k[8];
    gamepad_poll(&b);
    g_pad.buttons = b;
    static int lmx = -1, lmy = -1;
    if (mx != lmx || my != lmy) { pad_ptr_active = 0; lmx = mx; lmy = my; }   /* the mouse takes the pointer back */
    if (pad_ptr_active) {
        g_pad.ir_x = pad_px;
        g_pad.ir_y = pad_py;
    } else {
        g_pad.ir_x = ww ? 2.0f * mx / ww - 1.0f : 0;
        g_pad.ir_y = wh ? 2.0f * my / wh - 1.0f : 0;
        pad_px = g_pad.ir_x;
        pad_py = g_pad.ir_y;
    }
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
    /* the frame is rendered for this window's shape (dynamic aspect): fill it, no bars */
    int dw = ww, dh = wh, dx = 0, dy = 0;
    glBlitFramebuffer(sx, efb_h - sy, sx + sw, efb_h - (sy + sh), dx, dy + dh, dx + dw, dy, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    (void)efb_w;
    SDL_GL_SwapWindow_(win);
}
