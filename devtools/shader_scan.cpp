// Finds Xenos shader programs in game files: every dword offset where a plausible program
// starts (ucode::plausible_program). Prints "file offset dwords" per program.
//   shader_scan <file>...
// Build: clang++ -std=c++20 -O2 -Iruntime devtools/shader_scan.cpp runtime/ucode.cpp -o build/shader_scan
#include "ucode.h"
#include <cstdio>
#include <fstream>
#include <iterator>
#include <vector>

int main( int argc, char **argv )
{
    for ( int a = 1; a < argc; ++a )
    {
        std::ifstream f( argv[a], std::ios::binary );
        std::vector<uint8_t> b{ std::istreambuf_iterator<char>( f ), {} };
        std::vector<uint32_t> w( b.size() / 4 );
        for ( size_t i = 0; i < w.size(); ++i ) w[i] = (uint32_t)b[i * 4] << 24 | b[i * 4 + 1] << 16 | b[i * 4 + 2] << 8 | b[i * 4 + 3];
        for ( size_t i = 0; i < w.size(); ++i )
        {
            uint32_t n;
            if ( ucode::plausible_program( &w[i], w.size() - i, n ) ) printf( "%s %#zx %u\n", argv[a], i * 4, n );
        }
    }
}
