# Third-party notices

This repository does not bundle any third-party code or binaries. During the
build, `scripts/setup.ps1` downloads the ReXGlue SDK and its dependencies (git
submodules) and compiles them on your machine. The resulting files in `build`
and `dist` are covered by the licenses below. If you redistribute any built
files, you are responsible for complying with them.

## ReXGlue SDK

https://github.com/rexglue/rexglue-sdk — `patches/rexglue-sdk.patch` is a
modification of this software and is distributed under the same license:

```
Copyright (c) 2026, Tom Clay <tomc@tctechstuff.com>

Portions of this software are derived from the Xenia project:
Copyright (c) 2022, Ben Vanik and Xenia project contributors
https://xenia.jp

All rights reserved.

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

## ReXGlue SDK dependencies

Fetched as submodules of the SDK into `build/rexglue-sdk/thirdparty`. Each
project's full license text is in its folder there.

| Project | License |
|---|---|
| FFmpeg (XMA audio decoding) | LGPL-2.1-or-later |
| SDL 3 | zlib |
| libmspack | LGPL-2.1 |
| fmt | MIT |
| spdlog | MIT |
| Dear ImGui | MIT |
| toml++ | MIT |
| inja | MIT |
| SIMDe | MIT |
| o1heap | MIT |
| Vulkan Memory Allocator | MIT |
| stb | MIT / public domain |
| xxHash | BSD-2-Clause |
| Tracy | BSD-3-Clause |
| CLI11 | BSD-3-Clause |
| glslang | BSD-3-Clause and others |
| SPIRV-Tools, SPIRV-Headers, Vulkan-Headers, Vulkan-Loader | Apache-2.0 |
| Catch2, utfcpp | BSL-1.0 |

The list reflects the SDK version pinned in `scripts/setup.ps1`; the SDK's own
repository is authoritative.

## Game data

Nothing in this repository comes from the game. `tools/display_tab` reads and
patches the `fe.big` file of your own copy of the game, on your own PC, during
setup. Fight Night Champion and everything extracted or generated from it
remain the property of their owner.
