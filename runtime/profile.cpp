// The player's profile (profile.h).
#include "profile.h"
#include "runtime.h"
#include "settings.h"
#include <ft2build.h>
#include FT_FREETYPE_H
extern "C" {
#include <libavcodec/avcodec.h>
}
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <ctime>

namespace fs = std::filesystem;

namespace
{
std::mutex g_lock;

fs::path data_dir()
{
    const char *xdg = getenv( "XDG_DATA_HOME" ), *home = getenv( "HOME" );
    fs::path base = xdg && *xdg ? fs::path( xdg ) : fs::path( home ? home : "." ) / ".local/share";
    return base / "crackdown-recomp";
}

// ------------------------------------------------------------------ the game's achievements (XDBF)
// The executable's XDBF resource (in the image): a header { 'XDBF', version, entry table length, entry
// count, free table length, free count }, entries { u16 namespace; u64 id; u32 offset; u32 length },
// free entries (8 bytes), then the data. Namespace 1 holds XACH (the achievements: { u16 id, label,
// description, unachieved; u32 image; u16 points; u16; u32 flags; 16 bytes }, 0x24 each after a 14-byte
// header), 2 the pictures (PNG, by image id), 3 the string tables by language (XSTR: { u16 id, length;
// UTF-8 }), English being 1.
struct Xdbf
{
    bool tried = false;
    uint32_t data = 0;  // guest address of the data
    std::map<uint64_t, std::pair<uint32_t, uint32_t>> entries[4];  // per namespace: id -> offset, length
    std::vector<Achievement> list;
} g_xdbf;

uint16_t r16( uint32_t a ) { return guest_read16( a ); }
uint32_t r32( uint32_t a ) { return guest_read32( a ); }

void load_xdbf()
{
    if ( g_xdbf.tried ) return;
    g_xdbf.tried = true;
    uint32_t at = 0;
    for ( uint32_t a = 0x82000000; a < 0x84000000 && !at; a += 0x1000 )
        if ( r32( a ) == 0x58444246 && r32( a + 4 ) == 0x10000 ) at = a;  // 'XDBF', version 1.0
    if ( !at ) { rt_log( "profile: no achievements in the game's image\n" ); return; }
    const uint32_t etl = r32( at + 8 ), count = r32( at + 12 ), ftl = r32( at + 16 );
    g_xdbf.data = at + 24 + etl * 18 + ftl * 8;
    for ( uint32_t i = 0; i < count; ++i )
    {
        const uint32_t e = at + 24 + i * 18, ns = r16( e );
        if ( ns >= 1 && ns <= 3 ) g_xdbf.entries[ns][guest_read64( e + 2 )] = { r32( e + 10 ), r32( e + 14 ) };
    }
    std::map<uint16_t, std::string> strings;
    if ( auto s = g_xdbf.entries[3].find( 1 ); s != g_xdbf.entries[3].end() )
    {
        uint32_t p = g_xdbf.data + s->second.first;
        const uint32_t n = r16( p + 12 );
        p += 14;
        for ( uint32_t i = 0; i < n; ++i )
        {
            const uint16_t id = r16( p ), len = r16( p + 2 );
            strings[id].assign( (const char *)g_base + p + 4, len );
            p += 4 + len;
        }
    }
    if ( auto x = g_xdbf.entries[1].find( 0x58414348 ); x != g_xdbf.entries[1].end() )  // 'XACH'
    {
        uint32_t p = g_xdbf.data + x->second.first;
        const uint32_t n = r16( p + 12 );
        p += 14;
        for ( uint32_t i = 0; i < n; ++i, p += 0x24 )
            g_xdbf.list.push_back( { r16( p ), r32( p + 8 ), r16( p + 12 ), r32( p + 16 ), strings[r16( p + 2 )], strings[r16( p + 4 )], strings[r16( p + 6 )] } );
    }
    uint32_t points = 0;
    for ( auto &a : g_xdbf.list ) points += a.points;
    rt_log( "profile: the game has %zu achievements (%u points)\n", g_xdbf.list.size(), points );
}

// An achievement's picture (PNG in the XDBF) as R, G, B, A pixels.
bool icon( uint32_t image, std::vector<uint8_t> &rgba, int &w, int &h )
{
    auto it = g_xdbf.entries[2].find( image );
    if ( it == g_xdbf.entries[2].end() ) return false;
    const AVCodec *codec = avcodec_find_decoder( AV_CODEC_ID_PNG );
    AVCodecContext *cx = codec ? avcodec_alloc_context3( codec ) : nullptr;
    AVPacket *pkt = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    bool ok = false;
    std::vector<uint8_t> bytes( g_base + g_xdbf.data + it->second.first, g_base + g_xdbf.data + it->second.first + it->second.second );
    bytes.resize( bytes.size() + AV_INPUT_BUFFER_PADDING_SIZE );
    if ( cx && pkt && frame && avcodec_open2( cx, codec, nullptr ) == 0 )
    {
        pkt->data = bytes.data(), pkt->size = (int)it->second.second;
        if ( avcodec_send_packet( cx, pkt ) == 0 && avcodec_receive_frame( cx, frame ) == 0 &&
             ( frame->format == AV_PIX_FMT_RGBA || frame->format == AV_PIX_FMT_RGB24 || frame->format == AV_PIX_FMT_PAL8 ) )
        {
            w = frame->width, h = frame->height;
            rgba.resize( (size_t)w * h * 4 );
            const bool pal = frame->format == AV_PIX_FMT_PAL8;  // (the achievements' pictures: a palette of A, R, G, B words)
            const int bpp = pal ? 1 : frame->format == AV_PIX_FMT_RGBA ? 4 : 3;
            for ( int y = 0; y < h; ++y )
                for ( int x = 0; x < w; ++x )
                {
                    const uint8_t *s = frame->data[0] + y * frame->linesize[0] + x * bpp;
                    uint8_t *d = &rgba[( (size_t)y * w + x ) * 4];
                    if ( pal )
                    {
                        uint32_t c;
                        memcpy( &c, frame->data[1] + s[0] * 4, 4 );
                        d[0] = c >> 16, d[1] = c >> 8, d[2] = c, d[3] = c >> 24;
                    }
                    else d[0] = s[0], d[1] = s[1], d[2] = s[2], d[3] = bpp == 4 ? s[3] : 255;
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

// ------------------------------------------------------------------ the profile's files
// achievements.txt: "<id> <unix time>" per unlocked achievement; settings.bin: per setting { u32 id;
// u8 type; u32 length; value }.
struct Store
{
    bool loaded = false;
    std::map<uint32_t, int64_t> unlocked;
    std::map<uint32_t, std::pair<uint8_t, std::vector<uint8_t>>> settings;
} g_store;

void save_summary();
void write_file( const fs::path &path, const std::string &bytes );
void load_store()
{
    if ( g_store.loaded ) return;
    g_store.loaded = true;
    std::error_code ec;
    fs::create_directories( profile_dir(), ec );
    if ( FILE *f = fopen( ( profile_dir() / "achievements.txt" ).c_str(), "r" ) )
    {
        unsigned id;
        long long t;
        while ( fscanf( f, "%u %lld", &id, &t ) == 2 ) g_store.unlocked[id] = t;
        fclose( f );
    }
    if ( FILE *f = fopen( ( profile_dir() / "settings.bin" ).c_str(), "rb" ) )
    {
        uint32_t id, len;
        uint8_t type;
        while ( fread( &id, 4, 1, f ) == 1 && fread( &type, 1, 1, f ) == 1 && fread( &len, 4, 1, f ) == 1 && len <= 0x10000 )
        {
            std::vector<uint8_t> v( len );
            if ( len && fread( v.data(), 1, len, f ) != len ) break;
            g_store.settings[id] = { type, std::move( v ) };
        }
        fclose( f );
    }
    rt_log( "profile: %s (%s), %zu achievements unlocked\n", profile_name().c_str(), profile_dir().c_str(), g_store.unlocked.size() );
    save_summary();
}

void write_file( const fs::path &path, const std::string &bytes )
{
    const fs::path tmp = path.string() + ".new";
    if ( FILE *f = fopen( tmp.c_str(), "wb" ) )
    {
        bool ok = fwrite( bytes.data(), 1, bytes.size(), f ) == bytes.size();
        ok = fclose( f ) == 0 && ok;
        std::error_code ec;
        if ( ok ) fs::rename( tmp, path, ec );
    }
}

// summary.ini, for the launcher: the achievements unlocked and their points, of the game's all, and when
// the profile last played (unix time).
void save_summary()
{
    load_xdbf();
    uint32_t points = 0, all_points = 0;
    for ( const Achievement &a : g_xdbf.list )
    {
        all_points += a.points;
        if ( g_store.unlocked.count( a.id ) ) points += a.points;
    }
    std::string s = "achievements = " + std::to_string( g_store.unlocked.size() ) + "\n" + "achievements_all = " + std::to_string( g_xdbf.list.size() ) + "\n" +
                    "points = " + std::to_string( points ) + "\n" + "points_all = " + std::to_string( all_points ) + "\n" +
                    "last_played = " + std::to_string( (long long)time( nullptr ) ) + "\n";
    write_file( profile_dir() / "summary.ini", s );
}

void save_achievements()
{
    std::string s;
    for ( auto &[id, t] : g_store.unlocked ) s += std::to_string( id ) + " " + std::to_string( t ) + "\n";
    write_file( profile_dir() / "achievements.txt", s );
    save_summary();
}

void save_settings()
{
    std::string s;
    for ( auto &[id, v] : g_store.settings )
    {
        const uint32_t len = (uint32_t)v.second.size();
        s.append( (const char *)&id, 4 ), s.push_back( (char)v.first ), s.append( (const char *)&len, 4 );
        s.append( (const char *)v.second.data(), len );
    }
    write_file( profile_dir() / "settings.bin", s );
}

// ------------------------------------------------------------------ the note on screen
// Drawn in software like the Keyboard Controls page, in the game's menu colours.
constexpr int TW = 620, TH = 112;
struct Toast { uint32_t id; };
std::deque<Toast> g_queue;
std::vector<uint32_t> g_px;
uint64_t g_seq;
std::chrono::steady_clock::time_point g_shown;
bool g_showing;

FT_Face font( bool bold )
{
    static FT_Library lib;
    static FT_Face regular, heavy;
    static bool tried;
    if ( !tried )
    {
        tried = true;
        static const char *const dirs[] = { "/usr/share/fonts/liberation/", "/usr/share/fonts/truetype/liberation/", "/usr/share/fonts/liberation-sans/",
                                            "/usr/share/fonts/truetype/liberation2/" };
        if ( !FT_Init_FreeType( &lib ) )
            for ( const char *d : dirs )
            {
                if ( !regular ) FT_New_Face( lib, ( std::string( d ) + "LiberationSans-Regular.ttf" ).c_str(), 0, &regular );
                if ( !heavy ) FT_New_Face( lib, ( std::string( d ) + "LiberationSans-Bold.ttf" ).c_str(), 0, &heavy );
            }
        if ( !heavy ) heavy = regular;
    }
    return bold ? heavy : regular;
}

void blend( int x, int y, const uint8_t c[4], float cover )
{
    if ( x < 0 || y < 0 || x >= TW || y >= TH ) return;
    const float a = c[3] / 255.0f * cover;
    if ( a <= 0 ) return;
    uint8_t *d = (uint8_t *)&g_px[(size_t)y * TW + x];
    const float da = d[3] / 255.0f, oa = a + da * ( 1 - a );
    for ( int i = 0; i < 3; ++i ) d[i] = (uint8_t)std::lround( ( c[i] * a + d[i] * da * ( 1 - a ) ) / oa );
    d[3] = (uint8_t)std::lround( oa * 255 );
}

void text( const std::string &s, float x, float y, int size, const uint8_t c[4], bool bold, float max_w )
{
    FT_Face f = font( bold );
    if ( !f ) return;
    auto width = [&]( int px ) {
        FT_Set_Pixel_Sizes( f, 0, px );
        float w = 0;
        for ( unsigned char ch : s )
            if ( !FT_Load_Char( f, ch, FT_LOAD_DEFAULT ) ) w += f->glyph->advance.x / 64.0f;
        return w;
    };
    while ( size > 10 && width( size ) > max_w ) --size;
    FT_Set_Pixel_Sizes( f, 0, size );
    float pen = x;
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

void draw( const Achievement &a )
{
    g_px.assign( (size_t)TW * TH, 0 );
    static const uint8_t PANEL[4] = { 14, 30, 78, 235 }, RIM[4] = { 92, 150, 235, 255 }, TEXT[4] = { 235, 240, 250, 255 }, DIM[4] = { 150, 170, 210, 255 };
    const float r = 14;
    for ( int y = 0; y < TH; ++y )
        for ( int x = 0; x < TW; ++x )
        {
            const float px = x + 0.5f, py = y + 0.5f;
            const float qx = std::max( { r - px, px - ( TW - r ), 0.0f } ), qy = std::max( { r - py, py - ( TH - r ), 0.0f } );
            const float depth = qx > 0 || qy > 0 ? r - std::sqrt( qx * qx + qy * qy ) : std::min( { px, TW - px, py, TH - py } );
            const float cover = std::clamp( depth + 0.5f, 0.0f, 1.0f );
            if ( cover > 0 ) blend( x, y, depth < 3 ? RIM : PANEL, cover );
        }
    // The achievement's picture at the left, scaled to 80 x 80.
    std::vector<uint8_t> pic;
    int w = 0, h = 0;
    const int S = 80, X0 = 16, Y0 = ( TH - S ) / 2;
    if ( icon( a.image, pic, w, h ) && w > 0 && h > 0 )
        for ( int y = 0; y < S; ++y )
            for ( int x = 0; x < S; ++x )
            {
                const uint8_t *s = &pic[( (size_t)( y * h / S ) * w + x * w / S ) * 4];
                blend( X0 + x, Y0 + y, s, 1.0f );
            }
    const float tx = X0 + S + 18, max_w = TW - tx - 18;
    text( "Achievement Unlocked", tx, 44, 24, DIM, false, max_w );
    text( std::to_string( a.points ) + "G - " + a.label, tx, 84, 30, TEXT, true, max_w );
}

} // namespace

std::string profile_name() { static const std::string n = settings().profile; return n; }

uint64_t profile_xuid()
{
    // An offline profile's XUID (E000...): "Player" keeps the one it had, others one from their name (FNV-1a,
    // as the launcher makes it). Kept in the profile's folder ("xuid", hex) the first time, so a profile the
    // launcher renames keeps it (its saves and stats are its).
    static const uint64_t xuid = [] {
        const fs::path file = profile_dir() / "xuid";
        if ( FILE *f = fopen( file.c_str(), "r" ) )
        {
            unsigned long long v = 0;
            const bool ok = fscanf( f, "%llx", &v ) == 1;
            fclose( f );
            if ( ok && v ) return (uint64_t)v;
        }
        uint64_t v;
        if ( profile_name() == "Player" ) v = 0xE000000000000001ull;
        else
        {
            uint64_t h = 1469598103934665603ull;
            for ( unsigned char ch : profile_name() ) h = ( h ^ ch ) * 1099511628211ull;
            v = 0xE000000000000000ull | ( h & 0x0000FFFFFFFFFFFFull ) | 2;
        }
        std::error_code ec;
        fs::create_directories( profile_dir(), ec );
        char hex[32];
        snprintf( hex, sizeof( hex ), "%016llX\n", (unsigned long long)v );
        write_file( file, hex );
        return v;
    }();
    return xuid;
}

fs::path profile_dir() { return data_dir() / "profiles" / profile_name(); }

fs::path profile_content_dir( uint32_t content_type )
{
    // "Player" (the profile from before there were profiles) keeps its saves in the shared content folder;
    // the launcher moves them into the profile's folder if it renames it.
    if ( content_type != 1 || profile_name() == "Player" ) return data_dir() / "content";
    return profile_dir() / "content";
}

const std::vector<Achievement> &achievements()
{
    std::lock_guard<std::mutex> g( g_lock );
    load_xdbf();
    return g_xdbf.list;
}

bool achievement_unlocked( uint32_t id, uint64_t *filetime )
{
    std::lock_guard<std::mutex> g( g_lock );
    load_store();
    auto it = g_store.unlocked.find( id );
    if ( it == g_store.unlocked.end() ) return false;
    if ( filetime ) *filetime = (uint64_t)( it->second + 11644473600ll ) * 10000000ull;  // FILETIME: 100 ns since 1601
    return true;
}

void achievement_unlock( uint32_t id )
{
    achievements();
    std::lock_guard<std::mutex> g( g_lock );
    load_store();
    if ( g_store.unlocked.count( id ) ) return;
    g_store.unlocked[id] = (int64_t)time( nullptr );
    save_achievements();
    const Achievement *a = nullptr;
    for ( auto &x : g_xdbf.list )
        if ( x.id == id ) a = &x;
    rt_log( "profile: achievement %u unlocked: %s (%u points)\n", id, a ? a->label.c_str() : "?", a ? a->points : 0 );
    if ( a ) g_queue.push_back( { id } );
}

bool profile_setting( uint32_t id, uint8_t &type, std::vector<uint8_t> &value )
{
    std::lock_guard<std::mutex> g( g_lock );
    load_store();
    auto it = g_store.settings.find( id );
    if ( it == g_store.settings.end() ) return false;
    type = it->second.first, value = it->second.second;
    return true;
}

void profile_set_setting( uint32_t id, uint8_t type, const std::vector<uint8_t> &value )
{
    std::lock_guard<std::mutex> g( g_lock );
    load_store();
    g_store.settings[id] = { type, value };
    save_settings();
}

const std::vector<uint32_t> *profile_toast( uint64_t &seq, int &w, int &h, float &alpha )
{
    std::lock_guard<std::mutex> g( g_lock );
    constexpr float SHOW = 5.5f, FADE_IN = 0.3f, FADE_OUT = 0.6f;
    const auto now = std::chrono::steady_clock::now();
    if ( g_showing && std::chrono::duration<float>( now - g_shown ).count() >= SHOW ) g_showing = false, g_queue.pop_front();
    if ( !g_showing )
    {
        if ( g_queue.empty() ) return nullptr;
        for ( auto &a : g_xdbf.list )
            if ( a.id == g_queue.front().id ) draw( a );
        g_showing = true, g_shown = now, ++g_seq;
    }
    const float t = std::chrono::duration<float>( now - g_shown ).count();
    alpha = std::clamp( std::min( t / FADE_IN, ( SHOW - t ) / FADE_OUT ), 0.0f, 1.0f );
    seq = g_seq, w = TW, h = TH;
    return &g_px;
}
