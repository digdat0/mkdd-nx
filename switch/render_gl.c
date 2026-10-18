/* GLES 3.2 renderer: replays each decoded GX frame on an emulated embedded framebuffer (EFB).
 * C/GL port of MKDD-Recomp's mkdd-bevy/src/gxrender.rs and its WGSL shaders.
 *
 * The GameCube draws into an on-chip framebuffer (colour + 24-bit depth) and gets results out of
 * it only through EFB copies (BP 0x52): to a texture in RAM that later draws sample, or to the
 * external framebuffer (XFB) for display, optionally clearing the EFB afterwards. Here:
 *  - batches are drawn in submission order into the EFB FBO, each with its GX blend / Z state,
 *    scissor rectangle and per-pixel TEV (TEV_FS);
 *  - at each copy the EFB rectangle is converted into a texture kept per destination address
 *    (COPY_FS); a copy with the clear bit then clears the rectangle;
 *  - the XFB copy is what the screen shows (BLIT_FS), letterboxed to 4:3.
 *
 * Orientation: the EFB is rendered with Y flipped, so every render target's texel row 0 is the
 * top of the image (as in the Rust/wgpu version and as decoded GX textures are uploaded). Scissor
 * rectangles, copy rectangles and texture coordinates then all use top-left coordinates. */
#include "render_gl.h"
#include <GLES3/gl32.h>
#include <EGL/egl.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../runtime/gx.h"
#include "../runtime/plat.h"
#include "../runtime/rt.h"
#include "hdtex.h"
#include "tev_gen.h"
#include "xf_vs.h"

/* Fallback if the XF vertex shader does not compile: vertices transformed on the CPU only. */
static const char *PRE_VS =
    "#version 320 es\n"
    "layout(location=0) in vec4 a_pos;\n"
    "layout(location=1) in vec4 a_c0;\n"
    "layout(location=2) in vec4 a_c1;\n"
    "layout(location=3) in vec4 a_uv01;\n"
    "layout(location=4) in vec4 a_uv23;\n"
    "layout(location=5) in vec4 a_uv45;\n"
    "layout(location=6) in vec4 a_uv67;\n"
    "layout(location=0) out vec4 v_c0; layout(location=1) out vec4 v_c1; layout(location=2) out vec4 v_uv01;"
    "layout(location=3) out vec4 v_uv23; layout(location=4) out vec4 v_uv45; layout(location=5) out vec4 v_uv67;\n"
    "void main() {\n"
    "    gl_Position = vec4(a_pos.x, -a_pos.y, 2.0 * a_pos.z - a_pos.w, a_pos.w);\n"
    "    v_c0 = a_c0; v_c1 = a_c1; v_uv01 = a_uv01; v_uv23 = a_uv23; v_uv45 = a_uv45; v_uv67 = a_uv67;\n"
    "}\n";

#define TEV_UNIFORM_SIZE 432
#define MAX_COPIES 256

static const char *TEV_FS =
    "#version 320 es\n"
    "precision highp float;\n"
    "precision highp int;\n"
    "layout(std140) uniform Tev {\n"
    "    uvec4 stages[16];\n"
    "    vec4 regs[4];\n"
    "    vec4 konst[4];\n"
    "    uvec4 params;   // x stages, y alpha compare, z swap tables\n"
    "    vec4 fog;       // A, C, B magnitude, B shift\n"
    "    vec4 fog_color; // rgb, w = fsel | proj << 3\n"
    "} tev;\n"
    "uniform sampler2D tex0; uniform sampler2D tex1; uniform sampler2D tex2; uniform sampler2D tex3;\n"
    "uniform sampler2D tex4; uniform sampler2D tex5; uniform sampler2D tex6; uniform sampler2D tex7;\n"
    "layout(location=0) in vec4 v_c0; layout(location=1) in vec4 v_c1; layout(location=2) in vec4 v_uv01;"
    "layout(location=3) in vec4 v_uv23; layout(location=4) in vec4 v_uv45; layout(location=5) in vec4 v_uv67;\n"
    "out vec4 o_color;\n"
    "vec4 sample_map(uint m, vec2 uv, vec2 gx, vec2 gy) {\n"
    "    switch (m) {\n"
    "    case 0u: return textureGrad(tex0, uv, gx, gy);\n"
    "    case 1u: return textureGrad(tex1, uv, gx, gy);\n"
    "    case 2u: return textureGrad(tex2, uv, gx, gy);\n"
    "    case 3u: return textureGrad(tex3, uv, gx, gy);\n"
    "    case 4u: return textureGrad(tex4, uv, gx, gy);\n"
    "    case 5u: return textureGrad(tex5, uv, gx, gy);\n"
    "    case 6u: return textureGrad(tex6, uv, gx, gy);\n"
    "    default: return textureGrad(tex7, uv, gx, gy);\n"
    "    }\n"
    "}\n"
    "vec4 swap_tbl(vec4 c, uint tbl) {\n"
    "    uint p = (tev.params.z >> (8u * tbl)) & 0xFFu;\n"
    "    return vec4(c[p & 3u], c[(p >> 2u) & 3u], c[(p >> 4u) & 3u], c[(p >> 6u) & 3u]);\n"
    "}\n"
    "vec4 konst_sel(uint sel) {\n"
    "    if (sel < 8u) return vec4(float(8u - sel) / 8.0);\n"
    "    if (sel >= 0x0Cu && sel <= 0x0Fu) return tev.konst[sel - 0x0Cu];\n"
    "    if (sel >= 0x10u) { vec4 k = tev.konst[sel & 3u]; return vec4(k[(sel - 0x10u) / 4u]); }\n"
    "    return vec4(1.0);\n"
    "}\n"
    "float bias_scale(float r, uint env) {\n"
    "    uint bias = (env >> 16u) & 3u;\n"
    "    if (bias == 1u) r += 0.5;\n"
    "    if (bias == 2u) r -= 0.5;\n"
    "    uint scale = (env >> 20u) & 3u;\n"
    "    if (scale == 1u) r *= 2.0;\n"
    "    if (scale == 2u) r *= 4.0;\n"
    "    if (scale == 3u) r *= 0.5;\n"
    "    if (((env >> 19u) & 1u) == 1u) return clamp(r, 0.0, 1.0);\n"
    "    return clamp(r, -1024.0 / 255.0, 1023.0 / 255.0);\n"
    "}\n"
    "uint u8v(float x) { return uint(round(clamp(x, 0.0, 1.0) * 255.0)); }\n"
    "bool cmp_scalar(vec3 a, vec3 b, uint mode) {\n"
    "    uint ar = u8v(a.r), ag = u8v(a.g), ab = u8v(a.b), br = u8v(b.r), bg = u8v(b.g), bb = u8v(b.b);\n"
    "    uint av, bv;\n"
    "    if ((mode >> 1u) == 0u) { av = ar; bv = br; }\n"
    "    else if ((mode >> 1u) == 1u) { av = (ag << 8u) | ar; bv = (bg << 8u) | br; }\n"
    "    else { av = (ab << 16u) | (ag << 8u) | ar; bv = (bb << 16u) | (bg << 8u) | br; }\n"
    "    return (mode & 1u) == 0u ? av > bv : av == bv;\n"
    "}\n"
    "bool alpha_test(uint a, uint func, uint refv) {\n"
    "    switch (func) {\n"
    "    case 0u: return false;\n"
    "    case 1u: return a < refv;\n"
    "    case 2u: return a == refv;\n"
    "    case 3u: return a <= refv;\n"
    "    case 4u: return a > refv;\n"
    "    case 5u: return a != refv;\n"
    "    case 6u: return a >= refv;\n"
    "    default: return true;\n"
    "    }\n"
    "}\n"
    "void main() {\n"
    "    vec4 uvp[4] = vec4[4](v_uv01, v_uv23, v_uv45, v_uv67);\n"
    "    vec4 ddx[4] = vec4[4](dFdx(v_uv01), dFdx(v_uv23), dFdx(v_uv45), dFdx(v_uv67));\n"
    "    vec4 ddy[4] = vec4[4](dFdy(v_uv01), dFdy(v_uv23), dFdy(v_uv45), dFdy(v_uv67));\n"
    "    vec4 regs[4] = tev.regs;\n"
    "    uint n = min(tev.params.x, 16u);\n"
    "    for (uint s = 0u; s < n; s++) {\n"
    "        uvec4 st = tev.stages[s];\n"
    "        uint cenv = st.x, aenv = st.y, order = st.z;\n"
    "        uint map = order & 7u, tc = (order >> 4u) & 7u, en = (order >> 7u) & 1u, chan = (order >> 8u) & 7u;\n"
    "        vec4 tx = vec4(1.0);\n"
    "        if (en == 1u) {\n"
    "            uint h = tc >> 1u;\n"
    "            if ((tc & 1u) == 0u) tx = sample_map(map, uvp[h].xy, ddx[h].xy, ddy[h].xy);\n"
    "            else tx = sample_map(map, uvp[h].zw, ddx[h].zw, ddy[h].zw);\n"
    "        }\n"
    "        tx = swap_tbl(tx, (aenv >> 2u) & 3u);\n"
    "        vec4 ras = vec4(0.0);\n"
    "        if (chan == 0u) ras = v_c0;\n"
    "        if (chan == 1u) ras = v_c1;\n"
    "        ras = swap_tbl(ras, aenv & 3u);\n"
    "        vec4 kc = konst_sel(st.w & 31u);\n"
    "        float ka = konst_sel((st.w >> 8u) & 31u).a;\n"
    "        vec3 cin[16] = vec3[16](regs[0].rgb, vec3(regs[0].a), regs[1].rgb, vec3(regs[1].a),\n"
    "                               regs[2].rgb, vec3(regs[2].a), regs[3].rgb, vec3(regs[3].a),\n"
    "                               tx.rgb, vec3(tx.a), ras.rgb, vec3(ras.a), vec3(1.0), vec3(0.5), kc.rgb, vec3(0.0));\n"
    "        float ain[8] = float[8](regs[0].a, regs[1].a, regs[2].a, regs[3].a, tx.a, ras.a, ka, 0.0);\n"
    "        vec3 ca = clamp(cin[(cenv >> 12u) & 15u], 0.0, 1.0);\n"
    "        vec3 cb = clamp(cin[(cenv >> 8u) & 15u], 0.0, 1.0);\n"
    "        vec3 cc = clamp(cin[(cenv >> 4u) & 15u], 0.0, 1.0);\n"
    "        vec3 cd = cin[cenv & 15u];\n"
    "        vec3 col;\n"
    "        if (((cenv >> 16u) & 3u) == 3u) {\n"
    "            uint mode = (((cenv >> 20u) & 3u) << 1u) | ((cenv >> 18u) & 1u);\n"
    "            if ((mode >> 1u) == 3u) {\n"
    "                bool eq = (mode & 1u) == 1u;\n"
    "                vec3 r = cd;\n"
    "                for (int k = 0; k < 3; k++) {\n"
    "                    bool hit = eq ? u8v(ca[k]) == u8v(cb[k]) : u8v(ca[k]) > u8v(cb[k]);\n"
    "                    if (hit) r[k] += cc[k];\n"
    "                }\n"
    "                col = r;\n"
    "            } else {\n"
    "                col = cd + (cmp_scalar(ca, cb, mode) ? cc : vec3(0.0));\n"
    "            }\n"
    "            col = clamp(col, 0.0, 1.0);\n"
    "        } else {\n"
    "            vec3 lerp_ = ca * (vec3(1.0) - cc) + cb * cc;\n"
    "            vec3 r = ((cenv >> 18u) & 1u) == 1u ? cd - lerp_ : cd + lerp_;\n"
    "            col = vec3(bias_scale(r.r, cenv), bias_scale(r.g, cenv), bias_scale(r.b, cenv));\n"
    "        }\n"
    "        float aa = clamp(ain[(aenv >> 13u) & 7u], 0.0, 1.0);\n"
    "        float ab = clamp(ain[(aenv >> 10u) & 7u], 0.0, 1.0);\n"
    "        float ac = clamp(ain[(aenv >> 7u) & 7u], 0.0, 1.0);\n"
    "        float ad = ain[(aenv >> 4u) & 7u];\n"
    "        float alpha;\n"
    "        if (((aenv >> 16u) & 3u) == 3u) {\n"
    "            uint mode = (((aenv >> 20u) & 3u) << 1u) | ((aenv >> 18u) & 1u);\n"
    "            bool hit;\n"
    "            if ((mode >> 1u) == 3u) hit = (mode & 1u) == 1u ? u8v(aa) == u8v(ab) : u8v(aa) > u8v(ab);\n"
    "            else hit = cmp_scalar(ca, cb, mode);\n"
    "            alpha = clamp(ad + (hit ? ac : 0.0), 0.0, 1.0);\n"
    "        } else {\n"
    "            float lerp_ = aa * (1.0 - ac) + ab * ac;\n"
    "            alpha = bias_scale(((aenv >> 18u) & 1u) == 1u ? ad - lerp_ : ad + lerp_, aenv);\n"
    "        }\n"
    "        uint dc = (cenv >> 22u) & 3u, da = (aenv >> 22u) & 3u;\n"
    "        regs[dc] = vec4(col, regs[dc].a);\n"
    "        regs[da].a = alpha;\n"
    "    }\n"
    "    vec4 res = clamp(regs[0], 0.0, 1.0);\n"
    "    uint acr = tev.params.y;\n"
    "    uint a8 = u8v(res.a);\n"
    "    bool t0 = alpha_test(a8, (acr >> 16u) & 7u, acr & 0xFFu);\n"
    "    bool t1 = alpha_test(a8, (acr >> 19u) & 7u, (acr >> 8u) & 0xFFu);\n"
    "    uint logic = (acr >> 22u) & 3u;\n"
    "    bool pass_ = logic == 0u ? (t0 && t1) : logic == 1u ? (t0 || t1) : logic == 2u ? (t0 != t1) : (t0 == t1);\n"
    "    if (!pass_) discard;\n"
    "    vec3 rgb = res.rgb;\n"
    "    uint fogsel = uint(tev.fog_color.w);\n"
    "    uint fsel = fogsel & 7u;\n"
    "    if (fsel >= 2u) {\n"
    "        float zs = clamp(gl_FragCoord.z * 16777215.0, 0.0, 16777215.0);\n"
    "        float ze;\n"
    "        if ((fogsel >> 3u) == 0u) ze = tev.fog.x / max(tev.fog.z - floor(zs / exp2(tev.fog.w)), 1.0);\n"
    "        else ze = tev.fog.x * zs / 16777216.0;\n"
    "        float f = clamp(ze - tev.fog.y, 0.0, 1.0);\n"
    "        if (fsel == 4u) f = 1.0 - exp2(-8.0 * f);\n"
    "        if (fsel == 5u) f = 1.0 - exp2(-8.0 * f * f);\n"
    "        if (fsel == 6u) f = exp2(-8.0 * (1.0 - f));\n"
    "        if (fsel == 7u) f = exp2(-8.0 * (1.0 - f) * (1.0 - f));\n"
    "        rgb = mix(rgb, tev.fog_color.rgb, f);\n"
    "    }\n"
    "    o_color = vec4(rgb, res.a);\n"
    "}\n";

/* Full-screen triangle; v_uv has v = 0 at the top of the screen / target. */
static const char *FULL_VS =
    "#version 320 es\n"
    "out vec2 v_uv;\n"
    "void main() {\n"
    "    vec2 uv = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
    "    gl_Position = vec4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.0, 1.0);\n"
    "    v_uv = uv;\n"
    "}\n";

static const char *COPY_FS =
    "#version 320 es\n"
    "precision highp float;\n"
    "precision highp int;\n"
    "uniform uvec4 u_src;     // x, y, w, h of the source rectangle in console EFB pixels\n"
    "uniform uvec4 u_mode;    // x format, y box filter, z depth source, w flags (intensity, pixfmt<<8, gamma<<16)\n"
    "uniform vec4 u_vfilter;  // rows above / at / below, w = clamp flags\n"
    "uniform int u_scale;\n"
    "uniform sampler2D efb_color;\n"
    "uniform highp sampler2D efb_depth;\n"
    "out vec4 o_color;\n"
    "float q(float v, float levels) { return round(clamp(v, 0.0, 1.0) * levels) / levels; }\n"
    "vec4 efb_texel(ivec2 p) {\n"
    "    ivec2 dims = textureSize(efb_color, 0);\n"
    "    ivec2 c = clamp(p, ivec2(0), dims - ivec2(1));\n"
    "    if (u_mode.z == 1u) {\n"
    "        uint z = uint(round(clamp(texelFetch(efb_depth, c, 0).r, 0.0, 1.0) * 16777215.0));\n"
    "        return vec4(float(z >> 16u), float((z >> 8u) & 255u), float(z & 255u), 255.0) / 255.0;\n"
    "    }\n"
    "    vec4 t = texelFetch(efb_color, c, 0);\n"
    "    uint pixfmt = (u_mode.w >> 8u) & 7u;\n"
    "    if (pixfmt == 0u) t.a = 1.0;\n"
    "    if (pixfmt == 1u) t = vec4(q(t.r, 63.0), q(t.g, 63.0), q(t.b, 63.0), q(t.a, 63.0));\n"
    "    if (pixfmt == 2u) t = vec4(q(t.r, 31.0), q(t.g, 63.0), q(t.b, 31.0), 1.0);\n"
    "    return t;\n"
    "}\n"
    "int copy_row(int y) {\n"
    "    uint flags = uint(u_vfilter.w);\n"
    "    int r = y;\n"
    "    if ((flags & 1u) != 0u) r = max(r, int(u_src.y) * u_scale);\n"
    "    if ((flags & 2u) != 0u) r = min(r, int(u_src.y + u_src.w) * u_scale - 1);\n"
    "    return r;\n"
    "}\n"
    "vec4 copy_tap(ivec2 s, int dy) {\n"
    "    if (u_mode.y == 1u) {\n"
    "        int r0 = copy_row(s.y + dy), r1 = copy_row(s.y + 1 + dy);\n"
    "        return (efb_texel(ivec2(s.x, r0)) + efb_texel(ivec2(s.x + 1, r0)) + efb_texel(ivec2(s.x, r1)) + efb_texel(ivec2(s.x + 1, r1))) * 0.25;\n"
    "    }\n"
    "    return efb_texel(ivec2(s.x, copy_row(s.y + dy)));\n"
    "}\n"
    "void main() {\n"
    "    ivec2 p = ivec2(floor(gl_FragCoord.xy));\n"
    "    ivec2 base = ivec2(u_src.xy) * u_scale;\n"
    "    int step_ = u_mode.y == 1u ? 2 : 1;\n"
    "    ivec2 s = base + step_ * p;\n"
    "    vec4 t;\n"
    "    if (u_mode.z == 1u) t = efb_texel(s);\n"
    "    else {\n"
    "        t = copy_tap(s, -u_scale) * u_vfilter.x + copy_tap(s, 0) * u_vfilter.y + copy_tap(s, u_scale) * u_vfilter.z;\n"
    "        t = clamp(t, 0.0, 1.0);\n"
    "    }\n"
    "    uint gidx = (u_mode.w >> 16u) & 3u;\n"
    "    if (gidx == 1u) t.rgb = pow(t.rgb, vec3(1.0 / 1.7));\n"
    "    if (gidx >= 2u) t.rgb = pow(t.rgb, vec3(1.0 / 2.2));\n"
    "    uint fmt = u_mode.x;\n"
    "    if ((u_mode.w & 1u) == 1u && fmt <= 3u) {\n"
    "        float y = clamp(dot(t.rgb, vec3(0.257, 0.504, 0.098)) + 16.0 / 255.0, 0.0, 1.0);\n"
    "        t = vec4(y, y, y, t.a);\n"
    "    }\n"
    "    switch (fmt) {\n"
    "    case 0u: { float r = q(t.r, 15.0); o_color = vec4(r); break; }\n"
    "    case 1u: case 8u: o_color = vec4(t.r); break;\n"
    "    case 2u: { float r = q(t.r, 15.0); o_color = vec4(r, r, r, q(t.a, 15.0)); break; }\n"
    "    case 3u: o_color = vec4(t.r, t.r, t.r, t.a); break;\n"
    "    case 4u: o_color = vec4(q(t.r, 31.0), q(t.g, 63.0), q(t.b, 31.0), 1.0); break;\n"
    "    case 5u:\n"
    "        if (t.a >= 1.0) o_color = vec4(q(t.r, 31.0), q(t.g, 31.0), q(t.b, 31.0), 1.0);\n"
    "        else o_color = vec4(q(t.r, 15.0), q(t.g, 15.0), q(t.b, 15.0), q(t.a, 7.0));\n"
    "        break;\n"
    "    case 7u: o_color = vec4(t.a); break;\n"
    "    case 9u: o_color = vec4(t.g); break;\n"
    "    case 10u: o_color = vec4(t.b); break;\n"
    "    case 11u: o_color = vec4(t.g, t.g, t.g, t.r); break;\n"
    "    case 12u: o_color = vec4(t.b, t.b, t.b, t.g); break;\n"
    "    default: o_color = t;\n"
    "    }\n"
    "}\n";

static const char *BLIT_FS =
    "#version 320 es\n"
    "precision highp float;\n"
    "uniform sampler2D xfb;\n"
    "uniform vec3 u_adj;\n" /* colour option: gamma exponent, saturation, contrast (1,1,1 = as rendered) */
    "in vec2 v_uv;\n"
    "out vec4 o_color;\n"
    "void main() {\n"
    /* shrinking (the picture is larger than the screen area): average 4 taps; enlarging: 1 */
    "    vec2 fw = fwidth(v_uv), qd = fw * 0.25;\n"
    "    vec3 c;\n"
    "    if (max(fw.x * float(textureSize(xfb, 0).x), fw.y * float(textureSize(xfb, 0).y)) <= 1.0) c = texture(xfb, v_uv).rgb;\n"
    "    else c = (texture(xfb, v_uv + vec2(-qd.x, -qd.y)).rgb + texture(xfb, v_uv + vec2(qd.x, -qd.y)).rgb\n"
    "            + texture(xfb, v_uv + vec2(-qd.x, qd.y)).rgb + texture(xfb, v_uv + vec2(qd.x, qd.y)).rgb) * 0.25;\n"
    "    c = pow(c, vec3(u_adj.x));\n"
    "    c = mix(vec3(dot(c, vec3(0.2126, 0.7152, 0.0722))), c, u_adj.y);\n"
    "    c = (c - 0.5) * u_adj.z + 0.5;\n"
    "    o_color = vec4(clamp(c, 0.0, 1.0), 1.0);\n"
    "}\n";

/* Movie frames (THP: the picture as Y, U, V I8 planes, colour at half size), redrawn at twice the
 * plane size before the game's draw samples them: MOVIE_DEBLOCK_FS smooths the JPEG 8x8 block
 * edges in flat areas, MOVIE_LUMA_FS scales brightness 2x with Catmull-Rom plus light sharpening,
 * MOVIE_CHROMA_FS brings each colour plane to the brightness plane's size guided by brightness (a
 * sample counts less the more its brightness differs), so colour edges follow the picture's edges
 * instead of bleeding. Kept cheap for the Switch GPU: 5 taps for brightness, colour at 1x from 9
 * (the first version, colour at 2x from 16 with exp(), took the intro down to ~31 fps). */
static const char *MOVIE_DEBLOCK_FS =
    "#version 320 es\n"
    "precision highp float;\n"
    "precision highp int;\n"
    "uniform highp sampler2D u_y;\n"
    "out vec4 o_color;\n"
    "float Y(ivec2 p) { return texelFetch(u_y, clamp(p, ivec2(0), textureSize(u_y, 0) - 1), 0).r; }\n"
    /* correction for texel p from the block edge across direction d (x: p's coordinate along d) */
    "float edge(ivec2 p, ivec2 d, int x, int n) {\n"
    "    int r = x & 7, b;\n"
    "    if (r >= 6) b = x - r + 8; else if (r <= 1) b = x - r; else return 0.0;\n"
    "    if (b <= 0 || b >= n) return 0.0;\n"
    "    ivec2 q = p + d * (b - x);\n"
    "    float p1 = Y(q - 2 * d), p0 = Y(q - d), q0 = Y(q), q1 = Y(q + d), t = 10.0 / 255.0;\n"
    "    if (abs(q0 - p0) >= t || abs(p1 - p0) >= t * 0.5 || abs(q1 - q0) >= t * 0.5) return 0.0;\n"
    "    float dd = (q0 - p0) * 0.375;\n"
    "    int k = x - b;\n"
    "    return k == -1 ? dd : k == 0 ? -dd : k == -2 ? dd / 3.0 : -dd / 3.0;\n"
    "}\n"
    "void main() {\n"
    "    ivec2 p = ivec2(gl_FragCoord.xy), n = textureSize(u_y, 0);\n"
    "    float v = Y(p) + edge(p, ivec2(1, 0), p.x, n.x) + edge(p, ivec2(0, 1), p.y, n.y);\n"
    "    o_color = vec4(clamp(v, 0.0, 1.0));\n"
    "}\n";

static const char *MOVIE_LUMA_FS =
    "#version 320 es\n"
    "precision highp float;\n"
    "uniform highp sampler2D u_y;\n" /* deblocked brightness, linear filtering */
    "out vec4 o_color;\n"
    /* Catmull-Rom in 5 bilinear taps (the corner taps' weights are tiny and dropped) */
    "void main() {\n"
    "    vec2 n = vec2(textureSize(u_y, 0)), inv = 1.0 / n;\n"
    "    vec2 s = gl_FragCoord.xy * 0.5, t1 = floor(s - 0.5) + 0.5, f = s - t1;\n"
    "    vec2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));\n"
    "    vec2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);\n"
    "    vec2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));\n"
    "    vec2 w3 = f * f * (-0.5 + 0.5 * f);\n"
    "    vec2 w12 = w1 + w2, t12 = (t1 + w2 / w12) * inv, t0 = (t1 - 1.0) * inv, t3 = (t1 + 2.0) * inv;\n"
    "    float c = texture(u_y, vec2(t12.x, t0.y)).r * w12.x * w0.y\n"
    "            + texture(u_y, vec2(t0.x, t12.y)).r * w0.x * w12.y\n"
    "            + texture(u_y, t12).r * w12.x * w12.y\n"
    "            + texture(u_y, vec2(t3.x, t12.y)).r * w3.x * w12.y\n"
    "            + texture(u_y, vec2(t12.x, t3.y)).r * w12.x * w3.y;\n"
    "    c /= w12.x * w0.y + w0.x * w12.y + w12.x * w12.y + w3.x * w12.y + w12.x * w3.y;\n"
    "    float l = texture(u_y, s * inv).r;\n"
    "    o_color = vec4(clamp(c + 0.35 * (c - l), 0.0, 1.0));\n"
    "}\n";

static const char *MOVIE_CHROMA_FS =
    "#version 320 es\n"
    "precision highp float;\n"
    "precision highp int;\n"
    "uniform highp sampler2D u_c;\n" /* U or V plane (half size) */
    "uniform highp sampler2D u_g;\n" /* deblocked brightness (this pass's size), linear filtering */
    "out vec4 o_color;\n"
    "void main() {\n"
    "    ivec2 n = textureSize(u_c, 0);\n"
    "    vec2 nf = vec2(n), uv = gl_FragCoord.xy / vec2(textureSize(u_g, 0));\n"
    "    float g0 = texelFetch(u_g, ivec2(gl_FragCoord.xy), 0).r;\n"
    "    vec2 s = uv * nf - 0.5;\n"
    "    ivec2 b = ivec2(floor(s + 0.5));\n"
    "    float sum = 0.0, wsum = 0.0;\n"
    "    for (int y = -1; y <= 1; y++)\n"
    "        for (int x = -1; x <= 1; x++) {\n"
    "            ivec2 t = clamp(b + ivec2(x, y), ivec2(0), n - 1);\n"
    "            vec2 d = abs(vec2(t) - s);\n"
    "            float ws = max(0.0, 1.0 - d.x / 1.5) * max(0.0, 1.0 - d.y / 1.5);\n"
    "            float dg = (g0 - texture(u_g, (vec2(t) + 0.5) / nf).r) * 16.0;\n" /* ~1/0.06 */
    "            float w = ws * (1.0 / (1.0 + dg * dg) + 0.002);\n"
    "            sum += w * texelFetch(u_c, t, 0).r;\n"
    "            wsum += w;\n"
    "        }\n"
    "    o_color = vec4(sum / max(wsum, 1e-5));\n"
    "}\n";

typedef struct { GLuint tex, fbo; uint32_t w, h; } Target;
/* Streamed per-frame data: NSLOT buffers of each kind used round-robin, each slot guarded by a
 * fence, written through unsynchronised maps (re-specifying a buffer with glBufferData every
 * frame makes switch-mesa wait for the GPU). */
#define NSLOT 3
typedef struct { GLuint buf; size_t cap; } Stream;
/* xb: the frame's XF records and matrices, read by the vertex shader as a texture buffer (unit 8) */
typedef struct { Stream vb, ib, ub, xb; GLuint vao, xtex; GLsync fence; uint32_t layout; } Slot;
/* chans: texels stored as 1 (I: R8), 2 (I, A: RG8, see gx_texture_decode_compact) or 4 (RGBA8) bytes */
/* hd: 0 game texture, 1 HD replacement loading (hdtex.h), 2 showing the HD replacement (BC7 with
 * mipmaps, its own GL texture: deleted on eviction, not pooled) */
/* movie: plane of a movie frame (GxTexture.movie); enh_set / enh_gen: its redrawn version in M.set
 * (valid while that set's gen still matches) */
typedef struct { GLuint tex; uint8_t wrap_s, wrap_t, chans, subst, hd, movie, enh_set; bool linear; uint64_t used, key; uint32_t w, h, hd_bytes, enh_gen; } GlTexture;
/* Evicted GL textures kept for reuse by a new texture of the same size (glTexSubImage2D): movies
 * and render-to-texture effects make a new texture every frame, and deleting a nouveau texture
 * the GPU used recently waits for the GPU. */
#define TEX_POOL 48
#define HD_BC7 0x8E8C /* GL_COMPRESSED_RGBA_BPTC_UNORM_EXT */
#define TEX_BC1 0x83F1 /* GL_COMPRESSED_RGBA_S3TC_DXT1_EXT: the game's CMPR textures as they are */
#define CHANS_BC1 8    /* GlTexture / PoolTex chans value of a BC1 texture */
typedef struct { GLuint tex; uint32_t w, h; uint64_t freed; uint8_t chans; } PoolTex;

static struct {
    GLuint prog_tev, prog_copy, prog_blit;
    GLint copy_src, copy_mode, copy_vfilter, copy_scale, blit_adj;
    GLuint empty_vao;
    Slot slots[NSLOT];
    unsigned slot;
    GLuint tev_vs;
    /* Separable programs: the XF vertex stage is one program shared by every TEV pipeline, so
     * each specialised shader compiles only its fragment stage (a whole program per material
     * re-linked the big vertex shader: ~0.6 s each on Mesa). tev programs are then pipelines. */
    bool sso;
    GLuint vs_prog, cur_pipe;
    GLint vs_squeeze;
    float squeeze;
    bool hud43;
    unsigned draws_since_flush;
    uint64_t gpu_wait_ns; /* waiting for the GPU to finish the frame NSLOT frames back (render_take_gpu_wait) */
    U64Map progs;          /* tev_shader_key -> specialised program (0 = failed) */
    unsigned nprogs, compiles_this_frame;
    uint64_t compile_ns;
    GLuint cur_tex[8], cur_smp[8], cur_prog;
    int cur_unit;
    uint32_t cur_z, cur_blend, cur_cull;
    bool pipe_valid;
    GLint ubo_align;
    size_t ubo_stride;
    GLuint samplers[3][3][2];
    GLuint samplers_mip[3][3]; /* trilinear, for HD replacements */
    bool bptc, hd_on, hd_flush;
    bool s3tc;                 /* BC1 sampling: CMPR textures stay compressed on the GPU */
    float aniso_max, aniso;    /* GL_EXT_texture_filter_anisotropic limit (0: none) and the current level */
    uint64_t hd_bytes; /* BC7 data of the HD textures on the GPU */
    GLuint white;
    Target efb_color;
    GLuint efb_depth;
    uint32_t efb_w, efb_h;
    Target xfb;
    bool have_xfb;
    U64Map rts;            /* address -> index into rt */
    Target rt[512];
    unsigned nrt;
    U64Map textures;       /* key -> index into tex */
    GlTexture *tex;
    unsigned ntex, cap_tex;
    PoolTex pool[TEX_POOL];
    unsigned npool;
    uint64_t tick, shown;
    uint8_t *ubo_data;
    size_t ubo_cap;
    uint8_t *rgba;
    size_t rgba_cap;
    uint32_t stat_batches, stat_verts;
} R;

static void target_init(Target *t, uint32_t w, uint32_t h);
static void state_reset(void);
static void use_program(GLuint p);
static void use_tev(GLuint p);

static GLuint compile(GLenum type, const char *src, const char *name) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[4096];
        glGetShaderInfoLog(s, sizeof log, NULL, log);
        rt_log("render: %s shader failed: %s", name, log);
        return 0;
    }
    return s;
}

static GLuint link_program(const char *vs, const char *fs, const char *name) {
    GLuint v = compile(GL_VERTEX_SHADER, vs, name), f = compile(GL_FRAGMENT_SHADER, fs, name);
    if (!v || !f) return 0;
    GLuint p = glCreateProgram();
    glAttachShader(p, v);
    glAttachShader(p, f);
    glLinkProgram(p);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[4096];
        glGetProgramInfoLog(p, sizeof log, NULL, log);
        rt_log("render: %s program link failed: %s", name, log);
        return 0;
    }
    glDeleteShader(v);
    glDeleteShader(f);
    return p;
}

static GLuint separable(GLenum type, const char *src, const char *name) {
    GLuint p = glCreateShaderProgramv(type, 1, &src);
    GLint ok = 0;
    if (p) glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[4096] = "";
        if (p) glGetProgramInfoLog(p, sizeof log, NULL, log);
        rt_log("render: %s separable program failed: %s", name, log);
        if (p) glDeleteProgram(p);
        return 0;
    }
    return p;
}

/* Program with the shared TEV vertex shader, the Tev block on binding 0 and texN on unit N
 * (with separable programs: a pipeline of the shared vertex program and this fragment one). */
static GLuint tev_link(const char *fs_src, const char *name) {
    if (R.sso) {
        GLuint p = separable(GL_FRAGMENT_SHADER, fs_src, name);
        if (!p) return 0;
        glUniformBlockBinding(p, glGetUniformBlockIndex(p, "Tev"), 0);
        for (int i = 0; i < 8; i++) {
            char nm[8] = {'t', 'e', 'x', (char)('0' + i), 0};
            GLint loc = glGetUniformLocation(p, nm);
            if (loc >= 0) glProgramUniform1i(p, loc, i);
        }
        GLuint pp = 0;
        glGenProgramPipelines(1, &pp);
        glUseProgramStages(pp, GL_VERTEX_SHADER_BIT, R.vs_prog);
        glUseProgramStages(pp, GL_FRAGMENT_SHADER_BIT, p);
        return pp;
    }
    GLuint f = compile(GL_FRAGMENT_SHADER, fs_src, name);
    if (!f) return 0;
    GLuint p = glCreateProgram();
    glAttachShader(p, R.tev_vs);
    glAttachShader(p, f);
    glLinkProgram(p);
    glDeleteShader(f);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[4096];
        glGetProgramInfoLog(p, sizeof log, NULL, log);
        rt_log("render: %s program link failed: %s", name, log);
        glDeleteProgram(p);
        return 0;
    }
    glUseProgram(p);
    R.cur_prog = p;
    glUniformBlockBinding(p, glGetUniformBlockIndex(p, "Tev"), 0);
    for (int i = 0; i < 8; i++) {
        char nm[8] = {'t', 'e', 'x', (char)('0' + i), 0};
        GLint loc = glGetUniformLocation(p, nm);
        if (loc >= 0) glUniform1i(loc, i);
    }
    return p;
}

/* Shader cache on the SD card: the TEV state of every specialised program compiled so far, so
 * the next boot compiles them up front (~20 ms each) instead of mid-race. */
#define CACHE_MAGIC 0x4D4B5456u /* "MKTV" */
static FILE *s_cache;

static GLuint tev_compile(const TevState *t, uint64_t key) {
    static char src[65536];
    uint64_t t0 = plat_now_ns();
    GLuint p = tev_gen_fs(t, src, sizeof src) ? tev_link(src, "tev-specialised") : 0;
    R.compile_ns += plat_now_ns() - t0;
    u64map_put(&R.progs, key, p);
    if (p) R.nprogs++;
    return p;
}

void render_shader_cache(const char *path, void (*progress)(unsigned done, unsigned total)) {
    size_t size = 0;
    uint8_t *d = plat_read_file(path, &size);
    uint32_t hdr[2] = {CACHE_MAGIC, (uint32_t)sizeof(TevState)};
    bool valid = d && size >= 8 && !memcmp(d, hdr, 8);
    unsigned total = valid ? (unsigned)((size - 8) / sizeof(TevState)) : 0;
    /* Each program also draws one empty triangle: Mesa translates shaders for the GPU at their
     * first draw, which would otherwise still stall the first race on every boot. */
    Target warm = {0};
    GLuint ubo = 0;
    if (total) {
        target_init(&warm, 4, 4);
        glViewport(0, 0, 4, 4);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_BLEND);
        glDisable(GL_SCISSOR_TEST);
        static const uint8_t zero[TEV_UNIFORM_SIZE];
        glGenBuffers(1, &ubo);
        glBindBuffer(GL_UNIFORM_BUFFER, ubo);
        glBufferData(GL_UNIFORM_BUFFER, sizeof zero, zero, GL_STATIC_DRAW);
        glBindBufferBase(GL_UNIFORM_BUFFER, 0, ubo);
        glBindVertexArray(R.empty_vao);
        state_reset();
    }
    uint64_t t0 = plat_now_ns();
    for (unsigned i = 0; i < total; i++) {
        TevState t;
        memcpy(&t, d + 8 + (size_t)i * sizeof t, sizeof t);
        uint64_t key = tev_shader_key(&t);
        if (!u64map_get(&R.progs, key, NULL)) {
            GLuint p = tev_compile(&t, key);
            if (p) {
                use_tev(p);
                glDrawArrays(GL_TRIANGLES, 0, 3);
                if (i % 16 == 15) glFlush();
            }
        }
        if (progress && (i % 8 == 0 || i + 1 == total)) progress(i + 1, total);
    }
    if (total) {
        glFinish();
        glBindVertexArray(0);
        glBindBufferBase(GL_UNIFORM_BUFFER, 0, 0);
        glDeleteBuffers(1, &ubo);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDeleteFramebuffers(1, &warm.fbo);
        glDeleteTextures(1, &warm.tex);
        use_program(0);
        if (R.sso) glBindProgramPipeline(0);
        state_reset();
        rt_log("render: %u cached shaders ready in %.1f s", total, (double)(plat_now_ns() - t0) / 1e9);
    }
    free(d);
    s_cache = fopen(path, valid ? "ab" : "wb");
    if (s_cache && !valid) { fwrite(hdr, 8, 1, s_cache); fflush(s_cache); }
    rt_log("render: shader cache %s: %u entries, %u programs", path, total, R.nprogs);
}

/* Specialised program for a batch, compiled on first use (at most MAX_COMPILES per frame: each
 * is a Mesa compile on the CPU); the uber shader draws the batch until then. */
#define MAX_COMPILES 2
static GLuint tev_program(const TevState *t) {
    uint64_t key = tev_shader_key(t), v;
    if (u64map_get(&R.progs, key, &v)) return v ? (GLuint)v : R.prog_tev;
    if (R.compiles_this_frame >= MAX_COMPILES) return R.prog_tev;
    R.compiles_this_frame++;
    GLuint p = tev_compile(t, key);
    if (p && s_cache) { fwrite(t, sizeof *t, 1, s_cache); fflush(s_cache); }
    return p ? p : R.prog_tev;
}

static void use_program(GLuint p) {
    if (R.cur_prog != p) { glUseProgram(p); R.cur_prog = p; }
}
/* A TEV program: a pipeline (glUseProgram(0) lets the bound pipeline draw) or a program. */
static void use_tev(GLuint p) {
    if (!R.sso) { use_program(p); return; }
    use_program(0);
    if (R.cur_pipe != p) { glBindProgramPipeline(p); R.cur_pipe = p; }
}

static void *stream_map(Stream *st, GLenum target, size_t size) {
    glBindBuffer(target, st->buf);
    if (size > st->cap) {
        st->cap = size + size / 2;
        glBufferData(target, (GLsizeiptr)st->cap, NULL, GL_DYNAMIC_DRAW);
    }
    if (!size) return NULL;
    return glMapBufferRange(target, 0, (GLsizeiptr)size, GL_MAP_WRITE_BIT | GL_MAP_UNSYNCHRONIZED_BIT | GL_MAP_INVALIDATE_RANGE_BIT);
}
static void stream_put(Stream *st, GLenum target, const void *data, size_t size) {
    void *m = stream_map(st, target, size);
    if (m) {
        memcpy(m, data, size);
        glUnmapBuffer(target);
    }
}

/* Vertex attribute pointers of the bound slot VAO for a vertex layout (runtime/gx.h): the full
 * 108-byte vertex (CPU transform), or the compact one with k texgen slots. A batch's indices count
 * in its layout's stride, so batches of the same layout share one set of pointers. */
static void slot_layout(Slot *sl, uint32_t layout) {
    if (sl->layout == layout) return;
    sl->layout = layout;
    glBindBuffer(GL_ARRAY_BUFFER, sl->vb.buf);
    GLsizei st = (GLsizei)gx_layout_stride(layout);
#define PTR(i, n, type, norm, off) do { glEnableVertexAttribArray(i); glVertexAttribPointer(i, n, type, norm, st, (const void *)(uintptr_t)(off)); } while (0)
    if (layout == GX_LAYOUT_FULL) {
        PTR(0, 4, GL_FLOAT, GL_FALSE, offsetof(GpuVertex, pos));
        PTR(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, offsetof(GpuVertex, color0));
        PTR(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, offsetof(GpuVertex, color1));
        for (int p = 0; p < 4; p++) PTR(3 + p, 4, GL_FLOAT, GL_FALSE, offsetof(GpuVertex, uv[2 * p]));
        PTR(7, 3, GL_FLOAT, GL_FALSE, offsetof(GpuVertex, nrm));
        glEnableVertexAttribArray(8);
        glVertexAttribIPointer(8, 2, GL_UNSIGNED_INT, st, (const void *)(uintptr_t)offsetof(GpuVertex, rec));
    } else {
        PTR(0, 3, GL_FLOAT, GL_FALSE, 0);
        PTR(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, 20);
        PTR(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, 24);
        for (uint32_t p = 0; p < 4; p++) {
            int comps = 2 * (int)layout - 4 * (int)p;
            if (comps <= 0) { glDisableVertexAttribArray(3 + p); glVertexAttrib4f(3 + p, 0, 0, 0, 1); }
            else PTR(3 + p, comps > 4 ? 4 : comps, GL_FLOAT, GL_FALSE, 36 + 16 * p);
        }
        PTR(7, 3, GL_SHORT, GL_FALSE, 28);
        glEnableVertexAttribArray(8);
        glVertexAttribIPointer(8, 2, GL_UNSIGNED_INT, st, (const void *)(uintptr_t)12);
    }
#undef PTR
}

static void target_init(Target *t, uint32_t w, uint32_t h) {
    if (!t->tex) glGenTextures(1, &t->tex);
    if (!t->fbo) glGenFramebuffers(1, &t->fbo);
    glBindTexture(GL_TEXTURE_2D, t->tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)w, (GLsizei)h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindFramebuffer(GL_FRAMEBUFFER, t->fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t->tex, 0);
    t->w = w;
    t->h = h;
}

bool render_init(void) {
    R.vs_prog = separable(GL_VERTEX_SHADER, XF_VS, "xf-vs");
    if (!R.vs_prog) {
        rt_log("render: XF vertex shader failed, transforming vertices on the CPU");
        g_gx_cpu_xform_forced = true;
        R.vs_prog = separable(GL_VERTEX_SHADER, PRE_VS, "pre-vs");
    }
    R.sso = R.vs_prog != 0;
    if (R.sso) R.vs_squeeze = glGetUniformLocation(R.vs_prog, "u_squeeze");
    else {
        rt_log("render: no separable programs, linking whole programs");
        R.tev_vs = compile(GL_VERTEX_SHADER, g_gx_cpu_xform_forced ? PRE_VS : XF_VS, "xf-vs");
        if (!R.tev_vs) {
            g_gx_cpu_xform_forced = true;
            R.tev_vs = compile(GL_VERTEX_SHADER, PRE_VS, "pre-vs");
        }
        if (!R.tev_vs) return false;
    }
    R.prog_tev = tev_link(TEV_FS, "tev");
    R.prog_copy = link_program(FULL_VS, COPY_FS, "copy");
    R.prog_blit = link_program(FULL_VS, BLIT_FS, "blit");
    if (!R.prog_tev || !R.prog_copy || !R.prog_blit) return false;
    glUseProgram(R.prog_copy);
    glUniform1i(glGetUniformLocation(R.prog_copy, "efb_color"), 0);
    glUniform1i(glGetUniformLocation(R.prog_copy, "efb_depth"), 1);
    R.copy_src = glGetUniformLocation(R.prog_copy, "u_src");
    R.copy_mode = glGetUniformLocation(R.prog_copy, "u_mode");
    R.copy_vfilter = glGetUniformLocation(R.prog_copy, "u_vfilter");
    R.copy_scale = glGetUniformLocation(R.prog_copy, "u_scale");
    glUseProgram(R.prog_blit);
    glUniform1i(glGetUniformLocation(R.prog_blit, "xfb"), 0);
    R.blit_adj = glGetUniformLocation(R.prog_blit, "u_adj");

    glGetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &R.ubo_align);
    if (R.ubo_align < 16) R.ubo_align = 16;
    R.ubo_stride = ((TEV_UNIFORM_SIZE + (size_t)R.ubo_align - 1) / (size_t)R.ubo_align) * (size_t)R.ubo_align;

    glGenVertexArrays(1, &R.empty_vao);
    for (int k = 0; k < NSLOT; k++) {
        Slot *sl = &R.slots[k];
        glGenVertexArrays(1, &sl->vao);
        glGenBuffers(1, &sl->vb.buf);
        glGenBuffers(1, &sl->ib.buf);
        glGenBuffers(1, &sl->ub.buf);
        glGenBuffers(1, &sl->xb.buf);
        glGenTextures(1, &sl->xtex);
        glBindVertexArray(sl->vao);
        glBindBuffer(GL_ARRAY_BUFFER, sl->vb.buf);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, sl->ib.buf);
        sl->layout = ~0u; /* attribute pointers are set per vertex layout (slot_layout) */
        glBindVertexArray(0);
    }

    static const GLenum wrap[3] = {GL_CLAMP_TO_EDGE, GL_REPEAT, GL_MIRRORED_REPEAT};
    for (int s = 0; s < 3; s++)
        for (int t = 0; t < 3; t++)
            for (int l = 0; l < 2; l++) {
                GLuint smp;
                glGenSamplers(1, &smp);
                glSamplerParameteri(smp, GL_TEXTURE_WRAP_S, (GLint)wrap[s]);
                glSamplerParameteri(smp, GL_TEXTURE_WRAP_T, (GLint)wrap[t]);
                glSamplerParameteri(smp, GL_TEXTURE_MIN_FILTER, l ? GL_LINEAR : GL_NEAREST);
                glSamplerParameteri(smp, GL_TEXTURE_MAG_FILTER, l ? GL_LINEAR : GL_NEAREST);
                R.samplers[s][t][l] = smp;
            }
    for (int s = 0; s < 3; s++)
        for (int t = 0; t < 3; t++) {
            GLuint smp;
            glGenSamplers(1, &smp);
            glSamplerParameteri(smp, GL_TEXTURE_WRAP_S, (GLint)wrap[s]);
            glSamplerParameteri(smp, GL_TEXTURE_WRAP_T, (GLint)wrap[t]);
            glSamplerParameteri(smp, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
            glSamplerParameteri(smp, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            R.samplers_mip[s][t] = smp;
        }
    /* HD replacements are BC7 (GL_EXT_texture_compression_bptc on GLES) */
    GLint next = 0, nfmt = 0;
    glGetIntegerv(GL_NUM_EXTENSIONS, &next);
    for (GLint i = 0; i < next; i++) {
        const char *e = (const char *)glGetStringi(GL_EXTENSIONS, (GLuint)i);
        if (e && (!strcmp(e, "GL_EXT_texture_compression_bptc") || !strcmp(e, "GL_ARB_texture_compression_bptc"))) R.bptc = true;
    }
    glGetIntegerv(GL_NUM_COMPRESSED_TEXTURE_FORMATS, &nfmt);
    if (nfmt > 0 && nfmt < 1024) {
        GLint fmts[1024];
        glGetIntegerv(GL_COMPRESSED_TEXTURE_FORMATS, fmts);
        for (GLint i = 0; i < nfmt; i++) { R.bptc |= fmts[i] == HD_BC7; R.s3tc |= fmts[i] == TEX_BC1; }
    }
    for (GLint i = 0; i < next; i++) {
        const char *e = (const char *)glGetStringi(GL_EXTENSIONS, (GLuint)i);
        if (e && !strcmp(e, "GL_EXT_texture_filter_anisotropic")) glGetFloatv(0x84FF /* MAX_TEXTURE_MAX_ANISOTROPY */, &R.aniso_max);
    }
    R.aniso = 1.0f;
    rt_log("render: anisotropic filtering up to %.0fx", (double)R.aniso_max);
    rt_log("render: BC7 textures %s (%d compressed formats); CMPR textures %s", R.bptc ? "supported" : "NOT supported: HD textures off", nfmt,
           R.s3tc ? "kept compressed (BC1)" : "decoded (no BC1)");
    static const uint8_t white[4] = {255, 255, 255, 255};
    glGenTextures(1, &R.white);
    glBindTexture(GL_TEXTURE_2D, R.white);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, white);
    return true;
}

static void ensure_efb(uint32_t w, uint32_t h) {
    if (R.efb_w == w && R.efb_h == h) return;
    target_init(&R.efb_color, w, h);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    if (!R.efb_depth) glGenTextures(1, &R.efb_depth);
    glBindTexture(GL_TEXTURE_2D, R.efb_depth);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT32F, (GLsizei)w, (GLsizei)h, 0, GL_DEPTH_COMPONENT, GL_FLOAT, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, R.efb_color.fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, R.efb_depth, 0);
    GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (st != GL_FRAMEBUFFER_COMPLETE) rt_log("render: EFB framebuffer incomplete (%#x)", st);
    /* Power-on state: black, depth at the far plane. */
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_TRUE);
    glClearColor(0, 0, 0, 1);
    glClearDepthf(1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    R.efb_w = w;
    R.efb_h = h;
}

static Target *rt_for(uint32_t addr, uint32_t w, uint32_t h) {
    uint64_t idx;
    if (u64map_get(&R.rts, addr, &idx)) {
        Target *t = &R.rt[idx];
        if (t->w != w || t->h != h) target_init(t, w, h);
        return t;
    }
    if (R.nrt >= sizeof R.rt / sizeof *R.rt) return NULL;
    Target *t = &R.rt[R.nrt];
    memset(t, 0, sizeof *t);
    target_init(t, w, h);
    u64map_put(&R.rts, addr, R.nrt++);
    return t;
}

/* Texture / sampler binds, skipped when the unit already has them. */
static void unit_bind(unsigned unit, GLuint tex, GLuint smp) {
    if (R.cur_tex[unit] != tex) {
        if (R.cur_unit != (int)unit) { glActiveTexture(GL_TEXTURE0 + unit); R.cur_unit = (int)unit; }
        glBindTexture(GL_TEXTURE_2D, tex);
        R.cur_tex[unit] = tex;
    }
    if (R.cur_smp[unit] != smp) { glBindSampler(unit, smp); R.cur_smp[unit] = smp; }
}
/* Forget tracked GL state (after code that binds textures / programs directly). */
static void state_reset(void) {
    memset(R.cur_tex, 0xFF, sizeof R.cur_tex);
    memset(R.cur_smp, 0xFF, sizeof R.cur_smp);
    R.cur_unit = -1;
    R.cur_prog = R.cur_pipe = ~0u;
    R.squeeze = -1;
    R.pipe_valid = false;
    glDisable(GL_CULL_FACE);
    R.cur_cull = 0;
}

static unsigned (*s_subst)(unsigned id);
void render_set_subst(unsigned (*texture)(unsigned id)) { s_subst = texture; }

/* Redrawn movie planes (see MOVIE_DEBLOCK_FS): a few sets in turn, so a set is not redrawn while
 * the GPU may still be drawing the frame that used it. */
#define MOVIE_SETS 4
static struct {
    GLuint prog_deblock, prog_luma, prog_chroma;
    int state;          /* 0 not tried, 1 ready, -1 shaders failed (movies drawn as before) */
    GLuint fbo, yd;     /* yd: deblocked brightness at the plane size */
    uint32_t w, h;
    struct { GLuint y, u, v; uint32_t gen; } set[MOVIE_SETS];
    unsigned next;
    uint32_t gen;
    uint64_t seen_frames, seen_tick;
} M;

/* GL texture + sampler for a texture slot key (decoded texture, EFB copy, or white). */
static void bind_slot(unsigned unit, uint64_t key) {
    if (!key) {
        unit_bind(unit, R.white, R.samplers[0][0][0]);
        return;
    }
    if ((key & 0xFFFF000000000000ull) == RT_TAG) {
        uint32_t mode = (uint32_t)((key >> 32) & 0xFFFF);
        uint64_t idx;
        unsigned ws = mode & 3, wt = (mode >> 2) & 3;
        if (ws > 2) ws = 0;
        if (wt > 2) wt = 0;
        unit_bind(unit, u64map_get(&R.rts, (uint32_t)key, &idx) ? R.rt[idx].tex : R.white, R.samplers[ws][wt][(mode >> 4) & 1]);
        return;
    }
    uint64_t idx;
    GlTexture *gt = NULL;
    if (u64map_get(&R.textures, key, &idx)) gt = &R.tex[idx];
    else {
        GxTexture *t = gx_texture_acquire(key);
        if (!t) {
            unit_bind(unit, R.white, R.samplers[0][0][0]);
            return;
        }
        size_t need = (size_t)t->width * t->height * 4;
        if (need > R.rgba_cap) { R.rgba_cap = need; R.rgba = realloc(R.rgba, need); }
        /* CMPR (most of MKDD's textures) stays compressed: an eighth of the memory and bandwidth of
         * RGBA8, the same texels (the GPU's BC1 decode follows the same rules) */
        unsigned chans = R.s3tc && gx_texture_cmpr_bc1(t, R.rgba) ? CHANS_BC1 : gx_texture_decode_compact(t, R.rgba);
        if (!chans) {
            gx_texture_decode(t, R.rgba);
            chans = 4;
        }
        if (R.ntex == R.cap_tex) { R.cap_tex = R.cap_tex ? R.cap_tex * 2 : 256; R.tex = realloc(R.tex, R.cap_tex * sizeof *R.tex); }
        gt = &R.tex[R.ntex];
        /* Reuse a pooled texture of this size that the GPU is done with (freed 3+ frames ago). */
        int reuse = -1;
        for (unsigned i = 0; i < R.npool; i++)
            if (R.pool[i].w == t->width && R.pool[i].h == t->height && R.pool[i].chans == chans && R.pool[i].freed + 3 <= R.tick) { reuse = (int)i; break; }
        if (reuse >= 0) {
            gt->tex = R.pool[reuse].tex;
            R.pool[reuse] = R.pool[--R.npool];
        } else {
            glGenTextures(1, &gt->tex);
        }
        if (R.cur_unit != (int)unit) { glActiveTexture(GL_TEXTURE0 + unit); R.cur_unit = (int)unit; }
        glBindTexture(GL_TEXTURE_2D, gt->tex);
        R.cur_tex[unit] = gt->tex;
        GLenum pf = chans == 1 ? GL_RED : chans == 2 ? GL_RG : GL_RGBA;
        glPixelStorei(GL_UNPACK_ALIGNMENT, chans == 4 ? 4 : 1);
        GLsizei bc1_size = (GLsizei)(t->width / 4 * (t->height / 4) * 8);
        if (chans == CHANS_BC1) {
            if (reuse >= 0) glCompressedTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, (GLsizei)t->width, (GLsizei)t->height, TEX_BC1, bc1_size, R.rgba);
            else glCompressedTexImage2D(GL_TEXTURE_2D, 0, TEX_BC1, (GLsizei)t->width, (GLsizei)t->height, 0, bc1_size, R.rgba);
        } else if (reuse >= 0)
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, (GLsizei)t->width, (GLsizei)t->height, pf, GL_UNSIGNED_BYTE, R.rgba);
        else {
            GLint ifmt = chans == 1 ? GL_R8 : chans == 2 ? GL_RG8 : GL_RGBA8;
            glTexImage2D(GL_TEXTURE_2D, 0, ifmt, (GLsizei)t->width, (GLsizei)t->height, 0, pf, GL_UNSIGNED_BYTE, R.rgba);
            if (chans < 4) { /* back to (I, I, I, I) / (I, I, I, A); pooled textures keep it (pool keyed by chans) */
                const GLint sw[4] = {GL_RED, GL_RED, GL_RED, chans == 1 ? GL_RED : GL_GREEN};
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, sw[0]);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_G, sw[1]);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, sw[2]);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_A, sw[3]);
            }
        }
        gt->chans = (uint8_t)chans;
        gt->w = t->width;
        gt->h = t->height;
        gt->wrap_s = t->wrap_s > 2 ? 0 : t->wrap_s;
        gt->wrap_t = t->wrap_t > 2 ? 0 : t->wrap_t;
        gt->linear = t->linear;
        gt->subst = t->subst;
        gt->movie = t->movie;
        gt->enh_set = 0;
        gt->enh_gen = 0;
        gt->key = key;
        gt->hd = 0;
        if (R.hd_on && !t->subst) {
            int hd = hdtex_find(t->fmt, t->width, t->height, t->raw, t->raw_len);
            if (hd >= 0 && hdtex_request(hd, key)) gt->hd = 1;
        }
        u64map_put(&R.textures, key, R.ntex++);
        gx_texture_release(t);
    }
    gt->used = R.tick;
    if (gt->movie && gt->enh_gen && M.set[gt->enh_set].gen == gt->enh_gen) {
        GLuint e = gt->movie == 1 ? M.set[gt->enh_set].y : gt->movie == 2 ? M.set[gt->enh_set].u : M.set[gt->enh_set].v;
        unit_bind(unit, e, R.samplers[gt->wrap_s][gt->wrap_t][1]);
        return;
    }
    GLuint sub = gt->subst && s_subst ? s_subst(gt->subst) : 0;
    if (sub) { unit_bind(unit, sub, R.samplers[0][0][1]); return; } /* clamped, linear */
    unit_bind(unit, gt->tex, gt->hd == 2 ? R.samplers_mip[gt->wrap_s][gt->wrap_t] : R.samplers[gt->wrap_s][gt->wrap_t][gt->linear]);
}

/* Drop GL textures not used for `age` frames into the reuse pool (oldest pool entries are
 * deleted when it is full). */
static void pool_add(GLuint tex, uint32_t w, uint32_t h, uint8_t chans) {
    if (R.npool == TEX_POOL) {
        unsigned old = 0;
        for (unsigned j = 1; j < R.npool; j++) if (R.pool[j].freed < R.pool[old].freed) old = j;
        glDeleteTextures(1, &R.pool[old].tex);
        R.pool[old] = R.pool[--R.npool];
    }
    R.pool[R.npool++] = (PoolTex){tex, w, h, R.tick, chans};
}

static void evict_textures(uint64_t age) {
    uint64_t cutoff = R.tick > age ? R.tick - age : 0;
    unsigned w = 0;
    u64map_clear(&R.textures);
    for (unsigned i = 0; i < R.ntex; i++) {
        if (R.tex[i].used < cutoff) {
            if (R.tex[i].hd == 2) { glDeleteTextures(1, &R.tex[i].tex); R.hd_bytes -= R.tex[i].hd_bytes; }
            else pool_add(R.tex[i].tex, R.tex[i].w, R.tex[i].h, R.tex[i].chans);
            continue;
        }
        R.tex[w] = R.tex[i];
        u64map_put(&R.textures, R.tex[w].key, w);
        w++;
    }
    R.ntex = w;
}

/* Swap finished HD loads in for their game textures (whose GL texture goes back to the pool). The
 * upload is spread over frames: at most HD_UPLOAD_BYTES and HD_UPLOAD_NS of uploads a frame, in
 * strips of 4-pixel block rows (a 4096-pixel level alone is 16 MB). The time cap matters most:
 * nouveau allocates a staging buffer and flushes for every glCompressedTexSubImage2D, ~0.5 ms
 * each whatever its size, so a frame finishing many small textures (a dozen levels each) at the
 * character select dropped to 48 fps. The game texture stays on screen until every level is in.
 * A load whose texture was evicted meanwhile is dropped. */
#define HD_UPLOAD_BYTES (2u << 20)
#define HD_UPLOAD_NS 2000000ull
static struct {
    uint8_t *data;       /* the load being uploaded, or NULL */
    uint64_t tag;
    GLuint tex;
    unsigned w, h, levels, level, row; /* next: block row `row` of `level` */
    size_t off;          /* start of `level` in data */
} s_up;

static void hd_upload_drop(bool delete_tex) {
    if (delete_tex && s_up.tex) {
        glDeleteTextures(1, &s_up.tex);
        if (R.cur_tex[0] == s_up.tex) R.cur_tex[0] = ~0u;
    }
    free(s_up.data);
    memset(&s_up, 0, sizeof s_up);
}

static void hd_upload(void) {
    size_t budget = HD_UPLOAD_BYTES;
    uint64_t ti, t0 = plat_now_ns();
    while (budget && plat_now_ns() - t0 < HD_UPLOAD_NS) {
        if (!s_up.data) {
            int idx;
            uint8_t *data;
            if (!hdtex_take(&idx, &s_up.tag, &data)) return;
            GlTexture *gt = u64map_get(&R.textures, s_up.tag, &ti) ? &R.tex[ti] : NULL;
            if (!gt || gt->hd != 1 || !data || !R.hd_on) {
                if (gt && gt->hd == 1) gt->hd = 0;
                free(data);
                continue;
            }
            s_up.data = data;
            hdtex_info(idx, &s_up.w, &s_up.h, &s_up.levels);
            glGenTextures(1, &s_up.tex);
            if (R.cur_unit != 0) { glActiveTexture(GL_TEXTURE0); R.cur_unit = 0; }
            glBindTexture(GL_TEXTURE_2D, s_up.tex);
            R.cur_tex[0] = s_up.tex;
            glTexStorage2D(GL_TEXTURE_2D, (GLsizei)s_up.levels, HD_BC7, (GLsizei)s_up.w, (GLsizei)s_up.h);
        }
        GlTexture *gt = u64map_get(&R.textures, s_up.tag, &ti) ? &R.tex[ti] : NULL;
        if (!gt || gt->hd != 1 || !R.hd_on) { /* evicted or switched off mid-upload */
            if (gt && gt->hd == 1) gt->hd = 0;
            hd_upload_drop(true);
            continue;
        }
        if (R.cur_unit != 0) { glActiveTexture(GL_TEXTURE0); R.cur_unit = 0; }
        if (R.cur_tex[0] != s_up.tex) { glBindTexture(GL_TEXTURE_2D, s_up.tex); R.cur_tex[0] = s_up.tex; }
        unsigned l = s_up.level, lw = s_up.w >> l ? s_up.w >> l : 1, lh = s_up.h >> l ? s_up.h >> l : 1;
        unsigned brows = (lh + 3) / 4;
        size_t rowbytes = (size_t)((lw + 3) / 4) * 16;
        unsigned n = (unsigned)(budget / rowbytes);
        if (n < 1) n = 1;
        if (n > brows - s_up.row) n = brows - s_up.row;
        unsigned y = s_up.row * 4, hgt = n * 4 < lh - y ? n * 4 : lh - y;
        while (glGetError() != GL_NO_ERROR) {} /* errors of this strip only (and the storage) */
        glCompressedTexSubImage2D(GL_TEXTURE_2D, (GLint)l, 0, (GLint)y, (GLsizei)lw, (GLsizei)hgt, HD_BC7,
                                  (GLsizei)(n * rowbytes), s_up.data + s_up.off + (size_t)s_up.row * rowbytes);
        GLenum err = glGetError();
        if (err != GL_NO_ERROR) {
            rt_log("render: HD texture upload failed (GL error %#x): HD textures off", err);
            gt->hd = 0;
            R.hd_on = false;
            hd_upload_drop(true);
            return;
        }
        budget = n * rowbytes >= budget ? 0 : budget - n * rowbytes;
        s_up.row += n;
        if (s_up.row < brows) continue;
        s_up.off += (size_t)brows * rowbytes;
        s_up.row = 0;
        if (++s_up.level < s_up.levels) continue;
        pool_add(gt->tex, gt->w, gt->h, gt->chans);
        gt->tex = s_up.tex;
        gt->hd = 2;
        gt->hd_bytes = (uint32_t)s_up.off;
        R.hd_bytes += s_up.off;
        hd_upload_drop(false);
    }
}

static GLenum compare(uint32_t f) {
    static const GLenum m[8] = {GL_NEVER, GL_LESS, GL_EQUAL, GL_LEQUAL, GL_GREATER, GL_NOTEQUAL, GL_GEQUAL, GL_ALWAYS};
    return m[f & 7];
}

static GLenum blend_factor(uint32_t f, bool src_side) {
    switch (f & 7) {
    case 0: return GL_ZERO;
    case 1: return GL_ONE;
    case 2: return src_side ? GL_DST_COLOR : GL_SRC_COLOR;
    case 3: return src_side ? GL_ONE_MINUS_DST_COLOR : GL_ONE_MINUS_SRC_COLOR;
    case 4: return GL_SRC_ALPHA;
    case 5: return GL_ONE_MINUS_SRC_ALPHA;
    case 6: return GL_DST_ALPHA;
    default: return GL_ONE_MINUS_DST_ALPHA;
    }
}

/* Fixed-function state for GX Z mode (BP 0x40) and blend mode (BP 0x41). */
/* GX culling (gen mode, hardware encoding 1 = back, 2 = front): GX front faces wind clockwise in
 * GX clip space (y up, measured on MKDD's culled draws). The vertex shader negates y (the EFB is
 * drawn upside down), which reverses the winding GL sees: front faces are counter-clockwise. */
static void apply_cull(uint32_t mode) {
    if (mode == R.cur_cull) return;
    R.cur_cull = mode;
    if (mode == 1 || mode == 2) {
        glEnable(GL_CULL_FACE);
        glFrontFace(GL_CCW);
        glCullFace(mode == 1 ? GL_BACK : GL_FRONT);
    } else {
        glDisable(GL_CULL_FACE);
    }
}

static void apply_pipe(uint32_t zmode, uint32_t blend) {
    if (R.pipe_valid && R.cur_z == zmode && R.cur_blend == blend) return;
    R.pipe_valid = true;
    R.cur_z = zmode;
    R.cur_blend = blend;
    if (zmode & 1) {
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(compare((zmode >> 1) & 7));
        glDepthMask((zmode >> 4) & 1 ? GL_TRUE : GL_FALSE);
    } else {
        glDisable(GL_DEPTH_TEST);
        glDepthMask(GL_FALSE);
    }
    bool color = (blend >> 3) & 1, alpha = (blend >> 4) & 1;
    bool enabled = blend & 1, logic = (blend >> 1) & 1, subtract = (blend >> 11) & 1;
    uint32_t logic_op = (blend >> 12) & 15;
    if (subtract) {
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE);
        glBlendEquation(GL_FUNC_REVERSE_SUBTRACT);
    } else if (enabled) {
        glEnable(GL_BLEND);
        glBlendFunc(blend_factor((blend >> 8) & 7, true), blend_factor((blend >> 5) & 7, false));
        glBlendEquation(GL_FUNC_ADD);
    } else if (logic && logic_op == 0) {
        glEnable(GL_BLEND);
        glBlendFunc(GL_ZERO, GL_ZERO);
        glBlendEquation(GL_FUNC_ADD);
    } else if (logic && logic_op == 5) {
        glDisable(GL_BLEND);
        color = alpha = false;
    } else if (logic && logic_op == 15) {
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE);
        glBlendEquation(GL_MAX);
    } else {
        glDisable(GL_BLEND);
    }
    glColorMask(color, color, color, alpha);
}

/* A decoded copy: where it reads, where it goes, and how. */
typedef struct {
    uint32_t at;
    bool xfb, clear;
    uint32_t src[4];
    uint32_t dst_w, dst_h, addr;
    uint32_t regs[8];
    uint32_t mode[4];
    float vfilter[4];
} CopyOp;

static inline uint32_t bitsu(uint32_t v, uint32_t lo, uint32_t n) { return (v >> lo) & ((1u << n) - 1); }

static void decode_copy(const EfbCopy *e, uint32_t scale, CopyOp *c) {
    uint32_t cmd = e->bp52;
    memset(c, 0, sizeof *c);
    c->at = e->at_batch;
    c->xfb = cmd & (1u << 14);
    c->clear = (cmd >> 11) & 1;
    c->src[0] = bitsu(e->src, 0, 10);
    c->src[1] = bitsu(e->src, 10, 10);
    c->src[2] = bitsu(e->size, 0, 10) + 1;
    c->src[3] = bitsu(e->size, 10, 10) + 1;
    bool half = !c->xfb && (cmd >> 9) & 1;
    uint32_t sw = c->src[2] * scale, sh = c->src[3] * scale;
    c->dst_w = half ? (sw / 2 ? sw / 2 : 1) : sw;
    c->dst_h = half ? (sh / 2 ? sh / 2 : 1) : sh;
    c->addr = gx_phys(e->dst);
    memcpy(c->regs, e->regs, sizeof c->regs);
    uint32_t fmt = c->xfb ? 6 : bitsu(cmd, 4, 3) | bitsu(cmd, 3, 1) << 3;
    uint32_t pixfmt = c->regs[0] & 7;
    uint32_t gamma = c->xfb ? (cmd >> 7) & 3 : 0;
    c->mode[0] = fmt;
    c->mode[1] = half;
    c->mode[2] = pixfmt == 3;
    c->mode[3] = ((cmd >> 15) & 1) | (pixfmt << 8) | (gamma << 16);
    /* Vertical copy filter (BP 0x53/0x54, 7 x 6-bit coefficients, sum normally 64). */
    uint32_t f0 = c->regs[6], f1 = c->regs[7];
#define W(r, i) (float)(((r) >> (6 * (i))) & 0x3F)
    if (((f0 | f1) & 0xFFFFFF) == 0) { c->vfilter[0] = 0; c->vfilter[1] = 1; c->vfilter[2] = 0; }
    else {
        c->vfilter[0] = (W(f0, 0) + W(f0, 1) + W(f0, 2)) / 64.0f;
        c->vfilter[1] = W(f0, 3) / 64.0f;
        c->vfilter[2] = (W(f1, 0) + W(f1, 1) + W(f1, 2)) / 64.0f;
    }
#undef W
    c->vfilter[3] = (float)(cmd & 3);
}

static bool movie_ready(void) {
    if (M.state) return M.state > 0;
    M.prog_deblock = link_program(FULL_VS, MOVIE_DEBLOCK_FS, "movie-deblock");
    M.prog_luma = link_program(FULL_VS, MOVIE_LUMA_FS, "movie-luma");
    M.prog_chroma = link_program(FULL_VS, MOVIE_CHROMA_FS, "movie-chroma");
    if (!M.prog_deblock || !M.prog_luma || !M.prog_chroma) { M.state = -1; return false; }
    glUseProgram(M.prog_deblock);
    glUniform1i(glGetUniformLocation(M.prog_deblock, "u_y"), 0);
    glUseProgram(M.prog_luma);
    glUniform1i(glGetUniformLocation(M.prog_luma, "u_y"), 0);
    glUseProgram(M.prog_chroma);
    glUniform1i(glGetUniformLocation(M.prog_chroma, "u_c"), 0);
    glUniform1i(glGetUniformLocation(M.prog_chroma, "u_g"), 1);
    R.cur_prog = ~0u;
    glGenFramebuffers(1, &M.fbo);
    M.state = 1;
    rt_log("render: movie frames are redrawn (deblocked, brightness 2x, colour guided by brightness)");
    return true;
}

static void movie_tex(GLuint *t, uint32_t w, uint32_t h) {
    if (*t) glDeleteTextures(1, t);
    glGenTextures(1, t);
    if (R.cur_unit != 0) { glActiveTexture(GL_TEXTURE0); R.cur_unit = 0; }
    glBindTexture(GL_TEXTURE_2D, *t);
    R.cur_tex[0] = *t;
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_R8, (GLsizei)w, (GLsizei)h);
    /* sampled as I8, like the game's planes */
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_G, GL_RED);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, GL_RED);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_A, GL_RED);
}

static void movie_pass(GLuint prog, GLuint dst, uint32_t w, uint32_t h) {
    use_program(prog);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, dst, 0);
    glViewport(0, 0, (GLsizei)w, (GLsizei)h);
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

/* Redraw one movie frame's planes (y: w x h, u and v: half that) into the next set. */
static void movie_enhance(GlTexture *y, GlTexture *u, GlTexture *v) {
    uint32_t w = y->w, h = y->h, W = w * 2, H = h * 2;
    if (M.w != w || M.h != h) {
        movie_tex(&M.yd, w, h);
        for (unsigned s = 0; s < MOVIE_SETS; s++) {
            movie_tex(&M.set[s].y, W, H);
            movie_tex(&M.set[s].u, w, h);
            movie_tex(&M.set[s].v, w, h);
            M.set[s].gen = 0;
        }
        M.w = w;
        M.h = h;
    }
    unsigned s = M.next++ % MOVIE_SETS;
    glBindFramebuffer(GL_FRAMEBUFFER, M.fbo);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    R.pipe_valid = false;
    apply_cull(0);
    glBindVertexArray(R.empty_vao);
    unit_bind(1, 0, 0); /* yd is the next target: not bound for sampling meanwhile */
    unit_bind(0, y->tex, R.samplers[0][0][0]);
    movie_pass(M.prog_deblock, M.yd, w, h);
    unit_bind(0, M.yd, R.samplers[0][0][1]); /* linear: the 5-tap Catmull-Rom relies on it */
    movie_pass(M.prog_luma, M.set[s].y, W, H);
    unit_bind(1, M.yd, R.samplers[0][0][1]);
    unit_bind(0, u->tex, R.samplers[0][0][0]);
    movie_pass(M.prog_chroma, M.set[s].u, w, h);
    unit_bind(0, v->tex, R.samplers[0][0][0]);
    movie_pass(M.prog_chroma, M.set[s].v, w, h);
    M.set[s].gen = ++M.gen;
    y->enh_set = u->enh_set = v->enh_set = (uint8_t)s;
    y->enh_gen = u->enh_gen = v->enh_gen = M.gen;
}

/* While a movie plays: redraw the planes of each draw that samples a movie frame's Y, U and V. */
static void movie_prepass(const Frame *f) {
    if (g_thp_frames != M.seen_frames) { M.seen_frames = g_thp_frames; M.seen_tick = R.tick; }
    if (!M.seen_tick || R.tick - M.seen_tick > 60 || M.state < 0) return;
    for (size_t i = 0; i < f->batches.len; i++) {
        const uint64_t *k = f->batches.data[i].tev.textures;
        GlTexture *pl[4] = {0};
        for (unsigned m = 0; m < 8; m++) {
            uint64_t idx;
            if (!k[m] || (k[m] & 0xFFFF000000000000ull) == RT_TAG) continue;
            if (!u64map_get(&R.textures, k[m], &idx)) {
                bind_slot(m, k[m]); /* upload it now (the draw would anyway) */
                if (!u64map_get(&R.textures, k[m], &idx)) continue;
            }
            GlTexture *gt = &R.tex[idx];
            if (gt->movie) pl[gt->movie] = gt;
        }
        if (!pl[1] || !pl[2] || !pl[3] || pl[2]->w * 2 != pl[1]->w || pl[3]->w != pl[2]->w) continue;
        if (pl[1]->enh_gen && M.set[pl[1]->enh_set].gen == pl[1]->enh_gen) continue; /* done */
        if (!movie_ready()) return;
        movie_enhance(pl[1], pl[2], pl[3]);
    }
}

static void bind_efb(void) {
    glBindFramebuffer(GL_FRAMEBUFFER, R.efb_color.fbo);
    glViewport(0, 0, (GLsizei)R.efb_w, (GLsizei)R.efb_h);
}

static void tev_bytes(const TevState *t, uint8_t *b) {
    memcpy(b, t->stages, 256);
    memcpy(b + 256, t->regs, 64);
    memcpy(b + 320, t->konst, 64);
    memcpy(b + 384, t->params, 16);
    memcpy(b + 400, t->fog, 16);
    memcpy(b + 416, t->fog_color, 16);
}

bool render_update(int scale_in) {
    Frame *f = gx_frame_acquire();
    if (!f) return false;
    uint64_t n = gx_frame_count();
    if (n == R.shown) { gx_frame_release(f); return false; }
    R.shown = n;
    R.tick++;
    if (R.hd_flush) { R.hd_flush = false; evict_textures(0); } /* HD switched on/off: everything reloads */
    if (hdtex_available()) hd_upload();
    /* Normally textures live 10 s unused; when many pile up (movies, screen effects), 2 s. */
    if (R.tick % 600 == 0) evict_textures(600);
    else if (R.tick % 30 == 0 && R.ntex > 768) evict_textures(120);
    uint32_t scale = scale_in < 1 ? 1 : scale_in > 4 ? 4 : (uint32_t)scale_in;
    ensure_efb((uint32_t)f->efb[0] * scale, (uint32_t)f->efb[1] * scale);

    unsigned ncopies = f->copies.len < MAX_COPIES ? (unsigned)f->copies.len : MAX_COPIES;
    static CopyOp copies[MAX_COPIES];
    for (unsigned k = 0; k < ncopies; k++) {
        decode_copy(&f->copies.data[k], scale, &copies[k]);
        CopyOp *c = &copies[k];
        if (c->xfb) {
            if (!R.have_xfb || R.xfb.w != c->dst_w || R.xfb.h != c->dst_h) { target_init(&R.xfb, c->dst_w, c->dst_h); R.have_xfb = true; }
        } else {
            rt_for(c->addr, c->dst_w, c->dst_h);
        }
    }

    /* Vertices, indices and TEV uniform blocks for the whole frame: one upload each, into this
     * frame's slot once the GPU is done with what it held NSLOT frames ago. */
    Slot *sl = &R.slots[R.slot];
    R.slot = (R.slot + 1) % NSLOT;
    if (sl->fence) {
        uint64_t w0 = plat_now_ns();
        glClientWaitSync(sl->fence, GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000ull);
        R.gpu_wait_ns += plat_now_ns() - w0;
        glDeleteSync(sl->fence);
        sl->fence = 0;
    }
    state_reset();
    R.compiles_this_frame = 0;
    glBindVertexArray(sl->vao);
    /* + 16: a compact layout's last uv attribute may read past the last vertex's own bytes */
    {
        uint8_t *m = stream_map(&sl->vb, GL_ARRAY_BUFFER, f->vb.len + 16);
        if (m) { memcpy(m, f->vb.data, f->vb.len); memset(m + f->vb.len, 0, 16); glUnmapBuffer(GL_ARRAY_BUFFER); }
    }
    stream_put(&sl->ib, GL_ELEMENT_ARRAY_BUFFER, f->indices.data, f->indices.len * 4);
    {
        /* XF data for GPU-transformed vertices (at least one texel so the texture buffer is valid) */
        static const uint32_t zero[4];
        size_t xn = f->xf.len ? f->xf.len * 4 : 16;
        size_t old_cap = sl->xb.cap;
        stream_put(&sl->xb, GL_TEXTURE_BUFFER, f->xf.len ? (const void *)f->xf.data : zero, xn);
        glActiveTexture(GL_TEXTURE8);
        R.cur_unit = 8;
        glBindTexture(GL_TEXTURE_BUFFER, sl->xtex);
        if (sl->xb.cap != old_cap || !old_cap) glTexBuffer(GL_TEXTURE_BUFFER, GL_RGBA32UI, sl->xb.buf);
    }
    /* A batch whose TEV block equals the previous batch's shares its copy (and its binding). */
    static uint32_t *ub_off;
    static size_t ub_cap;
    if (f->batches.len > ub_cap) { ub_cap = f->batches.len + f->batches.len / 2; ub_off = realloc(ub_off, ub_cap * sizeof *ub_off); }
    size_t ub = f->batches.len * R.ubo_stride;
    uint8_t *um = stream_map(&sl->ub, GL_UNIFORM_BUFFER, ub);
    if (um) {
        size_t used = 0;
        for (size_t i = 0; i < f->batches.len; i++) {
            uint8_t *dst = um + used * R.ubo_stride;
            tev_bytes(&f->batches.data[i].tev, dst);
            if (used && !memcmp(dst, dst - R.ubo_stride, TEV_UNIFORM_SIZE)) { ub_off[i] = (uint32_t)((used - 1) * R.ubo_stride); continue; }
            ub_off[i] = (uint32_t)(used * R.ubo_stride);
            used++;
        }
        glUnmapBuffer(GL_UNIFORM_BUFFER);
    }

    movie_prepass(f);
    size_t next = 0;
    R.stat_batches = (uint32_t)f->batches.len;
    R.stat_verts = f->nverts;
    for (unsigned k = 0; k <= ncopies; k++) {
        size_t end = k < ncopies ? copies[k].at : f->batches.len;
        if (end > f->batches.len) end = f->batches.len;
        if (next < end) {
            bind_efb();
            glBindVertexArray(sl->vao);
            /* 4:3 HUD: only in the pass that ends in the copy to the screen (2D drawn for
             * render-to-texture effects keeps its layout) */
            float sq = R.hud43 && k < ncopies && copies[k].xfb ? 0.25f : 0.0f;
            if (R.sso && R.vs_squeeze >= 0 && sq != R.squeeze) { glProgramUniform1f(R.vs_prog, R.vs_squeeze, sq); R.squeeze = sq; }
            glEnable(GL_SCISSOR_TEST);
            uint32_t sc[4] = {~0u, ~0u, ~0u, ~0u}, cur_ub = ~0u;
            for (size_t i = next; i < end; i++) {
                const Batch *b = &f->batches.data[i];
                uint32_t x0 = b->rect[0] * scale, y0 = b->rect[1] * scale, x1 = b->rect[2] * scale, y1 = b->rect[3] * scale;
                if (sq > 0 && b->hud) {
                    /* narrowed like the vertices, so 2D the game keeps beside the 4:3 screen stays hidden */
                    float cx = f->efb[0] * (float)scale * 0.5f;
                    float sh = b->hud == 2 ? -sq * cx : b->hud == 3 ? sq * cx : 0.0f;
                    x0 = (uint32_t)(cx + ((float)x0 - cx) * (1.0f - sq) + sh + 0.5f);
                    x1 = (uint32_t)(cx + ((float)x1 - cx) * (1.0f - sq) + sh + 0.5f);
                }
                if (x1 > R.efb_w) x1 = R.efb_w;
                if (y1 > R.efb_h) y1 = R.efb_h;
                if (!b->index_count || x1 <= x0 || y1 <= y0) continue;
                if (x0 != sc[0] || y0 != sc[1] || x1 != sc[2] || y1 != sc[3]) {
                    glScissor((GLint)x0, (GLint)y0, (GLsizei)(x1 - x0), (GLsizei)(y1 - y0));
                    sc[0] = x0; sc[1] = y0; sc[2] = x1; sc[3] = y1;
                }
                apply_pipe(b->tev.pipe[0], b->tev.pipe[1]);
                apply_cull(b->tev.pipe[2]);
                use_tev(tev_program(&b->tev));
                if (!um || ub_off[i] != cur_ub) {
                    cur_ub = um ? ub_off[i] : (uint32_t)(i * R.ubo_stride);
                    glBindBufferRange(GL_UNIFORM_BUFFER, 0, sl->ub.buf, (GLintptr)cur_ub, TEV_UNIFORM_SIZE);
                }
                for (unsigned m = 0; m < 8; m++) bind_slot(m, b->tev.textures[m]);
                slot_layout(sl, b->layout);
                glDrawElements(GL_TRIANGLES, (GLsizei)b->index_count, GL_UNSIGNED_INT, (const void *)(uintptr_t)(b->index_first * 4u));
                /* nouveau sends its command buffer to the GPU only when it is full or at the swap:
                 * kick it every 32 draws so the GPU works while the rest of the frame is built */
                if (++R.draws_since_flush >= 32) { glFlush(); R.draws_since_flush = 0; }
            }
            next = end;
        }
        if (k == ncopies) break;
        CopyOp *c = &copies[k];
        Target *dst = c->xfb ? (R.have_xfb ? &R.xfb : NULL) : rt_for(c->addr, c->dst_w, c->dst_h);
        if (dst) {
            glBindFramebuffer(GL_FRAMEBUFFER, dst->fbo);
            glViewport(0, 0, (GLsizei)dst->w, (GLsizei)dst->h);
            glDisable(GL_SCISSOR_TEST);
            glDisable(GL_DEPTH_TEST);
            glDisable(GL_BLEND);
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            use_program(R.prog_copy);
            R.pipe_valid = false;
            apply_cull(0);
            glUniform4ui(R.copy_src, c->src[0], c->src[1], c->src[2], c->src[3]);
            glUniform4ui(R.copy_mode, c->mode[0], c->mode[1], c->mode[2], c->mode[3]);
            glUniform4f(R.copy_vfilter, c->vfilter[0], c->vfilter[1], c->vfilter[2], c->vfilter[3]);
            glUniform1i(R.copy_scale, (GLint)scale);
            unit_bind(0, R.efb_color.tex, 0);
            unit_bind(1, R.efb_depth, 0);
            glBindVertexArray(R.empty_vao);
            glDrawArrays(GL_TRIANGLES, 0, 3);
            glBindVertexArray(sl->vao);
        }
        if (c->clear) {
            uint32_t x = c->src[0] * scale, y = c->src[1] * scale, x1 = x + c->src[2] * scale, y1 = y + c->src[3] * scale;
            if (x1 > R.efb_w) x1 = R.efb_w;
            if (y1 > R.efb_h) y1 = R.efb_h;
            if (x1 > x && y1 > y) {
                uint32_t z = c->regs[4], bl = c->regs[5], ar = c->regs[1], gb = c->regs[2], cz = c->regs[3];
                bind_efb();
                R.pipe_valid = false;
                glEnable(GL_SCISSOR_TEST);
                glScissor((GLint)x, (GLint)y, (GLsizei)(x1 - x), (GLsizei)(y1 - y));
                bool cu = (bl >> 3) & 1, au = (bl >> 4) & 1, zu = (z >> 4) & 1;
                glColorMask(cu, cu, cu, au);
                glDepthMask(zu ? GL_TRUE : GL_FALSE);
                glClearColor((float)(ar & 0xFF) / 255.0f, (float)((gb >> 8) & 0xFF) / 255.0f, (float)(gb & 0xFF) / 255.0f,
                             (float)((ar >> 8) & 0xFF) / 255.0f);
                glClearDepthf((float)(cz & 0xFFFFFF) / 16777215.0f);
                GLbitfield mask = (cu || au ? GL_COLOR_BUFFER_BIT : 0) | (zu ? GL_DEPTH_BUFFER_BIT : 0);
                if (mask) glClear(mask);
            }
        }
    }
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_TRUE);
    glBindVertexArray(0);
    for (unsigned m = 0; m < 8; m++) glBindSampler(m, 0);
    glActiveTexture(GL_TEXTURE0);
    sl->fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    state_reset();
    gx_frame_release(f);
    return true;
}

void render_set_hud43(bool on) { R.hud43 = on; }
/* Anisotropic filtering of the linear samplers (game textures and HD replacements): 1 = off. */
void render_set_aniso(float level) {
    if (level > R.aniso_max) level = R.aniso_max;
    if (level < 1.0f) level = 1.0f;
    if (level == R.aniso || R.aniso_max < 1.0f) return;
    R.aniso = level;
    for (int s = 0; s < 3; s++)
        for (int t = 0; t < 3; t++) {
            glSamplerParameterf(R.samplers[s][t][1], 0x84FE /* TEXTURE_MAX_ANISOTROPY */, level);
            glSamplerParameterf(R.samplers_mip[s][t], 0x84FE, level);
        }
}
void render_set_hd(bool on) {
    on = on && R.bptc && hdtex_available();
    if (on != R.hd_on) { R.hd_on = on; R.hd_flush = true; }
}
uint64_t render_hd_bytes(void) { return R.hd_bytes; }
unsigned render_hd_in_use(void) {
    unsigned n = 0;
    for (unsigned i = 0; i < R.ntex; i++) n += R.tex[i].hd == 2;
    return n;
}
/* GL_EXT_disjoint_timer_query. switch-mesa's results are 1/1.6276 of real time (PTIMER counted at
 * the wrong rate). Each query makes nvc0 wait for the GPU first, so it is sampled rarely. */
#define TIME_ELAPSED 0x88BF
#define GPU_DISJOINT 0x8FBB
static struct {
    int state; /* 0 untried, 1 available, -1 unavailable */
    GLuint q[2];               /* the game image (render_update), the output (present + menu) */
    int running;               /* query running: 0 none, 1 first, 2 second */
    bool pending;
    unsigned frames, n;
    double sum, max, part[2];
    void (*get_u64)(GLuint, GLenum, GLuint64 *);
} s_gt;

void render_gpu_timer_begin(void) {
    if (s_gt.state == 0) {
        const char *ext = (const char *)glGetString(GL_EXTENSIONS);
        s_gt.get_u64 = (void (*)(GLuint, GLenum, GLuint64 *))eglGetProcAddress("glGetQueryObjectui64vEXT");
        s_gt.state = ext && strstr(ext, "GL_EXT_disjoint_timer_query") && s_gt.get_u64 ? 1 : -1;
        if (s_gt.state == 1) glGenQueries(2, s_gt.q);
        rt_log("render: GPU timer %s", s_gt.state == 1 ? "available" : "not available");
    }
    if (s_gt.state != 1 || s_gt.running) return;
    if (s_gt.pending) {
        GLuint ready = 0;
        glGetQueryObjectuiv(s_gt.q[1], GL_QUERY_RESULT_AVAILABLE, &ready);
        if (!ready) return;
        GLint disjoint = 0;
        glGetIntegerv(GPU_DISJOINT, &disjoint);
        GLuint64 ns[2] = {0, 0};
        s_gt.get_u64(s_gt.q[0], GL_QUERY_RESULT, &ns[0]);
        s_gt.get_u64(s_gt.q[1], GL_QUERY_RESULT, &ns[1]);
        s_gt.pending = false;
        if (!disjoint) {
            double a = (double)ns[0] * 1.6276 / 1e6, b = (double)ns[1] * 1.6276 / 1e6;
            s_gt.sum += a + b;
            s_gt.part[0] += a;
            s_gt.part[1] += b;
            if (a + b > s_gt.max) s_gt.max = a + b;
            s_gt.n++;
        }
    }
    if (++s_gt.frames < 60) return;
    s_gt.frames = 0;
    glBeginQuery(TIME_ELAPSED, s_gt.q[0]);
    s_gt.running = 1;
}

void render_gpu_timer_mid(void) {
    if (s_gt.running != 1) return;
    glEndQuery(TIME_ELAPSED);
    glBeginQuery(TIME_ELAPSED, s_gt.q[1]);
    s_gt.running = 2;
}

void render_gpu_timer_end(void) {
    if (!s_gt.running) return;
    if (s_gt.running == 1) { glEndQuery(TIME_ELAPSED); glBeginQuery(TIME_ELAPSED, s_gt.q[1]); }
    glEndQuery(TIME_ELAPSED);
    s_gt.running = 0;
    s_gt.pending = true;
}

unsigned render_gpu_timer_take(double *avg_ms, double *max_ms, double *image_ms, double *output_ms) {
    unsigned n = s_gt.n;
    *avg_ms = n ? s_gt.sum / n : 0;
    *image_ms = n ? s_gt.part[0] / n : 0;
    *output_ms = n ? s_gt.part[1] / n : 0;
    *max_ms = s_gt.max;
    s_gt.n = 0;
    s_gt.sum = s_gt.max = s_gt.part[0] = s_gt.part[1] = 0;
    return n;
}

uint64_t render_take_gpu_wait(void) { uint64_t v = R.gpu_wait_ns; R.gpu_wait_ns = 0; return v; }

void render_present(int w, int h, const float adj[3], bool wide) {
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glViewport(0, 0, w, h);
    glClearColor(0, 0, 0, 1);
    float vw = (float)w, vh = (float)h;
    float aspect = wide ? 16.0f / 9.0f : 4.0f / 3.0f;
    if (vw / vh > aspect) vw = vh * aspect;
    else vh = vw / aspect;
    GLint vx = (GLint)(((float)w - vw) / 2), vy = (GLint)(((float)h - vh) / 2);
    if (!R.have_xfb) { glClear(GL_COLOR_BUFFER_BIT); return; }
    /* clear only the bars beside / above the picture (none when it fills the screen) */
    glEnable(GL_SCISSOR_TEST);
    if (vx > 0) {
        glScissor(0, 0, vx, h); glClear(GL_COLOR_BUFFER_BIT);
        glScissor(vx + (GLint)vw, 0, w - vx - (GLint)vw, h); glClear(GL_COLOR_BUFFER_BIT);
    }
    if (vy > 0) {
        glScissor(0, 0, w, vy); glClear(GL_COLOR_BUFFER_BIT);
        glScissor(0, vy + (GLint)vh, w, h - vy - (GLint)vh); glClear(GL_COLOR_BUFFER_BIT);
    }
    glDisable(GL_SCISSOR_TEST);
    glViewport(vx, vy, (GLsizei)vw, (GLsizei)vh);
    glUseProgram(R.prog_blit);
    glUniform3f(R.blit_adj, adj[0], adj[1], adj[2]);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, R.xfb.tex);
    glBindSampler(0, R.samplers[0][0][1]);
    /* The default framebuffer has row 0 at the bottom; the XFB has row 0 at the top: v_uv.y is 0
     * at the top of the screen, which samples row 0. */
    glBindVertexArray(R.empty_vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindSampler(0, 0);
}

void render_stats(uint32_t *batches, uint32_t *verts, uint32_t *textures) {
    *batches = R.stat_batches;
    *verts = R.stat_verts;
    *textures = R.ntex;
}

void render_shader_stats(unsigned *programs, double *compile_ms) {
    *programs = R.nprogs;
    *compile_ms = (double)R.compile_ns / 1e6;
}
