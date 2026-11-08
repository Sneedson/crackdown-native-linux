#!/bin/bash
# Runs the native Crackdown build in a window (log: build/play.log). Click the window to capture the
# mouse, Left Alt releases it.
# In game, keys and mouse buttons are the game's own actions (runtime/keyboard.cpp): WASD move (in cars
# W throttle, S brake / reverse, A / D steer), mouse look (CRACKDOWN_MOUSE_SENS=1.5, CRACKDOWN_MOUSE_INVERT=1),
# left button fire, right button lock on (hold), middle rear view, wheel swap weapon (zoomed in: zoom;
# in cars: radio track), Space jump /
# handbrake / respawn, C or Ctrl crouch, F melee / pick up / drop, E enter / exit vehicle, R reload,
# G grenade, Q next equipment, Z zoom, H horn / siren, N mute radio, - / = radio volume, Esc pause,
# Tab or M map. F12 captures a frame (build/frame-N.ppm), F11 starts / stops a video (build/rec-N.mkv).
# Menus: mouse pointer (click, wheel, right click back), arrows, Enter / Space select, Esc back.
# Enter, Space, Esc or a click skips each start-up logo movie and screen.
# The log lists shaders still compiled while playing and renderer timing every 150 frames.
cd "$(dirname "$0")"
# The previous session's log is kept as build/play.prev.log.
[ -f build/play.log ] && mv build/play.log build/play.prev.log
CRACKDOWN_SHADER_LOG=1 CRACKDOWN_DRAW_STATS=150 CRACKDOWN_GPU_TIMING=1 CRACKDOWN_AUDIO_GAPS=1 CRACKDOWN_XMA_STATS=1 CRACKDOWN_VOICE_LOG=1 CRACKDOWN_STREAM_LOG=1 CRACKDOWN_KEY_LOG=1 exec ./build/crackdown game_tu6/xex/default.xex 2> build/play.log
