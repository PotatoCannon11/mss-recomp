/* OpenGL renderer for the decoded GX command stream.
 *
 * The embedded framebuffer (EFB) is an FBO. Each primitive batch is drawn with a program generated
 * from the current GX state: the vertex stage does the XF transform (per-vertex position/normal/
 * texture matrices from XF memory), the two lighting channels and texture-coordinate generation;
 * the fragment stage evaluates the TEV stages, alpha test and fog. EFB copies become textures
 * (looked up by guest address) or, for the XFB, a presented frame. */
#define GL_GLEXT_PROTOTYPES
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#include <GL/glext.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include "runtime.h"
#include "gpu.h"

#define EFB_W 640
#define EFB_H 528
/* internal resolution: the EFB is rendered at RT_SCALE x the console's 640x528 (default 2 in a window,
 * 1 headless). Game-visible coordinates stay native; only GL viewports, scissors and copy targets scale. */
static int S = 1;
#define EW (EFB_W * S)
#define EH (EFB_H * S)

static GLuint efb_fbo, efb_color, efb_depth, vao, vbo, ibo, ubo;
static int ready;
static u64 draws, verts_total;

/* ---------------------------------------------------------------- headless context */

static int ctx_init(void) {
    PFNEGLGETPLATFORMDISPLAYEXTPROC gpd = (void *)eglGetProcAddress("eglGetPlatformDisplayEXT");
    EGLDisplay d = gpd ? gpd(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL) : EGL_NO_DISPLAY;
    EGLint maj, min;
    if (!eglInitialize(d, &maj, &min))
        return 0;
    eglBindAPI(EGL_OPENGL_API);
    EGLint attr[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 3,
                     EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT, EGL_NONE};
    EGLContext ctx = eglCreateContext(d, NULL, EGL_NO_CONTEXT, attr);
    return ctx && eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx);
}

/* ---------------------------------------------------------------- vertices */

/* GLVertex (gpu.h): attribute locations 0 pos, 1 nrm, 2-3 colours, 4-7 texcoord pairs, 8 pnmtx, 9-10 texmtx */


/* ---------------------------------------------------------------- shader generation */

typedef struct {
    u32 genmode, tev_c[16], tev_a[16], tref[8], ksel[8], alpha, colctl[4], texgen[8], postgen[8], dualtex, fog;
} PipeKey;

typedef struct { PipeKey k; GLuint prog; } Prog;
static Prog progs[4096];
static int nprogs;

static void append(char **s, size_t *n, size_t *cap, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
static void append(char **s, size_t *n, size_t *cap, const char *fmt, ...) {
    va_list ap;
    for (;;) {
        va_start(ap, fmt);
        int w = vsnprintf(*s + *n, *cap - *n, fmt, ap);
        va_end(ap);
        if (w >= 0 && (size_t)w < *cap - *n) { *n += w; return; }
        *cap *= 2;
        *s = realloc(*s, *cap);
    }
}
#define A(...) append(&src, &n, &cap, __VA_ARGS__)

#define UBO_DECL \
    "layout(std140) uniform XF { vec4 pmtx[64]; vec4 nmtx[32]; vec4 ptmtx[64]; vec4 lpos[8]; vec4 ldir[8];" \
    " vec4 lcos[8]; vec4 ldist[8]; vec4 lcol[8]; vec4 matc[2]; vec4 ambc[2]; vec4 proj[2]; vec4 vp[2]; vec4 kc[4];" \
    " vec4 tevreg[4]; vec4 fogp; vec4 fogc; vec4 alpharef; };\n"

static void gen_vs(char **out, const PipeKey *k) {
    size_t n = 0, cap = 8192;
    char *src = malloc(cap);
    src[0] = 0;
    A("#version 330 core\n" UBO_DECL);
    A("layout(location=0) in vec3 a_pos; layout(location=1) in vec3 a_nrm; layout(location=2) in vec4 a_col0;"
      " layout(location=3) in vec4 a_col1; layout(location=4) in vec4 a_tex[4]; layout(location=8) in float a_pnmtx;"
      " layout(location=9) in vec4 a_tm[2];\n"
      "out vec4 v_col0; out vec4 v_col1; out vec3 v_tex[8]; out float v_z;\n");
    A("void main() {\n int pm = int(a_pnmtx + 0.5);\n vec4 p = vec4(a_pos, 1.0);\n");
    A(" vec3 ep = vec3(dot(pmtx[pm], p), dot(pmtx[pm+1], p), dot(pmtx[pm+2], p));\n");
    A(" int nm = pm %% 32;\n vec3 en = vec3(dot(nmtx[nm].xyz, a_nrm), dot(nmtx[nm+1].xyz, a_nrm), dot(nmtx[nm+2].xyz, a_nrm));\n"
      " if (dot(en, en) > 0.0) en = normalize(en);\n");
    A(" vec4 c;\n if (proj[1].z < 0.5) c = vec4(proj[0].x*ep.x + proj[0].y*ep.z, proj[0].z*ep.y + proj[0].w*ep.z, proj[1].x*ep.z + proj[1].y, -ep.z);\n"
      " else c = vec4(proj[0].x*ep.x + proj[0].y, proj[0].z*ep.y + proj[0].w, proj[1].x*ep.z + proj[1].y, 1.0);\n");
    /* GX viewport in EFB pixels (y down), then GL clip space; depth as XF computes it */
    A(" float iw = 1.0 / c.w;\n float wx = c.x * iw * vp[0].x + vp[1].x - 342.0, wy = c.y * iw * vp[0].y + vp[1].y - 342.0;\n"
      " float wz = (c.z * iw * vp[0].z + vp[1].z) / 16777215.0;\n"
      " gl_Position = vec4((wx * (2.0 / %d.0) - 1.0) * c.w, (1.0 - wy * (2.0 / %d.0)) * c.w, (clamp(wz, 0.0, 1.0) * 2.0 - 1.0) * c.w, c.w);\n"
      " v_z = wz;\n", EFB_W, EFB_H);
    for (int ch = 0; ch < 2; ch++) {
        for (int part = 0; part < 2; part++) {
            u32 ctl = k->colctl[part * 2 + ch];
            const char *sw = part ? "a" : "rgb";
            const char *vcol = ch ? "a_col1" : "a_col0";
            char mat[32], amb[32];
            snprintf(mat, sizeof mat, "%s", (ctl & 1) ? vcol : (ch ? "matc[1]" : "matc[0]"));
            snprintf(amb, sizeof amb, "%s", (ctl & 0x40) ? vcol : (ch ? "ambc[1]" : "ambc[0]"));
            if (!(ctl & 2)) {
                A(" v_col%d.%s = %s.%s;\n", ch, sw, mat, sw);
                continue;
            }
            A(" { vec4 acc = %s;\n", amb);
            u32 mask = ((ctl >> 2) & 15) | (((ctl >> 11) & 15) << 4);
            int diff = (ctl >> 7) & 3, atten = (ctl >> 9) & 1, spot = (ctl >> 10) & 1;
            for (int l = 0; l < 8; l++) {
                if (!(mask & (1 << l))) continue;
                A("  { vec3 ld = lpos[%d].xyz - ep; float dist = length(ld); vec3 L = ld / max(dist, 1e-9);\n", l);
                A("    float df = %s;\n", diff == 0 ? "1.0" : diff == 1 ? "dot(L, en)" : "max(dot(L, en), 0.0)");
                if (atten && spot)
                    A("    float cs = max(dot(L, ldir[%d].xyz), 0.0); float at = max(lcos[%d].x + lcos[%d].y*cs + lcos[%d].z*cs*cs, 0.0)"
                      " / max(ldist[%d].x + ldist[%d].y*dist + ldist[%d].z*dist*dist, 1e-9);\n", l, l, l, l, l, l, l);
                else if (atten)
                    A("    float cs = max(dot(en, ldir[%d].xyz), 0.0); float at = max(lcos[%d].x + lcos[%d].y*cs + lcos[%d].z*cs*cs, 0.0)"
                      " / max(ldist[%d].x + ldist[%d].y*cs + ldist[%d].z*cs*cs, 1e-9);\n", l, l, l, l, l, l, l);
                else
                    A("    float at = 1.0;\n");
                A("    acc += lcol[%d] * df * at; }\n", l);
            }
            A("  v_col%d.%s = (%s * clamp(acc, 0.0, 1.0)).%s; }\n", ch, sw, mat, sw);
        }
    }
    int ntg = k->genmode & 15;
    for (int t = 0; t < 8; t++) A(" v_tex[%d] = vec3(0.0, 0.0, 1.0);\n", t);
    for (int t = 0; t < ntg; t++) {
        u32 g = k->texgen[t];
        int proj3 = (g >> 1) & 1, inrow = (g >> 7) & 31, type = (g >> 4) & 7, input4 = (g >> 2) & 1;
        A(" { vec4 src = ");
        if (inrow == 0) A("vec4(a_pos, 1.0)");
        else if (inrow == 1) A("vec4(a_nrm, 1.0)");
        else if (inrow >= 5 && inrow <= 12) A("vec4(a_tex[%d].%s, 1.0, 1.0)", (inrow - 5) >> 1, ((inrow - 5) & 1) ? "zw" : "xy");
        else A("vec4(0.0, 0.0, 1.0, 1.0)");
        A(";\n");
        if (!input4 && inrow >= 5) A("  src.z = 1.0;\n");
        if (type == 0) {
            A("  int tm = int(a_tm[%d][%d] + 0.5);\n", t >> 2, t & 3);
            if (proj3) A("  vec3 tc = vec3(dot(pmtx[tm], src), dot(pmtx[tm+1], src), dot(pmtx[tm+2], src));\n");
            else A("  vec3 tc = vec3(dot(pmtx[tm], src), dot(pmtx[tm+1], src), 1.0);\n");
        } else if (type == 1) {
            A("  vec3 tc = vec3(src.xy, 1.0);\n");   /* emboss */
        } else {
            A("  vec3 tc = vec3(%s, 1.0);\n", type == 2 ? "v_col0.rg" : "v_col1.rg");
        }
        if (k->dualtex) {
            u32 pg = k->postgen[t];
            int pm = pg & 63;
            if ((pg >> 8) & 1) A("  tc = normalize(tc);\n");
            A("  { vec4 q = vec4(tc, 1.0); tc = vec3(dot(ptmtx[%d], q), dot(ptmtx[%d], q), dot(ptmtx[%d], q)); }\n", pm, pm + 1, pm + 2);
        }
        A("  v_tex[%d] = tc; }\n", t);
    }
    A("}\n");
    *out = src;
}

static void gen_fs(char **out, const PipeKey *k) {
    size_t n = 0, cap = 16384;
    char *src = malloc(cap);
    src[0] = 0;
    A("#version 330 core\n" UBO_DECL);
    A("uniform sampler2D tex[8];\n"
      "in vec4 v_col0; in vec4 v_col1; in vec3 v_tex[8]; in float v_z;\nout vec4 o_col;\n"
      "vec3 lrp(vec3 a, vec3 b, vec3 c) { vec3 cc = floor(c * 255.0 + 0.5); cc += floor(cc / 128.0); return a + (b - a) * (cc / 256.0); }\n"
      "float lrp(float a, float b, float c) { float cc = floor(c * 255.0 + 0.5); cc += floor(cc / 128.0); return a + (b - a) * (cc / 256.0); }\n"
      "float q8(float x) { return floor(clamp(x, 0.0, 1.0) * 255.0 + 0.5); }\n"
      "vec2 pj(vec3 t) { float w = abs(t.z) > 1e-9 ? t.z : 1.0; return t.xy / w; }\n");
    A("void main() {\n vec4 prev = tevreg[0], r0 = tevreg[1], r1 = tevreg[2], r2 = tevreg[3];\n");
    int nstages = ((k->genmode >> 10) & 15) + 1;
    static const char *frac[8] = {"1.0", "0.875", "0.75", "0.625", "0.5", "0.375", "0.25", "0.125"};
    static const char comp[4] = {'r', 'g', 'b', 'a'};
    for (int s = 0; s < nstages; s++) {
        u32 cenv = k->tev_c[s], aenv = k->tev_a[s];
        u32 tref = k->tref[s >> 1] >> ((s & 1) * 12);
        int map = tref & 7, coord = (tref >> 3) & 7, en = (tref >> 6) & 1, chan = (tref >> 7) & 7;
        u32 ks = k->ksel[s >> 1] >> ((s & 1) * 10);
        int kcs = (ks >> 4) & 31, kas = (ks >> 9) & 31;
        int rsw = aenv & 3, tsw = (aenv >> 2) & 3;
        char rswz[5], tswz[5];
        u32 t0 = k->ksel[rsw * 2], t1 = k->ksel[rsw * 2 + 1];
        rswz[0] = comp[t0 & 3]; rswz[1] = comp[(t0 >> 2) & 3]; rswz[2] = comp[t1 & 3]; rswz[3] = comp[(t1 >> 2) & 3]; rswz[4] = 0;
        t0 = k->ksel[tsw * 2]; t1 = k->ksel[tsw * 2 + 1];
        tswz[0] = comp[t0 & 3]; tswz[1] = comp[(t0 >> 2) & 3]; tswz[2] = comp[t1 & 3]; tswz[3] = comp[(t1 >> 2) & 3]; tswz[4] = 0;
        A(" {\n");
        if (en) A("  vec4 tx = texture(tex[%d], pj(v_tex[%d])).%s;\n", map, coord, tswz);
        else A("  vec4 tx = vec4(1.0);\n");
        A("  vec4 ras = clamp(%s, 0.0, 1.0).%s;\n", chan == 0 ? "v_col0" : chan == 1 ? "v_col1" : "vec4(0.0)", rswz);
        if (kcs < 8) A("  vec3 kcol = vec3(%s);\n", frac[kcs]);
        else if (kcs >= 12 && kcs < 16) A("  vec3 kcol = kc[%d].rgb;\n", kcs - 12);
        else if (kcs >= 16) A("  vec3 kcol = vec3(kc[%d].%c);\n", (kcs - 16) & 3, comp[(kcs - 16) >> 2]);
        else A("  vec3 kcol = vec3(1.0);\n");
        if (kas < 8) A("  float kalp = %s;\n", frac[kas]);
        else if (kas >= 16) A("  float kalp = kc[%d].%c;\n", (kas - 16) & 3, comp[(kas - 16) >> 2]);
        else A("  float kalp = 1.0;\n");
        static const char *cin[16] = {"prev.rgb", "vec3(prev.a)", "r0.rgb", "vec3(r0.a)", "r1.rgb", "vec3(r1.a)", "r2.rgb", "vec3(r2.a)",
                                      "tx.rgb", "vec3(tx.a)", "ras.rgb", "vec3(ras.a)", "vec3(1.0)", "vec3(0.5)", "kcol", "vec3(0.0)"};
        static const char *ain[8] = {"prev.a", "r0.a", "r1.a", "r2.a", "tx.a", "ras.a", "kalp", "0.0"};
        static const char *dst[4] = {"prev", "r0", "r1", "r2"};
        static const char *scl[4] = {"1.0", "2.0", "4.0", "0.5"};
        static const char *bs[3] = {"", " + 0.5", " - 0.5"};
        /* both halves read the registers as they were before this stage */
        int cd_ = cenv & 15, cc_ = (cenv >> 4) & 15, cb_ = (cenv >> 8) & 15, ca_ = (cenv >> 12) & 15;
        int ad_ = (aenv >> 4) & 7, ac_ = (aenv >> 7) & 7, ab_ = (aenv >> 10) & 7, aa_ = (aenv >> 13) & 7;
        A("  vec3 ca = clamp(%s, 0.0, 1.0), cb = clamp(%s, 0.0, 1.0), cc = clamp(%s, 0.0, 1.0), cd = %s;\n", cin[ca_], cin[cb_], cin[cc_], cin[cd_]);
        A("  float aa = clamp(%s, 0.0, 1.0), ab = clamp(%s, 0.0, 1.0), ac = clamp(%s, 0.0, 1.0), ad = %s;\n", ain[aa_], ain[ab_], ain[ac_], ain[ad_]);
        {
            int bias = (cenv >> 16) & 3, sub = (cenv >> 18) & 1, clampv = (cenv >> 19) & 1, scale = (cenv >> 20) & 3, de = (cenv >> 22) & 3;
            if (bias == 3) {
                static const char *cmp[8] = {
                    "(q8(ca.r) > q8(cb.r) ? cc : vec3(0.0))", "(q8(ca.r) == q8(cb.r) ? cc : vec3(0.0))",
                    "((q8(ca.g)*256.0+q8(ca.r)) > (q8(cb.g)*256.0+q8(cb.r)) ? cc : vec3(0.0))",
                    "((q8(ca.g)*256.0+q8(ca.r)) == (q8(cb.g)*256.0+q8(cb.r)) ? cc : vec3(0.0))",
                    "((q8(ca.b)*65536.0+q8(ca.g)*256.0+q8(ca.r)) > (q8(cb.b)*65536.0+q8(cb.g)*256.0+q8(cb.r)) ? cc : vec3(0.0))",
                    "((q8(ca.b)*65536.0+q8(ca.g)*256.0+q8(ca.r)) == (q8(cb.b)*65536.0+q8(cb.g)*256.0+q8(cb.r)) ? cc : vec3(0.0))",
                    "vec3(q8(ca.r) > q8(cb.r) ? cc.r : 0.0, q8(ca.g) > q8(cb.g) ? cc.g : 0.0, q8(ca.b) > q8(cb.b) ? cc.b : 0.0)",
                    "vec3(q8(ca.r) == q8(cb.r) ? cc.r : 0.0, q8(ca.g) == q8(cb.g) ? cc.g : 0.0, q8(ca.b) == q8(cb.b) ? cc.b : 0.0)"};
                A("  vec3 cres = cd + %s;\n", cmp[(scale << 1) | sub]);
            } else
                A("  vec3 cres = (cd %s lrp(ca, cb, cc)%s) * %s;\n", sub ? "-" : "+", bs[bias], scl[scale]);
            A("  vec3 cout = %s;\n", clampv ? "clamp(cres, 0.0, 1.0)" : "clamp(cres, -4.0, 4.0)");
            int bias_a = (aenv >> 16) & 3, sub_a = (aenv >> 18) & 1, clamp_a = (aenv >> 19) & 1, scale_a = (aenv >> 20) & 3, de_a = (aenv >> 22) & 3;
            if (bias_a == 3) {
                int op = (scale_a << 1) | sub_a;
                static const char *cmpa[8] = {
                    "(q8(ca.r) > q8(cb.r) ? ac : 0.0)", "(q8(ca.r) == q8(cb.r) ? ac : 0.0)",
                    "((q8(ca.g)*256.0+q8(ca.r)) > (q8(cb.g)*256.0+q8(cb.r)) ? ac : 0.0)",
                    "((q8(ca.g)*256.0+q8(ca.r)) == (q8(cb.g)*256.0+q8(cb.r)) ? ac : 0.0)",
                    "((q8(ca.b)*65536.0+q8(ca.g)*256.0+q8(ca.r)) > (q8(cb.b)*65536.0+q8(cb.g)*256.0+q8(cb.r)) ? ac : 0.0)",
                    "((q8(ca.b)*65536.0+q8(ca.g)*256.0+q8(ca.r)) == (q8(cb.b)*65536.0+q8(cb.g)*256.0+q8(cb.r)) ? ac : 0.0)",
                    "(q8(aa) > q8(ab) ? ac : 0.0)", "(q8(aa) == q8(ab) ? ac : 0.0)"};
                A("  float ares = ad + %s;\n", cmpa[op]);
            } else
                A("  float ares = (ad %s lrp(aa, ab, ac)%s) * %s;\n", sub_a ? "-" : "+", bs[bias_a], scl[scale_a]);
            A("  float aout = %s;\n", clamp_a ? "clamp(ares, 0.0, 1.0)" : "clamp(ares, -4.0, 4.0)");
            A("  %s.rgb = cout; %s.a = aout;\n", dst[de], dst[de_a]);
        }
        A(" }\n");
    }
    A(" vec4 res = clamp(prev, 0.0, 1.0);\n");
    {
        const char *dbg = ENV("RT_DEBUG_SHADE");
        if (dbg && !strcmp(dbg, "white")) A(" res = vec4(1.0);\n");
        else if (dbg && !strcmp(dbg, "col")) A(" res = vec4(v_col0.rgb, 1.0);\n");
        else if (dbg && !strcmp(dbg, "alpha")) A(" res = vec4(res.aaa, 1.0);\n");
        else if (dbg && !strcmp(dbg, "cola")) A(" res = vec4(v_col0.aaa, 1.0);\n");
        else if (dbg && !strcmp(dbg, "tex")) A(" res = vec4(texture(tex[%d], pj(v_tex[%d])).rgb, 1.0);\n", k->tref[0] & 7, (k->tref[0] >> 3) & 7);
    }
    {
        u32 a = k->alpha;
        int c0 = (a >> 16) & 7, c1 = (a >> 19) & 7, logic = (a >> 22) & 3;
        static const char *op[8] = {"false", "<", "==", "<=", ">", "!=", ">=", "true"};
        static const char *lg[4] = {"&&", "||", "!=", "=="};
        char t0[64], t1[64];
        if (c0 == 0 || c0 == 7) snprintf(t0, sizeof t0, "%s", op[c0]); else snprintf(t0, sizeof t0, "(q8(res.a) %s alpharef.x)", op[c0]);
        if (c1 == 0 || c1 == 7) snprintf(t1, sizeof t1, "%s", op[c1]); else snprintf(t1, sizeof t1, "(q8(res.a) %s alpharef.y)", op[c1]);
        if (!(c0 == 7 && c1 == 7) && !ENV("RT_NOALPHATEST"))
            A(" if (!((%s) %s (%s))) discard;\n", t0, lg[logic], t1);
    }
    {
        int ftype = (k->fog >> 21) & 7;
        if (ftype >= 2) {
            A(" { float ze = fogp.x / (fogp.y - v_z * fogp.z); float f = clamp(ze - fogp.w, 0.0, 1.0);\n");
            if (ftype == 4) A("  f = 1.0 - exp2(-8.0 * f);\n");
            else if (ftype == 5) A("  f = 1.0 - exp2(-8.0 * f * f);\n");
            else if (ftype == 6) A("  f = exp2(-8.0 * (1.0 - f));\n");
            else if (ftype == 7) A("  f = 1.0 - f; f = exp2(-8.0 * f * f);\n");
            A("  res.rgb = mix(res.rgb, fogc.rgb, f); }\n");
        }
    }
    A(" o_col = res;\n}\n");
    *out = src;
}

static GLuint compile(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[4096];
        glGetShaderInfoLog(s, sizeof log, NULL, log);
        fprintf(stderr, "gfx: shader error:\n%s\n%s\n", log, src);
        exit(1);
    }
    return s;
}

#define PHASH 8192
static int prog_slot[PHASH];   /* program index + 1, open addressing */
static u32 key_hash(const PipeKey *k) {
    const u32 *w = (const u32 *)k;
    u32 h = 2166136261u;
    for (size_t i = 0; i < sizeof *k / 4; i++) h = (h ^ w[i]) * 16777619u;
    return h;
}
static GLuint get_prog(const PipeKey *k) {
    static int last = -1;
    if (last >= 0 && !memcmp(&progs[last].k, k, sizeof *k))
        return progs[last].prog;
    u32 hh = key_hash(k);
    for (u32 j = 0; j < PHASH; j++) {
        int sl = prog_slot[(hh + j) & (PHASH - 1)];
        if (!sl) break;
        if (!memcmp(&progs[sl - 1].k, k, sizeof *k)) { last = sl - 1; return progs[sl - 1].prog; }
    }
    char *vs, *fs;
    gen_vs(&vs, k);
    gen_fs(&fs, k);
    GLuint p = glCreateProgram();
    GLuint v = compile(GL_VERTEX_SHADER, vs), f = compile(GL_FRAGMENT_SHADER, fs);
    glAttachShader(p, v);
    glAttachShader(p, f);
    glLinkProgram(p);
    glDeleteShader(v);
    glDeleteShader(f);
    GLint ok;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[4096];
        glGetProgramInfoLog(p, sizeof log, NULL, log);
        fprintf(stderr, "gfx: link error: %s\n", log);
        exit(1);
    }
    if (ENV("RT_SHADERS"))
        fprintf(stderr, "---- program %d\n%s\n%s\n", nprogs, vs, fs);
    free(vs);
    free(fs);
    glUniformBlockBinding(p, glGetUniformBlockIndex(p, "XF"), 0);
    glUseProgram(p);
    for (int i = 0; i < 8; i++) {
        char nm[16];
        snprintf(nm, sizeof nm, "tex[%d]", i);
        glUniform1i(glGetUniformLocation(p, nm), i);
    }
    if (nprogs == (int)(sizeof progs / sizeof *progs)) { nprogs = 0; memset(prog_slot, 0, sizeof prog_slot); }
    progs[nprogs].k = *k;
    progs[nprogs].prog = p;
    for (u32 j = 0; j < PHASH; j++)
        if (!prog_slot[(hh + j) & (PHASH - 1)]) { prog_slot[(hh + j) & (PHASH - 1)] = nprogs + 1; break; }
    last = nprogs++;
    return p;
}

/* ---------------------------------------------------------------- textures */

static u8 tmem_tlut[1 << 20];   /* TMEM: palettes are loaded here (BP 0x64/0x65), then indexed */
static u64 tlut_gen;            /* bumped whenever palette contents change, so cached lookups recheck */

typedef struct { u32 addr, fmt, w, h, tlut, hash; GLuint tex; int efb; u32 mode0; int next; } TexEntry;
static TexEntry texcache[4096];
static u64 stat_tex_new, stat_tex_bytes, stat_verts;   /* RT_FPS counters */
static int ntex;
/* index by guest address (chains of texcache indices + 1): lookups stay O(1) however full the cache is */
#define TEXH 8192
static int tex_head[TEXH];
static u32 tex_slot(u32 addr) { return (addr * 2654435761u) >> 19 & (TEXH - 1); }
static TexEntry *tex_add(u32 addr) {
    if (ntex == (int)(sizeof texcache / sizeof *texcache)) {   /* full: start over */
        for (int k = 0; k < ntex; k++) glDeleteTextures(1, &texcache[k].tex);
        ntex = 0;
        memset(tex_head, 0, sizeof tex_head);
        g_gpu.efb_gen++;   /* drops this frame's fast-path entries, which point into the old slots */
    }
    TexEntry *e = &texcache[ntex];
    memset(e, 0, sizeof *e);
    e->addr = addr;
    e->next = tex_head[tex_slot(addr)];
    tex_head[tex_slot(addr)] = ++ntex;
    return e;
}
static TexEntry *tex_find_efb(u32 addr) {
    for (int i = tex_head[tex_slot(addr)]; i; i = texcache[i - 1].next)
        if (texcache[i - 1].efb && texcache[i - 1].addr == addr) return &texcache[i - 1];
    return NULL;
}

/* change detection for cached textures: every byte of small textures, ~1024 spread samples of big ones */
static u32 tex_hash(const u8 *p, u32 n) {
    u32 h = 2166136261u, step = n > 2048 ? n / 1024 : 1;
    for (u32 i = 0; i < n; i += step)
        h = (h ^ p[i]) * 16777619u;
    return h ^ n ^ p[n - 1];
}

static void rgb5a3(u16 v, u8 *o) {
    if (v & 0x8000) { o[0] = ((v >> 10) & 31) * 255 / 31; o[1] = ((v >> 5) & 31) * 255 / 31; o[2] = (v & 31) * 255 / 31; o[3] = 255; }
    else { o[0] = ((v >> 8) & 15) * 17; o[1] = ((v >> 4) & 15) * 17; o[2] = (v & 15) * 17; o[3] = ((v >> 12) & 7) * 255 / 7; }
}
static void rgb565(u16 v, u8 *o) { o[0] = ((v >> 11) & 31) * 255 / 31; o[1] = ((v >> 5) & 63) * 255 / 63; o[2] = (v & 31) * 255 / 31; o[3] = 255; }
static void palette(u16 v, int tfmt, u8 *o) {
    if (tfmt == 0) { o[0] = o[1] = o[2] = v & 255; o[3] = v >> 8; }
    else if (tfmt == 1) rgb565(v, o);
    else rgb5a3(v, o);
}

static void blk(int fmt, int *bw, int *bh, int *bb) {
    switch (fmt) {
    case 0: case 8: case 14: *bw = 8; *bh = 8; *bb = 32; break;
    case 1: case 2: case 9: *bw = 8; *bh = 4; *bb = 32; break;
    case 6: *bw = 4; *bh = 4; *bb = 64; break;
    case 3: case 4: case 5: case 10: *bw = 4; *bh = 4; *bb = 32; break;
    default: *bw = *bh = *bb = 0;
    }
}
static int tex_bytes(int fmt, int w, int h) {
    int bw, bh, bb;
    blk(fmt, &bw, &bh, &bb);
    return bw ? ((w + bw - 1) / bw) * ((h + bh - 1) / bh) * bb : 0;
}

static void decode_tex(const u8 *p, int fmt, int w, int h, u32 tlut_off, int tlut_fmt, u8 *out) {
    int bw, bh, bb;
    blk(fmt, &bw, &bh, &bb);
    const u8 *pal = tmem_tlut + (tlut_off & 0xFFFFF);
    for (int by = 0; by < h; by += bh)
        for (int bx = 0; bx < w; bx += bw) {
            if (fmt == 6) {
                for (int i = 0; i < 16; i++) {
                    int x = bx + (i & 3), y = by + (i >> 2);
                    if (x < w && y < h) {
                        u8 *o = out + 4 * (y * w + x);
                        o[3] = p[2 * i]; o[0] = p[2 * i + 1]; o[1] = p[32 + 2 * i]; o[2] = p[32 + 2 * i + 1];
                    }
                }
                p += 64;
                continue;
            }
            if (fmt == 14) {
                for (int sb = 0; sb < 4; sb++, p += 8) {
                    u16 c0 = p[0] << 8 | p[1], c1 = p[2] << 8 | p[3];
                    u8 pc[4][4];
                    rgb565(c0, pc[0]);
                    rgb565(c1, pc[1]);
                    for (int q = 0; q < 3; q++) {
                        if (c0 > c1) { pc[2][q] = (2 * pc[0][q] + pc[1][q]) / 3; pc[3][q] = (pc[0][q] + 2 * pc[1][q]) / 3; }
                        else { pc[2][q] = (pc[0][q] + pc[1][q]) / 2; pc[3][q] = pc[2][q]; }
                    }
                    pc[2][3] = 255;
                    pc[3][3] = c0 > c1 ? 255 : 0;
                    for (int y = 0; y < 4; y++)
                        for (int x = 0; x < 4; x++) {
                            int sel = (p[4 + y] >> (6 - 2 * x)) & 3;
                            int X = bx + (sb & 1) * 4 + x, Y = by + (sb >> 1) * 4 + y;
                            if (X < w && Y < h) memcpy(out + 4 * (Y * w + X), pc[sel], 4);
                        }
                }
                continue;
            }
            for (int y = 0; y < bh; y++)
                for (int x = 0; x < bw; x++) {
                    int X = bx + x, Y = by + y;
                    u8 px[4];
                    int i = y * bw + x;
                    switch (fmt) {
                    case 0: { u8 v = (p[i / 2] >> ((i & 1) ? 0 : 4)) & 15; px[0] = px[1] = px[2] = px[3] = v * 17; break; }
                    case 1: px[0] = px[1] = px[2] = px[3] = p[i]; break;
                    case 2: { u8 v = p[i]; px[0] = px[1] = px[2] = (v & 15) * 17; px[3] = (v >> 4) * 17; break; }
                    case 3: px[0] = px[1] = px[2] = p[2 * i + 1]; px[3] = p[2 * i]; break;
                    case 4: rgb565(p[2 * i] << 8 | p[2 * i + 1], px); break;
                    case 5: rgb5a3(p[2 * i] << 8 | p[2 * i + 1], px); break;
                    case 8: { u8 v = (p[i / 2] >> ((i & 1) ? 0 : 4)) & 15; palette(pal[2 * v] << 8 | pal[2 * v + 1], tlut_fmt, px); break; }
                    case 9: { u8 v = p[i]; palette(pal[2 * v] << 8 | pal[2 * v + 1], tlut_fmt, px); break; }
                    case 10: { u16 v = (p[2 * i] << 8 | p[2 * i + 1]) & 0x3FFF; palette(pal[2 * v] << 8 | pal[2 * v + 1], tlut_fmt, px); break; }
                    default: px[0] = 255; px[1] = 0; px[2] = 255; px[3] = 255;
                    }
                    if (X < w && Y < h) memcpy(out + 4 * (Y * w + X), px, 4);
                }
            p += bb;
        }
}

static GLuint tex_bound[8];   /* texture currently bound per unit (cleared when others bind) */
static void invalidate_gl_caches(void) { memset(tex_bound, 0, sizeof tex_bound); g_gpu.state_epoch++; }

static void bind_texture(int map) {
    int bank = map < 4 ? 0 : 0x20, i = map & 3;
    u32 mode0 = g_gpu.bp[0x80 + bank + i], img0 = g_gpu.bp[0x88 + bank + i], img3 = g_gpu.bp[0x94 + bank + i];
    u32 tlutr = g_gpu.bp[0x98 + bank + i];
    int w = (img0 & 0x3FF) + 1, h = ((img0 >> 10) & 0x3FF) + 1, fmt = (img0 >> 20) & 15;
    u32 addr = (img3 & 0xFFFFFF) << 5;
    u32 tlut_off = (tlutr & 0x3FF) << 9, tlut_fmt = (tlutr >> 10) & 3;
    GLuint *bound = tex_bound;
    /* fast path: the same texture registers already validated this frame (and no EFB copy since) */
    struct Fast { u32 img0, img3, tlut, mode0; u64 frame, gen, tgen; TexEntry *e; };
    static struct Fast fast[1024];
    u32 fh = ((img3 * 2654435761u) ^ img0 ^ (tlutr << 7)) & 1023;
    struct Fast *fe = &fast[fh];
    TexEntry *e = NULL;
    if (fe->e && fe->img0 == img0 && fe->img3 == img3 && fe->tlut == tlutr && fe->frame == g_gpu.frames && fe->gen == g_gpu.efb_gen && fe->tgen == tlut_gen)
        e = fe->e;
    if (e) {
        if (bound[map] != e->tex) { glActiveTexture(GL_TEXTURE0 + map); glBindTexture(GL_TEXTURE_2D, e->tex); bound[map] = e->tex; }
        if (e->mode0 != mode0 + 1) goto params;
        return;
    }
    glActiveTexture(GL_TEXTURE0 + map);
    int bytes = tex_bytes(fmt, w, h);
    e = tex_find_efb(addr);
    if (!e) {
        if (!bytes || (addr & 0x3FFFFFFFu) + bytes > 0x14000000u) { glBindTexture(GL_TEXTURE_2D, 0); return; }
        u32 hsh = tex_hash(MEM + addr, bytes) ^ (fmt >= 8 && fmt <= 10 ? tex_hash(tmem_tlut + tlut_off, fmt == 8 ? 32 : 512) * 31 : 0);
        for (int i = tex_head[tex_slot(addr)]; i && !e; i = texcache[i - 1].next) {
            TexEntry *t = &texcache[i - 1];
            if (!t->efb && t->addr == addr && t->w == (u32)w && t->h == (u32)h && t->fmt == (u32)fmt && t->hash == hsh) e = t;
        }
        if (!e) {
            e = tex_add(addr);
            e->fmt = (u32)fmt; e->w = (u32)w; e->h = (u32)h; e->tlut = tlut_off; e->hash = hsh;
            if (ENV("RT_SHADERS"))
                fprintf(stderr, "texture %08X fmt %d %dx%d tlut %05X/%d\n", addr, fmt, w, h, tlut_off, tlut_fmt);
            u8 *rgba = calloc(4, (size_t)w * h);
            stat_tex_new++;
            stat_tex_bytes += (u64)w * h * 4;
            decode_tex(MEM + addr, fmt, w, h, tlut_off, tlut_fmt, rgba);
            const char *td = ENV("RT_TEXDUMP");
            if (td) {
                char path[512];
                snprintf(path, sizeof path, "%s/tex_%08X_f%d_%dx%d.ppm", td, addr, fmt, w, h);
                FILE *tf = fopen(path, "wb");
                if (tf) {
                    fprintf(tf, "P6\n%d %d\n255\n", w, h);
                    for (int i = 0; i < w * h; i++) fwrite(rgba + 4 * i, 1, 3, tf);
                    fclose(tf);
                }
            }
            glGenTextures(1, &e->tex);
            glBindTexture(GL_TEXTURE_2D, e->tex);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
            free(rgba);
        }
    }
    glBindTexture(GL_TEXTURE_2D, e->tex);
    bound[map] = e->tex;
    *fe = (struct Fast){img0, img3, tlutr, mode0, g_gpu.frames, g_gpu.efb_gen, tlut_gen, e};
    if (e->mode0 == mode0 + 1) return;
params:
    glActiveTexture(GL_TEXTURE0 + map);
    e->mode0 = mode0 + 1;
    static const GLint wrap[4] = {GL_CLAMP_TO_EDGE, GL_REPEAT, GL_MIRRORED_REPEAT, GL_REPEAT};
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap[mode0 & 3]);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap[(mode0 >> 2) & 3]);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, (mode0 >> 4) & 1 ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, ((mode0 >> 5) & 7) == 0 ? GL_NEAREST : GL_LINEAR);
}

void gfx_load_tlut(void) {
    gfx_batch_flush();
    u32 src = (g_gpu.bp[0x64] & 0xFFFFFF) << 5;
    u32 r = g_gpu.bp[0x65];
    u32 tmem = (r & 0x3FF) << 9, count = ((r >> 10) & 0x3FF) * 16 * 2;
    if (tmem + count <= sizeof tmem_tlut && src + count < 0x14000000u && memcmp(tmem_tlut + tmem, MEM + src, count)) {
        memcpy(tmem_tlut + tmem, MEM + src, count);
        tlut_gen++;
    }
}

/* ---------------------------------------------------------------- uniforms */

typedef struct {
    float pmtx[64][4], nmtx[32][4], ptmtx[64][4], lpos[8][4], ldir[8][4], lcos[8][4], ldist[8][4], lcol[8][4];
    float matc[2][4], ambc[2][4], proj[2][4], vp[2][4], kc[4][4], tevreg[4][4], fogp[4], fogc[4], alpharef[4];
} Uniforms;

static float xf_f(u32 a) { return u2f(g_gpu.xfmem[a]); }
static void rgba(u32 v, float o[4]) { o[0] = (v >> 24) / 255.f; o[1] = ((v >> 16) & 255) / 255.f; o[2] = ((v >> 8) & 255) / 255.f; o[3] = (v & 255) / 255.f; }

/* convert one changed xfmem word into its place in the uniform block */
static void xf_word(Uniforms *u, u32 a) {
    if (a < 0x100) u->pmtx[a >> 2][a & 3] = xf_f(a);
    else if (a >= 0x400 && a < 0x460) u->nmtx[(a - 0x400) / 3][(a - 0x400) % 3] = xf_f(a);
    else if (a >= 0x500 && a < 0x600) u->ptmtx[(a - 0x500) >> 2][a & 3] = xf_f(a);
    else if (a >= 0x600 && a < 0x680) {
        int l = (a - 0x600) >> 4, o = (a - 0x600) & 15;
        if (o == 3) rgba(g_gpu.xfmem[a], u->lcol[l]);
        else if (o >= 4 && o < 7) u->lcos[l][o - 4] = xf_f(a);
        else if (o >= 7 && o < 10) u->ldist[l][o - 7] = xf_f(a);
        else if (o >= 10 && o < 13) u->lpos[l][o - 10] = xf_f(a);
        else if (o >= 13) u->ldir[l][o - 13] = xf_f(a);
    }
}

/* bring the uniform block up to date: only the xfmem words written since the last fill (tracked by
 * load_xf), plus the small register-derived part */
static void fill_uniforms(Uniforms *u) {
    for (u32 w = g_gpu.xf_lo; w <= g_gpu.xf_hi && w < 0x1000; w++) xf_word(u, w);
    g_gpu.xf_lo = 0xFFFFFFFFu;
    g_gpu.xf_hi = 0;
    rgba(g_gpu.xfreg[0x0A], u->ambc[0]);
    rgba(g_gpu.xfreg[0x0B], u->ambc[1]);
    rgba(g_gpu.xfreg[0x0C], u->matc[0]);
    rgba(g_gpu.xfreg[0x0D], u->matc[1]);
    for (int k = 0; k < 4; k++) u->proj[0][k] = u2f(g_gpu.xfreg[0x20 + k]);
    u->proj[1][0] = u2f(g_gpu.xfreg[0x24]);
    u->proj[1][1] = u2f(g_gpu.xfreg[0x25]);
    u->proj[1][2] = (float)(g_gpu.xfreg[0x26] & 1);
    for (int k = 0; k < 3; k++) {
        u->vp[0][k] = u2f(g_gpu.xfreg[0x1A + k]);
        u->vp[1][k] = u2f(g_gpu.xfreg[0x1D + k]);
    }
    memcpy(u->kc, g_gpu.kreg, sizeof u->kc);
    memcpy(u->tevreg, g_gpu.creg, sizeof u->tevreg);
    u32 ar = g_gpu.bp[0xF3];
    u->alpharef[0] = ar & 255;
    u->alpharef[1] = (ar >> 8) & 255;
    u32 fc = g_gpu.bp[0xF2];
    u->fogc[0] = ((fc >> 16) & 255) / 255.f; u->fogc[1] = ((fc >> 8) & 255) / 255.f; u->fogc[2] = (fc & 255) / 255.f;
    /* fog: a, b (mantissa + shift), c in GX's packed float formats */
    u32 fa = g_gpu.bp[0xEE], fbm = g_gpu.bp[0xEF], fbe = g_gpu.bp[0xF0], fcc = g_gpu.bp[0xF1];
    u->fogp[0] = u2f((fa & 0x7FFFF) << 12);
    u->fogp[1] = (fbm & 0xFFFFFF) / 8388608.0f;
    u->fogp[2] = (float)(1u << ((fbe & 31) ? (fbe & 31) - 1 : 0));
    u->fogp[3] = u2f((fcc & 0x7FFFF) << 12);
}

/* ---------------------------------------------------------------- render state */

static void apply_state(void) {
    static u32 last[6], valid;
    static int forcemask = -1;
    if (forcemask < 0) forcemask = ENV("RT_FORCEMASK") != NULL;
    u32 now[6] = {g_gpu.bp[0x00] & (3 << 14), g_gpu.bp[0x40], g_gpu.bp[0x41], g_gpu.bp[0x20], g_gpu.bp[0x21], g_gpu.state_epoch};
    if (valid && !memcmp(now, last, sizeof now)) return;
    memcpy(last, now, sizeof now);
    valid = 1;
    u32 gen = g_gpu.bp[0x00];
    int cull = (gen >> 14) & 3;
    static int nocull = -1;
    if (nocull < 0) nocull = ENV("RT_NOCULL") != NULL;
    if (nocull) cull = 0;
    if (cull == 0)
        glDisable(GL_CULL_FACE);
    else {
        glEnable(GL_CULL_FACE);
        glFrontFace(GL_CW);   /* GX front faces are clockwise in our clip space (verified against the attract-mode stadiums) */
        glCullFace(cull == 1 ? GL_BACK : cull == 2 ? GL_FRONT : GL_FRONT_AND_BACK);
    }
    u32 z = g_gpu.bp[0x40];
    static const GLenum zf[8] = {GL_NEVER, GL_LESS, GL_EQUAL, GL_LEQUAL, GL_GREATER, GL_NOTEQUAL, GL_GEQUAL, GL_ALWAYS};
    static int nodepth = -1;
    if (nodepth < 0) nodepth = ENV("RT_NODEPTH") != NULL;
    if ((z & 1) && !nodepth) { glEnable(GL_DEPTH_TEST); glDepthFunc(zf[(z >> 1) & 7]); }
    else glDisable(GL_DEPTH_TEST);
    glDepthMask((z >> 4) & 1);
    u32 b = g_gpu.bp[0x41];
    static const GLenum sf[8] = {GL_ZERO, GL_ONE, GL_DST_COLOR, GL_ONE_MINUS_DST_COLOR, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_DST_ALPHA, GL_ONE_MINUS_DST_ALPHA};
    static const GLenum df[8] = {GL_ZERO, GL_ONE, GL_SRC_COLOR, GL_ONE_MINUS_SRC_COLOR, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_DST_ALPHA, GL_ONE_MINUS_DST_ALPHA};
    if (b & 1) {
        glEnable(GL_BLEND);
        if (b & (1 << 11)) { glBlendEquation(GL_FUNC_REVERSE_SUBTRACT); glBlendFunc(GL_ONE, GL_ONE); }
        else { glBlendEquation(GL_FUNC_ADD); glBlendFunc(sf[(b >> 8) & 7], df[(b >> 5) & 7]); }
    } else
        glDisable(GL_BLEND);
    glColorMask((b >> 3) & 1, (b >> 3) & 1, (b >> 3) & 1, (b >> 4) & 1);
    if (forcemask) { glColorMask(1, 1, 1, 1); glDisable(GL_BLEND); }
    u32 s0 = g_gpu.bp[0x20], s1 = g_gpu.bp[0x21];
    int x0 = (int)((s0 >> 12) & 0x7FF) - 342, y0 = (int)(s0 & 0x7FF) - 342;
    int x1 = (int)((s1 >> 12) & 0x7FF) - 342, y1 = (int)(s1 & 0x7FF) - 342;
    glEnable(GL_SCISSOR_TEST);
    if (x1 >= x0 && y1 >= y0)
        glScissor(x0 * S, (EFB_H - 1 - y1) * S, (x1 - x0 + 1) * S, (y1 - y0 + 1) * S);
    else
        glScissor(0, 0, 0, 0);
}

/* ---------------------------------------------------------------- draw */

/* Batching: consecutive draws with the same program, textures, uniforms and render state are
 * appended to one indexed triangle list and drawn with a single GL call. */
#define MAXB (1 << 18)
static GLVertex bverts[MAXB];
static u32 bidx[MAXB * 3];
static u32 nbv, nbi;
static GLuint batch_prog;
typedef struct { GLuint prog; u32 tex[8][4]; u32 st[5]; } BatchSig;
static BatchSig cur_sig;
static int have_sig;

/* uniforms go into a ring buffer: each change is written to fresh space without waiting for the GPU
 * (the whole ring is orphaned when it wraps) and bound by offset */
#define UBO_RING (32u << 20)
static void upload_uniforms(const Uniforms *u) {
    static GLint align;
    static u32 off = UBO_RING;
    if (!align) { glGetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &align); if (align < 16) align = 16; }
    u32 size = (sizeof *u + align - 1) / align * align;
    glBindBuffer(GL_UNIFORM_BUFFER, ubo);
    if (off + size > UBO_RING) {
        glBufferData(GL_UNIFORM_BUFFER, UBO_RING, NULL, GL_STREAM_DRAW);
        off = 0;
    }
    void *dst = glMapBufferRange(GL_UNIFORM_BUFFER, off, sizeof *u, GL_MAP_WRITE_BIT | GL_MAP_UNSYNCHRONIZED_BIT | GL_MAP_INVALIDATE_RANGE_BIT);
    if (dst) {
        memcpy(dst, u, sizeof *u);
        glUnmapBuffer(GL_UNIFORM_BUFFER);
    }
    glBindBufferRange(GL_UNIFORM_BUFFER, 0, ubo, off, sizeof *u);
    off += size;
}

static void batch_flush(void) {
    if (!nbi) { nbv = 0; return; }
    glUseProgram(batch_prog);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, nbv * sizeof(GLVertex), bverts, GL_STREAM_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, nbi * 4, bidx, GL_STREAM_DRAW);
    glDrawElements(GL_TRIANGLES, nbi, GL_UNSIGNED_INT, 0);
    nbv = nbi = 0;
}
void gfx_batch_flush(void) { batch_flush(); have_sig = 0; }

void gfx_draw(int prim, u32 n, VertexFill fill, void *ctx) {
    if (!ready || !n)
        return;
    draws++;
    verts_total += n;
    stat_verts += n;
    if (n > MAXB / 2) n = MAXB / 2;
    PipeKey k;
    memset(&k, 0, sizeof k);
    k.genmode = g_gpu.bp[0x00] & (15 | (15 << 10));
    int ns = ((k.genmode >> 10) & 15) + 1;
    for (int s = 0; s < ns; s++) { k.tev_c[s] = g_gpu.bp[0xC0 + 2 * s] & 0xFFFFFF; k.tev_a[s] = g_gpu.bp[0xC1 + 2 * s] & 0xFFFFFF; }
    for (int i = 0; i < (ns + 1) / 2; i++) k.tref[i] = g_gpu.bp[0x28 + i] & 0xFFFFFF;
    for (int i = 0; i < 8; i++) k.ksel[i] = g_gpu.bp[0xF6 + i] & 0xFFFFFF;
    k.alpha = g_gpu.bp[0xF3] & 0xFF0000;
    k.colctl[0] = g_gpu.xfreg[0x0E]; k.colctl[1] = g_gpu.xfreg[0x0F]; k.colctl[2] = g_gpu.xfreg[0x10]; k.colctl[3] = g_gpu.xfreg[0x11];
    int ntg = k.genmode & 15;
    for (int t = 0; t < ntg; t++) { k.texgen[t] = g_gpu.xfreg[0x40 + t]; k.postgen[t] = g_gpu.xfreg[0x50 + t]; }
    k.dualtex = g_gpu.xfreg[0x12] & 1;
    k.fog = g_gpu.bp[0xF1] & (7u << 21);
    GLuint prog = get_prog(&k);
    BatchSig sig;
    memset(&sig, 0, sizeof sig);
    sig.prog = prog;
    for (int s = 0; s < ns; s++) {
        u32 tref = k.tref[s >> 1] >> ((s & 1) * 12);
        if ((tref >> 6) & 1) {
            int m = tref & 7, bank = m < 4 ? 0 : 0x20;
            sig.tex[m][0] = g_gpu.bp[0x80 + bank + (m & 3)] | 0x80000000u;
            sig.tex[m][1] = g_gpu.bp[0x88 + bank + (m & 3)];
            sig.tex[m][2] = g_gpu.bp[0x94 + bank + (m & 3)];
            sig.tex[m][3] = g_gpu.bp[0x98 + bank + (m & 3)];
        }
    }
    sig.st[0] = g_gpu.bp[0x00] & (3 << 14); sig.st[1] = g_gpu.bp[0x40]; sig.st[2] = g_gpu.bp[0x41];
    sig.st[3] = g_gpu.bp[0x20]; sig.st[4] = g_gpu.bp[0x21];
    int tri = prim <= 4;
    if (!tri || g_gpu.udirty || !have_sig || memcmp(&sig, &cur_sig, sizeof sig) || nbv + n > MAXB || nbi + n * 3 > MAXB * 3) {
        batch_flush();
        cur_sig = sig;
        have_sig = 1;
        batch_prog = prog;
        glUseProgram(prog);
        for (int s = 0; s < ns; s++) {
            u32 tref = k.tref[s >> 1] >> ((s & 1) * 12);
            if ((tref >> 6) & 1) bind_texture(tref & 7);
        }
        static Uniforms u;
        if (g_gpu.udirty) {
            fill_uniforms(&u);
            upload_uniforms(&u);
        }
        g_gpu.udirty = 0;
        glBindFramebuffer(GL_FRAMEBUFFER, efb_fbo);
        glViewport(0, 0, EW, EH);
        apply_state();
    }
    /* append vertices */
    u32 base = nbv;
    fill(&bverts[base], n, ctx);
    nbv += n;
    switch (prim) {
    case 0: case 1:   /* quads */
        for (u32 q = 0; q + 3 < n; q += 4) {
            bidx[nbi++] = base + q; bidx[nbi++] = base + q + 1; bidx[nbi++] = base + q + 2;
            bidx[nbi++] = base + q; bidx[nbi++] = base + q + 2; bidx[nbi++] = base + q + 3;
        }
        break;
    case 2:
        for (u32 t = 0; t + 2 < n; t += 3) { bidx[nbi++] = base + t; bidx[nbi++] = base + t + 1; bidx[nbi++] = base + t + 2; }
        break;
    case 3:   /* strip: keep the winding of every other triangle */
        for (u32 t = 0; t + 2 < n; t++) {
            if (t & 1) { bidx[nbi++] = base + t + 1; bidx[nbi++] = base + t; }
            else { bidx[nbi++] = base + t; bidx[nbi++] = base + t + 1; }
            bidx[nbi++] = base + t + 2;
        }
        break;
    case 4:   /* fan */
        for (u32 t = 1; t + 1 < n; t++) { bidx[nbi++] = base; bidx[nbi++] = base + t; bidx[nbi++] = base + t + 1; }
        break;
    default: {   /* lines / points: drawn on their own */
        static const GLenum mode[8] = {0, 0, 0, 0, 0, GL_LINES, GL_LINE_STRIP, GL_POINTS};
        glBindVertexArray(vao);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, n * sizeof(GLVertex), bverts + base, GL_STREAM_DRAW);
        glDrawArrays(mode[prim], 0, n);
        nbv = base;
        have_sig = 0;
        break;
    }
    }
    static long dump_frame = -2;
    if (dump_frame == -2) dump_frame = ENV("RT_DRAWDUMP") ? atol(ENV("RT_DRAWDUMP")) : -1;
    if (dump_frame == 0 && g_gpu.last_frame_draws > 1000) dump_frame = (long)g_gpu.frames + 1;   /* auto */
    if ((long)g_gpu.frames == dump_frame) {
        fprintf(stderr, "draw prim %d n %u stages %d texgens %d chans %d ind %d | cc0 %06X ca0 %06X tref0 %06X | alpha %06X blend %06X z %06X | colctl %08X %08X | fog %06X",
                prim, n, ns, k.genmode & 15, (g_gpu.bp[0] >> 4) & 7, (g_gpu.bp[0] >> 16) & 7, k.tev_c[0], k.tev_a[0], k.tref[0],
                g_gpu.bp[0xF3], g_gpu.bp[0x41], g_gpu.bp[0x40], k.colctl[0], k.colctl[2], g_gpu.bp[0xF1]);
        for (int s2 = 0; s2 < ns; s2++) {
            u32 tr = k.tref[s2 >> 1] >> ((s2 & 1) * 12);
            if ((tr >> 6) & 1) {
                int m = tr & 7, bank = m < 4 ? 0 : 0x20;
                u32 img0 = g_gpu.bp[0x88 + bank + (m & 3)];
                fprintf(stderr, " | tex%d fmt %u %ux%u @%08X", m, (img0 >> 20) & 15, (img0 & 0x3FF) + 1, ((img0 >> 10) & 0x3FF) + 1,
                        (g_gpu.bp[0x94 + bank + (m & 3)] & 0xFFFFFF) << 5);
            }
        }
        u32 lo = g_gpu.cp[0x50];
        int vat = g_gpu.cur_vat;
        fprintf(stderr, " | vat %d col0 mode %u fmt %u v0col %.2f %.2f %.2f %.2f nrm %u pos %u", vat, (lo >> 13) & 3,
                (g_gpu.cp[0x70 + vat] >> 14) & 7, bverts[base].col0[0] / 255.f, bverts[base].col0[1] / 255.f, bverts[base].col0[2] / 255.f, bverts[base].col0[3] / 255.f, (lo >> 11) & 3, (lo >> 9) & 3);
        fprintf(stderr, "\n");
    }
    static int pick_x = -1, pick_y;
    if (pick_x == -1) { const char *pk = ENV("RT_PICK"); pick_x = -2; if (pk) sscanf(pk, "%d,%d", &pick_x, &pick_y); }
    if (pick_x >= 0 && (long)g_gpu.frames == dump_frame) {
        gfx_batch_flush();
        static u8 last[4];
        u8 px[4];
        glReadPixels(pick_x * S, (EFB_H - 1 - pick_y) * S, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
        if (memcmp(px, last, 4)) {
            fprintf(stderr, "PICK draw %llu -> %02X%02X%02X%02X  stages:", (unsigned long long)draws, px[0], px[1], px[2], px[3]);
            for (int st = 0; st < ns; st++)
                fprintf(stderr, " [c %06X a %06X tref %03X ksel %06X]", k.tev_c[st], k.tev_a[st], (k.tref[st >> 1] >> ((st & 1) * 12)) & 0xFFF, k.ksel[st >> 1]);
            fprintf(stderr, " colctl %08X/%08X texgen0 %08X kreg0 %.2f %.2f %.2f %.2f creg1 %.2f %.2f %.2f matc %08X amb %08X\n", k.colctl[0], k.colctl[2], k.texgen[0],
                    g_gpu.kreg[0][0], g_gpu.kreg[0][1], g_gpu.kreg[0][2], g_gpu.kreg[0][3], g_gpu.creg[1][0], g_gpu.creg[1][1], g_gpu.creg[1][2], g_gpu.xfreg[0x0C], g_gpu.xfreg[0x0A]);
            memcpy(last, px, 4);
        }
    }
}

/* ---------------------------------------------------------------- EFB copies and frames */

/* EFB -> texture with the copy's pixel-format conversion. The copy command (BP 0x52) holds the
 * target format in bits 3-6 (real format = f/2 + (f&1)*8: R4 R8 RA4 RA8 RGB565 RGB5A3 RGBA8 A8,
 * then R8 G8 B8 RG8 GB8) and the intensity (YUV luma) flag in bit 15. The result is stored as RGBA
 * laid out the way the texture format the game samples it with expands it. */
static GLuint copy_prog, copy_vao;
static void efb_convert_copy(GLuint dst_tex, u32 cmd, int sx, int sy, int sw, int sh, int tw, int th) {
    if (!copy_prog) {
        const char *vs = "#version 330 core\nuniform vec4 rect; out vec2 uv;\n"
                         "void main() { vec2 p = vec2(gl_VertexID & 1, gl_VertexID >> 1); uv = rect.xy + p * rect.zw;"
                         " gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0); }\n";
        const char *fs = "#version 330 core\nuniform sampler2D efb; uniform int mode; uniform int intensity; uniform int has_alpha;"
                         " in vec2 uv; out vec4 o;\n"
                         "void main() { vec4 c = texture(efb, uv); if (has_alpha == 0) c.a = 1.0;\n"
                         " float y = dot(c.rgb, vec3(0.257, 0.504, 0.098)) + 16.0 / 255.0;\n"
                         " float r = intensity != 0 ? y : c.r;\n"
                         " if (mode == 0 || mode == 1 || mode == 8) o = vec4(r);\n"          /* R4/R8 (I4/I8) */
                         " else if (mode == 2 || mode == 3) o = vec4(r, r, r, c.a);\n"        /* RA4/RA8 (IA4/IA8) */
                         " else if (mode == 4) o = vec4(c.rgb, 1.0);\n"                       /* RGB565 */
                         " else if (mode == 7) o = vec4(c.a);\n"                              /* A8 */
                         " else if (mode == 9) o = vec4(c.g);\n"
                         " else if (mode == 10) o = vec4(c.b);\n"
                         " else if (mode == 11) o = vec4(c.r, c.r, c.r, c.g);\n"             /* RG8 */
                         " else if (mode == 12) o = vec4(c.g, c.g, c.g, c.b);\n"             /* GB8 */
                         " else o = c; }\n";                                                    /* RGB5A3 / RGBA8 */
        copy_prog = glCreateProgram();
        GLuint v = compile(GL_VERTEX_SHADER, vs), f = compile(GL_FRAGMENT_SHADER, fs);
        glAttachShader(copy_prog, v);
        glAttachShader(copy_prog, f);
        glLinkProgram(copy_prog);
        glGenVertexArrays(1, &copy_vao);
    }
    static GLuint fbo;
    if (!fbo) glGenFramebuffers(1, &fbo);
    u32 f = (cmd >> 3) & 15;
    int mode = (int)(f / 2 + (f & 1) * 8);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, dst_tex, 0);
    glViewport(0, 0, tw, th);
    glDisable(GL_SCISSOR_TEST); glDisable(GL_BLEND); glDisable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE);
    glColorMask(1, 1, 1, 1);
    glUseProgram(copy_prog);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, efb_color);
    glUniform1i(glGetUniformLocation(copy_prog, "efb"), 0);
    glUniform1i(glGetUniformLocation(copy_prog, "mode"), mode);
    glUniform1i(glGetUniformLocation(copy_prog, "intensity"), (cmd >> 15) & 1);
    glUniform1i(glGetUniformLocation(copy_prog, "has_alpha"), (g_gpu.bp[0x43] & 7) == 1);   /* RGBA6_Z24 */
    /* texture row 0 is the top of the copied rectangle; the EFB texture is stored bottom-up */
    glUniform4f(glGetUniformLocation(copy_prog, "rect"), (float)sx / EFB_W, (float)(EFB_H - sy) / EFB_H,
                (float)sw / EFB_W, -(float)sh / EFB_H);
    glBindVertexArray(copy_vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(vao);
    glViewport(0, 0, EW, EH);
}

static u8 *frame_rgba;   /* EW x EH readback for frame dumps */

static void write_ppm(const char *dir, u64 frame, int sx, int sy, int w, int h) {
    char path[512];
    snprintf(path, sizeof path, "%s/frame_%06llu.ppm", dir, (unsigned long long)frame);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    int k = ENV("RT_FRAMES_HIRES") ? S : 1;   /* full internal resolution, or native size */
    fprintf(f, "P6\n%d %d\n255\n", w * k, h * k);
    for (int y = 0; y < h * k; y++)
        for (int x = 0; x < w * k; x++)
            fwrite(frame_rgba + 4 * ((size_t)(EH - 1 - (sy * S + y * S / k)) * EW + sx * S + x * S / k), 1, 3, f);
    fclose(f);
}

#include <time.h>
static struct timespec g_start;

/* RT_VIDEO=file: stream presented frames to ffmpeg as constant 60 fps video in wall-clock time
 * (frames repeat while the game holds one), so playback speed is what a player would see */
#define VID_W 640
#define VID_H 480
static void record_video(int sx, int sy, int sw, int sh) {
    static FILE *pipe_;
    static int failed;
    static double next_t;
    static u8 canvas[VID_W * VID_H * 4];
    const char *out = ENV("RT_VIDEO");
    if (!out || failed) return;
    if (!pipe_) {
        char cmd[1024];
        snprintf(cmd, sizeof cmd, "ffmpeg -loglevel error -y -f rawvideo -pix_fmt rgba -s %dx%d -r 60 -i - "
                 "-vf vflip,scale=%d:480 -c:v libx264 -preset veryfast -crf 20 -pix_fmt yuv420p '%s'", VID_W, VID_H,
                 g_widescreen ? 854 : 640, out);
        if (!(pipe_ = popen(cmd, "w"))) { failed = 1; return; }
    }
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    double now = (t.tv_sec - g_start.tv_sec) + (t.tv_nsec - g_start.tv_nsec) / 1e9;
    if (now < next_t) return;   /* faster than 60 Hz: drop */
    if (sw > VID_W) sw = VID_W;
    if (sh > EFB_H) sh = EFB_H;
    if (sw < 1 || sh < 1) return;
    /* read the presented region (at internal resolution), then sample it into the canvas, stretched
     * to the full height like the VI does */
    static u8 *src;
    static size_t src_size;
    size_t need = (size_t)sw * S * sh * S * 4;
    if (need > src_size) { free(src); src = malloc(need); src_size = need; }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, efb_fbo);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(sx * S, (EFB_H - sy - sh) * S, sw * S, sh * S, GL_RGBA, GL_UNSIGNED_BYTE, src);
    for (int y = 0; y < VID_H; y++) {
        const u8 *row = src + (size_t)4 * sw * S * ((y * sh / VID_H) * S);
        u8 *out = canvas + 4 * VID_W * y;
        for (int x = 0; x < VID_W; x++) memcpy(out + 4 * x, row + (size_t)4 * ((x < sw ? x : sw - 1) * S), 4);
    }
    if (next_t == 0) { next_t = now; fprintf(stderr, "video: starts %.3f s after launch (offset for RT_WAV)\n", now); }
    int reps = 0;
    while (next_t <= now + 1e-6) { reps++; next_t += 1.0 / 60; }   /* fill the time this frame was shown */
    while (reps--) fwrite(canvas, 1, sizeof canvas, pipe_);
}
__attribute__((constructor)) static void note_start(void) { clock_gettime(CLOCK_MONOTONIC, &g_start); }
void gfx_present(int sx, int sy, int sw, int sh) {
    gfx_batch_flush();
    invalidate_gl_caches();
    static struct timespec t0;
    static u64 f0;
    if (ENV("RT_FPS") && g_gpu.frames % 120 == 0) {
        struct timespec t;
        clock_gettime(CLOCK_MONOTONIC, &t);
        static u64 busy0;
        double secs = (t.tv_sec - t0.tv_sec) + (t.tv_nsec - t0.tv_nsec) / 1e9;
        if (f0) fprintf(stderr, "fps %.1f (frame %llu, %llu draws, gpu thread %.0f%% busy)\n", (g_gpu.frames - f0) / secs,
                        (unsigned long long)g_gpu.frames, (unsigned long long)g_gpu.last_frame_draws, (gpu_busy_ns - busy0) / 1e7 / secs);
        busy0 = gpu_busy_ns;
        if (f0) fprintf(stderr, "    new textures %llu (%.1f MB decoded), %llu vertices\n", (unsigned long long)stat_tex_new, stat_tex_bytes / 1e6, (unsigned long long)stat_verts);
        stat_tex_new = stat_tex_bytes = stat_verts = 0;
        t0 = t;
        f0 = g_gpu.frames;
    }
    const char *dir = ENV("RT_FRAMES");
    int every = ENV("RT_FRAMES_EVERY") ? atoi(ENV("RT_FRAMES_EVERY")) : 60;
    if (sw > EFB_W - sx) sw = EFB_W - sx;
    if (sh > EFB_H - sy) sh = EFB_H - sy;
    static long ffrom = -2, fto;
    if (ffrom == -2) { ffrom = ENV("RT_FRAMES_FROM") ? atol(ENV("RT_FRAMES_FROM")) : 0; fto = ENV("RT_FRAMES_TO") ? atol(ENV("RT_FRAMES_TO")) : 1L << 40; }
    if (dir && every > 0 && g_gpu.frames % every == 0 && (long)g_gpu.frames >= ffrom && (long)g_gpu.frames <= fto) {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, efb_fbo);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        if (!frame_rgba) frame_rgba = malloc((size_t)EW * EH * 4);
        glReadPixels(0, 0, EW, EH, GL_RGBA, GL_UNSIGNED_BYTE, frame_rgba);
        write_ppm(dir, g_gpu.frames, sx, sy, sw, sh);
        /* wall-clock time of each dumped frame since startup, to line video up with RT_WAV audio */
        static FILE *times;
        if (!times) { char p[512]; snprintf(p, sizeof p, "%s/times.txt", dir); times = fopen(p, "w"); }
        if (times) {
            struct timespec t;
            clock_gettime(CLOCK_MONOTONIC, &t);
            fprintf(times, "%06llu %.4f\n", (unsigned long long)g_gpu.frames, (t.tv_sec - g_start.tv_sec) + (t.tv_nsec - g_start.tv_nsec) / 1e9);
            fflush(times);
        }
    }
    record_video(sx, sy, sw, sh);
    window_present(efb_color, sx * S, sy * S, sw * S, sh * S, EW, EH);
    glBindFramebuffer(GL_FRAMEBUFFER, efb_fbo);
}

void gfx_efb_copy(u32 cmd) {
    if (!ready)
        return;
    gfx_batch_flush();
    invalidate_gl_caches();
    u32 tl = g_gpu.bp[0x49], wh = g_gpu.bp[0x4A];
    int sx = tl & 0x3FF, sy = (tl >> 10) & 0x3FF, sw = (wh & 0x3FF) + 1, sh = ((wh >> 10) & 0x3FF) + 1;
    u32 dst = (g_gpu.bp[0x4B] & 0xFFFFFF) << 5;
    int to_xfb = (cmd >> 14) & 1, clear = (cmd >> 11) & 1;
    glDisable(GL_SCISSOR_TEST);
    if (to_xfb) {
        g_gpu.frames++;
        if (ENV("RT_GFXLOG"))
            fprintf(stderr, "frame %llu: %llu draws, %llu vertices, %d programs, %d textures, %llu DL calls, %d bad ops, %llu preloads\n", (unsigned long long)g_gpu.frames,
                    (unsigned long long)draws, (unsigned long long)verts_total, nprogs, ntex, (unsigned long long)g_gpu.ndl, g_gpu.bad_ops, (unsigned long long)g_gpu.preloads);
        g_gpu.last_frame_draws = draws;
        draws = verts_total = 0;
        gfx_present(sx, sy, sw, sh);
    } else if (sw > 0 && sh > 0 && sx + sw <= EFB_W && sy + sh <= EFB_H) {
        /* EFB -> texture: kept as a GL texture found by its guest address */
        int half = (cmd >> 9) & 1;
        if (ENV("RT_EFBLOG"))
            fprintf(stderr, "efb copy frame %llu: %d,%d %dx%d -> %08X fmt %u%s half %d clear %d | pixfmt %u\n", (unsigned long long)g_gpu.frames,
                    sx, sy, sw, sh, dst, (((cmd >> 3) & 15) / 2 + (((cmd >> 3) & 1) * 8)), (cmd >> 15) & 1 ? "(I)" : "", half, clear, g_gpu.bp[0x43] & 7);
        int tw = half ? (sw + 1) / 2 : sw, th = half ? (sh + 1) / 2 : sh;
        TexEntry *e = tex_find_efb(dst);
        if (!e) {
            e = tex_add(dst);
            glGenTextures(1, &e->tex);
        }
        if (e) {
            e->addr = dst; e->w = tw; e->h = th; e->efb = 1; e->mode0 = 0;
            g_gpu.efb_gen++;
            glBindTexture(GL_TEXTURE_2D, e->tex);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, tw * S, th * S, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
            efb_convert_copy(e->tex, cmd, sx, sy, sw, sh, tw * S, th * S);
            glBindFramebuffer(GL_FRAMEBUFFER, efb_fbo);
        }
    }
    if (clear) {
        u32 ar = g_gpu.bp[0x4F], gb = g_gpu.bp[0x50], zv = g_gpu.bp[0x51];
        glBindFramebuffer(GL_FRAMEBUFFER, efb_fbo);
        glEnable(GL_SCISSOR_TEST);
        glScissor(sx * S, (EFB_H - sy - sh) * S, sw * S, sh * S);
        u32 pe = g_gpu.bp[0x41];
        glColorMask((pe >> 3) & 1, (pe >> 3) & 1, (pe >> 3) & 1, (pe >> 4) & 1);
        glDepthMask((g_gpu.bp[0x40] >> 4) & 1);
        glClearColor((ar & 255) / 255.f, ((gb >> 8) & 255) / 255.f, (gb & 255) / 255.f, ((ar >> 8) & 255) / 255.f);
        if (ENV("RT_CLEARDEBUG")) glClearColor(1, 0, 1, 1);
        if (ENV("RT_CLEARDEBUG")) glClearColor(1, 0, 1, 1);
        glClearDepth((zv & 0xFFFFFF) / 16777215.0);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glDisable(GL_SCISSOR_TEST);
    }
}

void gfx_flush(void) {
    if (!ready) return;
    gfx_batch_flush();
    glFlush();
}

/* ---------------------------------------------------------------- init */

void gfx_init(void) {
    int windowed = gfx_window_init(EFB_W, EFB_H);
    S = ENV("RT_SCALE") ? atoi(ENV("RT_SCALE")) : windowed ? 2 : 1;
    if (S < 1) S = 1;
    if (S > 8) S = 8;
    if (!windowed && !ctx_init()) {
        fprintf(stderr, "gfx: no OpenGL context; running without rendering\n");
        return;
    }
    glGenFramebuffers(1, &efb_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, efb_fbo);
    glGenTextures(1, &efb_color);
    glBindTexture(GL_TEXTURE_2D, efb_color);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, EW, EH, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, efb_color, 0);
    glGenRenderbuffers(1, &efb_depth);
    glBindRenderbuffer(GL_RENDERBUFFER, efb_depth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, EW, EH);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, efb_depth);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "gfx: EFB framebuffer incomplete\n");
        return;
    }
    fprintf(stderr, "gfx: internal resolution %dx%d (RT_SCALE=%d)\n", EW, EH, S);
    glViewport(0, 0, EW, EH);
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(1, &vbo);
    glGenBuffers(1, &ibo);
    glGenBuffers(1, &ubo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    GLsizei st = sizeof(GLVertex);
    glEnableVertexAttribArray(0); glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, st, (void *)offsetof(GLVertex, pos));
    glEnableVertexAttribArray(1); glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, st, (void *)offsetof(GLVertex, nrm));
    glEnableVertexAttribArray(2); glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, st, (void *)offsetof(GLVertex, col0));
    glEnableVertexAttribArray(3); glVertexAttribPointer(3, 4, GL_UNSIGNED_BYTE, GL_TRUE, st, (void *)offsetof(GLVertex, col1));
    for (int i = 0; i < 4; i++) {
        glEnableVertexAttribArray(4 + i);
        glVertexAttribPointer(4 + i, 4, GL_FLOAT, GL_FALSE, st, (void *)(offsetof(GLVertex, tex) + 16 * i));
    }
    glEnableVertexAttribArray(8); glVertexAttribPointer(8, 1, GL_UNSIGNED_BYTE, GL_FALSE, st, (void *)offsetof(GLVertex, mtx));
    glEnableVertexAttribArray(9); glVertexAttribPointer(9, 4, GL_UNSIGNED_BYTE, GL_FALSE, st, (void *)(offsetof(GLVertex, mtx) + 4));
    glEnableVertexAttribArray(10); glVertexAttribPointer(10, 4, GL_UNSIGNED_BYTE, GL_FALSE, st, (void *)(offsetof(GLVertex, mtx) + 8));
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    ready = 1;
    fprintf(stderr, "gfx: %s\n", glGetString(GL_RENDERER));
}
