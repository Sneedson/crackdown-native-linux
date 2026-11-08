#!/bin/bash
# Builds build/crackdown-trace: the current runtime objects (from the last build.sh) plus the D3D call trace.
# devtools/d3d_trace_build.sh emit: build/crackdown-emit with the table of commands per D3D function instead.
set -e
cd "$(dirname "$0")/.."
K=${KIT:-$PWD/recompiler}
mkdir -p build/d3dtrace
if [ "$1" = emit ]; then devtools/d3d_emit_gen.sh; SRC=d3d_emit; OUT=build/crackdown-emit; else devtools/d3d_trace_gen.sh; SRC=d3d_trace; OUT=build/crackdown-trace; fi
CXX="clang++ -std=c++20 -O2 -march=x86-64-v3 -fno-strict-aliasing -g"
$CXX -Wno-everything -Iruntime -Igame_tu6/ppc -c build/d3dtrace/$SRC.cpp -o build/d3dtrace/$SRC.o
$CXX -o $OUT build/runtime/*.o build/d3dtrace/$SRC.o build/obj_tu6/*.o \
    "$K/build-linux/XenonUtils/libXenonUtils.a" "$K/build-linux/thirdparty/disasm/libdisasm.a" \
    "$K/build-linux/thirdparty/fmt/libfmt.a" "$K/build-linux/thirdparty/xxHash/cmake_unofficial/libxxhash.a" -lSDL3 -lvulkan -lshaderc_shared -lavcodec -lavutil -lpthread -lz
echo built $OUT
