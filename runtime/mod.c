/* Native mod loader and function hooks (see mod_api.h). Generated functions check one bit of
 * g_hookbits at entry (FN_ENTER); only hooked functions take the slow path through mod_enter. */
#include <dirent.h>
#include <dlfcn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "runtime.h"
#include "mod_api.h"

u8 g_hookbits[1 << 19];   /* one bit per instruction word of 0x80000000..0x80FFFFFF */

#define MAX_HOOKS 1024
static struct { u32 addr; MssHook fn; void *user; } hooks[MAX_HOOKS];
static int nhooks;
static u32 bypass;   /* call_original: the next entry of this function skips its hook */

int mod_enter(CPU *c, u32 addr) {
    if (bypass == addr) { bypass = 0; return 0; }
    for (int i = 0; i < nhooks; i++)
        if (hooks[i].addr == addr) return hooks[i].fn(c, hooks[i].user);
    return 0;
}

#define MAX_FRAME 64
static struct { MssFrameFn fn; void *user; } frames[MAX_FRAME];
static int nframes;
static unsigned long long frame_count;

/* called once per retrace on the game thread */
void mod_frame(void) {
    frame_count++;
    for (int i = 0; i < nframes; i++) frames[i].fn(frames[i].user);
}

#define MAX_CB 32
static struct { MssInputFn fn; void *user; } inputs[MAX_CB];
static int ninputs;
static struct { MssPresentFn fn; void *user; } presents[MAX_CB];
static int npresents;

/* game thread, once per tick after the host input is gathered */
void mod_input(void) {
    if (!ninputs) return;
    MssPad p = {g_pad.buttons, g_pad.acc_x, g_pad.acc_y, g_pad.acc_z, g_pad.ir_x, g_pad.ir_y, g_pad.ir_valid,
                g_pad.nc_x, g_pad.nc_y, g_pad.nc_c, g_pad.nc_z};
    for (int i = 0; i < ninputs; i++) inputs[i].fn(&p, inputs[i].user);
    g_pad.buttons = p.buttons; g_pad.acc_x = p.acc_x; g_pad.acc_y = p.acc_y; g_pad.acc_z = p.acc_z;
    g_pad.ir_x = p.ir_x; g_pad.ir_y = p.ir_y; g_pad.ir_valid = p.ir_valid;
    g_pad.nc_x = p.nc_x; g_pad.nc_y = p.nc_y; g_pad.nc_c = p.nc_c; g_pad.nc_z = p.nc_z;
}
/* GPU thread, after a frame was presented */
void mod_present(unsigned long long frame, int w, int h) {
    for (int i = 0; i < npresents; i++) presents[i].fn(frame, w, h, presents[i].user);
}
static void a_on_input(MssInputFn fn, void *user) { if (ninputs < MAX_CB) { inputs[ninputs].fn = fn; inputs[ninputs].user = user; ninputs++; } }
static void a_on_present(MssPresentFn fn, void *user) { if (npresents < MAX_CB) { presents[npresents].fn = fn; presents[npresents].user = user; npresents++; } }

static u32 a_r32(u32 a) { return mr32(a); }
static u16 a_r16(u32 a) { return mr16(a); }
static u8 a_r8(u32 a) { return mr8(a); }
static float a_rf(u32 a) { u32 v = mr32(a); float f; memcpy(&f, &v, 4); return f; }
static void a_w32(u32 a, u32 v) { mw32(a, v); }
static void a_w16(u32 a, u16 v) { mw16(a, v); }
static void a_w8(u32 a, u8 v) { mw8(a, v); }
static void a_wf(u32 a, float f) { u32 v; memcpy(&v, &f, 4); mw32(a, v); }

static u32 a_sym(const char *name) {
    FILE *f = fopen(NAMES_PATH, "r");
    char line[512];
    u32 found = 0;
    while (f && !found && fgets(line, sizeof line, f)) {
        u32 a;
        char nm[480];
        if (sscanf(line, "%x %479s", &a, nm) == 2 && !strcmp(nm, name)) found = a;
    }
    if (f) fclose(f);
    return found;
}

static int a_hook(u32 addr, MssHook fn, void *user) {
    if (nhooks == MAX_HOOKS || (addr & 3) || addr < 0x80000000u || addr >= 0x81000000u || !lookup_fn(addr)) return -1;
    for (int i = 0; i < nhooks; i++) if (hooks[i].addr == addr) return -1;
    hooks[nhooks].addr = addr; hooks[nhooks].fn = fn; hooks[nhooks].user = user;
    nhooks++;
    u32 idx = (addr - 0x80000000u) >> 2;
    g_hookbits[idx >> 3] |= (u8)(1u << (idx & 7));
    return 0;
}

static void a_call_original(CPU *c, u32 addr) { bypass = addr; call_indirect(c, addr); }
static void a_call(CPU *c, u32 addr) { call_indirect(c, addr); }
static void a_on_frame(MssFrameFn fn, void *user) {
    if (nframes < MAX_FRAME) { frames[nframes].fn = fn; frames[nframes].user = user; nframes++; }
}
static unsigned long long a_frame(void) { return frame_count; }
static void a_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fputs("[mod] ", stderr);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

static const MssApi api = {
    MSS_MOD_API_VERSION, NULL, a_r32, a_r16, a_r8, a_rf, a_w32, a_w16, a_w8, a_wf,
    a_sym, fn_name, a_hook, a_call_original, a_call, a_on_frame, a_frame, a_log,
    a_on_input, a_on_present,
};

static void load_one(const char *path) {
    void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!h) { fprintf(stderr, "mod: %s\n", dlerror()); return; }
    int (*init)(const MssApi *) = (int (*)(const MssApi *))dlsym(h, "mss_mod_init");
    if (!init) { fprintf(stderr, "mod: %s has no mss_mod_init\n", path); return; }
    MssApi a = api;
    a.mem = MEM;
    static MssApi apis[32];
    static int napis;
    if (napis == 32) return;
    apis[napis] = a;   /* stays valid for the mod's lifetime */
    int r = init(&apis[napis++]);
    fprintf(stderr, "mod: loaded %s%s\n", path, r ? " (init failed)" : "");
}

void mods_load(void) {
    const char *list = ENV("RT_MODS");
    if (list) {
        char buf[4096];
        snprintf(buf, sizeof buf, "%s", list);
        for (char *p = strtok(buf, ":"); p; p = strtok(NULL, ":")) load_one(p);
        return;
    }
    char dir[512];
    snprintf(dir, sizeof dir, "%s/.local/share/mss-recomp/mods", ENV("HOME") ? ENV("HOME") : ".");
    DIR *d = opendir(dir);
    struct dirent *e;
    while (d && (e = readdir(d))) {
        size_t l = strlen(e->d_name);
        if (l > 3 && !strcmp(e->d_name + l - 3, ".so")) {
            char p[1024];
            snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
            load_one(p);
        }
    }
    if (d) closedir(d);
}
