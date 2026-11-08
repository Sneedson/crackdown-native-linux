// AMD FSR 3.1's upscaler (the Recomp Options page's Upscaling: FSR 3), from AMD's FidelityFX SDK v1.1.4
// (MIT licence; $FFX or ~/src/ref/FidelityFX-SDK at build time, build.sh). It runs on any
// Vulkan card (AMD, Intel, NVIDIA) and takes what DLSS takes (dlss.h): each frame's picture of the world
// drawn smaller by the mode's share (AMD's modes: Native AA, Quality, Balanced, Performance, Ultra
// Performance), the scene's depth, the motion vectors and the see-through mask (as FSR's reactive mask),
// and puts its picture at the resolution in its place. Without the SDK at build time it is never on.
#pragma once
#include "dlss.h"  // (DlssImage)

bool fsr3_wanted();  // the setting asks for it and this build has it
void fsr3_features( VkPhysicalDeviceFeatures &enable, const VkPhysicalDeviceFeatures &have );  // what its shaders need
bool fsr3_init( VkPhysicalDevice phys, VkDevice dev );  // false: it can't run here (logged)
bool fsr3_ready();
float fsr3_share();  // the mode's share of the resolution's width (and height) the world is drawn at (1: Native AA)
// As dlss_evaluate (dlss.h): FSR 3 on cmd, color (with depth and motion vectors in render pixels) to out.
bool fsr3_evaluate( VkCommandBuffer cmd, const DlssImage &color, const DlssImage &depth, const DlssImage &motion, const DlssImage *mask, const DlssImage &out,
                    float jitter_x, float jitter_y, bool reset );

// The renderer's temporal upscaler: DLSS or FSR 3, whichever the setting asks for (they take the same inputs).
inline bool upscaler_wanted() { return dlss_wanted() || fsr3_wanted(); }
inline bool upscaler_ready() { return dlss_ready() || fsr3_ready(); }
inline float upscaler_share() { return dlss_ready() ? dlss_share() : fsr3_ready() ? fsr3_share() : 1.0f; }
inline bool upscaler_evaluate( VkCommandBuffer cmd, const DlssImage &color, const DlssImage &depth, const DlssImage &motion, const DlssImage *mask, const DlssImage &out,
                               float jitter_x, float jitter_y, bool reset )
{
    return dlss_ready() ? dlss_evaluate( cmd, color, depth, motion, mask, out, jitter_x, jitter_y, reset )
                        : fsr3_evaluate( cmd, color, depth, motion, mask, out, jitter_x, jitter_y, reset );
}
