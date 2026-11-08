// Native operations for the renderer thread (gpu.cpp run_ops): what the native D3D device
// (native_d3d.cpp) does in place of writing GPU commands, and what commands still written decode to.
// An operation is a word kind << 24 | payload words, then the payload.
#pragma once
#include <cstdint>
#include <memory>
#include <vector>

enum : uint32_t
{
    OP_REGS = 1,   // first register, values
    OP_REGS_SAME,  // register, values written to it in turn
    OP_SHADER,     // shader (pointer, two words)
    OP_TYPE3,      // a command's header, address, words
    OP_RPTR,       // ring read pointer
    OP_DRAW,       // VGT_DRAW_INITIATOR, index base, index size
    OP_BIN,        // which (0 mask low, 1 mask high, 2 select low, 3 select high), value
    OP_PRED,       // the next n words of operations run only where the bin mask and select meet
    OP_RMW,        // register, AND mask, OR mask
    OP_FENCE,      // address (GPU), value: written when the GPU work before it is done (D3D's fences)
    OP_WAIT,       // info, address or register, reference, mask: the GPU waits until they compare
    OP_INTERRUPT,  // mask of the hardware threads to interrupt (the game's GPU callback)
    OP_PRESENT,    // the front buffer's texture fetch constant (6 words): present it once the frame is drawn
    OP_ZPD,        // occlusion query sample counts to RB_SAMPLE_COUNT_ADDR (0x2325)
    OP_EXTENTS,    // screen extents of the draws since the last, to an address (the whole screen)
    OP_CLEAR,      // what (bits 0 colour, 4 depth, 5 stencil), x0, y0, x1, y1, RB_SURFACE_INFO, colour info,
                   // colour value (two words, as the console keeps the format), depth (float), stencil
    // Only in a command buffer's cached translation, resolved each time it is used (they read memory
    // outside the buffer): constants loaded from memory (first register, address, count) and a shader
    // loaded from memory (type, address, dwords).
    OP_LATE_REGS,
    OP_LATE_SHADER,
};


inline void op_begin( std::vector<uint32_t> &o, uint32_t kind, uint32_t words ) { o.push_back( kind << 24 | words ); }

// A predicated draw (D3D predicates its draws on the tile bins, which the game also uses to keep
// objects out of passes they're not in).
inline void op_draw_predicated( std::vector<uint32_t> &o, uint32_t initiator, uint32_t index_base, uint32_t index_size )
{
    op_begin( o, OP_PRED, 1 ), o.push_back( 4 );
    op_begin( o, OP_DRAW, 3 ), o.push_back( initiator ), o.push_back( index_base ), o.push_back( index_size );
}

// Registers base.. from n guest words at src (device shadow state).
void op_regs_from( std::vector<uint32_t> &o, uint32_t base, uint32_t src, uint32_t n );
inline void op_reg( std::vector<uint32_t> &o, uint32_t reg, uint32_t value )
{
    op_begin( o, OP_REGS, 2 ), o.push_back( reg ), o.push_back( value );
}

// Operations the device records go into batches, in the order of its command stream: where a batch
// starts the device leaves a marker in the stream (a NOP carrying the batch's number), so commands D3D
// still writes there run before or after it as they would. A batch is open for more operations until
// the stream moves past its marker or the batch is taken.
struct NativeBatch
{
    std::vector<uint32_t> ops;
    uint32_t seq = 0;
    bool taken = false;
};
// The operations of a command buffer in guest memory (the game's precompiled ones), translated once
// and kept while its contents stay the same.
void gpu_run_buffer( std::vector<uint32_t> &o, uint32_t addr, uint32_t dwords );

// The native device's submissions in D3D's order, in place of its ring: a command segment (GPU
// address, dwords; first what D3D wrote in the ring itself, up to its position there) or operations.
void gpu_submit_segment( uint32_t gpu_addr, uint32_t dwords, uint32_t ring_position );
void gpu_submit_ops( const std::vector<uint32_t> &ops );

// Whether the native device sends operations (CRACKDOWN_NATIVE_D3D=1, not CRACKDOWN_D3D_COMMANDS=1).
bool native_d3d_ops();

constexpr uint32_t NATIVE_MARKER = 0x4E415456;  // 'NATV'
// A marker whose batch is drawn with a bin mask D3D fills in later (predicated tiling: an object's
// tiles, patched into its slot once its screen extents are known): NOP { mask, NATIVE_MARKER_MASK, seq }.
constexpr uint32_t NATIVE_MARKER_MASK = 0x4E41544D;  // 'NATM'
// Where only two words fit (D3D's writes straight into the ring): NOP { 0x4E000000 | seq's low 24 bits }.
constexpr uint32_t NATIVE_MARKER_SHORT = 0x4E000000;
std::shared_ptr<NativeBatch> gpu_batch_new();     // queued; the caller writes its marker
// Adds operations to a batch if it is still open (not taken by the decoder); returns whether it was.
bool gpu_batch_add( NativeBatch &b, const std::vector<uint32_t> &ops );
