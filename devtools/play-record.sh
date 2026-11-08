#!/bin/bash
# Runs the game like run.sh while recording the screen (cursor included) to build/videos/<time>.mp4,
# with mouse logging and wall-clock log stamps so the video lines up with build/play.log. Recording stops
# when the game exits. Uses gpu-screen-recorder through the KDE portal; the session token is shared with
# the Urban Chaos recorder so no prompt appears. Frames: ffmpeg -ss T -i video -vf fps=4,scale=960:-2 out/f%04d.jpg
cd "$(dirname "$0")/.."
mkdir -p build/videos
out=build/videos/$(date +%Y%m%d-%H%M%S).mp4
gpu-screen-recorder -w portal -restore-portal-session yes \
    -portal-session-token-filepath ~/UrbanChaos-BurnerEdition/profiling/portal-token \
    -f 30 -fm cfr -k h264 -q high -cursor yes -c mp4 -o "$out" > build/videos/recorder.log 2>&1 &
rec=$!
CRACKDOWN_MOUSE_LOG=1 CRACKDOWN_LOG_TIME=1 ./build/crackdown game_tu6/xex/default.xex 2> build/play.log
kill -INT $rec; wait $rec
echo "$out"
