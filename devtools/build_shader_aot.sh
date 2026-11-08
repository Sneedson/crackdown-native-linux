#!/bin/bash
# Builds the shader tools and compiles every shader in the game files into the renderer's cache:
#   devtools/build_shader_aot.sh [disc folder]   (default: CRACKDOWN_DISC or the runtime's default)
set -e
cd "$(dirname "$0")/.."
INI=${XDG_DATA_HOME:-$HOME/.local/share}/crackdown-recomp/install.ini  # (devtools/setup.sh's choice)
D=${1:-${CRACKDOWN_DISC:-$( [ -f "$INI" ] && sed -n 's/^disc = //p' "$INI" | tail -1 )}}
[ -n "$D" ] || { echo "no disc folder: give it, or run devtools/setup.sh disc <folder>" >&2; exit 1; }
CXX="clang++ -std=c++20 -O2 -Iruntime"
$CXX devtools/shader_scan.cpp runtime/ucode.cpp -o build/shader_scan
$CXX devtools/shader_aot.cpp runtime/translate.cpp runtime/ucode.cpp -lshaderc_shared -lpthread -o build/shader_aot
$CXX devtools/vs_bind_check.cpp runtime/ucode.cpp -o build/vs_bind_check
# Every shader is in the shader packages or the executable (its own few). Vertex shaders the game
# dumped as bound (build/shaders, CRACKDOWN_DUMP_SHADERS=1) can be checked against the templates with
# build/vs_bind_check <cache>/crackdown-recomp/vs_templates.bin build/shaders/vs_*.bin.
# The downloadable content's packages (mounted from the content folder as save0: / save1: ...) add their own.
C=${CRACKDOWN_CONTENT:-${XDG_DATA_HOME:-$HOME/.local/share}/crackdown-recomp/content}
shopt -s nullglob
./build/shader_scan "$D"/vfx_shaders.spk "$D"/boundshaders.bsh "$D"/ScreenRenderer/*.bin "$D"/ScreenRenderer/*.dat game_tu6/image.bin \
    "$C"/*/*/*.spk "$C"/*/*/*.bsh > build/shader_scan.txt
./build/shader_aot build/shader_scan.txt
