#!/bin/bash
# Recompiles the title-update build (game_tu6/: xex/default.xex = the disc's default.xex patched with
# TU6's default.xexp by devtools/xexpatch.cpp, config.toml, switch.toml, image.bin) into game_tu6/ppc,
# then inlines the shared return tails (inline_error_tails.py). The runtime's own ppc_context.h is
# used in place of the recompiler's. Then: GEN=game_tu6/ppc OBJ=build/obj_tu6 ./build.sh
set -e
cd "$(dirname "$0")/.."
# The recompiler (recompiler/, XenonRecomp; KIT for another).
K=${KIT:-$PWD/recompiler}
mkdir -p game_tu6/ppc  # (XenonRecomp crashes without its output folder)
( cd game_tu6 && "$K/build-linux/XenonRecomp/XenonRecomp" config.toml "$K/XenonUtils/ppc_context.h" > recomp.log 2>&1 )
python3 -I devtools/inline_error_tails.py game_tu6/ppc game_tu6/image.bin
rm -f game_tu6/ppc/ppc_context.h
grep -c "ERROR" game_tu6/ppc/*.cpp | awk -F: '{ n += $2 } END { print n " ERROR comments left" }'
