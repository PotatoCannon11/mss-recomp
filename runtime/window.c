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
 * right stick = pointer, RT/RB = swing / pull up, LT = throw. Each gamepad (and each real Wii Remote,
 * wiimote.c) takes the lowest free player slot when it connects: the first is player 1 together with
 * the keyboard and mouse, the next player 2, and so on.
 *   Esc              quit */
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glext.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
static void (*SDL_GameControllerClose_)(void *);
static const char *(*SDL_GameControllerNameForIndex_)(int);
static s32 (*SDL_JoystickGetDeviceInstanceID_)(int);

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
    SYM(SDL_GameControllerClose); SYM(SDL_GameControllerNameForIndex); SYM(SDL_JoystickGetDeviceInstanceID);
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
typedef struct {
    void *h;
    s32 id;               /* joystick instance id */
    int ch;               /* WPAD channel (player - 1) */
    float px, py;         /* pointer driven by the right stick, -1..1 */
    int ptr_active;
    int swing_was, throw_was;
} Pad;
static Pad pads[MAX_PADS];

/* look for newly plugged controllers (about once a second) and drop unplugged ones */
static void gamepad_scan(void) {
    for (int j = 0; j < MAX_PADS; j++)
        if (pads[j].h && !SDL_GameControllerGetAttached_(pads[j].h)) {
            fprintf(stderr, "window: player %d controller unplugged\n", pads[j].ch + 1);
            SDL_GameControllerClose_(pads[j].h);
            input_release_channel(pads[j].ch);
            pads[j].h = NULL;
        }
    static int tick;
    if (tick++ % 60) return;
    for (int i = 0; i < SDL_NumJoysticks_(); i++) {
        if (!SDL_IsGameController_(i)) continue;
        s32 id = SDL_JoystickGetDeviceInstanceID_(i);
        int open = 0;
        for (int j = 0; j < MAX_PADS; j++) open |= pads[j].h && pads[j].id == id;
        if (open) continue;
        /* real Wii Remotes are read directly (wiimote.c); SDL would show the same remote again */
        const char *nm = SDL_GameControllerNameForIndex_(i);
        if (nm && !ENV("RT_NO_WIIMOTE") && (strstr(nm, "Wii Remote") || strstr(nm, "RVL-CNT"))) continue;
        Pad *p = NULL;
        for (int j = 0; j < MAX_PADS && !p; j++)
            if (!pads[j].h) p = &pads[j];
        if (!p) return;
        int ch = input_claim_channel();
        if (ch < 0) return;   /* four players already */
        if (!(p->h = SDL_GameControllerOpen_(i))) { input_release_channel(ch); continue; }
        p->id = id; p->ch = ch;
        p->px = p->py = 0; p->ptr_active = 0; p->swing_was = p->throw_was = 0;
        if (ch) g_pads[ch].ir_valid = 1, g_pads[ch].ir_x = g_pads[ch].ir_y = 0;
        fprintf(stderr, "window: player %d: controller %s\n", ch + 1, SDL_GameControllerName_(p->h));
    }
}

static void gamepad_poll(Pad *pd, u16 *b) {
    void *pad = pd->h;
    HostPad *hp = &g_pads[pd->ch];
    static const struct { int sdl; u16 wii; } map[] = {
        {0, 0x0800}, {1, 0x0400}, {2, 0x0200}, {3, 0x0100}, {4, 0x1000}, {5, 0x8000}, {6, 0x0010},
        {11, 0x0008}, {12, 0x0004}, {13, 0x0001}, {14, 0x0002},
    };
    for (unsigned i = 0; i < sizeof map / sizeof *map; i++)
        if (SDL_GameControllerGetButton_(pad, map[i].sdl)) *b |= map[i].wii;
    int lx = SDL_GameControllerGetAxis_(pad, 0), ly = SDL_GameControllerGetAxis_(pad, 1);
    if (ENV("RT_NUNCHUK")) {   /* left stick is the Nunchuk stick, LB = Z, left-stick click = C */
        if (lx * lx + ly * ly > 3000 * 3000) { hp->nc_x = lx / 32767.0f; hp->nc_y = ly / 32767.0f; }
        if (SDL_GameControllerGetButton_(pad, 9)) hp->nc_z = 1;
        if (SDL_GameControllerGetButton_(pad, 7)) hp->nc_c = 1;
    } else {
        if (lx < -16000) *b |= 0x0001;
        if (lx > 16000) *b |= 0x0002;
        if (ly > 16000) *b |= 0x0004;
        if (ly < -16000) *b |= 0x0008;
    }
    float rx = SDL_GameControllerGetAxis_(pad, 2) / 32767.0f, ry = SDL_GameControllerGetAxis_(pad, 3) / 32767.0f;
    if (ENV("RT_PADLOG") && (rx != 0 || ry != 0 || SDL_GameControllerGetAxis_(pad, 4) || SDL_GameControllerGetAxis_(pad, 5)))
        fprintf(stderr, "window: pad %d axes rx %.2f ry %.2f lt %d rt %d ptr %d %.2f %.2f\n", pd->ch + 1, rx, ry, SDL_GameControllerGetAxis_(pad, 4), SDL_GameControllerGetAxis_(pad, 5), pd->ptr_active, pd->px, pd->py);
    if (rx * rx + ry * ry > 0.04f) {   /* outside the dead zone: the stick drives the pointer */
        pd->ptr_active = 1;
        pd->px += rx * 0.035f;
        pd->py += ry * 0.035f;
        if (pd->px < -1) pd->px = -1; if (pd->px > 1) pd->px = 1;
        if (pd->py < -1) pd->py = -1; if (pd->py > 1) pd->py = 1;
    }
    int swing = SDL_GameControllerGetAxis_(pad, 5) > 16000 || SDL_GameControllerGetButton_(pad, 10);
    int thr = SDL_GameControllerGetAxis_(pad, 4) > 16000;
    if (swing && !pd->swing_was && !hp->swing_t) wpad_start_gesture_ch(pd->ch, 0);
    if (thr && !pd->throw_was && !hp->swing_t) wpad_start_gesture_ch(pd->ch, 1);
    pd->swing_was = swing;
    pd->throw_was = thr;
}

void window_poll(void) {
    if (!win) return;
    /* skip this poll while audio is opening: with sdl2-compat (SDL3) pumping events takes the audio
     * lock that a PipeWire device open holds while it waits, and the two threads deadlock */
    if (pthread_mutex_trylock(&g_sdl_lock)) return;
    u8 ev[64];
    while (SDL_PollEvent_(ev)) {
        u32 type = *(u32 *)ev;
        if (type == 0x100) rt_quit(0);   /* SDL_QUIT */
    }
    pthread_mutex_unlock(&g_sdl_lock);
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
    if (k[13]) b |= 0x0800;   /* J: A */
    if (k[14]) b |= 0x0400;   /* K: B */
    if (k[5]) b |= 0x0C00;    /* B: A+B together (trackpads can't hold both mouse buttons) */
    if (k[42]) b |= 0x1000;   /* Backspace: - */
    if (k[11]) b |= 0x8000;   /* H: HOME */
    /* Nunchuk (RT_NUNCHUK=1): WASD stick, Q = Z, E = C */
    g_pad.nc_x = (k[7] ? 1.0f : 0) - (k[4] ? 1.0f : 0);
    g_pad.nc_y = (k[22] ? 1.0f : 0) - (k[26] ? 1.0f : 0);
    g_pad.nc_z = k[20];
    g_pad.nc_c = k[8];
    for (int ch = 1; ch < MAX_PADS; ch++) { g_pads[ch].nc_x = g_pads[ch].nc_y = 0; g_pads[ch].nc_z = g_pads[ch].nc_c = 0; }
    u16 chb[MAX_PADS] = {b};   /* keyboard and mouse are player 1 */
    Pad *p1 = NULL;            /* a gamepad that is player 1 shares the pointer with the mouse */
    if (SDL_GameControllerGetButton_) {
        gamepad_scan();
        if (SDL_GameControllerUpdate_) SDL_GameControllerUpdate_();
        for (int j = 0; j < MAX_PADS; j++) {
            Pad *pd = &pads[j];
            if (!pd->h) continue;
            gamepad_poll(pd, &chb[pd->ch]);
            if (pd->ch == 0) p1 = pd;
            else { g_pads[pd->ch].ir_x = pd->px; g_pads[pd->ch].ir_y = pd->py; g_pads[pd->ch].ir_valid = 1; }
        }
    }
    for (int ch = 0; ch < MAX_PADS; ch++) g_pads[ch].buttons = chb[ch];
    static int lmx = -1, lmy = -1;
    int mouse_moved = mx != lmx || my != lmy;
    lmx = mx; lmy = my;
    if (p1 && mouse_moved) p1->ptr_active = 0;   /* the mouse takes the pointer back */
    if (p1 && p1->ptr_active) {
        g_pad.ir_x = p1->px;
        g_pad.ir_y = p1->py;
    } else {
        g_pad.ir_x = ww ? 2.0f * mx / ww - 1.0f : 0;
        g_pad.ir_y = wh ? 2.0f * my / wh - 1.0f : 0;
        if (p1) { p1->px = g_pad.ir_x; p1->py = g_pad.ir_y; }
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
    /* mods draw overlays here: window framebuffer bound, viewport = whole window */
    void mod_present(unsigned long long frame, int w, int h);
    extern GPUState g_gpu;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, ww, wh);
    mod_present(g_gpu.frames, ww, wh);
    SDL_GL_SwapWindow_(win);
}
