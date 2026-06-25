/* ═════════════════════════════════════════════════════════════════════════════
 * Octopus PRO XL v6.1.01 — Laser Harp Groovebox
 * © 2026 DIODAC ELECTRONICS / iSystem. All Rights Reserved.
 *
 * PROPRIETARY AND CONFIDENTIAL. Unauthorized copying, distribution, modification,
 * or use of this software or firmware, in whole or in part, is strictly prohibited
 * without prior written permission from DIODAC ELECTRONICS.
 * ═════════════════════════════════════════════════════════════════════════════
 * display.h — v6.1.01  OLED DECLARATIONS + MENU LABEL TABLES
 *
 * SH1106 128×64 layout constants, menu name arrays, telemetry views,
 * DRAW_* helpers.  Runtime drawing in display.cpp → renderUIState() only.
 * ═════════════════════════════════════════════════════════════════════════════ */
#pragma once
#ifndef DISPLAY_H
#define DISPLAY_H

#include <Arduino.h>
#include <Adafruit_SH110X.h>
extern Adafruit_SH1106G display;

#include <algorithm>
#include <cmath>
#include <cstring>
#include "globals.h"
#include "interface.h" /* kL1Count, l2CountFor, kL2*Count */

/* ═══════════════════════════════════════════════════════════════════════════
 * SECTION 1 — SCREEN GEOMETRY
 * ═══════════════════════════════════════════════════════════════════════════ */
static constexpr int SCREEN_W = 128;
static constexpr int SCREEN_H = 64;
static constexpr int CHAR_W = 6;
static constexpr int HEADER_Y = 0;
static constexpr int HEADER_H = 10;
static constexpr int DIVIDER_Y = HEADER_Y + HEADER_H;
static constexpr int LIST_START_Y = 12;
static constexpr int LIST_ITEM_H = 10;
static constexpr int LIST_MAX_ITEMS = 5;
static constexpr int DASH_PATCH_Y = 14;
static constexpr int DASH_PATCH_H = 15;
static constexpr int DASH_LINE2_Y = 31;
static constexpr int DASH_LINE3_Y = 42;
static constexpr int DASH_DBEAM_Y = 55;
static constexpr int LABEL_X = 4;

/* ═══════════════════════════════════════════════════════════════════════════
 * SECTION 2 — MENU LABEL TABLES
 *
 * Rule: every array is sized to exactly match the interface.h count constant.
 * safeL2Name() in display.cpp is the only accessor — never index directly.
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Play mode names (index matches PlayMode enum) */
inline const char* const kPMNames[3] = { "POLY8", "STRINGS", "SOLO" };

/* L1 menu category names */
inline const char* const kL1Names[16] = {
  "HARP SETUP", /*  0 kL2HarpSetupCount  = 13 */
  "D-BEAM",     /*  1 kL2DBeamCount      =  8 */
  "MASTER",     /*  2 kL2MasterCount     = 24 */
  "HARP SYNTH", /*  3 kL2SynthCount      = 25 */
  "MIDI I/O",   /*  4 kL2MidiCount       =  5 */
  "SEQ SETUP",  /*  5 kL2SeqSetupCount   = 13 */
  "SEQ MATRIX", /*  6 kL2SeqMatrixCount  =  1 */
  "AUX FX",     /*  7 kL2AuxFxCount      = 16 */
  "SEQ SYNTH",  /*  8 kL2SeqSynthCount   = 21 */
  "DRUM KIT",   /*  9 kL2DrumsCount      = 42 */
  "LASER SHOW", /* 10 kL2LaserShowCount  =  9 */
  "TELEMETRY",  /* 11 kL2TelemetryCount  =  7 */
  "RESET",      /* 12 kL2ResetCount      =  4 */
  "SONG",       /* 13 kL2SongCount       =  1 */
  "SAVE",       /* 14 kL2SaveCount       =  4 */
  "LOAD"        /* 15 kL2LoadCount       =  4 */
};

/* MAIN MENU in regrouped display order (slot → name). Sync with kL1Order[] in interface.h. */
inline const char* const kL1NamesOrdered[kL1Count] = {
  kL1Names[0],  /* HARP SETUP */
  kL1Names[3],  /* HARP SYNTH */
  kL1Names[5],  /* SEQ SETUP  */
  kL1Names[6],  /* SEQ MATRIX */
  kL1Names[8],  /* SEQ SYNTH  */
  kL1Names[13], /* SONG       */
  kL1Names[9],  /* DRUM KIT   */
  kL1Names[7],  /* AUX FX     */
  kL1Names[2],  /* MASTER     */
  kL1Names[1],  /* D-BEAM     */
  kL1Names[4],  /* MIDI I/O   */
  kL1Names[10], /* LASER SHOW */
  kL1Names[11], /* TELEMETRY  */
  kL1Names[12], /* RESET      */
  kL1Names[14], /* SAVE       */
  kL1Names[15]  /* LOAD       */
};

/* ── L2 label arrays — each is exactly [kL2<X>Count] entries ────────────── */

inline const char* const kL2HarpSetup[13] = {
  "Gate Hold", "White Lvl", "Touch On", "Touch Off",
  "Beam Red", "Beam Green", "Beam Blue", "Margin", "Stuck Rel", "Edge Comp",
  "Fog Reject", "Fog Margin", "Screensvr"
};

inline const char* const kL2DBeam[8] = {
  "Offset", "Range", "Curve", "Enable", "Env Atk", "Env Rel", "Route", "Target"
};

/* MASTER — 24 items (includes H/S/D pan at end) */
inline const char* const kL2Master[24] = {
  "M.Vol", "H.Vol", "S.Vol", "D.Vol",
  "M.Pitch", "FX Preset", "Drum Rev", "Drum Dly",
  "TB Drive", "TB Tone", "TB Mix",
  "DJ Freq", "DJ Res", "DJ Mix",
  "EQ Low", "EQ High",
  "Drm FX-A", "Drm FX-B",
  "Harp Mute", "Seq Mute", "Drm Mute",
  "H.Pan", "S.Pan", "D.Pan"
};

/* Synth params (HARP SYNTH and SEQ SYNTH share this table).
 * idx 18 "Snd Preset" — browse/recall the 128-name factory bank. */
inline const char* const kL2Synth[25] = {
  "Waveform", "Attack", "Decay", "Sustain", "Release",
  "Cutoff", "Resonance", "Noise", "Detune",
  "LFO Rate", "LFO Depth", "LFO Route",
  "Osc2 Wave", "Env>Cutoff",
  "FX-A Slot", "FX-B Slot",
  "Dly Send", "Rev Send",
  "Snd Preset",
  "Save Slot", "Load Slot",   /* idx 19/20: user sound bank */
  "H Arp On", "H Arp Pat", "H Arp Rate", "H Arp Gate"
};

inline const char* const kL2Midi[5] = {
  "PB Range", "PB Enable",
  "Harp Ch", "Seq Ch", "Drum Ch"
};

inline const char* const kL2SeqSetup[13] = {
  "Bank A-D", "View S/D", "Transpose",
  "Length", "Load Synth", "Load Drum", "Clear",
  "Save Pat", "Load Pat",
  "Arp On", "Arp Type", "Arp Rate", "Arp Gate"
};

inline const char* const kL2SeqMatrix[1] = { "Open Grid" };

/* AUX FX — shared room + sends + insert slots */
inline const char* const kL2AuxFx[16] = {
  "Room Time", "Room FB", "Room Size", "Room Dmp",
  "H.Dly Snd", "H.Rev Snd", "S.Dly Snd", "S.Rev Snd",
  "Harp FX-A", "Harp FX-B", "Seq FX-A", "Seq FX-B",
  "Drum FX-A", "Drum FX-B",
  "Room Scn", "Link Aux"
};

/* 16 shared-room scene names — AUX_SCENES[] in effect.cpp (1:1 index). */
inline const char* const kAuxSceneNames[16] = {
  "Dry Room", "Tight Plate", "Studio Booth", "Live Hall",
  "Cosmic Plate", "Tape Echo", "Slap Back", "Shimmer Hall",
  "Dark Cave", "Nebula Wash", "Pulse Chamber", "Ambient Bloom",
  "Drum Box", "Cathedral", "Lo-Fi Deck", "Void Infinite"
};

/* DRUM KIT — 5 params × 8 voices + kit selector (l2=40) + pitch (l2=41). */
inline const char* const kL2Drums[42] = {
  /* ch0 KICK  */ "Kick Tune", "Kick Dcay", "Kick Vol ", "Kick Nois", "Kick Wave",
  /* ch1 SNARE */ "Snr  Tune", "Snr  Dcay", "Snr  Vol ", "Snr  Nois", "Snr  Wave",
  /* ch2 CLAP  */ "Clap Tune", "Clap Dcay", "Clap Vol ", "Clap Nois", "Clap ----",
  /* ch3 HH-C  */ "HH-C Tune", "HH-C Dcay", "HH-C Vol ", "HH-C Nois", "HH-C ----",
  /* ch4 HH-O  */ "HH-O Tune", "HH-O Dcay", "HH-O Vol ", "HH-O Nois", "HH-O ----",
  /* ch5 TOM-H */ "TomH Tune", "TomH Dcay", "TomH Vol ", "TomH Nois", "TomH Wave",
  /* ch6 TOM-L */ "TomL Tune", "TomL Dcay", "TomL Vol ", "TomL Nois", "TomL Wave",
  /* ch7 PERC  */ "Perc Tune", "Perc Dcay", "Perc Vol ", "Perc Nois", "Perc Wave",
  /* 40        */ "Drum Kit ",
  /* 41        */ "Drm Pitch"
};

inline const char* const kL2LaserShow[9] = {
  "Show Mode", "MIDI Hue", "Base Hue", "Anim Mode", "Drum Flash",
  "Hue Attack", "Hue Decay", "Hue Sus", "Hue Rel"
};

/* Anim Mode display names (index = LaserShowAnim). */
inline const char* const kLaserAnimNames[4] = {
  "Pulse", "Chase", "Strobe", "Wave"
};

inline const char* const kL2Telemetry[7] = {
  "AC Scope", "DC Bias", "DAC Thresh", "D-BEAM Expr", "SNR", "System", "Fog Reject"
};

/* RESET — 4 scoped actions (YES/NO confirm). Order matches ResetScope. */
inline const char* const kL2Reset[4] = {
  "Full Reset", "Banks+Pats", "Motion Clr", "Settings"
};

/* SAVE — 4 scoped actions (YES/NO confirm → persist + reboot). */
inline const char* const kL2Save[4] = {
  "Full Save", "Banks+Pats", "Motion Save", "Settings"
};

/* LOAD — 4 scoped reloads from NVS (YES/NO confirm, no reboot). */
inline const char* const kL2Load[4] = {
  "Full Load", "Banks+Pats", "Motion Load", "Settings"
};

/* l2TableFor — maps L1 index → L2 string array (l2CountFor in interface.h). */
static inline const char* const* l2TableFor(int l1) {
  switch (l1) {
    case 0: return kL2HarpSetup;
    case 1: return kL2DBeam;
    case 2: return kL2Master;
    case 3: return kL2Synth;
    case 4: return kL2Midi;
    case 5: return kL2SeqSetup;
    case 6: return kL2SeqMatrix;
    case 7: return kL2AuxFx;
    case 8: return kL2Synth;
    case 9: return kL2Drums;
    case 10: return kL2LaserShow;
    case 11: return kL2Telemetry;
    case 12: return kL2Reset;
    case 14: return kL2Save;
    case 15: return kL2Load;
    default: return nullptr;
  }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * SECTION 3 — SOUND / FX NAME TABLES
 * ═══════════════════════════════════════════════════════════════════════════ */

/* 25 wavetable names — must match WAVE_TABLE_DIR[] order in assets.cpp */
inline const char* const kWaves[25] = {
  "Cosmic Saw", "Quantum Sq", "Pulsar 25%", "Stellar Tri",
  "Nebula Organ", "Astral Vocal", "Chrono Bell", "Aether String",
  "Singular Sine", "Pulsar 10%", "Pulsar 40%", "Hyper Glass",
  "Cygnus Tine", "Vortex Clav", "Void Choir", "Reso Quark",
  "Photon Reed", "Warp Cello", "Nova Harm", "Event Growl",
  "Solar Flute", "Plasma Pad", "Moog Gravity", "Meteor Tabla",
  "Deep Drone"
};
static constexpr int kWavesCount = 25;

/* 16 master FX preset names */
inline const char* const kMasterFxNames[16] = {
  "Aether Bypass", "Galactic Bus", "Magnetar Sat", "Pulsar Impact",
  "Sub-Zero Core", "Nebula Polish", "Horizon Limit", "LPF DeepSwp",
  "HPF Prism", "Spectral Scoop", "Cosmos Echo", "Nova OD",
  "Quantum Tube", "Lofi Singulr", "Interstellar", "Centauri Mstr"
};

/* 16 insert FX (slot A) preset names — abbreviated form of INSERT_FX_PRESETS[]
 * in effect.cpp.  Order MUST match that table and INSERT_FX_NAMES[] in
 * OctopusApp.html so index→DSP→label is identical on hw, device, and app. */
inline const char* const kInsertFxNames[16] = {
  "Nebula Taps",   "Snova Chorus", "Pulsar Mod",    "Quasar Phase",
  "Chronos Echo",  "Singul Tube",  "Jet Flange",    "Astral Shmr",
  "Dark SubRoom",  "Cosmos Tape",  "Hyper ResMod",  "Vortex Swirl",
  "Organic Drive", "Aether Gate",  "Void Satur",    "Zero Quantum"
};

/* 16 dynamics (insert slot B) preset names.
 * Order MUST match DYNAMICS_PRESETS[] in effect.cpp and DYN_NAMES[] in
 * OctopusApp.html so index→DSP mapping is identical on hw, device, and app. */
inline const char* const kDynNames[16] = {
  "Dyn Byp", "Glue Comp", "Punch Comp", "Soft Lim",
  "Brick Lim", "Noise Gate", "Tight Gate", "Trans Pun",
  "Snap Atk", "Drum Smk", "Bus Glue", "Vocal Ride",
  "Harp Sus", "Seq Pump", "Sub Gate", "Max Safe"
};

/* ── Semantic name tables (enums / wire values) ─────────────────────────── */

/* D-BEAM expression routing modes.
 * Order matches enum DbeamRoute: 0=OFF 1=MODULATION 2=VOLUME 3=CUTOFF.
 * Abbreviated table for tight dashboard overlays; Full table for the menu.      */
inline const char* const kDbeamRouteNames[4]     = { "OFF", "Mod", "Vol", "Cut" };
inline const char* const kDbeamRouteNamesFull[4] = { "OFF", "Modulation", "Volume", "Cutoff" };
/* D-BEAM target synth — order matches DbeamTarget (0=HARP 1=SEQ) */
inline const char* const kDbeamTargetNames[2] = { "Harp Synth", "Melody Synth" };

/* D-BEAM curve shapes */
inline const char* const kDBCurveNames[5] = {
  "Linear", "Inverted", "Exp", "Log", "Sigmoid"
};

/* LFO modulation route matrix (0–7) */
/* MUST match the route decode in harp.cpp / groovebox.cpp (lfo_pitch/filter/wave/tremolo):
 *   0 pitch · 1 filter · 2 wave · 3 pitch+filter · 4 filter+wave ·
 *   5 pitch+wave · 6 all three · 7 tremolo (amp). Must match harp/groovebox decode. */
inline const char* const kLfoRouteNames[8] = {
  "Pitch", "Filter", "Wave", "Ptch+Flt",
  "Flt+Wave", "Ptch+Wav", "All 3", "Tremolo"
};

/* ═══════════════════════════════════════════════════════════════════════════
 * SECTION 4 — SAFE ARRAY ACCESSORS
 *
 * All array-index lookups in display.cpp must use these.
 * Any out-of-range index returns a safe "---" or "?" fallback.
 * ═══════════════════════════════════════════════════════════════════════════ */
static inline const char* safeWaveName(int idx) {
  return kWaves[std::max(0, std::min(kWavesCount - 1, idx))];
}
static inline const char* safeFxName(int idx) {
  return kInsertFxNames[std::max(0, std::min(15, idx))];
}
static inline const char* safeDynName(int idx) {
  return kDynNames[std::max(0, std::min(15, idx))];
}
static inline const char* safeMasterFxName(int idx) {
  return kMasterFxNames[std::max(0, std::min(15, idx))];
}
static inline const char* safeAuxSceneName(int idx) {
  return kAuxSceneNames[std::max(0, std::min(15, idx))];
}
static inline const char* safeDbeamRouteName(int idx) {
  return kDbeamRouteNames[std::max(0, std::min(3, idx))];
}
static inline const char* safeDbeamTargetName(int idx) {
  return kDbeamTargetNames[std::max(0, std::min(1, idx))];
}
static inline const char* safeDbeamRouteNameFull(int idx) {
  return kDbeamRouteNamesFull[std::max(0, std::min(3, idx))];
}
static inline const char* safeDBCurveName(int idx) {
  return kDBCurveNames[std::max(0, std::min(4, idx))];
}
static inline const char* safeLfoRouteName(int idx) {
  return kLfoRouteNames[std::max(0, std::min(7, idx))];
}
static inline const char* safeL2Name(int l1, int l2) {
  if (l1 < 0 || l1 >= kL1Count) return "???";
  const char* const* tbl = l2TableFor(l1);
  const int cnt = l2CountFor(l1);
  if (!tbl || l2 < 0 || l2 >= cnt) return "???";
  return tbl[l2];
}
static inline const char* safeL1Name(int l1) {
  if (l1 < 0 || l1 >= kL1Count) return "MENU";
  return kL1Names[l1];
}

/* ═══════════════════════════════════════════════════════════════════════════
 * SECTION 5 — PRIMITIVE DRAW HELPERS
 * ═══════════════════════════════════════════════════════════════════════════ */

/* DRAW_BAR_GRAPH — filled rectangle showing a [0, maxV] value */
static inline void DRAW_BAR_GRAPH(int16_t x, int16_t y, int16_t w, int16_t h,
                                  float val, float maxV) {
  display.drawRect(x, y, w, h, SH110X_WHITE);
  int16_t fw = (int16_t)(std::min(1.f, std::max(0.f,
                                                val / (maxV + 1e-9f)))
                         * (float)(w - 2));
  if (fw > 0) display.fillRect(x + 1, y + 1, fw, h - 2, SH110X_WHITE);
}

/* DRAW_SLIDER_POT — Modern horizontal progress slider with knob */
static inline void DRAW_SLIDER_POT(int16_t x, int16_t y, int16_t w,
                                    float val, float minV, float maxV) {
  // Constrain percentage between 0.0 and 1.0 safely
  float pct = (val - minV) / (maxV - minV + 1e-6f);
  pct = std::max(0.0f, std::min(1.0f, pct));

  // Outer frame track (5px high)
  display.drawRect(x, y + 1, w, 5, SH110X_WHITE);
  
  // Calculate fill width and knob position (leaving 1px padding inside)
  int16_t max_fill_w = w - 2;
  int16_t fill_w = (int16_t)(pct * max_fill_w);
  
  // Draw the filled "progress" track
  if (fill_w > 0) {
    display.fillRect(x + 1, y + 2, fill_w, 3, SH110X_WHITE);
  }
  
  // Draw a distinct, taller 5px knob centered over the current value position
  int16_t knob_x = x + 1 + fill_w - 2;
  // Bound the knob so it doesn't spill past the slider's physical edges
  if (knob_x < x) knob_x = x;
  if (knob_x > x + w - 3) knob_x = x + w - 3;
  
  // Draw knob (clearing a tiny background slice behind it makes it pop, but a solid block is cleanest)
  display.fillRect(knob_x, y, 3, 7, SH110X_WHITE);
}

/* DRAW_SWITCH_TOGGLE — Modern pill-style toggle switch with label */
static inline void DRAW_SWITCH_TOGGLE(int16_t x, int16_t y,
                                      const char* label, bool state) {
  // Draw the label text
  display.setCursor(x, y);
  display.print(label);
  
  // Switch geometry configurations
  const int16_t sw_w = 22;
  const int16_t sw_h = 9;
  const int16_t sw_x = SCREEN_W - sw_w - 2; // 2px padding from right edge
  const int16_t sw_y = y - 1;                // Aligned with text baseline
  
  // Draw a sleek outer track frame
  display.drawRect(sw_x, sw_y, sw_w, sw_h, SH110X_WHITE);
  
  // Clear the inner area to ensure no artifacts
  display.fillRect(sw_x + 1, sw_y + 1, sw_w - 2, sw_h - 2, SH110X_BLACK);
  
  if (state) {
    // ON State: Fill the background track and leave a inverted hollow pocket, 
    // or just a solid clean block on the right. Let's do a solid modern block:
    int16_t knob_w = 8;
    display.fillRect(sw_x + sw_w - knob_w - 1, sw_y + 1, knob_w, sw_h - 2, SH110X_WHITE);
  } else {
    // OFF State: Solid block on the left side
    int16_t knob_w = 8;
    display.fillRect(sw_x + 1, sw_y + 1, knob_w, sw_h - 2, SH110X_WHITE);
  }
}

/* DRAW_STEP_BARGRAPH — full-width 16-cell page playhead.
 *   x,y,w   : origin + total width; pass (0, y, SCREEN_W) for the full 128 px bar
 *   absStep : absolute sequencer step 0..(seqLen-1) — up to 63
 *   seqLen  : pattern length (16/32/48/64); selects the visible 16-step page
 * The cell at (absStep % 16) is filled.  When seqLen > 16 a thin 4-segment page
 * strip is drawn 3 px above the bar with the current page lit, so the hardware
 * playhead tracks the App's P1-P4 paging instead of silently wrapping every 16
 * steps (the old `& 15` made >16-step patterns look out of sync). [v6.0] */
static inline void DRAW_STEP_BARGRAPH(int16_t x, int16_t y, int16_t w,
                                      uint16_t absStep, uint16_t seqLen) {
  if (seqLen < 1u) seqLen = 1u;
  const uint8_t cell   = (uint8_t)(absStep % 16u);
  const int16_t stride = (int16_t)(w / 16);                 /* 128/16 = 8 px    */
  const int16_t cw     = (int16_t)std::max<int>(2, (int)stride - 1);
  if (seqLen > 16u) {
    const uint8_t pages = (uint8_t)((seqLen + 15u) / 16u);  /* 2..4 pages       */
    const uint8_t page  = (uint8_t)std::min<uint16_t>((uint16_t)(pages - 1u),
                                                      (uint16_t)(absStep / 16u));
    const int16_t pw    = (int16_t)(w / pages);
    for (uint8_t p = 0; p < pages; ++p) {
      const int16_t px = (int16_t)(x + p * pw);
      display.drawRect(px, y - 3, pw - 1, 2, SH110X_WHITE);
      if (p == page) display.fillRect(px, y - 3, pw - 1, 2, SH110X_WHITE);
    }
  }
  for (uint8_t i = 0; i < 16; ++i) {
    const int16_t cx = (int16_t)(x + i * stride);
    display.drawRect(cx, y, cw, 6, SH110X_WHITE);
    if (i == cell) display.fillRect(cx + 1, y + 1, cw - 2, 4, SH110X_WHITE);
  }
}

/* DRAW_MINIBAR — compact percent bar for dashboard mute/activity status */
static inline void DRAW_MINIBAR(int16_t x, int16_t y, int16_t w, bool active) {
  display.drawRect(x, y, w, 6, SH110X_WHITE);
  if (active) display.fillRect(x + 1, y + 1, w - 2, 4, SH110X_WHITE);
}

/* DRAW_WARNING_TIMEOUT — inverted box for alerts (factory reset, errors) */
static inline void DRAW_WARNING_TIMEOUT(int16_t x, int16_t y, const char* msg) {
  display.fillRect(x, y, SCREEN_W - (x * 2), 24, SH110X_WHITE);
  display.setTextColor(SH110X_BLACK);
  display.setCursor(x + 6, y + 4);
  display.print(F("! EMERGENCY"));
  display.setCursor(x + 6, y + 14);
  display.print(msg);
  display.setTextColor(SH110X_WHITE);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * SECTION 6 — SCROLLING TEXT
 *
 * Draws `text` starting at (x, y) within `maxW` pixels.
 * If the text fits, it is drawn statically.
 * If the text is too long, it scrolls left automatically, driven by `now`
 * (millis()).  The scroll state is global — only one text scrolls at a time,
 * which is acceptable since the OLED shows one L3 edit screen at a time.
 * ═══════════════════════════════════════════════════════════════════════════ */
/* Per-call-site scroll state (key 0 = label, 1 = value on L3 screen). */
struct ScrollState {
  uint32_t hash   = 0;
  int      offset = 0;
  uint32_t t      = 0;
};
static inline void drawScrollingText(int16_t x, int16_t y, int16_t maxW,
                                     const char* text, uint32_t now,
                                     int stateIdx = 0) {
  if (!text || text[0] == '\0') return;
  const int len = (int)strlen(text);
  const int tw = len * CHAR_W;
  if (tw <= maxW) {
    display.setCursor(x, y);
    display.print(text);
    return;
  }
  /* Scrolling needed — move left by one char every 120 ms, pause at end */
  static ScrollState s_states[2];
  ScrollState& ss = s_states[stateIdx & 1];
  /* Simple djb2-style hash to detect content change */
  uint32_t h = 5381u;
  for (const char* p = text; *p; ++p) h = h * 33u ^ (uint8_t)*p;
  if (h != ss.hash) {
    ss.hash   = h;
    ss.offset = 0;
    ss.t      = now;
  }
  /* Advance one char every 120 ms; pause 1 s at each end */
  const int maxOffset = len - maxW / CHAR_W;
  if (now - ss.t > 120u) {
    ss.t = now;
    ++ss.offset;
    if (ss.offset > maxOffset + 8) ss.offset = -8; /* negative = pause at start */
    if (ss.offset < 0) ss.offset = 0;
  }
  const int off = std::max(0, std::min(maxOffset, ss.offset));
  display.setCursor(x - off * CHAR_W, y);
  display.print(text); /* Adafruit_GFX clips at screen edges naturally */
}

/* ═══════════════════════════════════════════════════════════════════════════
 * SECTION 7 — TELEMETRY ENCODER HANDLER
 * ═══════════════════════════════════════════════════════════════════════════ */
inline void handleTelemetryPageEncoder(int16_t delta) {
  if (delta == 0) return;
  int v = (int)currentScopeView.load(std::memory_order_relaxed);
  v += (delta > 0) ? 1 : -1;
  if (v < 1) v = (int)TelemetryView::FOG_REJECT;
  if (v > (int)TelemetryView::FOG_REJECT) v = 1;
  currentScopeView.store((TelemetryView)v, std::memory_order_relaxed);
  displayDirty.store(true, std::memory_order_relaxed);
}

/* ── Single external entry point ────────────────────────────────────────── */
void renderUIState();
void drawAppConnectedPage(); /* App-connected splash */
bool renderStepBarRegionIfVisible(); /* partial step-bar redraw (display.cpp) */
bool viewIsSeqMatrix();              /* SEQ MATRIX grid visible? (display.cpp) */
void displayFlushDiff();             /* page-diff I2C flush (display.cpp) */
void drawSaveToastIfActive();        /* SAVED / SAVE FAIL pill overlay (display.cpp) */

#endif /* DISPLAY_H */
