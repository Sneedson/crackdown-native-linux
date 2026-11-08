# Crackdown — Native Linux Port

A native Linux port of **Crackdown** (Xbox 360, 2007): the game's own code, statically recompiled from
PowerPC to x86-64 and running on Linux with its own runtime — Vulkan rendering, native audio, keyboard
and mouse, controllers. No emulator.

**No game files are included.** You need your own copy of the game: the folder dumped from the
Crackdown disc, title update 6, and (optionally) the downloadable content. The installer recompiles the
game's code from those files on your machine.

## Features

- Runs natively at any resolution up to 4K, up to 60 FPS, with a field of view slider
- Anti-aliasing: MSAA 2x/4x/8x or FXAA; anisotropic filtering up to 16x
- Upscaling: AMD FSR 3 (any GPU), NVIDIA DLSS / DLAA (RTX), FSR 1
- Draw Distance: people and traffic kept alive farther away, world detail out to 4x the distance
- Keyboard and mouse throughout (rebindable), with mouse-driven menus; controllers work as on the console
- An Agency-emblem mouse pointer in the menus, made at start-up from your own game files
- Profiles: your stats, saves and achievements kept under a name you choose
- Title update 6 and the downloadable content (Free-For-All Pack, Four-Play Pack)
- All of it in the game's own options menu (Options → Display Settings → Recomp Options)

## To be added

- **Windows support**: the same port and launcher, built and run natively on Windows

## What you need

**Your game files**
- The folder dumped from the Crackdown disc (it holds `Default.xex`, `global.dff`, `Audio/`, ...)
- Title update 6: its package, or the zip holding it (required to build: the port is made for that version)
- Downloadable content packages (optional; can be added any time)

**A graphics card with Vulkan 1.3** — NVIDIA GTX 900 series or newer, AMD RX 400 series or newer (Mesa
RADV), Intel Gen 9 or newer (Mesa ANV), on a current driver.

**Build tools and libraries** — on Arch Linux:

```
sudo pacman -S git clang cmake ninja python pkgconf sdl3 freetype2 vulkan-icd-loader vulkan-headers shaderc ffmpeg zlib qt6-base
```

(Other distributions: the same packages under their names, with development headers.) `clang` may also be
a versioned package (`clang20` and so on): the installer finds it.

Optional:
- **FSR 3**: AMD's [FidelityFX SDK](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK) at tag
  `v1.1.4` in `~/src/ref/FidelityFX-SDK` (or set `FFX`), and `glslang` (`glslangValidator`)
- **DLSS**: NVIDIA's [DLSS SDK](https://github.com/NVIDIA/DLSS) in `~/src/DLSS` (or set `DLSS`), and an RTX card

## Install

```
git clone https://github.com/Sneedson/crackdown-native-linux.git
cd crackdown-native-linux
./launcher.sh
```

The launcher's **Install** page takes you through it:
1. **Game disc** — choose the dumped disc folder (it's read where it is, not copied)
2. **Title update 6** — add its package or zip (or add it later; the build waits for it)
3. **Downloadable content** — add packages or zips now, or any time later
4. **Build** — recompiles the game's code and builds the port (a few minutes)

Then pick or create a profile and press **Play**.

Everything the launcher does is also available from a terminal:

```
devtools/setup.sh disc <dumped disc folder>
devtools/setup.sh update <title update 6 package or zip>
devtools/setup.sh dlc <content package or zip>
devtools/setup.sh build
./run.sh
```

Your data (title update, content, profiles, saves) lives in `~/.local/share/crackdown-recomp`, settings
in `~/.config/crackdown-recomp`.

## Controls

Keyboard and mouse: WASD move (in cars W throttle, S brake/reverse, A/D steer), mouse look, left button
fire, right button lock on, Space jump / handbrake, C or Ctrl crouch, F melee / pick up, E enter/exit
vehicle, R reload, G grenade, Q next equipment, Z zoom, H horn, Tab or M map, Esc pause. All of it can be
rebound in Options → Keyboard Controls. F12 saves a screenshot, F11 records a video.

## How it works

The recompiler (`recompiler/`, [XenonRecomp](https://github.com/hedge-dev/XenonRecomp)) translates the game's PowerPC code (the
`default.xex` patched with title update 6) into C++, which is compiled together with this project's
runtime (`runtime/`): the Xbox 360 kernel and system functions the game calls, the Xenos GPU's work
translated to Vulkan, XMA audio, input, files, saves and the game's menus. The game's data is used as it
is.

## Credits

- **[XenonRecomp](https://github.com/hedge-dev/XenonRecomp)** by hedge-dev and contributors: the static
  recompiler (`recompiler/`), MIT
- **XenonRecomp-XEO3** by WitherOrNot, and **[xeo3-recomp-kit](https://github.com/fxceless/xeo3-recomp-kit)**
  by its contributors: the version of the recompiler included here, MIT
- The recompiler's libraries (`recompiler/thirdparty/`): fmt, toml++, xxHash, libmspack, tiny-AES-c,
  TinySHA1 and a PowerPC disassembler from GNU binutils, each under its own licence (in its folder)
- **AMD FidelityFX** (FSR 1 headers in `runtime/`; FSR 3 from the SDK at build time), MIT
- **NVIDIA DLSS** through NVIDIA's SDK at build time (not included)

## Licence

This project's own code is MIT (see [LICENSE](LICENSE)). Everything listed above keeps its own licence
(`recompiler/LICENSE.md`, `recompiler/LICENSE.xeo3-recomp-kit` and the files beside each library).
Crackdown and its files belong to their owners and are not part of this project.
