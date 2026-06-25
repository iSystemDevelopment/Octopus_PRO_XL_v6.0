/* ═════════════════════════════════════════════════════════════════════════════
 * Octopus PRO XL v6.1.00 — Laser Harp Groovebox
 * © 2026 DIODAC ELECTRONICS / iSystem. All Rights Reserved.
 *
 * PROPRIETARY AND CONFIDENTIAL. Unauthorized copying, distribution, modification,
 * or use of this software or firmware, in whole or in part, is strictly prohibited
 * without prior written permission from DIODAC ELECTRONICS.
 * ═════════════════════════════════════════════════════════════════════════════
 * globals.h — v6.1.00  PRODUCTION SSOT REGISTRY
 *
 * Single source of truth for shared runtime state: pin map, audio constants,
 * voice structs, std::atomic mirrors (Level 2), livePatch arrays (Level 3),
 * hwSeqData/hwSongData/hwMotionData, FreeRTOS handles, UI mode atomics.
 * Parameter writes go through patches.h apply*; persistence through settings.h.
 * Task handle comments mirror init_audio_system() in audio.cpp.
 * ═════════════════════════════════════════════════════════════════════════════ */
#pragma once
#ifndef GLOBALS_H
#define GLOBALS_H

/* Scale MIDI note tables: patches.h (SCALES_NOTES, SCALES, SCALES_DAC_POS). */

#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <Preferences.h>
#include <atomic>
#include <algorithm>
#include <cmath>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>
#include <ESP32Encoder.h>
/* USB.h / USBMIDI.h intentionally NOT included here — every TU pulls globals.h;
 * dragging the USB stack into laser/dbeam/audio TUs bloats ipc0 boot callbacks.
 * USB lives in Octopus_PRO_XL_v6.0.ino + midi.h only.                         */

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "soc/soc.h"
#include "soc/gpio_reg.h"
#include "driver/mcpwm_prelude.h"
#include "driver/i2s_std.h"

/* ═══════════════════════════════════════════════════════════════════════════
 * SECTION 1 — SYSTEM CONSTANTS
 * ═══════════════════════════════════════════════════════════════════════════ */

/* ── Polyphony & array dimensions ──────────────────────────────────────── */
static constexpr int MAX_STRINGS = 8;
static constexpr int HARP_POLYPHONY = 8;
/* SOLO play mode: one shared voice (HARP_SOLO_VOICE), last-note priority. */
static constexpr int HARP_SOLO_VOICE = 0;
static constexpr int SEQ_POLYPHONY = 8;
static constexpr int DRUM_POLYPHONY = 8;

/* ── Audio engine ──────────────────────────────────────────────────────── */
/* 44.1 kHz engine — SAMPLE_RATE and FX_SR must stay equal. */
static constexpr int SAMPLE_RATE = 44100;
static constexpr int FX_SR = 44100;
static constexpr size_t DMA_BUFFER_FRAMES = 512;

/* ── Preset / scale tables ─────────────────────────────────────────────── */
static constexpr int NUM_PATCHES = 256;
/* Named/App-visible presets.  Slots 128-255 are unnamed expansion fillers, so the
 * hardware patch browser caps at this count to match the OctopusApp dropdown.    */
static constexpr int NUM_NAMED_PRESETS = 128;
static constexpr int NUM_SCALES = 16;
static constexpr int MAX_SCALE_NOTES = 8;
static constexpr int PARAMS_PER_PRESET = 16;

/* ── DSP numeric constants ─────────────────────────────────────────────── */
static constexpr float EFFECT_INV32768 = 1.0f / 32768.0f;
static constexpr float MASTER_PITCH_MIN = 0.25f; /* 2 octaves down */
static constexpr float MASTER_PITCH_MAX = 4.0f;  /* 2 octaves up   */

/* Per-bus loudness trim before user volume knobs (tune by ear).  Drum engine has
 * lower intrinsic peak than harp/seq due to per-voice scaling in groovebox.h. */
static constexpr float MIX_TRIM_HARP = 1.00f;
static constexpr float MIX_TRIM_SEQ  = 1.35f;
static constexpr float MIX_TRIM_DRUM = 3.00f;
static constexpr float MIX_BUS_SUM   = 0.38f;
static constexpr float INV_16383_UI = 1.0f / 16383.0f;
static constexpr int32_t Q15_ONE = 32768;
static constexpr int32_t SVF_CUT_MAX = 31000; /* harp SVF: cut*hp promoted to int64
  * before >>15 (|hp| can reach ~81919 → product exceeds int32 max). See harp.md RISK-1. */

/* ═══════════════════════════════════════════════════════════════════════════
 * SHARED FIXED-POINT DSP PRIMITIVES
 *
 * Register-only helpers for seq/drum (groovebox.h), laser.h, effect.h.
 * Harp keeps private copies in harp.cpp; these are the seq/drum + FX paths.
 * ═══════════════════════════════════════════════════════════════════════════ */

/* SVF state clamp — must stay below SVF_CUT_MAX so the coefficient cannot drive
 * the filter state outside range in a single iteration. */
static constexpr int32_t SVF_SAT       = 24576; /* 75 % of int16_t full scale */
static constexpr int32_t SOFTCLIP_KNEE = 26000; /* transparent below, asymptotic above */

/* Per-engine transparent output soft-clip: bit-exact for |x| ≤ knee, asymptotic
 * toward ±full-scale above it (graceful dense-chord headroom, no wrap/crack). */
static inline int16_t IRAM_ATTR engine_soft_clip(int32_t x) {
  if (x > SOFTCLIP_KNEE) {
    const int32_t e = x - SOFTCLIP_KNEE;
    const int32_t span = 32767 - SOFTCLIP_KNEE;
    return (int16_t)(SOFTCLIP_KNEE + (int32_t)(((int64_t)span * e) / (e + span)));
  }
  if (x < -SOFTCLIP_KNEE) {
    const int32_t e = -x - SOFTCLIP_KNEE;
    const int32_t span = 32768 - SOFTCLIP_KNEE;
    return (int16_t)(-(SOFTCLIP_KNEE + (int32_t)(((int64_t)span * e) / (e + span))));
  }
  return (int16_t)x;
}

/* safe_sinf — 5th-order minimax sine, register-only (no Flash sinf() in the hot
 * path; Flash sinf can trigger IROM cache eviction during NVS writes). */
#ifndef SAFE_SINF_DEFINED
#define SAFE_SINF_DEFINED
static inline float IRAM_ATTR safe_sinf(float x) {
  x -= 6.28318531f * floorf(x * (1.0f / 6.28318531f));
  if (x > 3.14159265f) x -= 6.28318531f;
  const float x2 = x * x;
  return x * (1.0f + x2 * (-0.16666667f + x2 * (0.00833333f - x2 * 0.00019841f)));
}
#endif /* SAFE_SINF_DEFINED */

/* Lock-free PRNG — eliminates cross-core correlation artefacts. */
static inline int32_t IRAM_ATTR fast_noise() {
  static std::atomic<uint32_t> seed{ 0xADEAD888u };
  uint32_t old_s = seed.load(std::memory_order_relaxed), new_s;
  do { new_s = old_s * 1664525u + 1013904223u; }
  while (!seed.compare_exchange_weak(old_s, new_s, std::memory_order_relaxed));
  return (int32_t)(new_s >> 17) - 16384;
}

/* MIDI note → frequency. */
static inline float IRAM_ATTR midi_note_to_freq(uint8_t note) {
  return 440.0f * exp2f(((float)note - 69.0f) / 12.0f);
}

/* 256-sample wavetable linear interpolation. */
static inline int32_t IRAM_ATTR synth_interpolate_wavetable(const int16_t* tbl,
                                                            uint32_t phase) {
  const uint32_t idx = (phase >> 24) & 255u;
  const uint32_t frac = (phase >> 16) & 255u;
  const int32_t s1 = tbl[idx];
  return s1 + (((int32_t)(tbl[(idx + 1u) & 255u] - s1) * (int32_t)frac) >> 8);
}

/* SVF coefficient helpers. */
static inline int32_t IRAM_ATTR calc_svf_cutoff_hz(float norm_cutoff) {
  const float f = (norm_cutoff / 16383.0f) * 0.44f;
  return (int32_t)(2.0f * safe_sinf(3.14159265f * std::min(0.49f, f)) * 32768.0f);
}
static inline int32_t IRAM_ATTR calc_svf_damping(uint16_t res_val) {
  return (int32_t)((1.0f - ((float)res_val / 16383.0f * 0.95f)) * 32768.0f);
}

/* ── MCPWM / laser constants ───────────────────────────────────────────── */
static constexpr uint32_t MCPWM_TIMER_FREQ = 10000000;
static constexpr uint32_t MCPWM_PWM_FREQ = 25000;
static constexpr uint32_t MCPWM_TICKS = 400;
/* ── D-BEAM carrier sweep select ─────────────────────────────────────────────
 * Pick a modulation-carrier multiple for hardware/filter tuning experiments.
 * Base = 9 652.5 Hz (10 MHz / 1036 ticks) ≈ MFB band-pass centre (9 656 Hz).
 * Carrier = 10 MHz / ticks; dwell cycles scale with the carrier so the dwell
 * TIME (hence brightness + per-beam refresh) stays ~constant across presets.
 *
 *   SEL  mult   ticks   carrier      dwell cycles (≈1.04 ms dwell)
 *    0   1.00×  1036    9 652 Hz      10     ← MFB centre · DEFAULT / production
 *    1   1.50×   691   14 472 Hz      15
 *    2   1.75×   592   16 892 Hz      18
 *    3   2.00×   518   19 305 Hz      20
 *    4   2.25×   460   21 739 Hz      23
 *    5   2.50×   414   24 155 Hz      25
 *
 * Field note: 2× gave ~+50% D-BEAM range, ~−50% margin needed (margin 600 →
 * ~150 cm onset, 100% at ~30 cm), more linear/stable — but OFF the MFB centre,
 * so the real optimum likely sits ABOVE centre (re-tune the filter to match).
 * Flip DBEAM_CARRIER_SEL and re-flash.                                          */
#define DBEAM_CARRIER_SEL 0

#if   DBEAM_CARRIER_SEL == 0
  #define DBEAM_CARRIER_TICKS  1036u
  #define DBEAM_DWELL_CYCLES     10u
#elif DBEAM_CARRIER_SEL == 1
  #define DBEAM_CARRIER_TICKS   691u
  #define DBEAM_DWELL_CYCLES     15u
#elif DBEAM_CARRIER_SEL == 2
  #define DBEAM_CARRIER_TICKS   592u
  #define DBEAM_DWELL_CYCLES     18u
#elif DBEAM_CARRIER_SEL == 3
  #define DBEAM_CARRIER_TICKS   518u
  #define DBEAM_DWELL_CYCLES     20u
#elif DBEAM_CARRIER_SEL == 4
  #define DBEAM_CARRIER_TICKS   460u
  #define DBEAM_DWELL_CYCLES     23u
#elif DBEAM_CARRIER_SEL == 5
  #define DBEAM_CARRIER_TICKS   414u
  #define DBEAM_DWELL_CYCLES     25u
#else
  #error "DBEAM_CARRIER_SEL must be 0..5"
#endif

/* Carrier label for telemetry (= 10 MHz / ticks, matches LASER_PWM_FREQ_HZ). */
#define DBEAM_CARRIER_FREQ_HZ (10000000u / DBEAM_CARRIER_TICKS)
#define DBEAM_BESSEL_ORDER 2
#define DBEAM_FREQ_RATIO 2.59

/* ═══════════════════════════════════════════════════════════════════════════
 * LASER BEAM-SCAN TIMING  (the "gold balance" knobs)
 *
 * Per-beam scan cycle in laser_sweep_task (laser.cpp), one string:
 *   MOVING   : galvo slews DARK to next string, wait getGalvoSettleTime()
 *              → clamp(distance×0.30 + SETTLE_MIN_US, SETTLE_MIN_US, SETTLE_MAX_US)
 *   LASER_ON : beam ON, wait LASER_STAB_US (beam + LT1016 comparator settle),
 *              then arm the 74HC74 latch
 *   DWELLING : beam stays lit BEAM_DWELL_US, sample trigger, advance
 *
 * With the current values one beam ≈ settle(500–800) + stab(200) + dwell(1037)
 * ≈ 1.74–2.04 ms.  The scan is PING-PONG (advanceIndex), so every move is just
 * one string apart → uniform, small settle and no big wrap-around jump.
 *
 * ⚠ The REAL dwell knob is BEAM_DWELL_CYCLES in laser.h (→ BEAM_DWELL_US ≈
 *   1037 µs at 10 cycles), NOT a constant here.  (The old legacy DWELL_TIME_US /
 *   ANIM_DWELL_US / ANIM_LASER_STAB_US + getDwellTime() were dead code and have
 *   been removed.)  Animation dwell is ANIM_DWELL_US_L in laser.h. */
static constexpr uint32_t LASER_STAB_US = 200;   /* ACTIVE: beam+comparator settle */
/* SETTLE_MIN_US — minimum dark galvo-settle before the beam fires (every move).
 * THE knob for the "faint 10 mm line": too short → the beam lights while the
 * galvo is still creeping/ringing → a faint tail leading into the strong dot.
 * 500 µs lets the mirror fully stop on a normal one-string move.  Endpoint/
 * direction-reversal moves get an extra GALVO_REVERSE_EXTRA_US on top (laser.cpp)
 * because the mirror overshoots when it reverses — that was the source of the
 * last 2 faint beams.  Raise toward 600 if any straight-move tail remains. */
static constexpr uint32_t SETTLE_MIN_US = 600;   /* ACTIVE: min galvo settle  */
static constexpr uint32_t SETTLE_MAX_US = 800;   /* ACTIVE: settle cap (big moves) */
/* GALVO_REVERSE_EXTRA_US — extra dark-settle added to the move INTO and OUT OF a
 * scan turnaround (the 2 endpoint beams), where the galvo physically reverses
 * and rings/overshoots more than a same-direction step.  This is the targeted
 * fix for "6 beams clean, 2 with little faint".  Raise toward 400 if a faint
 * still clings to either end beam; lower to 0 to disable. */
static constexpr uint32_t GALVO_REVERSE_EXTRA_US = 250;

static constexpr uint32_t BEAM_GATE_HOLD_MAX = 500;
/* Anti-stuck fail-safe upper bound (beamStuckReleaseMs in HARP SETUP; 0 = off). */
static constexpr uint32_t BEAM_STUCK_RELEASE_MAX = 2000;
static constexpr uint32_t DUTY_UNITY = 65025;
static constexpr int CONFIRM_MIN = 1;
static constexpr int CONFIRM_MAX = 15;

/* ── Physical / pluck simulation ───────────────────────────────────────── */
static constexpr float PLUCK_TENSION_K = 0.010f;
static constexpr int32_t PLUCK_BRIGHT_AMT = 9000;
static constexpr uint32_t PLUCK_VIB_BASE_US = 100000;
static constexpr int32_t PLUCK_VIB_MIN_AMP = 70;
static constexpr int32_t PLUCK_VIB_MAX_AMP = 330;

/* ── STRINGS audio micro-pitch vibrato (the AUDIO counterpart of the galvo
 *    wobble) — mimics natural plucked-string vibration in the SOUND.  Depth is
 *    scaled by each voice's envelope so it blooms on the pluck and fades as the
 *    note rings out.  Cheap: one shared oscillator + one float mul per voice
 *    (folded into the existing LFO-pitch multiply), active only in STRINGS. */
static constexpr float HARP_STR_VIB_HZ    = 5.5f;   /* vibrato rate (Hz)        */
static constexpr float HARP_STR_VIB_DEPTH = 0.004f; /* ±0.4 % ≈ ±7 cents @ full */

/* ── SOLO staccato steal release (seconds).  When a new note steals the SOLO
 *    voice, the previous note is released THIS fast (instead of the patch's
 *    release time) so last-note priority reads as a crisp staccato, not a
 *    polyphonic ring-over.  Long enough to avoid a click, short enough to feel
 *    monophonic. */
static constexpr float HARP_SOLO_STACCATO_REL_SEC = 0.040f;

/* Default drum body wave = WT_SINE (index 8).  applyDrumWave() in patches.h. */
static constexpr uint8_t DRUM_DEFAULT_WAVE_IDX = 8;

/* ═══════════════════════════════════════════════════════════════════════════
 * SECTION 2 — HARDWARE PIN MAPPING  (1:1 identical — do not modify)
 * ═══════════════════════════════════════════════════════════════════════════ */
static constexpr gpio_num_t PIN_LASER_R = GPIO_NUM_4;
static constexpr gpio_num_t PIN_LASER_G = GPIO_NUM_5;
static constexpr gpio_num_t PIN_LASER_B = GPIO_NUM_6;

static constexpr gpio_num_t PIN_TRIGGER = GPIO_NUM_18;
static constexpr gpio_num_t PIN_PEAK_CLR = GPIO_NUM_1;
static constexpr gpio_num_t PIN_DAC_CS = GPIO_NUM_10;
static constexpr gpio_num_t PIN_DAC_LDAC = GPIO_NUM_3;

static constexpr gpio_num_t PIN_I2S_BCLK = GPIO_NUM_15;
static constexpr gpio_num_t PIN_I2S_WS = GPIO_NUM_16;
static constexpr gpio_num_t PIN_I2S_DOUT = GPIO_NUM_17;

static constexpr gpio_num_t PIN_ENC_A = GPIO_NUM_13;
static constexpr gpio_num_t PIN_ENC_B = GPIO_NUM_14;
static constexpr gpio_num_t PIN_ENC_BTN = GPIO_NUM_21;
static constexpr gpio_num_t PIN_BTN_SCALE = GPIO_NUM_41;
static constexpr gpio_num_t PIN_BTN_OC = GPIO_NUM_2;

/* GPIO 42/39 unused — USB-only MIDI (no DIN UART). */

static constexpr uint8_t I2C_ADDR_OLED = 0x3C;

/* ═══════════════════════════════════════════════════════════════════════════
 * SECTION 3 — ENUMERATIONS
 * ═══════════════════════════════════════════════════════════════════════════ */
enum class DashboardMode : uint8_t { SEQUENCER,
                                     HARP };
enum class HarpMode : uint8_t { CLOSED,
                                OPENING,
                                OPEN,
                                CLOSING };
enum class PlayMode : uint8_t { POLY8,
                                STRINGS,
                                SOLO };
enum class SweepState : uint8_t { MOVING,
                                  LASER_ON,
                                  DWELLING };
enum class MenuState : uint8_t { IDLE,
                                 MENU_L1,
                                 MENU_L2,
                                 MENU_L3 };

enum class DBEAMCurve : uint8_t {
  LINEAR,
  INVERTED,
  EXPONENTIAL,
  LOGARITHMIC,
  SIGMOID
};

enum class DbeamRoute : uint8_t {
  OFF = 0,
  MODULATION = 1, /* mod_depth → LFO depth (local DSP)            */
  VOLUME = 2,     /* bus volume direct (local DSP)               */
  CUTOFF = 3      /* svf_cutoff → SVF (local DSP)                */
};

/* D-BEAM target synth: Harp or Melody (seq). */
enum class DbeamTarget : uint8_t {
  HARP = 0,   /* harp engine  (dbeam_svf_cutoff / dbeam_mod_depth / mixHarpVol) */
  SEQ  = 1    /* melody synth (dbeam_seq_svf_cutoff / dbeam_seq_mod_depth / mixSeqVol) */
};

/* Explicit values: display.h::handleTelemetryPageEncoder cycles 1–7 */
enum class TelemetryView : uint8_t {
  OFF = 0,
  RAW_AC = 1,
  DC_LEVEL = 2,
  CAL_BASELINE = 3,
  CC_OUT_14BIT = 4,
  SIGNAL_SNR = 5,
  STACK_STATS = 6,  /* FreeRTOS stack HWM + heap — updated every 5 s */
  FOG_REJECT = 7    /* fog-branch telemetry lines */
};

/* Sampled by updateTaskStackStats() — read by OLED STACK_STATS view + Serial */
struct TaskStackStats {
  uint16_t audio   = 0;
  uint16_t midi    = 0;   /* MidiUsbRx task stack HWM */
  uint16_t dbeam   = 0;
  uint16_t seq     = 0;
  uint16_t control = 0;
  uint16_t display = 0;
  uint16_t laser   = 0;
  uint16_t nvs     = 0;
  uint16_t minFree = 0;   /* minimum bytes free across active tasks */
  uint32_t dramFree  = 0;
  uint32_t psramFree = 0;
};
inline TaskStackStats g_stackStats{};

enum class EnvState : uint8_t { ENV_IDLE = 0,
                                ENV_ATTACK,
                                ENV_DECAY,
                                ENV_SUSTAIN,
                                ENV_RELEASE };
enum class EngineType : uint8_t { HARP,
                                  SEQUENCER };
enum class SensorState : uint8_t { UNINIT,
                                   STABLE,
                                   ERROR };

enum class ParamSource : uint8_t {
  SRC_DEFAULT = 0,
  NVS = 1,
  MIDI = 2,
  UI = 3,
  FACTORY = 4 /* load from PROGMEM SOUND_BANK, not user bank */
};

/* Maps directly to livePatch[] array indices 0–15. PARAMS_PER_PRESET = 16. */
enum class SynthParam : int {
  P_WAVEFORM = 0,
  P_ATTACK = 1,
  P_DECAY = 2,
  P_SUSTAIN = 3,
  P_RELEASE = 4,
  P_CUTOFF = 5,
  P_RESONANCE = 6,
  P_NOISE = 7,
  P_DETUNE = 8,
  P_LFO_RATE = 9,
  P_LFO_DEPTH = 10,
  P_LFO_ROUTE = 11,
  P_OSC2_WAVE = 12,
  P_ENV_CUT = 13,
  P_SPARE1 = 14, /* reserved, not exposed in UI */
  P_SPARE2 = 15  /* reserved, not exposed in UI */
};

/* ─────────────────────────────────────────────────────────────────────────────
 * sanitizePatch — clamp a raw 16-word patch to engine-safe ranges IN PLACE.
 *
 * Single safety choke point for EVERY load path (factory SOUND_BANK rows, RAM
 * user/seq banks, App-pushed patches, and sequencer companion presets in
 * patterns.h).  A bad authored value or a corrupt App payload can therefore
 * never reach the SVF / oscillator.  Plain ternary clamps only — no <algorithm>
 * dependency, so globals.h stays self-contained and this is safe to call from
 * any TU.  Cheap enough to run on every recall (16 compares).
 *
 * Guards:
 *   - waveform / osc2 index → 0..24 (was silently %25, so 25 became SAW)
 *   - lfo route             → 0..7
 *   - attack / release      → floored so note-on/off cannot click
 *   - resonance             → capped below SVF self-oscillation
 *   - all 14-bit params     → 0..16383
 * ─────────────────────────────────────────────────────────────────────────────*/
static constexpr uint16_t PATCH_RES_MAX = 15500u; /* below SVF self-osc knee   */
static constexpr uint16_t PATCH_ATK_MIN = 24u;    /* ~3 ms — no hard click     */
static constexpr uint16_t PATCH_REL_MIN = 48u;    /* ~12 ms — no note-off pop  */
static inline void sanitizePatch(uint16_t* p) {
  if (!p) return;
  auto c14 = [](uint16_t v) -> uint16_t { return v > 16383u ? 16383u : v; };
  p[(int)SynthParam::P_WAVEFORM]  = (p[(int)SynthParam::P_WAVEFORM]  > 24u) ? 24u : p[(int)SynthParam::P_WAVEFORM];
  p[(int)SynthParam::P_OSC2_WAVE] = (p[(int)SynthParam::P_OSC2_WAVE] > 24u) ? 24u : p[(int)SynthParam::P_OSC2_WAVE];
  p[(int)SynthParam::P_LFO_ROUTE] = (p[(int)SynthParam::P_LFO_ROUTE] >  7u) ?  7u : p[(int)SynthParam::P_LFO_ROUTE];
  uint16_t atk = c14(p[(int)SynthParam::P_ATTACK]);
  p[(int)SynthParam::P_ATTACK]    = (atk < PATCH_ATK_MIN) ? PATCH_ATK_MIN : atk;
  p[(int)SynthParam::P_DECAY]     = c14(p[(int)SynthParam::P_DECAY]);
  p[(int)SynthParam::P_SUSTAIN]   = c14(p[(int)SynthParam::P_SUSTAIN]);
  uint16_t rel = c14(p[(int)SynthParam::P_RELEASE]);
  p[(int)SynthParam::P_RELEASE]   = (rel < PATCH_REL_MIN) ? PATCH_REL_MIN : rel;
  p[(int)SynthParam::P_CUTOFF]    = c14(p[(int)SynthParam::P_CUTOFF]);
  p[(int)SynthParam::P_RESONANCE] = (p[(int)SynthParam::P_RESONANCE] > PATCH_RES_MAX) ? PATCH_RES_MAX : p[(int)SynthParam::P_RESONANCE];
  p[(int)SynthParam::P_NOISE]     = c14(p[(int)SynthParam::P_NOISE]);
  p[(int)SynthParam::P_DETUNE]    = c14(p[(int)SynthParam::P_DETUNE]);
  p[(int)SynthParam::P_LFO_RATE]  = c14(p[(int)SynthParam::P_LFO_RATE]);
  p[(int)SynthParam::P_LFO_DEPTH] = c14(p[(int)SynthParam::P_LFO_DEPTH]);
  p[(int)SynthParam::P_ENV_CUT]   = c14(p[(int)SynthParam::P_ENV_CUT]);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * SECTION 4 — DATA STRUCTURES
 * ═══════════════════════════════════════════════════════════════════════════ */

struct ScaleDef {
  const int* notes;
  const uint16_t* dacPos;
  int numActiveStrings;
  bool isRainbow;
  const char* name;
};


/* ── Song mode sequencer data structures ──────────────────────────────────── */
/* SongStep: one entry in a song's chain.
 *   bank     0-15  — which pattern bank to play
 *   chain    0-3   — pattern row within bank
 *   repeats  1-15  — how many times to play before advancing (0=step inactive)
 * Wire encoding: CMD_SONG_STEP v14 = [step:4][bank:4][chain:2][repeats:4]  */
struct SongStep {
  uint8_t bank = 0;     /* 0-15 */
  uint8_t chain = 0;    /* 0-3  */
  uint8_t repeats = 1;  /* 0=inactive, 1-15=repeat count */
  uint8_t _pad = 0;
};

/* SongSlot: one of 16 song programs.
 * A song chains up to 16 SongStep entries.  When seqCurrentStep wraps to 0,
 * the song advances to the next step, loading the new bank/chain.
 * Default member initialisers give every slot a safe minimal song (one step,
 * bank 0) so a freshly-enabled song slot is valid without setup, and so
 * AllSettings{} (settings.h) carries these defaults — no separate hwSongData
 * fill is needed at boot (which previously clobbered NVS-loaded songs).      */
static constexpr int SONG_MAX_STEPS = 16;  /* steps[] capacity per song slot */
struct SongSlot {
  SongStep steps[16];
  uint8_t numSteps = 1;        /* 1-16: number of active steps  */
  uint8_t _pad[3] = { 0, 0, 0 };
};
struct DBeamHWConfig {
  int offsetAdc;
  int rangeAdc;
  float gain;
};

struct PitchBendMapping {
  std::atomic<bool>    enabled{true};
  std::atomic<uint8_t> upSemi{2};
  std::atomic<uint8_t> downSemi{2};
};

/* ── Voice: per-oscillator DSP state ───────────────────────────────────── */
/* active/env_level atomic (Core 0 audio + Core 1 MIDI); other fields under patchMux. */
struct Voice {
  std::atomic<bool> active{ false };
  std::atomic<uint32_t> env_level{ 0 };

  uint32_t phase = 0;
  uint32_t phase2 = 0;
  uint32_t step = 0;
  uint32_t step2_target = 0;
  uint32_t attack_step = 0;
  uint32_t decay_step = 0;
  uint32_t release_step = 0;
  uint32_t sustain_level = 0;
  uint16_t velocity = 0;
  int32_t svf_low = 0;
  int32_t svf_band = 0;
  int32_t osc_mix_q15 = 0;
  EnvState env_state = EnvState::ENV_IDLE;
  uint8_t type = 0;
  uint8_t type2 = 0;
  bool is_accented = false;
  /* SOLO: fast staccato release when a newer note steals this voice. */
  bool fast_release = false;
};

/* ── DrumType / DrumVoice ──────────────────────────────────────────────── */
/* DrumType maps to the 8 physical drum channels:
 *   Ch 0 = KICK,  Ch 1 = SNARE, Ch 2 = CLAP,  Ch 3 = HAT-C,
 *   Ch 4 = HAT-O, Ch 5 = TOM-H, Ch 6 = TOM-L, Ch 7 = PERC             */
enum class DrumType : uint8_t {
  DRUM_KICK = 0,
  DRUM_SNARE = 1,
  DRUM_CLAP = 2,
  DRUM_HAT = 3, /* closed and open hat share the same synthesis model */
  DRUM_TOM = 4,
  DRUM_PERC = 5
};

struct DrumVoice {
  std::atomic<bool> active{ false };

  DrumType type = DrumType::DRUM_KICK;

  /* 6 independent oscillators — named rather than arrays so the drum engine
   * (groovebox.cpp) can reference them without index-to-name mapping overhead. */
  uint32_t phase1 = 0, phase2 = 0, phase3 = 0;
  uint32_t phase4 = 0, phase5 = 0, phase6 = 0;
  uint32_t step1 = 0, step2 = 0, step3 = 0;
  uint32_t step4 = 0, step5 = 0, step6 = 0;

  /* Amplitude envelope */
  uint32_t env_amp = 0;
  uint32_t env_decay_step = 0;

  /* Pitch envelope — drum body pitch sweeps down after impact */
  uint32_t env_pitch = 0;
  uint32_t env_pitch_decay_step = 0;

  /* Per-voice synthesis parameters (set by fire_tuned_drum) */
  uint16_t velocity = 0;
  uint16_t vol_q15 = 0;  /* Q15-scaled volume from drumLivePatch[vol]   */
  int32_t noise_mix = 0; /* signed Q15 noise/tone blend                 */
  uint16_t tone_val = 0; /* SVF cutoff from tune param                  */
  uint8_t kit = 0;       /* DrumKitId snapshot — selects kick/hat character */
  /* Body-oscillator wavetable — set in fire_tuned_drum; nullptr for noise-only voices. */
  const int16_t* body_wave = nullptr;

  /* Two-state SVF filter per voice */
  int32_t filter_low = 0;
  int32_t filter_high = 0;
};

/* ── HueEnvelopeState: per-string laser colour envelope ────────────────── */
struct HueEnvelopeState {
  std::atomic<EnvState> state{ EnvState::ENV_IDLE };
  std::atomic<uint32_t> level{ 0 };
};

/* ── SynthGlobal: shared LFO + pitch-bend state per engine ─────────────── */
struct SynthGlobal {
  uint32_t lfo_phase = 0;
  uint32_t lfo_step = 0;
  uint16_t lfo_depth = 0;
  /* STRINGS play mode: micro-pitch vibrato oscillator (string wobble). */
  uint32_t vib_phase = 0;
  uint32_t vib_step = 0;
  std::atomic<uint32_t> pitch_bend_q16{ 65536u }; /* 1.0 in Q16 = no bend */
  /* ── Zipper-free filter param slew (per-buffer one-pole toward target) ──
   * cut_cur / res_cur hold the smoothed SVF coefficients carried across
   * buffers; -1 = uninitialised → snap to target on the first buffer so the
   * very first note is correct.  Glides a preset/knob change over ~8 buffers
   * (a few ms) instead of stepping in one buffer, which removes the click /
   * zipper on patch recall and rapid App cutoff sweeps.                       */
  int32_t cut_cur = -1;
  int32_t res_cur = -1;
};

/* ── MotionLane: one P-lock automation lane within a bank/chain slot ───── */
/* targetCmd = 255  → lane is empty.
 * steps[s]  = 0xFFFF → no automation for that step (pass-through).       */
struct MotionLane {
  uint8_t targetCmd;
  uint16_t steps[16];
};

/* ═══════════════════════════════════════════════════════════════════════════
 * SECTION 5 — SYNCHRONIZATION PRIMITIVES
 * (declared before any atomic or guarded data structure)
 * ═══════════════════════════════════════════════════════════════════════════ */
class FxChain; /* forward — full definition in effect.h */

inline SemaphoreHandle_t fxMutex = nullptr;                   /* FX chain load guard      */
inline SemaphoreHandle_t midiMutex = nullptr;                 /* MIDI TX / voice alloc    */
inline SemaphoreHandle_t i2cMutex = nullptr;                  /* I2C bus (OLED)           */
inline portMUX_TYPE patchMux = portMUX_INITIALIZER_UNLOCKED;  /* livePatch + hwSeqData */
inline portMUX_TYPE motionMux = portMUX_INITIALIZER_UNLOCKED; /* motion matrix        */

/* ═══════════════════════════════════════════════════════════════════════════
 * SECTION 6 — FREERTOS TASK HANDLES
 * Mirrors init_audio_system() in audio.cpp — do not edit priorities here alone.
 * ═══════════════════════════════════════════════════════════════════════════ */
inline TaskHandle_t hAudioTask = nullptr;   /* Core 0, prio 24, stack 16384 */
inline TaskHandle_t hDBeamTask = nullptr;   /* Core 0, prio 19, stack  6144 */
inline TaskHandle_t hDisplayTask = nullptr; /* Core 0, prio 18, stack 16384 */
inline TaskHandle_t hControlTask = nullptr; /* Core 0, prio 17, stack  8192 */
inline TaskHandle_t hLaserTask = nullptr;   /* Core 1, prio 24, stack  8192 */
inline TaskHandle_t hSeqBgTask = nullptr;   /* Core 1, prio 12, stack  4096 */
inline TaskHandle_t hMidiTask = nullptr;    /* Core 1, prio  6, stack  8192 */
inline TaskHandle_t hNvsTask = nullptr;     /* Core 1, prio  3, stack 16384 */

/* ═══════════════════════════════════════════════════════════════════════════
 * SECTION 7 — CORE RUNTIME MODE & UI STATE
 * ═══════════════════════════════════════════════════════════════════════════ */
inline std::atomic<PlayMode> currentPlayMode{ PlayMode::POLY8 };
inline std::atomic<HarpMode> harpMode{ HarpMode::CLOSED };
inline std::atomic<DashboardMode> activeDashboard{ DashboardMode::SEQUENCER };
inline std::atomic<MenuState> menuState{ MenuState::IDLE };
inline std::atomic<int> currentMenuL1{ 0 };
inline std::atomic<int> currentMenuL2{ 0 };

/* ── Hardware SONG editor cursor (L1 = 13 "SONG") ──────────────────────────
 * songUI_row = which song step (0..numSteps-1); songUI_box = which value box in
 * that row (0=BANK, 1=REPEATS).  SCALE advances the box cursor (wrapping to the
 * next row), the encoder edits the box under it.  Read by display drawSongEditor. */
inline std::atomic<int> songUI_row{ 0 };
inline std::atomic<int> songUI_box{ 0 };
static constexpr int SONG_UI_BOXES = 2;   /* BANK, REPEATS */
inline std::atomic<int> octaveShift[2]{ { 0 }, { 0 } }; /* [0]=harp [1]=seq */

/* ── Master audio ──────────────────────────────────────────────────────── */
inline std::atomic<float> masterVol{ 0.75f };
inline std::atomic<float> masterPitch{ 1.0f }; /* [MASTER_PITCH_MIN, MASTER_PITCH_MAX] */
inline std::atomic<float> drumPitchMult{ 0.60f }; /* drum-only; independent of M.TUNE */
inline std::atomic<float> masterEqLow{ 0.0f }; /* ±12 dB shelf */
inline std::atomic<float> masterEqHigh{ 0.0f };

/* ── Patch / scale indices ─────────────────────────────────────────────── */
inline std::atomic<int> harpScaleIndex{ 0 };
inline std::atomic<int> harpPatchIndex{ 0 };
inline std::atomic<int> seqPatchIndex{ 4 };

/* ── Patch version stamps (incremented by pattern loaders) ─────────────── */
inline std::atomic<uint8_t> seqLivePatchVersion{ 0 };
inline std::atomic<uint8_t> drumLivePatchVersion{ 0 };

/* Factory preset browser readouts (SEQ SETUP + display.cpp). */
#ifndef G_LAST_PRESET_DECLARED
#define G_LAST_PRESET_DECLARED
inline std::atomic<uint8_t> g_lastSynthPreset{ 0 };
inline std::atomic<uint8_t> g_lastDrumPreset{ 0 };
#endif

/* ═══════════════════════════════════════════════════════════════════════════
 * SECTION 8 — HARP SYNTH PARAMETER ATOMICS
 *
 * Three-level parameter model:
 *   Level 1  g_settings  (settings.h)  — NVS blob, load/save only
 *   Level 2  these atomics             — UI reads, encoder + patches.h writes
 *   Level 3  harpLivePatch[]  (§19)    — DSP reads (harp.cpp)
 *
 * applyHarpParam(idx, v14) in patches.h keeps Level 2 and Level 3 in sync.
 * Direct writes to these atomics MUST be followed by a livePatch update.
 * ═══════════════════════════════════════════════════════════════════════════ */
/* Pre-boot placeholders — settings_sync_to_ssot() overwrites from settings.h at boot. */
inline std::atomic<float> harpWaveform{ 0.0f };
inline std::atomic<float> harpAttack{ 0.008f };
inline std::atomic<float> harpDecay{ 0.25f };
inline std::atomic<float> harpSustain{ 0.60f };
inline std::atomic<float> harpRelease{ 0.18f };
inline std::atomic<float> harpCutoff{ 0.65f };
inline std::atomic<float> harpResonance{ 0.08f };
inline std::atomic<float> harpNoise{ 0.0f };
inline std::atomic<float> harpDetune{ 0.0f }; /* bipolar [-1,+1] */
inline std::atomic<float> harpLfoRate{ 0.15f };
inline std::atomic<float> harpLfoDepth{ 0.0f };
inline std::atomic<int32_t> harpLfoRoute{ 0 }; /* 0–7 routing matrix */
inline std::atomic<float> harpOsc2Wave{ 0.0f };
inline std::atomic<float> harpEnvCutAmount{ 0.0f };

/* ═══════════════════════════════════════════════════════════════════════════
 * SECTION 9 — SEQ SYNTH PARAMETER ATOMICS
 * Same three-level model as Section 8; applySeqParam() is canonical setter.
 * ═══════════════════════════════════════════════════════════════════════════ */
/* Pre-boot placeholders — overwritten from settings.h at boot. */
inline std::atomic<float> seqWaveform{ 1.0f };
inline std::atomic<float> seqAttack{ 0.005f };
inline std::atomic<float> seqDecay{ 0.22f };
inline std::atomic<float> seqSustain{ 0.28f };
inline std::atomic<float> seqRelease{ 0.08f };
inline std::atomic<float> seqCutoff{ 0.50f };
inline std::atomic<float> seqResonance{ 0.22f };
inline std::atomic<float> seqNoise{ 0.0f };
inline std::atomic<float> seqDetune{ 0.0f }; /* bipolar [-1,+1] */
inline std::atomic<float> seqLfoRate{ 0.30f };
inline std::atomic<float> seqLfoDepth{ 0.0f };
inline std::atomic<int32_t> seqLfoRoute{ 0 };
inline std::atomic<float> seqOsc2Wave{ 0.0f };
inline std::atomic<float> seqEnvCutAmount{ 0.20f };

/* ═══════════════════════════════════════════════════════════════════════════
 * SECTION 10 — DRUM PARAMETER ATOMICS
 *
 * NOTE: std::atomic<float> default-constructor leaves value UNSPECIFIED in
 * C++17.  initDrumParameters() MUST be called before the audio task starts.
 * Accessing these arrays before init is undefined behaviour.
 * ═══════════════════════════════════════════════════════════════════════════ */
inline std::atomic<float> drumTune[8];     /* 0.0–1.0 → frequency range per voice */
inline std::atomic<float> drumDecay[8];    /* 0.0–1.0 → decay time constant       */
inline std::atomic<float> drumVolume[8];   /* 0.0–1.0 → output gain               */
inline std::atomic<float> drumNoiseMix[8]; /* 0.0–1.0 → noise / tone blend        */

/* Per-channel body wave index (WAVE_TABLE_RAM).  Default WT_SINE (8). */
inline std::atomic<uint8_t> drumWaveIdx[8];

/* ── Drum kits ─────────────────────────────────────────────────────────────
 * A kit selects (a) a per-voice tuning preset loaded into drumTune/Decay/Vol/
 * Noise[8] + drumLivePatch (via applyDrumKit in patches.h) and (b) a synthesis
 * "character" (kick pitch-sweep depth + hat base frequency) applied per voice in
 * the drum engine (groovebox.h).  TR-909 is kit 0 and matches settings.h factory
 * defaults.  Clap/hat also use KIT_CLAP_* / KIT_HAT_* synthesis tables below.      */
enum class DrumKitId : uint8_t { TR909 = 0, TR808, TRAP, HOUSE, COUNT };
inline std::atomic<uint8_t> drumKit{ 0 };  /* persisted in DrumSettings.kit */

inline const char* const DRUM_KIT_NAMES[(int)DrumKitId::COUNT] = {
  "TR-909", "TR-808", "Trap", "House"
};

/* Per-kit tuning tables — channels: KCK SNR CLP HHC HHO TMH TML PRC (all 0..1).
 * Row 0 (TR-909) is identical to settings.h DrumSettings factory defaults so a
 * kit-0 reload is a no-op vs the legacy sound.                                   */
inline constexpr float DRUM_KIT_TUNE[(int)DrumKitId::COUNT][8] = {
  { 0.50f, 0.52f, 0.54f, 0.62f, 0.54f, 0.62f, 0.38f, 0.50f }, /* 909 snare body + bright hats */
  { 0.30f, 0.36f, 0.46f, 0.44f, 0.40f, 0.55f, 0.30f, 0.60f }, /* 808 deep snare */
  { 0.22f, 0.60f, 0.58f, 0.68f, 0.60f, 0.55f, 0.28f, 0.62f }, /* Trap tight crack snare */
  { 0.48f, 0.50f, 0.54f, 0.58f, 0.52f, 0.62f, 0.40f, 0.52f }, /* House club snare */
};
inline constexpr float DRUM_KIT_DECAY[(int)DrumKitId::COUNT][8] = {
  { 0.60f, 0.42f, 0.36f, 0.24f, 0.70f, 0.48f, 0.52f, 0.38f }, /* 909 tight snare */
  { 0.90f, 0.58f, 0.50f, 0.20f, 0.82f, 0.70f, 0.80f, 0.45f }, /* 808 long snare tail */
  { 0.98f, 0.26f, 0.30f, 0.14f, 0.48f, 0.55f, 0.85f, 0.40f }, /* Trap staccato */
  { 0.55f, 0.38f, 0.34f, 0.26f, 0.62f, 0.46f, 0.50f, 0.36f }, /* House */
};
inline constexpr float DRUM_KIT_VOL[(int)DrumKitId::COUNT][8] = {
  { 0.85f, 0.76f, 0.72f, 0.68f, 0.58f, 0.70f, 0.70f, 0.65f }, /* 909 */
  { 0.90f, 0.70f, 0.64f, 0.60f, 0.52f, 0.70f, 0.72f, 0.62f }, /* 808 */
  { 0.92f, 0.78f, 0.74f, 0.64f, 0.52f, 0.68f, 0.72f, 0.62f }, /* Trap */
  { 0.88f, 0.75f, 0.71f, 0.66f, 0.58f, 0.70f, 0.70f, 0.66f }, /* House */
};
inline constexpr float DRUM_KIT_NOISE[(int)DrumKitId::COUNT][8] = {
  { 0.02f, 0.58f, 0.88f, 0.87f, 0.85f, 0.08f, 0.06f, 0.15f }, /* 909 snare wires */
  { 0.00f, 0.40f, 0.94f, 0.96f, 0.94f, 0.04f, 0.03f, 0.12f }, /* 808 tonal snare */
  { 0.00f, 0.72f, 0.90f, 0.91f, 0.89f, 0.05f, 0.03f, 0.14f }, /* Trap noisy crack */
  { 0.03f, 0.54f, 0.86f, 0.86f, 0.84f, 0.08f, 0.06f, 0.16f }, /* House */
};

/* Factory drumPitchMult default — hats/clap/snare body normalize to this so default
 * tuning matches classic TR voicing while kick/toms/perc follow Drm Pitch directly. */
static constexpr float DRUM_PITCH_FACTORY = 0.60f;

/* Per-kit synthesis character (groovebox.h) — not stored in NVS. */
static constexpr uint32_t KIT_KICK_SWEEP[(int)DrumKitId::COUNT]  = { 14u, 6u, 4u, 12u };
static constexpr float    KIT_HAT_BASE_HZ[(int)DrumKitId::COUNT] = {
  4800.0f, 3600.0f, 5100.0f, 4600.0f   /* 909 bright · 808 dark · Trap crisp · House */
};
static constexpr int32_t  KIT_HAT_METAL_AMP[(int)DrumKitId::COUNT] = {
  5200, 3400, 5600, 4800   /* square partial level — lower on 808 (noisier hats) */
};
/* Clap triple-burst end samples @ 44.1 kHz (~9 ms spacing on 909). */
static constexpr uint16_t KIT_CLAP_BURST1[(int)DrumKitId::COUNT] = { 400, 460, 300, 400 };
static constexpr uint16_t KIT_CLAP_BURST2[(int)DrumKitId::COUNT] = { 800, 920, 600, 800 };
static constexpr uint16_t KIT_CLAP_BURST3[(int)DrumKitId::COUNT] = { 1200, 1380, 900, 1200 };
static constexpr int32_t  KIT_CLAP_FILTER_LP[(int)DrumKitId::COUNT] = { 14500, 11500, 15800, 14200 };
static constexpr int32_t  KIT_CLAP_FILTER_HP[(int)DrumKitId::COUNT] = {  9500,  6800, 10200,  9200 };
/* Snare — body osc + bandpassed rattle (909 wires / 808 tone / Trap crack / House punch). */
static constexpr float    KIT_SNARE_BODY_LO[(int)DrumKitId::COUNT] = { 168.f, 138.f, 195.f, 172.f };
static constexpr float    KIT_SNARE_BODY_HI[(int)DrumKitId::COUNT] = { 285.f, 225.f, 335.f, 295.f };
static constexpr uint8_t  KIT_SNARE_WAVE[(int)DrumKitId::COUNT]   = {  8, 23,  0,  7 };
/* WT_SINE · Meteor Tabla · Cosmic Saw · Aether String */
static constexpr int32_t  KIT_SNARE_SNAP_FC[(int)DrumKitId::COUNT]  = { 12200, 8200, 14800, 11600 };
static constexpr int32_t  KIT_SNARE_RATTLE_DELTA[(int)DrumKitId::COUNT] = { 5200, 3200, 6800, 4800 };
static constexpr uint32_t KIT_SNARE_PITCH_MUL[(int)DrumKitId::COUNT]   = { 3u, 2u, 5u, 3u };
static constexpr uint16_t KIT_SNARE_CLICK[(int)DrumKitId::COUNT]  = { 130, 90, 100, 120 };
static constexpr float    KIT_SNARE_DECAY_SCALE[(int)DrumKitId::COUNT] = { 0.72f, 0.92f, 0.52f, 0.70f };

/* ═══════════════════════════════════════════════════════════════════════════
 * SECTION 11 — SEQUENCER TRANSPORT & PATTERN STATE
 * ═══════════════════════════════════════════════════════════════════════════ */
inline std::atomic<bool> seqPlaying{ false };
inline std::atomic<bool> seqRecording{ false };
inline std::atomic<bool> isMotionPlayback{ false };
inline std::atomic<int32_t> seqBpm{ 120 };       /* 40–240 BPM           */
inline std::atomic<int32_t> seqTranspose{ 0 };   /* ±12 semitones        */
/* Melody arpeggiator — latch rows 0–7; output on seq voice 7. */
inline std::atomic<bool>    seqArpEnabled{ false };
inline std::atomic<uint8_t> seqArpPattern{ 0 };  /* arp::Pattern 0–7     */
inline std::atomic<uint8_t> seqArpRate{ 5 };     /* arp::Rate; default 1/16 */
inline std::atomic<uint8_t> seqArpGate{ 2 };     /* arp::GATE_PCT index 0–7 */

/* Harp arpeggiator — POLY8/SOLO; simplified 4-pattern / 4-rate UI. */
inline std::atomic<bool>    harpArpEnabled{ false };
inline std::atomic<uint8_t> harpArpPattern{ 0 };
inline std::atomic<uint8_t> harpArpRate{ 2 };    /* default 1/16 */
inline std::atomic<uint8_t> harpArpGate{ 1 };    /* default 50% */
/* Harp pitch-bend multiplier (~0.5–2.0, ±1 octave).  SequencerSettings.harp_pitch in NVS. */
inline std::atomic<float> harpPitchMult{ 1.0f };
inline std::atomic<uint8_t> seqActiveBank{ 0 };  /* 0–15                 */
inline std::atomic<uint8_t> seqActiveChain{ 0 }; /* 0–3 pattern row      */
inline std::atomic<uint8_t> seqLength{ 16 };     /* 1–64 steps           */
inline std::atomic<uint8_t> seqCurrentStep{ 0 }; /* 0-(seqLength-1)      */

/* ── Song mode runtime state (§11 extension) ──────────────────────────────── */
inline SongSlot hwSongData[16];                     /* 16 song programs      */
inline std::atomic<bool> songModeActive{ false };   /* pattern vs song mode  */
inline std::atomic<uint8_t> activeSongSlot{ 0 };    /* 0-15 current song     */
inline std::atomic<uint8_t> songCurrentStep{ 0 };   /* song chain position   */
inline std::atomic<uint8_t> songCurrentRepeat{ 0 }; /* repeats done so far   */

/* ═══════════════════════════════════════════════════════════════════════════
 * SECTION 12 — MIXER, MUTES & FX SENDS
 * ═══════════════════════════════════════════════════════════════════════════ */
inline std::atomic<float> mixHarpVol{ 0.8f };
inline std::atomic<float> mixSeqVol{ 0.8f };
inline std::atomic<float> mixDrumsVol{ 0.9f };
/* Equal-power pan −1..+1 per bus. */
inline std::atomic<float> mixHarpPan{ 0.f };
inline std::atomic<float> mixSeqPan{ 0.f };
inline std::atomic<float> mixDrumsPan{ 0.f };
inline std::atomic<bool> mixHarpMute{ false };
inline std::atomic<bool> mixSeqMute{ false };
inline std::atomic<bool> mixDrumsMute{ false };

/* ── Drum aux sends ────────────────────────────────────────────────────── */
inline std::atomic<float> drumRevSend{ 0.0f };
inline std::atomic<float> drumDlySend{ 0.0f };

/* ── Tube / DJ insert FX ───────────────────────────────────────────────── */
inline std::atomic<float> tbDrive{ 0.0f };
inline std::atomic<float> tbTone{ 0.5f };
inline std::atomic<float> tbMix{ 0.0f };
inline std::atomic<float> djFreq{ 1.0f };
inline std::atomic<float> djRes{ 0.1f };
inline std::atomic<float> djMix{ 0.0f };

/* ── FX slot preset indices (0–15) ────────────────────────────────────── */
inline std::atomic<int> masterFxIndex{ 0 };
inline std::atomic<int> harpFxIndex{ 0 };
inline std::atomic<int> harpFxIndexB{ 0 };
inline std::atomic<int> seqFxIndex{ 0 };
inline std::atomic<int> seqFxIndexB{ 0 };
inline std::atomic<int> drumFxIndexA{ 0 };
inline std::atomic<int> drumFxIndexB{ 0 };

/* ── Shared aux bus parameters (one global return — all engines sum here) ── */
/* CMD_H_FX_TIME / CMD_S_FX_TIME → masterAuxDlyTime  (0.0–1.5 s) — same bus
 * CMD_H_FX_SIZE / CMD_S_FX_SIZE → masterAuxRevSize  (0.0–0.95) — same bus
 * CMD_AUX_DLY_FB  → masterAuxDlyFb    (0.0–0.95)
 * CMD_AUX_REV_DMP → masterAuxRevDamp  (0.0–1.0)
 * CMD_AUX_SCENE_IDX → loadAuxScene(0–15)  |  CMD_LINK_AUX_PRESET → optional
 *   copy of insert preset aux fields when recalling FX-A (default OFF).      */
inline std::atomic<float> masterAuxDlyTime{ 0.35f };
inline std::atomic<float> masterAuxDlyFb{ 0.45f };
inline std::atomic<float> masterAuxRevSize{ 0.55f };
inline std::atomic<float> masterAuxRevDamp{ 0.35f };
/* Last selected AUX_SCENES[] row (UI only — live aux floats are SSOT in NVS). */
inline std::atomic<int> auxSceneIndex{ 0 };
/* When false (default), insert-A recall sets sends only; room params stay put. */
inline std::atomic<bool> linkAuxToInsertPreset{ false };

/* ═══════════════════════════════════════════════════════════════════════════
 * SECTION 13 — D-BEAM EXPRESSION STATE
 *
 * Data flow (full-scale 0..1 float, no MIDI quantisation):
 *   adc_dma_processing_task → updateDbeamExpression() → routeDbeamExpression()
 *   DbeamRoute::CUTOFF     → dbeam_svf_cutoff → harp.cpp SVF cutoff add
 *   DbeamRoute::MODULATION → dbeam_mod_depth  → harp.cpp LFO depth add
 *   DbeamRoute::VOLUME     → mixHarpVol (direct)
 * ═══════════════════════════════════════════════════════════════════════════ */
inline std::atomic<uint32_t> stringDuty[MAX_STRINGS]; /* MCPWM duty per string */
inline std::atomic<uint8_t> dbeamLastStringIdx{ 0 };
/* Laser-lit gate: true only while a string's beam is actually ON (LASER_ON +
 * DWELLING), false during the dark galvo move.  The ADC task commits a reading
 * to a string ONLY when this is true, so dark-period samples (laser off → no
 * reflection → ambient noise) never contaminate the per-string amplitude.
 * This is the sync that kills cross-string jitter without an inline DMA flush. */
inline std::atomic<bool> dbeamLit{ false };
inline std::atomic<uint16_t> dbeamAmplitude{ 0 }; /* max across all strings, for display */
inline std::atomic<bool> dbeamEnabled{ true };
inline std::atomic<DBEAMCurve> currentDbeamCurve{ DBEAMCurve::LINEAR };
inline std::atomic<DbeamRoute> currentDbeamRoute{ DbeamRoute::OFF };
inline std::atomic<DbeamTarget> currentDbeamTarget{ DbeamTarget::HARP };

/* Written by dbeam.cpp routeDbeamExpression(), read by harp.cpp HARP engine  */
inline std::atomic<int32_t> dbeam_svf_cutoff{ 0 }; /* [0, 32768] → SVF cutoff addend */
inline std::atomic<uint16_t> dbeam_mod_depth{ 0 }; /* [0, 16383] → LFO depth addend  */
/* Seq-synth D-BEAM addends (when target = Melody).  routeDbeamExpression zeros the other target. */
inline std::atomic<int32_t> dbeam_seq_svf_cutoff{ 0 };
inline std::atomic<uint16_t> dbeam_seq_mod_depth{ 0 };
/* VOLUME route baseline levels (inverted pedal — see applyDbeamRoute in patches.h). */
inline std::atomic<float> dbeamVolBaseHarp{ 1.0f };
inline std::atomic<float> dbeamVolBaseSeq { 1.0f };

/* ═══════════════════════════════════════════════════════════════════════════
 * SECTION 14 — MIDI ROUTING
 * ═══════════════════════════════════════════════════════════════════════════ */
inline std::atomic<uint8_t> wireHarpMidiChannel{ 1 }; /* 1–16                      */
inline std::atomic<uint8_t> wireSeqMidiChannel{ 2 };
inline std::atomic<uint8_t> wireDrumMidiChannel{ 10 }; /* GM drums standard channel */
inline std::atomic<uint32_t> lastWebSysexMs{ 0 };      /* heartbeat watchdog        */

/* Transport (play/stop/record/BPM) always owned by hardware; App is read-only. */

/* ═══════════════════════════════════════════════════════════════════════════
 * SECTION 15 — LASER SHOW STATE
 * ═══════════════════════════════════════════════════════════════════════════ */
inline std::atomic<bool> laserShowMode{ false };
inline std::atomic<bool> midiHueControl{ false };

/* LASER SHOW projector animation (8-beam fan; sequencer-driven colour/brightness). */
enum class LaserShowAnim : uint8_t { PULSE = 0, CHASE = 1, STROBE = 2, WAVE = 3 };
inline std::atomic<LaserShowAnim> laserShowAnim{ LaserShowAnim::PULSE };

/* Drum-flash depth (0..1).  Scales the white flash a drum hit injects into the
 * whole fan; 0 disables drum-driven flashing entirely.                         */
inline std::atomic<float> laserDrumFlash{ 0.5f };
/* Runtime: micros() timestamp (low 32 bits) of the most recent drum hit while
 * the show runs.  Written by the audio core (sequencer drum trigger), read by
 * the laser core which derives a linear-decay flash level from the age — no
 * per-frame decay bookkeeping, lock-free single-cell.                          */
inline std::atomic<uint32_t> g_showDrumFlashUs{ 0u };
static constexpr uint32_t SHOW_DRUM_FLASH_US = 140000u; /* flash fade length    */

/* HUE ADSR full-scale times (seconds) — the single source of truth shared by the
 * firmware menu, the SysEx scaling, and the App knobs so all three agree.  The
 * App maps its 0..16383 knobs onto exactly these ranges (BASE HUE is 0..1 → hue
 * wheel; SUS is 0..1 → 0..100%).                                               */
static constexpr float HUE_ATK_MAX_S = 2.0f;
static constexpr float HUE_DEC_MAX_S = 3.0f;
static constexpr float HUE_REL_MAX_S = 4.0f;
/* Idle screensaver: when the harp is CLOSED, draw a gentle roving dot instead
 * of going fully dark.  Runtime-only toggle (not persisted — the harp always
 * boots into the OPENING animation; closing is intentional and rare).  Defaults
 * ON so a deliberate close shows the animation; turn OFF in the LASER menu
 * (item 7) for a fully dark closed harp.                                       */
inline std::atomic<bool> laserScreensaver{ true };
inline std::atomic<float> laserBaseHue{ 0.0f };
inline std::atomic<float> hueAttack{ 0.01f };
inline std::atomic<float> hueDecay{ 0.1f };
inline std::atomic<float> hueSustain{ 1.0f };
inline std::atomic<float> hueRelease{ 0.2f };

inline HueEnvelopeState stringHueEnv[MAX_STRINGS];

/* Per-beam latched melody note for LASER SHOW hue (written by seq trigger). */
inline int8_t g_showBeamNote[MAX_STRINGS] = { 60, 60, 60, 60, 60, 60, 60, 60 };

/* ═══════════════════════════════════════════════════════════════════════════
 * SECTION 16 — DISPLAY & TELEMETRY STATE
 * ═══════════════════════════════════════════════════════════════════════════ */
inline std::atomic<bool> displayDirty{ true };

/* ── Reusable YES/NO confirm dialog ──────────────────────────────────────────
 * A small modal the hardware UI can raise before any destructive action.  Drawn
 * as a centred popup; the encoder rotation moves the selection (left = NO,
 * right = YES), a single ENC click confirms, ENC double cancels.  Defaults to NO
 * for safety.  Generic so SEQ SETUP Clear (and later RESET / SAVE) can share it:
 * raise with openConfirm(action), and add the action to confirmDispatch().     */
enum class ConfirmAction : uint8_t {
  NONE = 0,
  SEQ_CLEAR  = 1,   /* clear active pattern grid + reset both sounds to preset 0  */
  SAVE       = 2,   /* scoped NVS persist  — confirmArg = ResetScope (0..3)       */
  RESET      = 3,   /* scoped RAM wipe + persist + reboot — confirmArg = scope    */
  LOAD       = 4,   /* scoped reload from NVS (no reboot) — confirmArg = scope     */
  USR_SOUND_SAVE = 5, /* save live patch → user slot — confirmArg = eng<<6|idx     */
  USR_PAT_SAVE   = 6, /* save active pattern → user pat slot — confirmArg = idx    */
  /* Add a new action: extend this enum, drawConfirmDialog() copy, confirmDispatch(). */
};
inline std::atomic<bool>          confirmOpen{ false };
inline std::atomic<uint8_t>       confirmSel{ 0 };          /* 0 = NO, 1 = YES   */
inline std::atomic<ConfirmAction> confirmActionId{ ConfirmAction::NONE };
inline std::atomic<uint8_t>       confirmArg{ 0 };          /* per-action payload (e.g. scope) */

/* Raise the confirm dialog for a given action + payload (defaults cursor to NO). */
inline void openConfirm(ConfirmAction a, uint8_t arg = 0) {
  confirmActionId.store(a, std::memory_order_relaxed);
  confirmArg.store(arg, std::memory_order_relaxed);
  confirmSel.store(0, std::memory_order_relaxed);
  confirmOpen.store(true, std::memory_order_relaxed);
  displayDirty.store(true, std::memory_order_relaxed);
}

inline std::atomic<bool> audioTaskReady{ false };
/* False until setup() finishes patch sync + sequencer init.  All RT tasks spin on
 * this so Core 0/1 heavy work cannot start while setup() still runs USB/NVS/IPC. */
inline std::atomic<bool> g_systemReady{ false };
inline std::atomic<TelemetryView> currentScopeView{ TelemetryView::OFF };
inline std::atomic<uint32_t> lastEncoderTurnMs{ 0 };
inline std::atomic<bool> uiSyncPending{ false };  /* interface.cpp → display  */
inline std::atomic<bool> panicRequested{ false }; /* interface.cpp → .ino     */

inline uint8_t scopeHistory[128];
inline std::atomic<int> scopeWritePtr{ 0 }; /* laser task writes, display reads */

/* ═══════════════════════════════════════════════════════════════════════════
 * SECTION 17 — NVS SAVE & SYSTEM FLAGS
 * ═══════════════════════════════════════════════════════════════════════════ */
inline std::atomic<bool> g_saveRequest{ false };
inline std::atomic<bool> g_saveArmed{ false };
inline std::atomic<bool> g_loopParked{ false }; /* audio.h NVS save gate     */
/* Set TRUE by settings_save_task after every NVS write completes.
 * The laser task consumes it (exchange→false) and runs beam re-home +
 * detection blackout — independently of whether it caught g_saveArmed and parked.
 * NVS flash can glitch beam-detect even without a formal park; this guarantees
 * clean recovery after every save regardless of park timing. */
inline std::atomic<bool> g_beamRecover{ false };
/* Set on play-mode / scale change so the laser task clears per-string physical-hold
 * state (stringActive[], counters, latch) — a beam held across the change cannot
 * leave a stuck/silent "active" string. */
inline std::atomic<bool> g_beamClearReq{ false };
/* millis() until which the OLED shows a "SAVED" toast. Set by settings_save_task
 * on a successful NVS write so the user gets confirmation in any view (including
 * the SEQ MATRIX grid, which otherwise stays unchanged after a long-press save). */
inline std::atomic<uint32_t> g_saveFlashMs{ 0 };
/** millis() until which the OLED shows a "SAVE FAIL" toast after a failed commit. */
inline std::atomic<uint32_t> g_saveFailFlashMs{ 0 };

/** True while SETTINGS/MOTION reset is committing via NvsWorker (OLED RESET pill). */
inline std::atomic<bool> g_resetInProgress{ false };

/** NVS blob target for the in-flight save (ResetScope values 0–3). */
inline std::atomic<uint8_t> g_persistScope{ 0 };

/** Signalled by NvsWorker after each save completes (binary sem, created in setup). */
inline SemaphoreHandle_t g_saveDoneSem{ nullptr };

/** Set by requestScopedSave; NvsWorker calls esp_restart() after a successful save. */
inline std::atomic<bool> g_restartAfterSave{ false };

/** Result of the last NvsWorker save (SETTINGS/MOTION reset or SAVE). */
inline std::atomic<bool> g_saveLastOk{ true };

/** SysEx cmd echoed as ACK after a successful persist (156=SAVE, 169=RESET). */
inline std::atomic<uint8_t> g_persistAckCmd{ 156 };

/** True while the save handshake or NVS write is in flight — ControlPoll should
 * not mutate patchMux / hwSeqData during this window. */
static inline bool saveInProgress() {
  return g_saveRequest.load(std::memory_order_acquire) ||
         g_saveArmed.load(std::memory_order_acquire);
}

/** Force-clear a wedged save handshake (NvsWorker crash / TWDT kill). */
static inline void saveForceUnlock() {
  g_loopParked.store(false, std::memory_order_release);
  g_saveArmed.store(false, std::memory_order_release);
  g_saveRequest.store(false, std::memory_order_release);
  g_restartAfterSave.store(false, std::memory_order_release);
  if (g_saveDoneSem) xSemaphoreGive(g_saveDoneSem);
}

/** Wait for NvsWorker to finish (or force-unlock a stuck request). Reset uses this
 * so it never queues behind the SAVE handshake.                                    */
static inline void waitNvsWorkerIdle(uint32_t timeoutMs = 4000u) {
  const uint32_t deadline = millis() + timeoutMs;
  while ((g_saveRequest.load(std::memory_order_acquire) ||
          g_saveArmed.load(std::memory_order_acquire)) &&
         (int32_t)(deadline - millis()) > 0) {
    vTaskDelay(pdMS_TO_TICKS(5));
  }
  if (g_saveRequest.load(std::memory_order_acquire) ||
      g_saveArmed.load(std::memory_order_acquire))
    saveForceUnlock();
}

/** Clear a wedged save handshake (stale g_saveRequest without NvsWorker progress). */
static inline void recoverWedgedPersistFlags() {
  if (!g_saveRequest.load(std::memory_order_acquire)) return;
  if (g_saveArmed.load(std::memory_order_acquire)) return;
  static uint32_t reqSince = 0;
  if (!reqSince) reqSince = millis();
  if (millis() - reqSince > 2500u) {
    saveForceUnlock();
    reqSince = 0;
  }
}

/** Arm a scoped NVS save. scope: 0=FULL 1=BANKS_PATTERNS 2=MOTION 3=SETTINGS. */
static inline bool requestScopedSave(uint8_t scope) {
  recoverWedgedPersistFlags();
  if (g_saveRequest.load(std::memory_order_acquire) ||
      g_saveArmed.load(std::memory_order_acquire))
    return false;
  g_persistAckCmd.store(156, std::memory_order_relaxed);
  g_persistScope.store(scope & 3u, std::memory_order_release);
  /* Reboot after successful save — beam-detect hardware recovers cleanly on cold boot. */
  g_restartAfterSave.store(true, std::memory_order_release);
  g_saveRequest.store(true, std::memory_order_release);
  displayDirty.store(true, std::memory_order_relaxed);
  return true;
}

/** Full session save (all four NVS blobs). */
static inline void requestSessionSave() {
  requestScopedSave(0u);
}

/* Audio load monitoring (written by audio_synthesis_task, read by display) */
inline std::atomic<uint8_t> g_audio_load_pct{ 0 };

/* [CPU-TELEMETRY] Health counters folded into the CMD_CPU_LOAD device→App frame.
 *   g_seqExtDrops    — cumulative out-ring (STEP_SYNC/SONG_POS) drops under flood.
 *                      Saturating 6-bit field; producer is the audio task.
 *   g_motionLanesFull — sticky: a P-lock record was dropped because all 4 lanes
 *                      of the active pattern were already allocated. */
inline std::atomic<uint16_t> g_seqExtDrops{ 0 };
inline std::atomic<bool>     g_motionLanesFull{ false };

/* DSP quality / capacity controls */
/* SVF oversampling: ×1 default (constant filter character under load). */
inline std::atomic<int> g_svf_oversample{ 1 };            /* 1=working default, 2=idle luxury */
inline std::atomic<int> g_seq_voice_cap{ SEQ_POLYPHONY }; /* voice polyphony cap */
inline std::atomic<int> g_aux_mode{ 2 };                  /* 1=low-load 2=normal (audio.h) */
/* Osc2 (detune) layer — load-shedder may disable above ~85% CPU. */
inline std::atomic<bool> g_osc2_enable{ true };

/* Auto-save support (saves are manual by default — ENC long press) */
inline std::atomic<bool> settings_dirty{ false };
inline std::atomic<uint32_t> settings_last_change_ms{ 0 };

/* ═══════════════════════════════════════════════════════════════════════════
 * SECTION 18 — PER-SCALE CONFIGURATION ARRAYS
 * Non-atomic: only accessed under patchMux from interface.cpp.
 * Never touched from the audio task.
 * ═══════════════════════════════════════════════════════════════════════════ */
inline uint32_t beamGateHoldMs = 200; /* gate-hold window in ms */
/* Anti-stuck fail-safe: force-release a held note if the beam has not been solidly
 * broken for this many ms (counter-based release still acts first under normal play).
 * Larger = more tolerant of momentary dropouts; smaller = snappier recovery.
 * Set to 0 to disable the timeout. */
inline uint32_t beamStuckReleaseMs = 350;
inline bool hasOLED = true;

inline uint8_t scaleR[NUM_SCALES] = { 0, 255, 0, 255, 127, 0, 255, 200, 50, 255, 100, 255, 0, 255, 255, 255 };
inline uint8_t scaleG[NUM_SCALES] = { 255, 0, 255, 127, 0, 255, 255, 50, 200, 100, 255, 0, 255, 255, 255, 255 };
inline uint8_t scaleB[NUM_SCALES] = { 255, 255, 0, 0, 255, 127, 0, 255, 255, 255, 100, 100, 255, 255, 255, 255 };
inline uint8_t scaleWhiteLevel[NUM_SCALES] = { 32, 64, 32, 64, 32, 64, 32, 40, 45, 50, 32, 64, 32, 10, 0, 0 };
/* Per-scale beam comparator margin (DAC threshold seed, 0–2000).  Persisted in LaserSettings.margin. */
inline uint16_t scaleMargin[NUM_SCALES] = { 1100, 1100, 1100, 1100, 1100, 1100, 1100, 1100, 1100, 1100, 1100, 1100, 1100, 1100, 1100, 1100 };
inline uint8_t scaleTouchConfirm[NUM_SCALES] = { 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2 };
inline uint8_t scaleReleaseConfirm[NUM_SCALES] = { 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3 };

/* ── Per-string edge compensation — runtime-editable + NVS-persisted ───────
 * Outer strings (0,7) reflect less light into the photosensor (shallower beam
 * angle + smaller aperture), so they need a LOWER comparator threshold to be
 * detected reliably.  This is a per-string MULTIPLIER on the DAC threshold,
 * applied in computeHardwareDACThreshold():  final = scaleMargin × edge × colour.
 * Stored as integer percent (100 = ×1.00) so it is encoder- and NVS-friendly.
 *
 * Independent per-string table per scale: edgeComp[NUM_SCALES][8].
 * Every scale gets its own 8 values because trigger height depends on the beam's
 * COLOUR (a red string trips at a different height than a blue one), so even two
 * mono-colour scales can need different compensation — and the two rainbow scales
 * (14,15) vary colour string-by-string.  computeHardwareDACThreshold() reads the
 * row for the active scale; the HARP SETUP → "Edge Comp" editor edits one scale at
 * a time (OC scrolls scales, the name shows on the bottom row).  Persisted as
 * LaserSettings.edge_comp[NUM_SCALES][8]. */
static constexpr uint8_t EDGE_COMP_PCT_MIN = 40;   /* ×0.40 — most sensitive   */
static constexpr uint8_t EDGE_COMP_PCT_MAX = 150;  /* ×1.50 — least sensitive  */
/* Factory seed rows — scales 0..13 use the geometric mono valley; scales 14,15
 * (Rainbow Maj/Min) use the hand-tuned per-colour set 40/110/100/100/142/54/47/75
 * so every rainbow string trips at the same physical height.  ONE macro feeds the
 * factory constant, the runtime array, and the NVS struct default (settings.h). */
#define EDGE_COMP_DEFAULT_ROWS                                  \
  { 70, 80,100,100,100,100, 80, 70}, /* 01 Major          */    \
  { 70, 80,100,100,100,100, 80, 70}, /* 02 Minor          */    \
  { 70, 80,100,100,100,100, 80, 70}, /* 03 Pentatonic     */    \
  { 70, 80,100,100,100,100, 80, 70}, /* 04 Blues          */    \
  { 70, 80,100,100,100,100, 80, 70}, /* 05 Dorian         */    \
  { 70, 80,100,100,100,100, 80, 70}, /* 06 Phrygian       */    \
  { 70, 80,100,100,100,100, 80, 70}, /* 07 Lydian         */    \
  { 70, 80,100,100,100,100, 80, 70}, /* 08 Mixolydian     */    \
  { 70, 80,100,100,100,100, 80, 70}, /* 09 Locrian        */    \
  { 70, 80,100,100,100,100, 80, 70}, /* 10 Harmonic Min   */    \
  { 70, 80,100,100,100,100, 80, 70}, /* 11 Melodic Min    */    \
  { 70, 80,100,100,100,100, 80, 70}, /* 12 Spanish        */    \
  { 70, 80,100,100,100,100, 80, 70}, /* 13 Arabic         */    \
  { 70, 80,100,100,100,100, 80, 70}, /* 14 Chromatic      */    \
  { 40,110,100,100,142, 54, 47, 75}, /* 15 Rainbow Maj    */    \
  { 40,110,100,100,142, 54, 47, 75}  /* 16 Rainbow Min    */
static constexpr uint8_t EDGE_COMP_FACTORY[NUM_SCALES][8] = { EDGE_COMP_DEFAULT_ROWS };
inline uint8_t edgeComp[NUM_SCALES][8] = { EDGE_COMP_DEFAULT_ROWS };
/* Edge-comp editor page state (full-screen 8-bar editor, like telemetry AGC). */
inline std::atomic<bool> edgeEditOpen{ false };
inline std::atomic<int>  edgeEditSel  { 0 };   /* selected string 0..7            */
inline std::atomic<int>  edgeEditScale{ 0 };   /* scale under edit via OC (synced with harpScaleIndex) */

/* ═══════════════════════════════════════════════════════════════════════════
 * SECTION 19 — DSP BUFFER ALLOCATIONS
 * Patch arrays and voice pools — sized at compile time, allocated inline.
 * ═══════════════════════════════════════════════════════════════════════════ */

/* ── Live patch arrays (updated under patchMux) ────────────────────────── */
inline uint16_t harpLivePatch[PARAMS_PER_PRESET]; /* harp synth DSP input   */
inline uint16_t seqLivePatch[PARAMS_PER_PRESET];  /* seq synth DSP input    */
inline uint16_t drumLivePatch[32];                /* 8 drums × 4 params     */

/* ── User patch banks (RAM, persisted to NVS) ──────────────────────────── */
inline uint16_t userBank[NUM_PATCHES][PARAMS_PER_PRESET]; /* harp user bank   */
inline uint16_t seqBank[NUM_PATCHES][PARAMS_PER_PRESET];  /* seq user bank    */

/* ── User sound bank ───────────────────────────────────────────────────────
 * Reuses the EXISTING 256-slot banks: factory presets live in slots 0..127
 * (NUM_NAMED_PRESETS), and slots 128..191 are the 64 user "save your sound"
 * slots per engine (harp = userBank, seq = seqBank).  Saving copies the live
 * patch into the slot; loading recalls it exactly like a factory preset.  The
 * sparse "banks" NVS delta store already persists any non-factory slot, so no
 * new memory region is needed.  Names are App-editable (sparse "usrnames" blob);
 * an empty name falls back to the generic "USER NN" label.                    */
static constexpr int USER_SLOT_BASE = 128;            /* first user slot index */
static constexpr int NUM_USER_SLOTS = 64;             /* user slots per engine */
static constexpr int MAX_RECALL_PATCH_INDEX =
    USER_SLOT_BASE + NUM_USER_SLOTS - 1;                /* 191 — factory 0..127 + user 128..191 */
static_assert(USER_SLOT_BASE >= NUM_NAMED_PRESETS &&
              USER_SLOT_BASE + NUM_USER_SLOTS <= NUM_PATCHES,
              "user-slot window must sit above the named presets and inside the bank");
static_assert(MAX_RECALL_PATCH_INDEX < NUM_PATCHES,
              "recall window must fit inside NUM_PATCHES RAM bank");

/* Clamp patch recall (MIDI PC, CMD_H/S_PATCH, NVS restore) to authored slots only.
 * Indices 192..255 are zero-padded bank rows — never valid recall targets.       */
static inline int clampRecallPatchIndex(int idx) {
  return std::max(0, std::min(idx, MAX_RECALL_PATCH_INDEX));
}

/* App-editable names, [engine 0=harp,1=seq][slot 0..63][15 chars + NUL].
 * BSS-zeroed at boot; empty first byte → generic "USER NN".                   */
inline char g_userSlotName[2][NUM_USER_SLOTS][16] = {};
/* Per-engine cursor for the Save Slot / Load Slot menu screens. */
inline std::atomic<uint8_t> userSlotCursor[2]{};

/* Resolve a user slot's display name (custom if set, else "USER NN"). */
inline void userSlotName(uint8_t engine, uint8_t uidx, char* out, size_t n) {
  if (!out || n == 0) return;
  engine &= 1u; uidx %= (uint8_t)NUM_USER_SLOTS;
  const char* nm = g_userSlotName[engine][uidx];
  if (nm[0]) snprintf(out, n, "%s", nm);
  else       snprintf(out, n, "USER %02u", (unsigned)(uidx + 1));
}
/* Set/clear a user slot name (empty/NULL clears → generic label). */
inline void setUserSlotName(uint8_t engine, uint8_t uidx, const char* nm) {
  engine &= 1u; uidx %= (uint8_t)NUM_USER_SLOTS;
  if (!nm || !nm[0]) { g_userSlotName[engine][uidx][0] = '\0'; return; }
  snprintf(g_userSlotName[engine][uidx], 16, "%s", nm);
}

/* 64 named melody+drum pattern library slots, separate from the 16-bank session
 * grid.  Each slot stores synth rows 0–7, drum rows 8–15, companion seq-synth +
 * drum presets, and per-pattern transpose.  Persisted in sparse "usrpat" NVS;
 * names in "usrpatnames" (generic "PAT NN" when empty). App rename via sysex. */
static constexpr int NUM_USER_PAT_SLOTS = 64;

struct UserPatternSlot {
  uint8_t  flags;       /* bit0 = slot has saved data */
  int8_t   transpose;   /* −12..+12 semitones */
  uint64_t synthRows[8];
  uint64_t drumRows[8];
  uint16_t synthPreset[16];
  uint16_t drumPreset[32];
};

inline UserPatternSlot g_userPat[NUM_USER_PAT_SLOTS] = {};
inline char            g_userPatName[NUM_USER_PAT_SLOTS][16] = {};
inline std::atomic<uint8_t> userPatCursor{};

inline void userPatName(uint8_t uidx, char* out, size_t n) {
  if (!out || n == 0) return;
  uidx %= (uint8_t)NUM_USER_PAT_SLOTS;
  const char* nm = g_userPatName[uidx];
  if (nm[0]) snprintf(out, n, "%s", nm);
  else       snprintf(out, n, "PAT %02u", (unsigned)(uidx + 1));
}
inline void setUserPatName(uint8_t uidx, const char* nm) {
  uidx %= (uint8_t)NUM_USER_PAT_SLOTS;
  if (!nm || !nm[0]) { g_userPatName[uidx][0] = '\0'; return; }
  snprintf(g_userPatName[uidx], 16, "%s", nm);
}

/* ── Hardware sequencer grid ───────────────────────────────────────────── */
/* hwSeqData[bank][chain][track]
 *   bank  = 0–15  (16 program banks)
 *   chain = 0–3   (pattern row; UI pins active chain to 0)
 *   track = 0–15  each is a 64-bit step bitmask (bit N = step N active,
 *                 N = 0..63).  seqLength (1–64) sets how many steps loop.  */
inline uint64_t hwSeqData[16][4][16];

/* Melody transpose (−12..+12 semitones) stored per pattern slot, mirroring
 * hwSeqData[bank][chain].  Persistent SSOT (PatternsBlob); the active pattern's
 * value is mirrored into seqTranspose, which the audio step engine reads every
 * buffer.  Single-byte cells — UI/MIDI edits write the active cell directly. */
inline int8_t seqPatternTranspose[16][4] = {};

/* ── P-lock motion matrix ──────────────────────────────────────────────── */
/* hwMotionData[bank][chain][lane]
 *   bank  = 0–15 (mirrors hwSeqData bank)
 *   chain = 0–3  (mirrors hwSeqData chain)
 *   lane  = 0–3  (4 simultaneous automation lanes per slot)               */
inline MotionLane hwMotionData[16][4][4];

/* ── Voice pools ───────────────────────────────────────────────────────── */
inline Voice harpVoices[HARP_POLYPHONY];
inline Voice seqVoices[SEQ_POLYPHONY];
inline DrumVoice drums[DRUM_POLYPHONY];
inline SynthGlobal harp_synth_g;
inline SynthGlobal seq_synth_g;

/* Physical string active-tracking (non-atomic: only .ino loop() touches) */
inline bool stringActive[MAX_STRINGS] = { false };

/* SOLO play-mode "king": the one beam currently sounding on the shared mono
 * voice (-1 = none). Maintained by harp.cpp's SOLO held stack; read by the laser
 * render to dim non-king held beams so only the sounding note glows. */
inline std::atomic<int> harpSoloKing{ -1 };

/* ═══════════════════════════════════════════════════════════════════════════
 * SECTION 20 — HARDWARE PERIPHERAL HANDLES
 * ═══════════════════════════════════════════════════════════════════════════ */
inline mcpwm_timer_handle_t laser_timer = nullptr;
inline mcpwm_oper_handle_t laser_oper_r = nullptr,
                           laser_oper_g = nullptr,
                           laser_oper_b = nullptr;
inline mcpwm_cmpr_handle_t laser_cmpr_r = nullptr,
                           laser_cmpr_g = nullptr,
                           laser_cmpr_b = nullptr;
inline mcpwm_gen_handle_t laser_gen_r = nullptr,
                          laser_gen_g = nullptr,
                          laser_gen_b = nullptr;
inline mcpwm_sync_handle_t laser_soft_sync = nullptr;
inline i2s_chan_handle_t tx_handle = nullptr;

/* ═══════════════════════════════════════════════════════════════════════════
 * SECTION 21 — RUNTIME CONFIGURATION OBJECTS
 * Non-atomic: accessed under patchMux or at init time only.
 * ═══════════════════════════════════════════════════════════════════════════ */
inline DBeamHWConfig dbeamHWCfg = { 2048, 1000, 1.2f };

/* D-BEAM expression envelope follower (Branch B) — runtime-adjustable [SSOT].
 * ATTACK = fast rise to the nearest hand's reflection; RELEASE = slow decay that
 * rides over the no-hand dwells.  Read every lit dwell by the ADC task; edited
 * via D-BEAM menu (Env Atk / Env Rel) and persisted in NVS.  Ranges chosen so
 * the response stays usable across every expression curve.                      */
inline std::atomic<float> dbeamExprAttack { 0.50f };
inline std::atomic<float> dbeamExprRelease{ 0.008f };
static constexpr float DBEAM_EXPR_ATTACK_MIN  = 0.20f;
static constexpr float DBEAM_EXPR_ATTACK_MAX  = 0.50f;
static constexpr float DBEAM_EXPR_RELEASE_MIN = 0.007f;
static constexpr float DBEAM_EXPR_RELEASE_MAX = 0.020f;

inline PitchBendMapping pbMapping;  /* defaults: enabled=true, ±2 semitones */

/* OLED display object — defined in display.cpp; forward-declared here so
 * any TU that includes only globals.h can reference hasOLED. */
extern Adafruit_SH1106G display;

/* ═══════════════════════════════════════════════════════════════════════════
 * SECTION 22 — INITIALIZATION FUNCTIONS
 *
 * Call order in setup():
 *   1. initDrumParameters()  — before audio task
 *   2. initStringDuty()      — before laser task
 *   3. initHueEnvelopes()    — before laser task
 * ═══════════════════════════════════════════════════════════════════════════ */

/* initDrumParameters — sets drum atomics to production defaults.
 * Also initialises drumWaveIdx[8] to DRUM_DEFAULT_WAVE_IDX (WT_SINE). */
static inline void initDrumParameters() {
  /* Placeholders until settings_sync_to_ssot() loads DrumSettings from NVS. */
  /*                                KCK    SNR    CLP    HHC    HHO    TMH    TML    PRC */
  static const float TUNE_INIT[8]  = { 0.50f, 0.52f, 0.54f, 0.62f, 0.54f, 0.62f, 0.38f, 0.50f };
  static const float DECAY_INIT[8] = { 0.60f, 0.42f, 0.36f, 0.24f, 0.70f, 0.48f, 0.52f, 0.38f };
  static const float VOL_INIT[8]   = { 0.85f, 0.76f, 0.72f, 0.68f, 0.58f, 0.70f, 0.70f, 0.65f };
  static const float NOISE_INIT[8] = { 0.02f, 0.58f, 0.88f, 0.87f, 0.85f, 0.08f, 0.06f, 0.15f };
  for (int i = 0; i < 8; ++i) {
    drumTune[i].store(TUNE_INIT[i], std::memory_order_release);
    drumDecay[i].store(DECAY_INIT[i], std::memory_order_release);
    drumVolume[i].store(VOL_INIT[i], std::memory_order_release);
    drumNoiseMix[i].store(NOISE_INIT[i], std::memory_order_release);
    drumWaveIdx[i].store(DRUM_DEFAULT_WAVE_IDX, std::memory_order_release);
  }
  std::atomic_thread_fence(std::memory_order_acq_rel);
}

static inline void initStringDuty() {
  for (int i = 0; i < MAX_STRINGS; ++i)
    stringDuty[i].store(DUTY_UNITY, std::memory_order_relaxed);
  std::atomic_thread_fence(std::memory_order_release);
}

static inline void initHueEnvelopes() {
  for (int i = 0; i < MAX_STRINGS; ++i) {
    stringHueEnv[i].state.store(EnvState::ENV_IDLE, std::memory_order_relaxed);
    stringHueEnv[i].level.store(0, std::memory_order_relaxed);
  }
  std::atomic_thread_fence(std::memory_order_release);
}

static inline void settings_mark_dirty() {
  settings_dirty.store(true, std::memory_order_release);
  settings_last_change_ms.store((uint32_t)millis(), std::memory_order_release);
}

/* ── allNotesOff: CC120/123 and panic button handler ───────────────────── */
static inline void allNotesOff() {
  for (int v = 0; v < HARP_POLYPHONY; ++v)
    if (harpVoices[v].active.load(std::memory_order_relaxed))
      harpVoices[v].env_state = EnvState::ENV_RELEASE;
  for (int v = 0; v < SEQ_POLYPHONY; ++v)
    if (seqVoices[v].active.load(std::memory_order_relaxed))
      seqVoices[v].env_state = EnvState::ENV_RELEASE;
  for (int d = 0; d < DRUM_POLYPHONY; ++d) {
    drums[d].env_amp = 0;
    drums[d].active.store(false, std::memory_order_relaxed);
  }
  std::atomic_thread_fence(std::memory_order_release);
}

/* ── noteOffHarp: clears physical string-active flag only ──────────────── */
/* Does NOT trigger DSP release — call harpNoteOff()/harpReleaseVoice() (harp.h). */
static inline void noteOffHarp(int stringIdx) {
  if (stringIdx >= 0 && stringIdx < MAX_STRINGS)
    stringActive[stringIdx] = false;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * SECTION 23 — P-LOCK MOTION CAPTURE
 * ═══════════════════════════════════════════════════════════════════════════ */

/* clearMotionMatrix — reset every P-lock lane to its empty sentinel state
 * (targetCmd=255, all steps=0xFFFF).  Shared by the .ino boot init, the
 * CLR_PLOCKS-style clears, and the scoped factory reset in settings.h.       */
static inline void clearMotionMatrix() {
  portENTER_CRITICAL(&motionMux);
  for (auto& bankArr : hwMotionData)
    for (auto& chainArr : bankArr)
      for (auto& lane : chainArr) {
        lane.targetCmd = 255u;
        for (auto& step : lane.steps) step = 0xFFFFu;
      }
  portEXIT_CRITICAL(&motionMux);
}

/* recordMotionParam — call from handleSysexCommand for cmd < CMD_BPM only.
 * Guards against playback re-recording with isMotionPlayback flag.          */
static inline void recordMotionParam(uint8_t cmd, uint16_t val) {
  if (!seqRecording.load(std::memory_order_acquire)) return;
  if (!seqPlaying.load(std::memory_order_acquire)) return;
  if (isMotionPlayback.load(std::memory_order_acquire)) return;
  const uint8_t bank = seqActiveBank.load(std::memory_order_relaxed);
  const uint8_t chain = seqActiveChain.load(std::memory_order_relaxed);
  const uint8_t step = seqCurrentStep.load(std::memory_order_acquire) & 15u;
  portENTER_CRITICAL(&motionMux);
  int tgt = -1, emp = -1;
  for (int l = 0; l < 4; ++l) {
    if (hwMotionData[bank][chain][l].targetCmd == cmd) {
      tgt = l;
      break;
    }
    if (hwMotionData[bank][chain][l].targetCmd == 255u && emp < 0) emp = l;
  }
  if (tgt < 0) {
    if (emp < 0) {
      /* All 4 lanes already allocated — new automation is dropped (lanes are
       * never stolen).  Flag it so the App can surface a one-shot warning via
       * the CMD_CPU_LOAD telemetry frame. */
      g_motionLanesFull.store(true, std::memory_order_relaxed);
      portEXIT_CRITICAL(&motionMux);
      return;
    }
    tgt = emp;
    hwMotionData[bank][chain][tgt].targetCmd = cmd;
  }
  hwMotionData[bank][chain][tgt].steps[step] = val;
  portEXIT_CRITICAL(&motionMux);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * SECTION 24 — FORWARD DECLARATIONS
 * Functions defined in other translation units.
 * ═══════════════════════════════════════════════════════════════════════════ */
bool initDBeamSensor();
void setupToFactoryDefaults();
uint16_t IRAM_ATTR getHardwareDACThreshold(int stringIdx);
void IRAM_ATTR computeHardwareDACThreshold(int stringIdx, float measured_ac_amplitude_mv);

/* setupDMA_ADC() removed in v6.1 — setup() calls initDBeamSensor() directly. */

#endif /* GLOBALS_H */