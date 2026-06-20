/* ═════════════════════════════════════════════════════════════════════════════
 * Octopus PRO XL v6.0.00 — Laser Harp Groovebox
 * © 2026 DIODAC ELECTRONICS / iSystem. All Rights Reserved.
 *
 * PROPRIETARY AND CONFIDENTIAL. Unauthorized copying, distribution, modification,
 * or use of this software or firmware, in whole or in part, is strictly prohibited
 * without prior written permission from DIODAC ELECTRONICS.
 * ═════════════════════════════════════════════════════════════════════════════
 * assets.h — v6.0.00  CENTRALIZED DSP DECLARATIONS + FACTORY SOUNDBANK INTERFACE
 *
 * Changes vs v5.0.04:
 *   • PTCH_FM macro REMOVED — it was used 0 times and silently discarded its own
 *     FM arguments (the engine has no phase-modulation path; FM presets were
 *     dual-osc approximations anyway).
 *   • PTCH macro REPLACED by SP() — same 16-slot layout, but parameters are
 *     expressed in intuitive units and CUTOFF is the DIRECT 0–16383 filter value
 *     (no hidden /2), so the table is readable and editable without guesswork.
 *   • NUM_FACTORY_PATCHES added — the count of authored presets (128).  The
 *     factory loaders/UI clamp to this so browsing never lands on a blank slot.
 *     (Previously SOUND_BANK was [256] but only ~86 rows were filled, and
 *      PRESET_NAMES held ~38 names → silent patches + blank names.)
 *
 * IMPORTANT — changing the factory SOUND_BANK invalidates the NVS user-bank delta
 * store (settings.h keeps only rows that DIFFER from this PROGMEM baseline).  You
 * MUST bump SETTINGS_VERSION (settings.h) so the old blob is discarded on first
 * boot; otherwise saved user patches decode against the new baseline = garbage.
 * ═════════════════════════════════════════════════════════════════════════════ */
 #pragma once
 #ifndef ASSETS_H
 #define ASSETS_H
 
 #include <Arduino.h>
 #include <pgmspace.h>
 #include "globals.h"
 
 static constexpr int NUM_WAVE_TABLES = 25;
 
 struct WaveTableEntry {
     const int16_t* data;
     uint16_t       len;
 };
 
 /* ── EXTERN DECLARATIONS ONLY: heavy data lives in assets.cpp ──────────────────
  * Wave index == position in WAVE_TABLE_DIR / WAVE_TABLE_RAM (NOT a coincidence
  * of declaration order — the DIR array below is the authoritative mapping):
  *   0 Saw       1 Square    2 Pulse25   3 Tri       4 Organ
  *   5 Vocal     6 Bell      7 Strings   8 Sine      9 Pulse10
  *  10 Pulse40  11 FM Glass 12 EP Tine  13 Clavi    14 Choir
  *  15 Resonant 16 Harmonium 17 Cello   18 GlassHrm 19 Growl
  *  20 Flute    21 PadWarm  22 MoogBass 23 Tabla    24 Didgeridoo
  * ──────────────────────────────────────────────────────────────────────────── */
 extern const int16_t WT_SAW[256] PROGMEM;
 extern const int16_t WT_SQUARE[256] PROGMEM;
 extern const int16_t WT_PULSE25[256] PROGMEM;
 extern const int16_t WT_TRI[256] PROGMEM;
 extern const int16_t WT_ORGAN[256] PROGMEM;
 extern const int16_t WT_VOCAL[256] PROGMEM;
 extern const int16_t WT_BELL[256] PROGMEM;
 extern const int16_t WT_STRINGS[256] PROGMEM;
 extern const int16_t WT_SINE[256] PROGMEM;
 extern const int16_t WT_PULSE10[256] PROGMEM;
 extern const int16_t WT_PULSE40[256] PROGMEM;
 extern const int16_t WT_FM_GLASS[256] PROGMEM;
 extern const int16_t WT_EP_TINE[256] PROGMEM;
 extern const int16_t WT_CLAVI[256] PROGMEM;
 extern const int16_t WT_CHOIR[256] PROGMEM;
 extern const int16_t WT_RESONANT[256] PROGMEM;
 extern const int16_t WT_HARMONIUM[256] PROGMEM;
 extern const int16_t WT_CELLO[256] PROGMEM;
 extern const int16_t WT_GLASS_HARM[256] PROGMEM;
 extern const int16_t WT_GROWL[256] PROGMEM;
 extern const int16_t WT_FLUTE[256] PROGMEM;
 extern const int16_t WT_PAD_WARM[256] PROGMEM;
 extern const int16_t WT_MOOG_BASS[256] PROGMEM;
 extern const int16_t WT_TABLA[256] PROGMEM;
 extern const int16_t WT_DIDGERIDOO[256] PROGMEM;
 
 extern const WaveTableEntry WAVE_TABLE_DIR[NUM_WAVE_TABLES] PROGMEM;
 
 /* ── RAM wavetable mirror [PERF] ─────────────────────────────────────────────
  * The oscillators read tbl[idx]/tbl[idx+1] for every osc, every voice, every
  * sample.  Sourcing those from PROGMEM (flash) routes them through the shared
  * flash data-cache, where 8 voices striding different tables cause cache-miss
  * stalls in the hot path.  All 25 tables are only 12.8 KB, so we mirror them into
  * internal DRAM once at boot and the DSP reads SRAM only: deterministic, no
  * flash-cache competition.  Length is fixed at 256 (phase accumulators mask &255),
  * so no per-table len is needed.                                              */
 static constexpr int WAVE_TABLE_LEN = 256;
 extern int16_t WAVE_TABLE_RAM[NUM_WAVE_TABLES][WAVE_TABLE_LEN];
 
 /* Copy every PROGMEM wavetable into WAVE_TABLE_RAM.  Call once at boot BEFORE
  * the audio task starts (init_audio_system).  Idempotent.                     */
 void wavetables_init_ram();
 
 /* ═══════════════════════════════════════════════════════════════════════════
  * FACTORY SOUNDBANK
  *
  * NUM_FACTORY_PATCHES is the count of REAL authored presets.  SOUND_BANK and
  * PRESET_NAMES keep the full [NUM_PATCHES] dimension so seedFactoryBanks() and
  * the sparse NVS store (settings.h) — which iterate NUM_PATCHES — never read out
  * of bounds.  Rows [NUM_FACTORY_PATCHES .. NUM_PATCHES-1] are zero-filled; the
  * loaders clamp the browse index to NUM_FACTORY_PATCHES so they are unreachable.
  * ═══════════════════════════════════════════════════════════════════════════ */
 static constexpr int NUM_FACTORY_PATCHES = 128;
 static_assert(NUM_FACTORY_PATCHES <= NUM_PATCHES,
               "NUM_FACTORY_PATCHES must fit inside SOUND_BANK[NUM_PATCHES]");
 
 /* ── SP — factory preset packer ──────────────────────────────────────────────
  * Emits one 16-entry row matching livePatch[]/SynthParam EXACTLY (loaders memcpy
  * SOUND_BANK rows straight in, no remap).  Slot layout:
  *   [0]=WAVEFORM [1]=ATTACK [2]=DECAY [3]=SUSTAIN [4]=RELEASE [5]=CUTOFF
  *   [6]=RESONANCE [7]=NOISE [8]=DETUNE [9]=LFO_RATE [10]=LFO_DEPTH
  *   [11]=LFO_ROUTE [12]=OSC2_WAVE [13]=ENV_CUT [14..15]=spare(0)
  *
  * Units (human-readable → 14-bit):
  *   wv  : osc1 wave index 0–24              o2  : osc2 wave index 0–24
  *   atk : attack  ms   (0–2000)             res : resonance 0.0–1.0
  *   dec : decay   ms   (0–3000)             det : osc2 detune in SEMITONES (±)
  *   sus : sustain %    (0–100)              nse : noise mix 0.0–1.0
  *   rel : release ms   (0–4000)             lr  : LFO rate  0.0–1.0
  *   cut : filter cutoff 0–16383 DIRECT      ld  : LFO depth 0.0–1.0 (0 = off)
  *         (~1800 dark · ~5000 warm ·        lrt : LFO route 0–7
  *          ~8000 mid · ~12000 bright)       ec  : env→cutoff 0.0–1.0 (SEQ only*)
  *
  *  *ENV_CUT is consumed by the SEQ engine (filter-envelope sweep) but ignored by
  *   the HARP engine.  Bass/acid presets below set ec>0 and shine on the SEQ; on
  *   the HARP they play as a static-filtered tone (harmless).                   */
 #define SP(wv, atk, dec, sus, rel, cut, res, det, nse, lr, ld, lrt, o2, ec) \
   { (uint16_t)(wv), \
     (uint16_t)((atk)/2000.0f*16383.0f), \
     (uint16_t)((dec)/3000.0f*16383.0f), \
     (uint16_t)((sus)/100.0f*16383.0f), \
     (uint16_t)((rel)/4000.0f*16383.0f), \
     (uint16_t)(cut), \
     (uint16_t)((res)*16383.0f), \
     (uint16_t)((nse)*16383.0f), \
     (uint16_t)((det)*100.0f+8192.0f), \
     (uint16_t)((lr)*16383.0f), \
     (uint16_t)((ld)*16383.0f), \
     (uint16_t)((uint16_t)(lrt) & 7u), \
     (uint16_t)(o2), \
     (uint16_t)((ec)*16383.0f), 0u, 0u }
 
 extern const uint16_t SOUND_BANK[NUM_PATCHES][PARAMS_PER_PRESET] PROGMEM;
 extern const char PRESET_NAMES[NUM_PATCHES][16] PROGMEM;
 
 #endif /* ASSETS_H */
