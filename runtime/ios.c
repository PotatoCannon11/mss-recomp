/* IOS (the Wii's I/O coprocessor) at its IPC API: IOS_Open/Close/Read/Write/Seek/Ioctl(v) and
 * their async forms. Devices:
 *   /dev/fs        NAND file system, backed by a host directory (saves, SYSCONF)
 *   NAND paths     files opened by absolute path
 *   /dev/es        title identity (title ID, data directory)
 * Anything else fails to open, as an absent device would. Async completions are delivered from
 * the interrupt path, like IPC replies. */
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#include "runtime.h"
#include "names.h"

#define HLE(name) void FN_##name(CPU *c)
#define LOG(...) do { if (ENV("RT_HLELOG")) fprintf(stderr, __VA_ARGS__); } while (0)

#define TITLE_ID 0x00010000524D4245ull   /* RMBE */
enum { FS_INVALID = -101, FS_ACCESS = -102, FS_EXISTS = -105, FS_NOEXISTS = -106, FS_NOTEMPTY = -115, IPC_ENOENT = -6, IPC_EINVAL = -4 };

static char nand_root[512];

typedef struct { int used, kind; FILE *f; char path[96]; u32 mode; } Fd;   /* kind: 1 fs, 2 file, 3 es, 4 null */
static Fd fds[32];

static void host_path(const char *nand, char *out, size_t n) { snprintf(out, n, "%s%s", nand_root, nand); }

static void mkdirs(const char *path) {
    char tmp[600];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++)
        if (*p == '/') { *p = 0; mkdir(tmp, 0755); *p = '/'; }
    mkdir(tmp, 0755);
}

/* a SYSCONF with the settings games read (language, sensor bar, rumble, speaker, display) */
static void put_item(u8 *buf, u32 *off, u16 *offs, int *n, const char *name, int type, const void *data, int len) {
    offs[(*n)++] = (u16)*off;
    int nl = (int)strlen(name);
    buf[(*off)++] = (u8)((type << 5) | (nl - 1));
    memcpy(buf + *off, name, nl);
    *off += nl;
    if (type == 2) buf[(*off)++] = (u8)(len - 1);
    memcpy(buf + *off, data, len);
    *off += len;
}
static void write_default_sysconf(const char *path) {
    static u8 buf[0x4000];
    memset(buf, 0, sizeof buf);
    u16 offs[64];
    int n = 0;
    u32 off = 0x100;
    u8 one = 1, zero = 0;
    u8 bar = 0;          /* sensor bar below the TV */
    u8 sens[4] = {0, 0, 0, 3};
    u8 spk = 0x58, lng = 1 /* English */, snd = 1 /* stereo */;
    u8 cb[4] = {0, 0, 0, 0};
    put_item(buf, &off, offs, &n, "IPL.LNG", 3, &lng, 1);
    put_item(buf, &off, offs, &n, "IPL.AR", 3, &zero, 1);
    put_item(buf, &off, offs, &n, "IPL.PGS", 3, &zero, 1);
    put_item(buf, &off, offs, &n, "IPL.E60", 3, &one, 1);
    put_item(buf, &off, offs, &n, "IPL.SND", 3, &snd, 1);
    put_item(buf, &off, offs, &n, "IPL.SSV", 3, &one, 1);
    put_item(buf, &off, offs, &n, "IPL.SADR", 2, (u8[0x100]){0}, 0x100);
    put_item(buf, &off, offs, &n, "IPL.CB", 5, cb, 4);
    put_item(buf, &off, offs, &n, "IPL.DH", 3, &zero, 1);
    put_item(buf, &off, offs, &n, "IPL.UPT", 3, &zero, 1);
    put_item(buf, &off, offs, &n, "IPL.EULA", 7, &one, 1);
    put_item(buf, &off, offs, &n, "BT.BAR", 3, &bar, 1);
    put_item(buf, &off, offs, &n, "BT.SENS", 5, sens, 4);
    put_item(buf, &off, offs, &n, "BT.MOT", 3, &one, 1);
    put_item(buf, &off, offs, &n, "BT.SPKV", 3, &spk, 1);
    put_item(buf, &off, offs, &n, "IPL.IDL", 2, (u8[2]){0, 0}, 2);
    memcpy(buf, "SCv0", 4);
    buf[4] = (u8)(n >> 8); buf[5] = (u8)n;
    for (int i = 0; i < n; i++) { buf[6 + 2 * i] = (u8)(offs[i] >> 8); buf[7 + 2 * i] = (u8)offs[i]; }
    buf[6 + 2 * n] = (u8)(off >> 8); buf[7 + 2 * n] = (u8)off;
    memcpy(buf + 0x3FFC, "SCed", 4);
    FILE *f = fopen(path, "wb");
    if (f) { fwrite(buf, 1, sizeof buf, f); fclose(f); }
}

/* /title/00000001/00000002/data/setting.txt: console region/product info, XOR-obfuscated */
static void write_default_setting(const char *path) {
    const char *txt = "AREA=USA\r\nMODEL=RVL-001(USA)\r\nDVD=0\r\nMPCH=0x7FFE\r\nCODE=LU\r\nSERNO=000000000\r\nVIDEO=NTSC\r\nGAME=US\r\n";
    u8 buf[0x100] = {0};
    memcpy(buf, txt, strlen(txt));
    u32 key = 0x73B5DBFA;
    for (int i = 0; i < 0x100; i++) {
        buf[i] ^= (u8)key;
        key = (key << 1) | (key >> 31);
    }
    FILE *f = fopen(path, "wb");
    if (f) { fwrite(buf, 1, sizeof buf, f); fclose(f); }
}

void ios_init(void) {
    const char *r = ENV("RT_NAND");
    if (r) snprintf(nand_root, sizeof nand_root, "%s", r);
    else snprintf(nand_root, sizeof nand_root, "%s/.local/share/mss-recomp/nand", ENV("HOME") ? ENV("HOME") : ".");
    char p[700];
    host_path("/shared2/sys", p, sizeof p);
    mkdirs(p);
    host_path("/title/00010000/524d4245/data", p, sizeof p);
    mkdirs(p);
    host_path("/tmp", p, sizeof p);
    mkdirs(p);
    host_path("/shared2/sys/SYSCONF", p, sizeof p);
    if (access(p, F_OK)) write_default_sysconf(p);
    host_path("/title/00000001/00000002/data", p, sizeof p);
    mkdirs(p);
    host_path("/title/00000001/00000002/data/setting.txt", p, sizeof p);
    if (access(p, F_OK)) write_default_setting(p);
    fprintf(stderr, "nand: %s\n", nand_root);
}

/* ---------------------------------------------------------------- deferred completion */

typedef struct { u32 cb, result, arg; } Pending;
static Pending pend[256];
static int npend;

static void complete(CPU *c, u32 cb, s32 result, u32 arg, int async) {
    if (!async) { c->r[3] = (u32)result; return; }
    if (npend < 256) pend[npend++] = (Pending){cb, (u32)result, arg};
    c->r[3] = 0;   /* IPC_OK: request queued */
}

int ios_pending(void) { return npend; }

/* called from the interrupt path */
int ios_deliver(CPU *c) {
    if (!npend) return 0;
    Pending p[256];
    int n = npend;
    memcpy(p, pend, sizeof(Pending) * n);
    npend = 0;
    for (int i = 0; i < n; i++)
        guest_call(c, p[i].cb, p[i].result, p[i].arg);
    return 1;
}

/* ---------------------------------------------------------------- operations */

static s32 do_open(const char *path, u32 mode) {
    int fd = -1;
    for (int i = 0; i < 32; i++) if (!fds[i].used) { fd = i; break; }
    if (fd < 0) return -109;
    Fd *f = &fds[fd];
    memset(f, 0, sizeof *f);
    snprintf(f->path, sizeof f->path, "%s", path);
    f->mode = mode;
    if (!strcmp(path, "/dev/fs")) f->kind = 1;
    else if (!strcmp(path, "/dev/es")) f->kind = 3;
    else if (!strcmp(path, "/dev/stm/eventhook")) f->kind = 5;   /* completes only on power/reset events */
    else if (!strcmp(path, "/dev/null") || !strcmp(path, "/dev/stm/immediate")) f->kind = 4;
    else if (path[0] == '/' && strncmp(path, "/dev/", 5)) {
        char hp[700];
        host_path(path, hp, sizeof hp);
        struct stat st;
        if (stat(hp, &st) || S_ISDIR(st.st_mode)) return FS_NOEXISTS;
        f->f = fopen(hp, (mode & 2) ? "r+b" : "rb");
        if (!f->f) return FS_ACCESS;
        f->kind = 2;
    } else
        return IPC_ENOENT;
    f->used = 1;
    return fd;
}

static Fd *getfd(s32 fd) { return fd >= 0 && fd < 32 && fds[fd].used ? &fds[fd] : NULL; }

static s32 do_close(s32 fd) {
    Fd *f = getfd(fd);
    LOG("ios: close fd %d\n", fd);
    if (!f) return IPC_EINVAL;
    if (f->f) fclose(f->f);
    f->used = 0;
    return 0;
}

static s32 do_read(s32 fd, u32 buf, u32 len) {
    Fd *f = getfd(fd);
    LOG("ios: read fd %d (%s) %u bytes\n", fd, f ? f->path : "?", len);
    if (!f || f->kind != 2) return IPC_EINVAL;
    return (s32)fread(MEM + buf, 1, len, f->f);
}
static s32 do_write(s32 fd, u32 buf, u32 len) {
    Fd *f = getfd(fd);
    if (!f || f->kind != 2) return IPC_EINVAL;
    s32 n = (s32)fwrite(MEM + buf, 1, len, f->f);
    fflush(f->f);
    return n;
}
static s32 do_seek(s32 fd, s32 off, s32 whence) {
    Fd *f = getfd(fd);
    LOG("ios: seek fd %d %d/%d\n", fd, off, whence);
    if (!f || f->kind != 2) return IPC_EINVAL;
    if (fseek(f->f, off, whence == 0 ? SEEK_SET : whence == 1 ? SEEK_CUR : SEEK_END)) return FS_INVALID;
    return (s32)ftell(f->f);
}

static void gpath(u32 a, char *out) { memcpy(out, MEM + a, 64); out[63] = 0; }

static s32 fs_ioctl(Fd *f, u32 cmd, u32 in, u32 inlen, u32 out, u32 outlen) {
    char p[64], p2[64], hp[700], hp2[700];
    (void)inlen;
    switch (cmd) {
    case 2:   /* GETSTATS */
        if (outlen >= 28) {
            memset(MEM + out, 0, 28);
            mw32(out + 0, 0x4000); mw32(out + 4, 0x4000); mw32(out + 8, 0x100);
        }
        return 0;
    case 3: case 9: {   /* CREATEDIR / CREATEFILE: attr block, path at +6 */
        gpath(in + 6, p);
        host_path(p, hp, sizeof hp);
        struct stat st;
        if (!stat(hp, &st)) return FS_EXISTS;
        if (cmd == 3) return mkdir(hp, 0755) ? FS_NOEXISTS : 0;
        FILE *x = fopen(hp, "wb");
        if (!x) return FS_NOEXISTS;
        fclose(x);
        return 0;
    }
    case 5:   /* SETATTR */
        return 0;
    case 6: {   /* GETATTR: out = owner(4) group(2) path(64) perms(3) attr(1) */
        gpath(in, p);
        host_path(p, hp, sizeof hp);
        struct stat st;
        if (stat(hp, &st)) return FS_NOEXISTS;
        memset(MEM + out, 0, outlen);
        mw32(out, 0x1000); mw16(out + 4, 0x3031);
        memcpy(MEM + out + 6, p, 64);
        mw8(out + 70, 3); mw8(out + 71, 3); mw8(out + 72, 3);
        return 0;
    }
    case 7: {   /* DELETE */
        gpath(in, p);
        host_path(p, hp, sizeof hp);
        struct stat st;
        if (stat(hp, &st)) return FS_NOEXISTS;
        if (S_ISDIR(st.st_mode)) return rmdir(hp) ? FS_NOTEMPTY : 0;
        return unlink(hp) ? FS_ACCESS : 0;
    }
    case 8: {   /* RENAME */
        gpath(in, p);
        gpath(in + 64, p2);
        host_path(p, hp, sizeof hp);
        host_path(p2, hp2, sizeof hp2);
        return rename(hp, hp2) ? FS_NOEXISTS : 0;
    }
    case 11: {   /* GETFILESTATS (on a file descriptor) */
        if (!f || f->kind != 2) return IPC_EINVAL;
        long pos = ftell(f->f);
        fseek(f->f, 0, SEEK_END);
        long sz = ftell(f->f);
        fseek(f->f, pos, SEEK_SET);
        mw32(out, (u32)sz);
        mw32(out + 4, (u32)pos);
        return 0;
    }
    case 13:   /* SHUTDOWN */
        return 0;
    }
    LOG("fs: ioctl %u unhandled\n", cmd);
    return 0;
}

static s32 fs_ioctlv(u32 cmd, u32 nin, u32 nout, u32 vec) {
    u32 b[8], l[8];
    for (u32 i = 0; i < nin + nout && i < 8; i++) { b[i] = mr32(vec + 8 * i); l[i] = mr32(vec + 8 * i + 4); }
    char p[64], hp[700];
    if (cmd == 4) {   /* READDIR: in path [, max]; out [names,] count */
        gpath(b[0], p);
        host_path(p, hp, sizeof hp);
        DIR *d = opendir(hp);
        if (!d) return FS_NOEXISTS;
        u32 max = nin > 1 ? mr32(b[1]) : 0xFFFFFFFF, count = 0, pos = 0;
        struct dirent *e;
        while ((e = readdir(d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            if (nout > 1 && count < max) {
                size_t ln = strlen(e->d_name) + 1;
                if (pos + ln <= l[nin]) { memcpy(MEM + b[nin] + pos, e->d_name, ln); pos += (u32)ln; }
            }
            count++;
        }
        closedir(d);
        if (nout > 1 && count > max) count = max;
        mw32(b[nin + nout - 1], count);
        return 0;
    }
    if (cmd == 12) {   /* GETUSAGE: in path; out blocks, inodes */
        mw32(b[nin], 1);
        mw32(b[nin + 1], 1);
        return 0;
    }
    LOG("fs: ioctlv %u unhandled\n", cmd);
    return 0;
}

static s32 es_ioctlv(u32 cmd, u32 nin, u32 nout, u32 vec) {
    u32 b[8], l[8];
    for (u32 i = 0; i < nin + nout && i < 8; i++) { b[i] = mr32(vec + 8 * i); l[i] = mr32(vec + 8 * i + 4); }
    switch (cmd) {
    case 0x20:   /* GETTITLEID */
        mw32(b[nin], (u32)(TITLE_ID >> 32)); mw32(b[nin] + 4, (u32)TITLE_ID);
        return 0;
    case 0x1D: {   /* GETDATADIR */
        char path[32];
        u64 t = (u64)mr32(b[0]) << 32 | mr32(b[0] + 4);
        snprintf(path, sizeof path, "/title/%08x/%08x/data", (u32)(t >> 32), (u32)t);
        memcpy(MEM + b[nin], path, strlen(path) + 1);
        return 0;
    }
    default:
        LOG("es: ioctlv %X (%u in, %u out)\n", cmd, nin, nout);
        for (u32 i = nin; i < nin + nout && i < 8; i++) memset(MEM + b[i], 0, l[i]);
        return 0;
    }
}

static s32 do_ioctl(s32 fd, u32 cmd, u32 in, u32 inlen, u32 out, u32 outlen) {
    Fd *f = getfd(fd);
    LOG("ios: ioctl fd %d (%s) cmd %u\n", fd, f ? f->path : "?", cmd);
    if (!f) return IPC_EINVAL;
    if (f->kind == 1 || f->kind == 2) return fs_ioctl(f, cmd, in, inlen, out, outlen);
    if (out && outlen) memset(MEM + out, 0, outlen);
    return 0;
}
static s32 do_ioctlv(s32 fd, u32 cmd, u32 nin, u32 nout, u32 vec) {
    Fd *f = getfd(fd);
    LOG("ios: ioctlv fd %d (%s) cmd %u\n", fd, f ? f->path : "?", cmd);
    if (!f) return IPC_EINVAL;
    if (f->kind == 1) return fs_ioctlv(cmd, nin, nout, vec);
    if (f->kind == 3) return es_ioctlv(cmd, nin, nout, vec);
    return 0;
}

/* ---------------------------------------------------------------- the IPC API */

HLE(IOS_Open) { s32 r = do_open((char *)MEM + c->r[3], c->r[4]); LOG("IOS_Open(%s) = %d\n", (char *)MEM + c->r[3], r); c->r[3] = (u32)r; }
HLE(IOS_OpenAsync) {
    s32 r = do_open((char *)MEM + c->r[3], c->r[4]);
    LOG("IOS_OpenAsync(%s) = %d\n", (char *)MEM + c->r[3], r);
    complete(c, c->r[5], r, c->r[6], 1);
}
HLE(IOS_Close) { c->r[3] = (u32)do_close((s32)c->r[3]); }
HLE(IOS_CloseAsync) { complete(c, c->r[4], do_close((s32)c->r[3]), c->r[5], 1); }
HLE(IOS_Read) { c->r[3] = (u32)do_read((s32)c->r[3], c->r[4], c->r[5]); }
HLE(IOS_ReadAsync) { complete(c, c->r[6], do_read((s32)c->r[3], c->r[4], c->r[5]), c->r[7], 1); }
HLE(IOS_Write) { c->r[3] = (u32)do_write((s32)c->r[3], c->r[4], c->r[5]); }
HLE(IOS_WriteAsync) { complete(c, c->r[6], do_write((s32)c->r[3], c->r[4], c->r[5]), c->r[7], 1); }
HLE(IOS_Seek) { c->r[3] = (u32)do_seek((s32)c->r[3], (s32)c->r[4], (s32)c->r[5]); }
HLE(IOS_SeekAsync) { complete(c, c->r[6], do_seek((s32)c->r[3], (s32)c->r[4], (s32)c->r[5]), c->r[7], 1); }
HLE(IOS_Ioctl) { c->r[3] = (u32)do_ioctl((s32)c->r[3], c->r[4], c->r[5], c->r[6], c->r[7], c->r[8]); }
HLE(IOS_IoctlAsync) {
    Fd *f = getfd((s32)c->r[3]);
    if (f && f->kind == 5) { c->r[3] = 0; return; }   /* event hook: stays pending */
    complete(c, c->r[9], do_ioctl((s32)c->r[3], c->r[4], c->r[5], c->r[6], c->r[7], c->r[8]), c->r[10], 1); }
HLE(IOS_Ioctlv) { c->r[3] = (u32)do_ioctlv((s32)c->r[3], c->r[4], c->r[5], c->r[6], c->r[7]); }
HLE(IOS_IoctlvAsync) { complete(c, c->r[8], do_ioctlv((s32)c->r[3], c->r[4], c->r[5], c->r[6], c->r[7]), c->r[9], 1); }
HLE(IOS_IoctlvReboot) { c->r[3] = 0; }
