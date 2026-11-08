// The game's font drawn again at the resolution's size. Its text comes from one picture,
// Local\<language>\Crackdown_Font.dds (DXT3, 1024x2048, white with the glyphs' coverage in alpha), with
// Crackdown_Font.abc saying where each glyph is: big-endian, a u16 glyph number per character from 0x10
// (65536 of them), then from 0x20014 16 bytes per glyph: 8 int16 (0, x0, y0, x1, y1 (its cell in the
// picture), left bearing, width, advance). The glyphs are Arial at 21 pixels to the em (measured: the
// picture's glyphs match Liberation Sans Regular, Arial's metric twin, at 0.96 on average).
//
// Here the picture is made k times bigger: each glyph Liberation Sans has is drawn by FreeType at 21 k
// pixels to the em into its cell (k times the size), placed where the game's glyph has its centre of
// coverage; the rest (symbols, other scripts, glyphs that don't match) are the game's glyphs enlarged
// with their edges cut at half coverage. The game lays its text out from its own table as ever: only the
// picture it samples is sharper (renderer.cpp uses it in the game's texture's place).
#include "runtime.h"
#include "fontatlas.h"
#include <zlib.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <vector>

namespace
{
std::mutex g_lock;
std::string g_dds;  // the font picture the game opened (host path)

std::vector<uint8_t> read_all( const std::string &p )
{
    std::ifstream f( p, std::ios::binary );
    return { std::istreambuf_iterator<char>( f ), {} };
}
int16_t be16( const uint8_t *p ) { return (int16_t)( p[0] << 8 | p[1] ); }

const char *font_path()
{
    if ( const char *e = getenv( "CRACKDOWN_TEXT_FONT" ) ) return e;
    // Arial itself where it is installed (the Microsoft core fonts: the game's glyphs are Arial, and
    // Liberation Sans differs in a few, its 1 with a foot), else its metric twin.
    static const char *const paths[] = { "/usr/share/fonts/TTF/arial.ttf",
                                         "/usr/share/fonts/truetype/msttcorefonts/arial.ttf",
                                         "/usr/share/fonts/truetype/msttcorefonts/Arial.ttf",
                                         "/usr/share/fonts/msttcore/arial.ttf",
                                         "/usr/share/fonts/corefonts/arial.ttf",
                                         "/usr/share/fonts/liberation/LiberationSans-Regular.ttf",
                                         "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
                                         "/usr/share/fonts/liberation-sans/LiberationSans-Regular.ttf",
                                         "/usr/share/fonts/truetype/liberation2/LiberationSans-Regular.ttf" };
    for ( const char *p : paths )
        if ( FILE *f = fopen( p, "rb" ) ) { fclose( f ); return p; }
    return nullptr;
}
} // namespace

// files.cpp: the game opened this file (its font picture, by name).
void fontatlas_seen( const std::string &host )
{
    std::lock_guard<std::mutex> l( g_lock );
    g_dds = host;
}

namespace
{
// The coverage (k times the picture, one byte a texel) made R, G, B, A texels: the colour is the game's,
// enlarged with each texel weighted by its coverage (no dark fringes from the empty texels around a glyph).
void colourise( std::vector<uint8_t> &cov, const std::vector<uint8_t> &a, const std::vector<uint32_t> &rgb, uint32_t W, uint32_t H, uint32_t k )
{
    const uint32_t KW = W * k, KH = H * k;
    std::vector<uint8_t> o( (size_t)KW * KH * 4 );
    for ( uint32_t y = 0; y < KH; ++y )
    {
        float fy = ( y + 0.5f ) / k - 0.5f;
        int y0 = (int)std::floor( fy );
        float ty = fy - y0;
        int ys[2] = { std::clamp( y0, 0, (int)H - 1 ), std::clamp( y0 + 1, 0, (int)H - 1 ) };
        for ( uint32_t x = 0; x < KW; ++x )
        {
            uint8_t *d = &o[( (size_t)y * KW + x ) * 4];
            d[3] = cov[(size_t)y * KW + x];
            d[0] = d[1] = d[2] = 255;
            if ( !d[3] ) continue;
            float fx = ( x + 0.5f ) / k - 0.5f;
            int x0 = (int)std::floor( fx );
            float tx = fx - x0;
            int xs[2] = { std::clamp( x0, 0, (int)W - 1 ), std::clamp( x0 + 1, 0, (int)W - 1 ) };
            float sum[3] = {}, wsum = 0;
            for ( int j = 0; j < 2; ++j )
                for ( int i = 0; i < 2; ++i )
                {
                    const size_t at = (size_t)ys[j] * W + xs[i];
                    const float w = ( i ? tx : 1 - tx ) * ( j ? ty : 1 - ty ) * a[at];
                    for ( int c = 0; c < 3; ++c ) sum[c] += w * ( rgb[at] >> ( 8 * c ) & 255 );
                    wsum += w;
                }
            if ( wsum > 0 )
                for ( int c = 0; c < 3; ++c ) d[c] = (uint8_t)std::lround( sum[c] / wsum );
        }
    }
    cov.swap( o );
}
} // namespace

// The controller's button pictures in the font (their characters: A, B, X, Y, START, BACK, LB, RB, LT,
// RT, the sticks' and the d-pad's) as they are in the atlas last made: cells (k times the picture) and
// their pixels, so the renderer can put a key's picture in their place and back (fontatlas_keycap).
namespace
{
std::vector<FontIcon> g_icons;
uint32_t g_icons_k;
bool is_icon( uint32_t c ) { return ( c >= 0x17F && c <= 0x181 ) || ( c >= 0x193 && c <= 0x19C ) || ( c >= 0x1A2 && c <= 0x1BF ); }
void keep_icons( const std::vector<uint8_t> &rgba, const std::vector<uint8_t> &abc, uint32_t W, uint32_t H, uint32_t k )
{
    constexpr uint32_t MAP = 0x10, RECS = 0x20014;
    const uint32_t glyphs = (uint32_t)( ( abc.size() - RECS ) / 16 ), KW = W * k;
    g_icons.clear();
    g_icons_k = k;
    for ( uint32_t c = 0x17F; c <= 0x1BF; ++c )
    {
        if ( !is_icon( c ) ) continue;
        const uint32_t g = (uint16_t)be16( &abc[MAP + 2 * c] );
        if ( g >= glyphs ) continue;
        const uint8_t *r = &abc[RECS + 16 * g];
        const int x0 = be16( r + 2 ), y0 = be16( r + 4 ), x1 = be16( r + 6 ), y1 = be16( r + 8 );
        if ( x1 <= x0 || y1 <= y0 || x0 < 0 || y0 < 0 || x1 > (int)W || y1 > (int)H ) continue;
        FontIcon i{ c, x0 * k, y0 * k, ( x1 - x0 ) * k, ( y1 - y0 ) * k, {} };
        i.rgba.resize( (size_t)i.w * i.h * 4 );
        for ( uint32_t y = 0; y < i.h; ++y ) memcpy( &i.rgba[(size_t)y * i.w * 4], &rgba[( (size_t)( i.y + y ) * KW + i.x ) * 4], (size_t)i.w * 4 );
        g_icons.push_back( std::move( i ) );
    }
}
} // namespace

const std::vector<FontIcon> &fontatlas_icons() { return g_icons; }

// A key's picture for a button's cell: a dark rounded key with a light rim and the key's name, as big as
// fits (one letter large; SPACE, ENTER smaller).
std::vector<uint8_t> fontatlas_keycap( const FontIcon &icon, const std::string &label, float scale )
{
    std::vector<uint8_t> px( (size_t)icon.w * icon.h * 4, 0 );
    const float k = scale > 0 ? scale : (float)std::max<uint32_t>( g_icons_k, 1 );
    const float inset = 1.5f * k, radius = 5.0f * k, rim = 1.6f * k;
    const float x0 = inset, y0 = inset, x1 = icon.w - inset, y1 = icon.h - inset;
    auto put = [&]( uint32_t x, uint32_t y, float r, float g, float b, float a ) {
        uint8_t *d = &px[( (size_t)y * icon.w + x ) * 4];
        const float da = d[3] / 255.0f, oa = a + da * ( 1 - a );
        if ( oa <= 0 ) return;
        for ( int c = 0; c < 3; ++c ) d[c] = (uint8_t)std::lround( ( ( c == 0 ? r : c == 1 ? g : b ) * a + d[c] * da * ( 1 - a ) ) / oa );
        d[3] = (uint8_t)std::lround( oa * 255 );
    };
    // The key: inside a rounded rectangle (distance to it, a pixel's softening at the edge).
    for ( uint32_t y = 0; y < icon.h; ++y )
        for ( uint32_t x = 0; x < icon.w; ++x )
        {
            const float px_ = x + 0.5f, py = y + 0.5f;
            const float qx = std::max( std::max( x0 + radius - px_, px_ - ( x1 - radius ) ), 0.0f ), qy = std::max( std::max( y0 + radius - py, py - ( y1 - radius ) ), 0.0f );
            const float outside = std::sqrt( qx * qx + qy * qy ) - radius;  // > 0 outside the rounded corners' reach
            const bool in_box = px_ >= x0 && px_ <= x1 && py >= y0 && py <= y1;
            if ( !in_box ) continue;
            const float edge = std::clamp( 0.5f - outside, 0.0f, 1.0f );
            if ( edge <= 0 ) continue;
            const float d_in = std::min( { px_ - x0, x1 - px_, py - y0, y1 - py } ) - ( outside > -radius ? std::max( 0.0f, radius + outside - radius ) : 0.0f );
            const bool on_rim = d_in < rim || outside > -rim;
            if ( on_rim ) put( x, y, 235, 235, 235, edge );
            else put( x, y, 34, 36, 42, edge * 0.92f );
        }
    // The name.
    const char *ttf = font_path();
    static FT_Library lib;
    static FT_Face face;
    static bool tried;
    if ( !tried )
    {
        tried = true;
        std::string bold = ttf ? std::string( ttf ) : std::string();
        if ( size_t at = bold.rfind( "Regular" ); at != std::string::npos ) bold.replace( at, 7, "Bold" );
        if ( FT_Init_FreeType( &lib ) || ( FT_New_Face( lib, bold.c_str(), 0, &face ) && ( !ttf || FT_New_Face( lib, ttf, 0, &face ) ) ) ) face = nullptr;
    }
    if ( !face || label.empty() ) return px;
    const float avail_w = ( x1 - x0 ) - 4 * k, avail_h = ( y1 - y0 ) - 6 * k;
    int size = (int)avail_h;
    auto width_at = [&]( int sz ) {
        FT_Set_Pixel_Sizes( face, 0, sz );
        long w = 0;
        for ( char ch : label )
            if ( !FT_Load_Char( face, (unsigned char)ch, FT_LOAD_DEFAULT ) ) w += face->glyph->advance.x >> 6;
        return (float)w;
    };
    while ( size > 4 && ( width_at( size ) > avail_w ) ) --size;
    const float w = width_at( size );
    const float cap = face->size->metrics.ascender / 64.0f * 0.72f;  // a capital's height, about
    float pen = ( icon.w - w ) * 0.5f;
    const float base = icon.h * 0.5f + cap * 0.5f;
    for ( char ch : label )
    {
        if ( FT_Load_Char( face, (unsigned char)ch, FT_LOAD_RENDER ) ) continue;
        const FT_GlyphSlot gs = face->glyph;
        for ( uint32_t y = 0; y < gs->bitmap.rows; ++y )
            for ( uint32_t x = 0; x < gs->bitmap.width; ++x )
            {
                const int X = (int)std::lround( pen ) + gs->bitmap_left + (int)x, Y = (int)std::lround( base ) - gs->bitmap_top + (int)y;
                if ( X < 0 || Y < 0 || X >= (int)icon.w || Y >= (int)icon.h ) continue;
                const float a = gs->bitmap.buffer[y * gs->bitmap.pitch + x] / 255.0f;
                if ( a > 0 ) put( X, Y, 255, 255, 255, a );
            }
        pen += gs->advance.x / 64.0f;
    }
    return px;
}
// The picture k times its size (1024 k x 2048 k, R, G, B, A bytes); false if it can't be made.
bool fontatlas_make( uint32_t k, std::vector<uint8_t> &out )
{
    std::string dds_path;
    {
        std::lock_guard<std::mutex> l( g_lock );
        dds_path = g_dds;
    }
    if ( dds_path.empty() || k < 2 ) return false;
    std::string abc_path = dds_path.substr( 0, dds_path.size() - 3 ) + ( dds_path.compare( dds_path.size() - 3, 3, "dds" ) ? "ABC" : "abc" );
    std::vector<uint8_t> dds = read_all( dds_path ), abc = read_all( abc_path );
    constexpr uint32_t W = 1024, H = 2048, MAP = 0x10, RECS = 0x20014;
    if ( dds.size() < 128 + W * H || abc.size() < RECS + 16 ) { rt_log( "fontatlas: %s / %s not as expected\n", dds_path.c_str(), abc_path.c_str() ); return false; }

    // The game's picture: DXT3 blocks in rows, the alpha's 4 bits per pixel, then the colour (two RGB565
    // end points and 2-bit indices). The colour is white for the letters; the controller buttons' glyphs (A,
    // B, X, Y, the bumpers, ...) are coloured pictures, kept as such.
    std::vector<uint8_t> a( (size_t)W * H );
    std::vector<uint32_t> rgb( (size_t)W * H );
    for ( uint32_t by = 0; by < H / 4; ++by )
        for ( uint32_t bx = 0; bx < W / 4; ++bx )
        {
            const uint8_t *blk = &dds[128 + ( (size_t)by * ( W / 4 ) + bx ) * 16];
            const uint32_t c0 = blk[8] | blk[9] << 8, c1 = blk[10] | blk[11] << 8;
            uint32_t pal[4][3];
            auto unpack = []( uint32_t c, uint32_t *o ) { o[0] = ( c >> 11 & 31 ) * 255 / 31, o[1] = ( c >> 5 & 63 ) * 255 / 63, o[2] = ( c & 31 ) * 255 / 31; };
            unpack( c0, pal[0] ), unpack( c1, pal[1] );
            for ( int i = 0; i < 3; ++i ) pal[2][i] = ( 2 * pal[0][i] + pal[1][i] ) / 3, pal[3][i] = ( pal[0][i] + 2 * pal[1][i] ) / 3;
            const uint32_t idx = blk[12] | blk[13] << 8 | blk[14] << 16 | (uint32_t)blk[15] << 24;
            for ( uint32_t p = 0; p < 16; ++p )
            {
                const uint32_t *c = pal[idx >> ( 2 * p ) & 3];
                rgb[(size_t)( by * 4 + p / 4 ) * W + bx * 4 + p % 4] = c[0] | c[1] << 8 | c[2] << 16;
            }
        }
    for ( uint32_t by = 0; by < H / 4; ++by )
        for ( uint32_t bx = 0; bx < W / 4; ++bx )
        {
            const uint8_t *blk = &dds[128 + ( (size_t)by * ( W / 4 ) + bx ) * 16];
            for ( uint32_t p = 0; p < 16; ++p )
            {
                uint32_t v = ( blk[p / 2] >> ( ( p & 1 ) * 4 ) ) & 15;
                a[(size_t)( by * 4 + p / 4 ) * W + bx * 4 + p % 4] = (uint8_t)( v * 17 );
            }
        }

    // Everything first as the game's glyphs enlarged, edges cut at half coverage.
    const uint32_t KW = W * k, KH = H * k;
    out.assign( (size_t)KW * KH, 0 );
    const float soft = std::clamp( 0.55f / k, 0.04f, 0.5f );
    for ( uint32_t y = 0; y < KH; ++y )
    {
        float fy = ( y + 0.5f ) / k - 0.5f;
        int y0 = (int)std::floor( fy );
        float ty = fy - y0;
        int ya = std::clamp( y0, 0, (int)H - 1 ), yb = std::clamp( y0 + 1, 0, (int)H - 1 );
        for ( uint32_t x = 0; x < KW; ++x )
        {
            float fx = ( x + 0.5f ) / k - 0.5f;
            int x0 = (int)std::floor( fx );
            float tx = fx - x0;
            int xa = std::clamp( x0, 0, (int)W - 1 ), xb = std::clamp( x0 + 1, 0, (int)W - 1 );
            float v = ( ( a[(size_t)ya * W + xa] * ( 1 - tx ) + a[(size_t)ya * W + xb] * tx ) * ( 1 - ty ) +
                        ( a[(size_t)yb * W + xa] * ( 1 - tx ) + a[(size_t)yb * W + xb] * tx ) * ty ) / 255.0f;
            float e = std::clamp( ( v - ( 0.5f - soft ) ) / ( 2 * soft ), 0.0f, 1.0f );
            out[(size_t)y * KW + x] = (uint8_t)std::lround( e * e * ( 3 - 2 * e ) * 255 );
        }
    }

    const char *ttf = font_path();
    FT_Library lib;
    FT_Face face;
    if ( !ttf || FT_Init_FreeType( &lib ) ) { rt_log( "fontatlas: no Liberation Sans: the game's glyphs enlarged\n" ); colourise( out, a, rgb, W, H, k ); return true; }
    if ( FT_New_Face( lib, ttf, 0, &face ) ) { FT_Done_FreeType( lib ); rt_log( "fontatlas: %s won't open\n", ttf ); colourise( out, a, rgb, W, H, k ); return true; }
    FT_Set_Pixel_Sizes( face, 0, 21 * k );

    // The first character naming each glyph.
    const uint32_t glyphs = (uint32_t)( ( abc.size() - RECS ) / 16 );
    std::vector<uint32_t> char_of( glyphs, 0 );
    std::vector<bool> named( glyphs, false );
    for ( uint32_t c = 1; c < 65536; ++c )
    {
        uint32_t g = (uint16_t)be16( &abc[MAP + 2 * c] );
        if ( g < glyphs && !named[g] ) named[g] = true, char_of[g] = c;
    }
    uint32_t drawn = 0, kept = 0;
    std::vector<double> baselines;
    double baseline = -1e9;
    for ( int baseline_pass = 1; baseline_pass >= 0; --baseline_pass )
    {
    if ( !baseline_pass && !baselines.empty() )
    {
        std::sort( baselines.begin(), baselines.end() );
        baseline = baselines[baselines.size() / 2];
    }
    drawn = kept = 0;
    static const double MATCH = getenv( "CRACKDOWN_FONTATLAS_MATCH" ) ? atof( getenv( "CRACKDOWN_FONTATLAS_MATCH" ) ) : 0.9;
    std::vector<float> cell, mine;
    for ( uint32_t g = 0; g < glyphs; ++g )
    {
        const uint8_t *r = &abc[RECS + 16 * g];
        int x0 = be16( r + 2 ), y0 = be16( r + 4 ), x1 = be16( r + 6 ), y1 = be16( r + 8 );
        if ( !named[g] || x1 <= x0 || y1 <= y0 || x0 < 0 || y0 < 0 || x1 > (int)W || y1 > (int)H ) continue;
        const int cw = x1 - x0, ch = y1 - y0;
        // The game's glyph: its coverage and centre.
        double m0 = 0, cx = 0, cy = 0;
        cell.assign( (size_t)cw * ch, 0 );
        for ( int y = 0; y < ch; ++y )
            for ( int x = 0; x < cw; ++x )
            {
                float v = a[(size_t)( y0 + y ) * W + x0 + x] / 255.0f;
                cell[(size_t)y * cw + x] = v;
                m0 += v, cx += v * ( x + 0.5 ), cy += v * ( y + 0.5 );
            }
        if ( m0 < 0.5 ) continue;  // nothing drawn (a space)
        cx /= m0, cy /= m0;
        FT_UInt index = FT_Get_Char_Index( face, char_of[g] );
        if ( !index || FT_Load_Glyph( face, index, FT_LOAD_NO_HINTING | FT_LOAD_RENDER ) ) { ++kept; continue; }
        const FT_Bitmap &bm = face->glyph->bitmap;
        double m1 = 0, bx = 0, by = 0;
        for ( uint32_t y = 0; y < bm.rows; ++y )
            for ( uint32_t x = 0; x < bm.width; ++x )
            {
                float v = bm.buffer[y * bm.pitch + x] / 255.0f;
                m1 += v, bx += v * ( x + 0.5 ), by += v * ( y + 0.5 );
            }
        if ( m1 <= 0 ) { ++kept; continue; }
        bx /= m1, by /= m1;
        const double ratio = m1 / ( (double)k * k ) / m0;
        // Placed with its centre across on the game's glyph's, and on the picture's baseline (the median of
        // the letters' own, found in a first pass), in the cell k times bigger.
        const double base_here = ( y0 + cy ) * k - by + face->glyph->bitmap_top;  // this glyph's baseline, centred
        if ( baseline_pass )
        {
            if ( char_of[g] >= 'A' && char_of[g] <= 'z' && std::isalpha( (int)char_of[g] ) ) baselines.push_back( base_here - y0 * k );
            continue;
        }
        const int px = (int)std::lround( ( x0 + cx ) * k - bx );
        const int py = baseline > -1e8 && char_of[g] < 0x2000 ? (int)std::lround( y0 * k + baseline - face->glyph->bitmap_top ) : (int)std::lround( ( y0 + cy ) * k - by );
        // The same at the game's size, compared with its glyph: a different picture (a symbol in a letter's
        // place, another shape) keeps the game's.
        mine.assign( (size_t)cw * ch, 0 );
        for ( uint32_t y = 0; y < bm.rows; ++y )
            for ( uint32_t x = 0; x < bm.width; ++x )
            {
                int X = px + (int)x - x0 * (int)k, Y = py + (int)y - y0 * (int)k;
                if ( X < 0 || Y < 0 || X >= cw * (int)k || Y >= ch * (int)k ) continue;
                mine[(size_t)( Y / k ) * cw + X / k] += bm.buffer[y * bm.pitch + x] / 255.0f / ( k * k );
            }
        // Compared a little blurred (3x3), so the game's pixel-snapped strokes still match their outlines.
        auto blur = [cw, ch]( const std::vector<float> &v ) {
            std::vector<float> o( v.size(), 0 );
            for ( int y = 0; y < ch; ++y )
                for ( int x = 0; x < cw; ++x )
                    for ( int dy = -1; dy <= 1; ++dy )
                        for ( int dx = -1; dx <= 1; ++dx )
                            if ( x + dx >= 0 && x + dx < cw && y + dy >= 0 && y + dy < ch ) o[(size_t)y * cw + x] += v[(size_t)( y + dy ) * cw + x + dx];
            return o;
        };
        const std::vector<float> bc = blur( cell ), bmine = blur( mine );
        double dot = 0, n0 = 0, n1 = 0;
        for ( size_t i = 0; i < bc.size(); ++i ) dot += bc[i] * bmine[i], n0 += bc[i] * bc[i], n1 += bmine[i] * bmine[i];
        const double match = dot / std::sqrt( n0 * n1 + 1e-12 );
        static const bool debug = getenv( "CRACKDOWN_FONTATLAS_LOG" );
        if ( debug && char_of[g] < 0x500 ) rt_log( "fontatlas: U+%04X match %.3f ratio %.2f\n", char_of[g], match, ratio );
        // Printable ASCII's glyphs are where the table says (each checked); elsewhere the table and the
        // picture can disagree (U+00D1's cell holds an O with a grave), so only a close match is drawn.
        const bool ascii = char_of[g] >= 0x21 && char_of[g] <= 0x7E;
        if ( match < ( ascii ? 0.6 : MATCH ) || ratio < 0.6 || ratio > 1.6 ) { ++kept; continue; }
        for ( int y = y0 * (int)k; y < y1 * (int)k; ++y ) memset( &out[(size_t)y * KW + x0 * k], 0, (size_t)cw * k );
        for ( uint32_t y = 0; y < bm.rows; ++y )
            for ( uint32_t x = 0; x < bm.width; ++x )
            {
                int X = px + (int)x, Y = py + (int)y;
                if ( X < x0 * (int)k || Y < y0 * (int)k || X >= x1 * (int)k || Y >= y1 * (int)k ) continue;
                out[(size_t)Y * KW + X] = bm.buffer[y * bm.pitch + x];
            }
        ++drawn;
    }
    }
    FT_Done_Face( face );
    FT_Done_FreeType( lib );
    rt_log( "fontatlas: %s at %ux: %u glyphs drawn from %s, %u kept as the game's\n", dds_path.c_str(), k, drawn, ttf, kept );
    colourise( out, a, rgb, W, H, k );
    keep_icons( out, abc, W, H, k );
    return true;
}

// A key's picture as a PNG (size x size, R, G, B, A), for the menus' button pictures (xui_options.cpp).
std::vector<uint8_t> keycap_png( const std::string &label, uint32_t size )
{
    FontIcon cell{ 0, 0, 0, size, size, {} };
    const std::vector<uint8_t> px = fontatlas_keycap( cell, label, size / 28.0f );
    std::vector<uint8_t> raw;
    raw.reserve( ( size * 4 + 1 ) * size );
    for ( uint32_t y = 0; y < size; ++y )
    {
        raw.push_back( 0 );  // no filter
        raw.insert( raw.end(), px.begin() + (size_t)y * size * 4, px.begin() + (size_t)( y + 1 ) * size * 4 );
    }
    uLongf zlen = compressBound( raw.size() );
    std::vector<uint8_t> z( zlen );
    if ( compress2( z.data(), &zlen, raw.data(), raw.size(), 9 ) != Z_OK ) return {};
    z.resize( zlen );
    std::vector<uint8_t> png = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    auto chunk = [&]( const char *type, const std::vector<uint8_t> &data ) {
        const uint32_t n = (uint32_t)data.size();
        for ( int s = 24; s >= 0; s -= 8 ) png.push_back( (uint8_t)( n >> s ) );
        const size_t at = png.size();
        png.insert( png.end(), type, type + 4 );
        png.insert( png.end(), data.begin(), data.end() );
        const uint32_t crc = (uint32_t)crc32( 0, png.data() + at, (uInt)( png.size() - at ) );
        for ( int s = 24; s >= 0; s -= 8 ) png.push_back( (uint8_t)( crc >> s ) );
    };
    std::vector<uint8_t> ihdr = { (uint8_t)( size >> 24 ), (uint8_t)( size >> 16 ), (uint8_t)( size >> 8 ), (uint8_t)size,
                                  (uint8_t)( size >> 24 ), (uint8_t)( size >> 16 ), (uint8_t)( size >> 8 ), (uint8_t)size, 8, 6, 0, 0, 0 };
    chunk( "IHDR", ihdr );
    chunk( "IDAT", z );
    chunk( "IEND", {} );
    return png;
}
