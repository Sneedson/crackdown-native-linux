// Checks that vertex shaders as the game loads them differ from their templates in the game files only
// in vertex fetch fields D3D patches when it binds a shader to a vertex declaration.
//   vs_template <dumped shader .bin (host-endian dwords)>... -- <game file>...
// For each dump: the window of a game file with the same words outside the vertex fetches (the binding
// fields masked, ucode::vfetch_binding_mask), and which fetch bits differ.
// Build: clang++ -std=c++20 -O2 -Iruntime devtools/vs_template.cpp runtime/ucode.cpp -o build/vs_template
#include "ucode.h"
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

static std::vector<uint8_t> read_file( const std::string &p )
{
    std::ifstream f( p, std::ios::binary );
    return { std::istreambuf_iterator<char>( f ), {} };
}

int main( int argc, char **argv )
{
    std::vector<std::string> dumps, files;
    bool sep = false;
    for ( int i = 1; i < argc; ++i )
        if ( !strcmp( argv[i], "--" ) ) sep = true;
        else ( sep ? files : dumps ).push_back( argv[i] );
    std::vector<std::vector<uint32_t>> data;
    for ( auto &f : files )
    {
        auto b = read_file( f );
        std::vector<uint32_t> w( b.size() / 4 );
        for ( size_t i = 0; i < w.size(); ++i ) w[i] = (uint32_t)b[i * 4] << 24 | b[i * 4 + 1] << 16 | b[i * 4 + 2] << 8 | b[i * 4 + 3];
        data.push_back( std::move( w ) );
    }
    int matched = 0, total = 0;
    uint32_t diff_bits[3] = {};
    for ( auto &d : dumps )
    {
        auto b = read_file( d );
        ucode::Program p;
        p.type = 0;
        p.words.resize( b.size() / 4 );
        memcpy( p.words.data(), b.data(), b.size() & ~3u );
        p.parse();
        std::vector<uint32_t> mask = ucode::binding_mask( p );  // per word: bits that are binding fields
        size_t n = p.words.size();
        ++total;
        bool found = false;
        for ( size_t fi = 0; fi < data.size() && !found; ++fi )
        {
            auto &w = data[fi];
            for ( size_t i = 0; i + n <= w.size() && !found; ++i )
            {
                if ( ( w[i + n - 1] & ~mask[n - 1] ) != ( p.words[n - 1] & ~mask[n - 1] ) ) continue;
                bool same = true;
                for ( size_t j = 0; j < n && same; ++j ) same = ( w[i + j] & ~mask[j] ) == ( p.words[j] & ~mask[j] );
                if ( !same ) continue;
                found = true;
                for ( size_t j = 0; j < n; ++j )
                    if ( mask[j] ) diff_bits[j % 3] |= w[i + j] ^ p.words[j];
                printf( "%s: template in %s at %#zx\n", d.c_str(), files[fi].c_str(), i * 4 );
            }
        }
        if ( found ) ++matched;
        else printf( "%s: NO TEMPLATE\n", d.c_str() );
    }
    printf( "%d of %d vertex shaders have a template; fetch bits that differed: %08X %08X %08X\n", matched, total, diff_bits[0], diff_bits[1], diff_bits[2] );
}
