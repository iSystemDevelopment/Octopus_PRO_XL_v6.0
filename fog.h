/* ═════════════════════════════════════════════════════════════════════════════
 * Octopus PRO XL v6.0.00 — Laser Harp Groovebox
 * © 2026 DIODAC ELECTRONICS / iSystem. All Rights Reserved.
 *
 * PROPRIETARY AND CONFIDENTIAL. Unauthorized copying, distribution, modification,
 * or use of this software or firmware, in whole or in part, is strictly prohibited
 * without prior written permission from DIODAC ELECTRONICS.
 * ═════════════════════════════════════════════════════════════════════════════
 * fog.h — v6.0.00  FOG REJECTION MODULE  (self-contained, isolated branch)
 *
 * Purpose
 *   Reduce FALSE laser-harp triggers caused by haze/fog.  Fog scatter is
 *   COMMON-MODE: it raises the reflected-light amplitude on ALL strings roughly
 *   together.  A real hand is DIFFERENTIAL: it spikes ONE string far above the
 *   rest.  So we accept a beam-break as a real hand only when that string's
 *   amplitude exceeds the across-string floor (2nd-smallest = the fog level) by a
 *   user margin.  Uniform haze raises the floor too, so the differential stays
 *   constant and fog is rejected without re-tuning as the haze density drifts.
 *
 * ISOLATION (by request)
 *   This module runs on its OWN data branch.  The dbeam ADC task publishes a
 *   COPY of each string's Kalman amplitude here via fogPublishAmp(); nothing in
 *   here writes back to g_kalman_ac[], the D-BEAM expression, or the comparator
 *   threshold.  fogAccept() is purely advisory: the laser detection ANDs it with
 *   the hardware beam-break, so the module can only ever PREVENT a trigger,
 *   never create one — and when disabled it is a guaranteed no-op (returns true).
 *
 * Threading
 *   g_fogAmp[] is written by the dbeam ADC task (Core 0) and read by the laser
 *   task (Core 1).  Plain relaxed atomics: the value is advisory and a one-cycle
 *   stale read is harmless, so no lock is taken (zero added latency on either
 *   core, no contention with patchMux).
 *
 * Hot-path placement
 *   fogAccept() runs inside laser_sweep_task (IRAM_ATTR, Core 1) where the DMA
 *   controller can briefly disable the instruction cache.  These functions are
 *   tiny static-inline so they normally INLINE into that IRAM task body; they are
 *   also tagged IRAM_ATTR so any non-inlined emission still lives in IRAM.  The
 *   floor is computed with a branch-only one-pass scan (NO std::sort) so there is
 *   ZERO flash-resident library dependency to fault during a cache-off window.
 * ═══════════════════════════════════════════════════════════════════════════ */
#pragma once
#include <atomic>
#include "globals.h"   /* MAX_STRINGS, IRAM_ATTR (via <Arduino.h>) */

/* ── User settings (persisted in LaserSettings) ──────────────────────────── */
static constexpr int FOG_MARGIN_MIN = 0;
static constexpr int FOG_MARGIN_MAX = 3000;   /* mV-domain (same units as g_kalman_ac) */
inline std::atomic<bool> fogRejectEnabled{ false };                 /* default OFF → no-op */
inline std::atomic<int>  fogRejectMargin { 50 };                    /* differential margin */

/* ── Isolated amplitude branch (a copy of g_kalman_ac[].x per string) ─────── */
inline std::atomic<float> g_fogAmp[MAX_STRINGS];

/** Publish one string's amplitude into the fog branch.  Called by the dbeam ADC
 *  task right after its Kalman update; a single relaxed store, side-effect free. */
static inline void IRAM_ATTR fogPublishAmp(int si, float amp) {
  if ((unsigned)si < (unsigned)MAX_STRINGS)
    g_fogAmp[si].store(amp, std::memory_order_relaxed);
}

/** Common-mode fog floor = the 2nd-SMALLEST string amplitude.  Fog lifts every
 *  string together, so the quietest hand-free strings sit at the fog level.
 *  Using the 2nd-smallest (not the absolute min) ignores one anomalously-low /
 *  dead channel, and — unlike a median — it does NOT rise with polyphony, so a
 *  real chord (up to ~6 held strings) is never falsely rejected.  A low floor
 *  only makes the gate MORE permissive (fails toward letting notes through),
 *  which is the safe direction for a performance instrument.  Also used by the
 *  TELEMETRY → FOG REJECT view to draw the floor/accept reference lines.        */
static inline float IRAM_ATTR fogFloor() {
  /* One-pass smallest / 2nd-smallest (no std::sort → no flash-resident call). */
  float m1 = 1e30f;   /* smallest      */
  float m2 = 1e30f;   /* 2nd-smallest  */
  for (int i = 0; i < MAX_STRINGS; ++i) {
    const float v = g_fogAmp[i].load(std::memory_order_relaxed);
    if (v < m1)      { m2 = m1; m1 = v; }
    else if (v < m2) { m2 = v; }
  }
  return m2;
}

/** Accept string ci as a REAL hand (true) or reject it as fog (false).
 *  Returns true unconditionally when disabled → existing trigger logic intact.
 *  Differential test: amp[ci] ≥ fogFloor() + margin.                            */
static inline bool IRAM_ATTR fogAccept(int ci) {
  if (!fogRejectEnabled.load(std::memory_order_relaxed)) return true;
  const float self = g_fogAmp[ci & (MAX_STRINGS - 1)].load(std::memory_order_relaxed);
  return self >= fogFloor() + (float)fogRejectMargin.load(std::memory_order_relaxed);
}
