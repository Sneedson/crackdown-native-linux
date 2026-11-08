// Presentation: the window, and copying the front buffer the game swaps to the screen.
// The GPU executes the swap packet VdSwap wrote; it carries the front buffer's texture fetch
// constant, which says where the image is, its size, format and whether it is tiled.
// CRACKDOWN_HEADLESS=1: no window. CRACKDOWN_DUMP_FRAME=N: write frame N to build/frame-N.ppm.
#include "ppc_config.h"
#include "ppc_context.h"
#include "runtime.h"
#include "settings.h"
#include "input.h"
#include "keybinds.h"
#include "profile.h"
uint32_t renderer_pipelines_pending();  // renderer.cpp: pipelines still being made
void xui_request_pause();               // xui.cpp: Esc in game

double input_script_clock();  // xam.cpp

#include <SDL3/SDL.h>
extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}
#include <memory>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

bool keyboard_scripted_key( int scancode );  // keyboard.cpp: CRACKDOWN_KEY_SCRIPT
uint64_t audio_sample_count();  // audio.cpp

namespace
{

std::mutex g_frame_lock;
std::condition_variable g_frame_cv;
std::vector<uint32_t> g_frame;  // RGBA8888 (SDL_PIXELFORMAT_ABGR8888 byte order: R, G, B, A)
// Pixel layouts a presented frame comes in: R, G, B, A bytes (converted, FSR's), B, G, R, A (the
// guest's 8-8-8-8 unswapped) and A, R, G, B (8-in-32 swapped, the usual).
enum PixelOrder : uint32_t { ORDER_RGBA, ORDER_BGRA, ORDER_ARGB };

uint32_t g_frame_w, g_frame_h;
uint64_t g_frame_seq;
const uint32_t *g_frame_raw;  // instead of g_frame: a readback slot, in g_frame_order (w x h packed)
uint32_t g_frame_order;
std::atomic<bool> g_quit{ false };

// Keyboard and mouse -> controller 0 (read by XamInputGetState). Keys: WASD move, Space A (jump),
// E B, R X, F Y, Q LB, G RB, Ctrl/C left stick click, Tab / M (Map) BACK in game / B in menus, Enter START + A, Esc START in game / B in menus,
// arrows d-pad.
// Mouse (captured by clicking in the window, released with Left Alt): movement turns the camera directly
// (mouselook.cpp; a right stick where that isn't running), left button RT (fire), right button LT (lock on), middle button right stick click,
// wheel d-pad up/down.
std::atomic<uint32_t> g_pad_buttons{ 0 }, g_pad_triggers{ 0 };
std::atomic<int32_t> g_pad_lx{ 0 }, g_pad_ly{ 0 };
std::atomic<int32_t> g_gamepad_rx{ 0 }, g_gamepad_ry{ 0 };  // a real controller's right stick (XInput signs)
// One input at a time: the keyboard and mouse, or a controller, whichever was used last. A key, a mouse
// move, click or wheel picks the keyboard and mouse, and the controller isn't read at all (a stick resting
// off centre moved menu selections); a controller button, or a stick or trigger pushed well past rest,
// picks the controller, and the mouse pointer then leaves the menu focus alone.
std::atomic<bool> g_using_pad{ false };
std::atomic<uint32_t> g_menu_vk{ 0 };  // CRACKDOWN_KEYBOARD_VK: menu keys held (bits as keyboard_vk_state)
SDL_Gamepad *g_gamepad;  // the first connected controller (window thread only)
std::atomic<float> g_mouse_dx{ 0 }, g_mouse_dy{ 0 };   // relative motion not yet turned into stick input
std::atomic<float> g_look_dx{ 0 }, g_look_dy{ 0 };     // the same motion for mouse look (mouselook.cpp)
std::atomic<int64_t> g_look_seen{ 0 };                 // when mouse look last took motion (steady clock, ms)
std::atomic<int64_t> g_vehicle_seen{ 0 };              // when mouse look last stood aside for a vehicle (ms)
std::atomic<int64_t> g_player_seen{ 0 };               // when the player's update last ran (ms): in game, not in a menu
std::atomic<int> g_wheel_frames{ 0 };                   // > 0: d-pad up held, < 0: down
std::atomic<bool> g_mouse_captured{ false };
// Buttons held when the mouse was captured (the capturing click, a menu's last click): kept from the game
// until released, so taking the mouse never fires. g_mouse_freed: Left Alt let it go (no recapture on a
// menu closing until a click takes it again).
std::atomic<uint32_t> g_capture_held{ 0 };
std::atomic<bool> g_mouse_freed{ false };
// Menus: the pointer in the game's 1280x720 coordinates (no capture while a menu is up).
std::atomic<bool> g_menu_control{ false };  // the menu up has its focus on a control (xui.cpp)
std::atomic<bool> g_menu_active{ false }, g_menu_left{ false }, g_menu_right{ false }, g_window_up{ false };
std::atomic<float> g_menu_x{ 0 }, g_menu_y{ 0 };
std::atomic<uint32_t> g_menu_moves{ 0 };
std::atomic<int> g_menu_wheel{ 0 };

// Keys and mouse buttons for the game's actions (keyboard.cpp), copied each poll while the window has
// the keyboard; mouse buttons only while the mouse is captured.
std::atomic<uint8_t> g_keys[SDL_SCANCODE_COUNT];
std::atomic<uint32_t> g_action_mouse{ 0 };  // SDL_BUTTON_*MASK
std::atomic<bool> g_capture_request{ false };  // F12: log the next frame's draws and write it out
std::atomic<bool> g_recording{ false };        // F11: presented frames go to build/rec-<n>.mkv until F11 again (mkv: still readable if the game is closed mid-recording)
std::atomic<bool> g_revealed{ true };
// The start-up cover (Skip Intro): from the first frame until the game's menus come up, the window shows the
// game's own loading movie (the spinning Agency logo, XUIFrontEnd/Loading.bik, looped) with the game's
// loading-screen sound (cover_thread) instead of the splash's white and the hidden steps.
std::atomic<bool> g_cover{ false };
std::mutex g_cover_m;
std::vector<uint32_t> g_cover_px;  // R, G, B, A from the low byte up
uint32_t g_cover_w, g_cover_h;
std::atomic<uint64_t> g_cover_seq{ 0 };
std::vector<std::shared_ptr<const std::vector<float>>> g_cover_sounds;
float g_cover_gain;  // false: the window holds its last frame and the game is silent (between the splash and the main menu)
std::atomic<int> g_action_wheel{ 0 };        // wheel steps in game, not yet taken

bool player_active()
{
    int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>( std::chrono::steady_clock::now().time_since_epoch() ).count();
    return now - g_player_seen.load() < 200;
}

}  // namespace
// The Keyboard Controls page (keyspage.cpp).
bool keyspage_open();
void keyspage_mouse_move( float x, float y );
void keyspage_mouse_button( int sdl_button );
void keyspage_key( int scancode );
const std::vector<uint32_t> *keyspage_picture( uint64_t &seq );
void keyspage_dump();
namespace
{
std::atomic<bool> g_keys_held_back{ false };  // after the page: the game gets no keys until all are up

void poll_keyboard()
{
    // The keyboard, with CRACKDOWN_KEY_SCRIPT's keys held as if pressed (tests reach the menu mapping too).
    static bool keys[SDL_SCANCODE_COUNT];
    static const bool script = getenv( "CRACKDOWN_KEY_SCRIPT" ) != nullptr;
    const bool *k = SDL_GetKeyboardState( nullptr );
    if ( script )
    {
        for ( int i = 0; i < SDL_SCANCODE_COUNT; ++i ) keys[i] = ( k && k[i] ) || keyboard_scripted_key( i );
        k = keys;
    }
    // The Keyboard Controls page takes the keyboard while it is up, and the keys held as it closes (the Esc
    // that closed it) stay the page's until they are let go: they don't go on to the menu.
    static bool none[SDL_SCANCODE_COUNT];
    if ( keyspage_open() ) g_keys_held_back = true;
    else if ( g_keys_held_back )
    {
        bool held = false;
        for ( int i = 0; i < SDL_SCANCODE_COUNT; ++i ) held |= k[i];
        if ( !held ) g_keys_held_back = false;
    }
    if ( g_keys_held_back ) k = none;
    bool any_key = false;
    for ( int i = 0; i < SDL_SCANCODE_COUNT; ++i ) g_keys[i] = k[i], any_key |= k[i];
    if ( any_key && g_using_pad.exchange( false ) ) rt_log( "input: keyboard and mouse\n" );
    {
        const uint32_t held = g_mouse_captured ? (uint32_t)SDL_GetMouseState( nullptr, nullptr ) : 0;
        g_capture_held = g_capture_held & held;  // released: the button is the game's again
        g_action_mouse = held & ~g_capture_held;
    }
    // In game (the player's update ran in the last 200 ms; it stops while paused) keys and buttons are
    // the game's actions (keyboard.cpp). Menus get a controller: arrows, Enter = A (and START on screens
    // without a control to focus: "Press START"), Esc and the Map keys = back, wheel = d-pad. A menu with a
    // control focused is a menu even while the game runs on behind it (a supply point's menus and map
    // don't pause: the keys went to the player's actions and left the map's empty boss list with no way out).
    bool in_game = player_active() && !g_menu_control;
    if ( static const bool klog = getenv( "CRACKDOWN_KEY_LOG" ); klog )  // debug: what the keys are taken as
    {
        static bool last_any;
        if ( any_key != last_any ) {
            rt_log( "input: keys %s, in game %d, menu active %d, sample %llu\n", any_key ? "down" : "up", in_game, g_menu_active.load(), (unsigned long long)audio_sample_count() );
        }
        last_any = any_key;
    }
    static bool esc_down;
    const bool esc_was_down = esc_down;
    esc_down = k[SDL_SCANCODE_ESCAPE];
    uint32_t b = 0;  // (a real controller's buttons, below: the keyboard and mouse never press any)
    // Menus get the keyboard as a keyboard: its keys as keyboard keystrokes (xam.cpp), which XUI takes
    // natively (arrows move and switch tabs, Return chooses, Esc backs out, with the game's sounds).
    // In game Esc opens the pause menu through the game's own handling (xui.cpp xui_pause_poll).
    // Esc and the Map keys (Tab, M) act where they went down: one held from a menu into the game (Esc
    // closing the pause menu) doesn't pause again, one held from the game into a menu doesn't back out of
    // it. In a menu a Map key is Esc: the key that opened the map closes it.
    static bool esc_from_game, tab_from_game, was_tab;
    bool tab_down = false;
    if ( static const int map = keybinds::find( "map" ); map >= 0 )
        for ( int key : keybinds::command( map ).keys )
            if ( key > 0 && key < SDL_SCANCODE_COUNT && k[key] ) tab_down = true;
    if ( esc_down && !esc_was_down ) esc_from_game = in_game;
    // Esc in game: the game's own "Pause Button" handling (xui.cpp xui_pause_poll), as START does.
    if ( esc_down && !esc_was_down && in_game ) xui_request_pause();
    if ( tab_down && !was_tab ) tab_from_game = in_game;
    was_tab = tab_down;
    uint32_t vk = 0;
    if ( !in_game )
    {
        if ( k[SDL_SCANCODE_UP] ) vk |= 1u << 0;
        if ( k[SDL_SCANCODE_DOWN] ) vk |= 1u << 1;
        if ( k[SDL_SCANCODE_LEFT] ) vk |= 1u << 2;
        if ( k[SDL_SCANCODE_RIGHT] ) vk |= 1u << 3;
        if ( k[SDL_SCANCODE_RETURN] || k[SDL_SCANCODE_KP_ENTER] ) vk |= 1u << 4;
        if ( ( esc_down && !esc_from_game ) || ( tab_down && !tab_from_game ) ) vk |= 1u << 5;
        if ( k[SDL_SCANCODE_SPACE] ) vk |= 1u << 7;
        if ( k[SDL_SCANCODE_BACKSPACE] ) vk |= 1u << 8;
    }
    // In game Tab and M open the map only as the game's "Display Map" action (keyboard.cpp), as BACK
    // does: the game holds it back until the HUD's scene is up. A VK_TAB keystroke opened it earlier too,
    // with no scene to go back to, and then nothing (Esc, M, B) could close it.
    g_menu_vk = vk;
    if ( in_game ) g_wheel_frames = 0;
    // A real controller (SDL's y axes point down, XInput's up): read only while it is the input in use.
    uint32_t triggers = 0;
    int32_t lx = 0, ly = 0, rx = 0, ry = 0;
    if ( g_gamepad && !g_using_pad )
    {
        bool pressed = false;
        for ( int bt = 0; bt < SDL_GAMEPAD_BUTTON_COUNT; ++bt ) pressed |= SDL_GetGamepadButton( g_gamepad, (SDL_GamepadButton)bt );
        for ( SDL_GamepadAxis a : { SDL_GAMEPAD_AXIS_LEFTX, SDL_GAMEPAD_AXIS_LEFTY, SDL_GAMEPAD_AXIS_RIGHTX, SDL_GAMEPAD_AXIS_RIGHTY,
                                    SDL_GAMEPAD_AXIS_LEFT_TRIGGER, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER } )
            pressed |= std::abs( (int32_t)SDL_GetGamepadAxis( g_gamepad, a ) ) > 20000;
        if ( pressed && !any_key ) { g_using_pad = true; rt_log( "input: controller\n" ); }
    }
    if ( g_gamepad && g_using_pad )
    {
        b = 0;  // the keyboard's menu keys aren't the input now either
        static const struct { SDL_GamepadButton sdl; uint32_t bit; } map[] = {
            { SDL_GAMEPAD_BUTTON_DPAD_UP, 0x1 }, { SDL_GAMEPAD_BUTTON_DPAD_DOWN, 0x2 }, { SDL_GAMEPAD_BUTTON_DPAD_LEFT, 0x4 },
            { SDL_GAMEPAD_BUTTON_DPAD_RIGHT, 0x8 }, { SDL_GAMEPAD_BUTTON_START, 0x10 }, { SDL_GAMEPAD_BUTTON_BACK, 0x20 },
            { SDL_GAMEPAD_BUTTON_LEFT_STICK, 0x40 }, { SDL_GAMEPAD_BUTTON_RIGHT_STICK, 0x80 }, { SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, 0x100 },
            { SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, 0x200 }, { SDL_GAMEPAD_BUTTON_SOUTH, 0x1000 }, { SDL_GAMEPAD_BUTTON_EAST, 0x2000 },
            { SDL_GAMEPAD_BUTTON_WEST, 0x4000 }, { SDL_GAMEPAD_BUTTON_NORTH, 0x8000 } };
        for ( auto &m : map ) if ( SDL_GetGamepadButton( g_gamepad, m.sdl ) ) b |= m.bit;
        auto axis = [&]( SDL_GamepadAxis a ) { return (int32_t)SDL_GetGamepadAxis( g_gamepad, a ); };
        auto trigger = [&]( SDL_GamepadAxis a ) { return (uint32_t)std::max( 0, axis( a ) ) * 255 / 32767; };
        triggers = trigger( SDL_GAMEPAD_AXIS_LEFT_TRIGGER ) | trigger( SDL_GAMEPAD_AXIS_RIGHT_TRIGGER ) << 8;
        auto flip = []( int32_t v ) { return std::min( 32767, -v ); };
        lx = axis( SDL_GAMEPAD_AXIS_LEFTX ); ly = flip( axis( SDL_GAMEPAD_AXIS_LEFTY ) );
        rx = axis( SDL_GAMEPAD_AXIS_RIGHTX ); ry = flip( axis( SDL_GAMEPAD_AXIS_RIGHTY ) );
    }
    g_pad_buttons = b;
    g_pad_triggers = triggers;
    g_pad_lx = lx;
    g_pad_ly = ly;
    g_gamepad_rx = rx;
    g_gamepad_ry = ry;
}

void mouse_event( SDL_Window *window, SDL_Renderer *renderer, const SDL_Event &event )
{
    SDL_Event e = event;
    SDL_ConvertEventToRenderCoordinates( renderer, &e );  // window pixels -> the frame's pixels
    int logical_w = 1280, logical_h = 720;
    SDL_GetRenderLogicalPresentation( renderer, &logical_w, &logical_h, nullptr );
    if ( logical_w > 1280 && logical_h > 720 && ( e.type == SDL_EVENT_MOUSE_MOTION || e.type == SDL_EVENT_MOUSE_BUTTON_DOWN || e.type == SDL_EVENT_MOUSE_BUTTON_UP ) )
    {
        float *x = e.type == SDL_EVENT_MOUSE_MOTION ? &e.motion.x : &e.button.x, *y = e.type == SDL_EVENT_MOUSE_MOTION ? &e.motion.y : &e.button.y;
        *x *= 1280.0f / logical_w, *y *= 720.0f / logical_h;  // back to the game's 1280x720
        if ( e.type == SDL_EVENT_MOUSE_MOTION ) e.motion.xrel *= 1280.0f / logical_w, e.motion.yrel *= 720.0f / logical_h;
    }
    static const bool log = getenv( "CRACKDOWN_MOUSE_LOG" );
    if ( log && ( e.type == SDL_EVENT_MOUSE_BUTTON_DOWN || e.type == SDL_EVENT_MOUSE_BUTTON_UP ) )
        rt_log( "mouse: button %d %s at %.0f,%.0f (window %.0f,%.0f) captured %d menu %d\n", e.button.button,
                e.type == SDL_EVENT_MOUSE_BUTTON_DOWN ? "down" : "up", e.button.x, e.button.y, event.button.x,
                event.button.y, (int)g_mouse_captured, (int)g_menu_active );
    if ( e.type == SDL_EVENT_MOUSE_MOTION || e.type == SDL_EVENT_MOUSE_BUTTON_DOWN || e.type == SDL_EVENT_MOUSE_WHEEL )
        if ( g_using_pad.exchange( false ) ) rt_log( "input: keyboard and mouse\n" );
    switch ( e.type )
    {
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
        if ( !g_mouse_captured )
        {
            bool down = e.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
            if ( e.button.button == SDL_BUTTON_LEFT ) g_menu_left = down;
            if ( e.button.button == SDL_BUTTON_RIGHT ) g_menu_right = down;
            if ( down && !g_menu_active )
            {
                SDL_SetWindowRelativeMouseMode( window, true );
                g_capture_held = SDL_GetMouseState( nullptr, nullptr ) | SDL_BUTTON_MASK( e.button.button );
                g_mouse_captured = true, g_mouse_freed = false;
                g_menu_left = g_menu_right = false;
            }
        }
        break;
    case SDL_EVENT_KEY_DOWN:
        if ( e.key.scancode == SDL_SCANCODE_LALT && g_mouse_captured ) { SDL_SetWindowRelativeMouseMode( window, false ); g_mouse_captured = false; g_mouse_freed = true; }
        if ( e.key.scancode == SDL_SCANCODE_F12 && !e.key.repeat ) { g_capture_request = true; rt_log( "video: F12 capture requested\n" ); }
        if ( e.key.scancode == SDL_SCANCODE_F11 && !e.key.repeat ) { g_recording = !g_recording; rt_log( "video: F11 recording %s\n", g_recording ? "on" : "off" ); }
        break;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        if ( g_mouse_captured ) { SDL_SetWindowRelativeMouseMode( window, false ); g_mouse_captured = false; }
        g_mouse_freed = false;  // Alt-Tab: its Alt was the way out, not a wish to have the pointer
        break;
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
        // Back in the window (Alt-Tab, a click) during play: the mouse is taken again by itself; a button held
        // then (the click that brought the window up) stays out of the game until released.
        if ( !g_mouse_captured && !g_menu_active && !g_using_pad )
        {
            SDL_SetWindowRelativeMouseMode( window, true );
            g_capture_held = SDL_GetMouseState( nullptr, nullptr ) | SDL_BUTTON_LMASK | SDL_BUTTON_RMASK;
            g_mouse_captured = true, g_mouse_freed = false;
            g_menu_left = g_menu_right = false;
            rt_log( "video: mouse taken again (window focused)\n" );
        }
        break;
    case SDL_EVENT_MOUSE_MOTION:
        if ( g_mouse_captured )
        {
            g_mouse_dx = g_mouse_dx + event.motion.xrel;
            g_mouse_dy = g_mouse_dy + event.motion.yrel;
            g_look_dx = g_look_dx + event.motion.xrel;
            g_look_dy = g_look_dy + event.motion.yrel;
        }
        else { g_menu_x = e.motion.x; g_menu_y = e.motion.y; ++g_menu_moves; }
        break;
    case SDL_EVENT_MOUSE_WHEEL:
        if ( g_mouse_captured && e.wheel.integer_y ) g_action_wheel += e.wheel.integer_y;  // in game: actions (keyboard.cpp)
        if ( !g_mouse_captured && e.wheel.integer_y ) g_menu_wheel += e.wheel.integer_y;
        break;
    default: break;
    }
}

std::atomic<uint32_t> g_display_w, g_display_h;

}  // namespace
std::string disc_file_path( const char *rel );                                   // files.cpp
std::vector<float> sound_file_entry_pcm( const std::string &path, size_t index );  // soundbanks.cpp
void audio_play_overlay( std::shared_ptr<const std::vector<float>> pcm, float gain );  // audio.cpp
void audio_fade_overlay( const std::vector<float> *pcm, float seconds );
bool audio_overlay_playing( const std::vector<float> *pcm );
void audio_loop_overlay( std::shared_ptr<const std::vector<float>> pcm, float gain );
namespace
{

// The cover's movie, decoded on a thread of its own at the movie's rate, round and round while the cover is up.
void cover_thread()
{
    const std::string path = disc_file_path( "XUIFrontEnd/Loading.bik" );
    // The sound: the game's own loading-screen sound (between the supply point choice and the spawn), three
    // Generic.xwb loops played together -- #499 (2.94 s), #521 (1.99 s), #525 (1.20 s) -- in the proportions
    // and at the level the game mixes them (fitted to its recorded drone: #521 nearly alone, the others at
    // 5% and 2.5% of it; RMS about 0.04 together).
    std::thread( [] {
        const std::string bank = disc_file_path( "Audio/Generic.xwb" );
        std::vector<std::shared_ptr<const std::vector<float>>> parts;
        static const float mix[3] = { 0.05f, 1.0f, 0.025f };
        for ( size_t index : { 499, 521, 525 } )
            if ( auto pcm = std::make_shared<std::vector<float>>( sound_file_entry_pcm( bank, index ) ); pcm->size() >= 2 ) parts.push_back( pcm );
        if ( const char *dir = getenv( "CRACKDOWN_COVER_DUMP" ) )  // debug: the decoded loops as raw float files
            for ( size_t i = 0; i < parts.size(); ++i )
                if ( FILE *f = fopen( ( std::string( dir ) + "/loop" + std::to_string( i ) + ".f32" ).c_str(), "wb" ) )
                    fwrite( parts[i]->data(), sizeof( float ), parts[i]->size(), f ), fclose( f );
        if ( parts.size() != 3 || !g_cover ) return;
        // Their level together over six seconds, each one looping.
        const size_t n = 6 * 48000 * 2;
        double sum = 0;
        for ( size_t i = 0; i < n; ++i )
        {
            float v = 0;
            for ( size_t k = 0; k < parts.size(); ++k ) v += mix[k] * ( *parts[k] )[i % ( parts[k]->size() & ~(size_t)1 )];
            sum += (double)v * v;
        }
        const float rms = (float)std::sqrt( sum / n );
        if ( rms <= 0 ) return;
        g_cover_gain = 0.04f / rms;
        g_cover_sounds = parts;
        for ( size_t k = 0; k < parts.size(); ++k ) audio_loop_overlay( parts[k], g_cover_gain * mix[k] );
        rt_log( "video: start-up cover sound (%zu loops)\n", parts.size() );
    } ).detach();
    unsigned loops = 0;
    while ( g_cover )
    {
        AVFormatContext *fmt = nullptr;
        if ( avformat_open_input( &fmt, path.c_str(), nullptr, nullptr ) < 0 ) { rt_log( "video: start-up cover: can't open %s\n", path.c_str() ); return; }
        avformat_find_stream_info( fmt, nullptr );
        const int vs = av_find_best_stream( fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0 );
        const AVCodec *codec = vs >= 0 ? avcodec_find_decoder( fmt->streams[vs]->codecpar->codec_id ) : nullptr;
        AVCodecContext *cc = codec ? avcodec_alloc_context3( codec ) : nullptr;
        if ( !cc || avcodec_parameters_to_context( cc, fmt->streams[vs]->codecpar ) < 0 || avcodec_open2( cc, codec, nullptr ) < 0 )
        {
            rt_log( "video: start-up cover: no decoder for %s\n", path.c_str() );
            avcodec_free_context( &cc );
            avformat_close_input( &fmt );
            return;
        }
        double fps = av_q2d( fmt->streams[vs]->r_frame_rate );
        if ( fps < 1 || fps > 120 ) fps = 30;
        AVPacket *pkt = av_packet_alloc();
        AVFrame *fr = av_frame_alloc();
        auto next = std::chrono::steady_clock::now();
        std::vector<uint32_t> px;
        while ( g_cover && av_read_frame( fmt, pkt ) >= 0 )
        {
            if ( pkt->stream_index == vs && avcodec_send_packet( cc, pkt ) >= 0 )
                while ( g_cover && avcodec_receive_frame( cc, fr ) == 0 )
                {
                    // YUV 4:2:0 (BT.601) to RGBA.
                    const int w = fr->width, h = fr->height;
                    px.resize( (size_t)w * h );
                    for ( int y = 0; y < h; ++y )
                    {
                        const uint8_t *Y = fr->data[0] + (size_t)y * fr->linesize[0], *U = fr->data[1] + (size_t)( y / 2 ) * fr->linesize[1],
                                      *V = fr->data[2] + (size_t)( y / 2 ) * fr->linesize[2];
                        uint32_t *d = px.data() + (size_t)y * w;
                        for ( int x = 0; x < w; ++x )
                        {
                            const float yy = 1.164f * ( Y[x] - 16 ), u = U[x / 2] - 128.0f, v = V[x / 2] - 128.0f;
                            auto c = []( float f ) { return (uint32_t)std::clamp( f, 0.0f, 255.0f ); };
                            d[x] = c( yy + 1.596f * v ) | c( yy - 0.392f * u - 0.813f * v ) << 8 | c( yy + 2.017f * u ) << 16 | 0xFF000000u;
                        }
                    }
                    {
                        std::lock_guard<std::mutex> g( g_cover_m );
                        g_cover_px.swap( px );
                        g_cover_w = w, g_cover_h = h;
                    }
                    ++g_cover_seq;
                    av_frame_unref( fr );
                    next += std::chrono::microseconds( (int64_t)( 1e6 / fps ) );
                    std::this_thread::sleep_until( next );
                }
            av_packet_unref( pkt );
        }
        av_packet_free( &pkt );
        av_frame_free( &fr );
        avcodec_free_context( &cc );
        avformat_close_input( &fmt );
        ++loops;
    }
    rt_log( "video: start-up cover ended after %u loops of the movie\n", loops );
}

void window_thread()
{
    if ( !SDL_Init( SDL_INIT_VIDEO | SDL_INIT_GAMEPAD ) ) { rt_log( "video: SDL_Init failed: %s\n", SDL_GetError() ); return; }
    if ( const SDL_DisplayMode *mode = SDL_GetDesktopDisplayMode( SDL_GetPrimaryDisplay() ) ) g_display_w = mode->w, g_display_h = mode->h;
    SDL_Window *window = SDL_CreateWindow( "Crackdown (native recomp)", 1280, 720, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY );
    SDL_Renderer *renderer = window ? SDL_CreateRenderer( window, nullptr ) : nullptr;
    if ( !renderer ) { rt_log( "video: no window: %s\n", SDL_GetError() ); return; }
    SDL_SetRenderLogicalPresentation( renderer, 1280, 720, SDL_LOGICAL_PRESENTATION_LETTERBOX );
    g_window_up = true;
    uint32_t applied = 0;  // the settings version the window has (fullscreen, vsync)
    SDL_Texture *texture = nullptr;
    uint32_t tw = 0, th = 0;
    uint64_t shown = 0;
    bool show_fps = false;
    uint64_t fps_from = 0;
    float fps = 0;
    auto fps_time = std::chrono::steady_clock::now();
    while ( !g_quit )
    {
        SDL_Event e;
        while ( SDL_PollEvent( &e ) )
        {
            if ( e.type == SDL_EVENT_QUIT ) { rt_log( "video: window closed\n" ); _exit( 0 ); }
            if ( e.type == SDL_EVENT_GAMEPAD_ADDED && !g_gamepad && ( g_gamepad = SDL_OpenGamepad( e.gdevice.which ) ) )
                rt_log( "video: controller '%s' connected\n", SDL_GetGamepadName( g_gamepad ) );
            if ( e.type == SDL_EVENT_GAMEPAD_REMOVED && g_gamepad && SDL_GetGamepadID( g_gamepad ) == e.gdevice.which )
            {
                SDL_CloseGamepad( g_gamepad );
                g_gamepad = nullptr;
                rt_log( "video: controller disconnected\n" );
            }
            if ( keyspage_open() )  // the page has the mouse and keyboard
            {
                if ( g_mouse_captured ) { SDL_SetWindowRelativeMouseMode( window, false ); g_mouse_captured = false; }
                if ( e.type == SDL_EVENT_MOUSE_MOTION || e.type == SDL_EVENT_MOUSE_BUTTON_DOWN )
                {
                    SDL_Event c = e;
                    SDL_ConvertEventToRenderCoordinates( renderer, &c );  // the frame's pixels
                    int lw = 1280, lh = 720;
                    SDL_GetRenderLogicalPresentation( renderer, &lw, &lh, nullptr );
                    const float x = ( e.type == SDL_EVENT_MOUSE_MOTION ? c.motion.x : c.button.x ) * 1920.0f / std::max( lw, 1 );
                    const float y = ( e.type == SDL_EVENT_MOUSE_MOTION ? c.motion.y : c.button.y ) * 1080.0f / std::max( lh, 1 );
                    keyspage_mouse_move( x, y );
                    if ( e.type == SDL_EVENT_MOUSE_BUTTON_DOWN ) keyspage_mouse_button( e.button.button );
                }
                else if ( e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat ) keyspage_key( e.key.scancode );
                else if ( e.type != SDL_EVENT_MOUSE_BUTTON_UP && e.type != SDL_EVENT_KEY_UP && e.type != SDL_EVENT_MOUSE_WHEEL ) mouse_event( window, renderer, e );
                g_menu_left = g_menu_right = false;
                continue;
            }
            mouse_event( window, renderer, e );
        }
        if ( g_menu_active && g_mouse_captured ) { SDL_SetWindowRelativeMouseMode( window, false ); g_mouse_captured = false; }
        // A menu closing into the game (Esc out of the pause menu, a dossier, the map) takes the mouse again
        // by itself, as the menu opening let it go; not after Left Alt freed it, nor in a window without focus.
        {
            static bool was_menu;
            const bool menu = g_menu_active;
            if ( was_menu && !menu && !g_mouse_captured && !g_mouse_freed && !g_using_pad && ( SDL_GetWindowFlags( window ) & SDL_WINDOW_INPUT_FOCUS ) )
            {
                SDL_SetWindowRelativeMouseMode( window, true );
                g_capture_held = SDL_GetMouseState( nullptr, nullptr );
                g_mouse_captured = true;
                g_menu_left = g_menu_right = false;
                rt_log( "video: mouse taken again (menu closed)\n" );
            }
            was_menu = menu;
        }
        if ( applied != settings_version() )
        {
            applied = settings_version();
            Settings set = settings();
            // Exclusive: the display's own mode at the desktop's size (no mode change, which this monitor blanks on).
            SDL_DisplayMode exclusive;
            SDL_DisplayID display = SDL_GetDisplayForWindow( window );
            const SDL_DisplayMode *desktop = SDL_GetDesktopDisplayMode( display );
            bool have_mode = set.fullscreen && set.exclusive && desktop && SDL_GetClosestFullscreenDisplayMode( display, desktop->w, desktop->h, desktop->refresh_rate, false, &exclusive );
            SDL_SetWindowFullscreenMode( window, have_mode ? &exclusive : nullptr );
            SDL_SetWindowFullscreen( window, set.fullscreen );
            SDL_SetRenderVSync( renderer, set.vsync ? 1 : 0 );
            show_fps = set.show_fps;
        }
        poll_keyboard();
        {
            // The frame is taken under the lock (a converted one by swapping buffers, a read-back one as
            // its slot) and written into the texture after it, so the game's next frame never waits here.
            // A read-back frame is converted here, off the game's threads, straight into the texture.
            static std::vector<uint32_t> mine;
            uint32_t fw = 0, fh = 0, order = ORDER_RGBA;
            const uint32_t *raw = nullptr;
            {
                std::unique_lock<std::mutex> lock( g_frame_lock );
                g_frame_cv.wait_for( lock, std::chrono::milliseconds( 16 ), [&] { return g_frame_seq != shown; } );
                if ( g_frame_seq != shown && g_frame_w && g_frame_h && g_revealed && !g_cover )
                {
                    shown = g_frame_seq, fw = g_frame_w, fh = g_frame_h, raw = g_frame_raw, order = g_frame_order;
                    if ( !raw ) mine.swap( g_frame );
                }
            }
            if ( fw && ( raw || mine.size() >= (size_t)fw * fh ) )
            {
                if ( !texture || tw != fw || th != fh )
                {
                    if ( texture ) SDL_DestroyTexture( texture );
                    tw = fw;
                    th = fh;
                    texture = SDL_CreateTexture( renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, tw, th );
                    SDL_SetRenderLogicalPresentation( renderer, tw, th, SDL_LOGICAL_PRESENTATION_LETTERBOX );
                }
                void *dst;
                int dpitch;
                if ( texture && SDL_LockTexture( texture, nullptr, &dst, &dpitch ) )
                {
                    const uint32_t *from = raw ? raw : mine.data();
                    // Into R, G, B, opaque from the low byte up: from A, R, G, B a shift, from B, G, R, A
                    // a swap (whole rows, which the compiler vectorises).
                    auto rows = [&]( auto conv ) {
                        for ( uint32_t y = 0; y < fh; ++y )
                        {
                            const uint32_t *__restrict s = from + (size_t)y * fw;
                            uint32_t *__restrict d = (uint32_t *)( (uint8_t *)dst + (size_t)y * dpitch );
                            for ( uint32_t x = 0; x < fw; ++x ) d[x] = conv( s[x] );
                        }
                    };
                    if ( order == ORDER_ARGB ) rows( []( uint32_t v ) { return ( v >> 8 ) | 0xFF000000u; } );
                    else if ( order == ORDER_BGRA ) rows( []( uint32_t v ) { return ( __builtin_bswap32( v ) >> 8 ) | 0xFF000000u; } );
                    else rows( []( uint32_t v ) { return v | 0xFF000000u; } );
                    SDL_UnlockTexture( texture );
                }
            }
        }
        if ( g_cover )  // the start-up cover's latest movie frame
        {
            static uint64_t cover_shown;
            if ( g_cover_seq != cover_shown )
            {
                std::lock_guard<std::mutex> g( g_cover_m );
                cover_shown = g_cover_seq;
                if ( !texture || tw != g_cover_w || th != g_cover_h )
                {
                    if ( texture ) SDL_DestroyTexture( texture );
                    tw = g_cover_w, th = g_cover_h;
                    texture = SDL_CreateTexture( renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, tw, th );
                    SDL_SetRenderLogicalPresentation( renderer, tw, th, SDL_LOGICAL_PRESENTATION_LETTERBOX );
                }
                if ( texture ) SDL_UpdateTexture( texture, nullptr, g_cover_px.data(), (int)tw * 4 );
            }
        }
        SDL_SetRenderDrawColor( renderer, 0, 0, 0, 255 );
        SDL_RenderClear( renderer );
        if ( texture ) SDL_RenderTexture( renderer, texture, nullptr, nullptr );
        if ( keyspage_open() )  // the Keyboard Controls page over the game
        {
            static SDL_Texture *page;
            static uint64_t page_seq;
            if ( !page ) page = SDL_CreateTexture( renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, 1920, 1080 ), SDL_SetTextureBlendMode( page, SDL_BLENDMODE_BLEND );
            if ( const std::vector<uint32_t> *px = keyspage_picture( page_seq ); px && page ) SDL_UpdateTexture( page, nullptr, px->data(), 1920 * 4 );
            if ( page ) SDL_RenderTexture( renderer, page, nullptr, nullptr );
        }
        // An achievement just unlocked (profile.h): the console's note, low in the middle.
        if ( tw )
        {
            static SDL_Texture *toast;
            static uint64_t toast_seq = ~0ull;
            uint64_t seq = 0;
            int w = 0, h = 0;
            float alpha = 0;
            if ( const std::vector<uint32_t> *px = profile_toast( seq, w, h, alpha ) )
            {
                if ( !toast ) toast = SDL_CreateTexture( renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, w, h ), SDL_SetTextureBlendMode( toast, SDL_BLENDMODE_BLEND );
                if ( toast && seq != toast_seq ) SDL_UpdateTexture( toast, nullptr, px->data(), w * 4 ), toast_seq = seq;
                if ( toast )
                {
                    const float k = tw / 1920.0f;
                    const SDL_FRect at{ ( tw - w * k ) / 2, th - ( h + 90 ) * k, w * k, h * k };
                    SDL_SetTextureAlphaModFloat( toast, alpha );
                    SDL_RenderTexture( renderer, toast, nullptr, &at );
                }
            }
        }
        if ( show_fps && tw )
        {
            auto now = std::chrono::steady_clock::now();
            if ( double s = std::chrono::duration<double>( now - fps_time ).count(); s >= 0.5 ) fps = (float)( ( shown - fps_from ) / s ), fps_from = shown, fps_time = now;
            char text[64];
            if ( const uint32_t pending = renderer_pipelines_pending() ) snprintf( text, sizeof( text ), "%.0f FPS - preparing shaders (%u)", fps, pending );
            else snprintf( text, sizeof( text ), "%.0f FPS", fps );
            float k = std::max( 2.0f, tw / 640.0f );
            SDL_SetRenderScale( renderer, k, k );
            SDL_SetRenderDrawColor( renderer, 0, 0, 0, 255 );
            SDL_RenderDebugText( renderer, 9, 9, text );
            SDL_SetRenderDrawColor( renderer, 255, 255, 0, 255 );
            SDL_RenderDebugText( renderer, 8, 8, text );
            SDL_SetRenderScale( renderer, 1, 1 );
        }
        SDL_RenderPresent( renderer );
    }
}

} // namespace

bool video_key( int scancode ) { return scancode >= 0 && scancode < SDL_SCANCODE_COUNT && g_keys[scancode]; }
uint32_t video_action_mouse() { return g_action_mouse; }
int video_take_wheel() { return g_action_wheel.exchange( 0 ); }
bool video_in_game() { return player_active(); }
bool video_menu_control() { return g_menu_control; }
bool video_using_controller() { return g_using_pad; }
bool video_gamepad_connected() { return g_gamepad != nullptr; }
uint32_t video_menu_keys() { return g_menu_vk; }
// F12 capture: the renderer takes the request at a frame's end (that next frame's draws are logged),
// and the next presented frame is written out.
namespace { std::atomic<bool> g_capture_dump{ false }; }
bool video_take_capture()
{
    if ( !g_capture_request.exchange( false ) ) return false;
    g_capture_dump = true;
    return true;
}
bool video_take_capture_dump() { return g_capture_dump.exchange( false ); }
void video_request_capture() { g_capture_request = true; }
void video_reveal( bool show ) { g_revealed = show; }
bool video_revealed() { return g_revealed; }

// Texel index of (x, y) in a 2D tiled surface (Xenos 32x32 macro tiles of 8x8 micro tiles);
// `log_bpp` = log2 of bytes per texel. This is the XDK's XGAddress2DTiledOffset.
uint32_t tiled_offset( uint32_t x, uint32_t y, uint32_t width, uint32_t log_bpp )
{
    uint32_t aligned_width = ( width + 31 ) & ~31u;
    uint32_t macro = ( ( x >> 5 ) + ( y >> 5 ) * ( aligned_width >> 5 ) ) << ( log_bpp + 7 );
    uint32_t micro = ( ( x & 7 ) + ( ( y & 6 ) << 2 ) ) << log_bpp;
    uint32_t offset = macro + ( ( micro & ~15u ) << 1 ) + ( micro & 15 ) + ( ( y & 8 ) << ( 3 + log_bpp ) ) + ( ( y & 1 ) << 4 );
    return ( ( ( offset & ~511u ) << 3 ) + ( ( offset & 448 ) << 2 ) + ( offset & 63 ) + ( ( y & 16 ) << 7 ) +
             ( ( ( ( ( y & 8 ) >> 2 ) + ( x >> 3 ) ) & 3 ) << 6 ) ) >> log_bpp;
}

void write_ppm( const char *path, const std::vector<uint32_t> &px, uint32_t w, uint32_t h )
{
    FILE *f = fopen( path, "wb" );
    if ( !f ) return;
    fprintf( f, "P6\n%u %u\n255\n", w, h );
    for ( uint32_t p : px ) { uint8_t rgb[3] = { (uint8_t)p, (uint8_t)( p >> 8 ), (uint8_t)( p >> 16 ) }; fwrite( rgb, 1, 3, f ); }
    fclose( f );
}

// Called on the GPU thread when it executes a swap. `fetch` = the front buffer's 6-dword texture
// fetch constant: dword0 bit 31 tiled, bits 22-30 pitch/32; dword1 bits 0-5 format, 6-7 endian,
// 12-31 base address; dword2 bits 0-12 width-1, 13-25 height-1.
// Debug: a 64x36 grey thumbnail of the next presented frame, on request (finding camera variables).
std::atomic<int> g_sig_request{ 0 };
std::atomic<uint32_t> g_sig_seq{ 0 };
float g_sig[64 * 36];
bool video_display_size( uint32_t &w, uint32_t &h )
{
    w = g_display_w, h = g_display_h;
    return w && h;
}

namespace
{

// F11 recording off the frame's thread: frames queue (two at most) for a writer thread that feeds
// ffmpeg; while it is behind, frames are left out rather than holding the game. ffmpeg stamps each
// frame with the time it came (so the video keeps real time over left-out frames) and encodes on the
// GPU (NVENC HEVC, which keeps up at 4K 60) or, without one, with x264. A read-back frame is written
// from its readback slot (the renderer reuses one every fourth frame, behind the two queued), any
// other from a copy.
struct RecFrame { const uint32_t *raw = nullptr; std::vector<uint32_t> own; };
struct Recorder
{
    std::mutex m;
    std::condition_variable cv;
    std::vector<RecFrame> queue;
    std::vector<std::vector<uint32_t>> pool;
    uint32_t w = 0, h = 0, order = 0;
    bool open = false, closing = false;
    unsigned dropped = 0;
};
Recorder g_rec;
// The recording's sound: the stereo mix (audio.cpp, video_record_audio) gathered while recording, written
// by the writer thread to a side file and put in with the video when the recording ends (FLAC: lossless).
std::mutex g_rec_audio_m;
std::vector<float> g_rec_audio;
std::atomic<bool> g_rec_audio_on{ false };

const char *rec_encoder()
{
    static const char *enc = [] {
        bool nv = system( "ffmpeg -hide_banner -loglevel quiet -f lavfi -i nullsrc=s=256x256 -frames:v 1 -c:v hevc_nvenc -f null - 2>/dev/null" ) == 0;
        rt_log( "video: recording encoder %s\n", nv ? "hevc_nvenc" : "libx264" );
        return nv ? "-c:v hevc_nvenc -preset p1 -tune ll -rc vbr -cq 18 -b:v 0" : "-c:v libx264 -preset ultrafast -crf 16";
    }();
    return enc;
}

void rec_thread( uint32_t w, uint32_t h, uint32_t order, unsigned fps )
{
    char name[64], cmd[512];
    time_t now = time( nullptr );
    tm t;
    localtime_r( &now, &t );
    strftime( name, sizeof( name ), "build/rec-%Y%m%d-%H%M%S.mkv", &t );
    static const char *const fmt[] = { "rgb0", "bgr0", "0rgb" };
    snprintf( cmd, sizeof( cmd ),
              "ffmpeg -y -loglevel error -use_wallclock_as_timestamps 1 -f rawvideo -pix_fmt %s -s %ux%u -i - -fps_mode cfr -r %u %s %s",
              fmt[order], w, h, fps, rec_encoder(), name );
    FILE *pipe = popen( cmd, "w" );
    rt_log( "video: recording %s (%ux%u, %u fps)\n", name, w, h, fps );
    char audio_name[80];
    snprintf( audio_name, sizeof( audio_name ), "%s.f32", name );
    FILE *audio = fopen( audio_name, "wb" );
    std::vector<float> chunk;
    auto drain = [&] {
        {
            std::lock_guard<std::mutex> g( g_rec_audio_m );
            chunk.swap( g_rec_audio );
        }
        if ( audio && !chunk.empty() ) fwrite( chunk.data(), sizeof( float ), chunk.size(), audio );
        chunk.clear();
    };
    {
        std::lock_guard<std::mutex> g( g_rec_audio_m );
        g_rec_audio.clear();
    }
    g_rec_audio_on = true;
    for ( ;; )
    {
        RecFrame f;
        {
            std::unique_lock<std::mutex> g( g_rec.m );
            g_rec.cv.wait( g, [] { return !g_rec.queue.empty() || g_rec.closing; } );
            if ( g_rec.queue.empty() ) break;
            f = std::move( g_rec.queue.front() );
            g_rec.queue.erase( g_rec.queue.begin() );
        }
        if ( pipe ) fwrite( f.raw ? f.raw : f.own.data(), 4, (size_t)w * h, pipe );
        drain();
        std::lock_guard<std::mutex> g( g_rec.m );
        if ( !f.raw ) g_rec.pool.push_back( std::move( f.own ) );
    }
    g_rec_audio_on = false;
    drain();
    if ( audio ) fclose( audio );
    if ( pipe ) pclose( pipe );
    // The sound goes in after (a copy of the video with it, then in the video's place), off this thread.
    std::thread( [video = std::string( name ), snd = std::string( audio_name )] {
        const std::string tmp = video + ".tmp.mkv";
        const std::string mux = "ffmpeg -y -loglevel error -i '" + video + "' -f f32le -ar 48000 -ac 2 -i '" + snd + "' -map 0:v -map 1:a -c:v copy -c:a flac '" + tmp + "'";
        if ( system( mux.c_str() ) == 0 && rename( tmp.c_str(), video.c_str() ) == 0 ) remove( snd.c_str() ), rt_log( "video: recording %s has its sound\n", video.c_str() );
        else remove( tmp.c_str() ), rt_log( "video: recording %s: sound left in %s (mux failed)\n", video.c_str(), snd.c_str() );
    } ).detach();
    std::lock_guard<std::mutex> g( g_rec.m );
    rt_log( "video: recording %s closed (%u frames left out)\n", name, g_rec.dropped );
    g_rec.open = g_rec.closing = false, g_rec.dropped = 0;
    g_rec.cv.notify_all();
}

void rec_close()
{
    std::unique_lock<std::mutex> g( g_rec.m );
    if ( !g_rec.open ) return;
    g_rec.closing = true;
    g_rec.cv.notify_all();
    g_rec.cv.wait( g, [] { return !g_rec.open; } );  // (the queue's last frames, and ffmpeg's end)
}

// A frame for the recording: `raw` (a readback slot, w x h packed) as it is, or `px` copied.
void rec_frame( const uint32_t *raw, const std::vector<uint32_t> *px, uint32_t w, uint32_t h, uint32_t order )
{
    if ( g_rec.open && ( w != g_rec.w || h != g_rec.h || order != g_rec.order ) ) rec_close();
    std::lock_guard<std::mutex> g( g_rec.m );
    if ( !g_rec.open )
    {
        g_rec.open = true, g_rec.w = w, g_rec.h = h, g_rec.order = order, g_rec.dropped = 0;
        g_rec.queue.clear(), g_rec.pool.clear();
        std::thread( rec_thread, w, h, order, settings().fps60 ? 60u : 30u ).detach();
    }
    if ( g_rec.queue.size() >= 2 ) { ++g_rec.dropped; return; }
    RecFrame f;
    f.raw = raw;
    if ( !raw )
    {
        if ( !g_rec.pool.empty() ) f.own = std::move( g_rec.pool.back() ), g_rec.pool.pop_back();
        g_rec.m.unlock();  // (the copy without the writer waiting on it)
        f.own.assign( px->begin(), px->end() );
        g_rec.m.lock();
    }
    g_rec.queue.push_back( std::move( f ) );
    g_rec.cv.notify_all();
}

}  // namespace

// The stereo mix as it is made (audio.cpp: interleaved, 48 kHz): kept while recording.
void video_record_audio( const float *s, size_t n )
{
    if ( !g_rec_audio_on ) return;
    std::lock_guard<std::mutex> g( g_rec_audio_m );
    if ( g_rec_audio.size() < 48000 * 2 * 10 ) g_rec_audio.insert( g_rec_audio.end(), s, s + n );  // (10 s at most waiting)
}

struct HiFrame { const void *pixels; uint32_t w, h; bool rgba; };
void video_present( const uint32_t fetch[6], HiFrame hi )
{
    static const bool headless = getenv( "CRACKDOWN_HEADLESS" );
    static const long dump_at = getenv( "CRACKDOWN_DUMP_FRAME" ) ? atol( getenv( "CRACKDOWN_DUMP_FRAME" ) ) : -1;
    static const long dump_every = getenv( "CRACKDOWN_DUMP_EVERY" ) ? atol( getenv( "CRACKDOWN_DUMP_EVERY" ) ) : 0;
    static uint64_t frame;
    ++frame;
    // CRACKDOWN_FRAME_LOG=ms: frames that came longer than that after the previous one (hitches).
    if ( static const double slow = getenv( "CRACKDOWN_FRAME_LOG" ) ? atof( getenv( "CRACKDOWN_FRAME_LOG" ) ) : 0; slow > 0 )
    {
        static auto last = std::chrono::steady_clock::now();
        auto now = std::chrono::steady_clock::now();
        if ( double ms = std::chrono::duration<double, std::milli>( now - last ).count(); ms > slow ) rt_log( "video: frame %llu after %.1f ms (monotonic %.3f s, thread %d)\n", (unsigned long long)frame, ms,
                                                                                    std::chrono::duration<double>( now.time_since_epoch() ).count(), (int)gettid() );
        last = now;
    }
    bool dump = (long)frame == dump_at || ( dump_every && frame % dump_every == 0 ) || video_take_capture_dump();
    bool sig = g_sig_request.load();
    // F11 recording: frames are piped to ffmpeg, one file per recording.
    // CRACKDOWN_RECORD_FROM=seconds (input script clock): record from then on, headless too (testing).
    static const double record_from = getenv( "CRACKDOWN_RECORD_FROM" ) ? atof( getenv( "CRACKDOWN_RECORD_FROM" ) ) : -1;
    if ( record_from >= 0 && !g_recording && input_script_clock() >= record_from ) { static bool once; if ( !once ) once = g_recording = true; }
    // CRACKDOWN_RECORD_UNTIL=seconds: and stop then (the recording is finished, its sound put in).
    static const double record_until = getenv( "CRACKDOWN_RECORD_UNTIL" ) ? atof( getenv( "CRACKDOWN_RECORD_UNTIL" ) ) : -1;
    if ( record_until >= 0 && g_recording && input_script_clock() >= record_until ) { static bool once; if ( !once ) once = true, g_recording = false; }
    bool recording = g_recording.load();
    if ( !recording ) rec_close();
    // The start-up cover (Skip Intro) from the first frame the game shows.
    // CRACKDOWN_DUMP_ENTRY="Bank.xwb:index:file" (debug): a wave bank entry decoded, 48 kHz stereo float, to the file.
    static std::once_flag page_dump_once;
    std::call_once( page_dump_once, [] { keyspage_dump(); } );
    static std::once_flag dump_once;
    std::call_once( dump_once, [] {
        if ( const char *e = getenv( "CRACKDOWN_DUMP_ENTRY" ) )
        {
            char bank[64], file[512];
            unsigned index;
            if ( sscanf( e, "%63[^:]:%u:%511s", bank, &index, file ) == 3 )
            {
                auto pcm = sound_file_entry_pcm( disc_file_path( ( std::string( "Audio/" ) + bank ).c_str() ), index );
                if ( FILE *f = fopen( file, "wb" ) ) fwrite( pcm.data(), sizeof( float ), pcm.size(), f ), fclose( f );
                rt_log( "dumped %s #%u: %.3f s\n", bank, index, pcm.size() / 96000.0 );
            }
        }
    } );
    static std::once_flag cover_once;
    std::call_once( cover_once, [] {
        if ( !settings().skip_intro || getenv( "CRACKDOWN_NO_COVER" ) ) return;
        g_cover = true;
        std::thread( cover_thread ).detach();
        rt_log( "video: start-up cover on\n" );
    } );
    if ( g_cover && recording )  // a recording shows what the window does: the cover
    {
        static uint64_t recorded;
        if ( g_cover_seq != recorded )
        {
            std::vector<uint32_t> copy;
            uint32_t cw, ch;
            {
                std::lock_guard<std::mutex> g( g_cover_m );
                recorded = g_cover_seq, copy = g_cover_px, cw = g_cover_w, ch = g_cover_h;
            }
            if ( cw ) rec_frame( nullptr, &copy, cw, ch, ORDER_RGBA );
        }
        return;
    }
    if ( headless && !dump && !sig && !recording ) return;
    static std::once_flag started;
    if ( !headless ) std::call_once( started, [] { std::thread( window_thread ).detach(); } );

    uint32_t base = fetch[1] & 0xFFFFF000, format = fetch[1] & 0x3F, endian = ( fetch[1] >> 6 ) & 3;
    uint32_t w = ( fetch[2] & 0x1FFF ) + 1, h = ( ( fetch[2] >> 13 ) & 0x1FFF ) + 1;
    bool tiled = fetch[0] >> 31;
    uint32_t pitch = ( ( fetch[0] >> 22 ) & 0x1FF ) * 32;
    if ( !pitch ) pitch = ( w + 31 ) & ~31u;
    if ( w > 4096 || h > 4096 || !base ) return;
    static bool described;
    if ( !described ) { described = true; rt_log( "video: front buffer %08X %ux%u format %u endian %u %s\n", base, w, h, format, endian, tiled ? "tiled" : "linear" ); }

    if ( sig )
    {
        const uint8_t *s0 = g_base + 0xA0000000 + ( base & 0x1FFFFFFF );
        for ( int sy = 0; sy < 36; ++sy )
            for ( int sx = 0; sx < 64; ++sx )
            {
                uint32_t x = sx * w / 64 + w / 128, y = sy * h / 36 + h / 72;
                uint32_t i = tiled ? tiled_offset( x, y, pitch, 2 ) : y * pitch + x, v;
                memcpy( &v, s0 + (size_t)i * 4, 4 );
                g_sig[sy * 64 + sx] = ( ( v & 0xFF ) + ( ( v >> 8 ) & 0xFF ) + ( ( v >> 16 ) & 0xFF ) + ( v >> 24 ) ) / 4.0f;
            }
        g_sig_request = 0;
        ++g_sig_seq;
        if ( headless && !dump ) return;
    }
    const uint8_t *src = g_base + 0xA0000000 + ( base & 0x1FFFFFFF );
    if ( hi.pixels ) src = (const uint8_t *)hi.pixels, w = hi.w, h = hi.h, pitch = hi.w, tiled = false;
    auto t0 = std::chrono::steady_clock::now();
    // A read-back frame (high resolution, linear, in a byte order the window and ffmpeg take) is shown and
    // recorded from its readback slot as it is: the window thread converts it into its texture, the
    // recorder writes it to ffmpeg, and this thread (the GPU's) only hands it on. Converting and copying
    // 33 MB a frame here took 4-6 ms of each 16.7 at 4K.
    const uint32_t order = hi.rgba ? ORDER_RGBA : endian == 2 ? ORDER_ARGB : ORDER_BGRA;
    if ( hi.pixels && ( hi.rgba || endian == 0 || endian == 2 ) && !dump )
    {
        if ( recording ) rec_frame( (const uint32_t *)src, nullptr, w, h, order );
        if ( static const bool timing = getenv( "CRACKDOWN_PRESENT_TIMING" ); timing )
        {
            static double t; static int n;
            t += std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() - t0 ).count();
            if ( ++n == 120 ) rt_log( "video: present (direct) %.3f ms (avg of 120)\n", t / n ), t = 0, n = 0;
        }
        if ( headless ) return;
        {
            std::lock_guard<std::mutex> g( g_frame_lock );
            g_frame_raw = (const uint32_t *)src, g_frame_order = order;
            g_frame_w = w;
            g_frame_h = h;
            ++g_frame_seq;
        }
        g_frame_cv.notify_one();
        return;
    }
    // Otherwise converted here, into one buffer kept between frames (it and the window's, swapped below,
    // go round).
    static std::vector<uint32_t> px;
    px.resize( (size_t)w * h );
    if ( hi.rgba ) memcpy( px.data(), src, px.size() * 4 );  // FSR's result
    else for ( uint32_t y = 0; y < h; ++y )
        for ( uint32_t x = 0; x < w; ++x )
        {
            uint32_t i = tiled ? tiled_offset( x, y, pitch, 2 ) : y * pitch + x;
            uint32_t v;
            memcpy( &v, src + (size_t)i * 4, 4 );
            if ( endian == 2 ) v = __builtin_bswap32( v );           // 8-in-32 swap
            else if ( endian == 1 ) v = ( ( v & 0x00FF00FF ) << 8 ) | ( ( v >> 8 ) & 0x00FF00FF );  // 8-in-16
            // k_8_8_8_8 is stored A, R, G, B from the high byte down once swapped into a host word.
            uint32_t a = v >> 24, r = ( v >> 16 ) & 0xFF, g = ( v >> 8 ) & 0xFF, b = v & 0xFF;
            px[(size_t)y * w + x] = r | ( g << 8 ) | ( b << 16 ) | ( 0xFFu << 24 );
            (void)a;
        }
    if ( recording ) rec_frame( nullptr, &px, w, h, ORDER_RGBA );
    if ( dump )
    {
        char path[64];
        snprintf( path, sizeof(path), "build/frame-%llu.ppm", (unsigned long long)frame );
        write_ppm( path, px, w, h );
        uint64_t audio_sample_count();  // audio.cpp
        rt_log( "video: wrote %s (audio sample %llu)\n", path, (unsigned long long)audio_sample_count() );
        // CRACKDOWN_DUMP_MEM=addr:bytes,...: raw guest physical memory as build/mem-<addr>.bin.
        if ( const char *list = getenv( "CRACKDOWN_DUMP_MEM" ) )
            for ( const char *c = list; c; c = strchr( c, ',' ) ? strchr( c, ',' ) + 1 : nullptr )
            {
                unsigned ma = 0, mn = 0;
                if ( sscanf( c, "%x:%x", &ma, &mn ) != 2 || mn > 0x20000000 ) continue;
                snprintf( path, sizeof( path ), "build/mem-%08X.bin", ma );
                if ( FILE *f = fopen( path, "wb" ) ) { fwrite( g_base + 0xA0000000 + ( ma & 0x1FFFFFFF ), 1, mn, f ); fclose( f ); }
            }
        // CRACKDOWN_DUMP_SURFACES=addr:w:h,...: also write those tiled 32bpp guest surfaces (resolve
        // targets) as build/surf-<addr>-<frame>.ppm, colour bytes as for the front buffer.
        if ( const char *list = getenv( "CRACKDOWN_DUMP_SURFACES" ) )
            for ( const char *c = list; *c; )
            {
                unsigned sa = 0, sw = 0, sh = 0, sbits = 32;  // optional :8 = 8bpp (grey)
                if ( sscanf( c, "%x:%u:%u:%u", &sa, &sw, &sh, &sbits ) >= 3 && sw && sh && sw <= 4096 && sh <= 4096 )
                {
                    uint32_t sp = ( sw + 31 ) & ~31u;
                    std::vector<uint32_t> spx( (size_t)sw * sh ), apx( (size_t)sw * sh );  // colour; alpha as grey (-a.ppm)
                    const uint8_t *ss = g_base + 0xA0000000 + ( sa & 0x1FFFFFFF );
                    for ( uint32_t y = 0; y < sh; ++y )
                        for ( uint32_t x = 0; x < sw; ++x )
                        {
                            if ( sbits == 8 )
                            {
                                uint32_t g = ss[tiled_offset( x, y, sp, 0 )];
                                spx[(size_t)y * sw + x] = g | ( g << 8 ) | ( g << 16 ) | ( 0xFFu << 24 );
                                continue;
                            }
                            uint32_t v;
                            memcpy( &v, ss + (size_t)tiled_offset( x, y, sp, 2 ) * 4, 4 );
                            v = __builtin_bswap32( v );
                            spx[(size_t)y * sw + x] = ( ( v >> 16 ) & 0xFF ) | ( ( ( v >> 8 ) & 0xFF ) << 8 ) | ( ( v & 0xFF ) << 16 ) | ( 0xFFu << 24 );
                            apx[(size_t)y * sw + x] = ( v >> 24 ) * 0x010101u;
                        }
                    snprintf( path, sizeof( path ), "build/surf-%08X-%llu.ppm", sa, (unsigned long long)frame );
                    write_ppm( path, spx, sw, sh );
                    if ( sbits != 8 )
                    {
                        snprintf( path, sizeof( path ), "build/surf-%08X-%llu-a.ppm", sa, (unsigned long long)frame );
                        write_ppm( path, apx, sw, sh );
                    }
                }
                const char *n = strchr( c, ',' );
                if ( !n ) break;
                c = n + 1;
            }
    }
    if ( headless ) return;
    {
        std::lock_guard<std::mutex> g( g_frame_lock );
        g_frame.swap( px );
        g_frame_raw = nullptr, g_frame_order = ORDER_RGBA;
        g_frame_w = w;
        g_frame_h = h;
        ++g_frame_seq;
    }
    g_frame_cv.notify_one();
}

// Controller 0 from the window's keyboard and mouse (all zero when headless). Mouse motion becomes right
// stick deflection proportional to its speed, measured over at least 8 ms so several polls in one frame
// share it; any motion starts past the game's stick dead zone. The mouse sensitivity setting scales it (default 1).
PadState video_pad_state()
{
    // Headless there is no window thread polling the keyboard: a key script's keys are polled here.
    if ( static const bool poll = getenv( "CRACKDOWN_HEADLESS" ) && getenv( "CRACKDOWN_KEY_SCRIPT" ); poll )
    {
        static std::mutex poll_lock;
        std::lock_guard<std::mutex> g( poll_lock );
        poll_keyboard();
    }
    PadState p;
    p.buttons = (uint16_t)g_pad_buttons.load();
    uint32_t t = g_pad_triggers.load();
    p.lt = t & 0xFF;
    p.rt = t >> 8;
    p.lx = (int16_t)g_pad_lx.load();
    p.ly = (int16_t)g_pad_ly.load();
    p.rx = (int16_t)g_gamepad_rx.load();
    p.ry = (int16_t)g_gamepad_ry.load();
    return p;
}

// The mouse as the game's look actions where mouse look isn't turning the camera itself (the car camera,
// keyboard.cpp): -127..127, up positive. Any motion starts past the game's dead zone.
void video_mouse_axes( int &x, int &y )
{
    static std::mutex lock;
    static auto last = std::chrono::steady_clock::now();
    static int ax, ay;
    std::lock_guard<std::mutex> g( lock );
    auto now = std::chrono::steady_clock::now();
    float dt = std::chrono::duration<float>( now - last ).count();
    if ( dt >= 0.008f )
    {
        last = now;
        float dx = g_mouse_dx.exchange( 0 ), dy = g_mouse_dy.exchange( 0 );
        int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>( now.time_since_epoch() ).count();
        if ( now_ms - g_look_seen.load() < 200 ) dx = dy = 0;  // mouse look has it
        // Menus take the mouse as a pointer only (also while a click had it captured for a moment).
        if ( g_menu_active || g_menu_control ) dx = dy = 0;
        const float sens = settings().mouse_sensitivity;
        auto axis = [&]( float d ) {
            float v = d / dt * sens * 40.0f;  // pixels per second -> deflection
            if ( v == 0.0f ) return 0;
            int m = (int)( std::min( 32767.0f, 8000.0f + std::fabs( v ) ) * 127.0f / 32767.0f );
            return v < 0 ? -m : m;
        };
        ax = axis( dx );
        ay = axis( -dy );
    }
    x = ax;
    y = ay;
}

// CRACKDOWN_MOUSE_SCRIPT="52:640,160,53:640,160:click,54:640,160:wheel-3,...": headless pointer moves (left clicks, :rclick, wheel notches) at
// those seconds, for testing the menu mouse.
MenuMouse video_menu_mouse()
{
    MenuMouse m;
    if ( const char *script = getenv( "CRACKDOWN_MOUSE_SCRIPT" ) )
    {
        static auto start = std::chrono::steady_clock::now();
        double t = std::chrono::duration<double>( std::chrono::steady_clock::now() - start ).count();
        m.present = true;
        uint32_t step = 0;
        for ( const char *c = script; c && *c; ++step )
        {
            double at; float x, y; int used = 0;
            if ( sscanf( c, "%lf:%f,%f%n", &at, &x, &y, &used ) != 3 || t < at ) break;
            bool click = !strncmp( c + used, ":click", 6 );
            m.x = x; m.y = y; m.moves = step + 1;
            m.left = click && t < at + 0.1;
            m.right = !strncmp( c + used, ":rclick", 7 ) && t < at + 0.1;
            static uint32_t wheeled;  // ":wheel-3": three notches down, once
            if ( !strncmp( c + used, ":wheel", 6 ) && wheeled <= step ) { m.wheel = atoi( c + used + 6 ); wheeled = step + 1; }
            c = strchr( c + used, ',' ); if ( c ) ++c;
        }
        return m;
    }
    m.present = g_window_up;
    m.x = g_menu_x; m.y = g_menu_y; m.moves = g_menu_moves;
    m.left = g_menu_left; m.right = g_menu_right;
    m.wheel = g_menu_wheel.exchange( 0 );
    return m;
}

void video_set_menu_active( bool active, bool control ) { g_menu_active = active; g_menu_control = control; }

// Mouse look not running this frame (in a vehicle): forget its counts; the mouse drives the stick.
void video_player_update()
{
    g_player_seen = std::chrono::duration_cast<std::chrono::milliseconds>( std::chrono::steady_clock::now().time_since_epoch() ).count();
}
void video_drop_look()
{
    g_look_dx = 0;
    g_look_dy = 0;
    g_vehicle_seen = std::chrono::duration_cast<std::chrono::milliseconds>( std::chrono::steady_clock::now().time_since_epoch() ).count();
}

// Raw mouse counts since the last call, for mouse look (all zero when the mouse isn't captured).
void video_take_look( float &dx, float &dy )
{
    dx = g_look_dx.exchange( 0 );
    dy = g_look_dy.exchange( 0 );
    // CRACKDOWN_LOOK_TEST="t:dx:dy,...": headless, add those counts once at t seconds of the input script clock.
    static const char *test = getenv( "CRACKDOWN_LOOK_TEST" );
    if ( test && *test && input_script_clock() >= atof( test ) )
    {
        float tx = 0, ty = 0;
        sscanf( test, "%*f:%f:%f", &tx, &ty );
        dx += tx;
        dy += ty;
        rt_log( "look test: %g, %g\n", tx, ty );
        const char *next = strchr( test, ',' );
        test = next ? next + 1 : nullptr;
    }
    g_look_seen = std::chrono::duration_cast<std::chrono::milliseconds>( std::chrono::steady_clock::now().time_since_epoch() ).count();
}

// The game's front end went to scene `id` (xam.cpp): the start-up cover ends where the game has something of
// its own to show -- past the splash (4) and, with the title skipped too, the hidden title steps (11, 10,
// 19, 18) -- and its music fades as the menu starts the same piece.
void video_cover_scene( uint32_t id )
{
    if ( !g_cover ) return;
    const bool hidden = id == 4 || ( settings().skip_title && ( id == 10 || id == 11 || id == 18 || id == 19 ) );
    if ( hidden ) return;
    g_cover = false;
    for ( auto &p : g_cover_sounds ) audio_fade_overlay( p.get(), 0.4f );
    rt_log( "video: start-up cover off (scene %u)\n", id );
}

// Whether the start-up cover is up (audio.cpp: the game's own sound waits, the cover plays the loading sound).
bool video_cover_active() { return g_cover; }
