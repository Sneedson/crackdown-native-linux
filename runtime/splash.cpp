// Skipping the start-up splash (Microsoft Game Studios and Realtime Worlds movies, then the Renderware,
// Havok, legal and health screens) with Enter, Space, Esc or a click, through the game's own sequencing.
//
// The splash object's per-frame update (826CD158, reached through its vtable, runs with this =
// object + 136) reads the step type at this+140 and, for a still screen, keeps the elapsed
// time at this+20 (from the start time stamp at this+8, or by adding frame times), calling the next-step
// function 826CD5E0 (object) once that passes the screen's duration. A movie step plays a Bink movie,
// whose HBINK starts {width, height, frames, frame number}; it ends when the frame number reaches the
// last frame.
// A skip sets the playing logo movie to its last frame, or a still screen's elapsed time past any
// duration, and the game moves on by itself. Bink decodes that last frame from the frame before it,
// which it never decoded (blocks of the wrong picture): the movie isn't drawn while it is there
// (splash_movie_hidden, renderer.cpp), so the skip shows black for that moment instead.
#include "ppc_config.h"
#include "ppc_context.h"
#include "runtime.h"
#include "settings.h"
#include "input.h"
#include <atomic>
#include <chrono>
#include <cstdlib>

PPCFunc *rt_hook_indirect( uint32_t guest, PPCFunc *f );
uint16_t xam_scripted_buttons();  // xam.cpp

namespace
{
constexpr uint32_t SPLASH_UPDATE = 0x826CD158;
PPCFunc *g_update;
std::atomic<uint32_t> g_skipped;          // the HBINK put on its last frame
std::atomic<int64_t> g_skipped_until;     // steady clock ms: hidden no longer than this

int64_t now_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>( std::chrono::steady_clock::now().time_since_epoch() ).count();
}

// Puts a logo movie on its last frame (the game then ends it).
void skip_movie( uint32_t movie )
{
    guest_write32( movie + 12, guest_read32( movie + 8 ) - 1 );
    g_skipped_until = now_ms() + 2000;
    g_skipped = movie;
}

// The playing start-up logo movie's HBINK (1280x720, 264 or 241 frames, between its first and last frame).
uint32_t find_logo_movie()
{
    for ( uint32_t a = 0x40000000u; a < 0x60000000u; a += 4 )
        if ( guest_read32( a ) == 1280 && guest_read32( a + 4 ) == 720 )
        {
            uint32_t frames = guest_read32( a + 8 ), n = guest_read32( a + 12 );
            if ( ( frames == 264 || frames == 241 ) && n > 0 && n + 1 < frames ) return a;
        }
    return 0;
}

void splash_update( uintptr_t ctx, uintptr_t base, uintptr_t frame )
{
    static bool was_down, jumped;
    PadState pad = video_pad_state();
    MenuMouse m = video_menu_mouse();
    bool down = ( ( pad.buttons | xam_scripted_buttons() ) & ( 0x10 | 0x1000 | 0x2000 ) ) || m.left;  // START, A, B (Enter, Space, Esc) or a click
    uint32_t self = ( (PPCContext *)( ctx - 0x80 ) )->r3.u32;
    // Skipping the intro (setting, or CRACKDOWN_SKIP_SPLASH=1): straight on. The step index (object +280 = this +144) is put on the step
    // before "done" (10) in the step list the next-step function reads (82C374E4 { 4, 5, 6, 7, 8, 10 }, or
    // 82C374FC { 8, 10 } when the byte at 82E7B9A3 is set), and the current step is ended as a skip does.
    static const bool show = !settings().skip_intro;
    if ( !show && !jumped )
    {
        jumped = true;
        guest_write32( self + 144, g_base[0x82E7B9A3] ? 0 : 4 );
        rt_log( "splash: skipped\n" );
        down = true, was_down = false;
    }
    else if ( !show )  // a logo movie that started after the jump ends at once too
        if ( uint32_t movie = find_logo_movie() ) skip_movie( movie );
    if ( down && !was_down )
    {
        if ( uint32_t movie = find_logo_movie() )
        {
            rt_log( "splash: skipping movie at frame %u of %u\n", guest_read32( movie + 12 ), guest_read32( movie + 8 ) );
            skip_movie( movie );
        }
        else
        {
            rt_log( "splash: skipping screen (step %u)\n", guest_read32( self + 140 ) );
            float done = 1000.0f;
            uint32_t bits;
            memcpy( &bits, &done, 4 );
            guest_write32( self + 20, bits );
            guest_write64( self + 8, 0 );  // started long ago
        }
    }
    was_down = down;
    g_update( ctx, base, frame );
}
} // namespace

// Whether movie draws are left out: a skipped movie still on the last frame it was put on.
bool splash_movie_hidden()
{
    static uint32_t hidden;  // draws left out for this skip
    uint32_t movie = g_skipped;
    if ( !movie ) return false;
    if ( now_ms() < g_skipped_until && guest_read32( movie + 12 ) + 1 >= guest_read32( movie + 8 ) ) return ++hidden, true;
    g_skipped = 0;
    rt_log( "splash: skipped movie's last frame left out (%u draws)\n", hidden );
    hidden = 0;
    return false;
}

void splash_install() { g_update = rt_hook_indirect( SPLASH_UPDATE, splash_update ); }
