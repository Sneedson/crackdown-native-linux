// The keyboard and mouse controls the player can change (the Keyboard Controls page, keyspage.cpp): each
// command is one thing the player does ("Jump", "Fire"), with up to two keys or mouse buttons. keyboard.cpp
// turns them into the game's own actions; the prompts' button pictures show them (fontatlas.cpp). Kept in
// keybinds.ini in the config folder (~/.config/crackdown-recomp), which can also be edited by hand.
#pragma once
#include <cstdint>
#include <string>

namespace keybinds
{
// Inputs besides SDL scancodes. The wheel's are fixed (not given to commands): it swaps weapons, or
// zooms while zoomed in, or changes the radio's track in a vehicle.
enum : int { MOUSE_L = 1000, MOUSE_R, MOUSE_M, MOUSE_X1, MOUSE_X2, WHEEL_UP, WHEEL_DOWN, WHEEL_SWAP, WHEEL_ZOOM_IN, WHEEL_ZOOM_OUT };

struct Command
{
    const char *id;     // in keybinds.ini
    const char *label;  // on the page
    const char *group;  // the page's heading it is under
    int def[2];         // the defaults
    int keys[2];        // now (0: none)
};

int count();
Command &command( int i );
int find( const char *id );            // index, -1 if none
void set_key( int command, int slot, int input );  // and saves; an input given elsewhere in the same group is taken from there
void reset_defaults();                 // and saves
uint32_t version();                    // goes up with each change
std::string key_name( int input );     // for the page: "Space", "Left Mouse"
std::string key_cap( int input );      // short, for a key's picture: "SPACE", "LMB"
bool bindable( int input );            // a key or button commands may have (not Esc, the menus' way out)
} // namespace keybinds
