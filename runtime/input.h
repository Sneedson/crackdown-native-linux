// Controller 0 as built from the host's keyboard and mouse (video.cpp) and read by the XAM input
// imports (xam.cpp); menu mouse support (xui.cpp).
#pragma once
#include <cstdint>

struct PadState
{
    uint16_t buttons = 0;          // XINPUT_GAMEPAD_* bits
    uint8_t lt = 0, rt = 0;        // triggers
    int16_t lx = 0, ly = 0;        // left stick
    int16_t rx = 0, ry = 0;        // right stick
};

PadState video_pad_state();  // all zero when headless

// The mouse in the game's 1280x720 coordinates while a menu is up (video.cpp).
struct MenuMouse
{
    float x = 0, y = 0;
    uint32_t moves = 0;            // changes whenever the pointer moves
    bool left = false, right = false;
    bool present = false;          // a window is there to point into
    int wheel = 0;                 // notches since the last read (up positive)
};
MenuMouse video_menu_mouse();
void video_set_menu_active( bool active, bool control = false );
bool video_menu_control();  // a menu with a control focused is up (the keyboard is its controller, even with the game running behind)  // a menu is up; control: its focus is on a control
void video_mouse_axes( int &x, int &y );        // the mouse as look actions, -127..127 (car camera)
void video_take_look( float &dx, float &dy );  // raw mouse counts since the last call (mouse look)
bool video_key( int scancode );                // SDL scancode held (window has the keyboard)
uint32_t video_action_mouse();                 // mouse buttons held while captured (SDL_BUTTON_*MASK)
int video_take_wheel();                        // mouse wheel steps in game since the last call
bool video_in_game();                          // the player's update ran in the last 200 ms
bool video_gamepad_connected();                // a controller is plugged in
bool video_using_controller();                 // a controller is the input in use (the mouse and keyboard aren't)
bool video_take_capture();                     // F12 was pressed: log this frame's draws (once)
void video_reveal( bool show );                // show the game, or hold the window's last frame and mute it
bool video_revealed();
void video_request_capture();                  // as F12 (CRACKDOWN_CAPTURE_AT=<script seconds>)
bool video_take_capture_dump();                // write the next presented frame out (once, after an F12 capture)
void video_player_update();                     // the player's update ran (gameplay, not paused)
void video_drop_look();                        // mouse look idle this frame: the mouse drives the stick  // menus show the system pointer instead of capturing the mouse

// Buttons the menu mouse is pressing (A for a left click, B for a right click); moves XUI focus to the
// control under the pointer. Call on a guest thread (it may call into the game's XUI).
struct PPCContext;
uint16_t xui_menu_mouse( PPCContext &c );
bool xui_menu_key( PPCContext &c, bool arrow );  // a menu key went down; true: it only showed the highlight
