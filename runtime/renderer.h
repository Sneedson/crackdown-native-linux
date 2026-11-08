// Vulkan renderer for Xenos draws (stage 2). Runs on the GPU command-processor thread.
#pragma once
#include <functional>
#include "ucode.h"

#include <cstdint>

struct RendererShader
{
    uint64_t hash;
    const ucode::Program *program;
};

// Register indices (dwords) the renderer reads.
namespace reg
{
constexpr uint32_t RB_SURFACE_INFO = 0x2000, RB_COLOR_INFO = 0x2001, RB_DEPTH_INFO = 0x2002;
constexpr uint32_t RB_COLOR1_INFO = 0x2003, RB_COLOR2_INFO = 0x2004, RB_COLOR3_INFO = 0x2005;
constexpr uint32_t PA_SC_WINDOW_OFFSET = 0x2080, PA_SC_WINDOW_SCISSOR_TL = 0x2081, PA_SC_WINDOW_SCISSOR_BR = 0x2082;
constexpr uint32_t VGT_MAX_VTX_INDX = 0x2100, VGT_MIN_VTX_INDX = 0x2101, VGT_INDX_OFFSET = 0x2102, VGT_MULTI_PRIM_IB_RESET_INDX = 0x2103;
constexpr uint32_t RB_COLOR_MASK = 0x2104, RB_BLEND_RED = 0x2105, RB_STENCILREFMASK_BF = 0x210C, RB_STENCILREFMASK = 0x210D, RB_ALPHA_REF = 0x210E;
constexpr uint32_t PA_CL_VPORT_XSCALE = 0x210F;  // .. ZOFFSET 0x2114
constexpr uint32_t RB_DEPTHCONTROL = 0x2200, RB_BLENDCONTROL0 = 0x2201, RB_COLORCONTROL = 0x2202;
constexpr uint32_t PA_CL_CLIP_CNTL = 0x2204, PA_SU_SC_MODE_CNTL = 0x2205, PA_CL_VTE_CNTL = 0x2206;
constexpr uint32_t RB_MODECONTROL = 0x2208, RB_BLENDCONTROL1 = 0x2209;
constexpr uint32_t RB_COPY_CONTROL = 0x2318, RB_COPY_DEST_BASE = 0x2319, RB_COPY_DEST_PITCH = 0x231A, RB_COPY_DEST_INFO = 0x231B;
constexpr uint32_t RB_DEPTH_CLEAR = 0x231D, RB_COLOR_CLEAR = 0x231E, RB_COLOR_CLEAR_LO = 0x231F;
constexpr uint32_t SHADER_CONSTANT_000_X = 0x4000, SHADER_CONSTANT_FETCH_00_0 = 0x4800, SHADER_CONSTANT_BOOL_000_031 = 0x4900,
                   SHADER_CONSTANT_LOOP_00 = 0x4908;
} // namespace reg

bool renderer_init();
void renderer_draw( const uint32_t *regs, uint32_t initiator, uint32_t index_base, uint32_t index_info, RendererShader vs, RendererShader ps );
void renderer_resolve( const uint32_t *regs, const ucode::Program *vs );
void renderer_clear( const uint32_t *regs, const uint32_t *op );  // OP_CLEAR (gpu_ops.h)
void renderer_flush();   // submit the recorded work (no wait)
void renderer_finish();  // submit and wait for all of it (debug readbacks)
// Run `f` once the GPU work recorded so far has completed (command-processor thread only).
void renderer_defer( std::function<void()> f );
bool renderer_poll( bool kick );  // finish completed work (kick: also submit work with deferred effects); true while any is pending
void renderer_print_stats();
bool renderer_tracing();  // inside the CRACKDOWN_EDRAM_TRACE frame
void renderer_frame_end( uint64_t frame );  // frames completed so far  // EDRAM traffic since the last call  // submit recorded work and wait for it (guest memory then holds the results)
