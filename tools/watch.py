#!/usr/bin/env python3
"""watch.py <watch.txt> [binary]: resolve RT_WATCH hits to the recompiled functions that touched the range."""
import bisect, subprocess, sys, collections
lines = open(sys.argv[1]).read().splitlines()
base = int(lines[0].split()[1], 16)
binary = sys.argv[2] if len(sys.argv) > 2 else "build/mss"
syms = sorted((int(p[0], 16), p[2]) for p in (l.split(None, 2) for l in subprocess.run(
    ["nm", "-C", "--defined-only", binary], capture_output=True, text=True).stdout.splitlines()) if len(p) == 3 and p[1] in "tTwW")
addrs = [a for a, _ in syms]
c = collections.defaultdict(collections.Counter)
for l in lines[1:]:
    rip, addr, n = l.split()
    i = bisect.bisect_right(addrs, int(rip, 16) - base) - 1
    c[syms[i][1] if i >= 0 else "?"][addr] += int(n)
for fn, d in sorted(c.items(), key=lambda kv: -sum(kv[1].values())):
    print("%-28s %s" % (fn, " ".join("%s:%d" % (a, n) for a, n in sorted(d.items()))))
