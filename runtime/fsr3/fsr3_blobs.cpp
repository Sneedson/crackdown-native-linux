// The FidelityFX SDK's shader lookup for FSR 3's upscaler (it takes the place of the SDK's
// ffx_fsr3upscaler_shaderblobs.cpp, which needs headers made by AMD's Windows-only shader compiler):
// the SPIR-V devtools/build_fsr3_shaders.sh compiled (fsr3_spirv.h), with what that compiler's reflection
// would say about each shader read from the SPIR-V itself: the name, binding and kind of every resource
// (images to read, images to write, uniform blocks). The SDK finds its resources by those names.
#include <FidelityFX/host/ffx_fsr3upscaler.h>
#include "blob_accessors/ffx_fsr3upscaler_shaderblobs.h"
#include "fsr3upscaler/ffx_fsr3upscaler_private.h"
#include "fsr3_spirv.h"
#include <map>
#include <string>
#include <vector>

void rt_log( const char *fmt, ... );  // runtime.h (not included: the SDK's headers come first here)

namespace
{
// The flags the shaders were compiled for (build_fsr3_shaders.sh); the rest pick a variant.
constexpr uint32_t FIXED = FSR3UPSCALER_SHADER_PERMUTATION_LOW_RES_MOTION_VECTORS | FSR3UPSCALER_SHADER_PERMUTATION_DEPTH_INVERTED;
constexpr uint32_t CHECKED = FSR3UPSCALER_SHADER_PERMUTATION_HDR_COLOR_INPUT | FSR3UPSCALER_SHADER_PERMUTATION_LOW_RES_MOTION_VECTORS |
                             FSR3UPSCALER_SHADER_PERMUTATION_JITTER_MOTION_VECTORS | FSR3UPSCALER_SHADER_PERMUTATION_DEPTH_INVERTED;

struct Group
{
    std::vector<const char *> names;
    std::vector<uint32_t> bindings, counts, spaces;
    void add( const char *n, uint32_t b, uint32_t c ) { names.push_back( n ), bindings.push_back( b ), counts.push_back( c ), spaces.push_back( 0 ); }
};

struct Reflected
{
    std::vector<std::string> strings;  // (owns the names)
    Group cbs, srv_textures, uav_textures;
};

// Reads the descriptor bindings of a SPIR-V module: OpName, OpDecorate (Binding, Block), the types and
// the variables. Samplers (bound by the backend at 1000 up) are left out.
bool reflect( const uint32_t *w, size_t words, Reflected &r )
{
    if ( words < 5 || w[0] != 0x07230203 ) return false;
    std::map<uint32_t, std::string> names;
    std::map<uint32_t, uint32_t> binding, constant;
    std::map<uint32_t, bool> block;
    struct Type { uint32_t op; std::vector<uint32_t> a; };
    std::map<uint32_t, Type> types;
    struct Var { uint32_t type, id, storage; };
    std::vector<Var> vars;
    std::map<uint32_t, bool> used;  // the entry point's interface: from SPIR-V 1.4 on, every global it uses
    for ( size_t i = 5; i < words; )
    {
        const uint32_t op = w[i] & 0xFFFF, n = w[i] >> 16;
        if ( !n || i + n > words ) return false;
        const uint32_t *a = w + i + 1;
        switch ( op )
        {
        case 5: names[a[0]] = (const char *)( a + 1 ); break;  // OpName
        case 15:  // OpEntryPoint: model, id, name (padded to words), interface ids
        {
            size_t k = 2;
            while ( k < n - 1 && ( a[k] >> 24 ) ) ++k;  // the word holding the name's last byte (its high byte is 0)
            for ( ++k; k < n - 1; ++k ) used[a[k]] = true;
            break;
        }
        case 71:  // OpDecorate
            if ( a[1] == 33 ) binding[a[0]] = a[2];
            if ( a[1] == 2 ) block[a[0]] = true;
            break;
        case 25: case 26: case 27: case 28: case 29: case 30: case 32:  // image, sampler, sampled image, arrays, struct, pointer
            types[a[0]] = { op, std::vector<uint32_t>( a, a + n - 1 ) };
            break;
        case 43: constant[a[1]] = a[2]; break;  // OpConstant
        case 59: vars.push_back( { a[0], a[1], a[2] } ); break;  // OpVariable
        }
        i += n;
    }
    // Only what the shader uses, as AMD's compiler reports (a declared, unused image's name is unknown to
    // the SDK's tables).
    std::vector<Var> kept;
    for ( const Var &v : vars )
        if ( used.count( v.id ) ) kept.push_back( v );
    vars.swap( kept );
    for ( const Var &v : vars )
    {
        if ( !binding.count( v.id ) || !types.count( v.type ) ) continue;
        uint32_t t = types[v.type].a[2], count = 1;  // the pointer's type
        while ( types.count( t ) && ( types[t].op == 28 || types[t].op == 29 ) )
        {
            if ( types[t].op == 28 ) count *= constant[types[t].a[2]];
            t = types[t].a[1];
        }
        if ( !types.count( t ) ) continue;
        r.strings.push_back( names[v.id] );
        const Type &ty = types[t];
        if ( ty.op == 25 && ty.a.size() > 6 ) ( ty.a[6] == 2 ? r.uav_textures : r.srv_textures ).add( nullptr, binding[v.id], count );
        else if ( ty.op == 30 && v.storage == 2 && block.count( t ) ) r.cbs.add( nullptr, binding[v.id], count );
        else if ( ty.op == 26 ) { r.strings.pop_back(); continue; }
        else { rt_log( "fsr3: shader resource %s of a kind not handled\n", names[v.id].c_str() ); return false; }
    }
    // The names point into strings, which no longer grows.
    size_t k = 0;
    for ( const Var &v : vars )
    {
        if ( !binding.count( v.id ) || !types.count( v.type ) ) continue;
        uint32_t t = types[v.type].a[2];
        while ( types.count( t ) && ( types[t].op == 28 || types[t].op == 29 ) ) t = types[t].a[1];
        if ( types[t].op == 26 ) continue;
        const char *name = r.strings[k++].c_str();
        for ( Group *g : { &r.cbs, &r.srv_textures, &r.uav_textures } )
            for ( size_t j = 0; j < g->bindings.size(); ++j )
                if ( g->bindings[j] == binding[v.id] && !g->names[j] ) g->names[j] = name;
    }
    return true;
}
} // namespace

extern "C" FfxErrorCode fsr3UpscalerGetPermutationBlobByIndex( FfxFsr3UpscalerPass pass, uint32_t options, FfxShaderBlob *out )
{
    if ( (uint32_t)pass >= sizeof( fsr3_spirv ) / sizeof( fsr3_spirv[0] ) || ( options & CHECKED ) != FIXED )
    {
        rt_log( "fsr3: no shader for pass %d with options %X\n", (int)pass, options );
        return FFX_ERROR_INVALID_ARGUMENT;
    }
    // (FP16 and wave 64 variants aren't made: the 32-bit, default-wave shaders serve.)
    const int variant = ( options & FSR3UPSCALER_SHADER_PERMUTATION_USE_LANCZOS_TYPE ? 1 : 0 ) | ( options & FSR3UPSCALER_SHADER_PERMUTATION_ENABLE_SHARPENING ? 2 : 0 );
    static std::map<int, Reflected> cache;
    const Fsr3Spirv &s = fsr3_spirv[pass][variant];
    const int key = pass * 4 + variant;
    if ( !cache.count( key ) && !reflect( s.code, s.bytes / 4, cache[key] ) )
    {
        cache.erase( key );
        rt_log( "fsr3: couldn't read shader %d.%d\n", (int)pass, variant );
        return FFX_ERROR_INVALID_ARGUMENT;
    }
    Reflected &r = cache[key];
    FfxShaderBlob blob = { (const uint8_t *)s.code, (uint32_t)s.bytes,
                           (uint32_t)r.cbs.names.size(), (uint32_t)r.srv_textures.names.size(), (uint32_t)r.uav_textures.names.size(), 0, 0, 0, 0,
                           r.cbs.names.data(), r.cbs.bindings.data(), r.cbs.counts.data(), r.cbs.spaces.data(),
                           r.srv_textures.names.data(), r.srv_textures.bindings.data(), r.srv_textures.counts.data(), r.srv_textures.spaces.data(),
                           r.uav_textures.names.data(), r.uav_textures.bindings.data(), r.uav_textures.counts.data(), r.uav_textures.spaces.data(),
                           nullptr, nullptr, nullptr, nullptr,
                           nullptr, nullptr, nullptr, nullptr,
                           nullptr, nullptr, nullptr, nullptr,
                           nullptr, nullptr, nullptr, nullptr };
    memcpy( (void *)out, &blob, sizeof( blob ) );
    return FFX_OK;
}

extern "C" FfxErrorCode fsr3UpscalerIsWave64( uint32_t, bool &wave64 )
{
    wave64 = false;
    return FFX_OK;
}

// The SDK's Vulkan backend points at its frame generation swapchain, which isn't built here (no frame
// generation: only the upscaler is used).
#include <FidelityFX/host/backends/vk/ffx_vk.h>
FFX_API FfxErrorCode ffxSetFrameGenerationConfigToSwapchainVK( FfxFrameGenerationConfig const * ) { return FFX_ERROR_BACKEND_API_ERROR; }
