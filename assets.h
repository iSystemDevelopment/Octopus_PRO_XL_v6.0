/* ═════════════════════════════════════════════════════════════════════════════
 * Octopus PRO XL v6.1.00 — Laser Harp Groovebox
 * © 2026 DIODAC ELECTRONICS / iSystem. All Rights Reserved.
 *
 * PROPRIETARY AND CONFIDENTIAL. Unauthorized copying, distribution, modification,
 * or use of this software or firmware, in whole or in part, is strictly prohibited
 * without prior written permission from DIODAC ELECTRONICS.
 * ═════════════════════════════════════════════════════════════════════════════
 * assets.h — v6.1.00  PROGMEM WAVETABLES + FACTORY SOUND BANK (declarations)
 *
 * Wave data and SOUND_BANK live in assets.cpp (PROGMEM).  wavetables_init_ram()
 * mirrors all 25 tables into WAVE_TABLE_RAM at boot for deterministic DSP reads.
 * SP macro packs factory preset rows matching SynthParam / livePatch[] layout.
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

/* Wave index == position in WAVE_TABLE_DIR (authoritative mapping):
 *   0 Saw … 8 Sine … 22 MoogBass … 24 Didgeridoo — see assets.cpp WAVE_TABLE_DIR */
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

/* RAM mirror — oscillators read WAVE_TABLE_RAM (internal DRAM), not PROGMEM flash. */
static constexpr int WAVE_TABLE_LEN = 256;
extern int16_t WAVE_TABLE_RAM[NUM_WAVE_TABLES][WAVE_TABLE_LEN];

void wavetables_init_ram(); /* call once at boot before audio task starts */

/* ═══════════════════════════════════════════════════════════════════════════
 * FACTORY SOUNDBANK
 *
 * NUM_FACTORY_PATCHES = authored preset count.  SOUND_BANK dimension is
 * NUM_PATCHES (256) for NVS sparse store; slots 128..191 are the 64 user sound
 * slots per engine; 192..255 are zero-padded (never recalled — clampRecallPatchIndex).
 * ═══════════════════════════════════════════════════════════════════════════ */
static constexpr int NUM_FACTORY_PATCHES = 128;
static_assert(NUM_FACTORY_PATCHES <= NUM_PATCHES,
              "NUM_FACTORY_PATCHES must fit inside SOUND_BANK[NUM_PATCHES]");

/* SP — factory preset row packer (16 slots = SynthParam order, see macro body).
 * ENV_CUT (slot 13) affects seq filter envelope; ignored by harp engine. */
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
