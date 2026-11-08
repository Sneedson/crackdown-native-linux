// NVIDIA DLSS (the Recomp Options page's Upscaling: DLSS) through NGX, the driver's library, with
// libnvidia-ngx-dlss.so beside the program (build.sh copies it from the DLSS SDK, $DLSS or ~/src/DLSS).
// The renderer turns on the Vulkan extensions NGX asks for, starts it once its device is made, and hands
// it each frame's picture of the world as it is resolved (renderer.cpp DlssFrame), drawn smaller by the
// mode's share (NVIDIA's modes: DLAA, Quality, Balanced, Performance, Ultra Performance), with the
// scene's depth and the camera's motion vectors; DLSS's picture at the resolution takes its place, and
// the post-processing and the HUD are drawn at the resolution over it. Without the SDK at build time, or
// an RTX card, DLSS is never on.
#pragma once
#include <vulkan/vulkan.h>
#include <cstdint>
#include <vector>

struct DlssImage
{
    VkImage image;
    VkImageView view;
    VkFormat format;
    uint32_t w, h;
};

bool dlss_wanted();  // the setting asks for it and this build has it
void dlss_extensions( std::vector<const char *> &instance, std::vector<const char *> &device );  // added to the lists (no duplicates)
bool dlss_init( VkInstance instance, VkPhysicalDevice phys, VkDevice dev );  // false: DLSS can't run here (logged)
bool dlss_ready();
float dlss_share();  // the mode's share of the resolution's width (and height) the world is drawn at (1: DLAA)
// Records DLSS on cmd: color (and depth, motion vectors in render pixels) to out. The feature is made
// on the first call and again when the sizes change. jitter: this frame's sub-pixel offset, in render
// pixels; reset: the picture has no history (a cut).
// mask (may be null): where the picture is to be taken as it is now (see-through drawing), 1.
bool dlss_evaluate( VkCommandBuffer cmd, const DlssImage &color, const DlssImage &depth, const DlssImage &motion, const DlssImage *mask, const DlssImage &out,
                    float jitter_x, float jitter_y, bool reset );
