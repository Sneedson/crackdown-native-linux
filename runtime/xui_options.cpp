// The Recomp Options page in the game's own menu. The game's scene archive (XUIFrontEnd\Scene.xzp) is
// served with two scenes changed, made from the player's copy when the game first opens it (and kept
// in the cache folder): the menu's Options tab (menuHud_3.xur) has its Xbox Live Settings entry (no use
// on the PC) open RecompOptions.xur instead, and RecompOptions.xur is the Controller Settings page
// (controllerOptions.xur) with its controller diagram taken out and its rows made the settings' rows
// (settings.h): its own On / Off rows for the switches and the Audio page's slider (audio.xur) for the
// mouse sensitivity. runtime/xui.cpp keeps the page and the settings in step.
#include "runtime.h"
#include "xur.h"
#include "fontatlas.h"
#include <SDL3/SDL.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <sys/stat.h>
#include <unistd.h>

namespace
{
std::vector<uint8_t> read_file( const std::string &path )
{
    std::ifstream f( path, std::ios::binary );
    return { std::istreambuf_iterator<char>( f ), {} };
}

std::vector<uint8_t> entry( const std::vector<uint8_t> &a, const char16_t *name )
{
    std::vector<XzpEntry> entries;
    uint32_t start;
    if ( !xzp_list( a, entries, start ) ) return {};
    for ( auto &e : entries )
        if ( e.name == name ) return std::vector<uint8_t>( a.begin() + start + e.offset, a.begin() + start + e.offset + e.size );
    return {};
}

XurObject *path( XurFile &f, std::initializer_list<const char16_t *> ids )
{
    XurObject *o = &f.root;
    for ( auto *id : ids )
        if ( !o || !( o = o->child( id, f ) ) ) return nullptr;
    return o;
}

// The Main Menu tab: Exit to Desktop under Quit (the same in the disc's menu and a DLC's).
void add_exit_button( XurEdit &e, XurFile &f )
{
    // The game's entries move up a row to make its place (the tab's timelines only fade them; a DLC's menu
    // has more of them). A new Id: nothing in the game knows it, xui.cpp presses it.
    XurObject *tab = path( f, { u"Menu", u"Menu", u"Tab2" } );
    XurObject *quit = tab ? tab->child( u"quit", f ) : nullptr, *top = tab ? tab->child( u"campaign", f ) : nullptr, *header = tab ? tab->child( u"mainMenuButton", f ) : nullptr;
    if ( !quit || !top || !header ) { rt_log( "xui options: no Quit entry in the menu\n" ); return; }
    auto up_a_row = [&]( XurObject *o ) {
        const XurRef *p = o ? o->prop( "Position" ) : nullptr;
        const XurFile::Section *v = f.section( "VECT" );
        if ( !p || !v ) return;
        const uint8_t *at = &f.bytes[p->at], *xyz = &f.bytes[v->offset + 12 * ( (uint32_t)at[0] << 24 | at[1] << 16 | at[2] << 8 | at[3] )];
        auto number = []( const uint8_t *b ) { uint32_t u = (uint32_t)b[0] << 24 | b[1] << 16 | b[2] << 8 | b[3]; float n; memcpy( &n, &u, 4 ); return n; };
        e.set_position( *o, number( xyz ), number( xyz + 4 ) - 38 );
    };
    for ( const char16_t *id : { u"campaign", u"timeTrials", u"StreetRacing", u"CheatMode", u"DLC", u"extract", u"quit" } ) up_a_row( tab->child( id, f ) );
    auto b = e.copy( f, *quit, tab, u"exitDesktop" );
    b.set_string( e, "Text", u"Exit to Desktop" );
    b.set_string( e, "NavUp", u"quit" );
    b.set_string( e, "NavDown", u"campaign" );
    b.set_position( e, 515, 525 );  // where Quit was
    e.add_child( *tab, b );
    e.set_string( *quit, "NavDown", u"exitDesktop" );
    e.set_string( *top, "NavUp", u"exitDesktop" );
    e.set_string( *header, "NavUp", u"exitDesktop" );
}

// The Options tab: Keyboard Controls under Controller Settings (the entries above move up a row; there is
// only Controller Settings). Its press opens the page drawn by the runtime (keyspage.cpp, xui.cpp), so it
// has no page of the game's to go to.
void add_keys_button( XurEdit &e, XurFile &f )
{
    XurObject *tab = path( f, { u"Menu", u"Menu", u"Tab1" } );
    XurObject *pad = tab ? tab->child( u"controllerSettings", f ) : nullptr;
    XurObject *below = tab ? tab->child( u"liveSetup", f ) : nullptr;
    if ( tab && !below ) below = tab->child( u"recompSetup", f );
    if ( !pad || !below ) { rt_log( "xui options: no Controller Settings entry in the menu\n" ); return; }
    const XurRef *p = pad->prop( "Position" );
    const XurFile::Section *v = f.section( "VECT" );
    if ( !p || !v ) return;
    const uint8_t *at = &f.bytes[p->at], *xyz = &f.bytes[v->offset + 12 * ( (uint32_t)at[0] << 24 | at[1] << 16 | at[2] << 8 | at[3] )];
    auto number = []( const uint8_t *b ) { uint32_t u = (uint32_t)b[0] << 24 | b[1] << 16 | b[2] << 8 | b[3]; float n; memcpy( &n, &u, 4 ); return n; };
    const float x = number( xyz ), y = number( xyz + 4 );
    auto b = e.copy( f, *pad, tab, u"keyboardControls" );
    b.set_string( e, "Text", u"Keyboard Controls" );
    b.set_string( e, "PressPath", u"" );
    b.set_string( e, "NavUp", u"controllerSettings" );
    b.set_string( e, "NavDown", below == tab->child( u"liveSetup", f ) ? u"liveSetup" : u"recompSetup" );
    b.set_position( e, x, y );  // where Controller Settings was
    e.add_child( *tab, b );
    e.set_position( *pad, x, y - 38 );
    e.set_string( *pad, "NavDown", u"keyboardControls" );
    e.set_string( *below, "NavUp", u"keyboardControls" );
}

// The menu: Options tab, Xbox Live Settings -> Recomp Options; Main Menu tab, Exit to Desktop.
std::vector<uint8_t> make_menu( const std::vector<uint8_t> &src )
{
    XurFile f;
    if ( !f.parse( src.data(), src.size() ) ) { rt_log( "xui options: menuHud_3.xur: %s\n", f.error.c_str() ); return {}; }
    XurObject *live = path( f, { u"Menu", u"Menu", u"Tab1", u"liveSetup" } );
    if ( !live || !live->prop( "Text" ) || !live->prop( "PressPath" ) ) { rt_log( "xui options: no Xbox Live Settings entry in the menu\n" ); return {}; }
    XurEdit e( f );
    e.set_string( *live, "Text", u"Recomp Options" );
    e.set_string( *live, "PressPath", u"RecompOptions.xur" );
    // Renamed (its Id, the links to it and its timelines alike): the game's menu code, which disables
    // the Live entry when not signed in to Live, doesn't find it, and the language's string table
    // (keyed by the entry's path) doesn't put Live's name back.
    e.rename_string( u"liveSetup", u"recompSetup" );
    add_exit_button( e, f );
    add_keys_button( e, f );
    return e.build();
}

// The Display Settings page (which a DLC's archive doesn't replace, as it does the menu): a Recomp
// Options button beside Restore Default Settings (below it for the d-pad), a copy of the menu's Display Settings entry (a
// navigation button: it opens its page itself), shown at once (not faded in with the page's timelines).
std::vector<uint8_t> make_display( const std::vector<uint8_t> &display, const std::vector<uint8_t> &menu )
{
    XurFile d, m;
    if ( !d.parse( display.data(), display.size() ) ) { rt_log( "xui options: DisplayOptions.xur: %s\n", d.error.c_str() ); return {}; }
    if ( !m.parse( menu.data(), menu.size() ) ) { rt_log( "xui options: menuHud_3.xur: %s\n", m.error.c_str() ); return {}; }
    XurObject *scene = path( d, { u"DisplayOptions" } ), *reset = scene ? scene->child( u"reset_button", d ) : nullptr;
    XurObject *preset = scene ? scene->child( u"preset", d ) : nullptr, *entry = path( m, { u"Menu", u"Menu", u"Tab1", u"displaySetup" } );
    if ( !reset || !preset || !entry ) { rt_log( "xui options: the Display Settings page isn't as expected\n" ); return {}; }
    XurEdit e( d );
    auto b = e.copy( m, *entry, nullptr, u"recompOptions" );
    b.set_string( e, "Text", u"Recomp Options" );
    b.set_string( e, "PressPath", u"RecompOptions.xur" );
    b.set_string( e, "NavUp", u"reset_button" );
    b.set_string( e, "NavDown", u"preset" );
    b.set_position( e, 692, 493 );  // right edge with the sliders' (the art is 83 narrower than Width, 14 in)
    b.set_float( "Width", 288 );
    b.set_float( "Height", 35 );
    // It looks and fades in as Restore Default Settings beside it does.
    b.set_string( e, "Visual", u"new-subMenuButton1" );
    b.set_float( "Opacity", 0 );
    b.timelines = e.copy( d, *reset, scene, u"recompOptions" ).timelines;
    e.add_child( *scene, b );
    e.set_string( *reset, "NavDown", u"recompOptions" );
    if ( const XurRef *w = reset->prop( "Width" ) ) e.set_float( w->at, 360 );  // (the game sets its text: all of it fits)
    e.set_string( *preset, "NavUp", u"recompOptions" );
    return e.build();
}

// The rows, top to bottom: (Id, label): On / Off switches, rows of three and the sliders Field of View, Upscaling, DLSS Model and
// Resolution. (The mouse's settings are on the Keyboard Controls page, keyspage.cpp.) The game won't load the
// page with many more objects in it (61 loads, 63 doesn't: each switch row is 5, a row of three 6, a slider
// row 2).
struct Row { const char16_t *id, *label; };
const Row ROWS[] = { { u"fov", u"Field of View" },  { u"fullscreen", u"Display Mode" },          { u"vsync", u"VSync" },
                     { u"fps60", u"Frame Rate" },   { u"resolution", u"Resolution" },             { u"antiAliasing", u"Anti-Aliasing" },
                     { u"anisotropy", u"Anisotropic Filtering" }, { u"upscaling", u"Upscaling (Restart)" }, { u"dlssModel", u"DLSS Model" },
                     { u"lod", u"Draw Distance" }, { u"showFps", u"Show FPS" }, { u"skipStart", u"Skip at Start" } };
constexpr float ROW_X = 121, ROW_Y = 92, ROW_STEP = 36.5f;
constexpr float BAR_LIFT = 12;  // a slider row's bar draws 12 lower in its box than a switch row's buttons: raised to match

// A control with this id anywhere under o.
XurObject *find_id( XurObject &o, const std::u16string &id, XurFile &f )
{
    if ( XurObject *c = o.child( id, f ) ) return c;
    for ( auto &c : o.children )
        if ( XurObject *in = find_id( c, id, f ) ) return in;
    return nullptr;
}

// The page's buttons along its foot (runtime/xui.cpp acts on them).
struct FootButton { const char16_t *id, *label; float x, w; };
const FootButton FOOT[] = { { u"optApply", u"Apply", 153, 224 }, { u"optConfirm", u"Confirm", 389, 224 }, { u"optRestore", u"Reset Defaults", 625, 288 }, { u"optCancel", u"Cancel", 925, 224 } };
constexpr float FOOT_Y = 534;

std::vector<uint8_t> make_page( const std::vector<uint8_t> &controller, const std::vector<uint8_t> &audio, const std::vector<uint8_t> &popup, const std::vector<uint8_t> &display )
{
    XurFile c, a, three, pop, dis;
    if ( !dis.parse( display.data(), display.size() ) ) { rt_log( "xui options: DisplayOptions.xur: %s\n", dis.error.c_str() ); return {}; }
    XurObject *dis_scene = path( dis, { u"DisplayOptions" } ), *dis_reset = dis_scene ? dis_scene->child( u"reset_button", dis ) : nullptr;
    if ( !dis_reset ) { rt_log( "xui options: no Restore Defaults button\n" ); return {}; }
    if ( !pop.parse( popup.data(), popup.size() ) ) { rt_log( "xui options: timeTrialPopUp.xur: %s\n", pop.error.c_str() ); return {}; }
    XurObject *foot_button = find_id( pop.root, u"continueButton", pop );
    if ( !foot_button ) { rt_log( "xui options: no button for the page's foot\n" ); return {}; }
    if ( !c.parse( controller.data(), controller.size() ) ) { rt_log( "xui options: controllerOptions.xur: %s\n", c.error.c_str() ); return {}; }
    // The vibration row with a third button, for the Resolution row.
    {
        XurObject *row = path( c, { u"controllerOptions", u"vibration" } ), *group = row ? row->child( u"options", c ) : nullptr;
        XurObject *on = group ? group->child( u"On", c ) : nullptr, *off = group ? group->child( u"Off", c ) : nullptr;
        if ( !on || !off ) { rt_log( "xui options: the vibration row isn't as expected\n" ); return {}; }
        XurEdit e( c );
        auto third = e.copy( c, *off, group, u"Third" );
        third.set_position( e, 470, 0 );
        third.set_string( e, "NavLeft", u"Off" );
        third.set_string( e, "NavRight", u"On" );
        e.add_child( *group, third );
        e.set_string( *on, "NavLeft", u"Third" );
        e.set_string( *off, "NavRight", u"Third" );
        std::vector<uint8_t> made = e.build();
        if ( !three.parse( made.data(), made.size() ) ) { rt_log( "xui options: three buttons: %s\n", three.error.c_str() ); return {}; }
    }
    XurObject *vibration3 = path( three, { u"controllerOptions", u"vibration" } );
    if ( !vibration3 ) return {};
    if ( !a.parse( audio.data(), audio.size() ) ) { rt_log( "xui options: audio.xur: %s\n", a.error.c_str() ); return {}; }
    XurObject *scene = path( c, { u"controllerOptions" } ), *ascene = path( a, { u"AudioOptions" } );
    XurObject *vibration = scene ? scene->child( u"vibration", c ) : nullptr, *invert = scene ? scene->child( u"invert", c ) : nullptr;
    XurObject *layout = scene ? scene->child( u"layout", c ) : nullptr, *diagram = scene ? scene->child( u"XuiTabScene1", c ) : nullptr;
    XurObject *title = scene ? scene->child( u"pageTitle", c ) : nullptr, *footer = scene ? scene->child( u"footer", c ) : nullptr;
    XurObject *bar = ascene ? ascene->child( u"audioVol-background", a ) : nullptr, *slider = ascene ? ascene->child( u"musicSlider", a ) : nullptr;
    if ( !vibration || !invert || !layout || !diagram || !title || !footer || !bar || !slider ) { rt_log( "xui options: the pages aren't as expected\n" ); return {}; }
    XurEdit e( c );
    e.set_string( *scene, "Id", u"RecompOptions" );
    e.set_string( *scene, "DefaultFocus", ROWS[0].id );
    e.set_string( *title, "Text", u"Recomp Options" );
    size_t n = sizeof( ROWS ) / sizeof( ROWS[0] );
    auto up = [&]( size_t i ) { return i ? ROWS[i - 1].id : FOOT[0].id; };
    auto down = [&]( size_t i ) { return i + 1 < n ? ROWS[i + 1].id : FOOT[0].id; };
    e.set_string( *footer, "NavUp", ROWS[n - 1].id );
    e.set_string( *footer, "NavDown", ROWS[0].id );
    // The sliders (Resolution, Field of View, Upscaling, DLSS Model: the Audio page's bar with its label, its slider over it; their
    // labels show the value, xui.cpp); the switches (the vibration row: On / Off).
    float y0 = ROW_Y;
    for ( size_t i = 0; i < n; ++i )
    {
        const std::u16string id = ROWS[i].id;
        if ( id == u"resolution" || id == u"fov" || id == u"upscaling" || id == u"dlssModel" || id == u"antiAliasing" || id == u"anisotropy" || id == u"lod" )
        {
            auto rbar = e.copy( a, *bar, ascene, ROWS[i].id + std::u16string( u"Bar" ) );
            rbar.set_string( e, "Text", ROWS[i].label );
            rbar.set_position( e, ROW_X, y0 + ROW_STEP * i - BAR_LIFT );
            e.add_child( *scene, rbar );
            auto rs = e.copy( a, *slider, ascene, ROWS[i].id );
            rs.set_position( e, ROW_X + 248, y0 + ROW_STEP * i - BAR_LIFT - 29 );
            rs.set_string( e, "NavUp", up( i ) );
            rs.set_string( e, "NavDown", down( i ) );
            e.add_child( *scene, rs );
            continue;
        }
        bool res = false, aa = ROWS[i].id == std::u16string( u"skipStart" ), display = ROWS[i].id == std::u16string( u"fullscreen" ), fsr = false;
        auto row = res || aa || display || fsr ? e.copy( three, *vibration3, path( three, { u"controllerOptions" } ), ROWS[i].id ) : e.copy( c, *vibration, scene, ROWS[i].id );
        row.set_position( e, ROW_X, y0 + ROW_STEP * i );
        row.set_string( e, "Text", ROWS[i].label, res || aa || display || fsr ? 3 : 2 );  // (the row's radio buttons have texts first)
        if ( display )
        {
            row.set_string( e, "Text", u"Windowed", 0 );
            row.set_string( e, "Text", u"Borderless", 1 );
            row.set_string( e, "Text", u"Fullscreen", 2 );
        }
        if ( aa )
        {
            row.set_string( e, "Text", u"None", 0 );
            row.set_string( e, "Text", u"Videos", 1 );
            row.set_string( e, "Text", u"Videos, Title", 2 );
        }
        if ( ROWS[i].id == std::u16string( u"fps60" ) )
        {
            row.set_string( e, "Text", u"60", 0 );
            row.set_string( e, "Text", u"30", 1 );
        }
        row.set_string( e, "NavUp", up( i ) );
        row.set_string( e, "NavDown", down( i ) );
        e.add_child( *scene, row );
    }
    constexpr size_t FOOTS = sizeof( FOOT ) / sizeof( FOOT[0] );
    for ( size_t i = 0; i < FOOTS; ++i )
    {
        auto b = e.copy( pop, *foot_button, nullptr, FOOT[i].id );
        b.set_string( e, "Text", FOOT[i].label );
        b.set_position( e, FOOT[i].x, FOOT_Y );
        b.set_float( "Width", FOOT[i].w );
        b.set_float( "Height", 35 );
        b.set_float( "Opacity", 0 );
        b.set_string( e, "NavLeft", FOOT[( i + FOOTS - 1 ) % FOOTS].id );
        b.set_string( e, "NavRight", FOOT[( i + 1 ) % FOOTS].id );
        b.set_string( e, "NavUp", ROWS[n - 1].id );
        b.set_string( e, "NavDown", ROWS[0].id );
        b.timelines = e.copy( dis, *dis_reset, dis_scene, FOOT[i].id ).timelines;  // fades in as the Display page's button (a timeline names properties by class: same class)
        e.add_child( *scene, b );
    }
    e.remove_child( *scene, *diagram );
    e.remove_child( *scene, *vibration );
    e.remove_child( *scene, *invert );
    e.remove_child( *scene, *layout );
    return e.build();
}
} // namespace

// The archive to open in place of one of the game's menu archives at host (the disc's
// XUIFrontEnd\Scene.xzp, a DLC's Scene.xzp or Scene_CommonDLC.xzp): the same with the Recomp Options page
// (made from the disc's pages, scenes) and, where it has the menu (menuHud_3.xur), the menu's entry for
// it; made once into the cache folder. The game's archive if it can't be made.
// The menus' button pictures (A and B along their foot: "A Select", "B Back") as the keys that do it with the
// keyboard -- Enter and Esc -- when the game starts without a controller (the archive is read once).
bool keyboard_prompts()
{
    static const bool kb = [] {
        if ( const char *e = getenv( "CRACKDOWN_PROMPTS" ) ) return !strcmp( e, "keyboard" );
        SDL_SetHint( SDL_HINT_NO_SIGNAL_HANDLERS, "1" );  // (SDL's own would take SIGTERM for a quit event nobody reads headless)
        SDL_InitSubSystem( SDL_INIT_GAMEPAD );
        const bool pad = SDL_HasGamepad();
        rt_log( "xui options: menu button pictures for the %s\n", pad ? "controller" : "keyboard" );
        return !pad;
    }();
    return kb;
}
void add_key_pictures( const std::vector<uint8_t> &a, std::vector<std::pair<std::u16string, std::vector<uint8_t>>> &files )
{
    if ( !keyboard_prompts() ) return;
    const struct { const char16_t *name; const char *label; } pics[] = { { u"xui-assets-3\\A.png", "ENTER" }, { u"xui-assets-3\\B.png", "ESC" } };
    for ( auto &p : pics )
        if ( !entry( a, p.name ).empty() )
            if ( std::vector<uint8_t> png = keycap_png( p.label, 40 ); !png.empty() ) files.push_back( { p.name, png } );
}

std::string xui_options_archive( const std::string &host, const std::string &scenes )
{
    static std::mutex lock;
    std::lock_guard<std::mutex> l( lock );
    struct stat st;
    if ( stat( host.c_str(), &st ) ) return host;
    const char *xdg = getenv( "XDG_CACHE_HOME" ), *home = getenv( "HOME" );
    std::string dir = ( xdg && *xdg ? std::string( xdg ) : std::string( home ? home : "." ) + "/.cache" ) + "/crackdown-recomp/xui";
    std::string name = std::filesystem::path( host ).stem().string();
    std::string out = dir + "/" + name + "-" + std::to_string( st.st_size ) + "-" + std::to_string( (long long)st.st_mtime ) + ( keyboard_prompts() ? "-28k.xzp" : "-28.xzp" );
    if ( !access( out.c_str(), R_OK ) ) return out;
    std::vector<uint8_t> a = read_file( host ), s = scenes == host ? a : read_file( scenes );
    std::vector<uint8_t> page = make_page( entry( s, u"controllerOptions.xur" ), entry( s, u"audio.xur" ), entry( s, u"timeTrialPopUp.xur" ), entry( s, u"DisplayOptions.xur" ) );
    if ( page.empty() ) return host;
    std::vector<std::pair<std::u16string, std::vector<uint8_t>>> files = { { u"RecompOptions.xur", page } };
    std::vector<uint8_t> display = make_display( entry( s, u"DisplayOptions.xur" ), entry( s, u"menuHud_3.xur" ) );
    if ( !display.empty() ) files.push_back( { u"DisplayOptions.xur", display } );
    if ( !entry( a, u"menuHud_3.xur" ).empty() )
    {
        std::vector<uint8_t> menu = make_menu( entry( a, u"menuHud_3.xur" ) );
        if ( menu.empty() ) return host;
        files.push_back( { u"menuHud_3.xur", menu } );
    }
    add_key_pictures( a, files );
    std::vector<uint8_t> b = xzp_build( a, files );
    if ( b.empty() ) return host;
    std::error_code ec;
    std::filesystem::create_directories( dir, ec );
    {
        std::ofstream f( out + ".new", std::ios::binary );
        f.write( (const char *)b.data(), (std::streamsize)b.size() );
    }
    std::filesystem::rename( out + ".new", out, ec );
    if ( ec ) return host;
    rt_log( "xui options: %s with the Recomp Options page made (%s)\n", host.c_str(), out.c_str() );
    return out;
}

// A DLC's menu archive (Scene_CommonDLC.xzp), which is the menu the game shows once the DLC is installed:
// the same with Exit to Desktop, made once into the cache folder. The game's archive if it can't be made.
std::string xui_menu_archive( const std::string &host )
{
    static std::mutex lock;
    std::lock_guard<std::mutex> l( lock );
    struct stat st;
    if ( stat( host.c_str(), &st ) ) return host;
    const char *xdg = getenv( "XDG_CACHE_HOME" ), *home = getenv( "HOME" );
    std::string dir = ( xdg && *xdg ? std::string( xdg ) : std::string( home ? home : "." ) + "/.cache" ) + "/crackdown-recomp/xui";
    std::string name = std::filesystem::path( host ).stem().string();
    std::string out = dir + "/" + name + "-" + std::to_string( st.st_size ) + "-" + std::to_string( (long long)st.st_mtime ) + "-menu-2.xzp";
    if ( !access( out.c_str(), R_OK ) ) return out;
    std::vector<uint8_t> a = read_file( host ), src = entry( a, u"menuHud_3.xur" );
    XurFile f;
    if ( src.empty() || !f.parse( src.data(), src.size() ) ) { rt_log( "xui options: no menu in %s\n", host.c_str() ); return host; }
    XurEdit e( f );
    add_exit_button( e, f );
    add_keys_button( e, f );
    std::vector<uint8_t> b = xzp_build( a, { { u"menuHud_3.xur", e.build() } } );
    if ( b.empty() ) return host;
    std::error_code ec;
    std::filesystem::create_directories( dir, ec );
    {
        std::ofstream o( out + ".new", std::ios::binary );
        o.write( (const char *)b.data(), (std::streamsize)b.size() );
    }
    std::filesystem::rename( out + ".new", out, ec );
    if ( ec ) return host;
    rt_log( "xui options: %s with Exit to Desktop made (%s)\n", host.c_str(), out.c_str() );
    return out;
}
