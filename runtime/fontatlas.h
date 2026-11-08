// The game's font made again at the resolution's size (fontatlas.cpp), and its controller button pictures.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

// A button picture's cell in the atlas (k times the game's picture) and its pixels there (R, G, B, A).
struct FontIcon { uint32_t ch, x, y, w, h; std::vector<uint8_t> rgba; };
bool fontatlas_make( uint32_t k, std::vector<uint8_t> &out );
const std::vector<FontIcon> &fontatlas_icons();                                  // as in the atlas last made
std::vector<uint8_t> fontatlas_keycap( const FontIcon &icon, const std::string &label, float scale = 0 );  // a key's picture for that cell (scale: px per font pixel; 0 the atlas's)
std::vector<uint8_t> keycap_png( const std::string &label, uint32_t size );  // the same as a PNG, size x size
