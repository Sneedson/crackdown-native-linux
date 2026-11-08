#!/bin/bash
# Stage-1 build: compiles runtime/*.cpp and links them with the generated objects.
# GEN (default game_tu6/ppc: the title update 6 build, devtools/regen_tu6.sh; never committed) holds the
# recompiled game, OBJ its objects (default build/obj_tu6), OUT the program (default build/crackdown).
# The disc version (tag disc-v0) used GEN=generated OBJ=build/obj.
# RT_FLAGS=-DRT_CHECK_NONVOLATILE adds a debug check (touch runtime/ppc_context.h to rebuild all).
set -e
cd "$(dirname "$0")"
# The recompiler (recompiler/, XenonRecomp; KIT for another).
K=${KIT:-$PWD/recompiler}
CXX="clang++ -std=c++20 -O2 -march=x86-64-v3 -fno-strict-aliasing -g ${RT_FLAGS:-}"
GEN=${GEN:-game_tu6/ppc}; OBJ=${OBJ:-build/obj_tu6}; OUT=${OUT:-build/crackdown}
mkdir -p "$OBJ" build/runtime
for f in "$GEN"/*.cpp; do
    o=$OBJ/$(basename "${f%.cpp}").o
    [ "$o" -nt "$f" ] && [ "$o" -nt runtime/ppc_context.h ] || echo "$f"
done | xargs -r -P "$(nproc)" -I{} sh -c 'f={}; $0 -Wno-everything -Iruntime -I$1 -c $f -o $2/$(basename ${f%.cpp}).o' "$CXX" "$GEN" "$OBJ"
# NVIDIA's DLSS SDK (github.com/NVIDIA/DLSS) if it is there: $DLSS, else ~/src/DLSS.
DLSS=${DLSS:-$HOME/src/DLSS}
DLSS_CFLAGS= DLSS_LIBS=
if [ -f "$DLSS/lib/Linux_x86_64/libnvsdk_ngx.a" ]; then
    DLSS_CFLAGS="-DHAVE_DLSS -I$DLSS/include" DLSS_LIBS="$DLSS/lib/Linux_x86_64/libnvsdk_ngx.a -ldl"
    cp -u "$DLSS"/lib/Linux_x86_64/rel/libnvidia-ngx-dlss.so.* "$(dirname "$OUT")/"
fi
# AMD's FidelityFX SDK v1.1.4 (github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK) if it is there: $FFX,
# else ~/src/ref/FidelityFX-SDK. Its FSR 3 upscaler host code and Vulkan backend are built as they are
# (runtime/fsr3/ffx_compat.h forced in front), its shaders by devtools/build_fsr3_shaders.sh.
FFX=${FFX:-$HOME/src/ref/FidelityFX-SDK}
FSR3_CFLAGS= FSR3_OBJS=
if [ -f "$FFX/sdk/src/components/fsr3upscaler/ffx_fsr3upscaler.cpp" ]; then
    S=$FFX/sdk
    FFX_INC="-include runtime/fsr3/ffx_compat.h -include FidelityFX/host/ffx_util.h -DFFX_FSR3UPSCALER -DFFX_GCC -I$S/include -I$S/include/FidelityFX/host -I$S/include/FidelityFX/gpu -I$S/src -I$S/src/components -I$S/src/backends/shared -I$S/src/shared -I$S/src/backends/vk -Ibuild/fsr3"
    [ build/fsr3/fsr3_spirv.h -nt devtools/build_fsr3_shaders.sh ] || devtools/build_fsr3_shaders.sh build/fsr3/fsr3_spirv.h
    mkdir -p build/fsr3/obj
    # The Vulkan backend lays its tables out in one scratch buffer aligned to 4 bytes only; with Linux's
    # 4-byte wchar_t one lands off 16 and its aligned SSE stores fault. Built from a copy laid out at
    # 64 bytes, with room for that (the SDK itself is left as it is).
    if [ ! build/fsr3/ffx_vk.cpp -nt "$S/src/backends/vk/ffx_vk.cpp" ] || [ ! build/fsr3/ffx_vk.cpp -nt build.sh ]; then
        sed -e '/ArraySize *= *FFX_ALIGN_UP/s/sizeof(uint32_t))/64)/' \
            -e 's|uint8_t\* pMem = (uint8_t\*)((BackendContext_VK\*)(backendContext + 1));|uint8_t* pMem = (uint8_t*)FFX_ALIGN_UP((uintptr_t)(backendContext + 1), 64);|' \
            -e 's|return FFX_ALIGN_UP(sizeof(BackendContext_VK) + extensionPropArraySize|return 64 + FFX_ALIGN_UP(sizeof(BackendContext_VK) + extensionPropArraySize|' \
            "$S/src/backends/vk/ffx_vk.cpp" > build/fsr3/ffx_vk.cpp
    fi
    for f in "$S/src/components/fsr3upscaler/ffx_fsr3upscaler.cpp" build/fsr3/ffx_vk.cpp "$S/src/backends/shared/ffx_shader_blobs.cpp" \
             "$S/src/shared/ffx_assert.cpp" "$S/src/shared/ffx_message.cpp" "$S/src/shared/ffx_breadcrumbs_list.cpp" "$S/src/shared/ffx_object_management.cpp" runtime/fsr3/fsr3_blobs.cpp; do
        o=build/fsr3/obj/$(basename "${f%.cpp}").o
        [ "$o" -nt "$f" ] && [ "$o" -nt runtime/fsr3/ffx_compat.h ] && [ "$o" -nt build/fsr3/fsr3_spirv.h ] || clang++ -std=c++17 -O${FFX_OPT:-2} -g -fno-strict-aliasing -fmax-type-align=8 -w $FFX_INC -c "$f" -o "$o"
        FSR3_OBJS="$FSR3_OBJS $o"
    done
    FSR3_CFLAGS="-DHAVE_FSR3 -fmax-type-align=8 $FFX_INC"
fi
$CXX -w $FSR3_CFLAGS -Iruntime -I"$GEN" -c runtime/fsr3/fsr3.cpp -o build/runtime/fsr3_glue.o
for f in runtime/*.cpp; do
    $CXX -Wall -Wno-unused-function $([ "$f" = runtime/dlss.cpp ] && echo $DLSS_CFLAGS) -Iruntime -I"$GEN" -I"$K/XenonUtils" -I"$K/thirdparty/xxHash" $(pkg-config --cflags freetype2) -c "$f" -o "build/runtime/$(basename "${f%.cpp}").o"
done
$CXX -o "$OUT" build/runtime/*.o "$OBJ"/*.o \
    $FSR3_OBJS "$K/build-linux/XenonUtils/libXenonUtils.a" "$K/build-linux/thirdparty/disasm/libdisasm.a" \
    "$K/build-linux/thirdparty/fmt/libfmt.a" "$K/build-linux/thirdparty/xxHash/cmake_unofficial/libxxhash.a" -lSDL3 -lfreetype -lvulkan -lshaderc_shared -lavcodec -lavformat -lavutil -lpthread -lz $DLSS_LIBS
echo "built $OUT"
