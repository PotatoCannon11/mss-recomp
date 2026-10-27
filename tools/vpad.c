/* vpad: a temporary virtual Xbox 360-style gamepad (uinput) for testing controller support.
 *   cc -O2 -o build/vpad tools/vpad.c
 *   build/vpad "sleep 5000; press a 100; axis rx 20000 1500; trigger rt 150; ..."
 * Commands (';'-separated): sleep MS | press a|b|x|y|lb|rb|start|back|guide MS | hold B | release B | axis lx|ly|rx|ry V MS
 * (held for MS then centred) | trigger lt|rt MS | hat up|down|left|right MS. The device disappears on exit. */
#include <fcntl.h>
#include <linux/uinput.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int fd;
static void emit(int type, int code, int val) {
    struct input_event e;
    memset(&e, 0, sizeof e);
    e.type = type; e.code = code; e.value = val;
    if (write(fd, &e, sizeof e) < 0) perror("write");
}
static void syn(void) { emit(EV_SYN, SYN_REPORT, 0); }

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: vpad \"commands\"\n"); return 1; }
    fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) { perror("/dev/uinput"); return 1; }
    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    int keys[] = {BTN_SOUTH, BTN_EAST, BTN_NORTH, BTN_WEST, BTN_TL, BTN_TR, BTN_SELECT, BTN_START, BTN_MODE, BTN_THUMBL, BTN_THUMBR};
    for (unsigned i = 0; i < sizeof keys / sizeof *keys; i++) ioctl(fd, UI_SET_KEYBIT, keys[i]);
    ioctl(fd, UI_SET_EVBIT, EV_ABS);
    struct uinput_abs_setup a;
    int axes[] = {ABS_X, ABS_Y, ABS_RX, ABS_RY, ABS_Z, ABS_RZ, ABS_HAT0X, ABS_HAT0Y};
    for (unsigned i = 0; i < sizeof axes / sizeof *axes; i++) {
        memset(&a, 0, sizeof a);
        a.code = axes[i];
        a.absinfo.minimum = axes[i] == ABS_Z || axes[i] == ABS_RZ ? 0 : axes[i] >= ABS_HAT0X ? -1 : -32768;
        a.absinfo.maximum = axes[i] == ABS_Z || axes[i] == ABS_RZ ? 255 : axes[i] >= ABS_HAT0X ? 1 : 32767;
        ioctl(fd, UI_SET_ABSBIT, axes[i]);
        ioctl(fd, UI_ABS_SETUP, &a);
    }
    struct uinput_setup u;
    memset(&u, 0, sizeof u);
    u.id.bustype = BUS_USB; u.id.vendor = 0x045e; u.id.product = 0x028e; u.id.version = 0x114;
    snprintf(u.name, sizeof u.name, "Microsoft X-Box 360 pad (virtual test)");
    ioctl(fd, UI_DEV_SETUP, &u);
    ioctl(fd, UI_DEV_CREATE);
    char *cmds = strdup(argv[1]);
    for (char *c = strtok(cmds, ";"); c; c = strtok(NULL, ";")) {
        char op[16] = "", arg[16] = "";
        int v1 = 0, v2 = 0;
        int n = sscanf(c, " %15s %15s %d %d", op, arg, &v1, &v2);
        if (!strcmp(op, "sleep") && n >= 2) { usleep(atoi(arg) * 1000); continue; }
        static const char *nm[] = {"a", "b", "y", "x", "lb", "rb", "back", "start", "guide"};
        static const int code[] = {BTN_SOUTH, BTN_EAST, BTN_NORTH, BTN_WEST, BTN_TL, BTN_TR, BTN_SELECT, BTN_START, BTN_MODE};
        if (!strcmp(op, "hold") || !strcmp(op, "release")) {
            for (unsigned i = 0; i < 9; i++)
                if (!strcmp(arg, nm[i])) { emit(EV_KEY, code[i], op[0] == 'h'); syn(); }
        } else if (!strcmp(op, "press")) {
            for (unsigned i = 0; i < 9; i++)
                if (!strcmp(arg, nm[i])) { emit(EV_KEY, code[i], 1); syn(); usleep((v1 ? v1 : 100) * 1000); emit(EV_KEY, code[i], 0); syn(); }
        } else if (!strcmp(op, "axis")) {
            int code = !strcmp(arg, "lx") ? ABS_X : !strcmp(arg, "ly") ? ABS_Y : !strcmp(arg, "rx") ? ABS_RX : ABS_RY;
            emit(EV_ABS, code, v1); syn(); usleep(v2 * 1000); emit(EV_ABS, code, 0); syn();
        } else if (!strcmp(op, "trigger")) {
            int code = !strcmp(arg, "lt") ? ABS_Z : ABS_RZ;
            emit(EV_ABS, code, 255); syn(); usleep((v1 ? v1 : 150) * 1000); emit(EV_ABS, code, 0); syn();
        } else if (!strcmp(op, "hat")) {
            int code = !strcmp(arg, "left") || !strcmp(arg, "right") ? ABS_HAT0X : ABS_HAT0Y;
            int val = !strcmp(arg, "left") || !strcmp(arg, "up") ? -1 : 1;
            emit(EV_ABS, code, val); syn(); usleep((v1 ? v1 : 100) * 1000); emit(EV_ABS, code, 0); syn();
        }
    }
    ioctl(fd, UI_DEV_DESTROY);
    close(fd);
    return 0;
}
