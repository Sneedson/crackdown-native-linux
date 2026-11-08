// Disassembles dumped Xenos shader microcode: xenos_dis build/shaders/vs_*.bin
#include "ucode.h"

#include <cstdio>
#include <cstring>

int main( int argc, char **argv )
{
    for ( int i = 1; i < argc; ++i )
    {
        FILE *f = fopen( argv[i], "rb" );
        if ( !f ) continue;
        ucode::Program p;
        uint32_t w;
        while ( fread( &w, 4, 1, f ) == 1 ) p.words.push_back( w );
        fclose( f );
        p.type = strstr( argv[i], "ps_" ) ? 1 : 0;
        p.parse();
        printf( "== %s (%zu dwords)\n%s\n", argv[i], p.words.size(), ucode::disassemble( p ).c_str() );
    }
}
