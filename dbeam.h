/* ═════════════════════════════════════════════════════════════════════════════
 * Octopus PRO XL v6.0.00 — Laser Harp Groovebox
 * © 2026 DIODAC ELECTRONICS / iSystem. All Rights Reserved.
 *
 * PROPRIETARY AND CONFIDENTIAL. Unauthorized copying, distribution, modification,
 * or use of this software or firmware, in whole or in part, is strictly prohibited
 * without prior written permission from DIODAC ELECTRONICS.
 * ═════════════════════════════════════════════════════════════════════════════
 * dbeam.h — v6.0.00  SPATIAL EXPRESSION & HIGH-SPEED ADC DMA DRIVER
 *
 * FIXES vs v5.1.00:
 *  [A] g_sensor_state: volatile SensorState → std::atomic<SensorState>.
 *      Cross-core writes need acquire/release semantics, not just volatile.
 *
 *  [B] SensorHealth: last_valid_read_ms and snr are written by Core 0
 *      (adc_dma_processing_task) and read by Core 1 (check_adc_dma_health,
 *      display).  Both are now under patchMux in dbeam.cpp; header updated
 *      to remove misleading 'volatile' that gave false safety impression.
 *
 *  [C] g_dbeam_snapshot_version moved here from a per-TU declaration.
 *      Already declared inline (C++17) — no change needed; just explicit doc.
 *
 *  [D] EDGE_COMP_NORMAL/RAINBOW and GLOBAL_RAINBOW_R/G/B moved here
 *      from laser.h.  These constants are dbeam calibration data; laser.h
 *      now includes dbeam.h to access them.  No functional change.
 *
 * COMPATIBILITY NOTES:
 *  — globals.h v5.0.04: setupDMA_ADC() is a thin wrapper around
 *    initDBeamSensor().  Do NOT call both in setup(); use initDBeamSensor()
 *    directly.  setupDMA_ADC() will be removed in a future globals cleanup.
 *    The double-call was the root cause of the GDMA boot error.
 *  — audio.h v5.2.02: hDBeamTask is now correctly assigned the handle from
 *    xTaskCreatePinnedToCore(adc_dma_processing_task, ...).
 * ═════════════════════════════════════════════════════════════════════════════ */
#pragma once
#ifndef DBEAM_H
#define DBEAM_H

#include <Arduino.h>
#include <algorithm>
#include <cmath>
#include <atomic>
#include <string.h>
#include "esp_adc/adc_continuous.h"
#include "driver/gpio.h"
#include "globals.h"

/* ── ADC hardware constants ──────────────────────────────────────────────── */
#define DBEAM_ADC_CHANNEL ADC_CHANNEL_6
#define DBEAM_ADC_GPIO GPIO_NUM_7
#define DBEAM_ADC_UNIT ADC_UNIT_1

/* ADC_BUFFER_READ_BYTES must be a multiple of conv_frame_size (64) and
 * of SOC_ADC_DIGI_RESULT_BYTES (4 on ESP32-S3).  256 = 4 frames = 64 samples ≈
 * 768 µs at 83 kHz ≈ ONE laser dwell — so a single read falls inside one lit (or
 * dark) window, making the dbeamLit sync-gate effective (no dark contamination)
 * while 64 samples still give a statistically stable RMS.                       */
static constexpr uint32_t ADC_BUFFER_READ_BYTES = 256;
static constexpr uint32_t ADC_SAMPLE_FREQ_HZ = 83333;
static constexpr float NOISE_FLOOR_MV = 110.0f;
static constexpr float FLT_DENORMAL_GUARD = 1.0e-5f;

/* Kalman filter tuning */
static constexpr float KALMAN_Q_AC = 0.05f; /* process noise  */
static constexpr float KALMAN_R_AC = 5.0f;  /* measurement noise */
static constexpr float MIN_SNR = 1.15f;

/* ═══════════════════════════════════════════════════════════════════════════
 * BEAM GEOMETRY COMPENSATION ARRAYS  [moved from laser.h]
 *
 * These constants belong in dbeam.h because they are ONLY consumed by
 * computeHardwareDACThreshold() — they are physics-based calibration values
 * for the 8-string frame geometry, not laser rendering parameters.
 *
 * EDGE_COMP_*: outer strings (0 and 7) receive shallower beam angles and
 *   smaller reflection apertures, requiring lower detection thresholds.
 *   Normal scale: symmetric roll-off (0.70 outer → 1.00 centre).
 *   Rainbow scale: asymmetric — slightly stronger left-side compensation
 *   because the red (lowest frequency) string has a larger beam waist.
 *
 * GLOBAL_RAINBOW_R/G/B: per-string RGB assignment for the two rainbow
 *   scales (indices 14–15).  Used both here (colour compensation weight)
 *   and in laser.h (PWM colour drive).  Declared inline in dbeam.h so that
 *   laser.h can include dbeam.h to get them without a separate header.
 *
 * DO NOT duplicate these in laser.h — include dbeam.h instead.
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Per-string threshold multipliers: index [0]=leftmost, [7]=rightmost.
 * Values tuned for a 500 mm frame with 60 mm string spacing.
 * [EDGE] SUPERSEDED at runtime by the editable per-scale edgeComp[NUM_SCALES][8]
 * (globals.h), seeded from EDGE_COMP_FACTORY[] and adjustable live in HARP SETUP →
 * Edge Comp (OC scrolls scales).  These constants are kept only as the documented
 * factory reference; computeHardwareDACThreshold() reads edgeComp[scale][string]
 * (the rainbow asymmetry table below is no longer applied to detection).      */
static constexpr float EDGE_COMP_NORMAL[8] = {
  0.70f, 0.80f, 1.00f, 1.00f, 1.00f, 1.00f, 0.80f, 0.70f
};
/* Rainbow-scale compensation — asymmetric due to red string beam waist.
 * String 5 (magenta/purple) gets 0.60 not 1.00 due to mixed-wavelength
 * extinction cross-section for the photosensor at that position.          */
static constexpr float EDGE_COMP_RAINBOW[8] = {
  0.55f, 0.80f, 1.00f, 1.00f, 1.00f, 0.60f, 0.80f, 0.55f
};

/* Per-string RGB for rainbow scales — TRUE rainbow: 8 hues spaced evenly
 * around the colour wheel (hue = string/8), each fully saturated.  The old
 * ROYGBIV table clustered three near-red strings (R/O/Y all share full red)
 * and three near-blue strings (B/I/V), so on the laser fan they read as
 * "shades of warm + shades of cool" rather than a rainbow.  Even hue spacing
 * maximises the perceptual distance between neighbouring beams.
 *   s0 red · s1 amber · s2 chartreuse · s3 green · s4 cyan · s5 azure ·
 *   s6 violet · s7 magenta                                                  */
static constexpr uint8_t GLOBAL_RAINBOW_R[8] = { 255, 255, 128,   0,   0,   0, 128, 255 };
static constexpr uint8_t GLOBAL_RAINBOW_G[8] = {   0, 191, 255, 255, 255,  64,   0,   0 };
static constexpr uint8_t GLOBAL_RAINBOW_B[8] = {   0,   0,   0,  64, 255, 255, 255, 191 };


/* ── Data structures ─────────────────────────────────────────────────────── */
struct KalmanState {
  float x = 0.0f;   /* state estimate */
  float p = 100.0f; /* error covariance */
  float q = KALMAN_Q_AC;
  float r = KALMAN_R_AC;
};

struct DACThresholdCalibration {
  uint16_t nominal_dac = 200;
  float edge_compensation = 1.0f;
  float color_compensation = 1.0f;
  uint16_t final_dac_voltage = 300;
};

/* SensorHealth fields are updated by Core 0 (DMA task) under patchMux.
 * Readers on any core must also take patchMux or read via g_last_good_data. */
struct SensorHealth {
  SensorState state = SensorState::UNINIT;
  uint32_t error_count = 0;
  bool watchdog_triggered = false;
  uint32_t last_valid_read_ms = 0;
  float confidence = 0.0f;
  float snr = 1.0f;
};

/* SensorData is snapshotted under patchMux into g_last_good_data.
 * All fields are explicitly initialised in initSensorArrays(). */
struct SensorData {
  int dcLevel = 0;
  float acAmplitude = 0.0f;
  SensorHealth health;
  float confidence = 0.0f;
};

/* ── Module snapshot version (inline C++17) ──────────────────────────────── */
inline std::atomic<uint8_t> g_dbeam_snapshot_version{ 0 };

/* ── Public API ──────────────────────────────────────────────────────────── */

/** Initialize ADC DMA driver.  Safe to call multiple times (idempotent).
 *  Returns true on first call (hardware init success) or subsequent calls
 *  where hardware is already running.  Returns false on hardware failure. */
bool initDBeamSensor();

/** Periodically call from loop() or a slow task to detect ADC watchdog. */
void check_adc_dma_health();

/** DMA ADC processing task — pin to Core 0 at priority 19. */
void IRAM_ATTR adc_dma_processing_task(void* pvParameters);

/** Compute per-string DAC comparator threshold from current scale/colour. */
void IRAM_ATTR computeHardwareDACThreshold(int stringIdx,
                                           float measured_ac_amplitude_mv);
/** Read back the last computed DAC threshold for a string. */
uint16_t IRAM_ATTR getHardwareDACThreshold(int stringIdx);


/* ── Expression routing helpers ──────────────────────────────────────────── */
/** Set the D-BEAM routing target (DbeamRoute: 0=OFF 1=MOD 2=VOL 3=CUT).
 *  Clears DSP accumulators when switching to OFF.                            */
static inline void applyDbeamRouteHW(uint8_t mode_v14) {
  const DbeamRoute mode = (DbeamRoute)(mode_v14 & 3u);
  const DbeamRoute prev = currentDbeamRoute.load(std::memory_order_relaxed);
  /* [DBEAM-VOL] Mirror applyDbeamRoute's volume-pedal baseline housekeeping. */
  if (mode == DbeamRoute::VOLUME && prev != DbeamRoute::VOLUME) {
    dbeamVolBaseHarp.store(mixHarpVol.load(std::memory_order_relaxed), std::memory_order_relaxed);
    dbeamVolBaseSeq .store(mixSeqVol .load(std::memory_order_relaxed), std::memory_order_relaxed);
  } else if (mode != DbeamRoute::VOLUME && prev == DbeamRoute::VOLUME) {
    mixHarpVol.store(dbeamVolBaseHarp.load(std::memory_order_relaxed), std::memory_order_release);
    mixSeqVol .store(dbeamVolBaseSeq .load(std::memory_order_relaxed), std::memory_order_release);
  }
  currentDbeamRoute.store(mode, std::memory_order_release);
  /* [ROUTE-FIX] Clear ALL addends (harp + seq) on every change so a previous
   * target can't stay stuck-modulated.  Mirrors applyDbeamRoute (patches.h). */
  dbeam_svf_cutoff.store(0, std::memory_order_release);
  dbeam_mod_depth.store(0, std::memory_order_release);
  dbeam_seq_svf_cutoff.store(0, std::memory_order_release);
  dbeam_seq_mod_depth.store(0, std::memory_order_release);
}
/** Apply the selected D-BEAM expression curve to the global raw amplitude.
 *  forceLinear=true keeps normalisation + gain but skips the curve shape
 *  (used by the VOLUME pedal, which applies its own inversion). */
float IRAM_ATTR applyDBEAMCurve(float rawAmplitude, bool forceLinear = false);

/** Update and return the GLOBAL expression as a full-resolution 0..1 float
 *  (continuous whole-reflection height).  stringIdx only stamps the legacy
 *  per-string mirror.  Also updates the dbeamAmplitude (14-bit) bargraph atomic. */
float IRAM_ATTR updateDbeamExpression(int stringIdx);

/** Route the full-scale 0..1 expression to the configured local DSP target
 *  (cutoff / mod / volume) per currentDbeamRoute.  No external MIDI. */
void IRAM_ATTR routeDbeamExpression(float norm01);

/** Re-apply the live expression after Target/Route changes (clears stale addends). */
static inline void dbeamRefreshAfterTargetChange() {
  routeDbeamExpression(updateDbeamExpression(-1));
}

/** Compare current Kalman estimate against a millivolt threshold.
 *  Returns HIGH (1) if below threshold (beam unblocked), LOW (0) if blocked. */
uint8_t IRAM_ATTR checkDbeamThreshold(float thresholdMv, int stringIdx = -1);

/* ── Extern storage (defined in dbeam.cpp) ───────────────────────────────── */
extern KalmanState g_kalman_ac[MAX_STRINGS];
extern SensorHealth g_health[MAX_STRINGS];
extern SensorData g_last_good_data[MAX_STRINGS];
extern DACThresholdCalibration dac_calibration[MAX_STRINGS];

#endif /* DBEAM_H */
