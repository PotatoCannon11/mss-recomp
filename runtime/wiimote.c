/* Real Wii Remotes over Bluetooth, through Linux's hid-wiimote driver, which exposes each remote as
 * evdev devices: "Nintendo Wii Remote" (buttons), "... Accelerometer" (ABS_RX/RY/RZ, raw - 0x200,
 * ~100 per g like WPAD's own units) and "... Nunchuk" (stick on HAT0X/Y, accel, C/Z). Buttons and real
 * motion go to the game as-is, so its own gesture detection sees real swings and throws. IR is not used
 * (no sensor bar on a PC): the pointer stays on the mouse / right stick. The first remote found is P1.
 * Polled from the retrace tick; devices are looked for about once a second (hot-plug). */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "runtime.h"

WiimoteState g_wm;

enum { DEV_CORE, DEV_ACCEL, DEV_NUNCHUK, NDEV };
static const char *dev_names[NDEV] = {"Nintendo Wii Remote", "Nintendo Wii Remote Accelerometer", "Nintendo Wii Remote Nunchuk"};
static int fds[NDEV] = {-1, -1, -1};

static void scan(void) {
    DIR *d = opendir("/sys/class/input");
    struct dirent *e;
    while (d && (e = readdir(d))) {
        if (strncmp(e->d_name, "event", 5)) continue;
        char p[300], name[128] = "";
        snprintf(p, sizeof p, "/sys/class/input/%s/device/name", e->d_name);
        FILE *f = fopen(p, "r");
        if (!f) continue;
        if (fgets(name, sizeof name, f)) name[strcspn(name, "\n")] = 0;
        fclose(f);
        for (int k = 0; k < NDEV; k++) {
            if (fds[k] >= 0 || strcmp(name, dev_names[k])) continue;
            snprintf(p, sizeof p, "/dev/input/%s", e->d_name);
            fds[k] = open(p, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
            if (fds[k] >= 0) fprintf(stderr, "wiimote: %s (%s)\n", name, p);
            else {
                static int warned;
                if (!warned++)
                    fprintf(stderr, "wiimote: found \"%s\" at %s but cannot open it (%s). Allow it with a udev rule, e.g.\n"
                            "  echo 'SUBSYSTEM==\"input\", ATTRS{name}==\"Nintendo Wii Remote*\", TAG+=\"uaccess\"' | sudo tee /etc/udev/rules.d/70-wiimote.rules\n"
                            "  sudo udevadm control --reload && sudo udevadm trigger\n", name, p, strerror(errno));
            }
        }
    }
    if (d) closedir(d);
}

static void drain(int k) {
    struct input_event ev[64];
    for (;;) {
        ssize_t n = read(fds[k], ev, sizeof ev);
        if (n < 0 && errno == EAGAIN) return;
        if (n <= 0) {   /* disconnected */
            close(fds[k]);
            fds[k] = -1;
            if (k == DEV_CORE) { g_wm.buttons = 0; fprintf(stderr, "wiimote: disconnected\n"); }
            if (k == DEV_ACCEL) g_wm.have_accel = 0;
            if (k == DEV_NUNCHUK) { g_wm.nunchuk = 0; g_wm.nc_c = g_wm.nc_z = 0; g_wm.nc_x = g_wm.nc_y = 0; }
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
                        if (e->value) g_wm.buttons |= map[j].bit;
                        else g_wm.buttons &= (u16)~map[j].bit;
                    }
            } else if (k == DEV_ACCEL && e->type == EV_ABS) {
                g_wm.have_accel = 1;
                if (e->code == ABS_RX) g_wm.acc[0] = (s16)e->value;
                if (e->code == ABS_RY) g_wm.acc[1] = (s16)e->value;
                if (e->code == ABS_RZ) g_wm.acc[2] = (s16)e->value;
            } else if (k == DEV_NUNCHUK) {
                g_wm.nunchuk = 1;
                if (e->type == EV_ABS && e->code == ABS_HAT0X) g_wm.nc_x = e->value / 100.0f;
                if (e->type == EV_ABS && e->code == ABS_HAT0Y) g_wm.nc_y = -e->value / 100.0f;   /* driver: up is +; ours: down is + */
                if (e->type == EV_KEY && e->code == BTN_C) g_wm.nc_c = e->value != 0;
                if (e->type == EV_KEY && e->code == BTN_Z) g_wm.nc_z = e->value != 0;
            }
        }
    }
}

/* called every retrace from wpad_tick */
void wiimote_poll(void) {
    static int tick;
    if (ENV("RT_NO_WIIMOTE")) return;
    if (tick++ % 60 == 0 && (fds[0] < 0 || fds[1] < 0 || fds[2] < 0)) scan();
    for (int k = 0; k < NDEV; k++)
        if (fds[k] >= 0) drain(k);
    g_wm.present = fds[DEV_CORE] >= 0;
}
