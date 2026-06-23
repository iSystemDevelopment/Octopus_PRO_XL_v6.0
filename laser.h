/* ═════════════════════════════════════════════════════════════════════════════
 * Octopus PRO XL v6.1.00 — Laser Harp Groovebox
 * © 2026 DIODAC ELECTRONICS / iSystem. All Rights Reserved.
 *
 * PROPRIETARY AND CONFIDENTIAL. Unauthorized copying, distribution, modification,
 * or use of this software or firmware, in whole or in part, is strictly prohibited
 * without prior written permission from DIODAC ELECTRONICS.
 * ═════════════════════════════════════════════════════════════════════════════
 * laser.h — v6.1.00  MCPWM LASER KERNEL + BEAM DETECTION PRIMITIVES
 *
 * IRAM_ATTR inline helpers for laser_sweep_task (Core 1).  Implementation of
 * setupMCPWM, initLaserSPI, tickAnimation, laser_sweep_task → laser.cpp.
 *
 * Carrier/dwell from globals.h DBEAM_CARRIER_SEL table (default 9652 Hz,
 * 10 PWM cycles ≈ 1036 µs dwell).  laserOffAndSync() resets PWM phase after
 * every off to avoid partial-pulse flicker.  Beam colour: scale RGB + preset
 * amp-envelope white blend (harp modes) or LASER SHOW v2 projector engine.
 *
 * Scan cycle per string: galvoMoveDark → laser ON → comparator latch → dwell →
 * laserOffAndSync → advance galvo (no light during galvo slew).
 * ═════════════════════════════════════════════════════════════════════════════ */
#pragma once
#ifndef LASER_H
#define LASER_H

#include <algorithm>
#include <atomic>
#include <cmath>
#include <soc/soc.h>
#include <soc/gpio_reg.h>
#include "driver/gpio.h"
#include "globals.h"
#include "interface.h"  /* fastWrite(), fastRead() — bare-metal GPIO */
#include "patches.h"    /* SCALES[], scaleWhiteLevel[], scaleR/G/B[]   */
#include "dbeam.h"      /* GLOBAL_RAINBOW_R/G/B factory tables */

/* ═══════════════════════════════════════════════════════════════════════════
 * SECTION 1 — CONSTANTS
 * ═══════════════════════════════════════════════════════════════════════════ */

/* ── MCPWM carrier + dwell (from globals.h DBEAM_CARRIER_SEL) ───────────── */
/* Carrier + dwell come from the single-source sweep table in globals.h
 * (DBEAM_CARRIER_SEL → DBEAM_CARRIER_TICKS / DBEAM_DWELL_CYCLES).  Carrier =
 * 10 MHz / ticks; dwell cycles scale so dwell TIME stays ~constant. */
#define MCPWM_MAX_DUTY DBEAM_CARRIER_TICKS
static constexpr uint32_t LASER_PWM_FREQ_HZ   = 10000000u / MCPWM_MAX_DUTY;
static constexpr uint32_t BEAM_DWELL_CYCLES   = DBEAM_DWELL_CYCLES;
static constexpr uint32_t LASER_PWM_PERIOD_US = 1000000u / LASER_PWM_FREQ_HZ;
/* Dwell must span an INTEGER number of full PWM periods or the last cycle is cut
 * short → a dim partial pulse (visible as a faint flicker on each beam).  The
 * true period is 103.6 µs, so plain N×103 µs would fall short of N cycles.
 * Round the cycle→µs conversion UP (ceil) so the dwell always ≥ N whole cycles.
 *
 * BRIGHTNESS: each beam is a scanned dot, so its perceived brightness ∝ the
 * fraction of the scan cycle it spends lit.  Peak laser power is already at the
 * MCPWM ceiling (duty ≈ unity), so the only lever left for "solid dots" is a
 * longer dwell.  Raised 7→10 cycles (≈726 µs → ≈1036 µs): ~+16 % integrated
 * light per dot.  Per-beam refresh drops ~100 Hz → ~80 Hz, still above the
 * point-source flicker-fusion threshold.  Dial back toward 8 if any beam
 * shimmer appears on long throws. */
static constexpr uint32_t BEAM_DWELL_US =
    (BEAM_DWELL_CYCLES * 1000000u + LASER_PWM_FREQ_HZ - 1u) / LASER_PWM_FREQ_HZ; /* ~1036 µs @ sel 0 */
static constexpr uint32_t ANIM_DWELL_US_L =
    (4u * 1000000u + LASER_PWM_FREQ_HZ - 1u) / LASER_PWM_FREQ_HZ;                /* 415 µs */

/* ── Galvo geometry ──────────────────────────────────────────────────────── */
static constexpr uint16_t GALVO_PHYS_MIN = 500u;
static constexpr uint16_t GALVO_PHYS_MAX = 3500u;
static constexpr uint16_t GALVO_CENTER = 2048u;
static constexpr uint16_t BEAM_WINDOW_MIN = 560u; /* safety margins    */
static constexpr uint16_t BEAM_WINDOW_MAX = 3440u;

/* Fixed 8-beam fan for LASER SHOW — evenly spaced, independent of harp scale. */
static constexpr uint16_t SHOW_DAC_POS[8] = {
  500u, 929u, 1357u, 1786u, 2214u, 2643u, 3071u, 3500u
};

/* ── Opening animation ───────────────────────────────────────────────────── */
static constexpr uint32_t ANIM_STEP_INTERVAL_US = 120000u; /* 120 ms per fan step */

/* Core-1 yield during galvo MOVING phase — keeps MidiUsbRx / NvsWorker fed. */
static constexpr uint32_t LASER_BREATHE_MS = 8u;

/* ═══════════════════════════════════════════════════════════════════════════
 * SECTION 2 — SWEEP STATE  (all Core 1 only — no lock needed unless noted)
 * ═══════════════════════════════════════════════════════════════════════════ */
inline std::atomic<int> currentIndex{ 0 };
inline std::atomic<int> prevIndex{ 0 };
inline std::atomic<int> direction{ 1 };
inline SweepState sweepState = SweepState::MOVING;
inline uint32_t stateStartUs = 0;

/* Opening-animation state */
inline uint8_t beamRevealMask = 0x00u;  /* bit i = beam i lit during animation */
static uint16_t lastPos = GALVO_CENTER; /* last written DAC galvo value         */

/* PWM cache — prevents redundant MCPWM comparator writes */
static uint32_t cache_val_r = 0, cache_val_g = 0, cache_val_b = 0;
static int8_t cache_force_r = -1, cache_force_g = -1, cache_force_b = -1;

/* ═══════════════════════════════════════════════════════════════════════════
 * SECTION 3 — IRAM-SAFE FAST-PATH HELPERS
 * ═══════════════════════════════════════════════════════════════════════════ */

/* fastPinRead — gpio_num_t overload; delegates to interface.h fastRead() so the
 * bare-metal GPIO register access lives in exactly one place. [unified]        */
#ifndef FAST_PIN_READ_DEFINED
#define FAST_PIN_READ_DEFINED
static inline bool IRAM_ATTR fastPinRead(gpio_num_t pin) {
  return fastRead((uint8_t)pin);
}
#endif

/* safe_sinf — Bhaskara I approximation; IRAM-safe (no flash sinf()). */
#ifndef SAFE_SINF_DEFINED
#define SAFE_SINF_DEFINED
static inline float IRAM_ATTR safe_sinf(float x) {
  static constexpr float kPi = 3.14159265359f;
  static constexpr float k2Pi = 6.28318530718f;
  static constexpr float kInv2Pi = 0.15915494309f;
  x -= k2Pi * floorf(x * kInv2Pi); /* wrap to [0, 2π] */
  bool neg = (x > kPi);
  if (neg) x -= kPi;
  const float n = 4.0f * x * (kPi - x);
  return (neg ? -1.0f : 1.0f) * (n / (39.478417604f - n));
}
#endif

/* ═══════════════════════════════════════════════════════════════════════════
 * SECTION 4 — MCP4922 SPI DAC
 *
 * Hot path: no SPI.beginTransaction (initLaserSPI configures once at boot).
 * MCP4922: word A = galvo (0x3000|pos), word B = threshold (0xB000|thresh),
 * separate CS pulses, one LDAC strobe.
 *
 * g_lastDACWord dedup is invalidated after NVS-save dark park so the physical
 * threshold DAC is always refreshed when scanning resumes.
 * ═══════════════════════════════════════════════════════════════════════════ */
inline uint32_t g_lastDACWord = 0xFFFFFFFFu;
static inline void IRAM_ATTR invalidateDacCache() { g_lastDACWord = 0xFFFFFFFFu; }

static inline void IRAM_ATTR mcp4922Write(uint16_t galvoPos, uint16_t threshold) {
  const uint16_t galvoWord = 0x3000u | (galvoPos & 0x0FFFu);
  const uint16_t threshWord = 0xB000u | (threshold & 0x0FFFu);
  const uint32_t combined = ((uint32_t)galvoWord << 16) | threshWord;
  if (combined == g_lastDACWord) return; /* deduplication cache */
  g_lastDACWord = combined;

  /* Channel A — galvo position */
  fastWrite(PIN_DAC_CS, LOW);
  SPI.transfer16(galvoWord);
  fastWrite(PIN_DAC_CS, HIGH);

  /* Channel B — beam-detect threshold */
  fastWrite(PIN_DAC_CS, LOW);
  SPI.transfer16(threshWord);
  fastWrite(PIN_DAC_CS, HIGH);

  /* Simultaneous latch — LDAC must be low ≥ 100 ns (MCP4922 spec).
   * esp_rom_delay_us(1) = 1 µs, 10× margin, guaranteed in IRAM context. */
  fastWrite(PIN_DAC_LDAC, LOW);
  esp_rom_delay_us(1);
  fastWrite(PIN_DAC_LDAC, HIGH);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * SECTION 5 — MCPWM CHANNEL HELPERS
 * ═══════════════════════════════════════════════════════════════════════════ */
static inline void IRAM_ATTR update_mcpwm_channel(
  mcpwm_gen_handle_t gen, mcpwm_cmpr_handle_t cmpr,
  uint32_t val, uint32_t& cval, int8_t& cforce) {
  if (!gen || !cmpr) return;
  const int8_t ft = (val == 0u) ? 0 : -1;
  if (ft != cforce) {
    mcpwm_generator_set_force_level(gen, ft, true);
    cforce = ft;
  }
  if (ft == -1 && val != cval) {
    mcpwm_comparator_set_compare_value(cmpr, val);
    cval = val;
  }
}

static inline void IRAM_ATTR laserOff() {
  update_mcpwm_channel(laser_gen_r, laser_cmpr_r, 0u, cache_val_r, cache_force_r);
  update_mcpwm_channel(laser_gen_g, laser_cmpr_g, 0u, cache_val_g, cache_force_g);
  update_mcpwm_channel(laser_gen_b, laser_cmpr_b, 0u, cache_val_b, cache_force_b);
}

/* laserOffAndSync — off + PWM phase reset so the next fire starts at cycle 0. */
static inline void IRAM_ATTR laserOffAndSync() {
  laserOff();
  if (laser_soft_sync) mcpwm_soft_sync_activate(laser_soft_sync);
}

static inline void IRAM_ATTR resetLaserPhase() {
  if (laser_soft_sync) mcpwm_soft_sync_activate(laser_soft_sync);
}

static inline void IRAM_ATTR laserRGB(uint8_t r, uint8_t g, uint8_t b, int si) {
  if (si < 0 || si >= MAX_STRINGS) {
    laserOff();
    return;
  }
  const uint32_t duty = stringDuty[si & 7].load(std::memory_order_relaxed);
  const uint32_t dnom = 255u * DUTY_UNITY;
  const uint32_t maxD = MCPWM_MAX_DUTY;
  update_mcpwm_channel(laser_gen_r, laser_cmpr_r,
                       std::min(maxD, ((uint32_t)r * duty * maxD) / dnom), cache_val_r, cache_force_r);
  update_mcpwm_channel(laser_gen_g, laser_cmpr_g,
                       std::min(maxD, ((uint32_t)g * duty * maxD) / dnom), cache_val_g, cache_force_g);
  update_mcpwm_channel(laser_gen_b, laser_cmpr_b,
                       std::min(maxD, ((uint32_t)b * duty * maxD) / dnom), cache_val_b, cache_force_b);
}

static inline void IRAM_ATTR laserWhite(uint8_t lum) {
  const uint32_t dv = ((uint32_t)lum * MCPWM_MAX_DUTY) / 255u;
  update_mcpwm_channel(laser_gen_r, laser_cmpr_r, dv, cache_val_r, cache_force_r);
  update_mcpwm_channel(laser_gen_g, laser_cmpr_g, dv, cache_val_g, cache_force_g);
  update_mcpwm_channel(laser_gen_b, laser_cmpr_b, dv, cache_val_b, cache_force_b);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * SECTION 6 — GALVO MOVEMENT
 * ═══════════════════════════════════════════════════════════════════════════ */

/* getGalvoSettleTime — adaptive settle: extra margin for large moves
 * (potential galvo overshoot on fast direction reversals).              */
static inline uint32_t IRAM_ATTR getGalvoSettleTime(uint16_t old_, uint16_t new_) {
  const int32_t d = std::abs((int32_t)new_ - (int32_t)old_);
  const uint32_t base = (uint32_t)(d * 0.30f) + SETTLE_MIN_US;
  const uint32_t extra = (d > 1200) ? 60u : (d > 600) ? 30u
                                                      : 0u;
  const uint32_t t = base + extra;
  return (t < SETTLE_MAX_US) ? t : SETTLE_MAX_US;
}

/* galvoMoveDark — laser off before DAC move; optional blocking settle. */
static inline void IRAM_ATTR galvoMoveDark(uint16_t pos, uint16_t thresh,
                                           bool blockingSettle = true) {
  laserOffAndSync();
  mcp4922Write(pos, thresh);
  if (blockingSettle) {
    esp_rom_delay_us(getGalvoSettleTime(lastPos, pos));
  }
  lastPos = pos;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * SECTION 7 — COLOUR HELPERS
 * ═══════════════════════════════════════════════════════════════════════════ */
static inline void IRAM_ATTR blitHueToRGB(float h, uint8_t& r, uint8_t& g, uint8_t& b) {
  h -= floorf(h);
  r = (uint8_t)(std::min(1.f, std::max(0.f, fabsf(h * 6.f - 3.f) - 1.f)) * 255.f);
  g = (uint8_t)(std::min(1.f, std::max(0.f, 2.f - fabsf(h * 6.f - 2.f))) * 255.f);
  b = (uint8_t)(std::min(1.f, std::max(0.f, 2.f - fabsf(h * 6.f - 4.f))) * 255.f);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * SECTION 8 — HUE ADSR (LASER SHOW + legacy stringHueEnv[])
 *
 * Show mode: harpHueAdvance() in harp.cpp drives stringHueEnv[] per visit;
 * laserForString() reads level for projector brightness.  Harp play modes use
 * preset amp envelope for white↔scale colour blend (below).
 * ═══════════════════════════════════════════════════════════════════════════ */
static constexpr uint32_t HUE_LEVEL_MAX = (uint32_t)Q15_ONE * 2u - 1u;

/* ═══════════════════════════════════════════════════════════════════════════
 * SECTION 9 — BEAM COLOUR OUTPUT
 *
 * laserForString — scale RGB + amp-envelope white blend, or LASER SHOW v2 path.
 * ═══════════════════════════════════════════════════════════════════════════ */
static inline void IRAM_ATTR laserForString(int idx) {
  if (idx < 0 || idx >= MAX_STRINGS) {
    laserOffAndSync();
    return;
  }
  const int si = harpScaleIndex.load(std::memory_order_relaxed) & (NUM_SCALES - 1);

  /* ── Base scale colour ───────────────────────────────────────────────── */
  uint8_t r = 0, g = 0, b = 0;

  /* LASER SHOW v2: 8-beam projector — HUE from base (+ MIDI→Hue note offset),
   * brightness from anim mode + melody ADSR + drum flash.  Skips harp play-mode
   * shading while active (hand detect gated in laser.cpp). */
  if (laserShowMode.load(std::memory_order_relaxed)) {
    const int      bm    = idx & 7;   /* beam index (avoid shadowing r/g/b) */
    const uint32_t nowUs = (uint32_t)esp_timer_get_time();

    /* ── HUE ── */
    float hue = laserBaseHue.load(std::memory_order_relaxed);
    if (midiHueControl.load(std::memory_order_relaxed)) {
      const int   note     = (int)g_showBeamNote[bm];
      const float noteNorm = std::min(1.0f, std::max(0.0f,
                               (float)(note - 36) * (1.0f / 48.0f))); /* C2..C6 */
      hue += noteNorm;             /* note sweeps the wheel; base hue rotates it */
      hue -= (float)(int)hue;      /* wrap to 0..1 (hue ≥ 0 here) */
    }
    uint8_t cr, cg, cb;
    blitHueToRGB(hue, cr, cg, cb);

    /* ── Per-beam melody ADSR brightness ── */
    const uint32_t lev  = stringHueEnv[bm].level.load(std::memory_order_relaxed);
    const float    env  = (float)lev / (float)HUE_LEVEL_MAX;  /* 0..1 */

    /* ── Anim-mode brightness shaping ── */
    float lvl;
    switch (laserShowAnim.load(std::memory_order_relaxed)) {
      case LaserShowAnim::CHASE: {
        /* Bright dot tracks the sequencer step; neighbours form a soft trail. */
        const int head = (int)seqCurrentStep.load(std::memory_order_relaxed) & 7;
        int d = head - bm; if (d < 0) d = -d; if (d > 4) d = 8 - d; /* circular */
        const float chase = (d == 0) ? 1.0f : (d == 1) ? 0.35f : 0.04f;
        lvl = std::max(env, chase);
        break;
      }
      case LaserShowAnim::STROBE: {
        /* Whole fan gated by a fixed-rate square wave (~9 Hz, 50% duty). */
        const bool on = ((nowUs / 55000u) & 1u) == 0u;
        lvl = on ? std::max(env, 0.85f) : 0.0f;
        break;
      }
      case LaserShowAnim::WAVE: {
        /* Ambient sinusoid travelling across the fan — animated even with no
         * notes playing (π/4 phase per beam, ~0.6 Hz scroll). */
        const float s = safe_sinf((float)nowUs * 3.7699e-6f + (float)bm * 0.7854f);
        lvl = std::max(env, 0.15f + 0.85f * (0.5f + 0.5f * s));
        break;
      }
      case LaserShowAnim::PULSE:
      default:
        lvl = env;                 /* notes light their own beam */
        break;
    }

    /* ── Drum flash (global white add, linear decay from the last hit) ── */
    float flash = 0.0f;
    const float depth = laserDrumFlash.load(std::memory_order_relaxed);
    if (depth > 0.001f) {
      const uint32_t age = nowUs - g_showDrumFlashUs.load(std::memory_order_relaxed);
      if (age < SHOW_DRUM_FLASH_US)
        flash = depth * (1.0f - (float)age / (float)SHOW_DRUM_FLASH_US);
    }

    float rf = (float)cr * lvl, gf = (float)cg * lvl, bf = (float)cb * lvl;
    if (flash > 0.0f) {
      const float w = 255.0f * flash;
      rf = std::min(255.0f, rf + w);
      gf = std::min(255.0f, gf + w);
      bf = std::min(255.0f, bf + w);
    }
    laserRGB((uint8_t)rf, (uint8_t)gf, (uint8_t)bf, idx);
    return;
  }

  /* ── Base scale colour (POLY8 / STRINGS / SOLO play modes) ───────────────── */
  {
    const bool isRainbow = SCALES[si].isRainbow;
    r = isRainbow ? GLOBAL_RAINBOW_R[idx & 7] : scaleR[si];
    g = isRainbow ? GLOBAL_RAINBOW_G[idx & 7] : scaleG[si];
    b = isRainbow ? GLOBAL_RAINBOW_B[idx & 7] : scaleB[si];
  }

  /* ── HUE driven by the SOUND PRESET's amp envelope ───────────────────────
   * On beam-break the active string goes WHITE and returns to the scale colour as
   * the preset envelope falls — the amp ADSR (decay/sustain/release) IS the hue
   * timeline.  STRINGS' decay-driven fade emerges naturally from its plucky
   * presets; the synth adds the string vibration + micro-pitch (harp.cpp).
   *
   * Voice selection differs by mode: POLY8/STRINGS use the fixed per-string voice
   * (vi = string % 8); SOLO has one shared voice (HARP_SOLO_VOICE) that only the
   * sounding "king" beam reads — the other beams stay at idle scale colour. */
  const PlayMode pm = currentPlayMode.load(std::memory_order_relaxed);

  int vi = -1;
  if (pm == PlayMode::SOLO) {
    if ((int)(idx & 7) == harpSoloKing.load(std::memory_order_relaxed))
      vi = HARP_SOLO_VOICE;
  } else {
    vi = idx % HARP_POLYPHONY;
  }

  float envNorm = 0.0f;
  if (vi >= 0 && harpVoices[vi].active.load(std::memory_order_acquire)) {
    const uint32_t lvl = harpVoices[vi].env_level.load(std::memory_order_relaxed);
    if (lvl > 0u) envNorm = (float)lvl / 2147483647.0f;
  }
  (void)pm; /* mode only selects the voice above; the blend is identical for all */

  /* ── Smooth white <-> scale-colour blend ──────────────────────────────────
   * The white amount eases toward the preset amp envelope (envNorm) through a
   * one-pole smoother, so the beam fades CONTINUOUSLY between WHITE (note just
   * struck) and the scale colour (note decayed/released) — no colour stepping,
   * and it lands softly on the scale colour after the voice goes idle.  Identical
   * for all three play modes (POLY8 / STRINGS / SOLO); the preset's
   * decay/sustain/release set the timing.  Brightness is held constant — the beam
   * never dims to black during the transition (only its hue changes).
   *
   * Per-string smoother state is safe as a function-static: laser_sweep_task is
   * the sole caller (single Core-1 thread, one visit per string per sweep). */
  static float s_white[MAX_STRINGS] = { 0.0f };
  float& sw = s_white[idx & 7];
  const float wTarget = (envNorm > 1.0f) ? 1.0f : envNorm;
  /* Asymmetric one-pole: fast RISE so the beam snaps to white on a break, gentle
   * FALL so it eases smoothly back to the scale colour as the release decays
   * (the envelope sets the overall fade time; the smoother just removes steps). */
  const float k = (wTarget > sw) ? 0.60f : 0.22f;
  sw += (wTarget - sw) * k;
  if (sw < 0.002f) sw = 0.0f;
  if (sw > 0.0f) {
    r = (uint8_t)std::min(255.f, (float)r + (255.f - (float)r) * sw);
    g = (uint8_t)std::min(255.f, (float)g + (255.f - (float)g) * sw);
    b = (uint8_t)std::min(255.f, (float)b + (255.f - (float)b) * sw);
  }

  laserRGB(r, g, b, idx);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * SECTION 10 — BIDIRECTIONAL SCAN ADVANCE
 * ═══════════════════════════════════════════════════════════════════════════ */
static inline void IRAM_ATTR advanceIndex() {
  prevIndex.store(currentIndex.load(std::memory_order_relaxed), std::memory_order_relaxed);
  const int n = SCALES[harpScaleIndex.load(std::memory_order_relaxed) & (NUM_SCALES - 1)]
                  .numActiveStrings;
  if (n <= 1) {
    currentIndex.store(0, std::memory_order_relaxed);
    direction.store(1, std::memory_order_relaxed);
    return;
  }
  int li = currentIndex.load(std::memory_order_relaxed)
           + direction.load(std::memory_order_relaxed);
  if (li >= n - 1) {
    li = n - 1;
    direction.store(-1, std::memory_order_relaxed);
  } else if (li <= 0) {
    li = 0;
    direction.store(1, std::memory_order_relaxed);
  }
  currentIndex.store(li, std::memory_order_relaxed);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * SECTION 11 — DECLARATIONS (implemented in laser.cpp)
 * ═══════════════════════════════════════════════════════════════════════════ */
void setupMCPWM();
void initLaserSPI();
void initLaser();
void IRAM_ATTR tickAnimation();
void IRAM_ATTR laser_sweep_task(void* pvParameters);

#endif /* LASER_H */
