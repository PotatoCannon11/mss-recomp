/* xkey: send key presses / mouse clicks through XTest, to drive Dolphin's emulated Wii Remote
 * (keyboard/mouse bindings in WiimoteNew.ini) for side-by-side comparisons.
 *   cc -O2 -o build/xkey tools/xkey.c -lX11 -lXtst
 *   DISPLAY=:1 build/xkey move X Y | click N [ms] | key NAME [ms] | down NAME | up NAME | bdown N | bup N | resize WIN W H | sleep MS
 *   (several commands may follow) */
#include <X11/Xlib.h>
#include <X11/extensions/XTest.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv) {
    Display *d = XOpenDisplay(NULL);
    if (!d) { fprintf(stderr, "xkey: cannot open display\n"); return 1; }
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "move") && i + 2 < argc) {
            XTestFakeMotionEvent(d, -1, atoi(argv[i + 1]), atoi(argv[i + 2]), 0);
            i += 2;
        } else if (!strcmp(argv[i], "click") && i + 1 < argc) {
            int b = atoi(argv[++i]), ms = (i + 1 < argc && atoi(argv[i + 1]) > 0) ? atoi(argv[++i]) : 80;
            XTestFakeButtonEvent(d, b, True, 0); XFlush(d); usleep(ms * 1000);
            XTestFakeButtonEvent(d, b, False, 0);
        } else if (!strcmp(argv[i], "key") && i + 1 < argc) {
            KeyCode k = XKeysymToKeycode(d, XStringToKeysym(argv[++i]));
            int ms = (i + 1 < argc && atoi(argv[i + 1]) > 0) ? atoi(argv[++i]) : 80;
            if (!k) { fprintf(stderr, "xkey: unknown key %s\n", argv[i]); continue; }
            XTestFakeKeyEvent(d, k, True, 0); XFlush(d); usleep(ms * 1000);
            XTestFakeKeyEvent(d, k, False, 0);
        } else if ((!strcmp(argv[i], "down") || !strcmp(argv[i], "up")) && i + 1 < argc) {
            KeyCode k = XKeysymToKeycode(d, XStringToKeysym(argv[i + 1]));
            if (k) XTestFakeKeyEvent(d, k, argv[i][0] == 'd', 0);
            i++;
        } else if ((!strcmp(argv[i], "bdown") || !strcmp(argv[i], "bup")) && i + 1 < argc) {
            XTestFakeButtonEvent(d, atoi(argv[i + 1]), argv[i][1] == 'd', 0);
            i++;
        } else if (!strcmp(argv[i], "resize") && i + 3 < argc) {   /* resize WINDOWID W H */
            XResizeWindow(d, (Window)strtoul(argv[i + 1], NULL, 0), atoi(argv[i + 2]), atoi(argv[i + 3]));
            i += 3;
        } else if (!strcmp(argv[i], "sleep") && i + 1 < argc) {
            usleep(atoi(argv[++i]) * 1000);
        }
        XFlush(d);
    }
    XCloseDisplay(d);
    return 0;
}
