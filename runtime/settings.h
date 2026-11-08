// The player's settings: the Recomp Options page in the game's menu sets them, and they are kept in
// settings.ini in the config folder (~/.config/crackdown-recomp, or $XDG_CONFIG_HOME/crackdown-recomp),
// which can also be edited by hand. An environment variable still decides its setting for that run
// (CRACKDOWN_MOUSE_SENS, CRACKDOWN_MOUSE_INVERT, CRACKDOWN_FULLSCREEN, CRACKDOWN_VSYNC,
// CRACKDOWN_SKIP_SPLASH, CRACKDOWN_SKIP_TITLE, CRACKDOWN_NATIVE_D3D, CRACKDOWN_AA, CRACKDOWN_FOV).
#pragma once
#include <cstdint>
#include <string>

struct Settings
{
    float mouse_sensitivity = 1.0f;  // 0.25 .. 4
    bool invert_y = false;
    bool fullscreen = false;  // borderless, filling the screen
    bool exclusive = false;   // with fullscreen: the display's own fullscreen mode
    bool vsync = true;
    bool skip_intro = false;  // the start-up videos (next start)
    bool skip_title = false;  // the "press START" title (next start)
    bool native_d3d = true;   // the native D3D device (next start)
    bool anti_aliasing = false; // supersampling instead: the game's multisampled surfaces drawn at 2x2 the pixels, at the console's resolution only, with msaa 0 (next start)
    int resolution = 0;         // rendered height, 720 to 2160 in steps of 90 (16:9: 1280x720 to 3840x2160); 0: match the display (next start)
    bool fsr = false;           // upscale the frame to the screen with FSR 1 (next start)
    bool dlss = false;          // NVIDIA DLSS instead (next start): the world drawn smaller and DLSS bringing it up to the resolution
    bool fsr3 = false;          // AMD FSR 3 instead (next start; any card): as DLSS, with AMD's upscaler (fsr3.h)
    int dlss_mode = 1;          // DLSS's or FSR 3's mode: 0 DLAA / Native AA (no smaller), 1 Quality, 2 Balanced, 3 Performance, 4 Ultra Performance (next start)
    int dlss_preset = 0;        // DLSS's model: 0 NVIDIA's default for the mode, 1 preset K, 2 L, 3 M, 4 J
    bool fxaa = false;          // FXAA on the finished frame (next start)
    int msaa = 2;               // multisampling of the game's multisampled surfaces at any resolution: 0 none, 2 (the console's), 4, 8 samples (next start)
    int anisotropy = 0;         // anisotropic filtering on every mipmapped texture: 2, 4, 8, 16 to 1; 0 the game's own
    int lod = 0;                // draw and keep-alive distances for cars and people: 0 the game's own, 1..4 farther (renderer-independent)
    bool show_fps = false;
    bool fps60 = false;         // presents every vblank (60 fps) instead of the game's every other one
    int fov = 70;               // the field of view across a 16:9 picture, degrees: 60 to 120 in steps of 5 (70: the game's own)
    std::string profile = "Player";  // the signed-in profile (profile.h): its achievements, settings and saves (next start)
};

Settings settings();                      // the settings in effect
void settings_change( const Settings &s ); // takes them on and saves them
uint32_t settings_version();               // goes up with each change (for those applying them live)
