/* The game disc, served from an extracted filesystem (dtk disc extract): disc offsets are mapped
 * to files through the partition's FST, exactly as the drive would see them. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "runtime.h"

typedef struct { u64 off, size; char *path; } Region;
static Region *regions;
static int nregions, cap;
static u8 *fst;
static u32 fst_size;

static void add(u64 off, u64 size, const char *path) {
    if (nregions == cap) {
        cap = cap ? cap * 2 : 1024;
        regions = realloc(regions, cap * sizeof *regions);
    }
    regions[nregions++] = (Region){off, size, strdup(path)};
}

static u8 *slurp(const char *path, u32 *size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    u8 *b = malloc(n);
    if (fread(b, 1, n, f) != (size_t)n) { fclose(f); free(b); return NULL; }
    fclose(f);
    *size = (u32)n;
    return b;
}

#define BE(p) ((u32)(p)[0] << 24 | (u32)(p)[1] << 16 | (u32)(p)[2] << 8 | (p)[3])

/* walk FST directory entries [first, end) under host directory `dir` */
static u32 walk(const char *root, const char *dir, u32 i, u32 end, const char *strtab) {
    while (i < end) {
        const u8 *e = fst + 12 * i;
        const char *name = strtab + (BE(e) & 0xFFFFFF);
        char path[1024];
        snprintf(path, sizeof path, "%s/%s", dir, name);
        if (e[0]) {
            i = walk(root, path, i + 1, BE(e + 8), strtab);
        } else {
            char host[1200];
            snprintf(host, sizeof host, "%s/files%s", root, path);
            add((u64)BE(e + 4) << 2, BE(e + 8), host);   /* Wii FST offsets are stored >> 2 */
            i++;
        }
    }
    return i;
}

int disc_init(const char *root) {
    char p[1024];
    u32 bsize;
    snprintf(p, sizeof p, "%s/sys/boot.bin", root);
    u8 *boot = slurp(p, &bsize);
    snprintf(p, sizeof p, "%s/sys/fst.bin", root);
    fst = slurp(p, &fst_size);
    if (!boot || !fst) {
        fprintf(stderr, "disc: %s is not an extracted disc (need sys/boot.bin, sys/fst.bin)\n", root);
        return 0;
    }
    u64 dol = (u64)BE(boot + 0x420) << 2, fsto = (u64)BE(boot + 0x424) << 2;
    snprintf(p, sizeof p, "%s/sys/boot.bin", root); add(0, 0x440, p);
    snprintf(p, sizeof p, "%s/sys/bi2.bin", root); add(0x440, 0x2000, p);
    snprintf(p, sizeof p, "%s/sys/apploader.img", root); add(0x2440, dol > 0x2440 ? dol - 0x2440 : 0, p);
    snprintf(p, sizeof p, "%s/sys/main.dol", root); add(dol, fsto - dol, p);
    snprintf(p, sizeof p, "%s/sys/fst.bin", root); add(fsto, fst_size, p);
    u32 n = BE(fst + 8);
    walk(root, "", 1, n, (const char *)fst + 12 * n);
    free(boot);
    fprintf(stderr, "disc: %d files\n", nregions);
    return 1;
}

/* place the FST at the top of MEM1 as the apploader does; returns its guest address */
u32 disc_load_fst(void) {
    u32 at = (0x81800000u - fst_size) & ~31u;
    memcpy(MEM + at, fst, fst_size);
    return at;
}
u32 disc_fst_size(void) { return fst_size; }

/* read `len` bytes at disc byte offset `off` into guest memory; returns bytes read */
u32 disc_read(u64 off, u32 dst, u32 len) {
    u32 done = 0;
    while (done < len) {
        u64 o = off + done;
        Region *r = NULL;
        for (int i = 0; i < nregions; i++)
            if (regions[i].off <= o && o < regions[i].off + regions[i].size) { r = &regions[i]; break; }
        if (!r) {
            fprintf(stderr, "disc: read outside any file at %llx\n", (unsigned long long)o);
            memset(MEM + dst + done, 0, len - done);
            return done;
        }
        u32 n = (u32)((r->off + r->size - o) < (len - done) ? (r->off + r->size - o) : (len - done));
        FILE *f = fopen(r->path, "rb");
        if (!f) { fprintf(stderr, "disc: cannot open %s\n", r->path); return done; }
        fseek(f, (long)(o - r->off), SEEK_SET);
        size_t got = fread(MEM + dst + done, 1, n, f);
        fclose(f);
        if (got < n) memset(MEM + dst + done + got, 0, n - got);
        done += n;
    }
    return done;
}
