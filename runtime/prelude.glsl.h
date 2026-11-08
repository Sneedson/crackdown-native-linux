// GLSL shared by every translated Xenos shader (see translate.cpp). Embedded as C++ strings.
#pragma once
#include <string>

namespace xlate
{

// The first line of every translated shader, and the declarations they share.
inline const char *kPreludeHead = R"GLSL(#version 450
#extension GL_EXT_scalar_block_layout : require
#extension GL_EXT_control_flow_attributes : require
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#extension GL_EXT_shader_atomic_int64 : require
#ifndef MIP_BIAS
#define MIP_BIAS 0.0  // CRACKDOWN_MIP_BIAS: added to every texture LOD (debugging the mip layout)
#endif

// Guest physical memory, as little-endian words of the big-endian data the console stores.
layout(std430, set = 0, binding = 0) buffer Mem { uint mem[]; };

// Per-draw constants (renderer.h DrawConstants mirrors this layout).
layout(std430, set = 0, binding = 1) readonly buffer DrawConstants {
  vec4 c[512];        // ALU constants: vertex 0-255, pixel 256-511
  uint fetch[192];    // 32 texture fetch constants x 6 dwords (= 96 vertex fetch constants x 2)
  uint bools[8];      // 256 bool constants
  uint loops[32];     // loop constants
  vec4 vp_scale;      // PA_CL_VPORT_{X,Y,Z}SCALE
  vec4 vp_offset;     // PA_CL_VPORT_{X,Y,Z}OFFSET
  vec4 rt;            // render target width, height, 1/width, 1/height
  vec4 window_offset; // PA_SC_WINDOW_OFFSET (x, y) when it applies to the vertices; z: image pixels per surface pixel; w: of those, the anti-aliasing's (texture detail follows the resolution)
  uint vte;           // PA_CL_VTE_CNTL
  uint prim;          // VGT primitive type
  uint index_mode;    // 0 DMA index buffer, 2 auto-generated
  uint index_addr;    // physical byte address of the index buffer
  uint index_info;    // bit 0: 32-bit indices; bits 4-5: endian
  uint index_offset;  // VGT_INDX_OFFSET
  uint first;         // first vertex of this sub-draw
  uint flags;
  uint tex[32];       // native textures: per texture fetch constant, its image and sampler group (tfetch)
  uint param_gen;     // pixel shaders: the register the screen position goes in (SQ_CONTEXT_MISC), else ~0
  uint sharp;         // per texture fetch constant: magnified with a sharper filter (tfetch)
  uint text;          // per texture fetch constant: the game's font, glyph edges redrawn crisp (tfetch)
  uint tsize[32];     // per texture fetch constant: its image's width - 1 | height - 1 << 16
  uint ucp_ena;       // user clip planes in use (PA_CL_CLIP_CNTL bits 0-5)
  vec4 ucp[6];        // their planes, in clip space
  uint mv_cur, mv_prev;  // DLSS's motion: where this draw's vertices' places go in mvpos this frame, and were last frame (~0: none)
  uint mv_pix;        // bit 0: its pixels' motion goes to mvpix; bit 1: see-through (to mvpix's second half); bit 2: drawn without the depth test;
                      // bit 3: marks DLSS's see-through mask; bit 4: added on (its alpha not its strength)
  uint mv_w, mv_h;    // mvpix's size (the scene's image pixels)
  float mv_jx, mv_jy; // this frame's jitter (surface pixels): places are kept without it
  uint mv_cam;        // 1: mv_m holds the camera's move (a vertex moving with the world needs no motion of its own)
  mat4 mv_m;          // last frame's view-projection times the inverse of this one's
} dc;

// DLSS's motion (renderer.cpp DlssFrame): each vertex's place on screen (surface pixels, without the
// jitter; z: its clip w), and per pixel of the scene the nearest drawn surface's depth (high word) and
// its motion since the last frame (image pixels, two halves); after MVPIX of those, the same for the
// nearest see-through drawing (blended, no depth written: glows, lights, smoke), whose motion is the
// pixel's where it lies in front of the surface (a car's tail lights move with the car, not the road).
const uint MVPIX = 3840u * 2160u;
layout(std430, set = 0, binding = 2) buffer MotionPos { vec4 mvpos[]; };
layout(std430, set = 0, binding = 3) buffer MotionPix { uint64_t mvpix[]; };

// The textures (as Vulkan images) and samplers draws read (renderer.cpp "Textures").
layout(set = 1, binding = 0) uniform texture2D xtex2d[];
layout(set = 1, binding = 1) uniform sampler xsamp[];
layout(set = 1, binding = 2) uniform texture2DArray xtexcube[];  // cube maps: their six faces as layers

)GLSL";

// Helpers (also the texture upload shader's, renderer.cpp: it declares its own mem and dc).
inline const char *kPreludeCommon = R"GLSL(uint bswap32(uint v) { return (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24); }
uint xswap(uint v, uint e) {
  if (e == 1u) return ((v & 0x00FF00FFu) << 8) | ((v >> 8) & 0x00FF00FFu);  // 8-in-16
  if (e == 2u) return bswap32(v);                                           // 8-in-32
  if (e == 3u) return (v << 16) | (v >> 16);                                // 16-in-32
  return v;
}
uint ld(uint addr, uint e) { return xswap(mem[(addr & 0x1FFFFFFFu) >> 2], e); }

// Texel index of (x, y) in a 2D tiled surface; log_bpp = log2(bytes per element). (XGAddress2DTiledOffset)
uint tiled_offset(uint x, uint y, uint width, uint log_bpp) {
  uint aligned = (width + 31u) & ~31u;
  uint macro = ((x >> 5) + (y >> 5) * (aligned >> 5)) << (log_bpp + 7u);
  uint micro = ((x & 7u) + ((y & 6u) << 2)) << log_bpp;
  uint offset = macro + ((micro & ~15u) << 1) + (micro & 15u) + ((y & 8u) << (3u + log_bpp)) + ((y & 1u) << 4);
  return ((((offset & ~511u) << 3) + ((offset & 448u) << 2) + (offset & 63u) + ((y & 16u) << 7) +
          (((((y & 8u) >> 2) + (x >> 3)) & 3u) << 6)) >> log_bpp);
}

float unorm_bits(uint v, uint bits) { return float(v) / float((1u << bits) - 1u); }
float snorm_bits(uint v, uint bits) {
  int s = int(v << (32u - bits)) >> (32u - bits);
  return max(float(s) / float((1 << (bits - 1u)) - 1), -1.0);
}
float comp(uint v, uint bits, uint flags) {
  // flags bit 0: signed, bit 1: integer (not normalised)
  if ((flags & 2u) != 0u) return (flags & 1u) != 0u ? float(int(v << (32u - bits)) >> (32u - bits)) : float(v);
  return (flags & 1u) != 0u ? snorm_bits(v, bits) : unorm_bits(v, bits);
}

// Vertex fetch: vertex fetch constant vf = 2 dwords (address | type, endian | size).
// A fetch result into a register by a destination swizzle (3 bits per component: 0-3 xyzw, 4 zero,
// 5 one, 7 keep), for fetches whose swizzle is a specialization constant.
vec4 fetch_swizzle(vec4 r, vec4 t, uint dsw) {
  for (int i = 0; i < 4; ++i) {
    uint s = (dsw >> uint(i * 3)) & 7u;
    if (s < 4u) r[i] = t[s]; else if (s == 4u) r[i] = 0.0; else if (s == 5u) r[i] = 1.0; else if (s != 7u) r[i] = 0.0;
  }
  return r;
}

vec4 vfetch(uint vf, float index, uint format, uint flags, int exp_adjust, uint stride, int offset) {
  uint f0 = dc.fetch[vf * 2u], f1 = dc.fetch[vf * 2u + 1u];
  uint e = f1 & 3u;
  uint addr = (f0 & 0xFFFFFFFCu) + uint((int(index) * int(stride) + offset) * 4);
  vec4 v = vec4(0.0, 0.0, 0.0, 1.0);
  switch (format) {
  case 6u: { uint w = ld(addr, e); v = vec4(comp(w & 255u, 8u, flags), comp((w >> 8) & 255u, 8u, flags), comp((w >> 16) & 255u, 8u, flags), comp(w >> 24, 8u, flags)); break; }
  case 7u: { uint w = ld(addr, e); v = vec4(comp(w & 1023u, 10u, flags), comp((w >> 10) & 1023u, 10u, flags), comp((w >> 20) & 1023u, 10u, flags), comp(w >> 30, 2u, flags)); break; }
  case 25u: { uint w = ld(addr, e); v = vec4(comp(w & 65535u, 16u, flags), comp(w >> 16, 16u, flags), 0.0, 1.0); break; }
  case 26u: { uint w = ld(addr, e), w2 = ld(addr + 4u, e); v = vec4(comp(w & 65535u, 16u, flags), comp(w >> 16, 16u, flags), comp(w2 & 65535u, 16u, flags), comp(w2 >> 16, 16u, flags)); break; }
  case 31u: { uint w = ld(addr, e); v = vec4(unpackHalf2x16(w), 0.0, 1.0); break; }
  case 32u: { v = vec4(unpackHalf2x16(ld(addr, e)), unpackHalf2x16(ld(addr + 4u, e))); break; }
  case 33u: v = vec4(comp(ld(addr, e), 32u, flags | 2u), 0.0, 0.0, 1.0); break;
  case 34u: v = vec4(comp(ld(addr, e), 32u, flags | 2u), comp(ld(addr + 4u, e), 32u, flags | 2u), 0.0, 1.0); break;
  case 35u: v = vec4(comp(ld(addr, e), 32u, flags | 2u), comp(ld(addr + 4u, e), 32u, flags | 2u), comp(ld(addr + 8u, e), 32u, flags | 2u), comp(ld(addr + 12u, e), 32u, flags | 2u)); break;
  case 36u: v = vec4(uintBitsToFloat(ld(addr, e)), 0.0, 0.0, 1.0); break;
  case 37u: v = vec4(uintBitsToFloat(ld(addr, e)), uintBitsToFloat(ld(addr + 4u, e)), 0.0, 1.0); break;
  case 38u: v = vec4(uintBitsToFloat(ld(addr, e)), uintBitsToFloat(ld(addr + 4u, e)), uintBitsToFloat(ld(addr + 8u, e)), uintBitsToFloat(ld(addr + 12u, e))); break;
  case 57u: v = vec4(uintBitsToFloat(ld(addr, e)), uintBitsToFloat(ld(addr + 4u, e)), uintBitsToFloat(ld(addr + 8u, e)), 1.0); break;
  default: break;
  }
  if (exp_adjust != 0 && format != 36u && format != 37u && format != 38u && format != 57u) v *= exp2(float(exp_adjust));
  return v;
}

// ------------------------------------------------------------------ textures
// Texture fetch constant (6 dwords): d0 bit 31 tiled, bits 22-30 pitch/32; d1 bits 0-5 format,
// 6-7 endian, 12-31 base; d2 width-1 (0-12), height-1 (13-25); d3 bits 1-12 swizzle, 19-24 filters.
vec3 rgb565(uint v) { return vec3(float((v >> 11) & 31u) / 31.0, float((v >> 5) & 63u) / 63.0, float(v & 31u) / 31.0); }

vec4 dxt_color(uint w0, uint w1, uint px, bool dxt1) {
  uint c0 = w0 & 0xFFFFu, c1 = w0 >> 16;
  vec3 a = rgb565(c0), b = rgb565(c1);
  uint sel = (w1 >> (2u * px)) & 3u;
  if (!dxt1 || c0 > c1) {
    if (sel == 0u) return vec4(a, 1.0);
    if (sel == 1u) return vec4(b, 1.0);
    if (sel == 2u) return vec4(mix(a, b, 1.0 / 3.0), 1.0);
    return vec4(mix(a, b, 2.0 / 3.0), 1.0);
  }
  if (sel == 0u) return vec4(a, 1.0);
  if (sel == 1u) return vec4(b, 1.0);
  if (sel == 2u) return vec4((a + b) * 0.5, 1.0);
  return vec4(0.0);
}

// Texture addressing (fetch constant dword 0: X mode bits 10-12, Y mode bits 13-15): 0 wrap,
// 1 mirror, 2/4/6 clamp (to edge, halfway, border: all to the edge here), 3/5/7 mirror once.
int tex_address(int c, int size, uint mode) {
  if (mode == 0u) return ((c % size) + size) % size;
  if (mode == 1u) { int period = 2 * size, m = ((c % period) + period) % period; return m < size ? m : period - 1 - m; }
  if ((mode & 1u) != 0u) c = c < 0 ? -1 - c : c;  // mirror once
  return clamp(c, 0, size - 1);
}

uint tex_log_bpp(uint format) {
  switch (format) {
  case 2u: return 0u;                                          // 8
  case 3u: case 4u: case 10u: case 15u: case 24u: case 30u: return 1u;  // 1_5_5_5, 5_6_5, 8_8, 4_4_4_4, 16, 16_FLOAT
  case 18u: case 26u: case 32u: case 35u: case 37u: return 3u; // DXT1 (per 4x4 block), 64-bit formats
  case 19u: case 20u: case 38u: return 4u;                     // DXT2-5 (per block), 128-bit
  default: return 2u;
  }
}

// Mip layout, as measured on this game's textures (devtools/mip_*_probe.py):
// - Level 1 onwards start at the mip address (fetch constant dword 5). Each level whose shorter side is
//   above 16 texels takes its own storage: width and height in blocks rounded up to 32 (the row pitch),
//   the size rounded up to 4 KB, one level after another.
// - With packed mips (dword 5 bit 11), the first level whose shorter side is <= 16 texels ("tail" level
//   T) and all smaller ones share one such 32-block-pitch region. Level T + p sits at texel offset
//     p < 3: (16 >> p, 0), or (0, 16 >> p) when T is wider than tall;
//     p >= 3: (0, L >> (p - 2)), or (L >> (p - 2), 0) when wider, L = T's longer side.
//   A base level that is itself <= 16 texels on its shorter side is placed the same way (p = 0).
uvec2 tail_offset(uint tw, uint th, uint p) {
  bool wide = tw > th;
  uint o = p < 3u ? 16u >> p : max(tw, th) >> (p - 2u);
  return (p < 3u) != wide ? uvec2(o, 0u) : uvec2(0u, o);
}

// Mip level `level` of texture k (layout above): where it starts, its row pitch (texels), its size and,
// for a packed level, its offset (texels) in the region it shares.
// A cube map (fetch constant dword 5 bits 9-10 = 3) keeps its six faces one after another at each
// level, each face laid out as that level of a 2D texture (its region rounded to 4 KB); face selects one.
void tex_level_face(uint k, uint level, uint face, out uint base, out uint pitch, out uint w, out uint h, out uvec2 off) {
  uint d0 = dc.fetch[k * 6u], d1 = dc.fetch[k * 6u + 1u], d2 = dc.fetch[k * 6u + 2u], d5 = dc.fetch[k * 6u + 5u];
  uint layers = ((d5 >> 9) & 3u) == 3u ? 6u : 1u;
  uint format = d1 & 63u;
  uint w0 = (d2 & 0x1FFFu) + 1u, h0 = ((d2 >> 13) & 0x1FFFu) + 1u;
  bool block = format == 18u || format == 19u || format == 20u;
  uint bw = block ? 4u : 1u;
  base = d1 & 0xFFFFF000u;
  pitch = ((d0 >> 22) & 0x1FFu) * 32u;
  if (pitch == 0u) pitch = (w0 + 31u) & ~31u;
  uint tail = 99u;  // first packed level
  if (((d5 >> 11) & 1u) != 0u)
    for (tail = 0u; tail < 14u && min(max(w0 >> tail, 1u), max(h0 >> tail, 1u)) > 16u; ++tail) {}
  w = max(w0 >> level, 1u), h = max(h0 >> level, 1u);
  off = uvec2(0u);
  if (level > 0u) {
    uint lb = tex_log_bpp(format);
    base = d5 & 0xFFFFF000u;
    uint stored = min(level, max(tail, 1u));
    for (uint i = 1u; i < stored; ++i) {
      uint pb = ((max(w0 >> i, 1u) + bw - 1u) / bw + 31u) & ~31u, hb = ((max(h0 >> i, 1u) + bw - 1u) / bw + 31u) & ~31u;
      base += layers * (((pb * hb << lb) + 4095u) & ~4095u);
    }
    pitch = (((max(w0 >> stored, 1u) + bw - 1u) / bw + 31u) & ~31u) * bw;
  }
  if (face > 0u) {  // the faces before it at this level
    uint lb = tex_log_bpp(format), lv = level > 0u ? min(level, max(tail, 1u)) : 0u;
    uint hb = ((max(h0 >> lv, 1u) + bw - 1u) / bw + 31u) & ~31u;
    base += face * ((((pitch / bw + 31u) & ~31u) * hb << lb) + 4095u & ~4095u);
  }
  if (level >= tail) off = tail_offset(max(w0 >> tail, 1u), max(h0 >> tail, 1u), level - tail);
}
void tex_level(uint k, uint level, out uint base, out uint pitch, out uint w, out uint h, out uvec2 off) {
  tex_level_face(k, level, 0u, base, pitch, w, h, off);
}

// Address of the element holding texel (ux, uy) of a level (texels, the level's offset applied): the
// texel, or for DXT its 4x4 block.
uint tex_elem_addr(uint k, uint base, uint pitch, uint ux, uint uy) {
  uint format = dc.fetch[k * 6u + 1u] & 63u;
  bool tiled = (dc.fetch[k * 6u] >> 31) != 0u;
  if (format == 18u || format == 19u || format == 20u) {
    uint bx = ux >> 2, by = uy >> 2, bpitch = max(pitch >> 2, 1u), lb = format == 18u ? 3u : 4u;
    return base + ((tiled ? tiled_offset(bx, by, bpitch, lb) : by * bpitch + bx) << lb);
  }
  uint lb = tex_log_bpp(format);
  return base + ((tiled ? tiled_offset(ux, uy, pitch, lb) : uy * pitch + ux) << lb);
}

// Texel (x, y) of mip level `level` (layout above).
vec4 texel_face(uint k, int x, int y, uint level, uint face) {
  uint d0 = dc.fetch[k * 6u], d1 = dc.fetch[k * 6u + 1u];
  uint format = d1 & 63u, e = (d1 >> 6) & 3u;
  uint base, pitch, w, h;
  uvec2 off;
  tex_level_face(k, level, face, base, pitch, w, h, off);
  uint ux = uint(tex_address(x, int(w), (d0 >> 10) & 7u)) + off.x, uy = uint(tex_address(y, int(h), (d0 >> 13) & 7u)) + off.y;
  uint addr = tex_elem_addr(k, base, pitch, ux, uy);
  if (format == 18u || format == 19u || format == 20u) {
    uint px = (uy & 3u) * 4u + (ux & 3u);
    if (format == 18u) return dxt_color(ld(addr, e), ld(addr + 4u, e), px, true);
    vec4 col = dxt_color(ld(addr + 8u, e), ld(addr + 12u, e), px, false);
    if (format == 19u) { // DXT2/3: explicit 4-bit alpha
      uint aw = ld(addr + (px >= 8u ? 4u : 0u), e);
      col.a = float((aw >> (4u * (px & 7u))) & 15u) / 15.0;
    } else {             // DXT4/5: interpolated alpha
      uint a0w = ld(addr, e), a1w = ld(addr + 4u, e);
      float a0 = float(a0w & 255u) / 255.0, a1 = float((a0w >> 8) & 255u) / 255.0;
      uint bit = 3u * px;
      uint bits = bit + 16u < 32u ? (a0w >> (bit + 16u)) : 0u;
      uint sel;
      if (bit + 16u >= 32u) sel = (a1w >> (bit + 16u - 32u)) & 7u;
      else if (bit + 16u + 3u > 32u) sel = ((a0w >> (bit + 16u)) | (a1w << (32u - (bit + 16u)))) & 7u;
      else sel = bits & 7u;
      float a;
      if (sel == 0u) a = a0; else if (sel == 1u) a = a1;
      else if (a0 > a1) a = mix(a0, a1, float(sel - 1u) / 7.0);
      else a = sel == 6u ? 0.0 : sel == 7u ? 1.0 : mix(a0, a1, float(sel - 1u) / 5.0);
      col.a = a;
    }
    return col;
  }
  uint word = ld(addr & ~3u, e);
  uint shift = (addr & 3u) * 8u;
  switch (format) {
  case 2u: { float v = float((word >> shift) & 255u) / 255.0; return vec4(v); }
  case 3u: { uint v = (word >> shift) & 65535u; return vec4(float((v >> 10) & 31u) / 31.0, float((v >> 5) & 31u) / 31.0, float(v & 31u) / 31.0, float(v >> 15)); }
  case 4u: { uint v = (word >> shift) & 65535u; return vec4(rgb565(v), 1.0); }
  case 10u: { uint v = (word >> shift) & 65535u; return vec4(float(v & 255u) / 255.0, float(v >> 8) / 255.0, 0.0, 1.0); }
  case 15u: { uint v = (word >> shift) & 65535u; return vec4(float((v >> 8) & 15u), float((v >> 4) & 15u), float(v & 15u), float(v >> 12)) / 15.0; }
  case 6u: return vec4(float(word & 255u), float((word >> 8) & 255u), float((word >> 16) & 255u), float(word >> 24)) / 255.0;  // X..W; the fetch swizzle orders them
  case 7u: return vec4(float(word & 1023u) / 1023.0, float((word >> 10) & 1023u) / 1023.0, float((word >> 20) & 1023u) / 1023.0, float(word >> 30) / 3.0);
  case 24u: return vec4(float((word >> shift) & 65535u) / 65535.0);                            // 16
  case 30u: return vec4(unpackHalf2x16((word >> shift) & 65535u).x);                           // 16_FLOAT
  case 25u: return vec4(float(word & 65535u) / 65535.0, float(word >> 16) / 65535.0, 0.0, 1.0);  // 16_16
  case 26u: { uint w1 = ld(addr + 4u, e);                                                     // 16_16_16_16
    return vec4(float(word & 65535u), float(word >> 16), float(w1 & 65535u), float(w1 >> 16)) / 65535.0; }
  case 31u: return vec4(unpackHalf2x16(word), 0.0, 1.0);                                       // 16_16_FLOAT
  case 32u: return vec4(unpackHalf2x16(word), unpackHalf2x16(ld(addr + 4u, e)));               // 16_16_16_16_FLOAT (particle curves)
  case 36u: return vec4(uintBitsToFloat(word));
  case 37u: return vec4(uintBitsToFloat(word), uintBitsToFloat(ld(addr + 4u, e)), 0.0, 1.0);  // 32_32_FLOAT
  case 38u: return vec4(uintBitsToFloat(word), uintBitsToFloat(ld(addr + 4u, e)), uintBitsToFloat(ld(addr + 8u, e)), uintBitsToFloat(ld(addr + 12u, e)));  // 32_32_32_32_FLOAT
  case 22u: return vec4(float(word >> 8) / 16777215.0);  // k_24_8: depth (top 24 bits), stencil below
  case 23u: {                                            // k_24_8_FLOAT: 20e4 float depth
    uint v = word >> 8, ex = v >> 20, m = v & 0xFFFFFu;
    return vec4(ex == 0u ? float(m) * exp2(-34.0) : (1.0 + float(m) / 1048576.0) * exp2(float(ex) - 15.0));
  }
  default: return vec4(1.0, 0.0, 1.0, 1.0);  // unsupported format: magenta
  }
}
vec4 texel(uint k, int x, int y, uint level) { return texel_face(k, x, y, level, 0u); }

)GLSL";

inline const char *kPreludeTail = R"GLSL(vec4 tex_swizzle(uint k, vec4 t) {
  // Each component's number format (fetch constant dword 0, bits 2-9, X..W): 0 unsigned, 1 signed
  // (two's complement: an 8-bit format's unorm value read again as snorm; float formats ignore it -- the
  // game marks its 16-bit float curves signed), 2 unsigned biased (2x - 1; the G-buffer normals the
  // outline pass reads -- without it camera-facing walls drew black), 3 gamma (not used by the game).
  uint signs = (dc.fetch[k * 6u] >> 2) & 0xFFu, format = dc.fetch[k * 6u + 1u] & 63u;
  if (signs != 0u)
    for (int i = 0; i < 4; ++i) {
      uint m = (signs >> (2 * i)) & 3u;
      if (m == 2u) t[i] = t[i] * 2.0 - 1.0;
      else if (m == 1u && (format == 2u || format == 6u)) { float b = floor(t[i] * 255.0 + 0.5); t[i] = max((b >= 128.0 ? b - 256.0 : b) / 127.0, -1.0); }
    }
  uint sw = (dc.fetch[k * 6u + 3u] >> 1) & 0xFFFu;
  vec4 r;
  for (int i = 0; i < 4; ++i) {
    uint s = (sw >> (3 * i)) & 7u;
    r[i] = s < 4u ? t[s] : s == 4u ? 0.0 : 1.0;
  }
  return r;
}

// The layer a 3D fetch's third coordinate picks in a stacked 2D texture (fetch constant dword 5 bits
// 9-10: 1; dword 2 bits 26-31: layers - 1; the coordinate normalised over them): the game keeps its two
// sun shadow cascades so. tfetch2d reads tex_layer.
uint tex_layer = 0u;
uint stack_layer(uint k, float z) {
  uint d2 = dc.fetch[k * 6u + 2u], d5 = dc.fetch[k * 6u + 5u], depth = (d2 >> 26) + 1u;
  return ((d5 >> 9) & 3u) == 1u ? uint(clamp(z * float(depth), 0.0, float(depth - 1u))) : 0u;
}

// What a bilinear fetch at uv would blend by (x, y): for shaders that filter their own samples.
vec4 tweights(uint k, vec2 uv, bool unnormalized, vec2 offset) {
  uint d2 = dc.fetch[k * 6u + 2u];
  vec2 size = vec2(float((d2 & 0x1FFFu) + 1u), float(((d2 >> 13) & 0x1FFFu) + 1u));
  return vec4(fract((unnormalized ? uv : uv * size) + offset - 0.5), 0.0, 0.0);
}

// Texture fetch. filt = the instruction's mag | min << 2 | mip << 4 filters (3 = take the fetch
// constant's), bias = its LOD bias; grad = (d uv/dx, d uv/dy) in pixel shaders, zero elsewhere;
// offset = the instruction's texel offset (applied at the base level); reg_lod = the level from SET_TEX_LOD
// when the instruction uses it (-1000 otherwise).
vec4 tfetch2d(uint k, vec2 uv, bool unnormalized, uint filt, float bias, vec4 grad, vec2 offset, float reg_lod) {
#ifdef PERF_NOTEX
  return vec4(0.5);
#endif
#ifdef PERF_NOTEX_MASK
  if (((PERF_NOTEX_MASK >> k) & 1u) != 0u) return vec4(0.5);
#endif
  uint d2 = dc.fetch[k * 6u + 2u], d3 = dc.fetch[k * 6u + 3u], d4 = dc.fetch[k * 6u + 4u], d5 = dc.fetch[k * 6u + 5u];
  vec2 size = vec2(float((d2 & 0x1FFFu) + 1u), float(((d2 >> 13) & 0x1FFFu) + 1u));
  vec2 p = (unnormalized ? uv : uv * size) + offset;
  uint mag = (filt & 3u) == 3u ? (d3 >> 19) & 3u : filt & 3u;
  uint minf = ((filt >> 2) & 3u) == 3u ? (d3 >> 21) & 3u : (filt >> 2) & 3u;
  uint mipf = ((filt >> 4) & 3u) == 3u ? (d3 >> 23) & 3u : (filt >> 4) & 3u;
  // Level of detail from the screen-space footprint, then the fetch constant's level range.
  vec4 g = unnormalized ? grad : grad * size.xyxy;
  float rho = max(dot(g.xy, g.xy), dot(g.zw, g.zw));
  float lod = reg_lod > -999.0 ? reg_lod : rho > 0.0 ? 0.5 * log2(rho) : -100.0;
  lod += bias + float(int(d4 << 10) >> 22) / 32.0 + MIP_BIAS;
  bool minify = lod > 0.0;
  bool linear = (minify ? minf : mag) == 1u;
  uint max_level = (d5 & 0xFFFFF000u) == 0u ? 0u : min((d4 >> 6) & 15u, uint(findMSB(uint(max(size.x, size.y)))));
  uint min_level = min((d4 >> 2) & 15u, max_level);
  float l = mipf == 2u || !minify ? 0.0 : clamp(lod, float(min_level), float(max_level));
  if (mipf == 0u) l = floor(l + 0.5);  // point: nearest level
  uint l0 = uint(l), levels = mipf == 1u && fract(l) > 0.0 ? 2u : 1u;
  // One texel() call site, looped over the taps: texel() is inlined wherever it is called, and a
  // shader with many fetches otherwise grows past what the driver compiles in reasonable time.
  vec4 t = vec4(0.0);
  int taps = linear ? 4 : 1;
  [[dont_unroll]] for (int n = 0; n < taps * int(levels); ++n) {
    uint lv = l0 + uint(n / taps);
    float lw = levels == 1u ? 1.0 : lv == l0 ? 1.0 - fract(l) : fract(l);
    vec2 q = p / exp2(float(lv));
    q = linear ? q - 0.5 : q;
    vec2 f = linear ? fract(q) : vec2(0.0);
    ivec2 i = ivec2(floor(q));
    int dx = n & 1, dy = (n >> 1) & 1;
    if (!linear) { dx = 0; dy = 0; }
    float wgt = (dx != 0 ? f.x : 1.0 - f.x) * (dy != 0 ? f.y : 1.0 - f.y) * lw;
    t += texel_face(k, i.x + dx, i.y + dy, lv, tex_layer) * wgt;
  }
  return tex_swizzle(k, t);
}

// Native texture sampling: the fetch constant's texture as a Vulkan image (dc.tex[k]: bits 0-15 its
// index in xtex2d, 16-31 its sampler group in xsamp: 12 samplers per group, by mag (bit 0, linear),
// min (bit 1) and mip filter (bits 2-3: point, linear, base level only); all ones: read from guest
// memory by tfetch2d, for comparisons, CRACKDOWN_DECODED_TEXTURES: compiled in only then). filt, bias, offset and
// reg_lod as tfetch2d's; grad: the coordinates' screen derivatives (computed level of detail), else
// the base level.
vec4 tfetch(uint k, vec2 uv, bool unnormalized, uint filt, float bias, vec4 grad, bool computed, vec2 offset, float reg_lod) {
#ifdef PERF_NOTEX
  return vec4(0.5);
#endif
#ifdef PERF_NOTEX_MASK
  if (((PERF_NOTEX_MASK >> k) & 1u) != 0u) return vec4(0.5);
#endif
  uint t = dc.tex[k], d3 = dc.fetch[k * 6u + 3u];
#ifdef TEX_DECODED_FALLBACK  // the renderer's debug switches (CRACKDOWN_DECODED_*): decoded here instead
  if (t == 0xFFFFFFFFu) return tfetch2d(k, uv, unnormalized, filt, bias, computed ? grad : vec4(0.0), offset, reg_lod);
#endif
  uint mag = (filt & 3u) == 3u ? (d3 >> 19) & 3u : filt & 3u;
  uint minf = ((filt >> 2) & 3u) == 3u ? (d3 >> 21) & 3u : (filt >> 2) & 3u;
  uint mipf = ((filt >> 4) & 3u) == 3u ? (d3 >> 23) & 3u : (filt >> 4) & 3u;
  vec2 size = vec2(dc.tsize[k] & 0xFFFFu, dc.tsize[k] >> 16) + 1.0;  // (the image's: no size query)
  // Magnified or minified decided as the texture unit does, from the footprint (and the biases), so a
  // texture sampled 1:1 takes its magnification filter: the sampler then uses that one filter.
  vec4 tg = unnormalized ? grad : grad * size.xyxy;
  float rho = max(dot(tg.xy, tg.xy), dot(tg.zw, tg.zw));
  float lod = reg_lod > -999.0 ? reg_lod : computed && rho > 0.0 ? 0.5 * log2(rho) : -100.0;
  lod += bias + float(int(dc.fetch[k * 6u + 4u] << 10) >> 22) / 32.0 + MIP_BIAS;
  bool linear = (lod > 0.0 ? minf : mag) == 1u;
  uint combo = (linear ? 3u : 0u) | (min(mipf, 2u) << 2);
  vec2 p = (unnormalized ? uv / size : uv) + offset / size;
  // Point sampling of the top level picks the texel the coordinate falls in exactly (floor), as the
  // memory decode does: on a texel's edge the sampler's fixed-point conversion could take its neighbour.
  if (!linear && (lod <= 0.0 || mipf == 2u)) p = (floor(p * size) + 0.5) / size;
  vec4 r;
  // Art magnified past the console's resolution (dc.sharp, renderer.cpp): Catmull-Rom (nine bilinear
  // taps) kept within the four nearest texels, so edges stay crisp without halos.
  // The game's font and XUI's glyph cache (dc.text): glyph coverage, cut at one half with about a screen
  // pixel's softening (the magnification from the footprint, else the resolution's scale).
  float glyph_mag = computed && rho > 0.0 ? exp2(-lod) : dc.window_offset.z;
  if (((dc.text >> k) & 1u) != 0u && linear && reg_lod <= -999.0 && glyph_mag > 1.4) {
    // Coverage through a cubic B-spline (four bilinear taps), not bilinear: the font's glyphs are 4-bit
    // stairs at 28 pixels (the 1's flag), which a cut of bilinear coverage magnifies; the spline rounds
    // them into smooth outlines. It thins strokes a little, so the cut is a bit under one half.
    uint ti = t & 0xFFFFu, si = (t >> 16) * 12u + 3u;
    vec2 sp = p * size - 0.5, tc = floor(sp), f = sp - tc, f2 = f * f, f3 = f2 * f;
    vec2 w0 = (1.0 - 3.0 * f + 3.0 * f2 - f3) / 6.0, w1 = (4.0 - 6.0 * f2 + 3.0 * f3) / 6.0;
    vec2 w2 = (1.0 + 3.0 * f + 3.0 * f2 - 3.0 * f3) / 6.0, w3 = f3 / 6.0;
    vec2 g0 = w0 + w1, g1 = w2 + w3;
    vec2 o0 = (tc - 0.5 + w1 / g0) / size, o1 = (tc + 1.5 + w3 / g1) / size;
#define GLYPH_TAP(x, y) textureLod(sampler2D(xtex2d[ti], xsamp[si]), vec2(x, y), 0.0)
    r = g0.y * (g0.x * GLYPH_TAP(o0.x, o0.y) + g1.x * GLYPH_TAP(o1.x, o0.y)) + g1.y * (g0.x * GLYPH_TAP(o0.x, o1.y) + g1.x * GLYPH_TAP(o1.x, o1.y));
#undef GLYPH_TAP
    float soft = clamp(0.55 / glyph_mag, 0.04, 0.42);
    r = smoothstep(vec4(0.42 - soft), vec4(0.42 + soft), r);  // (an 8-bit glyph cache holds coverage in whichever channel)
  } else
  if (((dc.sharp >> k) & 1u) != 0u && linear && computed && reg_lod <= -999.0 && lod < -0.5) {
    uint ti = t & 0xFFFFu, si = (t >> 16) * 12u + 3u;
#define SHARP_TAP(at) textureLod(sampler2D(xtex2d[ti], xsamp[si]), at, 0.0)
    vec2 sp = p * size, tc = floor(sp - 0.5) + 0.5, f = sp - tc;
    vec2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f)), w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    vec2 w2 = f * (0.5 + f * (2.0 - 1.5 * f)), w3 = f * f * (-0.5 + 0.5 * f), w12 = w1 + w2;
    vec2 t0 = (tc - 1.0) / size, t3 = (tc + 2.0) / size, t12 = (tc + w2 / w12) / size;
    r = (SHARP_TAP(vec2(t0.x, t0.y)) * w0.x + SHARP_TAP(vec2(t12.x, t0.y)) * w12.x + SHARP_TAP(vec2(t3.x, t0.y)) * w3.x) * w0.y +
        (SHARP_TAP(vec2(t0.x, t12.y)) * w0.x + SHARP_TAP(vec2(t12.x, t12.y)) * w12.x + SHARP_TAP(vec2(t3.x, t12.y)) * w3.x) * w12.y +
        (SHARP_TAP(vec2(t0.x, t3.y)) * w0.x + SHARP_TAP(vec2(t12.x, t3.y)) * w12.x + SHARP_TAP(vec2(t3.x, t3.y)) * w3.x) * w3.y;
    vec4 a = SHARP_TAP(tc / size), b = SHARP_TAP((tc + vec2(1.0, 0.0)) / size);
    vec4 c = SHARP_TAP((tc + vec2(0.0, 1.0)) / size), d = SHARP_TAP((tc + 1.0) / size);
#undef SHARP_TAP
    r = clamp(r, min(min(a, b), min(c, d)), max(max(a, b), max(c, d)));
  } else
  if (reg_lod > -999.0) r = textureLod(sampler2D(xtex2d[t & 0xFFFFu], xsamp[(t >> 16) * 12u + combo]), p, reg_lod + bias);
#ifdef XPIXEL
  else if (computed && dc.window_offset.w == 1.0)
    // One image pixel per surface pixel: the coordinates' own screen derivatives, as the hardware takes
    // them (explicit gradients sample at a fraction of the rate on NVIDIA's GPUs). Pixel shaders only.
    r = texture(sampler2D(xtex2d[t & 0xFFFFu], xsamp[(t >> 16) * 12u + combo]), p, bias);
#endif
  else if (computed) {
    vec4 g = (unnormalized ? grad / size.xyxy : grad) * exp2(bias);
    r = textureGrad(sampler2D(xtex2d[t & 0xFFFFu], xsamp[(t >> 16) * 12u + combo]), p, g.xy, g.zw);
  } else r = textureLod(sampler2D(xtex2d[t & 0xFFFFu], xsamp[(t >> 16) * 12u + combo]), p, 0.0);
  r = tex_swizzle(k, r);
#ifdef TEX_CHECK  // CRACKDOWN_TEX_CHECK (debug): magenta where the image and the decoded memory disagree
  vec4 d = tfetch2d(k, uv, unnormalized, filt, bias, computed ? grad : vec4(0.0), offset, reg_lod);
  if (any(greaterThan(abs(d - r), vec4(TEX_CHECK)))) return vec4(1.0, 0.0, 1.0, 1.0);
#endif
  return r;
}

// A light map is a few texels per wall: bilinear shows their grid as lumps along every shadow edge, so it
// is read through a cubic B-spline (four bilinear fetches). Textures with mipmaps read as usual.
vec4 tfetchsmooth(uint k, vec2 uv, uint filt, float bias, vec4 grad) {
  if (((dc.fetch[k * 6u + 4u] >> 6) & 15u) != 0u) return tfetch(k, uv, false, filt, bias, grad, true, vec2(0.0), -1000.0);
  uint d2 = dc.fetch[k * 6u + 2u];
  vec2 size = vec2(float((d2 & 0x1FFFu) + 1u), float(((d2 >> 13) & 0x1FFFu) + 1u));
  vec2 p = uv * size - 0.5, f = fract(p), i = floor(p) + 0.5;
  vec2 f2 = f * f, f3 = f2 * f;
  vec2 w0 = (1.0 - 3.0 * f + 3.0 * f2 - f3) / 6.0, w1 = (4.0 - 6.0 * f2 + 3.0 * f3) / 6.0;
  vec2 w2 = (1.0 + 3.0 * f + 3.0 * f2 - 3.0 * f3) / 6.0, w3 = f3 / 6.0;
  vec2 g0 = w0 + w1, g1 = w2 + w3, a = (i + w1 / g0 - 1.0) / size, b = (i + 1.0 + w3 / g1) / size;
  vec4 r = vec4(0.0);
  r += g0.x * g0.y * tfetch(k, a, false, filt, bias, grad, true, vec2(0.0), -1000.0);
  r += g1.x * g0.y * tfetch(k, vec2(b.x, a.y), false, filt, bias, grad, true, vec2(0.0), -1000.0);
  r += g0.x * g1.y * tfetch(k, vec2(a.x, b.y), false, filt, bias, grad, true, vec2(0.0), -1000.0);
  r += g1.x * g1.y * tfetch(k, b, false, filt, bias, grad, true, vec2(0.0), -1000.0);
  return r;
}

// Cube map fetch: c = (s, t, face) as the shader's cube instruction (xcube) and its scaling leave them,
// s and t in [1, 2] on the face: that face's texel at (s - 1, t - 1), sampled from the faces as layers
// (dc.tex[k] as tfetch's, its index in xtexcube). grad: the derivatives of (s, t).
vec4 tfetchcube(uint k, vec3 c, uint filt, float bias, vec4 grad, bool computed, float reg_lod) {
  uint t = dc.tex[k], d3 = dc.fetch[k * 6u + 3u];
#ifdef TEX_DECODED_FALLBACK
  if (t == 0xFFFFFFFFu) return tfetch2d(k, c.xy, false, filt, bias, computed ? grad : vec4(0.0), vec2(0.0), reg_lod);
#endif
  uint mag = (filt & 3u) == 3u ? (d3 >> 19) & 3u : filt & 3u;
  uint minf = ((filt >> 2) & 3u) == 3u ? (d3 >> 21) & 3u : (filt >> 2) & 3u;
  uint mipf = ((filt >> 4) & 3u) == 3u ? (d3 >> 23) & 3u : (filt >> 4) & 3u;
  vec2 size = vec2(dc.tsize[k] & 0xFFFFu, dc.tsize[k] >> 16) + 1.0;
  vec4 tg = grad * size.xyxy;
  float rho = max(dot(tg.xy, tg.xy), dot(tg.zw, tg.zw));
  float lod = reg_lod > -999.0 ? reg_lod : computed && rho > 0.0 ? 0.5 * log2(rho) : -100.0;
  lod += bias + float(int(dc.fetch[k * 6u + 4u] << 10) >> 22) / 32.0 + MIP_BIAS;
  bool linear = (lod > 0.0 ? minf : mag) == 1u;
  uint combo = (linear ? 3u : 0u) | (min(mipf, 2u) << 2);
  vec3 p = vec3(c.xy - 1.0, clamp(floor(c.z + 0.5), 0.0, 5.0));
  vec4 r;
  if (reg_lod > -999.0) r = textureLod(sampler2DArray(xtexcube[t & 0xFFFFu], xsamp[(t >> 16) * 12u + combo]), p, reg_lod + bias);
  else if (computed) {
    vec4 g = grad * exp2(bias);
    r = textureGrad(sampler2DArray(xtexcube[t & 0xFFFFu], xsamp[(t >> 16) * 12u + combo]), p, g.xy, g.zw);
  } else r = textureLod(sampler2DArray(xtexcube[t & 0xFFFFu], xsamp[(t >> 16) * 12u + combo]), p, 0.0);
  return tex_swizzle(k, r);
}

// A stacked 2D texture (a 3D fetch on a 2D texture with layers: the sun's shadow cascades) as layers of
// an array image (dc.tex[k]'s index in xtexcube), layer by the third coordinate (stack_layer); otherwise
// (true volumes, formats without an image) decoded from guest memory as before.
vec4 tfetchstack(uint k, vec2 uv, float z, bool unnormalized, uint filt, float bias, vec4 grad, bool computed, vec2 offset, float reg_lod) {
#ifdef PERF_NOTEX_MASK
  if (((PERF_NOTEX_MASK >> k) & 1u) != 0u) return vec4(0.5);
#endif
  uint t = dc.tex[k], d3 = dc.fetch[k * 6u + 3u];
  if (t == 0xFFFFFFFFu) { tex_layer = stack_layer(k, z); return tfetch2d(k, uv, unnormalized, filt, bias, computed ? grad : vec4(0.0), offset, reg_lod); }
  uint mag = (filt & 3u) == 3u ? (d3 >> 19) & 3u : filt & 3u;
  uint minf = ((filt >> 2) & 3u) == 3u ? (d3 >> 21) & 3u : (filt >> 2) & 3u;
  uint mipf = ((filt >> 4) & 3u) == 3u ? (d3 >> 23) & 3u : (filt >> 4) & 3u;
  vec2 size = vec2(dc.tsize[k] & 0xFFFFu, dc.tsize[k] >> 16) + 1.0;
  vec4 tg = unnormalized ? grad : grad * size.xyxy;
  float rho = max(dot(tg.xy, tg.xy), dot(tg.zw, tg.zw));
  float lod = reg_lod > -999.0 ? reg_lod : computed && rho > 0.0 ? 0.5 * log2(rho) : -100.0;
  lod += bias + float(int(dc.fetch[k * 6u + 4u] << 10) >> 22) / 32.0 + MIP_BIAS;
  bool linear = (lod > 0.0 ? minf : mag) == 1u;
  uint combo = (linear ? 3u : 0u) | (min(mipf, 2u) << 2);
  vec2 p = (unnormalized ? uv / size : uv) + offset / size;
  if (!linear && (lod <= 0.0 || mipf == 2u)) p = (floor(p * size) + 0.5) / size;
  vec3 q = vec3(p, float(stack_layer(k, z)));
  vec4 r;
  if (reg_lod > -999.0) r = textureLod(sampler2DArray(xtexcube[t & 0xFFFFu], xsamp[(t >> 16) * 12u + combo]), q, reg_lod + bias);
  else if (computed) {
    vec4 g = (unnormalized ? grad / size.xyxy : grad) * exp2(bias);
    r = textureGrad(sampler2DArray(xtexcube[t & 0xFFFFu], xsamp[(t >> 16) * 12u + combo]), q, g.xy, g.zw);
  } else r = textureLod(sampler2DArray(xtexcube[t & 0xFFFFu], xsamp[(t >> 16) * 12u + combo]), q, 0.0);
  return tex_swizzle(k, r);
}

vec4 xcube(vec4 a, vec4 b) {
  // The direction (x, y, z) comes as a = (z, z, x, y), b = (y, x, z, z): the shaders write it as
  // v.zzxy, v.yxzz (or v.wwyz, v.zyww for a direction in .yzw). Result (t, s, 2 * major axis, face).
  vec3 d = vec3(a.z, a.w, a.x);
  vec3 ad = abs(d);
  float ma; float sc; float tc; float face;
  if (ad.z >= ad.x && ad.z >= ad.y) { ma = d.z; face = d.z < 0.0 ? 5.0 : 4.0; sc = d.z < 0.0 ? -d.x : d.x; tc = -d.y; }
  else if (ad.y >= ad.x) { ma = d.y; face = d.y < 0.0 ? 3.0 : 2.0; sc = d.x; tc = d.y < 0.0 ? -d.z : d.z; }
  else { ma = d.x; face = d.x < 0.0 ? 1.0 : 0.0; sc = d.x < 0.0 ? d.z : -d.z; tc = -d.y; }
  return vec4(tc, sc, 2.0 * abs(ma), face);
}
#define XRET return
)GLSL";

inline const std::string kPrelude = std::string( kPreludeHead ) + kPreludeCommon + kPreludeTail;


// Vertex shader entry: maps the draw's vertex number to an index (index buffer, primitive expansion
// for rectangle and quad lists), runs the translated shader and converts its position to Vulkan NDC.
inline const char *kVertexMain = R"GLSL(
layout(location = 0) out vec4 v_interp[16];
layout(location = 16) out vec3 v_mvprev;                // the place a frame ago times its w (0: none)
layout(location = 17) noperspective out vec2 v_mvnow;   // the place now
layout(location = 18) out float v_mvrel;                // how far its own move is from the camera's (surface pixels)
// The same position from different shaders: a depth pre-pass and the colour pass after it (depth test
// GEQUAL) compute it with the same instructions, and the colour pass must land on exactly the same depth.
// Without this the driver fuses multiplies and adds differently per shader (walls drew black).
invariant gl_Position;
out float gl_ClipDistance[6];

// A position's place for DLSS's motion: window coordinates (surface pixels) without the jitter, and its w
// (the viewport transform below, kept in step with it).
vec3 mv_place(vec4 pos) {
  float w = (dc.vte & 0x400u) != 0u ? pos.w : 1.0 / pos.w;
  if (w == 0.0 || isinf(w) || isnan(w)) w = 1.0;
  vec2 n = pos.xy;
  if ((dc.vte & 0x100u) == 0u) n /= w;
  if ((dc.vte & 1u) != 0u) n.x *= dc.vp_scale.x;
  if ((dc.vte & 2u) != 0u) n.x += dc.vp_offset.x;
  if ((dc.vte & 4u) != 0u) n.y *= dc.vp_scale.y;
  if ((dc.vte & 8u) != 0u) n.y += dc.vp_offset.y;
  return vec3(n + dc.window_offset.xy - vec2(dc.mv_jx, dc.mv_jy), w);
}
// Its place a frame ago (as xy * w, w for interpolating; 0: none) after this frame's goes to slot i.
vec3 mv_history(uint i, vec3 now) {
  if (dc.mv_cur == 0xFFFFFFFFu) return vec3(0.0);
  mvpos[dc.mv_cur + i] = vec4(now, 0.0);
  if (dc.mv_prev == 0xFFFFFFFFu) return vec3(0.0);
  vec4 p = mvpos[dc.mv_prev + i];
  return p.z > 0.0 ? vec3(p.xy * p.z, p.z) : vec3(0.0);
}

uint fetch_index(uint i) {
  i += dc.first;
  uint v;
  if (dc.index_mode == 0u) {
    uint e = (dc.index_info >> 4) & 3u;
    if ((dc.index_info & 1u) != 0u) v = ld(dc.index_addr + i * 4u, e);
    else { uint w = ld(dc.index_addr + (i & ~1u) * 2u, e); v = (i & 1u) != 0u ? (w >> 16) : (w & 0xFFFFu); }
  } else v = i;
  return v + dc.index_offset;
}

void run(uint n, out vec4 pos, out vec4 o[16], out float psize) { xvs(fetch_index(n), pos, o, psize); }

// The translated shader is called from only two places (it is inlined at each, and a big shader
// with texture fetches inlined more often makes the driver compiler take minutes).
void main() {
  uint k = uint(gl_VertexIndex);
  vec4 pos; vec4 o[16]; float psize;
  uint mvi = 0xFFFFFFFFu;  // the vertex's number for DLSS's motion
  bool mv_done = false;    // (a rectangle's corners: done here)
  v_mvprev = vec3(0.0);
  if (dc.prim == 8u) {          // rectangle list: 3 corners per rectangle; the missing corner is opposite
    uint rect = k / 6u, c = k % 6u; // the right-angle vertex (the one not on the longest edge)
    vec4 p[3]; vec4 oo[3][16]; float s = 1.0;
    [[dont_unroll]] for (uint i = 0u; i < 3u; ++i) { vec4 t[16]; vec4 q; run(rect * 3u + i, q, t, s); p[i] = q; oo[i] = t; }
    float d01 = distance(p[0].xy, p[1].xy), d02 = distance(p[0].xy, p[2].xy), d12 = distance(p[1].xy, p[2].xy);
    uint corner = d12 >= d01 && d12 >= d02 ? 0u : d02 >= d01 ? 1u : 2u;
    uint a = corner == 0u ? 1u : 0u, b = corner == 2u ? 1u : 2u;
    psize = s;
    if (c < 3u) { pos = p[c]; o = oo[c]; }
    else if (c == 3u) { pos = p[a]; o = oo[a]; }
    else if (c == 4u) { pos = p[b]; o = oo[b]; }
    else {
      pos = p[a] + p[b] - p[corner];
      for (int i = 0; i < 16; ++i) o[i] = oo[a][i] + oo[b][i] - oo[corner][i];
    }
    // DLSS's motion: the three corners' places kept, the fourth's made from theirs as its position is.
    if (dc.mv_cur != 0xFFFFFFFFu) {
      vec3 h[3];
      for (uint i = 0u; i < 3u; ++i) h[i] = mv_history(rect * 3u + i, mv_place(p[i]));
      bool all = h[0].z > 0.0 && h[1].z > 0.0 && h[2].z > 0.0;
      v_mvprev = !all ? vec3(0.0) : c < 3u ? h[c] : c == 3u ? h[a] : c == 4u ? h[b] : h[a] + h[b] - h[corner];
    }
    mv_done = true;
  } else {
    uint v = k;
    if (dc.prim == 13u) {         // quad list: 0 1 2, 0 2 3
      uint quad = k / 6u, c = k % 6u;
      v = quad * 4u + (c == 0u || c == 3u ? 0u : c == 1u ? 1u : c == 2u || c == 4u ? 2u : 3u);
    } else if (dc.prim == 6u) {   // triangle strip as a list; the instance number = where this strip
      uint t = k / 3u, c = k % 3u; // starts in the index buffer (strips split at the reset index)
      v = uint(gl_InstanceIndex) + ((t & 1u) == 0u ? t + c : (c == 0u ? t + 1u : c == 1u ? t : t + 2u));
    } else if (dc.prim == 5u) {   // triangle fan as a list
      uint t = k / 3u, c = k % 3u;
      v = c == 0u ? 0u : t + c;
    }
    mvi = v;
    run(v, pos, o, psize);
  }

  // Xenos viewport: VTE bits 0-5 enable scale/offset per axis; bit 8: xy already divided by w,
  // bit 9: z already divided, bit 10 (VTX_W0_FMT): w is w itself; clear, the shader wrote 1/w
  // (gameplay's world shaders set it with an ordinary clip-space w of hundreds).
  float w = (dc.vte & 0x400u) != 0u ? pos.w : 1.0 / pos.w;
  if (w == 0.0 || isinf(w) || isnan(w)) w = 1.0;
  vec3 ndc = pos.xyz;
  if ((dc.vte & 0x100u) == 0u) ndc.xy /= w;
  if ((dc.vte & 0x200u) == 0u) ndc.z /= w;
  vec3 win = ndc;
  if ((dc.vte & 1u) != 0u) win.x *= dc.vp_scale.x;
  if ((dc.vte & 2u) != 0u) win.x += dc.vp_offset.x;
  if ((dc.vte & 4u) != 0u) win.y *= dc.vp_scale.y;
  if ((dc.vte & 8u) != 0u) win.y += dc.vp_offset.y;
  if ((dc.vte & 16u) != 0u) win.z *= dc.vp_scale.z;
  if ((dc.vte & 32u) != 0u) win.z += dc.vp_offset.z;
  win.xy += dc.window_offset.xy;
  v_mvnow = win.xy - vec2(dc.mv_jx, dc.mv_jy);
  if (!mv_done && mvi != 0xFFFFFFFFu) v_mvprev = mv_history(mvi, vec3(v_mvnow, w));
  v_mvrel = 1e6;
  if (dc.mv_cam != 0u && v_mvprev.z > 0.0) {
    vec4 cq = dc.mv_m * vec4(ndc, 1.0);
    vec2 cam = cq.w > 0.0 ? (cq.xy / cq.w - ndc.xy) * dc.vp_scale.xy : vec2(1e6);
    v_mvrel = length((v_mvprev.xy / v_mvprev.z - v_mvnow) - cam);
  }
  // Direct3D 9 pixel centres (PA_SU_VTX_CNTL bit 0 clear, renderer bit 16): pixel i's centre is at window
  // coordinate i, where Vulkan's is at i + 0.5, so the geometry moves half a pixel to cover the same
  // pixels. Unmoved, a full-target quad at the usual -0.5 offset left its last row and column undrawn
  // (stale memory: the ocean's wave FFT spread the NaNs left there over the whole sea).
  if ((dc.vte & 0x10000u) != 0u) win.xy += 0.5;
  vec2 clip = win.xy * dc.rt.zw * 2.0 - 1.0;
  gl_Position = vec4(clip * w, ndc.z * w, w);  // depth before the viewport z mapping (the viewport depth range does it)
  // User clip planes, against the clip-space position the shader wrote (kept where the dot product >= 0).
  vec4 cpos = vec4(ndc * w, w);
  for (int i = 0; i < 6; ++i) gl_ClipDistance[i] = ((dc.ucp_ena >> uint(i)) & 1u) != 0u ? dot(dc.ucp[i], cpos) : 1.0;
  if ((dc.flags & 512u) != 0u && k < 64u) {  // debug: record position, window coords, o0 and o1 (physical 0x1FF00000)
    uint d = (0x1FF00000u >> 2) + k * 16u;
    mem[d] = floatBitsToUint(pos.x); mem[d + 1u] = floatBitsToUint(pos.y); mem[d + 2u] = floatBitsToUint(pos.z); mem[d + 3u] = floatBitsToUint(pos.w);
    mem[d + 4u] = floatBitsToUint(win.x); mem[d + 5u] = floatBitsToUint(win.y);
    for (uint i = 0u; i < 4u; ++i) { mem[d + 6u + i] = floatBitsToUint(o[0][i]); mem[d + 10u + i] = floatBitsToUint(o[1][i]); }
  }
  gl_PointSize = dc.window_offset.z;  // one surface pixel (anti-aliasing: image pixels per surface pixel)
  v_interp = o;
}
)GLSL";

inline const char *kPixelMain = R"GLSL(
// Shaders that neither discard nor write depth have their depth test before them (as they would anyway
// without DLSS's motion writes, which would otherwise make the driver test after them); only they write
// motion.
#if !defined(XWRITES_DEPTH) && !defined(XDISCARDS)
layout(early_fragment_tests) in;
#define XMOTION 1
#endif
layout(location = 0) in vec4 v_interp[16];
layout(location = 16) in vec3 v_mvprev;
layout(location = 17) noperspective in vec2 v_mvnow;
layout(location = 18) in float v_mvrel;
layout(location = 0) out vec4 out0;
layout(location = 1) out vec4 out1;
layout(location = 2) out vec4 out2;
layout(location = 3) out vec4 out3;

void main() {
  vec4 oc[4]; float depth;
  xps(v_interp, oc, depth);
  out0 = oc[0]; out1 = oc[1]; out2 = oc[2]; out3 = oc[3];
#ifdef XWRITES_DEPTH
  gl_FragDepth = depth;  // only shaders that export depth (writing it costs early depth testing)
#endif
  if ((dc.flags & 256u) != 0u) out0 = vec4(1.0, 0.0, 1.0, 1.0);  // CRACKDOWN_DEBUG_MAGENTA
  // DLSS's motion: the nearest surface's (reversed depth: the largest) wins the pixel.
#ifdef XMOTION
  // Only where it moves on its own (by half a pixel or more against the camera's move).
  if ((dc.mv_pix & 3u) != 0u && v_mvprev.z > 0.0 && v_mvrel >= 0.5) {
    vec2 mv = (v_mvprev.xy / v_mvprev.z - v_mvnow) * dc.window_offset.z;
    uvec2 px = uvec2(gl_FragCoord.xy);
    if (px.x < dc.mv_w && px.y < dc.mv_h && all(lessThan(abs(mv), vec2(2048.0)))) {
#ifdef XWRITES_DEPTH
      float z = depth;
#else
      float z = gl_FragCoord.z;
#endif
      if ((dc.mv_pix & 4u) != 0u) z = 1.0;  // (no depth test: over everything)
      uint at = ((dc.mv_pix & 2u) != 0u ? MVPIX : 0u) + px.y * dc.mv_w + px.x;
      atomicMax(mvpix[at], (uint64_t(floatBitsToUint(max(z, 0.0))) << 32) | uint64_t(packHalf2x16(mv)));
    }
  }
  // DLSS's see-through mask: where a see-through drawing (blended, no depth written: light streaks, glows,
  // smoke) shows, a bit after mvpix's two halves; DLSS takes those pixels as they are now (no history to
  // smear them). Its strength: its colour times its alpha (bit 4: added on, its alpha not used).
  if ((dc.mv_pix & 8u) != 0u && max(out0.r, max(out0.g, out0.b)) * ((dc.mv_pix & 16u) != 0u ? 1.0 : out0.a) > 0.04) {
    uvec2 px = uvec2(gl_FragCoord.xy);
    if (px.x < dc.mv_w && px.y < dc.mv_h) {
      uint i = px.y * dc.mv_w + px.x;
      atomicOr(mvpix[2u * MVPIX + (i >> 6)], uint64_t(1) << (i & 63u));
    }
  }
#endif
}
)GLSL";

} // namespace xlate
