// Audio output: the frames the game's render-driver client submits (XAudioSubmitRenderDriverFrame)
// go to the default SDL playback device. A frame is 256 samples per channel at 48 kHz, six
// channels (front left, front right, centre, LFE, surround left, surround right), stored one
// channel after another as big-endian floats; it is mixed down to stereo here.
// Silent when CRACKDOWN_HEADLESS or CRACKDOWN_NO_AUDIO is set. CRACKDOWN_AUDIO_LOG=1 prints the
// signal level every 5 s.
#include "ppc_config.h"
#include "ppc_context.h"
#include "runtime.h"
#include "input.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <climits>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <memory>
#include <vector>

namespace
{
SDL_AudioStream *g_stream;
std::once_flag g_open_once;
bool g_enabled;
std::atomic<uint64_t> g_samples;  // stereo sample frames mixed so far
std::atomic<int> g_device_frames;  // the device's period (sample frames), once open

void open_device()
{
    // CRACKDOWN_AUDIO_DEVICE=1: open the device anyway (headless tests with SDL_AUDIO_DRIVER=disk).
    if ( ( getenv( "CRACKDOWN_HEADLESS" ) || getenv( "CRACKDOWN_NO_AUDIO" ) ) && !getenv( "CRACKDOWN_AUDIO_DEVICE" ) ) return;
    // A short device period (PipeWire's default is 1024 frames, up to 2048): 512 frames, 10.7 ms
    // (CRACKDOWN_AUDIO_FRAMES=n for another).
    SDL_SetHint( SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, getenv( "CRACKDOWN_AUDIO_FRAMES" ) ? getenv( "CRACKDOWN_AUDIO_FRAMES" ) : "512" );
    if ( !SDL_InitSubSystem( SDL_INIT_AUDIO ) ) { rt_log( "audio: SDL audio init failed: %s\n", SDL_GetError() ); return; }
    SDL_AudioSpec spec{ SDL_AUDIO_F32, 2, 48000 };
    g_stream = SDL_OpenAudioDeviceStream( SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr );
    if ( !g_stream ) { rt_log( "audio: no playback device: %s\n", SDL_GetError() ); return; }
    SDL_AudioSpec dev{};
    int frames = 0;
    SDL_GetAudioDeviceFormat( SDL_GetAudioStreamDevice( g_stream ), &dev, &frames );
    g_device_frames = std::max( frames, 256 );
    SDL_ResumeAudioStreamDevice( g_stream );
    g_enabled = true;
    rt_log( "audio: output open (48 kHz stereo; device %d Hz, %d frames a period)\n", dev.freq, frames );
}

float be_float( const uint8_t *p )
{
    uint32_t v;
    memcpy( &v, p, 4 );
    v = __builtin_bswap32( v );
    float f;
    memcpy( &f, &v, 4 );
    return std::isfinite( f ) ? f : 0.0f;
}
} // namespace

uint64_t audio_sample_count() { return g_samples; }

// Sounds the runtime plays itself over the game's mix (the menu sounds of the mouse, xui.cpp): 48 kHz
// stereo float, mixed in from the next frame.
namespace
{
struct Overlay { std::shared_ptr<const std::vector<float>> pcm; size_t pos; float gain; float fade = 0; bool loop = false; };  // fade: gain off per sample frame
std::mutex g_overlay_lock;
std::vector<Overlay> g_overlays;
}
void audio_play_overlay( std::shared_ptr<const std::vector<float>> pcm, float gain )
{
    if ( !pcm || pcm->empty() ) return;
    std::lock_guard<std::mutex> g( g_overlay_lock );
    g_overlays.push_back( { std::move( pcm ), 0, gain } );
}
// The same, round and round until faded out (audio_fade_overlay).
void audio_loop_overlay( std::shared_ptr<const std::vector<float>> pcm, float gain )
{
    if ( !pcm || pcm->size() < 2 ) return;
    std::lock_guard<std::mutex> g( g_overlay_lock );
    g_overlays.push_back( { std::move( pcm ), 0, gain, 0, true } );
}
// Fades a playing overlay (the same samples) out over `seconds`, after which it is gone.
void audio_fade_overlay( const std::vector<float> *pcm, float seconds )
{
    std::lock_guard<std::mutex> g( g_overlay_lock );
    for ( Overlay &o : g_overlays )
        if ( o.pcm.get() == pcm && !o.fade ) o.fade = o.gain / std::max( seconds * 48000.0f, 1.0f );
}
// Whether an overlay (those samples) is still playing.
bool audio_overlay_playing( const std::vector<float> *pcm )
{
    std::lock_guard<std::mutex> g( g_overlay_lock );
    for ( const Overlay &o : g_overlays )
        if ( o.pcm.get() == pcm ) return true;
    return false;
}

// The output's backlog: sample frames queued for the device, -1 without one (headless, none open).
int audio_output_backlog()
{
    if ( !g_enabled ) return -1;
    return SDL_GetAudioStreamQueued( g_stream ) / (int)( 2 * sizeof( float ) );
}
// What the render clock (system.cpp) keeps queued when paced by the device: two of its periods.
int audio_output_target() { return g_enabled ? 2 * g_device_frames : -1; }

void audio_submit_frame( const uint8_t *samples )
{
    std::call_once( g_open_once, open_device );
    static const bool log = getenv( "CRACKDOWN_AUDIO_LOG" );
    constexpr int N = 256;
    float out[N * 2];
    float mix_l[N], mix_r[N];
    float peak = 0;
    for ( int i = 0; i < N; ++i )
    {
        float fl = be_float( samples + ( 0 * N + i ) * 4 ), fr = be_float( samples + ( 1 * N + i ) * 4 );
        float c = be_float( samples + ( 2 * N + i ) * 4 ), lfe = be_float( samples + ( 3 * N + i ) * 4 );
        float sl = be_float( samples + ( 4 * N + i ) * 4 ), sr = be_float( samples + ( 5 * N + i ) * 4 );
        float l = fl + 0.7071f * c + 0.7071f * sl + 0.5f * lfe;
        float r = fr + 0.7071f * c + 0.7071f * sr + 0.5f * lfe;
        if ( static const bool log = getenv( "CRACKDOWN_RAW_MIX" ); log )  // testing: the game's own mix, per channel, once a second
        {
            static float pk[6];
            static uint32_t n;
            for ( int ch = 0; ch < 6; ++ch ) pk[ch] = std::max( pk[ch], std::fabs( be_float( samples + ( ch * N + i ) * 4 ) ) );
            if ( ++n == 48000 )
            {
                rt_log( "rawmix: peaks %.3f %.3f %.3f %.3f %.3f %.3f revealed %d\n", pk[0], pk[1], pk[2], pk[3], pk[4], pk[5], (int)video_revealed() );
                n = 0;
                memset( pk, 0, sizeof( pk ) );
            }
        }
        bool video_cover_active();  // video.cpp
        if ( !video_revealed() || video_cover_active() ) l = r = 0;  // the steps between the splash and the main menu are silent (the start-up cover has its own sound)
        mix_l[i] = l, mix_r[i] = r;
    }
    {
        std::lock_guard<std::mutex> g( g_overlay_lock );
        for ( Overlay &o : g_overlays )
            for ( int i = 0; i < N && o.pos + 1 < o.pcm->size() && o.gain > 0; ++i, o.pos += 2 )
            {
                if ( o.loop && o.pos + 3 >= o.pcm->size() ) o.pos = 0;
                mix_l[i] += ( *o.pcm )[o.pos] * o.gain, mix_r[i] += ( *o.pcm )[o.pos + 1] * o.gain;
                o.gain -= o.fade;
            }
        std::erase_if( g_overlays, []( const Overlay &o ) { return o.pos + 1 >= o.pcm->size() || o.gain <= 0; } );
    }
    static uint32_t clipped;  // samples past full scale (cut off: heard as distortion), logged once a second
    static float clip_peak;
    static uint32_t clip_frames;
    for ( int i = 0; i < N; ++i )
    {
        float l = mix_l[i], r = mix_r[i];
        out[i * 2] = std::clamp( l, -1.0f, 1.0f );
        out[i * 2 + 1] = std::clamp( r, -1.0f, 1.0f );
        peak = std::max( { peak, std::fabs( l ), std::fabs( r ) } );
        if ( std::fabs( l ) > 1.0f || std::fabs( r ) > 1.0f ) ++clipped, clip_peak = std::max( { clip_peak, std::fabs( l ), std::fabs( r ) } );
    }
    if ( ( clip_frames += N ) >= 48000 )
    {
        if ( clipped ) rt_log( "audio: %u samples clipped in the last second (peak %.2f)\n", clipped, clip_peak );
        clipped = 0, clip_peak = 0, clip_frames = 0;
    }
    void video_record_audio( const float *s, size_t n );  // video.cpp
    video_record_audio( out, (size_t)N * 2 );
    if ( log )
    {
        static float window_peak;
        static uint32_t frames, loud;
        static auto last = std::chrono::steady_clock::now();
        window_peak = std::max( window_peak, peak );
        ++frames;
        if ( peak > 1e-4f ) ++loud;
        auto now = std::chrono::steady_clock::now();
        if ( now - last > std::chrono::seconds( 5 ) )
        {
            rt_log( "audio: %u frames, %u with signal, peak %.3f\n", frames, loud, window_peak );
            window_peak = 0;
            frames = loud = 0;
            last = now;
        }
    }
    g_samples += N;
    // CRACKDOWN_AUDIO_DUMP=file: the stereo mix as raw 48 kHz float samples (testing; works headless).
    static FILE *dump = getenv( "CRACKDOWN_AUDIO_DUMP" ) ? fopen( getenv( "CRACKDOWN_AUDIO_DUMP" ), "wb" ) : nullptr;
    if ( dump ) { fwrite( out, sizeof( out ), 1, dump ); fflush( dump ); }
    if ( !g_enabled ) return;
    // Keep latency bounded: the game's 5.33 ms clock and the device clock drift apart slowly. Past 125 ms
    // queued the queue is emptied (the oldest sound goes, not the frame just made: it may hold a sound
    // that has only just started -- a menu click). Overflows and the device running dry (the queue empty
    // when a frame comes) are logged at most once a second.
    static uint32_t cleared, starved;
    static auto last = std::chrono::steady_clock::now();
    const int queued = SDL_GetAudioStreamQueued( g_stream );
    if ( static const bool gap_log = getenv( "CRACKDOWN_AUDIO_GAPS" ); gap_log )  // the backlog's range every 5 s, in ms
    {
        static int lo = INT32_MAX, hi = 0;
        static auto since = std::chrono::steady_clock::now();
        lo = std::min( lo, queued ), hi = std::max( hi, queued );
        if ( std::chrono::steady_clock::now() - since > std::chrono::seconds( 5 ) )
        {
            const double ms = 1000.0 / ( 48000 * 2 * sizeof( float ) );
            rt_log( "audio: output backlog %.1f-%.1f ms (device period %.1f ms)\n", lo * ms, hi * ms, g_device_frames / 48.0 );
            lo = INT32_MAX, hi = 0, since = std::chrono::steady_clock::now();
        }
    }
    if ( queued > (int)( 48000 * 2 * sizeof( float ) / 8 ) ) SDL_ClearAudioStream( g_stream ), ++cleared;
    else if ( queued == 0 ) ++starved;
    if ( ( cleared || starved ) && std::chrono::steady_clock::now() - last > std::chrono::seconds( 1 ) )
    {
        rt_log( "audio: output queue %u times over 125 ms (emptied), %u times empty\n", cleared, starved );
        cleared = starved = 0;
        last = std::chrono::steady_clock::now();
    }
    SDL_PutAudioStreamData( g_stream, out, sizeof( out ) );
}

// The audio engine's worker threads (master 82AD9ED8, helpers 82AD9FA0) quit when they wake and find
// the render callback's owner (engine +300) cleared. The callback (82ADA050) clears it after its
// frame, also when it stopped waiting for the master (its wait also ends on event 82DBE48C); a master
// held up past that (a critical section held through a movie change) then woke to a cleared owner and
// quit for good: audio stopped and the movie player crashed on a freed object. The game never shuts
// the engine down, so a worker that returns starts waiting again.
extern "C" PPC_FUNC( __imp__sub_82AD9ED8 );
extern "C" PPC_FUNC( __imp__sub_82AD9FA0 );

PPC_FUNC( sub_82AD9ED8 )
{
    for ( ;; )
    {
        __imp__sub_82AD9ED8( ctx, base, frame );
        rt_log( "audio: master worker woke to no frame; waiting again\n" );
    }
}

PPC_FUNC( sub_82AD9FA0 )
{
    for ( ;; ) __imp__sub_82AD9FA0( ctx, base, frame );
}

// Before it loads new sound banks (the downloadable content's voice lines, after START), the game
// stops every voice and polls each (82AD2860, its state) until the audio engine has stopped it, which
// takes the engine some frames: about 60 ms at a frame per 5.33 ms, one frame of the front end. While
// it waits, the render clock (system.cpp) asks for frames back to back.
extern std::atomic<bool> g_audio_hurry;
extern "C" PPC_FUNC( __imp__sub_82275BA0 );
void title_music_unmute();  // xam.cpp
PPC_FUNC( sub_82275BA0 )
{
    title_music_unmute();  // every sound is stopped here; the music starts again after
    g_audio_hurry = true;
    __imp__sub_82275BA0( ctx, base, frame );
    g_audio_hurry = false;
}
