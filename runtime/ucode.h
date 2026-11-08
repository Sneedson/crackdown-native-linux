// Xenos (Xbox 360 GPU) shader microcode: decoding of control flow, ALU and fetch instructions.
// Shared by the disassembler (devtools/xenos_dis) and the shader translator.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace ucode
{

// ------------------------------------------------------------------ control flow (48-bit words)
enum class CfOp : uint32_t
{
    Nop = 0, Exec = 1, ExecEnd = 2, CondExec = 3, CondExecEnd = 4, CondPredExec = 5, CondPredExecEnd = 6,
    LoopStart = 7, LoopEnd = 8, CondCall = 9, Return = 10, CondJmp = 11, Alloc = 12,
    CondExecPredClean = 13, CondExecPredCleanEnd = 14, MarkVsFetchDone = 15,
};

struct Cf
{
    uint64_t raw;
    CfOp op() const { return CfOp( ( raw >> 44 ) & 0xF ); }
    bool is_exec() const { auto o = op(); return o == CfOp::Exec || o == CfOp::ExecEnd || ( o >= CfOp::CondExec && o <= CfOp::CondPredExecEnd ) || o == CfOp::CondExecPredClean || o == CfOp::CondExecPredCleanEnd; }
    bool is_end() const { auto o = op(); return o == CfOp::ExecEnd || o == CfOp::CondExecEnd || o == CfOp::CondPredExecEnd || o == CfOp::CondExecPredCleanEnd; }
    // exec clauses
    uint32_t address() const { return raw & 0xFFF; }          // ALU/fetch slot (3 dwords each)
    uint32_t count() const { return ( raw >> 12 ) & 7; }
    bool yield() const { return ( raw >> 15 ) & 1; }
    uint32_t sequence() const { return ( raw >> 16 ) & 0xFFF; }  // 2 bits per instruction: bit0 fetch, bit1 serialize
    uint32_t bool_address() const { return ( raw >> 34 ) & 0xFF; }
    bool condition() const { return ( raw >> 42 ) & 1; }
    // jumps / calls / loops
    uint32_t target() const { return raw & 0x1FFF; }
    bool unconditional() const { return ( raw >> 13 ) & 1; }   // force_call / is_unconditional
    bool predicated_jmp() const { return ( raw >> 14 ) & 1; }
    uint32_t loop_id() const { return ( raw >> 16 ) & 0x1F; }
    // alloc
    uint32_t alloc_size() const { return raw & 0xF; }
    uint32_t alloc_type() const { return ( raw >> 41 ) & 3; }  // 1 position, 2 interpolators/colors, 3 memory
};

// ------------------------------------------------------------------ ALU (96-bit)
struct Alu
{
    uint32_t d[3];
    uint32_t vector_dest() const { return d[0] & 0x3F; }
    bool vector_dest_rel() const { return ( d[0] >> 6 ) & 1; }
    bool abs_constants() const { return ( d[0] >> 7 ) & 1; }
    uint32_t scalar_dest() const { return ( d[0] >> 8 ) & 0x3F; }
    bool scalar_dest_rel() const { return ( d[0] >> 14 ) & 1; }
    bool export_data() const { return ( d[0] >> 15 ) & 1; }
    uint32_t vector_mask() const { return ( d[0] >> 16 ) & 0xF; }
    uint32_t scalar_mask() const { return ( d[0] >> 20 ) & 0xF; }
    bool vector_clamp() const { return ( d[0] >> 24 ) & 1; }
    bool scalar_clamp() const { return ( d[0] >> 25 ) & 1; }
    uint32_t scalar_op() const { return ( d[0] >> 26 ) & 0x3F; }
    uint32_t swizzle( int src ) const { return ( d[1] >> ( 16 - ( src - 1 ) * 8 ) ) & 0xFF; }  // src 1..3
    bool negate( int src ) const { return ( d[1] >> ( 26 - ( src - 1 ) ) ) & 1; }
    bool pred_condition() const { return ( d[1] >> 27 ) & 1; }
    bool predicated() const { return ( d[1] >> 28 ) & 1; }
    bool address_absolute() const { return ( d[1] >> 29 ) & 1; }
    bool const1_rel() const { return ( d[1] >> 30 ) & 1; }
    bool const0_rel() const { return ( d[1] >> 31 ) & 1; }
    uint32_t reg( int src ) const { return ( d[2] >> ( 16 - ( src - 1 ) * 8 ) ) & 0xFF; }
    uint32_t vector_op() const { return ( d[2] >> 24 ) & 0x1F; }
    bool is_temp( int src ) const { return ( d[2] >> ( 31 - ( src - 1 ) ) ) & 1; }  // sel: 1 temp register, 0 constant
};

// ------------------------------------------------------------------ fetch (96-bit)
struct Fetch
{
    uint32_t d[3];
    uint32_t op() const { return d[0] & 0x1F; }  // 0 vertex, 1 texture, 16.. texture helpers
    uint32_t src_reg() const { return ( d[0] >> 5 ) & 0x3F; }
    uint32_t dst_reg() const { return ( d[0] >> 12 ) & 0x3F; }
    uint32_t const_index() const { return ( d[0] >> 20 ) & 0x1F; }
    // vertex fetch
    uint32_t vtx_const_sel() const { return ( d[0] >> 25 ) & 3; }   // which dword pair of the 6-dword fetch constant
    uint32_t vtx_src_swizzle() const { return ( d[0] >> 30 ) & 3; }
    uint32_t dst_swizzle() const { return d[1] & 0xFFF; }            // 3 bits per component: 0-3 xyzw, 4 zero, 5 one, 7 keep
    bool vtx_format_signed() const { return ( d[1] >> 12 ) & 1; }    // format_comp_all
    bool vtx_num_format_int() const { return ( d[1] >> 13 ) & 1; }   // 0 fraction (normalised), 1 integer
    uint32_t vtx_data_format() const { return ( d[1] >> 16 ) & 0x3F; }
    int32_t vtx_exp_adjust() const { return ( (int32_t)( d[1] << 3 ) ) >> 26; }  // bits 23-28
    bool vtx_mini_fetch() const { return ( d[1] >> 30 ) & 1; }
    uint32_t vtx_stride() const { return d[2] & 0xFF; }              // dwords
    int32_t vtx_offset() const { return ( (int32_t)( d[2] << 1 ) ) >> 9; }  // dwords
    // texture fetch
    uint32_t tex_src_swizzle() const { return ( d[0] >> 26 ) & 0x3F; }
    bool tex_unnormalized() const { return ( d[0] >> 25 ) & 1; }
    uint32_t tex_filters() const { return ( d[1] >> 12 ) & 0x3F; }  // mag | min << 2 | mip << 4; 3 = fetch constant's
    bool tex_computed_lod() const { return ( d[1] >> 28 ) & 1; }
    bool tex_register_lod() const { return ( d[1] >> 29 ) & 1; }   // level from SET_TEX_LOD
    float tex_offset_x() const { return (float)( ( (int32_t)( d[2] << 11 ) ) >> 27 ) / 2.0f; }  // bits 16-20, half texels
    float tex_offset_y() const { return (float)( ( (int32_t)( d[2] << 6 ) ) >> 27 ) / 2.0f; }   // bits 21-25
    float tex_lod_bias() const { return (float)( ( (int32_t)( d[2] << 23 ) ) >> 25 ) / 16.0f; }  // bits 2-8
    uint32_t tex_dimension() const { return ( d[2] >> 14 ) & 3; }   // 0 1D, 1 2D, 2 3D, 3 cube
    bool predicated() const { return ( d[1] >> 31 ) & 1; }
    bool pred_condition() const { return ( d[2] >> 31 ) & 1; }
};

// A shader: control flow plus the instruction slots it references.
struct Program
{
    uint32_t type = 0;  // 0 vertex, 1 pixel
    std::vector<uint32_t> words;
    std::vector<Cf> cf;  // up to and including the first end
    void parse();
    uint32_t slot_word( uint32_t slot, int i ) const { return slot * 3 + i < words.size() ? words[slot * 3 + i] : 0; }
};

std::string disassemble( const Program &p );
// Per word of a vertex shader: the bits of its vertex fetches that D3D rewrites when it binds the shader
// to a vertex declaration (fetch constant, format, swizzle, stride, offset, ...); 0 elsewhere.
std::vector<uint32_t> binding_mask( const Program &p );
// Whether a shader program starts at w (up to `avail` dwords there): control flow that reaches an end,
// exec clauses whose instructions lie after the control flow, decodable instructions. Its length in
// dwords (to the last instruction slot used) goes to `dwords`. For finding programs in game files.
bool plausible_program( const uint32_t *w, size_t avail, uint32_t &dwords );

// Whether a parsed program could be a shader of its stage (type): only real opcodes and export registers.
// plausible_program also passes data that merely decodes (the executable's code): this tells most of it apart.
bool valid_for_stage( const Program &p );
// Whether it writes memory (an `alloc memory`, memexport): the translator doesn't implement that.
bool uses_memory_export( const Program &p );

// D3D rewrites a vertex shader when it binds it: to the vertex declaration (each vertex fetch's fields,
// the order of a declaration's fetches) and to the pixel shader it draws with (interpolator exports moved
// to the registers that shader reads, those it doesn't read replaced by a no-op). The translator makes
// all of that specialization constants, so the shader as stored in the game files (its template)
// translates once for every binding.
//   fetch at slot s: ids s * 8 + 0..7 = fetch constant, format, signed / integer flags, exponent adjust,
//                    stride, offset, destination swizzle, destination register;
//   export at slot s: ids kExportSpecBase + s * 4 + 0..2 = register, vector mask, scalar mask.
constexpr uint32_t kExportSpecBase = 0x8000;
// An interpolator export (o0-o15) of a vertex shader: the kind D3D relinks.
bool linked_export( const Alu &a );
// Whether `bound` is `tmpl` as D3D binds it (both vertex shaders).
bool binds_to( const Program &tmpl, const Program &bound );
// What binding doesn't change: the control flow. A bound shader's template has the same key.
uint64_t template_key( const Program &p );
// The specialization constants of template `tmpl` bound as `bound` (binds_to; or a shader translated as
// its own template).
std::vector<std::pair<uint32_t, uint32_t>> vs_specialization( const Program &tmpl, const Program &bound );
const char *vector_op_name( uint32_t op );
const char *scalar_op_name( uint32_t op );

} // namespace ucode
