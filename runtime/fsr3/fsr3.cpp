// FSR 3.1's upscaler through the FidelityFX SDK's own host code and Vulkan backend (fsr3.h). Built with
// the SDK's includes and ffx_compat.h forced in front (build.sh).
#include "fsr3.h"
#include "runtime.h"
#include "settings.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <vector>

#ifdef HAVE_FSR3
#include <FidelityFX/host/backends/vk/ffx_vk.h>
#include <FidelityFX/host/ffx_fsr3upscaler.h>

namespace
{
bool g_ready;
VkPhysicalDevice g_phys;
VkDevice g_dev;
std::vector<uint8_t> g_scratch;
FfxInterface g_iface;
FfxFsr3UpscalerContext *g_ctx;  // (made as the SDK's opaque context: ffx_compat.h makes it big enough here)
// FSR 3.1 leaves three images to its user, which other effects (frame generation) can share: dilated depth,
// dilated motion vectors and the previous frame's nearest depth. Made as AMD's FSR 3 wrapper makes them, in
// a second backend context of their own.
uint32_t g_shared_ctx;
bool g_have_shared;
FfxResourceInternal g_shared[3];
uint32_t g_in_w, g_in_h, g_out_w, g_out_h;
std::chrono::steady_clock::time_point g_last;

// AMD's modes (settings dlss_mode, shared with DLSS's): the share of the output's width drawn.
const char *const MODE_NAME[] = { "Native AA", "Quality", "Balanced", "Performance", "Ultra Performance" };
const float MODE_SHARE[] = { 1.0f, 1.0f / 1.5f, 1.0f / 1.7f, 0.5f, 1.0f / 3.0f };
int mode() { static const int m = std::clamp( settings().dlss_mode, 0, 4 ); return m; }

void message( FfxMsgType type, const wchar_t *text )
{
    char s[512];
    size_t n = 0;
    for ( ; text && text[n] && n < sizeof( s ) - 1; ++n ) s[n] = text[n] < 128 ? (char)text[n] : '?';
    s[n] = 0;
    rt_log( "fsr3: %s: %s\n", type == FFX_MESSAGE_TYPE_ERROR ? "error" : "warning", s );
}

FfxResource resource( const DlssImage &i, const wchar_t *name, bool write )
{
    VkImageCreateInfo ci{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ci.imageType = VK_IMAGE_TYPE_2D, ci.format = i.format, ci.extent = { i.w, i.h, 1 }, ci.mipLevels = 1, ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT, ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | ( write ? VK_IMAGE_USAGE_STORAGE_BIT : 0 );
    FfxResourceDescription d = ffxGetImageResourceDescriptionVK( i.image, ci, write ? FFX_RESOURCE_USAGE_UAV : FFX_RESOURCE_USAGE_READ_ONLY );
    return ffxGetResourceVK( (void *)i.image, d, name, FFX_RESOURCE_STATE_UNORDERED_ACCESS );  // (all the renderer's images are in GENERAL)
}

void destroy()
{
    if ( !g_ctx ) return;
    vkDeviceWaitIdle( g_dev );
    if ( g_have_shared )
    {
        for ( FfxResourceInternal &r : g_shared ) g_iface.fpDestroyResource( &g_iface, r, g_shared_ctx );
        g_iface.fpDestroyBackendContext( &g_iface, g_shared_ctx );
        g_have_shared = false;
    }
    ffxFsr3UpscalerContextDestroy( g_ctx );
    delete g_ctx;
    g_ctx = nullptr;
}
} // namespace

// Asked for, or as DLSS's stand-in where DLSS can't run (no RTX card, or a build without its SDK): the
// renderer starts it only if DLSS didn't start.
bool fsr3_wanted()
{
    static const bool w = settings().fsr3 || settings().dlss;
    return w;
}

void fsr3_features( VkPhysicalDeviceFeatures &enable, const VkPhysicalDeviceFeatures &have )
{
    // The SDK's shaders read and write images declared without a format.
    enable.shaderStorageImageReadWithoutFormat = have.shaderStorageImageReadWithoutFormat;
    enable.shaderStorageImageWriteWithoutFormat = have.shaderStorageImageWriteWithoutFormat;
}

bool fsr3_init( VkPhysicalDevice phys, VkDevice dev )
{
    g_phys = phys, g_dev = dev;
    VkPhysicalDeviceFeatures f{};
    vkGetPhysicalDeviceFeatures( phys, &f );
    if ( !f.shaderStorageImageReadWithoutFormat || !f.shaderStorageImageWriteWithoutFormat )
    {
        rt_log( "fsr3: the card can't read and write images without a format; FSR 3 off\n" );
        return false;
    }
    g_scratch.resize( ffxGetScratchMemorySizeVK( phys, 2 ) );
    static VkDeviceContext ctx;
    ctx = { dev, phys, vkGetDeviceProcAddr };
    if ( ffxGetInterfaceVK( &g_iface, ffxGetDeviceVK( &ctx ), g_scratch.data(), g_scratch.size(), 2 ) != FFX_OK )
    {
        rt_log( "fsr3: the SDK's Vulkan backend didn't start; FSR 3 off\n" );
        return false;
    }
    g_ready = true;
    if ( !settings().fsr3 ) rt_log( "fsr3: in DLSS's place\n" );
    rt_log( "fsr3: ready (FSR %d.%d.%d, %s)\n", FFX_FSR3UPSCALER_VERSION_MAJOR, FFX_FSR3UPSCALER_VERSION_MINOR, FFX_FSR3UPSCALER_VERSION_PATCH, MODE_NAME[mode()] );
    return true;
}

bool fsr3_ready() { return g_ready; }

float fsr3_share() { return MODE_SHARE[mode()]; }

bool fsr3_evaluate( VkCommandBuffer cmd, const DlssImage &color, const DlssImage &depth, const DlssImage &motion, const DlssImage *mask, const DlssImage &out,
                    float jitter_x, float jitter_y, bool reset )
{
    if ( !g_ready ) return false;
    if ( g_ctx && ( g_in_w != color.w || g_in_h != color.h || g_out_w != out.w || g_out_h != out.h ) ) destroy();
    if ( !g_ctx )
    {
        FfxFsr3UpscalerContextDescription d{};
        d.flags = FFX_FSR3UPSCALER_ENABLE_DEPTH_INVERTED;  // (the game's depth: 1 near, 0 far; colour not HDR; motion at the render size, unjittered)
        if ( getenv( "CRACKDOWN_FSR3_CHECK" ) ) d.flags |= FFX_FSR3UPSCALER_ENABLE_DEBUG_CHECKING;
        d.maxRenderSize = { color.w, color.h };
        d.maxUpscaleSize = { out.w, out.h };
        d.fpMessage = message;
        d.backendInterface = g_iface;
        g_ctx = new FfxFsr3UpscalerContext{};
        const FfxErrorCode e = ffxFsr3UpscalerContextCreate( g_ctx, &d );
        if ( e != FFX_OK )
        {
            rt_log( "fsr3: couldn't make the context for %ux%u -> %ux%u (%d); FSR 3 off\n", color.w, color.h, out.w, out.h, (int)e );
            delete g_ctx;
            g_ctx = nullptr, g_ready = false;
            return false;
        }
        FfxFsr3UpscalerSharedResourceDescriptions shared{};
        bool ok = ffxFsr3UpscalerGetSharedResourceDescriptions( g_ctx, &shared ) == FFX_OK &&
                  g_iface.fpCreateBackendContext( &g_iface, FFX_EFFECT_SHAREDRESOURCES, nullptr, &g_shared_ctx ) == FFX_OK;
        g_have_shared = ok;
        const FfxCreateResourceDescription *descs[3] = { &shared.dilatedDepth, &shared.dilatedMotionVectors, &shared.reconstructedPrevNearestDepth };
        for ( int i = 0; ok && i < 3; ++i ) ok = g_iface.fpCreateResource( &g_iface, descs[i], g_shared_ctx, &g_shared[i] ) == FFX_OK;
        g_in_w = color.w, g_in_h = color.h, g_out_w = out.w, g_out_h = out.h;
        if ( !ok )
        {
            rt_log( "fsr3: couldn't make its shared images; FSR 3 off\n" );
            destroy();
            g_ready = false;
            return false;
        }
        rt_log( "fsr3: %ux%u -> %ux%u (%s)\n", color.w, color.h, out.w, out.h, color.w >= out.w ? "Native AA" : MODE_NAME[mode()] );
        reset = true;
    }
    const auto now = std::chrono::steady_clock::now();
    float ms = std::chrono::duration<float, std::milli>( now - g_last ).count();
    g_last = now;
    if ( !( ms > 0 && ms < 100 ) ) ms = 16.7f;

    FfxFsr3UpscalerDispatchDescription p{};
    p.commandList = ffxGetCommandListVK( cmd );
    p.color = resource( color, L"FSR3_InputColor", false );
    p.depth = resource( depth, L"FSR3_InputDepth", false );
    p.motionVectors = resource( motion, L"FSR3_InputMotionVectors", false );
    if ( mask ) p.reactive = resource( *mask, L"FSR3_InputReactiveMap", false );
    p.output = resource( out, L"FSR3_UpscaledOutput", true );
    p.dilatedDepth = g_iface.fpGetResource( &g_iface, g_shared[0] );
    p.dilatedMotionVectors = g_iface.fpGetResource( &g_iface, g_shared[1] );
    p.reconstructedPrevNearestDepth = g_iface.fpGetResource( &g_iface, g_shared[2] );
    p.jitterOffset = { jitter_x, jitter_y };
    p.motionVectorScale = { 1.0f, 1.0f };  // (render pixels, current to previous, as DLSS takes them)
    p.renderSize = { color.w, color.h };
    p.upscaleSize = { out.w, out.h };
    // RCAS after the accumulation, as AMD recommends (CRACKDOWN_FSR3_SHARPNESS=0..1, 0 off).
    static const float sharpness = getenv( "CRACKDOWN_FSR3_SHARPNESS" ) ? (float)atof( getenv( "CRACKDOWN_FSR3_SHARPNESS" ) ) : 0.3f;
    p.enableSharpening = sharpness > 0;
    p.sharpness = std::clamp( sharpness, 0.0f, 1.0f );
    p.frameTimeDelta = ms;
    p.preExposure = 1.0f;
    p.reset = reset;
    // The camera, for FSR's depth tests: the game's view is about 70 degrees across at 16:9 (fov.cpp),
    // its near and far planes about 0.5 and 2000 m.
    const float half_w = std::tan( settings().fov * 0.5f * 3.14159265f / 180.0f );
    p.cameraNear = 0.5f, p.cameraFar = 2000.0f;
    p.cameraFovAngleVertical = 2.0f * std::atan( half_w * 9.0f / 16.0f );
    p.viewSpaceToMetersFactor = 1.0f;
    const FfxErrorCode e = ffxFsr3UpscalerContextDispatch( g_ctx, &p );
    if ( e != FFX_OK )
    {
        static int logged;
        if ( logged++ < 5 ) rt_log( "fsr3: dispatch failed (%d)\n", (int)e );
        return false;
    }
    return true;
}

#else  // built without the FidelityFX SDK

bool fsr3_wanted() { return false; }
void fsr3_features( VkPhysicalDeviceFeatures &, const VkPhysicalDeviceFeatures & ) {}
bool fsr3_init( VkPhysicalDevice, VkDevice ) { return false; }
bool fsr3_ready() { return false; }
float fsr3_share() { return 1.0f; }
bool fsr3_evaluate( VkCommandBuffer, const DlssImage &, const DlssImage &, const DlssImage &, const DlssImage *, const DlssImage &, float, float, bool ) { return false; }

#endif
