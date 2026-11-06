# Notice: racing wheel support

The racing wheel logic in `src/input/wheel/` (which device is the wheel, the pedals, the shifter
and the handbrake; the H-pattern shifter, clutch and analog handbrake; the 900 and 270 degree
steering modes; the known-device tables) is ported from the racing wheel driver of
**Forza Horizon 1: ReImagined** (https://github.com/FH1-ReImagined/ForzaHorizon1-ReImagined), its
ReXGlue SDK patch series `patches/rexglue-sdk/0074`, `0099`, `0101`, `0127`, `0128`, `0129`,
`0131`, `0132`, `0133`, `0134` and `0135`. The force feedback (`src/input/wheel/wheel_ff.h`,
`src/kernel/imports_ff.cpp`) comes from `0101`, `0129` and `0131`. Those files carry the BSD 3-Clause License, reproduced below. The
DirectInput device layer (`wheel_system.cpp`), the launcher probe (`wheel_probe.cpp`) and the
launcher's Wheel stuff tab (`launcher/FM4Launcher/Wheel.cs`, `wwwroot/wheel.js`) are new code
for this project.

The `fm2` force feedback playback (`wheel_ffb_mode = fm2` in `wheel_system.cpp`) follows Forza
Motorsport 2 ReGPU's `src/input/wheel.cpp` (https://github.com/zer0ducksgiven/Forza-Motorsport-2-ReGPU,
MIT, by the same author as this project's base): its easing, fades, strengths and no rumble.

The device tables in those files credit their own sources: JacKeTUs/simracing-hwdb, Project Motor
Racing's device maps (Apkallu-Industries/Pitwall), gotzl/hid-fanatecff, mherbold/SimagicHPR, and
Logitech's and PCGamingWiki's button tables.

```text
BSD 3-Clause License

Copyright (c) 2026, Forza Horizon 1: ReImagined contributors

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

3. Neither the name of the copyright holder nor the names of its
   contributors may be used to endorse or promote products derived from
   this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```
