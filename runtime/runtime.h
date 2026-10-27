/* Host runtime interfaces shared by the runtime and HLE modules. */
#pragma once
#include <signal.h>
#include "cpu.h"

#define MEM1_SIZE 0x01800000u
#define MEM2_SIZE 0x04000000u
#define CPU_CLOCK 729000000u
#define BUS_CLOCK 243000000u
#define TB_CLOCK (BUS_CLOCK / 4)
#ifndef NAMES_PATH
#define NAMES_PATH "build/gen/names.txt"
#endif

extern CPU g_cpu;
const char *fn_name(u32 addr);
void rt_backtrace(void);
void hle_init(const char *disc_root);
int disc_init(const char *root);
u32 disc_load_fst(void);
u32 disc_fst_size(void);
u32 disc_read(u64 off, u32 dst, u32 len);
void guest_call(CPU *c, u32 fn, u32 a0, u32 a1);
void rt_set_dec(u32 v);
u32 rt_get_dec(void);

/* host input state for each Wii Remote channel (filled by the frontend; keyboard and mouse drive
 * channel 0, gamepads and real remotes the channel they claimed) */
#define MAX_PADS 4
typedef struct {
    u16 buttons;            /* WPAD_BUTTON_* bits */
    float acc_x, acc_y, acc_z; /* g */
    float ir_x, ir_y;       /* pointer, -1..1 */
    int ir_valid;
    int swing_t;            /* >0 while a swing gesture plays */
    float nc_x, nc_y;       /* Nunchuk stick, -1..1 (RT_NUNCHUK=1 attaches one) */
    int nc_c, nc_z;         /* Nunchuk buttons */
    int gesture;            /* 0 swing, 1 / 2 overhand throw (either sign) */
    float rest_y, rest_z;   /* resting gravity (flat: 0,1; pointing up: 1,0) */
} HostPad;
void rt_quit(int code);              /* flush recordings, exit immediately */
extern volatile sig_atomic_t g_quit_req;
/* a real Wii Remote through Linux hid-wiimote (wiimote.c) */
typedef struct {
    int present, have_accel, nunchuk;
    u16 buttons;
    s16 acc[3];              /* raw accelerometer minus 0x200, ~100 per g (WPAD units) */
    float nc_x, nc_y;
    int nc_c, nc_z;
} WiimoteState;
extern WiimoteState g_wm[MAX_PADS];   /* indexed by WPAD channel */
void wiimote_poll(void);
extern int g_widescreen;
extern float g_aspect_k;               /* surface aspect / game reference aspect */
void aspect_set_surface(int w, int h);   /* SYSCONF aspect is 16:9 (default; RT_43=1 for 4:3) */
void wpad_start_swing(void);      /* channel 0 */
void wpad_start_gesture(int g);   /* channel 0: 0 swing, 1 / 2 throw */
void wpad_start_gesture_ch(int ch, int g);
extern HostPad g_pads[MAX_PADS];
#define g_pad g_pads[0]
/* WPAD channels (players) for host controllers: the lowest free one, -1 if all four are taken */
int input_claim_channel(void);
void input_release_channel(int ch);
void wpad_tick(CPU *c);
void rt_timer_start(void);
void ios_init(void);
int ios_deliver(CPU *c);
int ios_pending(void);
void audio_frame_done(void);
int dvd_pending(void);
int dvd_deliver(CPU *c);
