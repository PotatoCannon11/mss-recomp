/* Host runtime interfaces shared by the runtime and HLE modules. */
#pragma once
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

/* host input state for the Wii Remote on channel 0 (filled by the frontend) */
typedef struct {
    u16 buttons;            /* WPAD_BUTTON_* bits */
    float acc_x, acc_y, acc_z; /* g */
    float ir_x, ir_y;       /* pointer, -1..1 */
    int ir_valid;
    int swing_t;            /* >0 while a swing gesture plays */
    int gesture;            /* 0 swing, 1 / 2 overhand throw (either sign) */
    float rest_y, rest_z;   /* resting gravity (flat: 0,1; pointing up: 1,0) */
} HostPad;
void wpad_start_swing(void);
extern HostPad g_pad;
void wpad_tick(CPU *c);
void rt_timer_start(void);
void ios_init(void);
int ios_deliver(CPU *c);
int ios_pending(void);
void audio_frame_done(void);
int dvd_pending(void);
int dvd_deliver(CPU *c);
