/* Wii Remote: the Bluetooth stack (WUD/BTE) is skipped; WPAD's public API is answered from host
 * input. Channel 0 (player 1) is always connected and also takes the keyboard and mouse; gamepads and
 * real remotes each claim the lowest free channel when they appear and release it when they go, which
 * the game hears through its connect callbacks. The KPAD library above runs as recompiled code. */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "runtime.h"
#include "names.h"

#define HLE(name) void FN_##name(CPU *c)
#define STUB(name, val) HLE(name) { c->r[3] = (u32)(val); }

HostPad g_pads[MAX_PADS] = {
    [0] = {.acc_z = 1.0f, .rest_z = 1.0f, .ir_valid = 1},
    [1 ... MAX_PADS - 1] = {.acc_z = 1.0f, .rest_z = 1.0f},
};

static u32 connect_cb[4], ext_cb[4], sampling_cb[4], fmt[4];
static int announced[4], ext_announced[4];   /* what the game was last told: connected / Nunchuk attached */
#define WPAD_DEV_FREESTYLE 1

static pthread_mutex_t chan_mu = PTHREAD_MUTEX_INITIALIZER;
static int chan_taken[MAX_PADS];
int input_claim_channel(void) {
    pthread_mutex_lock(&chan_mu);
    int ch = -1;
    for (int i = 0; i < MAX_PADS && ch < 0; i++)
        if (!chan_taken[i]) { chan_taken[i] = 1; ch = i; }
    pthread_mutex_unlock(&chan_mu);
    return ch;
}
void input_release_channel(int ch) {
    if (ch < 0 || ch >= MAX_PADS) return;
    pthread_mutex_lock(&chan_mu);
    chan_taken[ch] = 0;
    pthread_mutex_unlock(&chan_mu);
    HostPad *p = &g_pads[ch];
    p->buttons = 0; p->nc_x = p->nc_y = 0; p->nc_c = p->nc_z = 0; p->swing_t = 0;
    if (ch) p->ir_valid = 0;
}
static int connected(int ch) { return ch == 0 || chan_taken[ch]; }
/* RT_NUNCHUK=1 gives keyboard / gamepad players a Nunchuk; a real remote has one when it's plugged in */
static int nunchuk(int ch) { return g_wm[ch].present ? g_wm[ch].nunchuk : ENV("RT_NUNCHUK") != NULL && connected(ch); }

/* scripted input for testing without a window: RT_INPUT="from-to:KEYS;..." in retrace ticks
 * (60 per second). KEYS: A B 1 2 + - H (home) U D L R (d-pad), S (swing), T / Y (overhand throw,
 * either sign), P / Q (hold pointing up / down), @x,y (point at x,y in -1..1), Nunchuk (RT_NUNCHUK=1):
 * < > ^ v stick, Z / C buttons; e.g. "2200-2205:A" */
static u64 ticks;
static void scripted_input(void) {
    const char *s = ENV("RT_INPUT");
    if (!s) return;
    u16 b = 0;
    int swing = 0, pose = 0, ptr = 0, nx = 0, ny = 0, nz = 0, ncb = 0;
    float px = 0, py = 0;
    while (*s) {
        unsigned long a = strtoul(s, (char **)&s, 10), z = a;
        if (*s == '-') z = strtoul(s + 1, (char **)&s, 10);
        if (*s == ':') s++;
        int on = ticks >= a && ticks <= z;
        for (; *s && *s != ';'; s++) {
            if (*s == '@') {   /* @x,y: point at x,y (-1..1) */
                float x = strtof(s + 1, (char **)&s), y = 0;
                if (*s == ',') y = strtof(s + 1, (char **)&s);
                if (on) { ptr = 1; px = x; py = y; }
                s--;
                continue;
            }
            if (!on) continue;
            switch (*s) {
            case 'A': b |= 0x0800; break; case 'B': b |= 0x0400; break;
            case '1': b |= 0x0200; break; case '2': b |= 0x0100; break;
            case '+': b |= 0x0010; break; case '-': b |= 0x1000; break;
            case 'H': b |= 0x8000; break;
            case 'U': b |= 0x0008; break; case 'D': b |= 0x0004; break;
            case 'L': b |= 0x0001; break; case 'R': b |= 0x0002; break;
            case 'S': swing = 1; break;
            case 'T': swing = 2; break; case 'Y': swing = 3; break;
            default: if (*s >= 'a' && *s <= 'n') swing = 4 + (*s - 'a'); break;   /* probe bursts */
            case 'P': pose = 1; break; case 'Q': pose = -1; break;
            case '<': nx = -1; break; case '>': nx = 1; break; case '^': ny = -1; break; case 'v': ny = 1; break;
            case 'Z': nz = 1; break; case 'C': ncb = 1; break;
            }
        }
        if (*s == ';') s++;
    }
    g_pad.buttons = b;
    g_pad.nc_x = (float)nx; g_pad.nc_y = (float)ny; g_pad.nc_z = nz; g_pad.nc_c = ncb;
    /* pointer, -1..1; without RT_IR it points off-screen so scripts navigate with the d-pad */
    g_pad.ir_valid = ENV("RT_IR") && sscanf(ENV("RT_IR"), "%f,%f", &g_pad.ir_x, &g_pad.ir_y) == 2;
    if (ptr) { g_pad.ir_valid = 1; g_pad.ir_x = px; g_pad.ir_y = py; }
    /* P / Q: hold the remote pointing up / down (gravity along the remote's length) instead of flat */
    g_pad.rest_y = (float)pose; g_pad.rest_z = pose ? 0.0f : 1.0f;
    if (swing && !g_pad.swing_t) { wpad_start_swing(); g_pad.gesture = swing - 1; }
}

/* a swing as the accelerometer sees it (in g, sampled per retrace): a fast sideways spike while the
 * remote accelerates, a reversal as it stops, then back to resting (1 g on z) */
static const float swing_x[] = {0.5f, 1.5f, 3.0f, 4.0f, 4.0f, 3.2f, 2.0f, 0.5f, -1.5f, -2.8f, -2.0f, -0.8f, 0.0f};
static const float swing_y[] = {0.2f, 0.5f, 1.0f, 1.5f, 1.2f, 0.8f, 0.4f, 0.0f, -0.4f, -0.6f, -0.3f, 0.0f, 0.0f};
static const float swing_z[] = {1.2f, 1.6f, 2.2f, 2.6f, 2.4f, 2.0f, 1.5f, 1.0f, 0.6f, 0.5f, 0.8f, 1.0f, 1.0f};
#define SWING_N (int)(sizeof swing_x / sizeof *swing_x)
/* a downward swing (pitching "Throw: swing down"), shaped for the game's two-step matcher
 * (fn_801649B0 with patterns at 0x80643770): first a strong drop along the face axis (KPAD y+), then a
 * push along the remote's length toward the tip (KPAD z+, no y+); added to the resting pose
 * (T, or Y with the face axis mirrored) */
static const float throw_y[] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.5f, 3.0f, 3.5f, 3.0f, 2.0f, 0.8f, 0.0f};
static const float throw_z[] = {-1.5f, -3.0f, -3.5f, -3.5f, -3.0f, -2.0f, 0.0f, 0.3f, 0.5f, 0.3f, 0.0f, 0.0f, 0.0f};
void wpad_start_gesture_ch(int ch, int g) { if (ch >= 0 && ch < MAX_PADS) { g_pads[ch].swing_t = 1; g_pads[ch].gesture = g; } }
void wpad_start_swing(void) { wpad_start_gesture_ch(0, 0); }
void wpad_start_gesture(int g) { wpad_start_gesture_ch(0, g); }
static void swing_step(HostPad *p) {
    if (!p->swing_t) return;
    int i = p->swing_t - 1;
    if (p->gesture == 0) {
        p->acc_x = swing_x[i]; p->acc_y = p->rest_y + swing_y[i]; p->acc_z = p->rest_z + swing_z[i] - 1.0f;
    } else if (p->gesture >= 3) {   /* probe: a 3 g bell-shaped burst along one sign combination of x, y, z */
        static const float bell[] = {0.2f, 0.5f, 0.8f, 1.0f, 1.0f, 0.8f, 0.5f, 0.2f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
        int m = p->gesture - 3;
        float b = 3.0f * bell[i];
        if (m < 8) {   /* a-h: diagonals */
            p->acc_x = (m & 1 ? -b : b);
            p->acc_y = p->rest_y + (m & 2 ? -b : b);
            p->acc_z = p->rest_z + (m & 4 ? -b : b);
        } else {       /* i-n: +x -x +y -y +z -z */
            int ax = (m - 8) / 2;
            float v = (m - 8) & 1 ? -b : b;
            p->acc_x = ax == 0 ? v : 0;
            p->acc_y = p->rest_y + (ax == 1 ? v : 0);
            p->acc_z = p->rest_z + (ax == 2 ? v : 0);
        }
    } else {
        float sz = p->gesture == 1 ? 1.0f : -1.0f;
        p->acc_x = 0; p->acc_y = p->rest_y + throw_y[i]; p->acc_z = p->rest_z + sz * throw_z[i];
    }
    if (++p->swing_t > SWING_N) p->swing_t = 0;
}

/* called on every VI retrace (interrupt context) */
void watch_tick(u64 ticks);
void mod_frame(void);
void wpad_tick(CPU *c) {
    ticks++;
    mod_frame();
    watch_tick(ticks);
    scripted_input();
    for (int ch = 0; ch < MAX_PADS; ch++) {
        HostPad *p = &g_pads[ch];
        p->acc_x = 0; p->acc_y = p->rest_y; p->acc_z = p->rest_z;
        swing_step(p);
    }
    wiimote_poll();
    for (int ch = 0; ch < MAX_PADS; ch++) {
        HostPad *p = &g_pads[ch];
        const WiimoteState *w = &g_wm[ch];
        if (!w->present) continue;
        /* a real remote: its buttons add to the others, its motion replaces the synthetic rest pose */
        if (w->have_accel && !p->swing_t) {
            p->acc_x = w->acc[0] / 100.0f; p->acc_y = w->acc[1] / 100.0f; p->acc_z = w->acc[2] / 100.0f;
        }
        if (w->nunchuk) { p->nc_x = w->nc_x; p->nc_y = w->nc_y; p->nc_c = w->nc_c; p->nc_z = w->nc_z; }
    }
    void mod_input(void);
    mod_input();
    for (int ch = 0; ch < MAX_PADS; ch++) {
        int conn = connected(ch);
        if (conn != announced[ch]) {
            announced[ch] = conn;
            ext_announced[ch] = 0;
            if (conn && ch) fprintf(stderr, "wpad: player %d connected\n", ch + 1);
            if (connect_cb[ch])
                guest_call(c, connect_cb[ch], (u32)ch, conn ? 0 : (u32)-1);   /* WPAD_ERR_NONE / NO_CONTROLLER */
        } else if (conn && nunchuk(ch) != ext_announced[ch] && ext_cb[ch]) {
            ext_announced[ch] = nunchuk(ch);
            guest_call(c, ext_cb[ch], (u32)ch, ext_announced[ch] ? WPAD_DEV_FREESTYLE : 0);   /* Nunchuk in / out */
        }
        if (conn && sampling_cb[ch])
            guest_call(c, sampling_cb[ch], (u32)ch, 0);
    }
}

HLE(WPADInit) { (void)c; }
STUB(WUDGetStatus, 3)                      /* WPAD_LIB_STATUS_3: ready */
STUB(WUDInit, 1)
STUB(WUDIsBusy, 0)
HLE(WPADProbe) {
    s32 chan = (s32)c->r[3];
    u32 type = c->r[4];
    if (chan < 0 || chan >= MAX_PADS || !connected(chan)) { c->r[3] = (u32)-1; return; }   /* WPAD_ERR_NO_CONTROLLER */
    if (type) mw32(type, nunchuk(chan) ? WPAD_DEV_FREESTYLE : 0);   /* WPAD_DEV_CORE / FREESTYLE */
    c->r[3] = 0;
}
HLE(WPADSetConnectCallback) {
    s32 ch = (s32)c->r[3] & 3;
    u32 old = connect_cb[ch];
    connect_cb[ch] = c->r[4];
    announced[ch] = 0;   /* (re)announce to the new callback */
    c->r[3] = old;
}
HLE(WPADSetExtensionCallback) { s32 ch = (s32)c->r[3] & 3; u32 old = ext_cb[ch]; ext_cb[ch] = c->r[4]; c->r[3] = old; ext_announced[ch] = 0; }
HLE(WPADSetSamplingCallback) { s32 ch = (s32)c->r[3] & 3; u32 old = sampling_cb[ch]; sampling_cb[ch] = c->r[4]; c->r[3] = old; }
HLE(WPADSetDataFormat) { fmt[c->r[3] & 3] = c->r[4]; c->r[3] = 0; }
/* WPADGetDataFormat: KPAD only computes acceleration and pointer data for formats that carry them */
HLE(fn_806022D0) { c->r[3] = fmt[c->r[3] & 3]; }
STUB(WPADSetAutoSamplingBuf, 0)
STUB(WPADControlMotor, 0)
STUB(WPADControlLed, 0)
STUB(WPADControlSpeaker, 0)
STUB(WPADSetSpeakerVolume, 0)
STUB(WPADIsSpeakerEnabled, 0)
STUB(WPADCanSendStreamData, 0)
STUB(WPADSendStreamData, 0)
STUB(WPADIsDpdEnabled, 1)
STUB(WPADDisconnect, 0)
STUB(WPADSaveConfig, 0)
static u32 dpd_cmd[4];
/* current DPD (pointer camera) mode, compared by KPAD against the mode it wants */
HLE(fn_80604A64) { c->r[3] = dpd_cmd[c->r[3] & 3]; }
HLE(WPADControlDpd) {
    u32 cb = c->r[5];
    s32 ch = (s32)c->r[3];
    dpd_cmd[ch & 3] = c->r[4];
    guest_call(c, cb, (u32)ch, 0);
    c->r[3] = 0;
}
HLE(WPADGetInfoAsync) {
    s32 ch = (s32)c->r[3];
    u32 info = c->r[4], cb = c->r[5];
    memset(MEM + info, 0, 0x18);
    mw32(info + 0x00, 1);   /* dpd */
    mw32(info + 0x04, 1);   /* speaker */
    mw8(info + 0x14, 4);    /* battery level */
    mw8(info + 0x15, (u8)(1 << (ch & 3)));   /* led: the player number */
    guest_call(c, cb, (u32)ch, 0);
    c->r[3] = 0;
}
HLE(WPADGetAccGravityUnit) {
    u32 acc = c->r[5];
    mw16(acc + 0, 100); mw16(acc + 2, 100); mw16(acc + 4, 100);
}

/* WPADStatus: button, acc xyz (10-bit, 512 = 0g), four IR objects (x 0..1023, y 0..767) */
HLE(WPADRead) {
    s32 ch = (s32)c->r[3];
    u32 st = c->r[4];
    memset(MEM + st, 0, 0x2A);
    if (ch < 0 || ch >= MAX_PADS || !connected(ch)) { mw8(st + 0x29, (u8)-1); return; }
    const HostPad *p = &g_pads[ch];
    mw16(st + 0x0, p->buttons | g_wm[ch].buttons);
    mw16(st + 0x2, (u16)(s16)(p->acc_x * 100));
    mw16(st + 0x4, (u16)(s16)(p->acc_y * 100));
    mw16(st + 0x6, (u16)(s16)(p->acc_z * 100));
    if (p->ir_valid) {
        /* two sensor-bar dots 360 px apart around the pointed-at position (dots move opposite the pointer);
         * KPAD only accepts dot pairs that put the remote within this game's distance limit (~1 m);
         * the y offset cancels KPAD's correction for the sensor bar sitting below the TV */
        float cx = 512 - p->ir_x * 225, cy = 486 - p->ir_y * 225;
        for (int k = 0; k < 2; k++) {
            u32 o = st + 0x8 + 8 * k;
            mw16(o + 0, (u16)(s16)(cx + (k ? 180 : -180)));
            mw16(o + 2, (u16)(s16)cy);
            mw16(o + 4, 4);    /* size */
            mw8(o + 6, (u8)k); /* traceID */
        }
    }
    if (ENV("RT_PADLOG") && ch == 0 && c->lr == 0x8050B6E0) {   /* debug: the game's KPADStatus (read just before) */
        u32 k = st - 1056;
        static int shown; if (!shown++) fprintf(stderr, "pad: game KPADStatus buffer for channel 0 at %08X\n", k);
        float f[8];
        for (int i = 0; i < 8; i++) { u32 v = mr32(k + 0xC + 4 * i); memcpy(&f[i], &v, 4); }
        float sx, sy;
        { u32 v = mr32(k + 0x60); memcpy(&sx, &v, 4); v = mr32(k + 0x64); memcpy(&sy, &v, 4); }
        fprintf(stderr, "pad t%llu hold %04X acc %.2f %.2f %.2f pos %.3f %.3f dpd_valid %d dev %d stick %.2f %.2f\n", (unsigned long long)ticks,
                mr32(k), f[0], f[1], f[2], f[5], f[6], (s8)mr8(k + 0x5E), mr8(k + 0x5C), sx, sy);
    }
    u16 btn = p->buttons | g_wm[ch].buttons;
    if (nunchuk(ch)) {
        if (p->nc_z) btn |= 0x2000;   /* WPAD_BUTTON_Z */
        if (p->nc_c) btn |= 0x4000;   /* WPAD_BUTTON_C */
        mw16(st + 0x0, btn);
        mw16(st + 0x2A, 0); mw16(st + 0x2C, 0); mw16(st + 0x2E, 100);   /* Nunchuk at rest, 1 g */
        mw8(st + 0x30, (u8)(s8)(p->nc_x * 100));
        mw8(st + 0x31, (u8)(s8)(-p->nc_y * 100));   /* stick up is +y */
        mw8(st + 0x28, WPAD_DEV_FREESTYLE);
    } else
        mw8(st + 0x28, 0);   /* dev: core */
    mw8(st + 0x29, 0);   /* err: none */
}
