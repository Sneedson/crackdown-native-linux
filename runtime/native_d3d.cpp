// The native Direct3D device (the native D3D setting, or CRACKDOWN_NATIVE_D3D=1), replacing the game's statically linked Xbox 360
// D3D a part at a time.
//
// Tiling. The console's 10 MB of EDRAM can't hold the game's 1280x720 multisampled colour and depth, so
// D3D renders a frame in screen tiles (predicated tiling): the scene's command buffer is replayed once per
// tile, each tile resolved out. The renderer here draws into host images of any size, so the native
// device renders the frame once: BeginTiling gets one tile, the union of the game's. EDRAM is virtual
// and spaced out (renderer.cpp edram_scale) so a surface placed for a tile's height doesn't run into
// the next one at full height. The game's precompiled command buffers, recorded for its tiles, are
// adjusted as they're decoded (gpu.cpp regs).
//
// Submission. D3D's commands are decoded into native operations by the thread that submits them
// (gpu.cpp native_submit) and run by the renderer thread; no command processor reads them later.
#include "ppc_config.h"
#include "ppc_context.h"
#include "runtime.h"
#include "settings.h"

#include <immintrin.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>

bool native_d3d() { static const bool on = settings().native_d3d; return on; }  // (from start-up)

// The game's own tile layout for 1280x720: two tiles, (0,0)-(1280,484) and (0,480)-(1280,720), in a
// static pair of rects the game keeps several copies of (one per part of its renderer that includes
// it). It sizes screen surfaces from them -- its deferred-lighting buffers are 1280x484 -- so drawn as
// one tile, everything below row 484 went missing. Every copy becomes two full-screen rects before the
// game starts: it sizes its surfaces for the whole screen, and BeginTiling below makes them one tile.
namespace
{
PPCContext *regs( uintptr_t ctx ) { return (PPCContext *)( ctx - 0x80 ); }

void one_tile_layouts()
{
    static const uint32_t two[8] = { 0, 0, 1280, 484, 0, 480, 1280, 720 };
    int n = 0;
    for ( uint32_t a = PPC_IMAGE_BASE; a + 32 <= PPC_IMAGE_BASE + PPC_IMAGE_SIZE; a += 4 )
    {
        if ( guest_read32( a ) != two[0] || guest_read32( a + 12 ) != two[3] ) continue;
        bool match = true;
        for ( int i = 0; i < 8 && match; ++i ) match = guest_read32( a + i * 4 ) == two[i];
        if ( !match ) continue;
        guest_write32( a + 12, 720 ), guest_write32( a + 20, 0 );
        ++n;
    }
    rt_log( "native d3d: %d copies of the 1280x720 tile layout made one full-screen tile\n", n );
}
} // namespace

// At start-up, before the game reads its tile layouts.
void native_d3d_install()
{
    if ( native_d3d() ) one_tile_layouts();
}

// ---------------------------------------------------------------------------------------------------
// Native operations. D3D keeps the device's state in shadow copies (registers, constants, fetch
// constants) with dirty masks, and at a draw writes the dirty parts out as GPU commands, then the draw
// command. The native device sends the renderer the state and the draw itself (gpu_ops.h batches); its
// command stream keeps only a marker where each batch goes, so whatever D3D still writes there stays
// in order with it.
//
// Device (D3D's one device, *(*0x82000798); its heap address varies from run to run): +48 command pointer (at the last word written), +52 end, +56 the limit past which
// D3D makes room (827447A0), +10760 the owning thread's id, +10812 flags (0x80: recording a command
// buffer to replay, 0x02: tracking visibility queries).
#include "gpu_ops.h"
#include <deque>
#include <memory>
#include <thread>
#include <atomic>
#include <vector>

PPC_EXTERN_FUNC( sub_827447A0 );
PPC_EXTERN_FUNC( sub_827501B8 );
PPC_EXTERN_FUNC( sub_8274ECD8 );
PPC_EXTERN_FUNC( sub_82738708 );
PPC_EXTERN_FUNC( sub_82743858 );
PPC_EXTERN_FUNC( sub_82743CD0 );
PPC_FUNC( sub_82749960 );
PPC_FUNC( sub_82744960 );
PPC_FUNC( sub_82751820 );
PPC_FUNC( sub_82745900 );
PPC_FUNC( sub_82745AA0 );
PPC_EXTERN_FUNC( sub_82754968 );
PPC_EXTERN_FUNC( sub_82757570 );
PPC_EXTERN_FUNC( sub_827389E8 );
PPC_EXTERN_FUNC( sub_827483D8 );
PPC_EXTERN_FUNC( sub_8273AEF8 );
PPC_EXTERN_FUNC( sub_8273AFF8 );
PPC_EXTERN_FUNC( sub_827569A8 );
PPC_EXTERN_FUNC( sub_827437F0 );
PPC_EXTERN_FUNC( sub_82754B38 );
PPC_EXTERN_FUNC( sub_82756A98 );
PPC_EXTERN_FUNC( sub_82743E68 );
PPC_EXTERN_FUNC( sub_82745278 );
PPC_EXTERN_FUNC( sub_827451B8 );
PPC_EXTERN_FUNC( sub_82751900 );
PPC_EXTERN_FUNC( __import__VdGetSystemCommandBuffer );
PPC_EXTERN_FUNC( __import__VdSwap );
PPC_EXTERN_FUNC( __import__VdSetDisplayMode );
PPC_EXTERN_FUNC( __import__VdGetCurrentDisplayInformation );
PPC_EXTERN_FUNC( __import__VdPersistDisplay );
PPC_EXTERN_FUNC( __import__MmFreePhysicalMemory );
extern "C" unsigned long long zz_mftb( void );
PPC_EXTERN_FUNC( sub_82745B40 );
PPC_EXTERN_FUNC( sub_82744028 );
PPC_EXTERN_FUNC( sub_82744888 );
PPC_EXTERN_FUNC( sub_8274E1E0 );
PPC_EXTERN_FUNC( sub_8274FD10 );
PPC_EXTERN_FUNC( sub_8274F580 );
PPC_EXTERN_FUNC( sub_8274EAF0 );
PPC_EXTERN_FUNC( sub_82750070 );
PPC_EXTERN_FUNC( sub_827370E8 );
PPC_EXTERN_FUNC( sub_827485A8 );
PPC_EXTERN_FUNC( sub_82748A80 );
PPC_EXTERN_FUNC( sub_8274C7D8 );
PPC_EXTERN_FUNC( sub_827477F8 );
PPC_EXTERN_FUNC( sub_827475A8 );
PPC_EXTERN_FUNC( sub_827486F0 );
PPC_FUNC( sub_8274FFC0 );
PPC_FUNC( sub_8274FE50 );
const void *xenos_shader_find( uint32_t type, uint32_t addr, uint32_t dwords );
bool gpu_native_submission();

namespace
{
// The device: D3D keeps a pointer to it at *0x82000798.
uint32_t device() { return guest_read32( guest_read32( 0x82000798 ) ); }

bool native_ops() { static const bool on = gpu_native_submission() && !RT_ENV( "CRACKDOWN_D3D_COMMANDS" ); return on; }

uint32_t guest_read8( uint32_t a ) { return g_base[a]; }

// Whether operations can go native now: the device's own thread writing its own command stream.
bool can_record( PPCContext &c, uint32_t dev )
{
    if ( !native_ops() || !dev || dev != device() || ( guest_read8( dev + 10812 ) & 0x80 ) ) return false;
    uint32_t kthread = guest_read32( c.r13.u32 + 256 );
    return kthread && guest_read32( kthread + 332 ) == guest_read32( dev + 10760 );
}

// Operation buffers, reused: one per depth of native functions calling native functions.
thread_local std::deque<std::vector<uint32_t>> t_ops;  // (a deque: growing keeps the others in place)
thread_local size_t t_ops_depth;
struct Ops
{
    std::vector<uint32_t> &v;
    Ops() : v( ( t_ops.size() <= t_ops_depth ? t_ops.emplace_back() : t_ops[t_ops_depth] ) ) { ++t_ops_depth, v.clear(); }
    ~Ops() { --t_ops_depth; }
};

thread_local std::shared_ptr<NativeBatch> t_batch;
thread_local uint32_t t_batch_end;  // the command pointer just past the batch's marker

// Sends operations: into the open batch if nothing went into the stream since its marker, else into a
// new batch with its marker.
void record( uintptr_t ctx, uintptr_t base, uintptr_t frame, uint32_t dev, const std::vector<uint32_t> &ops )
{
    if ( ops.empty() ) return;
    if ( t_batch && guest_read32( dev + 48 ) == t_batch_end && gpu_batch_add( *t_batch, ops ) ) return;
    PPCContext &c = *regs( ctx );
    uint32_t p = guest_read32( dev + 48 );
    if ( p > guest_read32( dev + 56 ) )
    {
        uint64_t r3 = c.r3.u64;
        c.r3.u64 = dev;
        sub_827447A0( ctx, base, frame );
        p = c.r3.u32;
        c.r3.u64 = r3;
    }
    t_batch = gpu_batch_new();
    gpu_batch_add( *t_batch, ops );
    guest_write32( p + 4, 0xC0011000 );  // type-3 NOP, 2 words
    guest_write32( p + 8, NATIVE_MARKER );
    guest_write32( p + 12, t_batch->seq );
    guest_write32( dev + 48, p + 12 );
    t_batch_end = p + 12;
}

// Operations for commands a helper would write at a pointer its caller hands it (and gets back past
// what was written): a new batch with its marker there; the pointer past the marker comes back.
uint32_t record_at( uint32_t dev, uint32_t p, const std::vector<uint32_t> &ops )
{
    t_batch = gpu_batch_new();
    gpu_batch_add( *t_batch, ops );
    guest_write32( p + 4, 0xC0011000 );
    guest_write32( p + 8, NATIVE_MARKER );
    guest_write32( p + 12, t_batch->seq );
    t_batch_end = p + 12;
    return p + 12;
}

// A batch drawn under predicated tiling (+10812 bit 0x02): its marker carries the bin mask slot D3D
// patches with the object's tiles (82756DA0 writes entry + 8 for each entry of the list at +12960, from
// the screen extents the GPU wrote at entry + 4). The extents are the whole screen: drawn as one tile,
// every object is in it.
void record_masked( uintptr_t ctx, uintptr_t base, uintptr_t frame, uint32_t dev, const std::vector<uint32_t> &ops )
{
    PPCContext &c = *regs( ctx );
    uint32_t p = guest_read32( dev + 48 );
    if ( p > guest_read32( dev + 56 ) )
    {
        c.r3.u64 = dev;
        sub_827447A0( ctx, base, frame );
        p = c.r3.u32;
    }
    uint32_t list = dev + 12960, entry = guest_read32( list + 8 );
    if ( entry + 16 > guest_read32( list + 12 ) )
    {
        c.r3.u64 = list;
        sub_82743858( ctx, base, frame );
        entry = c.r3.u32;
    }
    guest_write32( dev + 12968, entry + 16 );
    guest_write32( entry, p );
    static const uint16_t screen[4] = { 0, 8191 >> 3, 0, 8191 >> 3 };
    for ( int i = 0; i < 4; ++i ) guest_write16( entry + 4 + i * 2, screen[i] );
    t_batch = gpu_batch_new();
    gpu_batch_add( *t_batch, ops );
    guest_write32( p + 4, 0xC0021000 );  // type-3 NOP, 3 words
    guest_write32( p + 8, ~0u );         // the slot
    guest_write32( p + 12, NATIVE_MARKER_MASK );
    guest_write32( p + 16, t_batch->seq );
    guest_write32( dev + 48, p + 16 );
    t_batch_end = p + 16;
}

// The dirty-state writers: a 64-bit mask, its bits from the top naming consecutive items.
template <class F> void each_bit( uint64_t mask, F f )
{
    for ( uint32_t k = 0; mask; ++k, mask <<= 1 )
        if ( mask >> 63 ) f( k );
}
// Runs of set bits as one operation each (fewer, longer register writes).
template <class F> void each_run( uint64_t mask, F f )
{
    for ( uint32_t k = 0; k < 64; )
    {
        if ( !( ( mask << k ) >> 63 ) ) { ++k; continue; }
        uint32_t n = 0;
        while ( k + n < 64 && ( ( mask << ( k + n ) ) >> 63 ) ) ++n;
        f( k, n );
        k += n;
    }
}
} // namespace

bool native_d3d_ops() { return native_ops(); }

// 8274F070(device, mask, first register, shadow): registers first + k from shadow word k.
extern "C" PPC_FUNC( __imp__sub_8274F070 );
PPC_FUNC( sub_8274F070 )
{
    PPCContext &c = *regs( ctx );
    uint32_t dev = c.r3.u32, first = c.r5.u32, src = c.r6.u32;
    if ( !can_record( c, dev ) ) return __imp__sub_8274F070( ctx, base, frame );
    Ops ops_;
    auto &o = ops_.v;
    each_run( c.r4.u64, [&]( uint32_t k, uint32_t n ) { op_regs_from( o, first + k, src + k * 4, n ); } );
    record( ctx, base, frame, dev, o );
}

// 8274F410(device, mask, first register, shadow): ALU constants, 4 vectors (16 words) per bit.
extern "C" PPC_FUNC( __imp__sub_8274F410 );
PPC_FUNC( sub_8274F410 )
{
    PPCContext &c = *regs( ctx );
    uint32_t dev = c.r3.u32, first = c.r5.u32, src = c.r6.u32;
    if ( !can_record( c, dev ) ) return __imp__sub_8274F410( ctx, base, frame );
    Ops ops_;
    auto &o = ops_.v;
    each_run( c.r4.u64, [&]( uint32_t k, uint32_t n ) { op_regs_from( o, first + k * 16, src + k * 64, n * 16 ); } );
    record( ctx, base, frame, dev, o );
}

// 8274F2B0(device, mask): fetch constants (6 words per bit) from the shadow at +1024, then a texture
// cache flush (register 0x2500).
extern "C" PPC_FUNC( __imp__sub_8274F2B0 );
PPC_FUNC( sub_8274F2B0 )
{
    PPCContext &c = *regs( ctx );
    uint32_t dev = c.r3.u32;
    if ( !can_record( c, dev ) ) return __imp__sub_8274F2B0( ctx, base, frame );
    Ops ops_;
    auto &o = ops_.v;
    each_run( c.r4.u64, [&]( uint32_t k, uint32_t n ) { op_regs_from( o, 0x4800 + k * 6, dev + 1024 + k * 24, n * 6 ); } );
    op_reg( o, 0x2500, 0 );
    record( ctx, base, frame, dev, o );
}

// 8274F130(device, mask): a cache flush (0x2007 from +10268, the coherency registers and a wait on
// them, nothing to the renderer), then 4 registers from 0x2388 per bit from the shadow at +10144.
extern "C" PPC_FUNC( __imp__sub_8274F130 );
PPC_FUNC( sub_8274F130 )
{
    PPCContext &c = *regs( ctx );
    uint32_t dev = c.r3.u32;
    if ( !can_record( c, dev ) ) return __imp__sub_8274F130( ctx, base, frame );
    Ops ops_;
    auto &o = ops_.v;
    op_reg( o, 0x2007, guest_read32( dev + 10268 ) );
    op_reg( o, 0x0A31, 0x10000 );
    op_begin( o, OP_REGS, 3 ), o.push_back( 0x0A2F ), o.push_back( 0 ), o.push_back( 0x1000 );
    each_run( c.r4.u64, [&]( uint32_t k, uint32_t n ) { op_regs_from( o, 0x2388 + k * 4, dev + 10144 + k * 16, n * 4 ); } );
    record( ctx, base, frame, dev, o );
}

namespace
{
uint64_t read64( uint32_t a ) { return (uint64_t)guest_read32( a ) << 32 | guest_read32( a + 4 ); }
void write64( uint32_t a, uint64_t v ) { guest_write32( a, (uint32_t)( v >> 32 ) ), guest_write32( a + 4, (uint32_t)v ); }

// The device's dirty state out (as D3D's draws do before drawing): dirty masks at +0 (vertex shader
// constants), +8 (pixel shader constants), +16, +24, +32. 827501B8 and 8274ECD8 (shaders and the state
// derived from them) still write commands, in order with the batches.
void flush_dirty( uintptr_t ctx, uintptr_t base, uintptr_t frame, uint32_t dev )
{
    PPCContext &c = *regs( ctx );
    auto q = [dev]( uint32_t off ) { return read64( dev + off ); };
    auto clear = [dev]( uint32_t off ) { write64( dev + off, 0 ); };
    auto call = [&]( void ( *f )( uintptr_t, uintptr_t, uintptr_t ), uint64_t r3, uint64_t r4, uint64_t r5 = 0, uint64_t r6 = 0 ) {
        c.r3.u64 = r3, c.r4.u64 = r4, c.r5.u64 = r5, c.r6.u64 = r6;
        f( ctx, base, frame );
        return c.r3.u64;
    };
    uint64_t m_vs = q( 0 ), m_ps = q( 8 ), m16 = q( 16 ), m24 = q( 24 ), m32 = q( 32 );
    if ( m_vs ) call( sub_8274F410, dev, m_vs, 0x4000, dev + 1792 ), clear( 0 );
    if ( m_ps ) call( sub_8274F410, dev, m_ps, 0x4400, dev + 5888 ), clear( 8 );
    if ( m16 )
    {
        if ( m16 & 0x1E0000 ) m16 = call( sub_827501B8, dev, m16 );
        if ( q( 40 ) & m16 ) m16 = call( sub_8274ECD8, dev, m16, guest_read32( dev + 10432 ) );
        if ( m16 & 0xFFF ) call( sub_8274F070, dev, m16 << 52, 0x2200, dev + 10420 );
        if ( m16 & 0x1F000 ) call( sub_8274F070, dev, ( m16 >> 12 ) << 59, 0x2180, dev + 10400 );
        if ( m16 & ( 0xFFFFull << 42 ) ) call( sub_8274F070, dev, ( m16 >> 42 ) << 48, 0x2000, dev + 10240 );
        if ( m16 & 0x3FFFFE00000ull ) call( sub_8274F070, dev, ( m16 >> 21 ) << 43, 0x2100, dev + 10316 );
        clear( 16 );
    }
    if ( m24 )
    {
        if ( m24 & 0xFFFFFFFF ) call( sub_8274F2B0, dev, m24 << 32 );
        if ( m24 & ( 0x1FFFFFull << 34 ) ) call( sub_8274F070, dev, ( m24 >> 34 ) << 43, 0x2280, dev + 10468 );
        clear( 24 );
    }
    if ( m32 )
    {
        if ( m32 & 0x3FFFFFFFFFull ) call( sub_8274F070, dev, m32 << 26, 0x2300, dev + 10552 );
        if ( m32 & ( 0xFFull << 38 ) ) call( sub_8274F070, dev, ( m32 >> 38 ) << 56, 0x2380, dev + 10704 );
        if ( m32 & ( 0x3Full << 49 ) ) call( sub_8274F130, dev, ( m32 >> 49 ) << 58 );
        if ( m32 & ( 1ull << 56 ) ) call( sub_8274F070, dev, 0xFFFFFFFFFF000000ull, 0x4900, dev + 9984 );
        clear( 32 );
    }

}
} // namespace

// D3DDevice_DrawIndexedVertices (82743390: device, primitive, base vertex, start index, index count):
// the dirty state, then the draw. The draw is split where
// the count passes 65535, as D3D does. Under predicated tiling each part has its own mask slot.
extern "C" PPC_FUNC( __imp__sub_82743390 );
PPC_FUNC( sub_82743390 )
{
    PPCContext &c = *regs( ctx );
    uint32_t dev = c.r3.u32;
    if ( !can_record( c, dev ) ) return __imp__sub_82743390( ctx, base, frame );
    bool tiled = guest_read8( dev + 10812 ) & 0x02;
    uint32_t prim = c.r4.u32, base_vertex = c.r5.u32, start = c.r6.u32, count = c.r7.u32;
    flush_dirty( ctx, base, frame, dev );

    // The draw. Index buffer (+12156): +0 format (bit 31: 32-bit indices; bits 30-31 also give the
    // endian swap), +24 address. +11907 indexes a table at +11842 of the draw's visibility word.
    uint32_t ib = guest_read32( dev + 12156 ), fmt = guest_read32( ib ), ib_addr = guest_read32( ib + 24 );
    uint32_t sel = guest_read8( dev + 11907 ), viz = ( guest_read8( dev + 11842 + sel ) | ( sel ? 0x100 : 0 ) ) & 0x13F;
    uint32_t endian = ( fmt << 1 | fmt >> 31 ) & 0xC0000000;
    auto physical = []( uint32_t a ) { return ( ( ( a >> 20 ) + 0x200 ) & 0x1000 ) + ( a & 0x1FFFFFFF ); };
    (void)viz;
    Ops ops_;
    auto &o = ops_.v;
    for ( ;; )
    {
        uint32_t n = count;
        if ( n > 65535 )
        {
            uint32_t step = guest_read32( 0x82089E78 + ( prim & 0x3F ) * 8 );
            n = ( 65535 / step & ~1u ) * step;
        }
        op_reg( o, 0x2102, base_vertex );  // VGT_INDX_OFFSET
        uint32_t initiator = n << 16 | ( prim & 0x3F ), index_base, index_size;
        if ( fmt >> 31 ) initiator |= 0x800, index_base = physical( start * 4 + ib_addr ), index_size = ( n << 1 & 0x00FFFFFE ) | endian;
        else index_base = physical( start * 2 + ib_addr ), index_size = ( n & 0xFFFFFF ) | endian;
        op_draw_predicated( o, initiator, index_base, index_size );
        if ( tiled )  // its own mask slot; then the bin mask D3D leaves after each tiled draw
        {
            op_begin( o, OP_BIN, 2 ), o.push_back( 0 ), o.push_back( 0x80000000 );
            record_masked( ctx, base, frame, dev, o );
            o.clear();
        }
        if ( count == n ) break;
        uint32_t back = guest_read32( 0x82089E78 + ( prim & 0x3F ) * 8 + 4 );  // vertices the next part repeats (strips)
        start += n - back, count = count - n + back;
    }
    record( ctx, base, frame, dev, o );
}

// D3DDevice_RunCommandBuffer (8274CAE8: device, command buffer, predication select): replays one of
// the game's precompiled command buffers. The dirty state the buffer doesn't set itself goes out
// first; the buffer's segments (+116: list of { next, count, (address, dwords)... }) run from their
// cached translations; the bin mask comes back to the device's (+12428/+12432) if the buffer changed
// it (+108 bit 0x40). Then D3D's bookkeeping: the buffer's resources (+112) are marked used by this
// frame's fence, the scissor comes back unless the buffer kept it, and what the buffer set is dirty.
// Under predicated tiling (a buffer flagged 0x800) it has a mask slot as draws do. A buffer flagged 0x20
// (nothing to run) goes through D3D's own.
extern "C" PPC_FUNC( __imp__sub_8274CAE8 );
PPC_FUNC( sub_8274CAE8 )
{
    PPCContext &c = *regs( ctx );
    uint32_t dev = c.r3.u32, cb = c.r4.u32, select = c.r5.u32;
    uint32_t flags = cb ? guest_read32( cb + 108 ) : 0;
    if ( !can_record( c, dev ) || ( flags & 0x20 ) ) return __imp__sub_8274CAE8( ctx, base, frame );
    for ( int i = 0; i < 5; ++i ) write64( dev + i * 8, read64( dev + i * 8 ) & read64( cb + 24 + i * 8 ) );
    flush_dirty( ctx, base, frame, dev );

    Ops ops_;
    auto &o = ops_.v;
    op_begin( o, OP_BIN, 2 ), o.push_back( 3 ), o.push_back( select );
    bool tiled = ( guest_read8( dev + 10812 ) & 0x02 ) && ( flags & 0x800 );
    bool predicated = tiled || ( guest_read32( dev + 12436 ) & 1 );
    for ( uint32_t node = guest_read32( cb + 116 ); node; node = guest_read32( node ) )
        for ( uint32_t k = 0, n = guest_read32( node + 4 ); k < n; ++k )
        {
            uint32_t addr = guest_read32( node + 8 + k * 8 ), dwords = guest_read32( node + 12 + k * 8 ) & 0xFFFFF;
            addr = addr < 0x20000000 ? 0xA0000000 | addr : addr;
            size_t at = o.size();
            if ( predicated ) op_begin( o, OP_PRED, 1 ), o.push_back( 0 );
            gpu_run_buffer( o, addr, dwords );
            if ( predicated ) o[at + 1] = (uint32_t)( o.size() - at - 2 );
        }
    if ( tiled ) op_begin( o, OP_BIN, 2 ), o.push_back( 0 ), o.push_back( 0x80000000 );
    if ( flags & 0x40 )
    {
        op_begin( o, OP_BIN, 2 ), o.push_back( 0 ), o.push_back( guest_read32( dev + 12428 ) );
        op_begin( o, OP_BIN, 2 ), o.push_back( 1 ), o.push_back( guest_read32( dev + 12432 ) );
    }
    if ( tiled ) record_masked( ctx, base, frame, dev, o );
    else record( ctx, base, frame, dev, o );

    uint32_t fence = guest_read32( dev + 10780 );
    if ( uint32_t release = guest_read32( cb + 120 ); release && ( guest_read8( dev + 10812 ) & 0x20 ) && guest_read32( cb + 8 ) != fence )
    {
        uint32_t list = dev + 12980, at = guest_read32( list + 8 );
        if ( at + 4 > guest_read32( list + 12 ) )
        {
            c.r3.u64 = list;
            sub_82743858( ctx, base, frame );
            at = c.r3.u32;
        }
        guest_write32( at, release );
        guest_write32( dev + 12988, at + 4 );
    }
    guest_write32( cb + 8, fence );
    uint32_t used = select ? select : ~0u;
    for ( uint32_t node = guest_read32( cb + 112 ); node; node = guest_read32( node ) )
        for ( uint32_t k = 0, n = guest_read32( node + 4 ) & 0x7FFFFFFF; k < n; ++k )
        {
            uint32_t e0 = guest_read32( node + 8 + k * 8 ), e1 = guest_read32( node + 12 + k * 8 ), res = e0 << 2;
            if ( !res ) continue;
            if ( e1 & used ) guest_write32( res + 8, fence );
            if ( e0 & 0x40000000 ) guest_write32( res + 12, fence );
        }
    if ( ( read64( cb + 96 ) & ( 7ull << 46 ) ) != ( 7ull << 46 ) )
    {
        int32_t s0 = guest_read32( dev + 10308 ), s1 = guest_read32( dev + 10312 );
        c.r3.u64 = dev;
        c.r4.s64 = (int32_t)( ( (uint32_t)s0 << 17 | (uint32_t)s0 >> 15 ) & 0xFFFE0000 ) >> 17;
        c.r5.s64 = (int32_t)( (uint32_t)s0 << 1 ) >> 17;
        c.r6.s64 = (int32_t)( ( (uint32_t)s1 << 17 | (uint32_t)s1 >> 15 ) & 0xFFFE0000 ) >> 17;
        c.r7.s64 = (int32_t)( (uint32_t)s1 << 1 ) >> 17;
        sub_82738708( ctx, base, frame );
    }
    for ( int i = 0; i < 5; ++i ) write64( dev + i * 8, ~read64( cb + 64 + i * 8 ) );
}



// D3DDevice_DrawVertices (82742FA0: device, primitive, start vertex, vertex count): as the indexed draw,
// with the vertices' own indices (initiator bit 0x80: auto-index) from the start vertex (0x2102).
extern "C" PPC_FUNC( __imp__sub_82742FA0 );
PPC_FUNC( sub_82742FA0 )
{
    PPCContext &c = *regs( ctx );
    uint32_t dev = c.r3.u32;
    if ( !can_record( c, dev ) ) return __imp__sub_82742FA0( ctx, base, frame );
    bool tiled = guest_read8( dev + 10812 ) & 0x02;
    uint32_t prim = c.r4.u32, start = c.r5.u32, count = c.r6.u32;
    flush_dirty( ctx, base, frame, dev );
    Ops ops_;
    auto &o = ops_.v;
    for ( ;; )
    {
        uint32_t n = count;
        if ( n > 65535 )
        {
            uint32_t step = guest_read32( 0x82089E78 + ( prim & 0x3F ) * 8 );
            n = ( 65535 / step & ~1u ) * step;
        }
        op_reg( o, 0x2102, start );
        op_draw_predicated( o, n << 16 | ( prim & 0x3F ) | 0x80, 0, 0 );
        if ( tiled )
        {
            op_begin( o, OP_BIN, 2 ), o.push_back( 0 ), o.push_back( 0x80000000 );
            record_masked( ctx, base, frame, dev, o );
            o.clear();
        }
        if ( count == n ) break;
        uint32_t back = guest_read32( 0x82089E78 + ( prim & 0x3F ) * 8 + 4 );
        start += n - back, count = count - n + back;
    }
    record( ctx, base, frame, dev, o );
}

// D3DDevice_BeginVertices (82742578: device, primitive, vertex count, stride; returns where the caller
// writes the vertices): the vertices go in memory D3D takes off the top of the command segment
// (82743CD0), fetched through vertex fetch constant slot 31 (0x48BA..0x48BF); the draw is recorded now
// and runs with the stream, after the caller has written them. EndVertices (82742A10) puts the command
// pointer back to +13076. While the dirty state goes out, +12248 holds this draw's stride (in words).
extern "C" PPC_FUNC( __imp__sub_82742578 );
PPC_FUNC( sub_82742578 )
{
    PPCContext &c = *regs( ctx );
    uint32_t dev = c.r3.u32;
    if ( !can_record( c, dev ) ) return __imp__sub_82742578( ctx, base, frame );
    bool tiled = guest_read8( dev + 10812 ) & 0x02;
    uint32_t prim = c.r4.u32, count = c.r5.u32, stride = c.r6.u32, dwords = ( count * stride ) >> 2;
    uint8_t old = guest_read8( dev + 12248 ), words = ( stride >> 2 ) & 0xFF;
    g_base[dev + 12248] = words;
    if ( words != guest_read8( dev + 11552 ) && guest_read32( dev + 12416 ) ) write64( dev + 16, read64( dev + 16 ) | 0x80000 );
    flush_dirty( ctx, base, frame, dev );
    g_base[dev + 12248] = old;
    if ( guest_read32( dev + 48 ) > guest_read32( dev + 56 ) ) c.r3.u64 = dev, sub_827447A0( ctx, base, frame );
    c.r3.u64 = dev, c.r4.u64 = dwords, c.r5.u64 = 16;
    sub_82743CD0( ctx, base, frame );
    uint32_t v = c.r3.u32;
    if ( !v ) return;
    Ops ops_;
    auto &o = ops_.v;
    op_reg( o, 0x2102, 0 );
    uint32_t vphys = ( ( ( v >> 20 ) + 0x200 ) & 0x1000 ) + ( v & 0x1FFFFFFF );
    const uint32_t fetch[6] = { 1, 0, 1, 0, vphys | 3, 0x10000002 | ( ( dwords << 2 ) & 0x03FFFFFC ) };
    op_begin( o, OP_REGS, 7 ), o.push_back( 0x48BA ), o.insert( o.end(), fetch, fetch + 6 );
    op_begin( o, OP_REGS, 4 ), o.push_back( 0x5000 ), o.push_back( 0 ), o.push_back( 0 ), o.push_back( 0 );
    op_draw_predicated( o, count << 16 | ( prim & 0x3F ) | 0x80, 0, 0 );
    if ( tiled )
    {
        op_begin( o, OP_BIN, 2 ), o.push_back( 0 ), o.push_back( 0x80000000 );
        record_masked( ctx, base, frame, dev, o );
    }
    else record( ctx, base, frame, dev, o );
    write64( dev + 24, read64( dev + 24 ) | 1 );  // slot 31 is this draw's now
    guest_write32( dev + 13076, guest_read32( dev + 48 ) );
    guest_write32( dev + 13080, v );
    guest_write32( dev + 13088, dwords );
    c.r3.u64 = v;
}

namespace
{
// Whether the device draws in tiles now: +10812 bit 0x10, or bit 0x20 with the surfaces tiling was set
// up for (+12160..12176 equal to +12448..12464, or 0).
bool tiling( uint32_t dev )
{
    uint8_t flags = guest_read8( dev + 10812 );
    if ( flags & 0x10 ) return true;
    if ( !( flags & 0x20 ) ) return false;
    for ( int i = 0; i < 5; ++i )
        if ( uint32_t want = guest_read32( dev + 12160 + i * 4 ); want && want != guest_read32( dev + 12448 + i * 4 ) ) return false;
    return true;
}
} // namespace

// D3DDevice_SetScissorRect (82738708: device, x0, y0, x1, y1). Untiled: the window offset (0x2080) and
// the scissor (0x2081 top left, 0x2082 bottom right, 14 bits each). Under tiling (+10812 bit 0x10, or
// bit 0x20 with the device's surfaces those tiling was set up for, +12160..12176 = +12448..12464 or 0):
// for each tile (+12468 of them; rects { x0, y0, x1, y1 } at +12472, offsets { x, y } at +12712) the
// offset and the scissor clipped to the tile, predicated on the tile's bins (3 << 2 * tile); then (with
// +10815 and +10812 bit 0x40) the scissor unclipped for bin 1; then the device's bin mask back. As the
// decoded commands were, a bottom right of 1280x484 becomes 1280x720 (one tile all the way).
extern "C" PPC_FUNC( __imp__sub_82738708 );
PPC_FUNC( sub_82738708 )
{
    PPCContext &c = *regs( ctx );
    uint32_t dev = c.r3.u32;
    if ( !can_record( c, dev ) ) return __imp__sub_82738708( ctx, base, frame );
    int32_t x0 = c.r4.s32, y0 = c.r5.s32, x1 = c.r6.s32, y1 = c.r7.s32;
    auto corner = []( int32_t x, int32_t y ) { return ( (uint32_t)y << 16 & 0x3FFF0000 ) | ( (uint32_t)x & 0x3FFF ); };
    auto br = []( uint32_t v ) { return v == 0x01E40500 ? 0x02D00500u : v; };
    uint8_t flags = guest_read8( dev + 10812 );
    bool tiled = tiling( dev );
    Ops ops_;
    auto &o = ops_.v;
    auto bin = [&o]( uint32_t which, uint32_t v ) { op_begin( o, OP_BIN, 2 ), o.push_back( which ), o.push_back( v ); };
    if ( !tiled )
    {
        op_begin( o, OP_REGS, 4 ), o.push_back( 0x2080 ), o.push_back( 0 ), o.push_back( corner( x0, y0 ) ), o.push_back( br( corner( x1, y1 ) ) );
        return record( ctx, base, frame, dev, o );
    }
    bin( 1, 0 );
    for ( uint32_t k = 0, n = guest_read32( dev + 12468 ); k < n; ++k )
    {
        int32_t tx0 = guest_read32( dev + 12472 + k * 16 ), ty0 = guest_read32( dev + 12476 + k * 16 );
        int32_t tx1 = guest_read32( dev + 12480 + k * 16 ), ty1 = guest_read32( dev + 12484 + k * 16 );
        int32_t ox = guest_read32( dev + 12712 + k * 8 ), oy = guest_read32( dev + 12716 + k * 8 );
        int32_t cx0 = std::max( x0, tx0 ), cy0 = std::max( y0, ty0 ), cx1 = std::min( x1, tx1 ), cy1 = std::min( y1, ty1 );
        if ( cx0 >= cx1 || cy0 >= cy1 ) cx0 = cy0 = cx1 = cy1 = 0;
        bin( 0, 3u << ( 2 * k ) );
        op_begin( o, OP_PRED, 1 ), o.push_back( 5 );
        op_begin( o, OP_REGS, 4 ), o.push_back( 0x2080 );
        o.push_back( ( (uint32_t)-oy << 16 & 0x7FFF0000 ) | ( (uint32_t)-ox & 0x7FFF ) );
        o.push_back( corner( cx0, cy0 ) ), o.push_back( br( corner( cx1, cy1 ) ) );
    }
    if ( ( guest_read8( dev + 10815 ) & 0x40 ) && ( flags & 0x40 ) )
    {
        bin( 0, 1 );
        op_begin( o, OP_PRED, 1 ), o.push_back( 4 );
        op_begin( o, OP_REGS, 3 ), o.push_back( 0x2081 ), o.push_back( corner( x0, y0 ) ), o.push_back( br( corner( x1, y1 ) ) );
    }
    bin( 0, guest_read32( dev + 12428 ) );
    bin( 1, guest_read32( dev + 12432 ) );
    record( ctx, base, frame, dev, o );
}

// (The viewport, its scissor and the render target setters, 827389E8, 8273AEF8, 8273B278, 8273B5B0,
// write nothing themselves: their scissor is native. So do the clears' entry points, 82748318 and
// 827483D8, and EndTiling, 82746DD0, whose patching goes into the batches' mask slots.)

// D3DDevice_SetPredication (82746698: device, bin mask). A mask predicates what follows on those bins
// (+12436 = 1); 0 ends it: the mask is all bins, and the draws that follow are tiled (+10812 bit 0x02)
// if the device is tiling (and not +10812 bit 0x08), with +10815 bit 0x40 or neither +11841 nor +11907.
extern "C" PPC_FUNC( __imp__sub_82746698 );
PPC_FUNC( sub_82746698 )
{
    PPCContext &c = *regs( ctx );
    uint32_t dev = c.r3.u32, mask = c.r4.u32;
    if ( !can_record( c, dev ) ) return __imp__sub_82746698( ctx, base, frame );
    uint8_t flags = guest_read8( dev + 10812 );
    if ( !mask )
    {
        guest_write32( dev + 12436, 0 );
        bool tiled = tiling( dev ) && !( flags & 0x08 ) &&
                     ( ( guest_read8( dev + 10815 ) & 0x40 ) || ( !guest_read8( dev + 11841 ) && !guest_read8( dev + 11907 ) ) );
        guest_write32( dev + 12428, ~0u );
        g_base[dev + 10812] = ( flags & ~0x02 ) | ( tiled ? 0x02 : 0 );
    }
    else
    {
        guest_write32( dev + 12428, mask );
        guest_write32( dev + 12436, 1 );
        g_base[dev + 10812] = flags & ~0x02;
    }
    guest_write32( dev + 12432, 0 );
    guest_write32( dev + 10804, mask );
    guest_write32( dev + 10808, 0 );
    Ops ops_;
    auto &o = ops_.v;
    op_begin( o, OP_BIN, 2 ), o.push_back( 0 ), o.push_back( guest_read32( dev + 12428 ) );
    record( ctx, base, frame, dev, o );
}

namespace
{
// One of D3D's own command buffers (address and dwords in device fields), as RunCommandBuffer's.
void run_buffer( std::vector<uint32_t> &o, uint32_t addr, uint32_t dwords, bool predicated )
{
    addr = addr < 0x20000000 ? 0xA0000000 | addr : addr;
    size_t at = o.size();
    if ( predicated ) op_begin( o, OP_PRED, 1 ), o.push_back( 0 );
    gpu_run_buffer( o, addr, dwords & 0xFFFFF );
    if ( predicated ) o[at + 1] = (uint32_t)( o.size() - at - 2 );
}
} // namespace

// 8273CFE0 (device, flags, a, b; a and b 0: 64 each): D3D's own command buffers for a pass (+13408, the
// one for a at +13416 + a * 8, +14440), predicated with the device's predication when flags bit 0 is
// set; +10792 takes a and b unless flags bit 0. The state they set is dirty after; the scissor comes back.
extern "C" PPC_FUNC( __imp__sub_8273CFE0 );
PPC_FUNC( sub_8273CFE0 )
{
    PPCContext &c = *regs( ctx );
    uint32_t dev = c.r3.u32, flags = c.r4.u32, a = c.r5.u32, b = c.r6.u32;
    if ( !can_record( c, dev ) ) return __imp__sub_8273CFE0( ctx, base, frame );
    if ( !a && !b ) a = b = 64;
    uint32_t v = 0;
    if ( !( flags & 1 ) )
    {
        b = ( b & ~0x7F00u ) | ( a << 8 & 0x7F00 );
        v = ( guest_read32( dev + 10792 ) & ~0x7F7F0u ) | ( b << 4 & 0x7F7F0 );
    }
    guest_write32( dev + 10792, v );
    bool predicated = guest_read32( dev + 12436 ) & flags & 1;
    Ops ops_;
    auto &o = ops_.v;
    run_buffer( o, guest_read32( dev + 13408 ), guest_read32( dev + 13412 ), predicated );
    run_buffer( o, guest_read32( dev + 13416 + a * 8 ), guest_read32( dev + 13420 + a * 8 ), predicated );
    run_buffer( o, guest_read32( dev + 14440 ), guest_read32( dev + 14444 ), predicated );
    record( ctx, base, frame, dev, o );
    write64( dev + 16, read64( dev + 16 ) | 1ull << 44 | 1ull << 41 | 1ull << 40 | 1ull << 37 | 0x100000 | 0x80000 | 2048 | 256 | 128 | 64 | 32 | 8 );
    write64( dev + 24, read64( dev + 24 ) | 1ull << 54 | 0x80000000 );
    write64( dev + 32, read64( dev + 32 ) | 1ull << 35 | 0x80000 );
    int32_t s0 = guest_read32( dev + 10308 ), s1 = guest_read32( dev + 10312 );
    c.r3.u64 = dev;
    c.r4.s64 = (int32_t)( ( (uint32_t)s0 << 17 | (uint32_t)s0 >> 15 ) & 0xFFFE0000 ) >> 17;
    c.r5.s64 = (int32_t)( (uint32_t)s0 << 1 ) >> 17;
    c.r6.s64 = (int32_t)( ( (uint32_t)s1 << 17 | (uint32_t)s1 >> 15 ) & 0xFFFE0000 ) >> 17;
    c.r7.s64 = (int32_t)( (uint32_t)s1 << 1 ) >> 17;
    sub_82738708( ctx, base, frame );
}

namespace
{
uint32_t gpu_address( uint32_t a ) { return ( ( ( a >> 20 ) + 0x200 ) & 0x1000 ) + ( a & 0x1FFFFFFF ); }  // as D3D gives the GPU
}

// The occlusion query's sample count (82749960: device, query, slot): the count goes to the query's
// buffer (+24, one per tile with +144 tiles; slot * 32 in) through RB_SAMPLE_COUNT_ADDR (0x2325),
// written per tile predicated on its bins under tiling; RB_SAMPLE_COUNT_CTL (0x2208) = 6 around it.
extern "C" PPC_FUNC( __imp__sub_82749960 );
PPC_FUNC( sub_82749960 )
{
    PPCContext &c = *regs( ctx );
    uint32_t dev = c.r3.u32, query = c.r4.u32, slot = c.r5.u32;
    if ( !can_record( c, dev ) ) return __imp__sub_82749960( ctx, base, frame );
    Ops ops_;
    auto &o = ops_.v;
    auto bin = [&o]( uint32_t which, uint32_t v ) { op_begin( o, OP_BIN, 2 ), o.push_back( which ), o.push_back( v ); };
    op_reg( o, 0x2208, 6 );
    op_begin( o, OP_REGS, 3 ), o.push_back( 0x2000 ), o.push_back( 0x20000 ), o.push_back( 0 );
    uint32_t tiles = guest_read32( query + 144 );
    if ( tiles <= 1 ) op_reg( o, 0x2325, gpu_address( guest_read32( query + 24 ) + slot * 32 ) );
    else
    {
        bin( 1, 0 );
        for ( uint32_t k = 0; k < tiles; ++k )
        {
            bin( 0, 3u << ( 2 * k ) );
            op_begin( o, OP_PRED, 1 ), o.push_back( 3 );
            op_reg( o, 0x2325, gpu_address( guest_read32( query + 24 + k * 4 ) + slot * 32 ) );
        }
        bin( 0, guest_read32( dev + 12428 ) );
        bin( 1, guest_read32( dev + 12432 ) );
    }
    op_begin( o, OP_ZPD, 0 );
    record( ctx, base, frame, dev, o );
    write64( dev + 16, read64( dev + 16 ) | 1ull << 57 | 1ull << 56 | 8 );
    write64( dev + 32, read64( dev + 32 ) | 8192 );
}

// Issuing a query (82749ED0: query, flags; query +0 device, +4 type, +16 state, +20 fence, +24 buffer).
// Begin (flags bit 1): an occlusion query (type 9) marks its counts not yet written (-275 in each
// tile's) and samples the count at its start (slot 1); an extents query (type 10) marks its result
// unwritten and starts collecting (+10814 bit 0). End (bit 0): occlusion samples again (slot 0); extents
// has the screen extents written (the whole screen: drawn as one tile, everything is everywhere).
extern "C" PPC_FUNC( __imp__sub_82749ED0 );
PPC_FUNC( sub_82749ED0 )
{
    PPCContext &c = *regs( ctx );
    uint32_t query = c.r3.u32, flags = c.r4.u32, dev = query ? guest_read32( query ) : 0;
    if ( !can_record( c, dev ) ) return __imp__sub_82749ED0( ctx, base, frame );
    uint32_t type = guest_read32( query + 4 );
    auto sample = [&]( uint32_t slot ) {
        c.r3.u64 = dev, c.r4.u64 = query, c.r5.u64 = slot;
        sub_82749960( ctx, base, frame );
    };
    if ( flags & 2 )
    {
        guest_write32( query + 144, ( guest_read8( dev + 10812 ) & 0x20 ) ? guest_read32( dev + 12468 ) : 1 );
        if ( type == 9 )
        {
            for ( uint32_t k = 0, n = guest_read32( query + 144 ); k < n; ++k )
                for ( int i = 0; i < 4; ++i ) guest_write32( guest_read32( query + 24 + k * 4 ) + i * 4, (uint32_t)-275 );
            sample( 1 );
        }
        else if ( type == 10 )
        {
            guest_write32( guest_read32( query + 24 ), (uint32_t)-275 );
            g_base[dev + 10814] |= 1;
        }
        guest_write32( query + 16, 2 );
    }
    else if ( flags & 1 )
    {
        if ( type != 8 )
        {
            if ( type == 9 ) sample( 0 );
            else if ( type == 10 )
            {
                Ops ops_;
    auto &o = ops_.v;
                op_begin( o, OP_EXTENTS, 1 ), o.push_back( gpu_address( guest_read32( query + 24 ) ) );
                record( ctx, base, frame, dev, o );
                g_base[dev + 10814] &= ~1;
            }
            guest_write32( query + 16, 1 );
        }
        guest_write32( query + 20, guest_read32( dev + 10780 ) );
    }
}

namespace
{
// An atomic add to a guest word (as lwarx / stwcx.).
void guest_atomic_add( uint32_t a, uint32_t v )
{
    auto *p = (uint32_t *)( g_base + a );
    uint32_t old = __atomic_load_n( p, __ATOMIC_RELAXED );
    while ( !__atomic_compare_exchange_n( p, &old, __builtin_bswap32( __builtin_bswap32( old ) + v ), true, __ATOMIC_SEQ_CST, __ATOMIC_RELAXED ) ) {}
}
} // namespace

// Locking a resource's memory for the CPU (82735BE8: resource, a, b, c, d, address, bytes, flags; the
// 9th argument on the stack; returns the address to use). It waits for the GPU's last use of it (+8, or
// +12 when writing: flags 0x1010; a texture with its own memory object at +24 looks there) unless
// flags 0x12; the GPU's caches are flushed for the range: by the device's thread at once (the coherency
// registers: nothing to the renderer, which reads memory as it is), from other threads the range is
// added to the device's deferred one (+11544). Flags 0x10 or a resource with +0 bit 0x200000 keeps a
// dirty range in 128-byte units (+20, or +24 with b and d) unless flags bit 0, and flags 0x10 returns
// the GPU's view of the address. +0 counts locks (by 256).
extern "C" PPC_FUNC( __imp__sub_82735BE8 );
PPC_FUNC( sub_82735BE8 )
{
    PPCContext &c = *regs( ctx );
    uint32_t dev = guest_read32( guest_read32( 0x82000798 ) );
    if ( !native_ops() || ( guest_read8( dev + 10812 ) & 0x80 ) ) return __imp__sub_82735BE8( ctx, base, frame );
    uint32_t res = c.r3.u32, a = c.r4.u32, b = c.r5.u32, cc = c.r6.u32, d = c.r7.u32, addr = c.r8.u32, bytes = c.r9.u32, flags = c.r10.u32;
    uint32_t arg9 = guest_read32( c.r1.u32 + 84 );
    uint32_t hdr = guest_read32( res ), owner = res;
    if ( ( hdr & 0xF ) == 4 && ( hdr & 0x40000000 ) && guest_read32( res + 24 ) ) owner = guest_read32( res + 24 );
    if ( uint32_t fence = guest_read32( owner + ( ( flags & 0x1010 ) ? 12 : 8 ) ) )
    {
        c.r3.u64 = dev, c.r4.u64 = fence, c.r5.u64 = a;
        sub_82744888( ctx, base, frame );
    }
    if ( !( flags & 0x12 ) )
    {
        uint32_t lo = gpu_address( addr ), hi = lo + bytes;
        if ( !can_record( c, dev ) )  // another thread: widen the deferred range { high, low }
        {
            auto *p = (uint64_t *)( g_base + dev + 11544 );
            uint64_t old = __atomic_load_n( p, __ATOMIC_RELAXED ), want;
            do
            {
                uint64_t v = __builtin_bswap64( old );
                uint32_t vlo = (uint32_t)v, vhi = (uint32_t)( v >> 32 );
                want = __builtin_bswap64( (uint64_t)std::max( vhi, hi ) << 32 | std::min( vlo, lo ) );
            } while ( !__atomic_compare_exchange_n( p, &old, want, true, __ATOMIC_SEQ_CST, __ATOMIC_RELAXED ) );
        }
        else
        {
            Ops ops_;
    auto &o = ops_.v;
            op_reg( o, 0x0A31, arg9 );
            op_begin( o, OP_REGS, 3 ), o.push_back( 0x0A2F ), o.push_back( ( ( hi + 4095 ) & ~4095u ) - ( lo & ~4095u ) ), o.push_back( lo & ~4095u );
            record( ctx, base, frame, dev, o );
        }
    }
    if ( ( flags & 0x10 ) || ( guest_read32( res ) & 0x200000 ) )
    {
        if ( !( flags & 1 ) )
        {
            uint32_t off, range;
            if ( b && d ) off = addr - d, range = res + 24;
            else off = addr - cc, range = res + 20;
            uint32_t v = guest_read32( range ), first = std::min( ( off >> 7 ) & 0x1FFFFFF, v >> 16 ), end = std::max( ( ( off + bytes + 127 ) >> 7 ) & 0x1FFFFFF, v & 0xFFFF );
            guest_write32( range, first << 16 | end );
        }
        if ( flags & 0x10 ) addr = gpu_address( addr ) + 0xC0000000;
    }
    guest_atomic_add( res, 256 );
    c.r3.u64 = addr;
}

// The state the shaders and surfaces decide (8274ECD8: device, dirty mask, RB_COLORCONTROL value (from
// +10432); returns the mask with what it wrote taken out). Mask bit 0x100: RB_COLORCONTROL (0x2203),
// alpha-to-mask (bit 0) only where the surface allows it (+11572 bits 17-19); under tiling (with
// +10815 bit 0x40) per tile with the tile's own sample positions (+12832 + tile * 4, bits 17 up),
// predicated on its bins, and the unmasked value for bin 1. Bit 57: RB_SURFACE_INFO (0x2000) from
// +10240; bit 37: RB_DEPTH_INFO-like 0x2104 from +10332. Values other than what bin 1 should see go to
// it again (mask 0x55555555) when +10812 bit 0x40.
extern "C" PPC_FUNC( __imp__sub_8274ECD8 );
PPC_FUNC( sub_8274ECD8 )
{
    PPCContext &c = *regs( ctx );
    uint32_t dev = c.r3.u32;
    if ( !can_record( c, dev ) ) return __imp__sub_8274ECD8( ctx, base, frame );
    uint64_t mask = c.r4.u64;
    uint32_t color = c.r5.u32;
    uint32_t again_color = ~0u, again_surface = ~0u, again_depth = ~0u;
    uint8_t flags = guest_read8( dev + 10812 ), flags3 = guest_read8( dev + 10815 );
    Ops ops_;
    auto &o = ops_.v;
    auto bin = [&o]( uint32_t which, uint32_t v ) { op_begin( o, OP_BIN, 2 ), o.push_back( which ), o.push_back( v ); };
    auto predicated_reg = [&o]( uint32_t reg, uint32_t v ) { op_begin( o, OP_PRED, 1 ), o.push_back( 3 ), op_reg( o, reg, v ); };
    if ( mask & 0x100 )
    {
        if ( ( flags3 & 0x40 ) && tiling( dev ) )
        {
            bin( 1, 0 );
            uint32_t value = color, mode = ( guest_read32( dev + 11572 ) >> 17 ) & 7;
            for ( int32_t k = (int32_t)guest_read32( dev + 12468 ) - 1; k >= 0; --k )
            {
                color = ( color & 0x1FFFF ) | ( guest_read32( dev + ( 3208 + k ) * 4 ) << 17 );
                value = color;
                if ( mode == 0 || ( mode == 2 && ( k != 0 || !( flags3 & 0x20 ) ) ) ) value = color & ~1u;
                bin( 0, 3u << ( 2 * k ) );
                predicated_reg( 0x2203, value );
            }
            if ( value != color && ( flags & 0x40 ) ) bin( 0, 1 ), predicated_reg( 0x2203, color );
            bin( 0, guest_read32( dev + 12428 ) );
            bin( 1, guest_read32( dev + 12432 ) );
        }
        else
        {
            uint32_t value = ( guest_read32( dev + 11572 ) & 0xE0000 ) ? color : color & ~1u;
            op_reg( o, 0x2203, value );
            if ( value != color && ( flags & 0x40 ) ) again_color = color;
        }
        mask &= ~0x100ull;
    }
    uint64_t dirty = read64( dev + 40 ) & mask;
    if ( dirty & 1ull << 57 )
    {
        uint32_t surface = guest_read32( dev + 10240 );
        op_reg( o, 0x2000, surface );
        mask &= ~( 1ull << 57 );
        again_surface = ( guest_read32( dev + 12896 ) & 0x3FFF ) | ( surface & 0xFFFFC000 );
    }
    if ( dirty & 1ull << 37 )
    {
        op_reg( o, 0x2104, guest_read32( dev + 10332 ) );
        mask &= ~( 1ull << 37 );
        again_depth = 0;
    }
    if ( ( again_depth & again_surface & again_color ) != ~0u )
    {
        bin( 1, 0 );
        bin( 0, 0x55555555 );
        if ( again_color != ~0u ) predicated_reg( 0x2203, again_color );
        if ( again_surface != ~0u ) predicated_reg( 0x2000, again_surface );
        if ( again_depth != ~0u ) predicated_reg( 0x2104, again_depth );
        bin( 0, guest_read32( dev + 12428 ) );
        bin( 1, guest_read32( dev + 12432 ) );
    }
    record( ctx, base, frame, dev, o );
    c.r3.u64 = mask;
}

namespace
{
const uint32_t CONSTANT_BASES[5] = { 0x4000, 0x4800, 0x4900, 0x4908, 0x2000 };  // ALU, fetch, bool, loop, registers
uint32_t guest_view( uint32_t gpu ) { return gpu < 0x20000000 ? 0xA0000000 | gpu : gpu; }  // a GPU address's guest view
void op_shader( std::vector<uint32_t> &o, const void *shader )
{
    op_begin( o, OP_SHADER, 2 ), o.push_back( (uint32_t)(uintptr_t)shader ), o.push_back( (uint32_t)( (uintptr_t)shader >> 32 ) );
}
} // namespace

// A shader's constants loaded from memory (8274FFC0: device, shader, base address): the shader's table
// (+20: offset of { +16 bytes; then entries { u16 first (constant index * 4 with its type), u16 count,
// u32 offset } }) names ranges at base + offset; each goes to its constants as the memory is now.
extern "C" PPC_FUNC( __imp__sub_8274FFC0 );
PPC_FUNC( sub_8274FFC0 )
{
    PPCContext &c = *regs( ctx );
    uint32_t dev = c.r3.u32, shader = c.r4.u32, at = c.r5.u32;
    if ( !can_record( c, dev ) ) return __imp__sub_8274FFC0( ctx, base, frame );
    uint32_t table = guest_read32( shader + 20 );
    if ( !table ) return;
    uint32_t e = table + shader + 20, end = e + guest_read32( table + shader + 16 );
    Ops ops_;
    auto &o = ops_.v;
    for ( ; e < end; e += 8 )
    {
        uint32_t first = guest_read16( e ), count = guest_read16( e + 2 );
        if ( !count ) break;
        uint32_t d1 = first << 2, type = ( d1 >> 16 ) & 0xFF;
        if ( type < 5 && ( count & 0xFFF ) ) op_regs_from( o, CONSTANT_BASES[type] + ( d1 & 0x7FF ), guest_view( gpu_address( guest_read32( e + 4 ) + at ) ), count & 0xFFF );
    }
    record( ctx, base, frame, dev, o );
}

// A vertex shader bound to the current vertex layout when its own memory can't be patched (the GPU may
// still be using it): 8274FE50 (device, patch fetches, shader, a, b, layout, c, variant). D3D copies the
// variant's microcode (+872 offset from the shader's +32 base, +876 bytes) into the command stream,
// patches the copy (8274FD10: the vertex fetches for the layout; 8274F580: the variant's binding) and
// loads it from there. Here the copy is patched in a buffer of its own and the result is the shader.
extern "C" PPC_FUNC( __imp__sub_8274FE50 );
PPC_FUNC( sub_8274FE50 )
{
    PPCContext &c = *regs( ctx );
    uint32_t dev = c.r3.u32;
    if ( !can_record( c, dev ) ) return __imp__sub_8274FE50( ctx, base, frame );
    uint32_t patch = c.r4.u32, shader = c.r5.u32, a = c.r6.u32, b = c.r7.u32, layout = c.r8.u32, cc = c.r9.u32, variant = c.r10.u32;
    uint32_t v = guest_read32( shader + ( variant + 112 ) * 8 ) + shader;
    uint32_t bytes = guest_read32( v + 876 );
    static const uint32_t scratch = rt_alloc_physical( 0x40000, 0x1000 );
    if ( bytes > 0x40000 ) return __imp__sub_8274FE50( ctx, base, frame );
    uint32_t src = gpu_address( guest_read32( v + 872 ) + guest_read32( shader + 32 ) ) + 0xC0000000;
    memcpy( g_base + scratch, g_base + src, bytes );
    c.r3.u64 = src, c.r4.u64 = src + bytes;
    sub_8274E1E0( ctx, base, frame );
    if ( patch && ( guest_read8( layout + 8 ) & 7 ) != 7 )
    {
        c.r3.u64 = layout, c.r4.u64 = scratch, c.r5.u64 = a, c.r6.u64 = cc;
        sub_8274FD10( ctx, base, frame );
    }
    g_base[dev + 10813] = ( g_base[dev + 10813] & 0xFE ) | ( patch ? 1 : 0 );
    c.r3.u64 = shader, c.r4.u64 = scratch, c.r5.u64 = b, c.r6.u64 = dev + 12248, c.r7.u64 = variant;
    sub_8274F580( ctx, base, frame );
    write64( dev + 11552, read64( dev + 12248 ) );
    write64( dev + 11560, read64( dev + 12256 ) );
    Ops ops_;
    auto &o = ops_.v;
    op_shader( o, xenos_shader_find( 0, scratch, ( bytes >> 2 ) & 0x3FFF ) );
    record( ctx, base, frame, dev, o );
}

// Setting the shaders (827501B8: device, dirty mask; returns the mask). The pixel shader (+12412) loads
// from its own memory (mask bit 0x100000: with its constants; +10408 its program size, SQ_PROGRAM_CNTL
// mode +10452 = 4) and the vertex shader (+12416) for the vertex layout (+11540) of the variant to use
// (+904 for vertex-only use): one already bound to this layout and not in use by the GPU (82750070),
// else a patched copy (8274FE50); with neither a pixel shader nor a vertex shader nothing happens. The
// interpolator settings (0x2180/0x2181, +10400/+10404) follow. Under tiling with bins (+10812 bit
// 0x40) the mode, interpolators and vertex shader go to bins 0x55555555 (one pass) and 0xAAAAAAAA
// (another variant), predicated.
extern "C" PPC_FUNC( __imp__sub_827501B8 );
PPC_FUNC( sub_827501B8 )
{
    PPCContext &c = *regs( ctx );
    uint32_t dev = c.r3.u32;
    if ( !can_record( c, dev ) ) return __imp__sub_827501B8( ctx, base, frame );
    uint64_t m = c.r4.u64;
    uint32_t saved_sp = c.r1.u32, sp = ( saved_sp - 256 ) & ~15u, slot = sp + 80;  // a frame for the one value passed by address
    guest_write32( sp, saved_sp );
    auto call = [&]( void ( *f )( uintptr_t, uintptr_t, uintptr_t ), uint64_t a3, uint64_t a4, uint64_t a5 = 0, uint64_t a6 = 0, uint64_t a7 = 0,
                     uint64_t a8 = 0, uint64_t a9 = 0, uint64_t a10 = 0 ) {
        c.r1.u64 = sp;
        c.r3.u64 = a3, c.r4.u64 = a4, c.r5.u64 = a5, c.r6.u64 = a6, c.r7.u64 = a7, c.r8.u64 = a8, c.r9.u64 = a9, c.r10.u64 = a10;
        f( ctx, base, frame );
        c.r1.u64 = saved_sp;
        return c.r3.u64;
    };
    auto finish = [&]( uint64_t r ) { c.r1.u64 = saved_sp, c.r3.u64 = r; };
    int32_t mode_bin = -1, both = -1, pass = -1;  // r15, r14, r16
    if ( m & 0x20000 ) m = call( sub_8274EAF0, dev, m );
    uint32_t vs = guest_read32( dev + 12416 ), layout = guest_read32( dev + 11540 ), ps = guest_read32( dev + 12412 );
    if ( !vs ) return finish( m );
    uint32_t use = vs + guest_read32( vs + 896 ) + 872, variant = 0, psv = 0, ps_lo = 0, ps_hi = 0;
    Ops ops_;
    auto &o = ops_.v;
    auto program_mode = [&]( uint32_t mode ) {
        uint32_t v = guest_read32( dev + 10452 );
        if ( ( v & 7 ) != mode ) guest_write32( dev + 10452, ( v & ~7u ) | mode ), m |= 0x80008;
    };
    uint32_t differ;
    if ( !ps )
    {
        if ( guest_read32( vs + 872 ) & 0x20 ) variant = 1, use = vs + guest_read32( vs + 904 ) + 872;
        program_mode( 5 );
        differ = guest_read32( use + 20 ) & 0x1F;
    }
    else
    {
        psv = guest_read32( ps + 64 ) + ps + 40;
        ps_lo = guest_read32( psv + 8 ), ps_hi = guest_read32( psv + 12 );
        if ( ( ps_lo & 0x70000000 ) > 0x10000000 ) g_base[dev + 10814] |= 0x10;
        if ( m & 0x100000 )
        {
            call( sub_8274FFC0, dev, ps + 40, guest_read32( ps + 24 ) );
            m |= 0x4000;
            uint32_t at = guest_read32( ps + 64 ) + ps;
            op_shader( o, xenos_shader_find( 1, guest_view( gpu_address( guest_read32( at + 40 ) + guest_read32( ps + 24 ) ) ), ( guest_read32( at + 44 ) >> 2 ) & 0xFFFF ) );
            guest_write32( dev + 10408, guest_read32( psv + 24 ) );
            program_mode( 4 );
        }
        differ = ( guest_read32( psv + 20 ) ^ guest_read32( use + 20 ) ) & 0x7FFFF;
    }
    uint32_t old_mode = guest_read32( dev + 10452 ), mode = old_mode, interp = guest_read32( use + 12 );
    bool mismatch = differ != 0;
    guest_write32( slot, guest_read32( use + 8 ) );
    uint8_t flags = guest_read8( dev + 10812 );
    if ( flags & 0x40 )
    {
        bool keep = psv && ( ( guest_read32( dev + 10428 ) & 8 ) || ( guest_read32( psv + 28 ) & 0x30 ) );
        if ( !keep ) mode = ( mode & ~7u ) | 5;
        if ( mode != guest_read32( dev + 12420 ) || ( m & 8 ) ) m |= 8, guest_write32( dev + 12420, mode ), mode_bin = (int32_t)mode;
    }
    if ( !o.empty() ) record( ctx, base, frame, dev, o ), o.clear();  // before what the calls below record
    if ( ( guest_read8( dev + 10813 ) & 1 ) || mismatch )
    {
        if ( m & 0x180000 )
        {
            g_base[dev + 10814] &= 0x7F;
            call( sub_8274FFC0, dev, vs + 872, guest_read32( vs + 32 ) );
            call( sub_8274FE50, dev, mismatch ? 1 : 0, vs, slot, layout, use, psv, variant );
        }
    }
    else if ( m & 0x80000 )
    {
        uint32_t info = vs + 872;
        call( sub_8274FFC0, dev, info, guest_read32( vs + 32 ) );
        g_base[dev + 10814] &= 0x7F;
        bool second = ( ( mode ^ old_mode ) & 7 ) && ( guest_read32( info ) & 0x20 );
        if ( !layout || ( guest_read32( info ) & 0x40 ) )
        {
            pass = (int32_t)variant;
            if ( second ) both = 1, g_base[dev + 10814] |= 0x80;
        }
        else if ( flags & 0x80 ) call( sub_8274FE50, dev, 0, vs, slot, layout, use, psv, variant );
        else
        {
            if ( call( sub_82750070, dev, vs, layout, variant ) ) pass = (int32_t)variant;
            else call( sub_8274FE50, dev, 0, vs, slot, layout, use, psv, variant );
            if ( second && call( sub_82750070, dev, vs, layout, 1 ) ) both = 1, g_base[dev + 10814] |= 0x80;
        }
    }
    uint32_t interp0 = ~0u, interp1 = ~0u;  // r29, r28
    if ( m & 0x180000 )
    {
        m |= 0x18000;
        guest_write32( dev + 10404, interp | ps_hi );
        guest_write32( dev + 10400, guest_read32( slot ) | ps_lo );
        if ( g_base[dev + 10814] & 0x80 )
        {
            uint32_t alt = vs + guest_read32( vs + 904 );
            interp0 = guest_read32( alt + 880 ) | ps_lo, interp1 = guest_read32( alt + 884 ) | ps_hi;
        }
    }
    auto variant_shader = [&]( uint32_t v ) {
        uint32_t at = guest_read32( vs + ( v + 112 ) * 8 ) + vs;
        return xenos_shader_find( 0, guest_view( gpu_address( guest_read32( at + 872 ) + guest_read32( vs + 32 ) ) ), ( guest_read32( at + 876 ) >> 2 ) & 0xFFFF );
    };
    auto bin = [&o]( uint32_t which, uint32_t v ) { op_begin( o, OP_BIN, 2 ), o.push_back( which ), o.push_back( v ); };
    if ( flags & 0x40 )
    {
        bin( 0, 0x55555555 ), bin( 1, 0 );
        if ( m & 8 ) m &= ~8ull, op_reg( o, 0x2208, guest_read32( dev + 10452 ) );
        if ( mode_bin != -1 ) op_begin( o, OP_PRED, 1 ), o.push_back( 3 ), op_reg( o, 0x2208, (uint32_t)mode_bin );
        if ( m & 0x10000 )
        {
            m &= 0xFFFFFFFFFFFE7FFFull;
            op_begin( o, OP_REGS, 3 ), o.push_back( 0x2180 ), o.push_back( guest_read32( dev + 10400 ) ), o.push_back( guest_read32( dev + 10404 ) );
        }
        if ( interp0 != ~0u ) op_begin( o, OP_PRED, 1 ), o.push_back( 4 ), op_begin( o, OP_REGS, 3 ), o.push_back( 0x2180 ), o.push_back( interp0 ), o.push_back( interp1 );
        if ( both != -1 )
        {
            op_begin( o, OP_PRED, 1 ), o.push_back( 3 ), op_shader( o, variant_shader( (uint32_t)both ) );
            if ( pass != -1 ) bin( 0, 0xAAAAAAAA ), op_begin( o, OP_PRED, 1 ), o.push_back( 3 ), op_shader( o, variant_shader( (uint32_t)pass ) );
        }
        else if ( pass != -1 ) op_shader( o, variant_shader( (uint32_t)pass ) );
        bin( 0, guest_read32( dev + 12428 ) ), bin( 1, guest_read32( dev + 12432 ) );
    }
    else if ( pass != -1 ) op_shader( o, variant_shader( (uint32_t)pass ) );
    record( ctx, base, frame, dev, o );
    finish( m );
}

// The depth state the pixel shader and depth control decide (8274EAF0: device, dirty mask; returns it
// with 0x100, RB_COLORCONTROL, dirty). +10432 bit 0: depth writes allowed by +10814 bits 6-7 and depth
// control (+10420) bit 1; bit 1: the early depth test (hierarchical Z) usable: the surface allows it
// (+11572 bits 20-21), the pixel shader doesn't kill pixels or write depth (its +48 bit 27), the depth
// function suits, and stencil (if on) neither fails nor writes. Turning early depth back on wants the GPU
// to flush its depth tiles (event 15, which the renderer has no use for); +10813 bit 3 remembers when
// a switch the other way may need it.
extern "C" PPC_FUNC( __imp__sub_8274EAF0 );
PPC_FUNC( sub_8274EAF0 )
{
    PPCContext &c = *regs( ctx );
    uint32_t dev = c.r3.u32;
    if ( !can_record( c, dev ) ) return __imp__sub_8274EAF0( ctx, base, frame );
    uint64_t m = c.r4.u64;
    uint32_t ps = guest_read32( dev + 12412 ), kills = 0;
    if ( ps ) kills = ( guest_read32( guest_read32( ps + 64 ) + ps + 48 ) >> 27 ) & 1;
    uint32_t surface = ( guest_read32( dev + 11572 ) >> 20 ) & 0xFFF, depth = guest_read32( dev + 10420 );
    uint32_t writes = ( depth >> 1 ) & 0x7FFFFFFF;
    uint32_t v = ( ( guest_read8( dev + 10814 ) >> 6 ) & writes & 1 ) | ( guest_read32( dev + 10432 ) & ~1u );
    guest_write32( dev + 10432, v );
    uint32_t early;
    if ( !( surface & 2 ) ) early = surface & 1;
    else
    {
        uint32_t func = ( depth >> 4 ) & 7;
        bool suits = ( v & 0x10 ) ? ( func == 4 || func == 6 || func == 2 ) : ( func == 1 || func == 3 || func == 2 );
        early = ( kills == 0 ) & v & writes & ( suits ? 1 : 0 );
        if ( depth & 1 )
        {
            early &= !( depth & 0x3800 ) & !( depth & 0xE0000 );
            if ( depth & 0x80 ) early &= !( depth & 0xE0000000 ) & !( depth & 0x0E000000 );
        }
    }
    v = ( guest_read32( dev + 10432 ) & ~2u ) | ( early << 1 & 2 );
    guest_write32( dev + 10432, v );
    if ( v & 2 ) g_base[dev + 10813] &= ~8;
    else if ( ( writes & 1 ) && ( depth & 4 ) )
    {
        uint32_t func = ( depth >> 4 ) & 7;
        if ( func == 7 || func == 5 || ( !( v & 0x10 ) ? ( func == 4 || func == 6 ) : ( func == 1 || func == 3 ) ) ) g_base[dev + 10813] |= 8;
    }
    c.r3.u64 = m | 256;
}

namespace
{
// The dirty state a resolve or clear sends before drawing (as D3D's draws do, without the constants,
// fetch constants and shaders: those draws set their own): the shader-derived state, then registers
// 0x2200, 0x2180, 0x2000, 0x2100, 0x2280 and 0x2300 (each group's dirty bits cleared as it goes).
void flush_for_copy( uintptr_t ctx, uintptr_t base, uintptr_t frame, uint32_t dev )
{
    PPCContext &c = *regs( ctx );
    auto call = [&]( void ( *f )( uintptr_t, uintptr_t, uintptr_t ), uint64_t r3, uint64_t r4, uint64_t r5 = 0, uint64_t r6 = 0 ) {
        c.r3.u64 = r3, c.r4.u64 = r4, c.r5.u64 = r5, c.r6.u64 = r6;
        f( ctx, base, frame );
        return c.r3.u64;
    };
    if ( uint64_t m = read64( dev + 16 ) )
    {
        if ( read64( dev + 40 ) & m ) write64( dev + 16, call( sub_8274ECD8, dev, m, guest_read32( dev + 10432 ) ) );
        m = read64( dev + 16 );
        if ( m & 0xFFF ) call( sub_8274F070, dev, m << 52, 0x2200, dev + 10420 ), write64( dev + 16, read64( dev + 16 ) & ~0xFFFull );
        m = read64( dev + 16 );
        if ( m & 0x1F000 ) call( sub_8274F070, dev, ( m >> 12 ) << 59, 0x2180, dev + 10400 ), write64( dev + 16, read64( dev + 16 ) & ~0x1F000ull );
        m = read64( dev + 16 );
        if ( m & ( 0xFFFFull << 42 ) ) call( sub_8274F070, dev, ( m >> 42 ) << 48, 0x2000, dev + 10240 ), write64( dev + 16, read64( dev + 16 ) & ~( 0xFFFFull << 42 ) );
        m = read64( dev + 16 );
        if ( m & 0x3FFFFE00000ull ) call( sub_8274F070, dev, ( m >> 21 ) << 43, 0x2100, dev + 10316 ), write64( dev + 16, read64( dev + 16 ) & ~0x3FFFFE00000ull );
    }
    if ( uint64_t m = read64( dev + 24 ); m & ( 0x1FFFFFull << 34 ) )
        call( sub_8274F070, dev, ( m >> 34 ) << 43, 0x2280, dev + 10468 ), write64( dev + 24, read64( dev + 24 ) & ~( 0x1FFFFFull << 34 ) );
    if ( uint64_t m = read64( dev + 32 ); m & 0x3FFFFFFFFFull )
        call( sub_8274F070, dev, m << 26, 0x2300, dev + 10552 ), write64( dev + 32, read64( dev + 32 ) & ~0x3FFFFFFFFFull );
}

// The scissor back to the device's (+10308 top left, +10312 bottom right, 15-bit signed fields).
void restore_scissor( uintptr_t ctx, uintptr_t base, uintptr_t frame, uint32_t dev )
{
    PPCContext &c = *regs( ctx );
    int32_t s0 = guest_read32( dev + 10308 ), s1 = guest_read32( dev + 10312 );
    c.r3.u64 = dev;
    c.r4.s64 = (int32_t)( ( (uint32_t)s0 << 17 | (uint32_t)s0 >> 15 ) & 0xFFFE0000 ) >> 17;
    c.r5.s64 = (int32_t)( (uint32_t)s0 << 1 ) >> 17;
    c.r6.s64 = (int32_t)( ( (uint32_t)s1 << 17 | (uint32_t)s1 >> 15 ) & 0xFFFE0000 ) >> 17;
    c.r7.s64 = (int32_t)( (uint32_t)s1 << 1 ) >> 17;
    sub_82738708( ctx, base, frame );
}

float guest_float( uint32_t a ) { uint32_t v = guest_read32( a ); float f; memcpy( &f, &v, 4 ); return f; }
void guest_write_float( uint32_t a, float f ) { uint32_t v; memcpy( &v, &f, 4 ); guest_write32( a, v ); }

// After a GPU write to a texture: its fence (+8, and +12 as written), or with no fence yet the
// device's list of textures to fence later (+13148, grown by 8274C7D8) if it is tracked (+10784).
// (The entry's top bit is whatever D3D's stack had there: top.)
void texture_written( uintptr_t ctx, uintptr_t base, uintptr_t frame, uint32_t dev, uint32_t tex, uint32_t top )
{
    PPCContext &c = *regs( ctx );
    if ( uint32_t fence = guest_read32( dev + 10780 ) )
    {
        guest_write32( tex + 8, fence ), guest_write32( tex + 12, fence );
        return;
    }
    if ( !( guest_read32( dev + 10784 ) & guest_read32( tex ) ) ) return;
    uint32_t at = guest_read32( dev + 13148 );
    if ( at >= guest_read32( dev + 13152 ) ) c.r3.u64 = dev, sub_8274C7D8( ctx, base, frame ), at = c.r3.u32;
    guest_write32( at, ( top & 0x80000000 ) | 0x40000000 | ( tex >> 2 & 0x3FFFFFFF ) );
    guest_write32( at + 4, ~0u );
    guest_write32( dev + 13148, at + 8 );
}
} // namespace

// D3DDevice_Resolve (82748B58: device, flags, source rect, destination texture, destination point, a,
// b, clear color, clear depth (f1), two more on the stack): copies a render target or the depth buffer
// (flags & 7: 0-3, 4 depth) out of EDRAM into a texture. D3D computes the copy registers (RB_COPY_CONTROL
// / DEST_BASE / DEST_PITCH / DEST_INFO, shadowed at +10648..10660) from the destination's format and
// layout; the copy is a rectangle drawn in copy mode (RB_MODECONTROL 6) with a small vertex shader of
// its own, over the source rect (8-pixel aligned), the scissor opened to it if need be. Under tiling the
// destination base goes per tile (predicated), offset by each tile's position. Flags 0x200 and 0x100
// clear depth and color as part of the copy (827485A8, 82748A80).
extern "C" PPC_FUNC( __imp__sub_82748B58 );
PPC_FUNC( sub_82748B58 )
{
    PPCContext &c = *regs( ctx );
    uint32_t dev = c.r3.u32;
    if ( !can_record( c, dev ) ) return __imp__sub_82748B58( ctx, base, frame );
    uint32_t flags = c.r4.u32, src_rect = c.r5.u32, dest = c.r6.u32, dest_point = c.r7.u32, arg_a = c.r8.u32, arg_b = c.r9.u32, color = c.r10.u32;
    float clear_z = (float)c.f1.f64;
    uint32_t saved_sp = c.r1.u32, stack_a = guest_read32( saved_sp + 92 ), stack_b = guest_read32( saved_sp + 100 );
    uint32_t sp = ( saved_sp - 384 ) & ~15u;  // a frame for what the helpers write through pointers
    guest_write32( sp, saved_sp );
    auto at = [sp]( uint32_t off ) { return sp + off; };
    uint32_t which = flags & 7;
    if ( !( flags & 0x70 ) )
    {
        uint32_t surface = which == 4 ? guest_read32( dev + 12176 ) : guest_read32( dev + ( 3040 + which ) * 4 );
        uint32_t ms = guest_read16( surface + 24 ) & 3;
        flags |= ms == 0 ? 0x10 : ms == 1 ? 0x50 : 0x70;
    }
    const uint32_t consts = 0x82070594;
    float one = guest_float( consts + 8264 );
    if ( !color )
    {
        for ( int i = 0; i < 4; ++i ) guest_write_float( at( 192 ) + i * 4, one );
        color = at( 192 );
    }
    uint32_t info = guest_read32( dest + 40 ), tiled_kind = ( guest_read32( dest + 48 ) >> 9 ) & 3, size = guest_read32( dest + 36 );
    uint32_t extra = ( ( info >> 31 ) << 1 ) + 1, block, width, height;
    if ( tiled_kind == 2 ) width = ( size & 0x7FF ) + extra, block = 8, height = ( ( size >> 11 ) & 0x7FF ) + extra;
    else width = ( size & 0x1FFF ) + extra, block = 1, height = ( ( size >> 13 ) & 0x1FFF ) + extra;
    uint32_t fmt_word = guest_read32( dest + 32 ), layout = guest_read32( dest + 28 ), fmt = fmt_word & 0x3F;
    uint32_t dest_base = fmt_word & 0xFFFFF000, pitch = ( ( layout << 15 | layout >> 17 ) & 0x3FE0 );
    uint32_t bpp = ( guest_read16( 0x820F44D0 + fmt * 2 ) >> 3 ) & 0x1F, rows = 0;
    guest_write32( at( 112 ), height ), guest_write32( at( 116 ), width ), guest_write32( at( 120 ), dest_base ), guest_write32( at( 124 ), block );
    if ( arg_a || arg_b )
    {
        guest_write32( at( 84 ), at( 112 ) ), guest_write32( at( 92 ), at( 144 ) ), guest_write32( at( 100 ), at( 120 ) );
        c.r1.u64 = sp;
        c.r3.u64 = dest, c.r4.u64 = arg_b & ~( block - 1 ), c.r5.u64 = arg_a, c.r6.u64 = at( 128 ), c.r7.u64 = at( 176 ), c.r8.u64 = at( 160 ), c.r9.u64 = at( 152 ),
        c.r10.u64 = at( 116 );
        sub_827370E8( ctx, base, frame );
        c.r1.u64 = saved_sp;
        height = guest_read32( at( 112 ) ), dest_base = guest_read32( at( 120 ) ), pitch = guest_read32( at( 128 ) ) / bpp;
    }
    if ( !src_rect )
    {
        src_rect = at( 176 );
        guest_write32( src_rect, 0 ), guest_write32( src_rect + 4, 0 ), guest_write32( src_rect + 8, guest_read32( at( 116 ) ) ), guest_write32( src_rect + 12, height );
    }
    if ( !dest_point ) dest_point = 0x82056CB4;
    int32_t sx = guest_read32( src_rect ), sy = guest_read32( src_rect + 4 ), dx = guest_read32( dest_point ), dy = guest_read32( dest_point + 4 );
    uint32_t endian = ( layout >> 2 ) & 3;
    rows = height - dy;
    guest_write32( at( 128 ), rows );
    uint32_t copy_base = ( (uint32_t)( dy - sy ) * pitch + ( (uint32_t)( dx - sx ) << 5 ) ) * bpp + ( ( ( dest_base >> 20 ) + 0x200 ) & 0x1000 ) + ( dest_base & 0x1FFFFFFF );
    uint32_t copy_fmt = fmt;
    switch ( fmt )
    {
    case 54: copy_fmt = 7; break;
    case 55: copy_fmt = 16; break;
    case 56: copy_fmt = 17; break;
    case 27: copy_fmt = 30; break;
    case 28: copy_fmt = 31; break;
    case 29: copy_fmt = 32; break;
    case 22: case 23: copy_fmt = 6; break;
    }
    uint32_t swap = ( endian == 1 ) ? 1 : 0;
    if ( ( info & 1 ) == 1 ) swap += 2;
    if ( copy_fmt == 30 || copy_fmt == 31 || copy_fmt == 32 || copy_fmt == 36 || copy_fmt == 37 || copy_fmt == 38 || copy_fmt == 57 || copy_fmt == 63 ) swap = 7;
    uint32_t sel = ( ( flags >> 26 ) & 0x3F & ~0x100u ) | ( ( info & 0xE ) ? 0x100 : 0 );
    swap = ( swap & 7 ) | ( ( sel & 0x13F ) << 3 );
    uint32_t dest_pitch = ( rows << 16 & 0x3FFF0000 ) | ( pitch & 0x3FFF );
    uint32_t dest_info = ( ( ( copy_fmt & 0x3F ) | swap << 6 ) << 7 ) | ( ( fmt_word >> 6 ) & 3 );
    if ( tiled_kind == 2 )
    {
        guest_write32( dev + 10692, pitch * height );
        dest_info |= ( ( ( block - 1 ) & arg_b ) << 4 & 0x70 ) | 8;
        write64( dev + 32, read64( dev + 32 ) | 4 );
    }
    uint32_t sample = ( flags & 0x2000 ) ? 3 : which == 4 ? 0 : 1;
    uint32_t control = ( sample << 20 & 0x300000 ) | ( ( flags & 0x377 ) - 16 );
    guest_write32( dev + 10660, dest_info ), guest_write32( dev + 10652, copy_base ), guest_write32( dev + 10656, dest_pitch ), guest_write32( dev + 10648, control );
    write64( dev + 32, read64( dev + 32 ) | 0x3C00 );
    bool clear_depth = flags & 0x200;
    if ( clear_depth )
    {
        c.r3.u64 = dev, c.r4.u64 = stack_b, c.r5.u64 = info & 1, c.r6.u64 = stack_a, c.f1.f64 = clear_z;
        sub_827485A8( ctx, base, frame );
    }
    if ( flags & 0x100 )
    {
        uint32_t v = color & ~0xFu;
        for ( int i = 0; i < 16; ++i ) c.v1.u8[i] = g_base[v + 15 - i];
        c.r3.u64 = dev, c.r5.u64 = control & 7;
        sub_82748A80( ctx, base, frame );
    }
    int32_t x0 = guest_read32( src_rect ), y0 = guest_read32( src_rect + 4 );
    int32_t x1 = ( guest_read32( src_rect + 8 ) + 7 ) & ~7u, y1 = ( guest_read32( src_rect + 12 ) + 7 ) & ~7u;
    bool opened = false;
    {
        uint32_t s0 = guest_read32( dev + 10308 ), s1 = guest_read32( dev + 10312 );
        int32_t tlx = (int32_t)( s0 << 17 ) >> 17, tly = (int32_t)( s0 << 1 ) >> 17, brx = (int32_t)( s1 << 17 ) >> 17, bry = (int32_t)( s1 << 1 ) >> 17;
        if ( x0 < tlx || y0 < tly || x1 > brx || y1 > bry )
        {
            c.r3.u64 = dev, c.r4.s64 = x0, c.r5.s64 = y0, c.r6.s64 = x1, c.r7.s64 = y1;
            sub_82738708( ctx, base, frame );
            opened = true;
        }
    }
    flush_for_copy( ctx, base, frame, dev );

    Ops ops_;
    auto &o = ops_.v;
    auto bin = [&o]( uint32_t w, uint32_t v ) { op_begin( o, OP_BIN, 2 ), o.push_back( w ), o.push_back( v ); };
    if ( tiling( dev ) )
    {
        bin( 1, 0 );
        for ( uint32_t k = 0, n = guest_read32( dev + 12468 ); k < n; ++k )
        {
            uint32_t tx = guest_read32( dev + 12712 + k * 8 ), ty = guest_read32( dev + 12716 + k * 8 );
            bin( 0, 3u << ( 2 * k ) );
            op_begin( o, OP_PRED, 1 ), o.push_back( 3 );
            op_reg( o, 0x2319, ( ty * pitch + ( tx << 5 ) ) * bpp + copy_base );
        }
        bin( 0, guest_read32( dev + 12428 ) ), bin( 1, guest_read32( dev + 12432 ) );
    }
    g_base[dev + 10814] &= ~0x10;  // (the depth flushes it asked for: events the renderer has no use for)
    op_shader( o, xenos_shader_find( 0, 0x82066C3C, 15 ) );
    op_begin( o, OP_REGS, 3 ), o.push_back( 0x2180 ), o.push_back( 0x10002 ), o.push_back( 0 );
    op_reg( o, 0x2208, 6 );
    op_reg( o, 0x2200, 0 );
    uint32_t colorcontrol = 0;
    if ( clear_depth )
    {
        uint8_t f = guest_read8( dev + 10814 );
        g_base[dev + 10813] |= 8;
        uint32_t t = ( ( ( f & ~0x10u ) | ( f >> 2 & 0x10 ) ) >> 4 ) & 7 & ~2u;
        colorcontrol = t | ( guest_read32( dev + 10432 ) & ~0xFu );
        write64( dev + 16, read64( dev + 16 ) | 0x20000 );
    }
    op_reg( o, 0x05C8, 0x20000 );
    record( ctx, base, frame, dev, o ), o.clear();
    c.r3.u64 = dev, c.r4.u64 = 256, c.r5.u64 = colorcontrol;
    sub_8274ECD8( ctx, base, frame );
    op_begin( o, OP_REGS, 4 ), o.push_back( 0x2100 ), o.push_back( 0xFFFF ), o.push_back( 0 ), o.push_back( 0 );
    op_begin( o, OP_REGS, 4 ), o.push_back( 0x2204 ), o.push_back( 0x10000 ), o.push_back( 0x10000 ), o.push_back( 768 );
    op_reg( o, 0x2312, 0xFFFF );
    op_reg( o, 0x200D, 0 );
    write64( dev + 16, read64( dev + 16 ) | 1ull << 44 | 1ull << 41 | 1ull << 40 | 128 | 64 | 32 );
    write64( dev + 32, read64( dev + 32 ) | 0x80000 );
    if ( endian != 3 && which != 4 )
    {
        uint32_t rt = which ? which + 1 : 0, surface = guest_read32( dev + ( rt + 2561 ) * 4 );
        if ( ( surface & 0xF0000 ) == 0x10000 )
        {
            op_reg( o, 0x2001 + rt, surface & 0x03FFFFFF & ~0xFF000u );
            write64( dev + 16, read64( dev + 16 ) | ( 1ull << 63 ) >> ( ( rt + 7 ) & 0xFF ) );
        }
    }
    record( ctx, base, frame, dev, o ), o.clear();
    c.r3.u64 = dev, c.r4.u64 = 6, c.r5.u64 = 4;
    sub_82743CD0( ctx, base, frame );
    uint32_t v = c.r3.u32;
    if ( v )
    {
        float half = ( guest_read32( dev + 10560 ) & 1 ) ? one : guest_float( consts );
        float fx0 = (float)x0 - half, fy0 = (float)y0 - half, fx1 = (float)( x1 - x0 ) + fx0, fy1 = (float)( y1 - y0 ) + fy0;
        const float verts[6] = { fx0, fy0, fx1, fy0, fx1, fy1 };
        for ( int i = 0; i < 6; ++i ) guest_write_float( v + i * 4, verts[i] );
        uint32_t fetch[6] = { gpu_address( v ) | 3, 0x1000001A, 0, 0, 0, 0 };
        op_begin( o, OP_REGS, 7 ), o.push_back( 0x4800 ), o.insert( o.end(), fetch, fetch + 6 );
        op_begin( o, OP_REGS, 4 ), o.push_back( 0x5000 ), o.push_back( 0 ), o.push_back( 0 ), o.push_back( 0 );
        uint32_t initiator = 3 << 16 | 0x88;  // a rectangle list's 3 vertices, auto-indexed
        if ( guest_read32( dev + 12436 ) & 1 ) op_draw_predicated( o, initiator, 0, 0 );
        else op_begin( o, OP_DRAW, 3 ), o.push_back( initiator ), o.push_back( 0 ), o.push_back( 0 );
        op_reg( o, 0x2007, guest_read32( dev + 10268 ) );
        record( ctx, base, frame, dev, o ), o.clear();
        if ( opened ) restore_scissor( ctx, base, frame, dev );
        uint32_t bytes = rows * pitch * bpp * block;
        guest_write32( dest + 8, guest_read32( dev + 10780 ) );
        write64( dev + 16, read64( dev + 16 ) | 0x80000 | 8 | 0x10000 | 0x8000 | 2048 | 256 );
        write64( dev + 24, read64( dev + 24 ) | 0x80000000 );
        op_reg( o, 0x0A31, 0x03000100 );
        op_begin( o, OP_REGS, 3 ), o.push_back( 0x0A2F ), o.push_back( ( bytes + 8191 ) & ~4095u ), o.push_back( copy_base & ~4095u );
        guest_write32( dev + 10648, 0 ), guest_write32( dev + 10660, 0 );
        op_reg( o, 0x2318, 0 );
        op_reg( o, 0x231B, 0 );
        record( ctx, base, frame, dev, o );
        texture_written( ctx, base, frame, dev, dest, guest_read32( at( 176 ) ) );
    }
    c.r1.u64 = saved_sp;
}

// The clear's rectangles (827472B8: device, count, rects { x0, y0, x1, y1 }, color (4 floats), -,
// RB_SURFACE_INFO, RB_COLOR_INFO-like 0x2001, RB_DEPTHCONTROL, depth (f1), RB_MODECONTROL on the stack):
// each rectangle three vertices { x, y, z, color } (pixel centres offset by the device's half-pixel
// setting, +10560 bit 0) in memory off the top of the command segment, fetched through slot 0
// (0x4800), drawn as a predicated rectangle list with the clear's surface and depth state.
extern "C" PPC_FUNC( __imp__sub_827472B8 );
PPC_FUNC( sub_827472B8 )
{
    PPCContext &c = *regs( ctx );
    uint32_t dev = c.r3.u32;
    if ( !can_record( c, dev ) ) return __imp__sub_827472B8( ctx, base, frame );
    uint32_t count = c.r4.u32, rects = c.r5.u32, color = c.r6.u32, surface = c.r8.u32, info = c.r9.u32, depth = c.r10.u32;
    uint32_t mode = guest_read32( c.r1.u32 + 84 );
    float z = (float)c.f1.f64, offset = guest_float( ( guest_read32( dev + 10560 ) & 1 ) ? 0x820725DC : 0x82070594 );
    uint32_t dwords = count * 21;
    c.r3.u64 = dev, c.r4.u64 = dwords, c.r5.u64 = 4;
    sub_82743CD0( ctx, base, frame );
    uint32_t v = c.r3.u32;
    if ( !v ) return;
    float rgba[4];
    for ( int i = 0; i < 4; ++i ) rgba[i] = guest_float( color + i * 4 );
    for ( uint32_t r = 0; r < count; ++r )
    {
        int32_t x0 = guest_read32( rects + r * 16 ), y0 = guest_read32( rects + r * 16 + 4 ), x1 = guest_read32( rects + r * 16 + 8 ), y1 = guest_read32( rects + r * 16 + 12 );
        const float xy[3][2] = { { x0 - offset, y0 - offset }, { x1 - offset, y0 - offset }, { x1 - offset, y1 - offset } };
        for ( int k = 0; k < 3; ++k )
        {
            uint32_t at = v + ( r * 21 + k * 7 ) * 4;
            guest_write_float( at, xy[k][0] ), guest_write_float( at + 4, xy[k][1] ), guest_write_float( at + 8, z );
            for ( int i = 0; i < 4; ++i ) guest_write_float( at + 12 + i * 4, rgba[i] );
        }
    }
    Ops ops_;
    auto &o = ops_.v;
    uint32_t fetch[6] = { gpu_address( v ) | 3, 0x10000002 | ( dwords << 2 & 0x03FFFFFC ), 0, 0, 0, 0 };
    op_begin( o, OP_REGS, 7 ), o.push_back( 0x4800 ), o.insert( o.end(), fetch, fetch + 6 );
    op_begin( o, OP_REGS, 4 ), o.push_back( 0x5000 ), o.push_back( 0 ), o.push_back( 0 ), o.push_back( 0 );
    op_begin( o, OP_REGS, 3 ), o.push_back( 0x2000 ), o.push_back( surface ), o.push_back( info );
    uint32_t samples = ( surface >> 16 ) & 3;
    op_reg( o, 0x2301, samples == 0 ? 0 : samples == 1 ? 0x8001 : 0xC003 );
    op_reg( o, 0x2200, depth );
    op_reg( o, 0x2208, mode );
    op_draw_predicated( o, count * 3 << 16 | 0x88, 0, 0 );
    record( ctx, base, frame, dev, o );
    write64( dev + 24, read64( dev + 24 ) | 0x80000000 );
    write64( dev + 16, read64( dev + 16 ) | 1ull << 57 | 1ull << 56 | 2048 | 8 );
    write64( dev + 32, read64( dev + 32 ) | 1ull << 36 );
}

// The clear (82747CC0: device, flags, rect, color (4 floats, or 1.0s), -, a; depth f1): the rect
// clipped to the viewport (+12368.. floats) and, with +11576, the scissor (+12396..); depth and stencil
// (flags 0x30) only with a depth buffer (+12176). The device's dirty state goes (as a resolve's), then
// the clear's own shaders (from D3D's image) and state; depth / stencil alone are one clear
// (827477F8 with target -1), otherwise one per render target flagged (bits 0-3) that is set, the first
// taking depth and stencil along. The scissor comes back after.
extern "C" PPC_FUNC( __imp__sub_82747CC0 );
PPC_FUNC( sub_82747CC0 )
{
    PPCContext &c = *regs( ctx );
    uint32_t dev = c.r3.u32;
    if ( !can_record( c, dev ) ) return __imp__sub_82747CC0( ctx, base, frame );
    uint32_t flags = c.r4.u32, rect = c.r5.u32, color = c.r6.u32, arg_a = c.r8.u32;
    double z = c.f1.f64;
    uint32_t saved_sp = c.r1.u32, sp = ( saved_sp - 240 ) & ~15u;
    guest_write32( sp, saved_sp );
    if ( !color )
    {
        color = sp + 112;
        for ( int i = 0; i < 4; ++i ) guest_write_float( color + i * 4, guest_float( 0x820725DC ) );
    }
    if ( !guest_read32( dev + 12176 ) && !( flags &= ~0x30u ) ) return;
    int32_t vx = (int32_t)guest_float( dev + 12368 ), vy = (int32_t)guest_float( dev + 12372 ), vw = (int32_t)guest_float( dev + 12376 ), vh = (int32_t)guest_float( dev + 12380 );
    int32_t x0 = std::max( (int32_t)guest_read32( rect ), vx ), y0 = std::max( (int32_t)guest_read32( rect + 4 ), vy );
    int32_t x1 = std::min( (int32_t)guest_read32( rect + 8 ), vx + vw ), y1 = std::min( (int32_t)guest_read32( rect + 12 ), vy + vh );
    if ( guest_read32( dev + 11576 ) )
    {
        x0 = std::max( x0, (int32_t)guest_read32( dev + 12396 ) ), y0 = std::max( y0, (int32_t)guest_read32( dev + 12400 ) );
        x1 = std::min( x1, (int32_t)guest_read32( dev + 12404 ) ), y1 = std::min( y1, (int32_t)guest_read32( dev + 12408 ) );
    }
    if ( x1 <= x0 || y1 <= y0 ) return;
    flush_for_copy( ctx, base, frame, dev );
    Ops ops_;
    auto &o = ops_.v;
    op_begin( o, OP_BIN, 2 ), o.push_back( 0 ), o.push_back( guest_read32( dev + 12428 ) );
    op_shader( o, xenos_shader_find( 0, 0x8206BC78, 27 ) );
    op_shader( o, xenos_shader_find( 1, 0x82067394, 9 ) );
    op_begin( o, OP_REGS, 3 ), o.push_back( 0x2180 ), o.push_back( 0x10010001 ), o.push_back( 0 );
    op_begin( o, OP_REGS, 4 ), o.push_back( 0x2100 ), o.push_back( 0xFFFF ), o.push_back( 0 ), o.push_back( 0 );
    op_begin( o, OP_REGS, 4 ), o.push_back( 0x2204 ), o.push_back( 0x10000 ), o.push_back( 0x10000 ), o.push_back( 768 );
    op_reg( o, 0x2312, 0xFFFF );
    op_reg( o, 0x200D, 0 );
    write64( dev + 16, read64( dev + 16 ) | 1ull << 44 | 1ull << 41 | 1ull << 40 | 0x100000 | 0x80000 | 128 | 64 | 32 );
    write64( dev + 32, read64( dev + 32 ) | 0x80000 );
    auto clear = [&]( int32_t target ) {
        guest_write32( sp + 92, arg_a );
        c.r1.u64 = sp;
        c.r3.u64 = dev, c.r4.u64 = flags, c.r5.s64 = target, c.r6.s64 = x0, c.r7.s64 = y0, c.r8.s64 = x1, c.r9.s64 = y1, c.r10.u64 = color, c.f1.f64 = z;
        sub_827477F8( ctx, base, frame );
        c.r1.u64 = saved_sp;
    };
    if ( !( flags & 0xF ) )
    {
        record( ctx, base, frame, dev, o );
        clear( -1 );
    }
    else
    {
        op_reg( o, 0x2104, 0xFFFF );
        op_reg( o, 0x2202, 0 );
        op_reg( o, 0x2201, 0x10001 );
        record( ctx, base, frame, dev, o );
        write64( dev + 16, read64( dev + 16 ) | 1ull << 37 | 1024 | 512 );
        for ( int k = 0; k < 4; ++k )
            if ( ( flags & 1u << k ) && guest_read32( dev + 12160 + k * 4 ) ) clear( k ), flags &= ~0x30u;
    }
    restore_scissor( ctx, base, frame, dev );
    g_base[dev + 10813] |= 8;
    write64( dev + 16, read64( dev + 16 ) | 0x20000 );
}

// One clear (827477F8: device, flags, target (-1: depth / stencil only), x0, y0, x1, y1, color; depth
// f1; stencil value on the stack): the surface's rectangle cleared (OP_CLEAR) -- the colour as the
// console keeps it in the target's format (827486F0, D3D's own encoder, for 32-bit formats), depth
// (flags 0x10) and stencil (0x20). D3D draws a clear as rectangles of its own clear shaders; the
// renderer clears its images. Predicated as D3D's are: once, or under tiling per tile on its bins
// (offset to the tile), with a first pass for the bins 0x55555555 when +10815 and +10812 bit 0x40 ask
// for it. What D3D's clear leaves dirty (its state, and what its rectangle drawing, 827472B8, sets)
// is dirty after.
extern "C" PPC_FUNC( __imp__sub_827477F8 );
PPC_FUNC( sub_827477F8 )
{
    PPCContext &c = *regs( ctx );
    uint32_t dev = c.r3.u32;
    if ( !can_record( c, dev ) ) return __imp__sub_827477F8( ctx, base, frame );
    uint32_t flags = c.r4.u32, color = c.r10.u32;
    int32_t target = c.r5.s32, x0 = c.r6.s32, y0 = c.r7.s32, x1 = c.r8.s32, y1 = c.r9.s32;
    float z = (float)c.f1.f64;
    uint32_t saved_sp = c.r1.u32, stencil = guest_read32( saved_sp + 92 ), sp = ( saved_sp - 304 ) & ~15u;
    guest_write32( sp, saved_sp );
    uint32_t surface = guest_read32( dev + 10240 ), info = 0, lo = 0, hi = 0, what = flags & 0x30;
    if ( target != -1 )
    {
        what |= 1;
        info = guest_read32( guest_read32( dev + ( 3040 + target ) * 4 ) + 28 );
        uint32_t format = info;
        if ( ( format & 0xF0000 ) == 0xA0000 ) format = ( format & ~0xF0000u ) | 0x20000;
        if ( ( format & 0xF0000 ) == 0xC0000 ) format = ( format & ~0xF0000u ) | 0x30000;
        uint32_t kind = ( format >> 16 ) & 0xF;
        float rgba[4];
        for ( int i = 0; i < 4; ++i ) rgba[i] = guest_float( color + i * 4 );
        if ( kind == 15 )  // 32_32_FLOAT
            memcpy( &lo, &rgba[0], 4 ), memcpy( &hi, &rgba[1], 4 );
        else if ( kind == 5 || kind == 7 )  // 16_16_16_16 (_FLOAT): kept as halves here
        {
            auto half = []( float f ) { return (uint32_t)_cvtss_sh( f, 0 ); };
            lo = half( rgba[0] ) | half( rgba[1] ) << 16, hi = half( rgba[2] ) | half( rgba[3] ) << 16;
        }
        else
        {
            uint32_t v = color & ~0xFu;
            for ( int i = 0; i < 16; ++i ) c.v1.u8[i] = g_base[v + 15 - i];
            guest_write32( sp + 132, 0 ), guest_write32( sp + 136, 0 );
            c.r1.u64 = sp;
            c.r3.u64 = kind, c.r4.u64 = ( format >> 20 ) & 0x3F, c.r5.u64 = sp + 132, c.r6.u64 = sp + 136;
            sub_827486F0( ctx, base, frame );
            c.r1.u64 = saved_sp;
            lo = guest_read32( sp + 132 ), hi = guest_read32( sp + 136 );
        }
    }
    uint32_t colorcontrol = guest_read32( dev + 10432 ) & ~0xFu;
    if ( flags & 0x10 ) colorcontrol |= 1;
    if ( flags & 0x20 ) colorcontrol |= 4;
    c.r3.u64 = dev, c.r4.u64 = 256, c.r5.u64 = colorcontrol;
    sub_8274ECD8( ctx, base, frame );
    Ops ops_;
    auto &o = ops_.v;
    auto clear = [&]( uint32_t w, int32_t a, int32_t b, int32_t cc, int32_t d ) {
        uint32_t zb;
        memcpy( &zb, &z, 4 );
        op_begin( o, OP_PRED, 1 ), o.push_back( 12 );
        op_begin( o, OP_CLEAR, 11 );
        for ( uint32_t v : { w, (uint32_t)a, (uint32_t)b, (uint32_t)cc, (uint32_t)d, surface, info, lo, hi, zb, stencil & 0xFF } ) o.push_back( v );
    };
    auto bin = [&]( uint32_t v ) { op_begin( o, OP_BIN, 2 ), o.push_back( 0 ), o.push_back( v ); };
    uint8_t f0 = guest_read8( dev + 10812 );
    if ( !( f0 & 0x20 ) ) clear( what, x0, y0, x1, y1 );
    else
    {
        uint32_t bins = guest_read32( dev + 12428 );
        if ( ( guest_read8( dev + 10815 ) & 0x40 ) && ( f0 & 0x40 ) && ( bins & 0x55555555 ) )
        {
            if ( flags & 0x30 )
            {
                bin( bins & 0x55555555 );
                int32_t ox = guest_read32( dev + 12712 ), oy = guest_read32( dev + 12716 );
                clear( flags & 0x30, x0 - ox, y0 - oy, x1 - ox, y1 - oy );
            }
            bins &= 0xEAAAAAAA;
        }
        for ( uint32_t k = 0, n = guest_read32( dev + 12468 ); k < n; ++k )
        {
            int32_t tx0 = guest_read32( dev + 12472 + k * 16 ), ty0 = guest_read32( dev + 12476 + k * 16 ), tx1 = guest_read32( dev + 12480 + k * 16 ), ty1 = guest_read32( dev + 12484 + k * 16 );
            int32_t a = std::max( x0, tx0 ), b = std::max( y0, ty0 ), cc = std::min( x1, tx1 ), d = std::min( y1, ty1 );
            if ( a >= cc || b >= d ) continue;
            uint32_t m = ( 3u << ( 2 * k ) ) & bins;
            if ( !m ) continue;
            bin( m );
            int32_t ox = guest_read32( dev + 12712 + k * 8 ), oy = guest_read32( dev + 12716 + k * 8 );
            clear( what, a - ox, b - oy, cc - ox, d - oy );
        }
    }
    record( ctx, base, frame, dev, o );
    write64( dev + 16, read64( dev + 16 ) | 1ull << 57 | 1ull << 56 | 1ull << 55 | 0x10000000 | 2048 | 256 | 8 );
    write64( dev + 24, read64( dev + 24 ) | 0x80000000 );
    write64( dev + 32, read64( dev + 32 ) | 1ull << 36 );
}

// D3D's GPU callback / fence (82744960: device, command pointer, flags, callback, argument; returns the
// command pointer past it). Flags bit 1: the callback (callback and argument in SCRATCH4/5, 0x57C) and
// the hardware threads to interrupt (flags bits 24-29, or 4) in SCRATCH0 (0x578, which the GPU mirrors to
// memory), then waiting for it to land at +10772's address, the interrupt, and 0x45E; unless flags bit 0,
// WAIT_UNTIL (0x5C8) idle before. Flags bit 2: waiting for SCRATCH0 to be 0 again (the callback done),
// then SCRATCH4 back to 0x0BADF00D (no callback). Neither: both. The waits and interrupt are predicated.
extern "C" PPC_FUNC( __imp__sub_82744960 );
PPC_FUNC( sub_82744960 )
{
    PPCContext &c = *regs( ctx );
    uint32_t dev = c.r3.u32;
    if ( !can_record( c, dev ) ) return __imp__sub_82744960( ctx, base, frame );
    uint32_t p = c.r4.u32, flags = c.r5.u32, callback = c.r6.u32, arg = c.r7.u32;
    if ( !( flags & 6 ) ) flags |= 6;
    uint32_t threads = ( flags >> 24 ) & 0x3F, shadow = gpu_address( guest_read32( dev + 10772 ) ) | 2;
    Ops ops_;
    auto &o = ops_.v;
    auto wait = [&o, shadow]( uint32_t ref ) {
        op_begin( o, OP_PRED, 1 ), o.push_back( 5 );
        op_begin( o, OP_WAIT, 4 ), o.push_back( 0x13 ), o.push_back( shadow ), o.push_back( ref ), o.push_back( ~0u );
    };
    if ( flags & 2 )
    {
        g_base[dev + 10815] |= 0x10;
        if ( !( flags & 1 ) ) op_reg( o, 0x05C8, 0x20000 );
        op_begin( o, OP_REGS, 3 ), o.push_back( 0x057C ), o.push_back( callback ), o.push_back( arg );
        if ( !threads ) threads = 4;
        op_reg( o, 0x0578, threads );
        wait( threads );
        op_begin( o, OP_PRED, 1 ), o.push_back( 2 ), op_begin( o, OP_INTERRUPT, 1 ), o.push_back( threads );
        op_reg( o, 0x045E, threads );
    }
    if ( flags & 4 )
    {
        g_base[dev + 10815] &= ~0x10;
        wait( 0 );
        op_reg( o, 0x057C, 0x0BADF00D );
    }
    c.r3.u64 = record_at( dev, p, o );
}

// A command segment's end (82744458: device, where to write (the first word); returns past it): the
// GPU's fences for it, written when the GPU gets there: the segment's position (+48 with +14544's low
// bits) to the shadow +10768 + 4 and the fence count +10780 to +10768; the count goes up by 2. (+12952 /
// +12956 note the position; without a GPU worker (+21156 0, +10813 bit 2) the shadow is written now.)
extern "C" PPC_FUNC( __imp__sub_82744458 );
PPC_FUNC( sub_82744458 )
{
    PPCContext &c = *regs( ctx );
    uint32_t dev = c.r3.u32;
    if ( !can_record( c, dev ) ) return __imp__sub_82744458( ctx, base, frame );
    uint32_t p = c.r4.u32, low = guest_read32( dev + 14544 ), pos = guest_read32( dev + 48 ), fence = guest_read32( dev + 10780 );
    uint32_t shadow = guest_read32( dev + 10768 ), position = ( low & 3 ) | pos;
    guest_write32( dev + 12956, low ), guest_write32( dev + 12952, pos );
    Ops ops_;
    auto &o = ops_.v;
    op_begin( o, OP_FENCE, 2 ), o.push_back( gpu_address( shadow + 4 ) | 2 ), o.push_back( position );
    op_begin( o, OP_FENCE, 2 ), o.push_back( gpu_address( shadow ) | 2 ), o.push_back( fence );
    uint32_t next = record_at( dev, p - 4, o ) + 4;
    if ( !guest_read32( dev + 21156 ) && ( guest_read8( dev + 10813 ) & 4 ) ) guest_write32( shadow, fence ), guest_write32( shadow + 4, position );
    guest_write32( dev + 10780, fence + 2 );
    c.r3.u64 = next;
}

// Frame timing marks (82751820: device, start): with timing on (+21272 = 2) and room in its ring of
// slots (+21276 / +21280, 4 frames), the GPU writes a mark (0xDEADBEEF) to the frame's start or end
// slot (+21268) when it gets there.
extern "C" PPC_FUNC( __imp__sub_82751820 );
PPC_FUNC( sub_82751820 )
{
    PPCContext &c = *regs( ctx );
    uint32_t dev = c.r3.u32;
    if ( !can_record( c, dev ) ) return __imp__sub_82751820( ctx, base, frame );
    if ( guest_read32( dev + 21272 ) != 2 ) return;
    uint32_t k = guest_read32( dev + 21280 ) == 3 ? 0 : guest_read32( dev + 21280 ) + 1;
    if ( k == guest_read32( dev + 21276 ) ) return;
    uint32_t slot = k * 2 + ( c.r4.u32 ? 0 : 1 );
    Ops ops_;
    auto &o = ops_.v;
    op_begin( o, OP_FENCE, 2 ), o.push_back( gpu_address( guest_read32( dev + 21268 ) + slot * 4 ) | 2 ), o.push_back( 0xDEADBEEF );
    record( ctx, base, frame, dev, o );
}

// The sync around a present (82745900: device, no wait): frame timing marks around it; a GPU callback
// (82745768) with the display mode (+13220: 0 / 1 -> 1, 2 -> 2, 4 -> 3, 0x80000000 -> 0) and +11572 bits
// 23-29; unless asked not to, SCRATCH1 (0x579) goes 1 and the GPU waits for it to land at +10772 + 4
// before, and for it to go back to 0 (the callback done) after.
extern "C" PPC_FUNC( __imp__sub_82745900 );
PPC_FUNC( sub_82745900 )
{
    PPCContext &c = *regs( ctx );
    uint32_t dev = c.r3.u32;
    if ( !can_record( c, dev ) ) return __imp__sub_82745900( ctx, base, frame );
    uint32_t no_wait = c.r4.u32, slot1 = gpu_address( guest_read32( dev + 10772 ) + 4 ) | 2;
    c.r3.u64 = dev, c.r4.u64 = 1;
    sub_82751820( ctx, base, frame );
    Ops ops_;
    auto &o = ops_.v;
    auto wait = [&o, slot1]( uint32_t ref ) { op_begin( o, OP_WAIT, 4 ), o.push_back( 0x13 ), o.push_back( slot1 ), o.push_back( ref ), o.push_back( ~0u ); };
    if ( !no_wait ) op_reg( o, 0x0579, 1 ), wait( 1 ), record( ctx, base, frame, dev, o ), o.clear();
    uint32_t mode = guest_read32( dev + 13220 ), v;
    switch ( mode )
    {
    case 0: case 1: v = 1; break;
    case 2: v = 2; break;
    case 4: v = 3; break;
    case 0x80000000: v = 0; break;
    default: v = guest_read32( c.r1.u32 - 128 + 80 ); break;  // (what D3D's stack held there)
    }
    // 60 fps (settings fps60): the swap at the next vblank, not the second. The game times its frame by the
    // vblanks since the last one (its vblank callback 8235A220 counts them; 823583F0 takes the count each
    // frame), so its clock keeps time at either rate.
    // The game's own frame limiter (8272F7C0: after a frame it sleeps until 1000 / *82C3A9A4 ms have passed
    // since it began; 32 from the start) is told 60 too, or it holds the game near 32 whatever the swap.
    const bool fps60 = settings().fps60;
    if ( v == 2 && fps60 ) v = getenv( "CRACKDOWN_SWAP_IMMEDIATE" ) ? 0 : 1;
    if ( uint32_t limit = guest_read32( 0x82C3A9A4 ); fps60 ? limit != 60 : limit == 60 ) guest_write32( 0x82C3A9A4, fps60 ? 60 : 32 );
    // The game's frame clock (the object at *82E79A14) steps a fixed 33 ms a frame while its byte +13 is
    // set (8235A750 pins its least and most step, +40 and +44, to 33; 825B7020 and the frame-time readers
    // take 33 / 1/30 s), whatever the real time: at 60 frames that ran the game at twice its speed. Clear,
    // the clock steps the measured time (ms at 82E2C268, the game's own variable mode) within +40..+44:
    // 1 ms..33 ms here, so a slow frame still slows the game no more than the console does at 30.
    // (Back at 30 only if this changed it.)
    if ( static bool freed; uint32_t t = guest_read32( 0x82E79A14 ) )
    {
        if ( fps60 && ( g_base[t + 13] || guest_read32( t + 40 ) != 1 ) )
            g_base[t + 13] = 0, guest_write32( t + 40, 1 ), guest_write32( t + 44, 33 ), freed = true;
        else if ( !fps60 && freed )
            g_base[t + 13] = 1, guest_write32( t + 40, 33 ), guest_write32( t + 44, 33 ), freed = false;
    }
    if ( static const bool log = getenv( "CRACKDOWN_SWAP_LOG" ); log )
    {
        static int n;
        if ( n++ < 20 ) rt_log( "swap: present sync mode %08X -> %u vblanks, arg %08X\n", mode, v, ( ( guest_read32( dev + 11572 ) >> 23 ) & 0x7F ) | ( v << 8 ) | no_wait );
        if ( n % 30 == 0 )
            if ( uint32_t t = guest_read32( 0x82E79A14 ) )
                rt_log( "dt: fixed %u min %u max %u ms %u\n", g_base[t + 13], guest_read32( t + 40 ), guest_read32( t + 44 ), guest_read32( 0x82E2C268 ) );
    }
    uint32_t arg = ( ( guest_read32( dev + 11572 ) >> 23 ) & 0x7F ) | ( v << 8 ) | no_wait;
    c.r3.u64 = dev, c.r4.u64 = guest_read32( dev + 48 ), c.r5.u64 = 0, c.r6.u64 = 0x82745768, c.r7.u64 = arg;
    sub_82744960( ctx, base, frame );
    guest_write32( dev + 48, c.r3.u32 );
    if ( !no_wait ) wait( 0 ), record( ctx, base, frame, dev, o );
    c.r3.u64 = dev, c.r4.u64 = 0;
    sub_82751820( ctx, base, frame );
    g_base[dev + 10813] |= 32;
}

// The display's mode for a front buffer (82745AA0: device, its format): 0x1841 bits 8-10 and 0x1930
// for the one 10:10:10:2 format (0x28280136) the display handles specially.
extern "C" PPC_FUNC( __imp__sub_82745AA0 );
PPC_FUNC( sub_82745AA0 )
{
    PPCContext &c = *regs( ctx );
    uint32_t dev = c.r3.u32;
    if ( !can_record( c, dev ) ) return __imp__sub_82745AA0( ctx, base, frame );
    bool special = c.r4.u32 == 0x28280136;
    Ops ops_;
    auto &o = ops_.v;
    op_begin( o, OP_RMW, 3 ), o.push_back( 0x1841 ), o.push_back( 0xFFFFF8FF ), o.push_back( special ? 0x100 : 0 );
    op_reg( o, 0x1930, special ? 3 : 0 );
    record( ctx, base, frame, dev, o );
}

// A new frame's state (82738548: device, keep): unless keep, the bins back to the device's mask with
// every bin selected, the GPU idle (WAIT_UNTIL) and 0x0F01 from +10788; the scissor back; everything
// dirty.
extern "C" PPC_FUNC( __imp__sub_82738548 );
PPC_FUNC( sub_82738548 )
{
    PPCContext &c = *regs( ctx );
    uint32_t dev = c.r3.u32;
    if ( !can_record( c, dev ) ) return __imp__sub_82738548( ctx, base, frame );
    if ( !c.r4.u32 )
    {
        Ops ops_;
    auto &o = ops_.v;
        auto bin = [&o]( uint32_t which, uint32_t v ) { op_begin( o, OP_BIN, 2 ), o.push_back( which ), o.push_back( v ); };
        bin( 0, guest_read32( dev + 12428 ) ), bin( 2, ~0u ), bin( 1, guest_read32( dev + 12432 ) ), bin( 3, 0 );
        op_reg( o, 0x05C8, 0x20000 );
        op_reg( o, 0x0F01, guest_read32( dev + 10788 ) );
        record( ctx, base, frame, dev, o );
    }
    restore_scissor( ctx, base, frame, dev );
    for ( int i = 0; i < 5; ++i ) write64( dev + i * 8, ~0ull );
}

// D3DDevice_BlockUntilIdle (82744B70: device): the GPU to idle (WAIT_UNTIL), then the CPU waits for the
// last fence (82744888) and for the GPU worker's pending count (+10872) to drain.
extern "C" PPC_FUNC( __imp__sub_82744B70 );
PPC_FUNC( sub_82744B70 )
{
    PPCContext &c = *regs( ctx );
    uint32_t dev = c.r3.u32;
    if ( !can_record( c, dev ) ) return __imp__sub_82744B70( ctx, base, frame );
    Ops ops_;
    auto &o = ops_.v;
    op_reg( o, 0x05C8, 0x20000 );
    record( ctx, base, frame, dev, o );
    if ( uint32_t fence = guest_read32( dev + 10780 ) ) c.r3.u64 = dev, c.r4.u64 = fence, c.r5.u64 = 4, sub_82744888( ctx, base, frame );
    while ( guest_read32( dev + 10872 ) ) std::this_thread::yield();
}

// Scanning out a front buffer (82745C50: device, flags, address, format, a, b, c, d): the display's
// registers. Flags bit 2 (a new mode): the display is held (0x1973, 0x1964), waited for, its scaler set up
// (82754968 with a, b, c) and released after (0x1838, 0x1852) with the scan-out address (0x1844)
// between; the format's display mode (82745AA0); flags bit 1: the gamma ramp (82745B40 with d). With +21912
// bit 2 the present's sync (82745900) goes first. The renderer presents from the swap; these are kept
// for the register state.
extern "C" PPC_FUNC( __imp__sub_82745C50 );
PPC_FUNC( sub_82745C50 )
{
    PPCContext &c = *regs( ctx );
    uint32_t dev = c.r3.u32;
    if ( !can_record( c, dev ) ) return __imp__sub_82745C50( ctx, base, frame );
    uint32_t flags = c.r4.u32, address = c.r5.u32, format = c.r6.u32, a = c.r7.u32, b = c.r8.u32, cc = c.r9.u32, d = c.r10.u32;
    if ( guest_read32( dev + 21912 ) & 4 ) c.r3.u64 = dev, c.r4.u64 = 0, sub_82745900( ctx, base, frame );
    Ops ops_;
    auto &o = ops_.v;
    auto held = [&o]() { op_begin( o, OP_WAIT, 4 ), o.push_back( 3 ), o.push_back( 0x1973 ), o.push_back( 0 ), o.push_back( 1 ); };
    bool mode = flags & 4;
    if ( mode )
    {
        op_reg( o, 0x1973, 0x10000 ), op_reg( o, 0x1964, 1 ), op_reg( o, 0x1973, 0 );
        held();
        op_reg( o, 0x1852, 0 ), op_reg( o, 0x1838, 1 );
        record( ctx, base, frame, dev, o ), o.clear();
        c.r3.u64 = dev, c.r4.u64 = a, c.r5.u64 = b, c.r6.u64 = cc;
        sub_82754968( ctx, base, frame );
    }
    c.r3.u64 = dev, c.r4.u64 = format;
    sub_82745AA0( ctx, base, frame );
    op_reg( o, 0x1844, gpu_address( address ) );
    if ( mode ) op_reg( o, 0x1838, 0 ), held(), op_reg( o, 0x1852, 1 );
    record( ctx, base, frame, dev, o );
    if ( flags & 2 ) c.r3.u64 = dev, c.r4.u64 = format, c.r5.u64 = d, sub_82745B40( ctx, base, frame );
}

// D3DDevice_Swap (82745E30: device, the front buffer texture, the display settings or 0). Frame count
// (+16184) up; the front buffer's fetch constant (+28, 24 bytes, its format 50 / 7 made 3 / 27 for the
// display) and description (52 bytes to +14468). With frame timing (+21248) and room in its ring of
// slots (+21240 / +21244, 8), the GPU marks the swap's start and end slots (0xDEADBEEF fences, +10768
// + 64). New display settings (28 bytes, compared with +13264) are taken (82754B38). The present
// itself: VdSwap in 64 words D3D takes from the command buffer (827437F0), unless +21156 (no
// presenting). Then the scan-out: with display changes (+16332 bit 3: a mode set, VdSetDisplayMode /
// VdGetCurrentDisplayInformation; a new gamma ramp, +13292; the 10:10:10:2 format) through 82745C50,
// otherwise the present's sync (82745900, +21912 bit 2) or the scan-out address (0x1844) and its
// format's display mode (82745AA0); after a mode set, 0x1E4E bit 0 cleared and VdPersistDisplay. Then
// D3D's end of frame: the buffer kicked (827447A0), the bins (82756A98), the CPU's frame fence
// (82743E68, 82744888), with +21912 bit 2 the wait for the GPU to be at most 15 frames behind
// (82745278 / 827451B8), the new frame's state (82751900, 82738548).
extern "C" PPC_FUNC( __imp__sub_82745E30 );
PPC_FUNC( sub_82745E30 )
{
    PPCContext &c = *regs( ctx );
    uint32_t dev = c.r3.u32;
    if ( !can_record( c, dev ) ) return __imp__sub_82745E30( ctx, base, frame );
    uint32_t front = c.r4.u32, settings = c.r5.u32;
    uint32_t saved_sp = c.r1.u32, sp = saved_sp - 560;
    guest_write32( sp, saved_sp );
    auto copy = []( uint32_t dst, uint32_t src, uint32_t n ) { memmove( g_base + dst, g_base + src, n ); };
    auto call = [&]( auto fn ) { c.r1.u64 = sp; fn( ctx, base, frame ); c.r1.u64 = saved_sp; };
    auto room = [&]() {  // D3D's check that the command buffer has room (it may kick)
        if ( guest_read32( dev + 48 ) > guest_read32( dev + 56 ) ) c.r3.u64 = dev, call( sub_827447A0 ), guest_write32( dev + 48, c.r3.u32 );
    };
    Ops ops_;
    auto &o = ops_.v;

    guest_write32( dev + 16184, guest_read32( dev + 16184 ) + 1 );
    copy( sp + 128, front + 28, 24 );
    uint32_t fetch1 = guest_read32( sp + 132 ), r10 = guest_read32( dev + 13232 );
    if ( ( fetch1 & 63 ) == 50 ) guest_write32( sp + 132, fetch1 = ( fetch1 & ~63u ) | 6 );
    else if ( ( fetch1 & 63 ) == 7 ) guest_write32( sp + 132, fetch1 = ( fetch1 & ~63u ) | 54 );
    uint32_t r11 = ( ( r10 << 15 ) | ( r10 >> 17 ) ) & 0x7FFF;
    uint32_t r8 = guest_read32( sp + 128 ), r9 = guest_read32( sp + 140 );
    r8 = ( r8 & ~0x3FCu ) | ( ( ( r10 << 25 ) | ( r10 >> 7 ) ) & 0x3FC );
    uint32_t t = ( r11 & 0x1FFE ) | ( r9 & 0xFFFFE001u );
    r11 = ( t & 0xFFFFFFFEu ) | ( r11 & 1 );
    guest_write32( sp + 128, r8 );
    guest_write32( sp + 140, r11 );
    copy( dev + 14468, front, 52 );
    uint32_t r23 = guest_read32( dev + 13236 ), size = guest_read32( sp + 136 ), add = ( ( r11 >> 31 ) << 1 ) + 1;
    guest_write32( sp + 108, r23 );
    guest_write32( sp + 100, fetch1 & 0xFFFFF000u );
    guest_write32( sp + 112, ( size & 0x1FFF ) + add );
    guest_write32( sp + 104, ( ( size >> 13 ) & 0x1FFF ) + add );
    guest_write32( sp + 96, fetch1 & 63 );
    auto slot = [&]( uint32_t k ) { return guest_read32( dev + 10768 ) + ( ( k & 7 ) + 16 ) * 4; };
    auto mark = [&]( uint32_t at ) { op_begin( o, OP_FENCE, 2 ), o.push_back( gpu_address( at ) ), o.push_back( 0xDEADBEEF ); };
    bool marks = false;
    if ( guest_read32( dev + 21248 ) && guest_read32( dev + 21244 ) - guest_read32( dev + 21240 ) < 6 )
    {
        uint32_t k = guest_read32( dev + 21244 );
        guest_write32( slot( k ), 0 ), guest_write32( slot( k + 1 ), 0 );
        mark( slot( k ) ), record( ctx, base, frame, dev, o ), o.clear();
        marks = true;
    }
    bool new_settings = false;
    if ( settings ) new_settings = memcmp( g_base + settings, g_base + dev + 13264, 28 ) != 0;
    if ( guest_read32( 0x82E7BC4C ) ) room();
    if ( new_settings ) guest_write32( dev + 21896, guest_read32( dev + 21896 ) | 0x40000000 );
    c.r3.u64 = dev, c.r4.u64 = front, call( sub_827569A8 );
    if ( guest_read32( dev + 21156 ) ) memset( g_base + sp + 208, 0, 148 );
    else
    {
        c.r3.u64 = sp + 208, c.r4.u64 = sp + 116, call( __import__VdGetSystemCommandBuffer );
        c.r3.u64 = dev, c.r4.u64 = 64, call( sub_827437F0 );
        uint32_t words = c.r3.u32;
        guest_write32( sp + 84, sp + 112 ), guest_write32( sp + 92, sp + 104 );
        c.r3.u64 = words + 4, c.r4.u64 = sp + 128, c.r5.u64 = guest_read32( dev + 10768 ) + 8, c.r6.u64 = sp + 208, c.r7.u64 = guest_read32( sp + 116 );
        c.r8.u64 = sp + 100, c.r9.u64 = sp + 96, c.r10.u64 = sp + 108;
        call( __import__VdSwap );
        guest_write32( dev + 48, words + 256 );
    }
    guest_write32( dev + 21288, guest_read32( sp + 212 ) );
    if ( marks )
    {
        mark( slot( guest_read32( dev + 21244 ) + 1 ) ), record( ctx, base, frame, dev, o ), o.clear();
        guest_write32( dev + 21244, guest_read32( dev + 21244 ) + 2 );
    }
    if ( uint32_t v = guest_read32( sp + 216 ) ) guest_write32( dev + 21908, v );
    uint32_t changes = guest_read32( dev + 16332 );
    guest_write32( dev + 16332, 0 );
    if ( changes & 8 )  // a display mode set
    {
        uint32_t f = guest_read32( dev + 13232 ) & 63, h = guest_read32( dev + 13236 );
        uint32_t mode = 0x40000000u | ( ( ( h << 28 ) | ( h >> 4 ) ) & 0x30000000u ) | ( f == 7 || f == 54 ? 0x08000000u : 0 );
        if ( guest_read32( dev + 13168 ) == 720 && ( guest_read32( dev + 13172 ) == 480 || guest_read32( dev + 13172 ) == 576 ) ) mode |= 2;
        c.r3.u64 = mode, call( __import__VdSetDisplayMode );
        c.r3.u64 = sp + 368, call( __import__VdGetCurrentDisplayInformation );
        guest_write32( dev + 21164, guest_read16( sp + 440 ) );
        guest_write32( dev + 21168, guest_read16( sp + 442 ) );
        guest_write32( dev + 21172, guest_read16( sp + 454 ) );
    }
    if ( new_settings )
    {
        copy( dev + 13264, settings, 28 );
        c.r3.u64 = dev, c.r4.u64 = settings, call( sub_82754B38 );
    }
    uint32_t scanout = gpu_address( guest_read32( sp + 100 ) ), format, gamma, scaler = 0;
    uint8_t flags1 = guest_read8( dev + 10814 ), flags3 = guest_read8( dev + 10815 );
    if ( guest_read32( sp + 216 ) && guest_read32( sp + 108 ) == r23 )
    {
        gamma = dev + 13292;
        format = guest_read32( sp + 96 ) == 7 ? 0x28280136 : 0x28280106;
        bool ramp = false;
        if ( guest_read32( sp + 236 ) && memcmp( g_base + sp + 240, g_base + dev + 13292, 56 ) )
        {
            copy( dev + 13292, sp + 240, 56 );
            g_base[dev + 10814] = flags1 | 32;
            ramp = true;
        }
        else if ( ( flags1 & 32 ) && !guest_read32( sp + 352 ) )
            gamma = dev + 13348, ramp = true;
        if ( ramp ) changes |= 4;
        if ( !( flags3 & 0x80 ) ) changes |= 3;
        flags3 = ( flags3 & 0x7F ) | 0x80;
    }
    else
    {
        gamma = dev + 13348;
        format = guest_read32( dev + 13232 );
        scaler = dev + 14628;
        if ( flags1 & 32 )
        {
            memset( g_base + dev + 13292, 0, 56 );
            changes |= 4;
            g_base[dev + 10814] = guest_read8( dev + 10814 ) & 0xDF;
        }
        if ( flags3 & 0x80 ) changes |= 3;
        flags3 &= 0x7F;
    }
    g_base[dev + 10815] = flags3;
    if ( changes )
    {
        c.r3.u64 = dev, c.r4.u64 = changes, c.r5.u64 = scanout, c.r6.u64 = format, c.r7.u64 = guest_read32( sp + 112 ), c.r8.u64 = guest_read32( sp + 104 );
        c.r9.u64 = gamma, c.r10.u64 = scaler;
        call( sub_82745C50 );
    }
    else if ( guest_read32( dev + 21912 ) & 4 ) c.r3.u64 = dev, c.r4.u64 = scanout, call( sub_82745900 );
    else
    {
        op_reg( o, 0x1844, scanout ), record( ctx, base, frame, dev, o ), o.clear();
        c.r3.u64 = dev, c.r4.u64 = format, call( sub_82745AA0 );
    }
    if ( changes & 8 )
    {
        op_begin( o, OP_RMW, 3 ), o.push_back( 0x1E4E ), o.push_back( 0xFFFFFFFE ), o.push_back( 0 );
        record( ctx, base, frame, dev, o ), o.clear();
        memset( g_base + sp + 184, 0, 12 );
        c.r3.u64 = sp + 184, c.r4.u64 = sp + 160, call( __import__VdPersistDisplay );
        if ( c.r3.u32 ) c.r3.u64 = 1, c.r4.u64 = guest_read32( sp + 160 ), call( __import__MmFreePhysicalMemory );
    }
    uint32_t fence = guest_read32( dev + 10780 );
    c.r3.u64 = dev, call( sub_827447A0 );
    c.r3.u64 = dev, c.r4.u64 = 0, c.r5.u64 = 0, call( sub_82756A98 );
    c.r3.u64 = dev, c.r4.u64 = guest_read32( dev + 14536 ) >> 2, call( sub_82743E68 );
    if ( uint32_t f = guest_read32( dev + 14564 ) ) c.r3.u64 = dev, c.r4.u64 = f, c.r5.u64 = 3, call( sub_82744888 );
    if ( guest_read32( dev + 21912 ) & 4 )
    {
        uint32_t w = sp + 160;
        guest_write32( w, dev ), guest_write32( w + 4, 3 ), guest_write32( w + 8, guest_read32( guest_read32( dev + 10768 ) ) );
        uint32_t thread = guest_read32( guest_read32( c.r13.u32 + 256 ) + 88 );
        guest_write32( w + 12, thread ), guest_write32( w + 16, thread ), guest_write32( w + 20, (uint32_t)zz_mftb() );
        if ( static const bool log = getenv( "CRACKDOWN_SWAP_LOG" ); log )
        {
            static int n;
            if ( n++ % 60 == 0 )
                rt_log( "swap: issued %u flipped %u vblanks %u flip queue %u..%u\n", guest_read32( dev + 16184 ), guest_read32( dev + 16192 ), guest_read32( dev + 16172 ),
                        guest_read32( dev + 16324 ), guest_read32( dev + 16328 ) );
        }
        do
            c.r3.u64 = w, call( sub_82745278 );
        while ( c.r3.u32 && guest_read32( dev + 16184 ) - guest_read32( dev + 16192 ) >= 15 );
        c.r3.u64 = w, call( sub_827451B8 );
    }
    else guest_write32( dev + 14564, fence );
    g_base[dev + 10813] = guest_read8( dev + 10813 ) & 0xDF;
    c.r3.u64 = dev, call( sub_82751900 );
    c.r3.u64 = dev, c.r4.u64 = 0, call( sub_82738548 );
}

// D3DDevice_BeginTiling(device, flags, tile count, tile rects (D3DRECT: x1, y1, x2, y2), clear colour,
// clear z (f1), clear stencil). The game's tiles become one (their union); then, native: D3D's debug
// hook if one is set (*(*0x820007C0) or *(*0x82000924) + 24: event 43), the tiles (+12468: count,
// +12472: rects, +12712: their corners 32-aligned) and the screen extent (+12900 / +12904); flags bit
// 1 (predicated tiling): the surface pitch for the widest tile (+12892, RB_SURFACE_INFO at +10240,
// +12896) and each tile's EDRAM offset (+12832); unless flags bit 0 or +10815 bit 6, the tile area
// cleared (827483D8: colour, depth, stencil) with the viewport (827389E8) and scissor (8273AEF8) kept
// around it; the clear values (+12912..), the render targets of the tiled frame (+12440..), the bins
// (+12428), the tile list (82757570, +13000); the scissor back (82738708) and everything dirty. D3D
// also wrote an EVENT_WRITE (25) starting the tiled frame, which the renderer has no use for: not
// written.
namespace
{
// The game's tile rects as one tile (their union), in memory of its own.
uint32_t one_tile( uint32_t count, uint32_t rects )
{
    static const uint32_t one = rt_alloc_physical( 16, 16 );
    uint32_t x1 = ~0u, y1 = ~0u, x2 = 0, y2 = 0;
    for ( uint32_t i = 0; i < count; ++i )
    {
        uint32_t r = rects + i * 16;
        x1 = std::min( x1, guest_read32( r ) ), y1 = std::min( y1, guest_read32( r + 4 ) );
        x2 = std::max( x2, guest_read32( r + 8 ) ), y2 = std::max( y2, guest_read32( r + 12 ) );
    }
    guest_write32( one, x1 ), guest_write32( one + 4, y1 ), guest_write32( one + 8, x2 ), guest_write32( one + 12, y2 );
    static bool logged;
    if ( !logged ) logged = true, rt_log( "native d3d: %u tiles -> one, (%u,%u)-(%u,%u)\n", count, x1, y1, x2, y2 );
    return one;
}
} // namespace

extern "C" PPC_FUNC( __imp__sub_82746820 );
PPC_FUNC( sub_82746820 )
{
    PPCContext &c = *regs( ctx );
    if ( native_d3d() && c.r5.u32 > 1 && c.r6.u32 ) c.r6.u64 = one_tile( c.r5.u32, c.r6.u32 ), c.r5.u64 = 1;
    uint32_t dev = c.r3.u32;
    if ( !can_record( c, dev ) ) return __imp__sub_82746820( ctx, base, frame );
    uint32_t flags = c.r4.u32, count = c.r5.u32, rects = c.r6.u32, color = c.r7.u32, stencil = c.r9.u32;
    double z = c.f1.f64;
    uint32_t saved_sp = c.r1.u32, sp = saved_sp - 272;
    guest_write32( sp, saved_sp );
    auto call = [&]( auto fn ) { c.r1.u64 = sp; fn( ctx, base, frame ); c.r1.u64 = saved_sp; };
    guest_write32( sp + 308, count );

    // D3D's debug hook (event 43 with the tile count).
    bool tiled = flags & 2;
    uint32_t args = sp + ( tiled ? 80 : 96 ), hook = 0;
    guest_write32( args, 1 ), guest_write32( args + 4, tiled ? 92 : 86 ), guest_write32( args + 8, 1 ), guest_write32( args + 12, sp + 308 );
    if ( uint32_t h = guest_read32( guest_read32( 0x820007C0 ) ) ) hook = guest_read32( h );
    else if ( uint32_t h2 = guest_read32( guest_read32( 0x82000924 ) ) ) hook = guest_read32( h2 + 24 );
    if ( hook )
    {
        c.r3.u64 = 43, c.r4.u64 = args;
        c.r1.u64 = sp;
        PPC_CALL_INDIRECT_FUNC( hook );
        c.r1.u64 = saved_sp;
        count = guest_read32( sp + 308 );
    }
    guest_write32( dev + 12468, count );
    uint32_t right = 0, bottom = 0;
    for ( uint32_t i = 0; i < count; ++i )
    {
        uint32_t r = rects + i * 16, x1 = guest_read32( r ), y1 = guest_read32( r + 4 ), x2 = guest_read32( r + 8 ), y2 = guest_read32( r + 12 );
        right = std::max( right, x2 ), bottom = std::max( bottom, y2 );
        guest_write32( dev + 12472 + i * 16, x1 ), guest_write32( dev + 12476 + i * 16, y1 );
        guest_write32( dev + 12480 + i * 16, x2 ), guest_write32( dev + 12484 + i * 16, y2 );
        guest_write32( dev + 12712 + i * 8, x1 & ~31u ), guest_write32( dev + 12716 + i * 8, y1 & ~31u );
    }
    if ( tiled )
    {
        uint8_t f3 = guest_read8( dev + 10815 ) | 64;
        if ( flags & 4 ) f3 |= 32;
        g_base[dev + 10815] = f3;
        uint32_t ms = guest_read16( dev + 10240 ) & 3;
        uint32_t tile_h = ms == 0 ? 16 : 8, tile_w = ms >= 2 ? 16 : 32, align = ms >= 2 ? 40 : 80;
        uint32_t pitch = ( tile_w + right - 1 ) & ~( tile_w - 1 ), per_row = pitch / tile_w;
        for ( uint32_t i = 0; i < count; ++i )
            guest_write32( dev + 12832 + i * 4, guest_read32( dev + 12716 + i * 8 ) / tile_h * per_row + guest_read32( dev + 12712 + i * 8 ) / tile_w );
        guest_write32( dev + 12892, pitch );
        guest_write32( dev + 10240, ( guest_read32( dev + 10240 ) & 0x3FFFF ) | ( pitch << 18 ) );
        write64( dev + 16, read64( dev + 16 ) | 256 );
        guest_write32( dev + 12896, ( align + right - 1 ) / align * align );
        bool keep = !( guest_read8( dev + 10815 ) & 0x40 ) && ( guest_read32( dev + 11572 ) & 0xE0000 ) == 0x40000;
        write64( dev + 40, keep ? read64( dev + 40 ) & ~256ull : read64( dev + 40 ) | 256 );
    }
    guest_write32( dev + 12900, right ), guest_write32( dev + 12904, bottom ), guest_write32( dev + 12908, flags );
    const uint32_t state = 0x820F4550;  // D3D's state block for its clear
    if ( !( flags & 1 ) && !( guest_read8( dev + 10815 ) & 0x40 ) )
    {
        uint32_t w = 0, h = 0;
        for ( uint32_t i = 0; i < count; ++i )
        {
            w = std::max( w, guest_read32( dev + 12480 + i * 16 ) - guest_read32( dev + 12712 + i * 8 ) );
            h = std::max( h, guest_read32( dev + 12484 + i * 16 ) - guest_read32( dev + 12716 + i * 8 ) );
        }
        guest_write32( sp + 112, 0 ), guest_write32( sp + 116, 0 ), guest_write32( sp + 120, w ), guest_write32( sp + 124, h );
        memmove( g_base + sp + 144, g_base + dev + 12368, 28 );  // the viewport
        memmove( g_base + sp + 128, g_base + dev + 12396, 16 );  // the scissor
        c.r3.u64 = dev, c.r4.u64 = state, call( sub_8273AFF8 );
        c.r3.u64 = dev, c.r4.u64 = 0x820F4568, call( sub_8273AEF8 );
        c.r3.u64 = dev, c.r4.u64 = 63, c.r5.u64 = sp + 112, c.r6.u64 = color, c.r8.u64 = stencil, c.f1.f64 = z, call( sub_827483D8 );
        c.r3.u64 = dev, c.r10.u64 = guest_read32( sp + 168 );
        c.f1.f64 = guest_float( sp + 144 ), c.f2.f64 = guest_float( sp + 148 ), c.f3.f64 = guest_float( sp + 152 );
        c.f4.f64 = guest_float( sp + 156 ), c.f5.f64 = guest_float( sp + 160 ), c.f6.f64 = guest_float( sp + 164 );
        call( sub_827389E8 );
        c.r3.u64 = dev, c.r4.u64 = sp + 128, call( sub_8273AEF8 );
    }
    if ( color ) memmove( g_base + dev + 12912, g_base + ( color & ~15u ), 16 );
    else memset( g_base + dev + 12912, 0, 16 );
    guest_write_float( dev + 12928, (float)z );
    guest_write32( dev + 12932, stencil );
    uint32_t rt0 = guest_read32( dev + 12160 );
    guest_write32( dev + 12448, rt0 );
    guest_write32( dev + 12444, rt0 ? rt0 : guest_read32( dev + 12176 ) );
    for ( int k = 0; k < 4; ++k ) guest_write32( dev + 12452 + k * 4, guest_read32( dev + 12164 + k * 4 ) );
    guest_write32( dev + 12440, 2 );
    guest_write32( dev + 12436, 0 );
    uint8_t f0 = guest_read8( dev + 10812 ) | 32;
    g_base[dev + 10812] = f0;
    bool track = !( f0 & 8 ) && ( ( guest_read8( dev + 10815 ) & 0x40 ) || ( !guest_read8( dev + 11841 ) && !guest_read8( dev + 11907 ) ) );
    guest_write32( dev + 12432, 0 );
    guest_write32( dev + 10804, 0 ), guest_write32( dev + 10808, 0 );
    guest_write32( dev + 12428, ~0u );
    g_base[dev + 10812] = ( guest_read8( dev + 10812 ) & ~2 ) | ( track ? 2 : 0 );
    c.r3.u64 = dev, call( sub_82757570 );
    uint32_t list = dev + 13000, at = guest_read32( list + 8 );
    if ( at + 4 > guest_read32( list + 12 ) ) c.r3.u64 = list, call( sub_82743858 ), at = c.r3.u32;
    guest_write32( at, 0x87000000 );
    guest_write32( dev + 13008, at + 4 );
    c.r3.u64 = dev, c.r4.u64 = state, call( sub_8273AFF8 );
    auto half = []( uint32_t v ) { return (int32_t)( v << 1 ) >> 17; };     // bits 15-30
    auto low = []( uint32_t v ) { return (int32_t)( v << 17 ) >> 17; };     // bits 0-14
    uint32_t br = guest_read32( dev + 10312 ), tl = guest_read32( dev + 10308 );
    c.r3.u64 = dev, c.r4.s64 = low( tl ), c.r5.s64 = half( tl ), c.r6.s64 = low( br ), c.r7.s64 = half( br );
    call( sub_82738708 );
    for ( int i = 0; i < 5; ++i ) write64( dev + i * 8, ~0ull );
}

// D3DDevice_BeginIndexedVertices (82742A68: device, primitive, base vertex, vertex count, index count,
// flags (bit 2: 32-bit indices; bits 0-1: their endian swap), stride, where to put the index pointer;
// the vertex pointer's place on the stack; returns 0, or E_OUTOFMEMORY): as BeginVertices, with the
// indices in memory of their own off the top of the command segment and an indexed draw.
extern "C" PPC_FUNC( __imp__sub_82742A68 );
PPC_FUNC( sub_82742A68 )
{
    PPCContext &c = *regs( ctx );
    uint32_t dev = c.r3.u32;
    if ( !can_record( c, dev ) ) return __imp__sub_82742A68( ctx, base, frame );
    bool tiled = guest_read8( dev + 10812 ) & 0x02;
    uint32_t prim = c.r4.u32, base_vertex = c.r5.u32, vertices = c.r6.u32, count = c.r7.u32, flags = c.r8.u32, stride = c.r9.u32, index_out = c.r10.u32;
    uint32_t vertex_out = guest_read32( c.r1.u32 + 84 );
    bool wide = flags & 4;
    uint32_t vdwords = ( vertices * stride ) >> 2, idwords = wide ? count : ( count + 1 ) >> 1;
    uint8_t old = guest_read8( dev + 12248 ), words = ( stride >> 2 ) & 0xFF;
    g_base[dev + 12248] = words;
    if ( words != guest_read8( dev + 11552 ) && guest_read32( dev + 12416 ) ) write64( dev + 16, read64( dev + 16 ) | 0x80000 );
    flush_dirty( ctx, base, frame, dev );
    g_base[dev + 12248] = old;
    if ( guest_read32( dev + 48 ) > guest_read32( dev + 56 ) ) c.r3.u64 = dev, sub_827447A0( ctx, base, frame );
    c.r3.u64 = dev, c.r4.u64 = vdwords, c.r5.u64 = 16;
    sub_82743CD0( ctx, base, frame );
    uint32_t v = c.r3.u32;
    c.r3.u64 = dev, c.r4.u64 = idwords, c.r5.u64 = 16;
    sub_82743CD0( ctx, base, frame );
    uint32_t ix = c.r3.u32;
    if ( !v || !ix )
    {
        guest_write32( index_out, 0 ), guest_write32( vertex_out, 0 );
        c.r3.u64 = 0x8007000E;
        return;
    }
    Ops ops_;
    auto &o = ops_.v;
    op_reg( o, 0x2102, base_vertex );
    const uint32_t fetch[6] = { 0, 0, 0, 0, gpu_address( v ) | 3, 0x10000002 | ( vdwords << 2 & 0x03FFFFFC ) };
    op_begin( o, OP_REGS, 7 ), o.push_back( 0x48BA ), o.insert( o.end(), fetch, fetch + 6 );
    op_begin( o, OP_REGS, 4 ), o.push_back( 0x5000 ), o.push_back( 0 ), o.push_back( 0 ), o.push_back( 0 );
    uint32_t endian = ( flags & 3 ) << 30, initiator = ( prim & 0x3F ) | count << 16, size = ( count & 0xFFFFFF ) | endian;
    if ( wide ) size = ( count << 1 & 0x00FFFFFE ) | endian, initiator |= 0x800;
    op_draw_predicated( o, initiator, gpu_address( ix ), size );
    if ( tiled )
    {
        op_begin( o, OP_BIN, 2 ), o.push_back( 0 ), o.push_back( 0x80000000 );
        record_masked( ctx, base, frame, dev, o );
    }
    else record( ctx, base, frame, dev, o );
    write64( dev + 24, read64( dev + 24 ) | 1 );
    guest_write32( index_out, ix );
    guest_write32( dev + 13076, guest_read32( dev + 48 ) );
    guest_write32( dev + 13080, v ), guest_write32( dev + 13084, ix ), guest_write32( dev + 13088, vdwords ), guest_write32( dev + 13092, idwords );
    guest_write32( vertex_out, v );
    c.r3.u64 = 0;
}

// Before a segment goes to the GPU, D3D flushes the GPU's caches over memory the CPU wrote meanwhile
// (82744290: device, where to put the flush buffer's address, where to put its size in dwords): ranges
// other threads deferred (+11544, { high, low }, empty when its low word is 0xFFFFFFFF) and, with
// +10814 bit 3, the texture range +14528..14532, as a small buffer run ahead of the segment. The
// renderer reads guest memory as it is, with no such caches: the ranges are taken (reset) and there is
// nothing to run (size 0).
extern "C" PPC_FUNC( __imp__sub_82744290 );
PPC_FUNC( sub_82744290 )
{
    PPCContext &c = *regs( ctx );
    uint32_t dev = c.r3.u32;
    if ( !can_record( c, dev ) ) return __imp__sub_82744290( ctx, base, frame );
    auto *range = (uint64_t *)( g_base + dev + 11544 );
    __atomic_store_n( range, __builtin_bswap64( 0xFFFFFFFFull ), __ATOMIC_SEQ_CST );
    guest_write32( c.r5.u32, 0 );
}

// D3D handing a command segment to the GPU (827440F0: device, { dwords, GPU address }), from the device's
// thread or its worker: D3D's capture hooks (+21156's +24 / +28, +21296) see it as before; the
// segment goes to the renderer (gpu_submit_segment) instead of an INDIRECT_BUFFER in the ring and the
// write pointer, so D3D's ring holds only its start-up packets (handed over before the segment, up to
// its position there, +10824). With +10813 bit 2 the hooks only see it, as D3D's.
extern "C" PPC_FUNC( __imp__sub_827440F0 );
PPC_FUNC( sub_827440F0 )
{
    PPCContext &c = *regs( ctx );
    uint32_t dev = c.r3.u32;
    if ( !native_ops() || dev != device() ) return __imp__sub_827440F0( ctx, base, frame );  // (any thread)
    uint32_t list = c.r4.u32, dwords = guest_read32( list ), addr = guest_read32( list + 4 );
    uint32_t virt = addr + ( addr < 0x20000000 ? 0xC0000000u : 0xBF000000u );  // as D3D gives its hooks
    uint32_t saved_sp = c.r1.u32, sp = ( saved_sp - 112 ) & ~15u;
    guest_write32( sp, saved_sp );
    auto call = [&]( uint32_t fn, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6 ) {
        c.r1.u64 = sp, c.r3.u64 = a3, c.r4.u64 = a4, c.r5.u64 = a5, c.r6.u64 = a6;
        rt_lookup( fn )( ctx, base, frame );
        c.r1.u64 = saved_sp;
    };
    uint32_t worker = guest_read32( dev + 21156 );
    if ( guest_read8( dev + 10813 ) & 4 )
    {
        if ( worker ) call( guest_read32( guest_read32( worker ) + 24 ), worker, virt, dwords, 1 ), call( guest_read32( guest_read32( worker ) + 28 ), worker, 0, 0, 0 );
        return;
    }
    uint32_t capture = guest_read32( dev + 21296 );
    if ( worker ) call( guest_read32( guest_read32( worker ) + 24 ), worker, virt, dwords, 1 );
    if ( capture ) call( capture, 1, virt, dwords, 1 );
    gpu_submit_segment( addr, dwords, guest_read32( dev + 10824 ) );
    if ( worker ) call( guest_read32( guest_read32( worker ) + 28 ), worker, 0, 0, 0 );
    if ( capture ) call( capture, 2, 0, guest_read32( dev + 10824 ), 0 );
}

// A tile pass's bin select (82756B80: pass; +40 flags, +44 tile, +364 the select last written), written
// by D3D straight into the ring (+14520, index +10824, mask +14524) between the passes it replays:
// flags bit 31 the passes' second bins (0xAAAAAAAA), bit 30 the first (0x55555555), else all; bit 29
// one tile's (3 << 2 * tile; tile 0 also the extents bin 0x80000000 unless bit 31). Here the select is
// an operation submitted in D3D's order with its segments (gpu_submit_ops); nothing goes in the ring.
// (A GPU worker, +21156, and a capture tool, +21296, see the select as D3D would have written it.)
extern "C" PPC_FUNC( __imp__sub_82756B80 );
PPC_FUNC( sub_82756B80 )
{
    PPCContext &c = *regs( ctx );
    uint32_t pass = c.r3.u32, dev = device();
    if ( !native_ops() || !dev ) return __imp__sub_82756B80( ctx, base, frame );  // (any thread: D3D's worker replays tiles)
    int32_t flags = guest_read32( pass + 40 );
    uint32_t m = flags < 0 ? 0xAAAAAAAA : ( flags & 0x40000000 ) ? 0x55555555 : ~0u;
    if ( flags & 0x20000000 )
    {
        uint32_t tile = guest_read32( pass + 44 );
        m &= 3u << ( 2 * tile );
        if ( !tile && flags >= 0 ) m |= 0x80000000;
    }
    if ( m == guest_read32( pass + 364 ) ) return;
    guest_write32( pass + 364, m );
    uint32_t saved_sp = c.r1.u32, sp = ( saved_sp - 144 ) & ~15u, words = sp + 80;
    guest_write32( sp, saved_sp );
    guest_write32( words, 0xC0006200 ), guest_write32( words + 4, m );
    auto call = [&]( uint32_t fn, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6 ) {
        c.r1.u64 = sp, c.r3.u64 = a3, c.r4.u64 = a4, c.r5.u64 = a5, c.r6.u64 = a6;
        rt_lookup( fn )( ctx, base, frame );
        c.r1.u64 = saved_sp;
    };
    uint32_t worker = guest_read32( dev + 21156 ), capture = guest_read32( dev + 21296 );
    if ( worker ) call( guest_read32( guest_read32( worker ) + 24 ), worker, words, 2, 0 );
    if ( capture ) call( capture, 1, words, 2, 0 );
    {
        Ops ops_;
        auto &o = ops_.v;
        op_begin( o, OP_BIN, 2 ), o.push_back( 2 ), o.push_back( m );
        gpu_submit_ops( o );
    }
    if ( worker ) call( guest_read32( guest_read32( worker ) + 28 ), worker, 0, 0, 0 );
    if ( capture ) call( capture, 2, 0, 0, 0 );
}
