#include <algorithm>
// The player's settings (settings.h): settings.ini, environment variables over it.
#include "settings.h"
#include "runtime.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>

namespace
{
std::mutex g_lock;
Settings g_settings;
std::atomic<uint32_t> g_version{ 1 };
bool g_loaded;

std::string config_file()
{
    const char *xdg = getenv( "XDG_CONFIG_HOME" ), *home = getenv( "HOME" );
    std::string dir = xdg && *xdg ? std::string( xdg ) : std::string( home ? home : "." ) + "/.config";
    return dir + "/crackdown-recomp/settings.ini";
}

// The settings with one name; f(field) for the float or bool it is.
struct Field
{
    const char *name, *env;
    float Settings::*f;
    bool Settings::*b;
    int Settings::*i = nullptr;
    std::string Settings::*s = nullptr;
};
const Field FIELDS[] = {
    { "mouse_sensitivity", "CRACKDOWN_MOUSE_SENS", &Settings::mouse_sensitivity, nullptr },
    { "invert_y", "CRACKDOWN_MOUSE_INVERT", nullptr, &Settings::invert_y },
    { "fullscreen", "CRACKDOWN_FULLSCREEN", nullptr, &Settings::fullscreen },
    { "exclusive", "CRACKDOWN_EXCLUSIVE", nullptr, &Settings::exclusive },
    { "vsync", "CRACKDOWN_VSYNC", nullptr, &Settings::vsync },
    { "skip_intro", "CRACKDOWN_SKIP_SPLASH", nullptr, &Settings::skip_intro },
    { "skip_title", "CRACKDOWN_SKIP_TITLE", nullptr, &Settings::skip_title },
    { "native_d3d", "CRACKDOWN_NATIVE_D3D", nullptr, &Settings::native_d3d },
    { "anti_aliasing", "CRACKDOWN_AA", nullptr, &Settings::anti_aliasing },
    { "resolution", "CRACKDOWN_RESOLUTION", nullptr, nullptr, &Settings::resolution },
    { "fsr", "CRACKDOWN_FSR", nullptr, &Settings::fsr },
    { "dlss", "CRACKDOWN_DLSS", nullptr, &Settings::dlss },
    { "fsr3", "CRACKDOWN_FSR3", nullptr, &Settings::fsr3 },
    { "dlss_mode", "CRACKDOWN_DLSS_MODE", nullptr, nullptr, &Settings::dlss_mode },
    { "dlss_preset", "CRACKDOWN_DLSS_PRESET", nullptr, nullptr, &Settings::dlss_preset },
    { "fxaa", "CRACKDOWN_FXAA", nullptr, &Settings::fxaa },
    { "msaa", "CRACKDOWN_MSAA", nullptr, nullptr, &Settings::msaa },
    { "anisotropy", "CRACKDOWN_ANISOTROPY", nullptr, nullptr, &Settings::anisotropy },
    { "lod", "CRACKDOWN_LOD", nullptr, nullptr, &Settings::lod },
    { "show_fps", "CRACKDOWN_SHOW_FPS", nullptr, &Settings::show_fps },
    { "fps60", "CRACKDOWN_60FPS", nullptr, &Settings::fps60 },
    { "fov", "CRACKDOWN_FOV", nullptr, nullptr, &Settings::fov },
    { "profile", "CRACKDOWN_PROFILE", nullptr, nullptr, nullptr, &Settings::profile },
};

void set( Settings &s, const Field &f, const char *v )
{
    if ( f.s ) s.*f.s = v;
    else if ( f.i ) s.*f.i = atoi( v );
    else if ( f.f ) s.*f.f = (float)atof( v );
    else s.*f.b = atoi( v ) != 0 || !strcmp( v, "true" ) || !strcmp( v, "on" );
}

void load()
{
    if ( g_loaded ) return;
    g_loaded = true;
    if ( FILE *f = fopen( config_file().c_str(), "r" ) )
    {
        char line[256];
        while ( fgets( line, sizeof( line ), f ) )
        {
            char key[64], value[64];
            if ( sscanf( line, " %63[a-z0-9_] = %63[^\r\n]", key, value ) != 2 ) continue;  // (keys have digits: fps60; a profile's name has spaces)
            for ( size_t n = strlen( value ); n && value[n - 1] == ' '; ) value[--n] = 0;
            for ( auto &field : FIELDS )
                if ( !strcmp( key, field.name ) ) set( g_settings, field, value );
        }
        fclose( f );
    }
    for ( auto &field : FIELDS )
        if ( const char *v = getenv( field.env ) ) set( g_settings, field, v );
    // A profile's name: a gamertag's 15 characters at most, letters, digits and spaces (it names a folder).
    {
        std::string n;
        for ( char ch : g_settings.profile )
            if ( ( isalnum( (unsigned char)ch ) || ch == ' ' ) && n.size() < 15 ) n += ch;
        while ( !n.empty() && n.back() == ' ' ) n.pop_back();
        g_settings.profile = n.empty() ? "Player" : n;
    }
    if ( g_settings.mouse_sensitivity < 0.25f ) g_settings.mouse_sensitivity = 0.25f;
    if ( g_settings.mouse_sensitivity > 4.0f ) g_settings.mouse_sensitivity = 4.0f;
    if ( g_settings.resolution >= 1 && g_settings.resolution <= 3 ) g_settings.resolution *= 720;  // as earlier versions saved it
    g_settings.dlss_mode = std::clamp( g_settings.dlss_mode, 0, 4 );
    g_settings.dlss_preset = std::clamp( g_settings.dlss_preset, 0, 4 );
    g_settings.fov = std::clamp( ( g_settings.fov + 2 ) / 5 * 5, 60, 120 );
    { const int m = g_settings.msaa; g_settings.msaa = m >= 8 ? 8 : m >= 4 ? 4 : m >= 2 ? 2 : 0; }
    { const int a = g_settings.anisotropy; g_settings.anisotropy = a >= 16 ? 16 : a >= 8 ? 8 : a >= 4 ? 4 : a >= 2 ? 2 : 0; }
    g_settings.lod = std::clamp( g_settings.lod, 0, 4 );
    if ( g_settings.resolution != 0 ) g_settings.resolution = std::clamp( ( g_settings.resolution + 45 ) / 90 * 90, 720, 2160 );
}

void save( const Settings &s )
{
    std::string path = config_file();
    std::error_code ec;
    std::filesystem::create_directories( std::filesystem::path( path ).parent_path(), ec );
    FILE *f = fopen( ( path + ".new" ).c_str(), "w" );
    if ( !f ) { rt_log( "settings: can't write %s\n", path.c_str() ); return; }
    fprintf( f, "# Crackdown (native recomp) settings: the Recomp Options page sets these.\n" );
    for ( auto &field : FIELDS )
        if ( field.s ) fprintf( f, "%s = %s\n", field.name, ( s.*field.s ).c_str() );
        else if ( field.i ) fprintf( f, "%s = %d\n", field.name, s.*field.i );
        else if ( field.f ) fprintf( f, "%s = %.2f\n", field.name, s.*field.f );
        else fprintf( f, "%s = %d\n", field.name, s.*field.b ? 1 : 0 );
    fclose( f );
    std::filesystem::rename( path + ".new", path, ec );
}
} // namespace

Settings settings()
{
    std::lock_guard<std::mutex> l( g_lock );
    load();
    return g_settings;
}

void settings_change( const Settings &s )
{
    std::lock_guard<std::mutex> l( g_lock );
    load();
    g_settings = s;
    save( s );
    ++g_version;
    rt_log( "settings: mouse %.2f invert %d fullscreen %d vsync %d skip intro %d title %d native d3d %d\n", s.mouse_sensitivity, s.invert_y, s.fullscreen,
            s.vsync, s.skip_intro, s.skip_title, s.native_d3d );
}

uint32_t settings_version() { return g_version; }
