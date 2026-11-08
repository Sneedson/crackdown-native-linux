#!/bin/bash
# Generates build/d3dtrace/d3d_emit.cpp: which GPU commands each D3D entry point the game calls writes.
# Every entry point (build/d3d_entries.txt) is wrapped; a call from outside D3D (0x82730000-0x8275FFFF)
# notes the device's command pointer (device+48, D3D writes with stwu so it points at the last word) and
# on return parses the packets written since. A call that is handed a command pointer in r4 and returns
# the new one (the fence and wait helpers game code inlines around) is parsed from r4 to r3. Command
# buffer switches (827447A0) inside a call end one run and start the next. At swap CRACKDOWN_D3D_EMIT
# the table is written to build/d3d_emit.txt: per function, calls and "packet x count" (type 0 packets
# by first register, type 3 by opcode).
# devtools/d3d_trace_build.sh emit links it into build/crackdown-emit.
cd "$(dirname "$0")/.."
grep -hoE "PPC_FUNC\( *sub_[0-9A-F]*|CONVERTED(_DEV)?\( *[0-9A-F]{8}" runtime/*.cpp | grep -oE "[0-9A-F]{8}" | sort -u > build/d3dtrace/overridden.txt
{
cat <<'H'
#include "ppc_config.h"
#include "ppc_context.h"
#include "runtime.h"
#include <cstdio>
#include <cstdlib>
#include <atomic>
#include <map>
#include <mutex>
uint64_t gpu_swap_count();
namespace {
uint32_t dev_ptr() { return guest_read32( guest_read32( 0x82000798 ) ); }  // the device (its address varies)
#define DEV dev_ptr()
uint64_t g_at = ~0ull;
std::mutex g_lock;
std::map<uint32_t, std::map<uint32_t, uint64_t>> g_table;  // function -> packet key -> count
std::map<uint32_t, uint64_t> g_calls;
std::atomic<bool> g_done;
thread_local int t_depth;
thread_local uint32_t t_func, t_p0;
struct Init { Init() { if ( const char *s = getenv( "CRACKDOWN_D3D_EMIT" ) ) g_at = strtoull( s, nullptr, 0 ); } } g_init;
bool on() { return g_at != ~0ull; }
// Packets in (from, to]: D3D's pointer addresses the last word written.
void parse( uint32_t func, uint32_t from, uint32_t to )
{
    if ( to <= from || to - from > 0x100000 ) return;
    std::lock_guard<std::mutex> l( g_lock );
    auto &t = g_table[func];
    for ( uint32_t a = from + 4; a <= to; )
    {
        uint32_t h = guest_read32( a ), type = h >> 30, n = ( ( h >> 16 ) & 0x3FFF ) + 1, len;
        uint32_t key;
        if ( type == 0 ) key = 0x00000000 | ( h & 0x7FFF ), len = n + 1;
        else if ( type == 1 ) key = 0x10000000 | ( h & 0x7FF ), len = 3;
        else if ( type == 2 ) key = 0x20000000, len = 1;
        else key = 0x30000000 | ( ( h >> 8 ) & 0x7F ), len = n + 1;
        ++t[key];
        a += len * 4;
    }
}
void dump()
{
    FILE *f = fopen( "build/d3d_emit.txt", "w" );
    if ( !f ) return;
    std::lock_guard<std::mutex> l( g_lock );
    for ( auto &[fn, n] : g_calls )
    {
        fprintf( f, "%08X calls=%lu", fn, n );
        for ( auto &[k, c] : g_table[fn] )
        {
            uint32_t type = k >> 28;
            if ( type == 0 ) fprintf( f, " r%04X:%lu", k & 0xFFFF, c );
            else if ( type == 1 ) fprintf( f, " t1:%lu", c );
            else if ( type == 2 ) fprintf( f, " nop:%lu", c );
            else fprintf( f, " op%02X:%lu", k & 0x7F, c );
        }
        fprintf( f, "\n" );
    }
    fclose( f );
    rt_log( "d3d emit: table written at swap %lu\n", gpu_swap_count() );
}
bool outside( uintptr_t ctx ) { uint32_t lr = (uint32_t)( (PPCContext *)( ctx - 0x80 ) )->lr; return lr < 0x82730000 || lr >= 0x82760000; }
}
H
while read n a; do
  if [ "$a" = 827447A0 ]; then
    echo "extern \"C\" PPC_FUNC( __imp__sub_$a ); PPC_FUNC( sub_$a ) {
    PPCContext *c = (PPCContext *)( ctx - 0x80 ); uint32_t dev = c->r3.u32;
    bool ext = on() && outside( ctx ) && t_depth == 0, in = on() && t_depth > 0 && dev == DEV;
    if ( ext ) t_func = 0x$a, t_p0 = guest_read32( DEV + 48 );
    if ( ext || in ) parse( t_func, t_p0, guest_read32( DEV + 48 ) );
    ++t_depth; __imp__sub_$a( ctx, base, frame ); --t_depth;
    if ( ext || in ) t_p0 = guest_read32( DEV + 48 );
    if ( ext ) { std::lock_guard<std::mutex> l( g_lock ); ++g_calls[0x$a]; } }"
  else
    echo "extern \"C\" PPC_FUNC( __imp__sub_$a ); PPC_FUNC( sub_$a ) {
    PPCContext *c = (PPCContext *)( ctx - 0x80 );
    bool ext = on() && outside( ctx ) && t_depth == 0;
    uint32_t r4 = c->r4.u32;
    if ( ext ) t_func = 0x$a, t_p0 = guest_read32( DEV + 48 );
    ++t_depth; __imp__sub_$a( ctx, base, frame ); --t_depth;
    if ( !ext ) return;
    uint32_t p1 = guest_read32( DEV + 48 );
    if ( p1 != t_p0 ) parse( 0x$a, t_p0, p1 );
    else if ( c->r3.u32 > r4 && c->r3.u32 - r4 < 0x10000 && r4 >= 0x40000000 ) parse( 0x$a, r4, c->r3.u32 );
    { std::lock_guard<std::mutex> l( g_lock ); ++g_calls[0x$a]; }
    if ( !g_done && gpu_swap_count() >= g_at && !g_done.exchange( true ) ) dump(); }"
  fi
done < <(grep -vwFf build/d3dtrace/overridden.txt build/d3d_entries.txt)
} > build/d3dtrace/d3d_emit.cpp
