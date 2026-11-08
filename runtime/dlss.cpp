// DLSS through NGX (dlss.h). This software contains source code provided by NVIDIA Corporation (the DLSS
// SDK's helper functions, used here through its headers).
#include "dlss.h"
#include "runtime.h"
#include "settings.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <unistd.h>

#ifdef HAVE_DLSS
#include <nvsdk_ngx_helpers_vk.h>
#include <nvsdk_ngx_vk.h>

namespace
{
bool g_ready;
VkDevice g_dev;
NVSDK_NGX_Parameter *g_params;
NVSDK_NGX_Handle *g_feature;
uint32_t g_in_w, g_in_h, g_out_w, g_out_h;
int g_preset = -1;  // the model the feature was made with (settings dlss_preset)

// NVIDIA's modes (settings dlss_mode): their quality value and the share of the output's width drawn.
const NVSDK_NGX_PerfQuality_Value MODE_VALUE[] = { NVSDK_NGX_PerfQuality_Value_DLAA, NVSDK_NGX_PerfQuality_Value_MaxQuality, NVSDK_NGX_PerfQuality_Value_Balanced,
                                                   NVSDK_NGX_PerfQuality_Value_MaxPerf, NVSDK_NGX_PerfQuality_Value_UltraPerformance };
const char *const MODE_NAME[] = { "DLAA", "Quality", "Balanced", "Performance", "Ultra Performance" };
const float MODE_SHARE[] = { 1.0f, 2.0f / 3.0f, 0.58f, 0.5f, 1.0f / 3.0f };
// The model presets (settings dlss_preset): NVIDIA's default for the mode, K, L, M, J.
const unsigned PRESET[] = { NVSDK_NGX_DLSS_Hint_Render_Preset_Default, NVSDK_NGX_DLSS_Hint_Render_Preset_K, NVSDK_NGX_DLSS_Hint_Render_Preset_L,
                            NVSDK_NGX_DLSS_Hint_Render_Preset_M, NVSDK_NGX_DLSS_Hint_Render_Preset_J };
int mode() { static const int m = std::clamp( settings().dlss_mode, 0, 4 ); return m; }

std::wstring wide( const std::string &s ) { return std::wstring( s.begin(), s.end() ); }

void NVSDK_CONV ngx_log( const char *message, NVSDK_NGX_Logging_Level, NVSDK_NGX_Feature )
{
    std::string m = message;
    while ( !m.empty() && ( m.back() == '\n' || m.back() == '\r' ) ) m.pop_back();
    rt_log( "dlss: ngx: %s\n", m.c_str() );
}
} // namespace

bool dlss_wanted()
{
    static const bool w = settings().dlss;
    return w;
}

void dlss_extensions( std::vector<const char *> &instance, std::vector<const char *> &device )
{
    unsigned ni = 0, nd = 0;
    const char **inst = nullptr, **dev = nullptr;
    if ( NVSDK_NGX_FAILED( NVSDK_NGX_VULKAN_RequiredExtensions( &ni, &inst, &nd, &dev ) ) ) { rt_log( "dlss: no extension list\n" ); return; }
    auto add = []( std::vector<const char *> &to, const char *e ) {
        for ( const char *h : to )
            if ( !strcmp( h, e ) ) return;
        to.push_back( e );
    };
    for ( unsigned i = 0; i < ni; ++i ) add( instance, inst[i] );
    for ( unsigned i = 0; i < nd; ++i ) add( device, dev[i] );
}

bool dlss_init( VkInstance instance, VkPhysicalDevice phys, VkDevice dev )
{
    g_dev = dev;
    // The feature library (libnvidia-ngx-dlss.so.*) is looked for beside the program; NGX keeps its own
    // files in the cache folder.
    char exe[4096] = {};
    ssize_t n = readlink( "/proc/self/exe", exe, sizeof( exe ) - 1 );
    std::string dir = n > 0 ? std::filesystem::path( std::string( exe, n ) ).parent_path().string() : ".";
    const char *xdg = getenv( "XDG_CACHE_HOME" ), *home = getenv( "HOME" );
    std::string data = ( xdg && *xdg ? std::string( xdg ) : std::string( home ? home : "." ) + "/.cache" ) + "/crackdown-recomp/ngx";
    std::error_code ec;
    std::filesystem::create_directories( data, ec );
    static std::wstring wdir = wide( dir ), wdata = wide( data );
    static wchar_t *paths[] = { wdir.data() };
    NVSDK_NGX_FeatureCommonInfo info{};
    info.PathListInfo.Path = paths;
    info.PathListInfo.Length = 1;
    info.LoggingInfo.LoggingCallback = ngx_log;
    info.LoggingInfo.MinimumLoggingLevel = getenv( "CRACKDOWN_DLSS_LOG" ) ? NVSDK_NGX_LOGGING_LEVEL_VERBOSE : NVSDK_NGX_LOGGING_LEVEL_OFF;
    NVSDK_NGX_Result r = NVSDK_NGX_VULKAN_Init_with_ProjectID( "6c0b3f2e-9a51-4d7e-8b1a-c2d0e5f74a19", NVSDK_NGX_ENGINE_TYPE_CUSTOM, "1.0", wdata.c_str(), instance,
                                                               phys, dev, vkGetInstanceProcAddr, vkGetDeviceProcAddr, &info );
    if ( NVSDK_NGX_FAILED( r ) ) { rt_log( "dlss: NGX didn't start (%08X)\n", (unsigned)r ); return false; }
    if ( NVSDK_NGX_FAILED( NVSDK_NGX_VULKAN_GetCapabilityParameters( &g_params ) ) ) { rt_log( "dlss: no capabilities\n" ); return false; }
    int available = 0, driver_update = 0;
    NVSDK_NGX_Parameter_GetI( g_params, NVSDK_NGX_Parameter_SuperSampling_Available, &available );
    NVSDK_NGX_Parameter_GetI( g_params, NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &driver_update );
    if ( !available )
    {
        int code = 0;
        NVSDK_NGX_Parameter_GetI( g_params, NVSDK_NGX_Parameter_SuperSampling_FeatureInitResult, &code );
        rt_log( "dlss: not available here (%s, result %08X)\n", driver_update ? "the driver is too old" : "no RTX card or no library", (unsigned)code );
        return false;
    }
    g_ready = true;
    rt_log( "dlss: ready (library from %s)\n", dir.c_str() );
    return true;
}

bool dlss_ready() { return g_ready; }

float dlss_share() { return MODE_SHARE[mode()]; }

static NVSDK_NGX_Resource_VK resource( const DlssImage &i, bool write )
{
    return NVSDK_NGX_Create_ImageView_Resource_VK( i.view, i.image, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 }, i.format, i.w, i.h, write );
}

bool dlss_evaluate( VkCommandBuffer cmd, const DlssImage &color, const DlssImage &depth, const DlssImage &motion, const DlssImage *mask, const DlssImage &out,
                    float jitter_x, float jitter_y, bool reset )
{
    if ( !g_ready ) return false;
    const int preset = std::clamp( settings().dlss_preset, 0, 4 );  // (changes at once)
    if ( g_feature && ( g_in_w != color.w || g_in_h != color.h || g_out_w != out.w || g_out_h != out.h || g_preset != preset ) )
    {
        vkDeviceWaitIdle( g_dev );
        NVSDK_NGX_VULKAN_ReleaseFeature( g_feature );
        g_feature = nullptr;
    }
    if ( !g_feature )
    {
        NVSDK_NGX_DLSS_Create_Params p{};
        p.Feature.InWidth = color.w, p.Feature.InHeight = color.h;
        p.Feature.InTargetWidth = out.w, p.Feature.InTargetHeight = out.h;
        p.Feature.InPerfQualityValue = color.w >= out.w ? NVSDK_NGX_PerfQuality_Value_DLAA : MODE_VALUE[mode()];
        for ( const char *hint : { NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality,
                                   NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance,
                                   NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality } )
            NVSDK_NGX_Parameter_SetUI( g_params, hint, PRESET[preset] );
        p.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes | NVSDK_NGX_DLSS_Feature_Flags_DepthInverted;  // (the game's depth: 1 near, 0 far)
        NVSDK_NGX_Result r = NGX_VULKAN_CREATE_DLSS_EXT1( g_dev, cmd, 1, 1, &g_feature, g_params, &p );
        if ( NVSDK_NGX_FAILED( r ) )
        {
            rt_log( "dlss: couldn't make the feature for %ux%u -> %ux%u (%08X); DLSS off\n", color.w, color.h, out.w, out.h, (unsigned)r );
            g_feature = nullptr, g_ready = false;
            return false;
        }
        g_in_w = color.w, g_in_h = color.h, g_out_w = out.w, g_out_h = out.h, g_preset = preset;
        static const char *const preset_name[] = { "the mode's default", "K", "L", "M", "J" };
        rt_log( "dlss: %ux%u -> %ux%u (%s, model %s)\n", color.w, color.h, out.w, out.h, color.w >= out.w ? "DLAA" : MODE_NAME[mode()], preset_name[preset] );
        reset = true;
    }
    NVSDK_NGX_Resource_VK c = resource( color, false ), d = resource( depth, false ), m = resource( motion, false ), o = resource( out, true );
    NVSDK_NGX_VK_DLSS_Eval_Params e{};
    e.Feature.pInColor = &c;
    e.Feature.pInOutput = &o;
    e.pInDepth = &d;
    e.pInMotionVectors = &m;
    NVSDK_NGX_Resource_VK b;
    if ( mask ) b = resource( *mask, false ), e.pInBiasCurrentColorMask = &b;
    e.InJitterOffsetX = jitter_x, e.InJitterOffsetY = jitter_y;
    e.InRenderSubrectDimensions = { color.w, color.h };
    e.InReset = reset;
    e.InMVScaleX = 1.0f, e.InMVScaleY = 1.0f;
    NVSDK_NGX_Result r = NGX_VULKAN_EVALUATE_DLSS_EXT( cmd, g_feature, g_params, &e );
    if ( NVSDK_NGX_FAILED( r ) )
    {
        static int logged;
        if ( logged++ < 5 ) rt_log( "dlss: evaluate failed (%08X)\n", (unsigned)r );
        return false;
    }
    return true;
}

#else  // built without the DLSS SDK

bool dlss_wanted() { return false; }
void dlss_extensions( std::vector<const char *> &, std::vector<const char *> & ) {}
bool dlss_init( VkInstance, VkPhysicalDevice, VkDevice ) { return false; }
bool dlss_ready() { return false; }
float dlss_share() { return 1.0f; }
bool dlss_evaluate( VkCommandBuffer, const DlssImage &, const DlssImage &, const DlssImage &, const DlssImage *, const DlssImage &, float, float, bool ) { return false; }

#endif
