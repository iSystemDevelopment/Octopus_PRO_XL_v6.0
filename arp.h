/* ═════════════════════════════════════════════════════════════════════════════
 * Octopus PRO XL v6.0.00 — Laser Harp Groovebox
 * © 2026 DIODAC ELECTRONICS / iSystem. All Rights Reserved.
 *
 * PROPRIETARY AND CONFIDENTIAL. Unauthorized copying, distribution, modification,
 * or use of this software or firmware, in whole or in part, is strictly prohibited
 * without prior written permission from DIODAC ELECTRONICS.
 * ═════════════════════════════════════════════════════════════════════════════
 * arp.h — v6.0.00  SHARED ARPEGGIATOR ENGINE  (seq + harp adapters)
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
  int8_t  raw[MAX_NOTES];        /* latch order for As-Played               */
  int8_t  rows[MAX_NOTES];       /* source grid row per SORTED note (notes[i]) */
  /* [FIX-ASPLAYED-ROW] Source grid row per LATCH-order note (raw[i]).  rows[]
   * is permuted to follow the sorted notes[], so it is the WRONG row for the
   * latch-order raw[] used by AS_PLAYED.  Keeping the unsorted rows here lets
   * AS_PLAYED report the beam that actually played each note. */
  int8_t  raw_rows[MAX_NOTES];   /* source grid row per latch-order note     */
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
  /* [FIX-ARP-RACE] reset_pending decouples cross-core reset requests from
   * the audio task's read of non-atomic Engine fields.  seq_stop() and
   * harpArpCommitLatch(n=0) run on Core 1 (MIDI task); the Engine fields
   * (count, gate_open, step_idx…) are non-atomic and read by the audio task
   * on Core 0.  Instead of writing them directly from Core 1 (data race),
   * the caller only sets this flag (release) and the audio task applies the
   * reset at the next safe point (acquire) — exclusively on its own core.  */
  std::atomic<bool> reset_pending{ false };

  /* reset_safe() — call ONLY from the audio task (Core 0). */
  void reset_safe() {
    count = 0;
    step_idx = 0;
    last_period = UINT32_MAX;
    gate_off_tick = 0;
    gate_open = false;
    sounding_midi = -1;
    sounding_row = -1;
    last_play_idx = 0;
    last_play_row = 0;
    reset_pending.store(false, std::memory_order_release);
  }

  /* reset() — for use ONLY within the audio task (boot, same-core stop).
   * Cross-core callers must use request_reset() instead.                  */
  void reset() {
    count = 0;
    step_idx = 0;
    last_period = UINT32_MAX;
    gate_off_tick = 0;
    gate_open = false;
    sounding_midi = -1;
    sounding_row = -1;
    last_play_idx = 0;
    last_play_row = 0;
    reset_pending.store(false, std::memory_order_relaxed);
  }

  /* request_reset() — safe to call from ANY core/task.  The audio task
   * will apply the reset on its next buffer entry via check_reset().      */
  void request_reset() {
    reset_pending.store(true, std::memory_order_release);
  }

  /* check_reset() — call at the START of each audio buffer tick function
   * before touching any other Engine fields.                               */
  bool check_reset() {
    if (!reset_pending.load(std::memory_order_acquire)) return false;
    reset_safe();
    return true;
  }

  void latchNotes(const int8_t* midi, const int8_t* src_rows, int n) {
    count = (int8_t)std::min(n, MAX_NOTES);
    for (int i = 0; i < count; ++i) {
      raw[i]      = midi[i];
      rows[i]     = src_rows[i];
      raw_rows[i] = src_rows[i];   /* [FIX-ASPLAYED-ROW] latch-order copy, never sorted */
      notes[i]    = midi[i];
    }
    /* [FIX-2] Sort notes[] ascending AND keep rows[] in sync — rows[i] must
     * always be the source beam for notes[i] after sorting.  Without this
     * swap, rowForIndex(idx) returns the row for a different note (the one
     * that was originally latched at that position), causing wrong beam
     * lighting and hue on all sorted patterns (UP/DOWN/UP_DOWN/DOWN_UP/
     * UP_OCT/DOWN_OCT).  raw[] is deliberately NOT sorted — it keeps the
     * latch order for AS_PLAYED.                                           */
    for (int i = 0; i + 1 < count; ++i) {
      for (int j = i + 1; j < count; ++j) {
        if (notes[j] < notes[i]) {
          const int8_t t  = notes[i]; notes[i] = notes[j]; notes[j] = t;
          const int8_t tr = rows[i];  rows[i]  = rows[j];  rows[j]  = tr;
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

  int8_t rowForIndex(int idx) const {
    if (count <= 0) return 0;
    idx = ((idx % count) + count) % count;
    return rows[idx];
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
      break;
    case DOWN:
    case DOWN_OCT:
      idx = (count - 1) - (step_idx % count);
      step_idx = (int8_t)((step_idx + 1) % count);
      break;
    case UP_DOWN:
    case DOWN_UP: {
      if (count == 1) { idx = 0; break; }
      const int cycle = 2 * count - 2;
      const int pos = step_idx % cycle;
      if (pattern == UP_DOWN) {
        /* [FIX-1] UP_DOWN: ascend 0→n-1 then descend n-2→1 (bottom→top→bottom).
         * Example count=3: 0,1,2,1,0,1,2,1... (C,E,G,E,C,E,G,E...)           */
        if (pos < count) idx = pos;
        else             idx = (2 * count - 2) - pos;
      } else {
        /* [FIX-1] DOWN_UP: descend n-1→0 then ascend 1→n-2 (top→bottom→top).
         * Example count=3: 2,1,0,1,2,1,0,1... (G,E,C,E,G,E,C,E...)
         * Previous code: (cycle-pos)%cycle produced the SAME sequence as UP_DOWN
         * because the formula maps pos=0→0, pos=1→3, pos=2→2, pos=3→1 which
         * after the existing idx formula gives identical notes. Fixed formula:
         * mirror the position within the sorted array so step 0 = highest note. */
        if (pos < count) idx = (count - 1) - pos;
        else             idx = pos - (count - 1);
      }
      step_idx = (int8_t)((step_idx + 1) % cycle);
      break;
    }
    case RANDOM: {
      rng = rng * 1664525u + 1013904223u;
      /* [FIX-RANDOM] Defensive: guard against count==0 reaching the modulo even
       * if the caller's early-exit is bypassed by a compiler edge case.         */
      idx = (int)(rng % (uint32_t)std::max(1, (int)count));
      break;
    }
    case AS_PLAYED:
      idx = step_idx % count;
      step_idx = (int8_t)((step_idx + 1) % count);
      last_play_idx = (int8_t)idx;
      /* [FIX-ASPLAYED-ROW] AS_PLAYED returns raw[idx] (latch order), so its row
       * must come from the latch-order raw_rows[], NOT the sorted rows[]. */
      last_play_row = raw_rows[idx];
      return noteAtPatternIndex(AS_PLAYED, idx);
    default:
      idx = step_idx % count;
      step_idx = (int8_t)((step_idx + 1) % count);
      break;
    }
    last_play_idx = (int8_t)idx;
    last_play_row = rowForIndex(idx);
    return noteAtPatternIndex(pattern, idx);
  }

  /* Look up the source beam of a latched MIDI note.  Searches raw[] (latch
   * order), so it must return the latch-order raw_rows[i] — not the sorted
   * rows[i], which belongs to notes[i].  [FIX-ASPLAYED-ROW] */
  int8_t rowForMidi(int8_t midi) const {
    if (count <= 0) return 0;
    for (int i = 0; i < count; ++i)
      if (raw[i] == midi) return raw_rows[i];
    return raw_rows[0];
  }
};

} /* namespace arp */

#endif /* ARP_H */
