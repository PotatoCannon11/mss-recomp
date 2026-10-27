/* Mod API: native mods are shared libraries loaded at startup (RT_MODS=a.so:b.so, or every .so in
 * ~/.local/share/mss-recomp/mods/). Each exports
 *
 *     int mss_mod_init(const MssApi *api);   // return 0 on success
 *
 * Guest memory is big-endian; use the accessors. Function hooks run on the game thread at the
 * guest function's entry with its registers (arguments in r3..r10 / f1..f8): return 1 to skip the
 * original (set c->r[3] / c->f[1] for a return value), 0 to let it run. Inside a hook,
 * api->call_original(c, addr) runs the unhooked function (e.g. to post-process its result).
 * Frame callbacks run on the game thread once per retrace (60 Hz), where poking memory is safe.
 *
 *     cc -O2 -shared -fPIC -I runtime -o mymod.so mymod.c
 */
#pragma once
#include "cpu.h"

#define MSS_MOD_API_VERSION 1

typedef int (*MssHook)(CPU *c, void *user);
typedef void (*MssFrameFn)(void *user);

typedef struct MssApi {
    int version;
    u8 *mem;                                   /* guest memory base (MEM + guest address) */
    u32 (*r32)(u32 addr);
    u16 (*r16)(u32 addr);
    u8 (*r8)(u32 addr);
    float (*rf)(u32 addr);
    void (*w32)(u32 addr, u32 v);
    void (*w16)(u32 addr, u16 v);
    void (*w8)(u32 addr, u8 v);
    void (*wf)(u32 addr, float v);
    u32 (*sym)(const char *name);              /* function/data address by symbol name, 0 if unknown */
    const char *(*name_of)(u32 addr);          /* symbol containing addr */
    int (*hook)(u32 addr, MssHook fn, void *user);   /* 0 on success; one hook per function */
    void (*call_original)(CPU *c, u32 addr);   /* run a guest function, bypassing its hook */
    void (*call)(CPU *c, u32 addr);            /* run a guest function (hooks apply) */
    void (*on_frame)(MssFrameFn fn, void *user);
    unsigned long long (*frame)(void);         /* retrace counter */
    void (*log)(const char *fmt, ...);
} MssApi;
