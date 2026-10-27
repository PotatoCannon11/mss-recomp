/* Real Wii Remotes over Bluetooth, through Linux's hid-wiimote driver, which exposes each remote as
 * evdev devices: "Nintendo Wii Remote" (buttons), "... Accelerometer" (ABS_RX/RY/RZ, raw - 0x200,
 * ~100 per g like WPAD's own units) and "... Nunchuk" (stick on HAT0X/Y, accel, C/Z). Buttons and real
 * motion go to the game as-is, so its own gesture detection sees real swings and throws. IR is not used
 * (no sensor bar on a PC): player 1's pointer stays on the mouse / right stick.
 * Up to four remotes: the evdev nodes of one remote share a parent HID device; each remote takes the
 * lowest free WPAD channel when it connects (so the first remote is player 1, the next player 2, ...)
 * and keeps it until it disconnects. The remote's player LED is lit to match when /sys allows it.
 * Polled from the retrace tick; devices are looked for about once a second (hot-plug). */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "runtime.h"

WiimoteState g_wm[MAX_PADS];

enum { DEV_CORE, DEV_ACCEL, DEV_NUNCHUK, NDEV };
static const char *dev_names[NDEV] = {"Nintendo Wii Remote", "Nintendo Wii Remote Accelerometer", "Nintendo Wii Remote Nunchuk"};

typedef struct {
    char hid[PATH_MAX];   /* parent HID device (sysfs), shared by the remote's evdev nodes; "" = unused */
    char node[NDEV][32];  /* eventN of each open node */
    int fds[NDEV];
    int ch;
} Remote;
static Remote rem[MAX_PADS];

static void set_leds(const Remote *r) {
    const char *id = strrchr(r->hid, '/');
    if (!id) return;
    for (int k = 0; k < 4; k++) {
        char p[PATH_MAX + 64];
        snprintf(p, sizeof p, "/sys/class/leds/%s:blue:p%d/brightness", id + 1, k);
        FILE *f = fopen(p, "w");
        if (!f) {
            static int warned;
            if (!warned++)
                fprintf(stderr, "wiimote: can't set the player LEDs (%s); to allow it:\n"
                        "  echo 'SUBSYSTEM==\"leds\", KERNEL==\"*057E:0306*\", RUN+=\"/bin/chmod 666 /sys%%p/brightness\"' | sudo tee /etc/udev/rules.d/71-wiimote-leds.rules\n",
                        strerror(errno));
            return;
        }
        fputs(k == r->ch ? "1" : "0", f);
        fclose(f);
    }
}

static void drop(Remote *r) {
    for (int k = 0; k < NDEV; k++)
        if (r->fds[k] >= 0) close(r->fds[k]);
    fprintf(stderr, "wiimote: player %d disconnected\n", r->ch + 1);
    memset(&g_wm[r->ch], 0, sizeof g_wm[r->ch]);
    input_release_channel(r->ch);
    r->hid[0] = 0;
}

typedef struct { char ev[32], hid[PATH_MAX]; int kind; } Node;
static int by_hid(const void *a, const void *b) { return strcmp(((const Node *)a)->hid, ((const Node *)b)->hid); }

static void scan(void) {
    /* collect the remotes' nodes, oldest connection first (HID instance numbers count up) */
    static Node nodes[64];
    int n = 0;
    DIR *d = opendir("/sys/class/input");
    struct dirent *e;
    while (d && (e = readdir(d)) && n < 64) {
        if (strncmp(e->d_name, "event", 5)) continue;
        char p[300], name[128] = "";
        snprintf(p, sizeof p, "/sys/class/input/%s/device/name", e->d_name);
        FILE *f = fopen(p, "r");
        if (!f) continue;
        if (fgets(name, sizeof name, f)) name[strcspn(name, "\n")] = 0;
        fclose(f);
        for (int k = 0; k < NDEV; k++) {
            if (strcmp(name, dev_names[k])) continue;
            snprintf(p, sizeof p, "/sys/class/input/%s/device/device", e->d_name);
            if (!realpath(p, nodes[n].hid))   /* no parent (uinput stand-ins, tools/vwiimote.c): one remote */
                snprintf(nodes[n].hid, sizeof nodes[n].hid, "/virtual");
            snprintf(nodes[n].ev, sizeof nodes[n].ev, "%s", e->d_name);
            nodes[n++].kind = k;
        }
    }
    if (d) closedir(d);
    qsort(nodes, n, sizeof *nodes, by_hid);
    for (int i = 0; i < n; i++) {
        Node *nd = &nodes[i];
        Remote *r = NULL;
        for (int j = 0; j < MAX_PADS; j++)
            if (rem[j].hid[0] && !strcmp(rem[j].hid, nd->hid)) r = &rem[j];
        if (r && r->fds[nd->kind] >= 0 && !strcmp(r->node[nd->kind], nd->ev)) continue;   /* already open */
        if (!r) {
            if (nd->kind != DEV_CORE) continue;   /* a remote joins with its buttons node */
            for (int j = 0; j < MAX_PADS && !r; j++)
                if (!rem[j].hid[0]) r = &rem[j];
            if (!r) continue;
            int ch = input_claim_channel();
            if (ch < 0) { fprintf(stderr, "wiimote: all four players taken, ignoring another remote\n"); return; }
            snprintf(r->hid, sizeof r->hid, "%s", nd->hid);
            for (int k = 0; k < NDEV; k++) r->fds[k] = -1;
            r->ch = ch;
        }
        char p[64];
        snprintf(p, sizeof p, "/dev/input/%s", nd->ev);
        int fd = open(p, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) {
            static int warned;
            if (!warned++)
                fprintf(stderr, "wiimote: found \"%s\" at %s but cannot open it (%s). Allow it with a udev rule, e.g.\n"
                        "  echo 'SUBSYSTEM==\"input\", ATTRS{name}==\"Nintendo Wii Remote*\", TAG+=\"uaccess\"' | sudo tee /etc/udev/rules.d/70-wiimote.rules\n"
                        "  sudo udevadm control --reload && sudo udevadm trigger\n", dev_names[nd->kind], p, strerror(errno));
            if (nd->kind == DEV_CORE) { input_release_channel(r->ch); r->hid[0] = 0; }
            continue;
        }
        if (r->fds[nd->kind] >= 0) close(r->fds[nd->kind]);
        r->fds[nd->kind] = fd;
        snprintf(r->node[nd->kind], sizeof r->node[nd->kind], "%s", nd->ev);
        fprintf(stderr, "wiimote: player %d: %s (%s)\n", r->ch + 1, dev_names[nd->kind], p);
        if (nd->kind == DEV_CORE) set_leds(r);
    }
}

static void drain(Remote *r, int k) {
    WiimoteState *w = &g_wm[r->ch];
    struct input_event ev[64];
    for (;;) {
        ssize_t n = read(r->fds[k], ev, sizeof ev);
        if (n < 0 && errno == EAGAIN) return;
        if (n <= 0) {   /* disconnected */
            if (k == DEV_CORE) { drop(r); return; }
            close(r->fds[k]);
            r->fds[k] = -1;
            if (k == DEV_ACCEL) w->have_accel = 0;
            if (k == DEV_NUNCHUK) { w->nunchuk = 0; w->nc_c = w->nc_z = 0; w->nc_x = w->nc_y = 0; }
            return;
        }
        for (int i = 0; i < (int)(n / sizeof *ev); i++) {
            const struct input_event *e = &ev[i];
            if (k == DEV_CORE && e->type == EV_KEY) {
                static const struct { int code; u16 bit; } map[] = {
                    {KEY_LEFT, 0x0001}, {KEY_RIGHT, 0x0002}, {KEY_DOWN, 0x0004}, {KEY_UP, 0x0008}, {KEY_NEXT, 0x0010},
                    {BTN_2, 0x0100}, {BTN_1, 0x0200}, {BTN_B, 0x0400}, {BTN_A, 0x0800}, {KEY_PREVIOUS, 0x1000}, {BTN_MODE, 0x8000},
                };
                for (unsigned j = 0; j < sizeof map / sizeof *map; j++)
                    if (e->code == map[j].code) {
                        if (e->value) w->buttons |= map[j].bit;
                        else w->buttons &= (u16)~map[j].bit;
                    }
            } else if (k == DEV_ACCEL && e->type == EV_ABS) {
                w->have_accel = 1;
                if (e->code == ABS_RX) w->acc[0] = (s16)e->value;
                if (e->code == ABS_RY) w->acc[1] = (s16)e->value;
                if (e->code == ABS_RZ) w->acc[2] = (s16)e->value;
            } else if (k == DEV_NUNCHUK) {
                w->nunchuk = 1;
                if (e->type == EV_ABS && e->code == ABS_HAT0X) w->nc_x = e->value / 100.0f;
                if (e->type == EV_ABS && e->code == ABS_HAT0Y) w->nc_y = -e->value / 100.0f;   /* driver: up is +; ours: down is + */
                if (e->type == EV_KEY && e->code == BTN_C) w->nc_c = e->value != 0;
                if (e->type == EV_KEY && e->code == BTN_Z) w->nc_z = e->value != 0;
            }
        }
    }
}

/* called every retrace from wpad_tick */
void wiimote_poll(void) {
    static int tick;
    if (ENV("RT_NO_WIIMOTE")) return;
    if (tick++ % 60 == 0) scan();
    for (int j = 0; j < MAX_PADS; j++) {
        Remote *r = &rem[j];
        for (int k = 0; k < NDEV && r->hid[0]; k++)
            if (r->fds[k] >= 0) drain(r, k);
        if (r->hid[0]) g_wm[r->ch].present = 1;
    }
}
