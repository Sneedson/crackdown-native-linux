// Read-only view of the game's XUI (Microsoft's 360 UI library, linked into the game without symbols)
// for mouse support in the menus. Layout found in the game's code:
//   handle table at 82E795F8: { entries, ?, count }; entry (12 bytes) { u16 generation, u16 flags (bit 0
//     valid), u32 node }; a handle is generation << 16 | index (sub_82762DB8).
//   node: +0 instance handle, +4 next node (one per class of the object), +20 class handle, +28 data
//     (sub_82762900, sub_82761F20: class data of an object = node of that class -> its instance's +28).
//   XuiElement class handle at 835D22F8. XuiElement data: +0 handle, +24/+32/+36/+40 related elements'
//     data, +44 position (x, y, z), +56 scale, +68 rotation (quaternion), +236 flags.
//   focus: per user at 835F3270 + user * 4, "any user" at 835D2E78 (sub_82768A58 sets it).
#include "ppc_config.h"
#include "ppc_context.h"
#include "runtime.h"
#include "settings.h"
#include <atomic>
#include <cstdio>
#include <cstring>
#include "input.h"
#include <algorithm>
#include <memory>
#include <unistd.h>
#include <chrono>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <linux/close_range.h>
#include <pthread.h>
#include <unistd.h>
#include <vector>
#include <cmath>
#include <string>
#include <functional>
#include <vector>

namespace
{
constexpr uint32_t HANDLE_TABLE = 0x82E795F8, ELEMENT_CLASS = 0x835D22F8, FOCUS_USER = 0x835F3270, FOCUS_ANY = 0x835D2E78;

uint32_t node_of( uint32_t h )
{
    uint32_t entries = guest_read32( HANDLE_TABLE ), count = guest_read32( HANDLE_TABLE + 8 ), idx = h & 0xFFFF;
    if ( !entries || idx >= count ) return 0;
    uint32_t e = entries + idx * 12;
    if ( !( guest_read16( e + 2 ) & 1 ) || guest_read16( e ) != h >> 16 ) return 0;
    return guest_read32( e + 4 );
}
}

// Object handle of a class-part handle (an element's data starts with its XuiElement part's handle;
// focus and messages use the object's): node +12 -> +0 (sub_82762DB8).
uint32_t xui_object( uint32_t part )
{
    uint32_t n = node_of( part );
    return n && guest_read32( n + 12 ) ? guest_read32( guest_read32( n + 12 ) ) : 0;
}

uint32_t xui_class_data( uint32_t h, uint32_t cls )
{
    if ( !h || !cls ) return 0;
    for ( uint32_t n = node_of( h ), guard = 0; n && guard < 64; n = guest_read32( n + 4 ), ++guard )
        if ( guest_read32( n + 20 ) == cls )
        {
            uint32_t inst = node_of( guest_read32( n ) );
            return inst ? guest_read32( inst + 28 ) : 0;
        }
    return 0;
}

uint32_t xui_element( uint32_t h ) { return xui_class_data( h, guest_read32( ELEMENT_CLASS ) ); }
uint32_t xui_focus( int user ) { return guest_read32( user < 0 ? FOCUS_ANY : FOCUS_USER + user * 4 ); }

// Debug dump of an element's data: words as hex, floats where they look like floats.
void xui_dump( const char *why, uint32_t h )
{
    uint32_t e = xui_element( h );
    std::string s;
    char b[48];
    for ( uint32_t o = 0; e && o < 244; o += 4 )
    {
        snprintf( b, sizeof( b ), " %u:%08X", o, guest_read32( e + o ) );
        s += b;
    }
    rt_log( "xui: %s handle %08X data %08X:%s\n", why, h, e, s.c_str() );
}

namespace
{
// Classes whose elements can take focus (their subclasses included).
constexpr uint32_t FOCUSABLE_CLASSES[] = { 0x835D2310 /* XuiButton */, 0x835D231C /* XuiCheckbox */, 0x835D2320 /* XuiRadioButton */,
                                           0x835D2338 /* XuiListItem */, 0x835D2348 /* XuiEdit */, 0x82E79634 /* XuiSlider */,
                                           0x835D235C /* XuiTabScene */ };
constexpr uint32_t SCENE_CLASS = 0x835D230C, TAB_SCENE_CLASS = 0x835D235C;
constexpr uint32_t RADIO_CLASS = 0x835D2320;
constexpr uint32_t SET_FOCUS = 0x82768A58;  // (object handle, user or 255)
// XuiTabScene: its message proc (82781DF8) steps tabs on d-pad / shoulder navigation through 82781A48 and
// 82781B58, which play the tab transition; 82781C58 (tab scene data, index, play transition) goes to a tab.
constexpr uint32_t TAB_SCENE_GOTO = 0x82781C58, TAB_SCENE_NEXT = 0x82781A48, TAB_SCENE_PREV = 0x82781B58;
// XuiSlider data: +4 SliderBody (object handle), +16 min, +20 max, +24 value, +28 step. 8277DE98 (data,
// value) is its set-value: clamps, updates the ScrollMin / ScrollMax arrows and notifies the scene, which
// applies the setting. The thumb (SliderBody's first child) is placed by 8277E578 playing SliderBody's
// timeline to the frame for the value.
constexpr uint32_t SLIDER_CLASS = 0x82E79634, SLIDER_SET_VALUE = 0x8277DE98;
constexpr uint32_t CONTROL_CLASS = 0x835D2304;

std::string wide_string( uint32_t a )
{
    std::string s;
    for ( int i = 0; a && i < 64; ++i )
    {
        uint16_t c = guest_read16( a + i * 2 );
        if ( !c ) break;
        s += c < 128 ? (char)c : '?';
    }
    return s;
}

float f32( uint32_t a ) { uint32_t v = guest_read32( a ); float f; memcpy( &f, &v, 4 ); return f; }

bool focusable( uint32_t h )
{
    for ( uint32_t c : FOCUSABLE_CLASSES )
        if ( xui_class_data( h, guest_read32( c ) ) ) return true;
    return false;
}

// Absolute top-left of an element: positions summed up the parent chain (scale and rotation ignored).
void absolute_pos( uint32_t e, float &x, float &y )
{
    x = y = 0;
    for ( int guard = 0; e && guard < 32; e = guest_read32( e + 40 ), ++guard ) { x += f32( e + 44 ); y += f32( e + 48 ); }
}

void list_controls( uint32_t e, int depth )
{
    for ( uint32_t c = guest_read32( e + 24 ), guard = 0; c && guard < 256; c = guest_read32( c + 32 ), ++guard )
    {
        uint32_t h = xui_object( guest_read32( c ) );
        if ( focusable( h ) )
        {
            float x, y;
            absolute_pos( c, x, y );
            uint32_t ctl = xui_class_data( h, guest_read32( CONTROL_CLASS ) );
            std::string words;
            char b[16];
            for ( uint32_t o = 0; ctl && o < 32; o += 4 ) { snprintf( b, sizeof( b ), " %08X", guest_read32( ctl + o ) ); words += b; }
            rt_log( "xui: %*scontrol %08X '%s' at %.0f,%.0f %.0fx%.0f opacity %.2f flags %08X control:%s\n", depth * 2, "", h,
                    wide_string( guest_read32( c + 4 ) ).c_str(), x, y, f32( c + 12 ), f32( c + 16 ), f32( c + 20 ), guest_read32( c + 236 ),
                    words.c_str() );
        }
        list_controls( c, depth + 1 );
    }
}
}

namespace
{
const char *class_name( uint32_t cls )
{
    static const struct { uint32_t global; const char *name; } known[] = {
        { 0x835D22F8, "Element" }, { 0x835D22FC, "Canvas" }, { 0x835D22F0, "Text" }, { 0x835D22F4, "Label" }, { 0x835D2300, "Figure" },
        { 0x835D2304, "Control" }, { 0x835D2308, "Visual" }, { 0x835D230C, "Scene" }, { 0x835D2310, "Button" }, { 0x835D2314, "NavButton" },
        { 0x835D2318, "BackButton" }, { 0x835D231C, "Checkbox" }, { 0x835D2320, "RadioButton" }, { 0x835D2324, "RadioGroup" },
        { 0x835D2328, "TextPresenter" }, { 0x835D232C, "Group" }, { 0x835D2330, "Sound" }, { 0x835D2334, "Transition" }, { 0x835D2338, "ListItem" },
        { 0x835D233C, "ScrollEnd" }, { 0x835D2340, "List" }, { 0x835D2344, "CommonList" }, { 0x82E79638, "ProgressBar" }, { 0x82E79634, "Slider" },
        { 0x835D2348, "Edit" }, { 0x835D234C, "Caret" }, { 0x835D2350, "MessageBox" }, { 0x835D2354, "ImagePresenter" }, { 0x835D2358, "Image" },
        { 0x835D235C, "TabScene" }, { 0x835D2360, "NineGrid" } };
    for ( auto &k : known )
        if ( guest_read32( k.global ) == cls ) return k.name;
    return nullptr;
}
std::string classes( uint32_t h )
{
    std::string s;
    char b[16];
    for ( uint32_t n = node_of( h ), g = 0; n && g < 12; n = guest_read32( n + 4 ), ++g )
    {
        const char *nm = class_name( guest_read32( n + 20 ) );
        if ( !nm ) { snprintf( b, sizeof( b ), "%08X", guest_read32( n + 20 ) ); nm = b; }
        s += s.empty() ? "" : "<";
        s += nm;
    }
    return s;
}
void tree( uint32_t e, int depth )
{
    for ( uint32_t c = guest_read32( e + 24 ), guard = 0; c && guard < 256 && depth < 8; c = guest_read32( c + 32 ), ++guard )
    {
        float x, y;
        absolute_pos( c, x, y );
        rt_log( "xui: %*s'%s' [%s] %.0f,%.0f %.0fx%.0f op %.2f flags %08X h %08X\n", depth * 2, "", wide_string( guest_read32( c + 4 ) ).c_str(),
                classes( xui_object( guest_read32( c ) ) ).c_str(), x, y, f32( c + 12 ), f32( c + 16 ), f32( c + 20 ), guest_read32( c + 236 ), xui_object( guest_read32( c ) ) );
        tree( c, depth + 1 );
    }
}
}

// Debug: the element tree of the scene around h, with class chains.
void xui_tree( uint32_t h )
{
    uint32_t root = xui_element( h );
    for ( uint32_t p; root && ( p = guest_read32( root + 40 ) ) && xui_element( guest_read32( p ) ) == p; ) root = p;
    if ( root ) tree( root, 0 );
}

// Debug: every root element (no element parent) in XUI's handle table, with its tree.
void xui_all_roots()
{
    uint32_t entries = guest_read32( HANDLE_TABLE ), count = guest_read32( HANDLE_TABLE + 8 );
    for ( uint32_t i = 0; entries && i < count; ++i )
    {
        uint32_t e = entries + i * 12;
        if ( !( guest_read16( e + 2 ) & 1 ) ) continue;
        uint32_t h = (uint32_t)guest_read16( e ) << 16 | i, el = xui_element( h );
        if ( !el || xui_object( guest_read32( el ) ) != h ) continue;
        uint32_t p = guest_read32( el + 40 );
        if ( p && xui_element( guest_read32( p ) ) == p ) continue;
        rt_log( "xui root %08X '%s' [%s] %.0fx%.0f op %.2f\n", h, wide_string( guest_read32( el + 4 ) ).c_str(), classes( h ).c_str(), f32( el + 12 ), f32( el + 16 ), f32( el + 20 ) );
        tree( el, 1 );
    }
}

// Debug: every focusable control under the element's scene root.
void xui_list_controls( uint32_t h )
{
    uint32_t root = xui_element( h ), depth = 0;
    for ( uint32_t p; root && ( p = guest_read32( root + 40 ) ) && xui_element( guest_read32( p ) ) == p && depth < 32; ++depth ) root = p;
    for ( uint32_t e = xui_element( h ), g = 0; e && g < 10; e = guest_read32( e + 40 ), ++g )
    {
        rt_log( "xui: up%u '%s' %08X scene %d %.0f,%.0f %.0fx%.0f\n", g, wide_string( guest_read32( e + 4 ) ).c_str(), xui_object( guest_read32( e ) ),
                xui_class_data( xui_object( guest_read32( e ) ), guest_read32( SCENE_CLASS ) ) != 0, f32( e + 44 ), f32( e + 48 ), f32( e + 12 ), f32( e + 16 ) );
        rt_log( "xui:   full matrix %.3f %.3f / %.3f %.3f / %.1f %.1f (flags %08X)\n", f32( e + 160 ), f32( e + 164 ), f32( e + 176 ),
                f32( e + 180 ), f32( e + 208 ), f32( e + 212 ), guest_read32( e + 236 ) );
    }
    rt_log( "xui: root %08X ('%s', %u levels up, %.0fx%.0f)\n", root, wide_string( guest_read32( root + 4 ) ).c_str(), depth, f32( root + 12 ), f32( root + 16 ) );
    if ( root ) list_controls( root, 0 );
}

namespace
{
// Calls a guest function on the current guest thread with the import's context (restored afterwards).
uint32_t guest_call( PPCContext &c, uint32_t addr, uint32_t a0, uint32_t a1, uint32_t a2 = 0, uint32_t a3 = 0 )
{
    PPCContext saved = c;
    c.r3.u64 = a0;
    c.r4.u64 = a1;
    c.r5.u64 = a2;
    c.r6.u64 = a3;
    c.r1.u64 = ( saved.r1.u32 - 1024 ) & ~15u;  // below the scratch area engine_hit builds its message in
    rt_lookup( addr )( (uintptr_t)&c + 0x80, (uintptr_t)g_base, 0 );
    uint32_t r = c.r3.u32;
    c = saved;
    return r;
}

// XUI's own hit test: message 3 with data {int x, int y, BOOL inside}. The element's handler (827739E0
// for plain elements; figures test their shape) maps the point through the inverse of the element's full
// transform (element+160, built by 82773340 from position, scale, rotation and pivot) and checks it
// against the element's size, so slides, scaling and hidden elements are the engine's business.
constexpr uint32_t SEND_MESSAGE = 0x82762B98;  // (object handle, XUIMessage *)
bool engine_hit( PPCContext &c, uint32_t h, float x, float y )
{
    uint32_t msg = ( c.r1.u32 - 512 ) & ~15u, data = msg + 32;
    guest_write32( msg + 0, 20 );  // cbSize
    guest_write32( msg + 4, 3 );   // message
    guest_write32( msg + 8, 0 );   // handled
    guest_write32( msg + 12, 0 );
    guest_write32( msg + 16, data );
    guest_write32( data + 0, (uint32_t)(int32_t)x );
    guest_write32( data + 4, (uint32_t)(int32_t)y );
    guest_write32( data + 8, 0 );
    guest_call( c, SEND_MESSAGE, h, msg );
    return guest_read32( data + 8 ) != 0;
}

// The largest visible leaf under e (its art: a button's nine-grid, a slider's track), whose box is centred
// on what is drawn even where the control's layout box is not (art carries transparent padding).
void largest_art( uint32_t e, uint32_t &best, float &area, int depth = 0 )
{
    for ( uint32_t c = guest_read32( e + 24 ), guard = 0; c && guard < 256 && depth < 16; c = guest_read32( c + 32 ), ++guard )
    {
        if ( f32( c + 20 ) <= 0.01f || !( guest_read32( c + 236 ) & 1 ) ) continue;
        if ( guest_read32( c + 24 ) ) { largest_art( c, best, area, depth + 1 ); continue; }
        float a = f32( c + 12 ) * f32( c + 16 );
        if ( a > area ) { area = a; best = c; }
    }
}

// XuiList: message 2008 sets the current item ({int index}, clamped to the item count at list data +136,
// stored at +132), which is what the game acts on; the list moves the focus to its row (or to itself when
// the row is scrolled away). 2011 sets the top item (+128, clamped; 2010 reads it), scrolling the rows.
constexpr uint32_t LIST_CLASS = 0x835D2340;
void send_int_message( PPCContext &c, uint32_t h, uint32_t id, int32_t value )
{
    uint32_t msg = ( c.r1.u32 - 512 ) & ~15u, data = msg + 32;
    guest_write32( msg + 0, 20 );
    guest_write32( msg + 4, id );
    guest_write32( msg + 8, 0 );
    guest_write32( msg + 12, 0 );
    guest_write32( msg + 16, data );
    guest_write32( data, (uint32_t)value );
    guest_call( c, SEND_MESSAGE, h, msg );
}

// The menu's Exit to Desktop (added to the Main Menu tab, xui_options.cpp): closes the game, as closing the
// window does. The game's Quit beside it stays the game's: its setup 826F4AF0 hooks its press in a game
// (it asks to save and comes back to the menu) and disables it at the main menu, with no game to leave.
// The help line: the setup's help object (its third argument) maps control handles
// to texts, shown when a control takes the focus; 826D7478 (help, &handle, string) adds a text of our own.
constexpr uint32_t HELP_ADD_TEXT = 0x826D7478, STRING_MAKE = 0x8214CAB0, STRING_FREE = 0x8214C578;
uint32_t g_menu_help;
bool is_exit( uint32_t el ) { return el && wide_string( guest_read32( el + 4 ) ) == "exitDesktop"; }
bool g_exit_requested;
bool is_keys( uint32_t el ) { return el && wide_string( guest_read32( el + 4 ) ) == "keyboardControls"; }
}  // namespace
void keyspage_show();  // keyspage.cpp
namespace
{

// The Recomp Options page's buttons along its foot: 0 none, else the choice for options_page.
enum OptionButton { OPT_NONE, OPT_APPLY, OPT_CONFIRM, OPT_RESTORE, OPT_CANCEL };
OptionButton g_option_pressed = OPT_NONE;
OptionButton option_button( uint32_t el )
{
    if ( !el ) return OPT_NONE;
    std::string id = wide_string( guest_read32( el + 4 ) );
    return id == "optApply" ? OPT_APPLY : id == "optConfirm" ? OPT_CONFIRM : id == "optRestore" ? OPT_RESTORE : id == "optCancel" ? OPT_CANCEL : OPT_NONE;
}

// XUI's press (XM_PRESS, 2021; data 12 bytes with the user at +4): what the button key handler
// (827776A0) sends a control when A / Enter / Space comes up on it. The control plays its press and
// notifies its scene, whose handler (the game's menu code) acts.
constexpr uint32_t BACK_BUTTON_CLASS = 0x835D2318;
// The back button under e (an XuiBackButton's object handle), or 0.
uint32_t back_button( uint32_t e, int depth = 0 )
{
    for ( uint32_t ch = guest_read32( e + 24 ), g = 0; ch && g < 256 && depth < 16; ch = guest_read32( ch + 32 ), ++g )
    {
        const uint32_t h = xui_object( guest_read32( ch ) );
        if ( xui_class_data( h, guest_read32( BACK_BUTTON_CLASS ) ) ) return h;
        if ( uint32_t b = back_button( ch, depth + 1 ) ) return b;
    }
    return 0;
}

void press( PPCContext &c, uint32_t h, int user )
{
    if ( is_exit( xui_element( h ) ) ) { g_exit_requested = true; return; }
    if ( is_keys( xui_element( h ) ) ) { keyspage_show(); return; }  // (xui_menu_mouse: save first in a game)
    if ( OptionButton b = option_button( xui_element( h ) ) ) { g_option_pressed = b; return; }
    uint32_t msg = ( c.r1.u32 - 512 ) & ~15u, data = msg + 32;
    guest_write32( msg + 0, 20 );
    guest_write32( msg + 4, 2021 );
    guest_write32( msg + 8, 0 );
    guest_write32( msg + 12, 0 );
    guest_write32( msg + 16, data );
    for ( int i = 0; i < 12; i += 4 ) guest_write32( data + i, 0 );
    guest_write8( data + 4, (uint8_t)( user == 255 ? 0 : user ) );
    guest_call( c, SEND_MESSAGE, h, msg );
}

// The first control of class cls under e (visible or not), depth first.
uint32_t find_class( uint32_t e, uint32_t cls, int depth = 0 )
{
    for ( uint32_t c = guest_read32( e + 24 ), guard = 0; c && guard < 256 && depth < 24; c = guest_read32( c + 32 ), ++guard )
    {
        uint32_t h = xui_object( guest_read32( c ) );
        if ( xui_class_data( h, guest_read32( cls ) ) ) return h;
        if ( uint32_t inner = find_class( c, cls, depth + 1 ) ) return inner;
    }
    return 0;
}

// The object handle of the element named name under e, depth first.
uint32_t find_named( uint32_t e, const char *name, int depth = 0 )
{
    for ( uint32_t c = guest_read32( e + 24 ), guard = 0; c && guard < 256 && depth < 24; c = guest_read32( c + 32 ), ++guard )
    {
        if ( wide_string( guest_read32( c + 4 ) ) == name ) return xui_object( guest_read32( c ) );
        if ( uint32_t inner = find_named( c, name, depth + 1 ) ) return inner;
    }
    return 0;
}

// For a list item: its list's data, and in *index the item's place in the list (the visible rows are
// reused as it scrolls: index = the list's top item (+128) plus the row's place on screen, top to bottom
// or, for a side by side list like the campaign's agent skins, left to right; *across tells which).
uint32_t list_of( uint32_t el, int *index, bool *across = nullptr )
{
    uint32_t list = 0;
    for ( uint32_t e = guest_read32( el + 40 ), g = 0; e && g < 4 && !list; e = guest_read32( e + 40 ), ++g )
        if ( xui_class_data( xui_object( guest_read32( e ) ), guest_read32( LIST_CLASS ) ) ) list = e;
    if ( !list ) return 0;
    uint32_t data = xui_class_data( xui_object( guest_read32( list ) ), guest_read32( LIST_CLASS ) );
    uint32_t parent = guest_read32( el + 40 );
    int before = 0;
    bool side = false;
    for ( uint32_t c = guest_read32( parent + 24 ), guard = 0; c && guard < 256; c = guest_read32( c + 32 ), ++guard )
        if ( c != el && xui_class_data( xui_object( guest_read32( c ) ), guest_read32( 0x835D2338 ) ) && ( guest_read32( c + 236 ) & 1 ) )
        {
            float dy = f32( c + 212 ) - f32( el + 212 ), dx = f32( c + 208 ) - f32( el + 208 );
            if ( std::fabs( dy ) < 1.0f ) side = true;
            if ( dy < -1.0f || ( std::fabs( dy ) < 1.0f && dx < 0 ) ) ++before;
        }
    *index = (int32_t)guest_read32( data + 128 ) + before;
    if ( across ) *across = side;
    return data;
}

// A spinner: a list showing one item at a time between arrows, stepped with the d-pad's left / right
// (Street Racing's track and vehicle rows). The list element around el (el itself or up to 4 levels
// up), or 0.
uint32_t spinner_of( uint32_t el )
{
    for ( uint32_t e = el, g = 0; e && g < 5; e = guest_read32( e + 40 ), ++g )
    {
        uint32_t data = xui_class_data( xui_object( guest_read32( e ) ), guest_read32( LIST_CLASS ) );
        if ( !data ) continue;
        int items = 0;
        for ( uint32_t c = guest_read32( e + 24 ), guard = 0; c && guard < 256; c = guest_read32( c + 32 ), ++guard )
            if ( xui_class_data( xui_object( guest_read32( c ) ), guest_read32( 0x835D2338 ) ) && ( guest_read32( c + 236 ) & 1 ) ) ++items;
        return items == 1 && (int32_t)guest_read32( data + 136 ) > 1 ? e : 0;
    }
    return 0;
}

// A spinner's arrow (its list's 'control_ScrollLeft' / 'control_ScrollRight') at (x, y) under e: the
// spinner's element, with the d-pad direction in *dir (4 left, 8 right). The arrows reach past the row's
// own box, so the engine's hit test on the row misses them.
uint32_t spinner_arrow( uint32_t e, float x, float y, uint16_t *dir, int depth = 0 )
{
    for ( uint32_t c = guest_read32( e + 24 ), guard = 0; c && guard < 256 && depth < 24; c = guest_read32( c + 32 ), ++guard )
    {
        if ( f32( c + 20 ) <= 0.01f || !( guest_read32( c + 236 ) & 1 ) ) continue;
        std::string name = wide_string( guest_read32( c + 4 ) );
        bool left = name == "control_ScrollLeft", right = name == "control_ScrollRight";
        if ( ( left || right ) && spinner_of( e ) == e )
        {
            float x0 = f32( c + 208 ), y0 = f32( c + 212 ), w = f32( c + 160 ) * f32( c + 12 ), h = f32( c + 180 ) * f32( c + 16 );
            if ( x >= x0 - 4 && x < x0 + w + 4 && y >= y0 - 4 && y < y0 + h + 4 ) { *dir = left ? 0x4 : 0x8; return e; }
            continue;
        }
        if ( uint32_t s = spinner_arrow( c, x, y, dir, depth + 1 ) ) return s;
    }
    return 0;
}

// A side by side list's arrow at (x, y) under e: a visible element named '...LeftArrow' / '...RightArrow'
// beside the list (the Agency garage's ScrollVehicleLeftArrow / RightArrow either side of its middle
// vehicle card, over the cards' edges). The list's data, with the d-pad direction in *dir (4 left, 8 right).
uint32_t card_arrow( uint32_t e, float x, float y, uint16_t *dir, int depth = 0 )
{
    auto ends = []( const std::string &s, const char *t ) { size_t n = strlen( t ); return s.size() >= n && s.compare( s.size() - n, n, t ) == 0; };
    for ( uint32_t c = guest_read32( e + 24 ), guard = 0; c && guard < 256 && depth < 8; c = guest_read32( c + 32 ), ++guard )
    {
        if ( f32( c + 20 ) <= 0.01f || !( guest_read32( c + 236 ) & 1 ) ) continue;
        std::string name = wide_string( guest_read32( c + 4 ) );
        bool left = ends( name, "LeftArrow" ), right = ends( name, "RightArrow" );
        if ( left || right )
        {
            float x0 = f32( c + 208 ), y0 = f32( c + 212 ), w = f32( c + 160 ) * f32( c + 12 ), h = f32( c + 180 ) * f32( c + 16 );
            if ( !( x >= x0 - 4 && x < x0 + w + 4 && y >= y0 - 4 && y < y0 + h + 4 ) ) continue;
            for ( uint32_t s = guest_read32( e + 24 ), g = 0; s && g < 256; s = guest_read32( s + 32 ), ++g )
                for ( uint32_t item = guest_read32( s + 24 ), gi = 0; item && gi < 256; item = guest_read32( item + 32 ), ++gi )
                {
                    int index;
                    bool across = false;
                    uint32_t data;
                    if ( ( guest_read32( item + 236 ) & 1 ) && xui_class_data( xui_object( guest_read32( item ) ), guest_read32( 0x835D2338 ) ) &&
                         ( data = list_of( item, &index, &across ) ) && across )
                    {
                        *dir = left ? 0x4 : 0x8;
                        return data;
                    }
                }
            continue;
        }
        if ( uint32_t l = card_arrow( c, x, y, dir, depth + 1 ) ) return l;
    }
    return 0;
}

// Whether the list element has such arrows beside it.
bool has_card_arrows( uint32_t list )
{
    uint32_t parent = guest_read32( list + 40 );
    for ( uint32_t c = parent ? guest_read32( parent + 24 ) : 0, g = 0; c && g < 256; c = guest_read32( c + 32 ), ++g )
    {
        std::string n = wide_string( guest_read32( c + 4 ) );
        if ( n.size() > 9 && ( n.compare( n.size() - 9, 9, "LeftArrow" ) == 0 || n.compare( n.size() - 10, 10, "RightArrow" ) == 0 ) ) return true;
    }
    return false;
}

// Whether (x, y) is on any visible art under e, by the engine's hit test on each leaf (a tab scene's
// arrows sit outside its own box).
bool art_hit( PPCContext &ctx, uint32_t e, float x, float y, int depth = 0 )
{
    for ( uint32_t c = guest_read32( e + 24 ), guard = 0; c && guard < 256 && depth < 16; c = guest_read32( c + 32 ), ++guard )
    {
        if ( f32( c + 20 ) <= 0.01f || !( guest_read32( c + 236 ) & 1 ) ) continue;
        if ( guest_read32( c + 24 ) ? art_hit( ctx, c, x, y, depth + 1 ) : engine_hit( ctx, xui_object( guest_read32( c ) ), x, y ) ) return true;
    }
    return false;
}

// Squared distance from (x, y) to the centre of the control's art on screen (full transforms), or of its
// layout box when it has none.
// The same, across only: which side of a control a point is on.
float centre_x( uint32_t e )
{
    uint32_t art = 0;
    float area = 0;
    largest_art( e, art, area );
    if ( art ) e = art;
    return f32( e + 208 ) + f32( e + 160 ) * f32( e + 12 ) * 0.5f + f32( e + 176 ) * f32( e + 16 ) * 0.5f;
}

// Which way a click at x steps a tab scene: toward the nearer of its own arrows ('...Arrow Left...' and
// '...Arrow Right...' among its children, the dossier's), else by the side of its art it is on. With
// arrows, a click between them is on the page shown (the dossier's middle card): *middle, no step.
bool tab_arrow_right( uint32_t tabs, float x, bool *middle = nullptr )
{
    if ( middle ) *middle = false;
    float left = 0, right = 0;
    bool have_left = false, have_right = false;
    for ( uint32_t c = guest_read32( tabs + 24 ), guard = 0; c && guard < 64; c = guest_read32( c + 32 ), ++guard )
    {
        const std::string n = wide_string( guest_read32( c + 4 ) );
        const float cx = f32( c + 208 ) + f32( c + 160 ) * f32( c + 12 ) * 0.5f + f32( c + 176 ) * f32( c + 16 ) * 0.5f;
        if ( n.find( "Arrow Left" ) != std::string::npos ) left = cx, have_left = true;
        if ( n.find( "Arrow Right" ) != std::string::npos ) right = cx, have_right = true;
    }
    if ( have_left && have_right )
    {
        const float lo = std::min( left, right ), hi = std::max( left, right ), reach = 24.0f;  // an arrow's own width, about
        if ( middle && x > lo + reach && x < hi - reach ) *middle = true;
        return std::fabs( x - right ) < std::fabs( x - left );
    }
    return x >= centre_x( tabs );
}

float centre_distance( uint32_t e, float x, float y )
{
    uint32_t art = 0;
    float area = 0;
    largest_art( e, art, area );
    if ( art ) e = art;
    float cx = f32( e + 208 ) + f32( e + 160 ) * f32( e + 12 ) * 0.5f + f32( e + 176 ) * f32( e + 16 ) * 0.5f;
    float cy = f32( e + 212 ) + f32( e + 164 ) * f32( e + 12 ) * 0.5f + f32( e + 180 ) * f32( e + 16 ) * 0.5f;
    return ( cx - x ) * ( cx - x ) + ( cy - y ) * ( cy - y );
}

bool contains( uint32_t outer, uint32_t e );
// Whether el is on another page of a tab scene than the one the focus is on (the gang dossier's side
// boxes are its other pages, their Replay Video buttons among them): hovering there doesn't take the
// focus, which would stop the page's video. A click still turns the page.
bool on_hidden_tab( uint32_t el, uint32_t fe )
{
    for ( uint32_t page = el, g = 0; page && g < 32; page = guest_read32( page + 40 ), ++g )
    {
        const uint32_t parent = guest_read32( page + 40 );
        if ( parent && xui_class_data( xui_object( guest_read32( parent ) ), guest_read32( TAB_SCENE_CLASS ) ) )
            return contains( parent, fe ) && !contains( page, fe );
    }
    return false;
}

bool contains( uint32_t outer, uint32_t e )
{
    for ( int g = 0; e && g < 32; e = guest_read32( e + 40 ), ++g )
        if ( e == outer ) return true;
    return false;
}

// Controls the pad can't land on: greyed out ones (Downloads, Xbox Live Settings) have bit 0 of the
// control's flags clear; bit 1 marks ones navigation passes over (the back button, the slider rows'
// background buttons).
bool enabled( uint32_t h )
{
    uint32_t ctl = xui_class_data( h, guest_read32( CONTROL_CLASS ) );
    return !ctl || ( guest_read32( ctl + 16 ) & 3 ) == 1;
}

// The first focusable control under element e, depth first.
uint32_t first_focusable( uint32_t e, int depth = 0 )
{
    for ( uint32_t c = guest_read32( e + 24 ), guard = 0; c && guard < 256 && depth < 24; c = guest_read32( c + 32 ), ++guard )
    {
        uint32_t h = xui_object( guest_read32( c ) );
        if ( focusable( h ) ) return c;
        if ( uint32_t inner = first_focusable( c, depth + 1 ) ) return inner;
    }
    return 0;
}

// The focusable control right before `goal` coming from direction `dir` (d-pad bits: 1 up, 2 down, 4 left,
// 8 right): the nearest one on the side the step comes from, roughly in line with it. 0 if none.
void collect_focusable( uint32_t e, std::vector<uint32_t> &out, int depth = 0 )
{
    for ( uint32_t c = guest_read32( e + 24 ), guard = 0; c && guard < 256 && depth < 24; c = guest_read32( c + 32 ), ++guard )
    {
        if ( !( guest_read32( c + 236 ) & 1 ) ) continue;  // hidden
        if ( focusable( xui_object( guest_read32( c ) ) ) ) out.push_back( c );
        else collect_focusable( c, out, depth + 1 );
    }
}
uint32_t control_before( uint32_t scene, uint32_t goal, uint16_t dir )
{
    std::vector<uint32_t> all;
    collect_focusable( scene, all );
    float gx, gy;
    absolute_pos( goal, gx, gy );
    uint32_t best = 0;
    float best_d = 1e9f;
    for ( uint32_t c : all )
    {
        if ( c == goal ) continue;
        float x, y;
        absolute_pos( c, x, y );
        float along = dir == 0x2 ? gy - y : dir == 0x1 ? y - gy : dir == 0x8 ? gx - x : x - gx;  // > 0: on the side the step comes from
        float across = dir <= 0x2 ? std::fabs( x - gx ) : std::fabs( y - gy );
        if ( along <= 0 || across > 64 ) continue;
        if ( along < best_d ) best_d = along, best = c;
    }
    return best;
}

// For a tab header (a button directly in a tab scene: the main menu's Options / Main Menu / Agency Info),
// the page it stands for: the k-th child scene holding controls for the k-th such button, with k in *index.
// 0 otherwise.
uint32_t tab_page( uint32_t el, int *index )
{
    uint32_t tabs = el ? guest_read32( el + 40 ) : 0;
    if ( !tabs || !xui_class_data( xui_object( guest_read32( tabs ) ), guest_read32( TAB_SCENE_CLASS ) ) ) return 0;
    int k = -1, n = 0;
    for ( uint32_t c = guest_read32( tabs + 24 ), guard = 0; c && guard < 256; c = guest_read32( c + 32 ), ++guard )
    {
        uint32_t h = xui_object( guest_read32( c ) );
        if ( c == el ) k = n;
        if ( focusable( h ) ) ++n;
    }
    *index = k;
    for ( uint32_t c = guest_read32( tabs + 24 ), guard = 0; c && guard < 256 && k >= 0; c = guest_read32( c + 32 ), ++guard )
        if ( xui_class_data( xui_object( guest_read32( c ) ), guest_read32( SCENE_CLASS ) ) && first_focusable( c ) && k-- == 0 ) return c;
    return 0;
}

// The focusable control at (x, y) under element e, by the engine's hit test; its element goes to *el. A
// layout box is often bigger than its art (the main menu's middle tab header reaches over Agency Info), so
// of several controls containing the point the one whose box centre is nearest wins. Scenes that don't
// hold the focus element fe are skipped: tab pages not shown, and a scene closing after Back that still
// sits on top while it fades.
struct Hit { uint32_t h = 0, el = 0; float d = 1e30f; };
// An options row: a scene with a radio group directly in it ('options'), part of the page around it.
bool options_row( uint32_t e )
{
    for ( uint32_t c = guest_read32( e + 24 ), guard = 0; c && guard < 64; c = guest_read32( c + 32 ), ++guard )
        if ( wide_string( guest_read32( c + 4 ) ) == "options" ) return true;
    return false;
}
void hit_test( PPCContext &ctx, uint32_t e, float x, float y, uint32_t fe, Hit &best, int depth = 0 )
{
    for ( uint32_t c = guest_read32( e + 24 ), guard = 0; c && guard < 256 && depth < 24; c = guest_read32( c + 32 ), ++guard )
    {
        if ( f32( c + 20 ) <= 0.01f || !( guest_read32( c + 236 ) & 1 ) ) continue;  // transparent or hidden, with everything inside
        uint32_t h = xui_object( guest_read32( c ) );
        bool is_tabs = xui_class_data( h, guest_read32( TAB_SCENE_CLASS ) );
        if ( xui_class_data( h, guest_read32( SCENE_CLASS ) ) && !is_tabs && !contains( c, fe ) && !options_row( c ) ) continue;
        if ( focusable( h ) && !is_tabs )  // a control's own parts are not separate targets
        {
            float d = centre_distance( c, x, y );
            // On its art, where it has any: the layout box reaches further, and the highlight came too early.
            // Two controls in the same box (the gang hierarchy's 'XuiButton7' and 'XuiButton7Dead', the latter
            // only the DECEASED stamp, never focused by the game): the first, as the game's own navigation has it.
            const bool stacked = best.el && std::fabs( f32( best.el + 208 ) - f32( c + 208 ) ) < 1.0f && std::fabs( f32( best.el + 212 ) - f32( c + 212 ) ) < 1.0f &&
                                 std::fabs( f32( best.el + 160 ) - f32( c + 160 ) ) < 1.0f && std::fabs( f32( best.el + 164 ) - f32( c + 164 ) ) < 1.0f;
            if ( d < best.d && !stacked && enabled( h ) && engine_hit( ctx, h, x, y ) && ( !guest_read32( c + 24 ) || art_hit( ctx, c, x, y ) ) ) best = { h, c, d };
            continue;
        }
        Hit before = best;
        hit_test( ctx, c, x, y, fe, best, depth + 1 );
        // A tab scene whose pages hold nothing to point at (the campaign's Difficulty) is a target itself.
        // Its arrows are thin and sit at its ends: a point up to one control height beyond either end counts.
        float pad = f32( c + 16 ) * f32( c + 180 ), mid = f32( c + 208 ) + f32( c + 160 ) * f32( c + 12 ) * 0.5f;
        if ( best.h == before.h && is_tabs && enabled( h ) &&
             ( engine_hit( ctx, h, x, y ) || art_hit( ctx, c, x, y ) || engine_hit( ctx, h, x < mid ? x + pad : x - pad, y ) ) )
        {
            float d = centre_distance( c, x, y );
            if ( d < best.d ) best = { h, c, d };
        }
    }
}
}

void restart_game();
// Changes on the page wait: Apply takes them on, Confirm takes them on and leaves, Cancel or Esc leaves
// without them, Restore Defaults puts the defaults on the page (to apply or not).
// The Recomp Options page (xui_options.cpp) and the settings (settings.h), kept in step: when the page
// comes up its rows show the settings (a radio button's checked state: XM_SET_CHECKED 2001, and its
// group's choice; the slider
// 1..16, quarters of the sensitivity, through its set-value), and what the player changes is taken on.
// A radio group's class data +4 holds its checked button (an object handle; 0 none).
constexpr uint32_t XM_SET_CHECKED = 2001, RADIO_GROUP_CLASS = 0x835D2324;

void options_page( PPCContext &c )
{
    static uint32_t shown;     // the page (its element) the rows were set for
    static Settings applied;   // what the page shows / last took on
    uint32_t page = 0;
    for ( uint32_t e = xui_element( xui_focus( 0 ) ? xui_focus( 0 ) : xui_focus( -1 ) ), g = 0; e && g < 16; e = guest_read32( e + 40 ), ++g )
        if ( wide_string( guest_read32( e + 4 ) ) == "RecompOptions" ) page = e;
    static bool keep;          // coming up again with what the page holds, not the settings
    if ( !page ) { shown = 0; g_option_pressed = OPT_NONE; return; }
    struct Switch { const char *row, *on, *off; bool Settings::*value; };
    static const Switch switches[] = {
                                       { "vsync", "On", "Off", &Settings::vsync },
                                       { "fps60", "On", "Off", &Settings::fps60 },                 { "showFps", "On", "Off", &Settings::show_fps } };
    // The rows of three buttons: which button a setting shows, and what choosing one sets.
    static const char *const three_ids[] = { "On", "Off", "Third" };
    struct Three { const char *row; int ( *get )( const Settings & ); void ( *set )( Settings &, int ); };
    static const Three threes[] = {
        { "fullscreen", []( const Settings &s ) { return !s.fullscreen ? 0 : s.exclusive ? 2 : 1; },
          []( Settings &s, int i ) { s.fullscreen = i != 0, s.exclusive = i == 2; } },
        { "skipStart", []( const Settings &s ) { return s.skip_title ? 2 : s.skip_intro ? 1 : 0; },
          []( Settings &s, int i ) { s.skip_intro = i >= 1, s.skip_title = i == 2; } } };
    // Sliders of named choices, their label the choice (after "<name>: " where there is a name): Upscaling
    // (Off, FSR 1, then DLSS's modes, then FSR 3's) and DLSS's model.
    struct Choices { const char *row, *name; int last; const char *const *names; int ( *get )( const Settings & ); void ( *set )( Settings &, int ); };
    static const char *const upscalings[] = { "Upscaling Off", "FSR 1", "DLAA", "DLSS Quality", "DLSS Balanced", "DLSS Performance", "DLSS Ultra Perf.",
                                              "FSR 3 Native AA", "FSR 3 Quality", "FSR 3 Balanced", "FSR 3 Performance", "FSR 3 Ultra Perf." };
    static const char *const models[] = { "Default", "Preset K", "Preset L", "Preset M", "Preset J" };
    static const char *const aas[] = { "Off", "FXAA", "MSAA 2x", "MSAA 4x", "MSAA 8x" };
    static const char *const anisos[] = { "Game", "2x", "4x", "8x", "16x" };
    static const char *const lods[] = { "Game", "Far", "Farther", "Farthest", "Maximum" };
    static const Choices choices[] = {
        { "antiAliasing", "Anti-Aliasing", 4, aas,
          []( const Settings &s ) { return s.msaa >= 8 ? 4 : s.msaa >= 4 ? 3 : s.msaa >= 2 ? 2 : s.fxaa ? 1 : 0; },
          []( Settings &s, int i ) { s.fxaa = i == 1, s.msaa = i >= 2 ? 1 << ( i - 1 ) : 0, s.anti_aliasing = false; } },
        { "anisotropy", "Anisotropic", 4, anisos,
          []( const Settings &s ) { return s.anisotropy >= 16 ? 4 : s.anisotropy >= 8 ? 3 : s.anisotropy >= 4 ? 2 : s.anisotropy >= 2 ? 1 : 0; },
          []( Settings &s, int i ) { s.anisotropy = i ? 1 << i : 0; } },
        { "lod", "Draw Distance", 4, lods, []( const Settings &s ) { return s.lod; }, []( Settings &s, int i ) { s.lod = i; } },
        { "upscaling", nullptr, 11, upscalings, []( const Settings &s ) { return s.dlss ? 2 + s.dlss_mode : s.fsr3 ? 7 + s.dlss_mode : s.fsr ? 1 : 0; },
          []( Settings &s, int i ) { s.fsr = i == 1, s.dlss = i >= 2 && i <= 6, s.fsr3 = i >= 7; if ( i >= 2 ) s.dlss_mode = ( i - 2 ) % 5; } },
        { "dlssModel", "DLSS Model", 4, models, []( const Settings &s ) { return s.dlss_preset; }, []( Settings &s, int i ) { s.dlss_preset = i; } } };
    constexpr size_t CHOICES = sizeof( choices ) / sizeof( choices[0] );
    static int choice_label[CHOICES] = { -1, -1, -1, -1, -1 };
    auto show_choice = [&]( size_t n, int v ) {
        uint32_t bar = find_named( page, ( std::string( choices[n].row ) + "Bar" ).c_str() );
        if ( !bar || v == choice_label[n] ) return;
        choice_label[n] = v;
        char text[64];
        if ( choices[n].name ) snprintf( text, sizeof text, "%s: %s", choices[n].name, choices[n].names[v] );
        else snprintf( text, sizeof text, "%s", choices[n].names[v] );
        const uint32_t at = ( c.r1.u32 - 512 ) & ~15u;
        for ( size_t i = 0; i <= strlen( text ); ++i ) guest_write16( at + 2 * i, (uint8_t)text[i] );
        guest_call( c, 0x827232D8, bar, at );
    };
    // DLSS Model only means something with DLSS (DLAA or a DLSS mode): greyed out otherwise, through XUI's
    // own set-enabled as the Enabled property's setter (82776650) does it: 82762DB8 finds the handle's
    // object, 82723108 (object, enabled) sends it the message. On the slider and its row's bar.
    static int model_enabled = -1;
    auto gate_model = [&]( bool on ) {
        if ( (int)on == model_enabled ) return;
        model_enabled = on;
        for ( const char *name : { "dlssModel", "dlssModelBar" } )
            if ( uint32_t h = find_named( page, name ) )
                if ( uint32_t o = guest_call( c, 0x82762DB8, h, 0 ) ) guest_call( c, 0x82723108, o, on ? 1u : 0u );
    };
    auto choice_data = [&]( size_t n ) { uint32_t h = find_named( page, choices[n].row ); return h ? xui_class_data( h, guest_read32( SLIDER_CLASS ) ) : 0u; };
    // Resolution: a slider, Match Display (0) then the sixteen-by-nine sizes 1280x720 to 3840x2160 in
    // steps of 160x90 (1 to 17), its row's label showing the choice (XUI's set-text, 827232D8 (handle, text)).
    uint32_t res_h = find_named( page, "resolution" ), res = res_h ? xui_class_data( res_h, guest_read32( SLIDER_CLASS ) ) : 0;
    static int res_label = -1;
    auto show_resolution = [&]( int height ) {
        uint32_t bar = find_named( page, "resolutionBar" );
        if ( !bar || height == res_label ) return;
        res_label = height;
        char text[48];
        if ( height ) snprintf( text, sizeof text, "Resolution %dx%d", height * 16 / 9, height );
        else snprintf( text, sizeof text, "Match Display" );
        const uint32_t at = ( c.r1.u32 - 512 ) & ~15u;
        for ( size_t i = 0; i <= strlen( text ); ++i ) guest_write16( at + 2 * i, (uint8_t)text[i] );
        guest_call( c, 0x827232D8, bar, at );
    };
    // Field of View: a slider, 60 to 120 degrees in steps of 5 (0 to 12), its label showing the value.
    uint32_t fov_h = find_named( page, "fov" ), fov = fov_h ? xui_class_data( fov_h, guest_read32( SLIDER_CLASS ) ) : 0;
    static int fov_label = -1;
    auto show_fov = [&]( int degrees ) {
        uint32_t bar = find_named( page, "fovBar" );
        if ( !bar || degrees == fov_label ) return;
        fov_label = degrees;
        char text[48];
        snprintf( text, sizeof text, "Field of View %d", degrees );
        const uint32_t at = ( c.r1.u32 - 512 ) & ~15u;
        for ( size_t i = 0; i <= strlen( text ); ++i ) guest_write16( at + 2 * i, (uint8_t)text[i] );
        guest_call( c, 0x827232D8, bar, at );
    };
    if ( shown != page )
    {
        shown = page;
        if ( !keep ) applied = settings();
        keep = false;
        for ( auto &s : switches )
            if ( uint32_t row = find_named( page, s.row ) )
            {
                uint32_t re = xui_element( row ), on = find_named( re, s.on ), off = find_named( re, s.off ), group = find_named( re, "options" );
                uint32_t gd = group ? xui_class_data( group, guest_read32( RADIO_GROUP_CLASS ) ) : 0;
                if ( !on || !off || !gd ) continue;
                send_int_message( c, on, XM_SET_CHECKED, applied.*s.value );
                send_int_message( c, off, XM_SET_CHECKED, !( applied.*s.value ) );
                guest_write32( gd + 4, applied.*s.value ? on : off );
            }
        for ( auto &t : threes )
            if ( uint32_t row = find_named( page, t.row ) )
            {
                uint32_t re = xui_element( row ), group = find_named( re, "options" ), gd = group ? xui_class_data( group, guest_read32( RADIO_GROUP_CLASS ) ) : 0;
                for ( int i = 0; gd && i < 3; ++i )
                    if ( uint32_t b = find_named( re, three_ids[i] ) )
                    {
                        send_int_message( c, b, XM_SET_CHECKED, t.get( applied ) == i );
                        if ( t.get( applied ) == i ) guest_write32( gd + 4, b );
                    }
            }
        if ( res )
        {
            guest_write32( res + 16, 0 ), guest_write32( res + 20, 17 ), guest_write32( res + 28, 1 );
            guest_call( c, SLIDER_SET_VALUE, res, applied.resolution ? (uint32_t)( ( applied.resolution - 720 ) / 90 + 1 ) : 0u );
        }
        if ( fov )
        {
            guest_write32( fov + 16, 0 ), guest_write32( fov + 20, 12 ), guest_write32( fov + 28, 1 );
            guest_call( c, SLIDER_SET_VALUE, fov, (uint32_t)( ( applied.fov - 60 ) / 5 ) );
        }
        res_label = -1;
        show_resolution( applied.resolution );
        fov_label = -1;
        show_fov( applied.fov );
        // DLSS and FSR 3 are the anti-aliasing (MSAA and FXAA are off under them): shown as such.
        if ( applied.dlss || applied.fsr3 ) applied.msaa = 0, applied.fxaa = false, applied.anti_aliasing = false;
        for ( size_t n = 0; n < CHOICES; ++n )
        {
            if ( uint32_t d = choice_data( n ) )
            {
                guest_write32( d + 16, 0 ), guest_write32( d + 20, (uint32_t)choices[n].last ), guest_write32( d + 28, 1 );
                guest_call( c, SLIDER_SET_VALUE, d, (uint32_t)choices[n].get( applied ) );
            }
            choice_label[n] = -1;
            show_choice( n, choices[n].get( applied ) );
        }
        model_enabled = -1;
        gate_model( applied.dlss );
        return;
    }
    Settings now = applied;
    for ( auto &s : switches )
        if ( uint32_t row = find_named( page, s.row ) )
        {
            uint32_t re = xui_element( row ), group = find_named( re, "options" ), gd = group ? xui_class_data( group, guest_read32( RADIO_GROUP_CLASS ) ) : 0;
            uint32_t selected = gd ? guest_read32( gd + 4 ) : 0;
            if ( selected && selected == find_named( re, s.on ) ) now.*s.value = true;
            else if ( selected && selected == find_named( re, s.off ) ) now.*s.value = false;
        }
    for ( auto &t : threes )
        if ( uint32_t row = find_named( page, t.row ) )
        {
            uint32_t re = xui_element( row ), group = find_named( re, "options" ), gd = group ? xui_class_data( group, guest_read32( RADIO_GROUP_CLASS ) ) : 0;
            uint32_t selected = gd ? guest_read32( gd + 4 ) : 0;
            for ( int i = 0; selected && i < 3; ++i )
                if ( selected == find_named( re, three_ids[i] ) ) t.set( now, i );
        }
    if ( res )
        if ( int32_t v = std::clamp( (int32_t)guest_read32( res + 24 ), 0, 17 ); true ) now.resolution = v ? 720 + 90 * ( v - 1 ) : 0;
    show_resolution( now.resolution );
    if ( fov ) now.fov = 60 + 5 * std::clamp( (int32_t)guest_read32( fov + 24 ), 0, 12 );
    show_fov( now.fov );
    for ( size_t n = 0; n < CHOICES; ++n )
    {
        if ( uint32_t d = choice_data( n ) ) choices[n].set( now, std::clamp( (int32_t)guest_read32( d + 24 ), 0, choices[n].last ) );
        show_choice( n, choices[n].get( now ) );
    }
    // Anti-aliasing and the temporal upscalers (DLSS, FSR 3, which anti-alias themselves) exclude each other:
    // the one just moved wins and the other slider is put back to match (Anti-Aliasing Off, or Upscaling
    // Off / FSR 1 as it was).
    if ( ( now.dlss || now.fsr3 ) && ( now.msaa > 0 || now.fxaa ) )
    {
        const bool upscaler_moved = now.dlss != applied.dlss || now.fsr3 != applied.fsr3 || now.dlss_mode != applied.dlss_mode;
        size_t aa_row = 0, up_row = 0;
        for ( size_t n = 0; n < CHOICES; ++n )
        {
            if ( !strcmp( choices[n].row, "antiAliasing" ) ) aa_row = n;
            if ( !strcmp( choices[n].row, "upscaling" ) ) up_row = n;
        }
        if ( upscaler_moved ) now.msaa = 0, now.fxaa = false, now.anti_aliasing = false;
        else now.dlss = now.fsr3 = false;
        const size_t other = upscaler_moved ? aa_row : up_row;
        if ( uint32_t d = choice_data( other ) ) guest_call( c, SLIDER_SET_VALUE, d, (uint32_t)choices[other].get( now ) );
        show_choice( other, choices[other].get( now ) );
    }
    gate_model( now.dlss );
    bool changed = now.resolution != applied.resolution || now.fov != applied.fov || now.fullscreen != applied.fullscreen || now.exclusive != applied.exclusive || now.anti_aliasing != applied.anti_aliasing || now.fxaa != applied.fxaa || now.fsr != applied.fsr || now.dlss != applied.dlss || now.fsr3 != applied.fsr3 || now.dlss_mode != applied.dlss_mode || now.dlss_preset != applied.dlss_preset ||
                   now.msaa != applied.msaa || now.anisotropy != applied.anisotropy || now.lod != applied.lod || now.skip_intro != applied.skip_intro || now.skip_title != applied.skip_title;
    for ( auto &s : switches ) changed |= now.*s.value != applied.*s.value;
    if ( changed ) applied = now;
    const OptionButton pressed = g_option_pressed;
    g_option_pressed = OPT_NONE;
    if ( pressed == OPT_APPLY || pressed == OPT_CONFIRM )
    {
        const Settings before = settings();
        applied.mouse_sensitivity = before.mouse_sensitivity, applied.invert_y = before.invert_y;  // the Keyboard Controls page's
        settings_change( applied );
        if ( before.resolution != applied.resolution || before.anti_aliasing != applied.anti_aliasing || before.fxaa != applied.fxaa || before.fsr != applied.fsr || before.dlss != applied.dlss || before.fsr3 != applied.fsr3 || ( before.msaa != applied.msaa && !applied.dlss && !applied.fsr3 ) || ( ( applied.dlss || applied.fsr3 ) && before.dlss_mode != applied.dlss_mode ) )
            restart_game();
    }
    // Leaving: the page's own back button pressed (XUI's press), as Esc or B would.
    if ( pressed == OPT_CONFIRM || pressed == OPT_CANCEL )
    {
        if ( uint32_t back = back_button( page ) ) press( c, back, xui_focus( 0 ) ? 0 : 255 );
        else rt_log( "xui options: no back button on the page\n" );
    }
    if ( pressed == OPT_RESTORE )
    {
        const bool native = applied.native_d3d;
        applied = Settings{};
        applied.native_d3d = native;
        applied.mouse_sensitivity = settings().mouse_sensitivity, applied.invert_y = settings().invert_y;  // not this page's
        keep = true;
        shown = 0;  // the rows show them at the next look
    }
}

// Mouse in menus: hovering a control focuses it (through XUI, so the game's own highlight and sounds
// follow); a left click on the focused control presses it for 100 ms. Esc leaves a menu, as a key the game reads itself.
// The menu sounds the mouse plays itself: the game's own (its Generic bank, in memory), mixed over its
// output (audio.cpp) at the level the game plays them -- moving the focus and changing a tab (#450),
// choosing (#448). The mouse works the menus through XUI directly (focus, press, tab scenes), which is
// silent; the pad's d-pad / A made the game play these.
std::vector<float> sound_entry_pcm( const char *bank_name, size_t index );                  // soundbanks.cpp
void audio_play_overlay( std::shared_ptr<const std::vector<float>> pcm, float gain );      // audio.cpp
uint64_t audio_sample_count();                                                             // audio.cpp
enum UiSound { UI_MOVE, UI_SELECT };
void ui_sound( UiSound s )
{
    static std::shared_ptr<const std::vector<float>> pcm[2];
    static const float gain = getenv( "CRACKDOWN_UI_SOUND_GAIN" ) ? (float)atof( getenv( "CRACKDOWN_UI_SOUND_GAIN" ) ) : 0.2f;  // measured: the game mixes them at 0.2
    if ( !pcm[s] )
    {
        std::vector<float> v = sound_entry_pcm( "Generic", s == UI_MOVE ? 450 : 448 );
        if ( v.empty() ) return;
        pcm[s] = std::make_shared<const std::vector<float>>( std::move( v ) );
    }
    audio_play_overlay( pcm[s], gain );
    if ( static const bool vlog = getenv( "CRACKDOWN_VOICE_LOG" ); vlog ) rt_log( "voice: menu mouse sound %s sample %llu\n", s == UI_MOVE ? "move" : "select", (unsigned long long)audio_sample_count() );
}

// Without a controller connected a menu opens with nothing highlighted, as on a PC: the control the game
// focuses shows its normal look until the user reaches for it. The focus itself stays where the game put
// it, so its menu code runs as ever; only the look changes (82776438 below). The pointer on the control,
// or the first menu key, brings the highlight back; the user's own moves are highlighted as usual.
struct Look { uint32_t part, look, look_end, slider_data, slider_enabled; bool shown; };  // the highlight a control asked for (a slider: its arguments)
std::vector<Look> g_looks;
uint32_t g_pointer_el;   // the control under the pointer
bool g_keyboard_nav;     // the keyboard is being used: the focus shows, as with a controller

bool focus_hidden_here() { return !video_gamepad_connected() && !video_using_controller(); }

bool highlight_wanted( uint32_t part )
{
    if ( !focus_hidden_here() || g_keyboard_nav ) return true;
    const uint32_t obj = xui_object( part ), e = obj ? xui_element( obj ) : 0;
    if ( !e ) return false;
    // A control told to look focused without the focus is showing something else: the map's marker of the
    // selected row. (A list's rows: the list keeps the focus while its current row changes.)
    int index = 0;
    if ( !contains( e, xui_element( xui_focus( xui_focus( 0 ) ? 0 : -1 ) ) ) && !list_of( e, &index ) ) return true;
    // A dossier file's page: the look of its Replay button runs with the video, and there is nothing else to pick.
    for ( uint32_t s = guest_read32( e + 40 ), g = 0; s && g < 8; s = guest_read32( s + 40 ), ++g )
        if ( xui_class_data( xui_object( guest_read32( s ) ), guest_read32( SCENE_CLASS ) ) )
        {
            if ( find_named( s, "video" ) ) return true;
            break;
        }
    return g_pointer_el && ( contains( e, g_pointer_el ) || contains( g_pointer_el, e ) );
}

// Controls whose highlight should now show or go ask for their look again (82769D50 and 8277E578 below).
void refresh_looks( PPCContext &c )
{
    const std::vector<Look> looks = g_looks;
    for ( const Look &l : looks )
    {
        if ( !xui_object( l.part ) )
        {
            g_looks.erase( std::remove_if( g_looks.begin(), g_looks.end(), [&]( const Look &x ) { return x.part == l.part; } ), g_looks.end() );
            continue;
        }
        if ( l.shown == highlight_wanted( l.part ) ) continue;
        const uint64_t r6 = c.r6.u64, r7 = c.r7.u64;
        c.r6.u64 = c.r7.u64 = 0;
        if ( l.slider_data ) guest_call( c, 0x8277E578, l.slider_data, l.slider_enabled, 0 );
        else guest_call( c, 0x82769D50, l.part, l.look, l.look_end );
        c.r6.u64 = r6, c.r7.u64 = r7;
    }
}

void mouse_focus( PPCContext &c, uint32_t h, int user ) { guest_call( c, SET_FOCUS, h, user ); }
// The Achievements page's object while it exists (made by 826C0610, its vtable ACHIEVEMENTS_PAGE_VTABLE; the
// selected achievement at +596). 826C0DC0 / 826C0E10 (this): the previous / next one, the list redrawn.
uint32_t g_achievements_page;
constexpr uint32_t ACHIEVEMENTS_PAGE_VTABLE = 0x820F10F4;

// Exit to Desktop: at the main menu (the game's Quit greyed out: no game) the game closes at once. In a
// game it saves first, through the game's own Quit: Quit is pressed, its "Save and Quit" pop-up
// ('extractPopUp', also Extract's) answered Yes, and once the main menu is back with Quit greyed out
// (the game left, its save written) the game closes. A step that doesn't come within its time gives up.
void exit_to_desktop( PPCContext &c, uint32_t fe, int user )
{
    enum { IDLE, QUIT_PRESSED, YES_PRESSED };
    static int stage = IDLE;
    static std::chrono::steady_clock::time_point deadline;
    const auto now = std::chrono::steady_clock::now();
    auto scene_of = []( uint32_t e ) {
        for ( uint32_t g = 0; e && g < 32; e = guest_read32( e + 40 ), ++g )
            if ( xui_class_data( xui_object( guest_read32( e ) ), guest_read32( SCENE_CLASS ) ) && !xui_class_data( xui_object( guest_read32( e ) ), guest_read32( TAB_SCENE_CLASS ) ) )
                return e;
        return 0u;
    };
    if ( g_exit_requested )
    {
        g_exit_requested = false;
        uint32_t quit = 0;
        for ( uint32_t e = fe, g = 0; e && g < 32 && !quit; e = guest_read32( e + 40 ), ++g ) quit = find_named( e, "quit" );
        if ( !quit || !enabled( quit ) )
        {
            rt_log( "menu: exit to desktop\n" );
            _exit( 0 );
        }
        rt_log( "menu: exit to desktop: saving first (the game's Quit)\n" );
        press( c, quit, user );
        stage = QUIT_PRESSED, deadline = now + std::chrono::seconds( 5 );
        return;
    }
    if ( stage == IDLE ) return;
    if ( now >= deadline )
    {
        rt_log( "menu: exit to desktop: gave up (%s)\n", stage == QUIT_PRESSED ? "no Save and Quit pop-up" : "the main menu didn't come back" );
        stage = IDLE;
        return;
    }
    if ( stage == QUIT_PRESSED )
    {
        uint32_t s = scene_of( fe );
        if ( s && wide_string( guest_read32( s + 4 ) ) == "extractPopUp" )
            if ( uint32_t yes = find_named( s, "yesButton" ) )
            {
                rt_log( "menu: exit to desktop: Save and Quit -> Yes\n" );
                press( c, yes, user );
                stage = YES_PRESSED, deadline = now + std::chrono::seconds( 120 );
            }
        return;
    }
    // YES_PRESSED: the main menu with nothing to quit.
    uint32_t quit = 0;
    for ( uint32_t e = fe, g = 0; e && g < 32 && !quit; e = guest_read32( e + 40 ), ++g ) quit = find_named( e, "quit" );
    if ( quit && !enabled( quit ) )
    {
        rt_log( "menu: exit to desktop (saved)\n" );
        _exit( 0 );
    }
}

// XUI's navigation from the focused control, as its key handling does once a key is a direction
// (82775988): the navigate message (2022, made by 82783E80 (message, data, direction: 0 up, 1 down,
// 2 left, 3 right, from)) sent to the focused control and passed up to whoever answers (a list: its next
// item, scrolled to), then the answer focused (SET_FOCUS). The control's page sees the move as from the
// d-pad (Street Racing's rows load their choice). False if it was refused outright (a list steps itself
// as the message passes it, without always answering).
bool navigate( PPCContext &c, int dir, int user )
{
    const uint32_t from = xui_focus( user == 255 ? -1 : 0 );
    if ( !from ) return false;
    const uint32_t msg = ( c.r1.u32 - 768 ) & ~15u, data = msg + 64;
    for ( uint32_t i = 0; i < 80; i += 4 ) guest_write32( msg + i, 0 );
    guest_call( c, 0x82783E80, msg, data, (uint32_t)dir, from );
    guest_call( c, SEND_MESSAGE, from, msg );
    const uint32_t handled = guest_read32( msg + 8 ), refused = guest_read32( data + 12 ), to = guest_read32( data + 8 );
    if ( refused ) return false;
    if ( handled && to ) guest_call( c, SET_FOCUS, to, (uint32_t)user );
    return true;
}

// A list (spinner, side by side cards) moved items each way (positive: right): its row focused, then
// XUI's own left / right navigation from it; the move sound played here, as for the mouse's other moves.
void list_step( PPCContext &c, uint32_t list_el, int dir )
{
    const uint32_t handle = list_el ? guest_read32( list_el ) : 0, data = handle ? xui_class_data( xui_object( handle ), guest_read32( LIST_CLASS ) ) : 0;
    if ( !data || !dir ) return;
    const int user = xui_focus( 0 ) ? 0 : 255;
    const int32_t cur = guest_read32( data + 132 ), count = guest_read32( data + 136 );
    bool moved = false;
    const bool room = dir > 0 ? cur < count - 1 : cur > 0;  // (the move sound only where there is a next item)
    for ( int n = 0; n < std::abs( dir ) && n < 8; ++n ) moved |= navigate( c, dir > 0 ? 3 : 2, user );
    if ( moved && room ) ui_sound( UI_MOVE );
    if ( getenv( "CRACKDOWN_MOUSE_LOG" ) ) rt_log( "menu mouse: list %08X step %d (item %d of %d): %s\n", handle, dir, cur, count, moved ? "navigated" : "refused" );
}

// A dossier page's text (an XuiEdit) scrolled lines each way (positive: down): the edit's own line down /
// up (8277F138 / 8277EF90 (edit data), what its key handling calls for down / up: they set where it slides
// to and start its timer, 82780920 moving it there).
void text_scroll( PPCContext &c, uint32_t edit_el, int lines )
{
    const uint32_t data = xui_class_data( xui_object( guest_read32( edit_el ) ), guest_read32( 0x835D2348 ) );
    if ( !data || !lines ) return;
    // It starts sliding (toward the end) and is stopped at a line as its own stop does when the key is let go
    // (the next line boundary in the direction it moves); notches while it still slides the same way add a
    // line each to where it is going.
    const float was_to = f32( data + 160 ), was_speed = f32( data + 168 ), line = (float)guest_read32( data + 136 );
    if ( line <= 0 ) return;
    const bool same_way = lines > 0 ? was_speed > 0 : was_speed < 0;
    const float at = f32( data + 156 );
    guest_call( c, lines > 0 ? 0x8277F138 : 0x8277EF90, data, 0 );
    float to;
    if ( same_way ) to = was_to + line * lines;
    else if ( lines > 0 ) to = ( std::floor( at / line ) + lines ) * line;
    else to = ( std::ceil( at / line ) + lines ) * line;
    to = std::clamp( to, 0.0f, std::max( f32( data + 164 ), 0.0f ) );
    uint32_t bits;
    memcpy( &bits, &to, 4 );
    guest_write32( data + 160, bits );
    if ( getenv( "CRACKDOWN_MOUSE_LOG" ) )
        rt_log( "menu mouse: text '%s' scrolled %d: at %.1f moving to %.1f (furthest %.1f)\n", wide_string( guest_read32( edit_el + 4 ) ).c_str(), lines,
                f32( data + 156 ), f32( data + 160 ), f32( data + 164 ) );
}

// The page's text that scrolls: the visible XuiEdit under e with more than fits (the furthest above 0).
uint32_t scrolling_text( uint32_t e, int depth = 0 )
{
    for ( uint32_t c = guest_read32( e + 24 ), g = 0; c && g < 256 && depth < 16; c = guest_read32( c + 32 ), ++g )
    {
        if ( !( guest_read32( c + 236 ) & 1 ) ) continue;
        if ( uint32_t data = xui_class_data( xui_object( guest_read32( c ) ), guest_read32( 0x835D2348 ) ); data && f32( data + 164 ) > 0 ) return c;
        if ( uint32_t t = scrolling_text( c, depth + 1 ) ) return t;
    }
    return 0;
}

uint16_t xui_menu_mouse( PPCContext &c )
{
    options_page( c );
    static uint32_t last_moves, dragging;  // dragging: the slider object held by the left button
    // Hovering: the focus goes to the control under the pointer through one d-pad press (from the control
    // before it), so the game's own menu code moves it and plays its move sound once (setting the focus
    // directly is silent). If the press doesn't land there, the control is focused directly.
    // A control can refuse the focus: the map lists take it straight back from the hidden holder under their
    // Supply Points / Boss Locations bar. Such a control is left alone from then on (each pointer move would
    // pull the focus away again), and the move sound waits to see that the focus stayed.
    static uint32_t nav_goal, nav_from, hover_goal, hover_refused;
    static std::chrono::steady_clock::time_point hover_check;
    static uint16_t nav_dir;
    static std::chrono::steady_clock::time_point nav_until;
    // A clicked tab header: the tab scene (its data, current tab at +12) is stepped toward it the same way.
    static uint32_t tab_scene;
    static int tab_goal, tab_from;
    static uint16_t tab_dir;
    static std::chrono::steady_clock::time_point tab_until;
    static bool was_left;
    auto now = std::chrono::steady_clock::now();
    MenuMouse m = video_menu_mouse();
    if ( const char *at = getenv( "CRACKDOWN_MENU_TREE_AT" ) )  // debug: every XUI root's element tree then (any screen)
    {
        double input_script_clock();
        static bool done;
        if ( !done && input_script_clock() >= atof( at ) ) { done = true; uint32_t f = xui_focus( 0 ) ? xui_focus( 0 ) : xui_focus( -1 ); rt_log( "xui tree at %.1f s (focus %08X)\n", input_script_clock(), f ); xui_all_roots(); }
    }
    if ( video_using_controller() )  // the controller drives the menus: the pointer, and any walk it started, stay out
    {
        nav_goal = 0, tab_scene = 0;
        m.left = false, m.wheel = 0, m.moves = last_moves;
    }
    int user = xui_focus( 0 ) ? 0 : 255;
    uint32_t focus = xui_focus( user == 255 ? -1 : 0 ), fe = xui_element( focus );
    exit_to_desktop( c, fe, user );
    // A menu is up when the focus is on a control (gameplay keeps it on the "In Game" scene, the title
    // screen on its video scene: those keep the mouse captured for looking around).
    // target = the focused control: the outermost focusable element around the focus (the Difficulty tab
    // scene passes focus down to a part of its button), a tab scene or list only when nothing inside it has
    // focus (the main menu's tabs hold its buttons; a list keeps the focus itself while it scrolls).
    uint32_t target = 0, tabs = 0, scene = 0;
    for ( uint32_t e = fe, g = 0; e && g < 32; e = guest_read32( e + 40 ), ++g )
    {
        uint32_t h = xui_object( guest_read32( e ) );
        if ( !xui_class_data( h, guest_read32( ELEMENT_CLASS ) ) ) break;
        if ( xui_class_data( h, guest_read32( TAB_SCENE_CLASS ) ) || xui_class_data( h, guest_read32( LIST_CLASS ) ) ) tabs = tabs ? tabs : e;
        else if ( focusable( h ) ) target = e;
    }
    if ( !target ) target = tabs;
    if ( !target ) g_looks.clear(), g_pointer_el = 0, hover_refused = hover_goal = 0;
    if ( hover_goal && now >= hover_check )
    {
        const uint32_t goal = xui_element( hover_goal );
        if ( goal && contains( goal, fe ) ) ui_sound( UI_MOVE );
        else
        {
            hover_refused = hover_goal;
            if ( log ) rt_log( "menu mouse: %08X refused the focus\n", hover_goal );
        }
        hover_goal = 0;
    }
    // Targets are searched where the pad navigates: the scene holding the focused control (the jukebox
    // opens over the audio screen and the main menu, whose controls stay alive behind it), or for a tab
    // page the tab scene holding the pages and their headers.
    for ( uint32_t e = target ? guest_read32( target + 40 ) : 0, g = 0; e && g < 32 && !scene; e = guest_read32( e + 40 ), ++g )
        if ( xui_class_data( xui_object( guest_read32( e ) ), guest_read32( SCENE_CLASS ) ) ) scene = e;
    for ( uint32_t p; scene && ( p = guest_read32( scene + 40 ) ) && xui_class_data( xui_object( guest_read32( p ) ), guest_read32( TAB_SCENE_CLASS ) ); )
        scene = p;
    // An options row is a scene of its own holding a radio group: the page around it is what is navigated.
    if ( scene && options_row( scene ) )
        if ( uint32_t p = guest_read32( scene + 40 ); p && xui_class_data( xui_object( guest_read32( p ) ), guest_read32( SCENE_CLASS ) ) ) scene = p;
    // Screens without controls: gameplay ("In Game") captures the mouse on a click; others ("Press START",
    // loading, videos) keep the pointer. "Press START" asks which controller is the player's, and the game
    // reads the pads themselves for it (XUI's key-down message to the scene does nothing): the keyboard and
    // mouse are controller 1, so a click there is its Start.
    bool playing = fe && wide_string( guest_read32( fe + 4 ) ) == "In Game";
    video_set_menu_active( target != 0 || ( fe && !playing ), target != 0 );
    static const bool log = getenv( "CRACKDOWN_MOUSE_LOG" );
    if ( !target || !m.present )
    {
        if ( log && m.moves != last_moves ) rt_log( "menu mouse: %.0f,%.0f no menu (focus %08X '%s', window %d)\n", m.x, m.y, focus, fe ? wide_string( guest_read32( fe + 4 ) ).c_str() : "", m.present );
        last_moves = m.moves;
        uint32_t video_menu_keys();
        static bool was_enter;
        const bool enter = video_menu_keys() & ( 1u << 4 | 1u << 7 );  // Return, Space
        const bool pressed = ( m.present && m.left && !was_left ) || ( enter && !was_enter && fe && wide_string( guest_read32( fe + 4 ) ) == "initialScreen" );
        was_enter = enter;
        if ( fe && !playing && pressed )
        {
            if ( log ) rt_log( "menu mouse: click on '%s': Start\n", wide_string( guest_read32( fe + 4 ) ).c_str() );
            void title_start();  // xam.cpp: the title's own START handling, for controller 0
            title_start();
        }
        was_left = m.left;
        return 0;
    }
    // Our entries' help lines (Exit to Desktop, Keyboard Controls), once per menu (its handle changes when the
    // menu is rebuilt).
    if ( scene && g_menu_help )
    {
        static const struct { const char *id, *text; } helps[] = { { "exitDesktop", "Close the game." },
                                                                  { "keyboardControls", "Change the keyboard and mouse controls." } };
        static uint32_t help_for[2], help_in[2];
        for ( int i = 0; i < 2; ++i )
            if ( uint32_t q = find_named( scene, helps[i].id ); q && ( q != help_for[i] || g_menu_help != help_in[i] ) )
            {
                const char *text = helps[i].text;
                const uint32_t text_at = ( c.r1.u32 - 512 ) & ~15u, key = text_at + 128, string = key + 16;
                for ( size_t j = 0; j <= strlen( text ); ++j ) guest_write16( text_at + 2 * j, (uint8_t)text[j] );
                guest_write32( key, q );
                guest_call( c, STRING_MAKE, string, text_at );
                guest_call( c, HELP_ADD_TEXT, g_menu_help, key, string );
                guest_call( c, STRING_FREE, string, 1, 0 );
                help_for[i] = q;
                help_in[i] = g_menu_help;
            }
    }
    // The pad or keyboard pressing Exit to Desktop (A / Enter on it) goes to its handler too.
    static bool was_a;
    uint32_t video_menu_keys();
    bool a = ( video_pad_state().buttons & 0x1000 ) || ( video_menu_keys() & ( 1u << 4 ) );
    if ( a && !was_a && ( is_exit( fe ) || is_keys( fe ) || option_button( fe ) ) ) press( c, focus, user );
    was_a = a;
    Hit best;
    if ( scene ) hit_test( c, scene, m.x, m.y, fe, best );
    // The search starts inside a tab scene when the focus is on a page's own holder (the map's Supply
    // Points / Boss Locations bar: a hidden button on each page); nothing hit there, the scene around it
    // is searched too, where the tab scene is a target itself (its arrows at its ends).
    if ( scene && !best.h && xui_class_data( xui_object( guest_read32( scene ) ), guest_read32( TAB_SCENE_CLASS ) ) )
        if ( uint32_t around = guest_read32( scene + 40 ) ) hit_test( c, around, m.x, m.y, fe, best );
    uint32_t hit_el = best.el, focused = xui_object( guest_read32( target ) ), hit = best.h;
    // A tab header: clicking it goes to its page through the tab scene, with the transition the pad's
    // left / right plays; hovering it leaves the focus alone.
    int tab = -1;
    uint32_t page = tab_page( hit_el, &tab );
    if ( page )
    {
        if ( m.left && !was_left && !contains( page, fe ) )
        {
            // Stepped to with d-pad left / right below (the game's tab sound); TAB_SCENE_GOTO if that stalls.
            tab_scene = xui_class_data( xui_object( guest_read32( guest_read32( hit_el + 40 ) ) ), guest_read32( TAB_SCENE_CLASS ) );
            tab_goal = tab, tab_dir = 0, nav_goal = 0;
            if ( log ) rt_log( "menu mouse: tab header %08X -> tab %d '%s'\n", hit, tab, wide_string( guest_read32( page + 4 ) ).c_str() );
        }
        hit = 0;
    }
    if ( log && m.moves != last_moves )
        rt_log( "menu mouse: %.0f,%.0f moves %u left %d focus %08X target %08X hit %08X\n", m.x, m.y, m.moves, m.left, focus, focused, hit );
    // Without a controller the highlight follows the pointer (or the keyboard, once used).
    if ( m.moves != last_moves ) g_keyboard_nav = false;
    // A tab scene counts only as a control of its own (Difficulty), not for the empty space around a page's buttons.
    g_pointer_el = hit && ( !xui_class_data( hit, guest_read32( TAB_SCENE_CLASS ) ) || !first_focusable( hit_el ) ) ? hit_el : 0;
    refresh_looks( c );
    if ( m.moves != last_moves )
    {
        last_moves = m.moves;
        int index = 0;
        bool across = false;
        uint32_t list = hit && hit != focused ? list_of( hit_el, &index, &across ) : 0;
        uint32_t spin = list ? spinner_of( hit_el ) : 0;
        if ( spin )  // a spinner's row: moved to as any control (its item is the spinner's choice, not the hovered one)
        {
            if ( !contains( spin, fe ) && hit != nav_goal ) nav_goal = hit;
        }
        else if ( list && across )
        {
            // A carousel (agent skins, the supply point's weapon rows) only turns on a click: hovering would spin
            // it. Hovering one that hasn't the focus focuses it, as the pad's up / down would: it lights up, and the
            // supply point's title names its current weapon.
            uint32_t own = xui_object( guest_read32( list ) );
            if ( !contains( xui_element( own ), fe ) && own != nav_goal && own != hover_refused && own != hover_goal && !spinner_of( fe ) )
                nav_goal = own;
        }
        else if ( list )
        {
            if ( log ) rt_log( "menu mouse: list item %d (top %d, current %d, count %d)\n", index, (int32_t)guest_read32( list + 128 ),
                               (int32_t)guest_read32( list + 132 ), (int32_t)guest_read32( list + 136 ) );
            int32_t was = guest_read32( list + 132 );
            send_int_message( c, guest_read32( list ), 2008, index );
            if ( (int32_t)guest_read32( list + 132 ) != was ) ui_sound( UI_MOVE );
        }
        else if ( xui_class_data( hit, guest_read32( RADIO_CLASS ) ) ) {}  // a radio button takes the choice with the focus: only a click may do that
        else if ( hit && hit != focused && hit != nav_goal && hit != hover_refused && hit != hover_goal &&  // moved to below, with the game's sound; not a tab scene
                  !( xui_class_data( hit, guest_read32( TAB_SCENE_CLASS ) ) && contains( hit_el, fe ) ) &&  // holding the focus (its d-pad steps tabs)
                  !on_hidden_tab( hit_el, fe ) &&
                  // nor a text panel (an XuiEdit: the dossier's location and activities text): it would take the
                  // focus from the page's video, which stops it. A click focuses one; the wheel scrolls it as it is.
                  !xui_class_data( hit, guest_read32( 0x835D2348 ) ) )
            nav_goal = hit;
    }
    // The Achievements page (agencyAchievementProfile) moves its own selection: a notch is the page's own
    // previous / next (as its up / down actions), on the page object (g_achievements_page).
    if ( m.wheel && g_achievements_page && guest_read32( g_achievements_page ) == ACHIEVEMENTS_PAGE_VTABLE )
        for ( uint32_t e = fe, g = 0; e && g < 32; e = guest_read32( e + 40 ), ++g )
            if ( wide_string( guest_read32( e + 4 ) ) == "agencyAchievementProfile" )
            {
                for ( int n = 0; n < std::abs( m.wheel ) && n < 8; ++n ) guest_call( c, m.wheel > 0 ? 0x826C0DC0u : 0x826C0E10u, g_achievements_page, 0 );
                if ( log ) rt_log( "menu mouse: wheel %d, achievement %u\n", m.wheel, guest_read32( g_achievements_page + 596 ) );
                m.wheel = 0;
                break;
            }
    // The wheel scrolls the focused list a row per notch; then the row now under the pointer becomes the
    // current item, as on hovering.
    if ( m.wheel )
    {
        int index = 0;
        uint32_t own = xui_class_data( xui_object( guest_read32( target ) ), guest_read32( LIST_CLASS ) );
        bool across = false;
        uint32_t list = own ? own : list_of( target, &index, &across );
        if ( uint32_t spin = spinner_of( target ) ) list_step( c, spin, -m.wheel );  // up: the previous choice, as the left arrow
        else if ( list && across )
        {
            int32_t cur = guest_read32( list + 132 ), count = guest_read32( list + 136 );
            send_int_message( c, guest_read32( list ), 2008, std::clamp( cur - m.wheel, 0, std::max( count - 1, 0 ) ) );
            if ( log ) rt_log( "menu mouse: wheel %d, list item %d -> %d\n", m.wheel, cur, (int32_t)guest_read32( list + 132 ) );
        }
        else if ( list )
        {
            int32_t top = guest_read32( list + 128 );
            send_int_message( c, guest_read32( list ), 2011, std::max( top - m.wheel, 0 ) );
            if ( log ) rt_log( "menu mouse: wheel %d, list top %d -> %d\n", m.wheel, top, (int32_t)guest_read32( list + 128 ) );
            last_moves = m.moves - 1;
        }
        else
        {
            // A dossier file's page (a tab scene whose pages hold text with up / down arrows): the text scrolls a
            // line per d-pad up / down press, as on the pad; a wheel notch is one.
            uint32_t tab = 0;
            for ( uint32_t e = fe, g = 0; e && g < 32 && !tab; e = guest_read32( e + 40 ), ++g )
                if ( xui_class_data( xui_object( guest_read32( e ) ), guest_read32( TAB_SCENE_CLASS ) ) ) tab = e;
            // (Each page has its own text boxes under the same names: the page holding the focus is the one shown.)
            uint32_t page = 0;
            for ( uint32_t e = fe, g = 0; tab && e && g < 32 && !page; e = guest_read32( e + 40 ), ++g )
                if ( guest_read32( e + 40 ) == tab ) page = e;
            if ( uint32_t text = page && find_named( page, "XuiScene_Vertical Arrows 2" ) ? scrolling_text( page ) : 0 )
            {
                text_scroll( c, text, -m.wheel );
                if ( log ) rt_log( "menu mouse: wheel %d, text scroll\n", m.wheel );
            }
        }
    }
    if ( log && m.left && !was_left ) rt_log( "menu mouse: click at %.0f,%.0f focus %08X hit %08X\n", m.x, m.y, focused, hit );
    // Sliders: pressing on one holds it; while held, the value follows the pointer through the slider's own
    // set-value. The thumb is drawn by the slider's timeline, linear in the value, and only moves once the
    // timeline has run, so the line is measured from the engine: the thumb's position at two settled
    // values (one step apart to start with), after which each pointer position maps straight to a value.
    if ( m.left && !was_left && hit && xui_class_data( hit, guest_read32( SLIDER_CLASS ) ) )
    {
        if ( hit != focused ) mouse_focus( c, hit, user );
        dragging = hit;
    }
    if ( !m.left ) dragging = 0;
    if ( uint32_t data = dragging ? xui_class_data( dragging, guest_read32( SLIDER_CLASS ) ) : 0 )
    {
        static uint32_t cal_slider;
        static int cal_v[2], cal_n;
        static float cal_x[2], wait_x;
        static bool waiting;
        static std::chrono::steady_clock::time_point wait_until;
        if ( cal_slider != dragging ) { cal_slider = dragging; cal_n = 0; waiting = false; }
        int32_t lo = guest_read32( data + 16 ), hi = guest_read32( data + 20 ), v = guest_read32( data + 24 ), step = guest_read32( data + 28 );
        uint32_t body = xui_element( guest_read32( data + 4 ) ), thumb = body ? guest_read32( body + 24 ) : 0;
        if ( thumb && hi > lo && step > 0 )
        {
            float tx = f32( thumb + 208 ) + f32( thumb + 160 ) * f32( thumb + 12 ) * 0.5f;
            if ( waiting && ( tx != wait_x || now >= wait_until ) ) waiting = false;  // settled
            if ( !waiting )
            {
                if ( cal_n == 0 || ( cal_n == 1 && cal_v[0] != v && tx != cal_x[0] ) ) { cal_v[cal_n] = v; cal_x[cal_n] = tx; ++cal_n; }
                int32_t want = v;
                if ( cal_n == 2 )
                {
                    float f = cal_v[0] + ( m.x - cal_x[0] ) * ( cal_v[1] - cal_v[0] ) / ( cal_x[1] - cal_x[0] );
                    want = lo + (int32_t)std::lround( ( f - lo ) / step ) * step;
                }
                else if ( std::fabs( m.x - tx ) > 2.0f )  // one step toward the pointer gives the second point
                    want = v + ( m.x > tx ? step : -step );
                want = std::clamp( want, lo, hi );
                if ( want != v )
                {
                    guest_call( c, SLIDER_SET_VALUE, data, want );
                    if ( log ) rt_log( "menu mouse: slider %08X %d -> %d (thumb %.0f, pointer %.0f)\n", dragging, v, (int32_t)guest_read32( data + 24 ), tx, m.x );
                    waiting = true;
                    wait_x = tx;
                    wait_until = now + std::chrono::milliseconds( 150 );
                }
            }
        }
    }
    // A click presses the control under the pointer, focusing it first when hovering hasn't yet (a
    // click straight after a move). A list row is made the list's current item first: a click on a side
    // card of a side by side list (agent skins) only does that, cycling the list; a list that refuses
    // the new item is the jukebox (826F2B20) holding its playing track until that row is pressed again,
    // so the click takes the game's own steps: press the playing row (stop), make the clicked row
    // current, press it (play). The game handles the stop a little later, so the new row is retried
    // until the list takes it.
    static uint32_t switch_list, switch_row;
    static int switch_index;
    static std::chrono::steady_clock::time_point switch_until;
    int row = 0;
    bool across = false;
    uint32_t row_list = m.left && !was_left && hit && !dragging ? list_of( hit_el, &row, &across ) : 0;
    // A spinner's arrow: its row is focused (directly: the step plays the sound), then stepped.
    uint16_t arrow_dir = 0;
    uint32_t arrow = m.left && !was_left && scene && !dragging ? spinner_arrow( scene, m.x, m.y, &arrow_dir ) : 0;
    // A side by side list of its own with arrows beside it (the Agency garage's vehicles), an arrow or a
    // side card clicked: the list is focused (its row, as the pad's up / down would) and stepped with d-pad
    // left / right, so the game moves it and plays its sound.
    bool own_cards = row_list && across && has_card_arrows( xui_element( guest_read32( row_list ) ) );
    uint16_t cards_dir = 0;
    // Not over a tab scene of its own (Difficulty above the agent skins): its arrows are the ones meant.
    uint32_t cards = m.left && !was_left && scene && !dragging && !arrow && !xui_class_data( hit, guest_read32( TAB_SCENE_CLASS ) ) ? card_arrow( scene, m.x, m.y, &cards_dir ) : 0;
    if ( !cards && own_cards && row != (int32_t)guest_read32( row_list + 132 ) )
        cards = row_list, cards_dir = row > (int32_t)guest_read32( row_list + 132 ) ? 0x8 : 0x4;
    // A side by side list showing the focused spinner's choices (Street Racing's pictures): a side card
    // steps the spinner toward it, the middle one presses as the row does. (The campaign's agent skins
    // are clicked through to as a list, below: the focus is on a button then, not a spinner.)
    uint32_t carousel = row_list && across && !own_cards && spinner_of( fe ) && !contains( guest_read32( hit_el + 40 ), fe ) ? row_list : 0;
    if ( arrow )
    {
        if ( !contains( arrow, fe ) )
            if ( uint32_t item = first_focusable( arrow ) ) mouse_focus( c, xui_object( guest_read32( item ) ), user );
        list_step( c, arrow, arrow_dir == 0x8 ? 1 : -1 );
        nav_goal = 0;
        if ( log ) rt_log( "menu mouse: spinner arrow %s\n", arrow_dir == 0x8 ? "right" : "left" );
    }
    else if ( cards )
    {
        uint32_t list = xui_object( guest_read32( cards ) );
        if ( !contains( xui_element( list ), fe ) ) mouse_focus( c, list, user );
        list_step( c, guest_read32( cards ) ? xui_element( guest_read32( cards ) ) : 0, cards_dir == 0x8 ? 1 : -1 );
        nav_goal = 0;
        if ( log ) rt_log( "menu mouse: card list %08X step %s (current %d)\n", list, cards_dir == 0x8 ? "right" : "left", (int32_t)guest_read32( cards + 132 ) );
    }
    else if ( carousel )
    {
        int32_t cur = guest_read32( carousel + 132 );
        if ( row != cur ) list_step( c, spinner_of( fe ), row > cur ? 1 : -1 );
        else press( c, focused, user ), ui_sound( UI_SELECT );
        nav_goal = 0;
        if ( log ) rt_log( "menu mouse: carousel card %d (current %d)\n", row, cur );
    }
    else if ( m.left && !was_left && hit && !dragging )
    {
        int32_t cur = row_list ? (int32_t)guest_read32( row_list + 132 ) : row;
        if ( row_list && cur != row ) send_int_message( c, guest_read32( row_list ), 2008, row );
        bool moved = !row_list || (int32_t)guest_read32( row_list + 132 ) == row;
        if ( log && row_list ) rt_log( "menu mouse: list item %d clicked (current %d%s)\n", row, cur, moved ? "" : ", refused" );
        uint32_t tabs = xui_class_data( hit, guest_read32( TAB_SCENE_CLASS ) );
        if ( tabs )  // a clicked tab scene (Difficulty) steps a tab toward the clicked half, as the pad's left / right do
        {
            bool middle = false, next = tab_arrow_right( hit_el, m.x, &middle );
            if ( middle )  // the page shown: its control is pressed, as A would (the dossier's Replay Video); no step
            {
                if ( focus && focus != hit ) press( c, focus, user ), ui_sound( UI_SELECT );
                if ( log ) rt_log( "menu mouse: tab scene %08X middle page clicked (focus %08X)\n", hit, focus );
            }
            else
            {
                // Focused first unless the focus is already on one of its pages (the map's bar: a page's own holder).
                if ( hit != focused && !contains( hit_el, fe ) ) mouse_focus( c, hit, user );
                guest_call( c, next ? TAB_SCENE_NEXT : TAB_SCENE_PREV, tabs, 0 );  // what its d-pad handling calls
                ui_sound( UI_MOVE );
                if ( log ) rt_log( "menu mouse: tab scene %08X %s -> tab %u\n", hit, next ? "next" : "previous", guest_read32( tabs + 12 ) );
            }
        }
        else if ( moved && !( across && cur != row ) )
        {
            if ( hit != focused ) mouse_focus( c, hit, user );
            // Pressed through XUI (as list rows and Exit to Desktop always were); the select sound played here.
            press( c, hit, user );
            if ( !row_list && !is_exit( hit_el ) ) ui_sound( UI_SELECT );
            nav_goal = 0;
        }
        else if ( !moved && !across )
        {
            press( c, focused, user );
            switch_list = guest_read32( row_list );
            switch_row = hit;
            switch_index = row;
            switch_until = now + std::chrono::milliseconds( 500 );
        }
    }
    if ( switch_list )
    {
        uint32_t data = xui_class_data( switch_list, guest_read32( LIST_CLASS ) );
        if ( data ) send_int_message( c, switch_list, 2008, switch_index );
        bool moved = data && (int32_t)guest_read32( data + 132 ) == switch_index;
        if ( moved ) press( c, switch_row, user );
        if ( moved || now >= switch_until || !data )
        {
            if ( log ) rt_log( "menu mouse: list switch to %d %s\n", switch_index, moved ? "pressed" : "refused" );
            switch_list = 0;
        }
    }
    was_left = m.left;
    if ( tab_scene )  // a clicked tab header: its page, through the tab scene (its transition), the move sound here
    {
        if ( (int32_t)guest_read32( tab_scene + 12 ) != tab_goal )
        {
            guest_call( c, TAB_SCENE_GOTO, tab_scene, tab_goal, 1 );
            ui_sound( UI_MOVE );
        }
        tab_scene = 0;
    }
    if ( nav_goal )
    {
        // Straight to the hovered control: focused through XUI, the game's move sound played here.
        mouse_focus( c, nav_goal, user );
        if ( log ) rt_log( "menu mouse: focus %08X '%s' (was %08X)\n", nav_goal, xui_element( nav_goal ) ? wide_string( guest_read32( xui_element( nav_goal ) + 4 ) ).c_str() : "", focused );
        hover_goal = nav_goal, hover_check = now + std::chrono::milliseconds( 50 );
        nav_goal = 0;
    }
    return 0;
}

// The front end's wait messages (sign in, storage, loading the save): top-level scenes with a 'waitText'
// label. The whole scene is hidden (flags bit 0, which the engine draws by), its menu bar included, so
// nothing of it shows between the title and the main menu.
void xui_hide_wait_message()
{
    uint32_t root = xui_element( xui_focus( -1 ) );
    for ( uint32_t p; root && ( p = guest_read32( root + 40 ) ) && xui_element( guest_read32( p ) ) == p; ) root = p;
    if ( !root ) return;
    for ( uint32_t s = guest_read32( root + 24 ), g = 0; s && g < 64; s = guest_read32( s + 32 ), ++g )  // the top-level scenes
        if ( find_named( s, "waitText" ) ) guest_write32( s + 236, guest_read32( s + 236 ) & ~1u );
}

// Debug (CRACKDOWN_XUI_PROPDEFS=1): the property definitions of each class XUI looks up by name while
// loading a scene (82761AD0: class name -> class), which is what a .xur's property masks index. Class:
// +4 name, +8 base class name, +24 definitions, +28 count, +32 base class. CRACKDOWN_XUI_CLASSDUMP=1:
// every registered class (XUI's table at 831CBF18: 27 buckets of { hash, name, class, next }) to
// build/xui_classes.txt, with the definitions of object properties (type 9; definition +40 is a function
// (&count) -> definitions) nested. Definition (48 bytes): +0 flags (bit 0 indexed), +4 index,
// +8 offset in the class data, +16 name, +20 type (1 bool, 2 int, 3 unsigned, 4 float, 5 string, 6 colour,
// 7 vector, 8 quaternion, 9 object, 10 custom).
extern "C" PPC_FUNC( __imp__sub_82761AD0 );
PPC_FUNC( sub_82761AD0 )
{
    uint32_t name = ( (PPCContext *)( ctx - 0x80 ) )->r3.u32;
    __imp__sub_82761AD0( ctx, base, frame );
    static const bool on = getenv( "CRACKDOWN_XUI_PROPDEFS" );
    uint32_t cls = ( (PPCContext *)( ctx - 0x80 ) )->r3.u32;
    static bool dumped;
    if ( !dumped && getenv( "CRACKDOWN_XUI_CLASSDUMP" ) )
    {
        dumped = true;
        PPCContext &c = *(PPCContext *)( ctx - 0x80 );
        FILE *f = fopen( "build/xui_classes.txt", "w" );
        std::function<void( uint32_t, uint32_t, int )> defs = [&]( uint32_t d0, uint32_t n, int depth ) {
            for ( uint32_t i = 0; i < n && i < 64; ++i )
            {
                uint32_t d = d0 + i * 48, type = guest_read32( d + 20 );
                fprintf( f, "%*s%u %s %u %u\n", depth * 2 + 2, "", i, wide_string( guest_read32( d + 16 ) ).c_str(), type, guest_read32( d ) & 1 );
                if ( type == 9 && guest_read32( d + 40 ) && depth < 3 )
                {
                    PPCContext saved = c;
                    uint32_t slot = ( c.r1.u32 - 512 ) & ~15u;
                    c.r1.u64 = slot - 256;
                    c.r3.u64 = slot;
                    rt_lookup( guest_read32( d + 40 ) )( ctx, base, frame );
                    uint32_t sub = c.r3.u32, count = guest_read32( slot );
                    c = saved;
                    defs( sub, count, depth + 1 );
                }
            }
        };
        for ( int b = 0; f && b < 27; ++b )
            for ( uint32_t e = guest_read32( 0x831CBF18 + b * 4 ), g = 0; e && g < 1000; e = guest_read32( e + 12 ), ++g )
            {
                uint32_t k = guest_read32( e + 8 );
                fprintf( f, "class %s base %s\n", wide_string( guest_read32( e + 4 ) ).c_str(), k ? wide_string( guest_read32( k + 8 ) ).c_str() : "" );
                if ( k ) fprintf( f, "  # procs %08X %08X %08X\n", guest_read32( k + 12 ), guest_read32( k + 16 ), guest_read32( k + 20 ) );
                if ( k ) defs( guest_read32( k + 24 ), guest_read32( k + 28 ), 0 );
            }
        if ( f ) fclose( f );
        rt_log( "xui: classes written to build/xui_classes.txt\n" );
    }
    if ( !on || !cls ) return;
    static std::vector<uint32_t> seen;
    for ( uint32_t k = cls; k && seen.size() < 4096; k = guest_read32( k + 32 ) )
    {
        if ( std::find( seen.begin(), seen.end(), k ) != seen.end() ) break;
        seen.push_back( k );
        uint32_t defs = guest_read32( k + 24 ), n = guest_read32( k + 28 );
        std::string line = "xui propdefs: " + wide_string( guest_read32( k + 4 ) ) + " base " + wide_string( guest_read32( k + 8 ) ) + ":";

        for ( uint32_t i = 0; i < n && i < 64; ++i )
        {
            uint32_t d = defs + i * 48;
            char b[96];
            snprintf( b, sizeof( b ), " %u=%s/t%u%s", i, wide_string( guest_read32( d + 16 ) ).c_str(), guest_read32( d + 20 ), guest_read32( d ) & 1 ? "[]" : "" );
            line += b;
        }
        rt_log( "%s\n", line.c_str() );
    }
}

// The focus a caller last asked XuiElementSetFocus for. CRACKDOWN_FOCUS_LOG=1 (testing): each one, with its caller.
static std::atomic<uint32_t> g_focus_asked;
// A menu key (arrow, Return, Space) just went down. True: all it did is bring the highlight out.
bool xui_menu_key( PPCContext &c, bool arrow )
{
    if ( g_keyboard_nav || !focus_hidden_here() ) return false;
    g_keyboard_nav = true;
    const bool any = std::any_of( g_looks.begin(), g_looks.end(), []( const Look &l ) { return !l.shown; } );
    refresh_looks( c );
    return any;
}

// 82769D50 (control's part handle, first named frame, last named frame, 0, 0) plays a look: every XUI
// control's way of showing its state ends here (buttons 82776438, check boxes 82777A88, list rows 827797F0,
// list groups 827777A0, scroll arrows 82779D20). A look that says "this is where you are" is swapped for the
// plain one while the highlight is held back.
namespace
{
struct LookSwap { uint32_t look, plain, plain_end, chosen, chosen_end; };  // chosen: the same for a side by side list's item, which stays marked
constexpr uint32_t NORMAL = 0x820F65F0, NORMAL_END = 0x820F65DC, NORMAL_OFF = 0x820F658C, NORMAL_OFF_END = 0x820F6568;
constexpr uint32_t NORMAL_CHECK = 0x820F64BC, NORMAL_CHECK_END = 0x820F649C, NORMAL_CHECK_OFF = 0x820F6440, NORMAL_CHECK_OFF_END = 0x820F6414;
constexpr uint32_t SEL = 0x820F63B0, SEL_END = 0x820F6394, SEL_OFF = 0x820F632C, SEL_OFF_END = 0x820F6304;
constexpr uint32_t SEL_CHECK = 0x820F6374, SEL_CHECK_END = 0x820F6350, SEL_CHECK_OFF = 0x820F62D8, SEL_CHECK_OFF_END = 0x820F62A4;
constexpr uint32_t KILL_FOCUS = 0x820F65A8;
// NormalSel and its forms are not here: they mark a list's chosen item (the agent, an equipped weapon).
constexpr LookSwap LOOKS[] = {
    { 0x820F65D0 /* Focus */, NORMAL, NORMAL_END, SEL, SEL_END },
    { 0x820F61DC /* InitFocus */, NORMAL, NORMAL_END, SEL, SEL_END },
    { 0x820F654C /* FocusDisable */, NORMAL_OFF, NORMAL_OFF_END, SEL_OFF, SEL_OFF_END },
    { 0x820F619C /* InitFocusDisable */, NORMAL_OFF, NORMAL_OFF_END, SEL_OFF, SEL_OFF_END },
    { 0x820F6484 /* FocusCheck */, NORMAL_CHECK, NORMAL_CHECK_END, SEL_CHECK, SEL_CHECK_END },
    { 0x820F6154 /* InitFocusCheck */, NORMAL_CHECK, NORMAL_CHECK_END, SEL_CHECK, SEL_CHECK_END },
    { 0x820F63F0 /* FocusCheckDisable */, NORMAL_CHECK_OFF, NORMAL_CHECK_OFF_END, SEL_CHECK_OFF, SEL_CHECK_OFF_END },
    { 0x820F6104 /* InitFocusCheckDisable */, NORMAL_CHECK_OFF, NORMAL_CHECK_OFF_END, SEL_CHECK_OFF, SEL_CHECK_OFF_END },
};
auto look_of( uint32_t part )
{
    return std::find_if( g_looks.begin(), g_looks.end(), [&]( const Look &l ) { return l.part == part; } );
}
} // namespace

extern "C" PPC_FUNC( __imp__sub_82769D50 );
PPC_FUNC( sub_82769D50 )
{
    auto &c = *(PPCContext *)( ctx - 0x80 );
    const uint32_t part = c.r3.u32, look = c.r4.u32;
    static const bool log = getenv( "CRACKDOWN_LOOK_LOG" );
    const LookSwap *swap = nullptr;
    for ( const LookSwap &l : LOOKS )
        if ( l.look == look ) swap = &l;
    auto known = look_of( part );
    if ( swap )
    {
        const bool shown = highlight_wanted( part );
        if ( known == g_looks.end() ) g_looks.push_back( { part, look, c.r5.u32, 0, 0, shown } );
        else known->look = look, known->look_end = c.r5.u32, known->shown = shown;
        if ( log ) rt_log( "look: %08X %08X shown %d\n", part, look, shown );
        if ( !shown )
        {
            int index = 0;
            bool across = false;
            const uint32_t obj = xui_object( part ), el = obj ? xui_element( obj ) : 0;
            const bool item = el && list_of( el, &index, &across ) && across;
            c.r4.u64 = item ? swap->chosen : swap->plain, c.r5.u64 = item ? swap->chosen_end : swap->plain_end;
        }
    }
    else if ( known != g_looks.end() )  // it left that state
    {
        if ( look == KILL_FOCUS && !known->shown ) c.r4.u64 = NORMAL;  // it never looked it: nothing to fade out
        g_looks.erase( known );
    }
    __imp__sub_82769D50( ctx, base, frame );
}

// 8277E578 (a slider or progress bar's data, enabled, use the next word, focused) moves its look by hand.
extern "C" PPC_FUNC( __imp__sub_8277E578 );
PPC_FUNC( sub_8277E578 )
{
    auto &c = *(PPCContext *)( ctx - 0x80 );
    const uint32_t data = c.r3.u32, part = guest_read32( data );
    const uint32_t enabled = c.r4.u32, use = c.r5.u32, said = c.r6.u32;
    const bool focused = use ? said != 0 : guest_call( c, 0x82769720, part, 0 ) != 0;
    auto known = look_of( part );
    if ( focused )
    {
        const bool shown = highlight_wanted( part );
        if ( known == g_looks.end() ) g_looks.push_back( { part, 0, 0, data, enabled, shown } );
        else known->shown = shown;
        if ( !shown ) c.r5.u64 = 1, c.r6.u64 = 0;
    }
    else if ( known != g_looks.end() ) g_looks.erase( known );
    __imp__sub_8277E578( ctx, base, frame );
}

extern "C" PPC_FUNC( __imp__sub_82768A58 );
PPC_FUNC( sub_82768A58 )
{
    auto &c = *(PPCContext *)( ctx - 0x80 );
    g_focus_asked = c.r3.u32;
    if ( static const bool log = getenv( "CRACKDOWN_FOCUS_LOG" ); log ) rt_log( "focus: set %08X user %u lr %08X\n", c.r3.u32, c.r4.u32, (uint32_t)c.lr );
    __imp__sub_82768A58( ctx, base, frame );
}

// The map's Supply Points and Boss Locations lists: 826AF758 and 826AF928 run when the focus moves and put
// it on the row the list holds (+164, +160), which only its d-pad flags (down +964, up +880) step, so the
// keyboard's and the mouse's moves were undone at once (the old row back, the sound played again for each
// pointer move). Rows are 80 and 16 bytes, each its element at +0 (handle at +4), with their end 16 bytes
// before them (+176, +672). The row a move reached by another way, -1 if none (or the flags have it).
static int row_of_focus( uint32_t list, uint32_t skip, uint32_t rows, uint32_t stride )
{
    if ( g_base[list + skip] || g_base[list + 964] || g_base[list + 880] ) return -1;
    const uint32_t asked = g_focus_asked, end = guest_read32( list + rows - 16 ), first = list + rows;
    for ( uint32_t row = first, i = 0; end > first && row < end && i < 64; row += stride, ++i )
        if ( uint32_t el = guest_read32( row ); el && guest_read32( el + 4 ) == asked ) return i;
    return -1;
}
extern "C" PPC_FUNC( __imp__sub_826AF758 );
PPC_FUNC( sub_826AF758 )
{
    const uint32_t list = ( (PPCContext *)( ctx - 0x80 ) )->r3.u32;
    if ( int row = row_of_focus( list, 172, 192, 80 ); row >= 0 ) guest_write32( list + 164, row );
    __imp__sub_826AF758( ctx, base, frame );
}
// Without a flag this one goes to its first row, so a later row is reached as a d-pad down from the one before.
extern "C" PPC_FUNC( __imp__sub_826AF928 );
PPC_FUNC( sub_826AF928 )
{
    const uint32_t list = ( (PPCContext *)( ctx - 0x80 ) )->r3.u32;
    // The game's own up or down (its pad flags set) moves from the row it holds.
    const int row = g_base[list + 880] || g_base[list + 964] ? -1 : row_of_focus( list, 173, 688, 16 );
    if ( row > 0 ) guest_write32( list + 160, row - 1 ), g_base[list + 964] = 1;
    __imp__sub_826AF928( ctx, base, frame );
    if ( row > 0 ) g_base[list + 964] = 0;
}

// The game started again in this process's place: same program, arguments, folder and output, for the
// settings only a start takes on. The saved settings are already written.
void restart_game()
{
    std::vector<char> cmdline;
    if ( FILE *f = fopen( "/proc/self/cmdline", "rb" ) )
    {
        char chunk[4096];
        for ( size_t n; ( n = fread( chunk, 1, sizeof( chunk ), f ) ) > 0; ) cmdline.insert( cmdline.end(), chunk, chunk + n );
        fclose( f );
    }
    std::vector<char *> argv;
    for ( size_t i = 0; i < cmdline.size(); i += strlen( &cmdline[i] ) + 1 ) argv.push_back( &cmdline[i] );
    argv.push_back( nullptr );
    char path[4096];
    ssize_t n = readlink( "/proc/self/exe", path, sizeof( path ) - 1 );
    path[n > 0 ? n : 0] = 0;
    rt_log( "menu: restarting for the new settings\n" );
    close_range( 3, ~0u, CLOSE_RANGE_CLOEXEC );  // the new run doesn't keep this one's files (guest memory among them)
    sigset_t none;
    sigemptyset( &none );
    pthread_sigmask( SIG_SETMASK, &none, nullptr );
    if ( argv.size() > 1 ) execv( path, argv.data() );  // (its name, which is what tools find it by)
    if ( argv.size() > 1 ) execv( "/proc/self/exe", argv.data() );  // the program was replaced on disk
    rt_log( "menu: couldn't restart: %s\n", strerror( errno ) );
}

// The keys the menu code wants held (bits as video_menu_keys).

// The menu's setup: its help object, for Exit to Desktop's help line.
extern "C" PPC_FUNC( __imp__sub_826F4AF0 );
PPC_FUNC( sub_826F4AF0 )
{
    g_menu_help = ( (PPCContext *)( ctx - 0x80 ) )->r5.u32;
    __imp__sub_826F4AF0( ctx, base, frame );
}

// The d-pad buttons the menu mouse holds now, sent as controller 0's (xam.cpp keystrokes).

// The Achievements page made (r3: the object) and destroyed (r3: the object + 684).
extern "C" PPC_FUNC( __imp__sub_826C0610 );
extern "C" PPC_FUNC( __imp__sub_826C0D18 );
PPC_FUNC( sub_826C0610 )
{
    const uint32_t page = ( (PPCContext *)( ctx - 0x80 ) )->r3.u32;
    __imp__sub_826C0610( ctx, base, frame );
    g_achievements_page = page;
}
PPC_FUNC( sub_826C0D18 )
{
    if ( ( (PPCContext *)( ctx - 0x80 ) )->r3.u32 - 684 == g_achievements_page ) g_achievements_page = 0;
    __imp__sub_826C0D18( ctx, base, frame );
}

// The in-game HUD (InGame.xur's object, made by 8269ACC8; +4 vtable 820F1AD8) binds 8269AFF0 (this) to the
// front end's "Pause Button" input event (START): it opens the pause menu (scene 82) when the game allows
// it, returning 1. Esc in game calls the same, on the game's thread (xui_pause_poll, from the input poll).
namespace
{
uint32_t g_ingame_hud;
std::atomic<bool> g_pause_request{ false };
}
extern "C" PPC_FUNC( __imp__sub_8269ACC8 );
PPC_FUNC( sub_8269ACC8 )
{
    const uint32_t hud = ( (PPCContext *)( ctx - 0x80 ) )->r3.u32;
    __imp__sub_8269ACC8( ctx, base, frame );
    g_ingame_hud = hud;
}
void xui_request_pause() { g_pause_request = true; }
void xui_pause_poll( PPCContext &c )
{
    if ( !g_pause_request.exchange( false ) ) return;
    if ( !g_ingame_hud || guest_read32( g_ingame_hud + 4 ) != 0x820F1AD8 ) { rt_log( "pause: no in-game HUD\n" ); return; }
    const uint32_t opened = guest_call( c, 0x8269AFF0, g_ingame_hud, 0 );
    if ( getenv( "CRACKDOWN_MOUSE_LOG" ) ) rt_log( "pause: Esc -> %s\n", opened ? "paused" : "refused" );
}

// Debug (CRACKDOWN_KEY_LOG): a keyboard Esc delivered to a menu logs the focus then and the XUI
// messages of the half second after it (what the menu did with the key).
static std::atomic<int64_t> g_back_log_until{ 0 };
void xui_note_back_key()
{
    const uint32_t f = xui_focus( 0 ) ? xui_focus( 0 ) : xui_focus( -1 ), e = xui_element( f );
    rt_log( "xui: Esc with the focus on %08X '%s'\n", f, e ? wide_string( guest_read32( e + 4 ) ).c_str() : "" );
    g_back_log_until = ( std::chrono::steady_clock::now() + std::chrono::milliseconds( 500 ) ).time_since_epoch().count();
}

// Debug (CRACKDOWN_XUI_MSGLOG_AT=<script seconds>): every XUI message sent through 82762B98 (handle,
// XUIMessage *: +4 id, +16 data) for half a second from then, with its target's id and caller.
extern "C" PPC_FUNC( __imp__sub_82762B98 );
PPC_FUNC( sub_82762B98 )
{
    static const double at = getenv( "CRACKDOWN_XUI_MSGLOG_AT" ) ? atof( getenv( "CRACKDOWN_XUI_MSGLOG_AT" ) ) : -1;
    const bool after_back = g_back_log_until.load() && std::chrono::steady_clock::now().time_since_epoch().count() < g_back_log_until.load();
    if ( at < 0 && !after_back ) { __imp__sub_82762B98( ctx, base, frame ); return; }
    PPCContext &c = *(PPCContext *)( ctx - 0x80 );
    const uint32_t h = c.r3.u32, msg = c.r4.u32, id = msg ? guest_read32( msg + 4 ) : 0, data = msg ? guest_read32( msg + 16 ) : 0, lr = (uint32_t)c.lr;
    __imp__sub_82762B98( ctx, base, frame );
    double input_script_clock();
    const double t = input_script_clock();
    if ( ( after_back || ( t >= at && t < at + 0.5 ) ) && id != 3 && id != 4 && id != 5 && id != 6 && id != 15 && id != 16 && id != 17 && id != 2039 )
        rt_log( "xui msg: %08X '%s' msg %u data[0] %d (from %08X)\n", h, xui_element( h ) ? wide_string( guest_read32( xui_element( h ) + 4 ) ).c_str() : "", id,
                data ? (int32_t)guest_read32( data ) : 0, lr );
}

// Debug (CRACKDOWN_PRESS_TRACE=<vk hex>): the functions the front end's keystroke handling (8268FDD8,
// r4: the keystroke) ran for that key's press, logged as it returns.
extern "C" PPC_FUNC( __imp__sub_8268FDD8 );
PPC_FUNC( sub_8268FDD8 )
{
    static const char *env = getenv( "CRACKDOWN_PRESS_TRACE" );
    static const uint32_t vk = env ? strtoul( env, nullptr, 16 ) : 0;
    PPCContext &c = *(PPCContext *)( ctx - 0x80 );
    const bool traced = vk > 1 && guest_read16( c.r4.u32 ) == vk && ( guest_read16( c.r4.u32 + 4 ) & 1 );
    __imp__sub_8268FDD8( ctx, base, frame );
    if ( traced ) rt_dump_trace( "front end keystroke" );
}

// Debug (CRACKDOWN_KEY_LOG): the front end's search for the scene a new one goes forward from
// (8269B9B0: r4 + 4 receives it; none = the new scene has no scene to go back to). Logged with the
// element tree when it finds none.
extern "C" PPC_FUNC( __imp__sub_8269B9B0 );
PPC_FUNC( sub_8269B9B0 )
{
    const uint32_t out = ( (PPCContext *)( ctx - 0x80 ) )->r4.u32;
    __imp__sub_8269B9B0( ctx, base, frame );
    static const bool log = getenv( "CRACKDOWN_KEY_LOG" );
    if ( !log ) return;
    const uint32_t found = guest_read32( out + 4 ), e = xui_element( found );
    rt_log( "xui: scene to go forward from: %08X '%s'\n", found, e ? wide_string( guest_read32( e + 4 ) ).c_str() : "" );
    double input_script_clock();
    static const double dump_after = getenv( "CRACKDOWN_SCENE_DUMP_AFTER" ) ? atof( getenv( "CRACKDOWN_SCENE_DUMP_AFTER" ) ) : 1e9;
    static int dumps;
    if ( !found && input_script_clock() >= dump_after && dumps++ < 2 ) xui_all_roots();
}

// cursor.cpp: the pointer is on a control (its glow brightens there).
bool xui_pointer_on_control() { return g_pointer_el != 0; }
