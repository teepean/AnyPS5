# Stray on AnyKyty (Linux)

This branch is [AnyKyty](https://github.com/simnJS/AnyPS5/tree/AnyKyty) at `7d8e5d163c`, unchanged, plus this guide and two helper scripts in `tools/stray/`. AnyKyty is AnyPS5 with the graphics core of KytyPS5-GTA. With it, Stray (PPSA02100) runs at its 60 FPS cap at the start of Dead City on stock game settings, and plays smoothly.

## What was measured

Stray PPSA02100 v01.005.000, stock game settings (`engine.ini` untouched, 4K), standing at the start of Dead City after loading a save. One run each:

| Build | FPS | GPU busy |
|---|---|---|
| AnyKyty, default 60 Hz vblank | 59.9 (the cap) | 77 % |
| AnyKyty, `ANYPS5_VBLANK_HZ=240` | 67.7 (65 to 70) | 83 % |
| AnyPS5's own GPU driver, our best tuned build (October 2026) | about 20 to 21 | |

The picture matches what AnyPS5's own driver renders at the same spot. The game was also played for a while from that save: it is fully playable, and with `KYTY_ASYNC_PIPELINES=2` (the default of `play-stray.sh`) there was no stutter.

Test machine: CachyOS (Linux 7.3.1), KDE Plasma with the game under Xwayland, AMD Ryzen 9 9950X, NVIDIA GeForce RTX 3090 with driver 615.78.08.

## 1. Build

Install what [the AnyKyty guide](ANYKYTY.md#linux) lists: Git, CMake, Ninja, GCC and an LLVM release (`clang`, `clang++` and `ld.lld` of the same major version on `PATH`), plus the X11, Wayland, audio and Vulkan development packages. The guide names GCC 14 and LLVM 21; GCC 16.2.1 and LLVM 23.1.4 (with CMake 4.4.5) worked here too.

```sh
git clone --recurse-submodules --branch stray-anykyty https://github.com/teepean/AnyPS5.git
cd AnyPS5
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF \
  -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++ \
  -DANYPS5_GPU_BACKEND=kyty -DANYPS5_KYTY_JOBS="$(nproc)"
cmake --build build --target kyty_gpu libs relinker --parallel "$(nproc)"
```

A CMake warning that the Kyty PGO profile is out of date for some sources is expected; the build still uses it for the others.

## 2. Game files

You need your own decrypted copy of Stray: the folder that the PS5 mounts as `/app0`, with a plain ELF `eboot.bin`, the bundled modules as plain ELF files in `sce_module/`, and the game data next to them. A container still in SELF form can be unwrapped with `tools/unwrap_self.py`, see [the relinker guide](USAGE.md). The setup below was tested with v01.005.000.

## 3. Set up a run folder

```sh
tools/stray/setup-stray.sh /path/to/Stray/app0 ~/stray-run build
```

The script leaves the game folder unchanged. It:

- copies `eboot.bin` and the `.prx` modules into `~/stray-run/source/`, skipping any `*.guest.prx` left over from an earlier conversion;
- links the game data into `~/stray-run/app0/`;
- converts the game with the relinker into `~/stray-run/eboot` (the converted modules go to `~/stray-run/app0/sce_module/`);
- then links `build/core/libs/libs` as `~/stray-run/libs`.

The link to `libs` is made after the conversion on purpose, as the AnyKyty guide does it: with `libs/` present, the relinker resolves one more library and writes a slightly different `eboot` than the one tested here.

## 4. Play

```sh
tools/stray/play-stray.sh ~/stray-run
```

The script sets these, and each can be overridden from the environment:

| Setting | Value here | Why |
|---|---|---|
| `KYTY_ASYNC_PIPELINES` | `2` | New shader pipelines compile in the background and their draws wait for nothing, so there is no compile stutter; something seen for the first time can appear a few frames late. AnyKyty's own default is `1`, because a heap corruption was seen with `2` in Ghost of Yōtei; on Stray none was seen. If you get crashes, use `KYTY_ASYNC_PIPELINES=1`. |
| `__GL_SHADER_DISK_CACHE_SIZE` | `10737418240` (10 GiB) | NVIDIA's driver keeps compiled pipelines in its disk cache, which is limited to about 1 GB by default. A larger cache keeps them across sessions. |
| `__GL_SHADER_DISK_CACHE_SKIP_CLEANUP` | `1` | Same reason: no automatic cleanup of that cache. |
| `SDL_VIDEODRIVER` | `x11` | The window under Xwayland, as tested. |

`ANYPS5_VBLANK_HZ=240 tools/stray/play-stray.sh ~/stray-run` lifts the 60 FPS cap.

The first launch, and the first visit to each area, compiles shaders. Kyty keeps its translated shaders and a list of the game's pipelines in `~/stray-run/_PipelineCache/`, and NVIDIA's driver keeps the compiled pipelines in `~/.cache/nvidia/GLCache/`, so later sessions start faster.

### Saves

Saves are in `~/stray-run/_sd/`. Without saves the game starts a new game. To continue elsewhere, copy an existing `_sd` folder there while the game is not running.

### Controls

| Keys | Pad |
|---|---|
| W A S D | Left stick (move) |
| T F G H | Right stick (camera: T up, F left, G down, H right) |
| Enter, Space | Cross |
| C | Circle |
| I | Triangle |
| Left mouse button | Square |
| Right mouse button | R2 |
| Q, E | L1, R1 |
| R | L2 |
| Arrow keys | D-pad |
| Shift, Ctrl | L3, R3 |
| Esc | Options |
| F11 | Fullscreen |
| F10 | Performance overlay |

The full mapping and how to change it is in [the input mapping guide](INPUT_MAPPING.md).

## Credits

AnyKyty is the work of [@simnJS](https://github.com/simnJS) and its contributors. Its graphics core comes from the KytyPS5-GTA and [KytyPS5](https://github.com/KytyPS5/KytyPS5) contributors, and everything else from [AnyPS5](https://github.com/boykopovar/AnyPS5) and its contributors. This branch adds only this guide and the scripts in `tools/stray/`.

Written with Claude Code; the runs are from the author's machine.
