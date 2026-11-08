// The player's profile, as the console kept one per gamer: its name and XUID, the achievements it has
// unlocked (with when), the profile settings the game wrote, and its saves. The profile signed in is
// settings profile (settings.ini "profile = <name>", "Player" if none); each one lives in
// <data dir>/crackdown-recomp/profiles/<name>/. The achievements themselves (names, descriptions, points,
// pictures) are the game's own, from its XDBF resource in the executable's image. Unlocking one shows the
// console's "Achievement Unlocked" note over the game (video.cpp draws profile_toast).
#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

std::string profile_name();
uint64_t profile_xuid();
std::filesystem::path profile_dir();
// Where the profile's own content of a type goes (saves: type 1; other types, downloadable content, are
// the console's, shared). "Player" keeps the content folder of before profiles.
std::filesystem::path profile_content_dir( uint32_t content_type );

struct Achievement
{
    uint32_t id, image, points, flags;
    std::string label, description, unachieved;
};
const std::vector<Achievement> &achievements();     // the game's, in its order
bool achievement_unlocked( uint32_t id, uint64_t *filetime = nullptr );
void achievement_unlock( uint32_t id );             // recorded, saved and shown, the first time

// Profile settings the game wrote (XamUserWriteProfileSettings): the setting's XUSER_DATA type and its
// value's bytes (big-endian as the console keeps them; strings and binary data as they were).
bool profile_setting( uint32_t id, uint8_t &type, std::vector<uint8_t> &value );
void profile_set_setting( uint32_t id, uint8_t type, const std::vector<uint8_t> &value );

// The note on screen now: its picture (R, G, B, A bytes, w x h at a 1920 x 1080 screen's scale), and its
// opacity; null when none. seq changes when the picture does.
const std::vector<uint32_t> *profile_toast( uint64_t &seq, int &w, int &h, float &alpha );
