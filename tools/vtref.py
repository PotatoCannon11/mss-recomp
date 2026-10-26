#!/usr/bin/env python3
"""vtref.py <addr>...: where the DOL's data holds a pointer to each function, with the surrounding
table and the class name string that MWCC places after vtables when present."""
import struct, sys, re
d = open(sys.argv[1] if sys.argv[1].endswith(".dol") else "../repo/orig/RMBE01/sys/main.dol", "rb").read()
args = [a for a in sys.argv[1:] if not a.endswith(".dol")]
offs = struct.unpack(">18I", d[0:72]); addrs = struct.unpack(">18I", d[72:144]); sizes = struct.unpack(">18I", d[144:216])
def rd(a):
    for i in range(18):
        if sizes[i] and addrs[i] <= a < addrs[i] + sizes[i]:
            return struct.unpack(">I", d[offs[i] + a - addrs[i]:offs[i] + a - addrs[i] + 4])[0]
    return 0
names = {int(l.split()[0], 16): (l.split() + [""])[1] for l in open("build/gen/names.txt")}
cov = {int(l, 16) for l in open("build/cov.txt")}
for t in (int(a, 16) for a in args):
    for i in range(18):
        if not sizes[i] or i in (0, 1, 7, 8): continue   # data sections only (skip text/extab index)
        sec = d[offs[i]:offs[i] + sizes[i]]
        for k in range(0, len(sec) - 3, 4):
            if struct.unpack(">I", sec[k:k + 4])[0] == t:
                a = addrs[i] + k
                print(f"{t:08X} referenced at {a:08X}")
                for x in range(a - 16, a + 24, 4):
                    v = rd(x)
                    s = struct.pack(">I", v)
                    txt = s.decode("latin-1") if all(32 <= b < 127 for b in s) else ""
                    print(f"   {x:08X}: {v:08X} {names.get(v, '')} {'(ran)' if v in cov else ''} {txt}")
