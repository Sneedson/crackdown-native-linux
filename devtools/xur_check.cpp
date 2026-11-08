// Reads every scene in an XUI archive and builds each again unchanged: they must parse to the end of
// their objects and come out the same (files with the loader's hints, header flag 1, from their sections on).
//   xur_check <file.xzp>
// Build: clang++ -std=c++20 -O2 -Iruntime devtools/xur_check.cpp runtime/xur.cpp -o build/xur_check
#include "xur.h"
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>

int main( int argc, char **argv )
{
    std::ifstream in( argv[1], std::ios::binary );
    std::vector<uint8_t> a{ std::istreambuf_iterator<char>( in ), {} };
    std::vector<XzpEntry> entries;
    uint32_t data_start;
    if ( !xzp_list( a, entries, data_start ) ) { printf( "not an archive\n" ); return 1; }
    int ok = 0, bad = 0, xur = 0;
    for ( auto &e : entries )
    {
        std::string name( e.name.begin(), e.name.end() );
        if ( name.size() < 4 || name.compare( name.size() - 4, 4, ".xur" ) ) continue;
        ++xur;
        XurFile f;
        if ( !f.parse( &a[data_start + e.offset], e.size ) ) { printf( "PARSE %s: %s\n", name.c_str(), f.error.c_str() ); ++bad; continue; }
        XurEdit edit( f );
        std::vector<uint8_t> b = edit.build();
        uint32_t flags = (uint32_t)f.bytes[8] << 24 | f.bytes[9] << 16 | f.bytes[10] << 8 | f.bytes[11];
        uint32_t skip_a = 0x14 + ( flags & 1 ? 40 : 0 ) + (uint32_t)f.sections.size() * 12, skip_b = 0x14 + (uint32_t)f.sections.size() * 12;
        bool same = f.bytes.size() - skip_a == b.size() - skip_b && !memcmp( &f.bytes[skip_a], &b[skip_b], b.size() - skip_b ) &&
                    ( flags & 1 || !memcmp( f.bytes.data(), b.data(), skip_b ) );
        if ( same ) ++ok;
        else { printf( "DIFFERS %s (%zu vs %zu bytes)\n", name.c_str(), f.bytes.size(), b.size() ); ++bad; }
    }
    std::vector<uint8_t> again = xzp_build( a, {} );
    printf( "%d scenes: %d the same, %d not; archive rebuilt %s\n", xur, ok, bad, again == a ? "the same" : "DIFFERENT" );
}
