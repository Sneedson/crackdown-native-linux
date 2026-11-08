#!/bin/bash
# Generates build/d3dtrace/d3d_trace.cpp: a wrapper for every D3D entry point the game calls (build/
# d3d_entries.txt, "count addr" lines) that logs calls from outside D3D (0x82730000-0x8275FFFF) with
# their caller and r3-r8, in the swaps CRACKDOWN_D3D_TRACE=first:last, to build/d3d_trace.log.
# devtools/d3d_trace_build.sh links it into build/crackdown-trace.
cd "$(dirname "$0")/.."
{
cat <<'H'
#include "ppc_config.h"
#include "ppc_context.h"
#include "runtime.h"
#include <cstdio>
#include <cstdlib>
uint64_t gpu_swap_count();
namespace {
FILE *g_out;
uint64_t g_first = ~0ull, g_last;
thread_local int t_depth;
struct Init { Init() { if ( const char *s = getenv( "CRACKDOWN_D3D_TRACE" ) ) { sscanf( s, "%lu:%lu", &g_first, &g_last ); g_out = fopen( "build/d3d_trace.log", "w" ); } } } g_init;
void trace( uint32_t f, uintptr_t ctx )
{
    PPCContext *c = (PPCContext *)( ctx - 0x80 );
    uint32_t lr = (uint32_t)c->lr;
    if ( !g_out || ( lr >= 0x82730000 && lr < 0x82760000 ) ) return;
    uint64_t s = gpu_swap_count();
    if ( s < g_first || s > g_last ) return;
    fprintf( g_out, "%lu %d %08X from %08X: %08X %08X %08X %08X %08X %08X\n", s, t_depth, f, lr, c->r3.u32, c->r4.u32, c->r5.u32, c->r6.u32, c->r7.u32, c->r8.u32 );
}
}
H
while read n a; do
  echo "extern \"C\" PPC_FUNC( __imp__sub_$a ); PPC_FUNC( sub_$a ) { trace( 0x$a, ctx ); ++t_depth; __imp__sub_$a( ctx, base, frame ); --t_depth; }"
done < build/d3d_entries.txt
} > build/d3dtrace/d3d_trace.cpp
