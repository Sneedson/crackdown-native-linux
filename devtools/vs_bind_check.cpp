// Offline check of ucode::binds_to: for each vertex shader dumped as the game bound it (build/shaders,
// CRACKDOWN_DUMP_SHADERS=1), whether one of the templates in <cache>/vs_templates.bin (devtools/
// build_shader_aot.sh) binds to it, and which slots the binding changed.
//   vs_bind_check <vs_templates.bin> <dumped vs .bin>...
// Build: clang++ -std=c++20 -O2 -Iruntime devtools/vs_bind_check.cpp runtime/ucode.cpp -o build/vs_bind_check
#include "ucode.h"
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <vector>

int main( int argc, char **argv )
{
    std::multimap<uint64_t, ucode::Program> templates;
    FILE *f = fopen( argv[1], "rb" );
    for ( uint32_t n; f && fread( &n, 4, 1, f ) == 1; )
    {
        ucode::Program p;
        p.words.resize( n );
        if ( fread( p.words.data(), 4, n, f ) != n ) break;
        p.parse();
        templates.emplace( ucode::template_key( p ), std::move( p ) );
    }
    int found = 0, total = 0, exact = 0;
    for ( int i = 2; i < argc; ++i )
    {
        std::ifstream in( argv[i], std::ios::binary );
        std::vector<uint8_t> b{ std::istreambuf_iterator<char>( in ), {} };
        ucode::Program p;
        p.words.resize( b.size() / 4 );
        memcpy( p.words.data(), b.data(), b.size() & ~3u );
        p.parse();
        ++total;
        const ucode::Program *t = nullptr;
        auto [lo, hi] = templates.equal_range( ucode::template_key( p ) );
        for ( ; lo != hi && !t; ++lo )
            if ( ucode::binds_to( lo->second, p ) ) t = &lo->second;
        if ( !t ) { printf( "%s: NO TEMPLATE\n", argv[i] ); continue; }
        ++found;
        exact += t->words == p.words;
        int changed = 0;
        for ( size_t w = 0; w < p.words.size(); w += 3 )
            changed += p.slot_word( w / 3, 0 ) != t->slot_word( w / 3, 0 ) || p.slot_word( w / 3, 1 ) != t->slot_word( w / 3, 1 ) || p.slot_word( w / 3, 2 ) != t->slot_word( w / 3, 2 );
        if ( changed ) printf( "%s: template, %d slots rebound\n", argv[i], changed );
    }
    printf( "%d of %d vertex shaders have a template (%d unchanged), %zu templates\n", found, total, exact, templates.size() );
}
