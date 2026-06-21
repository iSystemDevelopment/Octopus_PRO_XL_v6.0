/* ═════════════════════════════════════════════════════════════════════════════
 * Octopus PRO XL v6.0.00 — Laser Harp Groovebox
 * © 2026 DIODAC ELECTRONICS / iSystem. All Rights Reserved.
 *
 * PROPRIETARY AND CONFIDENTIAL. Unauthorized copying, distribution, modification,
 * or use of this software or firmware, in whole or in part, is strictly prohibited
 * without prior written permission from DIODAC ELECTRONICS.
 * ═════════════════════════════════════════════════════════════════════════════
 * groovebox.h — v6.0.00  MELODY SEQUENCER + DRUM ENGINE — PUBLIC INTERFACE
 *
 * [GB-MERGE] Consolidates: sequencer.h/cpp · patterns.h · drum_synth.h
 *
 * SCOPE: seq melody engine + TR-909 drum engine + pattern management +
 *        transport + grid UI.  HARP IS NOT HERE — laser harp is in harp.h/cpp.
 *
 * [v6.0.00] Maintenance fixes (seq note-off under patchMux, drum snapshot):
 *  [FIX-1] release_seq_note serialised under patchMux (matches harp path).
 *          ADSR rates come from seq_synth_fill_buf each buffer — no integer
 *          release_step recompute on note-off.
 *  [FIX-2] fire_tuned_drum snapshots drumLivePatch[ch*4..+3] under patchMux
 *          before touching any DrumVoice field — no torn reads on concurrent
 *          App/encoder write + audio-task trigger.
 *  [FIX-3] body_wave pointer moved from global s_drumBodyWave[] into DrumVoice
 *          (requires globals.h DrumVoice += `const int16_t* body_wave`).
 *          Disarm→update→arm sequence makes the read in drum_fill_buf safe
 *          across the Core-0 task boundary without an extra lock.
 *  [FIX-4] seqVoiceOwner[row] written BEFORE trigger_seq_note in render_block
 *          so MIDI noteOff path never reads a stale owner after a voice arms.
 *  [FIX-5] lastDrumMask removed — dead state; drums are one-shot (no gate-off).
 *  [FIX-6] g_saveArmed early-exit documented as intentional transport hold.
 *  [FIX-OV] Drum bus accumulator bounds verified + comment; scales safely in
 *           int32_t for 8 voices (max ~16 k per voice after all scaling).
 *
 * ── GLOBALS.H REQUIRED ADDITION ─────────────────────────────────────────────
 *   Add to struct DrumVoice (after `uint8_t kit`):
 *     const int16_t* body_wave = nullptr;
 *   Eliminates the global s_drumBodyWave[] pointer cache (FIX-3).
 * ═════════════════════════════════════════════════════════════════════════════ */
#pragma once
#ifndef GROOVEBOX_H
#define GROOVEBOX_H

/* ─────────────────────────────────────────────────────────────────────────────
 * §1  INCLUDES
 * ─────────────────────────────────────────────────────────────────────────────*/
#include <Arduino.h>
#include <pgmspace.h>
#include <atomic>
#include <algorithm>
#include "globals.h"
#include "assets.h"
                   
/* ─────────────────────────────────────────────────────────────────────────────
 * §2  SEQUENCER VOICE OPS  (implemented in groovebox.cpp)
 *
 * trigger_seq_note / release_seq_note / seq_synth_fill_buf are out-of-line in
 * groovebox.cpp — dedicated sequencer-only DSP (no synth_core.h, no HARP path).
 * seq_active_voice_count stays inline (telemetry only).
 * ─────────────────────────────────────────────────────────────────────────────*/

void IRAM_ATTR trigger_seq_note(int vi, float freq, uint16_t vel);
void IRAM_ATTR release_seq_note(int vi);
/* gb_seq_fill_buf — the real seq-synth DSP (groovebox.cpp); returns true if any
 * voice produced audio.  seq_synth_fill_buf (audio.cpp) is the thin bool wrapper
 * the audio orchestrator calls so the engine entry points read uniformly. */
bool IRAM_ATTR gb_seq_fill_buf(int16_t* out_buf, size_t frames);

/* Telemetry — active sequencer voices.                                       */
static inline int seq_active_voice_count() {
  int n = 0;
  for (int i = 0; i < SEQ_POLYPHONY; ++i)
    if (seqVoices[i].active.load(std::memory_order_relaxed)) ++n;
  return n;
}

/* ─────────────────────────────────────────────────────────────────────────────
 * §3  DRUM SYNTH ENGINE  (TR-909 oversampled, 8 voices)
 *
 * Voice map: KICK(0) SNARE(1) CLAP(2) HAT-C(3) HAT-O(4) TOM-H(5) TOM-L(6) PERC(7)
 * drumLivePatch[]: [ch*4+0]=Tune [ch*4+1]=Decay [ch*4+2]=Volume [ch*4+3]=Noise
 *
 * [FIX-2] fire_tuned_drum snapshots all 4 drumLivePatch words for the channel
 *         under patchMux before touching the DrumVoice — avoids a torn read
 *         when App/encoder writes drum params while the step engine fires drums.
 *
 * [FIX-3] body_wave pointer lives in DrumVoice (not a global array).  The
 *         disarm(active=false) → write body_wave → arm(active=true,release)
 *         sequence gates all reads in drum_fill_buf through the acquire fence,
 *         so the pointer swap cannot be observed mid-buffer.
 *         Requires DrumVoice to have `const int16_t* body_wave` (globals.h).
 *
 * [FIX-OV] Drum bus accumulator: per-voice max contribution is ~2047 counts
 *          after the three-stage >>15/>>15/>>19 scaling chain.  8 voices ×2047
 *          = ~16 k, ×3/4 = ~12 k — well within int32_t.  No int64_t needed
 *          (unlike the synth path which uses int64_t for the SVF output chain).
 * ─────────────────────────────────────────────────────────────────────────────*/

static constexpr float    DRUM_PITCH_MAGIC = 4294967296.0f / (float)SAMPLE_RATE;
static constexpr float    DRUM_ENV_MAGIC   = (float)0x7FFFFFFFu / (float)SAMPLE_RATE;
static constexpr int32_t  DRUM_SAT_LIMIT   = 24576;
static constexpr int      DRUM_SINE_IDX    = 8;

static constexpr uint32_t KIT_KICK_SWEEP[(int)DrumKitId::COUNT]  = { 14u, 6u, 4u, 12u };
static constexpr float    KIT_HAT_BASE_HZ[(int)DrumKitId::COUNT] = {
  4150.0f, 3700.0f, 4400.0f, 4200.0f
};

static inline int32_t IRAM_ATTR drum_clip(int32_t s) {
  return s > 32767 ? 32767 : (s < -32768 ? -32768 : s);
}

static inline int32_t IRAM_ATTR drum_wave(const int16_t* tbl, uint32_t phase) {
  if (!tbl) return 0;
  const uint32_t i0   = (phase >> 24) & 255u;
  const uint32_t frac = (phase >> 16) & 255u;
  return (int32_t)tbl[i0] + (((int32_t)(tbl[(i0 + 1u) & 255u] - tbl[i0]) * (int32_t)frac) >> 8);
}

/* fire_tuned_drum — arm one drum voice.
 * Thread-safe: called from Core 0 (audio task render_block + MIDI noteOnDrums).
 * [FIX-2] Snapshot of 4 drum patch params under patchMux before any write.
 * [FIX-3] body_wave stored in voice struct; disarm→write→arm gates access.   */
static inline void IRAM_ATTR fire_tuned_drum(int ch, float velocity) {
  if ((unsigned)ch >= 8u) return;

  /* [FIX-2] Snapshot 4 drum params atomically — prevents torn reads if the App
   * or encoder writes drum params concurrently with the step engine trigger.  */
  uint16_t drumSnap[4] = {};  /* [FIX-INIT] zero before critical section */
  {
    portENTER_CRITICAL(&patchMux);
    const int base = ch << 2;
    drumSnap[0] = drumLivePatch[base + 0];  /* Tune  */
    drumSnap[1] = drumLivePatch[base + 1];  /* Decay */
    drumSnap[2] = drumLivePatch[base + 2];  /* Vol   */
    drumSnap[3] = drumLivePatch[base + 3];  /* Noise */
    portEXIT_CRITICAL(&patchMux);
  }

  /* HH choke: closed hat immediately silences open hat (TR-909 behaviour).   */
  if (ch == 3 && drums[4].active.load(std::memory_order_relaxed)) {
    drums[4].env_amp = 0;
    drums[4].active.store(false, std::memory_order_relaxed);
  }

  DrumVoice* d = &drums[ch];
  /* Disarm first so drum_fill_buf skips this voice during reinit. [FIX-3]    */
  d->active.store(false, std::memory_order_relaxed);

  const uint8_t kitRaw = drumKit.load(std::memory_order_relaxed);
  const uint8_t kit    = (kitRaw < (uint8_t)DrumKitId::COUNT) ? kitRaw : 0u;
  d->kit = kit;

  const float    pMult = drumPitchMult.load(std::memory_order_relaxed);
  const float    pm    = pMult * DRUM_PITCH_MAGIC;  /* [gbox OPT-8] hoist once */
  const float    vTune = (float)drumSnap[0] / 16383.0f;
  const float    vDec  = (float)drumSnap[1] / 16383.0f;
  const uint16_t vVol  = drumSnap[2];
  const int32_t  vNse  = (int32_t)drumSnap[3];

  d->vol_q15   = (uint16_t)(vVol << 1);        /* 0..32766 */
  d->noise_mix = vNse << 1;
  d->tone_val  = (uint16_t)(2000u + (uint16_t)(vTune * 20000.0f));

  switch (ch) {
    case 0: /* KICK */
      d->step1          = (uint32_t)((22.0f + vTune * 110.0f) * pm);
      d->env_decay_step = (uint32_t)(DRUM_ENV_MAGIC / std::max(0.05f, vDec * 1.5f));
      break;
    case 1: /* SNARE */
      d->step1          = (uint32_t)((130.0f + vTune * 140.0f) * pm);
      d->env_decay_step = (uint32_t)(DRUM_ENV_MAGIC / std::max(0.05f, vDec * 0.75f));
      break;
    case 2: /* CLAP */
      d->step1          = 0;
      d->env_decay_step = (uint32_t)(DRUM_ENV_MAGIC / std::max(0.06f, vDec * 0.9f));
      break;
    case 3: /* HH CLOSED */
    case 4: /* HH OPEN */
      {
        const float bs = KIT_HAT_BASE_HZ[kit] * pm;
        d->step1 = (uint32_t)(bs * 2.00f); d->step2 = (uint32_t)(bs * 3.00f);
        d->step3 = (uint32_t)(bs * 4.16f); d->step4 = (uint32_t)(bs * 5.43f);
        d->step5 = (uint32_t)(bs * 6.79f); d->step6 = (uint32_t)(bs * 8.21f);
        const float db = (ch == 3) ? 0.05f : 0.14f;
        const float dm = (ch == 3) ? 0.30f : 1.10f;
        d->env_decay_step = (uint32_t)(DRUM_ENV_MAGIC / std::max(db, vDec * dm));
        break;
      }
    case 5: /* TOM HIGH */
      d->step1          = (uint32_t)((140.0f + vTune * 160.0f) * pm);
      d->env_decay_step = (uint32_t)(DRUM_ENV_MAGIC / std::max(0.08f, vDec * 0.85f));
      break;
    case 6: /* TOM LOW */
      d->step1          = (uint32_t)((75.0f + vTune * 90.0f) * pm);
      d->env_decay_step = (uint32_t)(DRUM_ENV_MAGIC / std::max(0.08f, vDec * 0.85f));
      break;
    case 7: /* PERC */
      d->step1          = (uint32_t)((1100.0f + vTune * 900.0f) * pm);
      d->step2          = (d->step1 * 3u) >> 2;
      d->env_decay_step = (uint32_t)(DRUM_ENV_MAGIC / std::max(0.04f, vDec * 0.2f));
      break;
  }

  static constexpr DrumType kTypeMap[8] = {
    DrumType::DRUM_KICK, DrumType::DRUM_SNARE, DrumType::DRUM_CLAP, DrumType::DRUM_HAT,
    DrumType::DRUM_HAT,  DrumType::DRUM_TOM,   DrumType::DRUM_TOM,  DrumType::DRUM_PERC
  };
  d->type = kTypeMap[ch];

  /* [FIX-3] body_wave in voice struct — gated by the disarm/arm sequence.
   * drum_fill_buf only reads body_wave AFTER acquiring the active fence,
   * which synchronises with the release store below.                         */
  d->body_wave = (ch != 2u && ch != 3u && ch != 4u)
    ? WAVE_TABLE_RAM[drumWaveIdx[ch].load(std::memory_order_relaxed) % NUM_WAVE_TABLES]
    : nullptr;

  d->env_amp              = 0x7FFFFFFFu;
  d->env_pitch            = 0x7FFFFFFFu;
  d->env_pitch_decay_step = (uint32_t)(0x7FFFFFFFu / (SAMPLE_RATE * 0.045f));
  d->velocity             = (uint16_t)(velocity * 32767.0f);

  d->phase1 = d->phase2 = d->phase3 = d->phase4 = d->phase5 = d->phase6 = 0;
  d->filter_low = d->filter_high = 0;

  /* Publish: all fields written before active flag is set.                   */
  std::atomic_thread_fence(std::memory_order_release);
  d->active.store(true, std::memory_order_release);
}

/* drum_fill_buf — per-sample 909-style synthesis, 8 voices.
 * [FIX-3] Uses d->body_wave (per-voice pointer) instead of global cache.
 * [FIX-OV] Per-voice max contribution ≈ 2047 after >>15/>>15/>>19 chain;
 *           8 voices × 2047 × 3/4 ≈ 12 k — safe in int32_t.                */
static inline bool IRAM_ATTR drum_fill_buf(int16_t* out_buf, size_t frames) {
  /* [MUTE-GATE] Drums muted → skip synthesis and free voices (the env ager that
   * frees them runs in the loop below).  FX already applies gain 0 to a muted
   * engine, so rendering is pure waste; freeing avoids an un-aged voice pile-up
   * on unmute.  Drums are one-shots, so unmute simply starts firing fresh. */
  if (mixDrumsMute.load(std::memory_order_relaxed)) {
    for (int ch = 0; ch < 8; ++ch)
      drums[ch].active.store(false, std::memory_order_relaxed);
    memset(out_buf, 0, frames * sizeof(int16_t));
    return false;
  }

  /* [PERF] Idle early-out — bail to silence when no drum voice is active so an
   * idle kit costs ~0 instead of clipping 512 zero-mix samples every buffer. */
  bool any_active = false;
  for (int ch = 0; ch < 8; ++ch) {
    if (drums[ch].active.load(std::memory_order_relaxed)) { any_active = true; break; }
  }
  if (!any_active) {
    memset(out_buf, 0, frames * sizeof(int16_t));
    return false;                         // [3a]
  }

  /* [gbox OPT-2] One acquire load per voice per buffer instead of 4096; the
   * inner loop tests a plain bitmask. Voices that exhaust their envelope clear
   * their bit so they are skipped for the remaining samples (same behaviour).  */
  uint8_t live_mask = 0;
  for (int ch = 0; ch < 8; ++ch)
    if (drums[ch].active.load(std::memory_order_acquire))
      live_mask |= (uint8_t)(1u << ch);

  for (size_t i = 0; i < frames; ++i) {
    int32_t mix = 0;

    for (int ch = 0; ch < 8; ++ch) {
      if (!(live_mask & (uint8_t)(1u << ch))) continue;
      DrumVoice* d = &drums[ch];

      if (d->env_amp > d->env_decay_step) {
        d->env_amp -= d->env_decay_step;
      } else {
        d->env_amp = 0;
        d->active.store(false, std::memory_order_release);
        live_mask &= ~(uint8_t)(1u << ch);
        continue;
      }

      if (d->env_pitch > d->env_pitch_decay_step)
        d->env_pitch -= d->env_pitch_decay_step;
      else
        d->env_pitch = 0;

      int32_t out = 0;

      switch (d->type) {
        case DrumType::DRUM_KICK: {
          d->phase3++;
          const uint32_t ps = (uint32_t)(((uint64_t)d->env_pitch * d->env_pitch) >> 31);
          const uint32_t step = d->step1
              + (uint32_t)(((uint64_t)d->step1 * ps * KIT_KICK_SWEEP[d->kit]) >> 31);
          d->phase1 += step;
          const int32_t body = drum_wave(d->body_wave, d->phase1); /* [FIX-3] */
          d->filter_low += ((body - d->filter_low) * (int32_t)d->tone_val) >> 15;
          d->filter_low = std::min(DRUM_SAT_LIMIT, std::max(-DRUM_SAT_LIMIT, d->filter_low));
          /* [FIX-CLICK] Cast (180u - phase3) to int32 before multiplying by the
           * signed fast_noise() result.  Without the cast C++ promotes the
           * int32 noise value to uint32, turning any negative noise sample into
           * a huge positive, producing a distorted click on ~50% of samples.  */
          const int32_t click = (d->phase3 < 180u)
              ? ((int32_t)(180u - d->phase3) * (fast_noise() >> 5)) : 0;
          /* [gbox BUG-1] |click| can reach ~91980; click * noise_mix (≤32766)
           * exceeds int32 (~3.0e9). Widen only this multiply to int64.        */
          out = drum_clip(((d->filter_low * (int32_t)(32768 - d->noise_mix)) >> 15) +
                          (int32_t)(((int64_t)click * d->noise_mix) >> 15));
          break;
        }
        case DrumType::DRUM_SNARE: {
          d->phase1 += d->step1 + (uint32_t)(((uint64_t)d->step1 * d->env_pitch * 3u) >> 32);
          const int32_t body      = drum_wave(d->body_wave, d->phase1); /* [FIX-3] */
          const int32_t snap_fc   = (int32_t)d->tone_val;
          const int32_t rattle_fc = std::max<int32_t>(1000, snap_fc - 6000);
          const int32_t wn = fast_noise();
          d->filter_low  += ((wn           - d->filter_low)  * snap_fc)   >> 15;
          d->filter_high += ((d->filter_low - d->filter_high) * rattle_fc) >> 15;
          out = drum_clip((body * (int32_t)(32768 - d->noise_mix) +
                           (d->filter_low - d->filter_high) * (int32_t)d->noise_mix) >> 15);
          break;
        }
        case DrumType::DRUM_CLAP: {
          d->phase3++;
          uint32_t burst;
          if      (d->phase3 < 440u)  burst = (440u  - d->phase3) << 20;
          else if (d->phase3 < 880u)  burst = (880u  - d->phase3) << 20;
          else if (d->phase3 < 1320u) burst = (1320u - d->phase3) << 20;
          else                        burst = d->env_amp;
          d->filter_low  += ((fast_noise() - d->filter_low)   * 14000) >> 15;
          d->filter_high += ((d->filter_low - d->filter_high) *  9000) >> 15;
          const int32_t sc = ((d->filter_low - d->filter_high) * (int32_t)(burst >> 16)) >> 15;
          mix += (((sc * (int32_t)d->velocity) >> 15) * (int32_t)d->vol_q15) >> 19;
          continue;
        }
        case DrumType::DRUM_HAT: {
          d->phase1 += d->step1; d->phase2 += d->step2; d->phase3 += d->step3;
          d->phase4 += d->step4; d->phase5 += d->step5; d->phase6 += d->step6;
          const int32_t metal =
            ((d->phase1 & 0x80000000u) ? 5000 : -5000) +
            ((d->phase2 & 0x80000000u) ? 5000 : -5000) +
            ((d->phase3 & 0x80000000u) ? 5000 : -5000) +
            ((d->phase4 & 0x80000000u) ? 5000 : -5000) +
            ((d->phase5 & 0x80000000u) ? 5000 : -5000) +
            ((d->phase6 & 0x80000000u) ? 5000 : -5000);
          const int32_t comb =
            (metal * (int32_t)(32768 - d->noise_mix) +
             fast_noise() * (int32_t)d->noise_mix) >> 15;
          const int32_t cut = std::min(DRUM_SAT_LIMIT, (int32_t)d->tone_val);
          d->filter_low += ((comb - d->filter_low) * cut) >> 15;
          out = drum_clip(comb - d->filter_low);
          break;
        }
        case DrumType::DRUM_TOM: {
          const uint32_t pm = (uint32_t)(((uint64_t)d->env_pitch * d->env_pitch) >> 31);
          d->phase1 += d->step1 + (uint32_t)(((uint64_t)d->step1 * pm * 5u) >> 31);
          out = drum_clip(
              ((drum_wave(d->body_wave, d->phase1) * (int32_t)(32768 - d->noise_mix)) >> 15) + /* [FIX-3] */
              ((fast_noise() * (int32_t)d->noise_mix) >> 15)); /* [gbox BUG-2] >>15, consistent with all other voices */
          break;
        }
        case DrumType::DRUM_PERC: {
          d->phase1 += d->step1; d->phase2 += d->step2;
          const int32_t ring =
            (drum_wave(d->body_wave, d->phase1) *           /* [FIX-3] */
             drum_wave(d->body_wave, d->phase2)) >> 15;     /* [FIX-3] */
          const int32_t wn = fast_noise();
          d->filter_low += ((wn - d->filter_low) * (int32_t)d->tone_val) >> 15;
          out = drum_clip(((ring * (int32_t)(32768 - d->noise_mix)) >> 15) +
                          (((wn - d->filter_low) * (int32_t)d->noise_mix) >> 15));
          break;
        }
      }

      /* Per-voice scaling: [FIX-OV] max = (32767>>16)*(32767>>15)*(32766>>19)
       * = 32767 * 32767 / 32768 / 524288 ≈ 2047 per voice, 8 voices = 16376  */
      const int32_t s = (out * (int32_t)(d->env_amp >> 16)) >> 15;
      mix += (((s * (int32_t)d->velocity) >> 15) * (int32_t)d->vol_q15) >> 19;
    }

    out_buf[i] = engine_soft_clip((mix * 3) >> 2);
  }
  return true; 
}

/* ─────────────────────────────────────────────────────────────────────────────
 * §4  PATTERN MODEL CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────*/
struct SynthPatternROM {
  const char* name;
  uint8_t  gate[16];
  int8_t   pitch[16];                  /* semitone offset, range −12…+12 */
  uint16_t preset[PARAMS_PER_PRESET];
};

struct DrumPatternROM {
  const char* name;
  uint16_t tracks[8];   /* 16-bit step bitmask per drum track        */
  uint16_t preset[32];  /* companion drum patch: 8 drums × 4 params  */
};

static constexpr int NUM_SYNTH_PATS = 29; /* [WS5] 8 cosmic + 21 appended (patt.md) */
static constexpr int NUM_DRUM_PATS  = 29; /* [WS5] 8 cosmic + 21 appended (patt.md) */

/* ─────────────────────────────────────────────────────────────────────────────
 * §5  PATTERN MANAGEMENT API  (extern — groovebox.cpp)
 * ─────────────────────────────────────────────────────────────────────────────*/
void loadFactorySynthPattern(int bankIdx, int chainIdx, int presetIdx);
void loadFactoryDrumPattern(int bankIdx, int chainIdx, int presetIdx);
void loadFactorySynthPreset(uint8_t presetIdx);
void loadFactoryDrumPreset(uint8_t presetIdx);
void autoCloneSynthPatternToBank(uint8_t bankIdx, uint8_t presetIdx);
void autoCloneDrumPatternToBank(uint8_t bankIdx, uint8_t presetIdx);
void clearPatternSlot(uint8_t bankIdx, uint8_t chainIdx);
void clearActivePattern();
void clearAllPatterns();
void copyPatternSlot(uint8_t srcBank, uint8_t srcChain, uint8_t dstBank, uint8_t dstChain);
/* [SEQ-CLEAR] Blank the active pattern (steps + P-locks) and reset both seq-synth
 * and drum sound to the factory default image — used by the SEQ SETUP Clear
 * confirm dialog.  Not a factory reset (active bank/chain only). */
void seqClearActiveAndResetSounds();
void seqSoftResetWorkingImage();   /* [SOFT-RESET] factory settings + sounds + seq nav → initial (RAM-only); echoes full state to App */
/* [USER-PAT-SLOTS] Save/load the active bank/chain pattern into the 64-slot
 * user pattern library (melody+drum grid + companion sounds + transpose). */
bool saveActivePatternToUserSlot(uint8_t uidx);
void loadUserPatternToActive(uint8_t uidx);

/* ─────────────────────────────────────────────────────────────────────────────
 * §6  SEQUENCER PUBLIC INTERFACE  (impl in groovebox.cpp)
 * ─────────────────────────────────────────────────────────────────────────────*/
static constexpr int SEQ_UI_CHAIN = 0;

extern std::atomic<int> seqUI_row;
extern std::atomic<int> seqUI_col;
extern std::atomic<int> seqUI_page;

void seq_start();
void seq_stop();
void seq_pause();
void seq_toggle();
/* [RND-RESTART] Reset step counter to 0 while keeping playback running.
 * Called by CMD_SEQ_RESTART (App → firmware) after pattern randomisation. */
void IRAM_ATTR seq_restart_from_step_zero();
void setSequencerBpm(int32_t bpm);
void initSequencer();
void song_rewind_rt();   /* [SONG-FIX] reset song chain to step 0 + load its bank */

void seqUI_moveUp();
void seqUI_moveDown();
void seqUI_moveLeft();
void seqUI_moveRight();
void seqUI_selectBank(int bank);
void seqUI_toggleStep();
void seqUI_renderMatrix();

void IRAM_ATTR sequencer_render_block(uint32_t frames);
void           sequencer_background_task(void* pvParameters);

#endif /* GROOVEBOX_H */
