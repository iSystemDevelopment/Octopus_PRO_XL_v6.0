/* ═════════════════════════════════════════════════════════════════════════════
 * Octopus PRO XL v6.0.00 — Laser Harp Groovebox
 * © 2026 DIODAC ELECTRONICS / iSystem. All Rights Reserved.
 *
 * PROPRIETARY AND CONFIDENTIAL. Unauthorized copying, distribution, modification,
 * or use of this software or firmware, in whole or in part, is strictly prohibited
 * without prior written permission from DIODAC ELECTRONICS.
 * ═════════════════════════════════════════════════════════════════════════════
 * patches.h — v6.0.00  LIVE PATCH BRAIN + WEBAPP WIRE ARBITRATION
 *
 * Consolidates: patches.h (v5.0.03) + wires.h (v5.0.02)
 *
 * ── STAGE 1: BIDIRECTIONAL LIVE PATCH CONSISTENCY ─────────────────────────
 *
 *  THE ROOT BUG (fixed here):
 *  handleSysexCommand (midi.cpp) receives cmd 0–63 (synth/drum params) and
 *  writes directly to livePatch[] without updating the corresponding atomic
 *  mirrors (harpAttack, seqCutoff, drumTune[], etc.).  The audio task reads
 *  livePatch[] (correct), but the display, settings save, and interface.cpp
 *  all read the atomics — which remain stale after a WebApp parameter change.
 *  The encoder path (mutateSynth in interface.cpp) correctly updates both
 *  atomic and livePatch[] inside patchMux.  The WebApp path did not.
 *
 *  FIX: applyHarpParam / applySeqParam / applyDrumParam
 *  Each function writes livePatch[], the corresponding atomic, and userBank[]
 *  in one atomic critical section.  handleSysexCommand now calls these instead
 *  of writing livePatch[] directly.
 *
 *  ADDITIONAL FIXES:
 *  [W1] syncLivePatchFromAtomics() — replaces updateHarpPatch / updateSeqPatch
 *       / updateSampleBuffersSync in audio.h.  Canonical atomics → livePatch[]
 *       direction.  Called by settings_sync_to_ssot() after NVS load.
 *  [W2] recallHarpPatch / recallSeqPatch — real implementations (not stubs).
 *  [W3] BPM is the seqBpm atomic (SSOT); DMA-locked step engine reads it live.
 *  [W4] WebApp authority logic absorbed from wires.h — no second file needed.
 *  [W5] Complete parameter footprint table — PARAM_TABLE[PARAM_TABLE_COUNT].
 *       Every controllable CMD is documented with name, group, default, flags.
 *
 * ── THREE-LEVEL PARAMETER MODEL ────────────────────────────────────────────
 *
 *   Level 1  g_settings struct  (settings.h)   — NVS persist / load only
 *   Level 2  atomic mirrors     (globals.h)    — UI read, encoder write
 *   Level 3  livePatch[]        (globals.h)    — DSP read (harp.cpp/groovebox.cpp)
 *
 *   Direction A  NVS load:     g_settings → atomics → livePatch[]
 *                              settings_sync_to_ssot() → syncLivePatchFromAtomics()
 *   Direction B  Encoder turn: atomic + livePatch[] written together in patchMux
 *                              (mutateSynth in interface.cpp, calls txSysex echo)
 *   Direction C  WebApp rx:    applyHarpParam / applySeqParam / applyDrumParam
 *                              (writes both levels, was only livePatch[])
 *   Direction D  NVS save:     atomics → g_settings
 *                              settings_sync_from_ssot() — unchanged
 *
 * ── PARAMETER GROUPS ────────────────────────────────────────────────────────
 *
 *   GROUP 0  HARP_SYNTH    cmd   0–15   (16 params,  P_WAVEFORM..P_SPARE2)
 *   GROUP 1  SEQ_SYNTH     cmd  16–31   (16 params)
 *   GROUP 2  DRUM          cmd  32–63   (32 params,  8 drums × 4)
 *   GROUP 3  MASTER        cmd  64–96   (33 params,  vol/pitch/EQ/FX)
 *   GROUP 4  TRANSPORT     cmd  97–127  (31 params,  BPM/bank/seq control)
 *   GROUP 5  LASER         cmd 128–134  ( 7 params)
 *   GROUP 6  WIRE          cmd 135–137  ( 3 params,  MIDI channel routing)
 *
 * ═════════════════════════════════════════════════════════════════════════════ */
#pragma once
#ifndef PATCHES_H
#define PATCHES_H

#include <Arduino.h>
#include <atomic>
#include <cstring>
#include <algorithm>
#include "globals.h"
#include "midi.h"   /* txSysex, CMD_* constants, encodeMasterPitch */
#include "assets.h" /* SOUND_BANK, PRESET_NAMES, NUM_PATCHES */
#include "effect.h" /* [P4] FxChain fx — applyFxSend writes fx.*Insert sends */

/* [P4] decodeMasterPitch lives in interface.cpp (declared in interface.h).
 * Forward-declared here so applyMasterParam can map CMD_PITCH without pulling
 * the whole interface/GPIO header into every patches.h consumer.            */
float decodeMasterPitch(uint16_t v14);

/* ── CMD 138–147 are now declared in midi.h [M1] ───────────────────────── */
/* CMD_SEQ_CHAIN=138 … CMD_DB_ROUTE=145 come from midi.h via #include chain */

/* ═══════════════════════════════════════════════════════════════════════════
 * SECTION 1 — PARAMETER FOOTPRINT INDEX
 * Complete registry of every controllable parameter in the system.
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Parameter group identifiers */
enum class PGroup : uint8_t {
  HARP_SYNTH = 0,
  SEQ_SYNTH = 1,
  DRUM = 2,
  MASTER = 3,
  TRANSPORT = 4,
  LASER = 5,
  WIRE = 6
};

/* Parameter entry — one row per CMD in the footprint table */
struct ParamEntry {
  uint8_t cmd;          /* sysex CMD constant                           */
  PGroup group;         /* logical group                                */
  const char name[28];  /* human-readable label (max 27 chars + NUL)    */
  uint16_t default_v14; /* factory default in 14-bit wire format         */
  bool automatable;     /* can be recorded as a P-lock motion lane       */
  bool echo_on_hw;      /* hardware encoder change fires txSysex echo    */
};

/* clang-format off */
static const ParamEntry PARAM_TABLE[] = {
  /* ── GROUP 0: HARP SYNTH (cmd 0–15) ──────────────────────────────────── */
  { CMD_H_WAVE +  0, PGroup::HARP_SYNTH, "H.Waveform",        0,    true,  true  },
  { CMD_H_WAVE +  1, PGroup::HARP_SYNTH, "H.Attack",          164,  true,  true  },  /* 0.01 s */
  { CMD_H_WAVE +  2, PGroup::HARP_SYNTH, "H.Decay",           3277, true,  true  },  /* 0.2 s  */
  { CMD_H_WAVE +  3, PGroup::HARP_SYNTH, "H.Sustain",         11469,true,  true  },  /* 0.7    */
  { CMD_H_WAVE +  4, PGroup::HARP_SYNTH, "H.Release",         4915, true,  true  },  /* 0.3 s  */
  { CMD_H_WAVE +  5, PGroup::HARP_SYNTH, "H.Cutoff",          9830, true,  true  },  /* 0.6    */
  { CMD_H_WAVE +  6, PGroup::HARP_SYNTH, "H.Resonance",       1638, true,  true  },  /* 0.1    */
  { CMD_H_WAVE +  7, PGroup::HARP_SYNTH, "H.Noise",           0,    true,  true  },
  { CMD_H_WAVE +  8, PGroup::HARP_SYNTH, "H.Detune",          8192, true,  true  },  /* centre */
  { CMD_H_WAVE +  9, PGroup::HARP_SYNTH, "H.LFO Rate",        1638, true,  true  },  /* 0.1    */
  { CMD_H_WAVE + 10, PGroup::HARP_SYNTH, "H.LFO Depth",       0,    true,  true  },
  { CMD_HLFO_RT,     PGroup::HARP_SYNTH, "H.LFO Route",       0,    true,  true  },
  { CMD_H_WAVE + 12, PGroup::HARP_SYNTH, "H.Osc2 Wave",       0,    true,  true  },
  { CMD_H_WAVE + 13, PGroup::HARP_SYNTH, "H.Env→Cut",         0,    true,  true  },
  /* cmd 14–15 = spare, no atomics, not exposed in UI */

  /* ── GROUP 1: SEQ SYNTH (cmd 16–31) ──────────────────────────────────── */
  { CMD_S_WAVE +  0, PGroup::SEQ_SYNTH,  "S.Waveform",        1,    true,  true  },
  { CMD_S_WAVE +  1, PGroup::SEQ_SYNTH,  "S.Attack",          164,  true,  true  },
  { CMD_S_WAVE +  2, PGroup::SEQ_SYNTH,  "S.Decay",           3277, true,  true  },
  { CMD_S_WAVE +  3, PGroup::SEQ_SYNTH,  "S.Sustain",         8192, true,  true  },  /* 0.5    */
  { CMD_S_WAVE +  4, PGroup::SEQ_SYNTH,  "S.Release",         3277, true,  true  },  /* 0.2 s  */
  { CMD_S_WAVE +  5, PGroup::SEQ_SYNTH,  "S.Cutoff",          6553, true,  true  },  /* 0.4    */
  { CMD_S_WAVE +  6, PGroup::SEQ_SYNTH,  "S.Resonance",       4915, true,  true  },  /* 0.3    */
  { CMD_S_WAVE +  7, PGroup::SEQ_SYNTH,  "S.Noise",           0,    true,  true  },
  { CMD_S_WAVE +  8, PGroup::SEQ_SYNTH,  "S.Detune",          8192, true,  true  },
  { CMD_S_WAVE +  9, PGroup::SEQ_SYNTH,  "S.LFO Rate",        3277, true,  true  },  /* 0.2    */
  { CMD_S_WAVE + 10, PGroup::SEQ_SYNTH,  "S.LFO Depth",       0,    true,  true  },
  { CMD_SLFO_RT,     PGroup::SEQ_SYNTH,  "S.LFO Route",       0,    true,  true  },
  { CMD_S_WAVE + 12, PGroup::SEQ_SYNTH,  "S.Osc2 Wave",       0,    true,  true  },
  { CMD_S_WAVE + 13, PGroup::SEQ_SYNTH,  "S.Env→Cut",         0,    true,  true  },

  /* ── GROUP 2: DRUM (cmd 32–63, 8 drums × 4 params) ──────────────────── */
  /* Each entry is cmd = 32 + (drum_ch * 4) + param_idx                    */
  { 32,  PGroup::DRUM, "Kick Tune",       8192, true, true  },
  { 33,  PGroup::DRUM, "Kick Decay",      8192, true, true  },
  { 34,  PGroup::DRUM, "Kick Vol",        13107,true, true  },  /* 0.8 */
  { 35,  PGroup::DRUM, "Kick Noise",      0,    true, true  },
  { 36,  PGroup::DRUM, "Snare Tune",      8192, true, true  },
  { 37,  PGroup::DRUM, "Snare Decay",     8192, true, true  },
  { 38,  PGroup::DRUM, "Snare Vol",       13107,true, true  },
  { 39,  PGroup::DRUM, "Snare Noise",     6553, true, true  },  /* 0.4 */
  { 40,  PGroup::DRUM, "Clap Tune",       8192, true, true  },
  { 41,  PGroup::DRUM, "Clap Decay",      8192, true, true  },
  { 42,  PGroup::DRUM, "Clap Vol",        13107,true, true  },
  { 43,  PGroup::DRUM, "Clap Noise",      8192, true, true  },  /* 0.5 */
  { 44,  PGroup::DRUM, "HH-C Tune",       8192, true, true  },
  { 45,  PGroup::DRUM, "HH-C Decay",      8192, true, true  },
  { 46,  PGroup::DRUM, "HH-C Vol",        11469,true, true  },  /* 0.7 */
  { 47,  PGroup::DRUM, "HH-C Noise",      14745,true, true  },  /* 0.9 */
  { 48,  PGroup::DRUM, "HH-O Tune",       8192, true, true  },
  { 49,  PGroup::DRUM, "HH-O Decay",      8192, true, true  },
  { 50,  PGroup::DRUM, "HH-O Vol",        9830, true, true  },  /* 0.6 */
  { 51,  PGroup::DRUM, "HH-O Noise",      14745,true, true  },
  { 52,  PGroup::DRUM, "Tom-H Tune",      8192, true, true  },
  { 53,  PGroup::DRUM, "Tom-H Decay",     8192, true, true  },
  { 54,  PGroup::DRUM, "Tom-H Vol",       11469,true, true  },
  { 55,  PGroup::DRUM, "Tom-H Noise",     1638, true, true  },  /* 0.1 */
  { 56,  PGroup::DRUM, "Tom-L Tune",      8192, true, true  },
  { 57,  PGroup::DRUM, "Tom-L Decay",     8192, true, true  },
  { 58,  PGroup::DRUM, "Tom-L Vol",       11469,true, true  },
  { 59,  PGroup::DRUM, "Tom-L Noise",     1638, true, true  },
  { 60,  PGroup::DRUM, "Perc Tune",       8192, true, true  },
  { 61,  PGroup::DRUM, "Perc Decay",      8192, true, true  },
  { 62,  PGroup::DRUM, "Perc Vol",        11469,true, true  },
  { 63,  PGroup::DRUM, "Perc Noise",      3277, true, true  },  /* 0.2 */

  /* ── GROUP 3: MASTER (cmd 64–96) ──────────────────────────────────────── */
  { CMD_M_VOL,     PGroup::MASTER, "Master Vol",        12287,true,  true  },  /* 0.75 */
  { CMD_H_VOL,     PGroup::MASTER, "Harp Vol",          13107,true,  true  },  /* 0.8  */
  { CMD_S_VOL,     PGroup::MASTER, "Seq Vol",           13107,true,  true  },
  { CMD_D_VOL,     PGroup::MASTER, "Drum Vol",          14745,true,  true  },  /* 0.9  */
  { CMD_PITCH,     PGroup::MASTER, "Master Pitch",      4369, false, true  },  /* 1.0 in [0.25,4.0] range */
  { CMD_EQ_L,      PGroup::MASTER, "EQ Low",            8192, true,  true  },  /* 0 dB */
  { CMD_EQ_H,      PGroup::MASTER, "EQ High",           8192, true,  true  },
  { CMD_D_REV,     PGroup::MASTER, "Drum Rev Send",     0,    true,  true  },
  { CMD_D_DLY,     PGroup::MASTER, "Drum Dly Send",     0,    true,  true  },
  { CMD_TB_DRV,    PGroup::MASTER, "Tube Drive",        0,    true,  true  },
  { CMD_TB_TONE,   PGroup::MASTER, "Tube Tone",         8192, true,  true  },  /* 0.5  */
  { CMD_TB_MIX,    PGroup::MASTER, "Tube Mix",          0,    true,  true  },
  { CMD_DJ_FQ,     PGroup::MASTER, "DJ Freq",           16383,true,  true  },  /* 1.0  */
  { CMD_DJ_RES,    PGroup::MASTER, "DJ Res",            1638, true,  true  },
  { CMD_DJ_MIX,    PGroup::MASTER, "DJ Mix",            0,    true,  true  },
  { CMD_HLFO_R,    PGroup::MASTER, "H.LFO Rate (CC)",   1638, false, false },  /* mirror */
  { CMD_HLFO_D,    PGroup::MASTER, "H.LFO Depth (CC)",  0,    false, false },
  { CMD_SLFO_R,    PGroup::MASTER, "S.LFO Rate (CC)",   3277, false, false },
  { CMD_SLFO_D,    PGroup::MASTER, "S.LFO Depth (CC)",  0,    false, false },
  { CMD_DB_CURVE,  PGroup::MASTER, "DBeam Curve",       0,    false, true  },
  { CMD_DB_OFFSET, PGroup::MASTER, "DBeam Offset",      2048, false, true  },
  { CMD_DB_RANGE,  PGroup::MASTER, "DBeam Range",       1000, false, true  },
  { CMD_DB_ENABLED,PGroup::MASTER, "DBeam Enable",      16383,false, true  },
  { CMD_HW_H_OCT,  PGroup::MASTER, "Harp Octave",       0,    false, true  },
  { CMD_HW_S_OCT,  PGroup::MASTER, "Seq Octave",        0,    false, true  },
  { CMD_HW_GATE,   PGroup::MASTER, "Beam Gate ms",      200,  false, true  },
  { CMD_HW_WHITE,  PGroup::MASTER, "White Level",       32,   false, true  },
  { CMD_HW_MARGIN, PGroup::MASTER, "Beam Margin",       200,  false, true  },

  /* ── GROUP 4: TRANSPORT (cmd 97–127) ──────────────────────────────────── */
  { CMD_BPM,       PGroup::TRANSPORT, "BPM",            120,  false, true  },
  { CMD_BANK,      PGroup::TRANSPORT, "Seq Bank",        0,    false, true  },
  { CMD_TRANSPOSE, PGroup::TRANSPORT, "Transpose",       12,   true,  true  }, /* -12..+12, 0=12 */
  { CMD_HW_S_LEN,  PGroup::TRANSPORT, "Seq Length",      16,   false, true  },
  { CMD_TRIG_MODE, PGroup::TRANSPORT, "Play/Stop",       0,    false, false },
  { CMD_GRID_TOG,  PGroup::TRANSPORT, "Grid Toggle",     0,    false, false },
  { CMD_CLR_PLOCKS,PGroup::TRANSPORT, "Clear P-Locks",   0,    false, false },
  { CMD_LOAD_PAT_S,PGroup::TRANSPORT, "Load Synth Pat",  0,    false, false },
  { CMD_LOAD_PAT_D,PGroup::TRANSPORT, "Load Drum Pat",   0,    false, false },
  { CMD_STEP_SYNC, PGroup::TRANSPORT, "Step Sync (TX)",  0,    false, false }, /* TX only */
  { CMD_H_PATCH,   PGroup::TRANSPORT, "Harp Patch",      0,    false, true  },
  { CMD_S_PATCH,   PGroup::TRANSPORT, "Seq Patch",       4,    false, true  },
  { CMD_H_SCALE,   PGroup::TRANSPORT, "Harp Scale",      0,    false, true  },
  { CMD_D_FX_IDX,  PGroup::TRANSPORT, "Drum FX",         0,    true,  true  },
  { CMD_D_FX_IDX_B,PGroup::TRANSPORT, "Drum Dyn",        0,    true,  true  },
  { CMD_H_FX_IDX,  PGroup::TRANSPORT, "Harp FX",         0,    true,  true  },
  { CMD_H_FX_IDX_B,PGroup::TRANSPORT, "Harp Dyn",        0,    true,  true  },
  { CMD_S_FX_IDX,  PGroup::TRANSPORT, "Seq FX",          0,    true,  true  },
  { CMD_S_FX_IDX_B,PGroup::TRANSPORT, "Seq Dyn",         0,    true,  true  },
  { CMD_M_FX_IDX,  PGroup::TRANSPORT, "Master FX",       0,    false, true  },
  { CMD_H_FX_MIX,  PGroup::TRANSPORT, "Harp FX Mix",     0,    true,  true  },
  { CMD_H_FX_TIME, PGroup::TRANSPORT, "Harp Dly Time",   4915, true,  true  },  /* 0.3 s */
  { CMD_H_FX_SIZE, PGroup::TRANSPORT, "Harp Rev Size",   8192, true,  true  },  /* 0.5   */
  { CMD_S_FX_MIX,  PGroup::TRANSPORT, "Seq FX Mix",      0,    true,  true  },
  { CMD_S_FX_TIME, PGroup::TRANSPORT, "Seq Dly Time",    4915, true,  true  },
  { CMD_S_FX_SIZE, PGroup::TRANSPORT, "Seq Rev Size",    8192, true,  true  },
  { CMD_H_DLY_MIX, PGroup::TRANSPORT, "Harp Dly Send",   0,    true,  true  },
  { CMD_H_REV_MIX, PGroup::TRANSPORT, "Harp Rev Send",   0,    true,  true  },
  { CMD_S_DLY_MIX, PGroup::TRANSPORT, "Seq Dly Send",    0,    true,  true  },
  { CMD_S_REV_MIX, PGroup::TRANSPORT, "Seq Rev Send",    0,    true,  true  },
  { CMD_FETCH,     PGroup::TRANSPORT, "Fetch State",     0,    false, false },
  { CMD_HARD_SAVE, PGroup::TRANSPORT, "Hard Save",       0,    false, false },

  /* ── GROUP 5: LASER SHOW (cmd 128–134) ───────────────────────────────── */
  { CMD_LSR_SHOW,  PGroup::LASER, "Laser Show",          0,    false, true  },
  { CMD_MIDI_HUE,  PGroup::LASER, "MIDI→Hue",            0,    false, true  },
  { CMD_HUE_BASE,  PGroup::LASER, "Base Hue",            0,    false, true  },
  { CMD_HUE_ATK,   PGroup::LASER, "Hue Attack",          164,  false, true  },
  { CMD_HUE_DEC,   PGroup::LASER, "Hue Decay",           1638, false, true  },
  { CMD_HUE_SUS,   PGroup::LASER, "Hue Sustain",         16383,false, true  },
  { CMD_HUE_REL,   PGroup::LASER, "Hue Release",         3277, false, true  },
  { CMD_LSR_ANIM,  PGroup::LASER, "Anim Mode",           0,    false, true  },
  { CMD_LSR_DRUMFLASH, PGroup::LASER, "Drum Flash",      8192, false, true  },

  /* ── GROUP 6: WIRE / MIDI ROUTING (cmd 135–137) ──────────────────────── */
  { CMD_WIRE_HARP_CH, PGroup::WIRE, "Harp MIDI Ch",      1,    false, true  },
  { CMD_WIRE_SEQ_CH,  PGroup::WIRE, "Seq MIDI Ch",       2,    false, true  },
  { CMD_WIRE_DRUM_CH, PGroup::WIRE, "Drum MIDI Ch",      10,   false, true  },

  /* ── GROUP 6 cont.: Sequencer state (cmd 138–141) ─────────────────────── */
  { CMD_SEQ_CHAIN, PGroup::WIRE,      "Seq Chain",          0,    false, true  },
  { CMD_H_MUTE,   PGroup::MASTER,     "Harp Mute",          0,    false, true  },
  { CMD_S_MUTE,   PGroup::MASTER,     "Seq Mute",           0,    false, true  },
  { CMD_D_MUTE,   PGroup::MASTER,     "Drum Mute",          0,    false, true  },

  /* ── Aux FX holes (cmd 142–143) ───────────────────────────────────────── */
  { CMD_AUX_DLY_FB,  PGroup::MASTER,  "Aux Dly Feedback",  11469,true,  true  },
  { CMD_AUX_REV_DMP, PGroup::MASTER,  "Aux Rev Damp",      3277, true,  true  },

  /* ── Drum body waveform (cmd 144) ─────────────────────────────────────── */
  /* v14 encoding: bits 7–5 = ch (0–7), bits 4–0 = wave_idx (0–24)          */
  { CMD_DRUM_WAVE,   PGroup::DRUM,    "Drum Body Wave",     8,    true,  true  },

  /* ── D-BEAM expression routing (cmd 145) ──────────────────────────────── */
  /* CMD_DB_ROUTE: 0=OFF 1=MODULATION 2=VOLUME 3=CUTOFF                   */
  { CMD_DB_ROUTE,  PGroup::MASTER,  "DBeam Route",        3,    false, true  },

  /* ── Drum global pitch (cmd 181) — independent of CMD_PITCH ─────────── */
  { CMD_DRUM_PITCH, PGroup::DRUM,   "Drum Pitch",       16383, false, true  },

  /* ── Seq arpeggiator (cmd 182–185) ─────────────────────────────────── */
  { CMD_SEQ_ARP_EN,   PGroup::SEQ_SYNTH, "Seq Arp Enable",   1,    false, true  },
  { CMD_SEQ_ARP_PAT,  PGroup::SEQ_SYNTH, "Seq Arp Pattern",  7,    false, true  },
  { CMD_SEQ_ARP_RATE, PGroup::SEQ_SYNTH, "Seq Arp Rate",     7,    false, true  },
  { CMD_SEQ_ARP_GATE, PGroup::SEQ_SYNTH, "Seq Arp Gate",     7,    false, true  },
  { CMD_HARP_ARP_EN,   PGroup::HARP_SYNTH, "Harp Arp Enable",  1,    false, true  },
  { CMD_HARP_ARP_PAT,  PGroup::HARP_SYNTH, "Harp Arp Pattern", 3,    false, true  },
  { CMD_HARP_ARP_RATE, PGroup::HARP_SYNTH, "Harp Arp Rate",    3,    false, true  },
  { CMD_HARP_ARP_GATE, PGroup::HARP_SYNTH, "Harp Arp Gate",    3,    false, true  },
};
/* clang-format on */
static constexpr int PARAM_TABLE_COUNT =
  (int)(sizeof(PARAM_TABLE) / sizeof(PARAM_TABLE[0]));

/* Linear search in PARAM_TABLE — call at init time, not in hot path */
static inline const ParamEntry* findParamEntry(uint8_t cmd) {
  for (int i = 0; i < PARAM_TABLE_COUNT; ++i)
    if (PARAM_TABLE[i].cmd == cmd) return &PARAM_TABLE[i];
  return nullptr;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * SECTION 2 — SCALE TABLES
 * ═══════════════════════════════════════════════════════════════════════════ */

inline const int SCALES_NOTES[NUM_SCALES][MAX_SCALE_NOTES] = {
  { 60, 62, 64, 65, 67, 69, 71, 72 }, { 60, 62, 63, 65, 67, 68, 70, 72 }, { 60, 62, 64, 67, 69, 72, 74, 76 }, { 60, 63, 65, 66, 67, 70, 72, 75 }, { 60, 62, 63, 65, 67, 69, 70, 72 }, { 60, 61, 63, 65, 67, 68, 70, 72 }, { 60, 62, 63, 65, 67, 69, 70, 72 }, { 60, 62, 64, 66, 67, 69, 71, 72 }, { 60, 61, 63, 65, 66, 68, 70, 72 }, { 60, 62, 63, 65, 67, 68, 71, 72 }, { 60, 62, 63, 65, 67, 69, 71, 72 }, { 60, 61, 64, 65, 67, 68, 70, 72 }, { 60, 61, 64, 65, 67, 68, 71, 72 }, { 60, 61, 62, 63, 64, 65, 66, 67 }, { 60, 62, 64, 65, 67, 69, 71, 72 }, { 60, 62, 63, 65, 67, 68, 70, 72 }
};

inline const uint16_t SCALES_DAC_POS[NUM_SCALES][MAX_SCALE_NOTES] = {
  { 600, 1000, 1400, 1800, 2200, 2600, 3000, 3400 },
  { 600, 1000, 1400, 1800, 2200, 2600, 3000, 3400 },
  { 600, 1000, 1400, 1800, 2200, 2600, 3000, 3400 },
  { 600, 1000, 1400, 1800, 2200, 2600, 3000, 3400 },
  { 600, 1000, 1400, 1800, 2200, 2600, 3000, 3400 },
  { 600, 1000, 1400, 1800, 2200, 2600, 3000, 3400 },
  { 600, 1000, 1400, 1800, 2200, 2600, 3000, 3400 },
  { 600, 1000, 1400, 1800, 2200, 2600, 3000, 3400 },
  { 600, 1000, 1400, 1800, 2200, 2600, 3000, 3400 },
  { 600, 1000, 1400, 1800, 2200, 2600, 3000, 3400 },
  { 600, 1000, 1400, 1800, 2200, 2600, 3000, 3400 },
  { 600, 1000, 1400, 1800, 2200, 2600, 3000, 3400 },
  { 600, 1000, 1400, 1800, 2200, 2600, 3000, 3400 },
  { 600, 1000, 1400, 1800, 2200, 2600, 3000, 3400 },
  { 600, 1000, 1400, 1800, 2200, 2600, 3000, 3400 },
  { 600, 1000, 1400, 1800, 2200, 2600, 3000, 3400 }
};

inline ScaleDef SCALES[NUM_SCALES] = {
  { SCALES_NOTES[0], SCALES_DAC_POS[0], 8, false, "01 Major" },
  { SCALES_NOTES[1], SCALES_DAC_POS[1], 8, false, "02 Minor" },
  { SCALES_NOTES[2], SCALES_DAC_POS[2], 8, false, "03 Pentatonic" },
  { SCALES_NOTES[3], SCALES_DAC_POS[3], 8, false, "04 Blues" },
  { SCALES_NOTES[4], SCALES_DAC_POS[4], 8, false, "05 Dorian" },
  { SCALES_NOTES[5], SCALES_DAC_POS[5], 8, false, "06 Phrygian" },
  { SCALES_NOTES[6], SCALES_DAC_POS[6], 8, false, "07 Lydian" },
  { SCALES_NOTES[7], SCALES_DAC_POS[7], 8, false, "08 Mixolydian" },
  { SCALES_NOTES[8], SCALES_DAC_POS[8], 8, false, "09 Locrian" },
  { SCALES_NOTES[9], SCALES_DAC_POS[9], 8, false, "10 Harmonic Min" },
  { SCALES_NOTES[10], SCALES_DAC_POS[10], 8, false, "11 Melodic Min" },
  { SCALES_NOTES[11], SCALES_DAC_POS[11], 8, false, "12 Spanish" },
  { SCALES_NOTES[12], SCALES_DAC_POS[12], 8, false, "13 Arabic" },
  { SCALES_NOTES[13], SCALES_DAC_POS[13], 8, false, "14 Chromatic" },
  { SCALES_NOTES[14], SCALES_DAC_POS[14], 8, true, "15 Rainbow Maj" },
  { SCALES_NOTES[15], SCALES_DAC_POS[15], 8, true, "16 Rainbow Min" }
};

/* ── [EDGE-PERSCALE] Active edge-comp row selector ────────────────────────────
 * Every scale owns an independent per-string edge-comp row (edgeComp[scale][8]),
 * because trigger height tracks the beam COLOUR — different per scale, and per
 * string on the rainbow scales.  These helpers resolve the row for the live scale
 * so the threshold math and the editor always agree.                            */
static inline bool harpScaleIsRainbow() {
  return SCALES[(int)(harpScaleIndex.load(std::memory_order_relaxed) & (NUM_SCALES - 1))].isRainbow;
}
/* [EDGE-PERSCALE] Each scale owns an independent edge-comp row (edgeComp[scale]). */
static inline int harpScaleNow() {
  return (int)(harpScaleIndex.load(std::memory_order_relaxed) & (NUM_SCALES - 1));
}
static inline uint8_t*       activeEdgeComp()    { return edgeComp[harpScaleNow()]; }
static inline const uint8_t* activeEdgeFactory() { return EDGE_COMP_FACTORY[harpScaleNow()]; }

/* Same, for an ARBITRARY scale index — used by the Edge-Comp editor so OC can
 * scroll through all 16 scales (incl. rainbow) and correct each one's table. */
static inline bool scaleIsRainbow(int s) {
  return SCALES[s & (NUM_SCALES - 1)].isRainbow;
}
static inline uint8_t*       edgeCompFor(int s)    { return edgeComp[s & (NUM_SCALES - 1)]; }
static inline const uint8_t* edgeFactoryFor(int s) { return EDGE_COMP_FACTORY[s & (NUM_SCALES - 1)]; }

/* ═══════════════════════════════════════════════════════════════════════════
 * SECTION 3 — ATOMIC ↔ LIVEPATCH HELPERS
 *
 * applyHarpParam / applySeqParam / applyDrumParam
 * THE FIX for the root bug: every write to livePatch[] now also writes the
 * corresponding atomic mirror, inside the same patchMux critical section.
 *
 * Callers:
 *   handleSysexCommand (midi.cpp) — WebApp (USB SysEx) parameter changes
 *   recallHarpPatch / recallSeqPatch — preset recall
 *   syncLivePatchFromAtomics — NVS restore (reverse: atomics drive livePatch)
 * ═══════════════════════════════════════════════════════════════════════════ */

/* v14 → normalised float [0,1] */
static inline float v14_to_norm(uint16_t v) {
  return (float)v / 16383.0f;
}
/* normalised float [0,1] → v14 */
static inline uint16_t norm_to_v14(float f) {
  return (uint16_t)std::min(16383.0f, std::max(0.0f, f * 16383.0f));
}

/* ── applyHarpParam — write one HARP synth parameter (Direction C) ───────── */
static inline void applyHarpParam(int idx, uint16_t v14) {
  if (idx < 0 || idx >= PARAMS_PER_PRESET) return;

  portENTER_CRITICAL(&patchMux);
  const int pi = harpPatchIndex.load(std::memory_order_relaxed) & (NUM_PATCHES - 1);
  harpLivePatch[idx] = v14;
  userBank[pi][idx] = v14;
  /* [W1-FIX] Also update the atomic mirror so UI/display/settings stay in sync */
  switch (idx) {
    case (int)SynthParam::P_WAVEFORM:
      harpWaveform.store((float)(v14 % 25u), std::memory_order_relaxed);
      break;
    case (int)SynthParam::P_ATTACK:
      harpAttack.store(v14_to_norm(v14), std::memory_order_relaxed);
      break;
    case (int)SynthParam::P_DECAY:
      harpDecay.store(v14_to_norm(v14), std::memory_order_relaxed);
      break;
    case (int)SynthParam::P_SUSTAIN:
      harpSustain.store(v14_to_norm(v14), std::memory_order_relaxed);
      break;
    case (int)SynthParam::P_RELEASE:
      harpRelease.store(v14_to_norm(v14), std::memory_order_relaxed);
      break;
    case (int)SynthParam::P_CUTOFF:
      harpCutoff.store(v14_to_norm(v14), std::memory_order_relaxed);
      break;
    case (int)SynthParam::P_RESONANCE:
      harpResonance.store(v14_to_norm(v14), std::memory_order_relaxed);
      break;
    case (int)SynthParam::P_NOISE:
      harpNoise.store(v14_to_norm(v14), std::memory_order_relaxed);
      break;
    case (int)SynthParam::P_DETUNE:
      /* Bipolar: v14=8192→0.0, v14=0→-1.0, v14=16383→+1.0 */
      harpDetune.store((v14 / 16383.0f) * 2.0f - 1.0f, std::memory_order_relaxed);
      break;
    case (int)SynthParam::P_LFO_RATE:
      harpLfoRate.store(v14_to_norm(v14), std::memory_order_relaxed);
      break;
    case (int)SynthParam::P_LFO_DEPTH:
      harpLfoDepth.store(v14_to_norm(v14), std::memory_order_relaxed);
      break;
    case (int)SynthParam::P_LFO_ROUTE:
      harpLfoRoute.store((int32_t)(v14 & 7u), std::memory_order_relaxed);
      harpLivePatch[idx] = (uint16_t)(v14 & 7u); /* clamp route to 0–7 */
      break;
    case (int)SynthParam::P_OSC2_WAVE:
      harpOsc2Wave.store((float)(v14 % 25u), std::memory_order_relaxed);
      break;
    case (int)SynthParam::P_ENV_CUT:
      harpEnvCutAmount.store(v14_to_norm(v14), std::memory_order_relaxed);
      break;
    default: break; /* P_SPARE1/P_SPARE2 — no atomic to update */
  }
  portEXIT_CRITICAL(&patchMux);
}

/* ── applySeqParam — write one SEQ synth parameter ───────────────────────── */
static inline void applySeqParam(int idx, uint16_t v14) {
  if (idx < 0 || idx >= PARAMS_PER_PRESET) return;

  portENTER_CRITICAL(&patchMux);
  const int pi = seqPatchIndex.load(std::memory_order_relaxed) & (NUM_PATCHES - 1);
  seqLivePatch[idx] = v14;
  seqBank[pi][idx] = v14;
  /* [W1-FIX] atomic mirror sync */
  switch (idx) {
    case (int)SynthParam::P_WAVEFORM:
      seqWaveform.store((float)(v14 % 25u), std::memory_order_relaxed);
      break;
    case (int)SynthParam::P_ATTACK:
      seqAttack.store(v14_to_norm(v14), std::memory_order_relaxed);
      break;
    case (int)SynthParam::P_DECAY:
      seqDecay.store(v14_to_norm(v14), std::memory_order_relaxed);
      break;
    case (int)SynthParam::P_SUSTAIN:
      seqSustain.store(v14_to_norm(v14), std::memory_order_relaxed);
      break;
    case (int)SynthParam::P_RELEASE:
      seqRelease.store(v14_to_norm(v14), std::memory_order_relaxed);
      break;
    case (int)SynthParam::P_CUTOFF:
      seqCutoff.store(v14_to_norm(v14), std::memory_order_relaxed);
      break;
    case (int)SynthParam::P_RESONANCE:
      seqResonance.store(v14_to_norm(v14), std::memory_order_relaxed);
      break;
    case (int)SynthParam::P_NOISE:
      seqNoise.store(v14_to_norm(v14), std::memory_order_relaxed);
      break;
    case (int)SynthParam::P_DETUNE:
      seqDetune.store((v14 / 16383.0f) * 2.0f - 1.0f, std::memory_order_relaxed);
      break;
    case (int)SynthParam::P_LFO_RATE:
      seqLfoRate.store(v14_to_norm(v14), std::memory_order_relaxed);
      break;
    case (int)SynthParam::P_LFO_DEPTH:
      seqLfoDepth.store(v14_to_norm(v14), std::memory_order_relaxed);
      break;
    case (int)SynthParam::P_LFO_ROUTE:
      seqLfoRoute.store((int32_t)(v14 & 7u), std::memory_order_relaxed);
      seqLivePatch[idx] = (uint16_t)(v14 & 7u);
      break;
    case (int)SynthParam::P_OSC2_WAVE:
      seqOsc2Wave.store((float)(v14 % 25u), std::memory_order_relaxed);
      break;
    case (int)SynthParam::P_ENV_CUT:
      seqEnvCutAmount.store(v14_to_norm(v14), std::memory_order_relaxed);
      break;
    default: break;
  }
  portEXIT_CRITICAL(&patchMux);
}

/* ── applyDrumParam — write one DRUM parameter (idx = drum_ch*4 + param) ─── */
static inline void applyDrumParam(int idx, uint16_t v14) {
  if (idx < 0 || idx >= 32) return;
  const int ch = idx >> 2; /* drum channel 0–7      */
  const int par = idx & 3; /* 0=tune 1=decay 2=vol 3=noise */
  portENTER_CRITICAL(&patchMux);
  drumLivePatch[idx] = v14;
  /* [W1-FIX] atomic mirror sync */
  const float fv = v14_to_norm(v14);
  switch (par) {
    case 0: drumTune[ch].store(fv, std::memory_order_relaxed); break;
    case 1: drumDecay[ch].store(fv, std::memory_order_relaxed); break;
    case 2: drumVolume[ch].store(fv, std::memory_order_relaxed); break;
    case 3: drumNoiseMix[ch].store(fv, std::memory_order_relaxed); break;
  }
  portEXIT_CRITICAL(&patchMux);
}

/* ── encodeHarpParam — get current v14 value for any harp param (for echo) ── */
static inline uint16_t encodeHarpParam(int idx) {
  switch (idx) {
    case (int)SynthParam::P_WAVEFORM: return (uint16_t)harpWaveform.load(std::memory_order_relaxed);
    case (int)SynthParam::P_ATTACK: return norm_to_v14(harpAttack.load(std::memory_order_relaxed));
    case (int)SynthParam::P_DECAY: return norm_to_v14(harpDecay.load(std::memory_order_relaxed));
    case (int)SynthParam::P_SUSTAIN: return norm_to_v14(harpSustain.load(std::memory_order_relaxed));
    case (int)SynthParam::P_RELEASE: return norm_to_v14(harpRelease.load(std::memory_order_relaxed));
    case (int)SynthParam::P_CUTOFF: return norm_to_v14(harpCutoff.load(std::memory_order_relaxed));
    case (int)SynthParam::P_RESONANCE: return norm_to_v14(harpResonance.load(std::memory_order_relaxed));
    case (int)SynthParam::P_NOISE: return norm_to_v14(harpNoise.load(std::memory_order_relaxed));
    case (int)SynthParam::P_DETUNE: return norm_to_v14((harpDetune.load(std::memory_order_relaxed) + 1.0f) * 0.5f);
    case (int)SynthParam::P_LFO_RATE: return norm_to_v14(harpLfoRate.load(std::memory_order_relaxed));
    case (int)SynthParam::P_LFO_DEPTH: return norm_to_v14(harpLfoDepth.load(std::memory_order_relaxed));
    case (int)SynthParam::P_LFO_ROUTE: return (uint16_t)(harpLfoRoute.load(std::memory_order_relaxed) & 7);
    case (int)SynthParam::P_OSC2_WAVE: return (uint16_t)harpOsc2Wave.load(std::memory_order_relaxed);
    case (int)SynthParam::P_ENV_CUT: return norm_to_v14(harpEnvCutAmount.load(std::memory_order_relaxed));
    default:
      portENTER_CRITICAL(&patchMux);
      uint16_t v = (idx >= 0 && idx < PARAMS_PER_PRESET) ? harpLivePatch[idx] : 0;
      portEXIT_CRITICAL(&patchMux);
      return v;
  }
}

/* ── encodeSeqParam — get current v14 value for any seq param ──────────────── */
static inline uint16_t encodeSeqParam(int idx) {
  switch (idx) {
    case (int)SynthParam::P_WAVEFORM: return (uint16_t)seqWaveform.load(std::memory_order_relaxed);
    case (int)SynthParam::P_ATTACK: return norm_to_v14(seqAttack.load(std::memory_order_relaxed));
    case (int)SynthParam::P_DECAY: return norm_to_v14(seqDecay.load(std::memory_order_relaxed));
    case (int)SynthParam::P_SUSTAIN: return norm_to_v14(seqSustain.load(std::memory_order_relaxed));
    case (int)SynthParam::P_RELEASE: return norm_to_v14(seqRelease.load(std::memory_order_relaxed));
    case (int)SynthParam::P_CUTOFF: return norm_to_v14(seqCutoff.load(std::memory_order_relaxed));
    case (int)SynthParam::P_RESONANCE: return norm_to_v14(seqResonance.load(std::memory_order_relaxed));
    case (int)SynthParam::P_NOISE: return norm_to_v14(seqNoise.load(std::memory_order_relaxed));
    case (int)SynthParam::P_DETUNE: return norm_to_v14((seqDetune.load(std::memory_order_relaxed) + 1.0f) * 0.5f);
    case (int)SynthParam::P_LFO_RATE: return norm_to_v14(seqLfoRate.load(std::memory_order_relaxed));
    case (int)SynthParam::P_LFO_DEPTH: return norm_to_v14(seqLfoDepth.load(std::memory_order_relaxed));
    case (int)SynthParam::P_LFO_ROUTE: return (uint16_t)(seqLfoRoute.load(std::memory_order_relaxed) & 7);
    case (int)SynthParam::P_OSC2_WAVE: return (uint16_t)seqOsc2Wave.load(std::memory_order_relaxed);
    case (int)SynthParam::P_ENV_CUT: return norm_to_v14(seqEnvCutAmount.load(std::memory_order_relaxed));
    default:
      portENTER_CRITICAL(&patchMux);
      uint16_t v = (idx >= 0 && idx < PARAMS_PER_PRESET) ? seqLivePatch[idx] : 0;
      portEXIT_CRITICAL(&patchMux);
      return v;
  }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * SECTION 4 — DIRECTION A: ATOMICS → LIVEPATCH (NVS load, patch recall)
 *
 * [W1] syncLivePatchFromAtomics — replaces updateHarpPatch / updateSeqPatch /
 *      updateSampleBuffersSync from audio.h.  Call after settings_sync_to_ssot()
 *      and after any bulk-atomic update that should be immediately audible.
 * ═══════════════════════════════════════════════════════════════════════════ */
static inline void syncLivePatchFromAtomics() {
  /* [PATCH-PERF] Snapshot atomics outside patchMux; the lock only copies the
   * derived v14 arrays (~96 B) so audio/encoder writers aren't blocked for 32+
   * atomic loads.
   * [FIX-SPARE] Zero-initialise all arrays so P_SPARE1/P_SPARE2 (indices 14-15)
   * don't carry stack garbage into livePatch — they have no corresponding atomic. */
  uint16_t hp[PARAMS_PER_PRESET] = {};
  uint16_t sp[PARAMS_PER_PRESET] = {};
  uint16_t dp[32] = {};

  hp[(int)SynthParam::P_WAVEFORM] = (uint16_t)(std::min((float)24u, std::max(0.f, harpWaveform.load(std::memory_order_relaxed))));
  hp[(int)SynthParam::P_ATTACK] = norm_to_v14(std::min(1.f, std::max(0.f, harpAttack.load(std::memory_order_relaxed))));
  hp[(int)SynthParam::P_DECAY] = norm_to_v14(std::min(1.f, std::max(0.f, harpDecay.load(std::memory_order_relaxed))));
  hp[(int)SynthParam::P_SUSTAIN] = norm_to_v14(std::min(1.f, std::max(0.f, harpSustain.load(std::memory_order_relaxed))));
  hp[(int)SynthParam::P_RELEASE] = norm_to_v14(std::min(1.f, std::max(0.f, harpRelease.load(std::memory_order_relaxed))));
  hp[(int)SynthParam::P_CUTOFF] = norm_to_v14(std::min(1.f, std::max(0.f, harpCutoff.load(std::memory_order_relaxed))));
  hp[(int)SynthParam::P_RESONANCE] = norm_to_v14(std::min(1.f, std::max(0.f, harpResonance.load(std::memory_order_relaxed))));
  hp[(int)SynthParam::P_NOISE] = norm_to_v14(std::min(1.f, std::max(0.f, harpNoise.load(std::memory_order_relaxed))));
  hp[(int)SynthParam::P_DETUNE] = (uint16_t)std::min<uint32_t>(16383u,
      (uint32_t)((harpDetune.load(std::memory_order_relaxed) + 1.f) * 8191.5f));
  hp[(int)SynthParam::P_LFO_RATE] = norm_to_v14(std::min(1.f, std::max(0.f, harpLfoRate.load(std::memory_order_relaxed))));
  hp[(int)SynthParam::P_LFO_DEPTH] = norm_to_v14(std::min(1.f, std::max(0.f, harpLfoDepth.load(std::memory_order_relaxed))));
  hp[(int)SynthParam::P_LFO_ROUTE] = (uint16_t)(harpLfoRoute.load(std::memory_order_relaxed) & 7u);
  hp[(int)SynthParam::P_OSC2_WAVE] = (uint16_t)(std::min((float)24u, std::max(0.f, harpOsc2Wave.load(std::memory_order_relaxed))));
  hp[(int)SynthParam::P_ENV_CUT] = norm_to_v14(std::min(1.f, std::max(0.f, harpEnvCutAmount.load(std::memory_order_relaxed))));

  sp[(int)SynthParam::P_WAVEFORM] = (uint16_t)(std::min((float)24u, std::max(0.f, seqWaveform.load(std::memory_order_relaxed))));
  sp[(int)SynthParam::P_ATTACK] = norm_to_v14(std::min(1.f, std::max(0.f, seqAttack.load(std::memory_order_relaxed))));
  sp[(int)SynthParam::P_DECAY] = norm_to_v14(std::min(1.f, std::max(0.f, seqDecay.load(std::memory_order_relaxed))));
  sp[(int)SynthParam::P_SUSTAIN] = norm_to_v14(std::min(1.f, std::max(0.f, seqSustain.load(std::memory_order_relaxed))));
  sp[(int)SynthParam::P_RELEASE] = norm_to_v14(std::min(1.f, std::max(0.f, seqRelease.load(std::memory_order_relaxed))));
  sp[(int)SynthParam::P_CUTOFF] = norm_to_v14(std::min(1.f, std::max(0.f, seqCutoff.load(std::memory_order_relaxed))));
  sp[(int)SynthParam::P_RESONANCE] = norm_to_v14(std::min(1.f, std::max(0.f, seqResonance.load(std::memory_order_relaxed))));
  sp[(int)SynthParam::P_NOISE] = norm_to_v14(std::min(1.f, std::max(0.f, seqNoise.load(std::memory_order_relaxed))));
  sp[(int)SynthParam::P_DETUNE] = (uint16_t)std::min<uint32_t>(16383u,
      (uint32_t)((seqDetune.load(std::memory_order_relaxed) + 1.f) * 8191.5f));
  sp[(int)SynthParam::P_LFO_RATE] = norm_to_v14(std::min(1.f, std::max(0.f, seqLfoRate.load(std::memory_order_relaxed))));
  sp[(int)SynthParam::P_LFO_DEPTH] = norm_to_v14(std::min(1.f, std::max(0.f, seqLfoDepth.load(std::memory_order_relaxed))));
  sp[(int)SynthParam::P_LFO_ROUTE] = (uint16_t)(seqLfoRoute.load(std::memory_order_relaxed) & 7u);
  sp[(int)SynthParam::P_OSC2_WAVE] = (uint16_t)(std::min((float)24u, std::max(0.f, seqOsc2Wave.load(std::memory_order_relaxed))));
  sp[(int)SynthParam::P_ENV_CUT] = norm_to_v14(std::min(1.f, std::max(0.f, seqEnvCutAmount.load(std::memory_order_relaxed))));

  for (int ch = 0; ch < 8; ++ch) {
    dp[ch * 4 + 0] = norm_to_v14(std::min(1.f, std::max(0.f, drumTune[ch].load(std::memory_order_relaxed))));
    dp[ch * 4 + 1] = norm_to_v14(std::min(1.f, std::max(0.f, drumDecay[ch].load(std::memory_order_relaxed))));
    dp[ch * 4 + 2] = norm_to_v14(std::min(1.f, std::max(0.f, drumVolume[ch].load(std::memory_order_relaxed))));
    dp[ch * 4 + 3] = norm_to_v14(std::min(1.f, std::max(0.f, drumNoiseMix[ch].load(std::memory_order_relaxed))));
  }

  portENTER_CRITICAL(&patchMux);
  memcpy(harpLivePatch, hp, sizeof(hp));
  memcpy(seqLivePatch, sp, sizeof(sp));
  memcpy(drumLivePatch, dp, sizeof(dp));
  portEXIT_CRITICAL(&patchMux);
  std::atomic_thread_fence(std::memory_order_release);
}

/* syncPatchesToAudio — kept as the canonical barrier alias.
 * Now calls syncLivePatchFromAtomics() to ensure livePatch[] is always fresh. */
static inline void syncPatchesToAudio() {
  syncLivePatchFromAtomics();
}

/* ═══════════════════════════════════════════════════════════════════════════
 * SECTION 5 — PATCH RECALL
 * [W2] Real implementations — were bare-fence stubs in v5.0.03
 * ═══════════════════════════════════════════════════════════════════════════ */

/* recallHarpPatch — load from userBank[] or factory SOUND_BANK and apply. */
static inline void recallHarpPatch(int idx, ParamSource src) {
  idx = std::max(0, std::min((int)NUM_PATCHES - 1, idx));
  harpPatchIndex.store(idx, std::memory_order_relaxed);

  portENTER_CRITICAL(&patchMux);
  if (src == ParamSource::NVS || src == ParamSource::UI) {
    /* User bank: copy userBank[idx] → harpLivePatch then sync atomics */
    memcpy(harpLivePatch, userBank[idx], PARAMS_PER_PRESET * sizeof(uint16_t));
  } else {
    /* Factory bank */
    uint16_t factory[PARAMS_PER_PRESET];
    memcpy_P(factory, &SOUND_BANK[idx], PARAMS_PER_PRESET * sizeof(uint16_t));
    memcpy(harpLivePatch, factory, PARAMS_PER_PRESET * sizeof(uint16_t));
  }
  sanitizePatch(harpLivePatch); /* clamp before atomics derive from it */
  portEXIT_CRITICAL(&patchMux);

  /* Update atomics to match the newly loaded livePatch — uses the reverse
   * of applyHarpParam (livePatch → atomic) so display is immediately correct. */
  harpWaveform.store((float)(harpLivePatch[(int)SynthParam::P_WAVEFORM] % 25u), std::memory_order_relaxed);
  harpAttack.store(v14_to_norm(harpLivePatch[(int)SynthParam::P_ATTACK]), std::memory_order_relaxed);
  harpDecay.store(v14_to_norm(harpLivePatch[(int)SynthParam::P_DECAY]), std::memory_order_relaxed);
  harpSustain.store(v14_to_norm(harpLivePatch[(int)SynthParam::P_SUSTAIN]), std::memory_order_relaxed);
  harpRelease.store(v14_to_norm(harpLivePatch[(int)SynthParam::P_RELEASE]), std::memory_order_relaxed);
  harpCutoff.store(v14_to_norm(harpLivePatch[(int)SynthParam::P_CUTOFF]), std::memory_order_relaxed);
  harpResonance.store(v14_to_norm(harpLivePatch[(int)SynthParam::P_RESONANCE]), std::memory_order_relaxed);
  harpNoise.store(v14_to_norm(harpLivePatch[(int)SynthParam::P_NOISE]), std::memory_order_relaxed);
  harpDetune.store((harpLivePatch[(int)SynthParam::P_DETUNE] / 16383.0f) * 2.0f - 1.0f, std::memory_order_relaxed);
  harpLfoRate.store(v14_to_norm(harpLivePatch[(int)SynthParam::P_LFO_RATE]), std::memory_order_relaxed);
  harpLfoDepth.store(v14_to_norm(harpLivePatch[(int)SynthParam::P_LFO_DEPTH]), std::memory_order_relaxed);
  harpLfoRoute.store((int32_t)(harpLivePatch[(int)SynthParam::P_LFO_ROUTE] & 7u), std::memory_order_relaxed);
  harpOsc2Wave.store((float)(harpLivePatch[(int)SynthParam::P_OSC2_WAVE] % 25u), std::memory_order_relaxed);
  harpEnvCutAmount.store(v14_to_norm(harpLivePatch[(int)SynthParam::P_ENV_CUT]), std::memory_order_relaxed);

  std::atomic_thread_fence(std::memory_order_release);
  displayDirty.store(true, std::memory_order_relaxed);

  /* [BLOB] Echo the full patch to the App in one atomic message so its knobs
   * follow this recall — from ANY source (hardware menu, MIDI PC, or App).
   * Internally a no-op when no App is connected. */
  txPatchBlob(0u);
}

/* [SEQ-SOUND-PERSIST] Fan the current seqLivePatch[] out to the seq atomic
 * mirrors.  The atomics are the SSOT that settings_sync_from_ssot() reads at
 * SAVE time, so ANY code that writes seqLivePatch[] directly (recallSeqPatch,
 * loadFactorySynthPattern companion preset, …) MUST call this — otherwise the
 * sound you HEAR (driven by seqLivePatch) and the sound that gets SAVED (driven
 * by the atomics) diverge and the seq synth's sound silently fails to persist. */
static inline void syncSeqAtomicsFromLivePatch() {
  seqWaveform.store((float)(seqLivePatch[(int)SynthParam::P_WAVEFORM] % 25u), std::memory_order_relaxed);
  seqAttack.store(v14_to_norm(seqLivePatch[(int)SynthParam::P_ATTACK]), std::memory_order_relaxed);
  seqDecay.store(v14_to_norm(seqLivePatch[(int)SynthParam::P_DECAY]), std::memory_order_relaxed);
  seqSustain.store(v14_to_norm(seqLivePatch[(int)SynthParam::P_SUSTAIN]), std::memory_order_relaxed);
  seqRelease.store(v14_to_norm(seqLivePatch[(int)SynthParam::P_RELEASE]), std::memory_order_relaxed);
  seqCutoff.store(v14_to_norm(seqLivePatch[(int)SynthParam::P_CUTOFF]), std::memory_order_relaxed);
  seqResonance.store(v14_to_norm(seqLivePatch[(int)SynthParam::P_RESONANCE]), std::memory_order_relaxed);
  seqNoise.store(v14_to_norm(seqLivePatch[(int)SynthParam::P_NOISE]), std::memory_order_relaxed);
  seqDetune.store((seqLivePatch[(int)SynthParam::P_DETUNE] / 16383.0f) * 2.0f - 1.0f, std::memory_order_relaxed);
  seqLfoRate.store(v14_to_norm(seqLivePatch[(int)SynthParam::P_LFO_RATE]), std::memory_order_relaxed);
  seqLfoDepth.store(v14_to_norm(seqLivePatch[(int)SynthParam::P_LFO_DEPTH]), std::memory_order_relaxed);
  seqLfoRoute.store((int32_t)(seqLivePatch[(int)SynthParam::P_LFO_ROUTE] & 7u), std::memory_order_relaxed);
  seqOsc2Wave.store((float)(seqLivePatch[(int)SynthParam::P_OSC2_WAVE] % 25u), std::memory_order_relaxed);
  seqEnvCutAmount.store(v14_to_norm(seqLivePatch[(int)SynthParam::P_ENV_CUT]), std::memory_order_relaxed);
}

/* recallSeqPatch — load seq patch from seqBank[]. */
static inline void recallSeqPatch(int idx, ParamSource /*src*/) {
  idx = std::max(0, std::min((int)NUM_PATCHES - 1, idx));
  seqPatchIndex.store(idx, std::memory_order_relaxed);

  portENTER_CRITICAL(&patchMux);
  memcpy(seqLivePatch, seqBank[idx], PARAMS_PER_PRESET * sizeof(uint16_t));
  sanitizePatch(seqLivePatch); /* clamp before atomics derive from it */
  portEXIT_CRITICAL(&patchMux);

  syncSeqAtomicsFromLivePatch(); /* [SEQ-SOUND-PERSIST] keep atomics == live sound */

  uint8_t newVer = (seqLivePatchVersion.load(std::memory_order_relaxed) + 1u) & 0xFFu;
  seqLivePatchVersion.store(newVer, std::memory_order_release);
  std::atomic_thread_fence(std::memory_order_release);
  displayDirty.store(true, std::memory_order_relaxed);

  /* [BLOB] One-shot full patch echo so the App's seq knobs follow this recall
   * (hardware menu, MIDI PC, or App).  No-op when no App is connected. */
  txPatchBlob(1u);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * SECTION 6 — BPM
 * Tempo lives in the seqBpm atomic (SSOT).  The DMA-locked step engine
 * (groovebox.cpp sequencer_render_block) reads it directly every audio buffer,
 * so there is no external clock object to keep in sync.  setSequencerBpm() is
 * the only writer; settings restore writes seqBpm directly.
 * ═══════════════════════════════════════════════════════════════════════════ */

/* ═══════════════════════════════════════════════════════════════════════════
 * SECTION 7 — WEBAPP WIRE ARBITRATION (absorbed from wires.h)
 * [W4] Heartbeat watchdog + authority router merged here — no second file.
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Thread-safe flag: WebApp has been heard from in the last 4.5 seconds */
inline std::atomic<bool> appSyncActive{ false };

/* Check WebApp heartbeat — called from checkWireAuthority and display task */
static inline void pollSyncHeartbeat() {
  const uint32_t now = millis();
  const uint32_t last = lastWebSysexMs.load(std::memory_order_relaxed);
  const bool wasConnected = appSyncActive.load(std::memory_order_relaxed);
  /* last==0 → no app sysex ever received; never report connected at boot. */
  const bool nowConnected = (last != 0UL) && (now - last < 4500UL);
  appSyncActive.store(nowConnected, std::memory_order_release);
  /* [v6.0] Connection state changed → repaint (splash ⇄ dashboard).  Transport
   * ownership no longer toggles here: the hardware always owns it.            */
  if (wasConnected != nowConnected)
    displayDirty.store(true, std::memory_order_relaxed);
}

/* Arbitration: returns true if this parameter change is authorised.
 *
 * Rules:
 *   — Transport-critical params (CMD_TRIG_MODE, CMD_BPM, CMD_CLR_PLOCKS)
 *     always pass through regardless of WebApp lock.
 *   — When the WebApp has been active in the last 4.5 s, local hardware
 *     encoder turns are blocked from *echoing* (prevents wire fights) but
 *     [WS4] the parameter still applies on-device for live performance.
 *   — All WebApp-sourced changes and all non-local calls always pass. */
static inline bool checkWireAuthority(uint8_t cmdKey, bool isLocalUiHardwareCall) {
  pollSyncHeartbeat();
  /* Transport-critical commands always bypass the lock */
  if (cmdKey == CMD_TRIG_MODE || cmdKey == CMD_BPM || cmdKey == CMD_CLR_PLOCKS)
    return true;
  if (appSyncActive.load(std::memory_order_acquire) && isLocalUiHardwareCall)
    return false;
  return true;
}

/* Transmit a parameter change over the wire (sysex) if authority permits.
 * isLocalCall = true when called from hardware encoder / button handler.    */
static inline void wireTransmitParameter(uint8_t cmdKey, uint16_t value14Bit,
                                         bool isLocalCall) {
  if (!checkWireAuthority(cmdKey, isLocalCall)) return;
  txSysex(cmdKey, value14Bit);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * SECTION 8 — [P0 CLEANUP] removed updateHarpPatch / updateSeqPatch /
 * updateSampleBuffersSync inline aliases (no remaining callers).  All call
 * sites use syncLivePatchFromAtomics() directly — the canonical atomics →
 * livePatch[] sync.
 * ═══════════════════════════════════════════════════════════════════════════ */

/* ═══════════════════════════════════════════════════════════════════════════
 * SECTION 9 — SEQUENCER & GRID PARAMETER MANAGEMENT
 *
 * Covers the 4 holes found in the audit:
 *
 *  HOLE #1  seqActiveChain — no CMD constant existed (CMD_SEQ_CHAIN = 138)
 *  HOLE #2  mixHarpMute    — no CMD constant existed (CMD_H_MUTE    = 139)
 *  HOLE #3  mixSeqMute     — no CMD constant existed (CMD_S_MUTE    = 140)
 *  HOLE #4  mixDrumsMute   — no CMD constant existed (CMD_D_MUTE    = 141)
 *
 *  ECHO GAP  interface.cpp SEQ SETUP changed bank/chain/length/transpose
 *            without txSysex echo — WebApp display was permanently stale.
 *            Fixed: use applySeqBank/Chain/Length/Transpose from here.
 *
 *  FETCH GAP sendFullStateSync missed 12+ parameters.
 *            Fixed: call echoFullSeqState() at the end of sendFullStateSync().
 *
 *  BOUNDARY  seqUI_row / seqUI_col / seqUI_move* / seqUI_renderMatrix:
 *            Pure local grid cursor state — NO CMD, no echo, no WebApp need.
 *            Lives in globals.h; driven by the SEQ MATRIX editor in interface.cpp.
 *            Not managed here.
 *
 * NEW CMD constants required in midi.h (add after CMD_WIRE_DRUM_CH = 137):
 *   static constexpr uint8_t CMD_SEQ_CHAIN = 138;
 *   static constexpr uint8_t CMD_H_MUTE    = 139;
 *   static constexpr uint8_t CMD_S_MUTE    = 140;
 *   static constexpr uint8_t CMD_D_MUTE    = 141;
 *
 * NEW handleSysexCommand cases required in midi.cpp extended switch:
 *   case CMD_SEQ_CHAIN: seqActiveChain.store((uint8_t)(v14&3u)); displayDirty.store(true); break;
 *   case CMD_H_MUTE:    mixHarpMute .store(v14 > 0u); break;
 *   case CMD_S_MUTE:    mixSeqMute  .store(v14 > 0u); break;
 *   case CMD_D_MUTE:    mixDrumsMute.store(v14 > 0u); break;
 * ═══════════════════════════════════════════════════════════════════════════ */

/* ── seqSyncTransposeToActivePattern — [PER-PATTERN-TRANSPOSE] ─────────────────
 * Pull the active pattern's stored transpose into the live seqTranspose atomic
 * (read lock-free by the audio engine) and echo it so the App's transpose box
 * follows whichever pattern is now active.  Call after any bank/chain switch. */
static inline void seqSyncTransposeToActivePattern() {
  const uint8_t bank = seqActiveBank.load(std::memory_order_relaxed) & 15u;
  const uint8_t chain = seqActiveChain.load(std::memory_order_relaxed) & 3u;
  const int32_t t = (int32_t)seqPatternTranspose[bank][chain];
  seqTranspose.store(t, std::memory_order_relaxed);
  displayDirty.store(true, std::memory_order_relaxed);
  txSysex(CMD_TRANSPOSE, (uint16_t)(t + 12));
}

/* ── applySeqBank — set active bank + displayDirty + echo ─────────────────── */
static inline void applySeqBank(uint8_t bank) {
  bank = bank & 0x0Fu;
  seqActiveBank.store(bank, std::memory_order_release);
  displayDirty.store(true, std::memory_order_relaxed);
  txSysex(CMD_BANK, (uint16_t)bank);
  seqSyncTransposeToActivePattern(); /* new pattern → its own transpose */
}

/* ── applySeqChain — set active chain (0=synth 1=drum) + echo ────────────── */
/* Requires CMD_SEQ_CHAIN = 138 to be added to midi.h                         */
static inline void applySeqChain(uint8_t chain) {
  chain = chain & 0x03u;
  seqActiveChain.store(chain, std::memory_order_release);
  displayDirty.store(true, std::memory_order_relaxed);
  txSysex(CMD_SEQ_CHAIN, (uint16_t)chain);
  seqSyncTransposeToActivePattern(); /* new pattern → its own transpose */
}

/* ── applySeqLength — set step count (1–64) + echo ──────────────────────── */
static inline void applySeqLength(uint16_t raw) {
  const uint8_t len = (uint8_t)std::min<int>(64, std::max<int>(1, (int)raw));
  seqLength.store(len, std::memory_order_relaxed);
  displayDirty.store(true, std::memory_order_relaxed);
  txSysex(CMD_HW_S_LEN, (uint16_t)len);
}

/* ── applySeqTranspose — set transpose ±12 semitones + echo ─────────────── */
/* v14 wire encoding: v14 = transpose + 12, so v14=0 → -12, v14=12 → 0,
 *                    v14=24 → +12.  Matches handleSysexCommand CMD_TRANSPOSE. */
static inline void applySeqTranspose(uint16_t v14) {
  /* Compact wire encoding only (v14 = semitone + 12, range 0..24).  Full-scale
   * 0..16383 values from P-lock cross-talk or misrouted knobs must be ignored —
   * (v14 - 12) on e.g. 8192 would clamp to +12 and look like a random octave jump. */
  if (v14 > 24u) return;
  const int32_t t = std::min<int32_t>(12, std::max<int32_t>(-12,
                                                            (int32_t)v14 - 12));
  seqTranspose.store(t, std::memory_order_relaxed);
  /* [PER-PATTERN-TRANSPOSE] Persist the edit into the ACTIVE pattern cell so it
   * survives pattern switches + the next NVS save.  Single-byte write → no lock
   * needed (audio core only ever reads its own active cell). */
  const uint8_t bank = seqActiveBank.load(std::memory_order_relaxed) & 15u;
  const uint8_t chain = seqActiveChain.load(std::memory_order_relaxed) & 3u;
  seqPatternTranspose[bank][chain] = (int8_t)t;
  displayDirty.store(true, std::memory_order_relaxed);
  txSysex(CMD_TRANSPOSE, (uint16_t)(t + 12));
}

/* ── Seq arpeggiator (CMD 182–185) ─────────────────────────────────────── */
/* Compact 0..max UI index, or scale a 0..16383 knob value when misrouted. */
static inline uint8_t clampDiscreteUi(uint16_t v14, uint8_t max) {
  if (v14 <= max) return (uint8_t)v14;
  return (uint8_t)std::min<uint32_t>(
      max, (uint32_t)(v14 * (uint32_t)max + 8192u) / 16383u);
}
static inline void applySeqArpEnable(uint16_t v14) {
  seqArpEnabled.store(v14 > 0u, std::memory_order_relaxed);
  txSysex(CMD_SEQ_ARP_EN, v14 > 0u ? 16383u : 0u);
}
static inline void applySeqArpPattern(uint16_t v14) {
  const uint8_t p = clampDiscreteUi(v14, 7u);
  seqArpPattern.store(p, std::memory_order_relaxed);
  txSysex(CMD_SEQ_ARP_PAT, p);
}
static inline void applySeqArpRate(uint16_t v14) {
  const uint8_t r = clampDiscreteUi(v14, 7u);
  seqArpRate.store(r, std::memory_order_relaxed);
  txSysex(CMD_SEQ_ARP_RATE, r);
}
static inline void applySeqArpGate(uint16_t v14) {
  const uint8_t g = clampDiscreteUi(v14, 7u);
  seqArpGate.store(g, std::memory_order_relaxed);
  txSysex(CMD_SEQ_ARP_GATE, g);
}
static inline void echoSeqArpState() {
  txSysex(CMD_SEQ_ARP_EN,   seqArpEnabled.load(std::memory_order_relaxed) ? 16383u : 0u);
  txSysex(CMD_SEQ_ARP_PAT,  (uint16_t)seqArpPattern.load(std::memory_order_relaxed));
  txSysex(CMD_SEQ_ARP_RATE, (uint16_t)seqArpRate.load(std::memory_order_relaxed));
  txSysex(CMD_SEQ_ARP_GATE, (uint16_t)seqArpGate.load(std::memory_order_relaxed));
}

/* ── Harp arpeggiator (CMD 186–189) ────────────────────────────────────── */
static inline void applyHarpArpEnable(uint16_t v14) {
  if (currentPlayMode.load(std::memory_order_relaxed) == PlayMode::STRINGS) {
    harpArpEnabled.store(false, std::memory_order_relaxed);
    txSysex(CMD_HARP_ARP_EN, 0u);
    return;
  }
  harpArpEnabled.store(v14 > 0u, std::memory_order_relaxed);
  txSysex(CMD_HARP_ARP_EN, v14 > 0u ? 16383u : 0u);
}
static inline void applyHarpArpPattern(uint16_t v14) {
  const uint8_t p = clampDiscreteUi(v14, 3u);
  harpArpPattern.store(p, std::memory_order_relaxed);
  txSysex(CMD_HARP_ARP_PAT, p);
}
static inline void applyHarpArpRate(uint16_t v14) {
  const uint8_t r = clampDiscreteUi(v14, 3u);
  harpArpRate.store(r, std::memory_order_relaxed);
  txSysex(CMD_HARP_ARP_RATE, r);
}
static inline void applyHarpArpGate(uint16_t v14) {
  const uint8_t g = clampDiscreteUi(v14, 3u);
  harpArpGate.store(g, std::memory_order_relaxed);
  txSysex(CMD_HARP_ARP_GATE, g);
}
static inline void echoHarpArpState() {
  txSysex(CMD_HARP_ARP_EN,   harpArpEnabled.load(std::memory_order_relaxed) ? 16383u : 0u);
  txSysex(CMD_HARP_ARP_PAT,  (uint16_t)harpArpPattern.load(std::memory_order_relaxed));
  txSysex(CMD_HARP_ARP_RATE, (uint16_t)harpArpRate.load(std::memory_order_relaxed));
  txSysex(CMD_HARP_ARP_GATE, (uint16_t)harpArpGate.load(std::memory_order_relaxed));
}

/* ── applyMute — set a single mute flag + echo ───────────────────────────── */
/* cmd must be CMD_H_MUTE / CMD_S_MUTE / CMD_D_MUTE                           */
static inline void applyMute(uint8_t cmd, bool muted) {
  switch (cmd) {
    case CMD_H_MUTE: mixHarpMute.store(muted, std::memory_order_relaxed); break;
    case CMD_S_MUTE: mixSeqMute.store(muted, std::memory_order_relaxed); break;
    case CMD_D_MUTE: mixDrumsMute.store(muted, std::memory_order_relaxed); break;
    default: return;
  }
  txSysex(cmd, muted ? 16383u : 0u);
}

/* ── applySeqOctave — set per-engine octave shift + echo ────────────────── */
/* engine: 0 = harp (CMD_HW_H_OCT), 1 = seq (CMD_HW_S_OCT)                   */
static inline void applySeqOctave(int engine, int16_t delta) {
  if (engine < 0 || engine > 1) return;
  const int32_t cur = octaveShift[engine].load(std::memory_order_relaxed);
  const int32_t nv = std::min<int32_t>(4, std::max<int32_t>(-4, cur + delta));
  octaveShift[engine].store(nv, std::memory_order_relaxed);
  const uint8_t cmd = (engine == 0) ? CMD_HW_H_OCT : CMD_HW_S_OCT;
  /* Wire encoding: v14 = octave + 4 (0..8) so v14=4 → oct=0 */
  txSysex(cmd, (uint16_t)(nv + 4));
}

/* ── applySeqOctaveSet — absolute octave from App (v14 = oct + 4) + echo ── */
static inline void applySeqOctaveSet(int engine, uint16_t v14) {
  if (engine < 0 || engine > 1) return;
  if (v14 > 8u) return; /* reject full-scale knob values mistaken for oct+4 */
  const int32_t oct = std::min<int32_t>(4, std::max<int32_t>(-4, (int32_t)v14 - 4));
  octaveShift[engine].store(oct, std::memory_order_relaxed);
  displayDirty.store(true, std::memory_order_relaxed);
  const uint8_t cmd = (engine == 0) ? CMD_HW_H_OCT : CMD_HW_S_OCT;
  txSysex(cmd, (uint16_t)(oct + 4));
}

/* ─────────────────────────────────────────────────────────────────────────────
 * echoFullSeqState — complete echo of all sequencer/grid/mute state.
 *
 * Call at the END of sendFullStateSync() in midi.cpp (one line: echoFullSeqState();)
 * Closes the FETCH gap — WebApp reconnect now gets full sequencer state.
 * ─────────────────────────────────────────────────────────────────────────────*/
/* Forward declarations — functions defined later in this file */
static inline void echoAuxParams();
static inline void echoAllDrumWaves();
static inline void echoDbeamExprState();
static inline void echoPbMapping();

/* [G2] echoGridRow — send one row's ABSOLUTE 64-step state to the App.
 * [FIX-GRID-ENC] Now uses txGridRow (SX_SUB_GRID_ROW, sub 0x05) instead of
 * the old txSysex(CMD_GRID_ROW_LO/HI, pageAddr|byte) encoding.  The old format
 * overlapped page bits with byte bits (both shared bits 4-5 of v14), causing
 * steps 4 and 5 to be spuriously set for pages 1-3 even when they were inactive.
 * New format packs all fields into dedicated bytes — no overlap, no data loss.
 * Grid uses chain 0 (synth rows 0-7, drum rows 8-15) — the OctopusApp model.  */
static inline void echoGridRow(uint8_t bank, uint8_t row) {
  bank &= 3u;
  row &= 15u;
  portENTER_CRITICAL(&patchMux);
  const uint64_t mask = hwSeqData[bank][0][row]; /* chain pinned 0 */
  portEXIT_CRITICAL(&patchMux);
  for (uint8_t page = 0; page < 4u; ++page) {
    const uint8_t lo = (uint8_t)((mask >> (page * 16u))      & 0xFFu);
    const uint8_t hi = (uint8_t)((mask >> (page * 16u + 8u)) & 0xFFu);
    txGridRow(bank, row, page, lo, hi);
  }
}

/* [G2] echoFullGrid — dump all 4 banks × 16 rows (absolute).  Connect-time
 * mirror so the App grid exactly matches hwSeqData (incl. hardware edits).    */
static inline void echoFullGrid() {
  for (uint8_t b = 0; b < 4u; ++b)
    for (uint8_t r = 0; r < 16u; ++r)
      echoGridRow(b, r);
}

static inline void echoPbMapping() {
  txSysex(CMD_PB_RANGE, (uint16_t)pbMapping.upSemi.load(std::memory_order_relaxed));
  txSysex(CMD_PB_ENABLE, pbMapping.enabled.load(std::memory_order_relaxed) ? 16383u : 0u);
}

static inline void echoFullSeqState() {
  /* ── Transport ──────────────────────────────────────────────────────────
   * [v6.0] Transport is hardware-owned; this is the App's read-only snapshot on
   * connect.  Play, record (3/4) and the current step are all sent so a mid-play
   * connect shows the playhead immediately (the per-step STEP_SYNC stream then
   * keeps it live); the sync supervisor re-asserts BPM/play/record afterwards.  */
  const bool playingNow = seqPlaying.load(std::memory_order_relaxed);
  txSysex(CMD_BPM, (uint16_t)seqBpm.load(std::memory_order_relaxed));
  txSysex(CMD_TRIG_MODE, playingNow ? 16383u : 0u);
  txSysex(CMD_TRANSPORT, playingNow ? 1u : 0u);
  txSysex(CMD_TRANSPORT, seqRecording.load(std::memory_order_relaxed) ? 3u : 4u);
  if (playingNow)
    txSysex(CMD_STEP_SYNC, (uint16_t)(seqCurrentStep.load(std::memory_order_relaxed) & 63u));
  txSysex(CMD_SONG_MODE, songModeActive.load(std::memory_order_relaxed) ? 16383u : 0u);
  txSysex(CMD_BANK, (uint16_t)(seqActiveBank.load(std::memory_order_relaxed) & 15u));
  txSysex(CMD_SEQ_CHAIN, (uint16_t)(seqActiveChain.load(std::memory_order_relaxed) & 3u));
  txSysex(CMD_HW_S_LEN, (uint16_t)seqLength.load(std::memory_order_relaxed));
  txSysex(CMD_TRANSPOSE, (uint16_t)(seqTranspose.load(std::memory_order_relaxed) + 12));
  /* [WS1] Harp continuous pitch-bend: v14 = (12·log2(mult))/12·8192 + 8192. */
  {
    const float mult = harpPitchMult.load(std::memory_order_relaxed);
    const float semis = 12.0f * log2f(mult > 0.0001f ? mult : 0.0001f);
    int hv = (int)lrintf(semis / 12.0f * 8192.0f + 8192.0f);
    hv = std::min(16383, std::max(0, hv));
    txSysex(CMD_H_PITCH, (uint16_t)hv);
  }

  /* ── Octave shifts ────────────────────────────────────────────────────── */
  txSysex(CMD_HW_H_OCT, (uint16_t)(octaveShift[0].load(std::memory_order_relaxed) + 4));
  txSysex(CMD_HW_S_OCT, (uint16_t)(octaveShift[1].load(std::memory_order_relaxed) + 4));
  echoPbMapping();

  echoSeqArpState();
  echoHarpArpState();

  /* ── Mute states ──────────────────────────────────────────────────────── */
  txSysex(CMD_H_MUTE, mixHarpMute.load(std::memory_order_relaxed) ? 16383u : 0u);
  txSysex(CMD_S_MUTE, mixSeqMute.load(std::memory_order_relaxed) ? 16383u : 0u);
  txSysex(CMD_D_MUTE, mixDrumsMute.load(std::memory_order_relaxed) ? 16383u : 0u);

  /* ── Seq patch + FX B-slots ───────────────────────────────────────────── */
  txSysex(CMD_S_PATCH, (uint16_t)seqPatchIndex.load(std::memory_order_relaxed));
  txSysex(CMD_H_FX_IDX_B, (uint16_t)harpFxIndexB.load(std::memory_order_relaxed));
  txSysex(CMD_S_FX_IDX_B, (uint16_t)seqFxIndexB.load(std::memory_order_relaxed));
  txSysex(CMD_D_FX_IDX, (uint16_t)drumFxIndexA.load(std::memory_order_relaxed));
  txSysex(CMD_D_FX_IDX_B, (uint16_t)drumFxIndexB.load(std::memory_order_relaxed));
  /* Aux FX parameters */
  echoAuxParams();
  /* Drum body waveform selections */
  echoAllDrumWaves();
  /* D-BEAM expression routing */
  echoDbeamExprState();
  /* [G2] Full 16×16 grid (all 4 banks) — App becomes a 1:1 mirror of hwSeqData */
  echoFullGrid();
}
/* ═════════════════════════════════════════════════════════════════════════════
 * SECTION 10 — SOUND ADDITIONS  (v6.0.00)
 *
 *   A. Two missing FX globals (aux delay feedback + reverb damp)
 *   B. Sound bank preset management (factory recall, user save, name query)
 *   C. Drum voice waveform selection (5 of 8 voices are wavetable-capable)
 *
 *        after Section 9 (echoFullSeqState).
 *
 * ═════════════════════════════════════════════════════════════════════════════
 * WAVEFORM CATALOG (25 wavetables, index 0–24)
 * ─────────────────────────────────────────────────────────────────────────────
 * Index  Name in kWaves[]      Good for
 *   0    Cosmic Saw            Harp pluck, seq bass, bright synth
 *   1    Quantum Sq            Retro keys, chiptune, hard lead
 *   2    Pulsar 25%            Thin buzz, hi-pitch arps
 *   3    Stellar Tri           Flute-like, soft leads, mellow
 *   4    Nebula Organ          Drawbar organ, pads
 *   5    Astral Vocal          Vowel-like formant, choir prep
 *   6    Chrono Bell           Percussion body, metallic
 *   7    Aether String         Bowed strings, warm pads
 *   8    Singular Sine         Sub bass, kick body, smooth lead
 *   9    Pulsar 10%            Very thin, high harmonics
 *  10    Pulsar 40%            Medium pulse, classic bassline
 *  11    Hyper Glass           Crystalline, overtone-rich
 *  12    Cygnus Tine           Electric piano tine transient
 *  13    Vortex Clav           Clavinet-style twang
 *  14    Void Choir            Lush choir, evolving pad
 *  15    Reso Quark            Resonant character, hard sync-ish
 *  16    Photon Reed           Accordion/harmonium reed
 *  17    Warp Cello            Bowed cello, thick strings
 *  18    Nova Harm             Glass harmonica, airy overtones
 *  19    Event Growl           Distorted, aggressive, LFO fodder
 *  20    Solar Flute           Breathy flute, low noise-mix
 *  21    Plasma Pad            Evolving warm pad
 *  22    Moog Gravity          Analog bass, detuned sub
 *  23    Meteor Tabla          Tabla-style body — use in KICK/TOM
 *  24    Deep Drone            Sustained sub drone
 *
 * Access via: applyHarpParam(P_WAVEFORM, idx) or applySeqParam(P_WAVEFORM, idx)
 *             applyDrumWave(ch, idx) for drum body oscillators
 *
 * DRUM WAVEFORM ELIGIBILITY:
 *   YES (body oscillator): KICK(0), SNARE(1), TOM-H(5), TOM-L(6), PERC(7)
 *   NO  (noise/square):    CLAP(2), HAT-C(3), HAT-O(4) — waveform ignored
 *
 * INTERESTING DRUM COMBOS:
 *   KICK   + idx 22 (Moog Gravity)  → deep sub kick
 *   KICK   + idx 23 (Meteor Tabla)  → acoustic drum shell
 *   SNARE  + idx  7 (Aether String) → gated string snap
 *   SNARE  + idx  0 (Cosmic Saw)    → industrial noise snare
 *   TOM    + idx  6 (Chrono Bell)   → metallic pitched tom
 *   TOM    + idx 17 (Warp Cello)    → woody acoustic tom
 *   PERC   + idx 11 (Hyper Glass)   → crystalline hi-perc
 *   PERC   + idx 19 (Event Growl)   → ring-mod aggression
 * ═════════════════════════════════════════════════════════════════════════════ */

/* ═══════════════════════════════════════════════════════════════════════════
 * SECTION 10A — AUX FX BUS PARAMS (CMD 142–143, 64–67)
 *
 * masterAuxDlyFb / masterAuxRevDamp and the shared aux delay/reverb bus are
 * driven through applyAuxParam() + echoAuxParams() — hardware AUX FX menu,
 * App AUX_ENC, and midi.cpp RX all share this path (since v6.0 / WS11).
 * ═══════════════════════════════════════════════════════════════════════════ */

/* applyAuxParam — set delay feedback or reverb damp with v14 input + echo */
static inline void applyAuxParam(uint8_t cmd, uint16_t v14) {
  const float f = v14_to_norm(v14);
  switch (cmd) {
    case CMD_AUX_DLY_FB:
      /* [WS11-FIX2] Hardware encoder sends v14 in [0,16383] → scale to [0,0.95]
       * so that full-turn = max safe feedback, matching the App's AUX_ENC encoding.
       * Echo via echoAuxParams encoding (stored/0.95) keeps App display in sync. */
      masterAuxDlyFb.store(std::min(0.95f, f * 0.95f), std::memory_order_relaxed);
      txSysex(CMD_AUX_DLY_FB,
              (uint16_t)(std::min(1.0f, masterAuxDlyFb.load(std::memory_order_relaxed)
                                  / 0.95f) * 16383.0f));
      break;
    case CMD_AUX_REV_DMP:
      masterAuxRevDamp.store(f, std::memory_order_relaxed);
      txSysex(CMD_AUX_REV_DMP, v14);
      break;
    case CMD_H_FX_TIME:
    case CMD_S_FX_TIME:
      masterAuxDlyTime.store(f * 1.5f, std::memory_order_relaxed);
      txSysex(cmd, v14);
      break;
    case CMD_H_FX_SIZE:
    case CMD_S_FX_SIZE:
      masterAuxRevSize.store(std::min(0.95f, f), std::memory_order_relaxed);
      txSysex(cmd, v14);
      break;
    default: break;
  }
}

/* Encode aux params back to v14 for echo / sendFullStateSync */
static inline void echoAuxParams() {
  txSysex(CMD_H_FX_TIME, norm_to_v14(masterAuxDlyTime.load(std::memory_order_relaxed) / 1.5f));
  txSysex(CMD_H_FX_SIZE, norm_to_v14(masterAuxRevSize.load(std::memory_order_relaxed) / 0.95f));
  txSysex(CMD_AUX_DLY_FB, norm_to_v14(masterAuxDlyFb.load(std::memory_order_relaxed) / 0.95f));
  txSysex(CMD_AUX_REV_DMP, norm_to_v14(masterAuxRevDamp.load(std::memory_order_relaxed)));
}

/* ═══════════════════════════════════════════════════════════════════════════
 * SECTION 9B — CANONICAL MASTER / MIX / FX APPLY LAYER  [P4: single source]
 *
 * The v14 ↔ value scaling for every master/mix/FX scalar lives HERE ONLY.
 * App-RX (midi.cpp), and optionally the encoder side (interface.cpp), funnel
 * through these so a curve/range change is made in exactly one place and the two
 * paths can never silently drift.  Scalings are verbatim from the old midi.cpp
 * RX cases — behaviour is identical to before.
 *
 * origin decides the echo: the App already knows what it sent (no echo back, or
 * it would fight its own widget); hardware/MIDI edits echo so the App display
 * can never go stale.
 * ═══════════════════════════════════════════════════════════════════════════ */
enum class Origin : uint8_t { APP,
                              HW,
                              MIDI,
                              RECALL };

static inline void echoIfNotApp(uint8_t cmd, uint16_t v14, Origin o) {
  if (o != Origin::APP) txSysex(cmd, v14);
}

/* Master / mix / FX SCALAR params (atomics + dbeam cfg).  Returns silently for a
 * cmd it doesn't own, so the RX switch can still handle action-type commands.   */
static inline void applyMasterParam(uint8_t cmd, uint16_t v14, Origin o) {
  const float n = (float)v14 / 16383.0f;
  switch (cmd) {
    case CMD_M_VOL: masterVol.store(n); break;
    case CMD_H_VOL: mixHarpVol.store(n); break;
    case CMD_S_VOL: mixSeqVol.store(n); break;
    case CMD_D_VOL: mixDrumsVol.store(n); break;
    case CMD_H_PAN: mixHarpPan.store(n * 2.f - 1.f); break;
    case CMD_S_PAN: mixSeqPan.store(n * 2.f - 1.f); break;
    case CMD_D_PAN: mixDrumsPan.store(n * 2.f - 1.f); break;
    case CMD_EQ_L: masterEqLow.store(n * 24.f - 12.f); break;
    case CMD_EQ_H: masterEqHigh.store(n * 24.f - 12.f); break;
    case CMD_PITCH: masterPitch.store(decodeMasterPitch(v14)); break;
    case CMD_DRUM_PITCH: drumPitchMult.store(decodeMasterPitch(v14)); break;
    case CMD_D_REV: drumRevSend.store(n); break;
    case CMD_D_DLY: drumDlySend.store(n); break;
    case CMD_TB_DRV: tbDrive.store(n); break;
    case CMD_TB_TONE: tbTone.store(n); break;
    case CMD_TB_MIX: tbMix.store(n); break;
    case CMD_DJ_FQ: djFreq.store(n); break;
    case CMD_DJ_RES: djRes.store(n); break;
    case CMD_DJ_MIX: djMix.store(n); break;
    /* H_/S_ FX_TIME and FX_SIZE share the master aux delay/reverb atomics — the
     * App's AUX knobs send the H_ variants (113/114); accept both.            */
    case CMD_H_FX_TIME:
    case CMD_S_FX_TIME: masterAuxDlyTime.store(n * 1.5f); break;
    case CMD_H_FX_SIZE:
    case CMD_S_FX_SIZE: masterAuxRevSize.store(std::min(0.95f, n)); break;
    /* [WS11-FIX2] App encodes as v14 = (f/0.95)*16383; decode back with *0.95.
     * Previously min(0.95,n) stored n≈f/0.95 instead of f, drifting ~5% high. */
    case CMD_AUX_DLY_FB: masterAuxDlyFb.store(n * 0.95f); break;
    case CMD_AUX_REV_DMP: masterAuxRevDamp.store(n); break;
    case CMD_H_MUTE: mixHarpMute.store(v14 > 0u); break;
    case CMD_S_MUTE: mixSeqMute.store(v14 > 0u); break;
    case CMD_D_MUTE: mixDrumsMute.store(v14 > 0u); break;
    case CMD_HUE_BASE: laserBaseHue.store(n); break;
    /* [LASER-SHOW v2] HUE ADSR times are stored in SECONDS on a per-stage scale
     * (ATK 0..2s, DEC 0..3s, REL 0..4s) so the firmware value, the menu readout
     * and the App knob all agree.  SUS stays a 0..1 fraction (0..100%).         */
    case CMD_HUE_ATK: hueAttack.store(n * HUE_ATK_MAX_S); break;
    case CMD_HUE_DEC: hueDecay.store(n * HUE_DEC_MAX_S); break;
    case CMD_HUE_SUS: hueSustain.store(n); break;
    case CMD_HUE_REL: hueRelease.store(n * HUE_REL_MAX_S); break;
    case CMD_LSR_ANIM: {
      int m = (int)(n * 3.0f + 0.5f); if (m < 0) m = 0; if (m > 3) m = 3;
      laserShowAnim.store((LaserShowAnim)m);
      break;
    }
    case CMD_LSR_DRUMFLASH: laserDrumFlash.store(n); break;
    case CMD_DB_ENABLED: dbeamEnabled.store(v14 > 0u); break;
    /* [FIX-CURVE] Clamp to valid DBEAMCurve range (0-4). Old code stored
     * v14 & 0xFF which allowed invalid enum values 5-255 to corrupt the state. */
    case CMD_DB_CURVE: currentDbeamCurve.store((DBEAMCurve)(v14 % 5u)); break;
    case CMD_DB_OFFSET: dbeamHWCfg.offsetAdc = (int)v14; break;
    case CMD_DB_RANGE: dbeamHWCfg.rangeAdc = (int)v14; break;
    default: return; /* not ours → caller (RX switch) handles it, no echo */
  }
  echoIfNotApp(cmd, v14, o);
}

/* FX insert sends — patchMux-guarded (the audio task reads fx.*Insert under it). */
static inline void applyFxSend(uint8_t cmd, uint16_t v14, Origin o) {
  const float n = (float)v14 / 16383.0f;
  bool owned = true;
  portENTER_CRITICAL(&patchMux);
  switch (cmd) {
    case CMD_H_DLY_MIX: fx.harpInsert.dly_send = n; break;
    case CMD_H_REV_MIX: fx.harpInsert.rev_send = n; break;
    case CMD_S_DLY_MIX: fx.seqInsert.dly_send = n; break;
    case CMD_S_REV_MIX: fx.seqInsert.rev_send = n; break;
    case CMD_H_FX_MIX: fx.harpInsert.dly_send = fx.harpInsert.rev_send = n; break;
    case CMD_S_FX_MIX: fx.seqInsert.dly_send = fx.seqInsert.rev_send = n; break;
    default: owned = false; break;
  }
  portEXIT_CRITICAL(&patchMux);
  if (owned) echoIfNotApp(cmd, v14, o);
}

/* Drum insert-A live tweak (CMD 190–192) — independent of D.REV/D.DLY aux atomics. */
static inline void applyDrumInsertParam(uint8_t cmd, uint16_t v14, Origin o) {
  const float n = (float)v14 / 16383.0f;
  bool owned = false;   /* [FIX-ECHO] only echo if cmd was actually handled */
  portENTER_CRITICAL(&patchMux);
  switch (cmd) {
    case CMD_D_FX_WET: fx.drumInsert.fx.fx_mix = n;         owned = true; break;
    case CMD_D_FX_P1:  fx.drumInsert.fx.p1 = n * 30.f;     owned = true; break;
    case CMD_D_FX_P2:  fx.drumInsert.fx.p2 = n * 250.f;    owned = true; break;
    default: break;
  }
  portEXIT_CRITICAL(&patchMux);
  if (owned) echoIfNotApp(cmd, v14, o);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * SECTION 10B — SOUND BANK PRESET MANAGEMENT
 *
 * The system has three preset tiers:
 *
 *   SOUND_BANK[256][16]   PROGMEM factory bank — read-only, harp synth
 *   userBank[256][16]     RAM user bank — read/write, harp synth (CMD_H_PATCH)
 *   seqBank[256][16]      RAM seq bank  — read/write, seq synth  (CMD_S_PATCH)
 *
 * Factory presets use the EXACT SynthParam/livePatch[] layout (memcpy'd with no
 * remap by recallHarpPatch / loadFactory{Harp,Seq}Preset).  Canonical order:
 *   [0]  P_WAVEFORM   (0-24)
 *   [1]  P_ATTACK
 *   [2]  P_DECAY
 *   [3]  P_SUSTAIN
 *   [4]  P_RELEASE
 *   [5]  P_CUTOFF      (full range)
 *   [6]  P_RESONANCE
 *   [7]  P_NOISE
 *   [8]  P_DETUNE      (centre = 8192)
 *   [9]  P_LFO_RATE
 *   [10] P_LFO_DEPTH
 *   [11] P_LFO_ROUTE   (0-7)
 *   [12] P_OSC2_WAVE   (0-24)
 *   [13] P_ENV_CUT
 *   [14-15] spare
 * (NOTE: an earlier doc/macro revision wrongly placed CUTOFF at [7] / detune at
 *  [12]; both the PTCH macro in assets.h and this table are now aligned to the
 *  SynthParam enum in globals.h — keep all three in lock-step.)
 *
 * All 256 factory presets rotate through all 25 waveforms (confirmed in
 * assets.cpp — the SOUND_BANK is designed to demonstrate the full wavetable
 * palette, not just sines).
 * ═══════════════════════════════════════════════════════════════════════════ */

/* getPresetName — copy factory preset name from PROGMEM into caller's buffer */
static inline void getPresetName(int idx, char* out, size_t bufLen) {
  if (!out || bufLen == 0) return;
  if (idx < 0 || idx >= NUM_PATCHES) {
    out[0] = '\0';
    return;
  }
  const size_t copyLen = std::min(bufLen - 1u, (size_t)15u);
  memcpy_P(out, PRESET_NAMES[idx], copyLen);
  out[copyLen] = '\0';
}

/* seedFactoryBanks — copy the PROGMEM factory SOUND_BANK into BOTH the harp
 * userBank[] and the seq seqBank[] RAM arrays.  These banks are not persisted
 * in NVS, so without seeding they are all-zeros at boot and a user-bank recall
 * (recallHarpPatch / recallSeqPatch with the USER source, or a CMD_S_PATCH
 * recall which always reads seqBank) would load silence / a closed filter.
 * Call once during boot, before any patch recall.                            */
static inline void seedFactoryBanks() {
  portENTER_CRITICAL(&patchMux);
  for (int p = 0; p < NUM_PATCHES; ++p) {
    memcpy_P(userBank[p], &SOUND_BANK[p][0], PARAMS_PER_PRESET * sizeof(uint16_t));
    memcpy_P(seqBank[p], &SOUND_BANK[p][0], PARAMS_PER_PRESET * sizeof(uint16_t));
    sanitizePatch(userBank[p]); /* clamp each row once at seed → safe-by-construction */
    sanitizePatch(seqBank[p]);
  }
  portEXIT_CRITICAL(&patchMux);
}

/* loadFactoryPreset — unified factory-bank loader for HARP (isHarp=true) or SEQ
 * (false).  Loads one PROGMEM SOUND_BANK row into the engine's livePatch + user
 * bank, syncs all named atomics, and echoes all 16 params to the WebApp.
 *
 * vs the old split pair: deduplicated into one function, and the browse index is
 * clamped to NUM_FACTORY_PATCHES (not NUM_PATCHES) so it can never land on a
 * zero-filled/silent slot.  Memory ordering is UNCHANGED — livePatch + bank are
 * swapped under ONE patchMux window so the DSP sees an all-old or all-new patch
 * (never a half-applied mix), and the seq version bump stays inside that lock.  */
static inline void loadFactoryPreset(bool isHarp, int idx) {
  idx = std::max(0, std::min(NUM_FACTORY_PATCHES - 1, idx)); /* clamp to real presets */
  (isHarp ? harpPatchIndex : seqPatchIndex).store(idx, std::memory_order_relaxed);

  uint16_t factory[PARAMS_PER_PRESET];
  memcpy_P(factory, &SOUND_BANK[idx][0], PARAMS_PER_PRESET * sizeof(uint16_t));
  sanitizePatch(factory); /* clamp once → livePatch, bank, atomics, echo all consistent */

  /* Atomic swap of livePatch + user bank under ONE lock (DSP sees all-old/all-new). */
  portENTER_CRITICAL(&patchMux);
  if (isHarp) {
    memcpy(harpLivePatch, factory, PARAMS_PER_PRESET * sizeof(uint16_t));
    memcpy(userBank[idx], factory, PARAMS_PER_PRESET * sizeof(uint16_t));
  } else {
    memcpy(seqLivePatch, factory, PARAMS_PER_PRESET * sizeof(uint16_t));
    memcpy(seqBank[idx], factory, PARAMS_PER_PRESET * sizeof(uint16_t));
    /* Seq render gates on this version — bump it inside the lock with the patch. */
    uint8_t newVer = (seqLivePatchVersion.load(std::memory_order_relaxed) + 1u) & 0xFFu;
    seqLivePatchVersion.store(newVer, std::memory_order_release);
  }
  portEXIT_CRITICAL(&patchMux);

  /* Sync all 14 named atomics — display / UI read these. */
  if (isHarp) {
    harpWaveform.store((float)(factory[(int)SynthParam::P_WAVEFORM] % 25u), std::memory_order_relaxed);
    harpAttack.store(v14_to_norm(factory[(int)SynthParam::P_ATTACK]), std::memory_order_relaxed);
    harpDecay.store(v14_to_norm(factory[(int)SynthParam::P_DECAY]), std::memory_order_relaxed);
    harpSustain.store(v14_to_norm(factory[(int)SynthParam::P_SUSTAIN]), std::memory_order_relaxed);
    harpRelease.store(v14_to_norm(factory[(int)SynthParam::P_RELEASE]), std::memory_order_relaxed);
    harpCutoff.store(v14_to_norm(factory[(int)SynthParam::P_CUTOFF]), std::memory_order_relaxed);
    harpResonance.store(v14_to_norm(factory[(int)SynthParam::P_RESONANCE]), std::memory_order_relaxed);
    harpNoise.store(v14_to_norm(factory[(int)SynthParam::P_NOISE]), std::memory_order_relaxed);
    harpDetune.store((factory[(int)SynthParam::P_DETUNE] / 16383.0f) * 2.0f - 1.0f, std::memory_order_relaxed);
    harpLfoRate.store(v14_to_norm(factory[(int)SynthParam::P_LFO_RATE]), std::memory_order_relaxed);
    harpLfoDepth.store(v14_to_norm(factory[(int)SynthParam::P_LFO_DEPTH]), std::memory_order_relaxed);
    harpLfoRoute.store((int32_t)(factory[(int)SynthParam::P_LFO_ROUTE] & 7u), std::memory_order_relaxed);
    harpOsc2Wave.store((float)(factory[(int)SynthParam::P_OSC2_WAVE] % 25u), std::memory_order_relaxed);
    harpEnvCutAmount.store(v14_to_norm(factory[(int)SynthParam::P_ENV_CUT]), std::memory_order_relaxed);
  } else {
    seqWaveform.store((float)(factory[(int)SynthParam::P_WAVEFORM] % 25u), std::memory_order_relaxed);
    seqAttack.store(v14_to_norm(factory[(int)SynthParam::P_ATTACK]), std::memory_order_relaxed);
    seqDecay.store(v14_to_norm(factory[(int)SynthParam::P_DECAY]), std::memory_order_relaxed);
    seqSustain.store(v14_to_norm(factory[(int)SynthParam::P_SUSTAIN]), std::memory_order_relaxed);
    seqRelease.store(v14_to_norm(factory[(int)SynthParam::P_RELEASE]), std::memory_order_relaxed);
    seqCutoff.store(v14_to_norm(factory[(int)SynthParam::P_CUTOFF]), std::memory_order_relaxed);
    seqResonance.store(v14_to_norm(factory[(int)SynthParam::P_RESONANCE]), std::memory_order_relaxed);
    seqNoise.store(v14_to_norm(factory[(int)SynthParam::P_NOISE]), std::memory_order_relaxed);
    seqDetune.store((factory[(int)SynthParam::P_DETUNE] / 16383.0f) * 2.0f - 1.0f, std::memory_order_relaxed);
    seqLfoRate.store(v14_to_norm(factory[(int)SynthParam::P_LFO_RATE]), std::memory_order_relaxed);
    seqLfoDepth.store(v14_to_norm(factory[(int)SynthParam::P_LFO_DEPTH]), std::memory_order_relaxed);
    seqLfoRoute.store((int32_t)(factory[(int)SynthParam::P_LFO_ROUTE] & 7u), std::memory_order_relaxed);
    seqOsc2Wave.store((float)(factory[(int)SynthParam::P_OSC2_WAVE] % 25u), std::memory_order_relaxed);
    seqEnvCutAmount.store(v14_to_norm(factory[(int)SynthParam::P_ENV_CUT]), std::memory_order_relaxed);
  }

  std::atomic_thread_fence(std::memory_order_release);

  /* Echo all 16 params + patch-select so the WebApp display updates at once.
   * [WS4] LFO route uses base+11 (cmd 11/27), not legacy 86/87 — matches App. */
  const uint8_t baseCmd = isHarp ? CMD_H_WAVE : CMD_S_WAVE;
  const uint8_t selCmd = isHarp ? CMD_H_PATCH : CMD_S_PATCH;
  for (int i = 0; i < PARAMS_PER_PRESET; ++i)
    txSysex((uint8_t)(baseCmd + i), factory[i]);
  txSysex(selCmd, (uint16_t)idx);
  displayDirty.store(true, std::memory_order_relaxed);
}

/* Thin wrappers — every existing call site keeps compiling unchanged. */
static inline void loadFactoryHarpPreset(int idx) {
  loadFactoryPreset(true, idx);
}
static inline void loadFactorySeqPreset(int idx) {
  loadFactoryPreset(false, idx);
}

/* saveCurrentHarpToUserBank — write harpLivePatch into userBank[slot].
 * Non-destructive: does not change the active patch index or livePatch.     */
static inline void saveCurrentHarpToUserBank(int slot) {
  slot = std::max(0, std::min(NUM_PATCHES - 1, slot));
  portENTER_CRITICAL(&patchMux);
  memcpy(userBank[slot], harpLivePatch, PARAMS_PER_PRESET * sizeof(uint16_t));
  portEXIT_CRITICAL(&patchMux);
}

/* saveCurrentSeqToUserBank — write seqLivePatch into seqBank[slot].         */
static inline void saveCurrentSeqToUserBank(int slot) {
  slot = std::max(0, std::min(NUM_PATCHES - 1, slot));
  portENTER_CRITICAL(&patchMux);
  memcpy(seqBank[slot], seqLivePatch, PARAMS_PER_PRESET * sizeof(uint16_t));
  portEXIT_CRITICAL(&patchMux);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * SECTION 10C — DRUM VOICE WAVEFORM SELECTION
 *
 * The drum engine's body oscillators (KICK, SNARE, TOM-H, TOM-L, PERC) read
 * from WT_SINE by default.  Swapping to any of the 25 wavetables in
 * WAVE_TABLE_DIR gives dramatically different timbres (see catalog above).
 *
 * CLAP (ch2), HAT-C (ch3), HAT-O (ch4) use only noise and square oscillators
 * — they have no body_wave pointer and are unaffected by this parameter.
 *
 * CMD_DRUM_WAVE = 144
 * v14 encoding: bits 7–5 = drum channel (0–7), bits 4–0 = wave_idx (0–24)
 *   v14 = (ch << 5) | (wave_idx & 0x1F)
 * Max value: (7 << 5) | 24 = 248 — well within 14-bit range.
 *
 * REQUIRED GLOBALS.H ADDITIONS:
 *   In DrumVoice struct (after the filter_high field), add:
 *     const int16_t* body_wave = nullptr;
 *
 *   After the DrumVoice struct, add:
 *     inline std::atomic<uint8_t> drumWaveIdx[DRUM_POLYPHONY]{};
 *     // Index 0-7 per channel; 0 = DRUM_SINE_IDX (WT_SINE) = default
 *
 * REQUIRED DRUM_SYNTH.H CHANGES (two edits):
 *
 *   [1] In fire_tuned_drum, after computing kTypeMap[ch], add:
 *       const uint8_t widx = drumWaveIdx[ch].load(std::memory_order_relaxed);
 *       d->body_wave = WAVE_TABLE_DIR[widx % NUM_WAVE_TABLES].data;
 *
 *   [2] In drum_fill_buf, replace the shared SIN_TBL fetch:
 *       REMOVE:  const int16_t* const SIN_TBL = WAVE_TABLE_DIR[DRUM_SINE_IDX].data;
 *       Then in each case that uses SIN_TBL, replace with d->body_wave:
 *         DRUM_KICK:  drum_wave(d->body_wave, d->phase1)
 *         DRUM_SNARE: drum_wave(d->body_wave, d->phase1)
 *         DRUM_TOM:   drum_wave(d->body_wave, d->phase1)
 *         DRUM_PERC:  drum_wave(d->body_wave, d->phase1) × drum_wave(d->body_wave, d->phase2)
 *       CLAP and HAT cases are untouched (they don't use SIN_TBL).
 *
 * REQUIRED MIDI.H ADDITION (after CMD_AUX_REV_DMP = 143):
 *   static constexpr uint8_t CMD_DRUM_WAVE = 144;
 *
 * REQUIRED MIDI.CPP ADDITION (in handleSysexCommand extended switch):
 *   case CMD_DRUM_WAVE: {
 *     const uint8_t ch   = (uint8_t)((v14 >> 5) & 7u);
 *     const uint8_t widx = (uint8_t)(v14 & 31u) % NUM_WAVE_TABLES;
 *     drumWaveIdx[ch].store(widx, std::memory_order_relaxed);
 *   } break;
 * ═══════════════════════════════════════════════════════════════════════════ */

/* applyDrumWave — set waveform for one drum voice body oscillator + echo.
 * ch = 0-7, wave_idx = 0-24.
 * Requires drumWaveIdx[] in globals.h and body_wave field in DrumVoice.     */
static inline void applyDrumWave(uint8_t ch, uint8_t wave_idx) {
  if (ch >= 8u || wave_idx >= (uint8_t)NUM_WAVE_TABLES) return;
  /* CLAP and HATs don't use a body oscillator — silently ignore */
  if (ch == 2u || ch == 3u || ch == 4u) return;
  drumWaveIdx[ch].store(wave_idx, std::memory_order_relaxed);
  /* v14 = (ch << 5) | wave_idx — pack both into one sysex */
  txSysex(CMD_DRUM_WAVE, (uint16_t)((ch << 5u) | wave_idx));
}

/* applyDrumKit — select a drum kit: store the kit id (drives kick/hat character
 * in groovebox.cpp) and load its per-voice tuning preset into drumLivePatch + the
 * drum atomic mirrors.  When echo==true, mirror all 32 drum params + the kit id
 * back to the WebApp so its knobs follow the new tuning.
 *   kit = 0..DrumKitId::COUNT-1 (clamped).                                       */
static inline void applyDrumKit(uint8_t kit, bool echo) {
  if (kit >= (uint8_t)DrumKitId::COUNT) kit = 0;
  drumKit.store(kit, std::memory_order_release);

  /* [PATCH-PERF] One patchMux window for all 8 voices (was 8 separate locks). */
  portENTER_CRITICAL(&patchMux);
  for (int ch = 0; ch < 8; ++ch) {
    const float t = DRUM_KIT_TUNE[kit][ch];
    const float d = DRUM_KIT_DECAY[kit][ch];
    const float v = DRUM_KIT_VOL[kit][ch];
    const float n = DRUM_KIT_NOISE[kit][ch];
    drumLivePatch[ch * 4 + 0] = norm_to_v14(t);
    drumLivePatch[ch * 4 + 1] = norm_to_v14(d);
    drumLivePatch[ch * 4 + 2] = norm_to_v14(v);
    drumLivePatch[ch * 4 + 3] = norm_to_v14(n);
    drumTune[ch].store(t, std::memory_order_relaxed);
    drumDecay[ch].store(d, std::memory_order_relaxed);
    drumVolume[ch].store(v, std::memory_order_relaxed);
    drumNoiseMix[ch].store(n, std::memory_order_relaxed);
  }
  portEXIT_CRITICAL(&patchMux);

  if (echo) {
    uint16_t snap[32];
    portENTER_CRITICAL(&patchMux);
    memcpy(snap, drumLivePatch, sizeof(snap));
    portEXIT_CRITICAL(&patchMux);
    for (uint8_t i = 0; i < 32u; ++i)
      txSysex((uint8_t)(32u + i), snap[i]);
    txSysex(CMD_DRUM_KIT, (uint16_t)kit);
  }
}

/* echoAllDrumWaves — echo current drum wave selections to WebApp */
static inline void echoAllDrumWaves() {
  for (uint8_t ch = 0; ch < 8u; ++ch) {
    if (ch == 2u || ch == 3u || ch == 4u) continue; /* noise-only voices */
    const uint8_t widx = drumWaveIdx[ch].load(std::memory_order_relaxed);
    txSysex(CMD_DRUM_WAVE, (uint16_t)((ch << 5u) | widx));
  }
}


/* ═══════════════════════════════════════════════════════════════════════════
 * SECTION 11 — D-BEAM EXPRESSION MATRIX
 *
 * The D-BEAM is a laser proximity sensor that produces a continuous 0-1
 * amplitude value per string.  It routes through the DbeamRoute selector to
 * one of four DSP targets:
 *
 * [USB-ONLY] D-BEAM is a LOCAL laser-harp controller: it writes the harp DSP
 * atomics directly and emits NO external MIDI / App echo (DIN bus removed v6.0).
 *
 *   DbeamRoute::OFF        — sensor active, no output
 *   DbeamRoute::MODULATION — dbeam_mod_depth → harp LFO depth addend
 *   DbeamRoute::VOLUME     — mixHarpVol (0-1 direct)
 *   DbeamRoute::CUTOFF     — dbeam_svf_cutoff → harp SVF cutoff addend
 *
 * [USB-ONLY v6.0] D-BEAM is a LOCAL controller: only CMD_DB_ROUTE (145) is
 * routed from the App (selects OFF/MOD/VOL/CUT target).  The old per-mode MIDI
 * CC-number config (CMD_DB_CUT_CC/MOD_CC) was removed — D-BEAM emits no MIDI.
 *
 *   case CMD_DB_ROUTE:
 *     currentDbeamRoute.store((DbeamRoute)(v14 & 3u), std::memory_order_release); break;
 * ═══════════════════════════════════════════════════════════════════════════ */

/* ── applyDbeamRoute — set D-BEAM routing function + echo ──────────────────
 * [ROUTE-FIX] Clear ALL DSP addends (harp + seq, cutoff + mod) on EVERY route
 * change, not just on OFF.  Previously switching e.g. Cutoff → Mod left
 * dbeam_svf_cutoff frozen at its last value, so the filter stayed stuck-open.
 * routeDbeamExpression() repopulates the active addend on the next buffer.    */
static inline void applyDbeamRoute(uint8_t mode_v14) {
  const DbeamRoute mode = (DbeamRoute)(mode_v14 & 3u);
  const DbeamRoute prev = currentDbeamRoute.load(std::memory_order_relaxed);
  /* [DBEAM-VOL] Volume-pedal baseline housekeeping around the VOLUME route:
   *   • entering VOLUME → snapshot the current bus levels as the rest baseline
   *   • leaving  VOLUME → restore both buses to that baseline (a mid-dip switch
   *     must not strand the synth at a reduced volume).                         */
  if (mode == DbeamRoute::VOLUME && prev != DbeamRoute::VOLUME) {
    dbeamVolBaseHarp.store(mixHarpVol.load(std::memory_order_relaxed), std::memory_order_relaxed);
    dbeamVolBaseSeq .store(mixSeqVol .load(std::memory_order_relaxed), std::memory_order_relaxed);
  } else if (mode != DbeamRoute::VOLUME && prev == DbeamRoute::VOLUME) {
    mixHarpVol.store(dbeamVolBaseHarp.load(std::memory_order_relaxed), std::memory_order_release);
    mixSeqVol .store(dbeamVolBaseSeq .load(std::memory_order_relaxed), std::memory_order_release);
  }
  currentDbeamRoute.store(mode, std::memory_order_release);
  dbeam_svf_cutoff.store(0, std::memory_order_release);
  dbeam_mod_depth.store(0, std::memory_order_release);
  dbeam_seq_svf_cutoff.store(0, std::memory_order_release);
  dbeam_seq_mod_depth.store(0, std::memory_order_release);
  txSysex(CMD_DB_ROUTE, (uint16_t)mode);
  displayDirty.store(true, std::memory_order_relaxed);
}

/* ── applyDbeamTarget — choose which synth the D-BEAM expression drives ─────
 * [DBEAM-TGT] Harp vs Melody synth.  Clear all addends so the synth we are
 * switching AWAY from is not left stuck-modulated.  Echoes CMD_DB_TARGET (180). */
static inline void applyDbeamTarget(uint8_t target_v14) {
  /* [DBEAM-VOL] If a VOLUME pedal is active, return BOTH buses to their rest
   * baseline before switching synth so the engine we leave isn't stuck dipped;
   * routeDbeamExpression then re-adopts the baseline for the new target.        */
  if (currentDbeamRoute.load(std::memory_order_relaxed) == DbeamRoute::VOLUME) {
    mixHarpVol.store(dbeamVolBaseHarp.load(std::memory_order_relaxed), std::memory_order_release);
    mixSeqVol .store(dbeamVolBaseSeq .load(std::memory_order_relaxed), std::memory_order_release);
  }
  currentDbeamTarget.store((DbeamTarget)(target_v14 & 1u), std::memory_order_release);
  dbeam_svf_cutoff.store(0, std::memory_order_release);
  dbeam_mod_depth.store(0, std::memory_order_release);
  dbeam_seq_svf_cutoff.store(0, std::memory_order_release);
  dbeam_seq_mod_depth.store(0, std::memory_order_release);
  txSysex(CMD_DB_TARGET, (uint16_t)(target_v14 & 1u));
  displayDirty.store(true, std::memory_order_relaxed);
}

/* ── echoDbeamExprState — echo full D-BEAM expression configuration ─────── */
static inline void echoDbeamExprState() {
  txSysex(CMD_DB_ENABLED, dbeamEnabled.load(std::memory_order_relaxed) ? 16383u : 0u);
  txSysex(CMD_DB_CURVE, (uint16_t)currentDbeamCurve.load(std::memory_order_relaxed));
  txSysex(CMD_DB_OFFSET, (uint16_t)dbeamHWCfg.offsetAdc);
  txSysex(CMD_DB_RANGE, (uint16_t)dbeamHWCfg.rangeAdc);
  txSysex(CMD_DB_ROUTE, (uint16_t)currentDbeamRoute.load(std::memory_order_relaxed));
  txSysex(CMD_DB_TARGET, (uint16_t)currentDbeamTarget.load(std::memory_order_relaxed));
}



/* ═══════════════════════════════════════════════════════════════════════════
 * SECTION 10F — APP CONNECTIVITY + TRANSPORT ARBITRATION + SONG MODE  [v5.3.1]
 * ═══════════════════════════════════════════════════════════════════════════ */

/* isAppConnected — true if the app sent a sysex heartbeat within 4.5 seconds.
 * Used by display_refresh_task to switch to the "APP CONNECTED" page, and
 * by midi.cpp when sending non-transport echoes.
 * [FIX-HEARTBEAT] Window changed from 3000ms → 4500ms to match pollSyncHeartbeat().
 * Old value (3000ms) caused a 1.5-second window where the OLED still showed
 * "APP CONNECTED" but txSysex() was already returning early — hardware encoder
 * turns during that gap were silently lost with no App knob update.            */
static inline bool isAppConnected() {
  const uint32_t last = lastWebSysexMs.load(std::memory_order_relaxed);
  /* last==0 = no app sysex ever received → never "connected" at boot.
   * (Without this guard the first 4.5 s after power-on read as connected because
   *  millis()-0 < 4500.) */
  return (last != 0u) && ((millis() - last) < 4500u);
}

/* [v6.0] transport_available() was removed — the hardware ALWAYS owns transport
 * (play/stop/record/BPM); there is no App hand-off to gate against anymore.   */

/* echoSongState — echo all 16 song slots + current position to OctopusApp.
 * Called on CMD_APP_SYNC_REQ and after song mode state changes.            */
static inline void echoSongState() {
  /* echo song mode flag */
  txSysex(CMD_SONG_MODE, songModeActive.load(std::memory_order_relaxed) ? 16383u : 0u);
  txSysex(CMD_SONG_SLOT, (uint16_t)activeSongSlot.load(std::memory_order_relaxed));

  /* Echo all steps of the active song slot */
  const uint8_t slot = activeSongSlot.load(std::memory_order_relaxed) & 15u;
  txSysex(CMD_SONG_STEPS_N, (uint16_t)hwSongData[slot].numSteps);
  for (int i = 0; i < hwSongData[slot].numSteps; ++i) {
    const SongStep& st = hwSongData[slot].steps[i];
    /* Pack: [step:4][bank:4][chain:2][repeats:4] */
    const uint16_t v14 = (uint16_t)(((uint16_t)(i & 0xFu) << 10) | ((uint16_t)(st.bank & 0xFu) << 6) | ((uint16_t)(st.chain & 0x3u) << 4) | ((uint16_t)(st.repeats & 0xFu)));
    txSysex(CMD_SONG_STEP, v14);
  }
  /* [FIX-SONG-POS] Use same CMD_SONG_POS encoding as the ring (groovebox.cpp):
   *   v14 = (step & 0xF) << 4
   * Old echoSongState used (step<<8|repeat)>>1 which puts step in bits 7-11,
   * while the App decoder reads (v14>>4)&0xF (bits 4-7) and the ring sets bit
   * pattern step<<4.  Old encoding: step=1 → v14=128, App reads (128>>4)&0xF=8.
   * New encoding: step=1 → v14=16, App reads (16>>4)&0xF=1.  Repeat is ignored
   * by the App so it is not included.                                           */
  txSysex(CMD_SONG_POS,
          (uint16_t)((songCurrentStep.load(std::memory_order_relaxed) & 0xFu) << 4));
}

/* applySongStep — decode CMD_SONG_STEP v14 and update hwSongData.          */
static inline void applySongStep(uint16_t v14) {
  const uint8_t stepIdx = (v14 >> 10) & 0xFu;
  const uint8_t bank = (v14 >> 6) & 0xFu;
  const uint8_t chain = (v14 >> 4) & 0x3u;
  const uint8_t repeats = (v14)&0xFu;
  const uint8_t slot = activeSongSlot.load(std::memory_order_relaxed) & 15u;
  hwSongData[slot].steps[stepIdx] = { bank, chain, repeats, 0 };
}

#endif /* PATCHES_H */
