#!/usr/bin/env python3
"""Static recompiler: Gekko (PowerPC 750CL) machine code in a Wii DOL -> C.

usage: ppcrecomp.py <main.dol> <symbols.txt> <out dir> [--per-file N]

Every function from symbols.txt becomes `void f_ADDR(CPU *c)`. Branches inside a function become
gotos, `bl` a direct call, branches leaving a function tail calls. Addresses entered other than at a
symbol start (mid-function bl/b targets) get their own entry covering the rest of the enclosing
function. bctr inside a function dispatches over its jump tables (dtk's jumptable_ symbols).
Writes out/gen_NNN.c, out/functab.c (sorted address -> function table) and out/names.txt.
"""
import argparse, os, re, struct
from collections import defaultdict


def load_dol(path):
    d = open(path, "rb").read()
    offs = struct.unpack(">18I", d[0:72]); addrs = struct.unpack(">18I", d[72:144]); sizes = struct.unpack(">18I", d[144:216])
    secs = [(addrs[i], d[offs[i]:offs[i] + sizes[i]], i < 7) for i in range(18) if sizes[i]]
    entry = struct.unpack(">I", d[0xE0:0xE4])[0]
    return secs, entry


class Image:
    def __init__(self, secs):
        self.secs = secs

    def word(self, a):
        for base, data, _ in self.secs:
            if base <= a < base + len(data) - 3:
                return struct.unpack(">I", data[a - base:a - base + 4])[0]
        return None

    def is_text(self, a):
        return any(t and base <= a < base + len(data) for base, data, t in self.secs)


DATA = {}


def parse_symbols(path):
    funcs, tables, names = [], [], {}
    for l in open(path):
        m = re.match(r"(\S+) = (\.\w+):0x([0-9A-F]+); // type:(\w+)(?: size:0x([0-9A-F]+))?", l)
        if not m:
            continue
        name, sec, addr, ty, size = m.group(1), m.group(2), int(m.group(3), 16), m.group(4), int(m.group(5) or "0", 16)
        if ty == "function" and sec in (".init", ".text") and size:
            funcs.append((addr, addr + size)); names[addr] = name
        elif ty == "object" and name.startswith("jumptable_") and size:
            tables.append((addr, size))
        elif ty == "object" and not re.match(r"(lbl|jumptable|gap)_", name) and re.fullmatch(r"[A-Za-z_]\w*", name):
            DATA.setdefault(name, addr)
    funcs.sort()
    return funcs, tables, names


IRQ_CHECK = "IRQ_POINT(c); "


def simm(v):
    return v - 0x10000 if v & 0x8000 else v


def mask(mb, me):
    a = 0xFFFFFFFF >> mb
    b = (0xFFFFFFFF << (31 - me)) & 0xFFFFFFFF
    return a & b if mb <= me else a | b


def branch_targets(w, pc):
    """(target, kind) of a direct branch; kind is 'b', 'bl', 'bc', 'bcl'"""
    op = w >> 26
    if op == 18:
        li = w & 0x03FFFFFC
        if li & 0x02000000:
            li -= 0x04000000
        t = (li if w & 2 else pc + li) & 0xFFFFFFFF
        return t, "bl" if w & 1 else "b"
    if op == 16:
        bd = w & 0xFFFC
        if bd & 0x8000:
            bd -= 0x10000
        t = (bd if w & 2 else pc + bd) & 0xFFFFFFFF
        return t, "bcl" if w & 1 else "bc"
    return None, None


class Gen:
    def __init__(self, img, funcs, tables, names):
        self.img, self.funcs, self.names = img, funcs, names
        self.starts = {a for a, _ in funcs}
        self.ends = dict(funcs)
        self.entries = {}   # entry addr -> end addr (exclusive)
        for a, e in funcs:
            self.entries[a] = e
        # jump tables: entries -> owning function
        self.jt_targets = defaultdict(set)
        for ta, size in tables:
            for k in range(0, size, 4):
                t = img.word(ta + k)
                f = self.containing(t) if t else None
                if f:
                    self.jt_targets[f[0]].add(t)

    def add_data_switch_targets(self):
        """unnamed switch tables: any data word pointing inside a function that has a bctr"""
        import bisect
        ptrs = set()
        for base, data, is_text in self.img.secs:
            if is_text:
                continue
            for k in range(0, len(data) - 3, 4):
                v = struct.unpack(">I", data[k:k + 4])[0]
                if v & 3 == 0 and self.img.is_text(v):
                    ptrs.add(v)
        ptrs = sorted(ptrs)
        for a, e in self.funcs:
            if not any((self.img.word(pc) or 0) == 0x4E800420 for pc in range(a, e, 4)):
                continue
            i = bisect.bisect_right(ptrs, a)
            while i < len(ptrs) and ptrs[i] < e:
                self.jt_targets[a].add(ptrs[i]); i += 1

    def containing(self, a):
        import bisect
        i = bisect.bisect_right(self.funcs, (a, 0xFFFFFFFF)) - 1
        if i >= 0 and self.funcs[i][0] <= a < self.funcs[i][1]:
            return self.funcs[i]
        return None

    def entry_for(self, t):
        """register t as a function entry; returns False if t is not inside known code"""
        if t in self.entries:
            return True
        f = self.containing(t)
        if not f:
            return False
        self.entries[t] = f[1]
        return True

    def close_entries(self):
        """find every entry reachable as a call / out-of-range branch target, to a fixed point"""
        todo = list(self.entries.items())
        seen = set()
        while todo:
            start, end = todo.pop()
            if start in seen:
                continue
            seen.add(start)
            for pc in range(start, end, 4):
                w = self.img.word(pc)
                if w is None:
                    continue
                t, kind = branch_targets(w, pc)
                if t is None:
                    continue
                if kind in ("bl", "bcl") and t == pc + 4:
                    continue
                if kind in ("bl", "bcl") or not (start <= t < end):
                    if t not in self.entries and self.entry_for(t):
                        todo.append((t, self.entries[t]))
            # fall-through off the end into the next function
            if end in self.starts or self.entry_for(end):
                pass

    # ---------------------------------------------------------------- emission
    def fn(self, a):
        return f"f_{a:08X}"

    def emit_function(self, start):
        end = self.entries[start]
        f = self.containing(start)
        labels = set()
        body = []
        for pc in range(start, end, 4):
            w = self.img.word(pc)
            t, kind = branch_targets(w, pc) if w is not None else (None, None)
            if t is not None and kind in ("b", "bc") and start <= t < end:
                labels.add(t)
        jt = sorted(x for x in self.jt_targets.get(f[0], ()) if start <= x < end) if f else []
        labels |= set(jt)
        self.cur = (start, end, jt)
        for pc in range(start, end, 4):
            w = self.img.word(pc)
            if pc in labels:
                body.append(f"L_{pc:08X}:")
            if w is None:
                body.append(f"    ppc_illegal(c, 0x{pc:08X}, 0);"); continue
            body.append(f"    /* {pc:08X} {w:08X} */ " + self.insn(w, pc))
        last = self.img.word(end - 4)
        tail = ""
        if not self.ends_flow(last):
            tail = f"    {self.call(end)} return;\n" if end in self.entries else "    return;\n"
        nm = self.names.get(start, "")
        return (f"WEAK void {self.fn(start)}(CPU *c) {{ /* {nm} */\n    FN_ENTER(0x{start:08X}); u32 ea; (void)ea; double t0, t1; (void)t0; (void)t1;\n"
                + "\n".join(body) + "\n" + tail + "}\n")

    def ends_flow(self, w):
        if w is None:
            return False
        op = w >> 26
        if op == 18 and not (w & 1):
            return True
        if op == 19 and ((w >> 1) & 0x3FF) in (16, 528) and not (w & 1) and ((w >> 21) & 0x14) == 0x14:
            return True
        if op == 19 and ((w >> 1) & 0x3FF) == 50:
            return True
        return False

    def call(self, t):
        if t in self.entries:
            return f"{self.fn(t)}(c);"
        return f"call_indirect(c, 0x{t:08X});"

    def cond(self, bo, bi):
        """C condition for a bc with BO/BI (CTR decrement emitted separately)"""
        parts = []
        if not (bo & 4):
            parts.append("c->ctr == 0" if bo & 2 else "c->ctr != 0")
        if not (bo & 16):
            parts.append(f"crbit(c, {bi})" if bo & 8 else f"!crbit(c, {bi})")
        return " && ".join(parts) if parts else "1"

    def insn(self, w, pc):
        try:
            return self._insn(w, pc)
        except KeyError:
            return f"ppc_illegal(c, 0x{pc:08X}, 0x{w:08X});"

    def _insn(self, w, pc):
        op = w >> 26
        D = (w >> 21) & 31; A = (w >> 16) & 31; B = (w >> 11) & 31; C = (w >> 6) & 31
        rc = w & 1
        R = lambda n: f"c->r[{n}]"
        F = lambda n: f"c->f[{n}]"
        P = lambda n: f"c->ps1[{n}]"
        imm = w & 0xFFFF
        si = simm(imm)
        ra0 = R(A) if A else "0"
        start, end, jt = self.cur
        RC = (lambda dst: f" rc0(c, {dst});") if rc else (lambda dst: "")

        if op == 18 or op == 16:
            t, kind = branch_targets(w, pc)
            if op == 18:
                if kind == "bl":
                    if t == pc + 4:
                        return f"c->lr = 0x{pc + 4:08X};"
                    return f"c->lr = 0x{pc + 4:08X}; {self.call(t)}"
                if start <= t < end:
                    return (IRQ_CHECK if t <= pc else "") + f"goto L_{t:08X};"
                return f"{self.call(t)} return;"
            bo, bi = D, A
            pre = "c->ctr--; " if not (bo & 4) else ""
            cnd = self.cond(bo, bi)
            if kind == "bcl":
                if t == pc + 4:
                    return pre + f"c->lr = 0x{pc + 4:08X};"
                return pre + f"if ({cnd}) {{ c->lr = 0x{pc + 4:08X}; {self.call(t)} }}"
            if start <= t < end:
                if t <= pc:   # loop back-edge: also a preemption point
                    return pre + f"if ({cnd}) {{ {IRQ_CHECK}goto L_{t:08X}; }}"
                return pre + f"if ({cnd}) goto L_{t:08X};"
            return pre + f"if ({cnd}) {{ {self.call(t)} return; }}"
        if op == 19:
            xo = (w >> 1) & 0x3FF
            if xo == 16 or xo == 528:   # bclr / bcctr
                bo, bi = D, A
                pre = "c->ctr--; " if (not (bo & 4) and xo == 16) else ""
                cnd = self.cond(bo, bi)
                if xo == 16:
                    act = f"{{ u32 t = c->lr; c->lr = 0x{pc + 4:08X}; call_indirect(c, t); }}" if rc else "return;"
                else:
                    if rc:
                        act = f"{{ c->lr = 0x{pc + 4:08X}; call_indirect(c, c->ctr); }}"
                    elif jt:
                        cases = " ".join(f"case 0x{t:08X}: goto L_{t:08X};" for t in jt)
                        act = f"{{ switch (c->ctr) {{ {cases} default: call_indirect(c, c->ctr); return; }} }}"
                    else:
                        act = "{ call_indirect(c, c->ctr); return; }"
                return pre + (act if cnd == "1" else f"if ({cnd}) {act}")
            if xo == 0:
                return f"setcr(c, {D >> 2}, (c->cr >> {28 - 4 * (A >> 2)}) & 0xF);"
            if xo == 150 or xo == 0x96:
                return ";"  # isync
            if xo == 50:
                return f"ppc_rfi(c, 0x{pc:08X}); return;"
            crops = {257: "{a} & {b}", 129: "{a} & ~{b}", 289: "~({a} ^ {b})", 225: "~({a} & {b})", 33: "~({a} | {b})",
                     449: "{a} | {b}", 417: "{a} | ~{b}", 193: "{a} ^ {b}"}
            if xo in crops:
                e = crops[xo].format(a=f"crbit(c, {A})", b=f"crbit(c, {B})")
                return f"setcrbit(c, {D}, {e});"
            raise KeyError
        if op == 14:
            return f"{R(D)} = {ra0} + {si};" if A else f"{R(D)} = 0x{si & 0xFFFFFFFF:08X};"
        if op == 15:
            v = (si << 16) & 0xFFFFFFFF
            return f"{R(D)} = {R(A)} + 0x{v:08X};" if A else f"{R(D)} = 0x{v:08X};"
        if op == 12:
            return f"{R(D)} = addc(c, {R(A)}, 0x{si & 0xFFFFFFFF:08X}, 0);"
        if op == 13:
            return f"{R(D)} = addc(c, {R(A)}, 0x{si & 0xFFFFFFFF:08X}, 0); rc0(c, {R(D)});"
        if op == 8:
            return f"{R(D)} = addc(c, ~{R(A)}, 0x{si & 0xFFFFFFFF:08X}, 1);"
        if op == 7:
            return f"{R(D)} = (u32)((s64)(s32){R(A)} * {si});"
        if op == 10:
            return f"cmpu(c, {D >> 2}, {R(A)}, 0x{imm:X});"
        if op == 11:
            return f"cmps(c, {D >> 2}, (s32){R(A)}, {si});"
        if op in (24, 25, 26, 27, 28, 29):
            S = D
            v = imm if op in (24, 26, 28) else imm << 16
            o = {24: "|", 25: "|", 26: "^", 27: "^", 28: "&", 29: "&"}[op]
            s = f"{R(A)} = {R(S)} {o} 0x{v:X}u;"
            return s + (f" rc0(c, {R(A)});" if op in (28, 29) else "")
        if op in (20, 21, 23):
            S, sh, mb, me = D, B, C, (w >> 1) & 31
            m = mask(mb, me)
            rot = f"rotl({R(S)}, {sh})" if op != 23 else f"rotl({R(S)}, {R(B)})"
            if op == 21 and sh == 0:
                rot = R(S)
            if op == 20:
                s = f"{R(A)} = ({rot} & 0x{m:08X}u) | ({R(A)} & 0x{(~m) & 0xFFFFFFFF:08X}u);"
            else:
                s = f"{R(A)} = {rot} & 0x{m:08X}u;"
            return s + RC(R(A))
        if op == 3:
            return f"ppc_trap(c, 0x{pc:08X});" if D == 31 else ";"  # twi (conditional traps ignored)
        if op == 17:
            return f"ppc_sc(c, 0x{pc:08X});"
        # ---- D-form loads / stores
        ld = {32: ("w", 0), 33: ("w", 1), 34: ("bz", 0), 35: ("bz", 1), 40: ("hz", 0), 41: ("hz", 1), 42: ("ha", 0), 43: ("ha", 1)}
        st = {36: ("w", 0), 37: ("w", 1), 38: ("b", 0), 39: ("b", 1), 44: ("h", 0), 45: ("h", 1)}
        if op in ld or op in st or op in (48, 49, 50, 51, 52, 53, 54, 55):
            upd = op in (33, 35, 41, 43, 37, 39, 45, 49, 51, 53, 55)
            ea = f"ea = {R(A)} + {si};" if (upd or A) else f"ea = 0x{si & 0xFFFFFFFF:08X};"
            u = f" {R(A)} = ea;" if upd else ""
            if op in ld:
                return ea + " " + self.load(ld[op][0], R(D)) + u
            if op in st:
                return ea + " " + self.store(st[op][0], R(D)) + u
            if op in (48, 49):
                return ea + f" {F(D)} = lfs(ea); {P(D)} = {F(D)};" + u
            if op in (50, 51):
                return ea + f" {F(D)} = lfd(ea);" + u
            if op in (52, 53):
                return ea + f" stfs(ea, {F(D)});" + u
            return ea + f" stfd(ea, {F(D)});" + u
        if op == 46:
            ea = f"ea = {ra0} + {si};"
            return ea + " " + " ".join(f"{R(r)} = mr32(ea + {4 * (r - D)});" for r in range(D, 32))
        if op == 47:
            ea = f"ea = {ra0} + {si};"
            return ea + " " + " ".join(f"mw32(ea + {4 * (r - D)}, {R(r)});" for r in range(D, 32))
        if op in (56, 57, 60, 61):   # psq_l / psq_lu / psq_st / psq_stu
            d12 = w & 0xFFF
            if d12 & 0x800:
                d12 -= 0x1000
            W = (w >> 15) & 1; I = (w >> 12) & 7
            upd = op in (57, 61)
            ea = f"ea = {R(A) if (A or upd) else '0'} + {d12};"
            u = f" {R(A)} = ea;" if upd else ""
            fn = "psq_load" if op in (56, 57) else "psq_store"
            return ea + f" {fn}(c, {D}, ea, {W}, {I});" + u
        if op == 31:
            return self.op31(w, pc, D, A, B, rc, R, F, P, ra0, RC)
        if op == 59:
            return self.op59(w, D, A, B, C, rc, F, P)
        if op == 63:
            return self.op63(w, D, A, B, C, rc, F, P)
        if op == 4:
            return self.op4(w, D, A, B, C, rc, F, P, R, ra0)
        raise KeyError

    def load(self, kind, dst):
        return {"w": f"{dst} = mr32(ea);", "bz": f"{dst} = mr8(ea);", "hz": f"{dst} = mr16(ea);",
                "ha": f"{dst} = (u32)(s32)(s16)mr16(ea);"}[kind]

    def store(self, kind, src):
        return {"w": f"mw32(ea, {src});", "b": f"mw8(ea, (u8){src});", "h": f"mw16(ea, (u16){src});"}[kind]

    def op31(self, w, pc, D, A, B, rc, R, F, P, ra0, RC):
        xo = (w >> 1) & 0x3FF
        xo9 = (w >> 1) & 0x1FF
        S = D
        # XO-form arithmetic (OE ignored)
        arith = {266: f"{R(A)} + {R(B)}", 40: f"{R(B)} - {R(A)}", 104: f"-{R(A)}",
                 235: f"(u32)((s64)(s32){R(A)} * (s32){R(B)})", 75: f"(u32)(((s64)(s32){R(A)} * (s32){R(B)}) >> 32)",
                 11: f"(u32)(((u64){R(A)} * {R(B)}) >> 32)", 491: f"divw((s32){R(A)}, (s32){R(B)})",
                 459: f"divwu({R(A)}, {R(B)})",
                 10: f"addc(c, {R(A)}, {R(B)}, 0)", 138: f"addc(c, {R(A)}, {R(B)}, c->xer_ca)",
                 202: f"addc(c, {R(A)}, 0, c->xer_ca)", 234: f"addc(c, {R(A)}, 0xFFFFFFFF, c->xer_ca)",
                 8: f"addc(c, ~{R(A)}, {R(B)}, 1)", 136: f"addc(c, ~{R(A)}, {R(B)}, c->xer_ca)",
                 200: f"addc(c, ~{R(A)}, 0, c->xer_ca)", 232: f"addc(c, ~{R(A)}, 0xFFFFFFFF, c->xer_ca)"}
        if xo9 in arith:
            return f"{R(D)} = {arith[xo9]};" + RC(R(D))
        logic = {28: "&", 444: "|", 316: "^", 60: "& ~", 412: "| ~"}
        if xo in logic:
            return f"{R(A)} = {R(S)} {logic[xo]}{R(B)};" + RC(R(A))
        if xo == 476:
            return f"{R(A)} = ~({R(S)} & {R(B)});" + RC(R(A))
        if xo == 124:
            return f"{R(A)} = ~({R(S)} | {R(B)});" + RC(R(A))
        if xo == 284:
            return f"{R(A)} = ~({R(S)} ^ {R(B)});" + RC(R(A))
        if xo == 26:
            return f"{R(A)} = {R(S)} ? __builtin_clz({R(S)}) : 32;" + RC(R(A))
        if xo == 954:
            return f"{R(A)} = (u32)(s32)(s8){R(S)};" + RC(R(A))
        if xo == 922:
            return f"{R(A)} = (u32)(s32)(s16){R(S)};" + RC(R(A))
        if xo == 24:
            return f"{{ u32 n = {R(B)} & 63; {R(A)} = n >= 32 ? 0 : {R(S)} << n; }}" + RC(R(A))
        if xo == 536:
            return f"{{ u32 n = {R(B)} & 63; {R(A)} = n >= 32 ? 0 : {R(S)} >> n; }}" + RC(R(A))
        if xo == 792:
            return f"{R(A)} = sraw(c, {R(S)}, {R(B)});" + RC(R(A))
        if xo == 824:
            return f"{R(A)} = sraw(c, {R(S)}, {B});" + RC(R(A))
        if xo in (0, 32):
            fn = "cmps" if xo == 0 else "cmpu"
            cast = "(s32)" if xo == 0 else ""
            return f"{fn}(c, {D >> 2}, {cast}{R(A)}, {cast}{R(B)});"
        # indexed loads / stores
        ea = f"ea = {ra0} + {R(B)};"
        eau = f"ea = {R(A)} + {R(B)};"
        ldx = {23: ("w", 0), 55: ("w", 1), 87: ("bz", 0), 119: ("bz", 1), 279: ("hz", 0), 311: ("hz", 1), 343: ("ha", 0), 375: ("ha", 1)}
        stx = {151: ("w", 0), 183: ("w", 1), 215: ("b", 0), 247: ("b", 1), 407: ("h", 0), 439: ("h", 1)}
        if xo in ldx:
            k, u = ldx[xo]
            return (eau if u else ea) + " " + self.load(k, R(D)) + (f" {R(A)} = ea;" if u else "")
        if xo in stx:
            k, u = stx[xo]
            return (eau if u else ea) + " " + self.store(k, R(S)) + (f" {R(A)} = ea;" if u else "")
        fx = {535: ("lfs", 0), 567: ("lfs", 1), 599: ("lfd", 0), 631: ("lfd", 1),
              663: ("stfs", 0), 695: ("stfs", 1), 727: ("stfd", 0), 759: ("stfd", 1)}
        if xo in fx:
            k, u = fx[xo]
            e = eau if u else ea
            uu = f" {R(A)} = ea;" if u else ""
            if k == "lfs":
                return e + f" {F(D)} = lfs(ea); {P(D)} = {F(D)};" + uu
            if k == "lfd":
                return e + f" {F(D)} = lfd(ea);" + uu
            return e + f" {k}(ea, {F(D)});" + uu
        if xo == 983:
            return ea + f" mw32(ea, (u32)d2u({F(D)}));"
        if xo == 534:
            return ea + f" {R(D)} = __builtin_bswap32(mr32(ea));"
        if xo == 790:
            return ea + f" {R(D)} = __builtin_bswap16(mr16(ea));"
        if xo == 662:
            return ea + f" mw32(ea, __builtin_bswap32({R(S)}));"
        if xo == 918:
            return ea + f" mw16(ea, __builtin_bswap16((u16){R(S)}));"
        if xo == 20:
            return ea + f" c->reserve = ea; {R(D)} = mr32(ea);"
        if xo == 150:
            return ea + f" mw32(ea, {R(S)}); setcr(c, 0, 2 | c->xer_so);"
        if xo == 597:
            return f"ppc_lswi(c, {D}, {ra0}, {B or 32});"
        if xo == 725:
            return f"ppc_stswi(c, {D}, {ra0}, {B or 32});"
        if xo == 1014:
            return ea + " ppc_dcbz(c, ea);"
        if xo == 982:
            return ea + " ppc_icbi(c, ea);"
        if xo in (54, 86, 246, 278, 470, 598, 854, 566, 306):
            return ";"  # cache hints / sync / eieio / tlbsync / tlbie
        if xo == 19:
            return f"{R(D)} = c->cr;"
        if xo == 144:
            fm = (w >> 12) & 0xFF
            m = 0
            for i in range(8):
                if fm & (0x80 >> i):
                    m |= 0xF << (28 - 4 * i)
            return f"c->cr = (c->cr & 0x{(~m) & 0xFFFFFFFF:08X}u) | ({R(S)} & 0x{m:08X}u);"
        if xo == 512:
            return f"setcr(c, {D >> 2}, (c->xer_so << 3) | (c->xer_ov << 2) | (c->xer_ca << 1)); c->xer_so = c->xer_ov = c->xer_ca = 0;"
        if xo == 83:
            return f"{R(D)} = c->msr;"
        if xo == 146:
            return f"ppc_mtmsr(c, {R(S)});"
        if xo in (339, 467, 371):
            spr = ((w >> 16) & 0x1F) | (((w >> 11) & 0x1F) << 5)
            if xo == 371:
                return f"{R(D)} = ppc_mftb(c, {1 if spr == 269 else 0});"
            if xo == 339:
                if spr == 8:
                    return f"{R(D)} = c->lr;"
                if spr == 9:
                    return f"{R(D)} = c->ctr;"
                if spr == 1:
                    return f"{R(D)} = (c->xer_so << 31) | (c->xer_ov << 30) | (c->xer_ca << 29) | c->xer_bc;"
                if 912 <= spr <= 919:
                    return f"{R(D)} = c->gqr[{spr - 912}];"
                return f"{R(D)} = ppc_mfspr(c, {spr});"
            if spr == 8:
                return f"c->lr = {R(S)};"
            if spr == 9:
                return f"c->ctr = {R(S)};"
            if spr == 1:
                return (f"c->xer_so = {R(S)} >> 31; c->xer_ov = ({R(S)} >> 30) & 1; c->xer_ca = ({R(S)} >> 29) & 1; "
                        f"c->xer_bc = {R(S)} & 0x7F;")
            if 912 <= spr <= 919:
                return f"c->gqr[{spr - 912}] = {R(S)};"
            return f"ppc_mtspr(c, {spr}, {R(S)});"
        if xo in (595, 659, 210, 242):
            return ";" if xo in (210, 242) else f"{R(D)} = 0;"   # segment registers
        if xo == 4:
            return f"ppc_trap(c, 0x{pc:08X});" if D == 31 else ";"
        raise KeyError

    def op59(self, w, D, A, B, C, rc, F, P):
        xo = (w >> 1) & 31
        e = {18: f"{F(A)} / {F(B)}", 20: f"{F(A)} - {F(B)}", 21: f"{F(A)} + {F(B)}", 22: f"sqrt({F(B)})",
             24: f"1.0 / {F(B)}", 25: f"{F(A)} * {F(C)}", 28: f"fma({F(A)}, {F(C)}, -{F(B)})",
             29: f"fma({F(A)}, {F(C)}, {F(B)})", 30: f"-fma({F(A)}, {F(C)}, -{F(B)})", 31: f"-fma({F(A)}, {F(C)}, {F(B)})"}[xo]
        return f"{F(D)} = rs({e}); {P(D)} = {F(D)};" + (" setcr(c, 1, c->fpscr >> 28);" if rc else "")

    def op63(self, w, D, A, B, C, rc, F, P):
        xo5 = (w >> 1) & 31
        tail = " setcr(c, 1, c->fpscr >> 28);" if rc else ""
        if xo5 >= 18 and xo5 not in (19, 27) and not (xo5 == 24):
            e = {18: f"{F(A)} / {F(B)}", 20: f"{F(A)} - {F(B)}", 21: f"{F(A)} + {F(B)}", 22: f"sqrt({F(B)})",
                 23: f"({F(A)} >= 0.0 ? {F(C)} : {F(B)})", 25: f"{F(A)} * {F(C)}", 26: f"frsqrte({F(B)})",
                 28: f"fma({F(A)}, {F(C)}, -{F(B)})", 29: f"fma({F(A)}, {F(C)}, {F(B)})",
                 30: f"-fma({F(A)}, {F(C)}, -{F(B)})", 31: f"-fma({F(A)}, {F(C)}, {F(B)})"}[xo5]
            return f"{F(D)} = {e};" + tail
        xo = (w >> 1) & 0x3FF
        if xo in (0, 32):
            return f"fcmp(c, {D >> 2}, {F(A)}, {F(B)});"
        if xo == 12:
            return f"{F(D)} = rs({F(B)}); {P(D)} = {F(D)};" + tail
        if xo == 14:
            return f"{F(D)} = fctiw({F(B)});" + tail
        if xo == 15:
            return f"{F(D)} = fctiwz({F(B)});" + tail
        if xo == 40:
            return f"{F(D)} = -{F(B)};" + tail
        if xo == 72:
            return f"{F(D)} = {F(B)};" + tail
        if xo == 136:
            return f"{F(D)} = -fabs({F(B)});" + tail
        if xo == 264:
            return f"{F(D)} = fabs({F(B)});" + tail
        if xo == 583:
            return f"{F(D)} = u2d(c->fpscr);"
        if xo == 711:
            fm = (w >> 17) & 0xFF
            m = 0
            for i in range(8):
                if fm & (0x80 >> i):
                    m |= 0xF << (28 - 4 * i)
            return f"c->fpscr = (c->fpscr & 0x{(~m) & 0xFFFFFFFF:08X}u) | ((u32)d2u({F(B)}) & 0x{m:08X}u);"
        if xo == 38:
            return f"c->fpscr |= 1u << {31 - D};"
        if xo == 70:
            return f"c->fpscr &= ~(1u << {31 - D});"
        if xo == 134:
            imm = (w >> 12) & 0xF
            sh = 28 - 4 * (D >> 2)
            return f"c->fpscr = (c->fpscr & ~(0xFu << {sh})) | ({imm}u << {sh});"
        if xo == 64:
            return f"setcr(c, {D >> 2}, (c->fpscr >> {28 - 4 * (A >> 2)}) & 0xF);"
        raise KeyError

    def op4(self, w, D, A, B, C, rc, F, P, R, ra0):
        xo5 = (w >> 1) & 31
        if xo5 in (6, 7):
            xo6 = (w >> 1) & 0x3F
            W = (w >> 10) & 1; I = (w >> 7) & 7
            upd = xo6 in (38, 39)
            ea = f"ea = {R(A) if upd else ra0} + {R(B)};"
            fn = "psq_load" if xo6 in (6, 38) else "psq_store"
            return ea + f" {fn}(c, {D}, ea, {W}, {I});" + (f" {R(A)} = ea;" if upd else "")
        if xo5 in (0, 8, 16, 22):
            xo = (w >> 1) & 0x3FF
            if xo in (0, 32):
                return f"fcmp(c, {D >> 2}, {F(A)}, {F(B)});"
            if xo in (64, 96):
                return f"fcmp(c, {D >> 2}, {P(A)}, {P(B)});"
            un = {40: "-x", 72: "x", 136: "-fabs(x)", 264: "fabs(x)"}
            if xo in un:
                return (f"t0 = {un[xo].replace('x', F(B))}; t1 = {un[xo].replace('x', P(B))}; "
                        f"{F(D)} = t0; {P(D)} = t1;")
            mg = {528: (F(A), F(B)), 560: (F(A), P(B)), 592: (P(A), F(B)), 624: (P(A), P(B))}
            if xo in mg:
                return f"t0 = {mg[xo][0]}; t1 = {mg[xo][1]}; {F(D)} = t0; {P(D)} = t1;"
            if xo == 1014:
                return f"ppc_dcbz(c, {ra0} + {R(B)});"
            raise KeyError
        a0, a1, b0, b1, c0, c1 = F(A), P(A), F(B), P(B), F(C), P(C)
        ops = {
            18: (f"{a0} / {b0}", f"{a1} / {b1}"), 20: (f"{a0} - {b0}", f"{a1} - {b1}"),
            21: (f"{a0} + {b0}", f"{a1} + {b1}"), 23: (f"({a0} >= 0.0 ? {c0} : {b0})", f"({a1} >= 0.0 ? {c1} : {b1})"),
            24: (f"1.0 / {b0}", f"1.0 / {b1}"), 25: (f"{a0} * {c0}", f"{a1} * {c1}"),
            26: (f"frsqrte({b0})", f"frsqrte({b1})"),
            28: (f"fma({a0}, {c0}, -{b0})", f"fma({a1}, {c1}, -{b1})"), 29: (f"fma({a0}, {c0}, {b0})", f"fma({a1}, {c1}, {b1})"),
            30: (f"-fma({a0}, {c0}, -{b0})", f"-fma({a1}, {c1}, -{b1})"), 31: (f"-fma({a0}, {c0}, {b0})", f"-fma({a1}, {c1}, {b1})"),
            10: (f"{a0} + {b1}", c1), 11: (c0, f"{a0} + {b1}"),
            12: (f"{a0} * {c0}", f"{a1} * {c0}"), 13: (f"{a0} * {c1}", f"{a1} * {c1}"),
            14: (f"fma({a0}, {c0}, {b0})", f"fma({a1}, {c0}, {b1})"), 15: (f"fma({a0}, {c1}, {b0})", f"fma({a1}, {c1}, {b1})"),
        }
        e0, e1 = ops[xo5]
        return f"t0 = rs({e0}); t1 = rs({e1}); {F(D)} = t0; {P(D)} = t1;" + (" setcr(c, 1, c->fpscr >> 28);" if rc else "")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dol"); ap.add_argument("symbols"); ap.add_argument("out")
    ap.add_argument("--per-file", type=int, default=150)
    ap.add_argument("--names", help="extra 'ADDR name' lines (e.g. reference-decomp identifications)")
    a = ap.parse_args()
    secs, entry = load_dol(a.dol)
    img = Image(secs)
    funcs, tables, names = parse_symbols(a.symbols)
    if a.names:
        taken = set(names.values())
        for l in open(a.names):
            ad, nm = l.split()
            ad = int(ad, 16)
            if re.fullmatch(r"(fn|dtor)_[0-9A-F]{8}", names.get(ad, "fn_00000000")) and nm not in taken:
                names[ad] = nm; taken.add(nm)
    g = Gen(img, funcs, tables, names)
    g.add_data_switch_targets()
    g.entry_for(entry)
    g.close_entries()
    os.makedirs(a.out, exist_ok=True)
    order = sorted(g.entries)
    nfile, illegal = 0, 0
    for i in range(0, len(order), a.per_file):
        chunk = order[i:i + a.per_file]
        with open(os.path.join(a.out, f"gen_{nfile:03d}.c"), "w") as f:
            f.write('#include "cpu.h"\n')
            for e in chunk:
                f.write(f"void {g.fn(e)}(CPU *c);\n")
            # prototypes of everything this file calls
            body = "".join(g.emit_function(e) for e in chunk)
            called = sorted(set(re.findall(r"\bf_[0-9A-F]{8}\b", body)) - {g.fn(e) for e in chunk})
            f.write("".join(f"void {n}(CPU *c);\n" for n in called))
            f.write(body)
            illegal += body.count("ppc_illegal(")
        nfile += 1
    with open(os.path.join(a.out, "functab.c"), "w") as f:
        f.write('#include "cpu.h"\n')
        f.write("".join(f"void {g.fn(e)}(CPU *c);\n" for e in order))
        f.write(f"const u32 g_fn_count = {len(order)};\nconst u32 g_entry = 0x{entry:08X};\n")
        f.write("const u32 g_fn_addr[] = {" + ",".join(f"0x{e:08X}" for e in order) + "};\n")
        f.write("const GuestFn g_fn_ptr[] = {" + ",".join(g.fn(e) for e in order) + "};\n")
    with open(os.path.join(a.out, "names.txt"), "w") as f:
        for e in order:
            f.write(f"{e:08X} {names.get(e, '')}\n")
    # FN_<name> -> f_ADDR, for host replacements (HLE) of named functions
    with open(os.path.join(a.out, "names.h"), "w") as f:
        f.write("#pragma once\n")
        seen = set()
        for e in order:
            n = names.get(e, "")
            if re.fullmatch(r"[A-Za-z_]\w*", n) and n not in seen:
                seen.add(n)
                f.write(f"#define FN_{n} f_{e:08X}\n#define ADDR_{n} 0x{e:08X}u\n")
        for n, ad in sorted(DATA.items()):
            f.write(f"#define DATA_{n} 0x{ad:08X}u\n")
    print(f"{len(order)} entries ({len(order) - len(funcs)} extra), {nfile} files, {illegal} undecoded instructions")


if __name__ == "__main__":
    main()
