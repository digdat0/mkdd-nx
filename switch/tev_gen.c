/* Specialised TEV fragment shaders.
 *
 * The uber shader (render_gl.c TEV_FS) interprets the stage configuration per pixel: a loop over
 * up to 16 stages with dynamically indexed input arrays, which nouveau spills to local memory.
 * Here the configuration that selects code paths is baked into GLSL instead (one program per
 * distinct configuration); colours, konst values, alpha references and fog parameters stay in the
 * same Tev uniform block, so a specialised program is a drop-in replacement for the uber shader.
 * Semantics follow TEV_FS line by line. Pure C (no GL) so the PC build can test it. */
#include "tev_gen.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static inline uint32_t bitsu(uint32_t v, uint32_t lo, uint32_t n) { return (v >> lo) & ((1u << n) - 1); }

uint64_t tev_shader_key(const TevState *t) {
    unsigned n = t->params[0] > 16 ? 16 : t->params[0];
    uint64_t h = 0xCBF29CE484222325ull;
#define MIX(v) do { h = (h ^ (uint64_t)(v)) * 0x100000001B3ull; h ^= h >> 31; } while (0)
    MIX(n);
    for (unsigned s = 0; s < n; s++) {
        MIX(t->stages[s][0]); MIX(t->stages[s][1]);
        /* order: drop the map index of disabled stages' textures? no: map/tc/en/chan all matter */
        MIX(t->stages[s][2]); MIX(t->stages[s][3]);
    }
    /* alpha compare functions + logic (refs are uniforms) */
    MIX(t->params[1] & 0xFF0000u);
    MIX(t->params[1] >> 24 & 0xFF);
    MIX(t->params[2]);
    MIX((uint32_t)t->fog_color[3]);
#undef MIX
    return h ? h : 1;
}

typedef struct { char *p; size_t cap, len; } Buf;
static void out(Buf *b, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(b->p + b->len, b->len < b->cap ? b->cap - b->len : 0, fmt, ap);
    va_end(ap);
    if (n > 0) b->len += (size_t)n;
}

static void swizzle(uint32_t swap, uint32_t tbl, char o[6]) {
    static const char rgba[4] = {'r', 'g', 'b', 'a'};
    uint32_t p = (swap >> (8 * tbl)) & 0xFF;
    o[0] = '.';
    o[1] = rgba[p & 3]; o[2] = rgba[(p >> 2) & 3]; o[3] = rgba[(p >> 4) & 3]; o[4] = rgba[(p >> 6) & 3];
    o[5] = 0;
}

static void konst_expr(uint32_t sel, char *o, size_t n) {
    if (sel < 8) snprintf(o, n, "vec4(%.9g)", (double)(8 - sel) / 8.0);
    else if (sel >= 0x0C && sel <= 0x0F) snprintf(o, n, "tev.konst[%u]", sel - 0x0C);
    else if (sel >= 0x10) snprintf(o, n, "vec4(tev.konst[%u].%c)", sel & 3, "rgba"[(sel - 0x10) / 4]);
    else snprintf(o, n, "vec4(1.0)");
}

static const char *CIN[16] = {"R0.rgb", "vec3(R0.a)", "R1.rgb", "vec3(R1.a)", "R2.rgb", "vec3(R2.a)", "R3.rgb", "vec3(R3.a)",
                              "tx.rgb", "vec3(tx.a)", "ras.rgb", "vec3(ras.a)", "vec3(1.0)", "vec3(0.5)", "kc.rgb", "vec3(0.0)"};
static const char *AIN[8] = {"R0.a", "R1.a", "R2.a", "R3.a", "tx.a", "ras.a", "ka", "0.0"};

/* bias / scale / clamp of a lerp result (component expression e of type T) */
static void bias_scale(Buf *b, const char *T, const char *dst, uint32_t env) {
    uint32_t bias = bitsu(env, 16, 2), scale = bitsu(env, 20, 2);
    if (bias == 1) out(b, "%s += %s(0.5);", dst, T);
    if (bias == 2) out(b, "%s -= %s(0.5);", dst, T);
    if (scale == 1) out(b, "%s *= 2.0;", dst);
    if (scale == 2) out(b, "%s *= 4.0;", dst);
    if (scale == 3) out(b, "%s *= 0.5;", dst);
    if (bitsu(env, 19, 1)) out(b, "%s = clamp(%s, 0.0, 1.0);", dst, dst);
    else out(b, "%s = clamp(%s, -1024.0 / 255.0, 1023.0 / 255.0);", dst, dst);
}

static const char *cmp_op(uint32_t f) {
    static const char *ops[8] = {"false", "<", "==", "<=", ">", "!=", ">=", "true"};
    return ops[f & 7];
}
static void alpha_test_expr(Buf *b, const char *name, uint32_t func, const char *ref) {
    if (func == 0) out(b, "bool %s = false;", name);
    else if (func == 7) out(b, "bool %s = true;", name);
    else out(b, "bool %s = a8 %s %s;", name, cmp_op(func), ref);
}

size_t tev_gen_fs(const TevState *t, char *dst, size_t cap) {
    Buf B = {dst, cap, 0}, *b = &B;
    unsigned n = t->params[0] > 16 ? 16 : t->params[0];
    uint32_t swap = t->params[2], acr = t->params[1];
    out(b, "#version 320 es\nprecision highp float;\nprecision highp int;\n"
           "layout(std140) uniform Tev { uvec4 stages[16]; vec4 regs[4]; vec4 konst[4]; uvec4 params; vec4 fog; vec4 fog_color; } tev;\n"
           "uniform sampler2D tex0; uniform sampler2D tex1; uniform sampler2D tex2; uniform sampler2D tex3;\n"
           "uniform sampler2D tex4; uniform sampler2D tex5; uniform sampler2D tex6; uniform sampler2D tex7;\n"
           "layout(location=0) in vec4 v_c0; layout(location=1) in vec4 v_c1; layout(location=2) in vec4 v_uv01;"
           "layout(location=3) in vec4 v_uv23; layout(location=4) in vec4 v_uv45; layout(location=5) in vec4 v_uv67;\n"
           "out vec4 o_color;\n"
           "uint u8v(float x) { return uint(round(clamp(x, 0.0, 1.0) * 255.0)); }\n"
           "void main() {\n"
           "vec4 R0 = tev.regs[0], R1 = tev.regs[1], R2 = tev.regs[2], R3 = tev.regs[3];\n"
           "vec4 tx, ras, kc; float ka; vec3 ca, cb, cc, cd, col; float aa, ab, ac, ad, alpha;\n");
    static const char *uvn[4] = {"v_uv01", "v_uv23", "v_uv45", "v_uv67"};
    for (unsigned s = 0; s < n; s++) {
        uint32_t cenv = t->stages[s][0], aenv = t->stages[s][1], order = t->stages[s][2], ksel = t->stages[s][3];
        uint32_t map = order & 7, tc = (order >> 4) & 7, en = (order >> 7) & 1, chan = (order >> 8) & 7;
        char sw[6], kexpr[64];
        out(b, "// stage %u\n", s);
        swizzle(swap, (aenv >> 2) & 3, sw);
        if (en) out(b, "tx = texture(tex%u, %s.%s)%s;", map, uvn[tc >> 1], tc & 1 ? "zw" : "xy", sw);
        else out(b, "tx = vec4(1.0);");
        swizzle(swap, aenv & 3, sw);
        out(b, "ras = %s%s;", chan == 0 ? "v_c0" : chan == 1 ? "v_c1" : "vec4(0.0)", chan <= 1 ? sw : "");
        konst_expr(ksel & 31, kexpr, sizeof kexpr);
        out(b, "kc = %s;", kexpr);
        konst_expr((ksel >> 8) & 31, kexpr, sizeof kexpr);
        out(b, "ka = (%s).a;\n", kexpr);
        /* colour */
        out(b, "ca = clamp(%s, 0.0, 1.0); cb = clamp(%s, 0.0, 1.0); cc = clamp(%s, 0.0, 1.0); cd = %s;\n",
            CIN[(cenv >> 12) & 15], CIN[(cenv >> 8) & 15], CIN[(cenv >> 4) & 15], CIN[cenv & 15]);
        if (bitsu(cenv, 16, 2) == 3) {
            uint32_t mode = (bitsu(cenv, 20, 2) << 1) | bitsu(cenv, 18, 1);
            if ((mode >> 1) == 3) {
                const char *op = mode & 1 ? "==" : ">";
                out(b, "col = cd;");
                for (int k = 0; k < 3; k++) out(b, "if (u8v(ca[%d]) %s u8v(cb[%d])) col[%d] += cc[%d];", k, op, k, k, k);
            } else {
                const char *op = mode & 1 ? "==" : ">";
                if ((mode >> 1) == 0) out(b, "col = cd + ((u8v(ca.r) %s u8v(cb.r)) ? cc : vec3(0.0));", op);
                else if ((mode >> 1) == 1)
                    out(b, "col = cd + ((((u8v(ca.g) << 8u) | u8v(ca.r)) %s ((u8v(cb.g) << 8u) | u8v(cb.r))) ? cc : vec3(0.0));", op);
                else
                    out(b, "col = cd + ((((u8v(ca.b) << 16u) | (u8v(ca.g) << 8u) | u8v(ca.r)) %s ((u8v(cb.b) << 16u) | (u8v(cb.g) << 8u) | u8v(cb.r))) ? cc : vec3(0.0));", op);
            }
            out(b, "col = clamp(col, 0.0, 1.0);\n");
        } else {
            out(b, "col = cd %c (ca * (vec3(1.0) - cc) + cb * cc);", bitsu(cenv, 18, 1) ? '-' : '+');
            bias_scale(b, "vec3", "col", cenv);
            out(b, "\n");
        }
        /* alpha */
        out(b, "aa = clamp(%s, 0.0, 1.0); ab = clamp(%s, 0.0, 1.0); ac = clamp(%s, 0.0, 1.0); ad = %s;\n",
            AIN[(aenv >> 13) & 7], AIN[(aenv >> 10) & 7], AIN[(aenv >> 7) & 7], AIN[(aenv >> 4) & 7]);
        if (bitsu(aenv, 16, 2) == 3) {
            uint32_t mode = (bitsu(aenv, 20, 2) << 1) | bitsu(aenv, 18, 1);
            const char *op = mode & 1 ? "==" : ">";
            /* like TEV_FS: the non-alpha compare modes compare the colour inputs ca / cb */
            if ((mode >> 1) == 3) out(b, "alpha = clamp(ad + ((u8v(aa) %s u8v(ab)) ? ac : 0.0), 0.0, 1.0);\n", op);
            else if ((mode >> 1) == 0) out(b, "alpha = clamp(ad + ((u8v(ca.r) %s u8v(cb.r)) ? ac : 0.0), 0.0, 1.0);\n", op);
            else if ((mode >> 1) == 1)
                out(b, "alpha = clamp(ad + ((((u8v(ca.g) << 8u) | u8v(ca.r)) %s ((u8v(cb.g) << 8u) | u8v(cb.r))) ? ac : 0.0), 0.0, 1.0);\n", op);
            else
                out(b, "alpha = clamp(ad + ((((u8v(ca.b) << 16u) | (u8v(ca.g) << 8u) | u8v(ca.r)) %s ((u8v(cb.b) << 16u) | (u8v(cb.g) << 8u) | u8v(cb.r))) ? ac : 0.0), 0.0, 1.0);\n", op);
        } else {
            out(b, "alpha = ad %c (aa * (1.0 - ac) + ab * ac);", bitsu(aenv, 18, 1) ? '-' : '+');
            bias_scale(b, "float", "alpha", aenv);
            out(b, "\n");
        }
        uint32_t dc = (cenv >> 22) & 3, da = (aenv >> 22) & 3;
        out(b, "R%u.rgb = col; R%u.a = alpha;\n", dc, da);
    }
    out(b, "vec4 res = clamp(R0, 0.0, 1.0);\n");
    uint32_t f0 = (acr >> 16) & 7, f1 = (acr >> 19) & 7, logic = (acr >> 22) & 3;
    bool always = (logic == 0 && f0 == 7 && f1 == 7) || (logic == 1 && (f0 == 7 || f1 == 7));
    if (!always) {
        out(b, "uint a8 = u8v(res.a);");
        alpha_test_expr(b, "t0", f0, "(tev.params.y & 0xFFu)");
        alpha_test_expr(b, "t1", f1, "((tev.params.y >> 8u) & 0xFFu)");
        static const char *lg[4] = {"t0 && t1", "t0 || t1", "t0 != t1", "t0 == t1"};
        out(b, "if (!(%s)) discard;\n", lg[logic]);
    }
    out(b, "vec3 rgb = res.rgb;\n");
    uint32_t fogsel = (uint32_t)t->fog_color[3], fsel = fogsel & 7;
    if (fsel >= 2) {
        out(b, "float zs = clamp(gl_FragCoord.z * 16777215.0, 0.0, 16777215.0);");
        if ((fogsel >> 3) == 0) out(b, "float ze = tev.fog.x / max(tev.fog.z - floor(zs / exp2(tev.fog.w)), 1.0);");
        else out(b, "float ze = tev.fog.x * zs / 16777216.0;");
        out(b, "float f = clamp(ze - tev.fog.y, 0.0, 1.0);");
        if (fsel == 4) out(b, "f = 1.0 - exp2(-8.0 * f);");
        if (fsel == 5) out(b, "f = 1.0 - exp2(-8.0 * f * f);");
        if (fsel == 6) out(b, "f = exp2(-8.0 * (1.0 - f));");
        if (fsel == 7) out(b, "f = exp2(-8.0 * (1.0 - f) * (1.0 - f));");
        out(b, "rgb = mix(rgb, tev.fog_color.rgb, f);\n");
    }
    out(b, "o_color = vec4(rgb, res.a);\n}\n");
    return B.len < cap ? B.len : 0;
}
