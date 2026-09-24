# SuperMarioKartRecomp Architecture Guide

## Overview

SuperMarioKartRecomp is a static recompiled native port of the 1992 SNES game *Super Mario Kart* (USA).
It uses the hybrid **LLE / AOT** execution model provided by the `snesrecomp` framework.

## 1. Dual-Tier Execution: CPU AOT + Silicon LLE

* **Static CPU AOT (`src/game/`)**:
  All 65816 CPU assembly routines from the original ROM have been statically analyzed, decoded, and transformed into equivalent C code. Each 64KB ROM bank is emitted as a dedicated compilation unit (`bank00_system_core.c` through `bank85_effects_ending.c`).
* **Silicon Emulation (`snesrecomp/runner/src/snes/`)**:
  Hardware components that lack a static instruction stream (the Picture Processing Unit, the SPC700 audio coprocessor, DMA channels, and Mode 7 matrix registers) run via an embedded cycle-accurate emulator core derived from LakeSnes and snes9x.
* **DSP-1 Coprocessor**:
  Super Mario Kart uses the NEC µPD77C25 (DSP-1) chip on cartridge for Mode 7 3D coordinate transformations and track projection. This is handled via high-level emulation (HLE) without requiring external ROM dumps.

## 2. Raster Timing & Split-Screen Interrupts

Super Mario Kart divides the 224-line screen into two distinct rendering zones:
1. **Upper Screen (Lines 0–111)**: First-person 3D Mode 7 perspective of the track and player kart.
2. **Lower Screen (Lines 112–223)**: 2D aerial top-down view of the course and 8-kart positions, or second player split-screen.

At scanline 112, the game triggers a timer IRQ (`0x808B3D`) that reconfigures the Mode 7 matrix registers (`M7A`, `M7B`, `M7C`, `M7D`, `M7X`, `M7Y`) and OBSEL sprite layer settings.
In `snesrecomp/runner/src/snes/snes.c`, strict clock monotonicity is enforced (`if (master_clock <= snes->beamMasterLast) return;`) to guarantee zero phase drift between the CPU and PPU raster beam across thousands of frames.

## 3. High-Precision Frame Pacing

* **Windows 1ms Multimedia Clock**:
  The host runtime dynamically activates `timeBeginPeriod(1)` via `winmm.dll` to bypass the default 15.625 ms Windows timer quantization.
* **Time Accumulator**:
  The main loop advances via `next_frame_time += target_frame_cycles` (`SDL_GetPerformanceFrequency() / 60.09881389744051`) with sub-millisecond spin-wait micro-adjustments, ensuring exact 60.0988 Hz hardware timing.
* **Asynchronous Audio Pull**:
  Audio is streamed on a dedicated SDL callback thread (`AudioCallback`) at 44.1 kHz stereo, synchronized with the APU core via `RtlApuLock` / `RtlApuUnlock` recursive mutexes.
