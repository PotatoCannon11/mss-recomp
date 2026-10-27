# Recompiled Mario Super Sluggers for Linux.
#   make GEN    regenerate C from the game's main.dol (needs ../repo/orig and symbols)
#   make        build build/mss
DOL ?= ../repo/orig/RMBE01/sys/main.dol
SYMBOLS ?= ../repo/config/RMBE01/symbols.txt
CC = gcc
OPT ?= -O2
EXTRA ?=
# `make EXTRA=-DTRACE` builds the debug variant (function tracing, profiles, coverage, call logs)
CFLAGS = $(OPT) -g0 -fno-strict-aliasing -fwrapv -ffp-contract=off -w -Iruntime -Ibuild/gen $(EXTRA)

GEN_SRC = $(wildcard build/gen/gen_*.c)
GEN_OBJ = $(GEN_SRC:build/gen/%.c=build/obj/%.o)
RT_SRC = $(wildcard runtime/*.c)
RT_OBJ = $(RT_SRC:runtime/%.c=build/obj/rt_%.o)

build/mss: $(GEN_OBJ) $(RT_OBJ) build/obj/functab.o
	$(CC) -o $@ $^ -lm -lpthread -lEGL -lGL -ldl

build/obj/%.o: build/gen/%.c runtime/cpu.h | build/obj
	$(CC) $(CFLAGS) -c $< -o $@
build/obj/rt_%.o: runtime/%.c runtime/cpu.h runtime/runtime.h runtime/gpu.h | build/obj
	$(CC) $(CFLAGS) -W -Wall -c $< -o $@
build/obj:
	mkdir -p $@

GEN:
	python3 tools/ppcrecomp.py $(DOL) $(SYMBOLS) build/gen --names tools/ref_names.txt

.PHONY: GEN

# example mods (mods/*.c -> build/mods/*.so); load with RT_MODS=build/mods/x.so or copy to
# ~/.local/share/mss-recomp/mods/
MOD_SRC = $(wildcard mods/*.c)
mods: $(MOD_SRC:mods/%.c=build/mods/%.so)
build/mods/%.so: mods/%.c runtime/mod_api.h runtime/cpu.h
	@mkdir -p build/mods
	$(CC) -O2 -shared -fPIC -Iruntime -o $@ $< -lm -lGL
.PHONY: mods
