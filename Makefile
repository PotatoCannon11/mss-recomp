# Recompiled Mario Super Sluggers for Linux.
#   make GEN    regenerate C from the game's main.dol (needs ../repo/orig and symbols)
#   make        build build/mss
DOL ?= ../repo/orig/RMBE01/sys/main.dol
SYMBOLS ?= ../repo/config/RMBE01/symbols.txt
CC = gcc
OPT ?= -O2
EXTRA ?= -DTRACE
CFLAGS = $(OPT) -g0 -fno-strict-aliasing -fwrapv -ffp-contract=off -w -Iruntime -Ibuild/gen $(EXTRA)

GEN_SRC = $(wildcard build/gen/gen_*.c)
GEN_OBJ = $(GEN_SRC:build/gen/%.c=build/obj/%.o)
RT_SRC = $(wildcard runtime/*.c)
RT_OBJ = $(RT_SRC:runtime/%.c=build/obj/rt_%.o)

build/mss: $(GEN_OBJ) $(RT_OBJ) build/obj/functab.o
	$(CC) -o $@ $^ -lm -lpthread

build/obj/%.o: build/gen/%.c runtime/cpu.h | build/obj
	$(CC) $(CFLAGS) -c $< -o $@
build/obj/rt_%.o: runtime/%.c runtime/cpu.h runtime/runtime.h | build/obj
	$(CC) $(CFLAGS) -W -Wall -c $< -o $@
build/obj:
	mkdir -p $@

GEN:
	python3 tools/ppcrecomp.py $(DOL) $(SYMBOLS) build/gen --names tools/ref_names.txt

.PHONY: GEN
