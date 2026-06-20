/* ═════════════════════════════════════════════════════════════════════════════
 * Octopus PRO XL v6.0.00 — Laser Harp Groovebox
 * © 2026 DIODAC ELECTRONICS / iSystem. All Rights Reserved.
 *
 * PROPRIETARY AND CONFIDENTIAL. Unauthorized copying, distribution, modification,
 * or use of this software or firmware, in whole or in part, is strictly prohibited
 * without prior written permission from DIODAC ELECTRONICS.
 * ═════════════════════════════════════════════════════════════════════════════
 * effect.h — v6.0.00  CONSOLIDATED FX ENGINE  (3-instrument groovebox)
 *
 * Changes vs v5.1.01:
 *
 *  [H1] Drum aux send wired.
 *       fx.drumInsert.dly_send / rev_send are written every buffer by
 *       audio_synthesis_task (from drumRevSend / drumDlySend atomics) but
 *       were never read inside fx_process_multi_buf_safe.  The shared aux
 *       bus (reverb + delay) now correctly includes the drum contribution.
 *
 *  [H2] Tail gate includes drum sends.
 *       The 3-second reverb tail counter was gated only on harp+seq sends.
 *       Kick and snare reverb never ran.  Fixed: any non-zero send from
 *       harp, seq, or drum extends the tail.
 *
 *  [H3] loadInsert no longer stomps shared aux parameters.
 *       Loading any instrument's FX-A slot previously overwrote
 *       masterAuxDlyTime / masterAuxDlyFb / masterAuxRevSize / masterAuxRevDamp
 *       for ALL instruments simultaneously.  These aux parameters are now
 *       set only via sysex (CMD_D_REV, CMD_D_DLY) and NVS recall — not by
 *       preset loading.  The InsertFxPreset dl_time/rv_size fields are
 *       preserved in the struct for future per-chain aux (not applied here).
 *
 *  [H4] Scratch float buffers try PSRAM first.
 *       [P0] Mono pipeline → 3 × FX_BUF_SIZE × 4 bytes = 6 KB (was 6 buffers).
 *       On boards with 8 MB PSRAM these live in SPI RAM, freeing internal DRAM
 *       for FreeRTOS stacks.  Sequential access keeps cache-prefetch latency low.
 *       Falls back to internal DRAM if PSRAM is unavailable.
 *
 *  [P0] effect.h split into effect.h (decls + hot inline) + effect.cpp (tables,
 *       global fx, scratch bufs, cold primitive config, fx_init, and the IRAM
 *       hot path).  Dead stereo process()/R-channel state/R scratch removed.
 *
 * Retained from v5.1.01 (all fixes preserved):
 *  [A] SynthFX FLANGE and RING_MOD implementations.
 *  [B] Tail gate rev_send check (partial — fully corrected by [H2]).
 *  [C] FxBiquad denormal flush on v1/v2.
 *  [D] FxCombFilter denormal flush on filterState.
 *  [E] SharedAux graceful delay-line size fallback (65536→16384→4096).
 *
 * Architecture summary (signal flow, left to right):
 *
 *   [harp mono] ─→ harpInsert(A→B) ──────────────────────────────────┐
 *   [seq  mono] ─→ seqInsert (A→B) ──────────────────────────────────┤
 *   [drum mono] ─→ DrumFX ─→ drumInsert(A→B) ────────────────────────┤
 *                                                                      ↓
 *                    dly_send/rev_send ──→ SharedAux ──→ wetL / wetR ─┤
 *                                                                      ↓
 *                             sumL/sumR = Σ(dry) + aux_wet ──→ MasterFX ─→ out
 *
 * PSRAM usage (8 MB board):
 *   SynthFX delay lines:  6 chains × 2 ch × 1024 samples × 4 B =   48 KB PSRAM
 *   SharedAux delay line: 65536 samples × 4 B                   =  256 KB PSRAM
 *   FX scratch buffers:   6 × 512 × 4 B                         =   12 KB PSRAM
 *   FxCombFilter / allpass: hot-path, 4+2 × ~1200 s × 4 B       =   29 KB DRAM
 * ═════════════════════════════════════════════════════════════════════════════ */
#pragma once
#ifndef EFFECT_H
#define EFFECT_H

#include <Arduino.h>
#include <pgmspace.h>
#include <math.h>
#include <string.h>
#include <algorithm>
#include <atomic>
#include <esp_heap_caps.h>
#include "globals.h"

/* ── Scalar helpers ──────────────────────────────────────────────────────── */
static inline float fx_clampf(float v) {
  return v > 1.0f ? 1.0f : (v < -1.0f ? -1.0f : v);
}
static inline float fx_denormal(float v) {
  return (fabsf(v) < 1.0e-6f) ? 0.0f : v;
}
static inline float fx_tanh(float x) {
  if (x > 3.0f) return 1.0f;
  if (x < -3.0f) return -1.0f;
  const float x2 = x * x;
  return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

/* ── FX engine constants ─────────────────────────────────────────────────── */
static constexpr float FX_INV_SR = 1.0f / (float)FX_SR;
static constexpr float DENORMAL_FLOOR = 1.0e-6f;
static constexpr uint32_t INSERT_DELAY_LEN = 1024u; /* power-of-2, ~21 ms @ 48 kHz */
static constexpr size_t FX_BUF_SIZE = 512u;         /* must match DMA_BUFFER_FRAMES  */

/* Amplitude-aware aux tail gate (effect.cpp).  Keeps shared delay/reverb draining
 * until the mixed output actually falls quiet — avoids the old fixed 0.5–3 s
 * sample counter that cut long pad tails mid-decay.  Safety caps still bound CPU. */
/* [FX-BUG1] peakOut is tracked in the post-master FLOAT domain [0,1], so the
 * threshold must be float too. The old 20.0f (an int16-domain value) could never
 * be exceeded → the amplitude gate never held the tail and aux always cut at
 * FX_TAIL_QUIET_BUFFERS (~280 ms), chopping long pad/reverb tails. 20/32767 keeps
 * the intended ~-64 dBFS gate in the correct domain. */
static constexpr float FX_TAIL_PEAK_THRESH = 20.0f / 32767.0f; /* ~6.1e-4f, -64 dBFS */
static constexpr int   FX_TAIL_QUIET_BUFFERS = 24;  /* ~280 ms below thresh → aux off */
static constexpr int   FX_TAIL_MAX_BUFFERS_NORMAL =
    (int)((12.0f * (float)FX_SR) / (float)DMA_BUFFER_FRAMES); /* 12 s hard cap */
static constexpr int   FX_TAIL_MAX_BUFFERS_LOW =
    (int)((4.0f * (float)FX_SR) / (float)DMA_BUFFER_FRAMES);  /* 4 s when CPU shedding */

/* ─────────────────────────────────────────────────────────────────────────────
 * FxLFO — single-channel low-frequency oscillator
 * ─────────────────────────────────────────────────────────────────────────────*/
class FxLFO {
public:
  float phase = 0.0f;
  inline float tri(float hz) {
    phase += hz * FX_INV_SR;
    if (phase >= 1.0f) phase -= 1.0f;
    return (phase < 0.5f) ? (4.0f * phase - 1.0f) : (3.0f - 4.0f * phase);
  }
  /* safe_sinf: IRAM polynomial — avoids Flash-ROM sinf() cache eviction     */
  inline float sine(float hz) {
    phase += hz * FX_INV_SR;
    if (phase >= 1.0f) phase -= 1.0f;
    return safe_sinf(phase * 6.28318531f);
  }
};

/* ─────────────────────────────────────────────────────────────────────────────
 * FxBiquad — Transposed Direct Form II biquad filter
 * [C] Denormal flush on state variables prevents Xtensa FPU subnormal spikes.
 * ─────────────────────────────────────────────────────────────────────────────*/
class FxBiquad {
  float b0 = 1.f, b1 = 0.f, b2 = 0.f, a1 = 0.f, a2 = 0.f, v1 = 0.f, v2 = 0.f;
public:
  /* [P0] Coefficient math moved out-of-line to effect.cpp (cold path). */
  void setLowPass(float freq, float q);
  void setLowShelf(float freq, float gain_db);
  void setHighShelf(float freq, float gain_db);
  void setPeaking(float freq, float q, float gain_db);
  void reset() {
    v1 = v2 = 0.0f;
  }
  inline float process(float x) {
    float y = b0 * x + v1;
    v1 = fx_denormal(b1 * x - a1 * y + v2); /* [C] */
    v2 = fx_denormal(b2 * x - a2 * y);      /* [C] */
    return y;
  }
};

/* ─────────────────────────────────────────────────────────────────────────────
 * FxDelayLine — power-of-two circular buffer
 *
 * [P1] Memory tier is explicit at init():
 *   DRAM_FIRST  — insert chorus/flange (modulated readFrac every sample)
 *   PSRAM_FIRST — SharedAux long delay (sequential, large)
 * ─────────────────────────────────────────────────────────────────────────────*/
enum class FxDelayTier : uint8_t { DRAM_FIRST, PSRAM_FIRST };

class FxDelayLine {
public:
  float* buf = nullptr;
  uint32_t mask = 0;
  uint32_t pos = 0;

  /* [P0] Allocation moved out-of-line to effect.cpp (cold path). */
  bool init(uint32_t power_of_two, FxDelayTier tier = FxDelayTier::DRAM_FIRST);
  void clear();
  inline void write(float v) {
    if (buf) buf[pos & mask] = (fabsf(v) >= DENORMAL_FLOOR) ? v : 0.0f;
    ++pos;
  }
  inline float read(uint32_t tap) const {
    return buf ? buf[(pos - tap - 1u) & mask] : 0.0f;
  }
  /* [FX-OPT6] Single address computation per tap (was two read() calls that each
   * re-masked the same index).  Null-safe — kept as the public API. */
  inline float readFrac(float tap) const {
    if (!buf) return 0.0f;
    const uint32_t idx = (uint32_t)tap;
    const float frac   = tap - (float)idx;
    const float a = buf[(pos - idx - 1u) & mask];
    const float b = buf[(pos - idx - 2u) & mask];
    return a + (b - a) * frac;
  }
  /* [FX-OPT5] Null-check-free hot-path variants.  ONLY call once the FX chain is
   * fully initialised (fx.initialized): fx_process_multi_buf_safe bails out before
   * the sample loop when init is incomplete, so buf is guaranteed valid here. */
  inline void write_unsafe(float v) {
    buf[pos & mask] = (fabsf(v) >= DENORMAL_FLOOR) ? v : 0.0f;
    ++pos;
  }
  inline float read_unsafe(uint32_t tap) const {
    return buf[(pos - tap - 1u) & mask];
  }
  inline float readFrac_unsafe(float tap) const {
    const uint32_t idx = (uint32_t)tap;
    const float frac   = tap - (float)idx;
    const float a = buf[(pos - idx - 1u) & mask];
    const float b = buf[(pos - idx - 2u) & mask];
    return a + (b - a) * frac;
  }
};

/* ─────────────────────────────────────────────────────────────────────────────
 * FxCombFilter — Schroeder comb with one-pole lowpass damping
 * Allocated in DRAM — called every sample, cannot tolerate PSRAM cache misses.
 * [D] Denormal flush on filterState.
 * ─────────────────────────────────────────────────────────────────────────────*/
class FxCombFilter {
  float* buf = nullptr;
  uint32_t len = 0;
  uint32_t pos = 0;
  float filterState = 0.0f;
public:
  /* [P0] Allocation moved out-of-line to effect.cpp (cold path). */
  bool init(uint32_t samples);
  void clear();
  inline float process(float in, float feedback, float damp) {
    if (!buf) return in;
    const float out = buf[pos];
    filterState = fx_denormal(filterState + (out - filterState) * (1.0f - damp)); /* [D] */
    buf[pos] = std::min(3.0f, std::max(-3.0f, in + filterState * feedback));
    if (++pos >= len) pos = 0;
    return out;
  }
};

/* ─────────────────────────────────────────────────────────────────────────────
 * FxAllpassFilter — Schroeder allpass (g = 0.5), DRAM only
 * ─────────────────────────────────────────────────────────────────────────────*/
class FxAllpassFilter {
  float* buf = nullptr;
  uint32_t len = 0;
  uint32_t pos = 0;
public:
  /* [P0] Allocation moved out-of-line to effect.cpp (cold path). */
  bool init(uint32_t samples);
  void clear();
  inline float process(float in) {
    if (!buf) return in;
    const float d = buf[pos];
    const float v = in + 0.5f * d;
    buf[pos] = std::min(3.0f, std::max(-3.0f, v));
    if (++pos >= len) pos = 0;
    return d - 0.5f * v;
  }
};

/* ─────────────────────────────────────────────────────────────────────────────
 * Insert FX types and preset tables
 * ─────────────────────────────────────────────────────────────────────────────*/
enum class InsertMode : uint8_t { OFF = 0,
                                  CHORUS,
                                  FLANGE,
                                  RING_MOD,
                                  DISTORTION, /* tube-drive — legacy name kept   */
                                  PHASER };

/* [P2] Dynamics insert bank — loaded via CMD_*_FX_IDX_B (repurposed B slot). */
enum class DynMode : uint8_t { OFF = 0,
                               COMPRESSOR,
                               GATE,
                               TRANSIENT,
                               LIMITER };

enum class InsertBank : uint8_t { FX = 0, DYN = 1 };

struct InsertFxPreset {
  char name[24];
  InsertMode type;
  float p1, p2, p3;
  float fx_mix;
  float dl_time, dl_fb, dl_mix;
  float rv_size, rv_damp, rv_mix;
};

struct DynPreset {
  char name[24];
  DynMode mode;
  float thr;    /* threshold 0–1 (linear)                         */
  float ratio;  /* comp ratio 1–20; gate slope; transient amt     */
  float atk;    /* attack coef 0.001–0.5                          */
  float rel;    /* release coef 0.001–0.2                         */
  float makeup; /* output gain 0.5–2.0                            */
  float mix;    /* wet/dry                                        */
};

struct MasterFxPreset {
  char name[20];
  float tb_drive, tb_tone, tb_mix;
  float dj_freq, dj_res, dj_mix;
  float eq_low, eq_high;
};

/* [P0] Preset tables defined in effect.cpp (single instance). */
extern const InsertFxPreset INSERT_FX_PRESETS[16];
extern const DynPreset DYNAMICS_PRESETS[16];
extern const MasterFxPreset MASTER_FX_PRESETS[16];

/* ─────────────────────────────────────────────────────────────────────────────
 * SynthFX — per-instrument insert effect (chorus / flange / ring-mod / distortion)
 *
 * [P0] Mono: one delay line (ch_dlL).  [A] FLANGE and RING_MOD implemented.
 *
 * Critical rule for all modes:
 *   ch_dlL.write() MUST be called in every code path so the delay-line cursor
 *   stays in sync.  If a mode switch happens between buffer calls, a stale
 *   cursor read would produce a pop.
 * ─────────────────────────────────────────────────────────────────────────────*/
class SynthFX {
  FxDelayLine ch_dlL;
  FxLFO lfoL;
  float dist_lpL = 0.0f;
public:
  InsertMode mode = InsertMode::OFF;
  float p1 = 0.0f, p2 = 0.0f, p3 = 0.0f;
  float fx_mix = 0.0f;

  bool init() {
    lfoL.phase = 0.25f;
    /* [P1] default DRAM_FIRST — modulated insert delay must not live in PSRAM */
    return ch_dlL.init(INSERT_DELAY_LEN);
  }
  void clear() {
    ch_dlL.clear();
    dist_lpL = 0.0f;
  }
  void loadPreset(const InsertFxPreset& p) {
    mode = p.type;
    p1 = p.p1;
    p2 = p.p2;
    p3 = p.p3;
    fx_mix = p.fx_mix;
  }

  /* [P0] Stereo process() + R-channel state removed — the pipeline is mono
   * (process_mono below).  Stereo image is created at the reverb/master stage. */

  /* [MONO-FX] Single-channel variant — used by the mono insert pipeline.
   * Uses ONLY the L-side state (ch_dlL / lfoL / dist_lpL).  The synth sources
   * are mono, so processing one channel is bit-identical to the old L output at
   * half the cost.  Consistency rule preserved: ch_dlL.write() runs on every
   * path.  NOTE: never mix process()/process_mono() on the same instance — the
   * pipeline uses exclusively one or the other.
   * NOTE: do NOT mark this IRAM_ATTR — large header-defined class methods
   * trigger Xtensa "dangerous relocation: l32r" linker errors when the TU
   * that includes effect.h (audio.cpp) instantiates them.  The outer
   * fx_process_multi_buf_safe() is IRAM-resident; these inline into it when
   * the compiler chooses, otherwise they run from flash (acceptable).      */
  inline void process_mono(float& x) {
    /* [FX-OPT5] _unsafe delay-line ops: this runs only from the IRAM hot path,
     * which bails out before the sample loop unless the chain is fully init'd. */
    if (fx_mix < 0.001f || mode == InsertMode::OFF) {
      ch_dlL.write_unsafe(x);
      return;
    }
    float wet = x;
    switch (mode) {
      case InsertMode::CHORUS: {
        const float swing = p2 * 220.5f;
        wet = ch_dlL.readFrac_unsafe(308.7f + lfoL.tri(p1) * swing);
        ch_dlL.write_unsafe(x);
        break;
      }
      case InsertMode::FLANGE: {
        const float modL = 2.0f + p2 * 100.0f * (0.5f + 0.5f * lfoL.tri(p1));
        wet = ch_dlL.readFrac_unsafe(modL);
        ch_dlL.write_unsafe(fx_clampf(x + wet * p3));
        break;
      }
      case InsertMode::RING_MOD: {
        wet = x * lfoL.sine(p1);
        ch_dlL.write_unsafe(x);
        break;
      }
      case InsertMode::DISTORTION: {
        const float coef = std::min(0.999f, std::max(0.001f, p2));
        dist_lpL += (fx_tanh(x * p1) - dist_lpL) * coef;
        wet = fx_clampf(dist_lpL * p3);
        ch_dlL.write_unsafe(x);
        break;
      }
      case InsertMode::PHASER: {
        /* [P2] Short modulated comb — p1=LFO Hz, p2=depth 0–1, p3=feedback   */
        const float mod = 3.0f + p2 * 80.0f * (0.5f + 0.5f * lfoL.tri(p1));
        wet = ch_dlL.readFrac_unsafe(mod);
        ch_dlL.write_unsafe(fx_clampf(x + wet * std::min(0.75f, p3)));
        break;
      }
      default:
        ch_dlL.write_unsafe(x);
        return;
    }
    x = x * (1.0f - fx_mix) + wet * fx_mix;
  }
};

/* ─────────────────────────────────────────────────────────────────────────────
 * InsertSlot — [P2] two INDEPENDENT insert stages per instrument, in series A→B.
 *
 * CMD_*_FX_IDX   → slot A: FX bank preset (chorus/flange/ring-mod/dist/phaser)
 * CMD_*_FX_IDX_B → slot B: Dynamics bank preset (comp/gate/transient/limiter)
 *
 * [v6.0-FIX] A and B are processed serially and never override one another;
 * each self-bypasses when OFF / zero-mix.
 *
 * dly_send / rev_send → shared SharedAux bus.
 * ─────────────────────────────────────────────────────────────────────────────*/
struct InsertSlot {
  SynthFX fx;
  /* Dynamics state (active when bank == DYN) */
  DynMode dyn_mode = DynMode::OFF;
  float dyn_thr = 0.5f, dyn_ratio = 4.f, dyn_atk = 0.05f, dyn_rel = 0.01f;
  float dyn_makeup = 1.f, dyn_mix = 0.f;
  float dyn_env = 0.f;
  float dyn_thr_safe = 0.5f; /* [FX-OPT2] max(dyn_thr,1e-4) cached at preset load */
  float dly_send = 0.0f;
  float rev_send = 0.0f;

  bool init() { return fx.init(); }
  void clear() {
    fx.clear();
    dyn_env = 0.f;
  }
  void loadDynPreset(const DynPreset& p) {
    dyn_mode = p.mode;
    dyn_thr = p.thr;
    dyn_thr_safe = std::max(p.thr, 1.e-4f);
    dyn_ratio = p.ratio;
    dyn_atk = p.atk;
    dyn_rel = p.rel;
    dyn_makeup = p.makeup;
    dyn_mix = p.mix;
    dyn_env = 0.f;
  }
  inline void process_dyn_mono(float& x) {
    if (dyn_mix < 0.001f || dyn_mode == DynMode::OFF) return;
    const float dry = x;
    float wet = x;
    const float a = fabsf(x);
    switch (dyn_mode) {
      case DynMode::COMPRESSOR: {
        dyn_env += (a - dyn_env) * (a > dyn_env ? dyn_atk : dyn_rel);
        float gr = 1.f;
        if (dyn_env > dyn_thr) {
          gr = (dyn_thr_safe + (dyn_env - dyn_thr_safe) / std::max(1.f, dyn_ratio)) / dyn_env;
        }
        wet = x * gr * dyn_makeup;
        break;
      }
      case DynMode::GATE: {
        dyn_env += (a - dyn_env) * (a > dyn_thr ? dyn_atk : dyn_rel);
        wet = (dyn_env > dyn_thr) ? x : x * (dyn_env / dyn_thr_safe);
        wet *= dyn_makeup;
        break;
      }
      case DynMode::TRANSIENT: {
        const float peak = a;
        dyn_env += (peak - dyn_env) * dyn_rel;
        const float delta = peak - dyn_env;
        wet = x + delta * dyn_ratio * dyn_makeup;
        break;
      }
      case DynMode::LIMITER: {
        const float lim = dyn_thr;
        wet = fx_clampf(x * dyn_makeup);
        if (fabsf(wet) > lim)
          wet = (wet > 0.f ? 1.f : -1.f) * lim;
        break;
      }
      default: return;
    }
    x = dry * (1.f - dyn_mix) + wet * dyn_mix;
  }
  /* [v6.0-FIX] Slots A and B are INDEPENDENT and run in SERIES (A → B), restoring
   * the pre-v6.0 behaviour the user expects.  Slot A = modulation FX bank, slot
   * B = dynamics bank.  Each stage self-bypasses when OFF / zero-mix, so loading
   * a preset into one slot never silences the other.  (The old code gated on
   * `bank` and ran only ONE stage — that is what made A override B.) */
  inline void process_mono(float& x) {
    fx.process_mono(x);   /* slot A — chorus / flange / ring-mod / dist / phaser */
    process_dyn_mono(x);  /* slot B — compressor / gate / transient / limiter    */
  }
};

/* ─────────────────────────────────────────────────────────────────────────────
 * DrumFX — drum bus saturation + tone shaping, applied before drumInsert.
 *
 * Fixed character processor (not exposed in UI menu): symmetric tanh saturation
 * followed by a one-pole lowpass.  Gives the drum bus a warm analogue quality
 * that sits well below the melodic instruments at the summing stage.
 * ─────────────────────────────────────────────────────────────────────────────*/
class DrumFX {
  FxBiquad toneL;
  float cached_drive = -1.0f, cached_k = 1.0f, cached_makeup = 1.0f;
  float cached_tone = -1.0f;
public:
  float drive = 0.50f; /* 0–1; analogue-style saturation amount */
  float tone = 0.80f;  /* 0–1 → LP cutoff 200–14200 Hz          */

  bool init() {
    toneL.setLowPass(8000.f, 0.5f);
    return true;
  }
  void clear() {
    toneL.reset();
    cached_drive = cached_tone = -1.0f;
  }
  inline void update_params() {
    if (drive != cached_drive) {
      cached_k = 1.0f + drive * 8.5f;
      cached_makeup = 1.0f / fx_tanh(cached_k);
      cached_drive = drive;
    }
    if (tone != cached_tone) {
      const float fc = tone * 14000.0f + 200.0f;
      toneL.setLowPass(fc, 0.55f);
      cached_tone = tone;
    }
  }
  /* [P0] Stereo process() + toneR removed — mono pipeline only. */
  inline void process_mono(float& x) { /* [MONO-FX] L-side state only */
    if (drive > 0.001f) x = fx_tanh(x * cached_k) * cached_makeup;
    x = toneL.process(x);
  }
};

/* ─────────────────────────────────────────────────────────────────────────────
 * SharedDelay — [P3] long stereo delay bus (PSRAM), mono in → mono tap out.
 * ─────────────────────────────────────────────────────────────────────────────*/
class SharedDelay {
  FxDelayLine dlM;
  /* [FX-OPT4] tap + feedback are constant across a buffer → cached in setParams. */
  float dt_samples = 0.f, dl_fb = 0.f;
public:
  bool init() {
    static const uint32_t DL_SIZES[] = { 65536u, 16384u, 4096u };
    for (uint32_t sz : DL_SIZES) {
      if (dlM.init(sz, FxDelayTier::PSRAM_FIRST)) return true;
    }
    Serial.println(F("SharedDelay: all sizes failed"));
    return false;
  }
  void clear() { dlM.clear(); }
  /* [FX-OPT4] Call once per buffer (cold) before the sample loop. */
  inline void setParams(float dlyTime, float dlyFb) {
    dt_samples = std::min(dlyTime * (float)FX_SR, (float)dlM.mask);
    dl_fb = dlyFb;
  }
  inline void process(float in, float& out) {
    out = dlM.buf ? dlM.readFrac(dt_samples) : 0.f;
    dlM.write(std::min(3.0f, std::max(-3.0f, in + out * dl_fb)));
  }
};

/* ─────────────────────────────────────────────────────────────────────────────
 * SharedReverb — [P3] Schroeder reverb, mono in → decorrelated stereo wet.
 * ─────────────────────────────────────────────────────────────────────────────*/
class SharedReverb {
  FxAllpassFilter apM2;
  FxCombFilter combM[4];
  FxAllpassFilter apM[2];
  /* [FX-OPT4] comb feedback + damping are constant across a buffer → cached. */
  float rev_fb = 0.84f, rev_damp = 0.5f;
public:
  bool init() {
    bool ok = apM2.init(347u);
    static const uint32_t COMB_L[4] = { 1116u, 1188u, 1277u, 1356u };
    static const uint32_t AP_LEN[2] = { 556u, 441u };
    for (int i = 0; i < 4; ++i) ok &= combM[i].init(COMB_L[i]);
    for (int i = 0; i < 2; ++i) ok &= apM[i].init(AP_LEN[i]);
    return ok;
  }
  void clear() {
    apM2.clear();
    for (int i = 0; i < 4; ++i) combM[i].clear();
    for (int i = 0; i < 2; ++i) apM[i].clear();
  }
  /* [FX-OPT4] Call once per buffer (cold) before the sample loop. */
  inline void setParams(float revSize, float revDamp) {
    rev_fb = 0.70f + revSize * 0.28f;
    rev_damp = revDamp;
  }
  inline void process(float inRevMono, float& wetL, float& wetR) {
    float rOut = 0.f;
    for (int i = 0; i < 4; ++i) rOut += combM[i].process(inRevMono, rev_fb, rev_damp);
    const float diff = apM[0].process(rOut * 0.25f);
    wetL = apM[1].process(diff) * 0.8f;
    wetR = apM2.process(diff) * 0.8f;
  }
};

/* [P3] SharedAux kept as alias wrapper for any legacy references. */
class SharedAux {
  SharedDelay delay;
  SharedReverb reverb;
public:
  bool init() { return delay.init() & reverb.init(); }
  void clear() { delay.clear(); reverb.clear(); }
  /* [FX-OPT4] Latch delay/reverb coefficients once per buffer (cold path). */
  inline void setParams(float dlyTime, float dlyFb, float revSize, float revDamp) {
    delay.setParams(dlyTime, dlyFb);
    reverb.setParams(revSize, revDamp);
  }
  inline void process(float inDly, float inRevMono, bool runDelay,
                      float& wetL, float& wetR) {
    float dOut = 0.f;
    if (runDelay) delay.process(inDly, dOut);
    float revL = 0.f, revR = 0.f;
    reverb.process(inRevMono, revL, revR);
    wetL = fx_tanh(dOut + revL);
    wetR = fx_tanh(dOut + revR);
  }
};

/* ─────────────────────────────────────────────────────────────────────────────
 * MasterFX — [P5] Master DAP: glue comp → EQ3 (low/mid/high) → soft limiter.
 *
 * Parameter mapping (backward compatible with existing atomics + presets):
 *   tb_drive / tb_mix  → bus compressor threshold / mix
 *   dj_freq/res/mix    → mid peaking EQ (filter-sweep presets; dj_mix=0 bypasses)
 *   eq_low / eq_high   → low / high shelves
 *   lim_thresh/ceiling → final soft-knee limiter
 * ─────────────────────────────────────────────────────────────────────────────*/
class MasterFX {
  FxBiquad lowL, lowR, midL, midR, highL, highR;
  float cached_eq_low = -999.f, cached_eq_high = -999.f;
  float cached_dj_freq = -1.f, cached_dj_res = -1.f, cached_dj_mix = -1.f;
  float comp_env = 0.f;
  float cached_comp_drv = -1.f, cached_comp_mix = -1.f, cached_comp_tone = -1.f;
  float comp_thr = 0.55f, comp_ratio = 4.f, comp_mix = 0.f;
  float comp_atk = 0.12f, comp_rel = 0.018f;
  /* [FX-OPT2] tube-sat + compressor constants cached per-buffer in update_params()
   * (they depend only on drive/tube_tone/tube_mix), not recomputed per sample. */
  float comp_sat = 1.0f, comp_tilt = 0.35f, comp_w = 0.0f;
  float comp_makeup = 1.0f;   /* [WS11-FIX] level restore after tube tanh */
  float comp_thr_safe = 0.55f;
  float lim_A = 0.18f, cached_lim_t = 0.80f, cached_lim_c = 0.98f;
  /* [PERF] EQ3 bypass: a 0 dB shelf/peak is mathematically passthrough, so the
   * 6 biquad evals per sample are pure waste when the master EQ is flat (the
   * common case).  Recomputed in update_params(); gates the EQ block in process(). */
  bool eq_active = false;

  inline float softKneeLimit(float x) const {
    const float a = fabsf(x);
    if (a <= cached_lim_t) return x;
    const float g = cached_lim_t + lim_A * (a - cached_lim_t) / ((a - cached_lim_t) + lim_A);
    return (x < 0.0f) ? -g : g;
  }
public:
  /* Legacy public fields — still fed from atomics in fx_process_multi_buf_safe */
  float dj_freq = 1.f, dj_res = 0.1f, dj_mix = 0.f;
  float eq_low = 0.f, eq_high = 0.f;
  float drive = 0.f, tube_tone = 0.5f, tube_mix = 0.f;
  float lim_thresh = 0.80f, lim_ceiling = 0.98f;

  bool init() {
    lowL.setLowShelf(120.f, 0.f);
    lowR.setLowShelf(120.f, 0.f);
    midL.setPeaking(1000.f, 1.f, 0.f);
    midR.setPeaking(1000.f, 1.f, 0.f);
    highL.setHighShelf(6000.f, 0.f);
    highR.setHighShelf(6000.f, 0.f);
    return true;
  }
  void clear() {
    lowL.reset(); lowR.reset();
    midL.reset(); midR.reset();
    highL.reset(); highR.reset();
    comp_env = 0.f;
    cached_eq_low = cached_eq_high = -999.f;
    cached_dj_freq = -1.f;
    cached_comp_drv = -1.f;
  }
  inline void update_params() {
    if (eq_low != cached_eq_low) {
      lowL.setLowShelf(120.f, eq_low);
      lowR.setLowShelf(120.f, eq_low);
      cached_eq_low = eq_low;
    }
    if (eq_high != cached_eq_high) {
      highL.setHighShelf(6000.f, eq_high);
      highR.setHighShelf(6000.f, eq_high);
      cached_eq_high = eq_high;
    }
    /* Mid EQ — repurposed DJ-filter params for sweep/cut presets */
    if (dj_freq != cached_dj_freq || dj_res != cached_dj_res || dj_mix != cached_dj_mix) {
      if (dj_mix > 0.001f) {
        const float fc = dj_freq * 8000.f + 120.f;
        const float q  = 0.5f + dj_res * 4.0f;
        /* Negative gain = cut (LPF/HPF sweep character from old DJ presets) */
        const float g  = -dj_mix * (6.f + dj_res * 12.f);
        midL.setPeaking(fc, q, g);
        midR.setPeaking(fc, q, g);
      } else {
        midL.setPeaking(1000.f, 1.f, 0.f);
        midR.setPeaking(1000.f, 1.f, 0.f);
      }
      cached_dj_freq = dj_freq;
      cached_dj_res  = dj_res;
      cached_dj_mix  = dj_mix;
    }
    /* Glue compressor — tb_drive lowers threshold, tb_mix is wet amount.
     * [FX-OPT2] recompute tube-sat constants here too (depend on drive/tone/mix).
     * tube_tone is folded in so a tone-only change still refreshes comp_tilt. */
    if (drive != cached_comp_drv || tube_mix != cached_comp_mix ||
        tube_tone != cached_comp_tone) {
      comp_thr  = 0.75f - drive * 0.55f;
      comp_mix  = std::max(drive, tube_mix);
      comp_ratio = 2.f + drive * 6.f;
      /* [WS11-FIX] DRV felt far too weak: the old linear 1+drive*5 barely
       * saturated, and with no makeup the tanh just soft-compressed peaks.
       * Quadratic curve keeps the default (~0.3) baseline gentle but makes the
       * top of the knob bite hard (1.0 → 12× pre-gain), and the makeup term
       * restores level so the added harmonics are AUDIBLE rather than just
       * limited away. */
      comp_sat   = 1.0f + drive * drive * 11.0f;
      comp_tilt  = 0.35f + tube_tone * 1.65f;
      comp_makeup = 1.0f / fx_tanh(comp_sat * comp_tilt);
      comp_w     = std::max(drive, tube_mix);
      comp_thr_safe = std::max(comp_thr, 1.e-4f);
      cached_comp_drv = drive;
      cached_comp_mix = tube_mix;
      cached_comp_tone = tube_tone;
    }
    if (lim_thresh != cached_lim_t || lim_ceiling != cached_lim_c) {
      cached_lim_t = constrain(lim_thresh, 0.05f, 0.99f);
      cached_lim_c = constrain(lim_ceiling, cached_lim_t + 1.e-3f, 0.999f);
      lim_A = cached_lim_c - cached_lim_t;
    }
    /* EQ3 is engaged only when a shelf is off-flat or the mid (DJ) band is wet. */
    eq_active = (fabsf(eq_low) > 0.01f) || (fabsf(eq_high) > 0.01f) || (dj_mix > 0.001f);
  }
  inline void process(float& L, float& R) {
    /* 0. Tube saturation (TUBE DRV/TONE/MIX) — was compressor-only; DRV now
     * audibly warms the bus.  wet = max(drive, tube_mix). */
    if (drive > 0.001f || tube_mix > 0.001f) {
      const float tL = fx_tanh(L * comp_sat * comp_tilt) * comp_makeup;
      const float tR = fx_tanh(R * comp_sat * comp_tilt) * comp_makeup;
      L += (tL - L) * comp_w;
      R += (tR - R) * comp_w;
    }
    /* 1. Glue compressor (stereo-linked) */
    if (comp_mix > 0.001f) {
      const float peak = std::max(fabsf(L), fabsf(R));
      comp_env += (peak - comp_env) * (peak > comp_env ? comp_atk : comp_rel);
      float gr = 1.f;
      if (comp_env > comp_thr) {
        gr = (comp_thr_safe + (comp_env - comp_thr_safe) / comp_ratio) / comp_env;
      }
      const float g = 1.f + (gr - 1.f) * comp_mix;
      L *= g;
      R *= g;
    }
    /* 2. EQ3: low shelf → mid peak → high shelf (skipped when flat [PERF]) */
    if (eq_active) {
      L = highL.process(midL.process(lowL.process(L)));
      R = highR.process(midR.process(lowR.process(R)));
    }
    /* 3. Mid/side soft-knee limiter */
    const float m = (L + R) * 0.5f;
    const float s = fx_tanh((L - R) * 0.5f * 1.4f);
    L = softKneeLimit(m + s);
    R = softKneeLimit(m - s);
  }
};

/* ─────────────────────────────────────────────────────────────────────────────
 * FxChain — top-level container: three InsertSlots + DrumFX + SharedAux + Master
 * ─────────────────────────────────────────────────────────────────────────────*/
struct FxChainMetrics {
  uint32_t process_calls = 0, last_cycles = 0, peak_cycles = 0;
  float avg_load_pct = 0.f;
};
static inline void resetFxProfiler(FxChainMetrics& m) {
  m.process_calls = m.last_cycles = m.peak_cycles = 0;
  m.avg_load_pct = 0.f;
}

class FxChain {
public:
  InsertSlot harpInsert, seqInsert, drumInsert;
  DrumFX drumFx;
  SharedAux auxFx;
  MasterFX masterFx;
  FxChainMetrics metrics;
  /* [FX-OPT1] true only when every sub-chain allocated.  The hot path bails out
   * when this is false, which is what makes the FxDelayLine _unsafe ops sound. */
  bool initialized = false;

  bool init() {
    /* All chains attempted — partial failure still initialises what's available */
    bool ok = harpInsert.init();
    ok &= seqInsert.init();
    ok &= drumInsert.init();
    ok &= drumFx.init();
    ok &= auxFx.init(); /* [E] graceful size fallback inside */
    ok &= masterFx.init();
    initialized = ok;
    return ok;
  }
  void clear() {
    harpInsert.clear();
    seqInsert.clear();
    drumInsert.clear();
    drumFx.clear();
    auxFx.clear();
    masterFx.clear();
  }

  /* ── loadInsert [P2] ─────────────────────────────────────────────────────
   * bankSlot 0 = FX bank (CMD_*_FX_IDX)   → slot A (modulation FX)
   * bankSlot 1 = Dynamics bank (CMD_*_FX_IDX_B) → slot B (dynamics)
   * [v6.0-FIX] The two banks are INDEPENDENT: each writes only its own sub-state
   * (slot.fx vs the dyn_* fields), so loading one bank never disturbs the other.
   * Both run in series at process time (A → B).
   * [H3] Shared aux globals are NOT stomped here.                            */
  void loadInsert(InsertSlot& slot, int bankSlot, int idx) {
    if (idx < 0 || idx >= 16) return;
    portENTER_CRITICAL(&patchMux);
    if (bankSlot == 0) {
      InsertFxPreset p;
      memcpy_P(&p, &INSERT_FX_PRESETS[idx], sizeof(InsertFxPreset));
      slot.fx.loadPreset(p);
      slot.dly_send = p.dl_mix;
      slot.rev_send = p.rv_mix;
    } else {
      DynPreset p;
      memcpy_P(&p, &DYNAMICS_PRESETS[idx], sizeof(DynPreset));
      slot.loadDynPreset(p);
    }
    portEXIT_CRITICAL(&patchMux);
  }

  void loadHarpInsert(int bankSlot, int idx) {
    loadInsert(harpInsert, bankSlot, idx);
    (bankSlot == 0 ? harpFxIndex : harpFxIndexB).store(idx);
  }
  void loadSeqInsert(int bankSlot, int idx) {
    loadInsert(seqInsert, bankSlot, idx);
    (bankSlot == 0 ? seqFxIndex : seqFxIndexB).store(idx);
  }
  void loadDrumInsert(int bankSlot, int idx) {
    loadInsert(drumInsert, bankSlot, idx);
    (bankSlot == 0 ? drumFxIndexA : drumFxIndexB).store(idx);
  }

  void loadMasterFx(int idx) {
    if (idx < 0 || idx >= 16) return;
    MasterFxPreset p;
    memcpy_P(&p, &MASTER_FX_PRESETS[idx], sizeof(MasterFxPreset));
    tbDrive.store(p.tb_drive);
    tbTone.store(p.tb_tone);
    tbMix.store(p.tb_mix);
    djFreq.store(p.dj_freq);
    djRes.store(p.dj_res);
    djMix.store(p.dj_mix);
    masterEqLow.store(p.eq_low);
    masterEqHigh.store(p.eq_high);
    masterFxIndex.store(idx);
    displayDirty.store(true, std::memory_order_relaxed);
  }
};

/* [P0] Single global instance defined in effect.cpp. */
extern FxChain fx;

/* ── Convenience loaders (used by sysex handlers and display.cpp) ─────────── */
inline void loadHarpFx(int i) {
  fx.loadHarpInsert(0, i);
}
inline void loadHarpFxB(int i) {
  fx.loadHarpInsert(1, i);
}
inline void loadSeqFx(int i) {
  fx.loadSeqInsert(0, i);
}
inline void loadSeqFxB(int i) {
  fx.loadSeqInsert(1, i);
}
inline void loadDrumFx(int i) {
  fx.loadDrumInsert(0, i);
}
inline void loadDrumFxB(int i) {
  fx.loadDrumInsert(1, i);
}

/* [DRUM-FX-SYNC] User-initiated drum FX (slot A) recall.  Unlike harp/seq —
 * whose insert send IS the live value — the drum bus reads its sends from the
 * drumDlySend/drumRevSend atomics (snapAudioParams overwrites drumInsert.*_send
 * every buffer).  So a *deliberate* recall must publish the preset's sends INTO
 * those atomics for them to take effect (and for the App knob to follow via
 * txDrumFxSends).  The audio-task/NVS paths keep calling plain loadDrumFx, which
 * leaves the atomics — and thus the saved sends — untouched. */
inline void loadDrumFxLive(int i) {
  fx.loadDrumInsert(0, i);
  float dly, rev;
  portENTER_CRITICAL(&patchMux);
  dly = fx.drumInsert.dly_send;
  rev = fx.drumInsert.rev_send;
  portEXIT_CRITICAL(&patchMux);
  drumDlySend.store(dly, std::memory_order_relaxed);
  drumRevSend.store(rev, std::memory_order_relaxed);
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Lifecycle + per-buffer pipeline — [P0] defined in effect.cpp.
 *   fx_process_multi_buf_safe — main per-buffer FX pipeline (IRAM, Core 0).
 *   fx_init / fx_free_buffers — scratch + chain allocation.
 * Scratch buffers (mono hL/sL/dL) and the global fx live in effect.cpp.
 * ─────────────────────────────────────────────────────────────────────────────*/
void IRAM_ATTR fx_process_multi_buf_safe(
  int16_t* h_buf, int16_t* s_buf, int16_t* d_buf,
  int16_t* out_buf, size_t frames,
  bool hOn, bool sOn, bool dOn);
bool fx_init();
void fx_free_buffers();
#endif /* EFFECT_H */
