// Keyboard (and mouse buttons) as input of the game's own actions, not as a pretend controller.
//
// The game turns controller input into named actions ("Jump", "Fast Attack", "Accelerate", ... with
// config names like character_jump; 8245F728 registers them, each in a global holding its index). Each
// frame the pad poll (82461560, r3 = pad manager, pads at +4 + 2216 * n) runs the action update
// (82461E58, r3 = pad) for each pad: it works out every action's value from that action's controller
// binding (table at *(pad + 8), 24 bytes per action) and stores it in the action's control
// (pad + 492 + 20 * index: +4 just pressed, +5 just released, +6 held over from a block, +8 value
// -127..127) with 82459D10(control, value). Code everywhere then asks the controls.
// The keyboard joins in at that setter: for the first player's action controls, a held key bound to the
// action gives its value (127, or -127 for the negative key of an axis) in place of the controller's,
// so the game's own press / release tracking and everything reading the action see the key. In menus
// the keyboard stays a controller (video.cpp), as the menus only read controllers.
#include "ppc_config.h"
#include "ppc_context.h"
#include "runtime.h"
#include "input.h"
#include "keybinds.h"
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <string>
#include <SDL3/SDL_keyboard.h>
#include <vector>

extern "C" PPC_FUNC( __imp__sub_82461560 );  // the recompiled originals (sub_* are weak aliases of them)
extern "C" PPC_FUNC( __imp__sub_82461E58 );
extern "C" PPC_FUNC( __imp__sub_82459D10 );

double input_script_clock();  // xam.cpp
bool player_zoomed();          // mouselook.cpp
bool player_locked();          // mouselook.cpp
bool player_in_vehicle();      // mouselook.cpp

namespace
{
constexpr uint32_t ACTION_COUNT = 0x82E79D98;  // number of registered actions
uint32_t g_pads;                               // the pad manager seen by the last pad poll

using namespace keybinds;

// The game's actions (the global holding each one's index) and what drives them: the keys of the player's
// commands (keybinds.h) in `pos` give 127, those in `neg` -127; `fixed` are inputs that aren't the
// player's to change (the wheel).
struct Binding
{
    uint32_t action;
    const char *name;
    const char *pos[3], *neg[2];
    int fixed[2];
};
const Binding BINDINGS[] = {
    { 0x82E27CA4, "Forward/Back", { "move_forward" }, { "move_back" } },
    { 0x82E27CA8, "Strafe", { "strafe_right" }, { "strafe_left" } },
    { 0x82E27CA0, "Enter/Exit Vehicle", { "use", "exit_vehicle" } },
    { 0x82E27C54, "Activate Agency Zones", { "use" } },
    { 0x82E27C68, "Jump", { "jump" } },
    { 0x82E27C10, "Fast Attack", { "melee" } },
    { 0x82E27C9C, "Object Interact", { "melee" } },
    { 0x82E27C98, "Object Drop", { "melee" } },
    { 0x82E27C8C, "Primary Fire", { "fire" } },
    { 0x82E27C88, "Throw Start", { "fire" } },
    { 0x82E27C84, "Throw Power", { "fire" } },
    { 0x82E27C94, "Target HardLock", { "lock_on" } },
    { 0x82E27C80, "Secondary Fire Start", { "grenade" } },
    { 0x82E27C7C, "Secondary Fire Power", { "grenade" } },
    { 0x82E27C74, "Detonator", { "grenade" } },
    { 0x82E27C70, "Reload", { "reload" } },
    { 0x82E27C6C, "Next Equipment", { "switch_weapon" }, {}, { WHEEL_SWAP } },
    { 0x82E27C78, "Equipment Pick Up", { "switch_weapon" } },
    { 0x82E27C48, "Zoom In", {}, {}, { WHEEL_ZOOM_IN } },
    { 0x82E27C44, "Zoom Out", {}, {}, { WHEEL_ZOOM_OUT } },
    { 0x82E27C4C, "Zoom Toggle", { "zoom" } },
    { 0x82E27BF8, "Turn Vehicle", { "steer_right" }, { "steer_left" } },
    { 0x82E27BF4, "Tilt", { "accelerate" }, { "brake" } },
    { 0x82E27C04, "Accelerate", { "accelerate" } },
    { 0x82E27C00, "Brake/Reverse", { "brake" } },
    { 0x82E27BFC, "HandBrake", { "handbrake" } },
    { 0x82E27BEC, "Horn", { "horn" } },
    { 0x82E27BE8, "Siren", { "horn" } },
    { 0x82E27BF0, "Rear View", { "rear_view" } },
    { 0x82E27BD0, "Fire mounted gun", { "vehicle_fire" } },
    { 0x82E27BE4, "Next Music Track", { "next_track" }, {}, { WHEEL_UP } },
    { 0x82E27BE0, "Prev Music Track", { "prev_track" }, {}, { WHEEL_DOWN } },
    { 0x82E27BDC, "Mute Music Track", { "mute_track" } },
    { 0x82E27BD8, "Music Volume Up", { "volume_up" } },
    { 0x82E27BD4, "Music Volume Down", { "volume_down" } },
    { 0x82E27BCC, "Pause", {}, {}, { SDL_SCANCODE_ESCAPE } },
    { 0x82E27BC8, "Restart Player", { "respawn" } },
    { 0x82E27BC4, "Display Map", { "map" } },
};

// CRACKDOWN_KEY_SCRIPT="t:key:seconds,..." (SDL key names: W, Space, Escape, ...; mouse buttons as
// MouseLeft / MouseRight / MouseMiddle, WheelUp / WheelDown): keys held at those times of the input script clock (testing).
// "t:EnterVehicle:seconds" taps E from t until the player is in a vehicle; times written "+t" count from
// the moment the player got in (a drive that starts wherever the walk to the car ended).
bool scripted_key( int input )
{
    struct Press { double at, len; int input; bool after_entry = false, enter = false; };
    static const std::vector<Press> presses = [] {
        std::vector<Press> v;
        const char *e = getenv( "CRACKDOWN_KEY_SCRIPT" );
        for ( std::string item; e && *e; )
        {
            const char *comma = strchr( e, ',' );
            item.assign( e, comma ? comma - e : strlen( e ) );
            e = comma ? comma + 1 : nullptr;
            char name[32] = {};
            double at = 0, len = 0;
            bool after = item[0] == '+';
            if ( sscanf( item.c_str() + after, "%lf:%31[^:]:%lf", &at, name, &len ) != 3 ) continue;
            bool enter = !strcmp( name, "EnterVehicle" );
            int in = enter ? (int)SDL_SCANCODE_E : !strcmp( name, "WheelUp" ) ? WHEEL_UP : !strcmp( name, "WheelDown" ) ? WHEEL_DOWN
                   : !strcmp( name, "MouseLeft" ) ? MOUSE_L : !strcmp( name, "MouseRight" ) ? MOUSE_R
                   : !strcmp( name, "MouseMiddle" ) ? MOUSE_M : (int)SDL_GetScancodeFromName( name );
            if ( in ) v.push_back( { at, len, in, after, enter } );
        }
        return v;
    }();
    if ( presses.empty() ) return false;
    double t = input_script_clock();
    static double entered = -1;  // script time the player was first seen in a vehicle
    if ( entered < 0 && player_in_vehicle() ) { entered = t; rt_log( "keyscript: in a vehicle at %.1f s\n", t ); }
    for ( const Press &p : presses )
    {
        if ( p.input != input ) continue;
        if ( p.after_entry ) { if ( entered >= 0 && t >= entered + p.at && t < entered + p.at + p.len ) return true; continue; }
        if ( p.enter ) { if ( entered < 0 && t >= p.at && t < p.at + p.len && fmod( t - p.at, 0.75 ) < 0.25 ) return true; continue; }
        if ( t >= p.at && t < p.at + p.len ) return true;
    }
    return false;
}

// Per action index: the keyboard's value this frame (0 = nothing held).
int g_value[256];
int g_wheel, g_wheel_frames;  // a wheel step holds its action for a few frames

bool held( int input )
{
    switch ( input )
    {
    case 0: return false;
    case MOUSE_L: return ( video_action_mouse() & 1 ) || scripted_key( input );
    case MOUSE_R: return ( video_action_mouse() & 4 ) || scripted_key( input );
    case MOUSE_M: return ( video_action_mouse() & 2 ) || scripted_key( input );
    case MOUSE_X1: return video_action_mouse() & 8;
    case MOUSE_X2: return video_action_mouse() & 16;
    case WHEEL_UP: return g_wheel > 0;
    case WHEEL_DOWN: return g_wheel < 0;
    case WHEEL_SWAP: return g_wheel && !player_zoomed() && !player_in_vehicle();  // on foot only: in a vehicle it changes the music
    case WHEEL_ZOOM_IN: return g_wheel > 0 && player_zoomed();
    case WHEEL_ZOOM_OUT: return g_wheel < 0 && player_zoomed();
    default: return video_key( input ) || scripted_key( input );
    }
}

// The keyboard's values for this frame, worked out once per pad poll.
void read_keyboard()
{
    if ( int w = video_take_wheel() ) { g_wheel = w; g_wheel_frames = 4; }
    else if ( g_wheel_frames && !--g_wheel_frames ) g_wheel = 0;
    if ( scripted_key( WHEEL_UP ) ) g_wheel = 1;
    else if ( scripted_key( WHEEL_DOWN ) ) g_wheel = -1;
    else if ( !g_wheel_frames ) g_wheel = 0;
    memset( g_value, 0, sizeof( g_value ) );
    if ( !video_in_game() || video_menu_control() ) return;  // menus: the keyboard is a controller there
    auto down = []( const char *id ) {
        if ( !id ) return false;
        const int c = keybinds::find( id );
        if ( c < 0 ) return false;
        const Command &cmd = keybinds::command( c );
        return held( cmd.keys[0] ) || held( cmd.keys[1] );
    };
    for ( const Binding &b : BINDINGS )
    {
        uint32_t i = guest_read32( b.action );
        if ( i >= 256 ) continue;
        bool pos = false, neg = false;
        for ( const char *id : b.pos ) pos |= down( id );
        for ( int k : b.fixed ) pos |= held( k );
        for ( const char *id : b.neg ) neg |= down( id );
        g_value[i] = pos && neg ? 0 : pos ? 127 : neg ? -127 : 0;
    }
    // Where mouse look isn't turning the camera itself the mouse gives the game's own look actions: the car
    // camera's, or "Look L/R" and "Look U/D" on foot.
    int x, y;
    video_mouse_axes( x, y );
    if ( uint32_t i = guest_read32( player_in_vehicle() ? 0x82E27C0C : 0x82E27CB0 ); i < 256 && x ) g_value[i] = x;
    if ( uint32_t i = guest_read32( player_in_vehicle() ? 0x82E27C08 : 0x82E27CAC ); i < 256 && y ) g_value[i] = y;
}

// CRACKDOWN_ACTION_DUMP=1: log every action's controller binding once.
void dump_bindings( uint32_t pad )
{
    static bool done;
    if ( done || !getenv( "CRACKDOWN_ACTION_DUMP" ) ) return;
    uint32_t n = guest_read32( ACTION_COUNT ), table = guest_read32( pad + 8 );
    bool bound = false;
    for ( uint32_t i = 0; table && i < n; ++i ) bound |= guest_read32( table + i * 24 + 16 ) != 0;
    if ( !bound ) return;  // the controller config isn't loaded yet
    done = true;
    auto control = [&]( uint32_t id ) -> std::string {
        if ( !id ) return "-";
        uint32_t p = guest_read32( pad + ( id - 153 ) * 4 );
        if ( !p ) return "id " + std::to_string( id );
        int32_t off = (int32_t)( p - pad );
        char b[64];
        if ( off >= 132 && off < 132 + 20 * 16 ) snprintf( b, sizeof( b ), "button %d", ( off - 132 ) / 20 );
        else snprintf( b, sizeof( b ), "pad+%d (id %u)", off, id );
        return b;
    };
    for ( const Binding &b : BINDINGS ) rt_log( "action %2u is %s\n", guest_read32( b.action ), b.name );
    for ( uint32_t i = 0; i < n; ++i )
    {
        uint32_t e = table + i * 24;
        rt_log( "action %2u: kind %u flags %u handler %08X controls %s / %s\n", i, guest_read32( e + 4 ), guest_read32( e + 8 ),
                guest_read32( e + 12 ), control( guest_read32( e + 16 ) ).c_str(), control( guest_read32( e + 20 ) ).c_str() );
    }
}
} // namespace

// CRACKDOWN_KEY_SCRIPT for video.cpp's keyboard state (the menus' key mapping).
bool keyboard_scripted_key( int scancode ) { return scripted_key( scancode ); }

PPC_FUNC( sub_82461560 )
{
    g_pads = ( (PPCContext *)( ctx - 0x80 ) )->r3.u32;
    read_keyboard();
    __imp__sub_82461560( ctx, base, frame );
}

PPC_FUNC( sub_82461E58 )
{
    uint32_t pad = ( (PPCContext *)( ctx - 0x80 ) )->r3.u32;
    __imp__sub_82461E58( ctx, base, frame );
    if ( pad == g_pads + 4 ) dump_bindings( pad );
}

// The action control setter (control, value): for the first player's actions, a held key's value.
PPC_FUNC( sub_82459D10 )
{
    auto *c = (PPCContext *)( ctx - 0x80 );
    uint32_t control = c->r3.u32, first = g_pads + 4 + 492;
    if ( g_pads && control >= first && ( control - first ) % 20 == 0 && ( control - first ) / 20 < 256 )
    {
        // CRACKDOWN_AXIS_LOG=1: what a controller's stick gives the car camera's actions (testing).
        if ( static const bool alog = getenv( "CRACKDOWN_AXIS_LOG" ); alog && c->r4.s32 )
            if ( uint32_t i = ( control - first ) / 20; i == guest_read32( 0x82E27C0C ) || i == guest_read32( 0x82E27C08 ) )
                rt_log( "axis: action %u = %d at %.1f s\n", i, c->r4.s32, input_script_clock() );
        if ( int v = g_value[( control - first ) / 20] )
        {
            static const bool log = getenv( "CRACKDOWN_KEY_LOG" );  // CRACKDOWN_KEY_LOG=1: each key value given (testing)
            if ( log ) rt_log( "key: action %u = %d at %.1f s\n", ( control - first ) / 20, v, input_script_clock() );
            c->r4.s64 = v;
        }
    }
    __imp__sub_82459D10( ctx, base, frame );
}
