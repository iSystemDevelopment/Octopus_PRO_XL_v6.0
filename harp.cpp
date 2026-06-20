/* ═════════════════════════════════════════════════════════════════════════════
 * Octopus PRO XL v6.0.00 — Laser Harp Groovebox
 * © 2026 DIODAC ELECTRONICS / iSystem. All Rights Reserved.
 *
 * PROPRIETARY AND CONFIDENTIAL. Unauthorized copying, distribution, modification,
 * or use of this software or firmware, in whole or in part, is strictly prohibited
 * without prior written permission from DIODAC ELECTRONICS.
 * ═════════════════════════════════════════════════════════════════════════════
 * harp.cpp — v6.0.00  LASER-HARP INSTRUMENT — IMPLEMENTATION
 *
 * Dedicated laser-harp engine.  The per-sample DSP and all its math primitives
 * live in the anonymous namespace below — a fully private copy (sinf / noise /
 * wavetable / SVF / soft-clip), sharing NO symbols with the groovebox seq/drum
 * engine in synth_core.h.  This is the "full isolation" split: the harp owns its
 * whole signal path top-to-bottom.
 *
 * Fixes folded in vs the old synth.* path:
 *   • Single ADSR model — per-buffer float rates are the only source of truth;
 *     the dead integer release-step recompute and accent constants are gone.
 *   • Correct MIDI note ownership — the MIDI keyboard path allocates its own
 *     voice and stamps a MIDI owner (the old code mis-stamped a physical owner,
 *     so note-off could never find the voice).
 *   • Full-scale velocity — 0–127 → Q15 (was a half-scale shift that never
 *     reached the accent threshold).
 *   • D-BEAM MOD depth now reaches the pitch/wave LFO routes (was gated out).
 *   • [PD-2] Poly headroom: sum × (32768/√N) before h_soft_clip.
 *   • Voice release serialised under patchMux (recursive on-core), matching the
 *     trigger path, so env_state can't tear against the audio task.
 * ═════════════════════════════════════════════════════════════════════════════ */
#include "harp.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <atomic>
#include <esp_timer.h>

#include "arp.h"       /* arp::Engine — harp arpeggiator (POLY8/SOLO only)       */
#include "assets.h"    /* WAVE_TABLE_DIR, NUM_WAVE_TABLES                        */
#include "dbeam.h"     /* updateDbeamExpression, routeDbeamExpression            */
#include "patches.h"   /* SCALES, SCALES_NOTES                                   */
#include "laser.h"     /* BEAM_WINDOW_MIN/MAX, HUE_LEVEL_MAX, HUE_SCAN_HZ        */

/* ═══════════════════════════════════════════════════════════════════════════
 * PRIVATE DSP — fully isolated harp signal path (no shared symbols)
 * ═══════════════════════════════════════════════════════════════════════════ */
namespace {

/* SVF state clamp: must stay below SVF_CUT_MAX (globals) so a single iteration
 * cannot drive the state outside range under high resonance. */
constexpr int32_t H_SVF_SAT       = 24576;  /* 75 % of int16 full scale */
constexpr int32_t H_SOFTCLIP_KNEE = 26000;  /* transparent below, asymptotic above */

/* Accent threshold on Q15 velocity (≈ MIDI vel 93). */
constexpr uint16_t H_ACCENT_VEL_Q15 = 24000u;

/* [PD-2] Per-voice headroom: Q15 ≈ 32768/√n (1 voice unchanged). */
constexpr uint16_t HARP_POLY_GAIN_Q15[9] = {
  32768, 32768, 23170, 18919, 16384, 14654, 13377, 12361, 11585
};

/* ── 5th-order minimax sine, register-only (no flash sinf in the hot path). ── */
inline float IRAM_ATTR h_sinf(float x) {
  x -= 6.28318531f * floorf(x * (1.0f / 6.28318531f));
  if (x > 3.14159265f) x -= 6.28318531f;
  const float x2 = x * x;
  return x * (1.0f + x2 * (-0.16666667f + x2 * (0.00833333f - x2 * 0.00019841f)));
}

/* ── PRNG (private seed, AUDIO-CORE ONLY). ─────────────────────────────────────
 * [harp.md OPT-1] Plain static uint32 — no atomic.  h_noise() lives in this
 * anonymous namespace and is only ever called from harp_synth_fill_buf on Core 0,
 * so the old compare_exchange (a locked S32C1I RMW that never contends) was pure
 * overhead — up to 4096 CAS ops/buffer when noise is active.  Same LCG, identical
 * noise character.  If a cross-core path ever needs it, lock at the call site. */
inline int32_t IRAM_ATTR h_noise() {
  static uint32_t seed = 0x1BADF00Du;
  seed = seed * 1664525u + 1013904223u;
  return (int32_t)(seed >> 17) - 16384;
}

/* ── MIDI note → frequency. ────────────────────────────────────────────────────
 * [harp.md OPT-9] 128-entry DRAM LUT — eliminates the Flash-resident exp2f libm
 * call from every note-on (h_arm_voice).  The table is built exactly once by the
 * first caller; the C++11 magic-static guard makes that safe even if two cores
 * trigger a note simultaneously on the very first event. */
inline const float* h_freq_lut() {
  static const struct LutInit {
    float v[128];
    LutInit() { for (int n = 0; n < 128; ++n) v[n] = 440.0f * exp2f(((float)n - 69.0f) / 12.0f); }
  } lut;
  return lut.v;
}
inline float IRAM_ATTR h_note_to_freq(uint8_t note) {
  return h_freq_lut()[note & 127u];
}

/* ── 256-sample wavetable linear interpolation. ────────────────────────────── */
inline int32_t IRAM_ATTR h_interp(const int16_t* tbl, uint32_t phase) {
  const uint32_t idx  = (phase >> 24) & 255u;
  const uint32_t frac = (phase >> 16) & 255u;
  const int32_t  s1   = tbl[idx];
  return s1 + (((int32_t)(tbl[(idx + 1u) & 255u] - s1) * (int32_t)frac) >> 8);
}

/* ── SVF coefficient helpers. ──────────────────────────────────────────────── */
inline int32_t IRAM_ATTR h_svf_cutoff_hz(float norm_cutoff) {
  const float f = (norm_cutoff / 16383.0f) * 0.44f;
  return (int32_t)(2.0f * h_sinf(3.14159265f * std::min(0.49f, f)) * 32768.0f);
}
inline int32_t IRAM_ATTR h_svf_damping(uint16_t res_val) {
  return (int32_t)((1.0f - ((float)res_val / 16383.0f * 0.95f)) * 32768.0f);
}

/* ── Transparent per-engine output soft-clip. ──────────────────────────────── */
inline int16_t IRAM_ATTR h_soft_clip(int32_t x) {
  if (x > H_SOFTCLIP_KNEE) {
    const int32_t e = x - H_SOFTCLIP_KNEE;
    const int32_t span = 32767 - H_SOFTCLIP_KNEE;
    return (int16_t)(H_SOFTCLIP_KNEE + (int32_t)(((int64_t)span * e) / (e + span)));
  }
  if (x < -H_SOFTCLIP_KNEE) {
    const int32_t e = -x - H_SOFTCLIP_KNEE;
    const int32_t span = 32768 - H_SOFTCLIP_KNEE;
    return (int16_t)(-(H_SOFTCLIP_KNEE + (int32_t)(((int64_t)span * e) / (e + span))));
  }
  return (int16_t)x;
}

/* ── Arm one harp voice.  Caller holds patchMux. ───────────────────────────────
 * Only the fields the per-sample loop needs before the first prep pass are set
 * here; the ADSR rate steps are (re)derived every buffer in the fill loop, so
 * there is exactly ONE ADSR model.  active is published LAST (release) after a
 * fence so the audio task's relaxed load never sees a half-armed voice.        */
void IRAM_ATTR h_arm_voice(Voice* v, float freq, uint16_t velQ15) {
  /* [LIVE-PITCH] The step is stored at UNITY pitch (note frequency only).
   * masterPitch (M.TUNE) and harpPitchMult (BEND) are NO LONGER baked in here —
   * they are applied PER BUFFER in the fill loop (folded into the bend multiplier)
   * so a held note tracks the M.TUNE / BEND knobs LIVE instead of only picking up
   * the new value on the next note-on.  At unity (both = 1.0) the fill path is a
   * no-op, so there is zero cost in the common case.                            */

  /* Is this voice already sounding?  (Retrigger / steal of a ringing voice.)
   * If so, slamming env_level / phase / filter to zero makes the output drop
   * from its current level straight to silence in one sample — that's the click.
   * Keep phase, filter and the current envelope level and just re-enter ATTACK:
   * the new note ramps up FROM where it was → continuous signal, no click.      */
  const bool retrigger =
      v->active.load(std::memory_order_relaxed) &&
      v->env_state != EnvState::ENV_IDLE;

  v->step  = (uint32_t)((freq * 512.0f / (float)SAMPLE_RATE) * 8388608.0f);
  v->type  = (uint8_t)(harpLivePatch[(int)SynthParam::P_WAVEFORM]  % 25u);
  v->type2 = (uint8_t)(harpLivePatch[(int)SynthParam::P_OSC2_WAVE] % 25u);
  v->velocity    = velQ15;
  v->is_accented = (velQ15 > H_ACCENT_VEL_Q15);
  v->fast_release = false; /* a freshly armed voice always uses the patch release */

  if (!retrigger) {
    /* Fresh note from silence → clean reset (identical to current behaviour). */
    v->phase = v->phase2 = 0;
    v->svf_low = v->svf_band = 0;
    v->env_level.store(0u, std::memory_order_relaxed);
  }
  /* Retrigger: phase / phase2 / svf_low / svf_band / env_level LEFT AS-IS so the
   * waveform and filter stay continuous; only the stage flips back to ATTACK.    */

  v->env_state = EnvState::ENV_ATTACK;
  std::atomic_thread_fence(std::memory_order_release);
  v->active.store(true, std::memory_order_release);
}

} /* anonymous namespace */

constexpr uint32_t HARP_TICKS_PER_BEAT = 480u;

/* ── Harp arpeggiator (POLY8 / SOLO only; STRINGS excluded) ───────────────
 * Declared before harp_synth_fill_buf — fill_buf integrates arp ticks here. */
static arp::Engine s_harpArp;
static uint16_t    s_harpArpVelQ15 = 0;
static uint32_t    s_harpArpTick   = 0;
static float       s_harpArpFrac   = 0.f;

static inline uint8_t harpStringMidi(int stringIdx) {
  const int si       = harpScaleIndex.load(std::memory_order_relaxed) & (NUM_SCALES - 1);
  const int octShift = (int)octaveShift[0].load(std::memory_order_relaxed);
  return (uint8_t)std::min(127, std::max(0,
      (int)SCALES_NOTES[si][stringIdx & (MAX_STRINGS - 1)] + octShift * 12));
}

static inline bool harpArpActiveNow() {
  return harpArpEnabled.load(std::memory_order_relaxed)
         && currentPlayMode.load(std::memory_order_relaxed) != PlayMode::STRINGS;
}

static void harpArpReleaseVoice() {
  if (!s_harpArp.gate_open) return;
  portENTER_CRITICAL(&patchMux);
  if (harpVoices[HARP_SOLO_VOICE].active.load(std::memory_order_relaxed)) {
    const EnvState es = harpVoices[HARP_SOLO_VOICE].env_state;
    if (es != EnvState::ENV_RELEASE && es != EnvState::ENV_IDLE)
      harpVoices[HARP_SOLO_VOICE].env_state = EnvState::ENV_RELEASE;
  }
  s_harpArp.gate_open = false;
  s_harpArp.sounding_midi = -1;
  s_harpArp.sounding_row  = -1;
  portEXIT_CRITICAL(&patchMux);
}

/* When only one beam is held, expand to nearby scale degrees so Up/Down/UpDn/Rnd
 * are audible even without a multi-beam chord. */
static void harpArpExpandScaleMotif(int8_t* midi, int8_t* rows, int& n) {
  if (n != 1) return;
  const int si       = harpScaleIndex.load(std::memory_order_relaxed) & (NUM_SCALES - 1);
  const int octShift = (int)octaveShift[0].load(std::memory_order_relaxed);
  const int baseRow  = (int)rows[0];
  int8_t nm[arp::MAX_NOTES];
  int8_t nr[arp::MAX_NOTES];
  int cn = 0;
  for (int d = -1; d <= 2 && cn < arp::MAX_NOTES; ++d) {
    const int r = baseRow + d;
    if (r < 0 || r >= MAX_STRINGS) continue;
    nr[cn] = (int8_t)r;
    nm[cn] = (int8_t)std::min(127, std::max(0,
        (int)SCALES_NOTES[si][r] + octShift * 12));
    ++cn;
  }
  if (cn >= 2) {
    for (int i = 0; i < cn; ++i) {
      midi[i] = nm[i];
      rows[i] = nr[i];
    }
    n = cn;
  }
}

static void harpArpCommitLatch(int8_t* midi, int8_t* rows, int n, uint16_t velQ15) {
  if (n <= 0) {
    s_harpArp.reset();
    harpArpReleaseVoice();
    s_harpArpVelQ15 = 0;
    return;
  }
  harpArpExpandScaleMotif(midi, rows, n);
  s_harpArp.latchNotes(midi, rows, n);
  s_harpArpVelQ15 = velQ15;
  s_harpArp.last_period = UINT32_MAX;
}

static void harpArpStep(uint32_t tick) {
  if (!harpArpActiveNow()) return;
  if (s_harpArp.count <= 0) return;

  const uint8_t ui_rate = std::min<uint8_t>(3u, harpArpRate.load(std::memory_order_relaxed));
  const uint8_t ui_pat  = std::min<uint8_t>(3u, harpArpPattern.load(std::memory_order_relaxed));
  const uint8_t rate_i = arp::harpRateIndex(ui_rate);
  const uint8_t gate_i = arp::harpGateIndex(
      std::min<uint8_t>(3u, harpArpGate.load(std::memory_order_relaxed)));
  const uint8_t pat = arp::harpPatternIndex(ui_pat);

  static uint8_t s_cachedUiRate = 0xFFu;
  static uint8_t s_cachedUiPat  = 0xFFu;
  if (ui_rate != s_cachedUiRate) {
    s_cachedUiRate = ui_rate;
    s_harpArp.last_period = UINT32_MAX;
  }
  if (ui_pat != s_cachedUiPat) {
    s_cachedUiPat = ui_pat;
    s_harpArp.step_idx = 0;
  }

  const uint32_t period = arp::RATE_TICKS[rate_i];
  const uint32_t cur_period = tick / period;

  if (s_harpArp.gate_open && tick >= s_harpArp.gate_off_tick)
    harpArpReleaseVoice();

  if (cur_period == s_harpArp.last_period) return;
  s_harpArp.last_period = cur_period;

  if (s_harpArp.gate_open) harpArpReleaseVoice();

  const int8_t midi = s_harpArp.nextNote(pat);
  if (midi < 0) return;

  const int8_t row = s_harpArp.last_play_row;
  const uint16_t vel = s_harpArpVelQ15
                       ? s_harpArpVelQ15
                       : (uint16_t)((uint32_t)127u * (uint32_t)Q15_ONE / 127u);

  portENTER_CRITICAL(&patchMux);
  sanitizePatch(harpLivePatch);
  for (int v = 0; v < HARP_POLYPHONY; ++v) {
    if (v == HARP_SOLO_VOICE) continue;
    if (harpVoices[v].active.load(std::memory_order_relaxed) &&
        harpVoices[v].env_state != EnvState::ENV_IDLE) {
      harpVoices[v].fast_release = true;
      harpVoices[v].env_state    = EnvState::ENV_RELEASE;
    }
  }
  harpVoiceOwner[HARP_SOLO_VOICE] = (int16_t)(HV_PHYS_BASE + row);
  h_arm_voice(&harpVoices[HARP_SOLO_VOICE], h_note_to_freq((uint8_t)midi), vel);
  harpSoloKing.store(row, std::memory_order_relaxed);
  portEXIT_CRITICAL(&patchMux);

  s_harpArp.gate_open = true;
  s_harpArp.gate_off_tick = tick + (period * (uint32_t)arp::GATE_PCT[gate_i]) / 100u;
  s_harpArp.sounding_midi = midi;
  s_harpArp.sounding_row  = row;
}

static void harpArpIntegrate(uint32_t elapsedUs) {
  if (!harpArpActiveNow() || s_harpArp.count <= 0) return;
  const int32_t bpm = std::min<int32_t>(240, std::max<int32_t>(40, seqBpm.load(std::memory_order_relaxed)));
  s_harpArpFrac += (float)elapsedUs
                   * ((float)bpm * (float)HARP_TICKS_PER_BEAT * (1.0f / 60000000.0f));
  const uint32_t n = (uint32_t)s_harpArpFrac;
  s_harpArpFrac -= (float)n;
  for (uint32_t i = 0; i < n; ++i)
    harpArpStep(s_harpArpTick++);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * PER-SAMPLE DSP — 8-voice ADSR + dual-osc + SVF + LFO (HARP only)
 * ═══════════════════════════════════════════════════════════════════════════ */
bool IRAM_ATTR harp_synth_fill_buf(int16_t* out_buf, size_t frames) {
  /* [PERF] Idle early-out.  The per-sample outer loop below advances the LFO,
   * evaluates exp2f() pitch modulation, selects the wave-morph table and slews
   * the SVF coefficients EVERY frame — work that is pure waste when no voice is
   * sounding.  Without this guard an idle harp still grinds all 512 frames of
   * every DMA buffer (and can call Flash exp2f 48 kHz·times/s if the patch LFO
   * routes to pitch), inflating baseline load and stealing headroom from the
   * engine that IS playing.  Bail to silence the instant the engine is quiet. */
  /* [MUTE-GATE] If the harp is muted, skip the entire DSP pass — the FX stage
   * already multiplies this engine by gain 0, so rendering it is pure waste.
   * Free the voices too: the envelope state machine that ages/frees them lives
   * in the loop below, so leaving them un-rendered would let them pile up
   * "active forever" and starve allocation on unmute.  Mute = silence, unmute
   * starts fresh — standard mixer behaviour, and the click profile is identical
   * to the previous gain-0 path (both cut instantly). */
  if (mixHarpMute.load(std::memory_order_relaxed)) {
    for (int v = 0; v < HARP_POLYPHONY; ++v)
      harpVoices[v].active.store(false, std::memory_order_relaxed);
    s_harpArp.reset();
    memset(out_buf, 0, frames * sizeof(int16_t));
    return false;
  }

  const uint32_t arpElapsedUs =
      (uint32_t)((uint64_t)frames * 1000000ull / (uint64_t)SAMPLE_RATE);
  harpArpIntegrate(arpElapsedUs);

  bool any_active = false;
  int active_n = 0;
  for (int v = 0; v < HARP_POLYPHONY; ++v) {
    if (harpVoices[v].active.load(std::memory_order_relaxed)) {
      any_active = true;
      ++active_n;
    }
  }
  const bool arpHold = harpArpActiveNow() && s_harpArp.count > 0;
  if (!any_active && !arpHold) {
    memset(out_buf, 0, frames * sizeof(int16_t));
    return false;
  }

  /* Atomic patch snapshot: copy the 16 live words once under patchMux so the
   * whole buffer renders from a single coherent patch (no torn cutoff/res). */
  uint16_t lp[PARAMS_PER_PRESET];
  portENTER_CRITICAL(&patchMux);
  memcpy(lp, harpLivePatch, PARAMS_PER_PRESET * sizeof(uint16_t));
  portEXIT_CRITICAL(&patchMux);

  SynthGlobal& g = harp_synth_g;

  /* LFO (once per buffer). */
  const float lfo_hz = 0.1f + ((float)lp[(int)SynthParam::P_LFO_RATE] / 16383.0f) * 29.9f;
  g.lfo_step  = (uint32_t)((lfo_hz / (float)SAMPLE_RATE) * 4294967296.0f);
  g.lfo_depth = lp[(int)SynthParam::P_LFO_DEPTH];

  /* [LIVE-PITCH] MIDI pitch-bend wheel (Q16) × the two continuous tuning knobs.
   * masterPitch (M.TUNE) and harpPitchMult (BEND) used to be baked into v->step at
   * arm time, so a held note never tracked them.  Folding them into the per-buffer
   * bend multiplier makes them live for sounding voices.  At unity (1.0 × 1.0 ×
   * 65536) eff_pb == 65536 → has_bend is false → the per-sample 64-bit mul is
   * skipped exactly as before, so the common case stays free.                    */
  const uint32_t midi_pb_q16      = g.pitch_bend_q16.load(std::memory_order_relaxed);
  const float    live_pitch_mul   = masterPitch.load(std::memory_order_relaxed)
                                  * harpPitchMult.load(std::memory_order_relaxed);
  const uint32_t cached_pb        = (uint32_t)((float)midi_pb_q16 * live_pitch_mul);
  const int32_t  lfo_depth_cached = (int32_t)g.lfo_depth;
  const uint16_t lfo_route        = lp[(int)SynthParam::P_LFO_ROUTE] % 8u;

  /* STRINGS: physical string-vibration voice (micro-pitch vibrato below).  The
   * envelope now FOLLOWS THE PATCH ADSR — the note holds at its sustain level
   * while the beam stays broken (finger in the beam) and only enters RELEASE
   * (decaying per the patch's release time) once the beam clears.  A genuinely
   * plucky sound is achieved by a patch with sustain≈0 / short decay, i.e. the
   * ring-out is now "depending on the sound-bank preset setup", not forced. */
  const bool pluck_phys =
      (currentPlayMode.load(std::memory_order_relaxed) == PlayMode::STRINGS);

  const int     svf_passes       = g_svf_oversample.load(std::memory_order_relaxed);
  const int32_t cached_dbeam     = dbeam_svf_cutoff.load(std::memory_order_relaxed);
  const int32_t cached_dbeam_lfo = (int32_t)dbeam_mod_depth.load(std::memory_order_relaxed);

  /* ADSR rates (once per buffer). */
  const float atk = std::max(0.005f, (float)lp[(int)SynthParam::P_ATTACK]  / 16383.0f * 2.0f);
  const float dec = std::max(0.005f, (float)lp[(int)SynthParam::P_DECAY]   / 16383.0f * 3.0f);
  const float rel = std::max(0.010f, (float)lp[(int)SynthParam::P_RELEASE] / 16383.0f * 4.0f);
  const float sus = std::min(1.0f, std::max(0.0f,
                                            (float)lp[(int)SynthParam::P_SUSTAIN] / 16383.0f));

  const float    magic        = (float)0x7FFFFFFFu / (float)SAMPLE_RATE;
  const uint32_t c_atk_step   = (uint32_t)(magic / atk);
  const uint32_t c_dec_normal = (uint32_t)(magic / dec);
  const uint32_t c_dec_accent = (uint32_t)(magic / std::max(0.005f, dec * 0.3f));
  const uint32_t c_rel_step   = (uint32_t)(magic / rel);
  /* [SOLO] Fast staccato release for stolen voices (patch-independent). */
  const uint32_t c_rel_stacc  = (uint32_t)(magic / HARP_SOLO_STACCATO_REL_SEC);
  const uint32_t c_sus_level  = (uint32_t)(sus * (float)0x7FFFFFFFu);
  /* [STRINGS-FIX] Sustain is the patch's level in ALL modes now — STRINGS no
   * longer force-decays a held beam to silence.  Release on beam-clear (handled
   * in harpNoteOff → ENV_RELEASE at c_rel_step) does the ring-out. */
  const uint32_t eff_sus      = c_sus_level;

  /* Detune → osc2 frequency multiplier.
   * [harp.md OPT-3] exp2f is Flash-resident libm; P_DETUNE only changes on a
   * preset/knob edit.  Cache the result against the raw parameter word so the
   * Flash call fires at most once per detune change instead of every buffer. */
  static uint16_t s_det_param = 0xFFFFu;   /* force compute on first call */
  static float    s_det_semi  = 0.0f;
  static float    s_det_mult  = 1.0f;
  const uint16_t det_raw = lp[(int)SynthParam::P_DETUNE];
  if (det_raw != s_det_param) {
    /* [WS11-FIX] P_DETUNE is a CENTS control (App + docs: ±82 ct, centre 8192).
     * (det_raw-8192)/100 IS the cents value, so the ratio is exp2(cents/1200) —
     * NOT exp2(cents/12) (the semitone formula).  The old /12 detuned osc2 by up
     * to ~6.8 OCTAVES (2^(81.9/12) ≈ 114×) instead of ±82 cents, so any nudge
     * threw osc2 into aliasing garbage and detune appeared "dead".  /1200 gives
     * the intended musical ±4.8 % at the extremes. */
    s_det_semi  = ((int)det_raw - 8192) / 100.0f;     /* cents */
    s_det_mult  = exp2f(s_det_semi / 1200.0f);        /* cents → frequency ratio */
    s_det_param = det_raw;
  }
  const float detune_semi = s_det_semi;
  const float detune_mult = s_det_mult;
  /* [LOAD-SHED] The shedder disables the osc2 unison layer above ~85 % load
   * (g_osc2_enable=false) so dense detuned patches cost one oscillator/voice
   * instead of two — sheds before any note is dropped. */
  const bool osc2_on = g_osc2_enable.load(std::memory_order_relaxed);
  /* [WS11-FIX] osc2 also joins when its waveform DIFFERS from osc1 — not only when
   * detuned.  Previously osc_mix was forced to 0 unless detune was off-centre, so
   * selecting an OSC2 WAVE at the default (centre) detune was inaudible ("Wave
   * doesn't work").  Identical wave + zero detune still skips osc2 (no point
   * doubling an identical oscillator). */
  const bool osc2_wave_diff =
      ((int)lp[(int)SynthParam::P_OSC2_WAVE] % 25) != ((int)lp[(int)SynthParam::P_WAVEFORM] % 25);

  for (int v = 0; v < HARP_POLYPHONY; ++v) {
    if (!harpVoices[v].active.load(std::memory_order_relaxed)) continue;
    harpVoices[v].attack_step   = c_atk_step;
    harpVoices[v].decay_step    = harpVoices[v].is_accented ? c_dec_accent : c_dec_normal;
    harpVoices[v].release_step  = harpVoices[v].fast_release ? c_rel_stacc : c_rel_step;
    harpVoices[v].sustain_level = eff_sus;
    /* [harp.md OPT-8] Only compute the osc2 phase step when osc2 is actually
     * audible — when off, osc_mix_q15 is 0 and the osc2 block is skipped in the
     * sample loop, so the float×uint32 multiply would be wasted per voice. */
    const bool osc2_active = osc2_on && (detune_semi != 0.0f || osc2_wave_diff);
    if (osc2_active)
      harpVoices[v].step2_target = (uint32_t)((float)harpVoices[v].step * detune_mult);
    harpVoices[v].osc_mix_q15   = osc2_active ? (Q15_ONE / 2) : 0;
  }

  /* [STRINGS] Per-buffer vibrato phase increment for the audio micro-pitch. */
  if (pluck_phys)
    g.vib_step = (uint32_t)((HARP_STR_VIB_HZ / (float)SAMPLE_RATE) * 4294967296.0f);

  /* Filter coefficient slew (one-pole toward target, ~8-buffer glide). */
  const int32_t cut_target = h_svf_cutoff_hz((float)lp[(int)SynthParam::P_CUTOFF]);
  const int32_t res_target = h_svf_damping(lp[(int)SynthParam::P_RESONANCE]);
  if (g.cut_cur < 0) {
    g.cut_cur = cut_target;
    g.res_cur = res_target;
  } else {
    g.cut_cur += (cut_target - g.cut_cur) >> 3;
    g.res_cur += (res_target - g.res_cur) >> 3;
  }
  const int32_t live_cut   = g.cut_cur;
  const int32_t live_res   = g.res_cur;
  const uint16_t live_noise =
      (uint16_t)(((float)lp[(int)SynthParam::P_NOISE] / 16383.0f) * 32767.0f);
  const int base_wave  = (int)lp[(int)SynthParam::P_WAVEFORM]  % 25;
  const int base_wave2 = (int)lp[(int)SynthParam::P_OSC2_WAVE] % 25;

  /* ── [PERF] Loop-invariants hoisted out of the per-sample loop ───────────────
   * Routing flags and effective LFO depth depend only on per-buffer state, so
   * they are computed ONCE here instead of 512× inside the sample loop.  Skipping
   * recomputation removes a pile of redundant branches + a modulo per sample. */
  /* [harp.md OPT-10] Both addends are bounded to [0,16383] today (lfo_depth from
   * P_LFO_DEPTH, dbeam from a uint16 clamped to 16383), so no int32 overflow is
   * possible.  The per-operand clamp is a free per-buffer hardening that keeps the
   * sum well-defined even if either source's range is widened in future. */
  const int32_t lfo_depth_eff =
      std::min<int32_t>(16383,
          std::min<int32_t>(16383, lfo_depth_cached) +
          std::min<int32_t>(16383, cached_dbeam_lfo));
  const bool lfo_pitch   = (lfo_route == 0 || lfo_route == 3 || lfo_route == 5 || lfo_route == 6);
  const bool lfo_filter  = (lfo_route == 1 || lfo_route == 3 || lfo_route == 4 || lfo_route == 6);
  const bool lfo_wave    = (lfo_route == 2 || lfo_route == 4 || lfo_route == 5 || lfo_route == 6);
  const bool lfo_tremolo = (lfo_route == 7);
  const bool lfo_do_pitch = (lfo_pitch && lfo_depth_eff > 0);
  /* [harp.md OPT-4] Hoisted out of the per-voice loop: both operands are
   * per-buffer constants, so the OR is identical for all 8 voices × 512 samples.
   * Lifting it lets the compiler dead-code the pitch-mul branch when it is false. */
  const bool do_pitch_mul = lfo_do_pitch || pluck_phys;
  /* No pitch bend (1.0 in Q16) is the common case → skip the per-osc 64-bit mul. */
  const bool has_bend = (cached_pb != 65536u);
  /* When the LFO does not morph the wave the table pointer is constant for the
   * whole buffer; precompute both osc pointers once. */
  const int16_t* const wav_static = WAVE_TABLE_RAM[base_wave];
  const int16_t* const wav2_ptr   = WAVE_TABLE_RAM[base_wave2];

  /* [harp.md OPT-2] Live-voice bitmask built ONCE per buffer with 8 acquire loads
   * instead of one acquire load per voice per sample (≤ 4096/buffer).  The acquire
   * fence synchronises with Core 1's active.store(release) when a voice is armed;
   * that publish only needs to be observed once per buffer.  A voice armed
   * mid-buffer simply joins on the next buffer (loses ≤ a few attack samples, well
   * under the ≥5 ms attack floor — inaudible).  Voices that go idle below clear
   * their bit so they are skipped for the rest of the buffer. */
  uint8_t live_mask = 0;
  for (int v = 0; v < HARP_POLYPHONY; ++v) {
    if (harpVoices[v].active.load(std::memory_order_acquire))
      live_mask |= (uint8_t)(1u << v);
  }

  for (size_t i = 0; i < frames; ++i) {
    /* Triangle LFO. */
    g.lfo_phase += g.lfo_step;
    const uint32_t lp_phase = g.lfo_phase >> 16;
    const int32_t  lfo = (lp_phase < 32768u)
                           ? (int32_t)(lp_phase << 1) - 32768
                           : (int32_t)((65535u - lp_phase) << 1) - 32768;

    /* LFO→pitch multiplier.  The argument is tiny (|x| < 0.09 octaves), so a
     * 2nd-order exp2 polynomial is < 0.01 % off the libm exp2f — and avoids a
     * per-sample Flash-resident libm call (the real cost when MOD/D-BEAM is on). */
    float lfo_pitch_mul = 1.0f;
    if (lfo_do_pitch) {
      const float xp = (float)((lfo * lfo_depth_eff) >> 15) * (1.0f / (12.0f * 16383.0f));
      lfo_pitch_mul = 1.0f + xp * (0.6931472f + xp * 0.2402265f);
    }

    /* Wave morph table — recomputed only when the LFO actually morphs the wave. */
    const int16_t* wav_ptr = wav_static;
    if (lfo_wave) {
      const int wave_morph = (int)((lfo * lfo_depth_eff) >> 20);
      int morph_idx = (base_wave + wave_morph) % 25;
      if (morph_idx < 0) morph_idx += 25;
      wav_ptr = WAVE_TABLE_RAM[morph_idx];
    }

    /* [PERF] SVF cutoff + tremolo gain are identical for every voice this sample
     * (they depend on the shared LFO, not on the voice) → compute them ONCE here
     * instead of 8× inside the voice loop. */
    int32_t cut = live_cut;
    if (cached_dbeam > 0)
      cut = std::min(SVF_CUT_MAX, cut + cached_dbeam);
    if (lfo_filter && lfo_depth_eff > 0)
      cut = std::min(SVF_CUT_MAX, std::max<int32_t>(0,
                     cut + ((lfo * lfo_depth_eff) >> 3)));

    int32_t trem_mul = 32768; /* Q15 unity */
    if (lfo_tremolo && lfo_depth_eff > 0)
      trem_mul = 32768 - (((lfo + 32768) * lfo_depth_eff) >> 16);

    /* [STRINGS] Shared micro-pitch wobble for this sample (fundamental + 3rd
     * harmonic, matching the galvo waveshape).  Per-voice depth scaling by the
     * voice envelope happens in the oscillator section below. */
    float strings_wob = 0.0f;
    if (pluck_phys && live_mask) {
      g.vib_phase += g.vib_step;
      const float vph = (float)(g.vib_phase >> 16) * (6.28318531f / 65536.0f);
      strings_wob = h_sinf(vph) + 0.28f * h_sinf(vph * 3.0f);
    }

    int32_t acc = 0;

    for (int v = 0; v < HARP_POLYPHONY; ++v) {
      /* [harp.md OPT-2] Cheap bitwise test — the acquire publish already happened
       * once before the sample loop when live_mask was built. */
      if (!(live_mask & (uint8_t)(1u << v))) continue;

      EnvState es = harpVoices[v].env_state;
      uint32_t el = harpVoices[v].env_level.load(std::memory_order_relaxed);
      const uint32_t sl = harpVoices[v].sustain_level;
      bool act = true;

      switch (es) {
        case EnvState::ENV_ATTACK:
          if (0x7FFFFFFFu - el <= harpVoices[v].attack_step) {
            el = 0x7FFFFFFFu;
            es = EnvState::ENV_DECAY;
          } else {
            el += harpVoices[v].attack_step;
          }
          break;

        case EnvState::ENV_DECAY:
          if (el > sl + harpVoices[v].decay_step) {
            el -= harpVoices[v].decay_step;
          } else {
            el = sl;
            if (sl == 0) { es = EnvState::ENV_IDLE; act = false; }
            else         { es = EnvState::ENV_SUSTAIN; }
          }
          break;

        case EnvState::ENV_SUSTAIN:
          /* Hold until release.  In STRINGS pluck sl == 0 so voices pass
           * through DECAY straight to IDLE and never linger here. */
          break;

        case EnvState::ENV_RELEASE:
          if (el > harpVoices[v].release_step) {
            el -= harpVoices[v].release_step;
          } else {
            el = 0;
            es = EnvState::ENV_IDLE;
            act = false;
          }
          break;

        default: break;
      }

      harpVoices[v].env_state = es;
      harpVoices[v].env_level.store(el, std::memory_order_relaxed);

      if (!act) {
        harpVoices[v].active.store(false, std::memory_order_release);
        live_mask &= (uint8_t)~(1u << v);  /* skip for remaining samples this buffer */
        continue;
      }

      const int32_t amp_q16 = (int32_t)(el >> 16); /* 0–32767 */

      /* Combined pitch multiplier: LFO pitch (if routed) × STRINGS micro-vibrato
       * (if in STRINGS mode), the latter scaled by THIS voice's envelope so the
       * wobble blooms on the pluck and decays as it rings out.  One float mul per
       * voice — same cost as the old LFO-only path when either is active. */
      float pitch_mul = lfo_do_pitch ? lfo_pitch_mul : 1.0f;
      if (pluck_phys) {
        const float env01 = (float)amp_q16 * (1.0f / 32767.0f);
        pitch_mul *= 1.0f + strings_wob * (HARP_STR_VIB_DEPTH * env01);
      }

      /* Oscillator 1 + pitch-bend + pitch modulation.  64-bit bend mul skipped when no bend. */
      uint32_t mstep = harpVoices[v].step;
      if (has_bend)      mstep = (uint32_t)(((uint64_t)mstep * cached_pb) >> 16);
      if (do_pitch_mul)  mstep = (uint32_t)((float)mstep * pitch_mul);
      harpVoices[v].phase += mstep;
      int32_t raw = h_interp(wav_ptr, harpVoices[v].phase);

      /* Oscillator 2 blend (only when detuned / different waveform). */
      if (harpVoices[v].osc_mix_q15 > 0) {
        uint32_t ms2 = harpVoices[v].step2_target;
        if (has_bend)      ms2 = (uint32_t)(((uint64_t)ms2 * cached_pb) >> 16);
        if (do_pitch_mul)  ms2 = (uint32_t)((float)ms2 * pitch_mul);
        harpVoices[v].phase2 += ms2;
        const int32_t osc2 = h_interp(wav2_ptr, harpVoices[v].phase2);
        raw = (raw * (Q15_ONE - harpVoices[v].osc_mix_q15)
               + osc2 * harpVoices[v].osc_mix_q15) >> 15;
      }

      /* Noise mix. */
      if (live_noise > 0) {
        raw = (raw * (int32_t)(32767 - live_noise) + h_noise() * (int32_t)live_noise) >> 15;
      }

      /* SVF low-pass — cutoff was computed once per sample above.
       * [harp.md RISK-1] cut*hp is promoted to int64 before the >>15: |hp| can
       * reach ~81919 and SVF_CUT_MAX is 31000, so cut*hp (~2.54e9) overflows a
       * signed int32 (max 2.147e9).  The overflow corrupted the shifted result
       * and destabilised the filter instead of clamping cleanly.  On Xtensa the
       * 32×32→64 mul is a single mull/mulh pair (~2 extra cycles). */
      for (int s = 0; s < svf_passes; ++s) {
        const int32_t hp = raw
                           - harpVoices[v].svf_low
                           - ((live_res * harpVoices[v].svf_band) >> 15);
        harpVoices[v].svf_band = std::max(-H_SVF_SAT, std::min(H_SVF_SAT,
            harpVoices[v].svf_band + (int32_t)(((int64_t)cut * hp) >> 15)));
        harpVoices[v].svf_low  = std::max(-H_SVF_SAT, std::min(H_SVF_SAT,
            harpVoices[v].svf_low + (int32_t)(((int64_t)cut * harpVoices[v].svf_band) >> 15)));
      }

      /* Tremolo (LFO route 7) — multiplier precomputed once per sample. */
      const int32_t fa = (trem_mul == 32768) ? amp_q16 : ((amp_q16 * trem_mul) >> 15);

      /* Accumulate (int64 intermediate prevents 8-voice overflow).
       * [harp.md OPT-5] Parenthesised explicitly: vel_weighted is the filtered,
       * velocity-scaled signal (svf_low × Q15 velocity >> 15); contrib then folds
       * in the tremolo/envelope amplitude.  Same math as before, no precedence
       * ambiguity (the bare `* velocity >> 15` read as shifting velocity). */
      const int64_t vel_weighted =
          ((int64_t)harpVoices[v].svf_low * (int32_t)harpVoices[v].velocity) >> 15;
      const int64_t contrib = vel_weighted * (int64_t)fa;
      acc += (int32_t)(contrib >> 16);
    }

    /* [PD-2] Poly headroom: RMS-style ÷√N — closer perceived level 1 vs few strings. */
    if (active_n > 1 && active_n < 9)
      acc = (int32_t)(((int64_t)acc * HARP_POLY_GAIN_Q15[active_n]) >> 15);

    out_buf[i] = h_soft_clip(acc);
  }
  return true;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * VOICE LIFECYCLE
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Release one harp voice → ENV_RELEASE.  Serialised under patchMux (recursive
 * on-core) so env_state cannot tear against the audio task or the trigger path.
 * release_step is left to the next fill pass (single ADSR model).             */
void IRAM_ATTR harpReleaseVoice(int vi) {
  if (vi < 0 || vi >= HARP_POLYPHONY) return;
  portENTER_CRITICAL(&patchMux);
  if (harpVoices[vi].active.load(std::memory_order_relaxed)) {
    const EnvState es = harpVoices[vi].env_state;
    if (es != EnvState::ENV_RELEASE && es != EnvState::ENV_IDLE)
      harpVoices[vi].env_state = EnvState::ENV_RELEASE;
  }
  portEXIT_CRITICAL(&patchMux);
}

/* ── SOLO note-priority stack ─────────────────────────────────────────────────
 * SOLO is MONOPHONIC with bidirectional LAST-NOTE priority.  All beams share
 * HARP_SOLO_VOICE; the newest beam-break SOUNDS while older still-held beams stay
 * silent but REMEMBERED in this stack (oldest→newest).  Releasing the sounding
 * beam falls back to the most-recent beam still physically held ("older becomes
 * latest"), regardless of pitch or scan direction.  Re-touching a held beam moves
 * it back to the top.  The laser scan keeps stringActive[] as pure physical-hold
 * truth and no longer force-clears stolen strings, so THIS stack is the single
 * owner of priority — which is what fixes notes "sticking" / going dead when the
 * newest beam is lifted with older beams still down.
 * All access is under patchMux (portMUX is non-recursive — never nest enters). */
static int     g_soloStr[MAX_STRINGS];
static uint8_t g_soloVel[MAX_STRINGS];
static int     g_soloN = 0;

/* POLY8 + ARP: all held beams (as-played order) feed the arp latch. */
static int     g_polyArpStr[MAX_STRINGS];
static uint8_t g_polyArpVel[MAX_STRINGS];
static int     g_polyArpN = 0;

static inline void polyArpStackRemove(int s) {
  for (int i = 0; i < g_polyArpN; ++i) {
    if (g_polyArpStr[i] != s) continue;
    for (int j = i; j < g_polyArpN - 1; ++j) {
      g_polyArpStr[j] = g_polyArpStr[j + 1];
      g_polyArpVel[j] = g_polyArpVel[j + 1];
    }
    --g_polyArpN;
    return;
  }
}
static inline void polyArpStackPush(int s, uint8_t v) {
  polyArpStackRemove(s);
  if (g_polyArpN < MAX_STRINGS) {
    g_polyArpStr[g_polyArpN] = s;
    g_polyArpVel[g_polyArpN] = v;
    ++g_polyArpN;
  }
}

static inline void soloStackRemove(int s) {
  for (int i = 0; i < g_soloN; ++i) {
    if (g_soloStr[i] != s) continue;
    for (int j = i; j < g_soloN - 1; ++j) {
      g_soloStr[j] = g_soloStr[j + 1];
      g_soloVel[j] = g_soloVel[j + 1];
    }
    --g_soloN;
    return;
  }
}
static inline void soloStackPush(int s, uint8_t v) {
  soloStackRemove(s);                 /* re-touch → promote to newest */
  if (g_soloN < MAX_STRINGS) {
    g_soloStr[g_soloN] = s;
    g_soloVel[g_soloN] = v;
    ++g_soloN;
  }
}

static void harpArpRebuildFromSoloStack() {
  int8_t midi[arp::MAX_NOTES];
  int8_t rows[arp::MAX_NOTES];
  const int n = std::min(g_soloN, arp::MAX_NOTES);
  uint16_t velQ15 = 0;
  for (int i = 0; i < n; ++i) {
    rows[i] = (int8_t)g_soloStr[i];
    midi[i] = (int8_t)harpStringMidi(g_soloStr[i]);
    velQ15 = (uint16_t)((uint32_t)g_soloVel[i] * (uint32_t)Q15_ONE / 127u);
  }
  harpArpCommitLatch(midi, rows, n, velQ15);
}

static void harpArpRebuildFromPolyStack() {
  int8_t midi[arp::MAX_NOTES];
  int8_t rows[arp::MAX_NOTES];
  const int n = std::min(g_polyArpN, arp::MAX_NOTES);
  uint16_t velQ15 = 0;
  for (int i = 0; i < n; ++i) {
    rows[i] = (int8_t)g_polyArpStr[i];
    midi[i] = (int8_t)harpStringMidi(g_polyArpStr[i]);
    velQ15 = (uint16_t)((uint32_t)g_polyArpVel[i] * (uint32_t)Q15_ONE / 127u);
  }
  harpArpCommitLatch(midi, rows, n, velQ15);
}

/* Arm the shared SOLO voice for one string (scale/octave map + click-free
 * retrigger).  MUST be called with patchMux already held. */
static inline void soloArmVoiceLocked(int stringIdx, uint8_t vel) {
  const int si       = harpScaleIndex.load(std::memory_order_relaxed) & (NUM_SCALES - 1);
  const int octShift = (int)octaveShift[0].load(std::memory_order_relaxed);
  const uint8_t midiNote = (uint8_t)std::min(127, std::max(0,
      (int)SCALES_NOTES[si][stringIdx & (MAX_STRINGS - 1)] + octShift * 12));
  const uint16_t velQ15 = (uint16_t)((uint32_t)std::min<uint8_t>(127, vel)
                                     * (uint32_t)Q15_ONE / 127u);
  sanitizePatch(harpLivePatch);
  /* Staccato-steal any OTHER voices still ringing (only matters right after a
   * POLY→SOLO switch; SOLO otherwise touches HARP_SOLO_VOICE alone). */
  for (int v = 0; v < HARP_POLYPHONY; ++v) {
    if (v == HARP_SOLO_VOICE) continue;
    if (harpVoices[v].active.load(std::memory_order_relaxed) &&
        harpVoices[v].env_state != EnvState::ENV_IDLE) {
      harpVoices[v].fast_release = true;
      harpVoices[v].env_state    = EnvState::ENV_RELEASE;
    }
  }
  harpVoiceOwner[HARP_SOLO_VOICE] = (int16_t)(HV_PHYS_BASE + stringIdx);
  h_arm_voice(&harpVoices[HARP_SOLO_VOICE], h_note_to_freq(midiNote), velQ15);
  harpSoloKing.store(stringIdx, std::memory_order_relaxed); /* render: glow this beam */
}

void IRAM_ATTR harpNoteOn(int stringIdx, uint8_t vel) {
  if (stringIdx < 0 || stringIdx >= MAX_STRINGS) return;

  const PlayMode pm = currentPlayMode.load(std::memory_order_relaxed);
  const uint16_t velQ15 = (uint16_t)((uint32_t)std::min<uint8_t>(127, vel)
                                     * (uint32_t)Q15_ONE / 127u);

  /* Harp ARP: latch all held notes; voice driven by arp clock (not direct arm). */
  if (harpArpActiveNow() && pm != PlayMode::STRINGS) {
    portENTER_CRITICAL(&patchMux);
    if (pm == PlayMode::SOLO) {
      soloStackPush(stringIdx, vel);
      harpArpRebuildFromSoloStack();
    } else {
      polyArpStackPush(stringIdx, vel);
      harpArpRebuildFromPolyStack();
    }
    portEXIT_CRITICAL(&patchMux);
    return;
  }

  /* SOLO: monophonic last-note priority via the held stack (see above). */
  if (pm == PlayMode::SOLO) {
    portENTER_CRITICAL(&patchMux);
    soloStackPush(stringIdx, vel);          /* newest = king */
    soloArmVoiceLocked(stringIdx, vel);
    portEXIT_CRITICAL(&patchMux);
    return;
  }

  /* POLY8 / STRINGS — dedicated per-string voice.  Fine pitch is the continuous
   * harpPitchMult applied in h_arm_voice (manual bend/tune), not a note offset. */
  const int si       = harpScaleIndex.load(std::memory_order_relaxed) & (NUM_SCALES - 1);
  const int octShift = (int)octaveShift[0].load(std::memory_order_relaxed);
  const uint8_t midiNote = (uint8_t)std::min(127, std::max(0,
      (int)SCALES_NOTES[si][stringIdx & (MAX_STRINGS - 1)] + octShift * 12));
  const int vi = stringIdx % HARP_POLYPHONY;

  portENTER_CRITICAL(&patchMux);
  sanitizePatch(harpLivePatch); /* last-resort clamp if patch bypassed recall */
  harpVoiceOwner[vi] = (int16_t)(HV_PHYS_BASE + stringIdx);
  h_arm_voice(&harpVoices[vi], h_note_to_freq(midiNote), velQ15);
  portEXIT_CRITICAL(&patchMux);
}

void IRAM_ATTR harpNoteOff(int stringIdx) {
  if (stringIdx < 0 || stringIdx >= MAX_STRINGS) return;

  const PlayMode pm = currentPlayMode.load(std::memory_order_relaxed);

  if (harpArpActiveNow() && pm != PlayMode::STRINGS) {
    portENTER_CRITICAL(&patchMux);
    if (pm == PlayMode::SOLO) {
      soloStackRemove(stringIdx);
      if (g_soloN > 0) harpArpRebuildFromSoloStack();
      else {
        s_harpArp.reset();
        harpArpReleaseVoice();
        s_harpArpVelQ15 = 0;
      }
    } else {
      polyArpStackRemove(stringIdx);
      if (g_polyArpN > 0) harpArpRebuildFromPolyStack();
      else {
        s_harpArp.reset();
        harpArpReleaseVoice();
        s_harpArpVelQ15 = 0;
      }
    }
    portEXIT_CRITICAL(&patchMux);
    return;
  }

  /* SOLO: pop from the held stack.  If the lifted beam was the one sounding,
   * fall back to the most-recent beam still held; if none remain, release. */
  if (currentPlayMode.load(std::memory_order_relaxed) == PlayMode::SOLO) {
    portENTER_CRITICAL(&patchMux);
    const bool wasKing = (g_soloN > 0 && g_soloStr[g_soloN - 1] == stringIdx);
    soloStackRemove(stringIdx);
    if (g_soloN > 0) {
      if (wasKing) soloArmVoiceLocked(g_soloStr[g_soloN - 1], g_soloVel[g_soloN - 1]);
      /* else: an older silent beam was lifted — king keeps sounding, no change. */
    } else {
      /* No beams held — release the shared voice.  Deliberately DON'T clear
       * harpSoloKing here: it is now a pure render hint, and leaving it pointing
       * at the just-lifted beam lets laserForString() show the white→scale-colour
       * RELEASE fade on that beam over the preset's release time (the envNorm gate
       * hides it once the voice goes idle).  It's overwritten on the next arm and
       * cleared on mode-switch/panic via harpAllNotesOff(). */
      if (harpVoiceOwner[HARP_SOLO_VOICE] >= HV_PHYS_BASE)
        harpVoiceOwner[HARP_SOLO_VOICE] = HV_FREE;
      if (harpVoices[HARP_SOLO_VOICE].active.load(std::memory_order_relaxed)) {
        const EnvState es = harpVoices[HARP_SOLO_VOICE].env_state;
        if (es != EnvState::ENV_RELEASE && es != EnvState::ENV_IDLE)
          harpVoices[HARP_SOLO_VOICE].env_state = EnvState::ENV_RELEASE;
      }
    }
    portEXIT_CRITICAL(&patchMux);
    return;
  }

  /* POLY8 / STRINGS — release the owning per-string voice. */
  const int vi = stringIdx % HARP_POLYPHONY;
  portENTER_CRITICAL(&patchMux);
  const bool owns = (harpVoiceOwner[vi] == (int16_t)(HV_PHYS_BASE + stringIdx));
  if (owns) harpVoiceOwner[vi] = HV_FREE;
  if (owns && harpVoices[vi].active.load(std::memory_order_relaxed)) {
    const EnvState es = harpVoices[vi].env_state;
    if (es != EnvState::ENV_RELEASE && es != EnvState::ENV_IDLE)
      harpVoices[vi].env_state = EnvState::ENV_RELEASE;
  }
  portEXIT_CRITICAL(&patchMux);
}

void harpAllNotesOff() {
  portENTER_CRITICAL(&patchMux);
  for (int v = 0; v < HARP_POLYPHONY; ++v) {
    if (harpVoices[v].active.load(std::memory_order_relaxed))
      harpVoices[v].env_state = EnvState::ENV_RELEASE;
    harpVoiceOwner[v] = HV_FREE;
  }
  g_soloN = 0; /* drop the SOLO held stack — no stale king across a mode switch */
  g_polyArpN = 0;
  harpSoloKing.store(-1, std::memory_order_relaxed);
  s_harpArp.reset();
  s_harpArpVelQ15 = 0;
  portEXIT_CRITICAL(&patchMux);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * MIDI KEYBOARD PATH — polyphonic, free-voice allocation (POLY8)
 * ═══════════════════════════════════════════════════════════════════════════ */
void IRAM_ATTR harpMidiNoteOn(uint8_t note, uint8_t vel) {
  const uint16_t velQ15 = (uint16_t)((uint32_t)std::min<uint8_t>(127, vel)
                                     * (uint32_t)Q15_ONE / 127u);

  portENTER_CRITICAL(&patchMux);
  sanitizePatch(harpLivePatch);

  int vi = -1;
  for (int v = 0; v < HARP_POLYPHONY; ++v) {
    if (!harpVoices[v].active.load(std::memory_order_relaxed)) { vi = v; break; }
  }
  if (vi < 0) { /* steal a MIDI-owned voice if no free slot */
    for (int v = 0; v < HARP_POLYPHONY; ++v) {
      if (hvIsMidi(harpVoiceOwner[v])) { vi = v; break; }
    }
  }
  if (vi >= 0) {
    harpVoiceOwner[vi] = (int16_t)note;             /* MIDI owner (0–127) */
    h_arm_voice(&harpVoices[vi], h_note_to_freq(note), velQ15);
  }
  portEXIT_CRITICAL(&patchMux);
}

void IRAM_ATTR harpMidiNoteOff(uint8_t note) {
  portENTER_CRITICAL(&patchMux);
  for (int v = 0; v < HARP_POLYPHONY; ++v) {
    if (hvIsMidi(harpVoiceOwner[v]) && harpVoiceOwner[v] == (int16_t)note) {
      harpVoiceOwner[v] = HV_FREE;
      if (harpVoices[v].active.load(std::memory_order_relaxed)) {
        const EnvState es = harpVoices[v].env_state;
        if (es != EnvState::ENV_RELEASE && es != EnvState::ENV_IDLE)
          harpVoices[v].env_state = EnvState::ENV_RELEASE;
      }
    }
  }
  portEXIT_CRITICAL(&patchMux);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * PLAY MODE
 * ═══════════════════════════════════════════════════════════════════════════ */
void harpSetPlayMode(PlayMode m) {
  if (m == currentPlayMode.load(std::memory_order_relaxed)) return;
  if (m == PlayMode::STRINGS)
    harpArpEnabled.store(false, std::memory_order_relaxed);
  currentPlayMode.store(m, std::memory_order_release);
  harpAllNotesOff();                               /* drop voices/owners/solo stack */
  /* [STUCK-FIX] Also clear the laser's per-string physical-hold detection state
   * (lives in laser_sweep_task) so a beam held across the switch doesn't stay a
   * silent stuck "active" string in the new mode. */
  g_beamClearReq.store(true, std::memory_order_release);
  displayDirty.store(true, std::memory_order_relaxed);
}
PlayMode harpPlayMode() {
  return currentPlayMode.load(std::memory_order_relaxed);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * D-BEAM EXPRESSION PUMP  (consumes dbeam.cpp)
 * updateDbeamExpression() returns the amplitude-follower as a full-scale 0..1
 * float; routeDbeamExpression() applies it to CUTOFF / MOD / VOLUME per
 * currentDbeamRoute at each target's native resolution (no MIDI quantisation).
 * ═══════════════════════════════════════════════════════════════════════════ */
void IRAM_ATTR harpPumpExpression() {
  routeDbeamExpression(updateDbeamExpression(-1));
}

/* ═══════════════════════════════════════════════════════════════════════════
 * STRINGS STRING-VIBRATION EMULATION  (control owned by harp)
 * Returns the galvo target for a string: nominal position + ADSR-scaled
 * physical wobble (fundamental + 3rd harmonic).  envNorm → 0 on release, so the
 * vibration decays to stop with the note (no separate timer).  Returns nomPos
 * unchanged when the owning voice is silent.
 * ═══════════════════════════════════════════════════════════════════════════ */
/* [harp.md OPT-7] Per-string running phase accumulator so the visual vibrato no
 * longer needs `nowUs % period` — Xtensa has no hardware integer divider, so each
 * 32-bit `%` was a ~20–40-cycle software routine (×1000 laser calls/sec).  The
 * accumulator advances by elapsed-time/period and wraps with one float subtract.
 * Micro-continuity matters here, not absolute phase accuracy, so the changing
 * period per call is fine. */
static float    s_vib_phase[HARP_POLYPHONY]   = {};
static uint32_t s_vib_last_us[HARP_POLYPHONY] = {};

uint16_t IRAM_ATTR harpStringVibratoTarget(int stringIdx, uint16_t nomPos, int scaleIdx) {
  const int vi = stringIdx % HARP_POLYPHONY;
  if (!harpVoices[vi].active.load(std::memory_order_relaxed)) return nomPos;
  const uint32_t lvl = harpVoices[vi].env_level.load(std::memory_order_relaxed);
  if (lvl == 0u) return nomPos;

  const float envNorm  = (float)lvl / 2147483647.0f;
  const float decayNorm = (float)harpLivePatch[(int)SynthParam::P_DECAY] * INV_16383_UI;

  float vibDepth = (float)PLUCK_VIB_MIN_AMP
                 + decayNorm * (float)(PLUCK_VIB_MAX_AMP - PLUCK_VIB_MIN_AMP);
  const float pitchScale = 1.30f - 0.60f *
      ((float)stringIdx / (float)std::max(1, SCALES[scaleIdx].numActiveStrings - 1));
  vibDepth *= pitchScale;

  const float    visTension = 1.0f + (PLUCK_TENSION_K * 12.0f * envNorm * envNorm);
  const uint32_t period     = (uint32_t)((float)PLUCK_VIB_BASE_US * pitchScale / visTension);

  /* [harp.md OPT-7] Phase from a per-string accumulator: one float add, one
   * compare, at most one subtract — no integer division. */
  const uint32_t nowUs = (uint32_t)esp_timer_get_time();
  const uint32_t dtUs  = nowUs - s_vib_last_us[vi];   /* unsigned wrap-safe delta */
  s_vib_last_us[vi]    = nowUs;
  const float period_f = (float)(period ? period : 1u);
  s_vib_phase[vi] += (float)dtUs / period_f;
  if (s_vib_phase[vi] >= 1.0f)
    s_vib_phase[vi] -= floorf(s_vib_phase[vi]);
  const float vibPhase = s_vib_phase[vi];

  /* Fundamental + 3rd harmonic for a realistic pluck waveshape. */
  const float wob = h_sinf(vibPhase * 6.28318531f)
                  + 0.28f * h_sinf(vibPhase * 18.84955592f);

  return (uint16_t)std::min(
      std::max((int32_t)(nomPos + (int32_t)(wob * vibDepth * envNorm)),
               (int32_t)BEAM_WINDOW_MIN + 20),
      (int32_t)BEAM_WINDOW_MAX - 20);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * HUE ADSR ENGINE  (control owned by harp; laser renders the colour)
 * Driven once per string visit (~125 Hz scan) by laser_sweep_task.
 * ═══════════════════════════════════════════════════════════════════════════ */
void IRAM_ATTR harpHueNoteOn(int stringIdx) {
  stringHueEnv[stringIdx & 7].state.store(EnvState::ENV_ATTACK, std::memory_order_relaxed);
}
void IRAM_ATTR harpHueNoteOff(int stringIdx) {
  stringHueEnv[stringIdx & 7].state.store(EnvState::ENV_RELEASE, std::memory_order_relaxed);
}

/* [harp.md OPT-6] Hue ADSR step sizes derive from the hueAttack/Decay/Release/
 * Sustain atomics, which only change when the user edits them.  Cache the
 * computed step sizes (incl. the expensive HUE_LEVEL_MAX/(t*SR) float divisions)
 * and regenerate only when a parameter actually moves — the dirty check is 4
 * float compares vs 3 atomic loads + 3 float divides per laser-task call. */
static uint32_t s_hue_atk_step = 0, s_hue_dec_step = 0, s_hue_rel_step = 0;
static uint32_t s_hue_sus_lev  = 0;
static float    s_hue_atk = -1.f, s_hue_dec = -1.f, s_hue_rel = -1.f, s_hue_sus = -1.f;

static void hue_steps_update() {
  const float SR = HUE_SCAN_HZ;
  const float at = std::max(0.005f, hueAttack .load(std::memory_order_relaxed));
  const float dc = std::max(0.005f, hueDecay  .load(std::memory_order_relaxed));
  const float rl = std::max(0.005f, hueRelease.load(std::memory_order_relaxed));
  const float su = std::min(1.f, std::max(0.f, hueSustain.load(std::memory_order_relaxed)));
  if (at == s_hue_atk && dc == s_hue_dec && rl == s_hue_rel && su == s_hue_sus) return;
  s_hue_atk_step = (uint32_t)(HUE_LEVEL_MAX / (at * SR));
  s_hue_dec_step = (uint32_t)(HUE_LEVEL_MAX / (dc * SR));
  s_hue_rel_step = (uint32_t)(HUE_LEVEL_MAX / (rl * SR));
  s_hue_sus_lev  = (uint32_t)(su * HUE_LEVEL_MAX);
  s_hue_atk = at; s_hue_dec = dc; s_hue_rel = rl; s_hue_sus = su;
}

void IRAM_ATTR harpHueAdvance(int stringIdx) {
  const int idx = stringIdx & 7;
  HueEnvelopeState& he = stringHueEnv[idx];
  const EnvState s = he.state.load(std::memory_order_relaxed);
  if (s == EnvState::ENV_IDLE) return;

  hue_steps_update();
  uint32_t lev = he.level.load(std::memory_order_relaxed);

  switch (s) {
    case EnvState::ENV_ATTACK: {
      const uint32_t step = s_hue_atk_step;
      lev = (lev + step >= HUE_LEVEL_MAX) ? HUE_LEVEL_MAX : (lev + step);
      if (lev >= HUE_LEVEL_MAX)
        he.state.store(EnvState::ENV_DECAY, std::memory_order_relaxed);
      break;
    }
    case EnvState::ENV_DECAY: {
      const uint32_t susLev = s_hue_sus_lev;
      const uint32_t step   = s_hue_dec_step;
      lev = (lev > susLev + step) ? (lev - step) : susLev;
      if (lev <= susLev)
        he.state.store(EnvState::ENV_SUSTAIN, std::memory_order_relaxed);
      break;
    }
    case EnvState::ENV_SUSTAIN:
      break;
    case EnvState::ENV_RELEASE: {
      const uint32_t step = s_hue_rel_step;
      lev = (lev > step) ? (lev - step) : 0u;
      if (lev == 0u)
        he.state.store(EnvState::ENV_IDLE, std::memory_order_relaxed);
      break;
    }
    default: break;
  }
  he.level.store(lev, std::memory_order_relaxed);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * TELEMETRY
 * ═══════════════════════════════════════════════════════════════════════════ */
int harpActiveVoiceCount() {
  int n = 0;
  for (int v = 0; v < HARP_POLYPHONY; ++v)
    if (harpVoices[v].active.load(std::memory_order_relaxed)) ++n;
  return n;
}
