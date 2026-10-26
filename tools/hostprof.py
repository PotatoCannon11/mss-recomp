#!/usr/bin/env python3
"""hostprof.py <samples> [binary] [top]: resolve RT_HOSTPROF samples to functions (via nm for the
binary, library name otherwise) and print the top."""
import bisect, subprocess, sys, collections
lines = open(sys.argv[1]).read().splitlines()
base = int(lines[0].split()[1], 16)
pcs, rets, maps = [], [], []
for l in lines[1:]:
    if l.startswith("map "):
        p = l.split()
        lo, hi = (int(x, 16) for x in p[1].split("-"))
        maps.append((lo, hi, p[-1].rsplit("/", 1)[-1] if len(p) > 6 else "[anon]"))
    elif l:
        p = l.split()
        pcs.append(int(p[0], 16))
        rets.append(int(p[1], 16) if len(p) > 1 else 0)
binary = sys.argv[2] if len(sys.argv) > 2 else "build/mss"
syms = []
for l in subprocess.run(["nm", "-C", "--defined-only", binary], capture_output=True, text=True).stdout.splitlines():
    p = l.split(None, 2)
    if len(p) == 3 and p[1] in "tTwW":
        syms.append((int(p[0], 16), p[2]))
syms.sort()
addrs = [a for a, _ in syms]
exe = [m for m in maps if m[2] == binary.rsplit("/", 1)[-1]]
def sym(pc):
    i = bisect.bisect_right(addrs, pc - base) - 1   # PIE: nm addresses are offsets from the load base
    return syms[i][1] if i >= 0 else "?"
def in_exe(pc):
    return any(m[0] <= pc < m[1] for m in exe)
c = collections.Counter()
via = collections.Counter()   # library samples by the binary function that called into the library
for pc, ret in zip(pcs, rets):
    lib = next((m[2] for m in maps if m[0] <= pc < m[1]), None)
    if lib is not None and not in_exe(pc):
        c["[" + lib + "]"] += 1
        via["[" + lib + "] <- " + (sym(ret) if in_exe(ret) else "?")] += 1
        continue
    c[sym(pc)] += 1
tot = len(pcs) or 1
print("%d samples" % len(pcs))
for name, n in c.most_common(int(sys.argv[3]) if len(sys.argv) > 3 else 30):
    print("%6.2f%%  %s" % (100 * n / tot, name))
print("library time by caller (leaf calls only):")
for name, n in via.most_common(15):
    print("%6.2f%%  %s" % (100 * n / tot, name))
