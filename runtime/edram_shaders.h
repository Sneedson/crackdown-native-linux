// The renderer's own compute shaders (render-target copies and resolves), GLSL. Shared with
// devtools/shader_aot, which compiles them ahead of time with the game's shaders.
#pragma once
#include <cstring>
#include <string>

// ------------------------------------------------------------------ render-target compute shaders
// Render targets are host images (renderer.cpp "Render targets"). Where the game reads, through one
// surface, memory another surface wrote -- surfaces are ranges of the console's EDRAM, and the game
// places them to share it on purpose -- the other surface's pixels are converted into it directly,
// following the console's sample layout: pixel (x, y) sample s of a surface at `base` with `pitch`
// pixels per row sits at edram_index(); 2x MSAA stacks two samples vertically, 4x makes a 2x2 block,
// a 64bpp tile holds 40x16 samples of two words. A surface's pixels are read as the words the console
// keeps (src_bits: the host image's bits re-encoded per kind; renderer.cpp XferKind). A surface's image
// has scale / 8 pixels per surface pixel across and down (renderer.cpp Target::scale8: anti-aliasing 2, or
// the resolution's 1 to 3 in eighths): surface pixel x covers image pixels [ceil(x s), ceil((x + 1) s)),
// image pixel i belongs to pixel floor(i / s). A console sample is read at its place in the pixel (2x: the
// upper and lower halves; 4x: the quarters).
#define EDRAM_GLSL R"GLSL(
layout(constant_id = 0) const uint EDRAM_TILE_MASK = 2047u;  // tiles - 1 (renderer.cpp EDRAM_TILES)
uint edram_index(uint base, uint pitch, uint msaa, uint bpp64, uint x, uint y, uint s) {
  uint sx = msaa >= 2u ? 2u : 1u, sy = msaa >= 1u ? 2u : 1u;
  uint xs = x * sx + (msaa >= 2u ? (s & 1u) : 0u);
  uint ys = y * sy + (msaa >= 2u ? (s >> 1) : (msaa == 1u ? s : 0u));
  uint tw = bpp64 != 0u ? 40u : 80u;
  uint pitch_tiles = max((pitch * sx + tw - 1u) / tw, 1u);
  uint tile = (base + (ys >> 4) * pitch_tiles + xs / tw) & EDRAM_TILE_MASK;
  return tile * 1280u + (ys & 15u) * 80u + (xs % tw) * (bpp64 != 0u ? 2u : 1u);
}
uint to_7e3(float f) {
  f = clamp(f, 0.0, 31.875);
  if (f < 0.25) return min(uint(f * 512.0 + 0.5), 127u);
  int e = clamp(int(floor(log2(f))), -2, 4);
  uint m = uint((f / exp2(float(e)) - 1.0) * 128.0 + 0.5);
  uint ee = uint(e + 3);
  if (m >= 128u) { m = 0u; ee++; }
  return ee > 7u ? 0x3FFu : (ee << 7) | m;
}
float from_7e3(uint v) {
  uint e = (v >> 7) & 7u, m = v & 127u;
  return e == 0u ? float(m) / 512.0 : (1.0 + float(m) / 128.0) * exp2(float(e) - 3.0);
}
// The surface read: colour as its host bits (an R32_UINT / R32G32_UINT view), depth and stencil.
layout(set = 0, binding = 3) uniform usampler2D src_raw;
layout(set = 0, binding = 4) uniform sampler2D src_depth;
layout(set = 0, binding = 5) uniform usampler2D src_stencil;
ivec2 src_size(uint kind) { return kind == 4u ? textureSize(src_depth, 0) : textureSize(src_raw, 0); }
// Pixel p of the surface as the console's words (lo, hi), kind: 0 8_8_8_8, 1 other 32bpp, 2 64bpp,
// 3 2_10_10_10_FLOAT (held as RGBA16F), 4 depth / stencil.
uvec2 src_bits(uint kind, ivec2 p) {
  if (kind == 4u) {
    float d = texelFetch(src_depth, p, 0).r;
    uint st = texelFetch(src_stencil, p, 0).r;
    return uvec2((uint(clamp(d, 0.0, 1.0) * 16777215.0 + 0.5) << 8) | (st & 255u), 0u);
  }
  uvec2 v = texelFetch(src_raw, p, 0).xy;
  if (kind == 0u) return uvec2((v.x & 0xFF00FF00u) | ((v.x & 0xFFu) << 16) | ((v.x >> 16) & 0xFFu), 0u);  // RGBA8 -> ARGB
  if (kind == 1u) return uvec2(v.x, 0u);
  if (kind == 2u) return v;
  vec2 a = unpackHalf2x16(v.x), b = unpackHalf2x16(v.y);
  return uvec2(to_7e3(a.x) | (to_7e3(a.y) << 10) | (to_7e3(b.x) << 20) | (uint(clamp(b.y, 0.0, 1.0) * 3.0 + 0.5) << 30), 0u);
}
)GLSL"

// Surface -> surface: the rectangle (x0, y0, w, h) of the destination surface, where its tiles are in
// the mask, takes the source surface's words there, written to the staging buffer in the destination's
// host layout (colour: its texels; depth: floats, then the stencil bytes at stencil_off), which the
// renderer copies into the destination image.
inline const char *kCopyShader = "#version 450\n"
"layout(local_size_x = 8, local_size_y = 8) in;\n"
"layout(std430, set = 0, binding = 1) buffer Stage { uint stage[]; };\n"
"layout(std430, set = 0, binding = 2) readonly buffer Mask { uint mask[]; };\n"
"layout(push_constant) uniform P { uint base, pitch, msaa, kind, bpp64; int x0, y0, w, h; uint stencil_off;\n"
"                                  uint sbase, spitch, smsaa, skind, sbpp64, scale, sscale; } pc;\n"
EDRAM_GLSL R"GLSL(
// EDRAM word e as the source surface holds it.
uint src_word(uint e) {
  uint tile = e / 1280u, within = e % 1280u, row = within / 80u, col = within % 80u;
  uint sx = pc.smsaa >= 2u ? 2u : 1u, sy = pc.smsaa >= 1u ? 2u : 1u;
  uint tw = pc.sbpp64 != 0u ? 40u : 80u;
  uint pitch_tiles = max((pc.spitch * sx + tw - 1u) / tw, 1u);
  uint rel = (tile - pc.sbase) & EDRAM_TILE_MASK;
  uint xs = (rel % pitch_tiles) * tw + (pc.sbpp64 != 0u ? col >> 1 : col), ys = (rel / pitch_tiles) * 16u + row;
  ivec2 p = ivec2(((2u * xs + 1u) * pc.sscale) / (16u * sx), ((2u * ys + 1u) * pc.sscale) / (16u * sy)), size = src_size(pc.skind);
  if (p.x >= size.x || p.y >= size.y) return 0u;
  uvec2 b = src_bits(pc.skind, p);
  return pc.sbpp64 != 0u && (col & 1u) != 0u ? b.y : b.x;
}
void main() {
  ivec2 q = ivec2(gl_GlobalInvocationID.xy);
  if (q.x >= pc.w || q.y >= pc.h) return;
  uint ix = uint(q.x + pc.x0), iy = uint(q.y + pc.y0);  // image pixel
  uint x = ix * 8u / pc.scale, y = iy * 8u / pc.scale;
  uint sub_x = (ix * 8u - x * pc.scale) * 2u / pc.scale, sub_y = (iy * 8u - y * pc.scale) * 2u / pc.scale;
  uint e = edram_index(pc.base, pc.pitch, pc.msaa, pc.bpp64, x, y, pc.msaa >= 2u ? sub_y * 2u + sub_x : pc.msaa == 1u ? sub_y : 0u);
  uint tile = e / 1280u;
  if ((mask[tile >> 5] & (1u << (tile & 31u))) == 0u) return;
  uint lo = src_word(e), hi = pc.bpp64 != 0u ? src_word(e + 1u) : 0u;
  uint i = uint(q.y * pc.w + q.x);
  if (pc.kind == 0u) stage[i] = (lo & 0xFF00FF00u) | ((lo & 0xFFu) << 16) | ((lo >> 16) & 0xFFu);
  else if (pc.kind == 1u) stage[i] = lo;
  else if (pc.kind == 2u) { stage[2u * i] = lo; stage[2u * i + 1u] = hi; }
  else if (pc.kind == 3u) {
    stage[2u * i] = packHalf2x16(vec2(from_7e3(lo & 1023u), from_7e3((lo >> 10) & 1023u)));
    stage[2u * i + 1u] = packHalf2x16(vec2(from_7e3((lo >> 20) & 1023u), float(lo >> 30) / 3.0));
  } else {
    stage[i] = floatBitsToUint(float(lo >> 8) / 16777215.0);
    uint w = pc.stencil_off / 4u + i / 4u, sh = (i & 3u) * 8u;
    atomicAnd(stage[w], ~(255u << sh));
    atomicOr(stage[w], (lo & 255u) << sh);
  }
}
)GLSL";

// Resolve: a surface's rectangle -> guest memory (tiled or linear) in the destination format.
inline const char *kResolveShader = "#version 450\n"
"#ifdef BLOCKED\nlayout(local_size_x = 256) in;\n#else\nlayout(local_size_x = 8, local_size_y = 8) in;\n#endif\n"
"layout(std430, set = 0, binding = 0) buffer Mem { uint mem[]; };\n"
"layout(push_constant) uniform P {\n"
"  uint dest_base, dest_pitch, dest_height, info;   // info: endian 0-2, linear bit 3, format 7-12, swap bit 24\n"
"  uint src_base, src_pitch, src_msaa, src_format;  // src_format: colour render-target format, 0x100 = depth (0x101: 24-bit float)\n"
"  int src_x, src_y, dst_x, dst_y, width, height;\n"
"  uint src_scale, sample_select;  // image pixels per surface pixel in eighths; RB_COPY_CONTROL sample select (0-3 one, 4-6 averaged)\n"
"} pc;\n"
EDRAM_GLSL R"GLSL(
uint bswap32(uint v) { return (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24); }
uint xswap(uint v, uint e) {
  if (e == 1u) return ((v & 0x00FF00FFu) << 8) | ((v >> 8) & 0x00FF00FFu);
  if (e == 2u) return bswap32(v);
  if (e == 3u) return (v << 16) | (v >> 16);
  return v;
}
uint tiled_offset(uint x, uint y, uint width, uint log_bpp) {
  uint aligned = (width + 31u) & ~31u;
  uint macro = ((x >> 5) + (y >> 5) * (aligned >> 5)) << (log_bpp + 7u);
  uint micro = ((x & 7u) + ((y & 6u) << 2)) << log_bpp;
  uint offset = macro + ((micro & ~15u) << 1) + (micro & 15u) + ((y & 8u) << (3u + log_bpp)) + ((y & 1u) << 4);
  return ((((offset & ~511u) << 3) + ((offset & 448u) << 2) + (offset & 63u) + ((y & 16u) << 7) +
          (((((y & 8u) >> 2) + (x >> 3)) & 3u) << 6)) >> log_bpp);
}
// 20e4 float (no sign, 4-bit exponent biased by 15, 20-bit mantissa) as in D24FS8 depth.
uint to_20e4(float f) {
  f = clamp(f, 0.0, 1.9999998);
  uint b = floatBitsToUint(f);
  int e = int((b >> 23) & 255u) - 112;  // 127 - 15
  if (e <= 0) return e < -20 ? 0u : (((b & 0x7FFFFFu) | 0x800000u) >> uint(4 - e));
  return (uint(e) << 20) | ((b >> 3) & 0xFFFFFu);
}
vec4 decode(uint f, uint lo, uint hi) {
  if (f <= 1u) return vec4((lo >> 16) & 255u, (lo >> 8) & 255u, lo & 255u, lo >> 24) / 255.0;
  if (f == 2u || f == 10u) return vec4(vec3(lo & 1023u, (lo >> 10) & 1023u, (lo >> 20) & 1023u) / 1023.0, float(lo >> 30) / 3.0);
  if (f == 3u || f == 12u) return vec4(from_7e3(lo & 1023u), from_7e3((lo >> 10) & 1023u), from_7e3((lo >> 20) & 1023u), float(lo >> 30) / 3.0);
  if (f == 4u || f == 6u) return vec4(unpackHalf2x16(lo), 0.0, 1.0);
  if (f == 5u || f == 7u) return vec4(unpackHalf2x16(lo), unpackHalf2x16(hi));
  if (f == 14u) return vec4(uintBitsToFloat(lo), 0.0, 0.0, 1.0);
  if (f == 15u) return vec4(uintBitsToFloat(lo), uintBitsToFloat(hi), 0.0, 1.0);
  return vec4(0.0);
}
// Pixel p of the rectangle: its destination dword index w (w + 1 too when wide) and values.
bool resolve_pixel(uvec2 p, out uint w, out uint o0, out uint o1) {
  if (int(p.x) >= pc.width || int(p.y) >= pc.height) return false;
  bool depth = (pc.src_format & ~1u) == 0x100u;
  uint bpp64 = (!depth && (pc.src_format == 5u || pc.src_format == 7u || pc.src_format == 15u)) ? 1u : 0u;
  uint kind = depth ? 4u : pc.src_format <= 1u ? 0u : (pc.src_format == 3u || pc.src_format == 12u) ? 3u : bpp64 != 0u ? 2u : 1u;
  uvec2 q = uvec2(ivec2(p) + ivec2(pc.src_x, pc.src_y)), size = uvec2(src_size(kind));
  uvec2 first = (q * pc.src_scale + 7u) / 8u, last = ((q + 1u) * pc.src_scale + 7u) / 8u;  // the pixel's image pixels
  ivec2 sp = ivec2(first);
  if (first.x >= size.x || first.y >= size.y) return false;
  // Above one image pixel per pixel: averaging the samples averages the pixel's image pixels; one sample
  // is the image pixel at its place in the pixel.
  bool average = pc.src_scale > 8u && !depth && pc.sample_select >= 4u;
  vec4 sum = vec4(0.0);
  float count = 0.0;
  if (average)
    for (uint y = first.y; y < min(last.y, size.y); ++y)
      for (uint x = first.x; x < min(last.x, size.x); ++x) {
        uvec2 bj = src_bits(kind, ivec2(x, y));
        sum += decode(pc.src_format, bj.x, bj.y);
        count += 1.0;
      }
  else if (pc.src_scale > 8u) {
    uint n = pc.sample_select & 3u;
    uvec2 per = uvec2(pc.src_msaa >= 2u ? 2u : 1u, pc.src_msaa >= 1u ? 2u : 1u);
    uvec2 sub = uvec2(pc.src_msaa >= 2u ? n & 1u : 0u, pc.src_msaa >= 2u ? n >> 1 : pc.src_msaa == 1u ? n & 1u : 0u);
    sp = ivec2(min(((2u * (q * per + sub) + 1u) * pc.src_scale) / (16u * per), size - 1u));
  }
  uvec2 b = src_bits(kind, sp);
  uint lo = b.x, hi = b.y;
  uint format = (pc.info >> 7) & 63u;
  uint v0, v1 = 0u;
  bool wide = format == 32u || format == 37u;
  if (depth) {
    // Depth is kept as 24-bit unorm; a float depth buffer (D24FS8) reads back as 20e4 float.
    float d = float(lo >> 8) / 16777215.0;
    if (format == 36u) v0 = floatBitsToUint(d);
    else if (pc.src_format == 0x101u) v0 = (to_20e4(d) << 8) | (lo & 255u);
    else v0 = lo;
  } else {
    vec4 c = average ? sum / max(count, 1.0) : decode(pc.src_format, lo, hi);
    if ((pc.info & (1u << 24)) != 0u) c = c.bgra;   // swap red and blue
    if (format == 7u) { uvec4 q = uvec4(clamp(c, 0.0, 1.0) * vec4(1023.0, 1023.0, 1023.0, 3.0) + 0.5); v0 = q.x | (q.y << 10) | (q.z << 20) | (q.w << 30); }
    else if (format == 31u) v0 = packHalf2x16(c.xy);
    else if (format == 32u) { v0 = packHalf2x16(c.xy); v1 = packHalf2x16(c.zw); }
    else if (format == 36u) v0 = floatBitsToUint(c.x);
    else if (format == 37u) { v0 = floatBitsToUint(c.x); v1 = floatBitsToUint(c.y); }
    else { uvec4 q = uvec4(clamp(c, 0.0, 1.0) * 255.0 + 0.5); v0 = q.x | (q.y << 8) | (q.z << 16) | (q.w << 24); }
  }
  if ((pc.info & 0x80000000u) != 0u) v0 = 0xFF00FFFFu;  // CRACKDOWN_DEBUG_RESOLVE test colour
  bool linear = (pc.info & 8u) != 0u;
  uvec2 d = p + uvec2(pc.dst_x, pc.dst_y);
  if (d.x >= pc.dest_pitch || d.y >= max(pc.dest_height, 1u)) return false;
  uint lb = wide ? 3u : 2u;
  uint idx = linear ? d.y * pc.dest_pitch + d.x : tiled_offset(d.x, d.y, pc.dest_pitch, lb);
  w = ((pc.dest_base & 0x1FFFFFFFu) >> 2) + (wide ? idx * 2u : idx);
  o0 = xswap(v0, pc.info & 7u);
  o1 = xswap(v1, pc.info & 7u);
  return true;
}
#ifndef BLOCKED
void main() {
  uint w, v0, v1;
  if (!resolve_pixel(gl_GlobalInvocationID.xy, w, v0, v1)) return;
  mem[w] = v0;
  uint format = (pc.info >> 7) & 63u;
  if (format == 32u || format == 37u) mem[w + 1u] = v1;
}
#else
// Tiled destinations: a workgroup per aligned 32x32 destination block, whose tiled addresses are one
// contiguous run of 1024 elements. The pixels are converted into shared memory at their places in the
// run, then written out in address order, so the writes to guest memory (host RAM) coalesce.
shared uint s_val[2048];
shared uint s_mask[32];
void main() {
  uint t = gl_LocalInvocationIndex;
  if (t < 32u) s_mask[t] = 0u;
  barrier();
  uint format = (pc.info >> 7) & 63u;
  bool wide = format == 32u || format == 37u;
  uvec2 o = uvec2(uint(pc.dst_x) & ~31u, uint(pc.dst_y) & ~31u) + gl_WorkGroupID.xy * 32u;
  uint first = tiled_offset(o.x, o.y, pc.dest_pitch, wide ? 3u : 2u) & ~1023u;  // the block's first element
  uint gbase = ((pc.dest_base & 0x1FFFFFFFu) >> 2) + (wide ? first * 2u : first);
  for (uint k = 0u; k < 4u; ++k) {
    uint i = t + k * 256u;
    ivec2 p = ivec2(o + uvec2(i & 31u, i >> 5)) - ivec2(pc.dst_x, pc.dst_y);
    uint w, v0, v1;
    if (p.x < 0 || p.y < 0 || !resolve_pixel(uvec2(p), w, v0, v1)) continue;
    uint local = w - gbase, elem = wide ? local >> 1 : local;
    if (elem >= 1024u) continue;
    s_val[local] = v0;
    if (wide) s_val[local + 1u] = v1;
    atomicOr(s_mask[elem >> 5], 1u << (elem & 31u));
  }
  barrier();
  uint n = wide ? 2048u : 1024u;
  for (uint j = t; j < n; j += 256u) {
    uint elem = wide ? j >> 1 : j;
    if ((s_mask[elem >> 5] & (1u << (elem & 31u))) != 0u) mem[gbase + j] = s_val[j];
  }
}
#endif
)GLSL";

// The resolve shader for 32x32 tiled blocks (each workgroup writes one contiguous run).
inline std::string resolve_blocked_source() { return std::string( kResolveShader ).insert( strlen( "#version 450\n" ), "#define BLOCKED 1\n" ); }
