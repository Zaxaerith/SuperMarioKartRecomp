/*
 * Super Mario Kart - Interactive Desktop Host (SNESRecomp Native AOT)
 * Provides real-time SDL2 window, keyboard controls, and 60 FPS gameplay.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#define SDL_MAIN_HANDLED
#include <SDL.h>

#include "game_rtl.h"
#include "common_rtl.h"
#include "cpu_state.h"
#include "snes/snes.h"
#include "snes/ppu.h"
#include "snes/dsp1.h"
#include "snes/apu.h"
#include "snes/dsp.h"
#include "snes/interp_bridge.h"
#include "config.h"

#ifdef _WIN32
#include <windows.h>
typedef MMRESULT (WINAPI *timeBeginPeriod_t)(UINT uPeriod);
typedef MMRESULT (WINAPI *timeEndPeriod_t)(UINT uPeriod);
static timeBeginPeriod_t pfn_timeBeginPeriod = NULL;
static timeEndPeriod_t pfn_timeEndPeriod = NULL;
static void init_timer_precision(void) {
  HMODULE h = LoadLibraryA("winmm.dll");
  if (h) {
    pfn_timeBeginPeriod = (timeBeginPeriod_t)GetProcAddress(h, "timeBeginPeriod");
    pfn_timeEndPeriod = (timeEndPeriod_t)GetProcAddress(h, "timeEndPeriod");
    if (pfn_timeBeginPeriod) pfn_timeBeginPeriod(1);
  }
}
static void cleanup_timer_precision(void) {
  if (pfn_timeEndPeriod) pfn_timeEndPeriod(1);
}
#endif

struct SpcPlayer *g_spc_player = NULL;
Config g_config;
int g_ws_extra = 0;
extern int g_watchdog_tripped;
extern uint64_t smk_nmi_count, smk_irq_count, smk_max_late;
extern uint64_t smk_total_slices, smk_dsp_slices, smk_decomp_slices;

static SDL_mutex *s_apu_mutex = NULL;
/* Optional frame profiler. All main-thread counters are read/reset only there;
 * callback counters use SDL atomics because the device owns another thread. */
static FILE *s_perf_log = NULL;
static Uint32 s_main_thread = 0;
static double s_main_lock_wait_ms = 0, s_main_lock_max_ms = 0;
static unsigned s_main_lock_calls = 0;
static _Thread_local unsigned s_lock_probe_tick = 0;
static SDL_atomic_t s_cb_count, s_cb_time_us, s_cb_lock_us, s_cb_starved;
static bool s_diag_silent_audio = false;
static double perf_ms(uint64_t a, uint64_t b) {
  return (double)(b - a) * 1000.0 / (double)SDL_GetPerformanceFrequency();
}
void RtlApuLock(void) {
  if (!s_apu_mutex) return;
  /* The guest can acquire this lock >10,000 times per frame. Sampling avoids
   * making the profiler itself a substantial source of missed deadlines. */
  if (s_perf_log && (++s_lock_probe_tick & 127u) == 0 &&
      SDL_ThreadID() == s_main_thread) {
    uint64_t start = SDL_GetPerformanceCounter();
    SDL_LockMutex(s_apu_mutex);
    double ms = perf_ms(start, SDL_GetPerformanceCounter());
    s_main_lock_wait_ms += ms * 128.0;
    if (ms > s_main_lock_max_ms) s_main_lock_max_ms = ms;
    s_main_lock_calls += 128;
  } else {
    SDL_LockMutex(s_apu_mutex);
  }
}
void RtlApuUnlock(void) {
  if (s_apu_mutex) SDL_UnlockMutex(s_apu_mutex);
}

#define HOST_AUDIO_PREFILL 2136u
static bool s_audio_primed = false;
static int g_frames_per_block = 534;
static int g_audio_channels = 2;
static uint8_t *g_audiobuffer = NULL;
static uint8_t *g_audiobuffer_cur = NULL;
static uint8_t *g_audiobuffer_end = NULL;

static void SDLCALL AudioCallback(void *userdata, Uint8 *stream, int len) {
  (void)userdata;
  uint64_t cb_start = s_perf_log ? SDL_GetPerformanceCounter() : 0;
  if (s_perf_log) SDL_AtomicIncRef(&s_cb_count);
  if (s_diag_silent_audio) {
    memset(stream, 0, len);
    if (s_perf_log) SDL_AtomicAdd(&s_cb_time_us,
        (int)(perf_ms(cb_start, SDL_GetPerformanceCounter()) * 1000.0));
    return;
  }
  if (!s_apu_mutex || !g_audiobuffer || SDL_LockMutex(s_apu_mutex) != 0) {
    memset(stream, 0, len);
    return;
  }
  if (s_perf_log) SDL_AtomicAdd(&s_cb_lock_us,
      (int)(perf_ms(cb_start, SDL_GetPerformanceCounter()) * 1000.0));

  while (len != 0) {
    if (g_audiobuffer_end - g_audiobuffer_cur == 0) {
      uint32_t available = (g_snes && g_snes->apu && g_snes->apu->dsp)
          ? dsp_available(g_snes->apu->dsp) : 0;
      if (!s_audio_primed && available < HOST_AUDIO_PREFILL) {
        if (s_perf_log) SDL_AtomicIncRef(&s_cb_starved);
        memset(g_audiobuffer, 0, g_frames_per_block * g_audio_channels * sizeof(int16_t));
      } else {
        s_audio_primed = true;
        RtlRenderAudio((int16_t *)g_audiobuffer, g_frames_per_block, g_audio_channels);
        if (g_snes && g_snes->apu && g_snes->apu->dsp && dsp_available(g_snes->apu->dsp) < 4)
          s_audio_primed = false;
      }
      g_audiobuffer_cur = g_audiobuffer;
      g_audiobuffer_end = g_audiobuffer + g_frames_per_block * g_audio_channels * sizeof(int16_t);
    }
    int n = (len < (int)(g_audiobuffer_end - g_audiobuffer_cur))
        ? len : (int)(g_audiobuffer_end - g_audiobuffer_cur);
    memcpy(stream, g_audiobuffer_cur, n);
    g_audiobuffer_cur += n;
    stream += n;
    len -= n;
  }

  SDL_UnlockMutex(s_apu_mutex);
  if (s_perf_log) SDL_AtomicAdd(&s_cb_time_us,
      (int)(perf_ms(cb_start, SDL_GetPerformanceCounter()) * 1000.0));
}
void debug_on_block_enter(uint32_t a, uint32_t b, uint32_t c, uint32_t d) { (void)a; (void)b; (void)c; (void)d; }
void debug_on_wram_write_byte(uint32_t a, uint8_t b, uint8_t c) { (void)a; (void)b; (void)c; }
void debug_on_wram_write_word(uint32_t a, uint16_t b, uint16_t c) { (void)a; (void)b; (void)c; }
void NORETURN Die(const char *s) { fprintf(stderr, "FATAL: %s\n", s); exit(1); }

enum SnesButton {
  BTN_B      = 1 << 0,
  BTN_Y      = 1 << 1,
  BTN_SELECT = 1 << 2,
  BTN_START  = 1 << 3,
  BTN_UP     = 1 << 4,
  BTN_DOWN   = 1 << 5,
  BTN_LEFT   = 1 << 6,
  BTN_RIGHT  = 1 << 7,
  BTN_A      = 1 << 8,
  BTN_X      = 1 << 9,
  BTN_L      = 1 << 10,
  BTN_R      = 1 << 11,
};

static uint32_t s_pad_state = 0;
static SDL_GameController *s_controllers[2] = {NULL, NULL};
/* A deterministic route for performance probes only. Interactive input is
 * unchanged unless SMK_DIAG_SCRIPT is explicitly set. */
static struct {int begin, end; uint32_t mask;} s_diag_inputs[64];
static unsigned s_diag_input_count = 0;
static void parse_diag_script(void) {
  const char *script = getenv("SMK_DIAG_SCRIPT");
  if (!script || !*script) return;
  if (strlen(script) >= 2048) Die("diagnostic script too long");
  char buf[2048]; strcpy(buf, script);
  const char *names[] = {"B","Y","SELECT","START","UP","DOWN",
                         "LEFT","RIGHT","A","X","L","R"};
  for (char *t = strtok(buf, ","); t; t = strtok(NULL, ",")) {
    int a, b; char name[32];
    if (sscanf(t, "%d-%d:%31s", &a, &b, name) != 3) {
      if (sscanf(t, "%d:%31s", &a, name) != 2) Die("invalid diagnostic input");
      b = a + 3;
    }
    unsigned shift = 0; const char *button = name;
    if (!strncmp(name, "P1_", 3)) button = name + 3;
    else if (!strncmp(name, "P2_", 3)) {button = name + 3; shift = 12;}
    uint32_t mask = 0;
    for (int j = 0; j < 12; ++j)
      if (!strcmp(button, names[j])) mask = 1u << (j + shift);
    if (!mask || a < 0 || b < a || s_diag_input_count == 64)
      Die("invalid diagnostic input interval");
    s_diag_inputs[s_diag_input_count].begin = a;
    s_diag_inputs[s_diag_input_count].end = b;
    s_diag_inputs[s_diag_input_count++].mask = mask;
  }
}
static uint32_t diag_input_at(unsigned frame) {
  uint32_t buttons = 0;
  for (unsigned i = 0; i < s_diag_input_count; ++i)
    if (frame >= (unsigned)s_diag_inputs[i].begin &&
        frame <= (unsigned)s_diag_inputs[i].end)
      buttons |= s_diag_inputs[i].mask;
  return buttons;
}

static uint32_t poll_controller(SDL_GameController *ctrl) {
  if (!ctrl) return 0;
  uint32_t p = 0;
  if (SDL_GameControllerGetButton(ctrl, SDL_CONTROLLER_BUTTON_DPAD_UP))    p |= BTN_UP;
  if (SDL_GameControllerGetButton(ctrl, SDL_CONTROLLER_BUTTON_DPAD_DOWN))  p |= BTN_DOWN;
  if (SDL_GameControllerGetButton(ctrl, SDL_CONTROLLER_BUTTON_DPAD_LEFT))  p |= BTN_LEFT;
  if (SDL_GameControllerGetButton(ctrl, SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) p |= BTN_RIGHT;

  int16_t ax = SDL_GameControllerGetAxis(ctrl, SDL_CONTROLLER_AXIS_LEFTX);
  int16_t ay = SDL_GameControllerGetAxis(ctrl, SDL_CONTROLLER_AXIS_LEFTY);
  if (ax < -12000) p |= BTN_LEFT;
  if (ax > 12000)  p |= BTN_RIGHT;
  if (ay < -12000) p |= BTN_UP;
  if (ay > 12000)  p |= BTN_DOWN;

  /* SNES B is bottom button: on Xbox/standard gamepads, this is button A */
  if (SDL_GameControllerGetButton(ctrl, SDL_CONTROLLER_BUTTON_A)) p |= BTN_B;
  /* SNES A is right button: on Xbox/standard gamepads, this is button B */
  if (SDL_GameControllerGetButton(ctrl, SDL_CONTROLLER_BUTTON_B)) p |= BTN_A;
  /* SNES Y is left button: on Xbox/standard gamepads, this is button X */
  if (SDL_GameControllerGetButton(ctrl, SDL_CONTROLLER_BUTTON_X)) p |= BTN_Y;
  /* SNES X is top button: on Xbox/standard gamepads, this is button Y */
  if (SDL_GameControllerGetButton(ctrl, SDL_CONTROLLER_BUTTON_Y)) p |= BTN_X;

  if (SDL_GameControllerGetButton(ctrl, SDL_CONTROLLER_BUTTON_LEFTSHOULDER))  p |= BTN_L;
  if (SDL_GameControllerGetButton(ctrl, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)) p |= BTN_R;
  if (SDL_GameControllerGetAxis(ctrl, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 16000)  p |= BTN_L;
  if (SDL_GameControllerGetAxis(ctrl, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 16000) p |= BTN_R;

  if (SDL_GameControllerGetButton(ctrl, SDL_CONTROLLER_BUTTON_START)) p |= BTN_START;
  if (SDL_GameControllerGetButton(ctrl, SDL_CONTROLLER_BUTTON_BACK))  p |= BTN_SELECT;

  return p;
}

static void update_keyboard(const uint8_t *keys) {
  uint32_t p1 = 0;
  if (keys[SDL_SCANCODE_UP]    || keys[SDL_SCANCODE_W]) p1 |= BTN_UP;
  if (keys[SDL_SCANCODE_DOWN]  || keys[SDL_SCANCODE_S]) p1 |= BTN_DOWN;
  if (keys[SDL_SCANCODE_LEFT]  || keys[SDL_SCANCODE_A]) p1 |= BTN_LEFT;
  if (keys[SDL_SCANCODE_RIGHT] || keys[SDL_SCANCODE_D]) p1 |= BTN_RIGHT;
  if (keys[SDL_SCANCODE_J]     || keys[SDL_SCANCODE_SPACE]) p1 |= BTN_B;      /* Gas / Accelerate / Confirm */
  if (keys[SDL_SCANCODE_K])    p1 |= BTN_A;                                  /* Item / Confirm */
  if (keys[SDL_SCANCODE_U])    p1 |= BTN_Y;                                  /* Hop / Drift */
  if (keys[SDL_SCANCODE_I])    p1 |= BTN_X;                                  /* Rear view */
  if (keys[SDL_SCANCODE_Q])    p1 |= BTN_L;                                  /* Rear view / Drift */
  if (keys[SDL_SCANCODE_E])    p1 |= BTN_R;                                  /* Hop / Drift */
  if (keys[SDL_SCANCODE_RETURN]) p1 |= BTN_START;
  if (keys[SDL_SCANCODE_TAB]   || keys[SDL_SCANCODE_RSHIFT]) p1 |= BTN_SELECT;

  /* Controller 0 for Player 1 */
  p1 |= poll_controller(s_controllers[0]);

  /* Player 2 independent controls via Numpad */
  uint32_t p2 = 0;
  if (keys[SDL_SCANCODE_KP_8]) p2 |= BTN_UP;
  if (keys[SDL_SCANCODE_KP_2] || keys[SDL_SCANCODE_KP_5]) p2 |= BTN_DOWN;
  if (keys[SDL_SCANCODE_KP_4]) p2 |= BTN_LEFT;
  if (keys[SDL_SCANCODE_KP_6]) p2 |= BTN_RIGHT;
  if (keys[SDL_SCANCODE_KP_1] || keys[SDL_SCANCODE_KP_0]) p2 |= BTN_B;
  if (keys[SDL_SCANCODE_KP_3]) p2 |= BTN_A;
  if (keys[SDL_SCANCODE_KP_7]) p2 |= BTN_Y;
  if (keys[SDL_SCANCODE_KP_9]) p2 |= BTN_X;
  if (keys[SDL_SCANCODE_KP_ENTER]) p2 |= BTN_START;
  if (keys[SDL_SCANCODE_KP_PLUS])  p2 |= BTN_SELECT;
  if (keys[SDL_SCANCODE_KP_DIVIDE])   p2 |= BTN_L;
  if (keys[SDL_SCANCODE_KP_MULTIPLY]) p2 |= BTN_R;

  /* Controller 1 for Player 2 */
  p2 |= poll_controller(s_controllers[1]);

  s_pad_state = p1 | (p2 << 12);
}

static uint8_t* try_load_rom(const char *path, size_t *out_size) {
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  rewind(f);
  if (sz != 524288) {
    fclose(f);
    return NULL;
  }
  uint8_t *buf = (uint8_t*)malloc(sz);
  if (!buf || fread(buf, 1, sz, f) != (size_t)sz) {
    if (buf) free(buf);
    fclose(f);
    return NULL;
  }
  fclose(f);
  *out_size = (size_t)sz;
  return buf;
}

static uint8_t* find_and_load_rom(int argc, char **argv, size_t *out_size) {
  if (argc >= 2) {
    uint8_t *buf = try_load_rom(argv[1], out_size);
    if (buf) {
      printf("[ROM] Loaded from command line: %s\n", argv[1]);
      return buf;
    }
  }
  const char *candidates[] = {
    "Super Mario Kart (USA).sfc",
    "../Super Mario Kart (USA).sfc",
    "roms/Super Mario Kart (USA).sfc",
    "../roms/Super Mario Kart (USA).sfc",
    "Super Mario Kart (USA).smc",
    "../Super Mario Kart (USA).smc"
  };
  for (size_t i = 0; i < sizeof(candidates)/sizeof(candidates[0]); i++) {
    uint8_t *buf = try_load_rom(candidates[i], out_size);
    if (buf) {
      printf("[ROM] Found and loaded: %s\n", candidates[i]);
      return buf;
    }
  }
  return NULL;
}

int main(int argc, char **argv) {
#ifdef _WIN32
  init_timer_precision();
#endif
  printf("====================================================\n");
  printf("  Super Mario Kart (SNESRecomp Native AOT Desktop)  \n");
  printf("====================================================\n");
  printf("Player 1 Controls (Keyboard):\n");
  printf("  W / A / S / D or Arrow Keys : Steer / D-Pad\n");
  printf("  J or Space                  : Accelerate / Menu Confirm (B)\n");
  printf("  K                           : Use Item / Confirm (A)\n");
  printf("  U                           : Hop / Drift (Y)\n");
  printf("  I                           : Rearview Mirror (X)\n");
  printf("  Q / E                       : L / R Shoulder (Drift)\n");
  printf("  Enter                       : START / Confirm\n");
  printf("  Tab                         : SELECT\n");
  printf("  F11 / Alt+Enter             : Toggle Fullscreen\n");
  printf("  Esc                         : Quit\n");
  printf("----------------------------------------------------\n");
  printf("Player 2 Controls (Numpad):\n");
  printf("  Numpad 8/2/4/6 : D-Pad\n");
  printf("  Numpad 1/0     : Accelerate / Confirm (B)\n");
  printf("  Numpad 3       : Item (A)\n");
  printf("  Numpad Enter   : START\n");
  printf("----------------------------------------------------\n");
  printf("Gamepad Support:\n");
  printf("  Plug-and-play USB / Bluetooth controllers supported!\n");
  printf("----------------------------------------------------\n");
  printf("Single Player Guide (1P GAME):\n");
  printf("  1. Press Enter/Space on title screen to open menu.\n");
  printf("  2. Menu cursor defaults to '2P GAME'.\n");
  printf("  3. Press 'W' or 'Up Arrow' to move cursor to '1P GAME'.\n");
  printf("  4. Press Space or Enter to enter Single Player mode!\n");
  printf("====================================================\n\n");

  size_t rom_size = 0;
  uint8_t *rom_data = find_and_load_rom(argc, argv, &rom_size);
  if (!rom_data) {
    fprintf(stderr, "Error: Could not locate 'Super Mario Kart (USA).sfc' (524,288 bytes)!\n");
    fprintf(stderr, "Usage: smk_play [path_to_rom.sfc]\n");
    return 1;
  }

  RtlRegisterGame(&kGameInfo);
  if (!SnesInit(rom_data, (int)rom_size)) {
    fprintf(stderr, "Error: Failed to initialize SNES core!\n");
    free(rom_data);
    return 2;
  }
  free(rom_data);

  if (!cart_has_dsp1(g_snes->cart)) {
    fprintf(stderr, "Error: ROM does not have DSP-1 coprocessor!\n");
    return 3;
  }

  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0) {
    fprintf(stderr, "SDL_Init Error: %s\n", SDL_GetError());
    return 4;
  }
  s_main_thread = SDL_ThreadID();
  parse_diag_script();
  const char *perf_path = getenv("SMK_PERF_LOG");
  if (perf_path && *perf_path) {
    s_perf_log = strcmp(perf_path, "1") == 0 ? stdout : fopen(perf_path, "w");
    if (!s_perf_log) fprintf(stderr, "[perf] cannot open %s\n", perf_path);
  }
  const char *audio_mode = getenv("SMK_DIAG_AUDIO");
  bool diag_audio_off = audio_mode && strcmp(audio_mode, "off") == 0;
  s_diag_silent_audio = audio_mode && strcmp(audio_mode, "silent") == 0;
  const char *diag_frames_env = getenv("SMK_DIAG_FRAMES");
  unsigned diag_frames = diag_frames_env ? (unsigned)strtoul(diag_frames_env, NULL, 10) : 0;
  s_apu_mutex = SDL_CreateMutex();
  if (!s_apu_mutex) Die("SDL_CreateMutex failed");

  const int scale = 3;
  const int win_w = 256 * scale;
  const int win_h = 224 * scale;

  SDL_Window *window = SDL_CreateWindow(
    "Super Mario Kart - Recompiled Native AOT",
    SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
    win_w, win_h,
    SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE
  );
  if (!window) {
    fprintf(stderr, "SDL_CreateWindow Error: %s\n", SDL_GetError());
    SDL_Quit();
    return 5;
  }

  SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
  SDL_SetHint(SDL_HINT_RENDER_VSYNC, "0");

  SDL_Renderer *renderer = SDL_CreateRenderer(
    window, -1,
    SDL_RENDERER_ACCELERATED
  );
  if (!renderer) {
    renderer = SDL_CreateRenderer(window, -1, 0);
  }
  if (!renderer) Die("SDL_CreateRenderer failed");
  SDL_RendererInfo renderer_info;
  if (s_perf_log && SDL_GetRendererInfo(renderer, &renderer_info) == 0)
    fprintf(s_perf_log, "[perf] renderer=%s flags=0x%x vsync=%d audio=%s\n",
      renderer_info.name, renderer_info.flags,
      !!(renderer_info.flags & SDL_RENDERER_PRESENTVSYNC),
      audio_mode ? audio_mode : "normal");
  SDL_RenderSetLogicalSize(renderer, 256, 224);

  SDL_Texture *texture = SDL_CreateTexture(
    renderer,
    SDL_PIXELFORMAT_BGRA32,
    SDL_TEXTUREACCESS_STREAMING,
    256, 224
  );

  static uint8_t fb_pixels[256 * 4 * 256];
  PpuBeginDrawing(g_ppu, fb_pixels, 256 * 4, 0);

  SDL_AudioDeviceID audio_dev = 0;
  SDL_AudioSpec want, have;
  SDL_memset(&want, 0, sizeof(want));
  want.freq = 44100;
  want.format = AUDIO_S16;
  want.channels = 2;
  want.samples = 512;
  want.callback = AudioCallback;

  if (!diag_audio_off) audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
  if (audio_dev > 0) {
    g_audio_channels = 2;
    RtlSetAudioOutputRate(have.freq);
    g_frames_per_block = (534 * have.freq + 32040 / 2) / 32040;
    g_audiobuffer = (uint8_t *)calloc(g_frames_per_block * g_audio_channels * sizeof(int16_t), 1);
    g_audiobuffer_cur = g_audiobuffer;
    g_audiobuffer_end = g_audiobuffer;
    SDL_PauseAudioDevice(audio_dev, 0);
    printf("[Audio] SDL2 audio output opened at %d Hz stereo (%d samples/cb, %d frames/block)\n",
           have.freq, have.samples, g_frames_per_block);
  } else {
    printf("[Audio] Audio device not available: %s\n", SDL_GetError());
  }

  bool running = true;
  SDL_Event event;

  const uint64_t perf_freq = SDL_GetPerformanceFrequency();
  const double target_frame_cycles = (double)perf_freq / 60.09881389744051;
  uint64_t next_frame_time = SDL_GetPerformanceCounter();
  uint64_t perf_window_start = next_frame_time, prev_frame_start = 0;
  double sum_run = 0, sum_upload = 0, sum_present = 0, sum_wait = 0;
  double max_run = 0, max_present = 0, max_frame = 0, max_gap = 0;
  unsigned slow20 = 0, slow33 = 0, measured_frames = 0;
  int prev_cb_count = 0, prev_cb_us = 0, prev_cb_lock_us = 0, prev_cb_starved = 0;
  uint64_t prev_slices = 0, prev_dsp_slices = 0, prev_decomp_slices = 0;
  unsigned diag_frame_index = 0;

  printf("[Game] Engine initialized. Entering main interactive loop (60 FPS smooth)...\n");

  while (running) {
    uint64_t frame_start = s_perf_log ? SDL_GetPerformanceCounter() : 0;
    if (s_perf_log && prev_frame_start) {
      double gap = perf_ms(prev_frame_start, frame_start);
      if (gap > max_gap) max_gap = gap;
    }
    prev_frame_start = frame_start;
    while (SDL_PollEvent(&event)) {
      if (event.type == SDL_QUIT) {
        running = false;
      } else if (event.type == SDL_KEYDOWN) {
        if (event.key.keysym.sym == SDLK_ESCAPE) {
          running = false;
        } else if (event.key.keysym.sym == SDLK_F11 ||
                   (event.key.keysym.sym == SDLK_RETURN && (event.key.keysym.mod & KMOD_ALT))) {
          static bool s_fullscreen = false;
          s_fullscreen = !s_fullscreen;
          SDL_SetWindowFullscreen(window, s_fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
        }
      } else if (event.type == SDL_CONTROLLERDEVICEADDED) {
        int which = event.cdevice.which;
        if (!s_controllers[0]) {
          s_controllers[0] = SDL_GameControllerOpen(which);
          if (s_controllers[0]) {
            printf("[Input] Gamepad 1 connected: %s\n", SDL_GameControllerName(s_controllers[0]));
          }
        } else if (!s_controllers[1]) {
          s_controllers[1] = SDL_GameControllerOpen(which);
          if (s_controllers[1]) {
            printf("[Input] Gamepad 2 connected: %s\n", SDL_GameControllerName(s_controllers[1]));
          }
        }
      } else if (event.type == SDL_CONTROLLERDEVICEREMOVED) {
        SDL_JoystickID jid = event.cdevice.which;
        for (int i = 0; i < 2; i++) {
          if (s_controllers[i]) {
            SDL_Joystick *joy = SDL_GameControllerGetJoystick(s_controllers[i]);
            if (joy && SDL_JoystickInstanceID(joy) == jid) {
              printf("[Input] Gamepad %d disconnected\n", i + 1);
              SDL_GameControllerClose(s_controllers[i]);
              s_controllers[i] = NULL;
              break;
            }
          }
        }
      }
    }

    const uint8_t *key_states = SDL_GetKeyboardState(NULL);
    update_keyboard(key_states);
    if (s_diag_input_count) s_pad_state = diag_input_at(diag_frame_index);

    /* Run one hardware video frame (262 scanlines, AOT dispatched) */
    uint64_t run_start = s_perf_log ? SDL_GetPerformanceCounter() : 0;
    RtlRunFrame(s_pad_state);
    ++diag_frame_index;
    GameDrawPpuFrame();
    uint64_t run_end = s_perf_log ? SDL_GetPerformanceCounter() : 0;

    /* Upload active 256x224 viewport pixels to GPU texture */
    SDL_UpdateTexture(texture, NULL, fb_pixels, 256 * 4);
    SDL_RenderClear(renderer);
    SDL_RenderCopy(renderer, texture, NULL, NULL);
    uint64_t present_start = s_perf_log ? SDL_GetPerformanceCounter() : 0;
    SDL_RenderPresent(renderer);
    uint64_t present_end = s_perf_log ? SDL_GetPerformanceCounter() : 0;

    /* Regulate exact 60.0988 Hz frame pacing via high-precision accumulator */
    next_frame_time += (uint64_t)target_frame_cycles;
    uint64_t now_perf = SDL_GetPerformanceCounter();
    if (now_perf > next_frame_time + (uint64_t)(target_frame_cycles * 3)) {
      next_frame_time = now_perf;
    } else {
      while (now_perf < next_frame_time) {
        double rem_ms = (double)(next_frame_time - now_perf) * 1000.0 / (double)perf_freq;
        if (rem_ms > 2.0) {
          SDL_Delay((Uint32)(rem_ms - 1.0));
        } else {
#ifdef _WIN32
          Sleep(0);
#else
          SDL_Delay(0);
#endif
        }
        now_perf = SDL_GetPerformanceCounter();
      }
    }
    if (s_perf_log) {
      uint64_t frame_end = SDL_GetPerformanceCounter();
      double run_ms = perf_ms(run_start, run_end);
      double present_ms = perf_ms(present_start, present_end);
      double frame_ms = perf_ms(frame_start, frame_end);
      sum_run += run_ms;
      sum_upload += perf_ms(run_end, present_start);
      sum_present += present_ms;
      sum_wait += perf_ms(present_end, frame_end);
      if (run_ms > max_run) max_run = run_ms;
      if (present_ms > max_present) max_present = present_ms;
      if (frame_ms > max_frame) max_frame = frame_ms;
      slow20 += frame_ms > 20.0;
      slow33 += frame_ms > 33.0;
      ++measured_frames;
      if (measured_frames == 60 || diag_frames == 1) {
        int cb_count = SDL_AtomicGet(&s_cb_count);
        int cb_us = SDL_AtomicGet(&s_cb_time_us);
        int cb_lock_us = SDL_AtomicGet(&s_cb_lock_us);
        int cb_starved = SDL_AtomicGet(&s_cb_starved);
        uint64_t slices = smk_total_slices;
        uint64_t dsp_slices = smk_dsp_slices;
        uint64_t decomp_slices = smk_decomp_slices;
        double elapsed = perf_ms(perf_window_start, frame_end);
        fprintf(s_perf_log,
          "[perf] frames=%u fps=%.2f run=%.3fms upload=%.3fms present=%.3fms wait=%.3fms max_run=%.3fms max_present=%.3fms max_frame=%.3fms max_gap=%.3fms slow20=%u slow33=%u main_lock=%.3fms/%u max=%.3fms audio_cb=%d cb_time=%.3fms cb_lock=%.3fms prefill=%d slices=%llu dsp_slices=%llu decomp_slices=%llu mode=%u state=%u\n",
          measured_frames, elapsed > 0 ? measured_frames * 1000.0 / elapsed : 0,
          sum_run / measured_frames, sum_upload / measured_frames,
          sum_present / measured_frames, sum_wait / measured_frames,
          max_run, max_present, max_frame, max_gap, slow20, slow33,
          s_main_lock_wait_ms, s_main_lock_calls, s_main_lock_max_ms,
          cb_count - prev_cb_count, (cb_us - prev_cb_us) / 1000.0,
          (cb_lock_us - prev_cb_lock_us) / 1000.0,
          cb_starved - prev_cb_starved,
          (unsigned long long)(slices - prev_slices),
          (unsigned long long)(dsp_slices - prev_dsp_slices),
          (unsigned long long)(decomp_slices - prev_decomp_slices),
          g_ram[0x2e], g_ram[0x36]);
        fflush(s_perf_log);
        prev_cb_count = cb_count; prev_cb_us = cb_us;
        prev_cb_lock_us = cb_lock_us; prev_cb_starved = cb_starved;
        prev_slices = slices; prev_dsp_slices = dsp_slices;
        prev_decomp_slices = decomp_slices;
        measured_frames = slow20 = slow33 = 0;
        sum_run = sum_upload = sum_present = sum_wait = 0;
        max_run = max_present = max_frame = max_gap = 0;
        s_main_lock_wait_ms = s_main_lock_max_ms = 0;
        s_main_lock_calls = 0;
        perf_window_start = frame_end;
      }
    }
    if (diag_frames && --diag_frames == 0) running = false;
  }

  printf("[Game] Quitting cleanly...\n");
  for (int i = 0; i < 2; i++) {
    if (s_controllers[i]) {
      SDL_GameControllerClose(s_controllers[i]);
      s_controllers[i] = NULL;
    }
  }
  if (audio_dev > 0) {
    SDL_CloseAudioDevice(audio_dev);
  }
  if (s_apu_mutex) {
    SDL_DestroyMutex(s_apu_mutex);
    s_apu_mutex = NULL;
  }
  if (g_audiobuffer) {
    free(g_audiobuffer);
    g_audiobuffer = NULL;
  }
#ifdef _WIN32
  cleanup_timer_precision();
#endif
  SDL_DestroyTexture(texture);
  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  SDL_Quit();
  if (s_perf_log && s_perf_log != stdout) fclose(s_perf_log);

  return 0;
}
