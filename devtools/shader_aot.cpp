// Ahead-of-time shader compilation: every shader program in the game's files (devtools/shader_scan's
// list) is translated as the renderer would (xlate::module_source) and compiled to SPIR-V into the
// renderer's cache (~/.cache/crackdown-recomp/spirv/<xlate::source_hash>.spv), so no shader is
// translated or compiled while the game runs. The files don't say which stage a program is for, so
// each is done as both (the wrong one is never looked up). What D3D rewrites in a vertex shader when it
// binds it is specialization constants (ucode::vs_specialization), so the templates in the files cover
// every binding; their microcode goes to <cache>/vs_templates.bin, for the renderer to match bound ones.
//   shader_aot <scan list> [threads]
// Build: devtools/build_shader_aot.sh
#include "translate.h"
#include "edram_shaders.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <set>
#include <shaderc/shaderc.h>
#include <sstream>
#include <thread>
#include <vector>

static std::string cache_dir()
{
    const char *xdg = getenv( "XDG_CACHE_HOME" ), *home = getenv( "HOME" );
    std::string d = xdg && *xdg ? std::string( xdg ) : std::string( home ? home : "." ) + "/.cache";
    return d + "/crackdown-recomp/spirv";
}

int main( int argc, char **argv )
{
    std::map<std::string, std::vector<uint32_t>> files;
    struct Job { std::string src; int kind; };
    std::vector<Job> jobs;
    std::set<uint64_t> seen;
    std::set<std::vector<uint32_t>> templates;  // vertex shaders, for the renderer to find bound ones' templates
    std::ifstream list( argv[1] );
    std::string line;
    size_t programs = 0;
    while ( std::getline( list, line ) )
    {
        std::istringstream in( line );
        std::string file, off;
        uint32_t dwords;
        if ( !( in >> file >> off >> dwords ) ) continue;
        auto &w = files[file];
        if ( w.empty() )
        {
            std::ifstream f( file, std::ios::binary );
            std::vector<uint8_t> b{ std::istreambuf_iterator<char>( f ), {} };
            w.resize( b.size() / 4 );
            for ( size_t i = 0; i < w.size(); ++i ) w[i] = (uint32_t)b[i * 4] << 24 | b[i * 4 + 1] << 16 | b[i * 4 + 2] << 8 | b[i * 4 + 3];
        }
        size_t at = strtoul( off.c_str(), nullptr, 16 ) / 4;
        ++programs;
        for ( uint32_t type : { 0u, 1u } )
        {
            ucode::Program p;
            p.type = type;
            p.words.assign( w.begin() + at, w.begin() + std::min( w.size(), at + dwords ) );
            p.parse();
            if ( !ucode::valid_for_stage( p ) ) continue;
            std::string src = xlate::module_source( p );
            int kind = type ? shaderc_fragment_shader : shaderc_vertex_shader;
            if ( src.empty() ) continue;
            if ( type == 0 ) templates.insert( p.words );
            if ( !seen.insert( xlate::source_hash( src, kind ) ).second ) continue;
            jobs.push_back( { std::move( src ), kind } );
        }
    }
    // The renderer's own compute shaders.
    for ( std::string src : { std::string( kResolveShader ), resolve_blocked_source(), std::string( kCopyShader ) } )
        if ( seen.insert( xlate::source_hash( src, shaderc_compute_shader ) ).second ) jobs.push_back( { std::move( src ), shaderc_compute_shader } );
    std::string dir = cache_dir();
    std::filesystem::create_directories( dir );
    // <cache>/vs_templates.bin (renderer.cpp vs_template): per vertex shader its dword count and dwords.
    if ( FILE *f = fopen( ( dir + "/../vs_templates.bin.tmp" ).c_str(), "wb" ) )
    {
        for ( auto &w : templates )
        {
            uint32_t n = (uint32_t)w.size();
            fwrite( &n, 4, 1, f );
            fwrite( w.data(), 4, n, f );
        }
        fclose( f );
        std::filesystem::rename( dir + "/../vs_templates.bin.tmp", dir + "/../vs_templates.bin" );
    }
    std::atomic<size_t> next{ 0 }, compiled{ 0 }, cached{ 0 }, failed{ 0 };
    unsigned threads = argc > 2 ? atoi( argv[2] ) : std::max( 1u, std::thread::hardware_concurrency() );
    std::vector<std::thread> pool;
    std::mutex report;
    for ( unsigned t = 0; t < threads; ++t )
        pool.emplace_back( [&] {
            shaderc_compiler_t compiler = shaderc_compiler_initialize();
            for ( size_t i; ( i = next++ ) < jobs.size(); )
            {
                char name[32];
                snprintf( name, sizeof( name ), "/%016llx.spv", (unsigned long long)xlate::source_hash( jobs[i].src, jobs[i].kind ) );
                std::string path = dir + name;
                if ( std::filesystem::exists( path ) ) { ++cached; continue; }
                // The renderer's options (renderer.cpp compile): Vulkan 1.2, no optimisation (the driver does it).
                shaderc_compile_options_t opt = shaderc_compile_options_initialize();
                shaderc_compile_options_set_target_env( opt, shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_2 );
                shaderc_compile_options_set_optimization_level( opt, shaderc_optimization_level_zero );
                shaderc_compilation_result_t res = shaderc_compile_into_spv( compiler, jobs[i].src.c_str(), jobs[i].src.size(), (shaderc_shader_kind)jobs[i].kind, name + 1, "main", opt );
                shaderc_compile_options_release( opt );
                if ( shaderc_result_get_compilation_status( res ) == shaderc_compilation_status_success )
                {
                    std::string tmp = path + ".tmp";
                    if ( FILE *f = fopen( tmp.c_str(), "wb" ) )
                    {
                        fwrite( shaderc_result_get_bytes( res ), 1, shaderc_result_get_length( res ), f );
                        fclose( f );
                        rename( tmp.c_str(), path.c_str() );
                    }
                    ++compiled;
                }
                else
                {
                    ++failed;
                    std::string e = shaderc_result_get_error_message( res );
                    std::lock_guard<std::mutex> g( report );
                    if ( getenv( "SHADER_AOT_KEEP_FAILED" ) )  // the source, for translator work
                        if ( FILE *o = fopen( ( "build/aot-failed-" + std::string( name + 1 ) + ".glsl" ).c_str(), "w" ) ) { fputs( jobs[i].src.c_str(), o ); fclose( o ); }
                    fprintf( stderr, "%s %s: %s\n", jobs[i].kind == shaderc_vertex_shader ? "vs" : jobs[i].kind == shaderc_fragment_shader ? "ps" : "cs", name + 1,
                             e.substr( 0, e.find( '\n' ) ).c_str() );
                }
                shaderc_result_release( res );
            }
            shaderc_compiler_release( compiler );
        } );
    for ( auto &t : pool ) t.join();
    printf( "%zu programs (%zu vertex shader templates), %zu shader sources: %zu compiled, %zu already cached, %zu failed\n", programs, templates.size(), jobs.size(),
            compiled.load(), cached.load(), failed.load() );
}
