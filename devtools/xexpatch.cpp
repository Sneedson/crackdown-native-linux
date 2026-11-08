// Applies a title update's delta patch (default.xexp) to the disc's default.xex, with XenonUtils'
// patcher:  xexpatch <default.xex> <default.xexp> <patched default.xex>
// Build: clang++ -std=c++20 -I$KIT/XenonUtils devtools/xexpatch.cpp $KIT/build-linux/XenonUtils/libXenonUtils.a \
//        $KIT/build-linux/thirdparty/fmt/libfmt.a -o build/xexpatch   (KIT = recompiler/)
#include <xex_patcher.h>
#include <cstdio>

int main( int argc, char **argv )
{
    if ( argc != 4 ) { fprintf( stderr, "usage: %s <default.xex> <default.xexp> <out.xex>\n", argv[0] ); return 2; }
    auto r = XexPatcher::apply( argv[1], argv[2], argv[3] );
    if ( r != XexPatcher::Result::Success ) fprintf( stderr, "patching failed (%d)\n", (int)r );
    return (int)r;
}
