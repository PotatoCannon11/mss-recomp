#!/usr/bin/env python3
"""vtctor.py <lo> <hi>: functions that build an address in [lo, hi] with lis/addi (e.g. a vtable a
constructor stores), and whether the recompiled build executed them."""
import re, glob, sys
lo, hi = int(sys.argv[1], 16), int(sys.argv[2], 16)
cov = {l.strip() for l in open("build/cov.txt")}
names = {l.split()[0]: (l.split() + [""])[1] for l in open("build/gen/names.txt")}
hits = set()
for p in glob.glob("build/gen/gen_*.c"):
    cur, lis = None, {}
    for line in open(p):
        m = re.match(r"WEAK void f_([0-9A-F]{8})", line)
        if m: cur, lis = m.group(1), {}
        m = re.search(r"c->r\[(\d+)\] = 0x([0-9A-F]{4})0000;", line)
        if m: lis[m.group(1)] = int(m.group(2), 16) << 16
        m = re.search(r"c->r\[(\d+)\] = c->r\[(\d+)\] \+ (-?\d+);", line)
        if m and m.group(2) in lis:
            v = (lis[m.group(2)] + int(m.group(3))) & 0xFFFFFFFF
            if lo <= v <= hi: hits.add((cur, v))
for cur, v in sorted(hits):
    print(cur, names.get(cur, ""), "%08X" % v, "EXECUTED" if cur in cov else "not executed")
