#!/usr/bin/env python3
"""stackdiff.py <samples.txt> <coverage.txt>: functions on the real game's sampled stacks (Dolphin)
that the recompiled build never executed, earliest first, with one example chain each."""
import bisect, sys
names, starts = {}, []
for l in open("build/gen/names.txt"):
    a, *n = l.split()
    a = int(a, 16)
    names[a] = n[0] if n else ""
    starts.append(a)
starts.sort()
fn = lambda x: starts[bisect.bisect_right(starts, x) - 1]
cov = {int(l, 16) for l in open(sys.argv[2])}
miss = {}
for l in open(sys.argv[1]):
    t, *ch = l.split()
    fs = [fn(int(x, 16)) for x in ch]
    for f in fs:
        if f not in cov:
            miss.setdefault(f, []).append((float(t), fs))
for f, v in sorted(miss.items(), key=lambda kv: kv[1][0][0]):
    print("%08X %-36s t=%6.1f n=%3d  %s" % (f, names.get(f, "")[:36], v[0][0], len(v), " < ".join(names.get(g, "%08X" % g)[:28] for g in v[0][1][:9])))
