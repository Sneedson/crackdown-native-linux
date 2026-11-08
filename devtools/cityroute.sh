#!/bin/bash
# Headless drive from the Agency garage (The Keep) through the tunnel into the city. The save's incoming
# dossier update (opens ~43 s, script clock) is closed first; then the walk to the car, out of the tunnel
# by Los Banditos at ~100 s, then on along the seawall road. Extra environment (CRACKDOWN_CAPTURE_AT,
# CRACKDOWN_DRAW_STATS, ...) passes through; BIN picks the program (default build/crackdown), TMO the
# seconds to run (default 170). Silent; plays from a copy of the game's data (tunnelroute.sh).
cd "$(dirname "$0")/.."
export CRACKDOWN_NATIVE_D3D=1
IN="8:A,12:A,16:A,20:A,24:A,28:A,46:B,48:B"
for t in $(seq 50 59); do IN="$IN,$t:LSUP"; done
ROUTE_INPUT="$IN,60:RSLEFT,61:LSUP,63:LSUP,65:LSUP,67:LSUP${ROUTE_PAD:+,$ROUTE_PAD}" ROUTE_KEYS="61:EnterVehicle:14,+1:S:3,+2:D:1.7,+5:W:80${ROUTE_EXTRA:+,$ROUTE_EXTRA}" \
    BIN=${BIN:-build/crackdown} TMO=${TMO:-170} exec devtools/tunnelroute.sh
