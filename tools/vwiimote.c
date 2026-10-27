/* vwiimote: a stand-in for a Bluetooth Wii Remote as Linux's hid-wiimote driver presents it (same
 * device names and event codes), for testing without hardware. Creates "Nintendo Wii Remote" and
 * "Nintendo Wii Remote Accelerometer" (+ "... Nunchuk" with -n).
 *   cc -O2 -o build/vwiimote tools/vwiimote.c
 *   build/vwiimote [-n] "sleep 5000; press a 100; swing; stick 0 80 500; ..."
 * Commands: sleep MS | press a|b|1|2|plus|minus|home|up|down|left|right MS | swing (accel burst) |
 *           c MS | z MS | stick X Y MS (Nunchuk, -100..100). At rest the accelerometer reads 1 g on z. */
#include <fcntl.h>
#include <linux/uinput.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int mk(const char *name, const int *keys, int nkeys, const int *abs, int nabs, int lo, int hi) {
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) { perror("/dev/uinput"); exit(1); }
    if (nkeys) {
        ioctl(fd, UI_SET_EVBIT, EV_KEY);
        for (int i = 0; i < nkeys; i++) ioctl(fd, UI_SET_KEYBIT, keys[i]);
    }
    if (nabs) {
        ioctl(fd, UI_SET_EVBIT, EV_ABS);
        for (int i = 0; i < nabs; i++) {
            struct uinput_abs_setup a;
            memset(&a, 0, sizeof a);
            a.code = abs[i];
            a.absinfo.minimum = lo;
            a.absinfo.maximum = hi;
            ioctl(fd, UI_SET_ABSBIT, abs[i]);
            ioctl(fd, UI_ABS_SETUP, &a);
        }
    }
    struct uinput_setup u;
    memset(&u, 0, sizeof u);
    u.id.bustype = BUS_BLUETOOTH; u.id.vendor = 0x057e; u.id.product = 0x0306;
    snprintf(u.name, sizeof u.name, "%s", name);
    ioctl(fd, UI_DEV_SETUP, &u);
    ioctl(fd, UI_DEV_CREATE);
    return fd;
}
static void emit(int fd, int type, int code, int val) {
    struct input_event e;
    memset(&e, 0, sizeof e);
    e.type = type; e.code = code; e.value = val;
    if (write(fd, &e, sizeof e) < 0) perror("write");
}
static void syn(int fd) { emit(fd, EV_SYN, SYN_REPORT, 0); }
static void accel(int fd, int x, int y, int z) { emit(fd, EV_ABS, ABS_RX, x); emit(fd, EV_ABS, ABS_RY, y); emit(fd, EV_ABS, ABS_RZ, z); syn(fd); }

int main(int argc, char **argv) {
    int nun = argc > 2 && !strcmp(argv[1], "-n");
    const char *script = argv[argc - 1];
    if (argc < 2) { fprintf(stderr, "usage: vwiimote [-n] \"commands\"\n"); return 1; }
    int keys[] = {KEY_LEFT, KEY_RIGHT, KEY_UP, KEY_DOWN, KEY_NEXT, KEY_PREVIOUS, BTN_1, BTN_2, BTN_A, BTN_B, BTN_MODE};
    int axyz[] = {ABS_RX, ABS_RY, ABS_RZ};
    int core = mk("Nintendo Wii Remote", keys, 11, NULL, 0, 0, 0);
    int acc = mk("Nintendo Wii Remote Accelerometer", NULL, 0, axyz, 3, -500, 500);
    int nk[] = {BTN_C, BTN_Z}, nab[] = {ABS_HAT0X, ABS_HAT0Y};
    int nc = nun ? mk("Nintendo Wii Remote Nunchuk", nk, 2, nab, 2, -120, 120) : -1;
    usleep(300000);
    accel(acc, 0, 0, 100);
    char *cmds = strdup(script);
    for (char *c = strtok(cmds, ";"); c; c = strtok(NULL, ";")) {
        char op[16] = "", arg[16] = "";
        int v1 = 0, v2 = 0, v3 = 0;
        sscanf(c, " %15s %15s %d %d", op, arg, &v1, &v2);
        if (!strcmp(op, "sleep")) { usleep(atoi(arg) * 1000); accel(acc, 0, 0, 100); continue; }
        if (!strcmp(op, "press")) {
            static const char *nm[] = {"left", "right", "up", "down", "plus", "minus", "1", "2", "a", "b", "home"};
            for (int i = 0; i < 11; i++)
                if (!strcmp(arg, nm[i])) {
                    emit(core, EV_KEY, keys[i], 1); syn(core);
                    usleep(atoi(op + 5) * 0 + (v1 ? v1 : 100) * 1000);
                    emit(core, EV_KEY, keys[i], 0); syn(core);
                }
        } else if (!strcmp(op, "swing")) {   /* a sideways swing, 1 sample per 1/100 s */
            static const int sx[] = {50, 150, 300, 400, 400, 320, 200, 50, -150, -280, -200, -80, 0};
            static const int sz[] = {120, 160, 220, 260, 240, 200, 150, 100, 60, 50, 80, 100, 100};
            for (int i = 0; i < 13; i++) { accel(acc, sx[i], 0, sz[i]); usleep(16000); }
        } else if ((!strcmp(op, "c") || !strcmp(op, "z")) && nc >= 0) {
            int code = op[0] == 'c' ? BTN_C : BTN_Z;
            emit(nc, EV_KEY, code, 1); syn(nc); usleep(atoi(arg) * 1000); emit(nc, EV_KEY, code, 0); syn(nc);
        } else if (!strcmp(op, "stick") && nc >= 0) {
            v3 = v2; v2 = v1; v1 = atoi(arg);
            emit(nc, EV_ABS, ABS_HAT0X, v1); emit(nc, EV_ABS, ABS_HAT0Y, v2); syn(nc);
            usleep(v3 * 1000);
            emit(nc, EV_ABS, ABS_HAT0X, 0); emit(nc, EV_ABS, ABS_HAT0Y, 0); syn(nc);
        }
    }
    ioctl(core, UI_DEV_DESTROY); ioctl(acc, UI_DEV_DESTROY);
    if (nc >= 0) ioctl(nc, UI_DEV_DESTROY);
    return 0;
}
