#include <set>
// Vulkan renderer for Xenos draws.
//
// * Guest physical memory (the 512 MB memfd mapped at 0xA0000000) is imported as one Vulkan buffer
//   (VK_EXT_external_memory_host). Shaders fetch vertices, indices and textures straight from it,
//   and resolves write their results back into it, so CPU and GPU always see the same bytes.
// * EDRAM is a GPU buffer in the console's tile layout; render-target views are Vulkan images that
//   are stored to and reloaded from it as views alias each other (see "EDRAM and render targets").
// * A resolve copies an EDRAM rectangle into guest memory (tiled, in the destination format) with a
//   compute shader, and can clear it afterwards, like the console's resolve.
// * Work is recorded on the command-processor thread and submitted at resolves (flush + wait).
#include "renderer.h"
#include "xxhash.h"
#include "input.h"
#include "prelude.glsl.h"
#include "runtime.h"
#include "translate.h"
#include "memwatch.h"
#include "fsr1_headers.h"

// DLSS's inputs from the scene's depth surface (sampled at the picture's pixels: b.y surface image pixels per
// picture pixel): the depth itself, and motion vectors, each pixel's move to where it was a frame ago in
// picture pixels: its depth reprojected by m (last frame's view-projection times this frame's inverse; a: the
// viewport's scale and offset, b.x picture pixels per surface pixel, D3D pixel centres).
static const char *kDlssDepthShader = R"GLSL(#version 450
layout(local_size_x = 16, local_size_y = 16) in;
layout(set = 0, binding = 0) uniform sampler2D src;
layout(set = 0, binding = 1, r32f) uniform writeonly image2D dst;
layout(push_constant) uniform P { mat4 m; vec4 a; vec4 b; } pc;
void main() {
  ivec2 p = ivec2(gl_GlobalInvocationID.xy);
  if (p.x >= int(pc.b.z) || p.y >= int(pc.b.w)) return;
  imageStore(dst, p, vec4(texelFetch(src, ivec2((vec2(p) + 0.5) * pc.b.y), 0).r));
}
)GLSL";
// Where a moving thing's draw left its own motion for the pixel (MotionHistory: c.x != 0) at the scene's
// depth, that instead; where a see-through drawing with its own motion lies in front, that.
static const char *kDlssMotionShader = R"GLSL(#version 450
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
layout(local_size_x = 16, local_size_y = 16) in;
layout(set = 0, binding = 0) uniform sampler2D src;
layout(set = 0, binding = 1, rg16f) uniform writeonly image2D dst;
layout(std430, set = 0, binding = 2) readonly buffer MotionPix { uint64_t mvpix[]; };
layout(set = 0, binding = 3, r8) uniform writeonly image2D mask;  // (debug view: which motion each pixel took)
layout(push_constant) uniform P { mat4 m; vec4 a; vec4 b; uvec4 c; } pc;
void main() {
  ivec2 p = ivec2(gl_GlobalInvocationID.xy);
  if (p.x >= int(pc.b.z) || p.y >= int(pc.b.w)) return;
  float d = texelFetch(src, ivec2((vec2(p) + 0.5) * pc.b.y), 0).r;
  vec2 win = (vec2(p) + 0.5) / pc.b.x - 0.5;
  vec2 ndc = (win - pc.a.zw) / pc.a.xy;
  vec4 c = pc.m * vec4(ndc, d, 1.0);
  vec2 cam = c.w > 0.0 ? (c.xy / c.w - ndc) * pc.a.xy * pc.b.x : vec2(0.0), mv = cam;
  float see_through = 0.0;  // (debug view only: which motion the pixel took)
  if (pc.c.x != 0u) {
    uvec2 ip = uvec2((vec2(p) + 0.5) * pc.b.y);
    if (ip.x < pc.c.y && ip.y < pc.c.z) {
      uint64_t e = mvpix[ip.y * pc.c.y + ip.x];
      float z = uintBitsToFloat(uint(e >> 32));
      // (A thing's own motion far from the camera's is taken for a draw mistaken for another: more than a
      // quarter of the picture's width a frame apart.)
      vec2 own = unpackHalf2x16(uint(e & 0xFFFFFFFFul)) / pc.b.y;
      if (e != 0ul && abs(z - d) <= 1e-5 * max(d, 1e-3) && length(own - cam) < pc.b.z * 0.25) { mv = own; if (pc.c.w != 0u) see_through = 1.0; }
      else if (pc.c.w != 0u && e != 0ul) see_through = 0.5;  // (debug: there, but at another depth)
      // DLSS's see-through mask (the bits after mvpix's two halves, prelude).
      uint i = ip.y * pc.c.y + ip.x;
      if (pc.c.w == 0u) see_through = ((mvpix[2u * 3840u * 2160u + (i >> 6)] >> (i & 63u)) & 1ul) != 0ul ? 1.0 : 0.0;
      // A see-through drawing in front of the surface (a car's lights over the road): its motion.
      uint64_t t = mvpix[3840u * 2160u + ip.y * pc.c.y + ip.x];
      vec2 own_t = unpackHalf2x16(uint(t & 0xFFFFFFFFul)) / pc.b.y;
      if (t != 0ul && uintBitsToFloat(uint(t >> 32)) >= d * (1.0 - 1e-5) && length(own_t - cam) < pc.b.z * 0.25) {
        mv = own_t;
        if (pc.c.w != 0u) see_through = 0.75;
      }
    }
  }
  imageStore(mask, p, vec4(see_through));
  imageStore(dst, p, vec4(mv, 0.0, 0.0));
}
)GLSL";
// Debug (CRACKDOWN_DLSS_SHOW_MOTION): the motion vectors as colours in DLSS's picture's place (red / green:
// right / down, 8 picture pixels to full; blue: which motion the pixel took: 1 its own, 0.75 a see-through drawing's, 0 the camera's).
static const char *kDlssShowShader = R"GLSL(#version 450
layout(local_size_x = 16, local_size_y = 16) in;
layout(set = 0, binding = 0) uniform sampler2D src;
layout(set = 0, binding = 1, rgba8) uniform writeonly image2D dst;
layout(set = 0, binding = 3, r8) uniform readonly image2D msk;
layout(push_constant) uniform P { vec4 size; } pc;
void main() {
  ivec2 p = ivec2(gl_GlobalInvocationID.xy);
  if (p.x >= int(pc.size.x) || p.y >= int(pc.size.y)) return;
  vec2 uv = (vec2(p) + 0.5) / pc.size.xy;
  vec2 mv = textureLod(src, uv, 0.0).xy;
  imageStore(dst, p, vec4(0.5 + mv / 16.0, imageLoad(msk, ivec2(uv * vec2(imageSize(msk)))).r, 1.0));
}
)GLSL";
// FSR 3 writes its picture with alpha 1; the game's post-processing reads the scene's alpha, so the
// picture's own alpha (the input's, bilinear) is put back over it.
static const char *kUpscaleAlphaShader = R"GLSL(#version 450
layout(local_size_x = 16, local_size_y = 16) in;
layout(set = 0, binding = 0) uniform sampler2D src;
layout(set = 0, binding = 1, rgba8) uniform image2D dst;
layout(push_constant) uniform P { vec4 size; } pc;
void main() {
  ivec2 p = ivec2(gl_GlobalInvocationID.xy);
  if (p.x >= int(pc.size.x) || p.y >= int(pc.size.y)) return;
  vec2 uv = (vec2(p) + 0.5) / pc.size.xy;
  imageStore(dst, p, vec4(imageLoad(dst, p).rgb, textureLod(src, uv, 0.0).a));
}
)GLSL";
static const char *kFxaaShader = R"GLSL(#version 450
layout(local_size_x = 16, local_size_y = 16) in;
layout(set = 0, binding = 0) uniform sampler2D src;
layout(set = 0, binding = 1, rgba8) uniform writeonly image2D dst;
layout(push_constant) uniform P { uvec4 c0, c1, c2, c3; } pc;
float luma(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }
vec3 at(vec2 p) { return textureLod(src, p, 0.0).rgb; }
void main() {
  ivec2 id = ivec2(gl_GlobalInvocationID.xy);
  vec2 r = vec2(uintBitsToFloat(pc.c0.x), uintBitsToFloat(pc.c0.y)), uv = (vec2(id) + 0.5) * r;
  vec3 m = at(uv);
  float lm = luma(m), nw = luma(at(uv + vec2(-1, -1) * r)), ne = luma(at(uv + vec2(1, -1) * r)), sw = luma(at(uv + vec2(-1, 1) * r)), se = luma(at(uv + vec2(1, 1) * r));
  float lo = min(lm, min(min(nw, ne), min(sw, se))), hi = max(lm, max(max(nw, ne), max(sw, se)));
  if (hi - lo < max(0.0312, hi * 0.125)) { imageStore(dst, id, vec4(m, 1.0)); return; }
  vec2 dir = vec2((sw + se) - (nw + ne), (nw + sw) - (ne + se));
  float reduce = max((nw + ne + sw + se) * 0.03125, 1.0 / 128.0);
  dir = clamp(dir / (min(abs(dir.x), abs(dir.y)) + reduce), vec2(-8.0), vec2(8.0)) * r;
  vec3 a = 0.5 * (at(uv + dir * (1.0 / 3.0 - 0.5)) + at(uv + dir * (2.0 / 3.0 - 0.5)));
  vec3 b = a * 0.5 + 0.25 * (at(uv - dir * 0.5) + at(uv + dir * 0.5));
  float lb = luma(b);
  imageStore(dst, id, vec4(lb < lo || lb > hi ? a : b, 1.0));
}
)GLSL";

#include "settings.h"
#include "fontatlas.h"
#include "keybinds.h"
#include "fsr3.h"  // (and dlss.h): the temporal upscaler, DLSS or FSR 3
bool splash_movie_hidden();  // splash.cpp

bool native_d3d();  // native_d3d.cpp

uint64_t gpu_swap_count();  // gpu.cpp

#include <shaderc/shaderc.h>
#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <tuple>
#include <sys/resource.h>
#include <unistd.h>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <immintrin.h>
#include <string>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <vector>

uint32_t tiled_offset( uint32_t x, uint32_t y, uint32_t width, uint32_t log_bpp );  // video.cpp
void write_ppm( const char *path, const std::vector<uint32_t> &px, uint32_t w, uint32_t h );

namespace
{

#define VKCHECK( x ) do { VkResult r_ = ( x ); if ( r_ != VK_SUCCESS ) { rt_log( "vulkan: %s failed (%d)\n", #x, (int)r_ ); return false; } } while ( 0 )

constexpr uint32_t PHYS_SIZE = 0x20000000;

struct DrawConstants
{
    float c[512][4];
    uint32_t fetch[192];
    uint32_t bools[8];
    uint32_t loops[32];
    float vp_scale[4], vp_offset[4], rt[4], window_offset[4];
    uint32_t vte, prim, index_mode, index_addr, index_info, index_offset, first, flags;
    uint32_t tex[32];  // per texture fetch constant: image index | sampler group << 16 (renderer "Textures")
    uint32_t param_gen;  // the pixel shader register that gets the screen position, ~0: none
    uint32_t sharp;      // per texture fetch constant: magnified with a sharper filter (tfetch)
    uint32_t text;       // per texture fetch constant: the game's font, its glyph edges redrawn crisp (tfetch)
    uint32_t tsize[32];  // per texture fetch constant: its image's width - 1 | height - 1 << 16 (no size queries in shaders)
    uint32_t ucp_ena;    // user clip planes in use (PA_CL_CLIP_CNTL bits 0-5)
    float ucp[6][4];     // their planes (PA_CL_UCP_n, 0x2388..), in clip space
    uint32_t mv_cur, mv_prev, mv_pix, mv_w, mv_h;  // DLSS's motion (MotionHistory; prelude.glsl.h)
    float mv_jx, mv_jy;
    uint32_t mv_cam, mv_pad[3];
    float mv_m[16];  // columns
};

VkInstance g_instance;
VkPhysicalDevice g_phys;
VkDevice g_dev;
VkQueue g_queue;
uint32_t g_qfam;
VkCommandPool g_pool;
VkCommandBuffer g_cmd;
VkFence g_fence;

// Occlusion queries (EVENT_WRITE_ZPD). The game's D3D brackets the draws to measure with two ZPD events,
// each writing a 32-byte block of sample counters (little-endian dwords, pairs for the two halves of the
// chip: +16/+20 the samples that passed the depth test) to RB_SAMPLE_COUNT_ADDR: the end block at the
// query's record, the begin block 32 bytes after it. Before issuing the end it marks the record's first
// 16 bytes with FFFFFEED and polls until they change (sub_8274A150 reads visible = end - begin). Here each
// draw while a query is open is a Vulkan occlusion query; at the next flush the counts are written.
constexpr uint32_t QUERY_POOL_SIZE = 4096;
VkQueryPool g_query_pool;
bool g_precise_queries;
uint32_t g_query_next;  // next free query in this command buffer

// CRACKDOWN_GPU_TIMING=1: GPU timestamps around EDRAM stores, loads, resolves and resolve clears and
// the whole command buffer, summed and printed with the renderer stats (CRACKDOWN_DRAW_STATS).
enum GpuTimer { GT_STORE, GT_LOAD, GT_RESOLVE, GT_CLEAR, GT_ALL, GT_COUNT };
constexpr uint32_t TS_POOL_SIZE = 8192;
VkQueryPool g_ts_pool;
uint32_t g_ts_next;
float g_ts_period;  // ns per tick
std::vector<std::pair<int, uint32_t>> g_ts_marks;  // timer, first of its two queries
double g_ts_ms[GT_COUNT];
uint32_t g_ts_count[GT_COUNT];
uint32_t gpu_timer_begin( int timer )
{
    if ( !g_ts_pool || g_ts_next + 2 > TS_POOL_SIZE ) return ~0u;
    uint32_t q = g_ts_next;
    g_ts_next += 2;
    vkCmdWriteTimestamp( g_cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, g_ts_pool, q );
    g_ts_marks.push_back( { timer, q } );
    return q;
}
void gpu_timer_end( uint32_t q )
{
    if ( q != ~0u ) vkCmdWriteTimestamp( g_cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, g_ts_pool, q + 1 );
}
// CRACKDOWN_GPU_PASSES=N (with CRACKDOWN_GPU_TIMING): each rendering pass timed too, summed by its targets
// and first pixel shader; the N most costly printed with the stats (timer GT_COUNT + its index).
struct PassStats { std::string name; double ms; uint32_t passes, draws, vk_draws; };
std::vector<PassStats> g_pass_stats;
std::unordered_map<std::string, uint32_t> g_pass_index;
uint32_t g_pass_q = ~0u, g_pass_id;
// CRACKDOWN_PROBE=x0,y0,x1,y1 (debug; screen pixels, scaled to each target's width): in a census frame every draw is clipped to that rectangle of its
// target and counted by an occlusion query; the draws that drew there are logged ("probe:").
static std::string hex16( uint64_t h ) { char b[17]; snprintf( b, sizeof( b ), "%016llx", (unsigned long long)h ); return b; }
struct ProbeQuery { uint32_t query, draw; uint64_t vs, ps; uint32_t count, w, h, blend; };
std::vector<ProbeQuery> g_probes;
struct ZQuery
{
    uint32_t begin_addr, end_addr = 0;  // physical; end_addr set once the end event came
    uint64_t samples = 0;               // counted in completed command buffers
    uint32_t draws = 0;
    uint64_t vs = 0, ps = 0;            // CRACKDOWN_ZPD_LOG: the last draw counted
    uint32_t prim = 0, vertices = 0, depth_control = 0, surface = 0;
    std::vector<uint32_t> ids;          // Vulkan queries in the command buffer being recorded
    uint64_t serial = 0;
};
std::vector<ZQuery> g_zqueries;  // open (end_addr 0) and ended, not yet written
uint64_t g_zquery_serial;

// Asynchronous submission. Recorded work is submitted without waiting (at the frame's end, and when the
// command processor runs out of packets with guest-visible effects pending); up to SUBMITS command
// buffers are in flight, each starting with a barrier so it runs after the previous one. What the
// command processor makes visible to the CPU (fence and memory writes, interrupts, occlusion results,
// presenting the front buffer) is deferred until the GPU work recorded before it has completed
// (renderer_defer), so the game reuses memory and reads results only once the GPU is really done.
constexpr int SUBMITS = 8;  // in flight: a frame goes out in several (renderer_draw), so the GPU works while it is translated
struct Submission
{
    VkCommandBuffer cmd;
    VkFence fence;
    VkQueryPool queries, timestamps;
    bool busy = false;
    uint64_t serial = 0;
    VkDeviceSize dc_start = 0, dc_end = 0;                         // draw-constant ring range it reads
    std::vector<std::function<void()>> effects;                    // run after it completes, in order
    uint32_t query_count = 0;
    uint8_t query_scale8[QUERY_POOL_SIZE] = {};
    uint8_t query_samples[QUERY_POOL_SIZE] = {};  // per query: MSAA's samples per image pixel (its count / samples)  // per query: image pixels per surface pixel in eighths (its count / (s/8)^2: surface pixels)
    std::vector<std::pair<uint64_t, std::vector<uint32_t>>> zids;  // ZQuery serial -> its queries here
    std::vector<ProbeQuery> probes;
    uint32_t ts_count = 0;
    std::vector<std::pair<int, uint32_t>> ts_marks;
};
Submission g_subs[SUBMITS];
int g_cur;                    // the submission being recorded (g_recording) or recorded next
uint64_t g_submit_serial;
std::vector<std::function<void()>> g_effects;  // deferred behind the work being recorded
bool g_recording, g_ok;
VkPhysicalDeviceMemoryProperties g_memprops;
VkDeviceSize g_dc_align = 256;

VkBuffer g_mem_buf;           // guest physical memory
// Resolution above the console's: settings render_scale. Screen sized surfaces are drawn at that many
// times their pixels, and each resolve of one lands twice: in guest memory as the console would (the
// game's own reads), and at full size here, which is what later draws sample and what is shown.
constexpr VkDeviceSize HIRES_SIZE = 0x1FFFF000, HIRES_SHOW_SIZE = 5120 * 2880 * 4;
constexpr uint32_t HIRES_SHOWS = 4;
VkBuffer g_hires_buf, g_hires_show_buf;
VkDeviceMemory g_hires_mem, g_hires_show_mem;
uint8_t *g_hires_show;
uint32_t g_hires_next = 0x1000, g_hires_show_next;
struct HiRes { uint32_t offset, w, h, k8, pitch, bytes, gen, since, size, room; };  // k8: image pixels per pixel, in eighths; room: bytes held at offset
std::unordered_map<uint32_t, HiRes> g_hires;
// Guest memory resolves have written ([start, end) by start): pictures the GPU made, not the game's art.
std::map<uint32_t, uint32_t> g_resolve_ranges;
bool resolved( uint32_t a )
{
    auto it = g_resolve_ranges.upper_bound( a );
    return it != g_resolve_ranges.begin() && a < std::prev( it )->second;
}
// The resolution's image pixels per console pixel, in eighths: 8 (1280x720) to 24 (3840x2160) -- every
// 16:9 size whose 80x16 EDRAM tiles and 8x8 resolve blocks come out whole.
} // namespace
bool video_display_size( uint32_t &w, uint32_t &h );  // video.cpp
namespace
{
// Match Display (resolution 0): the largest of those that fits the desktop.
uint32_t render_scale8()
{
    static const uint32_t s = [] {
        uint32_t height = (uint32_t)settings().resolution, w = 0, h = 0;
        // (The window's thread finds the desktop's size as it starts, maybe after the renderer's.)
        for ( int i = 0; !height && !getenv( "CRACKDOWN_HEADLESS" ) && !video_display_size( w, h ) && i < 600; ++i )
            std::this_thread::sleep_for( std::chrono::milliseconds( 5 ) );
        if ( !height && video_display_size( w, h ) ) height = std::min( h, w * 9 / 16 );
        rt_log( "renderer: resolution %ux%u%s\n", std::clamp( height / 90u, 8u, 24u ) * 160, std::clamp( height / 90u, 8u, 24u ) * 90,
                settings().resolution ? "" : " (matching the display)" );
        return std::clamp( height / 90u, 8u, 24u );
    }();
    return s;
}
// Surface pixel x (or a count of them from 0) at s8 eighths: the first image pixel of x. Pixel x covers
// image pixels [at(x), at(x + 1)); image pixel i belongs to pixel i * 8 / s8.
inline int32_t at8( int32_t x, uint32_t s8 ) { return (int32_t)( ( (int64_t)x * s8 + 7 ) >> 3 ); }
bool fsr_wanted()
{
    static const bool f = settings().fsr;
    return f;
}
bool fxaa_wanted()
{
    static const bool f = settings().fxaa;
    return f;
}
// With DLSS: the scene's surfaces (the world, multisampled) at the mode's share of the resolution, at least
// 640x360; DLSS brings the picture up to the resolution as it is resolved (DlssFrame).
uint32_t scene_scale8()
{
    static const uint32_t s = std::clamp( (uint32_t)std::lround( render_scale8() * upscaler_share() ), 4u, render_scale8() );
    return upscaler_ready() ? s : render_scale8();
}
bool hires_on() { return render_scale8() > 8 || fsr_wanted() || fxaa_wanted() || upscaler_wanted(); }
// FSR 1: the frame, up to the screen's size, sharpened, then shown.
VkPipeline g_fsr_easu_pipe, g_fsr_rcas_pipe, g_fxaa_pipe;
VkSampler g_post_sampler;
VkDescriptorSetLayout g_fsr_layout;
VkPipelineLayout g_fsr_pl;
struct FsrImage { VkImage image = VK_NULL_HANDLE; VkDeviceMemory mem = VK_NULL_HANDLE; VkImageView view = VK_NULL_HANDLE; };
FsrImage g_fsr_in, g_fsr_mid, g_fsr_out, g_fxaa_out;
// DLSS in the frame (dlss.h). The world is drawn into a screen-sized multisampled surface with depth (the
// "scene", at scene_scale8: the mode's share of the resolution), resolved, then the post-processing draws
// full-screen quads from that picture into a single-sampled surface (at the resolution) and the HUD comes
// after. So: each frame the world's draws move by a sub-pixel jitter (their viewport offset); as the
// scene's colour is resolved its depth is turned into motion vectors (the camera's view-projection from
// the world shader, now and a frame ago), DLSS makes the picture at the resolution from it, and that is
// the resolve's full size picture (HiRes) the post-processing reads.
// A resolve's push constants (kResolveShader) and its rectangle, for its full size copy (hires_resolve).
struct ResolvePC { uint32_t base, pitch, height, info, sbase, spitch, smsaa, sformat; int32_t sx, sy, dx, dy, w, h; uint32_t sscale, sample_select; };
struct HiResolve { uint32_t dest, w, h, info, pitch; int32_t ex0, ey0, dx, dy, rw, rh; };
struct DlssFrame
{
    FsrImage in, out, depth, motion, held, mask;    // held: out copied into an image made as a surface's, for the resolve
    VkImageView out_raw = VK_NULL_HANDLE;     // held as the console's words (out_set)
    VkDescriptorSet out_set = VK_NULL_HANDLE;
    uint32_t w = 0, h = 0, out_w = 0, out_h = 0;  // its images: the scene's picture and DLSS's
    VkPipeline depth_pipe = VK_NULL_HANDLE, motion_pipe = VK_NULL_HANDLE, show_pipe = VK_NULL_HANDLE, alpha_pipe = VK_NULL_HANDLE;
    struct Target *scene_color = nullptr, *scene_depth = nullptr;
    HiResolve pending{};                      // the scene's last resolve (snapped): DLSS's picture goes to its full size copy
    ResolvePC pending_pc{};
    double vp[16], prev_vp[16];               // the world shader's c0-c3 (rows)
    float vps[2], vpo[2];                     // its viewport scale / offset (surface pixels)
    bool have_vp = false, prev_ok = false, snapped = false, done = false, ran = false, ran_before = false;
    uint32_t index = 0;
    float jx = 0, jy = 0;                     // this frame's jitter, in the picture's pixels
};
DlssFrame g_df;
// DLSS's motion of things that move (cars, people): each scene draw's vertices keep their places on screen
// in mvpos (prelude.glsl.h), this frame's half and last frame's taking turns; the same draw a frame later
// (the same shaders, vertex buffers, indices and count, and the how-many-th such draw) reads them back,
// and its pixels' motion, last place minus this one, goes to mvpix with their depth, the nearest
// surface's kept. The motion shader takes that wherever its depth is the scene's, the camera's elsewhere.
struct MotionHistory
{
    static constexpr uint32_t SLOTS = 1u << 22;          // vertex places per frame
    static constexpr VkDeviceSize PIX = 3840ull * 2160;  // pixels (mvpix: two of these, motion then see-through motion; then a bit each: DLSS's see-through mask)
    VkBuffer pos = VK_NULL_HANDLE, pix = VK_NULL_HANDLE;
    VkDeviceMemory pos_mem = VK_NULL_HANDLE, pix_mem = VK_NULL_HANDLE;
    bool on = false, cleared = false;
    bool cam = false;  // m holds the camera's move this frame (from DlssFrame's view-projections)
    float m[16];       // columns: last frame's view-projection times this one's inverse
    uint32_t half = 0, next = 0;
    struct Draw { uint32_t base, slots; };
    std::unordered_map<uint64_t, Draw> now, prev;
    std::unordered_map<uint64_t, uint32_t> seen;  // this frame's draws so far per key
};
MotionHistory g_mv;
// The camera's move (DlssFrame): last frame's view-projection times the inverse of this one's, as GLSL's
// columns; false if this one has no inverse.
bool dlss_camera_move( float cols[16] )
{
    // m = last frame's view-projection times the inverse of this one's (rows; doubles: the city is kilometres across).
    double inv[16], m[16];
    {
        const double *a = g_df.vp;
        double t[16];
        t[0] = a[5] * a[10] * a[15] - a[5] * a[11] * a[14] - a[9] * a[6] * a[15] + a[9] * a[7] * a[14] + a[13] * a[6] * a[11] - a[13] * a[7] * a[10];
        t[4] = -a[4] * a[10] * a[15] + a[4] * a[11] * a[14] + a[8] * a[6] * a[15] - a[8] * a[7] * a[14] - a[12] * a[6] * a[11] + a[12] * a[7] * a[10];
        t[8] = a[4] * a[9] * a[15] - a[4] * a[11] * a[13] - a[8] * a[5] * a[15] + a[8] * a[7] * a[13] + a[12] * a[5] * a[11] - a[12] * a[7] * a[9];
        t[12] = -a[4] * a[9] * a[14] + a[4] * a[10] * a[13] + a[8] * a[5] * a[14] - a[8] * a[6] * a[13] - a[12] * a[5] * a[10] + a[12] * a[6] * a[9];
        t[1] = -a[1] * a[10] * a[15] + a[1] * a[11] * a[14] + a[9] * a[2] * a[15] - a[9] * a[3] * a[14] - a[13] * a[2] * a[11] + a[13] * a[3] * a[10];
        t[5] = a[0] * a[10] * a[15] - a[0] * a[11] * a[14] - a[8] * a[2] * a[15] + a[8] * a[3] * a[14] + a[12] * a[2] * a[11] - a[12] * a[3] * a[10];
        t[9] = -a[0] * a[9] * a[15] + a[0] * a[11] * a[13] + a[8] * a[1] * a[15] - a[8] * a[3] * a[13] - a[12] * a[1] * a[11] + a[12] * a[3] * a[9];
        t[13] = a[0] * a[9] * a[14] - a[0] * a[10] * a[13] - a[8] * a[1] * a[14] + a[8] * a[2] * a[13] + a[12] * a[1] * a[10] - a[12] * a[2] * a[9];
        t[2] = a[1] * a[6] * a[15] - a[1] * a[7] * a[14] - a[5] * a[2] * a[15] + a[5] * a[3] * a[14] + a[13] * a[2] * a[7] - a[13] * a[3] * a[6];
        t[6] = -a[0] * a[6] * a[15] + a[0] * a[7] * a[14] + a[4] * a[2] * a[15] - a[4] * a[3] * a[14] - a[12] * a[2] * a[7] + a[12] * a[3] * a[6];
        t[10] = a[0] * a[5] * a[15] - a[0] * a[7] * a[13] - a[4] * a[1] * a[15] + a[4] * a[3] * a[13] + a[12] * a[1] * a[7] - a[12] * a[3] * a[5];
        t[14] = -a[0] * a[5] * a[14] + a[0] * a[6] * a[13] + a[4] * a[1] * a[14] - a[4] * a[2] * a[13] - a[12] * a[1] * a[6] + a[12] * a[2] * a[5];
        t[3] = -a[1] * a[6] * a[11] + a[1] * a[7] * a[10] + a[5] * a[2] * a[11] - a[5] * a[3] * a[10] - a[9] * a[2] * a[7] + a[9] * a[3] * a[6];
        t[7] = a[0] * a[6] * a[11] - a[0] * a[7] * a[10] - a[4] * a[2] * a[11] + a[4] * a[3] * a[10] + a[8] * a[2] * a[7] - a[8] * a[3] * a[6];
        t[11] = -a[0] * a[5] * a[11] + a[0] * a[7] * a[9] + a[4] * a[1] * a[11] - a[4] * a[3] * a[9] - a[8] * a[1] * a[7] + a[8] * a[3] * a[5];
        t[15] = a[0] * a[5] * a[10] - a[0] * a[6] * a[9] - a[4] * a[1] * a[10] + a[4] * a[2] * a[9] + a[8] * a[1] * a[6] - a[8] * a[2] * a[5];
        const double det = a[0] * t[0] + a[1] * t[4] + a[2] * t[8] + a[3] * t[12];
        if ( det == 0 || !std::isfinite( det ) ) return false;
        for ( int i = 0; i < 16; ++i ) inv[i] = t[i] / det;
    }
    const double *prev = g_df.prev_ok ? g_df.prev_vp : g_df.vp;
    for ( int r = 0; r < 4; ++r )
        for ( int c = 0; c < 4; ++c )
        {
            double v = 0;
            for ( int i = 0; i < 4; ++i ) v += prev[r * 4 + i] * inv[i * 4 + c];
            m[r * 4 + c] = v;
        }
    for ( int r = 0; r < 4; ++r )
        for ( int c = 0; c < 4; ++c ) cols[c * 4 + r] = (float)m[r * 4 + c];
    return true;
}
uint64_t g_frames_seen;  // renderer_frame_end's count
static bool dlss_snapshot( uint32_t surface_w, uint32_t surface_h );
static void dlss_run();
void hires_resolve( const HiResolve &a, ResolvePC pc, VkDescriptorSet set, uint32_t k, uint32_t sscale );
uint32_t g_fsr_in_w, g_fsr_in_h, g_fsr_out_w, g_fsr_out_h, g_fxaa_w, g_fxaa_h;
VkDeviceMemory g_mem_mem;
VkBuffer g_dc_buf;            // per-draw constants ring
VkDeviceMemory g_dc_mem;
uint8_t *g_dc_ptr;
VkDeviceSize g_dc_size = 256ull << 20, g_dc_head;

VkDescriptorSetLayout g_draw_layout;
VkPipelineLayout g_draw_pl;
VkDescriptorPool g_dpool;
VkDescriptorSet g_draw_set;
VkPipeline g_resolve_pipe, g_resolve_blocked_pipe;
VkSampler g_point_sampler;

shaderc_compiler_t g_shaderc;

uint32_t find_memory( uint32_t bits, VkMemoryPropertyFlags want )
{
    for ( uint32_t i = 0; i < g_memprops.memoryTypeCount; ++i )
        if ( ( bits & ( 1u << i ) ) && ( g_memprops.memoryTypes[i].propertyFlags & want ) == want ) return i;
    return ~0u;
}

// Compiled SPIR-V is cached on disk (~/.cache/crackdown-recomp/spirv/<hash of the GLSL>.spv): compiling
// runs on the command-processor thread, so a shader compiled mid-frame stalls the GPU (and a game
// spinning on a GPU fence). Optimisation is left to the driver, which redoes it anyway.
const std::string &cache_dir()
{
    static const std::string dir = [] {
        const char *xdg = getenv( "XDG_CACHE_HOME" ), *home = getenv( "HOME" );
        std::string d = xdg && *xdg ? std::string( xdg ) : std::string( home ? home : "." ) + "/.cache";
        d += "/crackdown-recomp";
        std::error_code ec;
        std::filesystem::create_directories( d + "/spirv", ec );
        return d;
    }();
    return dir;
}

// The driver's compiled pipelines persist in ~/.cache/crackdown-recomp/pipelines-<version>.bin, so a shader
// the driver is slow on is only slow the first time it is seen. The version is the shaders' common GLSL
// (prelude.glsl.h): when that changes every pipeline is made again, so the file starts afresh and the
// old ones are deleted rather than growing without end. Saved when the queue of pipelines to make runs
// out, and at most once a minute while it is long (each save writes the whole cache).
VkPipelineCache g_pcache = VK_NULL_HANDLE;
std::mutex g_pcache_save_lock;
std::atomic<bool> g_pcache_dirty{ false };

const std::string &pipeline_cache_path()
{
    static const std::string path = [] {
        uint64_t h = 1469598103934665603ull;
        for ( const std::string &part : { xlate::kPrelude, std::string( xlate::kVertexMain ), std::string( xlate::kPixelMain ) } )
            for ( unsigned char ch : part ) h = ( h ^ ch ) * 1099511628211ull;
        char name[64];
        snprintf( name, sizeof( name ), "/pipelines-%016llx.bin", (unsigned long long)h );
        return cache_dir() + name;
    }();
    return path;
}

void pipeline_cache_load()
{
    std::vector<char> data;
    {
        std::error_code ec;
        for ( auto &e : std::filesystem::directory_iterator( cache_dir(), ec ) )
        {
            const std::string n = e.path().filename().string();
            if ( n == "pipelines.bin" && !std::filesystem::exists( pipeline_cache_path(), ec ) )
            {
                std::filesystem::rename( e.path(), pipeline_cache_path(), ec );  // (from before versions: it holds this one's too)
                continue;
            }
            if ( n.rfind( "pipelines", 0 ) == 0 && n.size() > 4 && n.compare( n.size() - 4, 4, ".bin" ) == 0 && e.path().string() != pipeline_cache_path() )
            {
                rt_log( "renderer: removing %s (an older shader version's pipelines)\n", n.c_str() );
                std::filesystem::remove( e.path(), ec );
            }
        }
    }
    if ( FILE *f = fopen( pipeline_cache_path().c_str(), "rb" ) )
    {
        fseek( f, 0, SEEK_END );
        long n = ftell( f );
        fseek( f, 0, SEEK_SET );
        if ( n > 0 )
        {
            data.resize( n );
            if ( fread( data.data(), 1, n, f ) != (size_t)n ) data.clear();
        }
        fclose( f );
    }
    VkPipelineCacheCreateInfo ci{ VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO };
    ci.initialDataSize = data.size();
    ci.pInitialData = data.empty() ? nullptr : data.data();
    if ( vkCreatePipelineCache( g_dev, &ci, nullptr, &g_pcache ) != VK_SUCCESS )
    {
        ci.initialDataSize = 0;  // stale or foreign data: start empty
        ci.pInitialData = nullptr;
        vkCreatePipelineCache( g_dev, &ci, nullptr, &g_pcache );
    }
}

void pipeline_cache_save()
{
    std::lock_guard<std::mutex> g( g_pcache_save_lock );
    size_t n = 0;
    if ( !g_pcache || vkGetPipelineCacheData( g_dev, g_pcache, &n, nullptr ) != VK_SUCCESS || !n ) return;
    std::vector<char> data( n );
    if ( vkGetPipelineCacheData( g_dev, g_pcache, &n, data.data() ) != VK_SUCCESS ) return;
    g_pcache_dirty = false;
    std::string path = pipeline_cache_path(), tmp = path + ".tmp";
    if ( FILE *f = fopen( tmp.c_str(), "wb" ) )
    {
        bool ok = fwrite( data.data(), 1, n, f ) == n;
        ok = fclose( f ) == 0 && ok;
        if ( ok ) rename( tmp.c_str(), path.c_str() );
        else remove( tmp.c_str() );
    }
}

std::string spirv_cache_path( const std::string &src, shaderc_shader_kind kind )
{
    const std::string dir = cache_dir() + "/spirv";
    char name[32];
    snprintf( name, sizeof( name ), "/%016llx.spv", (unsigned long long)xlate::source_hash( src, kind ) );
    return dir + name;
}

VkShaderModule make_module( const uint32_t *code, size_t bytes )
{
    VkShaderModuleCreateInfo ci{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    ci.codeSize = bytes;
    ci.pCode = code;
    VkShaderModule m = VK_NULL_HANDLE;
    vkCreateShaderModule( g_dev, &ci, nullptr, &m );
    return m;
}

struct CompileStats { uint32_t compiled, cached; double ms; } g_compile_stats;
std::mutex g_compile_stats_lock;

VkShaderModule compile( const std::string &src, shaderc_shader_kind kind, const char *name, bool *compiled = nullptr )
{
    // CRACKDOWN_DUMP_GLSL=<part of a shader name>: write its source to build/shader-<name>.glsl.
    if ( const char *want = RT_ENV( "CRACKDOWN_DUMP_GLSL" ); want && strstr( name, want ) )
    {
        std::string out = std::string( "build/shader-" ) + name + ".glsl";
        if ( FILE *f = fopen( out.c_str(), "w" ) ) { fwrite( src.data(), 1, src.size(), f ); fclose( f ); }
    }
    std::string path = spirv_cache_path( src, kind );
    if ( FILE *f = fopen( path.c_str(), "rb" ) )
    {
        std::vector<uint32_t> code;
        fseek( f, 0, SEEK_END );
        long n = ftell( f );
        fseek( f, 0, SEEK_SET );
        if ( n > 0 && n % 4 == 0 )
        {
            code.resize( n / 4 );
            if ( fread( code.data(), 1, n, f ) != (size_t)n ) code.clear();
        }
        fclose( f );
        if ( !code.empty() )
        {
            std::lock_guard<std::mutex> g( g_compile_stats_lock );
            g_compile_stats.cached++;
            return make_module( code.data(), code.size() * 4 );
        }
    }
    auto t0 = std::chrono::steady_clock::now();
    shaderc_compile_options_t opt = shaderc_compile_options_initialize();
    shaderc_compile_options_set_target_env( opt, shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_2 );
    shaderc_compile_options_set_optimization_level( opt, shaderc_optimization_level_zero );
    thread_local shaderc_compiler_t compiler = shaderc_compiler_initialize();  // one per thread (workers compile concurrently)
    shaderc_compilation_result_t res = shaderc_compile_into_spv( compiler, src.c_str(), src.size(), kind, name, "main", opt );
    shaderc_compile_options_release( opt );
    VkShaderModule m = VK_NULL_HANDLE;
    if ( shaderc_result_get_compilation_status( res ) != shaderc_compilation_status_success )
        rt_log( "renderer: shader %s failed to compile:\n%s\n", name, shaderc_result_get_error_message( res ) );
    else
    {
        const char *bytes = shaderc_result_get_bytes( res );
        size_t n = shaderc_result_get_length( res );
        m = make_module( (const uint32_t *)bytes, n );
        std::string tmp = path + ".tmp";
        if ( FILE *f = fopen( tmp.c_str(), "wb" ) )
        {
            bool ok = fwrite( bytes, 1, n, f ) == n;
            ok = fclose( f ) == 0 && ok;
            if ( ok ) rename( tmp.c_str(), path.c_str() );
            else remove( tmp.c_str() );
        }
    }
    shaderc_result_release( res );
    // CRACKDOWN_SHADER_LOG=1: each shader compiled while the game runs (one devtools/build_shader_aot.sh missed).
    if ( static const bool log = RT_ENV( "CRACKDOWN_SHADER_LOG" ); log ) rt_log( "renderer: compiled %s at run time\n", name );
    if ( compiled ) *compiled = true;
    std::lock_guard<std::mutex> g( g_compile_stats_lock );
    g_compile_stats.compiled++;
    g_compile_stats.ms += std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() - t0 ).count();
    return m;
}

// ------------------------------------------------------------------ Render targets
// D3D's surfaces are ranges of the console's 10 MB of EDRAM (2048 tiles of 80x16 32-bit samples): a
// surface is a base tile, pitch, MSAA mode and format, and the game places surfaces to share memory on
// purpose -- it draws a shadow map as a 4x multisampled 520-pixel-wide depth surface and reads the same
// memory as a single-sampled 1040-wide one, reuses its scene's depth for later passes through other
// surfaces, and so on.
//
// Here every surface is a host image (one sample per pixel). Who last wrote each tile is kept; when a
// surface is bound and another one wrote some of its tiles since it last caught up, those pixels are
// converted from the other surface's image into it (kCopyShader, following the console's sample layout).
// Resolves read the surface's image; clears clear it.
//
// Anti-aliasing (settings anti_aliasing): a multisampled surface's image has 2x2 pixels for each of the
// surface's (Target::scale), drawn as such -- supersampling, where the console keeps 2 or 4 samples per
// pixel. Everything else stays in the surface's pixels: tiles, rectangles, clears' and draws' bounds.
// Copies map each host pixel to the console sample it stands for; resolves that average the console's
// samples average the host pixels, ones that pick a sample pick its host pixel.
//
// CRACKDOWN_NATIVE_D3D (native_d3d.cpp draws a tiled frame in one pass): EDRAM is virtual, four times the
// console's, with every surface's base spaced out four times, so a surface the game placed for a tile's
// height doesn't run into the one after it at the frame's height.
constexpr uint32_t EDRAM_MAX_TILES = 8192;
const uint32_t g_edram_scale = native_d3d() ? 4 : 1, EDRAM_TILES = 2048 * g_edram_scale;
uint32_t edram_base( uint32_t info ) { return ( info & 0xFFF ) * g_edram_scale; }
constexpr VkDeviceSize STAGE_SIZE = 64ull << 20;
constexpr uint32_t MAX_TARGET_W = 8192, MAX_TARGET_H = 2048;

VkBuffer g_stage_buf;
VkDeviceMemory g_stage_mem;
VkDescriptorSetLayout g_surface_layout;  // surface reads (copies, resolves): mem, staging, tile mask, the surface's views
VkPipelineLayout g_surface_pl;
VkPipeline g_copy_pipe;
struct DummyImage { VkImage image = VK_NULL_HANDLE; VkDeviceMemory mem = VK_NULL_HANDLE; VkImageView view = VK_NULL_HANDLE, stencil = VK_NULL_HANDLE; };
DummyImage g_dummy_color, g_dummy_depth;  // for the bindings a surface's reads leave unused
DummyImage g_tex_standin;  // texture index 0: magenta, for formats without a host equivalent (renderer "Textures")

// How a surface's host image holds the console's words (kCopyShader / kResolveShader src_bits).
enum XferKind : uint32_t { XK_8888 = 0, XK_RAW32 = 1, XK_RAW64 = 2, XK_7E3 = 3, XK_DEPTH = 4 };

struct Target
{
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkImageView raw = VK_NULL_HANDLE;      // colour: the host bits as unsigned integers; depth: the stencil aspect
    VkDescriptorSet set = VK_NULL_HANDLE;  // reading it (copies to other surfaces, resolves)
    VkFormat format;
    uint32_t w, h;
    bool depth, bpp64, fresh = true;
    uint32_t base, pitch, msaa, guest_format, kind, host_bpp;
    uint32_t pitch_tiles, tile_h;   // tiles per row of tiles; pixel rows per tile row
    uint32_t scale8 = 8;            // image pixels per surface pixel across and down, in eighths (anti-aliasing or resolution)
    uint32_t hi8 = 8;               // of that, the resolution's (render_scale8)
    VkDescriptorSet hi_set = VK_NULL_HANDLE;  // as set, with the resolves' buffer in guest memory's place
    std::vector<uint32_t> synced;   // per tile of the surface (row-major): the version it last caught up with
    uint32_t checked = 0, checked_h = 0;  // the version and height it was last found current at, and with which partner
    const Target *checked_partner = nullptr;
    int32_t owned[4] = {};          // a rectangle mark_written found all its own and unread, at g_tile_epoch
    uint64_t owned_epoch = ~0ull;
    // MSAA (settings msaa): a multisampled surface of the game's (the scene's colour and depth) also has a
    // multisampled image its draws go to (samples > 1). Everything else -- copies between surfaces, clears,
    // resolves, DLSS -- uses image as before; the two are kept in step: ms_newer, draws went to the
    // multisampled image since image last took its samples' average (ms_resolve); ss_newer, image was
    // written another way since the multisampled image last took it (ms_upload).
    uint32_t samples = 1;
    VkImage ms_image = VK_NULL_HANDLE;
    VkDeviceMemory ms_mem = VK_NULL_HANDLE;
    VkImageView ms_view = VK_NULL_HANDLE;
    bool ms_newer = false, ss_newer = false;
    uint32_t tile( uint32_t i ) const { return ( base + i ) % EDRAM_TILES; }  // EDRAM tile of tile i
};
uint32_t g_msaa_samples = 1;   // MSAA's samples (settings msaa, as the device allows; 1 off)
bool g_stencil_export;         // VK_EXT_shader_stencil_export (copying depth and stencil into a multisampled image)
std::unordered_map<uint64_t, Target> g_targets;

// Each tile's last writer and version. A version is new whenever a tile changes hands or its writer
// writes it again after some other surface took a copy (g_tile_read), so a surface is behind exactly
// where a tile's version is newer than the one it caught up with.
uint32_t g_ver = 1;
uint32_t g_tile_ver[EDRAM_MAX_TILES];
const Target *g_tile_writer[EDRAM_MAX_TILES];
bool g_tile_read[EDRAM_MAX_TILES];
uint64_t g_tile_epoch;  // goes up whenever a tile's writer or read flag changes

VkFormat color_format( uint32_t f )
{
    switch ( f )
    {
    case 0: case 1: return VK_FORMAT_R8G8B8A8_UNORM;                     // 8_8_8_8 (_GAMMA)
    case 2: case 10: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;          // 2_10_10_10 (_AS_10_10_10_10)
    case 4: case 6: return VK_FORMAT_R16G16_SFLOAT;                      // 16_16, 16_16_FLOAT
    case 14: return VK_FORMAT_R32_SFLOAT;
    case 15: return VK_FORMAT_R32G32_SFLOAT;
    default: return VK_FORMAT_R16G16B16A16_SFLOAT;  // 2_10_10_10_FLOAT (3, 12), 16_16_16_16 (5), 16_16_16_16_FLOAT (7)
    }
}

bool format_is_64bpp( uint32_t f ) { return f == 5 || f == 7 || f == 15; }

uint32_t xfer_kind( uint32_t f )
{
    switch ( f )
    {
    case 0: case 1: return XK_8888;
    case 3: case 12: return XK_7E3;
    case 5: case 7: case 15: return XK_RAW64;
    default: return XK_RAW32;
    }
}

uint32_t msaa_x( uint32_t msaa ) { return msaa >= 2 ? 2 : 1; }
uint32_t msaa_y( uint32_t msaa ) { return msaa >= 1 ? 2 : 1; }

VkImageView make_view( VkImage image, VkFormat format, VkImageAspectFlags aspect )
{
    VkImageViewCreateInfo vi{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vi.image = image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = format;
    vi.subresourceRange = { aspect, 0, 1, 0, 1 };
    VkImageView v = VK_NULL_HANDLE;
    vkCreateImageView( g_dev, &vi, nullptr, &v );
    return v;
}

VkImage make_image( VkFormat format, uint32_t w, uint32_t h, VkImageUsageFlags usage, VkImageCreateFlags flags, VkDeviceMemory &mem )
{
    VkImageCreateInfo ci{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ci.flags = flags;
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = format;
    ci.extent = { w, h, 1 };
    ci.mipLevels = ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.usage = usage;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImage image;
    if ( vkCreateImage( g_dev, &ci, nullptr, &image ) != VK_SUCCESS ) return VK_NULL_HANDLE;
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements( g_dev, image, &mr );
    VkMemoryAllocateInfo ai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = find_memory( mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT );
    if ( vkAllocateMemory( g_dev, &ai, nullptr, &mem ) != VK_SUCCESS ) { vkDestroyImage( g_dev, image, nullptr ); return VK_NULL_HANDLE; }
    vkBindImageMemory( g_dev, image, mem, 0 );
    return image;
}

// A surface's descriptor set for reading it: guest memory, staging, the tile mask (dynamic) and its views.
VkDescriptorSet surface_set( VkImageView raw, VkImageView depth, VkImageView stencil, VkBuffer mem = VK_NULL_HANDLE )
{
    VkDescriptorSetAllocateInfo ai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    ai.descriptorPool = g_dpool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &g_surface_layout;
    VkDescriptorSet set;
    if ( vkAllocateDescriptorSets( g_dev, &ai, &set ) != VK_SUCCESS ) return VK_NULL_HANDLE;
    VkDescriptorBufferInfo bi[3] = { { mem ? mem : g_mem_buf, 0, mem ? HIRES_SIZE : PHYS_SIZE }, { g_stage_buf, 0, STAGE_SIZE }, { g_dc_buf, 0, EDRAM_MAX_TILES / 8 } };
    VkDescriptorImageInfo ii[3] = { { g_point_sampler, raw, VK_IMAGE_LAYOUT_GENERAL }, { g_point_sampler, depth, VK_IMAGE_LAYOUT_GENERAL },
                                    { g_point_sampler, stencil, VK_IMAGE_LAYOUT_GENERAL } };
    VkWriteDescriptorSet w[6];
    for ( int i = 0; i < 6; ++i )
    {
        w[i] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        w[i].dstSet = set;
        w[i].dstBinding = i;
        w[i].descriptorCount = 1;
        w[i].descriptorType = i == 2 ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC : i < 2 ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        if ( i < 3 ) w[i].pBufferInfo = &bi[i];
        else w[i].pImageInfo = &ii[i - 3];
    }
    vkUpdateDescriptorSets( g_dev, 6, w, 0, nullptr );
    return set;
}

Target *target( uint32_t edram_base, uint32_t format, uint32_t pitch, uint32_t msaa, bool depth )
{
    if ( depth ) format = 0;
    msaa = std::min( msaa, 2u );
    uint64_t key = ( (uint64_t)depth << 63 ) | ( (uint64_t)edram_base << 40 ) | ( (uint64_t)format << 32 ) | ( (uint64_t)msaa << 16 ) | pitch;
    auto it = g_targets.find( key );
    if ( it != g_targets.end() ) return &it->second;
    Target t;
    t.depth = depth;
    t.base = edram_base;
    t.pitch = pitch;
    t.msaa = msaa;
    t.guest_format = format;
    t.bpp64 = !depth && format_is_64bpp( format );
    t.kind = depth ? XK_DEPTH : xfer_kind( format );
    t.format = depth ? VK_FORMAT_D32_SFLOAT_S8_UINT : color_format( format );
    t.host_bpp = t.format == VK_FORMAT_R16G16B16A16_SFLOAT || t.format == VK_FORMAT_R32G32_SFLOAT ? 8 : 4;
    uint32_t tile_w = t.bpp64 ? 40 : 80;
    t.pitch_tiles = std::max( ( pitch * msaa_x( msaa ) + tile_w - 1 ) / tile_w, 1u );
    t.tile_h = 16 / msaa_y( msaa );
    t.w = std::min( pitch, MAX_TARGET_W );
    t.h = std::min( EDRAM_TILES / t.pitch_tiles * t.tile_h, MAX_TARGET_H );
    if ( !t.h ) return nullptr;
    static const bool aa = settings().anti_aliasing;
    bool screen_sized = pitch >= 1280 && pitch <= 1312;
    t.hi8 = !screen_sized ? 8 : msaa >= 1 ? scene_scale8() : render_scale8();
    // Anti-aliasing (2x supersampling of the console's anti-aliased surfaces) at the console's resolution
    // only: on top of a higher one it would render 2560x1440 at 5120x2880 and 4K at 8K, for edges the
    // resolution already smooths.
    t.scale8 = aa && g_msaa_samples <= 1 && !upscaler_ready() && msaa >= 1 && t.hi8 == 8 ? 16 : t.hi8;  // (not with MSAA)
    VkImageUsageFlags usage = ( depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT ) | VK_IMAGE_USAGE_SAMPLED_BIT |
                              VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    t.image = make_image( t.format, at8( t.w, t.scale8 ), at8( t.h, t.scale8 ), usage, depth ? 0 : VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT, t.mem );
    if ( !t.image ) return nullptr;
    t.view = make_view( t.image, t.format, depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT );
    if ( depth )
    {
        t.raw = make_view( t.image, t.format, VK_IMAGE_ASPECT_STENCIL_BIT );
        t.set = surface_set( g_dummy_color.view, t.view, t.raw );
        if ( screen_sized && hires_on() ) t.hi_set = surface_set( g_dummy_color.view, t.view, t.raw, g_hires_buf );
    }
    else
    {
        t.raw = make_view( t.image, t.host_bpp == 8 ? VK_FORMAT_R32G32_UINT : VK_FORMAT_R32_UINT, VK_IMAGE_ASPECT_COLOR_BIT );
        t.set = surface_set( t.raw, g_dummy_depth.view, g_dummy_depth.stencil );
        if ( screen_sized && hires_on() ) t.hi_set = surface_set( t.raw, g_dummy_depth.view, g_dummy_depth.stencil, g_hires_buf );
    }
    t.synced.assign( t.pitch_tiles * ( ( t.h + t.tile_h - 1 ) / t.tile_h ), 0 );
    // MSAA: the game's multisampled screen-sized surfaces (the scene), at the resolution (not supersampled).
    if ( g_msaa_samples > 1 && msaa >= 1 && screen_sized && t.scale8 == t.hi8 )
    {
        VkImageFormatProperties fp;
        const VkImageUsageFlags ms_usage = depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        uint32_t samples = g_msaa_samples;
        if ( vkGetPhysicalDeviceImageFormatProperties( g_phys, t.format, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL, ms_usage, 0, &fp ) == VK_SUCCESS )
            while ( samples > 1 && !( fp.sampleCounts & samples ) ) samples >>= 1;
        else samples = 1;
        if ( samples > 1 )
        {
            VkImageCreateInfo ci{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
            ci.imageType = VK_IMAGE_TYPE_2D;
            ci.format = t.format;
            ci.extent = { (uint32_t)at8( t.w, t.scale8 ), (uint32_t)at8( t.h, t.scale8 ), 1 };
            ci.mipLevels = ci.arrayLayers = 1;
            ci.samples = (VkSampleCountFlagBits)samples;
            ci.usage = ms_usage;
            ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            VkMemoryRequirements mr;
            if ( vkCreateImage( g_dev, &ci, nullptr, &t.ms_image ) == VK_SUCCESS )
            {
                vkGetImageMemoryRequirements( g_dev, t.ms_image, &mr );
                VkMemoryAllocateInfo ai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
                ai.allocationSize = mr.size;
                ai.memoryTypeIndex = find_memory( mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT );
                if ( vkAllocateMemory( g_dev, &ai, nullptr, &t.ms_mem ) == VK_SUCCESS )
                {
                    vkBindImageMemory( g_dev, t.ms_image, t.ms_mem, 0 );
                    t.ms_view = make_view( t.ms_image, t.format, depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT );
                    t.samples = samples;
                }
                else vkDestroyImage( g_dev, t.ms_image, nullptr ), t.ms_image = VK_NULL_HANDLE;
            }
        }
    }
    rt_log( "renderer: %s surface EDRAM %03X format %u pitch %u msaa %ux -> %ux%u (image x%.3g)%s\n", depth ? "depth" : "color", edram_base, format, pitch,
            1u << msaa, t.w, t.h, t.scale8 / 8.0, t.samples > 1 ? ( " with " + std::to_string( t.samples ) + "x MSAA" ).c_str() : "" );
    auto &slot = g_targets[key];
    slot = std::move( t );
    return &slot;
}

// ------------------------------------------------------------------ command recording
VkImage g_rendering_color = VK_NULL_HANDLE, g_rendering_depth = VK_NULL_HANDLE;

void barrier_all()
{
    VkMemoryBarrier mb{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
    mb.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier( g_cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr );
}

void complete( Submission &sub );

// UNDEFINED -> GENERAL (used for everything), and cleared: the console's EDRAM starts out zero.
void first_use( VkImage image, bool depth, bool clear )
{
    VkImageMemoryBarrier ib{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    ib.srcAccessMask = 0;
    ib.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    ib.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    ib.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    ib.srcQueueFamilyIndex = ib.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    ib.image = image;
    VkImageSubresourceRange range{ (VkImageAspectFlags)( depth ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_COLOR_BIT ), 0, 1, 0, 1 };
    ib.subresourceRange = range;
    vkCmdPipelineBarrier( g_cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &ib );
    if ( !clear ) return;
    if ( depth )
    {
        VkClearDepthStencilValue z{ 0.0f, 0 };
        vkCmdClearDepthStencilImage( g_cmd, image, VK_IMAGE_LAYOUT_GENERAL, &z, 1, &range );
    }
    else
    {
        VkClearColorValue c{};
        vkCmdClearColorImage( g_cmd, image, VK_IMAGE_LAYOUT_GENERAL, &c, 1, &range );
    }
    barrier_all();
}

uint32_t g_sub_draws;  // Vulkan draws recorded into the open command buffer
void begin()
{
    if ( g_recording ) return;
    Submission &sub = g_subs[g_cur];
    if ( sub.busy ) complete( sub );  // the oldest in flight: wait for it
    g_cmd = sub.cmd;
    g_sub_draws = 0;
    g_query_pool = sub.queries;
    g_ts_pool = sub.timestamps;
    sub.dc_start = g_dc_head;
    vkResetCommandBuffer( g_cmd, 0 );
    VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer( g_cmd, &bi );
    g_recording = true;
    barrier_all();  // after the previous command buffer's work
    vkCmdResetQueryPool( g_cmd, g_query_pool, 0, QUERY_POOL_SIZE );
    g_query_next = 0;
    if ( g_ts_pool )
    {
        vkCmdResetQueryPool( g_cmd, g_ts_pool, 0, TS_POOL_SIZE );
        g_ts_next = 0;
        g_ts_marks.clear();
        gpu_timer_begin( GT_ALL );
    }
    static bool dummies;
    if ( !dummies )
    {
        dummies = true;
        first_use( g_dummy_color.image, false, true );
        first_use( g_dummy_depth.image, true, true );
        first_use( g_tex_standin.image, false, false );
        VkClearColorValue magenta{ { 1.0f, 0.0f, 1.0f, 1.0f } };
        VkImageSubresourceRange range{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        vkCmdClearColorImage( g_cmd, g_tex_standin.image, VK_IMAGE_LAYOUT_GENERAL, &magenta, 1, &range );
        barrier_all();
    }
}

void end_rendering()
{
    if ( g_rendering_color || g_rendering_depth ) vkCmdEndRendering( g_cmd );
    gpu_timer_end( g_pass_q );
    g_pass_q = ~0u;
    g_rendering_color = g_rendering_depth = VK_NULL_HANDLE;
}

void prepare( Target *t )
{
    if ( !t->fresh ) return;
    t->fresh = false;
    end_rendering();
    first_use( t->image, t->depth, true );
    if ( t->ms_image ) first_use( t->ms_image, t->depth, false ), t->ss_newer = true;  // (taken from image before its first draw)
}

// MSAA (Target::samples): the multisampled image's samples averaged into the surface's image (depth and
// stencil: each pixel's first sample), at the end of a rendering scope that resolves (Vulkan's own).
void ms_resolve( Target *t )
{
    if ( !t->ms_newer ) return;
    t->ms_newer = false;
    end_rendering();
    barrier_all();
    VkRenderingAttachmentInfo at{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    at.imageView = t->ms_view;
    at.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    at.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    at.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    at.resolveMode = t->depth ? VK_RESOLVE_MODE_SAMPLE_ZERO_BIT : VK_RESOLVE_MODE_AVERAGE_BIT;
    at.resolveImageView = t->view;
    at.resolveImageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkRenderingInfo ri{ VK_STRUCTURE_TYPE_RENDERING_INFO };
    ri.renderArea = { { 0, 0 }, { (uint32_t)at8( t->w, t->scale8 ), (uint32_t)at8( t->h, t->scale8 ) } };
    ri.layerCount = 1;
    ri.colorAttachmentCount = t->depth ? 0 : 1;
    ri.pColorAttachments = t->depth ? nullptr : &at;
    ri.pDepthAttachment = t->depth ? &at : nullptr;
    ri.pStencilAttachment = t->depth ? &at : nullptr;
    vkCmdBeginRendering( g_cmd, &ri );
    vkCmdEndRendering( g_cmd );
    barrier_all();
}

// MSAA: the surface's image copied into every sample of the multisampled image (a full-screen draw:
// colour as it is; depth and stencil exported).
extern PFN_vkCmdPushDescriptorSetKHR g_push_descriptors;
VkDescriptorSetLayout g_msup_layout;
VkPipelineLayout g_msup_pl;
std::unordered_map<uint64_t, VkPipeline> g_msup_pipes;
void ms_upload( Target *t )
{
    if ( !t->ss_newer ) return;
    t->ss_newer = false;
    end_rendering();
    static const char *vs_src = R"GLSL(#version 450
void main() { vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2); gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0); }
)GLSL";
    static const char *color_src = R"GLSL(#version 450
layout(set = 0, binding = 0) uniform sampler2D src;
layout(location = 0) out vec4 o;
void main() { o = texelFetch(src, ivec2(gl_FragCoord.xy), 0); }
)GLSL";
    // Depth, its stencil cleared; then (without stencil export) each stencil bit set where the image has it,
    // a pass a bit (the bit's write mask, the others' pixels discarded).
    static const char *depth_src = R"GLSL(#version 450
layout(set = 0, binding = 0) uniform sampler2D src;
void main() { gl_FragDepth = texelFetch(src, ivec2(gl_FragCoord.xy), 0).r; }
)GLSL";
    static const char *bit_src = R"GLSL(#version 450
layout(set = 0, binding = 1) uniform usampler2D stencil;
layout(push_constant) uniform P { uint bit; } pc;
void main() { if (((texelFetch(stencil, ivec2(gl_FragCoord.xy), 0).r >> pc.bit) & 1u) == 0u) discard; }
)GLSL";
    if ( !g_msup_layout )
    {
        VkDescriptorSetLayoutBinding b[2]{};
        for ( uint32_t i = 0; i < 2; ++i ) b[i] = { i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
        VkDescriptorSetLayoutCreateInfo li{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        li.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
        li.bindingCount = 2;
        li.pBindings = b;
        vkCreateDescriptorSetLayout( g_dev, &li, nullptr, &g_msup_layout );
        VkPushConstantRange pr{ VK_SHADER_STAGE_FRAGMENT_BIT, 0, 4 };
        VkPipelineLayoutCreateInfo pli{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        pli.setLayoutCount = 1;
        pli.pSetLayouts = &g_msup_layout;
        pli.pushConstantRangeCount = 1;
        pli.pPushConstantRanges = &pr;
        vkCreatePipelineLayout( g_dev, &pli, nullptr, &g_msup_pl );
    }
    // pass 0: colour, or depth with stencil 0; pass 1: a stencil bit.
    auto make = [&]( int pass ) -> VkPipeline {
        VkPipeline pipe = VK_NULL_HANDLE;
        static VkShaderModule mv = compile( vs_src, shaderc_vertex_shader, "msaa_upload_vs", nullptr );
        static VkShaderModule mc = compile( color_src, shaderc_fragment_shader, "msaa_upload_color", nullptr );
        static VkShaderModule md = compile( depth_src, shaderc_fragment_shader, "msaa_upload_depth", nullptr );
        static VkShaderModule mb = compile( bit_src, shaderc_fragment_shader, "msaa_upload_stencil_bit", nullptr );
        VkShaderModule mf = !t->depth ? mc : pass ? mb : md;
        VkPipelineShaderStageCreateInfo st[2]{ { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO }, { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO } };
        st[0].stage = VK_SHADER_STAGE_VERTEX_BIT, st[0].module = mv, st[0].pName = "main";
        st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT, st[1].module = mf, st[1].pName = "main";
        VkPipelineVertexInputStateCreateInfo vin{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
        VkPipelineInputAssemblyStateCreateInfo ia{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vp{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
        vp.viewportCount = vp.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rs{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
        rs.polygonMode = VK_POLYGON_MODE_FILL, rs.cullMode = VK_CULL_MODE_NONE, rs.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo ms{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
        ms.rasterizationSamples = (VkSampleCountFlagBits)t->samples;
        VkPipelineDepthStencilStateCreateInfo ds{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
        VkStencilOpState so{ VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_REPLACE, VK_COMPARE_OP_ALWAYS, 0xFF, 0xFF, pass ? 0xFFu : 0u };
        if ( t->depth )
            ds.depthTestEnable = VK_TRUE, ds.depthWriteEnable = !pass, ds.depthCompareOp = VK_COMPARE_OP_ALWAYS, ds.stencilTestEnable = VK_TRUE, ds.front = ds.back = so;
        VkPipelineColorBlendAttachmentState ba{};
        ba.colorWriteMask = 0xF;
        VkPipelineColorBlendStateCreateInfo cb{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
        cb.attachmentCount = t->depth ? 0 : 1;
        cb.pAttachments = &ba;
        VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK };
        VkPipelineDynamicStateCreateInfo dy{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
        dy.dynamicStateCount = t->depth ? 3 : 2, dy.pDynamicStates = dyn;
        VkFormat cf = t->format;
        VkPipelineRenderingCreateInfo rinfo{ VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
        rinfo.colorAttachmentCount = t->depth ? 0 : 1;
        rinfo.pColorAttachmentFormats = &cf;
        rinfo.depthAttachmentFormat = rinfo.stencilAttachmentFormat = t->depth ? t->format : VK_FORMAT_UNDEFINED;
        VkGraphicsPipelineCreateInfo ci{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
        ci.pNext = &rinfo;
        ci.stageCount = 2, ci.pStages = st;
        ci.pVertexInputState = &vin, ci.pInputAssemblyState = &ia, ci.pViewportState = &vp, ci.pRasterizationState = &rs;
        ci.pMultisampleState = &ms, ci.pDepthStencilState = &ds, ci.pColorBlendState = &cb, ci.pDynamicState = &dy;
        ci.layout = g_msup_pl;
        if ( !mv || !mf || vkCreateGraphicsPipelines( g_dev, g_pcache, 1, &ci, nullptr, &pipe ) != VK_SUCCESS ) pipe = VK_NULL_HANDLE;
        return pipe;
    };
    VkPipeline &pipe = g_msup_pipes[(uint64_t)t->format << 8 | t->samples], &bit_pipe = g_msup_pipes[(uint64_t)t->format << 8 | t->samples | 1u << 7];
    if ( !pipe ) pipe = make( 0 );
    if ( t->depth && !bit_pipe ) bit_pipe = make( 1 );
    if ( !pipe || ( t->depth && !bit_pipe ) ) { rt_log( "renderer: MSAA's copy into a multisampled image didn't build\n" ); return; }
    barrier_all();
    const uint32_t w = at8( t->w, t->scale8 ), h = at8( t->h, t->scale8 );
    VkRenderingAttachmentInfo at{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    at.imageView = t->ms_view;
    at.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    at.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    at.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri{ VK_STRUCTURE_TYPE_RENDERING_INFO };
    ri.renderArea = { { 0, 0 }, { w, h } };
    ri.layerCount = 1;
    ri.colorAttachmentCount = t->depth ? 0 : 1;
    ri.pColorAttachments = t->depth ? nullptr : &at;
    ri.pDepthAttachment = t->depth ? &at : nullptr;
    ri.pStencilAttachment = t->depth ? &at : nullptr;
    vkCmdBeginRendering( g_cmd, &ri );
    vkCmdBindPipeline( g_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe );
    static VkSampler nearest = [] {
        VkSamplerCreateInfo si{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        VkSampler sm = VK_NULL_HANDLE;
        vkCreateSampler( g_dev, &si, nullptr, &sm );
        return sm;
    }();
    VkDescriptorImageInfo src{ nearest, t->view, VK_IMAGE_LAYOUT_GENERAL }, sten{ nearest, t->depth ? t->raw : t->view, VK_IMAGE_LAYOUT_GENERAL };
    VkWriteDescriptorSet wr[2]{ { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET }, { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET } };
    wr[0].dstBinding = 0, wr[0].descriptorCount = 1, wr[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, wr[0].pImageInfo = &src;
    wr[1].dstBinding = 1, wr[1].descriptorCount = 1, wr[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, wr[1].pImageInfo = &sten;
    g_push_descriptors( g_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_msup_pl, 0, t->depth ? 2 : 1, wr );
    VkViewport vpt{ 0, 0, (float)w, (float)h, 0, 1 };
    VkRect2D sc{ { 0, 0 }, { w, h } };
    vkCmdSetViewport( g_cmd, 0, 1, &vpt );
    vkCmdSetScissor( g_cmd, 0, 1, &sc );
    if ( t->depth ) vkCmdSetStencilWriteMask( g_cmd, VK_STENCIL_FACE_FRONT_AND_BACK, 0xFF );
    vkCmdDraw( g_cmd, 3, 1, 0, 0 );
    if ( t->depth )
    {
        vkCmdBindPipeline( g_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, bit_pipe );
        for ( uint32_t bit = 0; bit < 8; ++bit )
        {
            vkCmdSetStencilWriteMask( g_cmd, VK_STENCIL_FACE_FRONT_AND_BACK, 1u << bit );
            vkCmdPushConstants( g_cmd, g_msup_pl, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 4, &bit );
            vkCmdDraw( g_cmd, 3, 1, 0, 0 );
        }
    }
    vkCmdEndRendering( g_cmd );
    barrier_all();
}

// Space in the draw-constant ring (host-visible, read by the GPU): waits for a command buffer in flight
// still reading where it would go.
VkDeviceSize dc_alloc( VkDeviceSize size )
{
    size = ( size + g_dc_align - 1 ) & ~( g_dc_align - 1 );
    if ( g_dc_head + size > g_dc_size ) g_dc_head = 0;  // wrap
    for ( Submission &sub : g_subs )
        if ( sub.busy && ( sub.dc_start <= sub.dc_end ? g_dc_head < sub.dc_end && g_dc_head + size > sub.dc_start
                                                      : g_dc_head < sub.dc_end || g_dc_head + size > sub.dc_start ) )
            complete( sub );
    VkDeviceSize at = g_dc_head;
    g_dc_head += size;
    return at;
}

// A draw's guest indices as a Vulkan index buffer in the draw-constant ring: byte order made the host's
// (`endian` as in the index buffer's fetch), 32-bit ones cut to the Xenos's 24 bits, and the strip reset
// index (`reset`, ~0 for none) made Vulkan's primitive restart (all ones). 16-bit indices stay 16-bit
// unless a reset index other than 0xFFFF would need the value 0xFFFF free.
// The largest index of a guest index buffer + 1 (DLSS's motion: its vertices' slots), read from guest
// memory (cached: the converted copy is in write-combined memory, very slow to read back), swapped as
// index_buffer does; the restart index counts as none.
uint32_t index_top( const uint8_t *src, uint32_t count, bool wide, uint32_t endian, uint32_t reset )
{
    static const __m256i swaps[4] = {
        _mm256_setr_epi8( 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 ),
        _mm256_setr_epi8( 1, 0, 3, 2, 5, 4, 7, 6, 9, 8, 11, 10, 13, 12, 15, 14, 1, 0, 3, 2, 5, 4, 7, 6, 9, 8, 11, 10, 13, 12, 15, 14 ),
        _mm256_setr_epi8( 3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12, 3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12 ),
        _mm256_setr_epi8( 2, 3, 0, 1, 6, 7, 4, 5, 10, 11, 8, 9, 14, 15, 12, 13, 2, 3, 0, 1, 6, 7, 4, 5, 10, 11, 8, 9, 14, 15, 12, 13 ) };
    const __m256i swap = swaps[endian & 3];
    auto word = [&]( uint32_t i ) {  // the 32-bit word holding index i, swapped
        uint32_t w;
        memcpy( &w, src + ( wide ? i * 4 : ( i & ~1u ) * 2 ), 4 );
        return endian == 1 ? ( ( w & 0x00FF00FF ) << 8 ) | ( ( w >> 8 ) & 0x00FF00FF ) : endian == 2 ? __builtin_bswap32( w ) : endian == 3 ? ( w << 16 ) | ( w >> 16 ) : w;
    };
    uint32_t top = 0, i = 0;
    if ( !wide )
    {
        const uint16_t r = (uint16_t)reset;
        const __m256i one = _mm256_set1_epi16( 1 ), rv = _mm256_set1_epi16( (short)r ), ones = _mm256_set1_epi16( -1 );
        __m256i m = _mm256_setzero_si256();
        for ( ; i + 16 <= count; i += 16 )
        {
            __m256i v = _mm256_shuffle_epi8( _mm256_loadu_si256( (const __m256i *)( src + i * 2 ) ), swap );
            v = _mm256_blendv_epi8( v, ones, _mm256_cmpeq_epi16( v, rv ) );
            m = _mm256_max_epu16( m, _mm256_add_epi16( v, one ) );
        }
        alignas( 32 ) uint16_t lanes[16];
        _mm256_store_si256( (__m256i *)lanes, m );
        for ( uint16_t l : lanes ) top = std::max<uint32_t>( top, l );
        for ( ; i < count; ++i )
        {
            const uint32_t w = word( i );
            const uint16_t v = ( i & 1 ) ? w >> 16 : w & 0xFFFF;
            if ( v != r ) top = std::max<uint32_t>( top, v + 1u );
        }
    }
    else
    {
        const uint32_t r = reset & 0xFFFFFF;
        const __m256i one = _mm256_set1_epi32( 1 ), rv = _mm256_set1_epi32( (int)r ), ones = _mm256_set1_epi32( -1 ), mask = _mm256_set1_epi32( 0xFFFFFF );
        __m256i m = _mm256_setzero_si256();
        for ( ; i + 8 <= count; i += 8 )
        {
            __m256i v = _mm256_and_si256( _mm256_shuffle_epi8( _mm256_loadu_si256( (const __m256i *)( src + i * 4 ) ), swap ), mask );
            v = _mm256_blendv_epi8( v, ones, _mm256_cmpeq_epi32( v, rv ) );
            m = _mm256_max_epu32( m, _mm256_add_epi32( v, one ) );
        }
        alignas( 32 ) uint32_t lanes[8];
        _mm256_store_si256( (__m256i *)lanes, m );
        for ( uint32_t l : lanes ) top = std::max( top, l );
        for ( ; i < count; ++i )
            if ( const uint32_t v = word( i ) & 0xFFFFFF; v != r ) top = std::max( top, v + 1 );
    }
    return top;
}

VkDeviceSize index_buffer( const uint8_t *src, uint32_t count, bool wide, uint32_t endian, uint32_t reset, VkIndexType &type )
{
    const bool narrow = !wide && ( reset == ~0u || ( reset & 0xFFFF ) == 0xFFFF );
    type = narrow ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
    const uint32_t words = narrow ? ( count + 1 ) / 2 : count;  // 32-bit words written
    VkDeviceSize at = dc_alloc( (VkDeviceSize)words * 4 + 32 );
    uint32_t *out = (uint32_t *)( g_dc_ptr + at );
    // xswap per 32-bit word (endian 1: bytes in halfwords, 2: bytes in words, 3: halfwords in words)
    static const __m256i swaps[4] = {
        _mm256_setr_epi8( 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 ),
        _mm256_setr_epi8( 1, 0, 3, 2, 5, 4, 7, 6, 9, 8, 11, 10, 13, 12, 15, 14, 1, 0, 3, 2, 5, 4, 7, 6, 9, 8, 11, 10, 13, 12, 15, 14 ),
        _mm256_setr_epi8( 3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12, 3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12 ),
        _mm256_setr_epi8( 2, 3, 0, 1, 6, 7, 4, 5, 10, 11, 8, 9, 14, 15, 12, 13, 2, 3, 0, 1, 6, 7, 4, 5, 10, 11, 8, 9, 14, 15, 12, 13 ) };
    const __m256i swap = swaps[endian & 3];
    if ( narrow )
    {
        // 16-bit: each guest word, swapped, holds index 2k in its low half and 2k + 1 in its high half --
        // already a little-endian pair; a reset of 0xFFFF is Vulkan's restart value as it is.
        for ( uint32_t i = 0; i < words; i += 8 )
            _mm256_storeu_si256( (__m256i *)( out + i ), _mm256_shuffle_epi8( _mm256_loadu_si256( (const __m256i *)( src + i * 4 ) ), swap ) );
    }
    else if ( wide )
    {
        const __m256i mask = _mm256_set1_epi32( 0xFFFFFF ), want = _mm256_set1_epi32( (int)( reset & 0xFFFFFF ) ), ones = _mm256_set1_epi32( -1 );
        for ( uint32_t i = 0; i < words; i += 8 )
        {
            __m256i v = _mm256_and_si256( _mm256_shuffle_epi8( _mm256_loadu_si256( (const __m256i *)( src + i * 4 ) ), swap ), mask );
            if ( reset != ~0u ) v = _mm256_blendv_epi8( v, ones, _mm256_cmpeq_epi32( v, want ) );
            _mm256_storeu_si256( (__m256i *)( out + i ), v );
        }
    }
    else
    {
        // 16-bit with a reset index other than 0xFFFF: widened to 32 bits.
        const uint16_t want = (uint16_t)reset;
        for ( uint32_t i = 0; i < count; ++i )
        {
            uint32_t w;
            memcpy( &w, src + ( i & ~1u ) * 2, 4 );
            w = endian == 1 ? ( ( w & 0x00FF00FF ) << 8 ) | ( ( w >> 8 ) & 0x00FF00FF ) : endian == 2 ? __builtin_bswap32( w ) : endian == 3 ? ( w << 16 ) | ( w >> 16 ) : w;
            uint16_t v = ( i & 1 ) ? w >> 16 : w & 0xFFFF;
            out[i] = v == want ? ~0u : v;
        }
    }
    return at;
}

struct RtStats { uint32_t copies, clears, flushes, vk_draws; uint64_t copy_px; double wait_ms; } g_rt_stats;
bool g_edram_trace;  // log every surface operation of one frame (CRACKDOWN_EDRAM_TRACE=frame number)
bool g_aniso_supported;
bool g_depth_clamp;      // device supports depth clamping (draws whose depth clipping is off)
uint32_t g_draw_census;  // nonzero during the CRACKDOWN_DRAW_CENSUS frame: next draw number + 1

const char *view_name( const Target *t )
{
    static char b[4][64];
    static int n;
    char *s = b[n++ & 3];
    if ( !t ) return "none";
    snprintf( s, 64, "%s %03X/%u/%ux/f%u", t->depth ? "D" : "C", t->base, t->pitch, 1u << t->msaa, t->guest_format );
    return s;
}

// The tiles under pixel rectangle [x0, x1) x [y0, y1) of a surface: f(tile index in the surface).
template <class F> void each_tile( const Target *t, int32_t x0, int32_t y0, int32_t x1, int32_t y1, F f )
{
    x0 = std::max( x0, 0 ); y0 = std::max( y0, 0 );
    x1 = std::min( x1, (int32_t)t->w ); y1 = std::min( y1, (int32_t)t->h );
    if ( x1 <= x0 || y1 <= y0 ) return;
    uint32_t tw = t->bpp64 ? 40 : 80, sx = msaa_x( t->msaa );
    uint32_t c0 = x0 * sx / tw, c1 = std::min( ( x1 * sx - 1 ) / tw + 1, t->pitch_tiles );
    uint32_t r0 = y0 / t->tile_h, r1 = ( y1 - 1 ) / t->tile_h + 1;
    for ( uint32_t r = r0; r < r1; ++r )
        for ( uint32_t c = c0; c < c1; ++c ) f( r * t->pitch_tiles + c );
}

void mark_tiles( Target *t, int32_t x0, int32_t y0, int32_t x1, int32_t y1 );
// A surface wrote that rectangle (its image holds the tiles' newest contents now).
void mark_written( Target *t, int32_t x0, int32_t y0, int32_t x1, int32_t y1 )
{
    // Draw after draw to the same surfaces: nothing to do while nothing else touched the tiles since.
    if ( t->owned_epoch == g_tile_epoch && x0 >= t->owned[0] && y0 >= t->owned[1] && x1 <= t->owned[2] && y1 <= t->owned[3] ) return;
    bool fresh_version = false;
    each_tile( t, x0, y0, x1, y1, [&]( uint32_t i ) {
        uint32_t tile = t->tile( i );
        fresh_version |= g_tile_writer[tile] != t || g_tile_read[tile];
    } );
    if ( fresh_version ) mark_tiles( t, x0, y0, x1, y1 );
    t->owned[0] = x0, t->owned[1] = y0, t->owned[2] = x1, t->owned[3] = y1;
    t->owned_epoch = g_tile_epoch;
}
void mark_tiles( Target *t, int32_t x0, int32_t y0, int32_t x1, int32_t y1 )
{
    ++g_tile_epoch;
    ++g_ver;
    each_tile( t, x0, y0, x1, y1, [&]( uint32_t i ) {
        uint32_t tile = t->tile( i );
        g_tile_writer[tile] = t;
        g_tile_ver[tile] = g_ver;
        g_tile_read[tile] = false;
        t->synced[i] = g_ver;
    } );
}

// Rows [y0, y1) of dst take src's words where dst's tiles are in mask (EDRAM tiles), through the
// staging buffer, in row chunks (of image rows) that fit it.
void copy_surface( Target *dst, const Target *src, const std::vector<uint32_t> &mask, int32_t y0, int32_t y1, bool whole_rows )
{
    ms_resolve( const_cast<Target *>( src ) );
    ms_resolve( dst );
    if ( dst->ms_image ) dst->ss_newer = true;
    end_rendering();
    struct TimerScope { uint32_t q; ~TimerScope() { gpu_timer_end( q ); } } timer{ gpu_timer_begin( GT_LOAD ) };
    VkDeviceSize mask_at = dc_alloc( EDRAM_MAX_TILES / 8 );
    memcpy( g_dc_ptr + mask_at, mask.data(), EDRAM_MAX_TILES / 8 );
    uint32_t w = at8( dst->w, dst->scale8 ), dyn = (uint32_t)mask_at;
    VkDeviceSize row = (VkDeviceSize)w * ( dst->host_bpp + ( dst->depth ? 1 : 0 ) ) + 8;
    int32_t chunk = (int32_t)std::max<VkDeviceSize>( STAGE_SIZE / row, 1 );
    const int32_t rows = y1 - y0;  // surface rows, for the stats
    y0 = at8( y0, dst->scale8 ), y1 = at8( y1, dst->scale8 );
    for ( int32_t y = y0; y < y1; y += chunk )
    {
        uint32_t h = std::min( chunk, y1 - y );
        uint32_t stencil_off = ( w * h * 4 + 3 ) & ~3u;
        VkBufferImageCopy r[2]{};
        r[0].imageSubresource = { (VkImageAspectFlags)( dst->depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT ), 0, 0, 1 };
        r[0].imageOffset = { 0, y, 0 };
        r[0].imageExtent = { w, h, 1 };
        r[1] = r[0];
        r[1].bufferOffset = stencil_off;
        r[1].imageSubresource.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
        uint32_t nr = dst->depth ? 2 : 1;
        if ( !whole_rows )  // the pixels the copy leaves alone keep their values
        {
            vkCmdCopyImageToBuffer( g_cmd, dst->image, VK_IMAGE_LAYOUT_GENERAL, g_stage_buf, nr, r );
            barrier_all();
        }
        struct { uint32_t base, pitch, msaa, kind, bpp64; int32_t x0, y0, w, h; uint32_t stencil_off, sbase, spitch, smsaa, skind, sbpp64, scale, sscale; } pc{
            dst->base, dst->pitch, dst->msaa, dst->kind, dst->bpp64, 0, y, (int32_t)w, (int32_t)h, stencil_off, src->base, src->pitch, src->msaa, src->kind,
            src->bpp64, dst->scale8, src->scale8 };
        vkCmdBindPipeline( g_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_copy_pipe );
        vkCmdBindDescriptorSets( g_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_surface_pl, 0, 1, &src->set, 1, &dyn );
        vkCmdPushConstants( g_cmd, g_surface_pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof( pc ), &pc );
        vkCmdDispatch( g_cmd, ( w + 7 ) / 8, ( h + 7 ) / 8, 1 );
        barrier_all();
        vkCmdCopyBufferToImage( g_cmd, g_stage_buf, dst->image, VK_IMAGE_LAYOUT_GENERAL, nr, r );
        barrier_all();
    }
    g_rt_stats.copies++;
    g_rt_stats.copy_px += (uint64_t)dst->w * rows;
}

// Brings rows [0, need_h) of a surface up to date: the tiles other surfaces (but its partner, bound
// with it) wrote since it last caught up are copied from them.
void sync_surface( Target *t, const Target *partner, uint32_t need_h )
{
    prepare( t );
    need_h = std::min( need_h, t->h );
    if ( t->checked == g_ver && t->checked_partner == partner && need_h <= t->checked_h ) return;
    t->checked = g_ver;
    t->checked_partner = partner;
    t->checked_h = std::max( t->checked_h, need_h );
    struct Stale { const Target *from; std::vector<uint32_t> mask; uint32_t r0 = UINT32_MAX, r1 = 0, tiles = 0; };
    std::vector<Stale> stale;
    std::vector<uint32_t> behind;  // tiles of t that catch up
    uint32_t rows = ( need_h + t->tile_h - 1 ) / t->tile_h;
    for ( uint32_t i = 0; i < rows * t->pitch_tiles; ++i )
    {
        uint32_t tile = t->tile( i );
        const Target *w = g_tile_writer[tile];
        if ( !w || w == t || w == partner || g_tile_ver[tile] <= t->synced[i] ) continue;
        Stale *s = nullptr;
        for ( Stale &x : stale ) if ( x.from == w ) s = &x;
        if ( !s ) { stale.push_back( { w, std::vector<uint32_t>( EDRAM_MAX_TILES / 32 ) } ); s = &stale.back(); }
        s->mask[tile >> 5] |= 1u << ( tile & 31 );
        uint32_t r = i / t->pitch_tiles;
        s->r0 = std::min( s->r0, r );
        s->r1 = std::max( s->r1, r + 1 );
        s->tiles++;
        behind.push_back( i );
    }
    for ( Stale &s : stale )
    {
        int32_t y0 = s.r0 * t->tile_h, y1 = std::min( s.r1 * t->tile_h, t->h );
        bool whole = s.tiles == ( s.r1 - s.r0 ) * t->pitch_tiles;
        if ( g_edram_trace ) rt_log( "edram: %s rows %d-%d (%u tiles) from %s\n", view_name( t ), y0, y1, s.tiles, view_name( s.from ) );
        copy_surface( t, s.from, s.mask, y0, y1, whole );
    }
    for ( uint32_t i : behind )
    {
        uint32_t tile = t->tile( i );
        t->synced[i] = g_tile_ver[tile];
        g_tile_read[tile] = true;
    }
    if ( !behind.empty() ) ++g_tile_epoch;
}

// A colour surface's clear value from the console's words for its format (lo, hi).
VkClearColorValue color_clear_value( uint32_t format, uint32_t lo, uint32_t hi )
{
    auto half = []( uint32_t h ) { return _cvtsh_ss( (unsigned short)h ); };
    auto f7e3 = []( uint32_t v ) { uint32_t e = ( v >> 7 ) & 7, m = v & 127; return e == 0 ? m / 512.0f : ( 1.0f + m / 128.0f ) * std::ldexp( 1.0f, (int)e - 3 ); };
    auto as_f = []( uint32_t v ) { float f; memcpy( &f, &v, 4 ); return f; };
    VkClearColorValue v{};
    float *c = v.float32;
    switch ( format )
    {
    case 0: case 1: c[0] = ( ( lo >> 16 ) & 255 ) / 255.0f; c[1] = ( ( lo >> 8 ) & 255 ) / 255.0f; c[2] = ( lo & 255 ) / 255.0f; c[3] = ( lo >> 24 ) / 255.0f; break;
    case 2: case 10: c[0] = ( lo & 1023 ) / 1023.0f; c[1] = ( ( lo >> 10 ) & 1023 ) / 1023.0f; c[2] = ( ( lo >> 20 ) & 1023 ) / 1023.0f; c[3] = ( lo >> 30 ) / 3.0f; break;
    case 3: case 12: c[0] = f7e3( lo & 1023 ); c[1] = f7e3( ( lo >> 10 ) & 1023 ); c[2] = f7e3( ( lo >> 20 ) & 1023 ); c[3] = ( lo >> 30 ) / 3.0f; break;
    case 4: case 6: c[0] = half( lo & 0xFFFF ); c[1] = half( lo >> 16 ); break;
    case 14: c[0] = as_f( lo ); break;
    case 15: c[0] = as_f( lo ); c[1] = as_f( hi ); break;
    default: c[0] = half( lo & 0xFFFF ); c[1] = half( lo >> 16 ); c[2] = half( hi & 0xFFFF ); c[3] = half( hi >> 16 ); break;
    }
    return v;
}

// Clears a rectangle of a surface (aspects: colour, or depth and / or stencil). The tiles it covers
// whole need nothing from other surfaces first; the rest of the ones it touches catch up.
void clear_surface( Target *t, int32_t x0, int32_t y0, int32_t x1, int32_t y1, VkImageAspectFlags aspects, VkClearValue value )
{
    x0 = std::max( x0, 0 ); y0 = std::max( y0, 0 );
    x1 = std::min( x1, (int32_t)t->w ); y1 = std::min( y1, (int32_t)t->h );
    if ( x1 <= x0 || y1 <= y0 ) return;
    bool whole = !t->depth || aspects == ( VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT );
    if ( whole )
    {
        uint32_t tw = ( t->bpp64 ? 40 : 80 ) / msaa_x( t->msaa );  // pixels per tile across
        each_tile( t, x0, y0, x1, y1, [&]( uint32_t i ) {
            int32_t tx0 = ( i % t->pitch_tiles ) * tw, ty0 = ( i / t->pitch_tiles ) * t->tile_h;
            int32_t tx1 = std::min( tx0 + (int32_t)tw, (int32_t)t->w ), ty1 = std::min( ty0 + (int32_t)t->tile_h, (int32_t)t->h );
            if ( x0 <= tx0 && x1 >= tx1 && y0 <= ty0 && y1 >= ty1 ) t->synced[i] = g_tile_ver[t->tile( i )];
        } );
        t->checked = 0;
    }
    sync_surface( t, nullptr, y1 );
    end_rendering();
    VkClearAttachment ca{ aspects, 0, value };
    VkRenderingAttachmentInfo at{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    at.imageView = t->view;
    at.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    at.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    at.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri{ VK_STRUCTURE_TYPE_RENDERING_INFO };
    const uint32_t k = t->scale8;
    ri.renderArea = { { at8( x0, k ), at8( y0, k ) }, { (uint32_t)( at8( x1, k ) - at8( x0, k ) ), (uint32_t)( at8( y1, k ) - at8( y0, k ) ) } };
    ri.layerCount = 1;
    ri.colorAttachmentCount = t->depth ? 0 : 1;
    ri.pColorAttachments = t->depth ? nullptr : &at;
    ri.pDepthAttachment = t->depth ? &at : nullptr;
    ri.pStencilAttachment = t->depth ? &at : nullptr;
    barrier_all();
    vkCmdBeginRendering( g_cmd, &ri );
    VkClearRect cr{ ri.renderArea, 0, 1 };
    vkCmdClearAttachments( g_cmd, 1, &ca, 1, &cr );
    vkCmdEndRendering( g_cmd );
    if ( t->ms_image )  // (the same in the multisampled image: which of the two is newer elsewhere stays so)
    {
        at.imageView = t->ms_view;
        vkCmdBeginRendering( g_cmd, &ri );
        vkCmdClearAttachments( g_cmd, 1, &ca, 1, &cr );
        vkCmdEndRendering( g_cmd );
    }
    barrier_all();
    mark_written( t, x0, y0, x1, y1 );
    if ( g_edram_trace ) rt_log( "edram: clear %s %d,%d-%d,%d aspects %X\n", view_name( t ), x0, y0, x1, y1, aspects );
    g_rt_stats.clears++;
}

// Clear with the console's words (lo, hi; depth: 24-bit depth << 8 | stencil).
void clear_surface( Target *t, int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint32_t lo, uint32_t hi )
{
    VkClearValue v{};
    if ( t->depth ) v.depthStencil = { ( lo >> 8 ) / 16777215.0f, lo & 0xFF };
    else v.color = color_clear_value( t->guest_format, lo, hi );
    clear_surface( t, x0, y0, x1, y1, t->depth ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_COLOR_BIT, v );
}

// ------------------------------------------------------------------ Textures
// A texture the game's shaders sample in 2D is a Vulkan image with its mip levels, made from guest
// memory -- its fetch constant says where and how it is laid out (tiling, endian, mip packing:
// prelude.glsl.h tex_level) -- and made again once that memory has changed: the CPU's writes are seen
// through page protection (memwatch.h), the GPU's (resolves) and the kernel's (file reads) are reported.
// DXT textures stay compressed (BC1-3, the same blocks once byte-swapped); the rest are decoded. Shaders
// sample them with samplers made from the fetch constant (prelude.glsl.h tfetch); the images and
// samplers are two descriptor arrays, a draw passes each fetch constant's indices (DrawConstants tex).
constexpr uint32_t MAX_TEXTURES = 16384, MAX_CUBES = 2048, MAX_SAMPLER_GROUPS = 1024, SAMPLERS_PER_GROUP = 12;
VkDescriptorSetLayout g_tex_layout, g_upload_layout;
VkDescriptorPool g_tex_pool;
VkDescriptorSet g_tex_set;
VkPipelineLayout g_upload_pl;
VkPipeline g_upload_pipe, g_upload_bc_pipe;
PFN_vkCmdPushDescriptorSetKHR g_push_descriptors;

struct TexKey
{
    uint32_t d0, d1, d2, d5;  // the fetch constant's words that decide the image (d5: mip address, packing, top level)
    bool operator==( const TexKey & ) const = default;
};
struct TexKeyHash { size_t operator()( const TexKey &k ) const { return XXH3_64bits( &k, sizeof( k ) ); } };
struct Texture
{
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    std::vector<VkImageView> levels;  // decoded formats: each level (and face) as a storage image, level * layers + face
    VkFormat format;
    bool bc, cube = false, fresh = true;
    uint32_t slot, w, h, nlevels, layers = 1;  // cube maps: six layers, the faces
    uint32_t base, size, mips, mips_size;  // the guest memory it is made from
    uint32_t made = 0;     // memwatch write number it was made at (0: not yet)
    uint32_t checked = 0;  // memwatch serial when it was last found current
    uint64_t hash = 0;     // debug (CRACKDOWN_TEX_STALE_CHECK)
    bool hires = false;    // made from a full size resolve (g_hires_buf), not guest memory
    bool font = false;     // the game's font drawn again at the resolution's size (fontatlas.cpp), not guest memory
    uint32_t hires_gen = 0;
};
std::unordered_map<TexKey, Texture, TexKeyHash> g_textures;
uint32_t g_tex_slots = 1, g_cube_slots = 1;  // descriptor array entries used (0: the stand-ins)
std::vector<uint32_t> g_slot_size, g_cube_slot_size;  // per slot: width - 1 | height - 1 << 16 (DrawConstants tsize)
void slot_size( bool cube, uint32_t slot, uint32_t w, uint32_t h )
{
    auto &v = cube ? g_cube_slot_size : g_slot_size;
    if ( v.size() <= slot ) v.resize( slot + 64, 0 );
    v[slot] = ( w - 1 ) | ( h - 1 ) << 16;
}
struct TexStats { uint32_t made, remade; uint64_t bytes; } g_tex_stats;

bool tex_bc( uint32_t f ) { return f == 18 || f == 19 || f == 20; }

// The host format a texture format is made into (VK_FORMAT_UNDEFINED: none, the stand-in is used).
VkFormat tex_host_format( uint32_t f )
{
    switch ( f )
    {
    case 18: return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;  // DXT1
    case 19: return VK_FORMAT_BC2_UNORM_BLOCK;       // DXT2/3
    case 20: return VK_FORMAT_BC3_UNORM_BLOCK;       // DXT4/5
    case 2: case 3: case 4: case 6: case 10: case 15: return VK_FORMAT_R8G8B8A8_UNORM;
    case 7: case 24: case 25: case 26: return VK_FORMAT_R16G16B16A16_UNORM;
    case 30: case 31: case 32: return VK_FORMAT_R16G16B16A16_SFLOAT;
    // Depth (24_8, 24_8_FLOAT) and 32_FLOAT are one value (read in every channel, as the decode gives it,
    // through the view's swizzle), 32_32_FLOAT two: a quarter / half the bytes of four floats -- the shadow
    // map and the scene's depth are sampled many times a pixel.
    case 22: case 23: case 36: return VK_FORMAT_R32_SFLOAT;
    case 37: return VK_FORMAT_R32G32_SFLOAT;
    case 38: return VK_FORMAT_R32G32B32A32_SFLOAT;
    default: return VK_FORMAT_UNDEFINED;
    }
}
// What a texture's sampled view reads in each channel (the decode's vec4 for the narrower host formats).
VkComponentMapping tex_view_swizzle( VkFormat host )
{
    if ( host == VK_FORMAT_R32_SFLOAT ) return { VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R };
    if ( host == VK_FORMAT_R32G32_SFLOAT ) return { VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ONE };
    return { VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY };
}

uint32_t tex_log_bpp( uint32_t f )
{
    switch ( f )
    {
    case 2: return 0;
    case 3: case 4: case 10: case 15: case 24: case 30: return 1;
    case 18: case 26: case 32: case 35: case 37: return 3;
    case 19: case 20: case 38: return 4;
    default: return 2;
    }
}

// The guest memory of a texture's levels (as prelude.glsl.h tex_level_face lays them out): the base
// level at d1's address, levels 1.. from d5's, each level whose shorter side is above 16 texels in its
// own 32-element-aligned region rounded to 4 KB, the smaller ones sharing one (packed); a cube map has
// six such regions (faces) per level.
void tex_memory( const TexKey &k, uint32_t nlevels, uint32_t &base, uint32_t &size, uint32_t &mips, uint32_t &mips_size )
{
    uint32_t format = k.d1 & 63, lb = tex_log_bpp( format ), bw = tex_bc( format ) ? 4 : 1;
    uint32_t w0 = ( k.d2 & 0x1FFF ) + 1, h0 = ( ( k.d2 >> 13 ) & 0x1FFF ) + 1;
    auto region = [&]( uint32_t l, uint32_t pitch ) {  // pitch: texels per row (0: the level's width)
        uint32_t pb = ( ( ( pitch ? pitch : std::max( w0 >> l, 1u ) ) + bw - 1 ) / bw + 31 ) & ~31u;
        uint32_t hb = ( ( std::max( h0 >> l, 1u ) + bw - 1 ) / bw + 31 ) & ~31u;
        return ( ( pb * hb << lb ) + 4095 ) & ~4095u;
    };
    // A cube map's faces, or a stacked 2D texture's layers (TexKey flag 0x20), one after another per level.
    uint32_t layers = ( ( k.d5 >> 9 ) & 3 ) == 3 ? 6 : ( k.d5 & 0x20u ) ? ( k.d2 >> 26 ) + 1 : 1;
    base = k.d1 & 0x1FFFF000;
    size = layers * region( 0, ( ( k.d0 >> 22 ) & 0x1FF ) * 32 );
    mips = k.d5 & 0x1FFFF000;
    mips_size = 0;
    if ( nlevels <= 1 || !mips ) return;
    uint32_t tail = 99;
    if ( ( k.d5 >> 11 ) & 1 )
        for ( tail = 0; tail < 14 && std::min( std::max( w0 >> tail, 1u ), std::max( h0 >> tail, 1u ) ) > 16; ++tail ) {}
    uint32_t stored = std::min( nlevels - 1, std::max( tail, 1u ) );
    for ( uint32_t l = 1; l <= stored; ++l ) mips_size += layers * region( l, 0 );
}

// The 12 samplers of a group (mag, min, mip filter) for a fetch constant's addressing, level range and
// border colour (dword 5 bits 0-1: 0 black with alpha 0, 1 white, 2 and 3 opaque black (the YCbCr
// blacks)), which clamp-to-border (mode 6) reads outside the texture: the game's shadow map is read so,
// white -- as far as can be, unshadowed -- around the area it covers.
std::unordered_map<uint32_t, uint32_t> g_sampler_groups;
uint32_t sampler_group( const uint32_t *f, uint32_t nlevels )
{
    uint32_t d0 = f[0], d4 = f[4], border = f[5] & 3;
    // Anisotropic filtering as the fetch constant asks (dword 3 bits 25-27: 0 off, 1-5 1:1 .. 16:1).
    static const bool no_aniso = RT_ENV( "CRACKDOWN_NO_ANISO" );
    uint32_t aniso = no_aniso || !g_aniso_supported ? 0 : ( f[3] >> 25 ) & 7;
    if ( aniso > 5 ) aniso = 0;
    // CRACKDOWN_FORCE_ANISO=1..5: at least that level on every texture with mip levels (testing).
    static const uint32_t force_aniso = RT_ENV( "CRACKDOWN_FORCE_ANISO" ) ? (uint32_t)atoi( RT_ENV( "CRACKDOWN_FORCE_ANISO" ) ) : 0;
    if ( force_aniso && g_aniso_supported && nlevels > 1 ) aniso = std::max( aniso, std::min( force_aniso, 5u ) );
    // The player's choice (settings anisotropy: 2, 4, 8 or 16 to 1; 0 the game's own) on every texture with
    // mip levels, taken on at once (read again when the settings change).
    static uint32_t chosen, chosen_version = ~0u;
    if ( settings_version() != chosen_version )
    {
        chosen_version = settings_version();
        const int a = settings().anisotropy;
        chosen = a >= 16 ? 5 : a >= 8 ? 4 : a >= 4 ? 3 : a >= 2 ? 2 : 0;
    }
    if ( chosen && g_aniso_supported && nlevels > 1 ) aniso = chosen;
    uint32_t addr = ( d0 >> 10 ) & 0x1FF;  // X, Y, Z: 3 bits each
    int32_t bias = (int32_t)( d4 << 10 ) >> 22;  // 1/32 levels
    uint32_t maxl = std::min( ( d4 >> 6 ) & 15, nlevels - 1 ), minl = std::min( ( d4 >> 2 ) & 15, maxl );
    uint32_t key = addr | ( (uint32_t)bias & 0x3FF ) << 9 | minl << 19 | maxl << 23 | border << 27 | aniso << 29;
    auto it = g_sampler_groups.find( key );
    if ( it != g_sampler_groups.end() ) return it->second;
    uint32_t group = (uint32_t)g_sampler_groups.size();
    if ( group >= MAX_SAMPLER_GROUPS ) return 0;
    static const float mip_bias = RT_ENV( "CRACKDOWN_MIP_BIAS" ) ? (float)atof( RT_ENV( "CRACKDOWN_MIP_BIAS" ) ) : 0.0f;
    auto mode = []( uint32_t m ) {
        return m == 0 ? VK_SAMPLER_ADDRESS_MODE_REPEAT : m == 1 ? VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT
             : ( m & 1 ) ? VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE  // mirror once (7, to the border: to the edge here)
             : m == 6 ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;  // 4: halfway, as the edge
    };
    VkSampler samplers[SAMPLERS_PER_GROUP];
    VkDescriptorImageInfo info[SAMPLERS_PER_GROUP];
    for ( uint32_t c = 0; c < SAMPLERS_PER_GROUP; ++c )
    {
        VkSamplerCreateInfo sci{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        sci.magFilter = c & 1 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        sci.minFilter = c & 2 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        sci.mipmapMode = ( c >> 2 ) == 1 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sci.addressModeU = mode( addr & 7 );
        sci.addressModeV = mode( ( addr >> 3 ) & 7 );
        sci.addressModeW = mode( ( addr >> 6 ) & 7 );
        sci.borderColor = border == 0 ? VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK : border == 1 ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE
                                                                                   : VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
        sci.anisotropyEnable = aniso >= 2 && ( c & 2 );  // with a linear minification filter
        sci.maxAnisotropy = (float)( 1u << ( aniso ? aniso - 1 : 0 ) );
        sci.mipLodBias = bias / 32.0f + mip_bias;
        sci.minLod = ( c >> 2 ) == 2 ? 0.0f : (float)minl;  // mip filter "base map": the top level only
        sci.maxLod = ( c >> 2 ) == 2 ? 0.0f : (float)maxl;
        if ( vkCreateSampler( g_dev, &sci, nullptr, &samplers[c] ) != VK_SUCCESS ) return 0;
        info[c] = { samplers[c], VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED };
    }
    VkWriteDescriptorSet w{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    w.dstSet = g_tex_set;
    w.dstBinding = 1;
    w.dstArrayElement = group * SAMPLERS_PER_GROUP;
    w.descriptorCount = SAMPLERS_PER_GROUP;
    w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    w.pImageInfo = info;
    vkUpdateDescriptorSets( g_dev, 1, &w, 0, nullptr );
    g_sampler_groups[key] = group;
    static uint32_t seen;
    if ( !( seen >> aniso & 1 ) ) seen |= 1u << aniso, rt_log( "renderer: textures with anisotropy level %u (%u:1)\n", aniso, aniso ? 1u << ( aniso - 1 ) : 0u );
    return group;
}

// Makes a texture's image from guest memory: compute passes write the decoded levels, or the DXT blocks
// (untiled, byte-swapped) to the staging buffer for copying in.
void upload( Texture &t, const uint32_t *fetch )
{
    end_rendering();
    barrier_all();  // after the GPU's own writes there (resolves)
    if ( t.fresh ) { first_use( t.image, false, false ); t.fresh = false; }
    struct { uint32_t fetch[6]; uint32_t level, w, h, stage_off, face; } pc;
    memcpy( pc.fetch, fetch, 24 );
    VkDescriptorBufferInfo mem{ t.hires ? g_hires_buf : g_mem_buf, 0, t.hires ? HIRES_SIZE : PHYS_SIZE }, stage{ g_stage_buf, 0, STAGE_SIZE };
    VkWriteDescriptorSet w[2]{ { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET }, { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET } };
    w[0].dstBinding = 0; w[0].descriptorCount = 1; w[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[0].pBufferInfo = &mem;
    vkCmdBindPipeline( g_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, t.bc ? g_upload_bc_pipe : g_upload_pipe );
    std::vector<VkBufferImageCopy> copies;
    uint32_t stage_off = 0;
    for ( uint32_t l = 0; l < t.nlevels; ++l )
        for ( uint32_t face = 0; face < t.layers; ++face )
    {
        uint32_t lw = std::max( t.w >> l, 1u ), lh = std::max( t.h >> l, 1u );
        pc.level = l, pc.face = face;
        if ( t.bc )
        {
            uint32_t bw = ( lw + 3 ) / 4, bh = ( lh + 3 ) / 4, bytes = bw * bh * ( t.format == VK_FORMAT_BC1_RGBA_UNORM_BLOCK ? 8 : 16 );
            if ( stage_off + bytes > STAGE_SIZE ) break;
            pc.w = bw, pc.h = bh, pc.stage_off = stage_off;
            w[1].dstBinding = 1; w[1].descriptorCount = 1; w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[1].pBufferInfo = &stage;
            g_push_descriptors( g_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_upload_pl, 0, 2, w );
            vkCmdPushConstants( g_cmd, g_upload_pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof( pc ), &pc );
            vkCmdDispatch( g_cmd, ( bw + 7 ) / 8, ( bh + 7 ) / 8, 1 );
            VkBufferImageCopy r{};
            r.bufferOffset = stage_off;
            r.bufferRowLength = bw * 4;
            r.bufferImageHeight = bh * 4;
            r.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, l, face, 1 };
            r.imageExtent = { lw, lh, 1 };
            copies.push_back( r );
            stage_off = ( stage_off + bytes + 255 ) & ~255u;
        }
        else
        {
            pc.w = lw, pc.h = lh, pc.stage_off = 0;
            VkDescriptorImageInfo img{ VK_NULL_HANDLE, t.levels[l * t.layers + face], VK_IMAGE_LAYOUT_GENERAL };
            w[1].dstBinding = 2; w[1].descriptorCount = 1; w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; w[1].pImageInfo = &img;
            g_push_descriptors( g_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_upload_pl, 0, 2, w );
            vkCmdPushConstants( g_cmd, g_upload_pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof( pc ), &pc );
            vkCmdDispatch( g_cmd, ( lw + 7 ) / 8, ( lh + 7 ) / 8, 1 );
        }
    }
    barrier_all();
    if ( !copies.empty() )
    {
        vkCmdCopyBufferToImage( g_cmd, g_stage_buf, t.image, VK_IMAGE_LAYOUT_GENERAL, (uint32_t)copies.size(), copies.data() );
        barrier_all();
    }
}

// The index of a fetch constant's texture and its sampler group (DrawConstants tex), made or made
// again first if its memory changed; cube: the shader samples it as a cube map (its index is then in the
// cube maps' array). A texture sampled as other than it is is decoded by the shader.
// The game's font drawn again k times bigger (fontatlas.cpp) as a texture in its picture's place: one
// texel of R, G, B, A (white letters with their coverage, the buttons' glyphs in their colours). Its slot, 0 if it can't be made.
} // namespace
bool player_in_vehicle();  // mouselook.cpp
namespace
{
uint32_t g_font_slot = 0;
uint32_t font_texture( const TexKey &key, uint32_t k )
{
    static bool failed;
    std::vector<uint8_t> px;
    if ( failed || g_tex_slots >= MAX_TEXTURES || !fontatlas_make( k, px ) ) { failed = true; return 0; }
    Texture t;
    t.font = true;
    t.w = 1024 * k, t.h = 2048 * k, t.nlevels = 1, t.format = VK_FORMAT_R8G8B8A8_UNORM, t.bc = false;
    VkImageCreateInfo ci{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = VK_FORMAT_R8G8B8A8_UNORM;
    ci.extent = { t.w, t.h, 1 };
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if ( vkCreateImage( g_dev, &ci, nullptr, &t.image ) != VK_SUCCESS ) { failed = true; return 0; }
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements( g_dev, t.image, &mr );
    VkMemoryAllocateInfo ai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = find_memory( mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT );
    if ( vkAllocateMemory( g_dev, &ai, nullptr, &t.mem ) != VK_SUCCESS ) { vkDestroyImage( g_dev, t.image, nullptr ); failed = true; return 0; }
    vkBindImageMemory( g_dev, t.image, t.mem, 0 );
    VkImageViewCreateInfo vi{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vi.image = t.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = VK_FORMAT_R8G8B8A8_UNORM;
    vi.components = { VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY };
    vi.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCreateImageView( g_dev, &vi, nullptr, &t.view );
    // In through a buffer the CPU can write, freed once the GPU has copied it.
    VkBufferCreateInfo bi{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bi.size = px.size();
    bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VkBuffer buf;
    vkCreateBuffer( g_dev, &bi, nullptr, &buf );
    VkMemoryRequirements br;
    vkGetBufferMemoryRequirements( g_dev, buf, &br );
    VkMemoryAllocateInfo bai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    bai.allocationSize = br.size;
    bai.memoryTypeIndex = find_memory( br.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT );
    VkDeviceMemory bmem;
    vkAllocateMemory( g_dev, &bai, nullptr, &bmem );
    vkBindBufferMemory( g_dev, buf, bmem, 0 );
    void *p;
    vkMapMemory( g_dev, bmem, 0, px.size(), 0, &p );
    memcpy( p, px.data(), px.size() );
    vkUnmapMemory( g_dev, bmem );
    end_rendering();
    barrier_all();
    first_use( t.image, false, false );
    t.fresh = false;
    VkBufferImageCopy r{};
    r.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    r.imageExtent = { t.w, t.h, 1 };
    vkCmdCopyBufferToImage( g_cmd, buf, t.image, VK_IMAGE_LAYOUT_GENERAL, 1, &r );
    barrier_all();
    renderer_finish();
    begin();
    vkDestroyBuffer( g_dev, buf, nullptr );
    vkFreeMemory( g_dev, bmem, nullptr );
    t.slot = g_tex_slots++;
    slot_size( false, t.slot, t.w, t.h );
    t.made = 1;
    VkDescriptorImageInfo ii{ VK_NULL_HANDLE, t.view, VK_IMAGE_LAYOUT_GENERAL };
    VkWriteDescriptorSet w{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    w.dstSet = g_tex_set;
    w.dstBinding = 0;
    w.dstArrayElement = t.slot;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    w.pImageInfo = &ii;
    vkUpdateDescriptorSets( g_dev, 1, &w, 0, nullptr );
    g_tex_stats.bytes += mr.size;
    g_font_slot = t.slot;
    const uint32_t slot = t.slot;
    g_textures.emplace( key, std::move( t ) );
    return slot;
}

// The controller's button pictures in the font as the player's input has them: the controller's own while a
// controller is in use; with the keyboard and mouse, a key with the name of what that button does there --
// the key of its command (keybinds.h) on foot or driving, the menus' own keys (Enter, Esc, Tab) in menus.
// Buttons without a key there keep the controller's picture.
std::string icon_label( uint32_t ch, int mode )
{
    using namespace keybinds;
    auto key = []( const char *id ) -> std::string {
        const int c = find( id );
        const int k = c >= 0 ? command( c ).keys[0] : 0;
        return k ? key_cap( k ) : "--";
    };
    auto cluster = [&]( const char *up, const char *left, const char *down, const char *right ) -> std::string {
        std::string a = key( up ), b = key( left ), c = key( down ), d = key( right );
        return a.size() == 1 && b.size() == 1 && c.size() == 1 && d.size() == 1 ? a + b + c + d : "MOVE";
    };
    if ( mode == 1 )  // menus
        switch ( ch )
        {
        case 0x17F: return "ENTER";
        case 0x180: return "ESC";
        case 0x198: return "TAB";
        default: return "";
        }
    const bool car = mode == 3;
    switch ( ch )
    {
    case 0x17F: return key( car ? "handbrake" : "jump" );            // A
    case 0x180: return key( car ? "horn" : "melee" );                // B
    case 0x181: return car ? "" : key( "switch_weapon" );           // X
    case 0x193: return key( car ? "exit_vehicle" : "use" );          // Y
    case 0x197: return "ESC";                                        // START
    case 0x198: return key( "map" );                                 // BACK
    case 0x199: return key( car ? "vehicle_fire" : "reload" );       // LB
    case 0x19A: return key( car ? "next_track" : "grenade" );        // RB
    case 0x19B: return key( car ? "brake" : "lock_on" );             // LT
    case 0x19C: return key( car ? "accelerate" : "fire" );           // RT
    default: break;
    }
    if ( ch >= 0x1A3 && ch <= 0x1A6 )  // the left stick moved
        return car ? cluster( "accelerate", "steer_left", "brake", "steer_right" ) : cluster( "move_forward", "strafe_left", "move_back", "strafe_right" );
    if ( ch >= 0x1A8 && ch <= 0x1AE ) return "MOUSE";  // the right stick moved: the camera
    return "";
}

void font_icons_update( Texture &t )
{
    static uint64_t shown = ~0ull;
    const int mode = video_using_controller() ? 0 : video_menu_control() || !video_in_game() ? 1 : player_in_vehicle() ? 3 : 2;
    const uint64_t want = (uint64_t)mode << 32 | ( mode ? keybinds::version() : 0 );
    if ( want == shown ) return;
    const std::vector<FontIcon> &icons = fontatlas_icons();
    if ( icons.empty() ) return;
    shown = want;
    std::vector<uint8_t> all;
    std::vector<VkBufferImageCopy> regions;
    for ( const FontIcon &i : icons )
    {
        const std::string label = mode ? icon_label( i.ch, mode ) : "";
        const std::vector<uint8_t> px = label.empty() ? i.rgba : fontatlas_keycap( i, label );
        VkBufferImageCopy r{};
        r.bufferOffset = all.size();
        r.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        r.imageOffset = { (int32_t)i.x, (int32_t)i.y, 0 };
        r.imageExtent = { i.w, i.h, 1 };
        regions.push_back( r );
        all.insert( all.end(), px.begin(), px.end() );
    }
    VkBufferCreateInfo bi{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bi.size = all.size();
    bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VkBuffer buf;
    if ( vkCreateBuffer( g_dev, &bi, nullptr, &buf ) != VK_SUCCESS ) return;
    VkMemoryRequirements br;
    vkGetBufferMemoryRequirements( g_dev, buf, &br );
    VkMemoryAllocateInfo bai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    bai.allocationSize = br.size;
    bai.memoryTypeIndex = find_memory( br.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT );
    VkDeviceMemory bmem;
    if ( vkAllocateMemory( g_dev, &bai, nullptr, &bmem ) != VK_SUCCESS ) { vkDestroyBuffer( g_dev, buf, nullptr ); return; }
    vkBindBufferMemory( g_dev, buf, bmem, 0 );
    void *p;
    vkMapMemory( g_dev, bmem, 0, all.size(), 0, &p );
    memcpy( p, all.data(), all.size() );
    vkUnmapMemory( g_dev, bmem );
    end_rendering();
    barrier_all();
    vkCmdCopyBufferToImage( g_cmd, buf, t.image, VK_IMAGE_LAYOUT_GENERAL, (uint32_t)regions.size(), regions.data() );
    barrier_all();
    renderer_finish();
    begin();
    vkDestroyBuffer( g_dev, buf, nullptr );
    vkFreeMemory( g_dev, bmem, nullptr );
    // CRACKDOWN_ICON_DUMP=dir (debug): the cells as drawn, side by side, to dir/icons-<mode>.ppm.
    if ( const char *dir = RT_ENV( "CRACKDOWN_ICON_DUMP" ) )
    {
        uint32_t W = 0, H = 0;
        for ( const FontIcon &i : icons ) W += i.w, H = std::max( H, i.h );
        std::vector<uint8_t> img( (size_t)W * H * 3, 64 );
        size_t off = 0;
        uint32_t x0 = 0;
        for ( const FontIcon &i : icons )
        {
            for ( uint32_t y = 0; y < i.h; ++y )
                for ( uint32_t x = 0; x < i.w; ++x )
                {
                    const uint8_t *s = &all[off + ( (size_t)y * i.w + x ) * 4];
                    for ( int c = 0; c < 3; ++c ) img[( (size_t)y * W + x0 + x ) * 3 + c] = (uint8_t)( ( s[c] * s[3] + 64 * ( 255 - s[3] ) ) / 255 );
                }
            off += (size_t)i.w * i.h * 4, x0 += i.w;
        }
        char path[512];
        snprintf( path, sizeof( path ), "%s/icons-%d.ppm", dir, mode );
        if ( FILE *f = fopen( path, "wb" ) ) fprintf( f, "P6\n%u %u\n255\n", W, H ), fwrite( img.data(), 1, img.size(), f ), fclose( f );
    }
    static const char *const names[] = { "controller", "menus", "on foot", "driving" };
    rt_log( "renderer: font's button pictures for %s (%zu)\n", names[mode], icons.size() );
}

uint32_t texture_index( const uint32_t *f, bool cube )
{
    if ( ( f[0] & 3 ) != 2 ) return 0;  // not a texture
    // A texture that is a full size resolve's image, as long as the game hasn't written the memory since.
    uint32_t hi_fetch[6];
    const HiRes *hi = nullptr;
    // CRACKDOWN_HIRES_TEXTURES=0 (debug): textures always from guest memory (the console's size).
    static const bool hires_textures = !RT_ENV( "CRACKDOWN_HIRES_TEXTURES" ) || atoi( RT_ENV( "CRACKDOWN_HIRES_TEXTURES" ) );
    if ( hires_textures && !g_hires.empty() && !cube && !( f[5] & 0xFFFFF000u ) )
        if ( auto h = g_hires.find( f[1] & 0x1FFFF000u ); h != g_hires.end() )
        {
            if ( memwatch_written( h->first, h->second.size, h->second.since ) ) g_hires.erase( h );
            else if ( h->second.w == ( f[2] & 0x1FFF ) + 1 && h->second.h == ( ( f[2] >> 13 ) & 0x1FFF ) + 1 )
            {
                hi = &h->second;
                memcpy( hi_fetch, f, 24 );
                hi_fetch[0] = ( f[0] & 0x003FFFFFu ) | ( hi->pitch / 32 ) << 22;  // linear
                hi_fetch[1] = ( f[1] & 0xFFFu ) | hi->offset;
                hi_fetch[2] = ( f[2] & 0xFC000000u ) | ( at8( hi->w, hi->k8 ) - 1 ) | ( at8( hi->h, hi->k8 ) - 1 ) << 13;
                f = hi_fetch;
            }
        }
    // Through the array binding: a cube map (six layers), or a 2D texture with layers (dword 2 bits 26-31: layers - 1)
    // a 3D fetch reads (stacked); a true volume is decoded by the shader.
    const uint32_t dim = ( f[5] >> 9 ) & 3, stack_layers = cube && dim == 1 ? ( f[2] >> 26 ) + 1 : 0;
    if ( cube && dim == 2 ) return 0xFFFFFFFFu;
    if ( ( dim == 3 ) != cube && !stack_layers ) return xlate::tex_decoded_fallback() ? 0xFFFFFFFFu : 0;  // the stand-in
    uint32_t format = f[1] & 63;
    // CRACKDOWN_DECODED_TEXTURES=<formats, hex mask by format number> or "all" (debug): those formats are
    // decoded from guest memory by the shader (tfetch2d), as before textures were images.
    static const uint64_t decoded = !RT_ENV( "CRACKDOWN_DECODED_TEXTURES" ) ? 0 : !strcmp( RT_ENV( "CRACKDOWN_DECODED_TEXTURES" ), "all" ) ? ~0ull
                                  : strtoull( RT_ENV( "CRACKDOWN_DECODED_TEXTURES" ), nullptr, 16 );
    if ( ( decoded >> format ) & 1 ) return 0xFFFFFFFFu;
    // CRACKDOWN_DECODED_ODD=1 (debug): every texture decoded by the shader in odd frames, so two frames
    // in a row compare the two ways.
    // CRACKDOWN_DECODED_AREA=<n> (debug, with CRACKDOWN_DECODED_TEXTURES): only textures of at least n
    // texels (negative: at most -n) are decoded by the shader.
    static const long area = RT_ENV( "CRACKDOWN_DECODED_AREA" ) ? atol( RT_ENV( "CRACKDOWN_DECODED_AREA" ) ) : 0;
    if ( ( decoded >> format ) & 1 )
    {
        long a = (long)( ( f[2] & 0x1FFF ) + 1 ) * ( ( ( f[2] >> 13 ) & 0x1FFF ) + 1 );
        if ( !area || ( area > 0 ? a >= area : a <= -area ) ) return 0xFFFFFFFFu;
    }
    // CRACKDOWN_DECODED_BASE=<hex address>,... (debug): textures at those addresses decoded by the shader.
    if ( static const char *db = RT_ENV( "CRACKDOWN_DECODED_BASE" ); db )
    {
        char hex[16];
        snprintf( hex, sizeof( hex ), "%08X", f[1] & 0xFFFFF000u );
        if ( strcasestr( db, hex ) ) return 0xFFFFFFFFu;
    }
    static const bool odd = RT_ENV( "CRACKDOWN_DECODED_ODD" );
    if ( odd && ( gpu_swap_count() & 1 ) ) return 0xFFFFFFFFu;
    VkFormat host = tex_host_format( format );
    if ( host == VK_FORMAT_UNDEFINED ) return stack_layers ? 0xFFFFFFFFu : 0;
    // The game's font picture (Crackdown_Font.dds: DXT3, 1024x2048) above 720p: drawn again k times bigger.
    static const uint32_t font_k = ( render_scale8() + 7 ) / 8;
    const bool font = !hi && !cube && font_k > 1 && format == 19 && ( f[2] & 0x1FFF ) + 1 == 1024 && ( ( f[2] >> 13 ) & 0x1FFF ) + 1 == 2048;
    TexKey key{ f[0] & 0xFFC00000u, f[1], f[2], ( f[5] & 0xFFFFFE00u ) | ( ( f[4] >> 6 ) & 15 ) | ( hi ? 0x100u : 0u ) | ( font ? 0x10u : 0u ) | ( stack_layers ? 0x20u : 0u ) };  // (flags in bits 4-8: 0-3 levels, 9+ the fetch's)
    auto it = g_textures.find( key );
    if ( it == g_textures.end() && font )
        if ( uint32_t slot = font_texture( key, font_k ) ) return slot | sampler_group( f, 1 ) << 16;
    if ( it == g_textures.end() )
    {
        if ( cube ? g_cube_slots >= MAX_CUBES : g_tex_slots >= MAX_TEXTURES ) return 0;
        Texture t;
        t.cube = cube;
        t.layers = stack_layers ? stack_layers : cube ? 6 : 1;
        t.w = ( f[2] & 0x1FFF ) + 1, t.h = ( ( f[2] >> 13 ) & 0x1FFF ) + 1;
        t.nlevels = ( f[5] & 0xFFFFF000u ) ? std::min( 32u - __builtin_clz( std::max( t.w, t.h ) ), ( ( f[4] >> 6 ) & 15 ) + 1 ) : 1;
        t.format = host;
        t.bc = tex_bc( format );
        VkImageCreateInfo ci{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        ci.imageType = VK_IMAGE_TYPE_2D;
        ci.format = host;
        ci.extent = { t.w, t.h, 1 };
        ci.mipLevels = t.nlevels;
        ci.arrayLayers = t.layers;
        ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | ( t.bc ? 0 : VK_IMAGE_USAGE_STORAGE_BIT );
        ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if ( vkCreateImage( g_dev, &ci, nullptr, &t.image ) != VK_SUCCESS ) return 0;
        VkMemoryRequirements mr;
        vkGetImageMemoryRequirements( g_dev, t.image, &mr );
        VkMemoryAllocateInfo ai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        ai.allocationSize = mr.size;
        ai.memoryTypeIndex = find_memory( mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT );
        if ( vkAllocateMemory( g_dev, &ai, nullptr, &t.mem ) != VK_SUCCESS ) { vkDestroyImage( g_dev, t.image, nullptr ); return 0; }
        vkBindImageMemory( g_dev, t.image, t.mem, 0 );
        VkImageViewCreateInfo vi{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vi.image = t.image;
        vi.viewType = cube ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
        vi.format = host;
        vi.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, t.nlevels, 0, t.layers };
        vi.components = tex_view_swizzle( host );
        vkCreateImageView( g_dev, &vi, nullptr, &t.view );
        vi.components = {};  // (storage views: as the image)
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        if ( !t.bc )
            for ( uint32_t l = 0; l < t.nlevels; ++l )
                for ( uint32_t face = 0; face < t.layers; ++face )
                {
                    vi.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, l, 1, face, 1 };
                    VkImageView v;
                    vkCreateImageView( g_dev, &vi, nullptr, &v );
                    t.levels.push_back( v );
                }
        tex_memory( key, t.nlevels, t.base, t.size, t.mips, t.mips_size );
        t.hires = hi;
        t.slot = cube ? g_cube_slots++ : g_tex_slots++;
        slot_size( cube, t.slot, t.w, t.h );
        VkDescriptorImageInfo ii{ VK_NULL_HANDLE, t.view, VK_IMAGE_LAYOUT_GENERAL };
        VkWriteDescriptorSet w{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        w.dstSet = g_tex_set;
        w.dstBinding = cube ? 2 : 0;
        w.dstArrayElement = t.slot;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        w.pImageInfo = &ii;
        vkUpdateDescriptorSets( g_dev, 1, &w, 0, nullptr );
        g_tex_stats.bytes += mr.size;
        // CRACKDOWN_CUBE_DUMP=1 (debug): the first cube maps' top-level faces decoded on the CPU from guest
        // memory as the faces are laid out (tex_level_face), side by side, to build/cube-<address>.ppm.
        static int cube_dumps;
        if ( cube && RT_ENV( "CRACKDOWN_CUBE_DUMP" ) && cube_dumps++ < 6 && ( format == 18 || format == 19 || format == 20 || format == 6 ) )
        {
            uint32_t e = ( f[1] >> 6 ) & 3, pitch = ( ( f[0] >> 22 ) & 0x1FF ) * 32;
            bool tiled = f[0] >> 31, block = format != 6;
            if ( !pitch ) pitch = ( t.w + 31 ) & ~31u;
            uint32_t bw = block ? 4 : 1, lb = format == 18 ? 3 : block ? 4 : 2;
            uint32_t slice = ( ( ( ( pitch / bw + 31 ) & ~31u ) * ( ( ( t.h + bw - 1 ) / bw + 31 ) & ~31u ) << lb ) + 4095 ) & ~4095u;
            auto ld = [&]( uint32_t a ) {
                uint32_t v;
                memcpy( &v, g_base + 0xA0000000 + ( a & 0x1FFFFFFF ), 4 );
                return e == 1 ? ( ( v & 0x00FF00FFu ) << 8 ) | ( ( v >> 8 ) & 0x00FF00FFu ) : e == 2 ? __builtin_bswap32( v ) : e == 3 ? ( v << 16 ) | ( v >> 16 ) : v;
            };
            std::vector<uint32_t> px( (size_t)t.w * 6 * t.h );
            for ( uint32_t face = 0; face < 6; ++face )
                for ( uint32_t y = 0; y < t.h; ++y )
                    for ( uint32_t x = 0; x < t.w; ++x )
                    {
                        uint32_t base = ( f[1] & 0xFFFFF000u ) + face * slice, c;
                        if ( block )
                        {
                            uint32_t bx = x / 4, by = y / 4, bp = std::max( pitch / 4, 1u );
                            uint32_t a = base + ( ( tiled ? tiled_offset( bx, by, bp, lb ) : by * bp + bx ) << lb ) + ( format == 18 ? 0 : 8 );
                            uint32_t w0 = ld( a ), w1 = ld( a + 4 ), sel = ( w1 >> ( 2 * ( ( y & 3 ) * 4 + ( x & 3 ) ) ) ) & 3;
                            auto c565 = []( uint32_t v ) { return ( ( v >> 11 ) & 31 ) * 255 / 31 | ( ( ( v >> 5 ) & 63 ) * 255 / 63 ) << 8 | ( ( v & 31 ) * 255 / 31 ) << 16; };
                            uint32_t c0 = c565( w0 & 0xFFFF ), c1 = c565( w0 >> 16 );
                            auto mix = [&]( int a, int b ) { uint32_t r = 0; for ( int i = 0; i < 3; ++i ) r |= ( ( ( ( c0 >> ( 8 * i ) ) & 255 ) * a + ( ( c1 >> ( 8 * i ) ) & 255 ) * b ) / ( a + b ) ) << ( 8 * i ); return r; };
                            c = sel == 0 ? c0 : sel == 1 ? c1 : sel == 2 ? mix( 2, 1 ) : mix( 1, 2 );
                        }
                        else
                        {
                            uint32_t v = ld( base + ( tiled ? tiled_offset( x, y, pitch, 2 ) : y * pitch + x ) * 4 );
                            c = ( v & 255 ) | ( ( v >> 8 ) & 255 ) << 8 | ( ( v >> 16 ) & 255 ) << 16;
                        }
                        px[(size_t)y * t.w * 6 + face * t.w + x] = c;
                    }
            char path[64];
            snprintf( path, sizeof( path ), "build/cube-%08X.ppm", f[1] & 0xFFFFF000u );
            write_ppm( path, px, t.w * 6, t.h );
            rt_log( "cubedump: %s %ux%u format %u, faces %u bytes apart\n", path, t.w, t.h, format, slice );
        }
        it = g_textures.emplace( key, std::move( t ) ).first;
    }
    Texture &t = it->second;
    if ( t.font )
    {
        font_icons_update( t );
        return t.slot | sampler_group( f, 1 ) << 16;
    }
    if ( hi )
    {
        if ( t.hires_gen != hi->gen ) t.hires_gen = hi->gen, upload( t, f );
        return t.slot | sampler_group( f, t.nlevels ) << 16;
    }
    uint32_t serial = memwatch_serial();
    // CRACKDOWN_TEX_ALWAYS=<texels> (debug): textures at least that big are made again at every use.
    static const long always = RT_ENV( "CRACKDOWN_TEX_ALWAYS" ) ? atol( RT_ENV( "CRACKDOWN_TEX_ALWAYS" ) ) : 0;
    if ( ( always && (long)t.w * t.h >= always ) || !t.made || ( t.checked != serial && ( memwatch_written( t.base, t.size, t.made ) || memwatch_written( t.mips, t.mips_size, t.made ) ) ) )
    {
        ( t.made ? g_tex_stats.remade : g_tex_stats.made )++;
        memwatch_protect( t.base, t.size );
        t.made = memwatch_protect( t.mips, t.mips_size );
        upload( t, f );
    }
    else if ( g_edram_trace && RT_ENV( "CRACKDOWN_TEX_STALE_CHECK" ) && t.w * t.h >= 100000 )
    {
        // Debug: in the traced frame, whether memory changed since the texture was made although it
        // counts as current (all GPU work done first, so resolves have landed).
        renderer_finish();
        begin();
        uint64_t h = XXH3_64bits( g_base + 0xA0000000u + t.base, t.size );
        if ( t.hash && h != t.hash ) rt_log( "texstale: %08X %ux%u fmt %u used stale (made at %u, serial %u)\n", t.base, t.w, t.h, format, t.made, serial );
    }
    // CRACKDOWN_TEX_READBACK=<hex base> (debug, 8_8_8_8 textures): in the traced frame, the image's top
    // level read back and compared with guest memory decoded on the CPU.
    if ( static const char *rb = RT_ENV( "CRACKDOWN_TEX_READBACK" ); rb && g_edram_trace && format == 6 && strtoul( rb, nullptr, 16 ) == ( f[1] & 0xFFFFF000u ) )
    {
        static int done;
        if ( done++ < 3 )
        {
            VkDeviceSize bytes = (VkDeviceSize)t.w * t.h * 4;
            VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
            bci.size = bytes;
            bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            VkBuffer buf;
            vkCreateBuffer( g_dev, &bci, nullptr, &buf );
            VkMemoryRequirements mr;
            vkGetBufferMemoryRequirements( g_dev, buf, &mr );
            VkMemoryAllocateInfo ai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
            ai.allocationSize = mr.size;
            ai.memoryTypeIndex = find_memory( mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT );
            VkDeviceMemory mem;
            vkAllocateMemory( g_dev, &ai, nullptr, &mem );
            vkBindBufferMemory( g_dev, buf, mem, 0 );
            end_rendering();
            barrier_all();
            VkBufferImageCopy r{};
            r.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            r.imageExtent = { t.w, t.h, 1 };
            vkCmdCopyImageToBuffer( g_cmd, t.image, VK_IMAGE_LAYOUT_GENERAL, buf, 1, &r );
            renderer_finish();
            begin();
            const uint32_t *px;
            vkMapMemory( g_dev, mem, 0, bytes, 0, (void **)&px );
            uint32_t pitch = ( ( f[0] >> 22 ) & 0x1FF ) * 32, bad = 0, e = ( f[1] >> 6 ) & 3;
            bool tiled = f[0] >> 31;
            for ( uint32_t y = 0; y < t.h; ++y )
                for ( uint32_t x = 0; x < t.w; ++x )
                {
                    uint32_t v;
                    memcpy( &v, g_base + 0xA0000000u + t.base + ( tiled ? tiled_offset( x, y, pitch, 2 ) : y * pitch + x ) * 4, 4 );
                    if ( e == 2 ) v = __builtin_bswap32( v );
                    else if ( e == 1 ) v = ( ( v & 0x00FF00FF ) << 8 ) | ( ( v >> 8 ) & 0x00FF00FF );
                    if ( v != px[y * t.w + x] && bad++ < 5 ) rt_log( "texreadback: %08X (%u,%u) image %08X memory %08X\n", t.base, x, y, px[y * t.w + x], v );
                }
            rt_log( "texreadback: %08X %ux%u: %u of %u texels differ (made at %u)\n", t.base, t.w, t.h, bad, t.w * t.h, t.made );
            vkUnmapMemory( g_dev, mem );
            vkDestroyBuffer( g_dev, buf, nullptr );
            vkFreeMemory( g_dev, mem, nullptr );
        }
    }
    if ( g_edram_trace && RT_ENV( "CRACKDOWN_TEX_STALE_CHECK" ) && t.w * t.h >= 100000 && t.checked != memwatch_serial() )
    {
        renderer_finish();
        begin();
        t.hash = XXH3_64bits( g_base + 0xA0000000u + t.base, t.size );  // as just made
    }
    t.checked = memwatch_serial();
    return t.slot | sampler_group( f, t.nlevels ) << 16;
}

// The texture fetch constants a shader samples as images (its 2D fetches; with cube, its cube map
// fetches), per shader.
uint32_t image_fetches( const RendererShader &s, bool cube = false )
{
    static std::unordered_map<uint64_t, uint32_t> masks[2];
    auto it = masks[cube].find( s.hash );
    if ( it != masks[cube].end() ) return it->second;
    uint32_t mask = 0;
    const ucode::Program &pr = *s.program;
    for ( const ucode::Cf &c : pr.cf )
        if ( c.is_exec() )
            for ( uint32_t k = 0; k < c.count(); ++k )
                if ( ( c.sequence() >> ( k * 2 ) ) & 1 )
                {
                    ucode::Fetch f{ { pr.slot_word( c.address() + k, 0 ), pr.slot_word( c.address() + k, 1 ), pr.slot_word( c.address() + k, 2 ) } };
                    // (The array binding: cube maps, and stacked 2D textures that 3D fetches read.)
                    if ( f.op() == 1 && ( cube ? f.tex_dimension() >= 2u : f.tex_dimension() == 1u ) ) mask |= 1u << f.const_index();
                }
    masks[cube].emplace( s.hash, mask );
    return mask;
}

std::string upload_source( bool bc )
{
    return std::string( "#version 450\n#extension GL_EXT_scalar_block_layout : require\n#extension GL_EXT_control_flow_attributes : require\n" ) +
           ( bc ? "#define BC 1\n" : "" ) + R"GLSL(
layout(local_size_x = 8, local_size_y = 8) in;
layout(std430, set = 0, binding = 0) buffer Mem { uint mem[]; };
#ifdef BC
layout(std430, set = 0, binding = 1) buffer Stage { uint stage[]; };
#else
layout(set = 0, binding = 2) uniform writeonly image2D dst;
#endif
// The texture's fetch constant, the level, its size (texels; blocks for DXT), where its blocks go and
// the face (cube maps).
layout(push_constant) uniform PC { uint fetch[6]; uint level, w, h, stage_off, face; } dc;
)GLSL" + xlate::kPreludeCommon + R"GLSL(
void main() {
  uvec2 p = gl_GlobalInvocationID.xy;
  if (p.x >= dc.w || p.y >= dc.h) return;
#ifdef BC
  uint base, pitch, w, h;
  uvec2 off;
  tex_level_face(0u, dc.level, dc.face, base, pitch, w, h, off);
  uint e = (dc.fetch[1] >> 6) & 3u, words = (dc.fetch[1] & 63u) == 18u ? 2u : 4u;
  uint addr = tex_elem_addr(0u, base, pitch, p.x * 4u + off.x, p.y * 4u + off.y);
  uint o = dc.stage_off / 4u + (p.y * dc.w + p.x) * words;
  for (uint i = 0u; i < words; ++i) stage[o + i] = ld(addr + i * 4u, e);
#else
  imageStore(dst, ivec2(p), texel_face(0u, int(p.x), int(p.y), dc.level, dc.face));
#endif
}
)GLSL";
}

// ------------------------------------------------------------------ pipelines
struct ShaderModules { VkShaderModule vs = VK_NULL_HANDLE, ps = VK_NULL_HANDLE; };
// Shader modules and pipelines are built on worker threads (see "pipeline workers"): a module is
// compiled once however many pipelines ask for it at the same time.
struct ModuleSlot { std::once_flag once; VkShaderModule m = VK_NULL_HANDLE; };
std::mutex g_module_lock;
std::unordered_map<uint64_t, std::unique_ptr<ModuleSlot>> g_modules;

// Whether a pixel shader exports depth (oDepth), cached per shader (command-processor thread).
bool writes_depth( const RendererShader &s )
{
    static std::unordered_map<uint64_t, bool> known;
    auto it = known.find( s.hash );
    if ( it != known.end() ) return it->second;
    bool w = s.program && xlate::translate( *s.program ).find( "XWRITES_DEPTH" ) != std::string::npos;
    known[s.hash] = w;
    return w;
}

// The vertex shaders in the game files, as devtools/shader_aot finds them (<cache>/vs_templates.bin: per
// program its dword count and dwords). A vertex shader D3D bound translates as its template there
// (ucode::binds_to), the module compiled ahead of time; one without a template as itself.
const ucode::Program &vs_template( const RendererShader &s )
{
    static std::once_flag loaded;
    static std::unordered_multimap<uint64_t, ucode::Program> templates;
    static std::mutex lock;
    static std::unordered_map<uint64_t, const ucode::Program *> chosen;
    std::call_once( loaded, [] {
        FILE *f = fopen( ( cache_dir() + "/vs_templates.bin" ).c_str(), "rb" );
        if ( !f ) return;
        for ( uint32_t n; fread( &n, 4, 1, f ) == 1 && n <= 0x4000; )
        {
            ucode::Program p;
            p.words.resize( n );
            if ( fread( p.words.data(), 4, n, f ) != n ) break;
            p.parse();
            uint64_t key = ucode::template_key( p );
            templates.emplace( key, std::move( p ) );
        }
        fclose( f );
        rt_log( "renderer: %zu vertex shader templates\n", templates.size() );
    } );
    std::lock_guard<std::mutex> g( lock );
    auto [it, fresh] = chosen.emplace( s.hash, s.program );
    if ( fresh )
    {
        auto [b, e] = templates.equal_range( ucode::template_key( *s.program ) );
        for ( ; b != e; ++b )
            if ( ucode::binds_to( b->second, *s.program ) ) { it->second = &b->second; break; }
        if ( it->second == s.program && RT_ENV( "CRACKDOWN_SHADER_LOG" ) ) rt_log( "renderer: vs_%016llx has no template\n", (unsigned long long)s.hash );
    }
    return *it->second;
}

VkShaderModule module_for( const RendererShader &s )
{
    ModuleSlot *slot;
    {
        std::lock_guard<std::mutex> g( g_module_lock );
        auto &p = g_modules[s.hash];
        if ( !p ) p = std::make_unique<ModuleSlot>();
        slot = p.get();
    }
    std::call_once( slot->once, [&] {
        // CRACKDOWN_SHOW_FETCH=<ps hash>:<fetch constant>: that pixel shader outputs that texture fetch.
        int debug_fetch = -1;
        if ( const char *sf = RT_ENV( "CRACKDOWN_SHOW_FETCH" ) )
            if ( const char *colon = strchr( sf, ':' ); colon && strtoull( sf, nullptr, 16 ) == s.hash ) debug_fetch = atoi( colon + 1 );
        bool pixel = s.program->type == 1;
        std::string src = xlate::module_source( pixel ? *s.program : vs_template( s ), debug_fetch );
        if ( src.empty() ) return;
        if ( const char *mb = RT_ENV( "CRACKDOWN_MIP_BIAS" ) )
            src.insert( src.find( '\n' ) + 1, "#define MIP_BIAS " + std::to_string( atof( mb ) ) + "\n" );
        if ( RT_ENV( "CRACKDOWN_PERF_NOTEX" ) ) src.insert( src.find( '\n' ) + 1, "#define PERF_NOTEX 1\n" );  // texture cost experiment
        // CRACKDOWN_PERF_NOTEX_PS=<hash>:<slot mask hex> (experiment): that pixel shader's fetches from those slots return 0.5.
        if ( const char *np = RT_ENV( "CRACKDOWN_PERF_NOTEX_PS" ); np && pixel && strtoull( np, nullptr, 16 ) == s.hash && strchr( np, ':' ) )
            src.insert( src.find( '\n' ) + 1, "#define PERF_NOTEX_MASK 0x" + std::string( strchr( np, ':' ) + 1 ) + "u\n" );
        char name[32];
        snprintf( name, sizeof(name), "%s_%016llx", pixel ? "ps" : "vs", (unsigned long long)s.hash );
        // CRACKDOWN_GLSL_DUMP=<hash>,...: those shaders' GLSL to build/<name>.glsl.
        if ( const char *gd = RT_ENV( "CRACKDOWN_GLSL_DUMP" ); gd && strstr( gd, name + 3 ) )
            if ( FILE *f = fopen( ( std::string( "build/" ) + name + ".glsl" ).c_str(), "w" ) ) { fputs( src.c_str(), f ); fclose( f ); }
        bool compiled = false;
        slot->m = compile( src, pixel ? shaderc_fragment_shader : shaderc_vertex_shader, name, &compiled );
        // With CRACKDOWN_SHADER_LOG, the microcode of a shader the ahead-of-time scan missed goes to
        // build/shaders/<name>.bin (as CRACKDOWN_DUMP_SHADERS), to find which game file holds it.
        if ( compiled && RT_ENV( "CRACKDOWN_SHADER_LOG" ) )
        {
            std::error_code ec;
            std::filesystem::create_directories( "build/shaders", ec );
            if ( FILE *f = fopen( ( std::string( "build/shaders/" ) + name + ".bin" ).c_str(), "wb" ) )
            {
                fwrite( s.program->words.data(), 4, s.program->words.size(), f );
                fclose( f );
            }
        }
    } );
    return slot->m;
}

VkBlendFactor blend_factor( uint32_t f )
{
    switch ( f )
    {
    case 0: return VK_BLEND_FACTOR_ZERO;
    case 1: return VK_BLEND_FACTOR_ONE;
    case 4: return VK_BLEND_FACTOR_SRC_COLOR;
    case 5: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case 6: return VK_BLEND_FACTOR_SRC_ALPHA;
    case 7: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 8: return VK_BLEND_FACTOR_DST_COLOR;
    case 9: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case 10: return VK_BLEND_FACTOR_DST_ALPHA;
    case 11: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case 12: return VK_BLEND_FACTOR_CONSTANT_COLOR;
    case 13: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case 14: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
    case 15: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    case 16: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    default: return VK_BLEND_FACTOR_ONE;
    }
}

VkBlendOp blend_op( uint32_t f )
{
    switch ( f )
    {
    case 1: return VK_BLEND_OP_SUBTRACT;
    case 2: return VK_BLEND_OP_MIN;
    case 3: return VK_BLEND_OP_MAX;
    case 4: return VK_BLEND_OP_REVERSE_SUBTRACT;
    default: return VK_BLEND_OP_ADD;
    }
}

VkPipeline create_pipeline( const RendererShader &vs, const RendererShader &ps, VkFormat cformat, bool depth, uint32_t blend, uint32_t mask,
                            uint32_t depthctl, VkPrimitiveTopology topo )
{
    VkPipeline p = VK_NULL_HANDLE;
    VkShaderModule mv = module_for( vs ), mp = ps.program ? module_for( ps ) : VK_NULL_HANDLE;
    if ( mv && ( mp || !ps.program ) )  // no pixel shader: depth only
    {
        VkPipelineShaderStageCreateInfo st[2]{ { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO }, { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO } };
        st[0].stage = VK_SHADER_STAGE_VERTEX_BIT; st[0].module = mv; st[0].pName = "main";
        // How D3D bound the vertex shader (translate.cpp makes it specialization constants of its template).
        auto spec = ucode::vs_specialization( vs_template( vs ), *vs.program );
        std::vector<VkSpecializationMapEntry> entries( spec.size() );
        std::vector<uint32_t> values( spec.size() );
        for ( size_t i = 0; i < spec.size(); ++i ) entries[i] = { spec[i].first, (uint32_t)( i * 4 ), 4 }, values[i] = spec[i].second;
        VkSpecializationInfo si{ (uint32_t)entries.size(), entries.data(), values.size() * 4, values.data() };
        if ( !spec.empty() ) st[0].pSpecializationInfo = &si;
        st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = mp; st[1].pName = "main";
        VkPipelineVertexInputStateCreateInfo vin{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
        VkPipelineInputAssemblyStateCreateInfo ia{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
        ia.topology = topo;
        ia.primitiveRestartEnable = topo == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;  // (only index_buffer's strips)
        VkPipelineViewportStateCreateInfo vp{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
        vp.viewportCount = vp.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rs{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode = VK_CULL_MODE_NONE;
        rs.lineWidth = 1.0f;
        rs.depthClampEnable = depth && ( depthctl >> 31 ) && g_depth_clamp;
        rs.frontFace = ( depthctl >> 3 ) & 1 ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;  // two-sided stencil
        VkPipelineMultisampleStateCreateInfo ms{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
        // MSAA (mask bits 12-15: samples; bit 8: the game's alpha-to-mask, as alpha to coverage).
        ms.rasterizationSamples = ( mask >> 12 & 15 ) > 1 ? (VkSampleCountFlagBits)( mask >> 12 & 15 ) : VK_SAMPLE_COUNT_1_BIT;
        ms.alphaToCoverageEnable = ( mask >> 12 & 15 ) > 1 && ( mask >> 8 & 1 );
        VkPipelineDepthStencilStateCreateInfo ds{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
        if ( depth )
        {
            ds.depthTestEnable = ( depthctl >> 1 ) & 1;
            ds.depthWriteEnable = ( depthctl >> 2 ) & 1;
            ds.depthCompareOp = VkCompareOp( ( depthctl >> 4 ) & 7 );
            // CRACKDOWN_NODEPTH_PS=<ps hash> (debug): that pixel shader's draws pass the depth test always.
            if ( static const char *nd = RT_ENV( "CRACKDOWN_NODEPTH_PS" ); nd && strtoull( nd, nullptr, 16 ) == ps.hash )
                ds.depthCompareOp = VK_COMPARE_OP_ALWAYS;
            // Stencil: RB_DEPTHCONTROL bit 0, then function / fail / depth-pass / depth-fail ops for front faces
            // (bits 8-19) and, with bit 7, back faces (bits 20-31). Xenos functions and ops are in Vulkan's order;
            // reference and masks are dynamic (RB_STENCILREFMASK / _BF).
            ds.stencilTestEnable = depthctl & 1;
            auto face = []( uint32_t f ) {
                VkStencilOpState o{};
                o.compareOp = VkCompareOp( f & 7 );
                o.failOp = VkStencilOp( ( f >> 3 ) & 7 );
                o.passOp = VkStencilOp( ( f >> 6 ) & 7 );
                o.depthFailOp = VkStencilOp( ( f >> 9 ) & 7 );
                return o;
            };
            ds.front = face( depthctl >> 8 );
            ds.back = ( depthctl >> 7 ) & 1 ? face( depthctl >> 20 ) : ds.front;
        }
        VkPipelineColorBlendAttachmentState ba{};
        ba.colorWriteMask = mask & 0xF;
        uint32_t cs = blend & 31, cf = ( blend >> 5 ) & 7, cd = ( blend >> 8 ) & 31, as = ( blend >> 16 ) & 31, af = ( blend >> 21 ) & 7, ad = ( blend >> 24 ) & 31;
        ba.blendEnable = !( cs == 1 && cd == 0 && cf == 0 && as == 1 && ad == 0 && af == 0 );
        ba.srcColorBlendFactor = blend_factor( cs ); ba.dstColorBlendFactor = blend_factor( cd ); ba.colorBlendOp = blend_op( cf );
        ba.srcAlphaBlendFactor = blend_factor( as ); ba.dstAlphaBlendFactor = blend_factor( ad ); ba.alphaBlendOp = blend_op( af );
        VkPipelineColorBlendStateCreateInfo cb{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
        cb.attachmentCount = cformat != VK_FORMAT_UNDEFINED ? 1 : 0;
        cb.pAttachments = &ba;
        VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_BLEND_CONSTANTS,
                                 VK_DYNAMIC_STATE_STENCIL_REFERENCE, VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
                                 VK_DYNAMIC_STATE_CULL_MODE, VK_DYNAMIC_STATE_FRONT_FACE };
        VkPipelineDynamicStateCreateInfo dy{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
        dy.dynamicStateCount = 8;
        dy.pDynamicStates = dyn;
        VkPipelineRenderingCreateInfo rinfo{ VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
        rinfo.colorAttachmentCount = cformat != VK_FORMAT_UNDEFINED ? 1 : 0;
        rinfo.pColorAttachmentFormats = &cformat;
        rinfo.depthAttachmentFormat = depth ? VK_FORMAT_D32_SFLOAT_S8_UINT : VK_FORMAT_UNDEFINED;
        rinfo.stencilAttachmentFormat = depth ? VK_FORMAT_D32_SFLOAT_S8_UINT : VK_FORMAT_UNDEFINED;
        VkGraphicsPipelineCreateInfo ci{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
        ci.pNext = &rinfo;
        ci.stageCount = mp ? 2 : 1;
        ci.pStages = st;
        ci.pVertexInputState = &vin;
        ci.pInputAssemblyState = &ia;
        ci.pViewportState = &vp;
        ci.pRasterizationState = &rs;
        ci.pMultisampleState = &ms;
        ci.pDepthStencilState = &ds;
        ci.pColorBlendState = &cb;
        ci.pDynamicState = &dy;
        ci.layout = g_draw_pl;
        if ( vkCreateGraphicsPipelines( g_dev, g_pcache, 1, &ci, nullptr, &p ) != VK_SUCCESS ) p = VK_NULL_HANDLE;
    }
    return p;
}

// ------------------------------------------------------------------ pipeline workers
// A new pipeline is compiled on a worker thread (the driver can take a long time over a translated
// shader); until it is ready its draws are skipped, so objects appear a moment late instead of the
// GPU -- and the game waiting on it -- freezing. CRACKDOWN_SYNC_SHADERS=1 compiles on the spot.
struct PipelineSlot { std::atomic<int> state{ 0 }; VkPipeline p = VK_NULL_HANDLE; };  // 0 pending, 1 ready, 2 failed
std::unordered_map<uint64_t, std::unique_ptr<PipelineSlot>> g_pipelines;  // command-processor thread only
std::mutex g_job_lock;
std::condition_variable g_job_cv;
std::deque<std::function<void()>> g_jobs;
std::atomic<uint32_t> g_jobs_pending, g_draws_skipped;
} // namespace
uint32_t renderer_pipelines_pending() { return g_jobs_pending.load(); }  // (video.cpp: shown with the FPS)
namespace
{

void pipeline_worker()
{
    setpriority( PRIO_PROCESS, (id_t)gettid(), 10 );  // behind the game's own threads
    for ( ;; )
    {
        std::function<void()> job;
        {
            std::unique_lock<std::mutex> lk( g_job_lock );
            g_job_cv.wait( lk, [] { return !g_jobs.empty(); } );
            job = std::move( g_jobs.front() );
            g_jobs.pop_front();
        }
        job();
        // The cache saved when the queue has run out, or once a minute while it is long.
        static std::mutex save_time_lock;
        static std::chrono::steady_clock::time_point last_save = std::chrono::steady_clock::now();
        if ( --g_jobs_pending == 0 || g_pcache_dirty )
        {
            bool save = false;
            {
                std::lock_guard<std::mutex> g( save_time_lock );
                const auto now = std::chrono::steady_clock::now();
                if ( g_pcache_dirty && ( g_jobs_pending == 0 || now - last_save > std::chrono::seconds( 60 ) ) ) save = true, last_save = now;
            }
            if ( save ) pipeline_cache_save();
        }
    }
}

// Pipeline warm-up. Every pipeline the game draws with is listed in <cache>/pipelines.list (its shaders'
// microcode, once each, and its fixed state); at start-up all of them are queued for the workers again,
// behind any the game asks for, so by the time play starts its draws find their pipelines ready (the
// driver's cache, pipelines.bin, makes each quick) instead of objects popping in. Records: tag 1 = program
// (u64 hash, u32 type, u32 dwords, the dwords), tag 2 = pipeline (PipelineRecord). CRACKDOWN_NO_WARMUP=1
// skips the warm-up (the list is still kept).
struct PipelineRecord { uint64_t vs, ps; uint32_t cformat, depth, blend, mask, depthctl, topo; };
FILE *g_pipeline_list;
std::unordered_set<uint64_t> g_listed_programs;
bool g_warming;

void list_program( const RendererShader &s )
{
    if ( !s.program || !g_listed_programs.insert( s.hash ).second ) return;
    uint32_t tag = 1, type = s.program->type, n = (uint32_t)s.program->words.size();
    fwrite( &tag, 4, 1, g_pipeline_list ); fwrite( &s.hash, 8, 1, g_pipeline_list );
    fwrite( &type, 4, 1, g_pipeline_list ); fwrite( &n, 4, 1, g_pipeline_list );
    fwrite( s.program->words.data(), 4, n, g_pipeline_list );
}

VkPipeline pipeline( const RendererShader &vs, const RendererShader &ps, VkFormat cformat, bool depth, uint32_t blend, uint32_t mask,
                     uint32_t depthctl, VkPrimitiveTopology topo );

void pipeline_warmup()
{
    const std::string path = cache_dir() + "/pipelines.list";
    static std::deque<ucode::Program> programs;  // the listed shaders (pipelines and modules keep pointers)
    std::unordered_map<uint64_t, const ucode::Program *> by_hash;
    std::vector<PipelineRecord> records;
    long good = 0;  // end of the last whole record (a crash can leave half of one)
    if ( FILE *f = fopen( path.c_str(), "rb" ) )
    {
        for ( uint32_t tag; fread( &tag, 4, 1, f ) == 1; good = ftell( f ) )
        {
            if ( tag == 1 )
            {
                uint64_t hash;
                uint32_t type, n;
                if ( fread( &hash, 8, 1, f ) != 1 || fread( &type, 4, 1, f ) != 1 || fread( &n, 4, 1, f ) != 1 || n > 0x4000 ) break;
                ucode::Program p;
                p.type = type;
                p.words.resize( n );
                if ( fread( p.words.data(), 4, n, f ) != n ) break;
                p.parse();
                programs.push_back( std::move( p ) );
                by_hash[hash] = &programs.back();
                g_listed_programs.insert( hash );
            }
            else if ( PipelineRecord r; tag == 2 && fread( &r, sizeof( r ), 1, f ) == 1 )
                records.push_back( r );
            else
                break;
        }
        fclose( f );
        if ( truncate( path.c_str(), good ) ) {}
    }
    g_pipeline_list = fopen( path.c_str(), "ab" );
    if ( RT_ENV( "CRACKDOWN_NO_WARMUP" ) ) return;
    g_warming = true;
    uint32_t queued = 0;
    for ( const PipelineRecord &r : records )
    {
        auto v = by_hash.find( r.vs ), p = by_hash.find( r.ps );
        if ( v == by_hash.end() || ( r.ps && p == by_hash.end() ) ) continue;
        pipeline( { r.vs, v->second }, r.ps ? RendererShader{ r.ps, p->second } : RendererShader{ 0, nullptr }, (VkFormat)r.cformat, r.depth, r.blend,
                  r.mask, r.depthctl, (VkPrimitiveTopology)r.topo );
        ++queued;
        // With MSAA on, the multisampled form of those listed without it too (the scene's: not told apart here).
        if ( g_msaa_samples > 1 && !( r.mask >> 12 ) )
        {
            pipeline( { r.vs, v->second }, r.ps ? RendererShader{ r.ps, p->second } : RendererShader{ 0, nullptr }, (VkFormat)r.cformat, r.depth, r.blend,
                      r.mask | g_msaa_samples << 12, r.depthctl, (VkPrimitiveTopology)r.topo );
            ++queued;
        }
    }
    g_warming = false;
    rt_log( "renderer: warming up %u pipelines (%zu shaders)\n", queued, programs.size() );
}

VkPipeline pipeline( const RendererShader &vs, const RendererShader &ps, VkFormat cformat, bool depth, uint32_t blend, uint32_t mask,
                     uint32_t depthctl, VkPrimitiveTopology topo )
{
    static bool warmed;  // (command-processor thread only; the warm-up calls back in here)
    if ( !warmed ) warmed = true, pipeline_warmup();
    uint64_t key = vs.hash * 31 + ps.hash;
    key = key * 131 + cformat;
    key = key * 131 + ( depth ? depthctl : 0 ) + 1;  // bit 31: depth clamp instead of near / far clipping; bit 3: clockwise front faces
    key = key * 131 + blend;
    key = key * 131 + mask;
    key = key * 131 + topo;
    auto &slot = g_pipelines[key];
    if ( !slot )
    {
        if ( !g_warming && g_pipeline_list )
        {
            list_program( vs );
            list_program( ps );
            PipelineRecord r{ vs.hash, ps.program ? ps.hash : 0, (uint32_t)cformat, depth, blend, mask, depthctl, (uint32_t)topo };
            uint32_t tag = 2;
            fwrite( &tag, 4, 1, g_pipeline_list );
            fwrite( &r, sizeof( r ), 1, g_pipeline_list );
            fflush( g_pipeline_list );
        }
        slot = std::make_unique<PipelineSlot>();
        PipelineSlot *ps_slot = slot.get();
        auto job = [=] {
            auto t0 = std::chrono::steady_clock::now();
            ps_slot->p = create_pipeline( vs, ps, cformat, depth, blend, mask, depthctl, topo );
            double ms = std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() - t0 ).count();
            if ( ms > 1000 )
                rt_log( "renderer: pipeline vs %016llx ps %016llx took %.1f s\n", (unsigned long long)vs.hash, (unsigned long long)ps.hash, ms / 1000 );
            if ( ms > 100 && ps_slot->p ) g_pcache_dirty = true;  // (saved by the workers: pipeline_worker)
            if ( RT_ENV( "CRACKDOWN_PIPELINE_LOG" ) )
                rt_log( "renderer: built pipeline vs %016llx ps %016llx in %.0f ms%s\n", (unsigned long long)vs.hash, (unsigned long long)ps.hash, ms,
                        ps_slot->p ? "" : " (failed)" );
            ps_slot->state.store( ps_slot->p ? 1 : 2, std::memory_order_release );
        };
        static const bool sync = RT_ENV( "CRACKDOWN_SYNC_SHADERS" );
        if ( sync ) job();
        else
        {
            static std::once_flag started;
            std::call_once( started, [] {
                unsigned n = std::max( 2u, std::thread::hardware_concurrency() / 2 );
                for ( unsigned i = 0; i < n; ++i ) std::thread( pipeline_worker ).detach();
            } );
            ++g_jobs_pending;
            if ( RT_ENV( "CRACKDOWN_PIPELINE_LOG" ) )
                rt_log( "renderer: queued pipeline vs %016llx ps %016llx\n", (unsigned long long)vs.hash, (unsigned long long)ps.hash );
            std::lock_guard<std::mutex> lk( g_job_lock );
            if ( g_warming ) g_jobs.push_back( job );
            else g_jobs.push_front( job );  // the game's own, ahead of the warm-up
            g_job_cv.notify_one();
        }
    }
    if ( slot->state.load( std::memory_order_acquire ) != 1 ) { ++g_draws_skipped; return VK_NULL_HANDLE; }
    return slot->p;
}

#include "edram_shaders.h"

bool init_vulkan()
{
    VkApplicationInfo app{ VK_STRUCTURE_TYPE_APPLICATION_INFO };
    app.pApplicationName = "crackdown-recomp";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici{ VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    ici.pApplicationInfo = &app;
    std::vector<const char *> iexts, dexts = { VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME };
    if ( dlss_wanted() ) dlss_extensions( iexts, dexts );  // what NGX needs (dlss.h)
    if ( fsr3_wanted() ) dexts.push_back( VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME );  // (the SDK calls its KHR name)
    ici.enabledExtensionCount = (uint32_t)iexts.size();
    ici.ppEnabledExtensionNames = iexts.data();
    VKCHECK( vkCreateInstance( &ici, nullptr, &g_instance ) );
    uint32_t n = 0;
    vkEnumeratePhysicalDevices( g_instance, &n, nullptr );
    std::vector<VkPhysicalDevice> devs( n );
    vkEnumeratePhysicalDevices( g_instance, &n, devs.data() );
    for ( auto d : devs )
    {
        VkPhysicalDeviceProperties pr;
        vkGetPhysicalDeviceProperties( d, &pr );
        if ( !g_phys || pr.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ) g_phys = d;
    }
    if ( !g_phys ) { rt_log( "vulkan: no device\n" ); return false; }
    // MSAA (settings msaa; not under DLSS, which smooths edges itself): as many samples as the device
    // allows of those asked for; depth and stencil copy into a multisampled image with stencil export.
    {
        uint32_t ne = 0;
        vkEnumerateDeviceExtensionProperties( g_phys, nullptr, &ne, nullptr );
        std::vector<VkExtensionProperties> ex( ne );
        vkEnumerateDeviceExtensionProperties( g_phys, nullptr, &ne, ex.data() );
        for ( auto &e : ex )
            if ( !strcmp( e.extensionName, VK_EXT_SHADER_STENCIL_EXPORT_EXTENSION_NAME ) ) g_stencil_export = true, dexts.push_back( VK_EXT_SHADER_STENCIL_EXPORT_EXTENSION_NAME );
        VkPhysicalDeviceProperties pr;
        vkGetPhysicalDeviceProperties( g_phys, &pr );
        const uint32_t can = pr.limits.framebufferColorSampleCounts & pr.limits.framebufferDepthSampleCounts & pr.limits.framebufferStencilSampleCounts;
        uint32_t want = (uint32_t)std::max( settings().msaa, 1 );
        if ( upscaler_wanted() ) want = 1;
        while ( want > 1 && !( can & want ) ) want >>= 1;
        g_msaa_samples = want;
        if ( settings().msaa > 1 ) rt_log( "renderer: MSAA %ux%s\n", g_msaa_samples, upscaler_wanted() ? " (off under DLSS / FSR 3)" : "" );
    }
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties( g_phys, &props );
    g_dc_align = std::max<VkDeviceSize>( props.limits.minStorageBufferOffsetAlignment, 16 );
    vkGetPhysicalDeviceMemoryProperties( g_phys, &g_memprops );
    vkGetPhysicalDeviceQueueFamilyProperties( g_phys, &n, nullptr );
    std::vector<VkQueueFamilyProperties> qf( n );
    vkGetPhysicalDeviceQueueFamilyProperties( g_phys, &n, qf.data() );
    for ( uint32_t i = 0; i < n; ++i )
        if ( ( qf[i].queueFlags & ( VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT ) ) == ( VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT ) ) { g_qfam = i; break; }
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{ VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    qci.queueFamilyIndex = g_qfam;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    VkPhysicalDeviceVulkan13Features f13{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
    f13.dynamicRendering = VK_TRUE;
    VkPhysicalDeviceVulkan12Features f12{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
    f12.scalarBlockLayout = VK_TRUE;
    f12.shaderBufferInt64Atomics = VK_TRUE;  // DLSS's motion (MotionHistory)
    // Textures: arrays of images and samplers, written while command buffers using other entries run.
    f12.descriptorIndexing = VK_TRUE;
    f12.runtimeDescriptorArray = VK_TRUE;
    f12.descriptorBindingPartiallyBound = VK_TRUE;
    f12.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
    f12.descriptorBindingUpdateUnusedWhilePending = VK_TRUE;
    f12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
    f12.samplerMirrorClampToEdge = VK_TRUE;
    f12.bufferDeviceAddress = dlss_wanted() ? VK_TRUE : VK_FALSE;  // (NGX's)
    f12.pNext = &f13;
    VkPhysicalDeviceFeatures2 f2{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
    f2.pNext = &f12;
    f2.features.vertexPipelineStoresAndAtomics = VK_TRUE;
    f2.features.shaderInt64 = VK_TRUE;
    f2.features.fragmentStoresAndAtomics = VK_TRUE;
    f2.features.shaderStorageImageWriteWithoutFormat = VK_TRUE;  // texture uploads
    VkPhysicalDeviceFeatures have;
    vkGetPhysicalDeviceFeatures( g_phys, &have );
    f2.features.occlusionQueryPrecise = have.occlusionQueryPrecise;
    f2.features.depthClamp = have.depthClamp;
    f2.features.samplerAnisotropy = have.samplerAnisotropy;
    f2.features.shaderClipDistance = have.shaderClipDistance;
    if ( fsr3_wanted() ) fsr3_features( f2.features, have );
    g_aniso_supported = have.samplerAnisotropy;
    g_depth_clamp = have.depthClamp;
    g_precise_queries = have.occlusionQueryPrecise;  // sample counts, not just any / none
    VkDeviceCreateInfo dci{ VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    dci.pNext = &f2;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = (uint32_t)dexts.size();
    dci.ppEnabledExtensionNames = dexts.data();
    VKCHECK( vkCreateDevice( g_phys, &dci, nullptr, &g_dev ) );
    if ( dlss_wanted() ) dlss_init( g_instance, g_phys, g_dev );
    if ( fsr3_wanted() && !dlss_ready() ) fsr3_init( g_phys, g_dev );  // (also in DLSS's place where it can't run)
    vkGetDeviceQueue( g_dev, g_qfam, 0, &g_queue );
    rt_log( "renderer: %s\n", props.deviceName );
    pipeline_cache_load();

    VkCommandPoolCreateInfo pci{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = g_qfam;
    VKCHECK( vkCreateCommandPool( g_dev, &pci, nullptr, &g_pool ) );
    VkCommandBufferAllocateInfo cai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cai.commandPool = g_pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    for ( Submission &sub : g_subs )
    {
        VKCHECK( vkAllocateCommandBuffers( g_dev, &cai, &sub.cmd ) );
        VkFenceCreateInfo fci{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        VKCHECK( vkCreateFence( g_dev, &fci, nullptr, &sub.fence ) );
        VkQueryPoolCreateInfo qpi{ VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
        qpi.queryType = VK_QUERY_TYPE_OCCLUSION;
        qpi.queryCount = QUERY_POOL_SIZE;
        VKCHECK( vkCreateQueryPool( g_dev, &qpi, nullptr, &sub.queries ) );
        if ( RT_ENV( "CRACKDOWN_GPU_TIMING" ) )
        {
            qpi.queryType = VK_QUERY_TYPE_TIMESTAMP;
            qpi.queryCount = TS_POOL_SIZE;
            VKCHECK( vkCreateQueryPool( g_dev, &qpi, nullptr, &sub.timestamps ) );
            VkPhysicalDeviceProperties pr;
            vkGetPhysicalDeviceProperties( g_phys, &pr );
            g_ts_period = pr.limits.timestampPeriod;
        }
    }

    // Guest physical memory as an imported host allocation.
    auto getHostProps = (PFN_vkGetMemoryHostPointerPropertiesEXT)vkGetDeviceProcAddr( g_dev, "vkGetMemoryHostPointerPropertiesEXT" );
    void *host = g_base + 0xA0000000;
    VkMemoryHostPointerPropertiesEXT hp{ VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT };
    VKCHECK( getHostProps( g_dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, host, &hp ) );
    VkExternalMemoryBufferCreateInfo ebi{ VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO };
    ebi.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
    VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bci.pNext = &ebi;
    bci.size = PHYS_SIZE;
    bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VKCHECK( vkCreateBuffer( g_dev, &bci, nullptr, &g_mem_buf ) );
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements( g_dev, g_mem_buf, &mr );
    VkImportMemoryHostPointerInfoEXT imp{ VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT };
    imp.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
    imp.pHostPointer = host;
    VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mai.pNext = &imp;
    mai.allocationSize = PHYS_SIZE;
    mai.memoryTypeIndex = find_memory( hp.memoryTypeBits & mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_COHERENT_BIT );
    VKCHECK( vkAllocateMemory( g_dev, &mai, nullptr, &g_mem_mem ) );
    VKCHECK( vkBindBufferMemory( g_dev, g_mem_buf, g_mem_mem, 0 ) );
    if ( hires_on() )
    {
        auto make = [&]( VkDeviceSize size, VkBufferUsageFlags use, VkMemoryPropertyFlags props, VkBuffer &b, VkDeviceMemory &m ) {
            VkBufferCreateInfo ci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
            ci.size = size, ci.usage = use;
            VKCHECK( vkCreateBuffer( g_dev, &ci, nullptr, &b ) );
            VkMemoryRequirements req;
            vkGetBufferMemoryRequirements( g_dev, b, &req );
            VkMemoryAllocateInfo ai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
            ai.allocationSize = req.size, ai.memoryTypeIndex = find_memory( req.memoryTypeBits, props );
            VKCHECK( vkAllocateMemory( g_dev, &ai, nullptr, &m ) );
            VKCHECK( vkBindBufferMemory( g_dev, b, m, 0 ) );
            return true;
        };
        make( HIRES_SIZE, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, g_hires_buf, g_hires_mem );
        make( HIRES_SHOW_SIZE * HIRES_SHOWS, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, g_hires_show_buf, g_hires_show_mem );
        VKCHECK( vkMapMemory( g_dev, g_hires_show_mem, 0, VK_WHOLE_SIZE, 0, (void **)&g_hires_show ) );
    }
    {
        // DLSS's motion (MotionHistory); with DLSS off, stand-ins (the draw shaders declare them).
        auto make = [&]( VkDeviceSize size, VkBuffer &b, VkDeviceMemory &m ) {
            VkBufferCreateInfo ci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
            ci.size = size, ci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            if ( vkCreateBuffer( g_dev, &ci, nullptr, &b ) != VK_SUCCESS ) return false;
            VkMemoryRequirements req;
            vkGetBufferMemoryRequirements( g_dev, b, &req );
            VkMemoryAllocateInfo ai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
            ai.allocationSize = req.size, ai.memoryTypeIndex = find_memory( req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT );
            return vkAllocateMemory( g_dev, &ai, nullptr, &m ) == VK_SUCCESS && vkBindBufferMemory( g_dev, b, m, 0 ) == VK_SUCCESS;
        };
        const bool want = upscaler_ready() && !RT_ENV( "CRACKDOWN_DLSS_CAMERA_ONLY" );  // debug: the camera's motion alone
        g_mv.on = want && make( 2ull * MotionHistory::SLOTS * 16, g_mv.pos, g_mv.pos_mem ) && make( MotionHistory::PIX * 16 + MotionHistory::PIX / 8, g_mv.pix, g_mv.pix_mem );
        if ( !g_mv.on && !( make( 256, g_mv.pos, g_mv.pos_mem ) && make( 256, g_mv.pix, g_mv.pix_mem ) ) ) { rt_log( "vulkan: motion buffers failed\n" ); return false; }
        if ( want ) rt_log( "renderer: DLSS's motion of moving things %s\n", g_mv.on ? "on" : "off (no memory)" );
    }

    // Draw-constant ring.
    bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bci.size = g_dc_size;
    bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT;  // (index_buffer)
    VKCHECK( vkCreateBuffer( g_dev, &bci, nullptr, &g_dc_buf ) );
    vkGetBufferMemoryRequirements( g_dev, g_dc_buf, &mr );
    mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = find_memory( mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT );
    VKCHECK( vkAllocateMemory( g_dev, &mai, nullptr, &g_dc_mem ) );
    VKCHECK( vkBindBufferMemory( g_dev, g_dc_buf, g_dc_mem, 0 ) );
    VKCHECK( vkMapMemory( g_dev, g_dc_mem, 0, g_dc_size, 0, (void **)&g_dc_ptr ) );

    // Descriptor layouts: draws (mem, dynamic dc), resolves (mem, sampled source).
    VkDescriptorSetLayoutBinding b[4]{};
    b[0].binding = 0; b[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; b[0].descriptorCount = 1;
    b[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    b[1].binding = 1; b[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC; b[1].descriptorCount = 1;
    b[1].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    b[2] = b[0], b[2].binding = 2;  // DLSS's motion (MotionHistory)
    b[3] = b[0], b[3].binding = 3;
    VkDescriptorSetLayoutCreateInfo lci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    lci.bindingCount = 4;
    lci.pBindings = b;
    VKCHECK( vkCreateDescriptorSetLayout( g_dev, &lci, nullptr, &g_draw_layout ) );
    // Textures (set 1): the images and the samplers draws index (renderer "Textures").
    VkDescriptorSetLayoutBinding tb[3]{};
    tb[0].binding = 0; tb[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE; tb[0].descriptorCount = MAX_TEXTURES;
    tb[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    tb[1] = tb[0]; tb[1].binding = 1; tb[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER; tb[1].descriptorCount = MAX_SAMPLER_GROUPS * SAMPLERS_PER_GROUP;
    tb[2] = tb[0]; tb[2].binding = 2; tb[2].descriptorCount = MAX_CUBES;
    VkDescriptorBindingFlags tbf[3];
    for ( auto &f : tbf ) f = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT;
    VkDescriptorSetLayoutBindingFlagsCreateInfo tbfi{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO };
    tbfi.bindingCount = 3;
    tbfi.pBindingFlags = tbf;
    VkDescriptorSetLayoutCreateInfo tlci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    tlci.pNext = &tbfi;
    tlci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
    tlci.bindingCount = 3;
    tlci.pBindings = tb;
    VKCHECK( vkCreateDescriptorSetLayout( g_dev, &tlci, nullptr, &g_tex_layout ) );
    VkDescriptorPoolSize tps[2] = { { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, MAX_TEXTURES + MAX_CUBES }, { VK_DESCRIPTOR_TYPE_SAMPLER, MAX_SAMPLER_GROUPS * SAMPLERS_PER_GROUP } };
    VkDescriptorPoolCreateInfo tpci{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    tpci.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
    tpci.maxSets = 1;
    tpci.poolSizeCount = 2;
    tpci.pPoolSizes = tps;
    VKCHECK( vkCreateDescriptorPool( g_dev, &tpci, nullptr, &g_tex_pool ) );
    VkDescriptorSetAllocateInfo tsai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    tsai.descriptorPool = g_tex_pool;
    tsai.descriptorSetCount = 1;
    tsai.pSetLayouts = &g_tex_layout;
    VKCHECK( vkAllocateDescriptorSets( g_dev, &tsai, &g_tex_set ) );
    VkDescriptorSetLayout draw_sets[2] = { g_draw_layout, g_tex_layout };
    VkPipelineLayoutCreateInfo plci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    plci.setLayoutCount = 2;
    plci.pSetLayouts = draw_sets;
    VKCHECK( vkCreatePipelineLayout( g_dev, &plci, nullptr, &g_draw_pl ) );
    plci.setLayoutCount = 1;

    // Surface reads (copies, resolves): guest memory, staging, the tile mask, the surface's views.
    VkDescriptorSetLayoutBinding sb[6]{};
    for ( uint32_t i = 0; i < 6; ++i )
    {
        sb[i].binding = i;
        sb[i].descriptorType = i == 2 ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC : i < 2 ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        sb[i].descriptorCount = 1;
        sb[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    lci.bindingCount = 6;
    lci.pBindings = sb;
    VKCHECK( vkCreateDescriptorSetLayout( g_dev, &lci, nullptr, &g_surface_layout ) );
    VkPushConstantRange pcr{ VK_SHADER_STAGE_COMPUTE_BIT, 0, 128 };
    plci.pSetLayouts = &g_surface_layout;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &pcr;
    VKCHECK( vkCreatePipelineLayout( g_dev, &plci, nullptr, &g_surface_pl ) );

    // The staging buffer surface copies go through.
    {
        VkBufferCreateInfo ci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        ci.size = STAGE_SIZE;
        ci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        VKCHECK( vkCreateBuffer( g_dev, &ci, nullptr, &g_stage_buf ) );
        VkMemoryRequirements r;
        vkGetBufferMemoryRequirements( g_dev, g_stage_buf, &r );
        VkMemoryAllocateInfo ai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        ai.allocationSize = r.size;
        ai.memoryTypeIndex = find_memory( r.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT );
        VKCHECK( vkAllocateMemory( g_dev, &ai, nullptr, &g_stage_mem ) );
        VKCHECK( vkBindBufferMemory( g_dev, g_stage_buf, g_stage_mem, 0 ) );
    }

    VkDescriptorPoolSize ps[3] = { { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4096 }, { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 2048 },
                                   { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 8192 } };
    VkDescriptorPoolCreateInfo dpci{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    dpci.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    dpci.maxSets = 4096;
    dpci.poolSizeCount = 3;
    dpci.pPoolSizes = ps;
    VKCHECK( vkCreateDescriptorPool( g_dev, &dpci, nullptr, &g_dpool ) );
    VkDescriptorSetAllocateInfo dsai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    dsai.descriptorPool = g_dpool;
    dsai.descriptorSetCount = 1;
    dsai.pSetLayouts = &g_draw_layout;
    VKCHECK( vkAllocateDescriptorSets( g_dev, &dsai, &g_draw_set ) );
    VkDescriptorBufferInfo dbi[4] = { { g_mem_buf, 0, PHYS_SIZE }, { g_dc_buf, 0, sizeof(DrawConstants) }, { g_mv.pos, 0, VK_WHOLE_SIZE }, { g_mv.pix, 0, VK_WHOLE_SIZE } };
    VkWriteDescriptorSet wr[4]{};
    for ( int i = 0; i < 4; ++i )
        wr[i] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET }, wr[i].dstSet = g_draw_set, wr[i].dstBinding = (uint32_t)i, wr[i].descriptorCount = 1,
        wr[i].descriptorType = i == 1 ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, wr[i].pBufferInfo = &dbi[i];
    vkUpdateDescriptorSets( g_dev, 4, wr, 0, nullptr );

    VkSamplerCreateInfo sci{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    sci.magFilter = sci.minFilter = VK_FILTER_NEAREST;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VKCHECK( vkCreateSampler( g_dev, &sci, nullptr, &g_point_sampler ) );

    // Stand-ins for the views a surface's reads leave unused.
    g_dummy_color.image = make_image( VK_FORMAT_R32_UINT, 1, 1, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, 0, g_dummy_color.mem );
    g_dummy_depth.image = make_image( VK_FORMAT_D32_SFLOAT_S8_UINT, 1, 1, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, 0, g_dummy_depth.mem );
    if ( !g_dummy_color.image || !g_dummy_depth.image ) { rt_log( "vulkan: surface images failed\n" ); return false; }
    g_dummy_color.view = make_view( g_dummy_color.image, VK_FORMAT_R32_UINT, VK_IMAGE_ASPECT_COLOR_BIT );
    g_dummy_depth.view = make_view( g_dummy_depth.image, VK_FORMAT_D32_SFLOAT_S8_UINT, VK_IMAGE_ASPECT_DEPTH_BIT );
    g_dummy_depth.stencil = make_view( g_dummy_depth.image, VK_FORMAT_D32_SFLOAT_S8_UINT, VK_IMAGE_ASPECT_STENCIL_BIT );

    // Texture uploads (push descriptors: guest memory, staging, the level's storage image) and the
    // stand-in texture, index 0.
    g_push_descriptors = (PFN_vkCmdPushDescriptorSetKHR)vkGetDeviceProcAddr( g_dev, "vkCmdPushDescriptorSetKHR" );
    VkDescriptorSetLayoutBinding ub[3]{};
    for ( uint32_t i = 0; i < 3; ++i )
    {
        ub[i].binding = i;
        ub[i].descriptorType = i < 2 ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        ub[i].descriptorCount = 1;
        ub[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo ulci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    ulci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    ulci.bindingCount = 3;
    ulci.pBindings = ub;
    VKCHECK( vkCreateDescriptorSetLayout( g_dev, &ulci, nullptr, &g_upload_layout ) );
    VkPushConstantRange upcr{ VK_SHADER_STAGE_COMPUTE_BIT, 0, 64 };
    VkPipelineLayoutCreateInfo uplci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    uplci.setLayoutCount = 1;
    uplci.pSetLayouts = &g_upload_layout;
    uplci.pushConstantRangeCount = 1;
    uplci.pPushConstantRanges = &upcr;
    VKCHECK( vkCreatePipelineLayout( g_dev, &uplci, nullptr, &g_upload_pl ) );
    g_tex_standin.image = make_image( VK_FORMAT_R8G8B8A8_UNORM, 1, 1, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, 0, g_tex_standin.mem );
    if ( !g_tex_standin.image ) return false;
    g_tex_standin.view = make_view( g_tex_standin.image, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT );
    {
        VkImageViewCreateInfo avi{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        avi.image = g_tex_standin.image;
        avi.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        avi.format = VK_FORMAT_R8G8B8A8_UNORM;
        avi.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        vkCreateImageView( g_dev, &avi, nullptr, &g_tex_standin.stencil );  // (its array view)
        VkDescriptorImageInfo ii[2] = { { VK_NULL_HANDLE, g_tex_standin.view, VK_IMAGE_LAYOUT_GENERAL }, { VK_NULL_HANDLE, g_tex_standin.stencil, VK_IMAGE_LAYOUT_GENERAL } };
        VkWriteDescriptorSet w[2]{ { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET }, { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET } };
        for ( int i = 0; i < 2; ++i )
        {
            w[i].dstSet = g_tex_set;
            w[i].dstBinding = i ? 2 : 0;
            w[i].descriptorCount = 1;
            w[i].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            w[i].pImageInfo = &ii[i];
        }
        vkUpdateDescriptorSets( g_dev, 2, w, 0, nullptr );
    }
    memwatch_init();

    g_shaderc = shaderc_compiler_initialize();
    auto make_compute = [&]( const char *src, const char *name, VkPipelineLayout layout, VkPipeline &pipe ) {
        VkShaderModule m = compile( src, shaderc_compute_shader, name );
        if ( !m ) return false;
        VkComputePipelineCreateInfo cpi{ VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
        cpi.stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
        cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        cpi.stage.module = m;
        cpi.stage.pName = "main";
        // EDRAM_TILE_MASK (edram_shaders.h): the (virtual) EDRAM size.
        static const uint32_t mask = EDRAM_TILES - 1;
        static const VkSpecializationMapEntry entry{ 0, 0, 4 };
        static const VkSpecializationInfo spec{ 1, &entry, 4, &mask };
        cpi.stage.pSpecializationInfo = &spec;
        cpi.layout = layout;
        return vkCreateComputePipelines( g_dev, VK_NULL_HANDLE, 1, &cpi, nullptr, &pipe ) == VK_SUCCESS;
    };
    std::string blocked = resolve_blocked_source();
    if ( fsr_wanted() || fxaa_wanted() || upscaler_wanted() )
    {
        VkSamplerCreateInfo psi{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        psi.magFilter = psi.minFilter = VK_FILTER_LINEAR;
        psi.addressModeU = psi.addressModeV = psi.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        VKCHECK( vkCreateSampler( g_dev, &psi, nullptr, &g_post_sampler ) );
        VkDescriptorSetLayoutBinding fb[4]{};
        fb[0].binding = 0, fb[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, fb[0].descriptorCount = 1, fb[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        fb[1].binding = 1, fb[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, fb[1].descriptorCount = 1, fb[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        fb[2].binding = 2, fb[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, fb[2].descriptorCount = 1, fb[2].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;  // (DLSS's motion)
        fb[3] = fb[1], fb[3].binding = 3;  // (DLSS's mask)
        VkDescriptorSetLayoutCreateInfo fl{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        fl.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR, fl.bindingCount = 4, fl.pBindings = fb;
        VKCHECK( vkCreateDescriptorSetLayout( g_dev, &fl, nullptr, &g_fsr_layout ) );
        VkPushConstantRange fr{ VK_SHADER_STAGE_COMPUTE_BIT, 0, 128 };
        VkPipelineLayoutCreateInfo fp{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        fp.setLayoutCount = 1, fp.pSetLayouts = &g_fsr_layout, fp.pushConstantRangeCount = 1, fp.pPushConstantRanges = &fr;
        VKCHECK( vkCreatePipelineLayout( g_dev, &fp, nullptr, &g_fsr_pl ) );
        if ( fxaa_wanted() && !make_compute( kFxaaShader, "fxaa", g_fsr_pl, g_fxaa_pipe ) ) rt_log( "renderer: FXAA didn't compile, it stays off\n" );
        if ( upscaler_wanted() && ( !make_compute( kDlssDepthShader, "dlss_depth", g_fsr_pl, g_df.depth_pipe ) || !make_compute( kDlssMotionShader, "dlss_motion", g_fsr_pl, g_df.motion_pipe ) ) )
            rt_log( "renderer: DLSS's depth / motion shaders didn't compile\n" ), g_df.depth_pipe = VK_NULL_HANDLE;
        if ( upscaler_wanted() && RT_ENV( "CRACKDOWN_DLSS_SHOW_MOTION" ) ) make_compute( kDlssShowShader, "dlss_show", g_fsr_pl, g_df.show_pipe );
        if ( fsr3_wanted() ) make_compute( kUpscaleAlphaShader, "fsr3_alpha", g_fsr_pl, g_df.alpha_pipe );
        std::string head = std::string( "#version 450\n#define A_GPU 1\n#define A_GLSL 1\n" ) + kFfxA +
                           "\nlayout(local_size_x = 16, local_size_y = 16) in;\nlayout(set = 0, binding = 0) uniform sampler2D src;\n"
                           "layout(set = 0, binding = 1, rgba8) uniform writeonly image2D dst;\nlayout(push_constant) uniform P { uvec4 c0, c1, c2, c3; } pc;\n";
        std::string easu = head + "#define FSR_EASU_F 1\nAF4 FsrEasuRF(AF2 p) { return textureGather(src, p, 0); }\nAF4 FsrEasuGF(AF2 p) { return textureGather(src, p, 1); }\n"
                                  "AF4 FsrEasuBF(AF2 p) { return textureGather(src, p, 2); }\n" + kFfxFsr1 +
                           "\nvoid main() { AF3 c; FsrEasuF(c, gl_GlobalInvocationID.xy, pc.c0, pc.c1, pc.c2, pc.c3); imageStore(dst, ivec2(gl_GlobalInvocationID.xy), vec4(c, 1.0)); }\n";
        std::string rcas = head + "#define FSR_RCAS_F 1\nAF4 FsrRcasLoadF(ASU2 p) { return texelFetch(src, ivec2(p), 0); }\nvoid FsrRcasInputF(inout AF1 r, inout AF1 g, inout AF1 b) {}\n" + kFfxFsr1 +
                           "\nvoid main() { AF1 r, g, b; FsrRcasF(r, g, b, gl_GlobalInvocationID.xy, pc.c0); imageStore(dst, ivec2(gl_GlobalInvocationID.xy), vec4(r, g, b, 1.0)); }\n";
        if ( ( fsr_wanted() || upscaler_wanted() ) && ( !make_compute( easu.c_str(), "fsr_easu", g_fsr_pl, g_fsr_easu_pipe ) || !make_compute( rcas.c_str(), "fsr_rcas", g_fsr_pl, g_fsr_rcas_pipe ) ) )
        {
            rt_log( "renderer: FSR 1 didn't compile, it stays off\n" );
            g_fsr_easu_pipe = VK_NULL_HANDLE;
        }
    }
    if ( !make_compute( kResolveShader, "resolve", g_surface_pl, g_resolve_pipe ) || !make_compute( blocked.c_str(), "resolve_blocked", g_surface_pl, g_resolve_blocked_pipe ) ||
         !make_compute( kCopyShader, "surface_copy", g_surface_pl, g_copy_pipe ) ||
         !make_compute( upload_source( false ).c_str(), "texture_upload", g_upload_pl, g_upload_pipe ) ||
         !make_compute( upload_source( true ).c_str(), "texture_upload_bc", g_upload_pl, g_upload_bc_pipe ) )
        return false;
    uint32_t standin = sampler_group( std::array<uint32_t, 6>{ 2, 0, 0, 0, 0, 0 }.data(), 1 );  // group 0
    (void)standin;
    return true;
}

float as_float( uint32_t v ) { float f; memcpy( &f, &v, 4 ); return f; }

// The screen-space rectangle of a 3-vertex rect list whose vertex shader's first vertex fetch reads
// float2+ positions (resolves, D3D's clear quads): the bounding box of the three corners.
bool rect_from_vertices( const uint32_t *regs, const ucode::Program *vs, int32_t &x0, int32_t &y0, int32_t &x1, int32_t &y1 )
{
    ucode::Fetch vf{};
    bool have_fetch = false;
    for ( size_t i = 0; vs && i < vs->cf.size() && !have_fetch; ++i )
    {
        const ucode::Cf &c = vs->cf[i];
        if ( !c.is_exec() ) continue;
        for ( uint32_t k = 0; k < c.count() && !have_fetch; ++k )
            if ( ( c.sequence() >> ( k * 2 ) ) & 1 )
            {
                uint32_t slot = c.address() + k;
                vf = ucode::Fetch{ { vs->slot_word( slot, 0 ), vs->slot_word( slot, 1 ), vs->slot_word( slot, 2 ) } };
                have_fetch = vf.op() == 0;
            }
    }
    if ( !have_fetch ) return false;
    uint32_t fmt = vf.vtx_data_format();
    if ( fmt != 37 && fmt != 38 && fmt != 57 ) return false;
    uint32_t idx = vf.const_index() * 3 + vf.vtx_const_sel();
    uint32_t f0 = regs[reg::SHADER_CONSTANT_FETCH_00_0 + idx * 2], f1 = regs[reg::SHADER_CONSTANT_FETCH_00_0 + idx * 2 + 1];
    uint32_t vb = f0 & 0x1FFFFFFC, e = f1 & 3, stride = vf.vtx_stride() * 4;
    int32_t off = vf.vtx_offset() * 4;
    if ( !vb ) return false;
    float xs[3], ys[3];
    for ( int i = 0; i < 3; ++i )
    {
        uint32_t a = vb + i * stride + off;
        uint32_t vx = *(uint32_t *)( g_base + 0xA0000000 + a ), vy = *(uint32_t *)( g_base + 0xA0000000 + a + 4 );
        if ( e == 2 ) { vx = __builtin_bswap32( vx ); vy = __builtin_bswap32( vy ); }
        memcpy( &xs[i], &vx, 4 ); memcpy( &ys[i], &vy, 4 );
        if ( !std::isfinite( xs[i] ) || !std::isfinite( ys[i] ) || std::fabs( xs[i] ) > 32768 || std::fabs( ys[i] ) > 32768 ) return false;
    }
    x0 = (int32_t)std::floor( std::min( { xs[0], xs[1], xs[2] } ) + 0.5f );
    y0 = (int32_t)std::floor( std::min( { ys[0], ys[1], ys[2] } ) + 0.5f );
    x1 = (int32_t)std::floor( std::max( { xs[0], xs[1], xs[2] } ) + 0.5f );
    y1 = (int32_t)std::floor( std::max( { ys[0], ys[1], ys[2] } ) + 0.5f );
    return x1 > x0 && y1 > y0;
}

} // namespace

bool renderer_init()
{
    static int state = 0;  // 0 untried, 1 ok, 2 failed
    if ( state == 0 )
    {
        state = RT_ENV( "CRACKDOWN_NO_RENDERER" ) ? 2 : init_vulkan() ? 1 : 2;
        if ( state == 2 ) rt_log( "renderer: disabled\n" );
        g_ok = state == 1;
    }
    return g_ok;
}

// CRACKDOWN_TEX_WATCH=1 (debug): each texture the draws read, hashed at its first use in a frame; the
// ones whose memory changed since their last use are logged with whether a resolve ever wrote there.
std::unordered_map<uint32_t, uint32_t> g_resolved;  // resolve destination base -> frames it was resolved in
void tex_watch( const uint32_t *regs, const RendererShader &vs, const RendererShader &ps )
{
    static std::unordered_map<uint64_t, uint32_t> slots;
    struct Seen { uint64_t hash; uint64_t frame; uint32_t changes, uses; };
    static std::unordered_map<uint64_t, Seen> seen;
    static uint64_t last_report;
    uint64_t frame = gpu_swap_count();
    for ( const RendererShader *sh : { &vs, &ps } )
    {
        auto it = slots.find( sh->hash );
        if ( it == slots.end() )
        {
            uint32_t mask = 0;
            const ucode::Program &pr = *sh->program;
            for ( const ucode::Cf &c : pr.cf )
                if ( c.is_exec() )
                    for ( uint32_t k = 0; k < c.count(); ++k )
                        if ( ( c.sequence() >> ( k * 2 ) ) & 1 )
                        {
                            ucode::Fetch f{ { pr.slot_word( c.address() + k, 0 ), pr.slot_word( c.address() + k, 1 ), pr.slot_word( c.address() + k, 2 ) } };
                            if ( f.op() == 1 ) mask |= 1u << f.const_index();
                        }
            it = slots.emplace( sh->hash, mask ).first;
        }
        for ( uint32_t k = 0; k < 32; ++k )
        {
            if ( !( ( it->second >> k ) & 1 ) ) continue;
            const uint32_t *f = regs + reg::SHADER_CONSTANT_FETCH_00_0 + k * 6;
            if ( ( f[0] & 3 ) != 2 ) continue;
            uint32_t format = f[1] & 63, base = f[1] & 0x1FFFF000u, w = ( f[2] & 0x1FFF ) + 1, h = ( ( f[2] >> 13 ) & 0x1FFF ) + 1;
            bool block = format == 18 || format == 19 || format == 20;
            uint32_t lb = block ? ( format == 18 ? 3 : 4 ) : ( format == 2 ? 0 : ( format == 3 || format == 4 || format == 10 || format == 15 || format == 24 || format == 30 ) ? 1 :
                                                              ( format == 26 || format == 32 || format == 37 ) ? 3 : format == 38 ? 4 : 2 );
            uint32_t bw = ( ( block ? ( w + 3 ) / 4 : w ) + 31 ) & ~31u, bh = ( ( block ? ( h + 3 ) / 4 : h ) + 31 ) & ~31u;
            size_t bytes = std::min<size_t>( (size_t)bw * bh << lb, 64u << 20 );
            if ( !base || base + bytes > 0x20000000u ) continue;
            uint64_t key = (uint64_t)base << 32 | f[2];
            Seen &s = seen[key];
            if ( s.frame == frame + 1 ) continue;  // hashed this frame
            uint64_t hsh = XXH3_64bits( g_base + 0xA0000000u + base, bytes );
            ++s.uses;
            if ( s.frame && hsh != s.hash )
            {
                ++s.changes;
                auto r = g_resolved.find( base );
                if ( s.changes <= 3 || ( s.changes & ( s.changes - 1 ) ) == 0 )
                    rt_log( "texwatch: %08X %ux%u fmt %u changed (%u of %u uses; last use frame %llu, now %llu)%s\n", base, w, h, format, s.changes, s.uses,
                            (unsigned long long)s.frame - 1, (unsigned long long)frame, r != g_resolved.end() ? " resolve target" : "" );
            }
            s.hash = hsh;
            s.frame = frame + 1;
        }
    }
    if ( frame >= last_report + 600 )
    {
        last_report = frame;
        uint32_t textures = 0, changing = 0, rt = 0;
        for ( auto &[k, s] : seen )
        {
            ++textures;
            if ( s.changes ) ( g_resolved.count( (uint32_t)( k >> 32 ) ) ? rt : changing )++;
        }
        rt_log( "texwatch: frame %llu: %u textures seen, %u changed (not resolve targets), %u resolve targets changed\n", (unsigned long long)frame, textures,
                changing, rt );
    }
}

void renderer_draw( const uint32_t *regs, uint32_t initiator, uint32_t index_base, uint32_t index_info, RendererShader vs, RendererShader ps )
{
    if ( !renderer_init() || !vs.program || !ps.program ) return;
    uint32_t mode = regs[reg::RB_MODECONTROL] & 7;
    if ( static const char *lp = RT_ENV( "CRACKDOWN_LOG_PS" ); lp && strtoull( lp, nullptr, 16 ) == ps.hash )
    {
        static int n;
        if ( n++ < 40 )
            rt_log( "psdraw: entry mode %u prim %u count %u scissor %08X %08X vp %g %g %g %g vte %X surface %08X\n", mode, initiator & 0x3F, initiator >> 16,
                    regs[reg::PA_SC_WINDOW_SCISSOR_TL], regs[reg::PA_SC_WINDOW_SCISSOR_BR], as_float( regs[reg::PA_CL_VPORT_XSCALE] ),
                    as_float( regs[reg::PA_CL_VPORT_XSCALE + 1] ), as_float( regs[reg::PA_CL_VPORT_XSCALE + 2] ), as_float( regs[reg::PA_CL_VPORT_XSCALE + 3] ),
                    regs[reg::PA_CL_VTE_CNTL], regs[reg::RB_SURFACE_INFO] );
        if ( n <= 40 )
        {
            std::string v;
            char b[16];
            for ( uint32_t r = 0x2280; r < 0x22A0; ++r ) { snprintf( b, sizeof( b ), " %08X", regs[r] ); v += b; }
            rt_log( "psdraw:   initiator %08X index base %08X info %08X vtx_cntl %08X  regs 2280..229F:%s\n", initiator, index_base, index_info, regs[0x2302], v.c_str() );
        }
    }
    if ( mode != 4 && mode != 5 ) return;  // 4 = colour + depth, 5 = depth only (6 = copy is a resolve)
    uint32_t prim = initiator & 0x3F, count = initiator >> 16;
    if ( g_draw_census )
        rt_log( "census: #%u mode %u prim %u count %u vs %016llx ps %016llx surface %08X depthinfo %08X color %08X blend %08X depth %08X vf95 %08X %08X index %08X modecntl %08X polyoffset %g %g %g %g\n", g_draw_census++ - 1, mode, prim, count,
                (unsigned long long)vs.hash, (unsigned long long)ps.hash, regs[reg::RB_SURFACE_INFO], regs[reg::RB_DEPTH_INFO], regs[reg::RB_COLOR_INFO], regs[reg::RB_BLENDCONTROL0], regs[reg::RB_DEPTHCONTROL],
                regs[reg::SHADER_CONSTANT_FETCH_00_0 + 190], regs[reg::SHADER_CONSTANT_FETCH_00_0 + 191], index_base, regs[reg::PA_SU_SC_MODE_CNTL],
                as_float( regs[0x2380] ), as_float( regs[0x2381] ), as_float( regs[0x2382] ), as_float( regs[0x2383] ) );
    uint32_t vertices;
    VkPrimitiveTopology topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    switch ( prim )
    {
    case 8: vertices = count / 3 * 6; break;
    case 13: vertices = count / 4 * 6; break;
    case 5: case 6: vertices = count >= 3 ? ( count - 2 ) * 3 : 0; break;
    case 4: vertices = count; break;
    case 1: vertices = count; topo = VK_PRIMITIVE_TOPOLOGY_POINT_LIST; break;
    case 2: vertices = count; topo = VK_PRIMITIVE_TOPOLOGY_LINE_LIST; break;
    case 3: vertices = count; topo = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP; break;
    default: return;
    }
    if ( !vertices ) return;
    // Triangle lists and strips with a reset index drawn from a Vulkan index buffer (index_buffer): the
    // GPU reuses shaded vertices, and a strip is one draw however many times it restarts.
    const bool restart = prim == 6 && ( ( regs[reg::PA_SU_SC_MODE_CNTL] >> 21 ) & 1 );
    static const bool no_index_buffer = RT_ENV( "CRACKDOWN_NO_INDEX_BUFFER" );  // debug: indices fetched by the vertex shader
    const bool indexed = !no_index_buffer && ( ( initiator >> 6 ) & 3 ) == 0 && ( prim == 4 || restart );
    if ( indexed && restart ) topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    if ( topo == VK_PRIMITIVE_TOPOLOGY_POINT_LIST && RT_ENV( "CRACKDOWN_SKIP_POINTS" ) ) return;
    static const char *skip_vs = RT_ENV( "CRACKDOWN_SKIP_VS" );  // debug: hex VS hash to leave out
    if ( skip_vs && strtoull( skip_vs, nullptr, 16 ) == vs.hash ) return;
    if ( ps.hash == 0xb082b1fa6526b8b6ull && splash_movie_hidden() ) return;  // a skipped start-up movie's undecodable last frame

    uint32_t surface = regs[reg::RB_SURFACE_INFO], cinfo = regs[reg::RB_COLOR_INFO], dinfo = regs[reg::RB_DEPTH_INFO];
    uint32_t pitch = surface & 0x3FFF, msaa = ( surface >> 16 ) & 3;
    if ( !pitch ) return;
    begin();
    uint32_t color_mask = mode == 5 ? 0 : regs[reg::RB_COLOR_MASK] & 0xF;  // EDRAM mode 5 renders depth only
    uint32_t depthctl = regs[reg::RB_DEPTHCONTROL];
    bool use_depth = ( depthctl & 7 ) != 0 && !RT_ENV( "CRACKDOWN_NO_DEPTH" );
    // Views that nothing in this draw writes or tests stay unbound (and unloaded).
    Target *ct = color_mask ? target( edram_base( cinfo ), ( cinfo >> 16 ) & 0xF, pitch, msaa, false ) : nullptr;
    Target *dt = use_depth ? target( edram_base( dinfo ), 0, pitch, msaa, true ) : nullptr;
    Target *rt = ct ? ct : dt;
    if ( !rt ) return;
    // DLSS (DlssFrame): the post-processing's first draw (a screen-sized single-sampled colour surface)
    // after the scene was resolved; the camera from the world's shader.
    if ( upscaler_ready() && !g_df.done && g_df.snapped && ct && ct->msaa == 0 && ct->pitch >= 1280 && ct->pitch <= 1312 ) dlss_run();
    const bool dlss_scene_part = upscaler_ready() && !g_df.done && rt->pitch >= 1280 && rt->pitch <= 1312 && rt->msaa >= 1;
    if ( dlss_scene_part && g_mv.on && !g_mv.cleared )  // a new frame's motion (MotionHistory)
    {
        end_rendering();
        vkCmdFillBuffer( g_cmd, g_mv.pix, 0, VK_WHOLE_SIZE, 0 );
        barrier_all();
        g_mv.cleared = true;
    }
    if ( dlss_scene_part )
    {
        if ( vs.hash == 0x4fdaae92cad15814ull && !g_df.have_vp && ct && dt )
        {
            // c0-c3 are its view-projection when c3 (the depth row) is the view's c6: no object transform.
            const float *k = (const float *)( regs + reg::SHADER_CONSTANT_000_X );
            bool plain = true;
            for ( int i = 0; i < 4; ++i ) plain = plain && std::fabs( k[12 + i] - k[24 + i] ) <= 1e-3f * std::max( 1.0f, std::fabs( k[24 + i] ) );
            if ( plain )
            {
                for ( int i = 0; i < 16; ++i ) g_df.vp[i] = k[i];
                for ( int i = 0; i < 2; ++i ) g_df.vps[i] = as_float( regs[reg::PA_CL_VPORT_XSCALE + i * 2] ), g_df.vpo[i] = as_float( regs[reg::PA_CL_VPORT_XSCALE + i * 2 + 1] );
                g_df.have_vp = true, g_df.scene_color = ct, g_df.scene_depth = dt;
                g_mv.cam = g_df.prev_ok && dlss_camera_move( g_mv.m );
            }
        }
    }

    // Window scissor (screen space), moved by the window offset unless TL bit 31 disables that.
    uint32_t wo = regs[reg::PA_SC_WINDOW_OFFSET];
    int32_t wox = ( (int32_t)( wo << 17 ) ) >> 17, woy = ( (int32_t)( wo << 1 ) ) >> 17;
    bool apply_wo = ( regs[reg::PA_SU_SC_MODE_CNTL] >> 16 ) & 1;
    uint32_t tl = regs[reg::PA_SC_WINDOW_SCISSOR_TL], br = regs[reg::PA_SC_WINDOW_SCISSOR_BR];
    int32_t sx0 = tl & 0x7FFF, sy0 = ( tl >> 16 ) & 0x7FFF, sx1 = br & 0x7FFF, sy1 = ( br >> 16 ) & 0x7FFF;
    if ( !( tl >> 31 ) ) sx0 += wox, sx1 += wox, sy0 += woy, sy1 += woy;
    // Primitives are clipped to the viewport, so with a full viewport transform it bounds the draw too.
    uint32_t vte = regs[reg::PA_CL_VTE_CNTL];
    if ( ( vte & 0xF ) == 0xF && !( ( regs[reg::PA_CL_CLIP_CNTL] >> 16 ) & 1 ) )
    {
        float xs = std::fabs( as_float( regs[reg::PA_CL_VPORT_XSCALE] ) ), xo = as_float( regs[reg::PA_CL_VPORT_XSCALE + 1] );
        float ys = std::fabs( as_float( regs[reg::PA_CL_VPORT_XSCALE + 2] ) ), yo = as_float( regs[reg::PA_CL_VPORT_XSCALE + 3] );
        int32_t ox = apply_wo ? wox : 0, oy = apply_wo ? woy : 0;
        if ( std::isfinite( xo - xs ) && std::isfinite( xo + xs ) && std::isfinite( yo - ys ) && std::isfinite( yo + ys ) )
        {
            sx0 = std::max( sx0, (int32_t)std::floor( std::max( xo - xs, -32768.0f ) ) + ox );
            sx1 = std::min( sx1, (int32_t)std::ceil( std::min( xo + xs, 32768.0f ) ) + ox );
            sy0 = std::max( sy0, (int32_t)std::floor( std::max( yo - ys, -32768.0f ) ) + oy );
            sy1 = std::min( sy1, (int32_t)std::ceil( std::min( yo + ys, 32768.0f ) ) + oy );
        }
    }
    // Screen-space rect lists (D3D clears) are bounded by their corners.
    int32_t qx0, qy0, qx1, qy1;
    if ( prim == 8 && count == 3 && ( vte & 0xF ) == 0 && ( vte & 0x100 ) && rect_from_vertices( regs, vs.program, qx0, qy0, qx1, qy1 ) )
    {
        int32_t ox = apply_wo ? wox : 0, oy = apply_wo ? woy : 0;
        sx0 = std::max( sx0, qx0 + ox ); sx1 = std::min( sx1, qx1 + ox );
        sy0 = std::max( sy0, qy0 + oy ); sy1 = std::min( sy1, qy1 + oy );
    }
    uint32_t area_w = ct && dt ? std::min( ct->w, dt->w ) : rt->w, area_h = ct && dt ? std::min( ct->h, dt->h ) : rt->h;
    sx0 = std::clamp( sx0, 0, (int32_t)area_w ); sx1 = std::clamp( sx1, 0, (int32_t)area_w );
    sy0 = std::clamp( sy0, 0, (int32_t)area_h ); sy1 = std::clamp( sy1, 0, (int32_t)area_h );
    if ( sx1 <= sx0 || sy1 <= sy0 ) return;
    VkRect2D sc{ { sx0, sy0 }, { (uint32_t)( sx1 - sx0 ), (uint32_t)( sy1 - sy0 ) } };
    static const char *log_ps = RT_ENV( "CRACKDOWN_LOG_PS" );  // hex PS hash: log its draws
    if ( log_ps && strtoull( log_ps, nullptr, 16 ) == ps.hash )
    {
        static int n;
        if ( n++ < 40 || g_draw_census )
        {
            std::string fc;
            char b[64];
            for ( uint32_t t = 0; t < 5; ++t )
            {
                const uint32_t *f = regs + reg::SHADER_CONSTANT_FETCH_00_0 + t * 6;
                snprintf( b, sizeof( b ), " tf%u %08X %08X %08X %08X", t, f[0], f[1], f[2], f[3] );
                fc += b;
            }
            rt_log( "psdraw:   stencil ref %08X bf %08X mode %08X clip %08X%s\n", regs[reg::RB_STENCILREFMASK], regs[reg::RB_STENCILREFMASK_BF], regs[reg::PA_SU_SC_MODE_CNTL],
                    regs[reg::PA_CL_CLIP_CNTL], fc.c_str() );
            for ( uint32_t i = 0; i < 6; ++i )
                if ( regs[reg::PA_CL_CLIP_CNTL] >> i & 1 )
                    rt_log( "psdraw:   ucp%u %g %g %g %g\n", i, as_float( regs[0x2388 + i * 4] ), as_float( regs[0x2389 + i * 4] ), as_float( regs[0x238A + i * 4] ), as_float( regs[0x238B + i * 4] ) );
        }
        if ( n <= 40 || g_draw_census )
            rt_log( "psdraw: %s depth %s rect %d,%d-%d,%d prim %u count %u vte %X vs %016llx blend %08X mask %X (full %08X) depth %08X alphatest %08X "
                    "color info %08X %08X %08X %08X\n", view_name( ct ), view_name( dt ),
                    sx0, sy0, sx1, sy1, prim, count, vte, (unsigned long long)vs.hash, regs[reg::RB_BLENDCONTROL0], color_mask, regs[reg::RB_COLOR_MASK],
                    depthctl, regs[reg::RB_COLORCONTROL], regs[reg::RB_COLOR_INFO], regs[reg::RB_COLOR1_INFO], regs[reg::RB_COLOR2_INFO],
                    regs[reg::RB_COLOR3_INFO] );
    }
    // CRACKDOWN_TEX_DUMP=<ps hash>:<fetch constant>: the base level of each distinct texture that pixel shader
    // reads there (DXT1/3/5 or 8888), decoded on the CPU to build/tex-<address>.ppm: colour left, alpha right.
    if ( static const char *td = RT_ENV( "CRACKDOWN_TEX_DUMP" ); td && strtoull( td, nullptr, 16 ) == ps.hash && strchr( td, ':' ) )
    {
        uint32_t slot = atoi( strchr( td, ':' ) + 1 );
        const uint32_t *f = regs + reg::SHADER_CONSTANT_FETCH_00_0 + slot * 6;
        static std::set<uint32_t> done;
        uint32_t format = f[1] & 63, e = ( f[1] >> 6 ) & 3, base = f[1] & 0xFFFFF000u, w = ( f[2] & 0x1FFF ) + 1, h = ( ( f[2] >> 13 ) & 0x1FFF ) + 1;
        bool tiled = f[0] >> 31, block = format == 18 || format == 19 || format == 20;
        uint32_t pitch = ( ( f[0] >> 22 ) & 0x1FF ) * 32;
        if ( !pitch ) pitch = ( w + 31 ) & ~31u;
        if ( format == 2 && w <= 2048 && h <= 2048 && g_draw_census && done.insert( base ).second )  // 8-bit: as a greyscale .pgm
        {
            std::vector<uint8_t> px( (size_t)w * h );
            uint64_t sum = 0;
            uint8_t lo = 255, hi = 0;
            for ( uint32_t y = 0; y < h; ++y )
                for ( uint32_t x = 0; x < w; ++x )
                {
                    uint8_t v = g_base[0xA0000000 + ( ( base + ( tiled ? tiled_offset( x, y, pitch, 0 ) : y * pitch + x ) ) & 0x1FFFFFFF )];
                    px[(size_t)y * w + x] = v, sum += v, lo = std::min( lo, v ), hi = std::max( hi, v );
                }
            rt_log( "tex8: %08X %ux%u tiled %u min %u max %u mean %.1f\n", base, w, h, tiled, lo, hi, (double)sum / px.size() );
            char path[64];
            snprintf( path, sizeof( path ), "build/tex8-%08X.pgm", base );
            if ( FILE *o = fopen( path, "wb" ) ) { fprintf( o, "P5\n%u %u\n255\n", w, h ), fwrite( px.data(), 1, px.size(), o ), fclose( o ); }
        }
        if ( ( block || format == 6 ) && w <= 2048 && h <= 2048 && done.insert( base ).second )
        {
            auto ld = [&]( uint32_t a ) {
                uint32_t v;
                memcpy( &v, g_base + 0xA0000000 + ( a & 0x1FFFFFFF ), 4 );
                if ( e == 1 ) v = ( ( v & 0x00FF00FFu ) << 8 ) | ( ( v >> 8 ) & 0x00FF00FFu );
                else if ( e == 2 ) v = __builtin_bswap32( v );
                else if ( e == 3 ) v = ( v << 16 ) | ( v >> 16 );
                return v;
            };
            auto c565 = []( uint32_t c, float o[3] ) { o[0] = ( ( c >> 11 ) & 31 ) / 31.f; o[1] = ( ( c >> 5 ) & 63 ) / 63.f; o[2] = ( c & 31 ) / 31.f; };
            std::vector<uint32_t> px( (size_t)w * 2 * h );
            for ( uint32_t y = 0; y < h; ++y )
                for ( uint32_t x = 0; x < w; ++x )
                {
                    float r, g, b, a = 1;
                    if ( block )
                    {
                        uint32_t bx = x >> 2, by = y >> 2, bp = std::max( pitch >> 2, 1u ), lb = format == 18 ? 3 : 4;
                        uint32_t idx = tiled ? tiled_offset( bx, by, bp, lb ) : by * bp + bx, addr = base + ( idx << lb ), pi = ( y & 3 ) * 4 + ( x & 3 );
                        uint32_t cw0 = ld( addr + ( format == 18 ? 0 : 8 ) ), cw1 = ld( addr + ( format == 18 ? 4 : 12 ) );
                        float c0[3], c1[3], c[3];
                        c565( cw0 & 0xFFFF, c0 ); c565( cw0 >> 16, c1 );
                        uint32_t sel = ( cw1 >> ( 2 * pi ) ) & 3;
                        bool four = format != 18 || ( cw0 & 0xFFFF ) > ( cw0 >> 16 );
                        for ( int i = 0; i < 3; ++i )
                            c[i] = sel == 0 ? c0[i] : sel == 1 ? c1[i] : four ? ( sel == 2 ? ( 2 * c0[i] + c1[i] ) / 3 : ( c0[i] + 2 * c1[i] ) / 3 ) : ( sel == 2 ? ( c0[i] + c1[i] ) / 2 : 0 );
                        r = c[0]; g = c[1]; b = c[2];
                        if ( format == 18 && !four && sel == 3 ) a = 0;
                        if ( format == 19 ) a = ( ( ld( addr + ( pi >= 8 ? 4 : 0 ) ) >> ( 4 * ( pi & 7 ) ) ) & 15 ) / 15.f;
                        if ( format == 20 )
                        {
                            uint64_t ab = ld( addr ) | (uint64_t)ld( addr + 4 ) << 32;
                            float a0 = ( ab & 255 ) / 255.f, a1 = ( ( ab >> 8 ) & 255 ) / 255.f;
                            uint32_t s = ( ab >> ( 16 + 3 * pi ) ) & 7;
                            a = s == 0 ? a0 : s == 1 ? a1 : a0 > a1 ? ( ( 8 - s ) * a0 + ( s - 1 ) * a1 ) / 7 : s == 6 ? 0 : s == 7 ? 1 : ( ( 6 - s ) * a0 + ( s - 1 ) * a1 ) / 5;
                        }
                    }
                    else
                    {
                        uint32_t idx = tiled ? tiled_offset( x, y, pitch, 2 ) : y * pitch + x, v = ld( base + idx * 4 );
                        r = ( v & 255 ) / 255.f; g = ( ( v >> 8 ) & 255 ) / 255.f; b = ( ( v >> 16 ) & 255 ) / 255.f; a = ( v >> 24 ) / 255.f;
                    }
                    auto q = []( float v ) { return (uint32_t)std::clamp( v * 255.f + 0.5f, 0.f, 255.f ); };
                    px[(size_t)y * w * 2 + x] = q( r ) | q( g ) << 8 | q( b ) << 16;
                    px[(size_t)y * w * 2 + w + x] = q( a ) * 0x010101u;
                }
            char path[64];
            snprintf( path, sizeof( path ), "build/tex-%08X.ppm", base );
            write_ppm( path, px, w * 2, h );
            rt_log( "texdump: %s %ux%u format %u endian %u tiled %d (ps %016llx tf%u)\n", path, w, h, format, e, (int)tiled, (unsigned long long)ps.hash, slot );
        }
    }
    // Textures in formats texel() can't decode come out magenta: log each (shader, slot, format) once.
    for ( const RendererShader *sh : { &vs, &ps } )
    {
        static std::unordered_map<uint64_t, uint32_t> slots;  // texture fetch constants each shader reads
        auto it = slots.find( sh->hash );
        if ( it == slots.end() )
        {
            uint32_t mask = 0;
            const ucode::Program &pr = *sh->program;
            for ( const ucode::Cf &c : pr.cf )
                if ( c.is_exec() )
                    for ( uint32_t k = 0; k < c.count(); ++k )
                        if ( ( c.sequence() >> ( k * 2 ) ) & 1 )
                        {
                            ucode::Fetch f{ { pr.slot_word( c.address() + k, 0 ), pr.slot_word( c.address() + k, 1 ), pr.slot_word( c.address() + k, 2 ) } };
                            if ( f.op() == 1 ) mask |= 1u << f.const_index();
                        }
            it = slots.emplace( sh->hash, mask ).first;
        }
        for ( uint32_t k = 0; k < 32; ++k )
            if ( ( it->second >> k ) & 1 )
            {
                const uint32_t *f = regs + reg::SHADER_CONSTANT_FETCH_00_0 + k * 6;
                uint32_t format = f[1] & 63;
                static const uint64_t known = ( 1ull << 2 ) | ( 1ull << 3 ) | ( 1ull << 4 ) | ( 1ull << 6 ) | ( 1ull << 7 ) | ( 1ull << 10 ) | ( 1ull << 15 ) |
                                              ( 1ull << 18 ) | ( 1ull << 19 ) | ( 1ull << 20 ) | ( 1ull << 22 ) | ( 1ull << 23 ) | ( 1ull << 24 ) | ( 1ull << 25 ) | ( 1ull << 26 ) | ( 1ull << 30 ) |
                                              ( 1ull << 31 ) | ( 1ull << 32 ) | ( 1ull << 36 ) | ( 1ull << 37 ) | ( 1ull << 38 );
                static std::set<std::tuple<uint64_t, uint32_t, uint32_t>> seen;
                if ( ( f[0] & 3 ) == 2 && !( ( known >> format ) & 1 ) && seen.insert( { sh->hash, k, format } ).second && seen.size() < 200 )
                    rt_log( "texformat: unsupported format %u (%s %016llx tf%u): %08X %08X %08X %08X %08X %08X\n", format, sh == &vs ? "vs" : "ps",
                            (unsigned long long)sh->hash, k, f[0], f[1], f[2], f[3], f[4], f[5] );
            }
    }
    // CRACKDOWN_PLANE_LOG=<ps hash>: per draw, the first texture's base and whether its memory holds anything
    // (Bink movie planes drawn before they are decoded show green).
    if ( static const char *pl = RT_ENV( "CRACKDOWN_PLANE_LOG" ); pl && strstr( pl, hex16( ps.hash ).c_str() ) )
    {
        uint32_t b0 = regs[reg::SHADER_CONSTANT_FETCH_00_0 + 1] & 0x1FFFF000, b1 = regs[reg::SHADER_CONSTANT_FETCH_00_0 + 7] & 0x1FFFF000,
                 b2 = regs[reg::SHADER_CONSTANT_FETCH_00_0 + 13] & 0x1FFFF000;
        auto mean = []( uint32_t b ) {  // mean byte over the plane's first 256 KB
            uint64_t sum = 0;
            for ( uint32_t i = 0; i < 4096; ++i ) sum += g_base[0xA0000000u + b + i * 61];
            return (unsigned)( sum / 4096 );
        };
        rt_log( "plane: ps %016llx tf0 %08X means %u %u %u (frame %llu)\n", (unsigned long long)ps.hash, b0, mean( b0 ), mean( b1 ), mean( b2 ),
                (unsigned long long)gpu_swap_count() );
        renderer_defer( [=] { rt_log( "plane:   at GPU completion %08X means %u %u %u\n", b0, mean( b0 ), mean( b1 ), mean( b2 ) ); } );
    }
    if ( static const char *fd = RT_ENV( "CRACKDOWN_FETCH_DUMP" ); fd && ( !strcmp( fd, "all" ) || strstr( fd, hex16( ps.hash ).c_str() ) || strstr( fd, hex16( vs.hash ).c_str() ) ) )  // texture fetch constants of these pixel / vertex shaders (comma-separated; "all": every shader)
    {
        static std::set<std::string> seen;  // each distinct constant once
        for ( uint32_t k = 0; k < 32; ++k )
        {
            const uint32_t *f = regs + reg::SHADER_CONSTANT_FETCH_00_0 + k * 6;
            if ( ( f[0] & 3 ) == 2 && seen.insert( std::to_string( k ) + std::string( (const char *)f, 24 ) ).second && seen.size() < 20000 )
                    rt_log( "fetchdump: vs %016llx ps %016llx tf%u %08X %08X %08X %08X %08X %08X; target %ux%u depthctl %08X depthinfo %08X\n", (unsigned long long)vs.hash,
                            (unsigned long long)ps.hash, k, f[0], f[1], f[2], f[3], f[4], f[5], rt->w, rt->h, regs[reg::RB_DEPTHCONTROL], regs[reg::RB_DEPTH_INFO] );
        }
    }
    // CRACKDOWN_CONST_DUMP_AT="t1,t2,...": at those input script times, the vertex constants c0-c15 of the
    // next 12 draws (each shader once).
    if ( static const char *ca = RT_ENV( "CRACKDOWN_CONST_DUMP_AT" ); ca )
    {
        double input_script_clock();
        static size_t done;
        static int left;
        static std::set<uint64_t> seen;
        size_t i = 0;
        for ( const char *c = ca; c && *c; c = strchr( c, ',' ) ? strchr( c, ',' ) + 1 : nullptr, ++i )
            if ( i == done && input_script_clock() >= atof( c ) ) { ++done; left = 12; seen.clear(); rt_log( "constdump at %.1f\n", input_script_clock() ); break; }
        if ( left > 0 && rt->w >= 1280 && rt->h >= 700 && seen.insert( vs.hash ).second )
        {
            --left;
            std::string v;
            for ( uint32_t c = 0; c < 16; ++c )
            {
                char b[96];
                const uint32_t *k = regs + reg::SHADER_CONSTANT_000_X + c * 4;
                snprintf( b, sizeof( b ), "\n    c%u = %g %g %g %g", c, as_float( k[0] ), as_float( k[1] ), as_float( k[2] ), as_float( k[3] ) );
                v += b;
            }
            rt_log( "constdump: vs %016llx target %ux%u:%s\n", (unsigned long long)vs.hash, rt->w, rt->h, v.c_str() );
        }
    }
    if ( static const char *cd = RT_ENV( "CRACKDOWN_CONST_DUMP" ); cd && strstr( cd, hex16( vs.hash ).c_str() ) )  // vertex shader constants (hashes, comma-separated)
    {
        static int n;
        if ( n++ < 3 || g_draw_census )
        {
            std::string v;
            for ( uint32_t c : { 0u, 1u, 2u, 3u, 9u, 10u, 11u, 16u, 17u, 18u, 19u, 100u, 101u, 102u, 103u, 252u, 253u, 254u, 255u } )
            {
                char b[96];
                const uint32_t *k = regs + reg::SHADER_CONSTANT_000_X + c * 4;
                snprintf( b, sizeof( b ), "\n    c%u = %g %g %g %g", c, as_float( k[0] ), as_float( k[1] ), as_float( k[2] ), as_float( k[3] ) );
                v += b;
            }
            rt_log( "constdump: draw #%u vs %016llx bools %08X:%s\n", g_draw_census ? g_draw_census - 2 : 0, (unsigned long long)vs.hash, regs[reg::SHADER_CONSTANT_BOOL_000_031], v.c_str() );
        }
    }
    // CRACKDOWN_PS_CONST_DUMP=<ps hash>: that pixel shader's constants c240-c255 (first 3 draws).
    if ( static const char *cd = RT_ENV( "CRACKDOWN_PS_CONST_DUMP" ); cd && strtoull( cd, nullptr, 16 ) == ps.hash )
    {
        static int n;
        if ( n++ < 3 )
        {
            std::string v;
            for ( uint32_t c = 240; c < 256; ++c )
            {
                char b[96];
                const uint32_t *k = regs + reg::SHADER_CONSTANT_000_X + ( 256 + c ) * 4;
                snprintf( b, sizeof( b ), "\n    c%u = %g %g %g %g", c, as_float( k[0] ), as_float( k[1] ), as_float( k[2] ), as_float( k[3] ) );
                v += b;
            }
            rt_log( "constdump: ps %016llx:%s\n", (unsigned long long)ps.hash, v.c_str() );
        }
    }
    // CRACKDOWN_CONST_FULL=<vs hash>,<vs hash>...: all 256 vertex constants of each one's first draw in a census frame.
    if ( static const char *cf = RT_ENV( "CRACKDOWN_CONST_FULL" ); cf && g_draw_census )
    {
        static std::set<uint64_t> done;
        char h[20];
        snprintf( h, sizeof( h ), "%016llx", (unsigned long long)vs.hash );
        if ( strstr( cf, h ) && done.insert( vs.hash ).second )
            for ( uint32_t c = 0; c < 256; ++c )
            {
                const uint32_t *k = regs + reg::SHADER_CONSTANT_000_X + c * 4;
                rt_log( "constfull: vs %s c%u = %g %g %g %g\n", h, c, as_float( k[0] ), as_float( k[1] ), as_float( k[2] ), as_float( k[3] ) );
            }
        snprintf( h, sizeof( h ), "%016llx", (unsigned long long)ps.hash );  // a pixel shader's: c256..c511 (its c0..c255)
        if ( strstr( cf, h ) && done.insert( ps.hash ).second )
            for ( uint32_t c = 0; c < 256; ++c )
            {
                const uint32_t *k = regs + reg::SHADER_CONSTANT_000_X + ( 256 + c ) * 4;
                rt_log( "constfull: ps %s c%u = %g %g %g %g\n", h, c, as_float( k[0] ), as_float( k[1] ), as_float( k[2] ), as_float( k[3] ) );
            }
    }
    if ( static const char *vd = RT_ENV( "CRACKDOWN_VTX_DUMP" ); vd && strtoull( vd, nullptr, 16 ) == vs.hash )  // raw vertex words (fetch 95)
    {
        static int n;
        if ( n++ < 3 )
        {
            uint32_t f0 = regs[reg::SHADER_CONSTANT_FETCH_00_0 + 190], f1 = regs[reg::SHADER_CONSTANT_FETCH_00_0 + 191];
            const uint32_t *vb = (const uint32_t *)( g_base + 0xA0000000 + ( f0 & 0x1FFFFFFC ) );
            std::string v;
            for ( int i = 0; i < 28; ++i ) { char b[12]; snprintf( b, sizeof( b ), "%s%08X", i % 7 ? " " : "\n   ", __builtin_bswap32( vb[i] ) ); v += b; }
            rt_log( "vtxdump: vf95 %08X %08X endian %u:%s\n", f0, f1, f1 & 3, v.c_str() );
        }
    }
    if ( g_edram_trace && count <= 6 && sx1 - sx0 >= 320 )  // full-screen passes: which textures they read
    {
        std::string t;
        for ( uint32_t k = 0; k < 32; ++k )
        {
            const uint32_t *f = regs + reg::SHADER_CONSTANT_FETCH_00_0 + k * 6;
            if ( ( f[0] & 3 ) != 2 || !( f[1] & 0xFFFFF000 ) ) continue;  // texture fetch constants only
            char b[64];
            snprintf( b, sizeof( b ), " t%u=%08X/%ux%u/f%u", k, f[1] & 0xFFFFF000, ( f[2] & 0x1FFF ) + 1, ( ( f[2] >> 13 ) & 0x1FFF ) + 1, f[1] & 63 );
            t += b;
        }
        rt_log( "edram: pass textures ps %016llx:%s\n", (unsigned long long)ps.hash, t.c_str() );
    }
    if ( g_edram_trace && dt && ( depthctl & 0x76 ) == 0x76 )  // depth ALWAYS + write: a clear quad; show its vertices
    {
        uint32_t f0 = regs[reg::SHADER_CONSTANT_FETCH_00_0], f1 = regs[reg::SHADER_CONSTANT_FETCH_00_0 + 1];
        const uint32_t *vb = (const uint32_t *)( g_base + 0xA0000000 + ( f0 & 0x1FFFFFFC ) );
        std::string v;
        for ( int i = 0; i < 9; ++i )
        {
            uint32_t w = ( f1 & 3 ) == 2 ? __builtin_bswap32( vb[i] ) : vb[i];
            float fl; memcpy( &fl, &w, 4 );
            char b[24]; snprintf( b, sizeof( b ), " %g", fl ); v += b;
        }
        rt_log( "edram: clear quad vf0 %08X %08X:%s depth clear reg %08X\n", f0, f1, v.c_str(), regs[reg::RB_DEPTH_CLEAR] );
    }
    if ( g_edram_trace )
        rt_log( "edram: draw %s %s rect %d,%d-%d,%d prim %u count %u zctl %X vte %X clip %08X scissor %08X %08X vp %g %g %g %g z %g %g mode %u vs %016llx ps %016llx\n",
                view_name( ct ), view_name( dt ), sx0, sy0, sx1, sy1, prim, count, depthctl, vte, regs[reg::PA_CL_CLIP_CNTL], tl, br,
                as_float( regs[reg::PA_CL_VPORT_XSCALE] ), as_float( regs[reg::PA_CL_VPORT_XSCALE + 1] ), as_float( regs[reg::PA_CL_VPORT_XSCALE + 2] ),
                as_float( regs[reg::PA_CL_VPORT_XSCALE + 3] ), as_float( regs[reg::PA_CL_VPORT_XSCALE + 4] ), as_float( regs[reg::PA_CL_VPORT_XSCALE + 5] ), mode, (unsigned long long)vs.hash, (unsigned long long)ps.hash );
    if ( static const bool watch = RT_ENV( "CRACKDOWN_TEX_WATCH" ); watch ) tex_watch( regs, vs, ps );
    if ( ct ) sync_surface( ct, dt, sy1 );
    if ( dt ) sync_surface( dt, ct, sy1 );
    // MSAA: drawn into the multisampled images when every surface bound has one (with the same samples),
    // each taken up to date first; otherwise into the images, those with a multisampled one averaged first.
    const bool use_ms = ( ct || dt ) && ( !ct || ct->samples > 1 ) && ( !dt || dt->samples > 1 ) && ( !ct || !dt || ct->samples == dt->samples );
    const uint32_t ms_samples = use_ms ? ( ct ? ct->samples : dt->samples ) : 1;
    for ( Target *t : { ct, dt } )
        if ( t && t->ms_image ) { if ( use_ms ) ms_upload( t ); else ms_resolve( t ); }

    if ( static bool census = RT_ENV( "CRACKDOWN_TEX_CENSUS" ); census )  // first draw per (format, dimension, pixel shader)
    {
        static std::unordered_map<uint64_t, bool> seen;
        const ucode::Program &pp = *ps.program;
        for ( size_t i = 0; i < pp.cf.size(); ++i )
        {
            const ucode::Cf &c = pp.cf[i];
            if ( !c.is_exec() ) continue;
            for ( uint32_t k = 0; k < c.count(); ++k )
                if ( ( c.sequence() >> ( k * 2 ) ) & 1 )
                {
                    uint32_t slot = c.address() + k;
                    ucode::Fetch f{ { pp.slot_word( slot, 0 ), pp.slot_word( slot, 1 ), pp.slot_word( slot, 2 ) } };
                    if ( f.op() != 1 ) continue;
                    const uint32_t *tf = regs + reg::SHADER_CONSTANT_FETCH_00_0 + f.const_index() * 6;
                    uint32_t fmt = tf[1] & 63, dim = ( tf[5] >> 9 ) & 3;
                    uint64_t key = ps.hash ^ ( (uint64_t)fmt << 56 ) ^ ( (uint64_t)dim << 62 ) ^ ( (uint64_t)f.const_index() << 48 );
                    if ( seen[key] ) continue;
                    seen[key] = true;
                    rt_log( "texcensus: ps %016llx t%u format %u dim %u %ux%u base %08X endian %u tiled %u (%s)\n", (unsigned long long)ps.hash,
                            f.const_index(), fmt, dim, ( tf[2] & 0x1FFF ) + 1, ( ( tf[2] >> 13 ) & 0x1FFF ) + 1, tf[1] & 0xFFFFF000, ( tf[1] >> 6 ) & 3,
                            tf[0] >> 31, view_name( ct ) );
                }
        }
    }
    // EDRAM mode 5 (depth only) runs no depth-exporting pixel shader: the one left bound (the fog pass's, which exports
    // depth) would otherwise replace the drawn depth of occlusion test quads.
    RendererShader pixel = mode == 5 && writes_depth( ps ) ? RendererShader{ 0, nullptr } : ps;
    // PA_CL_CLIP_CNTL: bit 16 clipping off, bits 26 / 27 near / far depth clipping off -> depth is clamped instead.
    uint32_t clip_cntl = regs[reg::PA_CL_CLIP_CNTL];
    bool zclamp = ( ( clip_cntl >> 16 ) & 1 ) || ( ( clip_cntl >> 26 ) & 3 );
    VkPipeline pipe = pipeline( vs, pixel, ct ? ct->format : VK_FORMAT_UNDEFINED, dt != nullptr, regs[reg::RB_BLENDCONTROL0],
                                color_mask | ( use_ms ? ms_samples << 12 | ( ( regs[reg::RB_COLORCONTROL] >> 4 & 1 ) ? 0x100u : 0u ) : 0u ),
                                ( depthctl & ( ( depthctl & 1 ) ? 0x7FFFFFF7u : 0x76u ) ) | ( zclamp ? 0x80000000u : 0u ) |
                                    ( ( depthctl & 0x81 ) == 0x81 && ( regs[reg::PA_SU_SC_MODE_CNTL] >> 2 ) & 1 ? 8u : 0u ), topo );
    if ( !pipe ) return;

    // The textures it samples (made first: making one is GPU work outside rendering).
    uint32_t tex[32] = {}, tsize[32] = {};
    for ( bool cube : { false, true } )
        for ( uint32_t mask = image_fetches( vs, cube ) | ( pixel.program ? image_fetches( pixel, cube ) : 0 ); mask; mask &= mask - 1 )
        {
            uint32_t k = __builtin_ctz( mask );
            tex[k] = texture_index( regs + reg::SHADER_CONSTANT_FETCH_00_0 + k * 6, cube );
            const auto &sizes = cube ? g_cube_slot_size : g_slot_size;
            const uint32_t slot = tex[k] & 0xFFFF;
            tsize[k] = tex[k] != 0xFFFFFFFFu && slot < sizes.size() ? sizes[slot] : 0;
        }
    static int tex_users_logged;
    if ( static const char *tu = RT_ENV( "CRACKDOWN_TEX_USERS" ); tu && ( g_edram_trace || ( RT_ENV( "CRACKDOWN_TEX_USERS_ALL" ) && tex_users_logged < 200 ) ) )  // debug: draws sampling that address as an image
        for ( uint32_t mask = image_fetches( vs ) | image_fetches( vs, true ) | ( pixel.program ? image_fetches( pixel ) | image_fetches( pixel, true ) : 0 ); mask; mask &= mask - 1 )
        {
            uint32_t k = __builtin_ctz( mask );
            if ( ( regs[reg::SHADER_CONSTANT_FETCH_00_0 + k * 6 + 1] & 0xFFFFF000u ) == strtoul( tu, nullptr, 16 ) )
                ++tex_users_logged, rt_log( "texusers: frame %llu progcntl %08X misc %08X vs %016llx (%s) ps %016llx (%s) tf%u %s count %u\n", (unsigned long long)gpu_swap_count(), regs[0x2180], regs[0x2181], (unsigned long long)vs.hash, ( image_fetches( vs ) >> k ) & 1 ? "samples" : "-",
                        (unsigned long long)ps.hash, pixel.program && ( ( image_fetches( pixel ) >> k ) & 1 ) ? "samples" : "-", k, view_name( ct ), count );
        }
    if ( static const char *dp = RT_ENV( "CRACKDOWN_DECODED_PS" ); dp && ( strstr( dp, hex16( ps.hash ).c_str() ) || strstr( dp, hex16( vs.hash ).c_str() ) ) )  // debug: that shader's textures decoded
        for ( uint32_t &t : tex ) t = 0xFFFFFFFFu;

    VkImage want_color = ct ? ( use_ms ? ct->ms_image : ct->image ) : VK_NULL_HANDLE, want_depth = dt ? ( use_ms ? dt->ms_image : dt->image ) : VK_NULL_HANDLE;
    if ( ( !g_rendering_color && !g_rendering_depth ) || g_rendering_color != want_color || g_rendering_depth != want_depth )
    {
        end_rendering();
        barrier_all();
        VkRenderingAttachmentInfo ca{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
        ca.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        ca.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderingAttachmentInfo da = ca;
        if ( ct ) ca.imageView = use_ms ? ct->ms_view : ct->view;
        if ( dt ) da.imageView = use_ms ? dt->ms_view : dt->view;
        VkRenderingInfo ri{ VK_STRUCTURE_TYPE_RENDERING_INFO };
        ri.renderArea = { { 0, 0 }, { (uint32_t)at8( area_w, rt->scale8 ), (uint32_t)at8( area_h, rt->scale8 ) } };  // colour and depth: the same scale (one MSAA mode)
        ri.layerCount = 1;
        ri.colorAttachmentCount = ct ? 1 : 0;
        ri.pColorAttachments = ct ? &ca : nullptr;
        ri.pDepthAttachment = dt ? &da : nullptr;
        ri.pStencilAttachment = dt ? &da : nullptr;
        static const int passes = RT_ENV( "CRACKDOWN_GPU_PASSES" ) ? atoi( RT_ENV( "CRACKDOWN_GPU_PASSES" ) ) : 0;
        if ( passes && g_ts_pool )
        {
            char name[160];
            snprintf( name, sizeof( name ), "%s %ux%u fmt %u%s | ps %016llx", ct ? "color" : "depth only", ( ct ? ct : dt )->w, ( ct ? ct : dt )->h,
                      ct ? ct->format : dt->format, ct && dt ? " +depth" : "", (unsigned long long)ps.hash );
            auto [it, fresh] = g_pass_index.try_emplace( name, (uint32_t)g_pass_stats.size() );
            if ( fresh ) g_pass_stats.push_back( { name, 0, 0, 0, 0 } );
            g_pass_id = it->second;
            g_pass_stats[g_pass_id].passes++;
            g_pass_q = gpu_timer_begin( GT_COUNT + (int)g_pass_id );
        }
        vkCmdBeginRendering( g_cmd, &ri );
        g_rendering_color = want_color;
        g_rendering_depth = want_depth;
    }
    if ( g_pass_q != ~0u ) g_pass_stats[g_pass_id].draws++;

    // Per-draw constants.
    VkDeviceSize size = ( sizeof(DrawConstants) + g_dc_align - 1 ) & ~( g_dc_align - 1 );
    if ( g_dc_head + size > g_dc_size ) g_dc_head = 0;  // wrap
    for ( Submission &sub : g_subs )  // still read by a command buffer in flight: wait for it
        if ( sub.busy && ( sub.dc_start <= sub.dc_end ? g_dc_head < sub.dc_end && g_dc_head + size > sub.dc_start
                                                      : g_dc_head < sub.dc_end || g_dc_head + size > sub.dc_start ) )
            complete( sub );
    DrawConstants *dc = (DrawConstants *)( g_dc_ptr + g_dc_head );
    memcpy( dc->c, regs + reg::SHADER_CONSTANT_000_X, sizeof( dc->c ) );
    memcpy( dc->fetch, regs + reg::SHADER_CONSTANT_FETCH_00_0, sizeof( dc->fetch ) );
    memcpy( dc->bools, regs + reg::SHADER_CONSTANT_BOOL_000_031, sizeof( dc->bools ) );
    memcpy( dc->loops, regs + reg::SHADER_CONSTANT_LOOP_00, sizeof( dc->loops ) );
    memcpy( dc->tex, tex, sizeof( tex ) );
    memcpy( dc->tsize, tsize, sizeof( tsize ) );
    // CRACKDOWN_SIGN_LOG (debug): each pixel shader / fetch constant / format with a non-unsigned number format, once.
    if ( static const bool sign_log = RT_ENV( "CRACKDOWN_SIGN_LOG" ); sign_log )
    {
        static std::set<std::tuple<uint64_t, uint32_t, uint32_t, uint32_t>> seen;
        for ( uint32_t k = 0; k < 32; ++k )
        {
            const uint32_t d0 = dc->fetch[k * 6], fmt = dc->fetch[k * 6 + 1] & 63, signs = ( d0 >> 2 ) & 0xFF;
            if ( ( d0 & 3 ) == 2 && signs && seen.insert( { ps.hash, k, signs, fmt } ).second )
                rt_log( "signlog: ps %016llx tf%u signs %02X format %u\n", (unsigned long long)ps.hash, k, signs, fmt );
        }
    }
    // SQ_PROGRAM_CNTL bit 18: parameter generation, into the register SQ_CONTEXT_MISC bits 8-15 name.
    static const bool no_param_gen = RT_ENV( "CRACKDOWN_NO_PARAM_GEN" );  // debug: as before parameter generation
    dc->param_gen = ( regs[0x2180] >> 18 ) & 1 && !no_param_gen ? ( regs[0x2181] >> 8 ) & 0xFF : ~0u;
    // User clip planes: PA_CL_CLIP_CNTL bits 0-5 enable PA_CL_UCP_0..5 (0x2388, 4 floats each, clip space;
    // bit 16 turns clipping off). The water's stencil pass marks the dry area by the Agency's tunnel mouth
    // with a whole wave patch clipped to that area's planes: unclipped, it hid the water around it too.
    static const bool no_ucp = RT_ENV( "CRACKDOWN_NO_CLIP_PLANES" );  // debug: as before
    dc->ucp_ena = no_ucp || ( regs[reg::PA_CL_CLIP_CNTL] >> 16 & 1 ) ? 0u : regs[reg::PA_CL_CLIP_CNTL] & 0x3F;
    for ( uint32_t i = 0; i < 6; ++i )
        for ( uint32_t j = 0; j < 4; ++j ) dc->ucp[i][j] = dc->ucp_ena >> i & 1 ? as_float( regs[0x2388 + i * 4 + j] ) : 0.0f;
    // Above the console's resolution, flat drawing (no depth: menus, movies, text, the HUD) magnifies the
    // game's 720p art: a sharper filter than bilinear there (tfetch). Not pictures the GPU made (bloom,
    // blurs: soft on purpose), not the 3D world.
    // The game's font (Local\<language>\Crackdown_Font.dds: one DXT3 picture, sampled as 1024x2048, no smaller levels,
    // glyphs rasterised at 28 pixels) is coverage in its alpha: above 720p its glyphs are cut at half
    // coverage with a screen pixel's softening, so letters stay crisp at any size, not stretched blurs.
    // Flat drawing: nothing depth-tested or written (movies), or XUI's own pixel shaders (menus, the HUD's
    // panels: XUI writes depth), its pictures (38a039da3bb38e80) and its text (1d8932c61407efbf).
    const bool xui = ps.hash == 0x38a039da3bb38e80ull || ps.hash == 0x1d8932c61407efbfull;
    const bool flat = xui || ( !( depthctl & 4 ) && ( !( depthctl & 2 ) || ( ( depthctl >> 4 ) & 7 ) == 7 ) );
    dc->sharp = dc->text = 0;
    if ( render_scale8() > 8 && flat && pixel.program )
        for ( uint32_t mask = image_fetches( pixel, false ); mask; mask &= mask - 1 )
        {
            uint32_t k = __builtin_ctz( mask );
            const uint32_t *f = regs + reg::SHADER_CONSTANT_FETCH_00_0 + k * 6;
            const uint32_t fmt = f[1] & 63, w = ( f[2] & 0x1FFF ) + 1, h = ( ( f[2] >> 13 ) & 0x1FFF ) + 1;
            if ( !tex[k] || tex[k] == 0xFFFFFFFFu || resolved( f[1] & 0x1FFFF000u ) ) continue;
            dc->sharp |= 1u << k;
            // The game's font: Crackdown_Font.dds, one 1024x2048 DXT3 picture (2048x1024 allowed too).
            if ( fmt == 19 && ( ( w == 1024 && h == 2048 ) || ( w == 2048 && h == 1024 ) ) && ( tex[k] & 0xFFFF ) != g_font_slot ) dc->text |= 1u << k;
        }
    for ( int i = 0; i < 3; ++i )
    {
        dc->vp_scale[i] = as_float( regs[reg::PA_CL_VPORT_XSCALE + i * 2] );
        dc->vp_offset[i] = as_float( regs[reg::PA_CL_VPORT_XSCALE + i * 2 + 1] );
    }
    // DLSS's jitter: the scene's draws (the world, with the viewport transform) move by it, in surface pixels.
    if ( dlss_scene_part && ( vte & 0x3F ) == 0x3F )
    {
        const float per = rt->scale8 / 8.0f;
        dc->vp_offset[0] += g_df.jx / per, dc->vp_offset[1] += g_df.jy / per;
    }
    // NDC conversion uses the image size: window coordinates map 1:1 onto image pixels.
    dc->rt[0] = (float)rt->w; dc->rt[1] = (float)rt->h; dc->rt[2] = 1.0f / rt->w; dc->rt[3] = 1.0f / rt->h;
    dc->window_offset[0] = apply_wo ? (float)wox : 0.0f;
    dc->window_offset[1] = apply_wo ? (float)woy : 0.0f;
    dc->window_offset[2] = rt->scale8 / 8.0f;  // image pixels per surface pixel
    dc->window_offset[3] = (float)rt->scale8 / (float)rt->hi8;
    // Bit 16 (ours): PA_SU_VTX_CNTL bit 0 clear, Direct3D 9's pixel centres (on whole window coordinates,
    // not Vulkan's +0.5; the shaders move vertices and the pixel position by half a pixel to match).
    static const bool gl_centres = RT_ENV( "CRACKDOWN_GL_PIXEL_CENTRES" );  // debug: the old way (no half-pixel move)
    dc->vte = vte | ( ( regs[0x2302] & 1 ) || gl_centres ? 0u : 0x10000u );
    dc->prim = prim;
    dc->index_mode = ( initiator >> 6 ) & 3;
    dc->index_addr = index_base & 0x1FFFFFFF;
    dc->index_info = ( ( initiator >> 11 ) & 1 ) | ( ( ( index_info >> 30 ) & 3 ) << 4 );
    dc->index_offset = regs[reg::VGT_INDX_OFFSET];
    dc->first = 0;
    if ( indexed ) dc->prim = 4, dc->index_mode = 2;  // the vertex number is the index
    // DLSS's motion (MotionHistory): the scene's draws through the viewport transform.
    dc->mv_cur = dc->mv_prev = ~0u, dc->mv_pix = 0, dc->mv_w = dc->mv_h = 0, dc->mv_jx = dc->mv_jy = 0, dc->mv_cam = 0;
    const bool mv_draw = g_mv.on && g_mv.cleared && dlss_scene_part && ( vte & 0x3F ) == 0x3F;
    uint64_t mv_key = 0;
    if ( mv_draw )
    {
        const float per = rt->scale8 / 8.0f;
        dc->mv_jx = g_df.jx / per, dc->mv_jy = g_df.jy / per;
        dc->mv_w = (uint32_t)at8( 1280, rt->scale8 ), dc->mv_h = (uint32_t)at8( 720, rt->scale8 );
        if ( g_mv.cam ) dc->mv_cam = 1, memcpy( dc->mv_m, g_mv.m, sizeof( g_mv.m ) );  // (vertices moving with the world write no motion)
        // Its pixels' motion where it writes depth, or as see-through where it is blended over what's
        // there without writing depth.
        const bool fits = (VkDeviceSize)dc->mv_w * dc->mv_h <= MotionHistory::PIX;
        const bool blended = ct && regs[reg::RB_BLENDCONTROL0] != 0x00010001u;
        dc->mv_pix = !fits ? 0u : ( depthctl & 6 ) == 6 ? 1u : !( depthctl & 4 ) && blended ? 2u | ( depthctl & 2 ? 0u : 4u ) : 0u;
        // DLSS's see-through mask (prelude): blended over the scene without writing depth; bit 4 where the
        // colour is added at full strength (source factor one).
        if ( fits && blended && !( depthctl & 4 ) && !RT_ENV( "CRACKDOWN_DLSS_NO_MASK" ) )
            dc->mv_pix |= 8u | ( ( regs[reg::RB_BLENDCONTROL0] & 0x1F ) == 1 ? 16u : 0u );
        uint64_t h = 1469598103934665603ull;
        auto mix = [&]( uint64_t v ) { for ( int i = 0; i < 8; ++i ) h = ( h ^ ( ( v >> ( i * 8 ) ) & 255 ) ) * 1099511628211ull; };
        mix( vs.hash ), mix( ps.hash ), mix( index_base ), mix( count ), mix( prim | (uint64_t)indexed << 8 );
        static const bool no_buffers = RT_ENV( "CRACKDOWN_MV_KEY_NO_BUFFERS" );  // debug: vertex buffers not in the key
        if ( !no_buffers )
            for ( uint32_t f = 92; f < 96; ++f )
            {
                // The buffer and a sample of what it holds (its start, middle and end): the game reuses buffers
                // for different things (streamed terrain tiles as the player moves), and a draw matched to
                // another's last frame gets that one's places: smears that grow with speed.
                const uint32_t f0 = regs[reg::SHADER_CONSTANT_FETCH_00_0 + f * 2], f1 = regs[reg::SHADER_CONSTANT_FETCH_00_0 + f * 2 + 1];
                mix( f0 );
                const uint32_t addr = f0 & 0x1FFFFFFC, bytes = ( f1 >> 2 & 0xFFFFFF ) * 4;
                if ( ( f0 & 3 ) != 3 || !addr || bytes < 32 || addr + bytes > 0x20000000 ) continue;
                const uint8_t *data = g_base + 0xA0000000 + addr;
                for ( uint32_t at : { 0u, ( bytes / 2 ) & ~7u, bytes - 32 } )
                    for ( uint32_t i = 0; i < 32; i += 8 ) { uint64_t w; memcpy( &w, data + at + i, 8 ); mix( w ); }
            }
        mv_key = h + 0x9E3779B97F4A7C15ull * g_mv.seen[h]++;
    }
    // Its vertices' places: a slot per vertex number (indexed: up to the largest index).
    static const int mv_test = RT_ENV( "CRACKDOWN_MV_TEST" ) ? atoi( RT_ENV( "CRACKDOWN_MV_TEST" ) ) : 0;  // debug: 1 no pixel motion, 2 no vertex places
    if ( mv_test == 1 ) dc->mv_pix = 0;
    auto mv_slots = [&]( uint32_t slots ) {
        if ( !mv_draw || !slots || g_mv.next + slots > MotionHistory::SLOTS || mv_test == 2 ) return;
        dc->mv_cur = g_mv.half * MotionHistory::SLOTS + g_mv.next;
        g_mv.next += slots;
        g_mv.now[mv_key] = { dc->mv_cur, slots };
        if ( auto it = g_mv.prev.find( mv_key ); it != g_mv.prev.end() && it->second.slots == slots ) dc->mv_prev = it->second.base;
        if ( static const long at = RT_ENV( "CRACKDOWN_MV_LOG_FRAME" ) ? atol( RT_ENV( "CRACKDOWN_MV_LOG_FRAME" ) ) : -1; at >= 0 && (long)g_frames_seen == at )
            rt_log( "mv: vs %016llx ps %016llx count %u slots %u prim %u indexed %d pix %u depthctl %08X %s\n", (unsigned long long)vs.hash, (unsigned long long)ps.hash,
                    count, slots, prim, (int)indexed, dc->mv_pix, depthctl, dc->mv_prev != ~0u ? "matched" : "NEW" );
    };
    // CRACKDOWN_CONST_FULL=<vs hash>,<vs hash>...: all 256 vertex constants of each one's first draw in a census frame.
    if ( static const char *cf = RT_ENV( "CRACKDOWN_CONST_FULL" ); cf && g_draw_census )
    {
        static std::set<uint64_t> done;
        char h[20];
        snprintf( h, sizeof( h ), "%016llx", (unsigned long long)vs.hash );
        if ( strstr( cf, h ) && done.insert( vs.hash ).second )
            for ( uint32_t c = 0; c < 256; ++c )
            {
                const uint32_t *k = regs + reg::SHADER_CONSTANT_000_X + c * 4;
                rt_log( "constfull: vs %s c%u = %g %g %g %g\n", h, c, as_float( k[0] ), as_float( k[1] ), as_float( k[2] ), as_float( k[3] ) );
            }
        snprintf( h, sizeof( h ), "%016llx", (unsigned long long)ps.hash );  // a pixel shader's: c256..c511 (its c0..c255)
        if ( strstr( cf, h ) && done.insert( ps.hash ).second )
            for ( uint32_t c = 0; c < 256; ++c )
            {
                const uint32_t *k = regs + reg::SHADER_CONSTANT_000_X + ( 256 + c ) * 4;
                rt_log( "constfull: ps %s c%u = %g %g %g %g\n", h, c, as_float( k[0] ), as_float( k[1] ), as_float( k[2] ), as_float( k[3] ) );
            }
    }
    // CRACKDOWN_VTX_LIST=<vs hash>:<stride dwords>: every vertex one indexed draw of that shader references,
    // as floats (the 5th such draw, once).
    if ( static const char *vl = RT_ENV( "CRACKDOWN_VTX_LIST" ); vl && strtoull( vl, nullptr, 16 ) == vs.hash && dc->index_mode == 0 && strchr( vl, ':' ) )
    {
        static int n;
        if ( ++n == 5 )
        {
            uint32_t stride = atoi( strchr( vl, ':' ) + 1 ), f0 = regs[reg::SHADER_CONSTANT_FETCH_00_0 + 190];
            const uint32_t *vb = (const uint32_t *)( g_base + 0xA0000000 + ( f0 & 0x1FFFFFFC ) );
            const uint8_t *ib = g_base + 0xA0000000 + dc->index_addr;
            std::set<uint32_t> seen;
            for ( uint32_t i = 0; i < count && seen.size() < 64; ++i )
            {
                uint32_t v;
                if ( dc->index_info & 1 ) { memcpy( &v, ib + i * 4, 4 ); v = __builtin_bswap32( v ); }
                else { uint16_t h; memcpy( &h, ib + i * 2, 2 ); v = __builtin_bswap16( h ); }
                v += dc->index_offset;
                if ( !seen.insert( v ).second ) continue;
                std::string line;
                for ( uint32_t k = 0; k < stride; ++k ) { char b[24]; snprintf( b, sizeof( b ), " %g", as_float( __builtin_bswap32( vb[v * stride + k] ) ) ); line += b; }
                rt_log( "vtxlist: #%u v%u:%s\n", i, v, line.c_str() );
            }
        }
    }
    if ( static const char *vd = RT_ENV( "CRACKDOWN_VTX_DUMP" ); vd && strtoull( vd, nullptr, 16 ) == vs.hash && dc->index_mode == 0 )
    {
        // Vertices this draw indexes whose dwords 5/6 (two half2 UV sets) hold |h| >= 64, inf or nan.
        static int logged;
        uint32_t f0 = regs[reg::SHADER_CONSTANT_FETCH_00_0 + 190];
        const uint32_t *vb = (const uint32_t *)( g_base + 0xA0000000 + ( f0 & 0x1FFFFFFC ) );
        const uint8_t *ib = g_base + 0xA0000000 + dc->index_addr;
        uint32_t big = 0, ex = 0, exi = 0;
        for ( uint32_t i = 0; i < count; ++i )
        {
            uint32_t v;
            if ( dc->index_info & 1 ) { memcpy( &v, ib + i * 4, 4 ); v = __builtin_bswap32( v ); }
            else { uint16_t h; memcpy( &h, ib + i * 2, 2 ); v = __builtin_bswap16( h ); }
            v += dc->index_offset;
            uint32_t w6 = __builtin_bswap32( vb[v * 7 + 6] );
            for ( uint32_t h : { w6 >> 16, w6 & 0xFFFF } )
                if ( ( ( h >> 10 ) & 31 ) >= 21 ) { if ( !big++ ) { ex = w6; exi = v; } }
        }
        if ( big && logged++ < 30 )
            rt_log( "vtxcheck: vb %08X ib %08X count %u idxoff %u: %u big uv2 halves, e.g. vertex %u = %08X %08X %08X | %08X %08X | %08X %08X\n", f0 & ~3u,
                    dc->index_addr, count, dc->index_offset, big, exi, __builtin_bswap32( vb[exi * 7] ), __builtin_bswap32( vb[exi * 7 + 1] ),
                    __builtin_bswap32( vb[exi * 7 + 2] ), __builtin_bswap32( vb[exi * 7 + 3] ), __builtin_bswap32( vb[exi * 7 + 4] ),
                    __builtin_bswap32( vb[exi * 7 + 5] ), ex );
    }
    static const char *magenta_ps = RT_ENV( "CRACKDOWN_MAGENTA_PS" );  // hex PS hashes, comma-separated: those output magenta
    bool listed = false;
    for ( const char *c = magenta_ps; c && *c; )
    {
        char *end;
        if ( strtoull( c, &end, 16 ) == ps.hash ) { listed = true; break; }
        c = *end == ',' ? end + 1 : nullptr;
    }
    const bool magenta = RT_ENV( "CRACKDOWN_DEBUG_MAGENTA" ) || listed;
    static const char *trace_vs = RT_ENV( "CRACKDOWN_TRACE_VS" );  // hex VS hash: record its first vertices
    static long trace_skip = RT_ENV( "CRACKDOWN_TRACE_SKIP" ) ? atol( RT_ENV( "CRACKDOWN_TRACE_SKIP" ) ) : 0;  // matching draws to pass over first
    bool trace = trace_vs && strtoull( trace_vs, nullptr, 16 ) == vs.hash && trace_skip-- <= 0;
    // CRACKDOWN_TRACE_CENSUS=<vs hash>,...: in a census frame, the first draw of each listed VS.
    if ( static const char *tc = RT_ENV( "CRACKDOWN_TRACE_CENSUS" ); tc && g_draw_census )
    {
        static std::set<uint64_t> done;
        char h[20];
        snprintf( h, sizeof( h ), "%016llx", (unsigned long long)vs.hash );
        static uint32_t last_census, traced_here;  // every draw of the listed shaders, up to 64 per census frame
        if ( g_draw_census < last_census ) traced_here = 0;  // a new census frame
        last_census = g_draw_census;
        if ( strstr( tc, h ) && traced_here < 64 ) { ++traced_here; trace = true; rt_log( "renderer: trace census draw #%u vs %s count %u\n", g_draw_census - 2, h, count ); }
        (void)done;
    }
    dc->flags = ( magenta ? 256u : 0u ) | ( trace ? 512u : 0u );
    uint32_t offset = (uint32_t)g_dc_head;
    g_dc_head += size;

    vkCmdBindPipeline( g_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe );
    VkDescriptorSet sets[2] = { g_draw_set, g_tex_set };
    vkCmdBindDescriptorSets( g_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_draw_pl, 0, 2, sets, 1, &offset );
    // The shader leaves depth before the viewport's z scale / offset: clipping happens there, as on the
    // Xenos, and the viewport depth range applies the mapping.
    float zs = ( vte & 16 ) ? dc->vp_scale[2] : 1.0f, zo = ( vte & 32 ) ? dc->vp_offset[2] : 0.0f;
    float zmin = std::clamp( zo, 0.0f, 1.0f ), zmax = std::clamp( zo + zs, 0.0f, 1.0f );
    VkViewport v{ 0, 0, rt->w * rt->scale8 / 8.0f, rt->h * rt->scale8 / 8.0f, zmin, zmax };
    vkCmdSetViewport( g_cmd, 0, 1, &v );
    static const char *probe = RT_ENV( "CRACKDOWN_PROBE" );
    int32_t px0, py0, px1, py1;
    float f0, f1, f2, f3;
    bool probing = probe && g_draw_census && ct && sscanf( probe, "%f,%f,%f,%f", &f0, &f1, &f2, &f3 ) == 4;
    if ( probing )
    {
        // Pixels of the 1280-wide screen, scaled to each target's width so low-resolution buffers are probed
        // at the same place.
        float k = rt->w / 1280.0f;
        px0 = (int32_t)( f0 * k ), py0 = (int32_t)( f1 * k ), px1 = (int32_t)( f2 * k + 0.99f ), py1 = (int32_t)( f3 * k + 0.99f );
        int32_t x0 = std::max( sc.offset.x, px0 ), y0 = std::max( sc.offset.y, py0 );
        int32_t x1 = std::min( sc.offset.x + (int32_t)sc.extent.width, px1 ), y1 = std::min( sc.offset.y + (int32_t)sc.extent.height, py1 );
        if ( x1 <= x0 || y1 <= y0 ) return;
        sc = { { x0, y0 }, { (uint32_t)( x1 - x0 ), (uint32_t)( y1 - y0 ) } };
    }
    {
        const uint32_t k = rt->scale8;
        const int32_t ix0 = at8( sc.offset.x, k ), iy0 = at8( sc.offset.y, k );
        VkRect2D isc{ { ix0, iy0 }, { (uint32_t)( at8( sc.offset.x + (int32_t)sc.extent.width, k ) - ix0 ), (uint32_t)( at8( sc.offset.y + (int32_t)sc.extent.height, k ) - iy0 ) } };
        vkCmdSetScissor( g_cmd, 0, 1, &isc );
    }
    float bc[4] = { as_float( regs[reg::RB_BLEND_RED] ), as_float( regs[reg::RB_BLEND_RED + 1] ), as_float( regs[reg::RB_BLEND_RED + 2] ), as_float( regs[reg::RB_BLEND_RED + 3] ) };
    vkCmdSetBlendConstants( g_cmd, bc );
    // Face culling, PA_SU_SC_MODE_CNTL: bit 0 front faces, bit 1 back faces; bit 2: clockwise (in window
    // coordinates, as the framebuffer's) is the front. Without it the backs of building walls drew into the
    // depth pre-pass in front of window glass, whose colour pass then never passed (black / stale windows).
    // Not for rectangle lists (their fourth corner is made here, its winding not the game's), points, lines.
    {
        const uint32_t mc = regs[reg::PA_SU_SC_MODE_CNTL];
        static const bool no_cull = RT_ENV( "CRACKDOWN_NO_CULL" );  // debug
        const bool faces = prim == 4 || prim == 5 || prim == 6 || prim == 13;
        vkCmdSetCullMode( g_cmd, no_cull || !faces ? VK_CULL_MODE_NONE : ( mc & 1 ? VK_CULL_MODE_FRONT_BIT : 0 ) | ( mc & 2 ? VK_CULL_MODE_BACK_BIT : 0 ) );
        vkCmdSetFrontFace( g_cmd, ( mc >> 2 ) & 1 ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE );
    }
    if ( dt && ( depthctl & 1 ) )  // RB_STENCILREFMASK: reference bits 0-7, compare mask 8-15, write mask 16-23 (_BF: back faces)
    {
        uint32_t f = regs[reg::RB_STENCILREFMASK], b = ( depthctl >> 7 ) & 1 ? regs[reg::RB_STENCILREFMASK_BF] : f;
        vkCmdSetStencilReference( g_cmd, VK_STENCIL_FACE_FRONT_BIT, f & 0xFF );
        vkCmdSetStencilReference( g_cmd, VK_STENCIL_FACE_BACK_BIT, b & 0xFF );
        vkCmdSetStencilCompareMask( g_cmd, VK_STENCIL_FACE_FRONT_BIT, ( f >> 8 ) & 0xFF );
        vkCmdSetStencilCompareMask( g_cmd, VK_STENCIL_FACE_BACK_BIT, ( b >> 8 ) & 0xFF );
        vkCmdSetStencilWriteMask( g_cmd, VK_STENCIL_FACE_FRONT_BIT, ( f >> 16 ) & 0xFF );
        vkCmdSetStencilWriteMask( g_cmd, VK_STENCIL_FACE_BACK_BIT, ( b >> 16 ) & 0xFF );
    }
    static int logged;
    if ( RT_ENV( "CRACKDOWN_DRAW_LOG" ) && logged++ < 12 )
        rt_log( "renderer: draw prim %u count %u -> %u verts, rt %ux%u, scissor %d,%d %ux%u, vte %X, vp %.1f %.1f %.1f %.1f, mask %X, blend %08X, vs %016llx ps %016llx\n",
                prim, count, vertices, rt->w, rt->h, sc.offset.x, sc.offset.y, sc.extent.width, sc.extent.height, dc->vte,
                dc->vp_scale[0], dc->vp_offset[0], dc->vp_scale[1], dc->vp_offset[1], regs[reg::RB_COLOR_MASK], regs[reg::RB_BLENDCONTROL0],
                (unsigned long long)vs.hash, (unsigned long long)ps.hash );
    if ( RT_ENV( "CRACKDOWN_DRAW_LOG" ) && logged <= 12 )
        rt_log( "renderer:   depthctl %08X colorinfo %08X depthinfo %08X surface %08X modectl %08X colorctl %08X\n", depthctl, cinfo, dinfo, surface,
                regs[reg::RB_MODECONTROL], regs[reg::RB_COLORCONTROL] );
    if ( RT_ENV( "CRACKDOWN_DRAW_LOG" ) && logged <= 12 )
    {
        uint32_t f0 = regs[reg::SHADER_CONSTANT_FETCH_00_0 + 190], f1 = regs[reg::SHADER_CONSTANT_FETCH_00_0 + 191];
        uint32_t base = f0 & 0x1FFFFFFC;
        std::string vtx;
        for ( uint32_t i = 0; i < 20 && i < ( f1 >> 2 ); ++i )
        {
            uint32_t v = *(uint32_t *)( g_base + 0xA0000000 + base + i * 4 );
            if ( ( f1 & 3 ) == 2 ) v = __builtin_bswap32( v );
            float fl; memcpy( &fl, &v, 4 );
            char b[24]; snprintf( b, sizeof(b), " %.2f", fl ); vtx += b;
        }
        rt_log( "renderer:   vf95 %08X %08X:%s\n", f0, f1, vtx.c_str() );
        for ( int t = 0; t < 3; ++t )
        {
            const uint32_t *tf = regs + reg::SHADER_CONSTANT_FETCH_00_0 + t * 6;
            rt_log( "renderer:   tf%d %08X %08X %08X %08X %08X %08X\n", t, tf[0], tf[1], tf[2], tf[3], tf[4], tf[5] );
        }
        rt_log( "renderer:   ps c0-c3: %g %g %g %g | %g %g %g %g | %g %g %g %g | %g %g %g %g\n",
                as_float( regs[0x4400] ), as_float( regs[0x4401] ), as_float( regs[0x4402] ), as_float( regs[0x4403] ),
                as_float( regs[0x4404] ), as_float( regs[0x4405] ), as_float( regs[0x4406] ), as_float( regs[0x4407] ),
                as_float( regs[0x4408] ), as_float( regs[0x4409] ), as_float( regs[0x440A] ), as_float( regs[0x440B] ),
                as_float( regs[0x440C] ), as_float( regs[0x440D] ), as_float( regs[0x440E] ), as_float( regs[0x440F] ) );
    }
    // Inside open occlusion queries the draw is counted by a Vulkan query of its own.
    uint32_t query = ~0u;
    bool counted = false;
    for ( ZQuery &q : g_zqueries ) counted |= !q.end_addr;
    static const bool query_all = RT_ENV( "CRACKDOWN_QUERY_ALL" );  // debug: count every draw
    if ( ( counted || query_all || probing ) && g_query_next < QUERY_POOL_SIZE )
    {
        query = g_query_next++;
        if ( probing ) g_probes.push_back( { query, g_draw_census - 2, vs.hash, ps.hash, count, rt->w, rt->h, regs[reg::RB_BLENDCONTROL0] } );
        for ( ZQuery &q : g_zqueries )
            if ( !q.end_addr )
            {
                q.ids.push_back( query ), ++q.draws;
                q.vs = vs.hash, q.ps = ps.hash, q.prim = prim, q.vertices = count, q.depth_control = regs[reg::RB_DEPTHCONTROL], q.surface = regs[reg::RB_SURFACE_INFO];
            }
        g_subs[g_cur].query_scale8[query] = (uint8_t)rt->scale8;
        g_subs[g_cur].query_samples[query] = (uint8_t)( use_ms ? ms_samples : 1 );
        vkCmdBeginQuery( g_cmd, g_query_pool, query, VK_QUERY_CONTROL_PRECISE_BIT & ( g_precise_queries ? ~0u : 0u ) );
    }
    const uint32_t vk_draws_before = g_rt_stats.vk_draws;
    if ( indexed )
    {
        VkIndexType type;
        VkDeviceSize at = index_buffer( g_base + 0xA0000000 + ( index_base & 0x1FFFFFFF ), count, ( initiator >> 11 ) & 1, ( index_info >> 30 ) & 3,
                                        restart ? regs[reg::VGT_MULTI_PRIM_IB_RESET_INDX] : ~0u, type );
        if ( mv_draw )
        {
            mv_slots( index_top( g_base + 0xA0000000 + ( index_base & 0x1FFFFFFF ), count, ( initiator >> 11 ) & 1, ( index_info >> 30 ) & 3,
                                 restart ? regs[reg::VGT_MULTI_PRIM_IB_RESET_INDX] : ~0u ) );
        }
        vkCmdBindIndexBuffer( g_cmd, g_dc_buf, at, type );
        vkCmdDrawIndexed( g_cmd, count, 1, 0, 0, 0 );
        g_rt_stats.vk_draws++;
    }
    else if ( mv_slots( std::max( count, vertices ) ); false ) {}
    else
    // Indexed strips with primitive reset (PA_SU_SC_MODE_CNTL bit 21): one draw per run between reset
    // indices, its start passed as the first instance.
    if ( prim == 6 && dc->index_mode == 0 && ( ( regs[reg::PA_SU_SC_MODE_CNTL] >> 21 ) & 1 ) )
    {
        const bool wide = dc->index_info & 1;
        const uint32_t reset = regs[reg::VGT_MULTI_PRIM_IB_RESET_INDX] & ( wide ? 0xFFFFFF : 0xFFFF );
        const uint32_t endian = ( dc->index_info >> 4 ) & 3;
        const uint8_t *ib = g_base + 0xA0000000 + dc->index_addr;
        auto index = [&]( uint32_t i ) {
            uint32_t w;
            memcpy( &w, ib + ( wide ? i * 4 : ( i & ~1u ) * 2 ), 4 );
            w = endian == 1 ? ( ( w & 0x00FF00FF ) << 8 ) | ( ( w >> 8 ) & 0x00FF00FF ) : endian == 2 ? __builtin_bswap32( w )
              : endian == 3 ? ( w << 16 ) | ( w >> 16 ) : w;
            return wide ? w & 0xFFFFFF : ( i & 1 ) ? w >> 16 : w & 0xFFFF;
        };
        uint32_t start = 0;
        auto run_end = [&]( uint32_t i ) {
            if ( i - start >= 3 ) vkCmdDraw( g_cmd, ( i - start - 2 ) * 3, 1, 0, start ), g_rt_stats.vk_draws++;
            start = i + 1;
        };
        if ( !wide )
        {
            // 16-bit indices: compare the stored halfwords 16 at a time with the reset index as stored
            // (endian 1 and 2 swap the bytes of each, 2 and 3 swap the halfwords of each pair).
            const uint16_t stored = endian == 1 || endian == 2 ? __builtin_bswap16( (uint16_t)reset ) : (uint16_t)reset;
            const bool pairs = endian >= 2;
            const __m256i want = _mm256_set1_epi16( (short)stored );
            for ( uint32_t b = 0; b < count; b += 16 )
            {
                __m256i v = _mm256_loadu_si256( (const __m256i *)( ib + b * 2 ) );
                // bit 2k for halfword k
                uint32_t m = (uint32_t)_mm256_movemask_epi8( _mm256_cmpeq_epi16( v, want ) ) & 0x55555555;
                if ( pairs ) m = ( ( m & 0x11111111 ) << 2 ) | ( ( m >> 2 ) & 0x11111111 );
                if ( count - b < 16 ) m &= ( 1u << ( ( count - b ) * 2 ) ) - 1;
                for ( ; m; m &= m - 1 ) run_end( b + __builtin_ctz( m ) / 2 );
            }
            run_end( count );
        }
        else
            for ( uint32_t i = 0; i <= count; ++i )
                if ( i == count || index( i ) == reset ) run_end( i );
    }
    else
        vkCmdDraw( g_cmd, vertices, 1, 0, 0 ), g_rt_stats.vk_draws++;
    if ( g_pass_q != ~0u ) g_pass_stats[g_pass_id].vk_draws += g_rt_stats.vk_draws - vk_draws_before;
    if ( query != ~0u ) vkCmdEndQuery( g_cmd, g_query_pool, query );
    if ( ct ) mark_written( ct, sx0, sy0, sx1, sy1 );
    if ( dt && ( depthctl & 5 ) ) mark_written( dt, sx0, sy0, sx1, sy1 );
    for ( Target *t : { ct, dt && ( depthctl & 5 ) ? dt : nullptr } )  // MSAA: which image has the newest
        if ( t && t->ms_image ) ( use_ms ? t->ms_newer : t->ss_newer ) = true;
    // A frame goes to the GPU in parts (CRACKDOWN_SUBMIT_DRAWS, default 1000 Vulkan draws: measured best): the GPU draws
    // the first while the rest is translated, instead of all of it after (the game lets one frame be in
    // flight: its time was translation + GPU added up).
    g_sub_draws += g_rt_stats.vk_draws - vk_draws_before;
    static const uint32_t chunk = RT_ENV( "CRACKDOWN_SUBMIT_DRAWS" ) ? (uint32_t)atoi( RT_ENV( "CRACKDOWN_SUBMIT_DRAWS" ) ) : 1000;
    if ( chunk && g_sub_draws >= chunk && !trace ) renderer_flush();
    if ( trace )
    {
        static int traced;
        if ( traced++ < 3 || RT_ENV( "CRACKDOWN_TRACE_CENSUS" ) )
        {
            renderer_finish();
            const float *f = (const float *)( g_base + 0xA0000000 + 0x1FF00000 );
            for ( uint32_t v = 0; v < std::min( vertices, 64u ); ++v, f += 16 )
                rt_log( "renderer: trace v%u pos %g %g %g %g win %g %g o0 %g %g %g %g o1 %g %g %g %g\n", v, f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7],
                        f[8], f[9], f[10], f[11], f[12], f[13] );
            memset( g_base + 0xA0000000 + 0x1FF00000, 0, 64 * 64 );
            // CRACKDOWN_TRACE_DUMP=1: the draw's registers and the memory its fetches read, for offline emulation
            // (build/vsdump/<draw>/regs.bin = the register file, mem-<addr>.bin = memory ranges, uniform big-endian).
            if ( RT_ENV( "CRACKDOWN_TRACE_DUMP" ) )
            {
                char dir[64];
                snprintf( dir, sizeof( dir ), "build/vsdump/%u", g_draw_census ? g_draw_census - 2 : traced );
                std::filesystem::create_directories( dir );
                auto put = [&]( const char *name, const void *p, size_t n ) {
                    std::string path = std::string( dir ) + "/" + name;
                    if ( FILE *fp = fopen( path.c_str(), "wb" ) ) { fwrite( p, 1, n, fp ); fclose( fp ); }
                };
                put( "regs.bin", regs, 0x10000 * 4 );
                char info[160];
                snprintf( info, sizeof( info ), "initiator %08X index_base %08X index_info %08X vs %016llx ps %016llx\n", initiator, index_base, index_info,
                          (unsigned long long)vs.hash, (unsigned long long)ps.hash );
                put( "draw.txt", info, strlen( info ) );
                auto range = [&]( uint32_t addr, uint32_t bytes ) {
                    addr &= 0x1FFFFFFF;
                    bytes = std::min( bytes, 16u << 20 );
                    if ( !bytes || addr + bytes > 0x20000000 ) return;
                    char name[40];
                    snprintf( name, sizeof( name ), "mem-%08X.bin", addr );
                    put( name, g_base + 0xA0000000 + addr, bytes );
                };
                for ( uint32_t i = 0; i < 96; ++i )  // vertex fetch constants that look valid
                {
                    uint32_t f0 = regs[reg::SHADER_CONSTANT_FETCH_00_0 + i * 2], f1 = regs[reg::SHADER_CONSTANT_FETCH_00_0 + i * 2 + 1];
                    if ( ( f0 & 3 ) == 3 && ( f1 >> 2 ) ) range( f0 & 0x1FFFFFFC, ( f1 >> 2 ) * 4 );
                }
                for ( uint32_t k = 0; k < 32; ++k )  // textures: base level, generously sized
                {
                    const uint32_t *t = regs + reg::SHADER_CONSTANT_FETCH_00_0 + k * 6;
                    if ( ( t[0] & 3 ) != 2 ) continue;
                    uint32_t w = ( t[2] & 0x1FFF ) + 1, hh = ( ( t[2] >> 13 ) & 0x1FFF ) + 1, pitch = ( ( t[0] >> 22 ) & 0x1FF ) * 32;
                    range( t[1] & 0xFFFFF000, std::max( pitch, ( w + 31 ) & ~31u ) * ( ( hh + 31 ) & ~31u ) * 16 );
                }
                if ( ( ( initiator >> 6 ) & 3 ) == 0 ) range( index_base, count * 4 );
                rt_log( "renderer: trace dumped to %s\n", dir );
            }
            begin();
        }
    }
}

static void hires_full( uint32_t dest )
{
    static bool said;
    if ( !said ) said = true, rt_log( "renderer: no room for the full size resolve at %08X\n", dest );
}

// Queues a copy of a front buffer's full size image for showing; its slot, or -1 if it has none.
void fsr1_easu_constants( uint32_t c[16], float in_w, float in_h, float out_w, float out_h );  // fsr1.cpp
void fsr1_rcas_constants( uint32_t c[4], float sharpness_stops );

static bool fsr_image( FsrImage &i, VkFormat format, uint32_t w, uint32_t h, VkImageUsageFlags usage )
{
    i.image = make_image( format, w, h, usage, 0, i.mem );
    if ( !i.image ) return false;
    i.view = make_view( i.image, format, VK_IMAGE_ASPECT_COLOR_BIT );
    first_use( i.image, false, false );
    return true;
}

static bool post_input( uint32_t w, uint32_t h )
{
    static uint32_t have_w, have_h;
    if ( have_w ) return have_w == w && have_h == h;  // the sizes are set for the run
    if ( !fsr_image( g_fsr_in, VK_FORMAT_B8G8R8A8_UNORM, w, h, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT ) ) return false;
    have_w = w, have_h = h;
    return true;
}

static bool fxaa_image( uint32_t w, uint32_t h )
{
    if ( g_fxaa_w ) return g_fxaa_w == w && g_fxaa_h == h;
    if ( !fsr_image( g_fxaa_out, VK_FORMAT_R8G8B8A8_UNORM, w, h, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT ) ) return false;
    g_fxaa_w = w, g_fxaa_h = h;
    return true;
}

// DLSS in the frame (DlssFrame).
namespace
{
bool dlss_frame_images( uint32_t w, uint32_t h, uint32_t out_w, uint32_t out_h )
{
    if ( g_df.w ) return g_df.w == w && g_df.h == h && g_df.out_w == out_w && g_df.out_h == out_h;  // the sizes are set for the run
    const VkImageUsageFlags rw = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
    if ( !fsr_image( g_df.in, VK_FORMAT_R8G8B8A8_UNORM, w, h, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT ) ||
         !fsr_image( g_df.depth, VK_FORMAT_R32_SFLOAT, w, h, rw ) || !fsr_image( g_df.motion, VK_FORMAT_R16G16_SFLOAT, w, h, rw ) ||
         !fsr_image( g_df.mask, VK_FORMAT_R8_UNORM, w, h, rw ) )
        return false;
    if ( !fsr_image( g_df.out, VK_FORMAT_R8G8B8A8_UNORM, out_w, out_h, rw | VK_IMAGE_USAGE_TRANSFER_SRC_BIT ) ) return false;
    g_df.held.image = make_image( VK_FORMAT_R8G8B8A8_UNORM, out_w, out_h,
                                  VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                  VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT, g_df.held.mem );
    if ( !g_df.held.image ) return false;
    g_df.out_raw = make_view( g_df.held.image, VK_FORMAT_R32_UINT, VK_IMAGE_ASPECT_COLOR_BIT );
    first_use( g_df.held.image, false, false );
    g_df.out_set = surface_set( g_df.out_raw, g_dummy_depth.view, g_dummy_depth.stencil, g_hires_buf );
    if ( !g_df.out_set ) return false;
    g_df.w = w, g_df.h = h, g_df.out_w = out_w, g_df.out_h = out_h;
    rt_log( "renderer: DLSS's pictures %ux%u -> %ux%u\n", w, h, out_w, out_h );
    return true;
}

// The scene's depth and motion vectors, at the picture's size (surface_w x surface_h of the surface, at the resolution).
// The scene's picture (its top left surface_w x surface_h), depth and motion vectors, kept for DLSS; false if not.
bool dlss_snapshot( uint32_t surface_w, uint32_t surface_h )
{
    const uint32_t k8 = g_df.scene_color->scale8, w = at8( surface_w, k8 ), h = at8( surface_h, k8 );
    Target *d = g_df.scene_depth;
    if ( d ) ms_resolve( d );
    if ( g_df.scene_color ) ms_resolve( g_df.scene_color );
    if ( !d || !g_df.depth_pipe || !dlss_frame_images( w, h, at8( surface_w, render_scale8() ), at8( surface_h, render_scale8() ) ) ) return false;
    struct { float m[16]; float a[4]; float b[4]; uint32_t c[4]; } pc;
    if ( !dlss_camera_move( pc.m ) ) return false;
    pc.a[0] = g_df.vps[0], pc.a[1] = g_df.vps[1], pc.a[2] = g_df.vpo[0], pc.a[3] = g_df.vpo[1];
    pc.b[0] = k8 / 8.0f, pc.b[1] = (float)d->scale8 / k8, pc.b[2] = (float)w, pc.b[3] = (float)h;
    pc.c[0] = g_mv.on && g_mv.cleared, pc.c[1] = (uint32_t)at8( 1280, d->scale8 ), pc.c[2] = (uint32_t)at8( 720, d->scale8 ), pc.c[3] = g_df.show_pipe != VK_NULL_HANDLE && atoi( RT_ENV( "CRACKDOWN_DLSS_SHOW_MOTION" ) ) != 2;  // (2: the mask as it is)  // (mv_w, mv_h)
    end_rendering();
    barrier_all();
    for ( int i = 0; i < 2; ++i )
    {
        const FsrImage &to = i ? g_df.motion : g_df.depth;
        VkDescriptorImageInfo src{ g_post_sampler, d->view, VK_IMAGE_LAYOUT_GENERAL }, dst{ VK_NULL_HANDLE, to.view, VK_IMAGE_LAYOUT_GENERAL };
        VkDescriptorBufferInfo pix{ g_mv.pix, 0, VK_WHOLE_SIZE };
        VkDescriptorImageInfo msk{ VK_NULL_HANDLE, g_df.mask.view, VK_IMAGE_LAYOUT_GENERAL };
        VkWriteDescriptorSet wr[4]{ { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET }, { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET }, { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET },
                                    { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET } };
        wr[0].dstBinding = 0, wr[0].descriptorCount = 1, wr[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, wr[0].pImageInfo = &src;
        wr[1].dstBinding = 1, wr[1].descriptorCount = 1, wr[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, wr[1].pImageInfo = &dst;
        wr[2].dstBinding = 2, wr[2].descriptorCount = 1, wr[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, wr[2].pBufferInfo = &pix;
        wr[3].dstBinding = 3, wr[3].descriptorCount = 1, wr[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, wr[3].pImageInfo = &msk;
        vkCmdBindPipeline( g_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, i ? g_df.motion_pipe : g_df.depth_pipe );
        g_push_descriptors( g_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_fsr_pl, 0, 4, wr );
        vkCmdPushConstants( g_cmd, g_fsr_pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof( pc ), &pc );
        vkCmdDispatch( g_cmd, ( w + 15 ) / 16, ( h + 15 ) / 16, 1 );
    }
    VkImageCopy c{};
    c.srcSubresource = c.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    c.extent = { w, h, 1 };
    vkCmdCopyImage( g_cmd, g_df.scene_color->image, VK_IMAGE_LAYOUT_GENERAL, g_df.in.image, VK_IMAGE_LAYOUT_GENERAL, 1, &c );
    barrier_all();
    g_df.snapped = true;
    return true;
}

// DLSS on the scene's picture (its top left surface_w x surface_h), into out at the resolution; false if it didn't run.
void dlss_run()
{
    g_df.done = true;
    if ( !g_df.snapped ) return;
    end_rendering();
    VkImageCopy c{};
    c.srcSubresource = c.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    const DlssImage color{ g_df.in.image, g_df.in.view, VK_FORMAT_R8G8B8A8_UNORM, g_df.w, g_df.h };
    const DlssImage depth{ g_df.depth.image, g_df.depth.view, VK_FORMAT_R32_SFLOAT, g_df.w, g_df.h };
    const DlssImage motion{ g_df.motion.image, g_df.motion.view, VK_FORMAT_R16G16_SFLOAT, g_df.w, g_df.h };
    const DlssImage out{ g_df.out.image, g_df.out.view, VK_FORMAT_R8G8B8A8_UNORM, g_df.out_w, g_df.out_h };
    static const bool still = RT_ENV( "CRACKDOWN_DLSS_NO_MOTION" );  // debug: as with a still camera
    if ( still )
    {
        VkClearColorValue zero{};
        VkImageSubresourceRange all{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        vkCmdClearColorImage( g_cmd, g_df.motion.image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &all );
        barrier_all();
    }
    static const float sign = RT_ENV( "CRACKDOWN_DLSS_JITTER_SIGN" ) ? (float)atof( RT_ENV( "CRACKDOWN_DLSS_JITTER_SIGN" ) ) : 1.0f;  // debug
    // The see-through mask (the motion shader's: where see-through drawings show), not while it's shown as debug.
    const DlssImage mask{ g_df.mask.image, g_df.mask.view, VK_FORMAT_R8_UNORM, g_df.w, g_df.h };
    const bool use_mask = g_mv.on && !RT_ENV( "CRACKDOWN_DLSS_NO_MASK" ) && !( g_df.show_pipe != VK_NULL_HANDLE && atoi( RT_ENV( "CRACKDOWN_DLSS_SHOW_MOTION" ) ) != 2 );
    if ( !upscaler_evaluate( g_cmd, color, depth, motion, use_mask ? &mask : nullptr, out, sign * g_df.jx, sign * g_df.jy, !g_df.ran_before || !g_df.prev_ok ) ) return;
    barrier_all();
    if ( fsr3_ready() && g_df.alpha_pipe && !RT_ENV( "CRACKDOWN_FSR3_NO_ALPHA" ) )  // (kUpscaleAlphaShader)
    {
        VkDescriptorImageInfo src{ g_post_sampler, g_df.in.view, VK_IMAGE_LAYOUT_GENERAL }, dst{ VK_NULL_HANDLE, g_df.out.view, VK_IMAGE_LAYOUT_GENERAL };
        VkWriteDescriptorSet wr[2]{ { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET }, { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET } };
        wr[0].dstBinding = 0, wr[0].descriptorCount = 1, wr[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, wr[0].pImageInfo = &src;
        wr[1].dstBinding = 1, wr[1].descriptorCount = 1, wr[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, wr[1].pImageInfo = &dst;
        float size[4] = { (float)g_df.out_w, (float)g_df.out_h, 0, 0 };
        vkCmdBindPipeline( g_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_df.alpha_pipe );
        g_push_descriptors( g_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_fsr_pl, 0, 2, wr );
        vkCmdPushConstants( g_cmd, g_fsr_pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof( size ), size );
        vkCmdDispatch( g_cmd, ( g_df.out_w + 15 ) / 16, ( g_df.out_h + 15 ) / 16, 1 );
        barrier_all();
    }
    if ( static const bool probe = RT_ENV( "CRACKDOWN_DLSS_PROBE" ); probe )  // debug: the motion at a few places, logged a frame later
    {
        static VkBuffer buf;
        static VkDeviceMemory mem;
        static float *host;
        static int pending;
        if ( !buf )
        {
            VkBufferCreateInfo ci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
            ci.size = 4096, ci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            vkCreateBuffer( g_dev, &ci, nullptr, &buf );
            VkMemoryRequirements req;
            vkGetBufferMemoryRequirements( g_dev, buf, &req );
            VkMemoryAllocateInfo ai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
            ai.allocationSize = req.size, ai.memoryTypeIndex = find_memory( req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT );
            vkAllocateMemory( g_dev, &ai, nullptr, &mem );
            vkBindBufferMemory( g_dev, buf, mem, 0 );
            vkMapMemory( g_dev, mem, 0, VK_WHOLE_SIZE, 0, (void **)&host );
        }
        static const float at[][2] = { { 0.49f, 0.62f }, { 0.49f, 0.75f }, { 0.50f, 0.85f }, { 0.20f, 0.85f }, { 0.80f, 0.85f }, { 0.20f, 0.30f }, { 0.80f, 0.30f }, { 0.5f, 0.45f } };
        if ( pending && ++pending > 3 )
        {
            std::string line;
            for ( int i = 0; i < 8; ++i )
            {
                uint16_t h[2];
                memcpy( h, (const char *)host + i * 4, 4 );
                auto half = []( uint16_t v ) { uint32_t s = v >> 15, e = ( v >> 10 ) & 31, m = v & 1023; float f = e ? ldexpf( 1.0f + m / 1024.0f, (int)e - 15 ) : ldexpf( m / 1024.0f, -14 ); return s ? -f : f; };
                char t[64];
                snprintf( t, sizeof t, " (%.2f,%.2f)=%.2f,%.2f", at[i][0], at[i][1], half( h[0] ), half( h[1] ) );
                line += t;
            }
            rt_log( "dlss probe:%s\n", line.c_str() );
            pending = 0;
        }
        if ( !pending && g_frames_seen % 30 == 0 )
        {
            VkBufferImageCopy r[8]{};
            for ( int i = 0; i < 8; ++i )
                r[i].bufferOffset = i * 4, r[i].imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                r[i].imageOffset = { (int32_t)( at[i][0] * g_df.w ), (int32_t)( at[i][1] * g_df.h ), 0 }, r[i].imageExtent = { 1, 1, 1 };
            vkCmdCopyImageToBuffer( g_cmd, g_df.motion.image, VK_IMAGE_LAYOUT_GENERAL, buf, 8, r );
            barrier_all();
            pending = 1;
        }
    }
    if ( g_df.show_pipe )  // debug: the motion in its place
    {
        VkDescriptorImageInfo src{ g_post_sampler, g_df.motion.view, VK_IMAGE_LAYOUT_GENERAL }, dst{ VK_NULL_HANDLE, g_df.out.view, VK_IMAGE_LAYOUT_GENERAL },
            msk{ VK_NULL_HANDLE, g_df.mask.view, VK_IMAGE_LAYOUT_GENERAL };
        VkWriteDescriptorSet wr[3]{ { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET }, { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET }, { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET } };
        wr[0].dstBinding = 0, wr[0].descriptorCount = 1, wr[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, wr[0].pImageInfo = &src;
        wr[1].dstBinding = 1, wr[1].descriptorCount = 1, wr[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, wr[1].pImageInfo = &dst;
        wr[2].dstBinding = 3, wr[2].descriptorCount = 1, wr[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, wr[2].pImageInfo = &msk;
        float size[4] = { (float)g_df.out_w, (float)g_df.out_h, 0, 0 };
        vkCmdBindPipeline( g_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_df.show_pipe );
        g_push_descriptors( g_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_fsr_pl, 0, 3, wr );
        vkCmdPushConstants( g_cmd, g_fsr_pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof( size ), size );
        vkCmdDispatch( g_cmd, ( g_df.out_w + 15 ) / 16, ( g_df.out_h + 15 ) / 16, 1 );
        barrier_all();
    }
    c.extent = { g_df.out_w, g_df.out_h, 1 };
    vkCmdCopyImage( g_cmd, g_df.out.image, VK_IMAGE_LAYOUT_GENERAL, g_df.held.image, VK_IMAGE_LAYOUT_GENERAL, 1, &c );
    barrier_all();
    hires_resolve( g_df.pending, g_df.pending_pc, g_df.out_set, render_scale8(), 8 );
    g_df.ran = true;
}
} // namespace

static bool fsr_images( uint32_t in_w, uint32_t in_h, uint32_t out_w, uint32_t out_h )
{
    if ( g_fsr_out_w ) return g_fsr_in_w == in_w && g_fsr_in_h == in_h && g_fsr_out_w == out_w && g_fsr_out_h == out_h;
    if ( !fsr_image( g_fsr_mid, VK_FORMAT_R8G8B8A8_UNORM, out_w, out_h, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT ) ||
         !fsr_image( g_fsr_out, VK_FORMAT_R8G8B8A8_UNORM, out_w, out_h, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT ) )
        return false;
    g_fsr_in_w = in_w, g_fsr_in_h = in_h, g_fsr_out_w = out_w, g_fsr_out_h = out_h;
    rt_log( "renderer: FSR 1 %ux%u to %ux%u\n", in_w, in_h, out_w, out_h );
    return true;
}

int renderer_hires_show( uint32_t base, uint32_t &w, uint32_t &h, const void *&pixels, bool &rgba )
{
    if ( !g_ok ) return -1;
    auto it = g_hires.find( base & 0x1FFFF000u );
    if ( it == g_hires.end() ) return -1;
    if ( memwatch_written( it->first, it->second.size, it->second.since ) ) { g_hires.erase( it ); return -1; }
    const HiRes &r = it->second;
    VkDeviceSize bytes = (VkDeviceSize)r.pitch * at8( r.h, r.k8 ) * r.bytes;
    if ( bytes > HIRES_SHOW_SIZE || r.bytes != 4 || ( r.k8 == 8 && !fsr_wanted() && !fxaa_wanted() && !upscaler_ready() ) ) return -1;
    uint32_t slot = g_hires_show_next++ % HIRES_SHOWS;
    begin();
    end_rendering();
    barrier_all();
    uint32_t in_w = at8( r.w, r.k8 ), in_h = at8( r.h, r.k8 );
    rgba = false;
    uint32_t out_w = 0, out_h = 0;
    bool fsr = fsr_wanted() && g_fsr_easu_pipe && video_display_size( out_w, out_h ) && out_w > in_w && out_w <= 5120 && out_h <= 2880;
    bool fxaa = fxaa_wanted() && g_fxaa_pipe && !upscaler_ready();  // (DLSS is the anti-aliasing)
    if ( ( fsr || fxaa ) && post_input( in_w, in_h ) && ( !fxaa || fxaa_image( in_w, in_h ) ) && ( !fsr || fsr_images( in_w, in_h, out_w, out_h ) ) )
    {
        VkBufferImageCopy in{};
        in.bufferOffset = r.offset;
        in.bufferRowLength = r.pitch;
        in.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        in.imageExtent = { in_w, in_h, 1 };
        vkCmdCopyBufferToImage( g_cmd, g_hires_buf, g_fsr_in.image, VK_IMAGE_LAYOUT_GENERAL, 1, &in );
        barrier_all();
        auto pass = [&]( VkPipeline pipe, VkImageView from, VkImageView to, const uint32_t *con, uint32_t dw, uint32_t dh ) {
            VkDescriptorImageInfo src{ g_post_sampler, from, VK_IMAGE_LAYOUT_GENERAL }, dst{ VK_NULL_HANDLE, to, VK_IMAGE_LAYOUT_GENERAL };
            VkWriteDescriptorSet w[2]{ { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET }, { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET } };
            w[0].dstBinding = 0, w[0].descriptorCount = 1, w[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, w[0].pImageInfo = &src;
            w[1].dstBinding = 1, w[1].descriptorCount = 1, w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, w[1].pImageInfo = &dst;
            vkCmdBindPipeline( g_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe );
            g_push_descriptors( g_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_fsr_pl, 0, 2, w );
            vkCmdPushConstants( g_cmd, g_fsr_pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, 64, con );
            vkCmdDispatch( g_cmd, ( dw + 15 ) / 16, ( dh + 15 ) / 16, 1 );
            barrier_all();
        };
        uint32_t con[16] = {};
        VkImageView from = g_fsr_in.view;
        if ( fxaa )
        {
            float rcp[2] = { 1.0f / in_w, 1.0f / in_h };
            memcpy( con, rcp, sizeof( rcp ) );
            pass( g_fxaa_pipe, from, g_fxaa_out.view, con, in_w, in_h );
            from = g_fxaa_out.view;
        }
        VkImage last = g_fxaa_out.image;
        if ( fsr )
        {
            memset( con, 0, sizeof( con ) );
            fsr1_easu_constants( con, (float)in_w, (float)in_h, (float)out_w, (float)out_h );
            pass( g_fsr_easu_pipe, from, g_fsr_mid.view, con, out_w, out_h );
            memset( con, 0, sizeof( con ) );
            fsr1_rcas_constants( con, 0.2f );
            pass( g_fsr_rcas_pipe, g_fsr_mid.view, g_fsr_out.view, con, out_w, out_h );
            last = g_fsr_out.image;
        }
        else out_w = in_w, out_h = in_h;
        VkBufferImageCopy o{};
        o.bufferOffset = slot * HIRES_SHOW_SIZE;
        o.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        o.imageExtent = { out_w, out_h, 1 };
        vkCmdCopyImageToBuffer( g_cmd, last, VK_IMAGE_LAYOUT_GENERAL, g_hires_show_buf, 1, &o );
        barrier_all();
        w = out_w, h = out_h, rgba = true;
    }
    else
    {
        VkBufferCopy c{ r.offset, slot * HIRES_SHOW_SIZE, bytes };
        vkCmdCopyBuffer( g_cmd, g_hires_buf, g_hires_show_buf, 1, &c );
        barrier_all();
        w = r.pitch, h = in_h;
    }
    pixels = g_hires_show + slot * HIRES_SHOW_SIZE;
    return (int)slot;
}

namespace
{
// A resolve's copy at full size (HiRes), k image pixels per pixel in eighths, from the surface read through
// set (sscale: its image pixels per surface pixel over k, in eighths).
void hires_resolve( const HiResolve &a, ResolvePC pc, VkDescriptorSet set, uint32_t k, uint32_t sscale )
{
    uint32_t fmt = ( a.info >> 7 ) & 63, bpp = fmt == 32 || fmt == 37 ? 8 : 4, hp = ( at8( a.w, k ) + 31 ) & ~31u;
    HiRes &r = g_hires[a.dest & 0x1FFFF000u];
    if ( r.w != a.w || r.h != a.h || r.k8 != k || r.bytes != bpp )
    {
        // (Kept at its largest: DLSS's picture and the scene's own take turns at the same address.)
        uint32_t need = hp * at8( a.h, k ) * bpp;
        if ( !r.offset || need > r.room )
        {
            if ( g_hires_next + need > HIRES_SIZE ) { g_hires.erase( a.dest & 0x1FFFF000u ); hires_full( a.dest ); return; }
            r.offset = g_hires_next, r.room = need;
            g_hires_next = ( g_hires_next + need + 4095 ) & ~4095u;
        }
        r.w = a.w, r.h = a.h, r.k8 = k, r.bytes = bpp, r.pitch = hp;
    }
    pc.base = r.offset, pc.pitch = hp, pc.height = at8( a.h, k ), pc.info = a.info | 8;
    pc.spitch = a.pitch;
    pc.sx = at8( a.ex0, k ), pc.sy = at8( a.ey0, k ), pc.dx = at8( a.dx, k ), pc.dy = at8( a.dy, k );
    pc.w = at8( a.dx + a.rw, k ) - pc.dx, pc.h = at8( a.dy + a.rh, k ) - pc.dy, pc.sscale = sscale;
    uint32_t dyn = 0;
    vkCmdBindPipeline( g_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_resolve_pipe );
    vkCmdBindDescriptorSets( g_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_surface_pl, 0, 1, &set, 1, &dyn );
    vkCmdPushConstants( g_cmd, g_surface_pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof( pc ), &pc );
    vkCmdDispatch( g_cmd, ( pc.w + 7 ) / 8, ( pc.h + 7 ) / 8, 1 );
    barrier_all();
    ++r.gen;
    r.size = ( ( ( a.w + 31 ) & ~31u ) * ( ( a.h + 31 ) & ~31u ) * bpp + 4095 ) & ~4095u;
}
} // namespace

void renderer_resolve( const uint32_t *regs, const ucode::Program *vs )
{
    if ( !renderer_init() ) return;
    uint32_t copy = regs[reg::RB_COPY_CONTROL], info = regs[reg::RB_COPY_DEST_INFO], dpitch = regs[reg::RB_COPY_DEST_PITCH];
    uint32_t surface = regs[reg::RB_SURFACE_INFO], pitch = surface & 0x3FFF, msaa = std::min( ( surface >> 16 ) & 3, 2u );
    uint32_t src = copy & 7;
    if ( !pitch ) return;
    uint32_t cinfo = src < 4 ? regs[reg::RB_COLOR_INFO + ( src ? src + 1 : 0 )] : regs[reg::RB_DEPTH_INFO];
    uint32_t w = dpitch & 0x3FFF, h = ( dpitch >> 16 ) & 0x3FFF;
    uint32_t dest = regs[reg::RB_COPY_DEST_BASE];
    ++g_resolved[dest & 0x1FFFF000u];
    // The resolve draw's rectangle (three float2 corners, screen space, from the bound vertex shader's
    // first vertex fetch) selects the area. The source
    // is EDRAM at that position moved by the window offset (predicated tiling renders each tile there),
    // and the destination too: per-tile resolves point the destination base at the tile.
    int32_t rx0 = 0, ry0 = 0, rx1 = (int32_t)w, ry1 = (int32_t)h;
    bool have_rect = rect_from_vertices( regs, vs, rx0, ry0, rx1, ry1 );
    if ( !have_rect ) rx0 = ry0 = 0, rx1 = (int32_t)w, ry1 = (int32_t)h;
    uint32_t wo = regs[reg::PA_SC_WINDOW_OFFSET];
    bool apply_wo = ( regs[reg::PA_SU_SC_MODE_CNTL] >> 16 ) & 1;
    int32_t wox = apply_wo ? ( (int32_t)( wo << 17 ) ) >> 17 : 0, woy = apply_wo ? ( (int32_t)( wo << 1 ) ) >> 17 : 0;
    // EDRAM-space rectangle (clamped to the surface) and the matching destination position.
    int32_t ex0 = rx0 + wox, ey0 = ry0 + woy, ex1 = rx1 + wox, ey1 = ry1 + woy;
    int32_t cx = std::max( 0, -ex0 ), cy = std::max( 0, -ey0 );
    ex0 += cx; ey0 += cy;
    ex1 = std::min( ex1, (int32_t)pitch );
    int32_t dx = ex0, dy = ey0;  // the destination base already accounts for the tile: same offset rectangle
    int32_t rw = ex1 - ex0, rh = ey1 - ey0;

    static const bool log_all = RT_ENV( "CRACKDOWN_RESOLVE_LOG" );
    bool log = log_all || g_draw_census || g_edram_trace;  // census frames list their resolves among the draws
    if ( log )
        rt_log( "renderer: resolve src %u EDRAM %03X fmt %u pitch %u msaa %ux rect (%d,%d)-(%d,%d) woff (%d,%d) -> %08X %ux%u (%d,%d) %dx%d info %08X copy %08X\n",
                src, cinfo & 0xFFF, ( cinfo >> 16 ) & 0xF, pitch, 1u << msaa, rx0, ry0, rx1, ry1, wox, woy, dest, w, h, dx, dy, rw, rh, info, copy );
    if ( log && ( copy >> 9 ) & 1 )
        rt_log( "renderer:   depth clear %08X depthinfo %08X depthctl %08X\n", regs[reg::RB_DEPTH_CLEAR], regs[reg::RB_DEPTH_INFO], regs[reg::RB_DEPTHCONTROL] );

    begin();
    end_rendering();
    if ( src <= 4 && w && h && dest && rw > 0 && rh > 0 && ( ( copy >> 20 ) & 3 ) != 3 )
    {
        Target *st = target( edram_base( cinfo ), src == 4 ? 0 : ( cinfo >> 16 ) & 0xF, pitch, msaa, src == 4 );
        if ( !st ) return;
        sync_surface( st, nullptr, ey0 + rh );
        ms_resolve( st );
        end_rendering();
        // DLSS (DlssFrame): the scene's picture, with its depth before the resolve clears it, made at the
        // resolution; that is the full size resolve below.
        ResolvePC pc{
            dest, w, h, info | ( RT_ENV( "CRACKDOWN_DEBUG_RESOLVE" ) ? 0x80000000u : 0u ),
            edram_base( cinfo ), pitch, msaa, src == 4 ? 0x100u | ( ( cinfo >> 16 ) & 1 ) : ( cinfo >> 16 ) & 0xF,
            ex0, ey0, dx, dy, rw, rh, st->scale8, ( copy >> 4 ) & 7 };
        static const bool per_pixel = RT_ENV( "CRACKDOWN_RESOLVE_PER_PIXEL" );  // the old way, for comparison
        bool blocked = !( info & 8 ) && !per_pixel;  // tiled destination
        vkCmdBindPipeline( g_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, blocked ? g_resolve_blocked_pipe : g_resolve_pipe );
        uint32_t dyn = 0;
        vkCmdBindDescriptorSets( g_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_surface_pl, 0, 1, &st->set, 1, &dyn );
        vkCmdPushConstants( g_cmd, g_surface_pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof( pc ), &pc );
        uint32_t timer = gpu_timer_begin( GT_RESOLVE );
        if ( blocked ) vkCmdDispatch( g_cmd, ( ( dx + rw + 31 ) >> 5 ) - ( dx >> 5 ), ( ( dy + rh + 31 ) >> 5 ) - ( dy >> 5 ), 1 );
        else vkCmdDispatch( g_cmd, ( rw + 7 ) / 8, ( rh + 7 ) / 8, 1 );
        barrier_all();
        gpu_timer_end( timer );
        {
            uint32_t fmt = ( info >> 7 ) & 63, bpp = fmt == 32 || fmt == 37 ? 8 : 4;
            uint32_t start = dest & 0x1FFFF000u, end = ( dest & 0x1FFFFFFFu ) + ( ( w + 31 ) & ~31u ) * ( ( h + 31 ) & ~31u ) * bpp;
            for ( auto it = g_resolve_ranges.lower_bound( start ); it != g_resolve_ranges.end() && it->first < end; ) it = g_resolve_ranges.erase( it );
            uint32_t &e = g_resolve_ranges[start];
            e = std::max( e, end );
        }
        if ( st->hi_set && w >= 640 )  // the same at full size
        {
            HiResolve hr{ dest, w, h, info, pitch, ex0, ey0, dx, dy, rw, rh };
            hires_resolve( hr, pc, st->hi_set, st->hi8, st->scale8 * 8 / st->hi8 );
            // DLSS (DlssFrame): the scene's picture, kept with its depth before the resolve clears it; the
            // last one before the post-processing is the one DLSS makes at the resolution.
            if ( upscaler_ready() && !g_df.done && src < 4 && st == g_df.scene_color && g_df.have_vp && rw >= 640 && ex0 == 0 && ey0 == 0 )
            {
                end_rendering();
                if ( dlss_snapshot( (uint32_t)rw, (uint32_t)rh ) ) g_df.pending = hr, g_df.pending_pc = pc;
            }
        }
    }
    if ( src <= 4 && w && h && dest && rw > 0 && rh > 0 )  // textures made from that memory are made again
    {
        uint32_t fmt = ( info >> 7 ) & 63, bpp = fmt == 32 || fmt == 37 ? 8 : 4, size = ( ( ( w + 31 ) & ~31u ) * ( ( h + 31 ) & ~31u ) * bpp + 4095 ) & ~4095u;
        memwatch_write( dest & 0x1FFFFFFF, size );
        if ( auto r = g_hires.find( dest & 0x1FFFF000u ); r != g_hires.end() ) r->second.since = memwatch_protect( dest & 0x1FFFFFFF, size );
    }
    // Clears of the same rectangle after the copy.
    auto clear = [&]( uint32_t cinfo_reg, bool depth, uint32_t lo, uint32_t hi ) {
        if ( rw <= 0 || rh <= 0 ) return;
        uint32_t ci = regs[cinfo_reg];
        if ( Target *t = target( edram_base( ci ), depth ? 0 : ( ci >> 16 ) & 0xF, pitch, msaa, depth ) ) clear_surface( t, ex0, ey0, ex0 + rw, ey0 + rh, lo, hi );
    };
    uint32_t cc = regs[reg::RB_COLOR_CLEAR], cclo = regs[reg::RB_COLOR_CLEAR_LO];
    uint32_t cfmt = ( regs[reg::RB_COLOR_INFO] >> 16 ) & 0xF;
    if ( ( copy >> 8 ) & 1 ) clear( reg::RB_COLOR_INFO, false, format_is_64bpp( cfmt ) ? cclo : cc, cc );
    if ( ( copy >> 9 ) & 1 ) clear( reg::RB_DEPTH_INFO, true, regs[reg::RB_DEPTH_CLEAR], 0 );
    // The copy reaches guest memory at the next flush (frame end), not before the CPU continues:
    // GPU readers are ordered by barriers, and CPU readers of resolve results are rare.
    static const bool sync = RT_ENV( "CRACKDOWN_SYNC_RESOLVES" );
    if ( sync ) renderer_finish(), begin();
    // CRACKDOWN_DUMP_RESOLVES=1 with CRACKDOWN_EDRAM_TRACE=N: every 32bpp resolve of frame N is written
    // to build/resolve-<n>.ppm as it lands (colour bytes as the front buffer; depth as grey).
    // CRACKDOWN_DUMP_RESOLVE_RAW=<dest hex>:<bytes hex> (debug): in a census frame, the guest memory at that
    // destination after each resolve into it, as build/rraw-<n>.bin.
    if ( static const char *rr = RT_ENV( "CRACKDOWN_DUMP_RESOLVE_RAW" ); rr && g_draw_census && ( dest & 0x1FFFF000u ) == ( strtoul( rr, nullptr, 16 ) & 0x1FFFF000u ) )
    {
        renderer_finish();
        begin();
        static int n;
        uint32_t bytes = strchr( rr, ':' ) ? strtoul( strchr( rr, ':' ) + 1, nullptr, 16 ) : 0x20000;
        char path[64];
        snprintf( path, sizeof( path ), "build/rraw-%03d.bin", n++ );
        if ( FILE *f = fopen( path, "wb" ) ) { fwrite( g_base + 0xA0000000 + ( dest & 0x1FFFFFFF ), 1, bytes, f ); fclose( f ); }
        rt_log( "edram: raw resolve %s: %08X %ux%u info %08X (before draw #%u)\n", path, dest, w, h, info, g_draw_census - 1 );
    }
    if ( g_edram_trace && RT_ENV( "CRACKDOWN_DUMP_RESOLVES" ) && w && h && w <= 4096 && h <= 4096 )
    {
        uint32_t fmt = ( info >> 7 ) & 63;
        if ( fmt == 6 || fmt == 22 || fmt == 23 )  // 8_8_8_8, 24_8, 24_8_FLOAT
        {
            renderer_finish();
            begin();
            static int n;
            std::vector<uint32_t> px( (size_t)w * h );
            const uint8_t *src = g_base + 0xA0000000 + ( dest & 0x1FFFFFFF );
            bool linear = info & 8;
            for ( uint32_t y = 0; y < h; ++y )
                for ( uint32_t x = 0; x < w; ++x )
                {
                    uint32_t v;
                    memcpy( &v, src + (size_t)( linear ? y * w + x : tiled_offset( x, y, w, 2 ) ) * 4, 4 );
                    v = __builtin_bswap32( v );
                    uint32_t c;
                    if ( fmt == 6 ) c = ( ( v >> 16 ) & 0xFF ) | ( ( ( v >> 8 ) & 0xFF ) << 8 ) | ( ( v & 0xFF ) << 16 );
                    else { uint32_t g = v >> 24; c = g | ( g << 8 ) | ( g << 16 ); }
                    px[(size_t)y * w + x] = c;
                }
            char path[64];
            snprintf( path, sizeof( path ), "build/resolve-%03d.ppm", n++ );
            write_ppm( path, px, w, h );
            rt_log( "edram: resolve dump %s: %08X %ux%u format %u\n", path, dest, w, h, fmt );
        }
    }
}

// D3D's Clear (native_d3d.cpp 827477F8, OP_CLEAR): what (bit 0 colour, 4 depth, 5 stencil), the
// rectangle, RB_SURFACE_INFO, the colour target's info and value (the console's words), depth, stencil.
// The depth / stencil surface is the one bound (RB_DEPTH_INFO).
void renderer_clear( const uint32_t *regs, const uint32_t *w )
{
    if ( !renderer_init() ) return;
    uint32_t what = w[0], surface = w[5], info = w[6], pitch = surface & 0x3FFF, msaa = std::min( ( surface >> 16 ) & 3, 2u );
    int32_t x0 = (int32_t)w[1], y0 = (int32_t)w[2], x1 = (int32_t)w[3], y1 = (int32_t)w[4];
    if ( !pitch ) return;
    begin();
    if ( what & 1 )
        if ( Target *t = target( edram_base( info ), ( info >> 16 ) & 0xF, pitch, msaa, false ) )
        {
            VkClearValue v{};
            v.color = color_clear_value( t->guest_format, w[7], w[8] );
            clear_surface( t, x0, y0, x1, y1, VK_IMAGE_ASPECT_COLOR_BIT, v );
        }
    if ( what & 0x30 )
        if ( Target *t = target( edram_base( regs[reg::RB_DEPTH_INFO] ), 0, pitch, msaa, true ) )
        {
            VkClearValue v{};
            float z;
            memcpy( &z, &w[9], 4 );
            v.depthStencil = { std::clamp( z, 0.0f, 1.0f ), w[10] };
            clear_surface( t, x0, y0, x1, y1, ( what & 0x10 ? VK_IMAGE_ASPECT_DEPTH_BIT : 0 ) | ( what & 0x20 ? VK_IMAGE_ASPECT_STENCIL_BIT : 0 ), v );
        }
}

// Submits the recorded work without waiting; what was deferred behind it runs once it completes.
void renderer_flush()
{
    if ( !g_ok || !g_recording ) return;
    end_rendering();
    if ( g_ts_pool ) gpu_timer_end( 0 );  // GT_ALL opened the command buffer
    vkEndCommandBuffer( g_cmd );
    Submission &sub = g_subs[g_cur];
    VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1;
    si.pCommandBuffers = &g_cmd;
    vkQueueSubmit( g_queue, 1, &si, sub.fence );
    g_rt_stats.flushes++;
    sub.busy = true;
    sub.serial = ++g_submit_serial;
    sub.dc_end = g_dc_head;
    sub.effects.swap( g_effects );
    g_effects.clear();
    sub.query_count = g_query_next;
    sub.zids.clear();
    for ( ZQuery &q : g_zqueries )
        if ( !q.ids.empty() ) { sub.zids.push_back( { q.serial, std::move( q.ids ) } ); q.ids.clear(); }
    sub.probes.swap( g_probes );
    g_probes.clear();
    sub.ts_count = g_ts_next;
    sub.ts_marks.swap( g_ts_marks );
    g_ts_marks.clear();
    g_recording = false;
    g_cur = ( g_cur + 1 ) % SUBMITS;
    static const bool sync = RT_ENV( "CRACKDOWN_SYNC_SUBMIT" );  // the old way: wait for every submission
    if ( sync ) complete( sub );
}

namespace
{
// Waits for a submitted command buffer (and any older one first), takes its query results and runs
// what was deferred behind it.
void complete( Submission &sub )
{
    for ( Submission &o : g_subs )
        if ( o.busy && o.serial < sub.serial ) complete( o );
    if ( !sub.busy ) return;
    auto t0 = std::chrono::steady_clock::now();
    vkWaitForFences( g_dev, 1, &sub.fence, VK_TRUE, UINT64_MAX );
    g_rt_stats.wait_ms += std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() - t0 ).count();
    vkResetFences( g_dev, 1, &sub.fence );
    sub.busy = false;
    if ( sub.timestamps && sub.ts_count )
    {
        static std::vector<uint64_t> ts( TS_POOL_SIZE );
        vkGetQueryPoolResults( g_dev, sub.timestamps, 0, sub.ts_count, sub.ts_count * 8, ts.data(), 8, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT );
        for ( auto &[timer, q] : sub.ts_marks )
        {
            double ms = (double)( ts[q + 1] - ts[q] ) * g_ts_period / 1e6;
            if ( timer >= GT_COUNT ) { g_pass_stats[timer - GT_COUNT].ms += ms; continue; }
            g_ts_ms[timer] += ms;
            g_ts_count[timer]++;
        }
    }
    // Occlusion query counts: add this command buffer's to their queries.
    if ( sub.query_count )
    {
        static std::vector<uint64_t> counts( QUERY_POOL_SIZE );
        VkResult qr = vkGetQueryPoolResults( g_dev, sub.queries, 0, sub.query_count, sub.query_count * 8, counts.data(), 8, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT );
        if ( RT_ENV( "CRACKDOWN_QUERY_ALL" ) )
        {
            uint64_t sum = 0, nonzero = 0;
            for ( uint32_t i = 0; i < sub.query_count; ++i ) sum += counts[i], nonzero += counts[i] != 0;
            static int k;
            if ( k++ % 50 == 0 ) rt_log( "renderer: %u queries, result %d, %llu nonzero, %llu samples, precise %d\n", sub.query_count, qr,
                                         (unsigned long long)nonzero, (unsigned long long)sum, (int)g_precise_queries );
        }
        for ( const ProbeQuery &p : sub.probes )
            if ( counts[p.query] )
                rt_log( "probe: #%u vs %016llx ps %016llx count %u, target %ux%u, blend %08X: %llu samples\n", p.draw, (unsigned long long)p.vs,
                        (unsigned long long)p.ps, p.count, p.w, p.h, p.blend, (unsigned long long)counts[p.query] );
        for ( auto &[serial, ids] : sub.zids )
            for ( ZQuery &q : g_zqueries )
                if ( q.serial == serial )
                    for ( uint32_t id : ids ) q.samples += (uint32_t)( (uint64_t)counts[id] * 64 / std::max<uint32_t>( (uint32_t)sub.query_scale8[id] * sub.query_scale8[id] * std::max<uint32_t>( sub.query_samples[id], 1 ), 1 ) );
    }
    sub.zids.clear();
    sub.probes.clear();
    std::vector<std::function<void()>> effects;
    effects.swap( sub.effects );
    for ( auto &f : effects ) f();
}

// An ended occlusion query: its count (all its command buffers have completed) to its record.
void write_zquery( uint64_t serial )
{
    for ( size_t i = 0; i < g_zqueries.size(); ++i )
    {
        ZQuery &q = g_zqueries[i];
        if ( q.serial != serial ) continue;
        uint32_t n = (uint32_t)std::min<uint64_t>( q.samples, 0x7FFFFFFF );
        if ( static const bool force = RT_ENV( "CRACKDOWN_QUERY_FORCE" ); force ) n = std::max( n, 2000u );  // debug: everything visible
        if ( static const bool log = RT_ENV( "CRACKDOWN_ZPD_LOG" ); log )
        {
            static int k;
            if ( k++ % 25 == 0 ) rt_log( "renderer: occlusion query %08X: %u samples from %u draws (vs %016llx ps %016llx prim %u count %u depth %08X surface %08X)\n", q.end_addr, n, q.draws,
                                       (unsigned long long)q.vs, (unsigned long long)q.ps, q.prim, q.vertices, q.depth_control, q.surface );
        }
        for ( int k = 0; k < 8; ++k ) guest_write32( 0xA0000000 + q.end_addr + k * 4, 0 );  // end block: counts since the begin
        guest_write32( 0xA0000000 + q.end_addr + 16, __builtin_bswap32( n ) );           // (stored little-endian)
        g_zqueries.erase( g_zqueries.begin() + i );
        return;
    }
}
} // namespace

// Runs `f` once the GPU work recorded so far has completed: after the command buffer being recorded,
// else after the newest one in flight, else now.
void renderer_defer( std::function<void()> f )
{
    if ( g_recording ) { g_effects.push_back( std::move( f ) ); return; }
    Submission *newest = nullptr;
    for ( Submission &sub : g_subs )
        if ( sub.busy && ( !newest || sub.serial > newest->serial ) ) newest = &sub;
    if ( newest ) newest->effects.push_back( std::move( f ) );
    else f();
}

// Finish what the GPU has completed; with `kick` (the command processor has been waiting a while)
// also submit recorded work that has deferred effects behind it. Returns whether work is in flight
// or waiting to be submitted.
bool renderer_poll( bool kick )
{
    if ( kick && g_recording && !g_effects.empty() ) renderer_flush();
    bool pending = false;
    for ( int n = 0; n < SUBMITS; ++n )
    {
        Submission *oldest = nullptr;
        for ( Submission &sub : g_subs )
            if ( sub.busy && ( !oldest || sub.serial < oldest->serial ) ) oldest = &sub;
        if ( !oldest ) break;
        if ( vkGetFenceStatus( g_dev, oldest->fence ) != VK_SUCCESS ) { pending = true; break; }
        complete( *oldest );
    }
    return pending || ( g_recording && !g_effects.empty() );
}

// Submits and waits for everything (debug readbacks).
void renderer_finish()
{
    renderer_flush();
    for ( Submission &sub : g_subs ) complete( sub );
}

// EVENT_WRITE_ZPD to physical address `addr`: the end of the query whose begin block is 32 bytes on
// (its record carries the FFFFFEED marks), or the beginning of a new one.
void renderer_zpd( uint32_t addr )
{
    for ( ZQuery &q : g_zqueries )
        if ( !q.end_addr && q.begin_addr == addr + 32 )
        {
            q.end_addr = addr;
            renderer_defer( [serial = q.serial] { write_zquery( serial ); } );
            return;
        }
    for ( int k = 0; k < 8; ++k ) guest_write32( 0xA0000000 + addr + k * 4, 0 );  // begin block: all counters 0
    if ( g_zqueries.size() < 1024 ) { g_zqueries.push_back( { addr } ); g_zqueries.back().serial = ++g_zquery_serial; }
}

void renderer_frame_end( uint64_t frame )
{
    g_frames_seen = frame;
    // CRACKDOWN_EDRAM_TRACE=N[,M]: frame N (and M).
    static const char *tr = RT_ENV( "CRACKDOWN_EDRAM_TRACE" );
    static const long trace = tr ? atol( tr ) : -1, trace2 = tr && strchr( tr, ',' ) ? atol( strchr( tr, ',' ) + 1 ) : -1;
    g_edram_trace = (long)frame + 1 == trace || (long)frame + 1 == trace2;
    static const long census = RT_ENV( "CRACKDOWN_DRAW_CENSUS" ) ? atol( RT_ENV( "CRACKDOWN_DRAW_CENSUS" ) ) : -1;  // frame N: log every draw
    g_draw_census = (long)frame + 1 == census || video_take_capture();
    // CRACKDOWN_FIND_U32=<hex>[,<hex>] (debug): at a capture, where guest memory holds those values
    // (the image, the heaps at 0x40000000-0x80000000, physical memory).
    if ( static const char *fu = RT_ENV( "CRACKDOWN_FIND_U32" ); fu && g_draw_census )
        for ( const char *p = fu; p && *p; p = strchr( p, ',' ) ? strchr( p, ',' ) + 1 : nullptr )
        {
            uint32_t want = __builtin_bswap32( (uint32_t)strtoul( p, nullptr, 16 ) ), n = 0;
            for ( auto [lo, hi] : { std::pair<uint64_t, uint64_t>{ 0x82000000, 0x83800000 }, { 0x40000000, 0x80000000 }, { 0xA0000000, 0xC0000000 } } )
                for ( uint64_t a = lo; a < hi; a += 4 )
                    if ( *(uint32_t *)( g_base + a ) == want && n++ < 40 ) rt_log( "find: %08X at %08llX\n", __builtin_bswap32( want ), (unsigned long long)a );
        }
    if ( g_edram_trace ) rt_log( "edram: --- frame %llu\n", (unsigned long long)frame + 1 );
    // DLSS (DlssFrame): this frame's camera is the next one's last, and the next jitter (Halton 2, 3; 16 of them).
    if ( upscaler_ready() )
    {
        if ( g_df.have_vp ) memcpy( g_df.prev_vp, g_df.vp, sizeof( g_df.vp ) );
        g_df.prev_ok = g_df.have_vp;
        g_df.ran_before = g_df.ran;
        g_df.have_vp = g_df.snapped = g_df.done = g_df.ran = false;
        if ( g_mv.on )
        {
            if ( static const bool log = RT_ENV( "CRACKDOWN_DLSS_LOG" ); log && frame % 300 == 0 )
                rt_log( "dlss: %zu moving-thing draws, %u vertex places\n", g_mv.now.size(), g_mv.next );
            std::swap( g_mv.prev, g_mv.now );
            g_mv.now.clear(), g_mv.seen.clear();
            g_mv.half ^= 1, g_mv.next = 0, g_mv.cleared = false, g_mv.cam = false;
        }
        auto halton = []( uint32_t i, uint32_t b ) { float f = 1, r = 0; for ( ; i; i /= b ) f /= b, r += f * ( i % b ); return r; };
        g_df.index = g_df.index % 16 + 1;
        static const bool no_jitter = RT_ENV( "CRACKDOWN_DLSS_NO_JITTER" );  // debug
        g_df.jx = no_jitter ? 0 : halton( g_df.index, 2 ) - 0.5f, g_df.jy = no_jitter ? 0 : halton( g_df.index, 3 ) - 0.5f;
        if ( static const bool log = RT_ENV( "CRACKDOWN_DLSS_LOG" ); log && frame % 300 == 0 )
            rt_log( "dlss: frame %llu ran %d (camera %d)\n", (unsigned long long)frame, (int)g_df.ran_before, (int)g_df.prev_ok );
    }
}

void renderer_print_stats()
{
    rt_log( "renderer: surfaces: %u copies (%.1f Mpx), %u clears, %zu surfaces; %u submits, %.1f ms waiting; %u Vulkan draws\n", g_rt_stats.copies,
            g_rt_stats.copy_px / 1e6, g_rt_stats.clears, g_targets.size(), g_rt_stats.flushes, g_rt_stats.wait_ms, g_rt_stats.vk_draws );
    if ( static const char *pv = RT_ENV( "CRACKDOWN_GPU_PASSES" ); pv && !g_pass_stats.empty() )
    {
        std::vector<PassStats *> order;
        double sum = 0;
        for ( PassStats &p : g_pass_stats ) order.push_back( &p ), sum += p.ms;
        std::sort( order.begin(), order.end(), []( PassStats *a, PassStats *b ) { return a->ms > b->ms; } );
        rt_log( "renderer: passes: %.1f ms in all\n", sum );
        for ( int i = 0; i < atoi( pv ) && i < (int)order.size() && order[i]->ms > 0; ++i )
            rt_log( "renderer:   %7.1f ms %5u passes %6u draws %7u vk  %s\n", order[i]->ms, order[i]->passes, order[i]->draws, order[i]->vk_draws,
                    order[i]->name.c_str() );
        for ( PassStats &p : g_pass_stats ) p.ms = 0, p.passes = p.draws = p.vk_draws = 0;
    }
    rt_log( "renderer: textures: %zu (%.0f MB), %u made, %u made again; %zu sampler groups; %u page writes caught\n", g_textures.size(),
            g_tex_stats.bytes / 1048576.0, g_tex_stats.made, g_tex_stats.remade, g_sampler_groups.size(), memwatch_faults() );
    g_tex_stats.made = g_tex_stats.remade = 0;
    g_rt_stats = {};
    if ( g_ts_pool )
    {
        static const char *names[GT_COUNT] = { "-", "surface copies", "resolves", "clears", "all" };
        std::string t;
        for ( int i = 0; i < GT_COUNT; ++i ) { char b[64]; snprintf( b, sizeof( b ), " %s %.1f ms (%u)", names[i], g_ts_ms[i], g_ts_count[i] ); t += b; }
        rt_log( "renderer: GPU time:%s\n", t.c_str() );
        memset( g_ts_ms, 0, sizeof( g_ts_ms ) );
        memset( g_ts_count, 0, sizeof( g_ts_count ) );
    }
    std::lock_guard<std::mutex> g( g_compile_stats_lock );
    if ( g_compile_stats.compiled || g_compile_stats.cached || g_draws_skipped )
        rt_log( "renderer: shaders %u compiled (%.0f ms), %u from cache; %u pipelines pending, %u draws skipped\n", g_compile_stats.compiled,
                g_compile_stats.ms, g_compile_stats.cached, g_jobs_pending.load(), g_draws_skipped.exchange( 0 ) );
    g_compile_stats = {};
}

bool renderer_tracing() { return g_edram_trace; }
