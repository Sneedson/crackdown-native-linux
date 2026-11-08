#!/bin/bash
# A headless run on the test copy of the game's data and settings (build/testdata, as tunnelroute.sh
# makes it): the player's save and settings are never read or written. Silent; the start-up movies are
# skipped unless SKIP=0. IN: the input script (CRACKDOWN_INPUT_SCRIPT, frame clock), TMO: seconds to run
# (default 30), BIN: the program (default build/crackdown). Other CRACKDOWN_* settings pass through
# (CRACKDOWN_CAPTURE_AT, CRACKDOWN_AUDIO_DUMP, ...).
cd "$(dirname "$0")/.."
D=$PWD/build/testdata
SEED=${SEED:-seed}  # which saved copy (build/testdata/<SEED>): seed (the garage), seed-keep (The Keep supply point)
[ -d "$D/$SEED" ] || { echo "no $D/$SEED: run devtools/tunnelroute.sh once first" >&2; exit 1; }
rm -rf "$D/run" && mkdir -p "$D/run" "$D/config" && cp -r --reflink=auto "$D/$SEED/crackdown-recomp" "$D/run/"
export XDG_DATA_HOME=$D/run XDG_CONFIG_HOME=$D/config
export CRACKDOWN_SCRIPT_FRAMES=1 CRACKDOWN_HEADLESS=1 CRACKDOWN_NO_AUDIO=1 CRACKDOWN_SKIP_SPLASH=${SKIP:-1} CRACKDOWN_SKIP_TITLE=${SKIPT:-1} CRACKDOWN_NATIVE_D3D=1
export CRACKDOWN_INPUT_SCRIPT="${IN:-}"
exec nice -n 19 timeout -k 5 "${TMO:-30}" ${WRAP:-} "./${BIN:-build/crackdown}" game_tu6/xex/default.xex
