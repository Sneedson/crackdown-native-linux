#!/bin/bash
# Starts the launcher (install, build, profiles, play), building it first if needed (Qt 6 Widgets, CMake,
# Ninja). Everything else is done from its pages.
cd "$(dirname "$0")"
if [ ! -x build/launcher/crackdown-launcher ] || [ launcher/main.cpp -nt build/launcher/crackdown-launcher ]; then
    echo "Building the launcher..."
    cmake -S launcher -B build/launcher -G Ninja -DCMAKE_BUILD_TYPE=Release > /dev/null && ninja -C build/launcher > /dev/null ||
        { echo "The launcher didn't build: it needs Qt 6 (Widgets), CMake and Ninja."; exit 1; }
fi
exec build/launcher/crackdown-launcher "$@"
