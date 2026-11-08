// Walks a run of Xenos shader programs stored back to back (a shader package's GPU data section):
// each program's length follows from its control flow (the highest instruction slot an exec clause
// uses). Prints "offset dwords" per program until the data stops parsing as a program.
//   ucode_walk <file> <start offset hex> [end offset hex]
// Build: clang++ -std=c++20 -O2 -Iruntime devtools/ucode_walk.cpp runtime/ucode.cpp -o build/ucode_walk
#include "ucode.h"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <vector>

// Program length in dwords from its control flow; 0 if the words don't look like a program.
uint32_t program_dwords( const std::vector<uint32_t> &w, size_t at, size_t end )
{
    ucode::Program p;
    p.words.assign( w.begin() + at, w.begin() + std::min( end, at + 0x4000 ) );
    p.parse();
    if ( p.cf.empty() || !p.cf.back().is_end() ) return 0;
    uint32_t cf_dwords = ( ( p.cf.size() + 1 ) / 2 ) * 3, slots_end = 0;
    for ( auto &c : p.cf )
        if ( c.is_exec() && c.count() ) slots_end = std::max( slots_end, c.address() + c.count() );
    if ( slots_end * 3 < cf_dwords ) return cf_dwords;  // a program with no instructions after its control flow
    return slots_end * 3;
}

int main( int argc, char **argv )
{
    std::ifstream f( argv[1], std::ios::binary );
    std::vector<uint8_t> b{ std::istreambuf_iterator<char>( f ), {} };
    std::vector<uint32_t> w( b.size() / 4 );
    for ( size_t i = 0; i < w.size(); ++i ) w[i] = (uint32_t)b[i * 4] << 24 | b[i * 4 + 1] << 16 | b[i * 4 + 2] << 8 | b[i * 4 + 3];
    size_t at = strtoul( argv[2], nullptr, 16 ) / 4, end = argc > 3 ? strtoul( argv[3], nullptr, 16 ) / 4 : w.size();
    while ( at < end )
    {
        uint32_t n = program_dwords( w, at, end );
        if ( !n ) { printf( "stop at %#zx\n", at * 4 ); break; }
        printf( "%#zx %u\n", at * 4, n );
        at += n;
    }
}
