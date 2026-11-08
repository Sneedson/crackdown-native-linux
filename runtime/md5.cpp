// The downloadable content check's MD5, run natively. At sign-in the game hashes every file of each
// installed pack (826FCDE8, 2 MB reads) and compares the digests with the pack's Digest.txt; the
// game's own MD5 (RFC 1321, context {state[4], bit count low / high, buffer[64]} as guest words and
// bytes) is the check's only user, its update 826FDD20 (context, data, length) and final 826FDEB8
// (digest out, context) each called from there alone. Recompiled it hashed the 98 MB of both packs in
// about a second; the same context layout is kept, so the game's inline set-up still starts it.
// Hashed in the check that still took 0.2 s, one frame: so the packs' files are hashed on a thread from
// start-up (by device, inode, size and modification time), and a file whose digest is ready when the
// check starts on it is only read: its updates skip the hashing and its final gives the digest.
#include "ppc_config.h"
#include "ppc_context.h"
#include "runtime.h"
#include "objects.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <thread>
#include <tuple>
#include <sys/stat.h>

namespace
{
inline uint32_t rotl( uint32_t x, int n ) { return ( x << n ) | ( x >> ( 32 - n ) ); }

void transform( uint32_t s[4], const uint8_t *block )
{
    static const uint32_t K[64] = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
        0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
        0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
        0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
        0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
        0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
        0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
        0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391 };
    static const int R[64] = { 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
                               5, 9, 14, 20, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 6, 10, 15, 21, 6, 10,
                               15, 21, 6, 10, 15, 21, 6, 10, 15, 21 };
    uint32_t m[16];
    for ( int i = 0; i < 16; ++i )
        m[i] = block[i * 4] | block[i * 4 + 1] << 8 | block[i * 4 + 2] << 16 | (uint32_t)block[i * 4 + 3] << 24;
    uint32_t a = s[0], b = s[1], c = s[2], d = s[3];
    for ( int i = 0; i < 64; ++i )
    {
        uint32_t f;
        int g;
        if ( i < 16 ) f = ( b & c ) | ( ~b & d ), g = i;
        else if ( i < 32 ) f = ( d & b ) | ( ~d & c ), g = ( 5 * i + 1 ) & 15;
        else if ( i < 48 ) f = b ^ c ^ d, g = ( 3 * i + 5 ) & 15;
        else f = c ^ ( b | ~d ), g = ( 7 * i ) & 15;
        uint32_t t = d;
        d = c;
        c = b;
        b = b + rotl( a + f + K[i] + m[g], R[i] );
        a = t;
    }
    s[0] += a, s[1] += b, s[2] += c, s[3] += d;
}

void update( uint32_t ctx, const uint8_t *data, uint32_t len )
{
    uint32_t s[4];
    for ( int i = 0; i < 4; ++i ) s[i] = guest_read32( ctx + i * 4 );
    uint32_t lo = guest_read32( ctx + 16 ), have = ( lo >> 3 ) & 63;
    uint64_t bits = ( (uint64_t)guest_read32( ctx + 20 ) << 32 | lo ) + (uint64_t)len * 8;
    guest_write32( ctx + 16, (uint32_t)bits );
    guest_write32( ctx + 20, (uint32_t)( bits >> 32 ) );
    uint8_t *buf = g_base + ctx + 24;
    if ( have )
    {
        uint32_t n = std::min( len, 64 - have );
        memcpy( buf + have, data, n );
        data += n, len -= n, have += n;
        if ( have < 64 ) goto done;
        transform( s, buf );
    }
    for ( ; len >= 64; data += 64, len -= 64 ) transform( s, data );
    memcpy( buf, data, len );
done:
    for ( int i = 0; i < 4; ++i ) guest_write32( ctx + i * 4, s[i] );
}

// A whole host file's MD5.
bool md5_file( const std::filesystem::path &path, std::array<uint8_t, 16> &out )
{
    FILE *f = fopen( path.c_str(), "rb" );
    if ( !f ) return false;
    uint32_t s[4] = { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476 };
    std::vector<uint8_t> buf( 1 << 20 );
    uint64_t total = 0;
    size_t n;
    while ( ( n = fread( buf.data(), 1, buf.size(), f ) ) == buf.size() )
    {
        for ( size_t i = 0; i < n; i += 64 ) transform( s, buf.data() + i );
        total += n;
    }
    fclose( f );
    total += n;
    size_t whole = n & ~size_t( 63 );
    for ( size_t i = 0; i < whole; i += 64 ) transform( s, buf.data() + i );
    uint8_t tail[128] = {};
    size_t rest = n - whole, len = rest < 56 ? 64 : 128;
    memcpy( tail, buf.data() + whole, rest );
    tail[rest] = 0x80;
    for ( int i = 0; i < 8; ++i ) tail[len - 8 + i] = (uint8_t)( ( total * 8 ) >> ( i * 8 ) );
    for ( size_t i = 0; i < len; i += 64 ) transform( s, tail + i );
    for ( int i = 0; i < 16; ++i ) out[i] = (uint8_t)( s[i / 4] >> ( ( i & 3 ) * 8 ) );
    return true;
}

using FileKey = std::tuple<dev_t, ino_t, off_t, int64_t>;
FileKey key_of( const struct stat &st ) { return { st.st_dev, st.st_ino, st.st_size, (int64_t)st.st_mtim.tv_sec * 1000000000 + st.st_mtim.tv_nsec }; }
std::mutex g_mutex;
std::map<FileKey, std::array<uint8_t, 16>> g_digests;

// The marketplace packs (content type 2) under the content folder, hashed at start-up.
struct Precompute
{
    Precompute()
    {
        std::thread( [] {
            const char *xdg = getenv( "XDG_DATA_HOME" ), *home = getenv( "HOME" );
            std::filesystem::path root = ( xdg && *xdg ? std::filesystem::path( xdg ) : std::filesystem::path( home ? home : "." ) / ".local/share" ) /
                                         "crackdown-recomp" / "content" / "00000002";
            std::error_code ec;
            for ( auto it = std::filesystem::recursive_directory_iterator( root, ec ); !ec && it != std::filesystem::recursive_directory_iterator(); it.increment( ec ) )
            {
                struct stat st;
                std::array<uint8_t, 16> d;
                if ( !it->is_regular_file( ec ) || stat( it->path().c_str(), &st ) || !md5_file( it->path(), d ) ) continue;
                std::lock_guard<std::mutex> lock( g_mutex );
                g_digests[key_of( st )] = d;
            }
        } ).detach();
    }
} g_precompute;

// The digest ready for the file the check is hashing (its handle sits just before the context, at
// 826FCDE8's +13696), decided when the file's hashing starts. ctx -> digest.
std::map<uint32_t, std::array<uint8_t, 16>> g_known;
}

extern "C" PPC_FUNC( __imp__sub_826FDD20 );
PPC_FUNC( sub_826FDD20 )
{
    auto *c = (PPCContext *)( ctx - 0x80 );
    uint32_t cx = c->r3.u32;
    if ( !guest_read32( cx + 16 ) && !guest_read32( cx + 20 ) )  // a file's first data
    {
        g_known.erase( cx );
        Object *o = obj_get( guest_read32( cx - 4 ) );
        struct stat st;
        if ( o && o->type == ObjType::File && !stat( static_cast<FileObject *>( o )->host_path.c_str(), &st ) )
        {
            std::lock_guard<std::mutex> lock( g_mutex );
            if ( auto it = g_digests.find( key_of( st ) ); it != g_digests.end() ) g_known[cx] = it->second;
        }
    }
    if ( g_known.count( cx ) )  // only counted, so the next update isn't taken for a new file
    {
        uint64_t bits = ( (uint64_t)guest_read32( cx + 20 ) << 32 | guest_read32( cx + 16 ) ) + (uint64_t)c->r5.u32 * 8;
        guest_write32( cx + 16, (uint32_t)bits ), guest_write32( cx + 20, (uint32_t)( bits >> 32 ) );
        return;
    }
    update( cx, g_base + c->r4.u32, c->r5.u32 );
}

extern "C" PPC_FUNC( __imp__sub_826FDEB8 );
PPC_FUNC( sub_826FDEB8 )
{
    auto *c = (PPCContext *)( ctx - 0x80 );
    uint32_t out = c->r3.u32, cx = c->r4.u32;
    if ( auto it = g_known.find( cx ); it != g_known.end() )
    {
        memcpy( g_base + out, it->second.data(), 16 );
        g_known.erase( it );
        memset( g_base + cx, 0, 88 );
        return;
    }
    uint8_t tail[72] = { 0x80 };
    uint32_t lo = guest_read32( cx + 16 ), hi = guest_read32( cx + 20 ), have = ( lo >> 3 ) & 63;
    uint32_t pad = have < 56 ? 56 - have : 120 - have;
    for ( int i = 0; i < 4; ++i ) tail[pad + i] = (uint8_t)( lo >> ( i * 8 ) ), tail[pad + 4 + i] = (uint8_t)( hi >> ( i * 8 ) );
    update( cx, tail, pad + 8 );
    for ( int i = 0; i < 16; ++i ) g_base[out + i] = (uint8_t)( guest_read32( cx + ( i & ~3 ) ) >> ( ( i & 3 ) * 8 ) );
    memset( g_base + cx, 0, 88 );
}
