/* ═════════════════════════════════════════════════════════════════════════════
 * Octopus PRO XL v6.1.01 — Laser Harp Groovebox
 * © 2026 DIODAC ELECTRONICS / iSystem. All Rights Reserved.
 *
 * PROPRIETARY AND CONFIDENTIAL. Unauthorized copying, distribution, modification,
 * or use of this software or firmware, in whole or in part, is strictly prohibited
 * without prior written permission from DIODAC ELECTRONICS.
 * ═════════════════════════════════════════════════════════════════════════════
 * dbeam.h — v6.1.01  SPATIAL EXPRESSION & HIGH-SPEED ADC DMA DRIVER
 *
 * Core 0 task adc_dma_processing_task (prio 19): continuous ADC DMA, per-string
 * Kalman filter, D-BEAM expression, fogPublishAmp() copy for fog.h.
 * initDBeamSensor() is the sole ADC init entry (called from setup()).
 *
 * EDGE_COMP_* / GLOBAL_RAINBOW_* calibration tables live here; laser.h includes
 * this header.  Runtime beam thresholds use edgeComp[NUM_SCALES][8] from NVS.
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
 * BEAM GEOMETRY COMPENSATION ARRAYS
 *
 * Factory reference tables for edge compensation and rainbow beam colours.
 * Runtime detection uses edgeComp[NUM_SCALES][8] (globals.h, persisted in NVS);
 * computeHardwareDACThreshold() reads edgeComp[scale][string], not these arrays
 * directly (EDGE_COMP_RAINBOW asymmetry is no longer applied to detection).
 *
 * GLOBAL_RAINBOW_R/G/B: per-string RGB for rainbow scales — also used by
 * laser.h for PWM colour drive.  Do not duplicate in laser.h.
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Factory edge-comp reference (symmetric normal scale). */
static constexpr float EDGE_COMP_NORMAL[8] = {
  0.70f, 0.80f, 1.00f, 1.00f, 1.00f, 1.00f, 0.80f, 0.70f
};
/* Factory edge-comp reference (rainbow scale — asymmetric). */
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

/* SensorHealth updated by Core 0 DMA task under patchMux; readers must
 * hold patchMux or use the g_last_good_data snapshot. */
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

/** Optional ADC watchdog — implemented in dbeam.cpp (not wired in v6.1 boot path). */
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
  /* Volume-pedal baseline: capture bus levels entering VOLUME route; restore on exit. */
  if (mode == DbeamRoute::VOLUME && prev != DbeamRoute::VOLUME) {
    dbeamVolBaseHarp.store(mixHarpVol.load(std::memory_order_relaxed), std::memory_order_relaxed);
    dbeamVolBaseSeq .store(mixSeqVol .load(std::memory_order_relaxed), std::memory_order_relaxed);
  } else if (mode != DbeamRoute::VOLUME && prev == DbeamRoute::VOLUME) {
    mixHarpVol.store(dbeamVolBaseHarp.load(std::memory_order_relaxed), std::memory_order_release);
    mixSeqVol .store(dbeamVolBaseSeq .load(std::memory_order_relaxed), std::memory_order_release);
  }
  currentDbeamRoute.store(mode, std::memory_order_release);
  /* Clear harp + seq addends on every route change (no stale modulation). */
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
