/* SMK headless integration. The upstream beam owns timing and HDMA.
 * One host frame spans the next 262 scanline boundaries, starting at RESET.
 * AOT/DMA may overshoot a deadline; max_late records that unresolved accuracy
 * limit instead of silently asserting cycle-accurate equivalence.
 */
#include "game_rtl.h"
#include "common_rtl.h"
#include "cpu_state.h"
#include "snes/snes.h"
#include "snes/dma.h"
#include "snes/ppu.h"
#include "snes/interp_bridge.h"
#include <stdio.h>
#include <stdlib.h>

#define MASTER_CYCLES_PER_LINE 1364ull
#define SCANLINES_PER_FRAME 262u
#define MASTER_CYCLES_PER_FRAME (SCANLINES_PER_FRAME * MASTER_CYCLES_PER_LINE)

static uint32_t resume_pc;
static uint64_t s_frame_base_clock;
static int parked;
static unsigned s_decomp_slice_step = 8;
uint64_t smk_nmi_count, smk_irq_count, smk_max_late;
uint64_t smk_total_slices, smk_dsp_slices, smk_decomp_slices;

static uint32_t vector(uint32_t adr) {
  return snes_read(g_snes,adr) | ((uint32_t)snes_read(g_snes,adr+1)<<8);
}
static void interrupt_at(uint32_t adr) {
  /* Save DMA channel 0 settings (used by mainline VRAM queue flush) so interrupt
   * routines like $83E7 (OBJ palette CGRAM upload) don't leave bAdr pointing
   * at $2122 CGDATA when mainline VRAM DMA resumes. Channel 1-7 are untouched (HDMA). */
  DmaChannel saved_ch0;
  if (g_snes && g_snes->dma) {
    saved_ch0 = g_snes->dma->channel[0];
  }

  /* RTI restores the interrupted stream. The bridge's resume cursor is not
   * an interrupt return value; keep the original mainline PC. */
  cpu_push_interrupt_frame_at(&g_cpu,resume_pc);
  interp_bridge_set_master_deadline(0);
  if(!interp_bridge_run_interrupt(&g_cpu,vector(adr))) Die("interrupt did not reach RTI");

  if (g_snes && g_snes->dma) {
    g_snes->dma->channel[0].bAdr = saved_ch0.bAdr;
    g_snes->dma->channel[0].mode = saved_ch0.mode;
    g_snes->dma->channel[0].aAdr = saved_ch0.aAdr;
    g_snes->dma->channel[0].aBank = saved_ch0.aBank;
    g_snes->dma->channel[0].size = saved_ch0.size;
  }
  parked=0;
}
static void run_scanline(uint64_t line_deadline) {
  /* Step 1: Advance beam to the scanline deadline, dispatching any raster IRQ that asserts */
  if (g_snes) {
    snes_sync_master_clock(g_snes, line_deadline);
    while (g_snes->inIrq && !g_cpu._flag_I) {
      ++smk_irq_count;
      interrupt_at(g_cpu.emulation ? 0xfffe : 0xffee);
      snes_sync_master_clock(g_snes, line_deadline);
    }
  }

  /* Step 2: Run CPU until it catches up to the scanline deadline or parks */
  for (unsigned slices = 0; g_cpu.master_cycles < line_deadline; ++slices) {
    if (slices > 100000) Die("frame slice limit");
    ++smk_total_slices;

    if (g_snes->inIrq && !g_cpu._flag_I) {
      ++smk_irq_count;
      interrupt_at(g_cpu.emulation ? 0xfffe : 0xffee);
      continue;
    }

    if (parked) {
      uint64_t target = line_deadline;
      uint32_t irq = snes_master_clocks_until_irq(g_snes);
      if (irq && !g_cpu._flag_I && g_cpu.master_cycles + irq < target)
        target = g_cpu.master_cycles + irq;
      g_cpu.master_cycles = target;
      snes_refresh_exempt();
      continue;
    }

    /* Fine slice only where strictly required (DSP-1 poll and decompressor) */
    uint64_t slice = line_deadline;
    uint32_t pc = resume_pc;
    unsigned pb = (pc >> 16) & 0xFF;
    unsigned off = pc & 0xFFFF;
    if (pb == 0x01 && off >= 0xF900 && off < 0xFA80) {
      slice = g_cpu.master_cycles + 2;
      ++smk_dsp_slices;
    } else if (pb == 0x84 && off >= 0xDF00 && off < 0xE200) {
      slice = g_cpu.master_cycles + s_decomp_slice_step;
      ++smk_decomp_slices;
    }
    if (slice > line_deadline) slice = line_deadline;

    interp_bridge_set_master_deadline(slice);
    if (!interp_bridge_run_until_quiescent(&g_cpu, resume_pc)) Die("mainline bridge bailed");
    interp_bridge_set_master_deadline(0);

    resume_pc = interp_bridge_lle_resume_pc();
    int wai = interp_bridge_lle_took_wai();
    int quiescent = interp_bridge_lle_took_quiescent();
    parked = wai || quiescent;
  }

  if (g_cpu.master_cycles > line_deadline && g_cpu.master_cycles - line_deadline > smk_max_late)
    smk_max_late = g_cpu.master_cycles - line_deadline;
}

void GameRunOneFrame(void) {
  static unsigned s_cur_frame = 0;
  s_cur_frame++;

  if (!resume_pc) {
    /* The AOT desktop route is state-exact at 32 master clocks on the
     * validated race script and avoids hundreds of thousands of bridge
     * re-entries while decompressing. Keep the interpreter-only reference at
     * 8: a wider deadline changed three strict stack-window snapshots there. */
    s_decomp_slice_step = interp_bridge_scheduler_aot_enabled() ? 32 : 8;
    resume_pc = vector(0xfffc);
    s_frame_base_clock = 0;
    g_cpu.master_cycles = 0;
    if (g_snes) {
      g_snes->beamMasterLast = 0;
      g_snes->hPos = 0;
      g_snes->vPos = 0;
    }
    ppu_runLine(g_ppu, 0);
  }

  for (unsigned line = 0; line < SCANLINES_PER_FRAME; ++line) {
    if (line == 0) g_snes->inNmi = false;
    if (line == 225) {
      ppu_checkOverscan(g_ppu);
      ppu_handleVblank(g_ppu);
      g_snes->inNmi = true;
      if (g_snes->nmiEnabled) {
        ++smk_nmi_count;
        interrupt_at(g_cpu.emulation ? 0xfffa : 0xffea);
      }
    }

    uint64_t line_deadline = s_frame_base_clock + (uint64_t)(line + 1) * MASTER_CYCLES_PER_LINE;
    run_scanline(line_deadline);

    if (line <= 224) {
      ppu_runLine(g_ppu, (int)line);
    }
  }
  s_frame_base_clock += MASTER_CYCLES_PER_FRAME;
}
void GameDrawPpuFrame(void) { /* Lines are rendered on the shared timeline. */ }
const RtlGameInfo kGameInfo={.title="smk",.run_frame=GameRunOneFrame,
  .draw_ppu_frame=GameDrawPpuFrame,.save_name_prefix="save",.tier2_capture=true};
void GameSessionReset(void) {
  resume_pc=0;s_frame_base_clock=0;parked=0;
  s_decomp_slice_step=8;
  smk_nmi_count=smk_irq_count=smk_max_late=0;
  smk_total_slices=smk_dsp_slices=smk_decomp_slices=0;
  if (g_snes) {
    g_snes->beamMasterLast = 0;
    g_snes->hPos = 0;
    g_snes->vPos = 0;
  }
}
