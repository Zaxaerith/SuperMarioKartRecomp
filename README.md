# SuperMarioKartRecomp

[![Build Status](https://img.shields.io/badge/build-passing-brightgreen)](#building-from-source)
[![Platform](https://img.shields.io/badge/platform-Windows%20%7C%20Linux-blue)](#prerequisites)
[![Language](https://img.shields.io/badge/language-C11-orange)](#what-static-recompilation-means-here)
[![License](https://img.shields.io/badge/license-PolyForm--Noncommercial--1.0.0-blue)](LICENSE)

Native C static recompilation of **Super Mario Kart (SNES, USA)** into standalone, native PC executable using the [snesrecomp](https://github.com/mstan/snesrecomp) framework.

---

## What "static recompilation" means here

The 65816 CPU assembly code from the original ROM is statically translated to pure native C — every function the game executes on the main SNES CPU is compiled as real C translation units located in `src/game/`.

**The rest of the SNES is not recompiled — it is hardware**:
* **PPU (Picture Processing Unit)**, Mode 7 matrix transforms, HDMA raster effects, and sprite rendering run through an optimized C hardware implementation in `snesrecomp/runner/src/snes/`.
* **APU (Audio Processing Unit)** and the SPC700 audio coprocessor run asynchronously with native sample resampling and anti-starvation buffers.
* **DSP-1 Coprocessor** uses firmware-free high-level emulation (HLE) to execute perspective projection and trigonometric track matrix operations.

This follows the established architecture of modern static recompilation projects: **recompile the CPU, emulate the silicon**.

---

## Current Status & Features

- [x] **Fully Playable**: Tested and verified end-to-end across Grand Prix cups and Battle mode.
- [x] **Native 60 FPS**: Exact 60.0988 Hz hardware pacing using sub-millisecond precision accumulators.
- [x] **Mode 7 Split-Screen Graphics**: Full resolution Mode 7 rendering with hardware raster interrupt at scanline 112 (upper 3D cockpit perspective, lower 2D aerial course map).
- [x] **DSP-1 Mathematical Coprocessor**: Integrated HLE for high-performance track projection and coordinate transformations.
- [x] **Controller Support**: Full plug-and-play support for Xbox, PlayStation, Switch Pro, and standard USB/Bluetooth gamepads via SDL2 GameController.
- [x] **Independent 2-Player Controls**: Complete simultaneous keyboard mapping (WASD + Arrow Keys for P1, Numpad for P2) and multi-gamepad routing.
- [x] **Ultra-Low Latency Audio**: Dedicated background audio pull callback at 44.1 kHz with APU mutex synchronization.

---

## Quick Start (ROM Requirement)

> [!IMPORTANT]
> **Legal Notice**: This repository does **NOT** contain any copyrighted ROM data, game assets, or proprietary Nintendo code. You must provide your own legally obtained ROM dump to run the game.

### Verified ROM Information
* **Game**: Super Mario Kart (USA)
* **File Name**: `Super Mario Kart (USA).sfc`
* **File Size**: `524,288` bytes (Headerless `.sfc`) or `524,800` bytes (512-byte headered `.smc`)
* **SHA256**: `2ada8919688087be60a6a48cace8f877add60c45d2e5d09e2442faa55be62a49`
* **CRC32**: `C7880A64`

Simply place your ROM file in the same directory as the executable (or in the project root) as `Super Mario Kart (USA).sfc`.

---

## Controls

### Player 1 (Keyboard & Gamepad)

| Action | Keyboard | Xbox Gamepad | PS Gamepad | SNES Original |
| :--- | :--- | :--- | :--- | :--- |
| **Steer / Menu Navigation** | `W / A / S / D` or `Arrow Keys` | `D-Pad` / `Left Stick` | `D-Pad` / `Left Stick` | `D-Pad` |
| **Accelerate / Confirm** | `J` or `Space` | `A` Button | `Cross (X)` | `B` Button |
| **Use Item / Cancel** | `K` | `B` Button | `Circle (O)` | `A` Button |
| **Hop / Drift** | `U` | `X` Button | `Square` | `Y` Button |
| **Rearview Mirror** | `I` | `Y` Button | `Triangle` | `X` Button |
| **Hop / Drift (Shoulder)** | `Q` / `E` | `LB` / `RB` / Triggers | `L1` / `R1` / Triggers | `L` / `R` Buttons |
| **Start / Pause** | `Enter` | `Start` | `Options` | `START` |
| **Select** | `Tab` | `Back` | `Share` | `SELECT` |
| **Toggle Fullscreen** | `F11` or `Alt+Enter` | - | - | - |
| **Quit Game** | `Esc` | - | - | - |

### Player 2 (Numpad)
* **Steer**: `Numpad 8 / 2 / 4 / 6`
* **Accelerate (B)**: `Numpad 1` or `Numpad 0`
* **Item (A)**: `Numpad 3`
* **Hop / Drift (Y)**: `Numpad 7`
* **Rearview (X)**: `Numpad 9`
* **Start**: `Numpad Enter`
* **Select**: `Numpad +`

> [!TIP]
> **Single Player (1P GAME) Tip**: On the title screen, press `Space` or `Enter` to open the main menu. The menu cursor defaults to `2P GAME`. Press `W` or `Up Arrow` to move the cursor to `1P GAME`, then press `Space` or `Enter` to begin your single-player Grand Prix!

---

## Building from Source

### Prerequisites
* **CMake** (>= 3.20)
* **C Compiler**: GCC (MinGW-w64 on Windows), Clang, or MSVC
* **Build System**: Ninja (recommended) or Make
* **SDL2**: Development library (`SDL2-devel`)

### Windows (Ninja + GCC / Clang)

```powershell
# Clone the repository
git clone https://github.com/Zaxaerith/SuperMarioKartRecomp.git
cd SuperMarioKartRecomp

# Configure and build using Ninja
mkdir build
cd build
cmake .. -G "Ninja" -DCMAKE_BUILD_TYPE=Release
ninja smk_play

# Run the game (make sure Super Mario Kart (USA).sfc is in the directory)
./smk_play.exe
```

Or simply run the automated build script:
```powershell
./build.ps1
```

### Linux (Ubuntu / Debian / Fedora / Arch)

```bash
# Install dependencies (Ubuntu/Debian)
sudo apt update && sudo apt install -y build-essential cmake ninja-build libsdl2-dev

# Build
mkdir build && cd build
cmake .. -G "Ninja" -DCMAKE_BUILD_TYPE=Release
ninja smk_play

# Run
./smk_play
```

---

## Repository Structure

```
SuperMarioKartRecomp/
├── CMakeLists.txt              # Unified CMake build configuration
├── README.md                   # Project documentation & guide
├── LICENSE                     # PolyForm Noncommercial License 1.0.0
├── build.ps1 / build.sh        # Quick build helpers
├── config/                     # Recompiler function symbols & configuration
├── src/
│   ├── desktop.c               # Interactive SDL2 desktop runner (window, audio, input)
│   ├── game_rtl.c              # Game runtime bridge (PPU rendering & scanline sync)
│   ├── headless.c             # Diagnostic headless benchmark runner
│   └── game/                   # Recompiled native C source code (by function)
│       ├── bank00_system_core.c       # Reset vector, NMI, IRQ & Core Engine
│       ├── bank01_kart_physics.c      # Kart physics, steering & race state
│       ├── bank04_audio_loader.c      # APU sound driver & music sequence loader
│       ├── bank05_title_menu.c        # Title screen, menu selection & Mode 7 demo
│       ├── bank80_raster_loop.c       # Main game loop, split-screen raster IRQ & DMA
│       ├── bank81_items_ai.c          # Items, weapons & driver AI logic
│       ├── bank83_dsp1_math.c         # DSP-1 coprocessor interface & 3D projection
│       ├── bank84_track_decompress.c  # Track data, tilemap & graphics decompressor
│       ├── bank85_effects_ending.c    # Awards ceremony, ending & special effects
│       ├── recomp_dispatch.c          # LLE/AOT function jump dispatch table
│       ├── unresolved_stubs.c         # Unresolved stubs fallback handler
│       └── program_manifest.json      # Complete recompiler code manifest
├── snesrecomp/                 # SNESRecomp core LLE execution framework
└── tools/                      # Validation, regression and profiling scripts
```

---

## License

This project is a recompiled derivative work based on [snesrecomp](https://github.com/mstan/snesrecomp) and is licensed under the **PolyForm Noncommercial License 1.0.0**.

* **Source-Available / Noncommercial**: Any noncommercial purpose (personal use, personal study, research, private entertainment, non-profit community testing) is permitted.
* **Commercial Use Prohibited**: Commercial use, monetized distribution, or deriving profit from this software is strictly prohibited under the upstream license terms.
* **Upstream Copyright**: `Copyright (c) 2026 Matthew Stanley`.
* For the full legal text, see the [LICENSE](LICENSE) file.

---

## Credits & Acknowledgments

* **[Zaxaerith](https://github.com/Zaxaerith)**: Project porting, recompilation integration, timing synchronization, and host runtime.
* **Nintendo**: Original creators of *Super Mario Kart* (1992).
* **[mstan](https://github.com/mstan)**: Author of the [snesrecomp](https://github.com/mstan/snesrecomp) framework and pioneer of SNES static recompilation.
* **LakeSnes & snes9x**: Foundation for embedded SNES silicon emulation.
* All contributors to the Super Mario Kart reverse engineering and recompilation efforts.
