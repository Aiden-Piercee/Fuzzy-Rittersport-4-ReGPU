# Fuzzy Rittersport 4

A native PC port of the Xbox 360 game. The game's code is recompiled ahead of time with [XenonRecomp](https://github.com/hedge-dev/XenonRecomp) and runs on a Direct3D 12 renderer, [ReGPU](https://github.com/zer0ducksgiven/ReGPU), which implements the Xbox 360 GPU.

You need your own copy of the game, and it must be the **Game of the Year (GOTY) edition**. No game files are included, and no separate title update is needed.

> **Status: early and experimental.** Many of the graphics features (anti-aliasing, upscalers, enhanced lighting, wide aspect ratios) are experimental. An unlocked frame rate is planned for a later build.

## Features
- Native Direct3D 12 renderer, windowed or borderless fullscreen, adjustable resolution.
- Launcher Graphics page: MSAA (off, 2x, 4x), SMAA, 16x anisotropic filtering, FSR upscaling with sharpening, enhanced lighting, aspect ratio, frame cap and VSync.
- Settings and DLC management from the launcher.
- Bugs and glitches, included at no extra charge. They may still be in there; please report them (below).
- Shader cache: the first start compiles shaders (one to four minutes); later starts load them from the cache.

Bugs, glitches and other issues may still exist. Please report them as GitHub issues, with your GPU, driver version, what you were doing, and the log if you have one.

## Requirements
- Windows 10 or 11, x64, with a Direct3D 12 GPU.
- Visual Studio 2022 Build Tools, LLVM (clang-cl), CMake, Ninja.
- Python 3.11 or newer (`py -3`) and Git Bash (`sh`).
- An FFmpeg Windows x64 LGPL *shared* build, extracted so that `thirdparty/ffmpeg/include`, `lib` and `bin` exist. It decodes the game's XMA audio.
- The AMD FidelityFX SDK in `thirdparty/fidelityfx`.
- Disc 1 of the Game of the Year edition, extracted to a folder containing `default.xex` and both facade XEX files (`tools/xiso.py extract` unpacks an ISO).

## Building
1. Clone and link the game files:

   ```
   git clone --recurse-submodules https://github.com/Aiden-Piercee/Fuzzy-Rittersport-4-ReGPU
   cd Fuzzy-Rittersport-4-ReGPU
   mklink /J game <extracted disc folder>
   ```

2. Patch XenonRecomp and build the tools:

   ```
   git -C thirdparty/XenonRecomp apply ../../patches/xenonrecomp-fm2.patch
   git -C thirdparty/XenonRecomp apply ../../patches/xenonrecomp-modules.patch
   git -C thirdparty/XenonRecomp apply ../../patches/xenonrecomp-fm4.patch
   git -C thirdparty/XenonRecomp apply ../../patches/xenonrecomp-fm4-instructions.patch
   git -C thirdparty/XenonRecomp apply ../../patches/xenonrecomp-switch-u32.patch
   tools\vcenv.cmd cmake -S tools/xexdump -B build/tools -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl
   ```

3. Recompile the game (once per game copy). The generated code goes to `out/`:

   ```
   sh tools/recomp-fm4.sh --prepare
   sh tools/recomp-fm4.sh
   ```

   Details: [docs/FM4-RECOMPILE.md](docs/FM4-RECOMPILE.md).

4. Build:

   ```
   tools\vcenv.cmd cmake -S . -B build/rel -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DCMAKE_LINKER=lld-link
   tools\vcenv.cmd cmake --build build/rel --target fm4 --parallel
   ```

   If the recompiled output is in a folder other than `out`, add `-DFM4_OUT_DIR=<folder>`. Use all your cores for the build.

## Running
```
build\rel\fm4.exe game
```
Or start the launcher (set `FM4_EXE` to the built `fm4.exe` and `FM4_GAME_DIR` to the game folder). Close the game before rebuilding, because Windows locks a running exe.

Keyboard: arrows = d-pad, Enter = A, Backspace = B, Space = X, Esc = Start, Tab = Back, Q/E = LB/RB, W/S = RT/LT, A/D = steer. A gamepad (XInput) works too.

## Disc 2 and DLC
Disc 2 holds four marketplace content packages (cars). They are STFS packages, and the game only accepts ones that identify themselves as content for this game.
1. Extract your own Disc 2 image (for example `py -3 tools/xiso.py extract "<disc 2>.iso" disc2`) and find the four package files, named `4d53091000000001` to `4d53091000000004`.
2. Close the game.
3. Open the launcher's DLC page and choose **Add DLC**. Select the four package files (other DLC packages work the same way).
4. Each package is unpacked into `Documents\fm4\dlc`. Start the game from the launcher; the installed cars appear in the car browser.

A package can be switched off in the launcher without deleting it; it moves to `Documents\fm4\dlc-disabled`. Already-unpacked packages can also be placed in a `dlc` folder next to the install, and the launcher copies any that are missing when the game starts.

## Troubleshooting switches
Environment variables, set before starting:

| Variable | Effect |
|---|---|
| `FM2_TRACE_AUDIO=1` | Once a second, log audio queue statistics |
| `FM2_SILENT=1` | Discard audio output |
| `FM2_NO_AUDIO_TOPUP=1` | Turn off the audio queue top-up (experimental) |
| `FM2_NO_XMA_BOOST=1` | Turn off the raised XMA decoder thread priority (experimental) |

## Credits and licence
- [ReGPU](https://github.com/zer0ducksgiven/ReGPU) by zer0ducksgiven, MIT licence, is the renderer this port builds on, with contributions from the shader precompile, boot screen and launcher work.
- [XenonRecomp](https://github.com/hedge-dev/XenonRecomp) performs the recompilation.
- FFmpeg is linked dynamically and stays under its own LGPL licence.

Fuzzy Rittersport 4 is a fan project. It is not affiliated with or endorsed by Microsoft or Turn 10 Studios.
