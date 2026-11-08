// Library routines the game links statically, run natively: memcpy, memset and the PNG decoder's hot
// loops (libpng 1.2's row filters and filler, zlib 1.1's inflate_fast, crc32, adler32). The front end
// decodes its menu images when START is pressed and as the main menu appears; recompiled, that took
// about 50 ms of one frame each time (two hitches). Each does what the game's own code does, byte for
// byte; CRACKDOWN_NATIVE_CHECK=1 runs the game's version too on the same input and reports differences.
#include "ppc_config.h"
#include "ppc_context.h"
#include "runtime.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <immintrin.h>
#include <utility>
#include <zlib.h>
#include <vector>

namespace
{
const bool g_check = getenv( "CRACKDOWN_NATIVE_CHECK" ) != nullptr;

PPCContext *regs( uintptr_t ctx ) { return (PPCContext *)( ctx - 0x80 ); }

// Runs the game's version from the same registers and guest bytes (saved first, restored after) and
// returns what it left in the given ranges and r3.
struct Snapshot
{
    std::vector<std::pair<uint32_t, std::vector<uint8_t>>> ranges;
    void add( uint32_t a, uint32_t n ) { ranges.push_back( { a, std::vector<uint8_t>( g_base + a, g_base + a + n ) } ); }
    void restore() const
    {
        for ( auto &[a, v] : ranges ) memcpy( g_base + a, v.data(), v.size() );
    }
    bool same() const
    {
        for ( auto &[a, v] : ranges )
            if ( memcmp( g_base + a, v.data(), v.size() ) ) return false;
        return true;
    }
    Snapshot now() const
    {
        Snapshot s;
        for ( auto &[a, v] : ranges ) s.add( a, (uint32_t)v.size() );
        return s;
    }
};

template <class Original, class Native>
void checked( const char *name, uintptr_t ctx, uintptr_t base, uintptr_t frame, Snapshot before, Original original, Native native, bool returns = true )
{
    PPCContext saved = *regs( ctx );
    original( ctx, base, frame );
    Snapshot theirs = before.now();
    uint64_t r3 = regs( ctx )->r3.u64;
    before.restore();
    *regs( ctx ) = saved;
    native( regs( ctx ) );
    if ( !theirs.same() || ( returns && regs( ctx )->r3.u64 != r3 ) )
    {
        static int reports;
        if ( reports++ < 20 ) rt_log( "native: %s differs from the game's (r3 %llx / %llx)\n", name, (unsigned long long)regs( ctx )->r3.u64, (unsigned long long)r3 );
        theirs.restore();
        if ( returns ) regs( ctx )->r3.u64 = r3;
    }
}

// libpng: png_read_filter_row(png_ptr, row_info, row, prev_row, filter); row_info {width, rowbytes,
// color_type, bit_depth, channels, pixel_depth}. 3 and 4 byte pixels a pixel at a time in 16-bit lanes
// (the menus' images are RGB / RGBA, megabytes of them per screen).
template <uint32_t bpp> __m128i load_px( const uint8_t *p )
{
    uint32_t v = 0;
    memcpy( &v, p, bpp );
    return _mm_unpacklo_epi8( _mm_cvtsi32_si128( (int)v ), _mm_setzero_si128() );
}
template <uint32_t bpp> void store_px( uint8_t *p, __m128i v )
{
    uint32_t w = (uint32_t)_mm_cvtsi128_si32( _mm_packus_epi16( v, v ) );
    memcpy( p, &w, bpp );
}

template <uint32_t bpp, uint32_t filter> void filter_wide( uint8_t *row, const uint8_t *prev, uint32_t n )
{
    __m128i a = _mm_setzero_si128(), c = a, low = _mm_set1_epi16( 0xFF );
    for ( uint32_t i = 0; i < n; i += bpp )
    {
        __m128i x = load_px<bpp>( row + i ), pred;
        if constexpr ( filter == 1 ) pred = a;
        else
        {
            __m128i b = load_px<bpp>( prev + i );
            if constexpr ( filter == 3 ) pred = _mm_srli_epi16( _mm_add_epi16( a, b ), 1 );
            else
            {
                __m128i pa = _mm_sub_epi16( b, c ), pb = _mm_sub_epi16( a, c ), pc = _mm_abs_epi16( _mm_add_epi16( pa, pb ) );
                pa = _mm_abs_epi16( pa ), pb = _mm_abs_epi16( pb );
                __m128i least = _mm_min_epi16( pc, _mm_min_epi16( pa, pb ) );
                pred = _mm_blendv_epi8( _mm_blendv_epi8( c, b, _mm_cmpeq_epi16( pb, least ) ), a, _mm_cmpeq_epi16( pa, least ) );
                c = b;
            }
        }
        a = _mm_and_si128( _mm_add_epi16( x, pred ), low );
        store_px<bpp>( row + i, a );
    }
}

template <uint32_t bpp> void filter_wide( uint8_t *row, const uint8_t *prev, uint32_t n, uint32_t filter )
{
    if ( filter == 1 ) filter_wide<bpp, 1>( row, prev, n );
    else if ( filter == 3 ) filter_wide<bpp, 3>( row, prev, n );
    else filter_wide<bpp, 4>( row, prev, n );
}

void filter_row( PPCContext *c )
{
    uint32_t info = c->r4.u32, filter = c->r7.u32;
    uint8_t *row = g_base + c->r5.u32, *prev = g_base + c->r6.u32;
    uint32_t n = guest_read32( info + 4 ), bpp = ( g_base[info + 11] + 7 ) >> 3;
    if ( bpp == 4 && filter != 2 && n % 4 == 0 ) return filter_wide<4>( row, prev, n, filter );
    if ( bpp == 3 && filter != 2 && n % 3 == 0 ) return filter_wide<3>( row, prev, n, filter );
    switch ( filter )
    {
    case 1:
        for ( uint32_t i = bpp; i < n; ++i ) row[i] += row[i - bpp];
        break;
    case 2:
        for ( uint32_t i = 0; i < n; ++i ) row[i] += prev[i];
        break;
    case 3:
        for ( uint32_t i = 0; i < bpp && i < n; ++i ) row[i] += prev[i] >> 1;
        for ( uint32_t i = bpp; i < n; ++i ) row[i] += ( prev[i] + row[i - bpp] ) >> 1;
        break;
    case 4:
        for ( uint32_t i = 0; i < bpp && i < n; ++i ) row[i] += prev[i];
        for ( uint32_t i = bpp; i < n; ++i )
        {
            int a = row[i - bpp], b = prev[i], cc = prev[i - bpp];
            int p = b - cc, pc = a - cc, pa = abs( p ), pb = abs( pc );
            pc = abs( p + pc );
            row[i] += pa <= pb && pa <= pc ? a : pb <= pc ? b : cc;
        }
        break;
    }
}

// libpng: png_do_read_swap_alpha(row_info, row), 8-bit RGBA -> ARGB and grey-alpha -> alpha-grey in
// place (16-bit samples go to the game's code).
bool swap_alpha_handles( PPCContext *c ) { uint32_t i = c->r3.u32; return g_base[i + 9] == 8 && ( g_base[i + 8] == 6 || g_base[i + 8] == 4 ); }
void swap_alpha( PPCContext *c )
{
    uint32_t info = c->r3.u32, width = guest_read32( info );
    uint8_t *end = g_base + c->r4.u32 + guest_read32( info + 4 );
    if ( g_base[info + 8] == 6 )
        for ( uint8_t *p = end - width * 4; p < end; p += 4 )
        {
            uint32_t v;
            memcpy( &v, p, 4 );
            v = v << 8 | v >> 24;
            memcpy( p, &v, 4 );
        }
    else
        for ( uint8_t *p = end - width * 2; p < end; p += 2 ) std::swap( p[0], p[1] );
}

// libpng: png_do_read_filler(row_info, row, filler, flags), 8-bit grey and RGB (the others go to the
// game's code).
bool filler_handles( PPCContext *c ) { uint32_t i = c->r3.u32; return g_base[i + 9] == 8 && ( g_base[i + 8] == 0 || g_base[i + 8] == 2 ); }
void filler( PPCContext *c )
{
    uint32_t info = c->r3.u32, width = guest_read32( info );
    uint8_t lo = (uint8_t)c->r5.u32, *row = g_base + c->r4.u32;
    int ch = g_base[info + 8] == 0 ? 1 : 3;
    uint8_t *sp = row + width * ch, *dp = sp + width;
    if ( c->r6.u32 & 0x80 )  // filler after
    {
        for ( uint32_t i = 1; i < width; ++i )
        {
            *--dp = lo;
            for ( int k = 0; k < ch; ++k ) *--dp = *--sp;
        }
        *--dp = lo;
    }
    else
        for ( uint32_t i = 0; i < width; ++i )
        {
            for ( int k = 0; k < ch; ++k ) *--dp = *--sp;
            *--dp = lo;
        }
    g_base[info + 10] = (uint8_t)( ch + 1 );
    g_base[info + 11] = (uint8_t)( ( ch + 1 ) * 8 );
    guest_write32( info + 4, width * ( ch + 1 ) );
}

// dst[i] = src[i] in order, as a byte loop does it (a source overlapping the copy repeats it). Eight
// bytes at a time where a chunk never reads what it writes; back-references are short.
inline void copy_forward( uint8_t *d, const uint8_t *s, uint32_t n )
{
    if ( s > d || d - s >= 8 )
        for ( ; n >= 8; d += 8, s += 8, n -= 8 ) memcpy( d, s, 8 );
    else if ( d - s == 1 )
    {
        memset( d, *s, n );
        return;
    }
    while ( n-- ) *d++ = *s++;
}

// zlib 1.1: inflate_fast(bl, bd, tl, td, s, z). inflate_blocks_state s: bitk +28, bitb +32, window
// +40, end +44, read +48, write +52; z_stream z: next_in +0, avail_in +4, total_in +8, msg +24; huft
// entries {exop, bits, base (word +4)}.
void inflate_fast( PPCContext *c )
{
    uint8_t *const mem = g_base;  // a local: stores through g_base would make the compiler reload it
    auto rd32 = [mem]( uint32_t a ) { return __builtin_bswap32( *(uint32_t *)( mem + a ) ); };
    uint32_t s = c->r7.u32, z = c->r8.u32, tl = c->r5.u32, td = c->r6.u32;
    uint32_t ml = ( 1u << c->r3.u32 ) - 1, md = ( 1u << c->r4.u32 ) - 1;
    uint32_t p = rd32( z ), n = rd32( z + 4 ), b = rd32( s + 32 ), k = rd32( s + 28 );
    uint32_t q = rd32( s + 52 ), rd = rd32( s + 48 ), end = rd32( s + 44 ), window = rd32( s + 40 );
    uint32_t m = q < rd ? rd - q - 1 : end - q;
    auto grab = [&]( uint32_t bits ) {  // whole bytes until k >= bits, in one load
        if ( k >= bits ) return;
        uint32_t bytes = ( bits + 7 - k ) >> 3, v;
        memcpy( &v, mem + p, 4 );
        b |= ( v & ( 0xFFFFFFFFu >> ( 32 - bytes * 8 ) ) ) << k;
        k += bytes * 8, p += bytes, n -= bytes;
    };
    auto mask = []( uint32_t e ) { return ( 1u << e ) - 1; };
    int32_t result = 0;
    uint32_t msg = 0;
    do
    {
        grab( 20 );
        uint32_t t = tl + ( b & ml ) * 8, e = mem[t];
        if ( e == 0 )
        {
            k -= mem[t + 1], b >>= mem[t + 1];
            mem[q++] = (uint8_t)rd32( t + 4 ), --m;
            continue;
        }
        for ( ;; )
        {
            k -= mem[t + 1], b >>= mem[t + 1];
            if ( e & 16 )
            {
                e &= 15;
                uint32_t cnt = rd32( t + 4 ) + ( b & mask( e ) );
                k -= e, b >>= e;
                grab( 15 );
                t = td + ( b & md ) * 8, e = mem[t];
                for ( ;; )
                {
                    k -= mem[t + 1], b >>= mem[t + 1];
                    if ( e & 16 )
                    {
                        e &= 15;
                        grab( e );
                        uint32_t d = rd32( t + 4 ) + ( b & mask( e ) );
                        k -= e, b >>= e;
                        m -= cnt;
                        uint32_t r = q - d;
                        if ( r < window )
                        {
                            do r += end - window;
                            while ( r < window );
                            e = end - r;
                            if ( cnt > e )
                            {
                                cnt -= e;
                                copy_forward( mem + q, mem + r, e ), q += e;
                                copy_forward( mem + q, mem + window, cnt ), q += cnt;
                                break;
                            }
                        }
                        copy_forward( mem + q, mem + r, cnt ), q += cnt;  // at least 3
                        break;
                    }
                    if ( e & 64 )
                    {
                        result = -3, msg = 0x820F2D34;  // "invalid distance code"
                        goto done;
                    }
                    t += ( rd32( t + 4 ) + ( b & mask( e ) ) ) * 8, e = mem[t];
                }
                break;
            }
            if ( e & 64 )
            {
                if ( e & 32 ) result = 1;
                else result = -3, msg = 0x820F2D18;  // "invalid literal/length code"
                goto done;
            }
            t += ( rd32( t + 4 ) + ( b & mask( e ) ) ) * 8, e = mem[t];
            if ( e == 0 )
            {
                k -= mem[t + 1], b >>= mem[t + 1];
                mem[q++] = (uint8_t)rd32( t + 4 ), --m;
                break;
            }
        }
    } while ( m >= 258 && n >= 10 );
done:
    if ( msg ) guest_write32( z + 24, msg );
    uint32_t back = std::min( k >> 3, rd32( z + 4 ) - n );  // unused whole bytes go back
    n += back, p -= back, k -= back << 3;
    guest_write32( s + 32, b ), guest_write32( s + 28, k );
    guest_write32( z + 4, n );
    guest_write32( z + 8, rd32( z + 8 ) + p - rd32( z ) );
    guest_write32( z, p );
    guest_write32( s + 52, q );
    c->r3.s64 = result;
}

// zlib's crc32 and adler32 (the host's, same results).
void crc32_native( PPCContext *c )
{
    if ( !c->r4.u32 ) c->r3.u64 = 0;
    else c->r3.u64 = 0xFFFFFFFF00000000 | crc32( c->r3.u32, g_base + c->r4.u32, c->r5.u32 );  // the game's 64-bit not
}

void adler32_native( PPCContext *c ) { c->r3.u64 = c->r4.u32 ? adler32( c->r3.u32, g_base + c->r4.u32, c->r5.u32 ) : 1; }
}

// memcpy(dst, src, n): returns dst. An overlap with dst after src (where the game's forward copy
// differs from memmove) goes to the game's code.
extern "C" PPC_FUNC( __imp__sub_82B475B0 );
PPC_FUNC( sub_82B475B0 )
{
    auto *c = regs( ctx );
    uint32_t d = c->r3.u32, s = c->r4.u32, n = c->r5.u32;
    if ( d > s && d - s < n ) return __imp__sub_82B475B0( ctx, base, frame );
    memmove( g_base + d, g_base + s, n );
}

// memset(dst, value, n): returns dst.
PPC_FUNC( sub_82B47460 ) { auto *c = regs( ctx ); memset( g_base + c->r3.u32, (uint8_t)c->r4.u32, c->r5.u32 ); }

extern "C" PPC_FUNC( __imp__sub_82B08E68 );
PPC_FUNC( sub_82B08E68 )
{
    auto *c = regs( ctx );
    if ( c->r7.u32 - 1 > 3 ) return __imp__sub_82B08E68( ctx, base, frame );  // none / bad: the game's
    if ( !g_check ) return filter_row( c );
    Snapshot s;
    s.add( c->r5.u32, guest_read32( c->r4.u32 + 4 ) );
    checked( "png filter row", ctx, base, frame, s, __imp__sub_82B08E68, filter_row, false );
}

extern "C" PPC_FUNC( __imp__sub_82819010 );
PPC_FUNC( sub_82819010 )
{
    auto *c = regs( ctx );
    if ( !filler_handles( c ) ) return __imp__sub_82819010( ctx, base, frame );
    if ( !g_check ) return filler( c );
    Snapshot s;
    s.add( c->r3.u32, 12 );
    s.add( c->r4.u32 - 1, guest_read32( c->r3.u32 ) * 4 + 1 );
    checked( "png filler", ctx, base, frame, s, __imp__sub_82819010, filler, false );
}

extern "C" PPC_FUNC( __imp__sub_82B11B00 );
PPC_FUNC( sub_82B11B00 )
{
    auto *c = regs( ctx );
    if ( !g_check ) return inflate_fast( c );
    uint32_t st = c->r7.u32, z = c->r8.u32, w = guest_read32( st + 40 );
    Snapshot s;
    s.add( st, 64 );
    s.add( z, 56 );
    s.add( w, guest_read32( st + 44 ) - w );
    checked( "inflate_fast", ctx, base, frame, s, __imp__sub_82B11B00, inflate_fast );
}

extern "C" PPC_FUNC( __imp__sub_82B0A138 );
PPC_FUNC( sub_82B0A138 )
{
    if ( !g_check ) return crc32_native( regs( ctx ) );
    checked( "crc32", ctx, base, frame, Snapshot(), __imp__sub_82B0A138, crc32_native );
}

extern "C" PPC_FUNC( __imp__sub_82B0F4E8 );
PPC_FUNC( sub_82B0F4E8 )
{
    if ( !g_check ) return adler32_native( regs( ctx ) );
    checked( "adler32", ctx, base, frame, Snapshot(), __imp__sub_82B0F4E8, adler32_native );
}

extern "C" PPC_FUNC( __imp__sub_82B04370 );
PPC_FUNC( sub_82B04370 )
{
    auto *c = regs( ctx );
    if ( !swap_alpha_handles( c ) ) return __imp__sub_82B04370( ctx, base, frame );
    if ( !g_check ) return swap_alpha( c );
    Snapshot s;
    s.add( c->r4.u32, guest_read32( c->r3.u32 + 4 ) );
    checked( "png swap alpha", ctx, base, frame, s, __imp__sub_82B04370, swap_alpha, false );
}

