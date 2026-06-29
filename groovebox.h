/* ═════════════════════════════════════════════════════════════════════════════
 * Octopus PRO XL v6.1.01 — Laser Harp Groovebox
 * © 2026 DIODAC ELECTRONICS / iSystem. All Rights Reserved.
 *
 * PROPRIETARY AND CONFIDENTIAL. Unauthorized copying, distribution, modification,
 * or use of this software or firmware, in whole or in part, is strictly prohibited
 * without prior written permission from DIODAC ELECTRONICS.
 * ═════════════════════════════════════════════════════════════════════════════
 * groovebox.h — v6.1.01  MELODY SEQUENCER + DRUM ENGINE — PUBLIC INTERFACE
 *
 * Seq melody synth (8 voices, voice 7 = arp output), TR-909 drum engine,
 * pattern/transport/grid UI hooks.  HARP is in harp.h — not here.
 *
 * Core 0 audio path: gb_seq_fill_buf / drum_fill_buf (groovebox.cpp).
 * Pattern data: hwSeqData, hwMotionData (globals.h); persistence in settings.h.
 * Seq note-off and drum triggers serialise drumLivePatch reads under patchMux.
 * DrumVoice.body_wave set in fire_tuned_drum (disarm→write→arm).
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
 * groovebox.cpp — dedicated sequencer-only DSP (no harp path).
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
 * drumLivePatch[ch*4+0..+3] = Tune / Decay / Volume / Noise.
 * fire_tuned_drum snapshots patch words under patchMux; disarm→write→arm sequence.
 * Drum bus mix stays within int32_t for 8 voices at full level.
 * ─────────────────────────────────────────────────────────────────────────────*/

static constexpr float    DRUM_PITCH_MAGIC = 4294967296.0f / (float)SAMPLE_RATE;
static constexpr float    DRUM_ENV_MAGIC   = (float)0x7FFFFFFFu / (float)SAMPLE_RATE;
static constexpr int32_t  DRUM_SAT_LIMIT   = 24576;
static constexpr int      DRUM_SINE_IDX    = 8;

static inline int32_t IRAM_ATTR drum_clip(int32_t s) {
  return s > 32767 ? 32767 : (s < -32768 ? -32768 : s);
}

static inline int32_t IRAM_ATTR drum_wave(const int16_t* tbl, uint32_t phase) {
  if (!tbl) return 0;
  const uint32_t i0   = (phase >> 24) & 255u;
  const uint32_t frac = (phase >> 16) & 255u;
  return (int32_t)tbl[i0] + (((int32_t)(tbl[(i0 + 1u) & 255u] - tbl[i0]) * (int32_t)frac) >> 8);
}

/* fire_tuned_drum — arm one drum voice (Core 0 audio / MIDI). */
static inline void IRAM_ATTR fire_tuned_drum(int ch, float velocity) {
  if ((unsigned)ch >= 8u) return;

  /* Snapshot drumLivePatch under patchMux. */
  uint16_t drumSnap[4];
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
  /* Disarm voice before reinit so drum_fill_buf skips it. */
  d->active.store(false, std::memory_order_relaxed);

  const uint8_t kitRaw = drumKit.load(std::memory_order_relaxed);
  const uint8_t kit    = (kitRaw < (uint8_t)DrumKitId::COUNT) ? kitRaw : 0u;
  d->kit = kit;

  const float    pMult = drumPitchMult.load(std::memory_order_relaxed);
  const float    pm    = pMult * DRUM_PITCH_MAGIC;
  /* Hats/clap/snare body: normalize to factory Drm Pitch so default ×0.60 stays
   * classic; kick/toms/perc still track the global knob directly. */
  const float    hatRel = pMult / DRUM_PITCH_FACTORY;
  const float    hatPm  = hatRel * DRUM_PITCH_MAGIC;
  const float    snarePm = hatPm;
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
      {
        const float lo = KIT_SNARE_BODY_LO[kit];
        const float hi = KIT_SNARE_BODY_HI[kit];
        d->step1 = (uint32_t)((lo + vTune * (hi - lo)) * snarePm);
        d->env_decay_step = (uint32_t)(DRUM_ENV_MAGIC /
            std::max(0.05f, vDec * KIT_SNARE_DECAY_SCALE[kit]));
        break;
      }
    case 2: /* CLAP */
      d->step1          = 0;
      d->env_decay_step = (uint32_t)(DRUM_ENV_MAGIC / std::max(0.06f, vDec * 0.9f));
      break;
    case 3: /* HH CLOSED */
    case 4: /* HH OPEN */
      {
        const float bs = KIT_HAT_BASE_HZ[kit] * hatPm;
        d->step1 = (uint32_t)(bs * 2.00f); d->step2 = (uint32_t)(bs * 3.00f);
        d->step3 = (uint32_t)(bs * 4.16f); d->step4 = (uint32_t)(bs * 5.43f);
        d->step5 = (uint32_t)(bs * 6.79f); d->step6 = (uint32_t)(bs * 8.21f);
        const float db = (ch == 3) ? 0.05f : 0.14f;
        const float dm = (ch == 3) ? 0.28f : 1.05f;
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

  /* body_wave: nullptr for noise-only voices (CLAP, hats). */
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

/* drum_fill_buf — per-sample 909-style synthesis, 8 voices. */
static inline bool IRAM_ATTR drum_fill_buf(int16_t* out_buf, size_t frames) {
  /* Drums muted → skip synthesis and free voices (env ager runs in the loop below). */
  if (mixDrumsMute.load(std::memory_order_relaxed)) {
    for (int ch = 0; ch < 8; ++ch)
      drums[ch].active.store(false, std::memory_order_relaxed);
    memset(out_buf, 0, frames * sizeof(int16_t));
    return false;
  }

  /* Idle early-out when no drum voice is active. */
  bool any_active = false;
  for (int ch = 0; ch < 8; ++ch) {
    if (drums[ch].active.load(std::memory_order_relaxed)) { any_active = true; break; }
  }
  if (!any_active) {
    memset(out_buf, 0, frames * sizeof(int16_t));
    return false;
  }

  /* One acquire load per voice per buffer; inner loop uses a live bitmask. */
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
          const int32_t body = drum_wave(d->body_wave, d->phase1);          d->filter_low += ((body - d->filter_low) * (int32_t)d->tone_val) >> 15;
          d->filter_low = std::min(DRUM_SAT_LIMIT, std::max(-DRUM_SAT_LIMIT, d->filter_low));
          const int32_t click = (d->phase3 < 180u)
              ? (int32_t)((180u - d->phase3) * (fast_noise() >> 5)) : 0;
          /* Kick click × noise_mix uses int64 — product can exceed int32. */
          out = drum_clip(((d->filter_low * (int32_t)(32768 - d->noise_mix)) >> 15) +
                          (int32_t)(((int64_t)click * d->noise_mix) >> 15));
          break;
        }
        case DrumType::DRUM_SNARE: {
          const uint8_t k = d->kit;
          d->phase1 += d->step1 +
              (uint32_t)(((uint64_t)d->step1 * d->env_pitch * KIT_SNARE_PITCH_MUL[k]) >> 32);
          const int32_t body = drum_wave(d->body_wave, d->phase1);
          const int32_t tuneBias = ((int32_t)d->tone_val - 11000) >> 1;
          int32_t snap_fc = KIT_SNARE_SNAP_FC[k] + tuneBias;
          if (snap_fc < 4000) snap_fc = 4000;
          if (snap_fc > 18000) snap_fc = 18000;
          const int32_t rattle_fc = std::max<int32_t>(800, snap_fc - KIT_SNARE_RATTLE_DELTA[k]);
          const int32_t wn = fast_noise();
          d->filter_low  += ((wn - d->filter_low) * snap_fc) >> 15;
          d->filter_high += ((d->filter_low - d->filter_high) * rattle_fc) >> 15;
          const int32_t tone = (body * (int32_t)(32768 - d->noise_mix)) >> 15;
          const int32_t ratt = ((d->filter_low - d->filter_high) * (int32_t)d->noise_mix) >> 15;
          d->phase3++;
          int32_t click = 0;
          const uint16_t cl = KIT_SNARE_CLICK[k];
          if (d->phase3 < cl) click = (int32_t)((cl - d->phase3) * (wn >> 4));
          out = drum_clip(tone + ratt + click);
          break;
        }
        case DrumType::DRUM_CLAP: {
          d->phase3++;
          const uint8_t k = d->kit;
          const uint16_t b1 = KIT_CLAP_BURST1[k];
          const uint16_t b2 = KIT_CLAP_BURST2[k];
          const uint16_t b3 = KIT_CLAP_BURST3[k];
          uint32_t burst;
          if      (d->phase3 < b1) burst = (uint32_t)(b1 - d->phase3) << 20;
          else if (d->phase3 < b2) burst = (uint32_t)(b2 - d->phase3) << 20;
          else if (d->phase3 < b3) burst = (uint32_t)(b3 - d->phase3) << 20;
          else                       burst = d->env_amp;
          /* Tune knob → bandpass centre; noise knob → layer level (was ignored). */
          const int32_t tuneBias = ((int32_t)d->tone_val - 11000) >> 1;
          int32_t lp = KIT_CLAP_FILTER_LP[k] + tuneBias;
          int32_t hp = KIT_CLAP_FILTER_HP[k] + (tuneBias >> 1);
          if (lp < 6000) lp = 6000;
          if (lp > 20000) lp = 20000;
          if (hp < 4000) hp = 4000;
          if (hp > 16000) hp = 16000;
          d->filter_low  += ((fast_noise() - d->filter_low)   * lp) >> 15;
          d->filter_high += ((d->filter_low - d->filter_high) * hp) >> 15;
          const int32_t sc = ((d->filter_low - d->filter_high) * (int32_t)(burst >> 16)) >> 15;
          const int32_t body = (sc * (32768 + (int32_t)d->noise_mix)) >> 16;
          mix += (((body * (int32_t)d->velocity) >> 15) * (int32_t)d->vol_q15) >> 19;
          continue;
        }
        case DrumType::DRUM_HAT: {
          d->phase1 += d->step1; d->phase2 += d->step2; d->phase3 += d->step3;
          d->phase4 += d->step4; d->phase5 += d->step5; d->phase6 += d->step6;
          const int32_t amp = KIT_HAT_METAL_AMP[d->kit];
          const int32_t metal =
            ((d->phase1 & 0x80000000u) ? amp : -amp) +
            ((d->phase2 & 0x80000000u) ? amp : -amp) +
            ((d->phase3 & 0x80000000u) ? amp : -amp) +
            ((d->phase4 & 0x80000000u) ? amp : -amp) +
            ((d->phase5 & 0x80000000u) ? amp : -amp) +
            ((d->phase6 & 0x80000000u) ? amp : -amp);
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
              ((drum_wave(d->body_wave, d->phase1) * (int32_t)(32768 - d->noise_mix)) >> 15) +
              ((fast_noise() * (int32_t)d->noise_mix) >> 15));
          break;
        }
        case DrumType::DRUM_PERC: {
          d->phase1 += d->step1; d->phase2 += d->step2;
          const int32_t ring =
            (drum_wave(d->body_wave, d->phase1) *                       drum_wave(d->body_wave, d->phase2)) >> 15;              const int32_t wn = fast_noise();
          d->filter_low += ((wn - d->filter_low) * (int32_t)d->tone_val) >> 15;
          out = drum_clip(((ring * (int32_t)(32768 - d->noise_mix)) >> 15) +
                          (((wn - d->filter_low) * (int32_t)d->noise_mix) >> 15));
          break;
        }
      }

      /* Per-voice scaling: max ≈ 2047/voice; 8 voices ≈ 16 k before soft clip. */
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

static constexpr int NUM_SYNTH_PATS = 29;
static constexpr int NUM_DRUM_PATS  = 29;

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
/* Clear active pattern + reset companion synth/drum sounds to factory defaults. */
void seqClearActiveAndResetSounds();
/* Clear one 16-step P-page on the active bank (chain 0); echoes grid to App. */
void seqClearPatternPage(uint8_t page);
/* Save/load the active bank/chain pattern into the 64-slot user pattern library. */
bool saveActivePatternToUserSlot(uint8_t uidx);
void loadUserPatternToActive(uint8_t uidx);

/* ─────────────────────────────────────────────────────────────────────────────
 * §6  SEQUENCER PUBLIC INTERFACE  (impl in groovebox.cpp)
 * ─────────────────────────────────────────────────────────────────────────────*/
static constexpr int SEQ_UI_CHAIN = 0;

extern std::atomic<int> seqUI_row;
extern std::atomic<int> seqUI_col;
extern std::atomic<int> seqUI_page;
extern std::atomic<int> seqUI_stepPage;  /* 0-3 horizontal 16-step window of the 64-step pattern */

void seq_start();
void seq_stop();
void seq_pause();
void seq_toggle();
void setSequencerBpm(int32_t bpm);
void initSequencer();
void song_rewind_rt();   /* reset song chain to step 0 + load its bank */
void seq_restart_rt();   /* restart step counter from 0 while playing (CMD_SEQ_RESTART) */

void seqUI_moveUp();
void seqUI_moveDown();
void seqUI_moveLeft();
void seqUI_moveRight();
void seqUI_selectBank(int bank);
void applySeqStepPage(uint8_t page);
void seqUI_toggleStep();
void seqUI_renderMatrix();

void IRAM_ATTR sequencer_render_block(uint32_t frames);
void           sequencer_background_task(void* pvParameters);

#endif /* GROOVEBOX_H */
