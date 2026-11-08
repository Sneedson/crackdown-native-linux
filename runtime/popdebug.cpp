// Debug (CRACKDOWN_POP_LOG=1): each character removed (82165280, r4: its distance from player one, its
// position at +304, and the guest call stack). Player one's position: *0x82E79C44 +1048 -> +484 -> +5280 -> +96, as 82577B00
// reads it. With the game's own settings people go at 105 m (drawdist.cpp).
#include "ppc_config.h"
#include "ppc_context.h"
#include "runtime.h"
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace
{
float rd( uint32_t a ) { uint32_t w = guest_read32( a ); float f; memcpy( &f, &w, 4 ); return f; }
bool player_pos( float p[3] )
{
    uint32_t a = guest_read32( 0x82E79C44 );
    if ( !a ) return false;
    a = guest_read32( a + 1048 );
    if ( !a ) return false;
    a = guest_read32( a + 484 );
    if ( !a ) return false;
    a = guest_read32( a + 5280 );
    if ( !a ) return false;
    for ( int i = 0; i < 3; i++ ) p[i] = rd( a + 96 + 4 * i );
    return true;
}
}

// CRACKDOWN_POP_LOG: who removes characters (82165280).
extern "C" PPC_FUNC( __imp__sub_82165280 );
PPC_FUNC( sub_82165280 )
{
    PPCContext &c = *(PPCContext *)( ctx - 0x80 );
    static const bool on = getenv( "CRACKDOWN_POP_LOG" ) != nullptr;
    if ( on )
    {
        char bt[160]; int n = 0;
        uint32_t sp = c.r1.u32;
        for ( int i = 0; i < 8 && sp; i++ )
        {
            sp = guest_read32( sp );
            if ( !sp ) break;
            n += snprintf( bt + n, sizeof( bt ) - n, " %08X", guest_read32( sp - 8 ) );
        }
        float pp[3], dist = -1;
        if ( player_pos( pp ) )
        {
            const float dx = rd( c.r4.u32 + 304 ) - pp[0], dz = rd( c.r4.u32 + 312 ) - pp[2];
            dist = std::sqrt( dx * dx + dz * dz );
        }
        rt_log( "pop: remove character %08X dist %.1f lr %08X bt%s\n", c.r4.u32, dist, (uint32_t)c.lr, bt );
    }
    __imp__sub_82165280( ctx, base, frame );
}

// CRACKDOWN_BUDGET_LOG: the entity manager's budgets (people 150, characters 152, vehicles 32, ...: the
// population's real limits. They can't be raised: at run time more capacity gives no more people, and made
// bigger (823DB4F8's r5) the character manager, one 15888-byte object sized for them, is overrun: a crash).
// *0x82E79AD8 +0x2C094: an array of entries, +4 the capacity, +16/+20 the list in use. Logged at 150 s.
void budget_log()
{
    static const char *log_env = getenv( "CRACKDOWN_BUDGET_LOG" );
    static bool done;
    extern double input_script_clock();
    if ( done || !log_env || input_script_clock() < 150 ) return;
    done = true;
    const uint32_t mgr = guest_read32( 0x82E79AD8 ), arr = mgr ? guest_read32( mgr + 0x2C094 ) : 0;
    for ( int i = 0; arr && i < 64; ++i )
    {
        const uint32_t e = guest_read32( arr + 4 * i );
        if ( !e || e < 0x40000000 ) continue;
        const uint32_t b = guest_read32( e + 16 ), en = guest_read32( e + 20 );
        rt_log( "budget %d at %08X: +0 %08X capacity %u in use %d +8 %08X +12 %08X\n", i, e, guest_read32( e ), guest_read32( e + 4 ), b ? (int)( en - b ) / 4 : 0,
                guest_read32( e + 8 ), guest_read32( e + 12 ) );
    }
}

