// Field of view (the Recomp Options page's slider, settings.h fov). Each frame 822ABE88 (r3: the camera
// settings list, r4: the camera, r5: where the angle goes) sets the camera up from the current camera
// settings (r3 + 96 * *0x82C39738): their +80 is the field of view in degrees, which it hands on at *r5
// and turns (tan of half of it) into the camera's +92 / +96 (half the view's width and height at distance
// 1) with their inverses at +100 / +104. The projection (8237AA40), the culling and the markers over
// people and things (which project with the angle) all follow from those. So the setting changes that
// angle for the call: tan(half) scaled by tan(fov / 2) / tan(35 degrees), the game's own view being about
// 70 degrees across at 16:9 (the setting's 70 leaves it alone), and puts the game's value back after.
// The camera's own changes (vehicles, cut scenes) keep their proportion.
#include "ppc_config.h"
#include "ppc_context.h"
#include "runtime.h"
#include "settings.h"
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace
{
float rd( uint32_t a ) { uint32_t w = guest_read32( a ); float f; memcpy( &f, &w, 4 ); return f; }
void wr( uint32_t a, float f ) { uint32_t w; memcpy( &w, &f, 4 ); guest_write32( a, w ); }
constexpr float DEG = 3.14159265f / 180.0f;

// The angle the setting makes of the game's (degrees).
float widened( float game, int fov )
{
    static const float DEFAULT_TAN = std::tan( 35.0f * DEG );
    const float k = std::tan( fov * 0.5f * DEG ) / DEFAULT_TAN;
    return 2.0f * std::atan( std::tan( game * 0.5f * DEG ) * k ) / DEG;
}

// The player's camera object (r3) keeps its own angle, +6552 times +6560 (degrees), which 822BE448 and
// 822BEE80 turn into the view the markers over people and things are placed in: the same change there,
// for the call.
template <void ( *F )( uintptr_t, uintptr_t, uintptr_t )>
void with_camera_angle( uintptr_t ctx, uintptr_t base, uintptr_t frame )
{
    PPCContext &c = *(PPCContext *)( ctx - 0x80 );
    const int fov = settings().fov;
    const uint32_t cam = c.r3.u32;
    const float a = rd( cam + 6552 ), b = rd( cam + 6560 );
    if ( fov == 70 || b == 0 || !std::isfinite( a * b ) ) { F( ctx, base, frame ); return; }
    wr( cam + 6552, widened( a * b, fov ) / b );
    F( ctx, base, frame );
    wr( cam + 6552, a );
}
}

extern "C" PPC_FUNC( __imp__sub_822BE448 );
extern "C" PPC_FUNC( __imp__sub_822BEE80 );
PPC_FUNC( sub_822BE448 ) { with_camera_angle<__imp__sub_822BE448>( ctx, base, frame ); }
PPC_FUNC( sub_822BEE80 ) { with_camera_angle<__imp__sub_822BEE80>( ctx, base, frame ); }

extern "C" PPC_FUNC( __imp__sub_822ABE88 );
PPC_FUNC( sub_822ABE88 )
{
    PPCContext &c = *(PPCContext *)( ctx - 0x80 );
    const int fov = settings().fov;
    if ( fov == 70 ) { __imp__sub_822ABE88( ctx, base, frame ); return; }
    const uint32_t angle_at = c.r3.u32 + guest_read32( 0x82C39738 ) * 96 + 80;
    const float game = rd( angle_at );
    const float wanted = widened( game, fov );
    wr( angle_at, wanted );
    __imp__sub_822ABE88( ctx, base, frame );
    wr( angle_at, game );
    if ( static int logged; getenv( "CRACKDOWN_FOV_LOG" ) && logged++ < 4 )
        rt_log( "fov: %d degrees: the game's %g made %g (camera %08X half width %g)\n", fov, game, wanted, c.r4.u32, rd( c.r4.u32 + 92 ) );
}
