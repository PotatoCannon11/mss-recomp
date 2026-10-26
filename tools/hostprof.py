#!/usr/bin/env python3
"""hostprof.py <samples> [binary]: resolve RT_HOSTPROF samples to functions (via nm) and print the top."""
import bisect, subprocess, sys, collections
lines = open(sys.argv[1]).read().split()
base = int(lines[1], 16)
pcs = [int(x, 16) for x in lines[2:]]
binary = sys.argv[2] if len(sys.argv) > 2 else "build/mss"
syms = []
for l in subprocess.run(["nm", "-C", "--defined-only", binary], capture_output=True, text=True).stdout.splitlines():
    p = l.split(None, 2)
    if len(p) == 3 and p[1] in "tTwW":
        syms.append((int(p[0], 16), p[2]))
syms.sort()
addrs = [a for a, _ in syms]
c = collections.Counter()
for pc in pcs:
    off = pc - base   # PIE: nm addresses are offsets from the load base
    i = bisect.bisect_right(addrs, off) - 1
    c[syms[i][1] if i >= 0 else "?"] += 1
tot = len(pcs) or 1
for name, n in c.most_common(int(sys.argv[3]) if len(sys.argv) > 3 else 30):
    print("%6.2f%%  %s" % (100 * n / tot, name))
