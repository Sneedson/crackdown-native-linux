#!/bin/bash
# Headless drive from the Agency garage into the tunnel under the sea (the water sheet shows ~10 s after
# the drive starts). Extra environment (CRACKDOWN_CAPTURE_AT, CRACKDOWN_RECORD_FROM, ...) passes through;
# BIN picks the program (default build/crackdown). Silent.
cd "$(dirname "$0")/.."
# A copy of the game's data folder (save, DLC, title update) and its own settings: a route never reads
# or writes the player's. The copy (build/testdata/seed, made from the player's on first use) starts
# each run afresh, so every run loads the same save.
D=$PWD/build/testdata
[ -d "$D/seed" ] || { mkdir -p "$D/seed" && cp -r --reflink=auto "${XDG_DATA_HOME:-$HOME/.local/share}/crackdown-recomp" "$D/seed/"; }
rm -rf "$D/run" && mkdir -p "$D/run" "$D/config" && cp -r --reflink=auto "$D/seed/crackdown-recomp" "$D/run/"
export XDG_DATA_HOME=$D/run XDG_CONFIG_HOME=$D/config
export CRACKDOWN_SCRIPT_FRAMES=1 CRACKDOWN_HEADLESS=1 CRACKDOWN_NO_AUDIO=1 CRACKDOWN_SKIP_SPLASH=1 CRACKDOWN_SKIP_TITLE=1
export CRACKDOWN_INPUT_SCRIPT="${ROUTE_INPUT:-8:A,12:A,16:A,20:A,24:A,28:A,31:LSUP,32:LSUP,33:LSUP,34:LSUP,35:LSUP,36:LSUP,37:LSUP,38:LSUP,39:LSUP,40:LSUP,41:RSLEFT,42:LSUP,44:LSUP,46:LSUP,48:LSUP}"
export CRACKDOWN_KEY_SCRIPT="${ROUTE_KEYS:-42:EnterVehicle:14,+1:S:3,+2:D:2.8,+5:W:30}"
exec timeout "${TMO:-100}" "./${BIN:-build/crackdown}" game_tu6/xex/default.xex
