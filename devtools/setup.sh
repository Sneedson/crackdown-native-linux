#!/bin/bash
# Installs and builds the port from the player's own game files (the launcher runs it; it also runs by
# hand). The game's data stays where it is (a folder dumped from the Crackdown disc); the title update
# and downloadable content packages are unpacked into the data folder ($XDG_DATA_HOME/crackdown-recomp),
# where the game finds them. The build recompiles the game's code from the disc's Default.xex patched
# with title update 6 (the port is made for that version) and builds the program, build/crackdown.
#
#   setup.sh status                  what is installed (key=value lines)
#   setup.sh check                   the tools and libraries the build needs
#   setup.sh disc <folder>           use a dumped disc folder (its Default.xex must be the original's)
#   setup.sh update <package>        install title update 6 (its package, or a zip holding it)
#   setup.sh dlc <package>           install downloadable content (a package, or a zip of packages)
#   setup.sh build                   recompile and build (needs the disc and the title update)
#
# Progress lines for the launcher: "STEP <n>/<total> <text>", "PROGRESS <percent>", "ERROR <text>".
# KIT: the recompiler (default recompiler/, XenonRecomp).
set -o pipefail
cd "$(dirname "$0")/.."
ROOT=$PWD
DATA=${XDG_DATA_HOME:-$HOME/.local/share}/crackdown-recomp
STATE=$DATA/install.ini
KIT=${KIT:-$ROOT/recompiler}
KIT=$(realpath -m "$KIT")
# Versioned LLVM packages (clang20 on Arch, llvm-N on Debian) keep clang outside /usr/bin, on a PATH a
# shell's rc file sets, which a launcher started from the desktop doesn't read: the newest such folder.
if ! command -v clang++ > /dev/null; then
    for d in $(ls -d /usr/lib/llvm*/bin /usr/lib/llvm-*/bin /opt/llvm*/bin 2>/dev/null | sort -V -r); do
        [ -x "$d/clang++" ] && { export PATH="$d:$PATH"; break; }
    done
fi
export KIT

# The versions the port is made for (SHA-1): the disc's Default.xex, title update 6's Default.xexp, the
# two patched together, and the image XenonAnalyse makes of that.
DISC_XEX=2c7201eca20554a33aee9083690667ecdc486da5
TU6_XEXP=3dd185571215327de1fea3c2ebd9235a5231069a
TU6_XEX=92ec8a9325777a0db7c53008be2f28e49f97f666
TU6_IMAGE=cb1015dd30464fcf25fd181afb2593f8f7919cfb
TITLE_ID=4D5307DC

say() { echo "$*"; }
fail() { echo "ERROR $*"; exit 1; }
sha1() { sha1sum "$1" | cut -d' ' -f1; }
state_get() { [ -f "$STATE" ] && sed -n "s/^$1 = //p" "$STATE" | tail -1; }
state_set() {
    mkdir -p "$DATA"
    touch "$STATE"
    grep -v "^$1 = " "$STATE" > "$STATE.tmp"; echo "$1 = $2" >> "$STATE.tmp"; mv "$STATE.tmp" "$STATE"
}
# A zip's packages (title updates and content come zipped): unpacked into a fresh folder, printed one a
# line. Python's zipfile (refusing paths outside the folder), so no unzip is needed.
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
unzip_packages() {
    local out; out=$(mktemp -d -p "$TMP")
    python3 -I -c '
import sys, zipfile, os
z = zipfile.ZipFile(sys.argv[1]); dest = os.path.realpath(sys.argv[2])
for i in z.infolist():
    if i.is_dir(): continue
    t = os.path.realpath(os.path.join(dest, i.filename))
    if not t.startswith(dest + os.sep): continue
    os.makedirs(os.path.dirname(t), exist_ok=True)
    with z.open(i) as a, open(t, "wb") as b:
        while True:
            c = a.read(1 << 20)
            if not c: break
            b.write(c)
' "$1" "$out" || fail "couldn't read the zip $1"
    find "$out" -type f -size +1k -print0 | while IFS= read -r -d '' f; do
        case $(head -c 4 "$f") in LIVE | PIRS | "CON ") echo "$f" ;; esac
    done
}
is_zip() { [ "$(head -c 2 "$1")" = "PK" ]; }
disc_xex() { for n in Default.xex default.xex; do [ -f "$1/$n" ] && { echo "$1/$n"; return; }; done; }

cmd_status() {
    local disc; disc=$(state_get disc)
    echo "data = $DATA"
    if [ -n "$disc" ] && [ -n "$(disc_xex "$disc")" ]; then echo "disc = $disc"; else echo "disc ="; fi
    if [ -f "$DATA/update/Default.xexp" ] && [ "$(sha1 "$DATA/update/Default.xexp")" = "$TU6_XEXP" ]; then echo "update = 6"; else echo "update ="; fi
    local n=0
    for d in "$DATA"/content/00000002/*/; do [ -d "$d" ] && n=$((n + 1)) && echo "dlc = $(cat "${d%/}.name" 2>/dev/null || basename "$d")"; done
    echo "dlc_count = $n"
    if [ -x build/crackdown ]; then echo "built = $(state_get built)"; else echo "built ="; fi
    echo "kit = $KIT"
}

need_cmds=(clang++ cmake ninja python3 pkg-config sha1sum)
need_pkgs=(sdl3 freetype2 vulkan shaderc libavcodec libavformat libavutil zlib)
cmd_check() {
    local missing=()
    for c in "${need_cmds[@]}"; do command -v "$c" > /dev/null || missing+=("$c"); done
    for p in "${need_pkgs[@]}"; do pkg-config --exists "$p" 2>/dev/null || missing+=("$p (development files)"); done
    [ -f "$KIT/CMakeLists.txt" ] || missing+=("the recompiler at $KIT (set KIT)")
    command -v glslangValidator > /dev/null || say "note: glslangValidator missing: no FSR 3 (optional)"
    [ -d "${FFX:-$HOME/src/ref/FidelityFX-SDK}/sdk" ] || say "note: no FidelityFX SDK: no FSR 3 (optional)"
    [ -d "${DLSS:-$HOME/src/DLSS}/include" ] || say "note: no DLSS SDK: no DLSS (optional)"
    if [ ${#missing[@]} -gt 0 ]; then
        for m in "${missing[@]}"; do echo "missing = $m"; done
        fail "missing what the build needs: ${missing[*]}"
    fi
    say "check: everything the build needs is there"
}

cmd_disc() {
    local dir=$1
    [ -d "$dir" ] || fail "no folder $dir"
    dir=$(cd "$dir" && pwd)
    local xex; xex=$(disc_xex "$dir")
    [ -n "$xex" ] || fail "$dir has no Default.xex: choose the folder dumped from the Crackdown disc (its top level)"
    [ "$(sha1 "$xex")" = "$DISC_XEX" ] || fail "$xex isn't the version the port is made for (the original Crackdown disc's)"
    for f in global.dff fullcity.dff Audio XUIFrontEnd; do [ -e "$dir/$f" ] || fail "$dir has no $f: the dump looks incomplete"; done
    state_set disc "$dir"
    say "disc: $dir"
}

cmd_update() {
    local src=$1
    [ -f "$src" ] || fail "no file $src"
    if is_zip "$src"; then
        local pkgs; pkgs=$(unzip_packages "$src")
        [ -n "$pkgs" ] || fail "$src holds no title update package"
        [ "$(echo "$pkgs" | wc -l)" = 1 ] || fail "$src holds several packages: unzip it and choose the title update"
        src=$pkgs
    fi
    # Unpacked into a folder of its own first: the installed update is replaced only by a good one.
    local stage; stage=$(mktemp -d -p "$TMP")
    mkdir -p "$stage/crackdown-recomp/update"
    case $(head -c 4 "$src") in
    LIVE | PIRS | "CON ")
        local info; info=$(python3 -I devtools/stfs.py "$src" | head -1)
        echo "$info" | grep -q "type 000B0000" || fail "$src isn't a title update package ($info)"
        echo "$info" | grep -q "title $TITLE_ID" || fail "$src is for another game ($info)"
        XDG_DATA_HOME=$stage python3 -I devtools/stfs.py --install "$src" > /dev/null || fail "couldn't unpack $src"
        ;;
    *) fail "$src isn't a title update package (choose the package, or the zip holding it)" ;;
    esac
    local xexp; xexp=$(find "$stage/crackdown-recomp/update" -maxdepth 1 -iname default.xexp | head -1)
    [ -n "$xexp" ] && [ "$(sha1 "$xexp")" = "$TU6_XEXP" ] || fail "this isn't title update 6, the one the port is made for"
    [ "$(basename "$xexp")" = Default.xexp ] || mv "$xexp" "$stage/crackdown-recomp/update/Default.xexp"
    mkdir -p "$DATA"
    rm -rf "$DATA/update" && mv "$stage/crackdown-recomp/update" "$DATA/update" || fail "couldn't install into $DATA/update"
    say "update: title update 6 installed"
}

cmd_dlc() {
    local src=$1
    [ -f "$src" ] || fail "no file $src"
    if is_zip "$src"; then
        local pkgs n=0; pkgs=$(unzip_packages "$src")
        [ -n "$pkgs" ] || fail "$src holds no content package"
        while IFS= read -r p; do ( cmd_dlc "$p" ) || exit 1; n=$((n + 1)); done <<< "$pkgs"
        say "dlc: $n package(s) from $(basename "$src")"
        return
    fi
    local info; info=$(python3 -I devtools/stfs.py "$src" 2>/dev/null | head -1)
    echo "$info" | grep -q "title $TITLE_ID" || fail "$src isn't a Crackdown content package ($info)"
    echo "$info" | grep -q "type 00000002" || fail "$src isn't downloadable content ($info)"
    python3 -I devtools/stfs.py --install "$src" > /dev/null || fail "couldn't unpack $src"
    say "dlc: installed $(basename "$src")"
}

cmd_build() {
    local disc; disc=$(state_get disc)
    [ -n "$disc" ] && [ -n "$(disc_xex "$disc")" ] || fail "choose the disc folder first"
    [ -f "$DATA/update/Default.xexp" ] || fail "title update 6 is needed to build: the port is made for it"
    cmd_check > /dev/null || { cmd_check; exit 1; }
    local K=$KIT T=7
    mkdir -p build  # (a fresh clone has none yet)
    # 1. The recompiler.
    say "STEP 1/$T Building the recompiler"
    if [ ! -x "$K/build-linux/XenonRecomp/XenonRecomp" ] || [ ! -x "$K/build-linux/XenonAnalyse/XenonAnalyse" ]; then
        cmake -S "$K" -B "$K/build-linux" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ > build/setup-kit.log 2>&1 &&
            ninja -C "$K/build-linux" >> build/setup-kit.log 2>&1 || fail "the recompiler didn't build (build/setup-kit.log)"
    fi
    # 2. The disc's code with the title update.
    say "STEP 2/$T Applying title update 6"
    mkdir -p build game_tu6/xex
    if [ ! -x build/xexpatch ] || [ devtools/xexpatch.cpp -nt build/xexpatch ]; then
        clang++ -std=c++20 -O2 -I"$K/XenonUtils" devtools/xexpatch.cpp "$K/build-linux/XenonUtils/libXenonUtils.a" \
            "$K/build-linux/thirdparty/fmt/libfmt.a" -o build/xexpatch > build/setup-xexpatch.log 2>&1 || fail "xexpatch didn't build (build/setup-xexpatch.log)"
    fi
    if [ ! -f game_tu6/xex/default.xex ] || [ "$(sha1 game_tu6/xex/default.xex)" != "$TU6_XEX" ]; then
        ./build/xexpatch "$(disc_xex "$disc")" "$DATA/update/Default.xexp" game_tu6/xex/default.xex > /dev/null || fail "the title update didn't apply"
    fi
    [ "$(sha1 game_tu6/xex/default.xex)" = "$TU6_XEX" ] || fail "the patched game isn't the one the port is made for"
    # 3. Its image (XenonAnalyse; its own switch tables are not used: game_tu6/switch.toml is the port's).
    say "STEP 3/$T Analysing the game"
    if [ ! -f game_tu6/image.bin ] || [ "$(sha1 game_tu6/image.bin)" != "$TU6_IMAGE" ]; then
        "$K/build-linux/XenonAnalyse/XenonAnalyse" game_tu6/xex/default.xex build/analyse-switch.toml game_tu6/image.bin > build/setup-analyse.log 2>&1 ||
            fail "XenonAnalyse failed (build/setup-analyse.log)"
    fi
    [ "$(sha1 game_tu6/image.bin)" = "$TU6_IMAGE" ] || fail "the game's image isn't as expected"
    # 4. Recompiling.
    say "STEP 4/$T Recompiling the game's code (a few minutes)"
    if [ ! -f game_tu6/ppc/ppc_func_mapping.cpp ] || [ game_tu6/config.toml -nt game_tu6/ppc/ppc_func_mapping.cpp ] || [ game_tu6/switch.toml -nt game_tu6/ppc/ppc_func_mapping.cpp ]; then
        devtools/regen_tu6.sh > build/setup-recompile.log 2>&1 || fail "recompiling failed (build/setup-recompile.log)"
    fi
    # 5. Building: build.sh compiles each generated file (the long part), counted for the progress.
    say "STEP 5/$T Compiling (the long part: many minutes)"
    local total done=0
    total=$(ls game_tu6/ppc/*.cpp | wc -l)
    OUT=build/crackdown.new ./build.sh 2>&1 | tee build/setup-build.log | while IFS= read -r line; do
        case $line in game_tu6/ppc/*.cpp) done=$((done + 1)); echo "PROGRESS $((done * 100 / total))" ;; esac
    done
    [ -x build/crackdown.new ] && grep -q "^built build/crackdown.new" build/setup-build.log || fail "the build failed (build/setup-build.log)"
    mv build/crackdown.new build/crackdown
    # 6. The shader cache.
    say "STEP 6/$T Compiling the game's shaders"
    devtools/build_shader_aot.sh "$disc" > build/setup-shaders.log 2>&1 || fail "the shader cache failed (build/setup-shaders.log)"
    say "STEP 7/$T Done"
    state_set built "$(date '+%Y-%m-%d %H:%M')"
    say "build: done"
}

case $1 in
    status) cmd_status ;;
    check) cmd_check ;;
    disc) [ -n "$2" ] || fail "usage: setup.sh disc <folder>"; cmd_disc "$2" ;;
    update) [ -n "$2" ] || fail "usage: setup.sh update <package or Default.xexp>"; cmd_update "$2" ;;
    dlc) [ -n "$2" ] || fail "usage: setup.sh dlc <package>"; cmd_dlc "$2" ;;
    build) cmd_build ;;
    *) sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'; exit 1 ;;
esac
