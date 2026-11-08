// The Keyboard Controls page (Options -> Keyboard Controls): a keyboard and mouse picture with the keys in
// use lit, and the player's commands (keybinds.h) with their two keys each. A slot clicked waits for a key
// or mouse button to take; Backspace or Delete clears it, Esc leaves it as it was. Drawn here in software
// (1920 x 1080, R, G, B, A bytes) and put over the game's picture by the window (video.cpp), which hands
// this page the mouse and keyboard while it is up: the game gets nothing then.
#include "keybinds.h"
#include "runtime.h"
#include "settings.h"
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_mouse.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

using namespace keybinds;

namespace
{
constexpr int W = 1920, H = 1080;
std::atomic<bool> g_open{ false };
std::mutex g_lock;  // the page's state (the window thread draws and feeds it; the game thread opens it)
std::vector<uint32_t> g_px;
bool g_dirty = true;
float g_mx = -1, g_my = -1;
int g_capture_cmd = -1, g_capture_slot = 0;
uint32_t g_drawn_version;

// Text: Liberation Sans (the game's font's twin), Bold for headings.
struct Font
{
    FT_Library lib = nullptr;
    FT_Face regular = nullptr, bold = nullptr;
    bool tried = false;
} g_font;

FT_Face face( bool bold )
{
    if ( !g_font.tried )
    {
        g_font.tried = true;
        static const char *const dirs[] = { "/usr/share/fonts/liberation/", "/usr/share/fonts/truetype/liberation/", "/usr/share/fonts/liberation-sans/",
                                            "/usr/share/fonts/truetype/liberation2/" };
        if ( !FT_Init_FreeType( &g_font.lib ) )
            for ( const char *d : dirs )
            {
                if ( !g_font.regular ) FT_New_Face( g_font.lib, ( std::string( d ) + "LiberationSans-Regular.ttf" ).c_str(), 0, &g_font.regular );
                if ( !g_font.bold ) FT_New_Face( g_font.lib, ( std::string( d ) + "LiberationSans-Bold.ttf" ).c_str(), 0, &g_font.bold );
            }
        if ( !g_font.bold ) g_font.bold = g_font.regular;
    }
    return bold ? g_font.bold : g_font.regular;
}

struct Rgba { uint8_t r, g, b, a; };
void blend( int x, int y, Rgba c, float cover = 1.0f )
{
    if ( x < 0 || y < 0 || x >= W || y >= H ) return;
    const float a = c.a / 255.0f * cover;
    if ( a <= 0 ) return;
    uint8_t *d = (uint8_t *)&g_px[(size_t)y * W + x];
    const float da = d[3] / 255.0f, oa = a + da * ( 1 - a );
    d[0] = (uint8_t)std::lround( ( c.r * a + d[0] * da * ( 1 - a ) ) / oa );
    d[1] = (uint8_t)std::lround( ( c.g * a + d[1] * da * ( 1 - a ) ) / oa );
    d[2] = (uint8_t)std::lround( ( c.b * a + d[2] * da * ( 1 - a ) ) / oa );
    d[3] = (uint8_t)std::lround( oa * 255 );
}

// A rectangle with rounded corners (radius r), filled, then its rim (width rim) in another colour.
void box( float x0, float y0, float x1, float y1, float r, Rgba fill, Rgba edge = { 0, 0, 0, 0 }, float rim = 0 )
{
    for ( int y = (int)std::floor( y0 ); y < (int)std::ceil( y1 ); ++y )
        for ( int x = (int)std::floor( x0 ); x < (int)std::ceil( x1 ); ++x )
        {
            const float px = x + 0.5f, py = y + 0.5f;
            const float qx = std::max( { x0 + r - px, px - ( x1 - r ), 0.0f } ), qy = std::max( { y0 + r - py, py - ( y1 - r ), 0.0f } );
            const float d = std::sqrt( qx * qx + qy * qy ) - r;  // inside: negative near the corners' reach
            const float inside = std::min( { px - x0, x1 - px, py - y0, y1 - py } );
            const float depth = qx > 0 || qy > 0 ? -d : inside;  // distance in from the edge
            const float cover = std::clamp( depth + 0.5f, 0.0f, 1.0f );
            if ( cover <= 0 ) continue;
            if ( rim > 0 && depth < rim ) blend( x, y, edge, cover );
            else blend( x, y, fill, cover );
        }
}

float text_width( const std::string &s, int size, bool bold )
{
    FT_Face f = face( bold );
    if ( !f ) return 0;
    FT_Set_Pixel_Sizes( f, 0, size );
    float w = 0;
    for ( unsigned char ch : s )
        if ( !FT_Load_Char( f, ch, FT_LOAD_DEFAULT ) ) w += f->glyph->advance.x / 64.0f;
    return w;
}

// Text with its baseline at y; align 0 left of x, 1 centred on x, 2 right of x. Shrunk to max_w if given.
void text( const std::string &s, float x, float y, int size, Rgba c, int align = 0, bool bold = false, float max_w = 0 )
{
    FT_Face f = face( bold );
    if ( !f || s.empty() ) return;
    while ( max_w > 0 && size > 8 && text_width( s, size, bold ) > max_w ) --size;
    const float w = text_width( s, size, bold );
    float pen = align == 1 ? x - w / 2 : align == 2 ? x - w : x;
    FT_Set_Pixel_Sizes( f, 0, size );
    for ( unsigned char ch : s )
    {
        if ( FT_Load_Char( f, ch, FT_LOAD_RENDER ) ) continue;
        const FT_GlyphSlot g = f->glyph;
        for ( uint32_t row = 0; row < g->bitmap.rows; ++row )
            for ( uint32_t col = 0; col < g->bitmap.width; ++col )
                if ( uint8_t v = g->bitmap.buffer[row * g->bitmap.pitch + col] )
                    blend( (int)std::lround( pen ) + g->bitmap_left + (int)col, (int)std::lround( y ) - g->bitmap_top + (int)row, c, v / 255.0f );
        pen += g->advance.x / 64.0f;
    }
}

// The game's menu colours: deep blue panels, light blue rims, white text.
const Rgba PANEL{ 14, 30, 78, 240 }, RIM{ 92, 150, 235, 255 }, TEXT{ 235, 240, 250, 255 }, DIM{ 150, 170, 210, 255 };
const Rgba KEY{ 30, 44, 86, 255 }, KEY_USED{ 40, 92, 170, 255 }, KEY_HOT{ 235, 150, 40, 255 }, SLOT{ 22, 40, 96, 255 }, SLOT_HOVER{ 50, 90, 170, 255 };

// The keyboard picture: rows of (scancode, width in key units, label) at one key unit's pixels.
struct Key { int sc; float w; const char *label; };
const std::vector<std::vector<Key>> &keyboard_rows()
{
#define S( x ) SDL_SCANCODE_##x
    static const std::vector<std::vector<Key>> rows = {
        { { S( ESCAPE ), 1, "Esc" }, { 0, 0.5f, "" }, { S( F1 ), 1, "F1" }, { S( F2 ), 1, "F2" }, { S( F3 ), 1, "F3" }, { S( F4 ), 1, "F4" }, { 0, 0.5f, "" },
          { S( F5 ), 1, "F5" }, { S( F6 ), 1, "F6" }, { S( F7 ), 1, "F7" }, { S( F8 ), 1, "F8" }, { 0, 0.5f, "" }, { S( F9 ), 1, "F9" }, { S( F10 ), 1, "F10" },
          { S( F11 ), 1, "F11" }, { S( F12 ), 1, "F12" } },
        { { S( GRAVE ), 1, "`" }, { S( 1 ), 1, "1" }, { S( 2 ), 1, "2" }, { S( 3 ), 1, "3" }, { S( 4 ), 1, "4" }, { S( 5 ), 1, "5" }, { S( 6 ), 1, "6" },
          { S( 7 ), 1, "7" }, { S( 8 ), 1, "8" }, { S( 9 ), 1, "9" }, { S( 0 ), 1, "0" }, { S( MINUS ), 1, "-" }, { S( EQUALS ), 1, "=" }, { S( BACKSPACE ), 2, "Backspace" } },
        { { S( TAB ), 1.5f, "Tab" }, { S( Q ), 1, "Q" }, { S( W ), 1, "W" }, { S( E ), 1, "E" }, { S( R ), 1, "R" }, { S( T ), 1, "T" }, { S( Y ), 1, "Y" },
          { S( U ), 1, "U" }, { S( I ), 1, "I" }, { S( O ), 1, "O" }, { S( P ), 1, "P" }, { S( LEFTBRACKET ), 1, "[" }, { S( RIGHTBRACKET ), 1, "]" }, { S( BACKSLASH ), 1.5f, "\\" } },
        { { S( CAPSLOCK ), 1.75f, "Caps" }, { S( A ), 1, "A" }, { S( S ), 1, "S" }, { S( D ), 1, "D" }, { S( F ), 1, "F" }, { S( G ), 1, "G" }, { S( H ), 1, "H" },
          { S( J ), 1, "J" }, { S( K ), 1, "K" }, { S( L ), 1, "L" }, { S( SEMICOLON ), 1, ";" }, { S( APOSTROPHE ), 1, "'" }, { S( RETURN ), 2.25f, "Enter" } },
        { { S( LSHIFT ), 2.25f, "Shift" }, { S( Z ), 1, "Z" }, { S( X ), 1, "X" }, { S( C ), 1, "C" }, { S( V ), 1, "V" }, { S( B ), 1, "B" }, { S( N ), 1, "N" },
          { S( M ), 1, "M" }, { S( COMMA ), 1, "," }, { S( PERIOD ), 1, "." }, { S( SLASH ), 1, "/" }, { S( RSHIFT ), 2.75f, "Shift" } },
        { { S( LCTRL ), 1.5f, "Ctrl" }, { S( LGUI ), 1.25f, "Super" }, { S( LALT ), 1.25f, "Alt" }, { S( SPACE ), 6.25f, "Space" }, { S( RALT ), 1.25f, "Alt" },
          { S( APPLICATION ), 1.25f, "Menu" }, { S( RCTRL ), 1.5f, "Ctrl" } },
    };
#undef S
    return rows;
}

// Layout.
constexpr float PX0 = 80, PY0 = 50, PX1 = W - 80, PY1 = H - 50;  // the panel
constexpr float KB_X = 130, KB_Y = 178, UNIT = 60, GAP = 6;       // the keyboard
constexpr float ARROW_X = KB_X + 15.5f * UNIT;                      // the arrow keys, right of it
constexpr float MOUSE_X = 1500, MOUSE_Y = 178;                      // the mouse
constexpr float LIST_Y = 600, ROW_H = 40, SLOT_W = 100;
struct Column { const char *group; int from, to; float x; };      // commands [from, to) of a group
float COL_W = 400;

struct SlotRect { int cmd, slot; float x0, y0, x1, y1; };
std::vector<SlotRect> g_slots;
struct ButtonRect { int id; float x0, y0, x1, y1; };  // 1 reset, 2 done; the mouse's: 10 -, 11 +, 12 the bar, 13 normal, 14 inverted
std::vector<ButtonRect> g_buttons;
struct KeyRect { int input; float x0, y0, x1, y1; };
std::vector<KeyRect> g_key_rects;  // the keyboard's and mouse's keys as last drawn

bool inside( float x, float y, float x0, float y0, float x1, float y1 ) { return x >= x0 && x < x1 && y >= y0 && y < y1; }

// The commands an input is given to.
std::vector<int> commands_of( int input )
{
    std::vector<int> out;
    if ( !input ) return out;
    for ( int i = 0; i < count(); ++i )
    {
        const Command &c = command( i );
        if ( c.keys[0] == input || c.keys[1] == input ) out.push_back( i );
    }
    return out;
}

int hovered_cmd()
{
    for ( const SlotRect &s : g_slots )
        if ( inside( g_mx, g_my, s.x0 - 230, s.y0, s.x1, s.y1 ) ) return s.cmd;  // its row (the label too)
    return -1;
}

void draw_key( float x0, float y0, float x1, float y1, int input, const char *label, int hot_cmd )
{
    const std::vector<int> cmds = commands_of( input );
    bool hot = false;
    for ( int c : cmds ) hot |= c == hot_cmd || c == g_capture_cmd;
    const bool hover = inside( g_mx, g_my, x0, y0, x1, y1 );
    Rgba fill = hot ? KEY_HOT : !cmds.empty() ? KEY_USED : KEY;
    if ( input == SDL_SCANCODE_ESCAPE ) fill = { 70, 40, 50, 255 };
    box( x0, y0, x1, y1, 7, fill, hover ? Rgba{ 255, 255, 255, 255 } : RIM, hover ? 3.0f : 1.5f );
    text( label, ( x0 + x1 ) / 2, y0 + ( y1 - y0 ) * 0.42f, 17, TEXT, 1, true, x1 - x0 - 8 );
    // What it does: the first command's name, small, under the key's own.
    if ( !cmds.empty() )
    {
        std::string what = command( cmds[0] ).label;
        if ( size_t slash = what.find( " /" ); slash != std::string::npos ) what = what.substr( 0, slash );
        if ( cmds.size() > 1 ) what += " +";
        text( what, ( x0 + x1 ) / 2, y1 - 9, 12, TEXT, 1, false, x1 - x0 - 6 );
    }
    if ( input == SDL_SCANCODE_ESCAPE ) text( "Pause", ( x0 + x1 ) / 2, y1 - 9, 12, DIM, 1, false, x1 - x0 - 6 );
    g_key_rects.push_back( { input, x0, y0, x1, y1 } );
}

void render()
{
    g_px.assign( (size_t)W * H, 0 );
    g_slots.clear();
    g_buttons.clear();
    g_key_rects.clear();
    const int hot = g_capture_cmd >= 0 ? g_capture_cmd : hovered_cmd();
    // The dimmed game behind, the panel.
    for ( auto &p : g_px ) p = 0xC8000000u;
    box( PX0, PY0, PX1, PY1, 14, PANEL, RIM, 2 );
    text( "Keyboard Controls", PX0 + 50, PY0 + 62, 40, TEXT, 0, true );
    text( "Click a command's key, then press the key or mouse button for it.  Backspace clears it, Esc cancels.", PX0 + 50, PY0 + 98, 20, DIM );

    // The keyboard.
    float y = KB_Y;
    for ( size_t r = 0; r < keyboard_rows().size(); ++r )
    {
        float x = KB_X;
        const float h = r == 0 ? UNIT * 0.8f : UNIT;
        for ( const Key &k : keyboard_rows()[r] )
        {
            const float w = k.w * UNIT;
            if ( k.sc ) draw_key( x, y, x + w - GAP, y + h - GAP, k.sc, k.label, hot );
            x += w;
        }
        y += h + ( r == 0 ? 10 : 0 );
    }
    // The arrow keys and the block above them.
    {
        const float by = KB_Y + UNIT * 0.8f + 10;
        const struct { int sc; float cx, cy; const char *l; } nav[] = {
            { SDL_SCANCODE_INSERT, 0, 0, "Ins" }, { SDL_SCANCODE_HOME, 1, 0, "Home" }, { SDL_SCANCODE_PAGEUP, 2, 0, "PgUp" },
            { SDL_SCANCODE_DELETE, 0, 1, "Del" }, { SDL_SCANCODE_END, 1, 1, "End" },   { SDL_SCANCODE_PAGEDOWN, 2, 1, "PgDn" },
            { SDL_SCANCODE_UP, 1, 3, "Up" },      { SDL_SCANCODE_LEFT, 0, 4, "Left" }, { SDL_SCANCODE_DOWN, 1, 4, "Down" }, { SDL_SCANCODE_RIGHT, 2, 4, "Right" } };
        for ( auto &n : nav )
            draw_key( ARROW_X + n.cx * UNIT, by + n.cy * UNIT, ARROW_X + ( n.cx + 1 ) * UNIT - GAP, by + ( n.cy + 1 ) * UNIT - GAP, n.sc, n.l, hot );
    }
    // The mouse: its body, the two buttons, the wheel, the side buttons.
    {
        const float mx = MOUSE_X, my = MOUSE_Y, mw = 210, mh = 290;
        box( mx, my, mx + mw, my + mh, 100, { 26, 38, 76, 255 }, RIM, 2 );
        draw_key( mx + 14, my + 14, mx + mw / 2 - 18, my + 150, MOUSE_L, "Left", hot );
        draw_key( mx + mw / 2 + 18, my + 14, mx + mw - 14, my + 150, MOUSE_R, "Right", hot );
        draw_key( mx + mw / 2 - 16, my + 30, mx + mw / 2 + 16, my + 110, MOUSE_M, "M", hot );
        draw_key( mx - 70, my + 170, mx - 6, my + 220, MOUSE_X2, "M5", hot );
        draw_key( mx - 70, my + 228, mx - 6, my + 278, MOUSE_X1, "M4", hot );
        text( "Wheel: switch weapon / zoom / radio", mx + mw / 2, my + mh + 26, 16, DIM, 1 );
        text( "Mouse: look and aim", mx + mw / 2, my + mh + 48, 16, DIM, 1 );
    }

    // The commands: On Foot in two columns, then Driving, then Radio.
    std::vector<Column> cols;
    int foot_n = 0, drive_from = -1, drive_to = -1, radio_from = -1, radio_to = -1;
    for ( int i = 0; i < count(); ++i )
    {
        const std::string g = command( i ).group;
        if ( g == "On Foot" ) ++foot_n;
        if ( g == "Driving" ) { if ( drive_from < 0 ) drive_from = i; drive_to = i + 1; }
        if ( g == "Radio" ) { if ( radio_from < 0 ) radio_from = i; radio_to = i + 1; }
    }
    const int half = ( foot_n + 1 ) / 2;
    cols.push_back( { "On Foot", 0, half, PX0 + 50 } );
    cols.push_back( { "", half, foot_n, PX0 + 50 + 430 } );
    cols.push_back( { "Driving", drive_from, drive_to, PX0 + 50 + 860 } );
    cols.push_back( { "Radio", radio_from, radio_to, PX0 + 50 + 1290 } );
    COL_W = 400;
    for ( const Column &col : cols )
    {
        if ( *col.group ) text( col.group, col.x, LIST_Y - 16, 26, TEXT, 0, true );
        float ry = LIST_Y;
        for ( int i = col.from; i < col.to; ++i, ry += ROW_H )
        {
            const Command &c = command( i );
            const bool row_hot = i == hot;
            if ( row_hot ) box( col.x - 8, ry + 2, col.x + COL_W - 4, ry + ROW_H - 2, 6, { 60, 90, 160, 160 } );
            text( c.label, col.x, ry + ROW_H * 0.66f, 19, row_hot ? Rgba{ 255, 210, 140, 255 } : TEXT, 0, false, COL_W - 2 * SLOT_W - 20 );
            for ( int s = 0; s < 2; ++s )
            {
                const float sx0 = col.x + COL_W - ( 2 - s ) * ( SLOT_W + 6 ) - 4, sy0 = ry + 5, sx1 = sx0 + SLOT_W, sy1 = ry + ROW_H - 5;
                const bool capturing = g_capture_cmd == i && g_capture_slot == s;
                const bool hover = inside( g_mx, g_my, sx0, sy0, sx1, sy1 );
                box( sx0, sy0, sx1, sy1, 5, capturing ? KEY_HOT : hover ? SLOT_HOVER : SLOT, RIM, 1.5f );
                const std::string name = capturing ? "Press a key" : c.keys[s] ? key_name( c.keys[s] ) : "-";
                text( name, ( sx0 + sx1 ) / 2, sy1 - 9, 16, c.keys[s] || capturing ? TEXT : DIM, 1, false, SLOT_W - 10 );
                g_slots.push_back( { i, s, sx0, sy0, sx1, sy1 } );
            }
        }
    }

    // The mouse's settings, under Radio: Sensitivity (0.25 to 4 in quarters: a bar of 16 to click along,
    // - and +) and Mouse Look (Normal / Inverted). They apply at once (settings.h).
    {
        const Settings set = settings();
        const float x = cols.back().x, hy = LIST_Y + ( radio_to - radio_from ) * ROW_H + 56;
        text( "Mouse", x, hy, 26, TEXT, 0, true );
        auto button = [&]( int id, float x0, float y0, float x1, float y1, const char *label, bool on ) {
            const bool hover = inside( g_mx, g_my, x0, y0, x1, y1 );
            box( x0, y0, x1, y1, 5, on ? KEY_HOT : hover ? SLOT_HOVER : SLOT, RIM, 1.5f );
            text( label, ( x0 + x1 ) / 2, y1 - 9, 16, TEXT, 1, true );
            g_buttons.push_back( { id, x0, y0, x1, y1 } );
        };
        float ry = hy + 12;
        char label[48];
        snprintf( label, sizeof label, "Sensitivity %.2f", set.mouse_sensitivity );
        text( label, x, ry + ROW_H * 0.66f, 19, TEXT );
        button( 10, x + 170, ry + 5, x + 204, ry + ROW_H - 5, "-", false );
        const float bx0 = x + 212, bx1 = x + 358;
        const bool bar_hover = inside( g_mx, g_my, bx0, ry + 5, bx1, ry + ROW_H - 5 );
        box( bx0, ry + 5, bx1, ry + ROW_H - 5, 5, bar_hover ? SLOT_HOVER : SLOT, RIM, 1.5f );
        const int steps = (int)std::lround( set.mouse_sensitivity * 4 );  // 1 .. 16
        for ( int i = 1; i <= 16; ++i )
        {
            const float nx = bx0 + 6 + ( i - 1 ) * ( bx1 - bx0 - 12 ) / 15.0f;
            box( nx - 2, ry + 12, nx + 2, ry + ROW_H - 12, 1, i <= steps ? Rgba{ 255, 210, 140, 255 } : DIM );
        }
        g_buttons.push_back( { 12, bx0, ry + 5, bx1, ry + ROW_H - 5 } );
        button( 11, x + 366, ry + 5, x + 400, ry + ROW_H - 5, "+", false );
        ry += ROW_H + 4;
        text( "Mouse Look", x, ry + ROW_H * 0.66f, 19, TEXT );
        button( 13, x + 170, ry + 5, x + 282, ry + ROW_H - 5, "Normal", !set.invert_y );
        button( 14, x + 288, ry + 5, x + 400, ry + ROW_H - 5, "Inverted", set.invert_y );
    }

    // The buttons along the foot.
    const struct { int id; const char *label; float x0, w; } buttons[] = { { 1, "Reset Defaults", PX1 - 560, 260 }, { 2, "Done", PX1 - 280, 220 } };
    for ( auto &b : buttons )
    {
        const float y0 = PY1 - 62, y1 = PY1 - 16;
        const bool hover = inside( g_mx, g_my, b.x0, y0, b.x0 + b.w, y1 );
        box( b.x0, y0, b.x0 + b.w, y1, 8, hover ? SLOT_HOVER : SLOT, hover ? Rgba{ 255, 255, 255, 255 } : RIM, hover ? 3.0f : 2.0f );
        text( b.label, b.x0 + b.w / 2, y1 - 16, 22, TEXT, 1, true );
        g_buttons.push_back( { b.id, b.x0, y0, b.x0 + b.w, y1 } );
    }
    text( "Esc: back to the menu.  The pictures of the controller's buttons in the game show these keys while you play with the keyboard.", PX0 + 50, PY1 - 30, 17,
          DIM, 0, false, PX1 - 600 - PX0 - 60 );
    g_drawn_version = version();
    g_dirty = false;
}

void close_page()
{
    g_capture_cmd = -1;
    g_open = false;
    rt_log( "keys page: closed\n" );
}

void take( int input )
{
    if ( g_capture_cmd < 0 ) return;
    set_key( g_capture_cmd, g_capture_slot, input );
    g_capture_cmd = -1;
    g_dirty = true;
}
} // namespace

bool keyspage_open() { return g_open; }
void keyspage_show()
{
    std::lock_guard<std::mutex> g( g_lock );
    g_open = true, g_dirty = true, g_capture_cmd = -1;
    rt_log( "keys page: open\n" );
}

// The window's events while the page is up (coordinates in the page's 1920 x 1080). Returns false once closed.
void keyspage_mouse_move( float x, float y )
{
    std::lock_guard<std::mutex> g( g_lock );
    // Drawn again only when what is under the pointer changes (a whole picture takes some 50 ms).
    auto under = []( float px, float py ) {
        int64_t sig = 0;
        for ( const SlotRect &r : g_slots )
            if ( inside( px, py, r.x0, r.y0, r.x1, r.y1 ) ) sig = 1000000 + r.cmd * 2 + r.slot;
        if ( !sig )
            for ( const SlotRect &r : g_slots )
                if ( inside( px, py, r.x0 - 230, r.y0, r.x1, r.y1 ) ) sig = 2000000 + r.cmd;
        for ( const ButtonRect &r : g_buttons )
            if ( inside( px, py, r.x0, r.y0, r.x1, r.y1 ) ) sig = 3000000 + r.id;
        for ( const KeyRect &r : g_key_rects )
            if ( inside( px, py, r.x0, r.y0, r.x1, r.y1 ) ) sig = 4000000 + r.input;
        return sig;
    };
    if ( under( x, y ) != under( g_mx, g_my ) ) g_dirty = true;
    g_mx = x, g_my = y;
}

void keyspage_mouse_button( int sdl_button )
{
    std::lock_guard<std::mutex> g( g_lock );
    if ( g_capture_cmd >= 0 )
    {
        const int input = sdl_button == SDL_BUTTON_LEFT ? MOUSE_L : sdl_button == SDL_BUTTON_RIGHT ? MOUSE_R : sdl_button == SDL_BUTTON_MIDDLE ? MOUSE_M
                        : sdl_button == SDL_BUTTON_X1 ? MOUSE_X1 : sdl_button == SDL_BUTTON_X2 ? MOUSE_X2 : 0;
        if ( input ) take( input );
        return;
    }
    if ( sdl_button != SDL_BUTTON_LEFT ) return;
    for ( const SlotRect &s : g_slots )
        if ( inside( g_mx, g_my, s.x0, s.y0, s.x1, s.y1 ) )
        {
            g_capture_cmd = s.cmd, g_capture_slot = s.slot, g_dirty = true;
            return;
        }
    for ( const ButtonRect &b : g_buttons )
        if ( inside( g_mx, g_my, b.x0, b.y0, b.x1, b.y1 ) )
        {
            Settings s = settings();
            const float was = s.mouse_sensitivity;
            const bool inverted = s.invert_y;
            if ( b.id == 1 ) reset_defaults(), g_dirty = true, s.mouse_sensitivity = Settings{}.mouse_sensitivity, s.invert_y = Settings{}.invert_y;
            if ( b.id == 2 ) close_page();
            if ( b.id == 10 ) s.mouse_sensitivity -= 0.25f;
            if ( b.id == 11 ) s.mouse_sensitivity += 0.25f;
            if ( b.id == 12 ) s.mouse_sensitivity = 0.25f * ( 1 + std::lround( std::clamp( ( g_mx - b.x0 - 6 ) / ( b.x1 - b.x0 - 12 ), 0.0f, 1.0f ) * 15 ) );
            if ( b.id == 13 ) s.invert_y = false;
            if ( b.id == 14 ) s.invert_y = true;
            s.mouse_sensitivity = std::clamp( s.mouse_sensitivity, 0.25f, 4.0f );
            if ( s.mouse_sensitivity != was || s.invert_y != inverted ) settings_change( s ), g_dirty = true;
            return;
        }
}

void keyspage_key( int scancode )
{
    std::lock_guard<std::mutex> g( g_lock );
    if ( g_capture_cmd >= 0 )
    {
        if ( scancode == SDL_SCANCODE_ESCAPE ) g_capture_cmd = -1;
        else if ( scancode == SDL_SCANCODE_BACKSPACE || scancode == SDL_SCANCODE_DELETE ) take( 0 );
        else if ( bindable( scancode ) ) take( scancode );
        g_dirty = true;
        return;
    }
    if ( scancode == SDL_SCANCODE_ESCAPE ) close_page();
}

// The page's picture if it changed since `seq` (1920 x 1080, R, G, B, A bytes); nullptr if not.
const std::vector<uint32_t> *keyspage_picture( uint64_t &seq )
{
    std::lock_guard<std::mutex> g( g_lock );
    if ( g_drawn_version != version() ) g_dirty = true;
    static uint64_t drawn;
    if ( g_dirty )
    {
        const auto t0 = std::chrono::steady_clock::now();
        render(), ++drawn;
        static int logged;
        if ( logged++ < 3 ) rt_log( "keys page: drawn in %.1f ms\n", std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() - t0 ).count() );
    }
    if ( seq == drawn ) return nullptr;
    seq = drawn;
    return &g_px;
}

// Debug (CRACKDOWN_KEYSPAGE_DUMP=file.ppm[,x,y[,click]]): the page drawn, with the pointer there (and that place
// clicked first), to the file.
void keyspage_dump()
{
    const char *e = getenv( "CRACKDOWN_KEYSPAGE_DUMP" );
    if ( !e ) return;
    char file[512] = {};
    float x = -1, y = -1;
    int click = 0, key = 0;
    sscanf( e, "%511[^,],%f,%f,%d,%d", file, &x, &y, &click, &key );
    keyspage_show();
    keyspage_mouse_move( x, y );
    uint64_t seq = 0;
    keyspage_picture( seq );
    if ( click ) keyspage_mouse_button( SDL_BUTTON_LEFT ), keyspage_mouse_move( x, y );
    if ( key ) keyspage_key( key ), keyspage_mouse_move( x, y );  // (a scancode: the slot clicked takes it)
    const std::vector<uint32_t> *p = keyspage_picture( seq );
    if ( !p ) p = &g_px;
    if ( FILE *f = fopen( file, "wb" ) )
    {
        fprintf( f, "P6\n%d %d\n255\n", W, H );
        for ( uint32_t v : *p ) fputc( v & 255, f ), fputc( v >> 8 & 255, f ), fputc( v >> 16 & 255, f );
        fclose( f );
    }
    rt_log( "keys page: drawn to %s\n", file );
}
