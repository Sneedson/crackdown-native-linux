// The player's keyboard and mouse controls (keybinds.h).
#include "keybinds.h"
#include "runtime.h"
#include <SDL3/SDL_keyboard.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>

namespace keybinds
{
namespace
{
#define K( x ) SDL_SCANCODE_##x
Command g_commands[] = {
    { "move_forward", "Move Forward", "On Foot", { K( W ) }, {} },
    { "move_back", "Move Back", "On Foot", { K( S ) }, {} },
    { "strafe_left", "Strafe Left", "On Foot", { K( A ) }, {} },
    { "strafe_right", "Strafe Right", "On Foot", { K( D ) }, {} },
    { "jump", "Jump", "On Foot", { K( SPACE ) }, {} },
    { "fire", "Fire / Throw", "On Foot", { MOUSE_L }, {} },
    { "lock_on", "Lock On", "On Foot", { MOUSE_R }, {} },
    { "melee", "Melee / Pick Up Object", "On Foot", { K( F ) }, {} },
    { "grenade", "Grenade / Detonate", "On Foot", { K( G ) }, {} },
    { "reload", "Reload", "On Foot", { K( R ) }, {} },
    { "switch_weapon", "Switch / Pick Up Weapon", "On Foot", { K( Q ) }, {} },
    { "zoom", "Zoom", "On Foot", { K( Z ) }, {} },
    { "use", "Enter Vehicle / Use", "On Foot", { K( E ) }, {} },
    { "map", "Map", "On Foot", { K( TAB ), K( M ) }, {} },
    { "respawn", "Respawn", "On Foot", { K( SPACE ), K( RETURN ) }, {} },
    { "accelerate", "Accelerate", "Driving", { K( W ) }, {} },
    { "brake", "Brake / Reverse", "Driving", { K( S ) }, {} },
    { "steer_left", "Steer Left", "Driving", { K( A ) }, {} },
    { "steer_right", "Steer Right", "Driving", { K( D ) }, {} },
    { "handbrake", "Handbrake", "Driving", { K( SPACE ) }, {} },
    { "vehicle_fire", "Fire Mounted Gun", "Driving", { MOUSE_L }, {} },
    { "horn", "Horn / Siren", "Driving", { K( H ) }, {} },
    { "rear_view", "Rear View", "Driving", { MOUSE_M }, {} },
    { "exit_vehicle", "Exit Vehicle", "Driving", { K( E ) }, {} },
    { "next_track", "Next Track", "Radio", { K( PERIOD ) }, {} },
    { "prev_track", "Previous Track", "Radio", { K( COMMA ) }, {} },
    { "mute_track", "Mute", "Radio", { K( N ) }, {} },
    { "volume_up", "Volume Up", "Radio", { K( EQUALS ) }, {} },
    { "volume_down", "Volume Down", "Radio", { K( MINUS ) }, {} },
};
#undef K
constexpr int COUNT = sizeof( g_commands ) / sizeof( g_commands[0] );
std::mutex g_lock;
std::atomic<uint32_t> g_version{ 1 };
bool g_loaded;

std::string file()
{
    const char *xdg = getenv( "XDG_CONFIG_HOME" ), *home = getenv( "HOME" );
    std::string dir = xdg && *xdg ? std::string( xdg ) : std::string( home ? home : "." ) + "/.config";
    return dir + "/crackdown-recomp/keybinds.ini";
}

const struct { int input; const char *name, *cap; } MOUSE_NAMES[] = {
    { MOUSE_L, "Left Mouse", "LMB" }, { MOUSE_R, "Right Mouse", "RMB" }, { MOUSE_M, "Middle Mouse", "MMB" },
    { MOUSE_X1, "Mouse 4", "M4" },    { MOUSE_X2, "Mouse 5", "M5" },     { WHEEL_UP, "Wheel Up", "WHL" },
    { WHEEL_DOWN, "Wheel Down", "WHL" } };

int parse( const std::string &s )
{
    if ( s.empty() || s == "none" ) return 0;
    for ( auto &m : MOUSE_NAMES )
        if ( s == m.name ) return m.input;
    SDL_Scancode sc = SDL_GetScancodeFromName( s.c_str() );
    return sc == SDL_SCANCODE_UNKNOWN ? 0 : (int)sc;
}

void save_locked()
{
    std::string path = file();
    std::error_code ec;
    std::filesystem::create_directories( std::filesystem::path( path ).parent_path(), ec );
    FILE *f = fopen( ( path + ".new" ).c_str(), "w" );
    if ( !f ) { rt_log( "keybinds: can't write %s\n", path.c_str() ); return; }
    fprintf( f, "# Crackdown (native recomp) keyboard and mouse controls: the Keyboard Controls page sets these.\n"
                "# command = key[, second key]  (key names as SDL writes them: W, Space, Left Ctrl, Left Mouse, Mouse 4, none)\n" );
    for ( const Command &c : g_commands )
    {
        fprintf( f, "%s = %s", c.id, c.keys[0] ? key_name( c.keys[0] ).c_str() : "none" );
        if ( c.keys[1] ) fprintf( f, ", %s", key_name( c.keys[1] ).c_str() );
        fprintf( f, "\n" );
    }
    fclose( f );
    std::filesystem::rename( path + ".new", path, ec );
}

void load_locked()
{
    if ( g_loaded ) return;
    g_loaded = true;
    for ( Command &c : g_commands ) c.keys[0] = c.def[0], c.keys[1] = c.def[1];
    FILE *f = fopen( file().c_str(), "r" );
    if ( !f ) return;
    char line[256];
    auto trim = []( std::string s ) {
        while ( !s.empty() && isspace( (unsigned char)s.back() ) ) s.pop_back();
        size_t i = 0;
        while ( i < s.size() && isspace( (unsigned char)s[i] ) ) ++i;
        return s.substr( i );
    };
    while ( fgets( line, sizeof( line ), f ) )
    {
        std::string l = line;
        if ( l.empty() || l[0] == '#' ) continue;
        size_t eq = l.find( '=' );
        if ( eq == std::string::npos ) continue;
        std::string id = trim( l.substr( 0, eq ) ), v = trim( l.substr( eq + 1 ) );
        for ( Command &c : g_commands )
            if ( id == c.id )
            {
                size_t comma = v.find( ',' );
                int a = parse( trim( v.substr( 0, comma ) ) ), b = comma == std::string::npos ? 0 : parse( trim( v.substr( comma + 1 ) ) );
                c.keys[0] = bindable( a ) ? a : 0, c.keys[1] = bindable( b ) ? b : 0;
            }
    }
    fclose( f );
}
} // namespace

int count() { return COUNT; }
Command &command( int i )
{
    std::lock_guard<std::mutex> g( g_lock );
    load_locked();
    return g_commands[i];
}
int find( const char *id )
{
    for ( int i = 0; i < COUNT; ++i )
        if ( !strcmp( g_commands[i].id, id ) ) return i;
    return -1;
}
void set_key( int ci, int slot, int input )
{
    std::lock_guard<std::mutex> g( g_lock );
    load_locked();
    if ( ci < 0 || ci >= COUNT || slot < 0 || slot > 1 ) return;
    Command &c = g_commands[ci];
    if ( input )
        for ( Command &o : g_commands )  // one input, one command per group (the movement keys are shared across groups)
            if ( !strcmp( o.group, c.group ) )
                for ( int &k : o.keys )
                    if ( k == input && !( &o == &c && &k == &c.keys[slot] ) ) k = 0;
    c.keys[slot] = input;
    if ( !c.keys[0] && c.keys[1] ) c.keys[0] = c.keys[1], c.keys[1] = 0;
    save_locked();
    ++g_version;
    rt_log( "keybinds: %s = %s, %s\n", c.id, key_name( c.keys[0] ).c_str(), key_name( c.keys[1] ).c_str() );
}
void reset_defaults()
{
    std::lock_guard<std::mutex> g( g_lock );
    for ( Command &c : g_commands ) c.keys[0] = c.def[0], c.keys[1] = c.def[1];
    g_loaded = true;
    save_locked();
    ++g_version;
    rt_log( "keybinds: defaults\n" );
}
uint32_t version() { return g_version; }

std::string key_name( int input )
{
    if ( !input ) return "none";
    for ( auto &m : MOUSE_NAMES )
        if ( m.input == input ) return m.name;
    const char *n = SDL_GetScancodeName( (SDL_Scancode)input );
    return n && *n ? n : "key " + std::to_string( input );
}

std::string key_cap( int input )
{
    if ( !input ) return "";
    for ( auto &m : MOUSE_NAMES )
        if ( m.input == input ) return m.cap;
    switch ( input )
    {
    case SDL_SCANCODE_SPACE: return "SPACE";
    case SDL_SCANCODE_RETURN: return "ENTER";
    case SDL_SCANCODE_ESCAPE: return "ESC";
    case SDL_SCANCODE_TAB: return "TAB";
    case SDL_SCANCODE_BACKSPACE: return "BKSP";
    case SDL_SCANCODE_LSHIFT: case SDL_SCANCODE_RSHIFT: return "SHIFT";
    case SDL_SCANCODE_LCTRL: case SDL_SCANCODE_RCTRL: return "CTRL";
    case SDL_SCANCODE_LALT: case SDL_SCANCODE_RALT: return "ALT";
    case SDL_SCANCODE_CAPSLOCK: return "CAPS";
    case SDL_SCANCODE_UP: return "UP";
    case SDL_SCANCODE_DOWN: return "DOWN";
    case SDL_SCANCODE_LEFT: return "LEFT";
    case SDL_SCANCODE_RIGHT: return "RIGHT";
    case SDL_SCANCODE_DELETE: return "DEL";
    case SDL_SCANCODE_INSERT: return "INS";
    case SDL_SCANCODE_HOME: return "HOME";
    case SDL_SCANCODE_END: return "END";
    case SDL_SCANCODE_PAGEUP: return "PGUP";
    case SDL_SCANCODE_PAGEDOWN: return "PGDN";
    default: break;
    }
    std::string n = key_name( input );
    for ( char &ch : n ) ch = (char)toupper( (unsigned char)ch );
    if ( n.rfind( "KEYPAD ", 0 ) == 0 ) n = "NUM" + n.substr( 7 );
    return n;
}

bool bindable( int input )
{
    if ( !input ) return true;
    if ( input == SDL_SCANCODE_ESCAPE || input == SDL_SCANCODE_F11 || input == SDL_SCANCODE_F12 ) return false;  // the way out, recording, capture
    return input < SDL_SCANCODE_COUNT || ( input >= MOUSE_L && input <= MOUSE_X2 );
}
} // namespace keybinds
