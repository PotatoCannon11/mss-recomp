#!/usr/bin/env python3
"""callpath.py <func> [depth]: walk direct callers of a guest function upward until reaching code the
recompiled build executed (build/cov.txt) -- that caller holds the branch that went the other way."""
import re, glob, sys
cov = {l.strip() for l in open("build/cov.txt")}
names = {l.split()[0]: (l.split() + [""])[1] for l in open("build/gen/names.txt")}
calls = {}
for p in glob.glob("build/gen/gen_*.c"):
    cur = None
    for line in open(p):
        m = re.match(r"WEAK void f_([0-9A-F]{8})", line)
        if m: cur = m.group(1)
        for t in re.findall(r"f_([0-9A-F]{8})\(c\)", line):
            if cur and t != cur:
                pc = re.search(r"/\* ([0-9A-F]{8})", line)
                calls.setdefault(t, []).append((cur, pc.group(1) if pc else "?"))
def walk(f, depth, seen, indent=""):
    for c, pc in calls.get(f, []):
        if (c, pc) in seen: continue
        seen.add((c, pc))
        ran = c in cov
        print(f"{indent}{f} <- {c} {names.get(c,'')} at {pc} {'EXECUTED' if ran else ''}")
        if not ran and depth > 0:
            walk(c, depth - 1, seen, indent + "  ")
walk(sys.argv[1], int(sys.argv[2]) if len(sys.argv) > 2 else 6, set())
