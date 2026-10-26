/* Wii Remote: the Bluetooth stack (WUD/BTE) is skipped; WPAD's public API is answered from host
 * input. One remote is connected on channel 0. The KPAD library above runs as recompiled code. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "runtime.h"
#include "names.h"

#define HLE(name) void FN_##name(CPU *c)
#define STUB(name, val) HLE(name) { c->r[3] = (u32)(val); }

HostPad g_pad = {.acc_z = 1.0f, .ir_valid = 1};

static u32 connect_cb[4], ext_cb[4], sampling_cb[4], fmt[4];
static int announced;

/* scripted input for testing without a window: RT_INPUT="from-to:KEYS;..." in retrace ticks
 * (60 per second). KEYS: A B 1 2 + - H (home) U D L R (d-pad) S (swing); e.g. "2200-2205:A" */
static u64 ticks;
static void scripted_input(void) {
    const char *s = getenv("RT_INPUT");
    if (!s) return;
    u16 b = 0;
    int swing = 0;
    while (*s) {
        unsigned long a = strtoul(s, (char **)&s, 10), z = a;
        if (*s == '-') z = strtoul(s + 1, (char **)&s, 10);
        if (*s == ':') s++;
        int on = ticks >= a && ticks <= z;
        for (; *s && *s != ';'; s++) {
            if (!on) continue;
            switch (*s) {
            case 'A': b |= 0x0800; break; case 'B': b |= 0x0400; break;
            case '1': b |= 0x0200; break; case '2': b |= 0x0100; break;
            case '+': b |= 0x0010; break; case '-': b |= 0x1000; break;
            case 'H': b |= 0x8000; break;
            case 'U': b |= 0x0008; break; case 'D': b |= 0x0004; break;
            case 'L': b |= 0x0001; break; case 'R': b |= 0x0002; break;
            case 'S': swing = 1; break;
            }
        }
        if (*s == ';') s++;
    }
    g_pad.buttons = b;
    g_pad.acc_x = swing ? 3.5f : 0; g_pad.acc_z = swing ? 3.0f : 1.0f;
}

/* called on every VI retrace (interrupt context) */
void wpad_tick(CPU *c) {
    ticks++;
    scripted_input();
    if (!announced) {
        announced = 1;
        if (connect_cb[0])
            guest_call(c, connect_cb[0], 0, 0);   /* chan 0 connected, WPAD_ERR_NONE */
    }
    if (sampling_cb[0])
        guest_call(c, sampling_cb[0], 0, 0);
}

HLE(WPADInit) { (void)c; }
STUB(WUDGetStatus, 3)                      /* WPAD_LIB_STATUS_3: ready */
STUB(WUDInit, 1)
STUB(WUDIsBusy, 0)
HLE(WPADProbe) {
    s32 chan = (s32)c->r[3];
    u32 type = c->r[4];
    if (chan != 0) { c->r[3] = (u32)-1; return; }   /* WPAD_ERR_NO_CONTROLLER */
    if (type) mw32(type, 0);                        /* WPAD_DEV_CORE */
    c->r[3] = 0;
}
HLE(WPADSetConnectCallback) {
    s32 ch = (s32)c->r[3] & 3;
    u32 old = connect_cb[ch];
    connect_cb[ch] = c->r[4];
    if (ch == 0) announced = 0;
    c->r[3] = old;
}
HLE(WPADSetExtensionCallback) { s32 ch = (s32)c->r[3] & 3; u32 old = ext_cb[ch]; ext_cb[ch] = c->r[4]; c->r[3] = old; }
HLE(WPADSetSamplingCallback) { s32 ch = (s32)c->r[3] & 3; u32 old = sampling_cb[ch]; sampling_cb[ch] = c->r[4]; c->r[3] = old; }
HLE(WPADSetDataFormat) { fmt[c->r[3] & 3] = c->r[4]; c->r[3] = 0; }
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
HLE(WPADControlDpd) {
    u32 cb = c->r[5];
    s32 ch = (s32)c->r[3];
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
    mw8(info + 0x15, 1);    /* led */
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
    if (ch != 0) { mw8(st + 0x29, (u8)-1); return; }
    mw16(st + 0x0, g_pad.buttons);
    mw16(st + 0x2, (u16)(s16)(g_pad.acc_x * 100));
    mw16(st + 0x4, (u16)(s16)(g_pad.acc_y * 100));
    mw16(st + 0x6, (u16)(s16)(g_pad.acc_z * 100));
    if (g_pad.ir_valid) {
        /* two sensor-bar dots ~200 px apart around the pointed-at position (pointer moves opposite the dots) */
        float cx = 512 - g_pad.ir_x * 400, cy = 384 - g_pad.ir_y * 300;
        for (int k = 0; k < 2; k++) {
            u32 o = st + 0x8 + 8 * k;
            mw16(o + 0, (u16)(s16)(cx + (k ? 100 : -100)));
            mw16(o + 2, (u16)(s16)cy);
            mw16(o + 4, 4);    /* size */
            mw8(o + 6, (u8)k); /* traceID */
        }
    }
    mw8(st + 0x28, 0);   /* dev: core */
    mw8(st + 0x29, 0);   /* err: none */
}
