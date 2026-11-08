// crackdown-recomp host: loads Crackdown's default.xex into a 4 GB guest address space and runs
// the recompiled game natively. Stage 1: kernel/xam imports are logged and return 0, so the log
// shows, in order, what the game asks of the system.
//
//   crackdown <path to default.xex>
#include <sys/resource.h>
#include "ppc_config.h"
#include "ppc_context.h"
#include "runtime.h"

#include <image.h>   // XenonUtils (the recompiler's own XEX loader)

#include <sys/mman.h>
#include <sys/resource.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <fstream>
#include <iterator>

extern PPCFuncMapping PPCFuncMappings[];

uint8_t *g_base;  // host address of guest address 0

namespace
{

// Guest-address -> host function, for the code range (one slot per 4-byte instruction).
std::vector<PPCFunc *> g_funcs;

void build_function_table()
{
    g_funcs.assign( PPC_CODE_SIZE / 4 + 1, nullptr );
    for ( int i = 0; i < HostFuncsCount; ++i )
    {
        uint64_t guest = PPC_IMAGE_BASE + PPCFuncMappings[i].guest_rva;
        if ( guest < PPC_CODE_BASE || guest >= PPC_CODE_BASE + PPC_CODE_SIZE ) continue;
        g_funcs[( guest - PPC_CODE_BASE ) / 4] = HostFuncs[i];
    }
    rt_log( "function table: %d functions\n", HostFuncsCount );
}

} // namespace

// Replaces the function indirect calls (vtables, function pointers) reach at guest; returns the old one.
PPCFunc *rt_hook_indirect( uint32_t guest, PPCFunc *f )
{
    PPCFunc *&slot = g_funcs[( guest - PPC_CODE_BASE ) / 4];
    PPCFunc *old = slot;
    slot = f;
    return old;
}

void splash_install();  // splash.cpp
void mouselook_install();  // mouselook.cpp
void native_d3d_install();  // native_d3d.cpp

// Whether an indirect call to guest would reach a recompiled function (rt_lookup without the fatal).
bool rt_has_function( uint32_t guest )
{
    return guest >= PPC_CODE_BASE && guest < PPC_CODE_BASE + PPC_CODE_SIZE && g_funcs[( guest - PPC_CODE_BASE ) / 4];
}

extern "C" PPCFunc *rt_lookup( uint32_t guest )
{
    if ( guest >= PPC_CODE_BASE && guest < PPC_CODE_BASE + PPC_CODE_SIZE )
        if ( PPCFunc *f = g_funcs[( guest - PPC_CODE_BASE ) / 4] ) return f;
    rt_dump_trace( "bad indirect call" );
    rt_fatal( "indirect call to %08X: no recompiled function there\n", guest );
}

int main( int argc, char **argv )
{
    if ( argc < 2 ) { fprintf( stderr, "usage: %s <default.xex>\n", argv[0] ); return 1; }

    // The file descriptor table, grown to 4096 now: Linux grows it in a threaded process by waiting
    // out an RCU grace period (about 25 ms, the whole thread blocked in that open), and the game holds
    // over a hundred files open by the main menu (the 128th was the content check's: a hitch after START).
    if ( rlimit r; !getrlimit( RLIMIT_NOFILE, &r ) )
        if ( int top = (int)std::min<rlim_t>( r.rlim_cur, 4096 ) - 1; top > 2 && dup2( 0, top ) == top ) close( top );

    // The whole 4 GB guest space, reserved lazily (pages are only backed when touched).
    g_base = (uint8_t *)mmap( nullptr, PPC_MEMORY_SIZE, PROT_READ | PROT_WRITE,
                              MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0 );
    if ( g_base == MAP_FAILED ) rt_fatal( "cannot reserve guest memory\n" );

    // Physical memory (512 MB) appears three times in the 360 address space -- 0xA0000000 (64 KB
    // pages), 0xC0000000 (16 MB pages), 0xE0000000 (4 KB pages) -- and the game mixes the views.
    // One shared memory object mapped at all three keeps them coherent.
    int phys = memfd_create( "xenon-physical", 0 );
    if ( phys < 0 || ftruncate( phys, 0x20000000 ) ) rt_fatal( "cannot create physical memory\n" );
    for ( uint32_t view : { 0xA0000000u, 0xC0000000u, 0xE0000000u } )
        if ( mmap( g_base + view, 0x20000000, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, phys, 0 ) == MAP_FAILED )
            rt_fatal( "cannot map physical view %08X\n", view );

    std::ifstream file( argv[1], std::ios::binary );
    std::vector<uint8_t> xex( ( std::istreambuf_iterator<char>( file ) ), std::istreambuf_iterator<char>() );
    if ( xex.empty() ) rt_fatal( "cannot read %s\n", argv[1] );
    Image image = Image::ParseImage( xex.data(), xex.size() );
    if ( !image.data ) rt_fatal( "%s is not a loadable XEX\n", argv[1] );
    if ( image.base != PPC_IMAGE_BASE || image.size > PPC_IMAGE_SIZE )
        rt_fatal( "image at %zx size %x does not match the recompiled one (%llx)\n", image.base, image.size, PPC_IMAGE_BASE );
    memcpy( g_base + image.base, image.data.get(), image.size );
    if ( const char *peek = getenv( "CRACKDOWN_PEEK" ) )  // CRACKDOWN_PEEK=hexaddr: print a loaded word
        rt_log( "peek %s = %08X\n", peek, guest_read32( (uint32_t)strtoul( peek, nullptr, 16 ) ) );
    rt_log( "image %08zX..%08zX, entry %08zX\n", image.base, image.base + image.size, image.entry_point );

    build_function_table();
    if ( getenv( "CRACKDOWN_DUMP_IMAGE" ) )  // debug: the loaded executable image, for devtools/xrefs.py
        if ( FILE *f = fopen( "build/image.bin", "wb" ) ) { fwrite( g_base + image.base, 1, image.size, f ); fclose( f ); }
    splash_install();
    mouselook_install();
    native_d3d_install();
    rt_kernel_init();
    rt_bind_variable_imports( xex.data() );

    rt_run_thread( (uint32_t)image.entry_point, 0, "main" );
    rt_log( "entry point returned\n" );
    return 0;
}
