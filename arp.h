/* ═════════════════════════════════════════════════════════════════════════════
 * Octopus PRO XL v6.0.00 — Laser Harp Groovebox
 * © 2026 DIODAC ELECTRONICS / iSystem. All Rights Reserved.
 *
 * PROPRIETARY AND CONFIDENTIAL. Unauthorized copying, distribution, modification,
 * or use of this software or firmware, in whole or in part, is strictly prohibited
 * without prior written permission from DIODAC ELECTRONICS.
 * ═════════════════════════════════════════════════════════════════════════════
 * arp.h — v6.0.01  SHARED ARPEGGIATOR ENGINE  (seq + harp adapters)
 *
 * v6.0.01: pitch_rows[] paired with sorted notes; DnUp ping-pong fix; AsIs latch rows.
 * ═════════════════════════════════════════════════════════════════════════════ */
#pragma once
#ifndef ARP_H
#define ARP_H

#include <algorithm>
#include <cstdint>
#include <cstring>

namespace arp {

constexpr int MAX_NOTES = 8;

/* Seq melody voice used for arp output (row 7 voice index). */
constexpr int SEQ_VOICE = 7;

enum Pattern : uint8_t {
  UP = 0,
  DOWN,
  UP_DOWN,
  DOWN_UP,
  RANDOM,
  AS_PLAYED,
  UP_OCT,
  DOWN_OCT,
  PATTERN_COUNT
};

enum Rate : uint8_t {
  R_1_1 = 0,
  R_1_2,
  R_1_4,
  R_1_8,
  R_1_8T,
  R_1_16,
  R_1_16T,
  R_1_32,
  RATE_COUNT
};

/* MIDI ticks per arp period (TICKS_PER_BEAT = 480). */
constexpr uint32_t RATE_TICKS[RATE_COUNT] = {
  1920u, 960u, 480u, 240u, 160u, 120u, 80u, 60u
};

inline const char* patternName(uint8_t p) {
  static const char* k[] = {
    "Up", "Down", "UpDn", "DnUp", "Rnd", "AsIs", "Up+1", "Dn-1"
  };
  return (p < PATTERN_COUNT) ? k[p] : "?";
}

inline const char* rateName(uint8_t r) {
  static const char* k[] = {
    "1/1", "1/2", "1/4", "1/8", "1/8T", "1/16", "1/16T", "1/32"
  };
  return (r < RATE_COUNT) ? k[r] : "?";
}

/* Harp arp UI maps 0–3 → engine indices (simplified vs seq). */
inline const char* harpPatternName(uint8_t p) {
  static const char* k[] = { "Up", "Down", "UpDn", "Rnd" };
  return (p < 4u) ? k[p] : "?";
}
inline const char* harpRateName(uint8_t r) {
  static const char* k[] = { "1/8", "1/8T", "1/16", "1/16T" };
  return (r < 4u) ? k[r] : "?";
}
inline uint8_t harpPatternIndex(uint8_t ui) {
  static const uint8_t map[4] = { UP, DOWN, UP_DOWN, RANDOM };
  return map[std::min<uint8_t>(ui, 3u)];
}
inline uint8_t harpRateIndex(uint8_t ui) {
  static const uint8_t map[4] = { R_1_8, R_1_8T, R_1_16, R_1_16T };
  return map[std::min<uint8_t>(ui, 3u)];
}
inline uint8_t harpGateIndex(uint8_t ui) {
  static const uint8_t map[4] = { 0, 2, 4, 6 };
  return map[std::min<uint8_t>(ui, 3u)];
}

/* Gate steps: duty cycle of each arp period (100% … 6%). */
constexpr uint8_t GATE_PCT[8] = { 100, 75, 50, 38, 25, 19, 13, 6 };

struct Engine {
  int8_t  notes[MAX_NOTES];      /* sorted ascending for Up/Down patterns   */
  int8_t  pitch_rows[MAX_NOTES]; /* grid row per sorted notes[i]            */
  int8_t  raw[MAX_NOTES];        /* latch order for As-Played               */
  int8_t  rows[MAX_NOTES];       /* source grid row per latched note        */
  int8_t  count = 0;
  int8_t  step_idx = 0;
  uint32_t last_period = UINT32_MAX;
  uint32_t gate_off_tick = 0;
  bool     gate_open = false;
  int8_t   sounding_midi = -1;
  int8_t   sounding_row = -1;
  int8_t   last_play_idx = 0;
  int8_t   last_play_row = 0;
  uint32_t rng = 0xA5A5A5A5u;

  void reset() {
    count = 0;
    step_idx = 0;
    last_period = UINT32_MAX;
    gate_off_tick = 0;
    gate_open = false;
    sounding_midi = -1;
    sounding_row = -1;
  }

  void latchNotes(const int8_t* midi, const int8_t* src_rows, int n) {
    count = (int8_t)std::min(n, MAX_NOTES);
    for (int i = 0; i < count; ++i) {
      raw[i]        = midi[i];
      rows[i]       = src_rows[i];
      notes[i]      = midi[i];
      pitch_rows[i] = src_rows[i];
    }
    /* Sort pitch + row together so Up/Down/UpDn laser rows match the note heard. */
    for (int i = 0; i + 1 < count; ++i) {
      for (int j = i + 1; j < count; ++j) {
        if (notes[j] < notes[i]) {
          const int8_t tn = notes[i];      notes[i]      = notes[j];      notes[j]      = tn;
          const int8_t tr = pitch_rows[i]; pitch_rows[i] = pitch_rows[j]; pitch_rows[j] = tr;
        }
      }
    }
    step_idx = 0;
    last_period = UINT32_MAX;
    gate_open = false;
    sounding_midi = -1;
    sounding_row = -1;
  }

  int8_t noteAtPatternIndex(uint8_t pattern, int idx) const {
    if (count <= 0) return -1;
    idx = ((idx % count) + count) % count;
    int8_t n = notes[idx];
    switch (pattern) {
      case AS_PLAYED: n = raw[idx]; break;
      case UP_OCT:    n = (int8_t)std::min(127, (int)notes[idx] + 12); break;
      case DOWN_OCT:  n = (int8_t)std::max(0,   (int)notes[idx] - 12); break;
      default: break;
    }
    return n;
  }

  /* Row for pitch-order patterns (Up/Down/UpDn/Rnd/Oct). */
  int8_t rowForPitchIndex(int idx) const {
    if (count <= 0) return 0;
    idx = ((idx % count) + count) % count;
    return pitch_rows[idx];
  }

  /* Row for As-Played (latch order, not sorted). */
  int8_t rowForLatchIndex(int idx) const {
    if (count <= 0) return 0;
    idx = ((idx % count) + count) % count;
    return rows[idx];
  }

  /* Ping-pong index: UpDown = 0..n-1..1, DownUp = n-1..0..1 (cycle 2n-2). */
  static int pingPongIndex(int count, int step, bool down_first) {
    if (count <= 1) return 0;
    const int cycle = 2 * count - 2;
    int pos = ((step % cycle) + cycle) % cycle;
    if (down_first) {
      if (pos < count) return (count - 1) - pos;
      return pos - (count - 1);
    }
    if (pos < count) return pos;
    return (2 * count - 2) - pos;
  }

  /* Returns MIDI note for this arp period; advances internal pattern index. */
  int8_t nextNote(uint8_t pattern) {
    if (count <= 0) return -1;

    int idx = 0;
    switch (pattern) {
    case UP:
    case UP_OCT:
      idx = step_idx % count;
      step_idx = (int8_t)((step_idx + 1) % count);
      last_play_idx = (int8_t)idx;
      last_play_row = rowForPitchIndex(idx);
      return noteAtPatternIndex(pattern, idx);
    case DOWN:
    case DOWN_OCT:
      idx = (count - 1) - (step_idx % count);
      step_idx = (int8_t)((step_idx + 1) % count);
      last_play_idx = (int8_t)idx;
      last_play_row = rowForPitchIndex(idx);
      return noteAtPatternIndex(pattern, idx);
    case UP_DOWN:
    case DOWN_UP: {
      idx = pingPongIndex(count, step_idx, pattern == DOWN_UP);
      if (count <= 1) step_idx = 0;
      else {
        const int cycle = 2 * count - 2;
        step_idx = (int8_t)((step_idx + 1) % cycle);
      }
      last_play_idx = (int8_t)idx;
      last_play_row = rowForPitchIndex(idx);
      return noteAtPatternIndex(pattern, idx);
    }
    case RANDOM: {
      rng = rng * 1664525u + 1013904223u;
      idx = (int)(rng % (uint32_t)count);
      last_play_idx = (int8_t)idx;
      last_play_row = rowForPitchIndex(idx);
      return noteAtPatternIndex(pattern, idx);
    }
    case AS_PLAYED:
      idx = step_idx % count;
      step_idx = (int8_t)((step_idx + 1) % count);
      last_play_idx = (int8_t)idx;
      last_play_row = rowForLatchIndex(idx);
      return noteAtPatternIndex(AS_PLAYED, idx);
    default:
      idx = step_idx % count;
      step_idx = (int8_t)((step_idx + 1) % count);
      last_play_idx = (int8_t)idx;
      last_play_row = rowForPitchIndex(idx);
      return noteAtPatternIndex(pattern, idx);
    }
  }

  int8_t rowForMidi(int8_t midi) const {
    for (int i = 0; i < count; ++i)
      if (raw[i] == midi) return rows[i];
    for (int i = 0; i < count; ++i)
      if (notes[i] == midi) return pitch_rows[i];
    return count > 0 ? rows[0] : 0;
  }
};

} /* namespace arp */

#endif /* ARP_H */
