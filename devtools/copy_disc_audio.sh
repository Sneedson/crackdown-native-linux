#!/bin/bash
# Recreates build/disc: a copy of the extracted disc's Audio folder beside the program, which files.cpp
# opens instead of the disc's (the disc folder can be on a spinning disk; sound banks are read as sounds
# play, and those opened from here are kept in memory whole). Nothing else belongs in it: files only, as
# redirecting folders breaks the game's relative opens.
#   devtools/copy_disc_audio.sh [extracted disc folder]   (default $CRACKDOWN_DISC)
set -e
cd "$(dirname "$0")/.."
SRC=${1:-$CRACKDOWN_DISC}
[ -d "$SRC/Audio" ] || { echo "no Audio folder in '$SRC': give the extracted disc folder" >&2; exit 1; }
mkdir -p build/disc.new
cp -r --reflink=auto "$SRC/Audio" build/disc.new/
diff -r "$SRC/Audio" build/disc.new/Audio
rm -rf build/disc
mv build/disc.new build/disc
echo "build/disc: $(find build/disc -type f | wc -l) files, $(du -sh build/disc | cut -f1)"
