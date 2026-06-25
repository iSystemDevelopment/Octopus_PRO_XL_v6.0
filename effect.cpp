/* ═════════════════════════════════════════════════════════════════════════════
 * Octopus PRO XL v6.1.01 — Laser Harp Groovebox
 * © 2026 DIODAC ELECTRONICS / iSystem. All Rights Reserved.
 *
 * PROPRIETARY AND CONFIDENTIAL. Unauthorized copying, distribution, modification,
 * or use of this software or firmware, in whole or in part, is strictly prohibited
 * without prior written permission from DIODAC ELECTRONICS.
 * ═════════════════════════════════════════════════════════════════════════════
 * effect.cpp — v6.1.01  FX ENGINE — OUT-OF-LINE DEFINITIONS
 *
 * Single compile unit for FxChain: PROGMEM preset tables, fx_init, buffer alloc,
 * and IRAM fx_process_multi_buf_safe (3-engine mix → inserts → aux → master FX).
 * Hot per-sample primitives stay inline in effect.h.
 * ═════════════════════════════════════════════════════════════════════════════ */
#include "effect.h"

/* ═══════════════════════════════════════════════════════════════════════════
 * Preset tables (single definition; declared extern in effect.h)
 * ═══════════════════════════════════════════════════════════════════════════ */
/* clang-format off */
/* ── [WS5] SP()-style compact FX authoring ───────────────────────────────────
 * One readable line per preset, mirroring the SP() synth macro in assets.h.
 * Values are stored verbatim (already human units — no v14 packing here):
 *   IFX( name, type, p1,p2,p3, fxMix, dlyTime,dlyFb,dlyMix, revSize,revDamp,revMix )
 *        p1: chorus depth / ring-mod Hz / phaser rate / dist drive   (mode-specific)
 *   MFX( name, drive,tone,mix, djFreq,djRes,djMix, eqLowDb,eqHighDb )
 *   DYN( name, mode, thr,ratio,atk,rel,makeup,mix )
 * ─────────────────────────────────────────────────────────────────────────── */
#define IFX(nm, ty, a,b,c, mix, dT,dF,dM, rS,rD,rM)                          \
  { nm, InsertMode::ty, (float)(a),(float)(b),(float)(c), (float)(mix),      \
    (float)(dT),(float)(dF),(float)(dM), (float)(rS),(float)(rD),(float)(rM) }
#define MFX(nm, drv,tn,mx, fq,rs,dm, eL,eH)                                  \
  { nm, (float)(drv),(float)(tn),(float)(mx),                                \
    (float)(fq),(float)(rs),(float)(dm), (float)(eL),(float)(eH) }
#define DYN(nm, md, thr,rat,atk,rel,mk,mix)                                  \
  { nm, DynMode::md, (float)(thr),(float)(rat),(float)(atk),                 \
    (float)(rel),(float)(mk),(float)(mix) }

const InsertFxPreset INSERT_FX_PRESETS[16] PROGMEM = {
  IFX("Nebula Void Taps",    OFF,           0,   0,   0,    0.00, 0.30,0.0,0.00, 0.70,0.5,0.48),
  IFX("Supernova ChorSwell", CHORUS,        1.5, 0.6, 0,    0.42, 0.00,0.0,0.00, 0.00,0.0,0.00),
  IFX("Pulsar Mod Shunt",    RING_MOD,   1200,   0.5, 0.2,  0.36, 0.00,0.0,0.00, 0.00,0.0,0.00),
  IFX("Quasar PhaseShift",   PHASER,        0.8, 0.6, 0.35, 0.30, 0.00,0.0,0.00, 0.85,0.6,0.61),
  IFX("Chronos Warp Echo",   OFF,           0,   0,   0,    0.00, 0.37,0.5,0.39, 0.00,0.0,0.00),
  IFX("Singularity TubeSat", DISTORTION,    3.0, 0.35,0.85, 0.30, 0.00,0.0,0.00, 0.00,0.0,0.00),
  IFX("Gravity Jet Flange",  FLANGE,        1.5, 0.7, 0.6,  0.42, 0.00,0.0,0.00, 0.00,0.0,0.00),
  IFX("Astral Shimmer Verb", CHORUS,        1.7, 0.4, 0,    0.33, 0.00,0.0,0.00, 0.60,0.2,0.20),
  IFX("DarkMatter SubRoom",  OFF,           0,   0,   0,    0.00, 0.00,0.0,0.00, 0.95,0.4,0.67),
  IFX("Cosmos Tape Echo",    OFF,           0,   0,   0,    0.00, 0.45,0.4,0.36, 0.00,0.0,0.00),
  IFX("HyperDrive ResMod",   RING_MOD,   1500,   0.4, 0.1,  0.30, 0.00,0.0,0.00, 0.00,0.0,0.00),
  IFX("Vortex Comb Swirl",   PHASER,        1.2, 0.5, 0.25, 0.27, 0.00,0.0,0.00, 0.00,0.0,0.00),
  IFX("Organic Drum Drive",  DISTORTION,    2.6, 0.68,0.88, 0.44, 0.00,0.0,0.00, 0.00,0.0,0.00),
  IFX("Aether Shimmer Gate", OFF,           0,   0,   0,    0.00, 0.00,0.0,0.00, 0.50,0.1,0.27),
  IFX("Void Saturation",     DISTORTION,    4.0, 0.6, 0.85, 0.24, 0.00,0.0,0.00, 0.60,0.5,0.30),
  IFX("ZeroPoint Quantum",   RING_MOD,   1200,   3.3, 0.6,  0.40, 0.00,0.0,0.00, 0.00,0.0,0.00)
};

const MasterFxPreset MASTER_FX_PRESETS[16] PROGMEM = {
  MFX("Aether Bypass",    0.00,0.50,0.00, 1.00,0.00,0.00,  0.0,  0.0),
  MFX("Galactic Bus",     0.10,0.60,0.15, 1.00,0.00,0.00, +1.0, +1.5),
  MFX("Magnetar Sat",     0.30,0.40,0.35, 1.00,0.00,0.00, +2.0, -1.5),
  MFX("Pulsar Impact",    0.38,0.65,0.40, 1.00,0.00,0.00, +2.5, +1.0),
  MFX("Sub-Zero Core",    0.20,0.30,0.15, 1.00,0.00,0.00, +4.0, -2.0),
  MFX("Nebula Polish",    0.05,0.80,0.10, 1.00,0.00,0.00, -1.0, +4.0),
  /* [CRACK-FIX] drive/mix/resonance pulled to musical levels — these slammed
   * the master limiter and (LPF/HPF) overshot via filter resonance.          */
  MFX("Horizon Limit",    0.42,0.50,0.40, 1.00,0.00,0.00, +1.0, +1.0),
  MFX("LPF Deep Sweep",   0.00,0.50,0.00, 0.30,0.45,0.85,  0.0,  0.0),
  MFX("HPF Prism Sweep",  0.00,0.50,0.00, 0.78,0.30,0.80, -3.0, +2.0),
  MFX("Spectral Scoop",   0.15,0.50,0.10, 1.00,0.00,0.00, +3.0, +3.0),
  MFX("Cosmos Echo",      0.35,0.35,0.35, 1.00,0.00,0.00, +3.0, -1.0),
  MFX("Nova Overdrive",   0.55,0.55,0.50, 1.00,0.00,0.00, +1.0, +2.0),
  MFX("Quantum Tube",     0.45,0.72,0.42, 1.00,0.00,0.00, -1.0, +3.0),
  MFX("Lofi Singularity", 0.40,0.20,0.60, 0.60,0.10,0.40, +2.0, -4.0),
  MFX("Interstellar Desk",0.42,0.40,0.45, 0.80,0.30,0.20, +1.5, -2.5),
  MFX("Centauri Master",  0.20,0.70,0.25, 1.00,0.00,0.00, +2.0, +2.5)
};

/* [P2] Dynamics insert bank — loaded via CMD_*_FX_IDX_B */
const DynPreset DYNAMICS_PRESETS[16] PROGMEM = {
  DYN("Dyn Bypass",      OFF,        0.50, 1.0, 0.05, 0.02,  1.00, 0.00),
  DYN("Glue Comp",       COMPRESSOR, 0.55, 4.0, 0.08, 0.015, 1.15, 0.65),
  DYN("Punch Comp",      COMPRESSOR, 0.45, 6.0, 0.15, 0.008, 1.25, 0.70),
  DYN("Soft Limiter",    LIMITER,    0.85, 1.0, 0.01, 0.05,  1.00, 0.80),
  DYN("Brick Limiter",   LIMITER,    0.92, 1.0, 0.005,0.03,  1.00, 1.00),
  DYN("Noise Gate",      GATE,       0.12, 1.0, 0.02, 0.08,  1.00, 1.00),
  DYN("Tight Gate",      GATE,       0.22, 1.0, 0.04, 0.05,  1.00, 1.00),
  DYN("Transient Punch", TRANSIENT,  0.30, 2.5, 0.10, 0.02,  1.10, 0.55),
  DYN("Snap Attack",     TRANSIENT,  0.25, 4.0, 0.18, 0.015, 1.20, 0.60),
  DYN("Drum Smack",      TRANSIENT,  0.35, 3.0, 0.12, 0.01,  1.15, 0.75),
  DYN("Bus Glue",        COMPRESSOR, 0.60, 3.0, 0.06, 0.02,  1.08, 0.50),
  DYN("Vocal Ride",      COMPRESSOR, 0.40, 2.5, 0.10, 0.025, 1.20, 0.55),
  DYN("Harp Sustain",    COMPRESSOR, 0.35, 2.0, 0.04, 0.04,  1.05, 0.45),
  DYN("Seq Pump",        COMPRESSOR, 0.50, 8.0, 0.20, 0.006, 1.30, 0.80),
  DYN("Sub Gate",        GATE,       0.08, 1.0, 0.015,0.12,  1.00, 1.00),
  DYN("Max Safety",      LIMITER,    0.88, 1.0, 0.008,0.04,  0.95, 1.00)
};
#undef IFX
#undef MFX
#undef DYN

/* Shared-room scenes — recall via loadAuxScene() / AUX FX → Room Scn menu. */
#define AUX(nm, t, fb, sz, dm)                                                 \
  { nm, (float)(t), (float)(fb), (float)(sz), (float)(dm) }

const AuxScenePreset AUX_SCENES[16] PROGMEM = {
  AUX("Dry Room",      0.12f, 0.00f, 0.15f, 0.55f),
  AUX("Tight Plate",   0.22f, 0.25f, 0.35f, 0.45f),
  AUX("Studio Booth",  0.30f, 0.30f, 0.50f, 0.48f),
  AUX("Live Hall",     0.38f, 0.35f, 0.70f, 0.52f),
  AUX("Cosmic Plate",  0.45f, 0.40f, 0.60f, 0.50f),
  AUX("Tape Echo",     0.45f, 0.42f, 0.20f, 0.35f),
  AUX("Slap Back",     0.28f, 0.55f, 0.25f, 0.40f),
  AUX("Shimmer Hall",  0.37f, 0.30f, 0.85f, 0.61f),
  AUX("Dark Cave",     0.55f, 0.50f, 0.95f, 0.67f),
  AUX("Nebula Wash",   0.30f, 0.00f, 0.70f, 0.48f),
  AUX("Pulse Chamber", 0.35f, 0.45f, 0.55f, 0.42f),
  AUX("Ambient Bloom", 0.50f, 0.38f, 0.75f, 0.55f),
  AUX("Drum Box",      0.18f, 0.20f, 0.32f, 0.65f),
  AUX("Cathedral",     0.60f, 0.35f, 0.90f, 0.40f),
  AUX("Lo-Fi Deck",    0.40f, 0.60f, 0.45f, 0.72f),
  AUX("Void Infinite", 0.65f, 0.48f, 0.95f, 0.30f)
};
#undef AUX
/* clang-format on */

/* ═══════════════════════════════════════════════════════════════════════════
 * Global FX chain + mono scratch buffers
 * [P0] R-channel scratch buffers removed — the pipeline is mono until the
 * stereo expansion at the reverb/master stage, so hR/sR/dR were dead.
 * ═══════════════════════════════════════════════════════════════════════════ */
FxChain fx;

/* [FX-OPT1] The 3 × 512-float DRAM scratch buffers were eliminated by the
 * single-pass merge in fx_process_multi_buf_safe — each channel now lives in a
 * register for the duration of a sample, so there is nothing to stage in DRAM. */

/* ═══════════════════════════════════════════════════════════════════════════
 * Primitive cold-path definitions (config / allocation; not per-sample)
 * ═══════════════════════════════════════════════════════════════════════════ */

/* ── FxBiquad coefficient math ──────────────────────────────────────────── */
void FxBiquad::setLowPass(float freq, float q) {
  freq = constrain(freq, 20.f, 20000.f);
  float w = 2.0f * (float)M_PI * freq * FX_INV_SR;
  float al = sinf(w) / (2.0f * q);
  float c = cosf(w);
  float ai = 1.0f / (1.0f + al);
  b0 = (1.0f - c) * 0.5f * ai;
  b1 = (1.0f - c) * ai;
  b2 = b0;
  a1 = -2.0f * c * ai;
  a2 = (1.0f - al) * ai;
}
void FxBiquad::setLowShelf(float freq, float gain_db) {
  freq = constrain(freq, 20.f, 20000.f);
  float w = 2.0f * (float)M_PI * freq * FX_INV_SR;
  float A = exp2f(gain_db / 40.0f);
  float alpha = sinf(w) / 2.0f * sqrtf((A + 1.0f / A) * (1.0f / 0.707f - 1.0f) + 2.0f);
  float cs = cosf(w);
  float ai = 1.0f / ((A + 1.0f) + (A - 1.0f) * cs + 2.0f * sqrtf(A) * alpha);
  b0 = A * ((A + 1.0f) - (A - 1.0f) * cs + 2.0f * sqrtf(A) * alpha) * ai;
  b1 = 2.0f * A * ((A - 1.0f) - (A + 1.0f) * cs) * ai;
  b2 = A * ((A + 1.0f) - (A - 1.0f) * cs - 2.0f * sqrtf(A) * alpha) * ai;
  a1 = -2.0f * ((A - 1.0f) + (A + 1.0f) * cs) * ai;
  a2 = ((A + 1.0f) + (A - 1.0f) * cs - 2.0f * sqrtf(A) * alpha) * ai;
}
void FxBiquad::setHighShelf(float freq, float gain_db) {
  freq = constrain(freq, 20.f, 20000.f);
  float w = 2.0f * (float)M_PI * freq * FX_INV_SR;
  float A = exp2f(gain_db / 40.0f);
  float alpha = sinf(w) / 2.0f * sqrtf((A + 1.0f / A) * (1.0f / 0.707f - 1.0f) + 2.0f);
  float cs = cosf(w);
  float ai = 1.0f / ((A + 1.0f) - (A - 1.0f) * cs + 2.0f * sqrtf(A) * alpha);
  b0 = A * ((A + 1.0f) + (A - 1.0f) * cs + 2.0f * sqrtf(A) * alpha) * ai;
  b1 = -2.0f * A * ((A - 1.0f) + (A + 1.0f) * cs) * ai;
  b2 = A * ((A + 1.0f) - (A - 1.0f) * cs - 2.0f * sqrtf(A) * alpha) * ai;
  a1 = 2.0f * ((A - 1.0f) - (A + 1.0f) * cs) * ai;
  a2 = ((A + 1.0f) - (A - 1.0f) * cs - 2.0f * sqrtf(A) * alpha) * ai;
}
void FxBiquad::setPeaking(float freq, float q, float gain_db) {
  freq = constrain(freq, 20.f, 20000.f);
  q = std::max(0.1f, q);
  const float A = powf(10.f, gain_db / 40.f);
  const float w = 2.0f * (float)M_PI * freq * FX_INV_SR;
  const float alpha = sinf(w) / (2.0f * q);
  const float cs = cosf(w);
  const float ai = 1.0f / (1.0f + alpha / A);
  b0 = (1.0f + alpha * A) * ai;
  b1 = (-2.0f * cs) * ai;
  b2 = (1.0f - alpha * A) * ai;
  a1 = (-2.0f * cs) * ai;
  a2 = (1.0f - alpha / A) * ai;
}

/* ── FxDelayLine allocation ─────────────────────────────────────────────── */
bool FxDelayLine::init(uint32_t power_of_two, FxDelayTier tier) {
  if (buf) {
    heap_caps_free(buf);
    buf = nullptr;
  }
  const size_t bytes = power_of_two * sizeof(float);
  const uint32_t caps_dram  = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
  const uint32_t caps_psram = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;

  if (tier == FxDelayTier::DRAM_FIRST) {
    /* [P1] Insert chorus/flange: modulated readFrac every sample — must live
     * in internal DRAM.  PSRAM random-access misses add audible jitter.     */
    buf = (float*)heap_caps_malloc(bytes, caps_dram);
    if (!buf)
      buf = (float*)heap_caps_malloc(bytes, caps_psram);
  } else {
    /* SharedAux long delay: large + mostly sequential — PSRAM is correct.   */
    buf = (float*)heap_caps_malloc(bytes, caps_psram);
    if (!buf)
      buf = (float*)heap_caps_malloc(bytes, caps_dram);
  }
  if (buf) memset(buf, 0, bytes);
  mask = power_of_two - 1u;
  return buf != nullptr;
}
void FxDelayLine::clear() {
  if (buf) memset(buf, 0, (mask + 1u) * sizeof(float));
  pos = 0;
}

/* ── FxCombFilter allocation (DRAM) ─────────────────────────────────────── */
bool FxCombFilter::init(uint32_t samples) {
  len = samples;
  buf = (float*)heap_caps_malloc(samples * sizeof(float),
                                 MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (buf) memset(buf, 0, samples * sizeof(float));
  return buf != nullptr;
}
void FxCombFilter::clear() {
  if (buf) memset(buf, 0, len * sizeof(float));
  pos = 0;
  filterState = 0.0f;
}

/* ── FxAllpassFilter allocation (DRAM) ──────────────────────────────────── */
bool FxAllpassFilter::init(uint32_t samples) {
  len = samples;
  buf = (float*)heap_caps_malloc(samples * sizeof(float),
                                 MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (buf) memset(buf, 0, samples * sizeof(float));
  return buf != nullptr;
}
void FxAllpassFilter::clear() {
  if (buf) memset(buf, 0, len * sizeof(float));
  pos = 0;
}


void IRAM_ATTR fx_process_multi_buf_safe(
  int16_t* h_buf, int16_t* s_buf, int16_t* d_buf,
  int16_t* out_buf, size_t frames,
  bool hOn, bool sOn, bool dOn) {                 // [3a] engine-active flags
  /* [FX-OPT1] Single-pass pipeline — no scratch buffers.  Guard on the chain
   * being fully initialised (replaces the old fx_buf_* null checks, and is the
   * safety precondition for the FxDelayLine _unsafe ops in the insert FX). */
  if (!fx.initialized) return;

  /* [CLICK-FREE] one-pole smoothed master gain (per-sample). */
  static float s_masterGain = 0.0f;
  static int   s_silentRun  = 0;   /* consecutive quiescent buffers → idle bypass */
  /* [TAIL-GATE] Amplitude-aware aux ring-out — NOT a fixed sample countdown. */
  static int   s_auxRingBuf  = 0;  /* >0 → keep aux + full chain alive           */
  static int   s_auxQuietBuf = 0;  /* consecutive buffers below peak threshold   */
  static int   s_auxCapBuf   = 0;  /* buffers since arm (hard safety cap)        */

  static constexpr int FX_IDLE_CONFIRM_BUFFERS = 8; /* ~93 ms silence before bypass */

  /* NVS save window: output silence during flash write. */
  if (g_saveArmed.load(std::memory_order_acquire)) {
    s_masterGain = 0.0f;
    s_auxRingBuf = 0;
    memset(out_buf, 0, frames * 2u * sizeof(int16_t));
    return;
  }

  const float mVol = std::min(1.0f, std::max(0.0f,
                                             masterVol.load(std::memory_order_relaxed)));

  const bool sendUp =
      fx.harpInsert.dly_send > 0.001f || fx.harpInsert.rev_send > 0.001f ||
      fx.seqInsert .dly_send > 0.001f || fx.seqInsert .rev_send > 0.001f ||
      fx.drumInsert.dly_send > 0.001f || fx.drumInsert.rev_send > 0.001f;

  /* Arm / extend aux tail while any engine is producing audio with sends up. */
  const int auxTailMax = (g_aux_mode.load(std::memory_order_relaxed) <= 1)
                           ? FX_TAIL_MAX_BUFFERS_LOW : FX_TAIL_MAX_BUFFERS_NORMAL;
  if (sendUp && (hOn || sOn || dOn)) {
    s_auxRingBuf  = auxTailMax;
    s_auxQuietBuf = 0;
    s_auxCapBuf   = 0;
  }

  /* ── Idle bypass — skip the whole chain once PROVABLY silent ───────────────
   * Quiescent = no engines, aux tail fully drained (amplitude gate), master
   * smoother at target.  s_auxRingBuf keeps inserts + aux draining until the
   * mixed output really falls below FX_TAIL_PEAK_THRESH for FX_TAIL_QUIET_BUFFERS,
   * so long delay/reverb tails on pads are not chopped at a fixed 2–3 s mark. */
  if (s_auxRingBuf > 0 || hOn || sOn || dOn)
    s_silentRun = 0;
  const bool quiescent = !hOn && !sOn && !dOn && s_auxRingBuf == 0 &&
                         fabsf(mVol - s_masterGain) < 1.0e-4f;
  s_silentRun = quiescent ? (s_silentRun < FX_IDLE_CONFIRM_BUFFERS
                               ? s_silentRun + 1 : FX_IDLE_CONFIRM_BUFFERS)
                          : 0;
  if (s_silentRun >= FX_IDLE_CONFIRM_BUFFERS) {
    memset(out_buf, 0, frames * 2u * sizeof(int16_t));
    return;
  }

  /* ── Pull master FX params from globals (cached coeff recompute) ───────── */
  fx.masterFx.drive     = tbDrive.load(std::memory_order_relaxed);
  fx.masterFx.tube_tone = tbTone.load(std::memory_order_relaxed);
  fx.masterFx.tube_mix  = tbMix.load(std::memory_order_relaxed);
  fx.masterFx.dj_mix    = djMix.load(std::memory_order_relaxed);
  fx.masterFx.dj_freq   = djFreq.load(std::memory_order_relaxed);
  fx.masterFx.dj_res    = djRes.load(std::memory_order_relaxed);
  fx.masterFx.eq_low    = masterEqLow.load(std::memory_order_relaxed);
  fx.masterFx.eq_high   = masterEqHigh.load(std::memory_order_relaxed);
  fx.drumFx.update_params();
  fx.masterFx.update_params();

  /* Per-instrument gain (mute flag + volume × [MIX-CAL] bus loudness trim).
   * The trim equalises the engines' intrinsic levels (drums were ~16× quieter
   * than the harp); it sits ahead of inserts/aux so the whole chain stays
   * balanced.  See MIX_TRIM_* / MIX_BUS_SUM in globals.h. */
  const float hG = mixHarpMute.load(std::memory_order_relaxed) ? 0.0f
                   : (EFFECT_INV32768 * MIX_TRIM_HARP * mixHarpVol.load(std::memory_order_relaxed));
  const float sG = mixSeqMute.load(std::memory_order_relaxed) ? 0.0f
                   : (EFFECT_INV32768 * MIX_TRIM_SEQ  * mixSeqVol.load(std::memory_order_relaxed));
  const float dG = mixDrumsMute.load(std::memory_order_relaxed) ? 0.0f
                   : (EFFECT_INV32768 * MIX_TRIM_DRUM * mixDrumsVol.load(std::memory_order_relaxed));

  /* [P4][FX-OPT3] Equal-power pan coefficients — recomputed only when a pan knob
   * actually moves (pan is user-input, rare).  Caching skips 6 cosf/sinf per
   * buffer (Flash-resident libm calls that can evict the I-cache on the hot core).
   * Float-equality against the cached value is fine: a race at worst leaves one
   * buffer with stale (inaudible) coefficients. */
  static float s_hPan = -2.f, s_sPan = -2.f, s_dPan = -2.f;
  static float s_hPL = 0.707f, s_hPR = 0.707f;
  static float s_sPL = 0.707f, s_sPR = 0.707f;
  static float s_dPL = 0.707f, s_dPR = 0.707f;
  {
    const float hPan = std::max(-1.f, std::min(1.f, mixHarpPan.load(std::memory_order_relaxed)));
    const float sPan = std::max(-1.f, std::min(1.f, mixSeqPan.load(std::memory_order_relaxed)));
    const float dPan = std::max(-1.f, std::min(1.f, mixDrumsPan.load(std::memory_order_relaxed)));
    if (hPan != s_hPan) { const float a = (hPan + 1.f) * 0.25f * (float)M_PI; s_hPL = cosf(a); s_hPR = sinf(a); s_hPan = hPan; }
    if (sPan != s_sPan) { const float a = (sPan + 1.f) * 0.25f * (float)M_PI; s_sPL = cosf(a); s_sPR = sinf(a); s_sPan = sPan; }
    if (dPan != s_dPan) { const float a = (dPan + 1.f) * 0.25f * (float)M_PI; s_dPL = cosf(a); s_dPR = sinf(a); s_dPan = dPan; }
  }
  const float hPL = s_hPL, hPR = s_hPR;
  const float sPL = s_sPL, sPR = s_sPR;
  const float dPL = s_dPL, dPR = s_dPR;

  /* ── Stage 3 setup: aux bus — ring while s_auxRingBuf > 0 (gate below) ──── */
  const bool runAux = sendUp && (s_auxRingBuf > 0);

  /* [SNAPSHOT] Latch insert sends + slot A/B params ONCE per buffer under patchMux.
   * Plain floats written from loadInsert / applyFxSend; per-sample reads risked a
   * cross-core mid-buffer tear.  Delay/dynamics envelope state stays in the slots. */
  InsertFxSnap hFxSnap, sFxSnap, dFxSnap;
  InsertDynSnap hDynSnap, sDynSnap, dDynSnap;
  float hDly, hRev, sDly, sRev, dDly, dRev;
  portENTER_CRITICAL(&patchMux);
  fx.harpInsert.captureSnap(hFxSnap, hDynSnap);
  fx.seqInsert .captureSnap(sFxSnap, sDynSnap);
  fx.drumInsert.captureSnap(dFxSnap, dDynSnap);
  hDly = fx.harpInsert.dly_send;
  hRev = fx.harpInsert.rev_send;
  sDly = fx.seqInsert .dly_send;
  sRev = fx.seqInsert .rev_send;
  dDly = fx.drumInsert.dly_send;
  dRev = fx.drumInsert.rev_send;
  portEXIT_CRITICAL(&patchMux);

  const float dT = masterAuxDlyTime.load(std::memory_order_relaxed);
  const float dF = masterAuxDlyFb.load(std::memory_order_relaxed);
  const float rS = masterAuxRevSize.load(std::memory_order_relaxed);
  const float rD = masterAuxRevDamp.load(std::memory_order_relaxed);
  /* [FX-OPT4] Latch aux delay/reverb coefficients once per buffer (cold). */
  if (runAux) fx.auxFx.setParams(dT, dF, rS, rD);

  /* ── [FX-OPT1] Single-pass: int16 → insert FX → sum/aux/master → int16 ────
   * The three instrument channels are independent (no cross-instrument data
   * dependency), so the old convert / insert / sum passes collapse into one loop
   * with each channel living in a register — eliminating the 3 × 512-float DRAM
   * scratch buffers and ~16 MB/s of redundant DRAM traffic.  Sample order within
   * each stateful insert/aux/master filter is unchanged → bit-identical output.
   * [FX-OPT7] out_p pointer-increment replaces the per-sample i*2 index multiply.
   * [FX-OPT8] peakOut is only needed by the tail gate, which runs only while the
   * aux ring is armed — so track it only then. */
  const bool trackPeak = (s_auxRingBuf > 0);
  float peakOut = 0.0f;
  int16_t* __restrict__ out_p = out_buf;
  for (size_t i = 0; i < frames; ++i, out_p += 2) {
    /* Stage 1+2: int16 → float gain → insert FX (all three in registers)
     * [FIX-GAIN-CLIP] Removed fx_clampf from the gain stage.  The old clamp
     * caused hard digital clipping BEFORE the insert FX:
     *   • Drums (MIX_TRIM_DRUM=3.00): max signal 32767/32768×3 ≈ 3.0 → hard-
     *     clipped to ±1.0, then DrumFX.process_mono received a already-clipped
     *     signal.  DrumFX uses fx_tanh saturation specifically designed to shape
     *     this level — the pre-clip made it sound like square-wave distortion
     *     instead of warm analogue saturation.
     *   • Seq (MIX_TRIM_SEQ=1.35): clips at 74% volume, capping the seq
     *     channel hard rather than letting the master limiter shape it smoothly.
     * The signal path is fully bounded: engine_soft_clip() guarantees int16
     * output (≤±32767), gain×(1/32768) keeps floats ≤ MIX_TRIM_x, DrumFX and
     * the master soft-knee limiter are the correct stages to handle headroom.
     * A guard against NaN/Inf is still provided by the int16 source type.    */
    float harpMono = (float)h_buf[i] * hG;
    float seqMono  = (float)s_buf[i] * sG;
    float drumMono = (float)d_buf[i] * dG;
    fx.harpInsert.process_mono(harpMono, hFxSnap, hDynSnap);
    fx.seqInsert .process_mono(seqMono,  sFxSnap, sDynSnap);
    fx.drumFx    .process_mono(drumMono);
    fx.drumInsert.process_mono(drumMono, dFxSnap, dDynSnap);

    /* Stage 3: aux send bus */
    float wetL = 0.0f, wetR = 0.0f;
    if (runAux) {
      const float smD = harpMono * hDly + seqMono * sDly + drumMono * dDly;
      const float smR = harpMono * hRev + seqMono * sRev + drumMono * dRev;
      fx.auxFx.process(smD, smR, true, wetL, wetR);
    }

    /* Stage 4+5: equal-power pan sum → master FX → smoothed gain → int16 out */
    float oL = (harpMono * hPL + seqMono * sPL + drumMono * dPL) * MIX_BUS_SUM + wetL * 0.6f;
    float oR = (harpMono * hPR + seqMono * sPR + drumMono * dPR) * MIX_BUS_SUM + wetR * 0.6f;

    oL = fx_denormal(oL);
    oR = fx_denormal(oR);

    fx.masterFx.process(oL, oR);
    s_masterGain += 0.0025f * (mVol - s_masterGain);
    oL *= s_masterGain;
    oR *= s_masterGain;

    out_p[0] = (int16_t)std::min(32767.f, std::max(-32768.f, oL * 32767.f));
    out_p[1] = (int16_t)std::min(32767.f, std::max(-32768.f, oR * 32767.f));

    if (trackPeak) {
      const float aL = fabsf(oL);
      const float aR = fabsf(oR);
      if (aL > peakOut) peakOut = aL;
      if (aR > peakOut) peakOut = aR;
    }
  }

  /* ── Amplitude tail gate (per buffer) ─────────────────────────────────────
   * Drain aux only after the mixed output stays below threshold for a short
   * window.  Hard cap prevents infinite CPU if feedback is pinned near unity. */
  if (s_auxRingBuf > 0) {
    s_auxCapBuf++;
    if (peakOut > FX_TAIL_PEAK_THRESH)
      s_auxQuietBuf = 0;
    else if (++s_auxQuietBuf >= FX_TAIL_QUIET_BUFFERS)
      s_auxRingBuf = 0;
    if (s_auxCapBuf >= auxTailMax)
      s_auxRingBuf = 0;
  }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * fx_free_buffers / fx_init — lifecycle helpers
 * ═══════════════════════════════════════════════════════════════════════════ */
void fx_free_buffers() {
  /* [FX-OPT1] No-op retained for API/teardown compatibility — the per-channel
   * DRAM scratch buffers were removed (single-pass hot loop keeps channels in
   * registers).  The FX sub-chain delay/comb/allpass buffers are owned by their
   * own objects and freed on re-init. */
}

bool fx_init() {
  resetFxProfiler(fx.metrics);

  const bool ok = fx.init();
  if (!ok) {
    Serial.printf("FX: chain init partial. PSRAM free=%u DRAM free=%u\n",
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
  }
  return ok;
}
