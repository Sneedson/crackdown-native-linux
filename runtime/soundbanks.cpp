// Names the sounds the game plays: which wave bank entry (devtools/xwb_index.py lists them all) a
// voice's input buffer came from. Reads from .xwb files (files.cpp) are noted as ranges of guest
// memory -> (bank, file offset); a buffer address then finds its range, the file offset its entry
// (whose play region holds it). In-memory banks are read whole once; streamed banks a chunk at a time
// into the streaming buffers, so a chunk's range is replaced by the next read into the same memory.
#include "ppc_config.h"
#include "ppc_context.h"
#include "runtime.h"
#include <cstdio>
#include <cstring>
#include <strings.h>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <algorithm>

std::vector<int16_t> xma_decode( const uint8_t *data, size_t bytes, uint32_t channels, uint32_t rate );  // xma.cpp

namespace
{
struct Bank
{
    std::string name;
    bool streaming = false;
    struct Entry { uint32_t off, len; float secs; uint32_t channels, rate; };
    std::vector<Entry> entries;
};
struct Range { uint32_t end; const Bank *bank; uint32_t file_off; };

std::mutex g_lock;
std::map<std::string, std::unique_ptr<Bank>> g_banks;  // by host path
std::map<uint32_t, Range> g_ranges;                    // guest start -> range

uint32_t be32( const uint8_t *p ) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

const Bank *bank_of( const std::string &path )
{
    auto it = g_banks.find( path );
    if ( it != g_banks.end() ) return it->second.get();
    auto b = std::make_unique<Bank>();
    if ( FILE *f = fopen( path.c_str(), "rb" ) )
    {
        uint8_t head[48];
        if ( fread( head, 1, 48, f ) == 48 && ( !memcmp( head, "DNBW", 4 ) || !memcmp( head, "WBND", 4 ) ) )
        {
            uint32_t bank_off = be32( head + 8 ), meta_off = be32( head + 16 ), meta_len = be32( head + 20 ), wave_off = be32( head + 40 );
            uint8_t bd[80];
            fseek( f, bank_off, SEEK_SET );
            if ( fread( bd, 1, 80, f ) == 80 )
            {
                b->streaming = be32( bd ) & 1;
                uint32_t count = be32( bd + 4 ), size = be32( bd + 72 );
                b->name.assign( (const char *)bd + 8, strnlen( (const char *)bd + 8, 64 ) );
                std::vector<uint8_t> meta( meta_len );
                fseek( f, meta_off, SEEK_SET );
                if ( size >= 24 && fread( meta.data(), 1, meta_len, f ) == meta_len )
                    for ( uint32_t i = 0; i < count && ( i + 1 ) * size <= meta_len; ++i )
                    {
                        const uint8_t *e = meta.data() + i * size;
                        uint32_t fmt = be32( e + 4 ), rate = ( fmt >> 5 ) & 0x3FFFF;
                        b->entries.push_back( { wave_off + be32( e + 8 ), be32( e + 12 ), rate ? ( be32( e ) >> 4 ) / (float)rate : 0.0f, ( fmt >> 2 ) & 7, rate } );
                    }
            }
        }
        fclose( f );
    }
    if ( b->name.empty() ) b->name = path.substr( path.find_last_of( '/' ) + 1 );
    return ( g_banks[path] = std::move( b ) ).get();
}
} // namespace

// files.cpp: a read of `size` bytes at file offset `off` of `path` landed at guest address `dest`.
void sound_bank_read( const std::string &path, uint64_t off, uint32_t dest, uint32_t size )
{
    if ( path.size() < 4 || strcasecmp( path.c_str() + path.size() - 4, ".xwb" ) || !size ) return;
    std::lock_guard<std::mutex> g( g_lock );
    const Bank *b = bank_of( path );
    dest = 0xA0000000u | ( dest & 0x1FFFFFFFu );  // one view (the decoder sees physical addresses)
    const uint32_t end = dest + size;
    // Ranges this read overlaps lose the overlapped part.
    auto it = g_ranges.upper_bound( dest );
    if ( it != g_ranges.begin() ) --it;
    while ( it != g_ranges.end() && it->first < end )
    {
        uint32_t s = it->first;
        Range r = it->second;
        if ( r.end <= dest ) { ++it; continue; }
        it = g_ranges.erase( it );
        if ( s < dest ) g_ranges[s] = { dest, r.bank, r.file_off };
        if ( r.end > end ) g_ranges[end] = { r.end, r.bank, r.file_off + ( end - s ) };
    }
    g_ranges[dest] = { end, b, (uint32_t)off };
}

// The bank entry a guest address (any view) holds data of, as "Bank #n (1.23 s)"; empty if unknown.
std::string sound_name( uint32_t addr )
{
    addr = 0xA0000000u | ( addr & 0x1FFFFFFFu );
    std::lock_guard<std::mutex> g( g_lock );
    auto it = g_ranges.upper_bound( addr );
    if ( it == g_ranges.begin() ) return {};
    --it;
    if ( addr >= it->second.end ) return {};
    const Bank *b = it->second.bank;
    uint32_t off = it->second.file_off + ( addr - it->first );
    for ( size_t i = 0; i < b->entries.size(); ++i )
        if ( off >= b->entries[i].off && off < b->entries[i].off + b->entries[i].len )
        {
            char s[96];
            snprintf( s, sizeof( s ), "%s #%zu (%.2f s%s)", b->name.c_str(), i, b->entries[i].secs, b->streaming ? ", streamed" : "" );
            return s;
        }
    return b->name + " (no entry)";
}

namespace
{
// Decoded samples (interleaved, `channels`) made 48 kHz stereo float.
std::vector<float> to_48k_stereo( const std::vector<int16_t> &pcm, uint32_t channels, uint32_t rate )
{
    if ( !channels || !rate || pcm.empty() ) return {};
    const size_t frames = pcm.size() / channels, out_frames = (size_t)( frames * 48000.0 / rate );
    std::vector<float> out( out_frames * 2 );
    for ( size_t i = 0; i < out_frames; ++i )
    {
        double at = i * (double)rate / 48000.0;
        size_t j = (size_t)at;
        float t = (float)( at - j );
        for ( uint32_t c = 0; c < 2; ++c )
        {
            uint32_t src = std::min( c, channels - 1 );
            float a = pcm[std::min( j, frames - 1 ) * channels + src], b2 = pcm[std::min( j + 1, frames - 1 ) * channels + src];
            out[i * 2 + c] = ( a + ( b2 - a ) * t ) / 32768.0f;
        }
    }
    return out;
}
}

// A wave bank entry decoded from the bank's file itself (any bank, streamed ones too), 48 kHz stereo float.
std::vector<float> sound_file_entry_pcm( const std::string &path, size_t index )
{
    Bank::Entry e{};
    {
        std::lock_guard<std::mutex> g( g_lock );
        const Bank *b = bank_of( path );
        if ( index >= b->entries.size() ) return {};
        e = b->entries[index];
    }
    std::vector<uint8_t> data( e.len );
    FILE *f = fopen( path.c_str(), "rb" );
    if ( !f ) return {};
    bool ok = fseek( f, e.off, SEEK_SET ) == 0 && fread( data.data(), 1, e.len, f ) == e.len;
    fclose( f );
    if ( !ok ) return {};
    return to_48k_stereo( xma_decode( data.data(), data.size(), e.channels, e.rate ), e.channels, e.rate );
}

// A bank entry the game holds in memory (an in-memory bank, read whole), decoded and made 48 kHz stereo
// (float, interleaved): the copy the game itself plays. Empty if that bank isn't in memory (yet).
std::vector<float> sound_entry_pcm( const char *bank_name, size_t index )
{
    uint32_t addr = 0, len = 0, channels = 0, rate = 0;
    {
        std::lock_guard<std::mutex> g( g_lock );
        for ( auto &[start, r] : g_ranges )
        {
            const Bank *b = r.bank;
            if ( b->name != bank_name || b->streaming || index >= b->entries.size() ) continue;
            const Bank::Entry &e = b->entries[index];
            if ( e.off < r.file_off || e.off + e.len > r.file_off + ( r.end - start ) ) continue;
            addr = start + ( e.off - r.file_off ), len = e.len, channels = e.channels, rate = e.rate;
        }
    }
    if ( !addr || !channels || !rate ) return {};
    std::vector<int16_t> pcm = xma_decode( g_base + addr, len, channels, rate );
    const size_t frames = pcm.size() / channels, out_frames = (size_t)( frames * 48000.0 / rate );
    std::vector<float> out( out_frames * 2 );
    for ( size_t i = 0; i < out_frames; ++i )
    {
        double at = i * (double)rate / 48000.0;
        size_t j = (size_t)at;
        float t = (float)( at - j );
        for ( uint32_t c = 0; c < 2; ++c )
        {
            uint32_t src = std::min( c, channels - 1 );
            float a = pcm[std::min( j, frames - 1 ) * channels + src], b2 = pcm[std::min( j + 1, frames - 1 ) * channels + src];
            out[i * 2 + c] = ( a + ( b2 - a ) * t ) / 32768.0f;
        }
    }
    return out;
}
