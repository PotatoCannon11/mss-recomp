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
The game runs in its native 16:9 widescreen mode (set `RT_43=1` for 4:3). Internal resolution is `RT_SCALE`x the console's 640x528 (default 2 in a window, e.g. `RT_SCALE=4` for 2560x2112). Enter = +, Backspace = -, H = HOME, Space = swing (batting) / pull up (pitching wind-up), C = throw (pitching), Esc = quit.

Debug knobs: `RT_TRACE`, `RT_HLELOG`, `RT_GFXLOG`, `RT_DUMP_AFTER=<s>` (thread dump),
`RT_DETERMINISTIC` (virtual time, reproducible runs), `tools/hang.sh <s>` (guest stack of a hang).
