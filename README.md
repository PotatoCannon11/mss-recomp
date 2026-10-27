# Mario Super Sluggers — Linux recompilation

Static recompilation of the game's PowerPC code to C, plus a host runtime that stands in for the
Wii hardware and SDK. Nothing from the game is stored here: `make GEN` regenerates the C from your
own `main.dol`, and the runtime reads game files from your own extracted disc.

    make GEN                      # recompile main.dol (+ ../repo symbols) to build/gen/*.c
    make -j8                      # build build/mss
    ./build/mss <main.dol> <extracted disc root>

Layout
- `tools/ppcrecomp.py` – Gekko → C translator (all 21k functions, jump tables, paired singles)
- `runtime/runtime.c` – guest memory map, DOL loader, low-memory boot state, CPU services
- `runtime/threads.c` – guest threads as host coroutines; interrupts (alarms, VI, GPU) at safe points
- `runtime/disc.c`, `hle.c` – disc access through the FST; SDK replacements (DVD, EXI, IOS, ...)
- `runtime/wpad.c` – Wii Remote from host input
- `runtime/gpu.c`, `gfx.c` – GX command stream decoding and the OpenGL renderer

Running (from ~/mss-decomp/recomp):

    ./build/mss ../repo/orig/RMBE01/sys/main.dol ../game

`../game` is the disc extracted with `dtk disc extract <your .wbfs> ../game`. Saves go to
`~/.local/share/mss-recomp/nand`. With a desktop session it opens a window (SDL2); without one it
runs headless (set `RT_FRAMES=dir` to dump frames, `RT_WAV=file.wav` to record audio).

Controls (Wii Remote): mouse = pointer, left/right click = A/B, arrows = d-pad, Z/X = 1/2,
Gamepads (anything SDL knows: Xbox, PlayStation, Switch Pro, ...) work too and can be plugged in at any
time: A/B = A/B, X/Y = 1/2, Start/Back/Guide = +/-/HOME, d-pad or left stick = d-pad, right stick = pointer,
RT or RB = swing (and the pitching pull-up), LT = throw. The mouse takes the pointer back when moved.
**Real Wii Remotes** (Bluetooth, through Linux's built-in hid-wiimote driver) are picked up
automatically, with a Nunchuk if plugged in: buttons and real motion go straight to the game. There's no
sensor bar, so point with the mouse. Pair once: hold 1+2 (or press the red sync button) and run
`bluetoothctl` -> `scan on` -> `connect <address of "Nintendo RVL-CNT-01">`. `RT_NO_WIIMOTE=1` ignores it.
**Multiplayer:** each Wii Remote and gamepad takes the next free player slot when it connects (first =
player 1, next = player 2, up to 4) and keeps it until it disconnects; the keyboard and mouse always
control player 1. Remotes light their matching player LED if allowed by
`echo 'SUBSYSTEM=="leds", KERNEL=="*057E:0306*", RUN+="/bin/chmod 666 /sys%p/brightness"' | sudo tee /etc/udev/rules.d/71-wiimote-leds.rules`.
`RT_NUNCHUK=1` attaches a Nunchuk (the game's Wii Remote + Nunchuk control style): WASD / left stick =
stick, Q / LB = Z, E / left-stick click = C.

The game runs in its native 16:9 widescreen mode (set `RT_43=1` for 4:3). Any window shape works and the window can be resized live: the picture fills it with no bars and nothing
stretched or cropped (wider than 16:9 shows more to the sides, taller shows more above and below; the HUD
keeps its proportions, centred). `RT_FULLSCREEN=1` uses the whole monitor at its own resolution and shape.
Internal resolution follows the window height (or `RT_SCALE=N` x the console's 528 lines);
`RT_ASPECT=21:9` sets the shape for headless runs, `RT_ASPECT_OFF=1` stretches instead. Enter = +, Backspace = -, H = HOME, Space = swing (batting) / pull up (pitching wind-up), C = throw (pitching), Esc = quit.

Debug knobs: `RT_TRACE`, `RT_HLELOG`, `RT_GFXLOG`, `RT_DUMP_AFTER=<s>` (thread dump),
`RT_DETERMINISTIC` (virtual time, reproducible runs), `tools/hang.sh <s>` (guest stack of a hang).

## Saves

`tools/save.py import <source>` installs a save from a Wii SD-card export (`data.bin`), a Dolphin save
folder or user directory, a plain folder with `gamedata`/`banner.bin`, or a `.zip` of any of these.
With no source it takes Dolphin's save if it finds one. The current save is backed up first
(`~/.local/share/mss-recomp/save-backups`); `tools/save.py list`, `backup` and `restore [name]` manage them.
Quit the game before importing.

## HD textures

`run.sh` saves each texture the game shows, once, to `~/.local/share/mss-recomp/texdump/RMBE01`
(`RT_TEXDUMP=0` turns it off). `./upscale.sh` runs Real-ESRGAN (x4plus, CPU) over everything dumped that
isn't upscaled yet and writes `~/.local/share/mss-recomp/textures/RMBE01`, which the game loads at
start-up (`RT_TEXPACK=<dir>` for another pack, `RT_TEXPACK=0` for none). Files are named
`tex1_<w>x<h>_<hash>_<fmt>.png` by content, so hand-made replacements work too (8-bit RGB/RGBA PNG, any
size). The upscaler needs a venv with torch, numpy and pillow at `~/.local/share/mss-recomp/upscaler-venv`
and the model at `~/.local/share/mss-recomp/upscaler/RealESRGAN_x4plus.pth`. Textures are mipmapped with
16x anisotropic filtering (`RT_ANISO=0` turns that off).

## Mods

Native mods are C shared libraries (`runtime/mod_api.h`). A mod can hook any recompiled game function
by name (run before it, replace it, or call the original and post-process), read/write guest memory and
run code every frame. `make mods` builds the examples in `mods/` (e.g. `mods/rainbow.c`, tinting lit
colours through the rainbow); run with `RT_MODS=build/mods/rainbow.so ./build/mss ...` or copy `.so` files
to `~/.local/share/mss-recomp/mods/`. Unhooked functions pay one bit test at entry.
API v2 adds `on_input` (read or rewrite the Wii Remote state each tick before the game sees it) and
`on_present` (each frame just before it is shown, window framebuffer bound: draw overlays with GL);
`mods/autoswing.c` turns the 2 button into a full swing, `mods/fpsbar.c` draws a frame-time bar.
