/* ═════════════════════════════════════════════════════════════════════════════
 * Octopus PRO XL v6.1.00 — Laser Harp Groovebox
 * © 2026 DIODAC ELECTRONICS / iSystem. All Rights Reserved.
 *
 * PROPRIETARY AND CONFIDENTIAL. Unauthorized copying, distribution, modification,
 * or use of this software or firmware, in whole or in part, is strictly prohibited
 * without prior written permission from DIODAC ELECTRONICS.
 * ═════════════════════════════════════════════════════════════════════════════
 * settings.h — v6.1.00  NVS PERSISTENT STATE + FACTORY DEFAULTS
 *
 * Four NVS blobs in namespace "octopus" (legacy "octopus_v5" auto-migrated once):
 *   "settings"     — AllSettings struct (SETTINGS_VERSION 0x0616 = wire layout ID)
 *   "patterns"     — hwSeqData + per-pattern transpose
 *   "banks"        — sparse userBank/seqBank deltas vs factory ROM
 *   "motion"       — sparse P-lock lanes
 *   + usrnames / usrpat / usrpatnames for user slot libraries
 *
 * Boot path: settings_execute_pending_reset_at_boot() (before loadSettings) →
 *            loadSettings() → seedFactoryBanks → settings_load → settings_sync_to_ssot
 *            → persisted_extras_load().  Scoped SAVE/LOAD/RESET via ResetScope.
 * Runtime FULL / BANKS+PATS reset: arm NVS flag "pend_rst" + immediate reboot only;
 *            heavy wipe runs on next boot (same safe window as OC+SCALE combo).
 * Runtime saves go through NvsWorker (16 KB stack) or settings_persist_blocking().
 *
 * Persisted vs transient atomics — see code_info.h §2.  Struct fields append-only;
 * bump SETTINGS_VERSION when the AllSettings layout changes.
 * ═════════════════════════════════════════════════════════════════════════════ */
#pragma once
#ifndef SETTINGS_H
#define SETTINGS_H

#include <cstddef>
#include <nvs_flash.h>
#include <nvs.h>
#include <esp_heap_caps.h>
#include <atomic>
#include <cstring>
#include <cmath>
#include <algorithm>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "globals.h"
#include "effect.h"
#include "patches.h" /* syncLivePatchFromAtomics(), applyHarpParam… */
#include "fog.h"

/* Forward-declared from groovebox.cpp — working-image SEQ view page for NVS resume. */
extern std::atomic<int> seqUI_page;

/* NVS partition namespace — product ID, not a firmware semver.
 * Legacy "octopus_v5" is copied here once on first boot after the rename.
 * [LEGACY-MIGRATION-v5] Keep until after 2026-Q4; safe to remove when v5.x
 * field units are no longer expected to upgrade in-place. */
static constexpr const char* NVS_NAMESPACE        = "octopus";
static constexpr const char* NVS_NAMESPACE_LEGACY = "octopus_v5";

/* Persisted AllSettings wire-layout version (independent of SYSTEM_FW_VERSION). */
static constexpr uint16_t SETTINGS_VERSION = 0x0616;

static inline bool nvs_migrate_legacy_namespace() {
  nvs_handle_t leg;
  if (nvs_open(NVS_NAMESPACE_LEGACY, NVS_READONLY, &leg) != ESP_OK) return false;

  nvs_handle_t neu;
  if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &neu) != ESP_OK) {
    nvs_close(leg);
    return false;
  }

  static const char* const kBlobKeys[] = {
    "settings", "patterns", "banks", "usrnames", "usrpat", "usrpatnames", "motion"
  };
  bool copied = false;
  for (const char* key : kBlobKeys) {
    size_t sz = 0;
    if (nvs_get_blob(leg, key, nullptr, &sz) != ESP_OK || !sz) continue;
    uint8_t* buf = (uint8_t*)heap_caps_malloc(sz, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!buf) continue;
    if (nvs_get_blob(leg, key, buf, &sz) == ESP_OK &&
        nvs_set_blob(neu, key, buf, sz) == ESP_OK)
      copied = true;
    heap_caps_free(buf);
  }
  if (copied) nvs_commit(neu);
  nvs_close(leg);
  nvs_close(neu);
  return copied;
}

/** Open the active NVS namespace; migrates legacy "octopus_v5" on first access. */
static inline esp_err_t nvs_open_octopus(nvs_open_mode_t mode, nvs_handle_t* out) {
  esp_err_t err = nvs_open(NVS_NAMESPACE, mode, out);
  if (err != ESP_ERR_NVS_NOT_FOUND) return err;
  if (!nvs_migrate_legacy_namespace()) return err;
  return nvs_open(NVS_NAMESPACE, mode, out);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * PERSISTENT DATA STRUCTURES
 *
 * AllSettings is one NVS blob.  CRC32 covers the entire struct with crc32
 * zeroed.  Version mismatch or corrupt CRC triggers factory reset.
 *
 * Struct field order = the NVS wire layout.  Fields must only be appended
 * at the end of each sub-struct — never reordered or inserted.  Any such
 * change requires a SETTINGS_VERSION bump.
 * ═══════════════════════════════════════════════════════════════════════════ */

/* ── Harp synth — 14 active SynthParam fields ───────────────────────────── */
/* Factory defaults: responsive laser-harp pluck — fast attack, moderate sustain
 * while the beam is held, gentle release, open filter for natural brightness. */
struct HarpSettings {
  uint16_t waveform = 0;        /* Cosmic Saw — bright pluck character  */
  float attack = 0.008f;        /* 8 ms — fast but not instant          */
  float decay = 0.25f;          /* moderate pluck fade                  */
  float sustain = 0.60f;        /* sustains while beam held             */
  float release = 0.18f;        /* slight tail after beam breaks        */
  float cutoff = 0.65f;         /* open filter — natural brightness     */
  float resonance = 0.08f;      /* subtle, not warbly                   */
  float noise = 0.00f;          /* pure tone                            */
  float detune = 0.00f;         /* bipolar [-1, +1]; 0 = no detune      */
  float lfo_rate = 0.15f;       /* gentle slow modulation               */
  float lfo_depth = 0.00f;      /* LFO off by default                   */
  uint8_t lfo_route = 0;        /* 0-7; 0 = pitch route                 */
  uint16_t osc2_wave = 0;       /* Cosmic Saw on both oscillators       */
  float env_cut_amount = 0.00f; /* no envelope→filter on pluck          */
};

/* ── Seq synth — 14 active SynthParam fields ────────────────────────────── */
/* Factory defaults: punchy step-sequencer gate — near-instant attack, low
 * sustain, quick release; slight env_cut for per-note filter articulation.  */
struct SeqSettings {
  uint16_t waveform = 1; /* Quantum Sq — classic bass/lead       */
  float attack = 0.005f; /* 5 ms — snappy gate attack            */
  float decay = 0.22f;
  float sustain = 0.28f;   /* low sustain → percussive gates       */
  float release = 0.08f;   /* tight release                        */
  float cutoff = 0.50f;    /* balanced                             */
  float resonance = 0.22f; /* moderate resonance for character     */
  float noise = 0.00f;
  float detune = 0.00f;
  float lfo_rate = 0.30f;
  float lfo_depth = 0.00f;
  uint8_t lfo_route = 0;
  uint16_t osc2_wave = 0;
  float env_cut_amount = 0.20f; /* subtle filter envelope per note      */
};

/* ── Drum kit — per-voice tune/decay/volume/noise + wave_idx[8] ─────────── */
/* TR-909 inspired factory calibration (not flat 0.5 placeholders):
 *   KICK: long decay + low noise → deep sub punch
 *   SNARE: balanced body/rattle noise
 *   CLAP: all noise, moderate decay
 *   HATS: mostly noise, short closed / longer open
 *   TOMS: pitched, minimal noise, tune differentiated
 *   PERC: metallic, shorter decay                                           */
struct DrumSettings {
  /*                         KCK    SNR    CLP    HHC    HHO    TMH    TML    PRC */
  float tune[8] = { 0.50f, 0.50f, 0.50f, 0.55f, 0.50f, 0.62f, 0.38f, 0.50f };
  float decay[8] = { 0.60f, 0.45f, 0.40f, 0.30f, 0.65f, 0.48f, 0.52f, 0.38f };
  float volume[8] = { 0.85f, 0.75f, 0.70f, 0.65f, 0.60f, 0.70f, 0.70f, 0.65f };
  float noise_mix[8] = { 0.02f, 0.45f, 0.50f, 0.90f, 0.90f, 0.08f, 0.06f, 0.15f };
  uint8_t wave_idx[8] = { 8, 8, 8, 8, 8, 8, 8, 8 };
  /* All defaults = DRUM_DEFAULT_WAVE_IDX (8 = WT_SINE).
   * Changed via applyDrumWave(ch, idx) — persists across reboot.           */
  uint8_t kit = 0; /* DrumKitId: 0=TR-909 (factory) 1=808 2=Trap 3=House    */
  /* Global drum pitch multiplier — independent of master_pitch. */
  float pitch_mult = 0.60f; /* [MASTER_PITCH_MIN, MASTER_PITCH_MAX]; default ×0.60 */
};

/* ── Sequencer transport & patch config ──────────────────────────────────── */
struct SequencerSettings {
  uint16_t bpm = 120;          /* 40–240 BPM                              */
  uint8_t active_bank = 0;     /* 0–3 = A/B/C/D (engine supports 0–15)    */
  uint8_t active_chain = 0;    /* 0–3; UI model pins this to 0            */
  uint8_t length = 16;         /* 1–64 steps                              */
  int8_t transpose = 0;        /* ±12 semitones                           */
  uint8_t harp_scale = 0;      /* 0 = Major                               */
  uint8_t harp_patch = 0;      /* user bank slot 0                        */
  uint8_t seq_patch = 4;       /* user bank slot 4 (different timbre)     */
  int8_t oct_harp = 0;         /* octaveShift[0]                          */
  int8_t oct_seq = 0;          /* octaveShift[1]                          */
  uint16_t beam_gate_ms = 200; /* laser gate hold window in ms            */
  /* Harp continuous pitch-bend / manual tune multiplier (1.0 = unity).
   * Stored here (not HarpSettings) to preserve NVS field order. */
  float harp_pitch = 1.0f;
  /* Anti-stuck fail-safe: force-release a held note if the beam has not been
   * solidly broken for this many ms.  0 = disabled. */
  uint16_t beam_stuck_ms = 350;
  /* Working-image fields — save→reboot restores dashboard, SEQ view page,
   * and last-loaded pattern readouts. */
  uint8_t dashboard      = 1;  /* DashboardMode: 0 = HARP, 1 = SEQUENCER       */
  uint8_t ui_page        = 0;  /* seqUI_page: 0 = synth rows, 1 = drum rows    */
  uint8_t last_synth_pat = 0;  /* g_lastSynthPreset readout                    */
  uint8_t last_drum_pat  = 0;  /* g_lastDrumPreset  readout                    */
  /* Melody arpeggiator (SETTINGS_VERSION 0x0613→0x0614). */
  uint8_t seq_arp_en   = 0;
  uint8_t seq_arp_pat  = 0;
  uint8_t seq_arp_rate = 5;    /* default 1/16                                 */
  uint8_t seq_arp_gate = 2;    /* default 50%                                  */
  /* Harp arpeggiator (SETTINGS_VERSION 0x0614→0x0615). */
  uint8_t harp_arp_en   = 0;
  uint8_t harp_arp_pat  = 0;
  uint8_t harp_arp_rate = 2;   /* default 1/16                                 */
  uint8_t harp_arp_gate = 1;   /* default 50%                                  */
};

/* ── Master — vol, pitch, EQ, play mode, MIDI channels, pitch bend ──────── */
struct MasterSettings {
  float master_volume = 0.75f;
  float master_pitch = 1.00f; /* [MASTER_PITCH_MIN, MASTER_PITCH_MAX] */
  float eq_low = 0.00f;       /* ±12 dB shelf                         */
  float eq_high = 0.00f;
  uint8_t play_mode = 0; /* PlayMode::POLY8                       */
  uint8_t harp_midi_ch = 1;
  uint8_t seq_midi_ch = 2;
  uint8_t drum_midi_ch = 10; /* GM channel 10                         */
  uint8_t pb_up_semi = 2;
  uint8_t pb_down_semi = 2;
  bool pb_enabled = true;
};

/* ── FX chain — tube, DJ filter, drum sends, aux bus, slot indices ──────── */
struct FxSettings {
  /* Tube saturation insert */
  float tb_drive = 0.00f;
  float tb_tone = 0.50f;
  float tb_mix = 0.00f;
  /* DJ filter insert */
  float dj_freq = 1.00f; /* fully open = bypassed               */
  float dj_res = 0.10f;
  float dj_mix = 0.00f;
  /* Drum aux sends */
  float drum_rev_send = 0.00f;
  float drum_dly_send = 0.00f;
  /* Shared aux bus — delay time/feedback + reverb size/damping */
  float aux_dly_time = 0.35f; /* 0.0–1.5 s                           */
  float aux_dly_fb = 0.45f;   /* CMD_AUX_DLY_FB                        */
  float aux_rev_size = 0.55f; /* 0.0–0.95                            */
  float aux_rev_damp = 0.35f; /* CMD_AUX_REV_DMP                       */
  /* FX preset slot indices (0–15) */
  int master_fx_idx = 0;
  int harp_fx_idx_a = 0;
  int harp_fx_idx_b = 0;
  int seq_fx_idx_a = 0;
  int seq_fx_idx_b = 0;
  int drum_fx_idx_a = 0;
  int drum_fx_idx_b = 0;
  /* Per-instrument insert sends */
  float harp_dly_send = 0.00f;
  float harp_rev_send = 0.00f;
  float seq_dly_send = 0.00f;
  float seq_rev_send = 0.00f;
  /* Aux scene + link toggle (SETTINGS_VERSION 0x0615→0x0616). */
  int aux_scene_idx = 0;          /* last Room Scene row 0–15 (display index) */
  bool link_aux_to_insert = false; /* apply insert preset aux fields when ON */
};

/* ── D-BEAM expression ─────────────────────────────────────────────────── */
struct DBeamSettings {
  bool enabled = true;       /* sensor active on boot               */
  uint8_t curve = 0;         /* DBEAMCurve::LINEAR                  */
  /* ADC calibration anchors (12-bit 0–4095).  The adaptive peak normaliser in
   * adc_dma_processing_task is the primary gain path; offset/range seed the
   * hardware menu defaults. */
  uint16_t offset_adc = 2048u;
  uint16_t range_adc  = 1000u;
  float hw_gain = 1.2f;      /* amplitude gain — used in applyDBEAMCurve() */
  uint8_t route = 0;       /* DbeamRoute::OFF — user enables per session */
  float expr_attack = 0.50f; /* Branch B envelope attack (0.20–0.50) */
  float expr_release = 0.008f;/* Branch B envelope release (0.007–0.020) */
  /* Target synth: 0 = Harp engine, 1 = Melody (seq) synth. */
  uint8_t target = 0;      /* DbeamTarget::HARP */
};

/* ── Laser show — beam detect, per-scale cal, fog reject, projector anim ─ */
struct LaserSettings {
  bool show_mode = false;
  bool midi_hue_control = false;
  float base_hue = 0.00f;
  float hue_attack = 0.01f;
  float hue_decay = 0.10f;
  float hue_sustain = 1.00f;
  float hue_release = 0.20f;
  /* Per-scale beam-detect comparator margin (DAC threshold seed).
   * Factory seed = 1100: playable range without exaggerated reach and resists
   * higher-than-average fog density.  HARP-SETUP edits override and persist. */
  uint16_t margin[NUM_SCALES] = { 1100, 1100, 1100, 1100, 1100, 1100, 1100, 1100,
                                  1100, 1100, 1100, 1100, 1100, 1100, 1100, 1100 };
  /* Screensaver + per-scale beam calibration (white level, touch/release confirm, RGB). */
  bool screensaver = false;
  uint8_t white_level[NUM_SCALES]     = { 32, 64, 32, 64, 32, 64, 32, 40, 45, 50, 32, 64, 32, 10, 0, 0 };
  uint8_t touch_confirm[NUM_SCALES]   = { 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2 };
  uint8_t release_confirm[NUM_SCALES] = { 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3 };
  uint8_t scale_r[NUM_SCALES] = { 0, 255, 0, 255, 127, 0, 255, 200, 50, 255, 100, 255, 0, 255, 255, 255 };
  uint8_t scale_g[NUM_SCALES] = { 255, 0, 255, 127, 0, 255, 255, 50, 200, 100, 255, 0, 255, 255, 255, 255 };
  uint8_t scale_b[NUM_SCALES] = { 255, 255, 0, 0, 255, 127, 0, 255, 255, 255, 100, 100, 255, 255, 255, 255 };
  /* Independent per-string edge-comp (percent, 100 = ×1.00) for every scale. */
  uint8_t edge_comp[NUM_SCALES][8] = { EDGE_COMP_DEFAULT_ROWS };
  /* Fog-reject: differential gate.  fog_reject = enable (default OFF);
   * fog_margin = mV the active string must clear the across-string floor by. */
  uint8_t  fog_reject = 0;
  uint16_t fog_margin = 50;
  /* Projector animation: anim_mode (0=Pulse 1=Chase 2=Strobe 3=Wave) + drum-flash depth. */
  uint8_t  anim_mode = 0;
  float    drum_flash = 0.50f;
};

/* ── Mixer — volumes + mutes ─────────────────────────────────────────────── */
struct MixSettings {
  float harp_vol = 0.80f;
  float seq_vol = 0.80f;
  float drum_vol = 0.90f;
  bool harp_mute = false;
  bool seq_mute = false;
  bool drum_mute = false;
  /* Equal-power pan −1..+1 */
  float harp_pan = 0.f;
  float seq_pan = 0.f;
  float drum_pan = 0.f;
};

/* ═══════════════════════════════════════════════════════════════════════════
 * TOP-LEVEL BLOB
 * ═══════════════════════════════════════════════════════════════════════════ */
/* ── Song sequencer programs ─────────────────────────────────────────────── */
struct SongSettings {
  SongSlot slots[16]; /* all 16 song programs          */
  bool modeActive;    /* song vs pattern mode          */
  uint8_t activeSlot; /* 0-15                          */
  uint8_t _pad[2];
};

/* Standard CRC-32 (0xEDB88320 reflected, init/final 0xFFFFFFFF). */
static inline uint32_t crc32_buf(const uint8_t* p, size_t n) {
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < n; ++i) {
    crc ^= p[i];
    for (int j = 0; j < 8; ++j)
      crc = (crc >> 1) ^ (0xEDB88320u & -(uint32_t)(crc & 1u));
  }
  return crc ^ 0xFFFFFFFFu;
}

struct AllSettings {
  uint16_t version = SETTINGS_VERSION;
  uint32_t crc32 = 0;
  HarpSettings harp;
  SeqSettings seq;
  DrumSettings drum;
  SequencerSettings seqr; /* sequencer transport + patch config   */
  MasterSettings master;
  FxSettings fx;
  DBeamSettings dbeam;
  LaserSettings laser;
  MixSettings mix;
  SongSettings song; /* 16 song programs + song/pattern mode flag */

  /* In-place CRC — no stack copy of the full AllSettings blob on NvsWorker. */
  uint32_t calculate_crc() const {
    AllSettings* mut = const_cast<AllSettings*>(this);
    const uint32_t saved = mut->crc32;
    mut->crc32 = 0;
    const uint32_t result = crc32_buf(reinterpret_cast<const uint8_t*>(this), sizeof(AllSettings));
    mut->crc32 = saved;
    return result;
  }

  bool verify() const {
    if (version != SETTINGS_VERSION) return false;
    return crc32 == calculate_crc();
  }
};

inline AllSettings g_settings;

/* ═══════════════════════════════════════════════════════════════════════════
 * DIRECTION REFERENCE
 *
 *   A. Boot load:   NVS blob → g_settings → atomics → livePatch
 *                   settings_load() → settings_sync_to_ssot()
 *
 *   B. Performance save:  atomics → g_settings → NVS blob
 *                   settings_sync_from_ssot() → settings_save()
 *
 * The canonical sync entry points:
 *   settings_sync_to_ssot()   — call after settings_load() succeeds
 *   settings_sync_from_ssot() — call inside settings_save() before commit
 *   setupToFactoryDefaults()  — call on first boot or user factory reset
 *
 * livePatch arrays are DERIVED state — they are NOT stored directly.
 * syncLivePatchFromAtomics() (patches.h) rebuilds them from atomics.
 * ═══════════════════════════════════════════════════════════════════════════ */

/* ── helpers ─────────────────────────────────────────────────────────────── */
static inline float clamp01(float v) {
  return std::min(1.f, std::max(0.f, v));
}
static inline float clampf(float v, float lo, float hi) {
  return std::min(hi, std::max(lo, v));
}
static inline uint8_t clamp8(int v, int lo, int hi) {
  return (uint8_t)std::min(hi, std::max(lo, v));
}




/* ═══════════════════════════════════════════════════════════════════════════
 * DIRECTION A: g_settings → atomics → livePatch
 *
 * Ends with syncLivePatchFromAtomics() (patches.h) and loadHarpFx/SeqFx/DrumFx
 * so FX chain presets match the saved slot indices (not just the indices).
 * ═══════════════════════════════════════════════════════════════════════════ */
static inline void settings_sync_to_ssot() {

  /* ── Harp synth atomics ────────────────────────────────────────────── */
  harpWaveform.store(clampf((float)g_settings.harp.waveform, 0.f, 24.f), std::memory_order_relaxed);
  harpAttack.store(clamp01(g_settings.harp.attack), std::memory_order_relaxed);
  harpDecay.store(clamp01(g_settings.harp.decay), std::memory_order_relaxed);
  harpSustain.store(clamp01(g_settings.harp.sustain), std::memory_order_relaxed);
  harpRelease.store(clamp01(g_settings.harp.release), std::memory_order_relaxed);
  harpCutoff.store(clamp01(g_settings.harp.cutoff), std::memory_order_relaxed);
  harpResonance.store(clamp01(g_settings.harp.resonance), std::memory_order_relaxed);
  harpNoise.store(clamp01(g_settings.harp.noise), std::memory_order_relaxed);
  harpDetune.store(clampf(g_settings.harp.detune, -1.f, 1.f), std::memory_order_relaxed);
  harpLfoRate.store(clamp01(g_settings.harp.lfo_rate), std::memory_order_relaxed);
  harpLfoDepth.store(clamp01(g_settings.harp.lfo_depth), std::memory_order_relaxed);
  harpLfoRoute.store((int32_t)(g_settings.harp.lfo_route & 7u), std::memory_order_relaxed);
  harpOsc2Wave.store(clampf((float)g_settings.harp.osc2_wave, 0.f, 24.f), std::memory_order_relaxed);
  harpEnvCutAmount.store(clamp01(g_settings.harp.env_cut_amount), std::memory_order_relaxed);

  /* ── Seq synth atomics ─────────────────────────────────────────────── */
  seqWaveform.store(clampf((float)g_settings.seq.waveform, 0.f, 24.f), std::memory_order_relaxed);
  seqAttack.store(clamp01(g_settings.seq.attack), std::memory_order_relaxed);
  seqDecay.store(clamp01(g_settings.seq.decay), std::memory_order_relaxed);
  seqSustain.store(clamp01(g_settings.seq.sustain), std::memory_order_relaxed);
  seqRelease.store(clamp01(g_settings.seq.release), std::memory_order_relaxed);
  seqCutoff.store(clamp01(g_settings.seq.cutoff), std::memory_order_relaxed);
  seqResonance.store(clamp01(g_settings.seq.resonance), std::memory_order_relaxed);
  seqNoise.store(clamp01(g_settings.seq.noise), std::memory_order_relaxed);
  seqDetune.store(clampf(g_settings.seq.detune, -1.f, 1.f), std::memory_order_relaxed);
  seqLfoRate.store(clamp01(g_settings.seq.lfo_rate), std::memory_order_relaxed);
  seqLfoDepth.store(clamp01(g_settings.seq.lfo_depth), std::memory_order_relaxed);
  seqLfoRoute.store((int32_t)(g_settings.seq.lfo_route & 7u), std::memory_order_relaxed);
  seqOsc2Wave.store(clampf((float)g_settings.seq.osc2_wave, 0.f, 24.f), std::memory_order_relaxed);
  seqEnvCutAmount.store(clamp01(g_settings.seq.env_cut_amount), std::memory_order_relaxed);

  /* ── Drum atomics — 4 params + wave_idx per channel ───────────────── */
  for (int i = 0; i < 8; ++i) {
    drumTune[i].store(clamp01(g_settings.drum.tune[i]), std::memory_order_relaxed);
    drumDecay[i].store(clamp01(g_settings.drum.decay[i]), std::memory_order_relaxed);
    drumVolume[i].store(clamp01(g_settings.drum.volume[i]), std::memory_order_relaxed);
    drumNoiseMix[i].store(clamp01(g_settings.drum.noise_mix[i]), std::memory_order_relaxed);
    drumWaveIdx[i].store(g_settings.drum.wave_idx[i] % NUM_WAVE_TABLES, std::memory_order_relaxed);
  }
  drumKit.store((g_settings.drum.kit < (uint8_t)DrumKitId::COUNT) ? g_settings.drum.kit : 0u,
                std::memory_order_relaxed);
  drumPitchMult.store(clampf(g_settings.drum.pitch_mult, MASTER_PITCH_MIN, MASTER_PITCH_MAX),
                      std::memory_order_relaxed);

  /* ── Sequencer transport ───────────────────────────────────────────── */
  seqBpm.store((int32_t)std::min<uint16_t>(240u, std::max<uint16_t>(40u, g_settings.seqr.bpm)),
               std::memory_order_relaxed);
  seqActiveBank.store(g_settings.seqr.active_bank & 15u, std::memory_order_relaxed);
  seqActiveChain.store(g_settings.seqr.active_chain & 3u, std::memory_order_relaxed);
  seqLength.store(std::min<uint8_t>(64u, std::max<uint8_t>(1u, g_settings.seqr.length)),
                  std::memory_order_relaxed);
  seqTranspose.store(std::min<int32_t>(12, std::max<int32_t>(-12, (int32_t)g_settings.seqr.transpose)),
                     std::memory_order_relaxed);
  /* harpPitchMult is the BEND control: ±1 octave → [0.5, 2.0].
   * MASTER_PITCH_MIN/MAX (0.25–4.0) is the global master-tune range — using it
   * here let corrupt NVS values in 0.25–0.49 or 2.01–4.0 bake in without error,
   * making the BEND knob appear broken after a factory-reset NVS migration. */
  harpPitchMult.store(clampf(g_settings.seqr.harp_pitch, 0.5f, 2.0f),
                      std::memory_order_relaxed);
  harpScaleIndex.store(g_settings.seqr.harp_scale & (NUM_SCALES - 1), std::memory_order_relaxed);
  harpPatchIndex.store((uint32_t)clampRecallPatchIndex((int)g_settings.seqr.harp_patch),
                       std::memory_order_relaxed);
  seqPatchIndex.store((uint32_t)clampRecallPatchIndex((int)g_settings.seqr.seq_patch),
                      std::memory_order_relaxed);
  octaveShift[0].store(std::min<int32_t>(4, std::max<int32_t>(-4, (int32_t)g_settings.seqr.oct_harp)),
                       std::memory_order_relaxed);
  octaveShift[1].store(std::min<int32_t>(4, std::max<int32_t>(-4, (int32_t)g_settings.seqr.oct_seq)),
                       std::memory_order_relaxed);
  beamGateHoldMs = std::min<uint32_t>(BEAM_GATE_HOLD_MAX,
                                      (uint32_t)g_settings.seqr.beam_gate_ms);
  beamStuckReleaseMs = std::min<uint32_t>(BEAM_STUCK_RELEASE_MAX,
                                          (uint32_t)g_settings.seqr.beam_stuck_ms);
  /* Restore working image (dashboard, SEQ page, last pattern readouts). */
  activeDashboard.store(g_settings.seqr.dashboard ? DashboardMode::SEQUENCER
                                                  : DashboardMode::HARP,
                        std::memory_order_relaxed);
  seqUI_page.store(g_settings.seqr.ui_page & 1, std::memory_order_relaxed);
  g_lastSynthPreset.store(g_settings.seqr.last_synth_pat, std::memory_order_relaxed);
  g_lastDrumPreset .store(g_settings.seqr.last_drum_pat,  std::memory_order_relaxed);
  seqArpEnabled.store(g_settings.seqr.seq_arp_en != 0, std::memory_order_relaxed);
  seqArpPattern.store(std::min<uint8_t>(7u, g_settings.seqr.seq_arp_pat), std::memory_order_relaxed);
  seqArpRate.store(std::min<uint8_t>(7u, g_settings.seqr.seq_arp_rate), std::memory_order_relaxed);
  seqArpGate.store(std::min<uint8_t>(7u, g_settings.seqr.seq_arp_gate), std::memory_order_relaxed);
  harpArpEnabled.store(g_settings.seqr.harp_arp_en != 0, std::memory_order_relaxed);
  harpArpPattern.store(std::min<uint8_t>(3u, g_settings.seqr.harp_arp_pat), std::memory_order_relaxed);
  harpArpRate.store(std::min<uint8_t>(3u, g_settings.seqr.harp_arp_rate), std::memory_order_relaxed);
  harpArpGate.store(std::min<uint8_t>(3u, g_settings.seqr.harp_arp_gate), std::memory_order_relaxed);
  /* BPM is the SSOT atomic seqBpm (stored above); the DMA-locked step engine
   * reads it directly each buffer — no separate clock object to sync.          */

  /* ── Master ────────────────────────────────────────────────────────── */
  masterVol.store(clamp01(g_settings.master.master_volume), std::memory_order_relaxed);
  masterPitch.store(clampf(g_settings.master.master_pitch, MASTER_PITCH_MIN, MASTER_PITCH_MAX), std::memory_order_relaxed);
  masterEqLow.store(clampf(g_settings.master.eq_low, -12.f, 12.f), std::memory_order_relaxed);
  masterEqHigh.store(clampf(g_settings.master.eq_high, -12.f, 12.f), std::memory_order_relaxed);
  currentPlayMode.store((PlayMode)std::min<uint8_t>(2u, g_settings.master.play_mode), std::memory_order_relaxed);
  wireHarpMidiChannel.store(std::min<uint8_t>(16u, std::max<uint8_t>(1u, g_settings.master.harp_midi_ch)), std::memory_order_relaxed);
  wireSeqMidiChannel.store(std::min<uint8_t>(16u, std::max<uint8_t>(1u, g_settings.master.seq_midi_ch)), std::memory_order_relaxed);
  wireDrumMidiChannel.store(std::min<uint8_t>(16u, std::max<uint8_t>(1u, g_settings.master.drum_midi_ch)), std::memory_order_relaxed);
  pbMapping.enabled.store(g_settings.master.pb_enabled, std::memory_order_relaxed);
  pbMapping.upSemi.store(g_settings.master.pb_up_semi, std::memory_order_relaxed);
  pbMapping.downSemi.store(g_settings.master.pb_down_semi, std::memory_order_relaxed);

  /* ── FX chain atomics ──────────────────────────────────────────────── */
  tbDrive.store(clamp01(g_settings.fx.tb_drive), std::memory_order_relaxed);
  tbTone.store(clamp01(g_settings.fx.tb_tone), std::memory_order_relaxed);
  tbMix.store(clamp01(g_settings.fx.tb_mix), std::memory_order_relaxed);
  djFreq.store(clamp01(g_settings.fx.dj_freq), std::memory_order_relaxed);
  djRes.store(clamp01(g_settings.fx.dj_res), std::memory_order_relaxed);
  djMix.store(clamp01(g_settings.fx.dj_mix), std::memory_order_relaxed);
  drumRevSend.store(clamp01(g_settings.fx.drum_rev_send), std::memory_order_relaxed);
  drumDlySend.store(clamp01(g_settings.fx.drum_dly_send), std::memory_order_relaxed);
  masterAuxDlyTime.store(clampf(g_settings.fx.aux_dly_time, 0.f, 1.5f), std::memory_order_relaxed);
  masterAuxDlyFb.store(clampf(g_settings.fx.aux_dly_fb, 0.f, 0.95f), std::memory_order_relaxed);
  masterAuxRevSize.store(clampf(g_settings.fx.aux_rev_size, 0.f, 0.95f), std::memory_order_relaxed);
  masterAuxRevDamp.store(clamp01(g_settings.fx.aux_rev_damp), std::memory_order_relaxed);
  auxSceneIndex.store(std::max(0, std::min(15, g_settings.fx.aux_scene_idx)), std::memory_order_relaxed);
  linkAuxToInsertPreset.store(g_settings.fx.link_aux_to_insert, std::memory_order_relaxed);

  /* FX slot indices → atomics, then load the matching FX chain presets */
  masterFxIndex.store(g_settings.fx.master_fx_idx, std::memory_order_relaxed);
  harpFxIndex.store(g_settings.fx.harp_fx_idx_a, std::memory_order_relaxed);
  harpFxIndexB.store(g_settings.fx.harp_fx_idx_b, std::memory_order_relaxed);
  seqFxIndex.store(g_settings.fx.seq_fx_idx_a, std::memory_order_relaxed);
  seqFxIndexB.store(g_settings.fx.seq_fx_idx_b, std::memory_order_relaxed);
  drumFxIndexA.store(g_settings.fx.drum_fx_idx_a, std::memory_order_relaxed);
  drumFxIndexB.store(g_settings.fx.drum_fx_idx_b, std::memory_order_relaxed);
  /* Load FX chain presets so runtime state matches saved slot indices */
  fx.loadMasterFx(g_settings.fx.master_fx_idx);
  loadHarpFx(g_settings.fx.harp_fx_idx_a);
  loadHarpFxB(g_settings.fx.harp_fx_idx_b);
  loadSeqFx(g_settings.fx.seq_fx_idx_a);
  loadSeqFxB(g_settings.fx.seq_fx_idx_b);
  loadDrumFx(g_settings.fx.drum_fx_idx_a);
  loadDrumFxB(g_settings.fx.drum_fx_idx_b);

  /* Insert sends — non-atomic, guarded by patchMux */
  portENTER_CRITICAL(&patchMux);
  fx.harpInsert.dly_send = clamp01(g_settings.fx.harp_dly_send);
  fx.harpInsert.rev_send = clamp01(g_settings.fx.harp_rev_send);
  fx.seqInsert.dly_send = clamp01(g_settings.fx.seq_dly_send);
  fx.seqInsert.rev_send = clamp01(g_settings.fx.seq_rev_send);
  portEXIT_CRITICAL(&patchMux);

  /* ── D-BEAM ─────────────────────────────────────────────────────────── */
  dbeamEnabled.store(g_settings.dbeam.enabled, std::memory_order_relaxed);
  currentDbeamCurve.store((DBEAMCurve)std::min<uint8_t>(4u, g_settings.dbeam.curve), std::memory_order_relaxed);
  dbeamHWCfg.offsetAdc = (int)g_settings.dbeam.offset_adc;
  dbeamHWCfg.rangeAdc  = (int)std::max<uint16_t>(1u, g_settings.dbeam.range_adc);
  dbeamHWCfg.gain = clampf(g_settings.dbeam.hw_gain, 0.1f, 10.f);
  currentDbeamRoute.store((DbeamRoute)std::min<uint8_t>(3u, g_settings.dbeam.route), std::memory_order_relaxed);
  currentDbeamTarget.store((DbeamTarget)std::min<uint8_t>(1u, g_settings.dbeam.target), std::memory_order_relaxed);
  dbeamExprAttack.store(clampf(g_settings.dbeam.expr_attack,
                        DBEAM_EXPR_ATTACK_MIN,  DBEAM_EXPR_ATTACK_MAX),  std::memory_order_relaxed);
  dbeamExprRelease.store(clampf(g_settings.dbeam.expr_release,
                        DBEAM_EXPR_RELEASE_MIN, DBEAM_EXPR_RELEASE_MAX), std::memory_order_relaxed);

  /* ── Laser show ─────────────────────────────────────────────────────── */
  laserShowMode.store(g_settings.laser.show_mode, std::memory_order_relaxed);
  midiHueControl.store(g_settings.laser.midi_hue_control, std::memory_order_relaxed);
  laserBaseHue.store(clamp01(g_settings.laser.base_hue), std::memory_order_relaxed);
  /* HUE ADSR times stored in seconds — clamp to per-stage full-scale. */
  hueAttack.store(clampf(g_settings.laser.hue_attack,  0.005f, HUE_ATK_MAX_S), std::memory_order_relaxed);
  hueDecay.store(clampf(g_settings.laser.hue_decay,    0.005f, HUE_DEC_MAX_S), std::memory_order_relaxed);
  hueSustain.store(clamp01(g_settings.laser.hue_sustain), std::memory_order_relaxed);
  hueRelease.store(clampf(g_settings.laser.hue_release, 0.005f, HUE_REL_MAX_S), std::memory_order_relaxed);
  laserScreensaver.store(g_settings.laser.screensaver, std::memory_order_relaxed);
  laserShowAnim.store((LaserShowAnim)(g_settings.laser.anim_mode & 3), std::memory_order_relaxed);
  laserDrumFlash.store(clamp01(g_settings.laser.drum_flash), std::memory_order_relaxed);
  for (int s = 0; s < NUM_SCALES; ++s) {
    scaleMargin[s]         = (uint16_t)std::min<uint16_t>(2000u, g_settings.laser.margin[s]);
    scaleWhiteLevel[s]     = g_settings.laser.white_level[s];
    scaleTouchConfirm[s]   = g_settings.laser.touch_confirm[s];
    scaleReleaseConfirm[s] = g_settings.laser.release_confirm[s];
    scaleR[s]              = g_settings.laser.scale_r[s];
    scaleG[s]              = g_settings.laser.scale_g[s];
    scaleB[s]              = g_settings.laser.scale_b[s];
  }
  /* Per-scale edge compensation (percent per string). */
  for (int s = 0; s < NUM_SCALES; ++s)
    for (int i = 0; i < 8; ++i)
      edgeComp[s][i] = (uint8_t)std::min<uint8_t>(EDGE_COMP_PCT_MAX,
                       std::max<uint8_t>(EDGE_COMP_PCT_MIN, g_settings.laser.edge_comp[s][i]));
  /* Fog-reject enable + margin (mV). */
  fogRejectEnabled.store(g_settings.laser.fog_reject != 0, std::memory_order_relaxed);
  fogRejectMargin .store((int)std::min<uint16_t>((uint16_t)FOG_MARGIN_MAX, g_settings.laser.fog_margin),
                         std::memory_order_relaxed);

  /* ── Mix ─────────────────────────────────────────────────────────────── */
  mixHarpVol.store(clamp01(g_settings.mix.harp_vol), std::memory_order_relaxed);
  mixSeqVol.store(clamp01(g_settings.mix.seq_vol), std::memory_order_relaxed);
  mixDrumsVol.store(clamp01(g_settings.mix.drum_vol), std::memory_order_relaxed);
  mixHarpMute.store(g_settings.mix.harp_mute, std::memory_order_relaxed);
  mixSeqMute.store(g_settings.mix.seq_mute, std::memory_order_relaxed);
  mixDrumsMute.store(g_settings.mix.drum_mute, std::memory_order_relaxed);
  mixHarpPan.store(clampf(g_settings.mix.harp_pan, -1.f, 1.f), std::memory_order_relaxed);
  mixSeqPan.store(clampf(g_settings.mix.seq_pan, -1.f, 1.f), std::memory_order_relaxed);
  mixDrumsPan.store(clampf(g_settings.mix.drum_pan, -1.f, 1.f), std::memory_order_relaxed);

  /* Song programs + mode flag */
  memcpy(hwSongData, g_settings.song.slots, sizeof(hwSongData));
  songModeActive.store(g_settings.song.modeActive, std::memory_order_relaxed);
  activeSongSlot.store(g_settings.song.activeSlot & 15u, std::memory_order_relaxed);

  std::atomic_thread_fence(std::memory_order_release);
  syncLivePatchFromAtomics(); /* patches.h */

  displayDirty.store(true, std::memory_order_relaxed);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * DIRECTION B: atomics → g_settings (before NVS write)
 * ═══════════════════════════════════════════════════════════════════════════ */
static inline void settings_sync_from_ssot() {

  /* ── Harp ────────────────────────────────────────────────────────────── */
  g_settings.harp.waveform = (uint16_t)harpWaveform.load(std::memory_order_relaxed);
  g_settings.harp.attack = harpAttack.load(std::memory_order_relaxed);
  g_settings.harp.decay = harpDecay.load(std::memory_order_relaxed);
  g_settings.harp.sustain = harpSustain.load(std::memory_order_relaxed);
  g_settings.harp.release = harpRelease.load(std::memory_order_relaxed);
  g_settings.harp.cutoff = harpCutoff.load(std::memory_order_relaxed);
  g_settings.harp.resonance = harpResonance.load(std::memory_order_relaxed);
  g_settings.harp.noise = harpNoise.load(std::memory_order_relaxed);
  g_settings.harp.detune = harpDetune.load(std::memory_order_relaxed);
  g_settings.harp.lfo_rate = harpLfoRate.load(std::memory_order_relaxed);
  g_settings.harp.lfo_depth = harpLfoDepth.load(std::memory_order_relaxed);
  g_settings.harp.lfo_route = (uint8_t)harpLfoRoute.load(std::memory_order_relaxed);
  g_settings.harp.osc2_wave = (uint16_t)harpOsc2Wave.load(std::memory_order_relaxed);
  g_settings.harp.env_cut_amount = harpEnvCutAmount.load(std::memory_order_relaxed);

  /* ── Seq ─────────────────────────────────────────────────────────────── */
  g_settings.seq.waveform = (uint16_t)seqWaveform.load(std::memory_order_relaxed);
  g_settings.seq.attack = seqAttack.load(std::memory_order_relaxed);
  g_settings.seq.decay = seqDecay.load(std::memory_order_relaxed);
  g_settings.seq.sustain = seqSustain.load(std::memory_order_relaxed);
  g_settings.seq.release = seqRelease.load(std::memory_order_relaxed);
  g_settings.seq.cutoff = seqCutoff.load(std::memory_order_relaxed);
  g_settings.seq.resonance = seqResonance.load(std::memory_order_relaxed);
  g_settings.seq.noise = seqNoise.load(std::memory_order_relaxed);
  g_settings.seq.detune = seqDetune.load(std::memory_order_relaxed);
  g_settings.seq.lfo_rate = seqLfoRate.load(std::memory_order_relaxed);
  g_settings.seq.lfo_depth = seqLfoDepth.load(std::memory_order_relaxed);
  g_settings.seq.lfo_route = (uint8_t)seqLfoRoute.load(std::memory_order_relaxed);
  g_settings.seq.osc2_wave = (uint16_t)seqOsc2Wave.load(std::memory_order_relaxed);
  g_settings.seq.env_cut_amount = seqEnvCutAmount.load(std::memory_order_relaxed);

  /* ── Drum ────────────────────────────────────────────────────────────── */
  for (int i = 0; i < 8; ++i) {
    g_settings.drum.tune[i] = drumTune[i].load(std::memory_order_relaxed);
    g_settings.drum.decay[i] = drumDecay[i].load(std::memory_order_relaxed);
    g_settings.drum.volume[i] = drumVolume[i].load(std::memory_order_relaxed);
    g_settings.drum.noise_mix[i] = drumNoiseMix[i].load(std::memory_order_relaxed);
    g_settings.drum.wave_idx[i] = drumWaveIdx[i].load(std::memory_order_relaxed);
  }
  g_settings.drum.kit = drumKit.load(std::memory_order_relaxed);
  g_settings.drum.pitch_mult = drumPitchMult.load(std::memory_order_relaxed);

  /* ── Sequencer transport ─────────────────────────────────────────────── */
  g_settings.seqr.bpm = (uint16_t)seqBpm.load(std::memory_order_relaxed);
  g_settings.seqr.active_bank = seqActiveBank.load(std::memory_order_relaxed);
  g_settings.seqr.active_chain = seqActiveChain.load(std::memory_order_relaxed);
  g_settings.seqr.length = seqLength.load(std::memory_order_relaxed);
  g_settings.seqr.transpose = (int8_t)seqTranspose.load(std::memory_order_relaxed);
  g_settings.seqr.harp_pitch = harpPitchMult.load(std::memory_order_relaxed);
  g_settings.seqr.harp_scale = (uint8_t)harpScaleIndex.load(std::memory_order_relaxed);
  g_settings.seqr.harp_patch = (uint8_t)harpPatchIndex.load(std::memory_order_relaxed);
  g_settings.seqr.seq_patch = (uint8_t)seqPatchIndex.load(std::memory_order_relaxed);
  g_settings.seqr.oct_harp = (int8_t)octaveShift[0].load(std::memory_order_relaxed);
  g_settings.seqr.oct_seq = (int8_t)octaveShift[1].load(std::memory_order_relaxed);
  g_settings.seqr.beam_gate_ms = (uint16_t)std::min<uint32_t>(BEAM_GATE_HOLD_MAX, beamGateHoldMs);
  g_settings.seqr.beam_stuck_ms = (uint16_t)std::min<uint32_t>(BEAM_STUCK_RELEASE_MAX, beamStuckReleaseMs);
  /* Working-image snapshot (dashboard / view page / last pattern). */
  g_settings.seqr.dashboard =
      (uint8_t)(activeDashboard.load(std::memory_order_relaxed) == DashboardMode::SEQUENCER ? 1u : 0u);
  g_settings.seqr.ui_page        = (uint8_t)(seqUI_page.load(std::memory_order_relaxed) & 1);
  g_settings.seqr.last_synth_pat = g_lastSynthPreset.load(std::memory_order_relaxed);
  g_settings.seqr.last_drum_pat  = g_lastDrumPreset.load(std::memory_order_relaxed);
  g_settings.seqr.seq_arp_en   = seqArpEnabled.load(std::memory_order_relaxed) ? 1u : 0u;
  g_settings.seqr.seq_arp_pat  = seqArpPattern.load(std::memory_order_relaxed);
  g_settings.seqr.seq_arp_rate = seqArpRate.load(std::memory_order_relaxed);
  g_settings.seqr.seq_arp_gate = seqArpGate.load(std::memory_order_relaxed);
  g_settings.seqr.harp_arp_en   = harpArpEnabled.load(std::memory_order_relaxed) ? 1u : 0u;
  g_settings.seqr.harp_arp_pat  = harpArpPattern.load(std::memory_order_relaxed);
  g_settings.seqr.harp_arp_rate = harpArpRate.load(std::memory_order_relaxed);
  g_settings.seqr.harp_arp_gate = harpArpGate.load(std::memory_order_relaxed);

  /* ── Master ─────────────────────────────────────────────────────────── */
  g_settings.master.master_volume = masterVol.load(std::memory_order_relaxed);
  g_settings.master.master_pitch = masterPitch.load(std::memory_order_relaxed);
  g_settings.master.eq_low = masterEqLow.load(std::memory_order_relaxed);
  g_settings.master.eq_high = masterEqHigh.load(std::memory_order_relaxed);
  g_settings.master.play_mode = (uint8_t)currentPlayMode.load(std::memory_order_relaxed);
  g_settings.master.harp_midi_ch = wireHarpMidiChannel.load(std::memory_order_relaxed);
  g_settings.master.seq_midi_ch = wireSeqMidiChannel.load(std::memory_order_relaxed);
  g_settings.master.drum_midi_ch = wireDrumMidiChannel.load(std::memory_order_relaxed);
  g_settings.master.pb_up_semi   = pbMapping.upSemi.load(std::memory_order_relaxed);
  g_settings.master.pb_down_semi = pbMapping.downSemi.load(std::memory_order_relaxed);
  g_settings.master.pb_enabled   = pbMapping.enabled.load(std::memory_order_relaxed);

  /* ── FX ──────────────────────────────────────────────────────────────── */
  g_settings.fx.tb_drive = tbDrive.load(std::memory_order_relaxed);
  g_settings.fx.tb_tone = tbTone.load(std::memory_order_relaxed);
  g_settings.fx.tb_mix = tbMix.load(std::memory_order_relaxed);
  g_settings.fx.dj_freq = djFreq.load(std::memory_order_relaxed);
  g_settings.fx.dj_res = djRes.load(std::memory_order_relaxed);
  g_settings.fx.dj_mix = djMix.load(std::memory_order_relaxed);
  g_settings.fx.drum_rev_send = drumRevSend.load(std::memory_order_relaxed);
  g_settings.fx.drum_dly_send = drumDlySend.load(std::memory_order_relaxed);
  g_settings.fx.aux_dly_time = masterAuxDlyTime.load(std::memory_order_relaxed);
  g_settings.fx.aux_dly_fb = masterAuxDlyFb.load(std::memory_order_relaxed);
  g_settings.fx.aux_rev_size = masterAuxRevSize.load(std::memory_order_relaxed);
  g_settings.fx.aux_rev_damp = masterAuxRevDamp.load(std::memory_order_relaxed);
  g_settings.fx.aux_scene_idx = auxSceneIndex.load(std::memory_order_relaxed);
  g_settings.fx.link_aux_to_insert = linkAuxToInsertPreset.load(std::memory_order_relaxed);
  g_settings.fx.master_fx_idx = masterFxIndex.load(std::memory_order_relaxed);
  g_settings.fx.harp_fx_idx_a = harpFxIndex.load(std::memory_order_relaxed);
  g_settings.fx.harp_fx_idx_b = harpFxIndexB.load(std::memory_order_relaxed);
  g_settings.fx.seq_fx_idx_a = seqFxIndex.load(std::memory_order_relaxed);
  g_settings.fx.seq_fx_idx_b = seqFxIndexB.load(std::memory_order_relaxed);
  g_settings.fx.drum_fx_idx_a = drumFxIndexA.load(std::memory_order_relaxed);
  g_settings.fx.drum_fx_idx_b = drumFxIndexB.load(std::memory_order_relaxed);
  portENTER_CRITICAL(&patchMux);
  g_settings.fx.harp_dly_send = fx.harpInsert.dly_send;
  g_settings.fx.harp_rev_send = fx.harpInsert.rev_send;
  g_settings.fx.seq_dly_send = fx.seqInsert.dly_send;
  g_settings.fx.seq_rev_send = fx.seqInsert.rev_send;
  portEXIT_CRITICAL(&patchMux);

  /* ── D-BEAM ──────────────────────────────────────────────────────────── */
  g_settings.dbeam.enabled = dbeamEnabled.load(std::memory_order_relaxed);
  g_settings.dbeam.curve = (uint8_t)currentDbeamCurve.load(std::memory_order_relaxed);
  g_settings.dbeam.offset_adc = (uint16_t)std::max<int>(0, dbeamHWCfg.offsetAdc);
  g_settings.dbeam.range_adc  = (uint16_t)std::max<int>(1, dbeamHWCfg.rangeAdc);
  g_settings.dbeam.hw_gain = dbeamHWCfg.gain;
  g_settings.dbeam.route = (uint8_t)currentDbeamRoute.load(std::memory_order_relaxed);
  g_settings.dbeam.target = (uint8_t)currentDbeamTarget.load(std::memory_order_relaxed);
  g_settings.dbeam.expr_attack  = dbeamExprAttack.load(std::memory_order_relaxed);
  g_settings.dbeam.expr_release = dbeamExprRelease.load(std::memory_order_relaxed);

  /* ── Laser ───────────────────────────────────────────────────────────── */
  g_settings.laser.show_mode = laserShowMode.load(std::memory_order_relaxed);
  g_settings.laser.midi_hue_control = midiHueControl.load(std::memory_order_relaxed);
  g_settings.laser.base_hue = laserBaseHue.load(std::memory_order_relaxed);
  g_settings.laser.hue_attack = hueAttack.load(std::memory_order_relaxed);
  g_settings.laser.hue_decay = hueDecay.load(std::memory_order_relaxed);
  g_settings.laser.hue_sustain = hueSustain.load(std::memory_order_relaxed);
  g_settings.laser.hue_release = hueRelease.load(std::memory_order_relaxed);
  g_settings.laser.screensaver = laserScreensaver.load(std::memory_order_relaxed);
  g_settings.laser.anim_mode  = (uint8_t)laserShowAnim.load(std::memory_order_relaxed);
  g_settings.laser.drum_flash = laserDrumFlash.load(std::memory_order_relaxed);
  for (int s = 0; s < NUM_SCALES; ++s) {
    g_settings.laser.margin[s]           = scaleMargin[s];
    g_settings.laser.white_level[s]     = scaleWhiteLevel[s];
    g_settings.laser.touch_confirm[s]   = scaleTouchConfirm[s];
    g_settings.laser.release_confirm[s] = scaleReleaseConfirm[s];
    g_settings.laser.scale_r[s]         = scaleR[s];
    g_settings.laser.scale_g[s]         = scaleG[s];
    g_settings.laser.scale_b[s]         = scaleB[s];
  }
  /* Per-scale edge compensation (percent per string). */
  for (int s = 0; s < NUM_SCALES; ++s)
    for (int i = 0; i < 8; ++i)
      g_settings.laser.edge_comp[s][i] = edgeComp[s][i];
  /* Fog-reject enable + margin (mV). */
  g_settings.laser.fog_reject = fogRejectEnabled.load(std::memory_order_relaxed) ? 1 : 0;
  g_settings.laser.fog_margin = (uint16_t)fogRejectMargin.load(std::memory_order_relaxed);

  /* ── Mix ─────────────────────────────────────────────────────────────── */
  /* When D-BEAM VOLUME pedal is active the live bus level may be expression-driven;
   * a transient dip; persist the rest baseline so a save never bakes in a
   * hand-over-sensor attenuation. */
  const bool dbVolPedal = (currentDbeamRoute.load(std::memory_order_relaxed) == DbeamRoute::VOLUME);
  g_settings.mix.harp_vol = dbVolPedal ? dbeamVolBaseHarp.load(std::memory_order_relaxed)
                                       : mixHarpVol.load(std::memory_order_relaxed);
  g_settings.mix.seq_vol  = dbVolPedal ? dbeamVolBaseSeq.load(std::memory_order_relaxed)
                                       : mixSeqVol.load(std::memory_order_relaxed);
  g_settings.mix.drum_vol = mixDrumsVol.load(std::memory_order_relaxed);
  g_settings.mix.harp_mute = mixHarpMute.load(std::memory_order_relaxed);
  g_settings.mix.seq_mute = mixSeqMute.load(std::memory_order_relaxed);
  g_settings.mix.drum_mute = mixDrumsMute.load(std::memory_order_relaxed);
  g_settings.mix.harp_pan = mixHarpPan.load(std::memory_order_relaxed);
  g_settings.mix.seq_pan = mixSeqPan.load(std::memory_order_relaxed);
  g_settings.mix.drum_pan = mixDrumsPan.load(std::memory_order_relaxed);

  /* ── Song programs + mode flag ─────────────────────────────────────────── */
  memcpy(g_settings.song.slots, hwSongData, sizeof(hwSongData));
  g_settings.song.modeActive = songModeActive.load(std::memory_order_relaxed);
  g_settings.song.activeSlot = activeSongSlot.load(std::memory_order_relaxed);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * PERSISTED EXTRAS — sequencer patterns + sparse user/seq patch banks
 *
 * These live in SEPARATE NVS blobs from "settings" (each with its own version
 * + CRC) so a corrupt/oversized extra can never invalidate the core settings.
 *
 *   "patterns"  PatternsBlob — full hwSeqData[16][4][16] snapshot (~2 KB).
 *   "banks"     BanksBlob    — SPARSE: only the userBank/seqBank slots that
 *               differ from the PROGMEM factory SOUND_BANK are stored, as
 *               (slot, 16-param) pairs.  NUM_PATCHES=256 makes a verbatim
 *               2×8 KB dump too large for the stock 20 KB NVS partition, so we
 *               store deltas only.  Up to MAX_BANK_OVR customised slots per
 *               bank are persisted (generous for live use); beyond that, extra
 *               customisations are not saved (factory recall still works).
 *
 * Thread-safety: hwSeqData / userBank / seqBank are guarded by patchMux.  The
 * snapshot reads take patchMux in tiny per-slot (or one-shot 2 KB) windows; the
 * slow flash write then runs on the heap copy with NO lock held.  Saves run
 * inside the NVS save handshake (audio muted, Core 1 parked) so even the 2 KB
 * pattern copy is safe.
 * ═══════════════════════════════════════════════════════════════════════════ */

static constexpr uint16_t PATTERNS_VERSION    = 0x0003; /* v3: + per-pattern transpose */
static constexpr uint16_t PATTERNS_VERSION_V2 = 0x0002; /* v2: 64 steps/row (uint64)  */
static constexpr uint16_t PATTERNS_VERSION_V1 = 0x0001; /* v1: 16 steps/row (uint16)  */
static constexpr uint16_t BANKS_VERSION    = 0x0001;
static constexpr uint16_t USRNAMES_VERSION = 0x0001;
static constexpr uint16_t MOTION_VERSION   = 0x0001;
static constexpr uint16_t MAX_BANK_OVR     = 64;  /* max customised slots / bank */
static constexpr uint16_t MAX_MOTION_LANES = 128; /* max persisted P-lock lanes  */

struct PatternsBlobV1 {
  uint16_t version;
  uint16_t _pad;
  uint32_t crc32;
  uint16_t data[16][4][16];       /* legacy: 16 steps per row            */
};

/* v2 layout — kept for one-time migration to v3 (adds per-pattern transpose). */
struct PatternsBlobV2 {
  uint16_t version;
  uint16_t _pad;
  uint32_t crc32;
  uint64_t data[16][4][16];
};

struct PatternsBlob {
  uint16_t version;
  uint16_t _pad;
  uint32_t crc32;                 /* zeroed while hashing */
  uint64_t data[16][4][16];       /* mirrors hwSeqData layout exactly    */
  int8_t   transpose[16][4];      /* per-pattern melody transpose −12..+12 */
};

struct BankOverride {
  uint16_t slot;                      /* patch index 0..NUM_PATCHES-1 */
  uint16_t params[PARAMS_PER_PRESET]; /* the 16 customised values     */
};

struct BanksBlob {
  uint16_t version;
  uint16_t harpCount;             /* valid entries in harp[]  */
  uint16_t seqCount;              /* valid entries in seq[]   */
  uint16_t _pad;
  uint32_t crc32;                 /* zeroed while hashing */
  BankOverride harp[MAX_BANK_OVR];
  BankOverride seq[MAX_BANK_OVR];
};

/* Sparse user-slot name store (only renamed slots persisted). */
struct NameOverride {
  uint16_t slot;                  /* user slot index 0..NUM_USER_SLOTS-1 */
  char     name[16];              /* 15 chars + NUL                      */
};
struct UserNamesBlob {
  uint16_t version;
  uint16_t harpCount;
  uint16_t seqCount;
  uint16_t _pad;
  uint32_t crc32;                 /* zeroed while hashing */
  NameOverride harp[NUM_USER_SLOTS];
  NameOverride seq[NUM_USER_SLOTS];
};

/* Sparse user-pattern library (flags bit0 = slot populated). */
static constexpr uint16_t USRPAT_VERSION       = 0x0001;
static constexpr uint16_t USRPATNAMES_VERSION = 0x0001;
static constexpr uint16_t MAX_USER_PAT_OVR   = 64;

struct UserPatOverride {
  uint16_t slot;                  /* user pattern index 0..NUM_USER_PAT_SLOTS-1 */
  uint8_t  flags;
  int8_t   transpose;
  uint64_t synthRows[8];
  uint64_t drumRows[8];
  uint16_t synthPreset[16];
  uint16_t drumPreset[32];
};

struct UserPatBlob {
  uint16_t version;
  uint16_t count;
  uint16_t _pad;
  uint32_t crc32;                 /* zeroed while hashing */
  UserPatOverride entries[MAX_USER_PAT_OVR];
};

struct UserPatNamesBlob {
  uint16_t version;
  uint16_t count;
  uint16_t _pad;
  uint32_t crc32;                 /* zeroed while hashing */
  NameOverride names[MAX_USER_PAT_OVR];
};

/* SPARSE P-lock motion matrix.  The App is the primary motion recorder;
 * persisting it lets the groovebox replay recorded automation in standalone
 * mode after a power cycle.  A full hwMotionData dump is ~8.7 KB which is too
 * large alongside the other blobs in the stock 20 KB NVS partition, so only
 * ALLOCATED lanes (targetCmd != 255) are stored, up to MAX_MOTION_LANES.  Most
 * lanes are empty in practice, so real-world size is a few KB.                 */
struct MotionLaneOverride {
  uint8_t  bank;      /* 0–15 */
  uint8_t  chain;     /* 0–3  */
  uint8_t  lane;      /* 0–3  */
  uint8_t  targetCmd; /* automation target command (!=255) */
  uint16_t steps[16]; /* per-step values (0xFFFF = no automation on that step) */
};

struct MotionBlob {
  uint16_t version;
  uint16_t count;                 /* valid entries in lanes[] */
  uint32_t crc32;                 /* zeroed while hashing */
  MotionLaneOverride lanes[MAX_MOTION_LANES];
};

/* ── patterns ────────────────────────────────────────────────────────────── */
static inline esp_err_t patterns_save_h(nvs_handle_t h) {
  PatternsBlob* b = (PatternsBlob*)heap_caps_malloc(sizeof(PatternsBlob), MALLOC_CAP_INTERNAL);
  if (!b) return ESP_ERR_NO_MEM;
  b->version = PATTERNS_VERSION;
  b->_pad    = 0;
  b->crc32   = 0;

  /* Row-by-row snapshot under short per-row patchMux windows instead of a single
   * 8+ KB critical section.  Each window covers exactly one row:
   * 16 × uint64_t steps (128 bytes) + 1 × int8_t transpose = ~129 bytes → the
   * lock is held for ~1–2 µs, which is below the interrupt watchdog threshold
   * even on the slowest PSRAM configuration.
   *
   * MidiUsbRx (the only other writer of hwSeqData) pauses while g_saveArmed is
   * true, so the only concurrent writer during the save window is ControlPoll
   * (Core 0, via seqUI_toggleStep).  ControlPoll also uses patchMux for the same
   * rows: that is normal lock contention, not a coherence issue. */
  for (int bk = 0; bk < 16; ++bk) {
    for (int ch = 0; ch < 4; ++ch) {
      portENTER_CRITICAL(&patchMux);
      memcpy(b->data[bk][ch], hwSeqData[bk][ch], 16u * sizeof(uint64_t));
      b->transpose[bk][ch] = seqPatternTranspose[bk][ch];
      portEXIT_CRITICAL(&patchMux);
    }
  }

  b->crc32 = crc32_buf((const uint8_t*)b, sizeof(PatternsBlob));
  esp_err_t err = nvs_set_blob(h, "patterns", b, sizeof(PatternsBlob));
  heap_caps_free(b);
  return err;
}

static inline esp_err_t patterns_load_h(nvs_handle_t h) {
  size_t sz = 0;
  esp_err_t err = nvs_get_blob(h, "patterns", nullptr, &sz);
  if (err != ESP_OK || sz == 0) return err;

  void* buf = heap_caps_malloc(sz, MALLOC_CAP_INTERNAL);
  if (!buf) return ESP_ERR_NO_MEM;
  err = nvs_get_blob(h, "patterns", buf, &sz);
  if (err != ESP_OK) { heap_caps_free(buf); return err; }

  if (sz == sizeof(PatternsBlob)) {
    PatternsBlob* b = (PatternsBlob*)buf;
    if (b->version == PATTERNS_VERSION) {
      const uint32_t stored = b->crc32;
      b->crc32 = 0;
      if (crc32_buf((const uint8_t*)b, sizeof(PatternsBlob)) == stored) {
        portENTER_CRITICAL(&patchMux);
        memcpy(hwSeqData, b->data, sizeof(hwSeqData));
        memcpy(seqPatternTranspose, b->transpose, sizeof(seqPatternTranspose));
        portEXIT_CRITICAL(&patchMux);
      } else {
        err = ESP_ERR_INVALID_CRC;
      }
    }
  } else if (sz == sizeof(PatternsBlobV2)) {
    /* v2 → v3: keep grid; seed per-pattern transpose from global default. */
    PatternsBlobV2* b = (PatternsBlobV2*)buf;
    if (b->version == PATTERNS_VERSION_V2) {
      const uint32_t stored = b->crc32;
      b->crc32 = 0;
      if (crc32_buf((const uint8_t*)b, sizeof(PatternsBlobV2)) == stored) {
        const int8_t glob = (int8_t)seqTranspose.load(std::memory_order_relaxed);
        portENTER_CRITICAL(&patchMux);
        memcpy(hwSeqData, b->data, sizeof(hwSeqData));
        for (auto& bk : seqPatternTranspose)
          for (auto& cell : bk) cell = glob;
        portEXIT_CRITICAL(&patchMux);
      } else {
        err = ESP_ERR_INVALID_CRC;
      }
    }
  } else if (sz == sizeof(PatternsBlobV1)) {
    /* v1 → v2: expand 16-bit rows to 64-bit (upper pages zero). */
    PatternsBlobV1* v1 = (PatternsBlobV1*)buf;
    if (v1->version == PATTERNS_VERSION_V1) {
      const uint32_t stored = v1->crc32;
      v1->crc32 = 0;
      if (crc32_buf((const uint8_t*)v1, sizeof(PatternsBlobV1)) == stored) {
        const int8_t glob = (int8_t)seqTranspose.load(std::memory_order_relaxed);
        portENTER_CRITICAL(&patchMux);
        for (int bk = 0; bk < 16; ++bk)
          for (int ch = 0; ch < 4; ++ch)
            for (int r = 0; r < 16; ++r)
              hwSeqData[bk][ch][r] = (uint64_t)v1->data[bk][ch][r];
        for (auto& bk : seqPatternTranspose)
          for (auto& cell : bk) cell = glob;
        portEXIT_CRITICAL(&patchMux);
      } else {
        err = ESP_ERR_INVALID_CRC;
      }
    }
  }
  heap_caps_free(buf);
  return err;
}

/* ── sparse banks ─────────────────────────────────────────────────────────── */
static inline esp_err_t banks_save_h(nvs_handle_t h) {
  BanksBlob* b = (BanksBlob*)heap_caps_malloc(sizeof(BanksBlob), MALLOC_CAP_INTERNAL);
  if (!b) return ESP_ERR_NO_MEM;
  memset(b, 0, sizeof(BanksBlob));
  b->version = BANKS_VERSION;

  /* Per-row copy into the blob: short critical sections, safe during save window
   * (audio muted, Core 1 parked).  Avoids large stack/allocation spikes. */
  uint16_t hc = 0, sc = 0;
  uint16_t fac[PARAMS_PER_PRESET];
  uint16_t htmp[PARAMS_PER_PRESET], stmp[PARAMS_PER_PRESET];
  const size_t rowBytes = PARAMS_PER_PRESET * sizeof(uint16_t);

  for (int i = 0; i < NUM_PATCHES; ++i) {
    memcpy_P(fac, &SOUND_BANK[i][0], rowBytes); /* PROGMEM read — outside lock */
    bool hd, sd;
    portENTER_CRITICAL(&patchMux);
    hd = (memcmp(userBank[i], fac, rowBytes) != 0);
    sd = (memcmp(seqBank[i],  fac, rowBytes) != 0);
    if (hd) memcpy(htmp, userBank[i], rowBytes);
    if (sd) memcpy(stmp, seqBank[i],  rowBytes);
    portEXIT_CRITICAL(&patchMux);
    if (hd && hc < MAX_BANK_OVR) { b->harp[hc].slot = (uint16_t)i; memcpy(b->harp[hc].params, htmp, rowBytes); ++hc; }
    if (sd && sc < MAX_BANK_OVR) { b->seq[sc].slot  = (uint16_t)i; memcpy(b->seq[sc].params,  stmp, rowBytes); ++sc; }
  }
  b->harpCount = hc;
  b->seqCount  = sc;
  b->crc32     = crc32_buf((const uint8_t*)b, sizeof(BanksBlob)); /* crc field is 0 here */
  esp_err_t err = nvs_set_blob(h, "banks", b, sizeof(BanksBlob));
  heap_caps_free(b);
  return err;
}

static inline esp_err_t banks_load_h(nvs_handle_t h) {
  size_t sz = sizeof(BanksBlob);
  BanksBlob* b = (BanksBlob*)heap_caps_malloc(sz, MALLOC_CAP_INTERNAL);
  if (!b) return ESP_ERR_NO_MEM;
  esp_err_t err = nvs_get_blob(h, "banks", b, &sz);
  if (err == ESP_OK && sz == sizeof(BanksBlob) && b->version == BANKS_VERSION) {
    const uint32_t stored = b->crc32;
    b->crc32 = 0;
    if (crc32_buf((const uint8_t*)b, sizeof(BanksBlob)) == stored) {
      const size_t   rowBytes = PARAMS_PER_PRESET * sizeof(uint16_t);
      const uint16_t hc  = std::min<uint16_t>(b->harpCount, MAX_BANK_OVR);
      const uint16_t scn = std::min<uint16_t>(b->seqCount,  MAX_BANK_OVR);
      for (uint16_t k = 0; k < hc; ++k) {
        const uint16_t s = b->harp[k].slot;
        if (s < NUM_PATCHES) {
          portENTER_CRITICAL(&patchMux);
          memcpy(userBank[s], b->harp[k].params, rowBytes);
          portEXIT_CRITICAL(&patchMux);
        }
      }
      for (uint16_t k = 0; k < scn; ++k) {
        const uint16_t s = b->seq[k].slot;
        if (s < NUM_PATCHES) {
          portENTER_CRITICAL(&patchMux);
          memcpy(seqBank[s], b->seq[k].params, rowBytes);
          portEXIT_CRITICAL(&patchMux);
        }
      }
    } else {
      err = ESP_ERR_INVALID_CRC;
    }
  }
  heap_caps_free(b);
  return err;
}

/* ── user-slot names (sparse: renamed slots only) ─────────────────────────── */
static inline esp_err_t usrnames_save_h(nvs_handle_t h) {
  UserNamesBlob* b = (UserNamesBlob*)heap_caps_malloc(sizeof(UserNamesBlob), MALLOC_CAP_INTERNAL);
  if (!b) return ESP_ERR_NO_MEM;
  memset(b, 0, sizeof(UserNamesBlob));
  b->version = USRNAMES_VERSION;
  uint16_t hc = 0, sc = 0;
  for (int i = 0; i < NUM_USER_SLOTS; ++i) {
    if (g_userSlotName[0][i][0]) { b->harp[hc].slot = (uint16_t)i;
      memcpy(b->harp[hc].name, g_userSlotName[0][i], 16); ++hc; }
    if (g_userSlotName[1][i][0]) { b->seq[sc].slot = (uint16_t)i;
      memcpy(b->seq[sc].name, g_userSlotName[1][i], 16); ++sc; }
  }
  b->harpCount = hc;
  b->seqCount  = sc;
  b->crc32     = crc32_buf((const uint8_t*)b, sizeof(UserNamesBlob));
  esp_err_t err = nvs_set_blob(h, "usrnames", b, sizeof(UserNamesBlob));
  heap_caps_free(b);
  return err;
}

static inline esp_err_t usrnames_load_h(nvs_handle_t h) {
  size_t sz = sizeof(UserNamesBlob);
  UserNamesBlob* b = (UserNamesBlob*)heap_caps_malloc(sz, MALLOC_CAP_INTERNAL);
  if (!b) return ESP_ERR_NO_MEM;
  esp_err_t err = nvs_get_blob(h, "usrnames", b, &sz);
  if (err == ESP_OK && sz == sizeof(UserNamesBlob) && b->version == USRNAMES_VERSION) {
    const uint32_t stored = b->crc32;
    b->crc32 = 0;
    if (crc32_buf((const uint8_t*)b, sizeof(UserNamesBlob)) == stored) {
      const uint16_t hc  = std::min<uint16_t>(b->harpCount, NUM_USER_SLOTS);
      const uint16_t scn = std::min<uint16_t>(b->seqCount,  NUM_USER_SLOTS);
      for (uint16_t k = 0; k < hc; ++k) {
        const uint16_t s = b->harp[k].slot;
        if (s < NUM_USER_SLOTS) { memcpy(g_userSlotName[0][s], b->harp[k].name, 16);
                                  g_userSlotName[0][s][15] = '\0'; }
      }
      for (uint16_t k = 0; k < scn; ++k) {
        const uint16_t s = b->seq[k].slot;
        if (s < NUM_USER_SLOTS) { memcpy(g_userSlotName[1][s], b->seq[k].name, 16);
                                  g_userSlotName[1][s][15] = '\0'; }
      }
    } else {
      err = ESP_ERR_INVALID_CRC;
    }
  }
  heap_caps_free(b);
  return err;
}

/* ── user pattern slots (sparse: saved slots only) ───────────────────────── */
static inline void reset_clear_user_pats() {
  memset(g_userPat, 0, sizeof(g_userPat));
  memset(g_userPatName, 0, sizeof(g_userPatName));
}

static inline esp_err_t usrpat_save_h(nvs_handle_t h) {
  UserPatBlob* b = (UserPatBlob*)heap_caps_malloc(sizeof(UserPatBlob), MALLOC_CAP_INTERNAL);
  if (!b) return ESP_ERR_NO_MEM;
  memset(b, 0, sizeof(UserPatBlob));
  b->version = USRPAT_VERSION;
  uint16_t n = 0;
  for (int i = 0; i < NUM_USER_PAT_SLOTS && n < MAX_USER_PAT_OVR; ++i) {
    if (!(g_userPat[i].flags & 1u)) continue;
    UserPatOverride* e = &b->entries[n];
    e->slot = (uint16_t)i;
    e->flags = g_userPat[i].flags;
    e->transpose = g_userPat[i].transpose;
    memcpy(e->synthRows, g_userPat[i].synthRows, sizeof(e->synthRows));
    memcpy(e->drumRows,  g_userPat[i].drumRows,  sizeof(e->drumRows));
    memcpy(e->synthPreset, g_userPat[i].synthPreset, sizeof(e->synthPreset));
    memcpy(e->drumPreset,  g_userPat[i].drumPreset,  sizeof(e->drumPreset));
    ++n;
  }
  b->count = n;
  b->crc32 = crc32_buf((const uint8_t*)b, sizeof(UserPatBlob));
  esp_err_t err = nvs_set_blob(h, "usrpat", b, sizeof(UserPatBlob));
  heap_caps_free(b);
  return err;
}

static inline esp_err_t usrpat_load_h(nvs_handle_t h) {
  size_t sz = sizeof(UserPatBlob);
  UserPatBlob* b = (UserPatBlob*)heap_caps_malloc(sz, MALLOC_CAP_INTERNAL);
  if (!b) return ESP_ERR_NO_MEM;
  esp_err_t err = nvs_get_blob(h, "usrpat", b, &sz);
  if (err == ESP_OK && sz == sizeof(UserPatBlob) && b->version == USRPAT_VERSION) {
    const uint32_t stored = b->crc32;
    b->crc32 = 0;
    if (crc32_buf((const uint8_t*)b, sizeof(UserPatBlob)) == stored) {
      memset(g_userPat, 0, sizeof(g_userPat));
      const uint16_t n = std::min<uint16_t>(b->count, MAX_USER_PAT_OVR);
      for (uint16_t k = 0; k < n; ++k) {
        const uint16_t s = b->entries[k].slot;
        if (s >= NUM_USER_PAT_SLOTS) continue;
        UserPatternSlot* dst = &g_userPat[s];
        dst->flags = b->entries[k].flags;
        dst->transpose = b->entries[k].transpose;
        memcpy(dst->synthRows, b->entries[k].synthRows, sizeof(dst->synthRows));
        memcpy(dst->drumRows,  b->entries[k].drumRows,  sizeof(dst->drumRows));
        memcpy(dst->synthPreset, b->entries[k].synthPreset, sizeof(dst->synthPreset));
        memcpy(dst->drumPreset,  b->entries[k].drumPreset,  sizeof(dst->drumPreset));
      }
    } else {
      err = ESP_ERR_INVALID_CRC;
    }
  }
  heap_caps_free(b);
  return err;
}

static inline esp_err_t usrpatnames_save_h(nvs_handle_t h) {
  UserPatNamesBlob* b = (UserPatNamesBlob*)heap_caps_malloc(sizeof(UserPatNamesBlob), MALLOC_CAP_INTERNAL);
  if (!b) return ESP_ERR_NO_MEM;
  memset(b, 0, sizeof(UserPatNamesBlob));
  b->version = USRPATNAMES_VERSION;
  uint16_t n = 0;
  for (int i = 0; i < NUM_USER_PAT_SLOTS && n < MAX_USER_PAT_OVR; ++i) {
    if (!g_userPatName[i][0]) continue;
    b->names[n].slot = (uint16_t)i;
    memcpy(b->names[n].name, g_userPatName[i], 16);
    ++n;
  }
  b->count = n;
  b->crc32 = crc32_buf((const uint8_t*)b, sizeof(UserPatNamesBlob));
  esp_err_t err = nvs_set_blob(h, "usrpatnames", b, sizeof(UserPatNamesBlob));
  heap_caps_free(b);
  return err;
}

static inline esp_err_t usrpatnames_load_h(nvs_handle_t h) {
  size_t sz = sizeof(UserPatNamesBlob);
  UserPatNamesBlob* b = (UserPatNamesBlob*)heap_caps_malloc(sz, MALLOC_CAP_INTERNAL);
  if (!b) return ESP_ERR_NO_MEM;
  esp_err_t err = nvs_get_blob(h, "usrpatnames", b, &sz);
  if (err == ESP_OK && sz == sizeof(UserPatNamesBlob) && b->version == USRPATNAMES_VERSION) {
    const uint32_t stored = b->crc32;
    b->crc32 = 0;
    if (crc32_buf((const uint8_t*)b, sizeof(UserPatNamesBlob)) == stored) {
      const uint16_t n = std::min<uint16_t>(b->count, MAX_USER_PAT_OVR);
      for (uint16_t k = 0; k < n; ++k) {
        const uint16_t s = b->names[k].slot;
        if (s < NUM_USER_PAT_SLOTS) {
          memcpy(g_userPatName[s], b->names[k].name, 16);
          g_userPatName[s][15] = '\0';
        }
      }
    } else {
      err = ESP_ERR_INVALID_CRC;
    }
  }
  heap_caps_free(b);
  return err;
}

/* ── motion (sparse: allocated lanes only) ────────────────────────────────── */
static inline esp_err_t motion_save_h(nvs_handle_t h) {
  MotionBlob* b = (MotionBlob*)heap_caps_malloc(sizeof(MotionBlob), MALLOC_CAP_INTERNAL);
  if (!b) return ESP_ERR_NO_MEM;
  memset(b, 0, sizeof(MotionBlob));
  b->version = MOTION_VERSION;

  uint16_t n = 0;
  MotionLane tmp;
  for (int bank = 0; bank < 16 && n < MAX_MOTION_LANES; ++bank) {
    for (int chain = 0; chain < 4 && n < MAX_MOTION_LANES; ++chain) {
      for (int lane = 0; lane < 4 && n < MAX_MOTION_LANES; ++lane) {
        bool used;
        portENTER_CRITICAL(&motionMux);
        used = (hwMotionData[bank][chain][lane].targetCmd != 255u);
        if (used) tmp = hwMotionData[bank][chain][lane]; /* 34-byte copy under lock */
        portEXIT_CRITICAL(&motionMux);
        if (used) {
          b->lanes[n].bank      = (uint8_t)bank;
          b->lanes[n].chain     = (uint8_t)chain;
          b->lanes[n].lane      = (uint8_t)lane;
          b->lanes[n].targetCmd = tmp.targetCmd;
          memcpy(b->lanes[n].steps, tmp.steps, sizeof(tmp.steps));
          ++n;
        }
      }
    }
  }
  b->count = n;
  b->crc32 = crc32_buf((const uint8_t*)b, sizeof(MotionBlob)); /* crc field is 0 */
  esp_err_t err = nvs_set_blob(h, "motion", b, sizeof(MotionBlob));
  heap_caps_free(b);
  return err;
}

static inline esp_err_t motion_load_h(nvs_handle_t h) {
  size_t sz = sizeof(MotionBlob);
  MotionBlob* b = (MotionBlob*)heap_caps_malloc(sz, MALLOC_CAP_INTERNAL);
  if (!b) return ESP_ERR_NO_MEM;
  esp_err_t err = nvs_get_blob(h, "motion", b, &sz);
  if (err == ESP_OK && sz == sizeof(MotionBlob) && b->version == MOTION_VERSION) {
    const uint32_t stored = b->crc32;
    b->crc32 = 0;
    if (crc32_buf((const uint8_t*)b, sizeof(MotionBlob)) == stored) {
      /* Base is already cleared to sentinels by persisted_extras_load(); just
       * overlay the saved lanes.                                               */
      const uint16_t cnt = std::min<uint16_t>(b->count, MAX_MOTION_LANES);
      for (uint16_t k = 0; k < cnt; ++k) {
        const uint8_t bank  = b->lanes[k].bank;
        const uint8_t chain = b->lanes[k].chain;
        const uint8_t lane  = b->lanes[k].lane;
        if (bank < 16 && chain < 4 && lane < 4) {
          portENTER_CRITICAL(&motionMux);
          hwMotionData[bank][chain][lane].targetCmd = b->lanes[k].targetCmd;
          memcpy(hwMotionData[bank][chain][lane].steps, b->lanes[k].steps,
                 sizeof(hwMotionData[bank][chain][lane].steps));
          portEXIT_CRITICAL(&motionMux);
        }
      }
    } else {
      err = ESP_ERR_INVALID_CRC;
    }
  }
  heap_caps_free(b);
  return err;
}

/* persisted_extras_load — restore patterns + sparse bank overrides + motion.
 * PRECONDITION: userBank/seqBank already seeded from factory (seedFactoryBanks)
 * so the sparse overrides land on a clean factory base.  Missing/corrupt blobs
 * are non-fatal: patterns/motion stay as-is, banks stay factory.              */
static inline void persisted_extras_load() {
  /* Set the motion matrix to its empty sentinel base FIRST.  hwMotionData is
   * BSS-zeroed at boot (targetCmd=0 is a VALID command, not "empty"), so a
   * fresh or corrupt "motion" blob must still leave valid sentinels.  A
   * successful motion_load_h() then overlays the saved lanes on top.  This also
   * makes the function the single authoritative motion initialiser — the .ino
   * no longer needs a separate initMotionMatrix() pass that would clobber it.  */
  clearMotionMatrix();
  nvs_handle_t h;
  if (nvs_open_octopus( NVS_READONLY, &h) != ESP_OK) return;
  banks_load_h(h);
  usrnames_load_h(h);
  usrpat_load_h(h);
  usrpatnames_load_h(h);
  patterns_load_h(h);
  motion_load_h(h);
  nvs_close(h);

  /* Adopt the active pattern's stored transpose into the live seqTranspose atomic
   * (overrides the legacy global from SequencerSettings, which is now just a default).
   * No echo — runs at boot before any App link. */
  {
    const uint8_t bank  = seqActiveBank.load(std::memory_order_relaxed)  & 15u;
    const uint8_t chain = seqActiveChain.load(std::memory_order_relaxed) & 3u;
    seqTranspose.store((int32_t)seqPatternTranspose[bank][chain],
                       std::memory_order_relaxed);
  }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * SCOPED RESET / SAVE
 *
 * ResetScope is defined here (before settings_save_scoped) so translation units
 * that include interface.h's forward decl still get the full enum before use.
 *
 *   FULL            settings + sound banks + patterns + motion → factory/empty
 *   BANKS_PATTERNS  sound banks → factory, patterns cleared (settings/motion kept)
 *   MOTION          P-lock motion cleared (everything else kept)
 *   SETTINGS        settings → factory (banks/patterns/motion kept)
 *
 * Subsystem → blob/scope map (SAVE/LOAD/RESET 1:1 consistency):
 *   • SEQ step grids + per-pattern transpose  → "patterns" blob  → BANKS_PATTERNS/FULL
 *   • SEQ MATRIX P-locks (motion automation)   → "motion"   blob  → MOTION/FULL
 *   • Sound banks (userBank/seqBank deltas)    → "banks"    blob  → BANKS_PATTERNS/FULL
 *   • User pattern library (64 melody+drum)    → "usrpat"   blob  → BANKS_PATTERNS/FULL
 *   • User pattern slot names (sparse)         → "usrpatnames"     → BANKS_PATTERNS/FULL
 *   • SONG arrangements (hwSongData) + active  → "settings" blob  → SETTINGS/FULL
 *   • All global params + RESUME working image → "settings" blob  → SETTINGS/FULL
 * Scopes are ORTHOGONAL by design: clearing patterns (BANKS_PATTERNS) deliberately
 * leaves the motion matrix alone (use MOTION for that), and SONG travels with the
 * settings blob (it is an arrangement of settings/state, not raw step data).
 * LOAD mirrors each scope exactly via settings_load_scoped().
 *
 * applyResetScope() wipes RAM to factory/empty per scope.
 *
 * Runtime reset paths:
 *   FULL / BANKS_PATTERNS — settings_arm_pending_reset() + esp_restart(); the wipe
 *     and NVS commit run in settings_execute_pending_reset_at_boot() before tasks
 *     start (mirrors the reliable OC+SCALE boot-time factory reset).
 *   SETTINGS / MOTION     — applyResetScope() + settings_commit_reset_scoped() via
 *     NvsWorker (small NVS writes, safe while running).
 * ═══════════════════════════════════════════════════════════════════════════ */
enum class ResetScope : uint8_t {
  FULL = 0,
  BANKS_PATTERNS,
  MOTION,
  SETTINGS
};

static inline void reset_clear_patterns() {
  portENTER_CRITICAL(&patchMux);
  memset(hwSeqData, 0, sizeof(hwSeqData));
  memset(seqPatternTranspose, 0, sizeof(seqPatternTranspose));
  portEXIT_CRITICAL(&patchMux);
}

static inline void reset_settings_to_factory() {
  g_settings = AllSettings{};
  initDrumParameters();
  initHueEnvelopes();
  initStringDuty();
  settings_sync_to_ssot();
}

/** Apply a scoped reset to RAM — persistence path depends on scope (see code_info.h §2). */
inline void applyResetScope(ResetScope scope) {
  switch (scope) {
    case ResetScope::FULL:
      seedFactoryBanks();
      memset(g_userSlotName, 0, sizeof(g_userSlotName)); /* names follow banks */
      reset_clear_user_pats();
      reset_clear_patterns();
      clearMotionMatrix();
      reset_settings_to_factory();
      break;
    case ResetScope::BANKS_PATTERNS:
      seedFactoryBanks();
      memset(g_userSlotName, 0, sizeof(g_userSlotName)); /* names follow banks */
      reset_clear_user_pats();
      reset_clear_patterns();
      break;
    case ResetScope::MOTION:
      clearMotionMatrix();
      break;
    case ResetScope::SETTINGS:
      reset_settings_to_factory();
      break;
  }
  /* Keep livePatch[] aligned with userBank/seqBank after any bank wipe. */
  syncLivePatchFromAtomics();
}

inline void setupToFactoryDefaults() {
  applyResetScope(ResetScope::FULL);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * CORE NVS OPERATIONS
 * ═══════════════════════════════════════════════════════════════════════════ */

static inline bool settings_save_scoped(ResetScope scope); /* defined below */

struct SettingsPersistCtx {
  ResetScope        scope;
  SemaphoreHandle_t done;
  bool              ok;
};

static void settings_persist_task_fn(void* arg) {
  auto* ctx = static_cast<SettingsPersistCtx*>(arg);
  ctx->ok = settings_save_scoped(ctx->scope);
  xSemaphoreGive(ctx->done);
  vTaskDelete(nullptr);
}

/** Run settings_save_scoped on a dedicated 16 KB stack (safe from setup/MIDI tasks). */
static inline bool settings_persist_blocking(ResetScope scope, uint32_t timeoutMs = 20000u) {
  SettingsPersistCtx ctx{ scope, xSemaphoreCreateBinary(), false };
  if (!ctx.done) {
    return false;
  }
  if (xTaskCreatePinnedToCore(settings_persist_task_fn, "NvsBlk", 16384, &ctx, 5,
                              nullptr, 1) != pdPASS) {
    vSemaphoreDelete(ctx.done);
    return false;
  }
  const bool gotQuick = (xSemaphoreTake(ctx.done, pdMS_TO_TICKS(timeoutMs)) == pdTRUE);
  if (!gotQuick) {
    /* Child may still be writing large blobs (patterns/banks).  Never delete the
     * semaphore or return false while NvsBlk is alive — that produced FAIL UI with
     * a successful flash commit and no esp_restart() on FULL / BANKS+PATS reset. */
    xSemaphoreTake(ctx.done, portMAX_DELAY);
  }
  const bool ok = ctx.ok;
  vSemaphoreDelete(ctx.done);
  return ok;
}

static inline bool settings_save_scoped(ResetScope scope) {

  nvs_handle_t h;
  const esp_err_t openErr = nvs_open_octopus( NVS_READWRITE, &h);
  if (openErr != ESP_OK) {
    return false;
  }

  esp_err_t err = ESP_OK;
  switch (scope) {
    case ResetScope::FULL:
      settings_sync_from_ssot();
      g_settings.crc32 = g_settings.calculate_crc();
      err = nvs_set_blob(h, "settings", &g_settings, sizeof(AllSettings));
      if (err != ESP_OK) break;
      err = patterns_save_h(h);
      if (err != ESP_OK) break;
      err = banks_save_h(h);
      if (err != ESP_OK) break;
      err = usrnames_save_h(h);
      if (err != ESP_OK) break;
      err = usrpat_save_h(h);
      if (err != ESP_OK) break;
      err = usrpatnames_save_h(h);
      if (err != ESP_OK) break;
      err = motion_save_h(h);
      break;
    case ResetScope::SETTINGS:
      settings_sync_from_ssot();
      g_settings.crc32 = g_settings.calculate_crc();
      err = nvs_set_blob(h, "settings", &g_settings, sizeof(AllSettings));
      break;
    case ResetScope::BANKS_PATTERNS:
      err = patterns_save_h(h);
      if (err != ESP_OK) break;
      err = banks_save_h(h);
      if (err != ESP_OK) break;
      err = usrnames_save_h(h);
      if (err != ESP_OK) break;
      err = usrpat_save_h(h);
      if (err != ESP_OK) break;
      err = usrpatnames_save_h(h);
      break;
    case ResetScope::MOTION:
      err = motion_save_h(h);
      break;
  }

  if (err == ESP_OK) err = nvs_commit(h);

  nvs_close(h);

  const bool ok = (err == ESP_OK);
  if (ok) settings_dirty.store(false, std::memory_order_release);
  return ok;
}

/** Commit a scoped reset to NVS after applyResetScope() has wiped RAM.
 * Uses erase for large blobs (patterns/banks/usrpat) — faster and more reliable
 * than re-writing 8 KB+ blobs while audio/MIDI are still running.  SETTINGS and
 * MOTION write/erase a single small key (same as before).                         */
static inline esp_err_t nvs_erase_key_ok(nvs_handle_t h, const char* key) {
  const esp_err_t e = nvs_erase_key(h, key);
  return (e == ESP_OK || e == ESP_ERR_NVS_NOT_FOUND) ? ESP_OK : e;
}

static inline bool settings_commit_reset_scoped(ResetScope scope) {
  nvs_handle_t h;
  if (nvs_open_octopus(NVS_READWRITE, &h) != ESP_OK) return false;

  esp_err_t err = ESP_OK;
  switch (scope) {
    case ResetScope::FULL:
      settings_sync_from_ssot();
      g_settings.crc32 = g_settings.calculate_crc();
      err = nvs_set_blob(h, "settings", &g_settings, sizeof(AllSettings));
      if (err != ESP_OK) break;
      err = nvs_erase_key_ok(h, "patterns");
      if (err != ESP_OK) break;
      err = nvs_erase_key_ok(h, "banks");
      if (err != ESP_OK) break;
      err = nvs_erase_key_ok(h, "usrnames");
      if (err != ESP_OK) break;
      err = nvs_erase_key_ok(h, "usrpat");
      if (err != ESP_OK) break;
      err = nvs_erase_key_ok(h, "usrpatnames");
      if (err != ESP_OK) break;
      err = nvs_erase_key_ok(h, "motion");
      break;
    case ResetScope::SETTINGS:
      settings_sync_from_ssot();
      g_settings.crc32 = g_settings.calculate_crc();
      err = nvs_set_blob(h, "settings", &g_settings, sizeof(AllSettings));
      break;
    case ResetScope::BANKS_PATTERNS:
      err = nvs_erase_key_ok(h, "patterns");
      if (err != ESP_OK) break;
      err = nvs_erase_key_ok(h, "banks");
      if (err != ESP_OK) break;
      err = nvs_erase_key_ok(h, "usrnames");
      if (err != ESP_OK) break;
      err = nvs_erase_key_ok(h, "usrpat");
      if (err != ESP_OK) break;
      err = nvs_erase_key_ok(h, "usrpatnames");
      break;
    case ResetScope::MOTION:
      err = nvs_erase_key_ok(h, "motion");
      break;
  }

  if (err == ESP_OK) err = nvs_commit(h);
  nvs_close(h);

  const bool ok = (err == ESP_OK);
  if (ok) settings_dirty.store(false, std::memory_order_release);
  return ok;
}

/* ── deferred reset (FULL / BANKS+PATS) ─────────────────────────────────────
 * NVS key "pend_rst" (u8): 0=none, 1=FULL, 2=BANKS_PATTERNS.
 * Runtime arms the flag + reboots; boot kernel executes before loadSettings(). */

static constexpr const char* NVS_KEY_PENDING_RESET = "pend_rst";

static inline bool settings_arm_pending_reset(ResetScope scope) {
  if (scope != ResetScope::FULL && scope != ResetScope::BANKS_PATTERNS) return false;
  nvs_handle_t h;
  if (nvs_open_octopus(NVS_READWRITE, &h) != ESP_OK) return false;
  const uint8_t v = (uint8_t)scope + 1u;
  esp_err_t err = nvs_set_u8(h, NVS_KEY_PENDING_RESET, v);
  if (err == ESP_OK) err = nvs_commit(h);
  nvs_close(h);
  return err == ESP_OK;
}

/** If a deferred reset was armed, wipe RAM + commit NVS then clear the flag.
 * Call once at boot BEFORE loadSettings() — no RT tasks, minimal stack.          */
static inline void settings_execute_pending_reset_at_boot() {
  nvs_handle_t h;
  if (nvs_open_octopus(NVS_READWRITE, &h) != ESP_OK) return;

  uint8_t v = 0;
  if (nvs_get_u8(h, NVS_KEY_PENDING_RESET, &v) != ESP_OK || v == 0u) {
    nvs_close(h);
    return;
  }

  nvs_erase_key(h, NVS_KEY_PENDING_RESET);
  nvs_commit(h);
  nvs_close(h);

  if (v < 1u || v > 2u) return; /* only FULL(1) or BANKS_PATTERNS(2) */

  const ResetScope scope = (ResetScope)(v - 1u);
  ESP_LOGI("settings", "deferred reset executing scope %u", (unsigned)v);
  applyResetScope(scope);
  settings_commit_reset_scoped(scope);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Save / load the live patch to a user sound slot (128..191).
 *
 * saveLiveToUserSlot — snapshot the engine's LIVE patch (the sound you hear)
 *   into its user slot, then persist the banks + names blob (RAM-live at once,
 *   NO reboot — routed through the NvsWorker so it is safe from any stack).
 * loadUserSlotToLive — recall a user slot exactly like a factory preset (reuses
 *   recallHarpPatch / recallSeqPatch → atomics + App echo).  Non-destructive.
 * engine: 0 = harp, 1 = seq.  uidx: 0..NUM_USER_SLOTS-1.                       */
static inline bool saveLiveToUserSlot(uint8_t engine, uint8_t uidx) {
  engine &= 1u; uidx %= (uint8_t)NUM_USER_SLOTS;
  const int    slot     = USER_SLOT_BASE + uidx;
  const size_t rowBytes = PARAMS_PER_PRESET * sizeof(uint16_t);
  portENTER_CRITICAL(&patchMux);
  if (engine == 0) memcpy(userBank[slot], harpLivePatch, rowBytes);
  else             memcpy(seqBank[slot],  seqLivePatch,  rowBytes);
  portEXIT_CRITICAL(&patchMux);
  /* Reflect the loaded-index so the readout shows the slot we just wrote. */
  if (engine == 0) harpPatchIndex.store(slot, std::memory_order_relaxed);
  else             seqPatchIndex .store(slot, std::memory_order_relaxed);
  displayDirty.store(true, std::memory_order_relaxed);
  return settings_persist_blocking(ResetScope::BANKS_PATTERNS);
}

static inline void loadUserSlotToLive(uint8_t engine, uint8_t uidx) {
  engine &= 1u; uidx %= (uint8_t)NUM_USER_SLOTS;
  const int slot = USER_SLOT_BASE + uidx;
  if (engine == 0) recallHarpPatch(slot, ParamSource::NVS);
  else             recallSeqPatch (slot, ParamSource::NVS);
  if (isAppConnected()) {
    sendFullStateSync();
    txSysex(CMD_USR_SOUND_LOAD, (uint16_t)(((uint16_t)(engine & 1u) << 13) | uidx));
  }
}

static inline bool settings_save_on_worker_stack() {
  const char* name = pcTaskGetName(nullptr);
  return name && (strcmp(name, "NvsWorker") == 0 || strcmp(name, "NvsBlk") == 0);
}

static inline bool settings_save() {
  if (settings_save_on_worker_stack())
    return settings_save_scoped(ResetScope::FULL);
  return settings_persist_blocking(ResetScope::FULL);
}

static inline bool settings_load() {
  nvs_handle_t h;
  esp_err_t err = nvs_open_octopus( NVS_READONLY, &h);
  if (err == ESP_ERR_NVS_NOT_FOUND) {
    /* First boot — seed and persist factory defaults.
     * CRITICAL ORDER: settings_save() pulls atomics→blob (settings_sync_from_ssot),
     * so the factory g_settings MUST be pushed to the atomics FIRST.  Otherwise the
     * save captures whatever the globals.h static-init placeholders happen to be and
     * the tuned AllSettings defaults never reach NVS. */
    g_settings = AllSettings{};
    initDrumParameters();
    initHueEnvelopes();
    initStringDuty();
    settings_sync_to_ssot();   /* factory g_settings → atomics → livePatch */
    /* Route first-boot persist through NvsWorker (16 KB stack), not inline here. */
    if (!settings_persist_blocking(ResetScope::FULL)) { /* first-boot persist failed */ }
    return true;
  }
  if (err != ESP_OK) return false;
  uint8_t blob[sizeof(AllSettings)];
  size_t sz = sizeof(blob);
  err = nvs_get_blob(h, "settings", blob, &sz);
  nvs_close(h);
  if (err != ESP_OK) return false;

  if (sz == sizeof(AllSettings)) {
    memcpy(&g_settings, blob, sz);
    if (g_settings.verify()) return true;
  }

  /* Corrupt or unknown SETTINGS_VERSION — reseed factory defaults. */
  {
    g_settings = AllSettings{};
    initDrumParameters();
    initHueEnvelopes();
    initStringDuty();
    settings_sync_to_ssot();
    if (!settings_persist_blocking(ResetScope::FULL)) { /* corrupt reseed persist failed */ }
    return false;
  }
}

/* init_nvs_flash() is not used — the Arduino-ESP32 core initializes NVS before
 * setup(); boot uses loadSettings() → settings_load() directly. */

/* ═══════════════════════════════════════════════════════════════════════════
 * PUBLIC API
 * ═══════════════════════════════════════════════════════════════════════════ */

/** Load from NVS and push to SSOT.  Returns true if stored data was valid. */
static inline bool loadSettings() {
  seedFactoryBanks();          /* RAM userBank/seqBank ← PROGMEM SOUND_BANK */
  const bool ok = settings_load();
  /* settings_load() already syncs on first-boot / corrupt paths; sync here only
   * after a successful load of a valid stored blob. */
  if (ok) settings_sync_to_ssot();
  persisted_extras_load();     /* patterns + sparse bank overrides on top    */
  return ok;
}

/** Reset all atomics to factory values.  Does NOT write to NVS.            */
static inline void resetToFactoryDefaults() {
  setupToFactoryDefaults();
}

/** Blocking full-session save on the 16 KB NvsBlk worker. Prefer requestScopedSave()
 *  at runtime (NvsWorker handshake); use this only when a synchronous commit is required. */
static inline bool saveSettingsSafe() {
  return settings_persist_blocking(ResetScope::FULL);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * SCOPED LOAD
 *
 * RAM-only reload of the selected scope from NVS — the 1:1 inverse of SAVE.
 * Unlike SAVE/RESET this writes NO flash, so it does NOT glitch the laser
 * beam-detect latch and needs NO reboot — the new RAM state is live at once.
 * Scopes mirror ResetScope exactly:
 *   FULL            full canonical restore (== boot / CMD_SESSION_LOAD)
 *   SETTINGS        re-read the "settings" blob → atomics (banks/patterns/motion kept)
 *   BANKS_PATTERNS  re-seed factory banks, overlay stored bank + pattern blobs
 *   MOTION          clear matrix, overlay stored motion lanes
 * Returns true if the requested blob(s) were present and valid; on failure RAM
 * for that scope is left as-is (best-effort, non-destructive).                 */
static inline bool settings_load_scoped(ResetScope scope) {
  switch (scope) {
    case ResetScope::FULL:
      return loadSettings();                  /* seed banks → settings → ssot → extras */

    case ResetScope::SETTINGS: {
      nvs_handle_t h;
      if (nvs_open_octopus( NVS_READONLY, &h) != ESP_OK) return false;
      uint8_t blob[sizeof(AllSettings)];
      size_t  sz  = sizeof(blob);
      esp_err_t e = nvs_get_blob(h, "settings", blob, &sz);
      nvs_close(h);
      if (e != ESP_OK || sz != sizeof(AllSettings)) return false;
      AllSettings tmp;
      memcpy(&tmp, blob, sz);
      if (!tmp.verify()) return false;
      g_settings = tmp;
      settings_sync_to_ssot();                /* → atomics → syncLivePatchFromAtomics */
      return true;
    }

    case ResetScope::BANKS_PATTERNS: {
      seedFactoryBanks();                      /* clean factory base for sparse overlay */
      memset(g_userSlotName, 0, sizeof(g_userSlotName)); /* re-overlay below */
      reset_clear_user_pats();
      nvs_handle_t h;
      if (nvs_open_octopus( NVS_READONLY, &h) != ESP_OK) return false;
      const esp_err_t be = banks_load_h(h);
      usrnames_load_h(h);                      /* names travel with banks */
      usrpat_load_h(h);
      usrpatnames_load_h(h);
      const esp_err_t pe = patterns_load_h(h);
      nvs_close(h);
      syncLivePatchFromAtomics();              /* keep livePatch[] aligned with banks  */
      /* Adopt the active pattern's stored transpose into the live atomic,
       * exactly as persisted_extras_load() does at boot. */
      {
        const uint8_t bank  = seqActiveBank .load(std::memory_order_relaxed) & 15u;
        const uint8_t chain = seqActiveChain.load(std::memory_order_relaxed) & 3u;
        seqTranspose.store((int32_t)seqPatternTranspose[bank][chain],
                           std::memory_order_relaxed);
      }
      return (be == ESP_OK && pe == ESP_OK);
    }

    case ResetScope::MOTION: {
      clearMotionMatrix();
      nvs_handle_t h;
      if (nvs_open_octopus( NVS_READONLY, &h) != ESP_OK) return false;
      const esp_err_t me = motion_load_h(h);
      nvs_close(h);
      return (me == ESP_OK);
    }
  }
  return false;
}

#endif /* SETTINGS_H */
