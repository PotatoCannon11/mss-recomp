#!/usr/bin/env python3
"""save.py: import / back up / restore Mario Super Sluggers saves for the recomp's NAND.

    tools/save.py import [SOURCE]   SOURCE: a Wii SD-card export (data.bin), a Dolphin save folder,
                                    a folder of save files, a Dolphin user/NAND dir, or a .zip of any
                                    of these. Without SOURCE, Dolphin's save is used if one is found.
    tools/save.py backup            copy the current save to the backups folder
    tools/save.py restore [NAME]    put a backup back (newest if NAME is omitted)
    tools/save.py list              show the current save, Dolphin saves found and backups

The current save is always backed up before an import or restore replaces it.
"""
import hashlib, io, os, shutil, struct, subprocess, sys, tempfile, time, zipfile

TITLE = "00010000/524d4245"            # RMBE: Mario Super Sluggers (USA)
TITLE_ID = 0x00010000524D4245
HOME = os.path.expanduser("~")
NAND = os.environ.get("RT_NAND", os.path.join(HOME, ".local/share/mss-recomp/nand"))
SAVE = os.path.join(NAND, "title", TITLE, "data")
BACKUPS = os.path.join(HOME, ".local/share/mss-recomp/save-backups")
DOLPHIN_ROOTS = [
    os.path.join(HOME, ".var/app/org.DolphinEmu.dolphin-emu/data/dolphin-emu"),
    os.path.join(HOME, ".local/share/dolphin-emu"),
    os.path.join(HOME, ".dolphin-emu"),
]

# ---------------------------------------------------------------- AES-128-CBC (decrypt only)

SBOX = bytearray(256)
INV = bytearray(256)


def _init_sbox():
    p = q = 1
    while True:
        p = p ^ ((p << 1) & 0xFF) ^ (0x1B if p & 0x80 else 0)
        q ^= q << 1; q ^= q << 2; q ^= q << 4; q &= 0xFF
        if q & 0x80: q ^= 0x09
        x = q ^ (q << 1 | q >> 7) ^ (q << 2 | q >> 6) ^ (q << 3 | q >> 5) ^ (q << 4 | q >> 4)
        SBOX[p] = (x ^ 0x63) & 0xFF
        if p == 1: break
    SBOX[0] = 0x63
    for i in range(256): INV[SBOX[i]] = i


_init_sbox()


def _xt(b): return ((b << 1) ^ 0x1B) & 0xFF if b & 0x80 else b << 1


def _mul(a, b):
    r = 0
    while b:
        if b & 1: r ^= a
        a = _xt(a); b >>= 1
    return r


MUL = {k: bytes(_mul(i, k) for i in range(256)) for k in (9, 11, 13, 14)}


def _expand(key):
    w = [list(key[i:i + 4]) for i in range(0, 16, 4)]
    rcon = 1
    for i in range(4, 44):
        t = list(w[i - 1])
        if i % 4 == 0:
            t = [SBOX[b] for b in t[1:] + t[:1]]; t[0] ^= rcon; rcon = _xt(rcon)
        w.append([a ^ b for a, b in zip(w[i - 4], t)])
    return [sum(w[r * 4:r * 4 + 4], []) for r in range(11)]


def _decrypt_block(rk, blk):
    s = [b ^ k for b, k in zip(blk, rk[10])]
    for r in range(9, -1, -1):
        s = [s[(i - 4 * (i % 4)) % 16] for i in range(16)]          # inverse ShiftRows
        s = [INV[b] ^ k for b, k in zip(s, rk[r])]                   # inverse SubBytes + AddRoundKey
        if r:
            m9, m11, m13, m14 = MUL[9], MUL[11], MUL[13], MUL[14]
            o = []
            for c in range(0, 16, 4):
                a0, a1, a2, a3 = s[c:c + 4]
                o += [m14[a0] ^ m11[a1] ^ m13[a2] ^ m9[a3], m9[a0] ^ m14[a1] ^ m11[a2] ^ m13[a3],
                      m13[a0] ^ m9[a1] ^ m14[a2] ^ m11[a3], m11[a0] ^ m13[a1] ^ m9[a2] ^ m14[a3]]
            s = o
    return bytes(s)


def aes_cbc_decrypt(key, iv, data):
    rk, out, prev = _expand(key), bytearray(), iv
    for i in range(0, len(data), 16):
        blk = data[i:i + 16]
        out += bytes(a ^ b for a, b in zip(_decrypt_block(rk, blk), prev))
        prev = blk
    return bytes(out)


assert aes_cbc_decrypt(bytes(range(16)), bytes(16), bytes.fromhex("69c4e0d86a7b0430d8cdb78070b4c55a")) \
    == bytes.fromhex("00112233445566778899aabbccddeeff"), "AES self-test failed"

# ---------------------------------------------------------------- Wii SD-card save (data.bin)

SD_KEY = bytes.fromhex("ab01b9d8e1622b08afbad84dbfc2a55d")
SD_IV = bytes.fromhex("216712e6aa1f689f95c5a22324dc6a98")
MD5_BLANKER = bytes.fromhex("0e65378199be4517ab06ec22451a5793")
HDR_SIZE, BK_SIZE, FILE_HDR = 0xF0C0, 0x80, 0x80


def read_data_bin(raw):
    """data.bin -> {relative path: bytes or None (directory)}"""
    hdr = aes_cbc_decrypt(SD_KEY, SD_IV, raw[:HDR_SIZE])
    tid, banner_size = struct.unpack(">QI", hdr[:12])
    if tid != TITLE_ID:
        sys.exit(f"this data.bin is for title {tid:016X} ({tid.to_bytes(8, 'big')[4:].decode('latin1')}), "
                 f"not Mario Super Sluggers USA (RMBE)")
    if hashlib.md5(hdr[:0xE] + MD5_BLANKER + hdr[0x1E:]).digest() != hdr[0xE:0x1E]:
        print("warning: data.bin header checksum mismatch; importing anyway")
    files = {"banner.bin": hdr[0x20:0x20 + banner_size]}
    bk = raw[HDR_SIZE:HDR_SIZE + BK_SIZE]
    if bk[4:8] != b"Bk\x00\x01":
        sys.exit("data.bin: missing Bk header (not a Wii save export?)")
    count = struct.unpack(">I", bk[0xC:0x10])[0]
    off = HDR_SIZE + BK_SIZE
    for _ in range(count):
        fh = raw[off:off + FILE_HDR]
        magic, size = struct.unpack(">II", fh[:8])
        if magic != 0x03ADF17E:
            sys.exit(f"data.bin: bad file header at 0x{off:X}")
        ftype = fh[0xA]
        name = fh[0xB:0x50].split(b"\0")[0].decode("latin1")
        off += FILE_HDR
        if ftype == 2:
            files[name] = None
            continue
        padded = (size + 0x3F) & ~0x3F
        files[name] = aes_cbc_decrypt(SD_KEY, fh[0x50:0x60], raw[off:off + padded])[:size]
        off += padded
    return files


# ---------------------------------------------------------------- finding saves

def save_dir_in(root):
    """a save folder inside root (Dolphin user dir, NAND root, title dir), or root itself"""
    for sub in ("Wii/title/" + TITLE + "/data", "title/" + TITLE + "/data", TITLE + "/data",
                "524d4245/data", "data", ""):
        d = os.path.join(root, sub)
        if os.path.isfile(os.path.join(d, "gamedata")) or os.path.isfile(os.path.join(d, "banner.bin")):
            return d
    for dirpath, _, names in os.walk(root):        # anything else: a data.bin or gamedata somewhere below
        if "gamedata" in names:
            return dirpath
    return None


def find_data_bin(root):
    for dirpath, _, names in os.walk(root):
        for n in names:
            if n.lower() == "data.bin":
                return os.path.join(dirpath, n)
    return None


def dolphin_saves():
    out = []
    for r in DOLPHIN_ROOTS:
        d = os.path.join(r, "Wii/title", TITLE, "data")
        if os.path.isdir(d) and os.listdir(d):
            out.append(d)
    return out


def load_source(src):
    """-> {relative path: bytes or None}"""
    if os.path.isdir(src):
        d = save_dir_in(src)
        if d is None:
            b = find_data_bin(src)
            if b: return load_source(b)
            sys.exit(f"{src}: no Mario Super Sluggers save found (looked for gamedata / banner.bin / data.bin)")
        files = {}
        for dirpath, dirs, names in os.walk(d):
            for n in dirs:
                files[os.path.relpath(os.path.join(dirpath, n), d)] = None
            for n in names:
                files[os.path.relpath(os.path.join(dirpath, n), d)] = open(os.path.join(dirpath, n), "rb").read()
        print(f"source: save folder {d}")
        return files
    raw = open(src, "rb").read()
    if zipfile.is_zipfile(src):
        with tempfile.TemporaryDirectory() as t:
            with zipfile.ZipFile(src) as z:
                for m in z.namelist():                   # refuse paths that escape the temp dir
                    p = os.path.realpath(os.path.join(t, m))
                    if not p.startswith(os.path.realpath(t) + os.sep):
                        sys.exit(f"{src}: unsafe path in zip: {m}")
                z.extractall(t)
            print(f"source: zip {src}")
            return load_source(t)
    if len(raw) > HDR_SIZE + BK_SIZE and raw[HDR_SIZE + 4:HDR_SIZE + 8] == b"Bk\x00\x01":
        print(f"source: Wii SD-card export {src}")
        return read_data_bin(raw)
    if os.path.basename(src) == "gamedata":
        return load_source(os.path.dirname(src) or ".")
    sys.exit(f"{src}: not a data.bin, save folder or zip")


# ---------------------------------------------------------------- operations

def game_running():
    return subprocess.run(["pgrep", "-x", "mss"], capture_output=True).returncode == 0


def backup(reason="manual"):
    if not os.path.isdir(SAVE) or not os.listdir(SAVE):
        return None
    name = time.strftime("%Y%m%d-%H%M%S") + "-" + reason
    dst = os.path.join(BACKUPS, name)
    shutil.copytree(SAVE, dst)
    print(f"backed up current save -> {dst}")
    return dst


def install(files):
    if not any(v for k, v in files.items() if os.path.basename(k) == "gamedata"):
        print("warning: no 'gamedata' file in this save; the game may not see it")
    if os.path.isdir(SAVE):
        shutil.rmtree(SAVE)
    os.makedirs(SAVE)
    for rel, data in sorted(files.items(), key=lambda kv: kv[0]):
        p = os.path.normpath(os.path.join(SAVE, rel))
        if not p.startswith(SAVE + os.sep):
            sys.exit(f"refusing unsafe path {rel}")
        if data is None:
            os.makedirs(p, exist_ok=True)
        else:
            os.makedirs(os.path.dirname(p), exist_ok=True)
            open(p, "wb").write(data)
    for rel, data in sorted(files.items()):
        if data is not None: print(f"  {rel:<16} {len(data):>8} bytes")
    print(f"installed -> {SAVE}")


def describe(d):
    if not os.path.isdir(d): return "(none)"
    g = os.path.join(d, "gamedata")
    when = time.strftime("%Y-%m-%d %H:%M", time.localtime(os.path.getmtime(g))) if os.path.exists(g) else "no gamedata"
    return f"{d}  [{len(os.listdir(d))} files, gamedata {when}]"


def main():
    a = sys.argv[1:]
    cmd = a[0] if a else "help"
    if cmd in ("import", "restore") and game_running():
        sys.exit("the game is running; quit it first (it would overwrite the imported save)")
    if cmd == "import":
        if len(a) > 1:
            src = os.path.expanduser(a[1])
        else:
            found = dolphin_saves()
            if not found:
                sys.exit("no Dolphin save found; pass a data.bin, folder or zip: tools/save.py import <path>")
            src = found[0]
        files = load_source(src)
        backup("before-import")
        install(files)
    elif cmd == "backup":
        if not backup(): print("no current save to back up")
    elif cmd == "restore":
        names = sorted(os.listdir(BACKUPS)) if os.path.isdir(BACKUPS) else []
        if not names: sys.exit("no backups")
        name = a[1] if len(a) > 1 else names[-1]
        src = os.path.join(BACKUPS, name)
        if not os.path.isdir(src): sys.exit(f"no backup named {name}; see tools/save.py list")
        files = load_source(src)
        backup("before-restore")
        install(files)
    elif cmd == "list":
        print("current save:\n  " + describe(SAVE))
        print("Dolphin saves:")
        for d in dolphin_saves() or ["(none found)"]: print("  " + (describe(d) if d.startswith("/") else d))
        print("backups (" + BACKUPS + "):")
        for n in (sorted(os.listdir(BACKUPS)) if os.path.isdir(BACKUPS) else []) or ["(none)"]: print("  " + n)
    else:
        print(__doc__.strip())


if __name__ == "__main__":
    main()
