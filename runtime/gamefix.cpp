// Guards around game code that met data it could not survive.
#include "ppc_config.h"
#include "ppc_context.h"
#include "runtime.h"
#include <atomic>
#include <cstdlib>

bool rt_has_function( uint32_t guest );  // main.cpp

namespace
{
// An object whose interface at +52 has a callable vtable slot 1 (its type check).
bool callable( uint32_t node )
{
    if ( node < 0x10000 ) return false;
    uint32_t vt = guest_read32( node + 52 );
    return vt >= 0x10000 && rt_has_function( guest_read32( vt + 4 ) );
}

uint32_t call( PPCContext &c, uint32_t addr, uint32_t a0, uint32_t a1 = 0, uint32_t a2 = 0 )
{
    PPCContext saved = c;
    c.r3.u64 = a0, c.r4.u64 = a1, c.r5.u64 = a2;
    c.r1.u64 = ( saved.r1.u32 - 1024 ) & ~15u;
    rt_lookup( addr )( (uintptr_t)&c + 0x80, (uintptr_t)g_base, 0 );
    uint32_t r = c.r3.u32;
    c = saved;
    return r;
}

// The object an entity reference resolves to (8214C480: out, reference, 0 -> out; the low word of it).
uint32_t resolve( PPCContext &c, uint32_t ref )
{
    uint32_t out = ( c.r1.u32 - 512 ) & ~15u;  // between this frame and the callee's
    return guest_read32( call( c, 0x8214C480, out, ref, 0 ) + 4 );
}
} // namespace

// 824DE428 (entity, other) -> whether other belongs with entity: other is entity +164, or the object
// other refers to (other + *(other +16)) is entity's attachment root (entity +180, less 32), one of the
// root's children (first +164, next +168) or one of its parents (+160 up), each asked its type (vtable
// slot 1 of +52, type 830BF1E4) on the way; then a check against entity +204 when byte +187 is set.
// Exploring the city a user met a child whose vtable slot 1 was 0 (a stale node: indirect call to 0).
// With every node callable the game's own code runs; otherwise this does the same, skipping the bad
// nodes (only the node equal to the object can make the walk succeed) and ending that list there.
// CRACKDOWN_NATIVE_CHECK=1 runs both on every call and reports differences.
namespace
{
std::atomic<int> g_reports;

bool belongs( PPCContext &c, uint32_t entity, uint32_t other )
{
    if ( guest_read32( entity + 164 ) == other ) return true;
    const uint32_t root_ref = guest_read32( entity + 180 );
    if ( root_ref && root_ref != 32 )
        if ( uint32_t ref = other + guest_read32( other + 16 ) )
        {
            const uint32_t root = root_ref - 32, obj = resolve( c, ref ), type = guest_read32( 0x830BF1E4 );
            if ( root == obj ) return true;
            auto walk = [&]( uint32_t n, uint32_t next ) {
                for ( ; n; n = guest_read32( n + next ) )
                {
                    if ( !callable( n ) )
                    {
                        if ( g_reports < 5 ) rt_log( "gamefix:   node %08X: +52 vtable %08X, slot 1 %08X\n", n, guest_read32( n + 52 ), guest_read32( guest_read32( n + 52 ) + 4 ) );
                        return false;
                    }
                    if ( n == obj && ( call( c, guest_read32( guest_read32( n + 52 ) + 4 ), n + 52, type ) & 0xFF ) ) return true;
                }
                return false;
            };
            if ( walk( guest_read32( root + 164 ), 168 ) || walk( guest_read32( root + 160 ), 160 ) ) return true;
        }
    if ( g_base[entity + 187] && guest_read32( entity + 188 ) != other )
    {
        const uint32_t ref2 = guest_read32( entity + 204 );
        if ( !ref2 || ref2 == 32 ) return true;
        const uint32_t ref = other + guest_read32( other + 16 );
        if ( !ref || resolve( c, ref ) != ref2 - 32 ) return true;
    }
    return false;
}
} // namespace

extern "C" PPC_FUNC( __imp__sub_824DE428 );
PPC_FUNC( sub_824DE428 )
{
    static const bool check = getenv( "CRACKDOWN_NATIVE_CHECK" );
    auto &c = *(PPCContext *)( ctx - 0x80 );
    const uint32_t entity = c.r3.u32, other = c.r4.u32;
    const uint32_t root_ref = guest_read32( entity + 180 );
    bool clean = true;
    if ( root_ref && root_ref != 32 )
    {
        for ( uint32_t n = guest_read32( root_ref - 32 + 164 ); n && clean; n = guest_read32( n + 168 ) ) clean = callable( n );
        for ( uint32_t n = guest_read32( root_ref - 32 + 160 ); n && clean; n = guest_read32( n + 160 ) ) clean = callable( n );
    }
    if ( clean && !check ) return __imp__sub_824DE428( ctx, base, frame );
    if ( !clean && g_reports++ < 5 )
        rt_log( "gamefix: 824DE428 entity %08X (vtable %08X) attachment root %08X has a node that cannot be called; skipped\n",
                entity, guest_read32( entity ), root_ref - 32 );
    const bool mine = belongs( c, entity, other );
    if ( clean )
    {
        static std::atomic<unsigned> calls, diffs;
        __imp__sub_824DE428( ctx, base, frame );
        if ( ( c.r3.u32 & 0xFF ) != mine && diffs++ < 20 ) rt_log( "native check: 824DE428 entity %08X other %08X: game %u, native %u\n", entity, other, c.r3.u32, mine );
        if ( ++calls % 100000 == 0 ) rt_log( "native check: 824DE428 %u calls, %u differences\n", calls.load(), diffs.load() );
        return;
    }
    c.r3.u64 = mine;
}
