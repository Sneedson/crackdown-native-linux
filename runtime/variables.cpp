// Kernel data exports the game imports by address (KeTimeStampBundle, XboxHardwareInfo, object
// types...). The XEX import records of type 0 are slots the loader fills with the variable's
// guest address; we allocate each variable in guest memory and keep the live ones updated.
#include "ppc_config.h"
#include "ppc_context.h"
#include "runtime.h"

#include <xex.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <unordered_map>

extern std::unordered_map<size_t, const char *> XboxKernelExports, XamExports;  // XenonUtils

namespace
{

uint32_t g_timestamp_bundle;

// KTIMESTAMP_BUNDLE { u64 InterruptTime; u64 SystemTime; u32 TickCount; } -- refreshed every ms.
void timestamp_thread()
{
    auto start = std::chrono::steady_clock::now();
    for ( ;; )
    {
        auto since = std::chrono::steady_clock::now() - start;
        uint64_t t100 = (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>( since ).count() / 100;
        uint64_t sys = (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
                           std::chrono::system_clock::now().time_since_epoch() ).count() / 100 + 116444736000000000ull;
        guest_write64( g_timestamp_bundle + 0, t100 );
        guest_write64( g_timestamp_bundle + 8, sys );
        guest_write32( g_timestamp_bundle + 16, (uint32_t)( t100 / 10000 ) );
        std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
    }
}

// Creates the variable `name` and returns its guest address.
uint32_t make_variable( const std::string &name )
{
    uint32_t p = rt_alloc_virtual( 0x100, 0x10 );
    memset( g_base + p, 0, 0x100 );
    if ( name == "KeTimeStampBundle" )
    {
        g_timestamp_bundle = p;
        std::thread( timestamp_thread ).detach();
    }
    else if ( name == "XboxHardwareInfo" ) { guest_write32( p, 0x20 ); g_base[p + 4] = 6; }  // HDD present, 6 threads
    else if ( name == "XboxKrnlVersion" || name == "XboxKrnlBaseVersion" )
    {
        guest_write16( p, 2 ); guest_write16( p + 2, 0 ); guest_write16( p + 4, 17559 ); guest_write16( p + 6, 0 );
    }
    else if ( name == "VdGpuClockInMHz" ) guest_write32( p, 500 );
    else if ( name == "VdGlobalDevice" && ( getenv( "CRACKDOWN_WATCH_PENDING" ) || getenv( "CRACKDOWN_HACK_PENDING" ) ) )
    {
        // Debug: sample D3D's pending-callback counter (device+10872) every millisecond.
        std::thread( [p] {
            int32_t last = 0;
            int logged = 0;
            for ( ;; )
            {
                std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
                uint32_t dev = guest_read32( p );
                if ( !dev ) continue;
                int32_t v = (int32_t)guest_read32( dev + 10872 );
                if ( v != last && logged++ < 60 && getenv( "CRACKDOWN_WATCH_PENDING" ) )
                    rt_log( "pending counter %d -> %d (frames %llu)\n", last, v, (unsigned long long)g_stats.swaps );
                // EXPERIMENT (CRACKDOWN_HACK_PENDING): the counter goes negative because of an emulation
                // bug not yet found; clamping it shows what blocks next. Not a fix.
                if ( v < 0 && getenv( "CRACKDOWN_HACK_PENDING" ) ) { guest_write32( dev + 10872, 0 ); v = 0; }
                last = v;
            }
        } ).detach();
    }
    else if ( name == "ExConsoleGameRegion" ) guest_write32( p, 0xFF );
    else if ( name.find( "ObjectType" ) == std::string::npos && name != "XexExecutableModuleHandle" &&
              name != "KeDebugMonitorData" && name != "VdGlobalDevice" && name != "VdGlobalXamDevice" &&
              name != "VdHSIOCalibrationLock" && name != "KeCertMonitorData" )
        rt_log( "variable import %s: zero-filled placeholder\n", name.c_str() );
    return p;
}

} // namespace

// Static TLS (__declspec(thread)): XEX_HEADER_TLS_INFO { slots, raw data address, data size,
// raw data size }. Every guest thread gets a copy of the template; r13 (the PCR) points at it.
uint32_t g_tls_raw, g_tls_size, g_tls_raw_size;

uint32_t rt_new_tls_block()
{
    uint32_t size = g_tls_size ? g_tls_size : 0x100;
    uint32_t p = rt_alloc_virtual( ( size + 0xF ) & ~0xFu, 0x10 );
    memset( g_base + p, 0, size );
    if ( g_tls_raw && g_tls_raw_size ) memcpy( g_base + p, g_base + g_tls_raw, g_tls_raw_size );
    return p;
}

void rt_bind_variable_imports( const uint8_t *xex )
{
    if ( auto *tls = (const be<uint32_t> *)getOptHeaderPtr( xex, 0x00020104 ) )
    {
        g_tls_raw = tls[1];
        g_tls_size = tls[2];
        g_tls_raw_size = tls[3];
        rt_log( "static TLS: %u bytes, template %08X (%u bytes)\n", g_tls_size, g_tls_raw, g_tls_raw_size );
    }
    auto *imports = reinterpret_cast<const Xex2ImportHeader *>( getOptHeaderPtr( xex, XEX_HEADER_IMPORT_LIBRARIES ) );
    if ( !imports ) return;
    std::vector<std::string> names;
    const char *strings = reinterpret_cast<const char *>( imports + 1 );
    for ( size_t i = 0, off = 0; i < imports->numImports; ++i )
    {
        names.emplace_back( strings + off );
        off += ( names.back().size() + 1 + 3 ) & ~3;
    }
    auto *lib = (const Xex2ImportLibrary *)( (const char *)imports + sizeof(Xex2ImportHeader) + imports->sizeOfStringTable );
    const char *end = (const char *)imports + imports->sizeOfHeader;
    unsigned bound = 0;
    std::unordered_map<std::string, uint32_t> made;
    while ( (const char *)lib + sizeof(Xex2ImportLibrary) <= end && lib->size != 0 )
    {
        const std::string &libname = lib->name < names.size() ? names[lib->name] : std::string();
        auto *table = libname == "xboxkrnl.exe" ? &XboxKernelExports : libname == "xam.xex" ? &XamExports : nullptr;
        auto *desc = (const Xex2ImportDescriptor *)( lib + 1 );
        // Every import has a type-0 record; a function's is followed by its thunk, which the
        // XenonUtils loader has already overwritten with nop/nop/nop/blr. Variables are the type-0
        // records not followed by a thunk. (The loader left type-0 records in host byte order.)
        auto is_thunk = [&]( size_t i ) { return i < lib->numberOfImports && *(uint32_t *)( g_base + desc[i].firstThunk ) == 0x00000060u; };
        if ( getenv( "CRACKDOWN_DUMP_IMPORTS" ) )
            for ( size_t i = 0; i < lib->numberOfImports && i < 8; ++i )
                rt_log( "%s rec %zu at %08X: raw %08X next4 %08X\n", libname.c_str(), i, (uint32_t)desc[i].firstThunk,
                        *(uint32_t *)( g_base + desc[i].firstThunk ), *(uint32_t *)( g_base + desc[i].firstThunk + 4 ) );
        for ( size_t i = 0; table && i < lib->numberOfImports; ++i )
        {
            uint32_t slot = desc[i].firstThunk;
            uint32_t v = *(uint32_t *)( g_base + slot );
            if ( is_thunk( i ) || ( v >> 24 ) != 0 || is_thunk( i + 1 ) ) continue;
            auto it = table->find( v & 0xFFFF );
            if ( it == table->end() ) continue;
            std::string name = it->second + 10;         // strip "__import__"
            uint32_t &addr = made[name];
            if ( !addr ) addr = make_variable( name );
            guest_write32( slot, addr );
            ++bound;
        }
        lib = (const Xex2ImportLibrary *)( (const char *)( lib + 1 ) + lib->numberOfImports * sizeof(Xex2ImportDescriptor) );
    }
    rt_log( "kernel variables: %u import slots bound (%zu variables)\n", bound, made.size() );
    for ( auto &[n, a] : made ) rt_log( "  %s at %08X\n", n.c_str(), a );
}
