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

Debug knobs: `RT_TRACE`, `RT_HLELOG`, `RT_GFXLOG`, `RT_DUMP_AFTER=<s>` (thread dump),
`RT_DETERMINISTIC` (virtual time, reproducible runs), `tools/hang.sh <s>` (guest stack of a hang).
