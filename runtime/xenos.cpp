// The drawing side of the Xenos GPU: shader programs the command processor loads and the draw calls
// it issues. Stage 2a: record and describe them (per-frame statistics) ahead of real rendering.
#include "ppc_config.h"
#include "ppc_context.h"
#include "runtime.h"
#include "renderer.h"
#include "ucode.h"

#define XXH_INLINE_ALL
#include "xxhash.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <string>
#include <vector>

namespace
{

struct Shader
{
    uint32_t type;                  // 0 vertex, 1 pixel
    std::vector<uint32_t> ucode;    // microcode dwords (host order)
    uint64_t hash;
    ucode::Program program;
};

std::unordered_map<uint64_t, Shader> g_shaders;
// The same loads by a fast hash of the guest words (thousands of loads a frame, few distinct programs).
std::unordered_map<uint64_t, const Shader *> g_loaded;
const Shader *g_bound[2];

uint64_t fnv1a( const std::vector<uint32_t> &v )
{
    uint64_t h = 1469598103934665603ull;
    for ( uint32_t d : v ) for ( int i = 0; i < 4; ++i ) { h ^= ( d >> ( i * 8 ) ) & 0xFF; h *= 1099511628211ull; }
    return h;
}

struct FrameStats { uint32_t draws, indices, shader_loads; std::unordered_map<uint32_t, uint32_t> prims; };
FrameStats g_frame;
uint64_t g_frames;

} // namespace

// The program for microcode in guest memory, translated the first time it's seen. Native submission
// (gpu.cpp) finds programs as D3D's commands are decoded and binds them when they run.
const void *xenos_shader_find( uint32_t type, uint32_t addr, uint32_t dwords )
{
    if ( dwords > 0x4000 ) return nullptr;
    uint64_t key = XXH3_64bits_withSeed( g_base + addr, dwords * 4, type & 1 );
    if ( auto hit = g_loaded.find( key ); hit != g_loaded.end() ) return hit->second;
    Shader s;
    s.type = type & 1;
    s.ucode.resize( dwords );
    for ( uint32_t i = 0; i < dwords; ++i ) s.ucode[i] = guest_read32( addr + i * 4 );
    s.hash = fnv1a( s.ucode ) ^ s.type;
    s.program.type = s.type;
    s.program.words = s.ucode;
    s.program.parse();
    auto [it, fresh] = g_shaders.emplace( s.hash, std::move( s ) );
    if ( fresh && ucode::uses_memory_export( it->second.program ) )  // nothing the game was seen to bind does: say so if one does
        rt_log( "renderer: shader %016llx writes memory (memexport), which isn't implemented\n", (unsigned long long)it->first );
    if ( fresh && RT_ENV( "CRACKDOWN_DUMP_SHADERS" ) )  // raw microcode for offline translator work (game data: stays in build/)
    {
        char path[96];
        snprintf( path, sizeof(path), "build/shaders/%s_%016llx.bin", it->second.type ? "ps" : "vs", (unsigned long long)it->first );
        if ( FILE *f = fopen( path, "wb" ) ) { fwrite( it->second.ucode.data(), 4, it->second.ucode.size(), f ); fclose( f ); }
    }
    g_loaded.emplace( key, &it->second );
    return &it->second;
}

void xenos_shader_bind( const void *shader )
{
    if ( !shader ) return;
    auto *s = (const Shader *)shader;
    g_bound[s->type] = s;
    ++g_frame.shader_loads;
}

void xenos_shader_load( uint32_t type, uint32_t addr, uint32_t dwords ) { xenos_shader_bind( xenos_shader_find( type, addr, dwords ) ); }

// VGT_DRAW_INITIATOR: bits 0-5 primitive type, 6-7 index source (0 DMA, 1 immediate, 2 auto),
// bit 11 32-bit indices, bits 16-31 index count.
void xenos_draw( uint32_t initiator, uint32_t index_base, uint32_t index_info, const uint32_t *regs )
{
    if ( ( regs[reg::RB_MODECONTROL] & 7 ) == 6 ) renderer_resolve( regs, g_bound[0] ? &g_bound[0]->program : nullptr );  // copy mode: a resolve
    else if ( g_bound[0] && g_bound[1] )
        renderer_draw( regs, initiator, index_base, index_info, { g_bound[0]->hash, &g_bound[0]->program }, { g_bound[1]->hash, &g_bound[1]->program } );
    ++g_frame.draws;
    g_frame.indices += initiator >> 16;
    ++g_frame.prims[initiator & 0x3F];
}

void xenos_frame_end()
{
    renderer_flush();
    ++g_frames;
    renderer_frame_end( g_frames );
    static const long every = RT_ENV( "CRACKDOWN_DRAW_STATS" ) ? atol( RT_ENV( "CRACKDOWN_DRAW_STATS" ) ) : 0;
    if ( every && g_frames % every == 0 )
    {
        std::string prims;
        for ( auto &[p, c] : g_frame.prims ) prims += " prim" + std::to_string( p ) + "=" + std::to_string( c );
        rt_log( "xenos: frame %llu: %u draws, %u indices, %u shader loads, %zu distinct shaders so far;%s\n",
                (unsigned long long)g_frames, g_frame.draws, g_frame.indices, g_frame.shader_loads, g_shaders.size(), prims.c_str() );
        renderer_print_stats();
    }
    g_frame = FrameStats();
}
