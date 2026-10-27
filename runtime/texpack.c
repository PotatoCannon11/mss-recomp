/* Texture dumping and replacement (Dolphin-style custom textures).
 *
 *   RT_TEXDUMP=1 (or a directory)   save every texture the game decodes, once, as PNG
 *                                   (default ~/.local/share/mss-recomp/texdump/RMBE01)
 *   RT_TEXPACK=<dir>                replacement textures (default ~/.local/share/mss-recomp/textures/RMBE01,
 *                                   searched recursively); RT_TEXPACK=0 turns replacement off
 *
 * Files are named tex1_<w>x<h>_<hash>_<fmt>.png, where hash covers the texture's bytes (and its
 * palette), so a name stays the same across runs whatever address the game loads the texture at.
 * A replacement may have any size (texture coordinates are normalised); tools/upscale_textures.py
 * turns a dump into a pack. PNG I/O is built in (zlib): 8-bit RGB / RGBA, non-interlaced. */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <zlib.h>
#include "runtime.h"

static char dump_dir[512];
static int dump_on = -1, pack_on = -1;

/* replacement index: hash -> path (open addressing) */
typedef struct { u64 hash; char *path; } PackEnt;
static PackEnt *pack;
static u32 pack_cap, pack_n;

static void mkdirs(const char *path) {
    char p[512];
    snprintf(p, sizeof p, "%s", path);
    for (char *s = p + 1; *s; s++)
        if (*s == '/') { *s = 0; mkdir(p, 0755); *s = '/'; }
    mkdir(p, 0755);
}

static int parse_name(const char *n, u64 *hash) {
    int w, h, fmt;
    unsigned long long hh;
    if (sscanf(n, "tex1_%dx%d_%16llx_%d.png", &w, &h, &hh, &fmt) != 4) return 0;
    *hash = hh;
    return 1;
}

static void pack_put(u64 hash, const char *path) {
    if ((pack_n + 1) * 2 > pack_cap) {
        u32 oc = pack_cap;
        PackEnt *old = pack;
        pack_cap = oc ? oc * 2 : 4096;
        pack = calloc(pack_cap, sizeof *pack);
        pack_n = 0;
        for (u32 i = 0; i < oc; i++)
            if (old[i].path) pack_put(old[i].hash, old[i].path);
        free(old);
    }
    u32 i = (u32)(hash ^ hash >> 32) & (pack_cap - 1);
    while (pack[i].path && pack[i].hash != hash) i = (i + 1) & (pack_cap - 1);
    if (pack[i].path) { free(pack[i].path); pack_n--; }   /* later files (deeper in the tree) win */
    pack[i].hash = hash;
    pack[i].path = strdup(path);
    pack_n++;
}

static void scan_pack(const char *dir, int depth) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char p[1024];
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(p, &st)) continue;
        u64 h;
        if (S_ISDIR(st.st_mode) && depth < 8) scan_pack(p, depth + 1);
        else if (parse_name(e->d_name, &h)) pack_put(h, p);
    }
    closedir(d);
}

static void init(void) {
    if (dump_on >= 0) return;
    const char *home = ENV("HOME") ? ENV("HOME") : ".";
    const char *td = ENV("RT_TEXDUMP");
    dump_on = td && strcmp(td, "0");
    if (dump_on) {
        if (!strcmp(td, "1")) snprintf(dump_dir, sizeof dump_dir, "%s/.local/share/mss-recomp/texdump/RMBE01", home);
        else snprintf(dump_dir, sizeof dump_dir, "%s", td);
        mkdirs(dump_dir);
        fprintf(stderr, "textures: dumping to %s\n", dump_dir);
    }
    const char *tp = ENV("RT_TEXPACK");
    char dir[512];
    if (tp && !strcmp(tp, "0")) { pack_on = 0; return; }
    if (tp) snprintf(dir, sizeof dir, "%s", tp);
    else snprintf(dir, sizeof dir, "%s/.local/share/mss-recomp/textures/RMBE01", home);
    scan_pack(dir, 0);
    pack_on = pack_n > 0;
    if (pack_on) fprintf(stderr, "textures: %u replacements from %s\n", pack_n, dir);
}

int texpack_active(void) { init(); return dump_on || pack_on; }

/* 64-bit FNV-1a over 8-byte words of the texture data, then its palette and palette format */
u64 texpack_hash(const u8 *data, u32 n, const u8 *pal, u32 pal_n, u32 pal_fmt) {
    u64 h = 0xCBF29CE484222325ull;
    u32 i = 0;
    for (; i + 8 <= n; i += 8) { u64 w; memcpy(&w, data + i, 8); h = (h ^ w) * 0x100000001B3ull; }
    for (; i < n; i++) h = (h ^ data[i]) * 0x100000001B3ull;
    for (i = 0; i < pal_n; i++) h = (h ^ pal[i]) * 0x100000001B3ull;
    if (pal_n) h = (h ^ pal_fmt) * 0x100000001B3ull;
    return h;
}

/* ---------------------------------------------------------------- PNG */

static void put32(u8 *p, u32 v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static u32 get32(const u8 *p) { return (u32)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

static void chunk(FILE *f, const char *type, const u8 *data, u32 n) {
    u8 hdr[8];
    put32(hdr, n);
    memcpy(hdr + 4, type, 4);
    fwrite(hdr, 1, 8, f);
    if (n) fwrite(data, 1, n, f);
    u32 crc = crc32(crc32(0, (const u8 *)type, 4), data, n);
    put32(hdr, crc);
    fwrite(hdr, 1, 4, f);
}

static int write_png(const char *path, int w, int h, const u8 *rgba) {
    size_t raw_n = (size_t)h * (w * 4 + 1);
    u8 *raw = malloc(raw_n);
    for (int y = 0; y < h; y++) {
        raw[y * (w * 4 + 1)] = 0;   /* filter: none */
        memcpy(raw + y * (w * 4 + 1) + 1, rgba + (size_t)y * w * 4, (size_t)w * 4);
    }
    uLongf zn = compressBound(raw_n);
    u8 *z = malloc(zn);
    int ok = compress2(z, &zn, raw, raw_n, 1) == Z_OK;
    free(raw);
    char tmp[600];
    snprintf(tmp, sizeof tmp, "%s.part", path);
    FILE *f = ok ? fopen(tmp, "wb") : NULL;
    if (f) {
        static const u8 sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
        u8 ihdr[13];
        put32(ihdr, w); put32(ihdr + 4, h);
        ihdr[8] = 8; ihdr[9] = 6; ihdr[10] = ihdr[11] = ihdr[12] = 0;   /* 8-bit RGBA */
        fwrite(sig, 1, 8, f);
        chunk(f, "IHDR", ihdr, 13);
        chunk(f, "IDAT", z, (u32)zn);
        chunk(f, "IEND", NULL, 0);
        ok = fclose(f) == 0 && rename(tmp, path) == 0;
    }
    free(z);
    return ok;
}

static u8 paeth(int a, int b, int c) {
    int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
    return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
}

/* -> malloc'd RGBA, NULL if unreadable or unsupported */
static u8 *read_png(const char *path, int *ow, int *oh) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    u8 *buf = malloc(n > 0 ? n : 1);
    if (n < 33 || fread(buf, 1, n, f) != (size_t)n || memcmp(buf + 1, "PNG", 3)) { fclose(f); free(buf); return NULL; }
    fclose(f);
    int w = 0, h = 0, ct = -1, depth = 0, interlace = 0;
    u8 *idat = NULL;
    size_t idat_n = 0;
    for (long p = 8; p + 12 <= n;) {
        u32 len = get32(buf + p);
        if (p + 12 + (long)len > n) break;
        const u8 *type = buf + p + 4, *d = buf + p + 8;
        if (!memcmp(type, "IHDR", 4)) { w = get32(d); h = get32(d + 4); depth = d[8]; ct = d[9]; interlace = d[12]; }
        else if (!memcmp(type, "IDAT", 4)) { idat = realloc(idat, idat_n + len); memcpy(idat + idat_n, d, len); idat_n += len; }
        else if (!memcmp(type, "IEND", 4)) break;
        p += 12 + len;
    }
    free(buf);
    int bpp = ct == 6 ? 4 : ct == 2 ? 3 : 0;
    if (!idat || !bpp || depth != 8 || interlace || w <= 0 || h <= 0 || w > 16384 || h > 16384) {
        static int warned;
        if (!warned++) fprintf(stderr, "textures: %s: only 8-bit RGB/RGBA non-interlaced PNGs are supported\n", path);
        free(idat);
        return NULL;
    }
    size_t stride = (size_t)w * bpp, raw_n = (stride + 1) * h;
    u8 *raw = malloc(raw_n);
    uLongf got = raw_n;
    if (uncompress(raw, &got, idat, idat_n) != Z_OK || got != raw_n) { free(idat); free(raw); return NULL; }
    free(idat);
    u8 *px = malloc(stride * h), *out = malloc((size_t)w * h * 4);
    for (int y = 0; y < h; y++) {
        u8 ft = raw[y * (stride + 1)], *src = raw + y * (stride + 1) + 1, *cur = px + y * stride;
        const u8 *up = y ? px + (y - 1) * stride : NULL;
        for (size_t x = 0; x < stride; x++) {
            int a = x >= (size_t)bpp ? cur[x - bpp] : 0, b = up ? up[x] : 0, c = up && x >= (size_t)bpp ? up[x - bpp] : 0;
            u8 v = src[x];
            switch (ft) {
            case 1: v += a; break;
            case 2: v += b; break;
            case 3: v += (a + b) / 2; break;
            case 4: v += paeth(a, b, c); break;
            }
            cur[x] = v;
        }
    }
    free(raw);
    for (size_t i = 0; i < (size_t)w * h; i++) {
        memcpy(out + i * 4, px + i * bpp, 3);
        out[i * 4 + 3] = bpp == 4 ? px[i * 4 + 3] : 255;
    }
    free(px);
    *ow = w;
    *oh = h;
    return out;
}

/* ---------------------------------------------------------------- API */

static void name(char *out, size_t n, const char *dir, u64 hash, int w, int h, int fmt) {
    snprintf(out, n, "%s/tex1_%dx%d_%016llx_%d.png", dir, w, h, (unsigned long long)hash, fmt);
}

void texpack_dump(u64 hash, int w, int h, int fmt, const u8 *rgba) {
    init();
    if (!dump_on) return;
    char p[700];
    name(p, sizeof p, dump_dir, hash, w, h, fmt);
    struct stat st;
    if (!stat(p, &st)) return;
    if (!write_png(p, w, h, rgba)) fprintf(stderr, "textures: can't write %s (%s)\n", p, strerror(errno));
}

/* a replacement for this texture: malloc'd RGBA (caller frees) and its size, or NULL */
u8 *texpack_replace(u64 hash, int *w, int *h) {
    init();
    if (!pack_on) return NULL;
    u32 i = (u32)(hash ^ hash >> 32) & (pack_cap - 1);
    while (pack[i].path && pack[i].hash != hash) i = (i + 1) & (pack_cap - 1);
    if (!pack[i].path) return NULL;
    u8 *px = read_png(pack[i].path, w, h);
    if (!px) fprintf(stderr, "textures: can't load %s\n", pack[i].path);
    return px;
}
