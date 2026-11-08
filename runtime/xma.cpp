// XMA decoder hardware (registers at 0x7FEA1800, little-endian; the game uses lwbrx/stwbrx).
// Contexts are 64-byte blocks in one array of 256 in physical memory; register 0x1800 holds its
// physical address and the game numbers a context by its offset in it ((address - base) >> 6).
// Per-context operations are banks of eight bitmask registers, bit n of register b = context
// 32 * b + n: kick 0x1940 (decode), lock 0x1A40, clear 0x1A80 (reset the decoder state).
// 0x1818 reads as the context being worked on; the mixer waits while one of its voices' contexts
// is (value ^ 0x200), so decoding a kicked context right away keeps it from waiting.
//
// Context (big-endian dwords, fields numbered from the low bit):
//   0: input buffer 0 packets 0-11, loop count 12-19, buffer 0 valid 20, buffer 1 valid 21,
//      output buffer blocks 22-26, output write offset 27-31 (blocks of 256 bytes)
//   1: input buffer 1 packets 0-11, sample rate 27-28 (24/32/44.1/48 kHz), stereo 29, output valid 31
//   2: input read offset 0-25 (bits, within the current buffer)
//   3: loop start 0-25   4: loop end 0-25, current input buffer 31
//   5, 6: input buffer 0 / 1 (physical)   7: output buffer (physical)   8: work buffer
//   9: output read offset 0-4
// Input buffers hold 2048-byte XMA packets; output is 16-bit big-endian PCM, channels interleaved.
// Packets are decoded by FFmpeg's xma2 decoder, whole packets at a time; loops jump between the
// packets holding the loop points.
// CRACKDOWN_XMA_LOG=1 logs kicks.
#include "ppc_config.h"
#include "ppc_context.h"
#include "runtime.h"

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
}

#include <algorithm>
#include <bitset>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>

uint64_t audio_sample_count();  // audio.cpp
uint32_t front_end_scene();     // xam.cpp
bool front_end_silencing();     // xam.cpp
bool title_music_muted();       // xam.cpp
#include <string>
std::string sound_name( uint32_t addr );  // soundbanks.cpp
#include <vector>

namespace
{
constexpr uint32_t kContexts = 256, kContextSize = 64, kPacketBits = 2048 * 8;
std::mutex g_xma_lock;
uint32_t g_array;  // guest (physical-window) address of the context array
std::bitset<kContexts> g_used;

struct Decoder
{
    AVCodecContext *codec = nullptr;
    AVPacket *packet = nullptr;
    AVFrame *frame = nullptr;
    uint32_t channels = 0, rate = 0;
    std::vector<int16_t> pending;  // decoded samples (interleaved) not yet written to the output ring
    uint32_t muted_buffer = 0;     // a one-shot sound being silenced (its input buffer), see ui_sound_silenced
    uint64_t last_kick = 0;        // audio sample count at its last kick (CRACKDOWN_XMA_STATS)
    uint64_t starved = 0;
    uint32_t seen_base = 0, seen_packet = 0, seen_count = 0;
    uint32_t last_base = 0, last_loops = 0;  // the input last decoded from (logs)
    uint64_t first_sample = 0, decoded = 0, written = 0;  // logs: audio clock at the first decode, sample frames decoded / into the ring  // where the last kick left its input (cut-off log)
    // A loop inside one packet (the scope's zoom motor: start bit 32, end bit 4358 of a 1-packet buffer):
    // one repeat of it decoded once (loop_key: buffer address and loop points), played over and over.
    uint64_t loop_key = 0;
    std::vector<int16_t> loop_pcm;

    void reset()
    {
        avcodec_free_context( &codec );
        av_packet_free( &packet );
        av_frame_free( &frame );
        channels = rate = 0;
        pending.clear();
        muted_buffer = 0;
        loop_key = 0;
        loop_pcm.clear();
        first_sample = decoded = written = 0;
        starved = 0;
        seen_base = 0;
    }
};
Decoder g_decoders[kContexts];

uint32_t array_locked()
{
    if ( !g_array )
    {
        g_array = rt_alloc_physical( kContexts * kContextSize, 0x1000 );
        memset( g_base + g_array, 0, kContexts * kContextSize );
        rt_log( "xma: context array at %08X (physical %08X)\n", g_array, g_array & 0x1FFFFFFF );
    }
    return g_array;
}

uint8_t *phys( uint32_t p ) { return g_base + 0xA0000000 + ( p & 0x1FFFFFFF ); }

bool open_codec( Decoder &d, uint32_t channels, uint32_t rate )
{
    d.reset();
    const AVCodec *c = avcodec_find_decoder( AV_CODEC_ID_XMA2 );
    if ( !c ) return false;
    d.codec = avcodec_alloc_context3( c );
    av_channel_layout_default( &d.codec->ch_layout, (int)channels );
    d.codec->sample_rate = (int)rate;
    // XMA2WAVEFORMATEX-sized extradata: one stream of `channels` channels.
    d.codec->extradata = (uint8_t *)av_mallocz( 34 + AV_INPUT_BUFFER_PADDING_SIZE );
    d.codec->extradata_size = 34;
    d.codec->extradata[0] = 1;  // stream count (little-endian u16)
    if ( avcodec_open2( d.codec, c, nullptr ) < 0 )
    {
        rt_log( "xma: xma2 decoder would not open (%u ch, %u Hz)\n", channels, rate );
        d.reset();
        return false;
    }
    d.packet = av_packet_alloc();
    d.frame = av_frame_alloc();
    d.channels = channels;
    d.rate = rate;
    return true;
}

void receive_frames( Decoder &d );
// Decodes one 2048-byte packet into d.pending (16-bit, interleaved).
void decode_packet( Decoder &d, const uint8_t *data )
{
    if ( av_new_packet( d.packet, 2048 ) < 0 ) return;
    memcpy( d.packet->data, data, 2048 );
    int r = avcodec_send_packet( d.codec, d.packet );
    av_packet_unref( d.packet );
    if ( r < 0 ) return;
    receive_frames( d );
}

// The frames the decoder has ready, into d.pending.
void receive_frames( Decoder &d )
{
    while ( avcodec_receive_frame( d.codec, d.frame ) == 0 )
    {
        int n = d.frame->nb_samples, ch = std::min<int>( d.frame->ch_layout.nb_channels, (int)d.channels );
        size_t at = d.pending.size();
        d.pending.resize( at + (size_t)n * d.channels );
        for ( int i = 0; i < n; ++i )
            for ( uint32_t c = 0; c < d.channels; ++c )
            {
                float s = 0;
                if ( (int)c < ch )
                {
                    if ( d.frame->format == AV_SAMPLE_FMT_FLTP ) s = ( (const float *)d.frame->extended_data[c] )[i];
                    else if ( d.frame->format == AV_SAMPLE_FMT_FLT ) s = ( (const float *)d.frame->extended_data[0] )[i * ch + c];
                }
                d.pending[at + (size_t)i * d.channels + c] = (int16_t)std::clamp( s * 32767.0f, -32768.0f, 32767.0f );
            }
        av_frame_unref( d.frame );
    }
}

// One repeat of a loop that lies inside a single packet: the packet decoded six times in a row (as the
// hardware streams it while looping) and drained; the third repeat's share is the seamless loop body (the
// first ones carry the decoder's start-up, the last its drain). Whole packet per repeat: the loop points
// inside it are not resolved to frames.
std::vector<int16_t> packet_loop( Decoder &d, const uint8_t *data )
{
    std::vector<int16_t> keep;
    keep.swap( d.pending );
    avcodec_flush_buffers( d.codec );
    constexpr int kRepeats = 6;
    for ( int i = 0; i < kRepeats; ++i ) decode_packet( d, data );
    if ( avcodec_send_packet( d.codec, nullptr ) == 0 ) receive_frames( d );  // the frames held back
    avcodec_flush_buffers( d.codec );                                            // (and out of the drained state)
    std::vector<int16_t> all;
    all.swap( d.pending );
    d.pending.swap( keep );
    const size_t per = all.size() / kRepeats / d.channels * d.channels;
    if ( !per ) return {};
    return std::vector<int16_t>( all.begin() + 2 * per, all.begin() + 3 * per );
}

// A voice stopped (cleared or released) with decoded samples it never got to play: its sound's tail lost.
void lost_log( uint32_t index, const char *how )
{
    const Decoder &d = g_decoders[index];
    if ( g_array && d.pending.size() >= 64 )
    {
        const uint32_t c = g_array + index * kContextSize;
        rt_log( "xma: context %u at its %s: w0 %08X w1 %08X w2 %08X w4 %08X in %08X/%08X, starved since %.3f s\n", index, how, guest_read32( c ), guest_read32( c + 4 ), guest_read32( c + 8 ),
                guest_read32( c + 16 ), guest_read32( c + 20 ), guest_read32( c + 24 ), d.starved ? ( audio_sample_count() - d.starved ) / 48000.0 : -1.0 );
    }
    if ( d.pending.size() < 64 && d.decoded && d.last_base && getenv( "CRACKDOWN_SPEECH_LOG" ) )  // CRACKDOWN_SPEECH_LOG=1: speech voices' normal ends too
    {
        const std::string n = sound_name( d.last_base );
        if ( !n.compare( 0, 3, "VOA" ) || !n.compare( 0, 6, "Vocals" ) )
            rt_log( "xma: context %u %s after %.3f s (decoded %.3f s), complete: %s\n", index, how, d.first_sample ? ( audio_sample_count() - d.first_sample ) / 48000.0 : 0.0,
                    d.decoded / (double)std::max<uint32_t>( d.rate, 1 ), n.c_str() );
    }
    int peak = 0;
    for ( int16_t v : d.pending ) peak = std::max( peak, std::abs( (int)v ) );
    if ( d.pending.size() >= 64 && d.channels )
        rt_log( "xma: context %u %s after %.3f s (decoded %.3f s, into the ring %.3f s) with %zu decoded samples unplayed (peak %d, %.3f s) at sample %llu: %s loops %u\n", index, how,
                d.first_sample ? ( audio_sample_count() - d.first_sample ) / 48000.0 : 0.0, d.decoded / (double)std::max<uint32_t>( d.rate, 1 ), d.written / (double)std::max<uint32_t>( d.rate, 1 ),
                d.pending.size() / d.channels, peak,
                d.pending.size() / (double)d.channels / std::max<uint32_t>( d.rate, 1 ), (unsigned long long)audio_sample_count(), sound_name( d.last_base ).c_str(), d.last_loops );
}

// A kicked context: decode until the output ring is full or the input runs out.
void kick( uint32_t index )
{
    uint32_t ctx = g_array + index * kContextSize;
    uint32_t w[10];
    for ( int i = 0; i < 10; ++i ) w[i] = guest_read32( ctx + i * 4 );
    Decoder &d = g_decoders[index];
    d.last_kick = audio_sample_count();
    // CRACKDOWN_XMA_STATS=1: every 5 s of audio, the contexts in use and those holding input that have
    // not been kicked for a second or more (a voice the decoder left waiting).
    if ( static const bool stats = getenv( "CRACKDOWN_XMA_STATS" ); stats )
    {
        static uint64_t next;
        if ( d.last_kick >= next )
        {
            next = d.last_kick + 5 * 48000;
            uint32_t used = 0, waiting = 0;
            uint64_t oldest = 0;
            for ( uint32_t i = 0; i < kContexts; ++i )
            {
                if ( !g_used[i] ) continue;
                ++used;
                uint32_t w0 = guest_read32( g_array + i * kContextSize );
                uint64_t idle = d.last_kick - std::min( d.last_kick, g_decoders[i].last_kick );
                if ( ( w0 >> 20 ) & 3 && idle >= 48000 ) ++waiting, oldest = std::max( oldest, idle );
            }
            rt_log( "xma: %u contexts in use, %u with input not kicked for 1 s or more (longest %.1f s)\n", used, waiting, oldest / 48000.0 );
        }
    }
    static const uint32_t rates[4] = { 24000, 32000, 44100, 48000 };
    uint32_t channels = ( w[1] >> 29 ) & 1 ? 2 : 1, rate = rates[( w[1] >> 27 ) & 3];
    if ( ( !d.codec || d.channels != channels || d.rate != rate ) && !open_codec( d, channels, rate ) ) return;

    // The input the last kick was part way through, gone now: the game replaced or withdrew the buffer
    // before its end (a sound stopped early). Logged always (rare; dialogue cut-off hunt).
    if ( d.seen_base && d.seen_packet < d.seen_count )
    {
        const uint32_t cur = w[4] >> 31, base = cur ? w[6] : w[5], valid = ( w[0] >> ( 20 + cur ) ) & 1, other = cur ? w[5] : w[6], ovalid = ( w[0] >> ( 21 - cur ) ) & 1;
        if ( !( ( valid && base == d.seen_base ) || ( ovalid && other == d.seen_base ) ) )
            rt_log( "xma: context %u left buffer %08X %s at packet %u of %u (now %08X valid %u) at sample %llu\n", index, d.seen_base, sound_name( d.seen_base ).c_str(), d.seen_packet,
                    d.seen_count, base, valid, (unsigned long long)audio_sample_count() );
        d.seen_base = 0;
    }
    uint32_t out_blocks = ( w[0] >> 22 ) & 31;
    if ( !out_blocks ) out_blocks = 32;
    uint32_t write_off = ( w[0] >> 27 ) & 31, read_off = w[9] & 31;
    bool out_valid = ( w[1] >> 31 ) & 1;
    const uint32_t block_samples = 256 / 2;  // 16-bit samples per 256-byte block (all channels)
    uint32_t packets_fed = 0, blocks_written = 0;

    for ( int guard = 0; guard < 4096; ++guard )
    {
        // Pending samples go out first, in whole blocks, while the ring has room.
        while ( out_valid && d.pending.size() >= block_samples )
        {
            uint32_t free_blocks = ( read_off + out_blocks - write_off - 1 ) % out_blocks;
            if ( !free_blocks ) break;
            uint8_t *dst = phys( w[7] ) + write_off * 256;
            for ( uint32_t i = 0; i < block_samples; ++i )
            {
                uint16_t s = __builtin_bswap16( (uint16_t)d.pending[i] );
                memcpy( dst + i * 2, &s, 2 );
            }
            d.pending.erase( d.pending.begin(), d.pending.begin() + block_samples );
            d.written += block_samples / std::max<uint32_t>( d.channels, 1 );
            write_off = ( write_off + 1 ) % out_blocks;
            ++blocks_written;
        }
        if ( !out_valid || d.pending.size() >= block_samples ) break;  // no room: the rest waits for the next kick

        // The next packet of the current input buffer.
        uint32_t cur = w[4] >> 31;
        bool valid = ( w[0] >> ( 20 + cur ) ) & 1;
        if ( !valid )
        {
            if ( !( ( w[0] >> ( 21 - cur ) ) & 1 ) )  // neither buffer has data
            {
                if ( !d.starved ) d.starved = audio_sample_count() | 1;
                break;
            }
            cur ^= 1;
            w[4] = ( w[4] & 0x7FFFFFFF ) | ( cur << 31 );
            w[2] = ( w[2] & ~0x3FFFFFFu ) | 32;
        }
        uint32_t count = cur ? w[1] & 0xFFF : w[0] & 0xFFF, base = cur ? w[6] : w[5];
        // (Logged always.) A voice whose input ran out and then got more (an underrun, not an end: the
        // context wasn't reset in between), with the gap and what it plays.
        if ( d.starved )
        {
            if ( true )
            {
                const double gap = ( audio_sample_count() - d.starved ) / 48000.0;
                if ( gap > 0.02 ) rt_log( "xma: context %u starved %.3f s (pending %zu samples), then buffer %08X (%u packets) %s at sample %llu\n", index, gap, d.pending.size(), base, count,
                                         sound_name( base ).c_str(), (unsigned long long)audio_sample_count() );
            }
            d.starved = 0;
        }
        uint32_t packet = ( w[2] & 0x3FFFFFF ) / kPacketBits;
        if ( static const bool vlog = getenv( "CRACKDOWN_VOICE_LOG" ); vlog && packet == 0 )  // a buffer starts: log it once per address
        {
            static std::set<uint64_t> seen;
            if ( seen.insert( (uint64_t)index << 32 | base ).second || ( count <= 8 && !( ( w[0] >> 12 ) & 255 ) ) )  // (a looping one once)
            {
                rt_log( "voice: context %u buffer %08X (%u packets) %s scene %u sample %llu\n", index, base, count, sound_name( base ).c_str(),
                        front_end_scene(), (unsigned long long)audio_sample_count() );
            }
        }
        // A short one-shot sound starting while the front end steps through scenes the runtime hides is
        // silenced to its end (front_end_silencing, xam.cpp: those steps' scene-change sound).
        if ( packet == 0 && count <= 8 )
        {
            d.muted_buffer = front_end_silencing() ? base : 0;
            if ( d.muted_buffer && getenv( "CRACKDOWN_VOICE_LOG" ) ) rt_log( "voice: context %u buffer %08X silenced\n", index, base );
        }
        else if ( packet == 0 && d.muted_buffer != base ) d.muted_buffer = 0;
        // A loop inside one packet: its repeat goes out as many times as the ring takes (no packet stepping:
        // re-decoding one packet from a flushed decoder gives nothing, and the loop spun 4096 times a kick).
        {
            const uint32_t loops = ( w[0] >> 12 ) & 255, loop_start = w[3] & 0x3FFFFFF, loop_end = w[4] & 0x3FFFFFF;
            if ( loops && loop_end > loop_start && packet < count && loop_start / kPacketBits == packet && ( loop_end - 1 ) / kPacketBits == packet )
            {
                const uint64_t key = (uint64_t)base << 32 ^ (uint64_t)loop_start << 26 ^ loop_end;
                if ( d.loop_key != key )
                {
                    d.loop_pcm = packet_loop( d, phys( base ) + packet * 2048 );
                    d.loop_key = key;
                    if ( getenv( "CRACKDOWN_VOICE_LOG" ) )
                        rt_log( "voice: context %u buffer %08X loops in packet %u (bits %u-%u): %zu samples a repeat\n", index, base, packet, loop_start, loop_end,
                                d.loop_pcm.size() / d.channels );
                }
                if ( d.loop_pcm.empty() )
                    packet = count;  // nothing to play: the buffer is done (not spun on)
                else
                {
                    d.pending.insert( d.pending.end(), d.loop_pcm.begin(), d.loop_pcm.end() );
                    ++packets_fed;
                    if ( loops != 255 )
                    {
                        w[0] = ( w[0] & ~( 255u << 12 ) ) | ( ( loops - 1 ) << 12 );
                        if ( loops == 1 ) ++packet;  // the last repeat: on past the loop
                    }
                }
                if ( packet >= count )
                {
                    w[0] &= ~( 1u << ( 20 + cur ) );
                    w[4] = ( w[4] & 0x7FFFFFFF ) | ( ( cur ^ 1 ) << 31 );
                    w[2] = ( w[2] & ~0x3FFFFFFu ) | 32;
                }
                else w[2] = ( w[2] & ~0x3FFFFFFu ) | ( packet * kPacketBits + 32 );
                continue;
            }
        }
        if ( packet < count )
        {
            d.last_base = base, d.last_loops = ( w[0] >> 12 ) & 255;
            size_t before = d.pending.size();
            if ( !d.first_sample ) d.first_sample = audio_sample_count() | 1;
            decode_packet( d, phys( base ) + packet * 2048 );
            d.decoded += ( d.pending.size() - before ) / std::max<uint32_t>( d.channels, 1 );
            if ( static const bool vlog = getenv( "CRACKDOWN_VOICE_LOG" ); vlog && count > 8 && packet == 4 )  // a streamed buffer's level
            {
                int peak = 0;
                for ( size_t i = before; i < d.pending.size(); ++i ) peak = std::max( peak, std::abs( (int)d.pending[i] ) );
                rt_log( "voice: context %u buffer %08X packet 4 peak %d (of 32767) sample %llu\n", index, base, peak, (unsigned long long)audio_sample_count() );
            }
            // Streamed sound (the music: buffers of more than 8 packets) after START on the title, until the
            // game resets its audio (title_music_muted, xam.cpp).
            static const bool mute_streams = getenv( "CRACKDOWN_MUTE_STREAMS" );  // debug: short sounds alone in the mix (latency tests)
            if ( d.muted_buffer == base || ( count > 8 && ( title_music_muted() || mute_streams ) ) )
            {
                std::fill( d.pending.begin() + before, d.pending.end(), (int16_t)0 );
                if ( static const bool vlog = getenv( "CRACKDOWN_VOICE_LOG" ); vlog )
                {
                    static uint64_t next;
                    if ( audio_sample_count() >= next )
                        next = audio_sample_count() + 48000, rt_log( "voice: context %u buffer %08X muted (%s)\n", index, base, d.muted_buffer == base ? "one-shot" : "title music" );
                }
            }
            ++packets_fed;
            ++packet;
            // Hardware loop: past the packet holding the loop end, go back to the one holding the loop
            // start (whole packets: the seam can be off by up to a packet). 255 loops forever.
            uint32_t loops = ( w[0] >> 12 ) & 255, loop_end = w[4] & 0x3FFFFFF;
            if ( loops && loop_end && packet > ( loop_end - 1 ) / kPacketBits )
            {
                packet = ( w[3] & 0x3FFFFFF ) / kPacketBits;
                avcodec_flush_buffers( d.codec );  // a frame that straddles the jump cannot be decoded
                if ( loops != 255 ) w[0] = ( w[0] & ~( 255u << 12 ) ) | ( ( loops - 1 ) << 12 );
            }
        }
        const bool other_valid = ( w[0] >> ( 21 - cur ) ) & 1;
        if ( packet >= count && !other_valid && d.pending.size() >= block_samples )
        {
            // The sound's last buffer is decoded but its samples aren't all out yet: the buffer stays the
            // game's "in use" until they are. Handed back at once, the game took the voice for finished
            // with up to 0.2 s still to play and released it: dialogue lines lost their ends.
            w[2] = ( w[2] & ~0x3FFFFFFu ) | ( count * kPacketBits + 32 );
            continue;
        }
        if ( packet >= count )  // buffer used up: hand it back and go on with the other one
        {
            // The very end of a sound: its last part-block out too (made whole with silence).
            if ( !other_valid && !d.pending.empty() ) d.pending.resize( block_samples, 0 );
            w[0] &= ~( 1u << ( 20 + cur ) );
            w[4] = ( w[4] & 0x7FFFFFFF ) | ( ( cur ^ 1 ) << 31 );
            w[2] = ( w[2] & ~0x3FFFFFFu ) | 32;
        }
        else w[2] = ( w[2] & ~0x3FFFFFFu ) | ( packet * kPacketBits + 32 );
    }
    w[0] = ( w[0] & ~( 31u << 27 ) ) | ( write_off << 27 );
    for ( int i : { 0, 2, 4 } ) guest_write32( ctx + i * 4, w[i] );
    {
        const uint32_t cur = w[4] >> 31;
        if ( ( w[0] >> ( 20 + cur ) ) & 1 )
            d.seen_base = cur ? w[6] : w[5], d.seen_count = cur ? w[1] & 0xFFF : w[0] & 0xFFF, d.seen_packet = ( w[2] & 0x3FFFFFF ) / kPacketBits;
        else d.seen_base = 0;
    }

    static const bool log = getenv( "CRACKDOWN_XMA_LOG" );
    if ( log )
        rt_log( "xma: kick %u: %u ch %u Hz, fed %u packets, wrote %u blocks (write %u read %u of %u), %zu samples pending, valid %u%u cur %u, "
                "subframes %u, w1 %08X, loops %u start %u end %u\n",
                index, channels, rate, packets_fed, blocks_written, write_off, read_off, out_blocks, d.pending.size(), ( w[0] >> 20 ) & 1,
                ( w[0] >> 21 ) & 1, w[4] >> 31, ( w[1] >> 20 ) & 15, w[1], ( w[0] >> 12 ) & 255, w[3] & 0x3FFFFFF, w[4] & 0x3FFFFFF );
}
} // namespace

// XMACreateContext(PVOID *context): a free entry of the array.
RT_IMPORT( XMACreateContext )
{
    std::lock_guard<std::mutex> g( g_xma_lock );
    uint32_t base = array_locked();
    for ( uint32_t i = 0; i < kContexts; ++i )
        if ( !g_used[i] )
        {
            g_used[i] = true;
            g_decoders[i].reset();
            uint32_t p = base + i * kContextSize;
            memset( g_base + p, 0, kContextSize );
            guest_write32( RT_ARG( 0 ), p );
            RT_RETURN( 0 );
        }
    rt_log( "xma: out of contexts\n" );
    RT_RETURN( 0xC000009A );  // STATUS_INSUFFICIENT_RESOURCES
}

// XMAReleaseContext(PVOID context)
RT_IMPORT( XMAReleaseContext )
{
    std::lock_guard<std::mutex> g( g_xma_lock );
    if ( g_array && RT_ARG( 0 ) >= g_array && RT_ARG( 0 ) < g_array + kContexts * kContextSize )
    {
        uint32_t i = ( RT_ARG( 0 ) - g_array ) / kContextSize;
        g_used[i] = false;
        if ( g_decoders[i].pending.size() >= 64 && getenv( "CRACKDOWN_RELEASE_TRACE" ) )  // who stops a sound early (testing)
        {
            void rt_guest_backtrace( char *out, size_t n );
            char bt[512];
            rt_guest_backtrace( bt, sizeof( bt ) );
            rt_log( "xma: context %u released from %s\n", i, bt );
        }
        lost_log( i, "released" );
        g_decoders[i].reset();
    }
    RT_RETURN( 0 );
}

// A whole XMA sound (2048-byte packets) decoded to 16-bit samples, channels interleaved: the menu sounds
// the mouse plays itself (xui.cpp, through soundbanks.cpp).
std::vector<int16_t> xma_decode( const uint8_t *data, size_t bytes, uint32_t channels, uint32_t rate )
{
    Decoder d;
    if ( !open_codec( d, channels, rate ) ) return {};
    for ( size_t o = 0; o + 2048 <= bytes; o += 2048 ) decode_packet( d, data + o );
    std::vector<int16_t> pcm = std::move( d.pending );
    d.reset();
    return pcm;
}

// MMIO hooks (values as the big-endian word the game's load or store sees; the registers
// themselves are little-endian). Return false for addresses outside the XMA block.
bool xma_mmio_read( uint32_t addr, uint32_t &value )
{
    if ( addr < 0x7FEA1800 || addr >= 0x7FEA4000 ) return false;
    uint32_t v = 0;
    if ( addr == 0x7FEA1800 )
    {
        std::lock_guard<std::mutex> g( g_xma_lock );
        v = array_locked() & 0x1FFFFFFF;
    }
    value = __builtin_bswap32( v );
    return true;
}

bool xma_mmio_write( uint32_t addr, uint32_t value )
{
    if ( addr < 0x7FEA1800 || addr >= 0x7FEA4000 ) return false;
    uint32_t bits = __builtin_bswap32( value );
    bool is_kick = addr >= 0x7FEA1940 && addr < 0x7FEA1960, is_clear = addr >= 0x7FEA1A80 && addr < 0x7FEA1AA0;
    if ( !is_kick && !is_clear ) return true;  // lock, reset: nothing to do
    std::lock_guard<std::mutex> g( g_xma_lock );
    if ( !g_array ) return true;
    uint32_t first = ( addr & 0x1F ) / 4 * 32;
    for ( uint32_t b = 0; b < 32; ++b )
        if ( bits >> b & 1 )
        {
            if ( is_kick ) kick( first + b );
            else
            {
                // (Logged always.) A voice cleared (stopped) by the game while input was still queued.
                if ( true )
                {
                    const uint32_t ctx = g_array + ( first + b ) * kContextSize, w0 = guest_read32( ctx ), w4 = guest_read32( ctx + 16 );
                    const uint32_t cur = w4 >> 31, base = guest_read32( ctx + ( cur ? 24 : 20 ) );
                    if ( ( w0 >> 20 ) & 3 )
                        rt_log( "xma: context %u stopped with input left (buffer %08X %s, %zu samples pending) at sample %llu\n", first + b, base, sound_name( base ).c_str(),
                                g_decoders[first + b].pending.size(), (unsigned long long)audio_sample_count() );
                }
                lost_log( first + b, "cleared" );
                g_decoders[first + b].reset();
            }
        }
    return true;
}
