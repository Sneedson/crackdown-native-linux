// Translates dumped Xenos shaders to GLSL and writes <input>.vert / .frag next to them:
//   xenos_xlate build/shaders/*.bin   (then glslangValidator checks them)
#include "translate.h"
#include "prelude.glsl.h"

#include <cstdio>
#include <cstring>
#include <string>

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
        std::string err, body = xlate::translate( p, &err );
        if ( body.empty() ) { printf( "%s: %s\n", argv[i], err.c_str() ); continue; }
        std::string src = std::string( xlate::kPrelude ) + body + ( p.type ? xlate::kPixelMain : xlate::kVertexMain );
        std::string out = std::string( argv[i] ) + ( p.type ? ".frag" : ".vert" );
        FILE *o = fopen( out.c_str(), "wb" );
        fwrite( src.data(), 1, src.size(), o );
        fclose( o );
    }
}
