// The mouse pointer: the Agency's emblem (the game's own picture, xui-assets-3\Agency-logo-big.png in the
// disc's XUIFrontEnd\Scene.xzp, made into a pointer here, never shipped with the port), chrome with a dark
// edge and an Agency-blue glow, pointing with the tip of its top spike. It is the window's cursor, so the
// system draws it (no lag behind the mouse); in game the mouse looks around and it is hidden as before.
// Over something the pointer can press (xui.cpp's g_pointer_el) its glow brightens. Sized for the display:
// 64 pixels at 2160 lines (CRACKDOWN_CURSOR_SIZE for another). Without the picture the system's own
// pointer stays.
// CRACKDOWN_CURSOR_DUMP=<file.ppm> (debug): the two pointers side by side on a menu blue.
#include "runtime.h"
#include "xur.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
extern "C" {
#include <libavcodec/avcodec.h>
}

std::string files_disc_root();  // files.cpp
bool xui_pointer_on_control();  // xui.cpp

namespace
{
SDL_Cursor *g_normal, *g_hover;
bool g_hovering;

// A PNG as R, G, B, A rows.
bool decode_png( const std::vector<uint8_t> &png, std::vector<uint8_t> &rgba, int &w, int &h )
{
    const AVCodec *codec = avcodec_find_decoder( AV_CODEC_ID_PNG );
    AVCodecContext *cx = codec ? avcodec_alloc_context3( codec ) : nullptr;
    AVPacket *pkt = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    std::vector<uint8_t> bytes = png;
    bytes.resize( bytes.size() + AV_INPUT_BUFFER_PADDING_SIZE );
    bool ok = false;
    if ( cx && pkt && frame && avcodec_open2( cx, codec, nullptr ) == 0 )
    {
        pkt->data = bytes.data(), pkt->size = (int)png.size();
        // 8 or 16 bits a channel (the emblem is 16), with or without alpha, or a palette.
        const int f = avcodec_send_packet( cx, pkt ) == 0 && avcodec_receive_frame( cx, frame ) == 0 ? frame->format : -1;
        const int bytes_per = f == AV_PIX_FMT_RGBA ? 4 : f == AV_PIX_FMT_RGB24 ? 3 : f == AV_PIX_FMT_RGBA64BE ? 8 : f == AV_PIX_FMT_RGB48BE ? 6 : f == AV_PIX_FMT_PAL8 ? 1 : 0;
        if ( bytes_per )
        {
            w = frame->width, h = frame->height;
            rgba.resize( (size_t)w * h * 4 );
            const bool wide = bytes_per >= 6, alpha = bytes_per == 4 || bytes_per == 8;
            for ( int y = 0; y < h; ++y )
                for ( int x = 0; x < w; ++x )
                {
                    const uint8_t *src = frame->data[0] + y * frame->linesize[0] + x * bytes_per;
                    uint8_t *d = &rgba[( (size_t)y * w + x ) * 4];
                    if ( f == AV_PIX_FMT_PAL8 )
                    {
                        uint32_t c;
                        memcpy( &c, frame->data[1] + src[0] * 4, 4 );
                        d[0] = c >> 16, d[1] = c >> 8, d[2] = c, d[3] = c >> 24;
                        continue;
                    }
                    for ( int c = 0; c < 4; ++c ) d[c] = c < 3 || alpha ? src[wide ? c * 2 : c] : 255;  // (16 bits: the high byte, first)
                }
            ok = true;
        }
    }
    pkt->data = nullptr, pkt->size = 0;
    av_frame_free( &frame );
    av_packet_free( &pkt );
    avcodec_free_context( &cx );
    return ok;
}

// The emblem from the disc's menus, or nothing.
bool emblem( std::vector<uint8_t> &rgba, int &w, int &h )
{
    const std::string path = files_disc_root() + "/XUIFrontEnd/Scene.xzp";
    std::ifstream f( path, std::ios::binary );
    if ( !f ) { rt_log( "cursor: can't open %s\n", path.c_str() ); return false; }
    const std::vector<uint8_t> a{ std::istreambuf_iterator<char>( f ), {} };
    std::vector<XzpEntry> list;
    uint32_t start;
    if ( !xzp_list( a, list, start ) ) return false;
    for ( const XzpEntry &e : list )
        if ( e.name == u"xui-assets-3\\Agency-logo-big.png" && (size_t)start + e.offset + e.size <= a.size() )
        {
            if ( decode_png( std::vector<uint8_t>( a.begin() + start + e.offset, a.begin() + start + e.offset + e.size ), rgba, w, h ) ) return true;
            rt_log( "cursor: the emblem's picture didn't decode\n" );
            return false;
        }
    rt_log( "cursor: no emblem among %zu pictures in %s\n", list.size(), path.c_str() );
    return false;
}

// Premultiplied float pixels, S x S.
struct Image
{
    int s;
    std::vector<float> px;  // r, g, b, a
    explicit Image( int size ) : s( size ), px( (size_t)size * size * 4, 0.0f ) {}
    float *at( int x, int y ) { return &px[( (size_t)y * s + x ) * 4]; }
};

// One channel's dilation by a disc of radius r, then a box blur of radius b (three passes, about a Gaussian).
std::vector<float> spread( const std::vector<float> &mask, int s, float r, float b )
{
    std::vector<float> d( mask.size(), 0.0f );
    const int ri = (int)std::ceil( r );
    for ( int y = 0; y < s; ++y )
        for ( int x = 0; x < s; ++x )
        {
            float m = 0;
            for ( int dy = -ri; dy <= ri; ++dy )
                for ( int dx = -ri; dx <= ri; ++dx )
                {
                    const int X = x + dx, Y = y + dy;
                    if ( X < 0 || Y < 0 || X >= s || Y >= s || dx * dx + dy * dy > r * r + 0.5f ) continue;
                    m = std::max( m, mask[(size_t)Y * s + X] );
                }
            d[(size_t)y * s + x] = m;
        }
    const int bi = (int)std::lround( b );
    for ( int pass = 0; pass < 3 && bi > 0; ++pass )
        for ( int axis = 0; axis < 2; ++axis )
        {
            std::vector<float> t( d.size(), 0.0f );
            for ( int y = 0; y < s; ++y )
                for ( int x = 0; x < s; ++x )
                {
                    float sum = 0;
                    int n = 0;
                    for ( int k = -bi; k <= bi; ++k )
                    {
                        const int X = axis ? x : x + k, Y = axis ? y + k : y;
                        if ( X >= 0 && Y >= 0 && X < s && Y < s ) sum += d[(size_t)Y * s + X];
                        ++n;
                    }
                    t[(size_t)y * s + x] = sum / n;
                }
            d.swap( t );
        }
    return d;
}

// The pointer at size s: the emblem scaled into 7/8 of it, with its edge and glow under it; hot: the
// top spike's tip.
std::vector<uint8_t> make_pointer( const std::vector<uint8_t> &src, int w, int h, int s, bool hover, int &hot_x, int &hot_y )
{
    // The emblem's opaque box.
    int x0 = w, y0 = h, x1 = -1, y1 = -1;
    for ( int y = 0; y < h; ++y )
        for ( int x = 0; x < w; ++x )
            if ( src[( (size_t)y * w + x ) * 4 + 3] > 8 ) x0 = std::min( x0, x ), x1 = std::max( x1, x ), y0 = std::min( y0, y ), y1 = std::max( y1, y );
    if ( x1 < x0 ) return {};
    const int bw = x1 - x0 + 1, bh = y1 - y0 + 1;
    const float fit = s * 0.875f, scale = fit / std::max( bw, bh );
    const int dw = std::max( 1, (int)std::lround( bw * scale ) ), dh = std::max( 1, (int)std::lround( bh * scale ) );
    const int ox = ( s - dw ) / 2, oy = ( s - dh ) / 2;
    Image img( s );
    std::vector<float> mask( (size_t)s * s, 0.0f );
    // Box-filtered (each pointer pixel the average of the emblem's pixels it covers), premultiplied.
    for ( int y = 0; y < dh; ++y )
        for ( int x = 0; x < dw; ++x )
        {
            const float sx0 = x0 + x / scale, sx1 = x0 + ( x + 1 ) / scale, sy0 = y0 + y / scale, sy1 = y0 + ( y + 1 ) / scale;
            float acc[4] = {}, n = 0;
            for ( int sy = (int)sy0; sy < (int)std::ceil( sy1 ) && sy <= y1; ++sy )
                for ( int sx = (int)sx0; sx < (int)std::ceil( sx1 ) && sx <= x1; ++sx )
                {
                    const uint8_t *p = &src[( (size_t)sy * w + sx ) * 4];
                    const float a = p[3] / 255.0f;
                    acc[0] += p[0] / 255.0f * a, acc[1] += p[1] / 255.0f * a, acc[2] += p[2] / 255.0f * a, acc[3] += a, n += 1;
                }
            if ( n == 0 ) continue;
            float *d = img.at( ox + x, oy + y );
            for ( int c = 0; c < 4; ++c ) d[c] = acc[c] / n;
            mask[(size_t)( oy + y ) * s + ox + x] = d[3];
        }
    // Under it: a dark edge, then the glow (both scale with the pointer: 64 pixels the reference).
    const float k = s / 64.0f;
    const std::vector<float> edge = spread( mask, s, 1.5f * k, 0 ), glow = spread( mask, s, 3.0f * k, 2.5f * k );
    const float glow_rgb[3] = { hover ? 0.55f : 0.23f, hover ? 0.80f : 0.63f, 1.0f }, glow_gain = hover ? 1.6f : 1.0f;
    const float edge_rgb[3] = { 0x0a / 255.0f, 0x1a / 255.0f, 0x33 / 255.0f };
    std::vector<uint8_t> out( (size_t)s * s * 4 );
    for ( int i = 0; i < s * s; ++i )
    {
        float *p = &img.px[(size_t)i * 4];
        float r = p[0], g = p[1], b = p[2], a = p[3];
        const float ea = std::min( 1.0f, edge[i] );  // the edge under the emblem
        r += edge_rgb[0] * ea * ( 1 - a ), g += edge_rgb[1] * ea * ( 1 - a ), b += edge_rgb[2] * ea * ( 1 - a ), a += ea * ( 1 - a );
        const float ga = std::min( 1.0f, glow[i] * glow_gain );  // the glow under both
        r += glow_rgb[0] * ga * ( 1 - a ), g += glow_rgb[1] * ga * ( 1 - a ), b += glow_rgb[2] * ga * ( 1 - a ), a += ga * ( 1 - a );
        uint8_t *o = &out[(size_t)i * 4];
        const float inv = a > 0 ? 1.0f / a : 0;
        o[0] = (uint8_t)std::lround( std::clamp( r * inv, 0.0f, 1.0f ) * 255 ), o[1] = (uint8_t)std::lround( std::clamp( g * inv, 0.0f, 1.0f ) * 255 );
        o[2] = (uint8_t)std::lround( std::clamp( b * inv, 0.0f, 1.0f ) * 255 ), o[3] = (uint8_t)std::lround( std::clamp( a, 0.0f, 1.0f ) * 255 );
    }
    // The tip: the topmost row the emblem covers (by half), at its middle.
    hot_x = s / 2, hot_y = oy;
    for ( int y = 0; y < s; ++y )
    {
        int first = -1, last = -1;
        for ( int x = 0; x < s; ++x )
            if ( mask[(size_t)y * s + x] > 0.5f ) last = x, first = first < 0 ? x : first;
        if ( first >= 0 ) { hot_x = ( first + last ) / 2, hot_y = y; break; }
    }
    return out;
}

SDL_Cursor *make_cursor( std::vector<uint8_t> &px, int s, int hx, int hy )
{
    SDL_Surface *surface = SDL_CreateSurfaceFrom( s, s, SDL_PIXELFORMAT_RGBA32, px.data(), s * 4 );
    if ( !surface ) return nullptr;
    SDL_Cursor *c = SDL_CreateColorCursor( surface, hx, hy );
    SDL_DestroySurface( surface );
    return c;
}
} // namespace

// After the window is made: the pointers, made from the disc's emblem (display_h: the display's lines).
void cursor_init( int display_h )
{
    std::vector<uint8_t> src;
    int w = 0, h = 0;
    if ( !emblem( src, w, h ) ) { rt_log( "cursor: no Agency emblem in the disc's menus: the system pointer\n" ); return; }
    // CRACKDOWN_CURSOR_SIZE=<pixels>: another size.
    const char *size_env = getenv( "CRACKDOWN_CURSOR_SIZE" );
    const int s = std::clamp( size_env ? atoi( size_env ) : (int)std::lround( 64.0 * display_h / 2160 ), 16, 256 );
    int hx, hy, hx2, hy2;
    std::vector<uint8_t> normal = make_pointer( src, w, h, s, false, hx, hy ), hover = make_pointer( src, w, h, s, true, hx2, hy2 );
    if ( normal.empty() || hover.empty() ) return;
    if ( const char *dump = getenv( "CRACKDOWN_CURSOR_DUMP" ) )
        if ( FILE *f = fopen( dump, "wb" ) )
        {
            fprintf( f, "P6\n%d %d\n255\n", s * 2, s );
            for ( int y = 0; y < s; ++y )
                for ( int x = 0; x < s * 2; ++x )
                {
                    const uint8_t *p = &( x < s ? normal : hover )[( (size_t)y * s + x % s ) * 4];
                    const float a = p[3] / 255.0f, bg[3] = { 0.13f, 0.27f, 0.62f };  // a menu blue
                    for ( int c = 0; c < 3; ++c ) fputc( (int)std::lround( p[c] * a + bg[c] * 255 * ( 1 - a ) ), f );
                }
            fclose( f );
        }
    g_normal = make_cursor( normal, s, hx, hy );
    g_hover = make_cursor( hover, s, hx2, hy2 );
    if ( g_normal ) SDL_SetCursor( g_normal );
    rt_log( "cursor: the Agency emblem, %d pixels, tip at %d,%d%s\n", s, hx, hy, g_normal ? "" : " (not made)" );
}

// Each pass of the window's loop: the hover look over something the pointer can press.
void cursor_update()
{
    if ( !g_normal || !g_hover ) return;
    const bool on = xui_pointer_on_control();
    if ( on == g_hovering ) return;
    g_hovering = on;
    SDL_SetCursor( on ? g_hover : g_normal );
}
