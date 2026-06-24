/* ═════════════════════════════════════════════════════════════════════════════
 * Octopus PRO XL v6.1.00 — Laser Harp Groovebox
 * © 2026 DIODAC ELECTRONICS / iSystem. All Rights Reserved.
 *
 * PROPRIETARY AND CONFIDENTIAL. Unauthorized copying, distribution, modification,
 * or use of this software or firmware, in whole or in part, is strictly prohibited
 * without prior written permission from DIODAC ELECTRONICS.
 * ═════════════════════════════════════════════════════════════════════════════
 * dbeam.cpp — v6.1.00  D-BEAM ADC + EXPRESSION
 *
 * adc_dma_processing_task (Core 0, prio 19): continuous ADC DMA, per-string Kalman,
 * peak envelope → g_dbeam_global_ac for Core-1 laser expression reads.
 * Three isolated paths (see code_info.h §H): digital trigger latch (laser.cpp),
 * per-string telemetry/threshold, continuous expression envelope.  D-BEAM routes
 * locally to harp/seq DSP — no MIDI CC output.
 * ═════════════════════════════════════════════════════════════════════════════ */
/* dbeam.h now owns EDGE_COMP_* and GLOBAL_RAINBOW_* — laser.h includes dbeam.h */
#include "dbeam.h"
#include "patches.h"   /* SCALES[].isRainbow [FIX-F]; syncLivePatchFromAtomics  */
#include "fog.h"       /* [FOG] isolated fog-reject branch — fogPublishAmp()    */
/* laser.h, audio.h, effect.h removed — no direct usage in this TU              */
/* ── Per-string state arrays ─────────────────────────────────────────────── */
KalmanState             g_kalman_ac      [MAX_STRINGS];
SensorHealth            g_health         [MAX_STRINGS];
SensorData              g_last_good_data [MAX_STRINGS];
DACThresholdCalibration dac_calibration  [MAX_STRINGS];

/* ── Private file-scope state ────────────────────────────────────────────── */
/* [beam] 4-byte aligned so the DMA parse loop can load each TYPE2 frame as one
 * uint32_t (SOC_ADC_DIGI_RESULT_BYTES == 4) without unaligned-load faults. */
alignas(4) static uint8_t   g_accumulated_data[ADC_BUFFER_READ_BYTES];
static adc_continuous_handle_t g_dma_adc_handle = nullptr;   /* [FIX-A] guard */
static_assert(ADC_BUFFER_READ_BYTES % 4u == 0u,
              "ADC_BUFFER_READ_BYTES must be a multiple of 4 for uint32 frame loads");
static_assert(SOC_ADC_DIGI_RESULT_BYTES == 4,
              "TYPE2 frame loader assumes 4-byte ADC_DIGI results");

/* [beam] Non-rainbow colMult cache — scale RGB is identical for all 8 strings,
 * so one divide per scale change instead of ~1000/s on the laser task.  Written
 * only on Core 1 (laser); Core 0 (interface scale edits) always recomputes so
 * there is no cross-core cache tear.                                          */
static int     s_col_cache_scale = -1;
static uint8_t s_col_cache_r = 0, s_col_cache_g = 0, s_col_cache_b = 0;
static float   s_col_cache_mult = 1.0f;
static std::atomic<SensorState> g_sensor_state { SensorState::UNINIT }; /* [FIX-I] */

/* Per-string rolling average of the raw RMS, taken in laser-sync (one tap per
 * lit dwell of that string).  Feeds the Kalman so the estimate is built from a
 * boxcar-smoothed input → playable, low-jitter response.  4 taps ≈ the last 4
 * sweep visits of a string (~30 ms) — enough to kill comparator/optical jitter
 * without adding perceptible lag.                                              */
static constexpr int DBEAM_MA_TAPS = 4;
/* [beam] ring index uses a bitwise-AND wrap, which requires a power-of-two size. */
static_assert((DBEAM_MA_TAPS & (DBEAM_MA_TAPS - 1)) == 0, "DBEAM_MA_TAPS must be power of two");
static float   s_dbeam_ma_ring[MAX_STRINGS][DBEAM_MA_TAPS];
static float   s_dbeam_ma_sum [MAX_STRINGS];
static uint8_t s_dbeam_ma_pos [MAX_STRINGS];

/* ── BRANCH B: EXPRESSION envelope follower (continuous, on-time only) ────────
 * Two fully separate branches [user spec]:
 *   • Branch A (per-string buckets above) — ONLY the dynamic detection threshold
 *     + telemetry.  Triggering itself never touches the ADC (pure LT1016/74HC74
 *     digital latch on PIN_TRIGGER), so the threshold branch is non-critical.
 *   • Branch B (this) — EXPRESSION / hand-height for CC, filter, pitch, etc.
 *
 * Why NOT an 8-string average: averaging every dwell mixes the ONE hand-covered
 * string with the 7 near-zero no-hand strings, diluting the signal toward the
 * noise floor and swinging it as hand coverage changes → jittery "kakophony",
 * worse than per-string.  Instead Branch B is a PEAK ENVELOPE FOLLOWER fed by
 * every lit dwell (on-time only — dark gaps feed nothing, so the stream is
 * gap-free and never flushed to 0):
 *     fast ATTACK  → instantly rises to the nearest hand's reflection
 *     slow RELEASE → no-hand dwells decay it only slightly between hand dwells
 * Net: one smooth, unbroken stream that follows the closest hand at full range.
 * Written on Core 0 (ADC task); published lock-free via the atomic for the
 * Core-1 expression read in updateDbeamExpression().
 * ATTACK / RELEASE are runtime-adjustable SSOT atomics (dbeamExprAttack /
 * dbeamExprRelease in globals.h), editable from the D-BEAM menu + NVS-persisted. */
static float              g_expr_env       = 0.0f; /* envelope state (Core 0)   */
static std::atomic<float> g_dbeam_global_ac{ 0.0f };   /* Core 0 → Core 1 bridge */
/* Core-1-only (touched solely inside updateDbeamExpression): */
static float   g_global_ema      = 0.0f;
static float   g_global_peak     = 100.0f;
static float   g_global_inv_max  = 1.0f / 200.0f;
static float   g_global_expr_out = 0.0f;

/* ─────────────────────────────────────────────────────────────────────────────
 * initSensorArrays — zero / default all per-string arrays
 * [FIX-G] SensorData fields fully initialised so first reads are not UB.
 * ─────────────────────────────────────────────────────────────────────────────*/
static void initSensorArrays() {
  for (int i = 0; i < MAX_STRINGS; i++) {
    g_kalman_ac[i]  = { 0.0f, 100.0f, KALMAN_Q_AC, KALMAN_R_AC };

    g_health[i].state              = SensorState::UNINIT;
    g_health[i].error_count        = 0;
    g_health[i].watchdog_triggered = false;
    g_health[i].last_valid_read_ms = 0;
    g_health[i].confidence         = 0.0f;
    g_health[i].snr                = 1.0f;

    /* [FIX-G] Explicit zero-init of all SensorData fields */
    g_last_good_data[i].dcLevel     = 0;
    g_last_good_data[i].acAmplitude = 0.0f;
    g_last_good_data[i].confidence  = 0.0f;
    g_last_good_data[i].health      = g_health[i];

    for (int t = 0; t < DBEAM_MA_TAPS; ++t) s_dbeam_ma_ring[i][t] = 0.0f;
    s_dbeam_ma_sum[i] = 0.0f;
    s_dbeam_ma_pos[i] = 0;

    dac_calibration[i] = { 200, 1.0f, 1.0f, 300 };
  }

  /* Branch B expression envelope reset */
  g_expr_env        = 0.0f;
  g_global_ema      = 0.0f;
  g_global_peak     = 100.0f;
  g_global_inv_max  = 1.0f / 200.0f;
  g_global_expr_out = 0.0f;
  g_dbeam_global_ac.store(0.0f, std::memory_order_relaxed);

  s_col_cache_scale = -1;   /* [beam] force colMult recompute after init/NVS load */
}

/* ─────────────────────────────────────────────────────────────────────────────
 * initDBeamSensor — idempotent [FIX-A]
 *
 * Allocates + configures the continuous ADC but does NOT start sampling.
 * [SAVE-FIX8] Call from setup() (original timing — must run before the laser task
 * so the shared ADC1/RTC setup is complete).  adc_continuous_start() is split out
 * into dbeamAdcStart(), which the D-BEAM task calls, so the ADC lock is owned by
 * the same task that releases it via adc_continuous_stop() in the self-heal —
 * otherwise FreeRTOS asserts xTaskPriorityDisinherit.  Idempotent: a second call
 * returns true immediately.
 * ─────────────────────────────────────────────────────────────────────────────*/
bool initDBeamSensor() {
  /* [FIX-A] Idempotent guard: second call returns true without re-init */
  if (g_dma_adc_handle != nullptr) return true;

  initSensorArrays();
  gpio_set_pull_mode(DBEAM_ADC_GPIO, GPIO_FLOATING);

  adc_continuous_handle_cfg_t adc_cfg = {
    .max_store_buf_size = 4096,   /* ring buffer: covers ~49ms at 83333 Hz */
    .conv_frame_size    = 64      /* 16 samples per frame (TYPE2 = 4 bytes each) */
  };
  esp_err_t err = adc_continuous_new_handle(&adc_cfg, &g_dma_adc_handle);
  if (err != ESP_OK) {
    g_dma_adc_handle = nullptr;
    return false;
  }

  adc_digi_pattern_config_t pat = {
    .atten    = ADC_ATTEN_DB_11,
    .channel  = DBEAM_ADC_CHANNEL,
    .unit     = DBEAM_ADC_UNIT,
    .bit_width= SOC_ADC_DIGI_MAX_BITWIDTH
  };
  adc_continuous_config_t dig_cfg = {
    .pattern_num  = 1,
    .adc_pattern  = &pat,
    .sample_freq_hz = ADC_SAMPLE_FREQ_HZ,
    .conv_mode    = ADC_CONV_SINGLE_UNIT_1,
    .format       = ADC_DIGI_OUTPUT_FORMAT_TYPE2,
  };
  err = adc_continuous_config(g_dma_adc_handle, &dig_cfg);
  if (err != ESP_OK) {
    adc_continuous_deinit(g_dma_adc_handle);
    g_dma_adc_handle = nullptr;
    return false;
  }

  /* [SAVE-FIX8] adc_continuous_start() is intentionally NOT called here.  It
   * acquires the shared ADC HW lock, which is OWNED by the calling task and is
   * released by adc_continuous_stop() in the task's self-heal (reinitDBeamAdc).
   * Start and stop MUST run on the same task or FreeRTOS asserts
   * xTaskPriorityDisinherit (the panic seen on every save).  Allocation + config
   * stay here so the shared ADC1 setup still completes in setup() BEFORE the
   * laser task starts (deferring all of it disturbed the laser-harp beam timing);
   * only the lock-taking start() is moved onto the D-BEAM task via dbeamAdcStart(). */
  g_sensor_state.store(SensorState::STABLE, std::memory_order_release);
  return true;
}

/* [SAVE-FIX8] Begin continuous sampling.  MUST be called from the D-BEAM task
 * (adc_dma_processing_task) so the ADC lock owner matches the task that stops it. */
static bool dbeamAdcStart() {
  if (g_dma_adc_handle == nullptr) return false;
  return adc_continuous_start(g_dma_adc_handle) == ESP_OK;
}

/* ─────────────────────────────────────────────────────────────────────────────
 * reinitDBeamAdc — [WS11 SELF-HEAL] tear down and rebuild the continuous ADC.
 *
 * The continuous driver can wedge if its DMA pool overflows while the task is
 * paused (long NVS save) or on a transient HW error — after which
 * adc_continuous_read returns timeout/0 forever and D-BEAM is dead until a power
 * cycle.  This fully recreates the handle (stop → deinit → new → config → start)
 * so the ADC task recovers on its own.  Returns true on a healthy restart.
 * ─────────────────────────────────────────────────────────────────────────────*/
static bool reinitDBeamAdc() {
  if (g_dma_adc_handle != nullptr) {
    adc_continuous_stop(g_dma_adc_handle);     /* ignore errors — best effort */
    adc_continuous_deinit(g_dma_adc_handle);
    g_dma_adc_handle = nullptr;                /* clears initDBeamSensor's guard */
  }
  /* [SAVE-FIX8] Rebuild handle + config, then start on THIS (D-BEAM) task so the
   * ADC lock is owned by the same task that just released it via the stop above. */
  if (!initDBeamSensor()) return false;
  return dbeamAdcStart();
}

/* ── Kalman filter update ────────────────────────────────────────────────── */
static inline void IRAM_ATTR updateKalman(KalmanState& ks, float z) {
  if (fabsf(z) < FLT_DENORMAL_GUARD) z = 0.0f;   /* [beam] explicit float */
  const float p_prior = ks.p + ks.q;
  const float k       = p_prior / (p_prior + ks.r);
  ks.x = ks.x + k * (z - ks.x);
  ks.p = (1.0f - k) * p_prior;
  if (ks.p < 0.01f)                    ks.p = 0.01f;
  if (fabsf(ks.x) < FLT_DENORMAL_GUARD) ks.x = 0.0f;
}

/* ─────────────────────────────────────────────────────────────────────────────
 * applyExpressionHysteresis [FIX-J]
 *
 * Previous: dead-band — output frozen for |delta| ≤ 3 mV, causing the
 * expression to snap and stick.  Replaced with exponential smoothing (α=0.25)
 * that always follows the signal but attenuates high-frequency jitter.
 * Sub-floor clamping retained to prevent near-zero drift.
 * ─────────────────────────────────────────────────────────────────────────────*/
static inline float IRAM_ATTR applyExpressionHysteresis(float target,
                                                         float reference_peak) {
  if (std::fabs(target) < FLT_DENORMAL_GUARD) target = 0.0f;

  /* Exponential smoothing: α=0.25 for responsiveness, 0.75 for smoothing.
   * Operates on the single GLOBAL expression accumulator (height tracking). */
  g_global_expr_out = g_global_expr_out * 0.75f + target * 0.25f;

  /* Clamp sub-floor noise to zero */
  if (g_global_expr_out < FLT_DENORMAL_GUARD) g_global_expr_out = 0.0f;
  if (reference_peak > 0.0f && g_global_expr_out > reference_peak)
    g_global_expr_out = reference_peak;

  return std::min(4095.0f, std::max(0.0f, g_global_expr_out));
}

/* ─────────────────────────────────────────────────────────────────────────────
 * adc_dma_processing_task — Core 0, priority 19  [DBEAM-FIX]
 *
 * Lives on Core 0 (opposite the laser): the commit below only fires while the
 * beam is lit, and the Core-1 laser busy-polls through the whole lit dwell, so a
 * Core-1 ADC task could never run in the lit window.  Priority 19 keeps it above
 * the cooperative UI tasks (OledRender 14 / ControlPoll 10) so it is not starved
 * under full play, but below AudioSynth (24) so it can never delay a buffer fill.
 *
 * Reads DMA ring buffer, computes RMS AC amplitude, Kalman-filters it, and
 * snapshots results into g_last_good_data under patchMux. [FIX-B]
 *
 * ADC TYPE2 frame (4 bytes, little-endian):
 *   bits[11:0]  → 12-bit ADC result
 *   bits[16:13] → ADC channel number
 *   bit[17]     → ADC unit
 * ─────────────────────────────────────────────────────────────────────────────*/
void IRAM_ATTR adc_dma_processing_task(void* pvParameters) {
  (void)pvParameters;
  while (!g_systemReady.load(std::memory_order_acquire))
    vTaskDelay(pdMS_TO_TICKS(1));

  /* [SAVE-FIX8] Start the continuous ADC ON THIS TASK.  The ESP-IDF shared ADC
   * HW lock is a plain mutex owned by whichever task calls adc_continuous_start;
   * adc_continuous_stop() (the self-heal below) releases it.  Previously start
   * ran in setup() on loopTask, so the self-heal's stop released a lock held by
   * another task → "assert xTaskPriorityDisinherit (pxTCB == current)" — the
   * panic seen on every save (the save pause wedges the DMA ring → self-heal).
   * Alloc + config already ran in setup() (initDBeamSensor) so the shared ADC1
   * setup completes before the laser task starts; here we only take the lock. */
  if (g_dma_adc_handle != nullptr) dbeamAdcStart();
  else if (initDBeamSensor())      dbeamAdcStart();   /* fallback: setup init failed */

  uint32_t bytesRead = 0;

  /* [WS11 SELF-HEAL] Stall watchdog: if adc_continuous_read returns no valid
   * frame for ~250 ms straight (≈25 × 10 ms timeouts), the continuous driver has
   * wedged (DMA pool overflow after an NVS-save pause, or a transient HW fault).
   * Rebuild it in place instead of spinning dead until a manual restart. */
  static constexpr uint32_t DBEAM_STALL_READS = 25u;
  uint32_t consecutiveFail = 0;

  for (;;) {
    /* Pause during NVS save window or before hardware is ready.  The save pause
     * is EXPECTED — don't count it toward the stall watchdog. */
    if (g_dma_adc_handle == nullptr ||
        g_saveArmed.load(std::memory_order_acquire)) {
      consecutiveFail = 0;
      vTaskDelay(pdMS_TO_TICKS(1));
      continue;
    }

    const esp_err_t res = adc_continuous_read(
        g_dma_adc_handle, g_accumulated_data, ADC_BUFFER_READ_BYTES,
        &bytesRead, 10 /* ms timeout */);

    /* No explicit trailing delay: adc_continuous_read blocks until a full frame
     * is ready (~ one dwell at 83 kHz), draining the HW ring back-to-back so the
     * reading is always FRESH.  The old trailing vTaskDelay(1ms) let the ring
     * back up by ~300 B/ms → tens-of-ms-stale data, the real source of "lag". */
    if (res != ESP_OK || bytesRead == 0) {
      if (++consecutiveFail >= DBEAM_STALL_READS) {
        /* Driver wedged — rebuild it.  On success resume cleanly; on failure
         * back off and keep retrying (handle stays null → top-of-loop pause). */
        reinitDBeamAdc();
        consecutiveFail = 0;
        vTaskDelay(pdMS_TO_TICKS(5));
      } else {
        vTaskDelay(1);
      }
      continue;
    }
    consecutiveFail = 0;

    {
      /* ── dbeamLit stuck-timer [WS11-FIX1] ────────────────────────────
       * If dbeamLit has been true for more than 50 ms, the laser task has
       * stalled mid-dwell (crashed or frozen galvo state machine).  Force it
       * false so ADC samples are gated correctly and the amplitude follower
       * doesn't saturate.  50 ms >> BEAM_DWELL_US (~1 ms) and << any real
       * laser freeze recovery time, so there are no false trips during normal
       * operation. */
      static uint32_t s_lit_since_ms = 0u;
      {
        const bool litNow = dbeamLit.load(std::memory_order_relaxed);
        const uint32_t nowMs2 = (uint32_t)(esp_timer_get_time() / 1000u);
        if (litNow) {
          if (s_lit_since_ms == 0u) s_lit_since_ms = nowMs2;
          else if ((uint32_t)(nowMs2 - s_lit_since_ms) > 50u) {
            dbeamLit.store(false, std::memory_order_relaxed);
            s_lit_since_ms = 0u;
          }
        } else {
          s_lit_since_ms = 0u;
        }
      }

      /* ── Laser-sync gate [beam] ──────────────────────────────────────── */
      const bool lit = dbeamLit.load(std::memory_order_relaxed);
      const int  totalSamples = lit ? (int)bytesRead / SOC_ADC_DIGI_RESULT_BYTES : 0;
      int      validCount    = 0;
      uint32_t sumAcc        = 0;
      uint32_t sqAcc         = 0;   /* [beam] ≤64 samples × 4089² ≈ 1.07e9 < UINT32_MAX */

      /* [beam] One uint32_t load per TYPE2 frame (buffer is alignas(4)); bit layout
       * matches the prior byte-wise [FIX-E] extract — channel bits[16:13], raw[11:0]. */
      const uint32_t* const frames =
          reinterpret_cast<const uint32_t*>(g_accumulated_data);
      for (int i = 0; i < totalSamples; ++i) {
        const uint32_t w   = frames[i];
        const uint8_t  ch  = (uint8_t)(((w >> 13) & 0x07u) | (((w >> 16) & 0x01u) << 3));
        if (ch != (uint8_t)DBEAM_ADC_CHANNEL) continue;

        const uint16_t raw = (uint16_t)(w & 0xFFFu);
        if (raw >= 4090u) continue;   /* reject saturation */

        sumAcc += raw;
        sqAcc  += (uint32_t)raw * raw;
        validCount++;
      }

      /* validCount stays 0 when dark (totalSamples=0), so this also gates "lit". */
      if (validCount > 0) {
        const float    invCount = 1.0f / (float)validCount;  /* [beam] one divide */
        const uint32_t dc  = sumAcc / (uint32_t)validCount;
        float variance = (float)sqAcc * invCount - (float)dc * (float)dc;
        if (variance < 0.0f) variance = 0.0f;
        const float rmsCount = sqrtf(variance);              /* [beam] explicit float */
        const float rmsMv    = (rmsCount / 4096.0f) * 3300.0f;  /* mV, 3.3 V ref */

        const uint8_t si = dbeamLastStringIdx.load(std::memory_order_relaxed) & 7u;

        /* ── BRANCH B: EXPRESSION envelope (continuous, on-time only) ──────────
         * Peak follower fed by EVERY lit dwell regardless of string.  Fast
         * attack rises to the nearest hand instantly; slow release rides over
         * the 7/8 no-hand dwells so they barely dent it.  No off-time samples
         * are fed → the stream is continuous and never flushed to 0, giving a
         * smooth hand-height signal (vs the old 8-string average that diluted
         * and jittered).  The per-string bucket below stays independent. */
        const float atk = dbeamExprAttack.load(std::memory_order_relaxed);
        const float rel = dbeamExprRelease.load(std::memory_order_relaxed);
        if (rmsMv > g_expr_env) g_expr_env += (rmsMv - g_expr_env) * atk;
        else                    g_expr_env += (rmsMv - g_expr_env) * rel;
        g_dbeam_global_ac.store(g_expr_env, std::memory_order_relaxed);

        /* ── BRANCH A: per-string rolling average (boxcar) of the raw RMS, one
         * tap per lit dwell.  Feeds the per-string Kalman (threshold/telemetry
         * only — never the trigger, which is pure digital). */
        s_dbeam_ma_sum[si]      -= s_dbeam_ma_ring[si][s_dbeam_ma_pos[si]];
        s_dbeam_ma_ring[si][s_dbeam_ma_pos[si]] = rmsMv;
        s_dbeam_ma_sum[si]      += rmsMv;
        s_dbeam_ma_pos[si]       = (uint8_t)((s_dbeam_ma_pos[si] + 1) & (DBEAM_MA_TAPS - 1)); /* [beam] pow2 mask */
        const float maMv         = s_dbeam_ma_sum[si] * (1.0f / (float)DBEAM_MA_TAPS);

        /* [PHASE3-LOCK] g_kalman_ac[si] is owned EXCLUSIVELY by this dbeam task
         * (cross-core readers go through g_last_good_data per [FIX-C]), so the
         * Kalman update and all FP/telemetry math now run OUTSIDE the lock.  Only
         * the cross-core-visible publish (g_health + g_last_good_data) stays in
         * the critical section, keeping the patchMux hold short.  This task runs
         * on Core 0 (the audio core), so this hold runs IRQs-off on Core 0 and
         * can briefly delay AudioSynth — keep the section minimal.  Cross-core
         * (Core-1 laser) readers go through g_last_good_data and spin only for
         * the duration of this publish.  [Future opt: a seqlock on
         * g_dbeam_snapshot_version could drop the lock entirely.]               */
        updateKalman(g_kalman_ac[si], maMv);
        const float kx   = g_kalman_ac[si].x;
        fogPublishAmp(si, kx);   /* [FOG] copy into the isolated fog-reject branch */
        const float snr  = (maMv > NOISE_FLOOR_MV) ? (maMv / NOISE_FLOOR_MV) : 0.0f;
        const float conf = (snr > MIN_SNR) ? std::min(1.0f, (snr - MIN_SNR) * 0.5f) : 0.0f;
        const uint32_t nowMs = millis();

        portENTER_CRITICAL(&patchMux);
        g_health[si].snr                 = snr;
        g_health[si].confidence          = conf;   /* [beam] was never written here →
                                                    * g_last_good_data[].health.confidence
                                                    * stayed 0 after the struct copy below,
                                                    * disagreeing with .confidence. Fixed so
                                                    * both fields are coherent for telemetry. */
        g_health[si].last_valid_read_ms  = nowMs;
        g_last_good_data[si].acAmplitude = kx;
        g_last_good_data[si].dcLevel     = (int)dc;
        g_last_good_data[si].confidence  = conf;
        g_last_good_data[si].health      = g_health[si];
        portEXIT_CRITICAL(&patchMux);

        /* [beam] Atomic RMW publish-counter bump (release) — re-publishes the
         * snapshot to other-core readers.  fetch_add is a single atomic op and
         * wraps uint8 at 256, vs the prior non-atomic load+store. */
        g_dbeam_snapshot_version.fetch_add(1u, std::memory_order_release);
      }
    }
    /* No trailing delay — the blocking adc_continuous_read paces this loop and
     * yields CPU while waiting for the next frame (see note above). */
  }
}

/* ─────────────────────────────────────────────────────────────────────────────
 * check_adc_dma_health — call periodically from loop() or a slow task.
 * [FIX-H] Reads g_health under patchMux to match the writer's lock.
 * ─────────────────────────────────────────────────────────────────────────────*/
void check_adc_dma_health() {
  const uint32_t now = millis();
  /* [beam] ONE critical section instead of up to 16 enter/exit pairs: read all
   * timestamps and set the watchdog flags in a single coherent, short window.
   * The prior per-string lock/unlock churn contended with the audio core and
   * the DMA publish (also on patchMux), adding interrupt-off jitter for no gain.
   * The atomic sensor-state store is kept OUTSIDE the lock.                     */
  bool anyStale = false;
  portENTER_CRITICAL(&patchMux);
  for (int i = 0; i < MAX_STRINGS; i++) {
    if ((now - g_health[i].last_valid_read_ms) > 1000u) {
      g_health[i].watchdog_triggered = true;
      anyStale = true;
    }
  }
  portEXIT_CRITICAL(&patchMux);

  if (anyStale)
    g_sensor_state.store(SensorState::ERROR, std::memory_order_release);
}

/* ─────────────────────────────────────────────────────────────────────────────
 * computeHardwareDACThreshold
 * [FIX-F] Uses SCALES[scaleIdx].isRainbow instead of hardcoded (idx >= 14).
 * ─────────────────────────────────────────────────────────────────────────────*/
/* ─────────────────────────────────────────────────────────────────────────────
 * computeHardwareDACThreshold — FreeRTOS-safe rewrite  [v5.3]
 *
 * WHY THIS MATTERS: scaleMargin[], scaleR/G/B[] are non-atomic byte arrays
 * shared between Core 0 (interface.cpp updateHardwareParameter case 0,
 * under patchMux) and Core 1 (laser task / loop() calling this function).
 * Reading them without synchronisation is a data race under C++ memory model.
 *
 * FIX: snapshot all scale-dependent inputs inside a portENTER_CRITICAL window.
 * Float computation runs entirely outside the lock — the critical section
 * is only 5 byte-reads (~100 ns at 240 MHz), so laser task jitter is negligible.
 *
 * Functional behaviour is 1:1 identical to v5.1.  IRAM_ATTR kept because
 * this function is called from the laser task which runs on Core 1 where the
 * DMA controller may briefly disable the instruction cache.
 * ─────────────────────────────────────────────────────────────────────────────*/
void IRAM_ATTR computeHardwareDACThreshold(int stringIdx,
                                            float /*measured_ac_amplitude_mv*/) {
  stringIdx = (stringIdx < 0) ? 0 : (stringIdx >= MAX_STRINGS ? MAX_STRINGS-1 : stringIdx);

  /* ── Step 1: atomic scale index (already safe) ───────────────────────── */
  const int  scaleIdx  = (int)(harpScaleIndex.load(std::memory_order_relaxed) & (NUM_SCALES - 1));
  const bool isRainbow = SCALES[scaleIdx].isRainbow;   /* SCALES is init-time const */

  /* ── Step 2: snapshot per-scale volatile inputs under patchMux ───────── */
  /* scaleMargin is a uint16_t array (0–2000) and scaleR/G/B are uint8_t arrays,
   * all written by Core 0 interface.cpp under patchMux.  Take the same lock
   * here for a consistent read.                                            */
  uint16_t snap_margin;
  uint8_t  snap_r, snap_g, snap_b;
  uint8_t  snap_edge;   /* [EDGE] per-string comp (percent, 100 = ×1.00) */
  portENTER_CRITICAL(&patchMux);
  snap_margin = scaleMargin[scaleIdx];
  /* [EDGE-PERSCALE] Each scale owns an independent per-string edge-comp row. */
  snap_edge   = edgeComp[scaleIdx][stringIdx];
  if (isRainbow) {
    snap_r = GLOBAL_RAINBOW_R[stringIdx];
    snap_g = GLOBAL_RAINBOW_G[stringIdx];
    snap_b = GLOBAL_RAINBOW_B[stringIdx];
  } else {
    snap_r = scaleR[scaleIdx];
    snap_g = scaleG[scaleIdx];
    snap_b = scaleB[scaleIdx];
  }
  portEXIT_CRITICAL(&patchMux);

  /* ── Step 3: all FP computation outside the lock ─────────────────────── */
  const float marginDac = (float)snap_margin * (4095.0f / 3300.0f);
  /* [EDGE-PERSCALE] Runtime per-string edge compensation (HARP SETUP → Edge Comp).
   * Independent row per scale (edgeComp[scaleIdx][]); seeded from
   * EDGE_COMP_FACTORY[] and persisted via LaserSettings.edge_comp[NUM_SCALES][8]. */
  const float edgeMult  = (float)snap_edge / 100.0f;

  /* [beam] Rainbow: per-string RGB → compute every call.  Non-rainbow: scale RGB
   * is shared across all strings — cache the colMult on Core 1 (laser hot path);
   * Core 0 (interface scale edits) always recomputes and never touches the cache. */
  float colMult;
  if (isRainbow) {
    const float colSum = (float)(snap_r + snap_g + snap_b);
    colMult = (colSum > 0.0f)
        ? ((float)snap_r * 1.2f + (float)snap_g * 1.0f + (float)snap_b * 1.3f) / colSum
        : 1.0f;
  } else if (xPortGetCoreID() == 1 &&
             scaleIdx == s_col_cache_scale &&
             snap_r == s_col_cache_r && snap_g == s_col_cache_g && snap_b == s_col_cache_b) {
    colMult = s_col_cache_mult;
  } else {
    const float colSum = (float)(snap_r + snap_g + snap_b);
    colMult = (colSum > 0.0f)
        ? ((float)snap_r * 1.2f + (float)snap_g * 1.0f + (float)snap_b * 1.3f) / colSum
        : 1.0f;
    if (xPortGetCoreID() == 1) {
      s_col_cache_scale = scaleIdx;
      s_col_cache_r     = snap_r;
      s_col_cache_g     = snap_g;
      s_col_cache_b     = snap_b;
      s_col_cache_mult  = colMult;
    }
  }

  const float target = marginDac * edgeMult * colMult;

  /* ── Step 4: write result (one uint16_t per field — ARM word writes are
   *   single-bus-cycle; a release fence ensures Core 0 sees the update)    */
  dac_calibration[stringIdx].nominal_dac        = snap_margin;
  dac_calibration[stringIdx].edge_compensation  = edgeMult;
  dac_calibration[stringIdx].color_compensation = colMult;
  dac_calibration[stringIdx].final_dac_voltage  =
      (uint16_t)std::min(4095.0f, std::max(0.0f, target));

  std::atomic_thread_fence(std::memory_order_release);
}

/* ── getHardwareDACThreshold ─────────────────────────────────────────────── */
uint16_t IRAM_ATTR getHardwareDACThreshold(int stringIdx) {
  return dac_calibration[stringIdx & 7].final_dac_voltage;
}

/* ── applyDBEAMCurve — operates on the GLOBAL expression signal ───────────────
 * Returns a normalised 0..1 expression.  forceLinear=true keeps the noise-floor
 * removal, peak normalisation and HW gain but SKIPS the user curve shape — used
 * by the VOLUME pedal, which applies its own base×(1−x) inversion and would be
 * double-shaped (and the Inverted curve cancelled) if the curve ran here too.  */
float IRAM_ATTR applyDBEAMCurve(float rawAmplitude, bool forceLinear) {
  if (std::isnan(rawAmplitude) || std::isinf(rawAmplitude)) rawAmplitude = 0.0f;
  if (rawAmplitude < FLT_DENORMAL_GUARD) return 0.0f;

  /* Remove structural noise floor, then normalise to observed peak range */
  static constexpr float kNoiseFloor = 150.0f;
  const float active   = std::max(0.0f, rawAmplitude - kNoiseFloor);
  float norm           = std::min(1.0f, active * g_global_inv_max);

  if (!forceLinear) {
    switch (currentDbeamCurve.load(std::memory_order_relaxed)) {
    case DBEAMCurve::INVERTED:    norm = 1.0f - norm; break;
    case DBEAMCurve::EXPONENTIAL: norm = norm * norm; break;
    case DBEAMCurve::LOGARITHMIC: norm = std::sqrt(norm); break;
    case DBEAMCurve::SIGMOID:     norm = norm * norm * (3.0f - 2.0f * norm); break;
    case DBEAMCurve::LINEAR:
    default: break;
    }
  }

  return std::min(1.0f, std::max(0.0f, norm * dbeamHWCfg.gain));
}

/* ─────────────────────────────────────────────────────────────────────────────
 * updateDbeamExpression
 * EXPRESSION = continuous hand-height from Branch B's peak envelope follower
 * [user spec].  Branch A's per-string sampling is reserved for the dynamic
 * detection threshold, so the height reads the single global envelope (fed by
 * every lit dwell, on-time only) — NOT the current string's bucket.  stringIdx
 * is now unused (kept for call-site compatibility).
 * ─────────────────────────────────────────────────────────────────────────────*/
float IRAM_ATTR updateDbeamExpression(int /*stringIdx*/) {
  const float currentAC = g_dbeam_global_ac.load(std::memory_order_relaxed);

  /* Final EMA (α=0.05) — removes any residual per-cycle ripple from the env */
  g_global_ema = currentAC * 0.05f + g_global_ema * 0.95f;

  /* Adaptive peak tracker with slow decay (global) */
  if (g_global_ema > g_global_peak)      g_global_peak = g_global_ema;
  else if (g_global_peak > 20.0f)        g_global_peak *= 0.999f;

  const float range = std::max(200.0f, g_global_peak - 150.0f);
  g_global_inv_max  = 1.0f / (range + FLT_DENORMAL_GUARD);

  if (!dbeamEnabled.load(std::memory_order_relaxed)) {
    dbeamAmplitude.store(0u, std::memory_order_release);
    return 0.0f;
  }

  const float smoothed = applyExpressionHysteresis(g_global_ema, g_global_peak);
  /* [DBEAM-VOL] Force LINEAR (skip the user curve shape only) while VOLUME is
   * routed — the pedal does its own inversion.  Normalisation/gain are kept, so
   * the result is still a proper 0..1 expression (NOT the raw 0..4095 mV).     */
  const bool  forceLin = (currentDbeamRoute.load(std::memory_order_relaxed) == DbeamRoute::VOLUME);
  const float curved   = applyDBEAMCurve(smoothed, forceLin);

  /* Bargraph mirror stays 14-bit (display only); the DSP path below gets the
   * full-resolution float so cutoff/mod/volume are not quantised to 7/14-bit. */
  dbeamAmplitude.store((uint16_t)(curved * 16383.0f), std::memory_order_release);
  return curved;   /* full-scale 0..1 expression */
}

/* ─────────────────────────────────────────────────────────────────────────────
 * routeDbeamExpression — apply the D-BEAM expression to the LOCAL harp DSP.
 *
 * D-BEAM is a dedicated laser-harp performance controller: its expression drives
 * the harp engine directly through these atomics (consumed in harp.cpp), nothing
 * else.  It is intentionally NOT routed to external MIDI or echoed to the App —
 * there is no musical reason to send a hand-over-laser gesture off-device.
 *
 * [FULL-SCALE] norm01 is the continuous 0..1 float straight off the expression
 * curve — no 7/14-bit MIDI quantisation.  Each function gets its own native
 * range; the TARGET (currentDbeamTarget) selects which synth's addend/volume
 * receives it, and the OTHER synth's addend is forced to 0 so only one engine
 * is ever modulated at a time:
 *   CUTOFF      → dbeam[/_seq]_svf_cutoff  20..90% of 32768 (SVF cutoff add)
 *   MODULATION  → dbeam[/_seq]_mod_depth   [0, 16383]  (LFO depth add)
 *   VOLUME      → mixHarpVol / mixSeqVol   INVERTED pedal: rests at the user's
 *                 level and dips toward 0 as the hand nears (vol = base×(1−x)),
 *                 auto-returning on lift (see VOLUME case below).
 * ─────────────────────────────────────────────────────────────────────────────*/
void IRAM_ATTR routeDbeamExpression(float norm01) {
  const DbeamRoute mode = currentDbeamRoute.load(std::memory_order_relaxed);
  if (mode == DbeamRoute::OFF) return;

  if (norm01 < 0.0f)      norm01 = 0.0f;
  else if (norm01 > 1.0f) norm01 = 1.0f;

  const bool toSeq = (currentDbeamTarget.load(std::memory_order_relaxed) == DbeamTarget::SEQ);

  switch (mode) {
  case DbeamRoute::CUTOFF: {
    /* [DBEAM-CUT] Map the 0..1 expression into a 20..90 % window of the cutoff
     * addend instead of the full 0..100 %.  The 20 % floor means the addend never
     * snaps up from fully-closed as the hand first enters (that step caused the
     * audible click), and the 90 % ceiling trims the abrupt top of the sweep.   */
    constexpr float kCutFloor = 0.20f;          /* addend at rest / hand far  */
    constexpr float kCutCeil  = 0.90f;          /* addend at hand-closest     */
    const int32_t cut = (int32_t)((kCutFloor + (kCutCeil - kCutFloor) * norm01) * 32768.0f);
    if (toSeq) { dbeam_seq_svf_cutoff.store(cut, std::memory_order_release);
                 dbeam_svf_cutoff.store(0, std::memory_order_release); }
    else       { dbeam_svf_cutoff.store(cut, std::memory_order_release);
                 dbeam_seq_svf_cutoff.store(0, std::memory_order_release); }
    break; }
  case DbeamRoute::MODULATION: {
    const uint16_t md = (uint16_t)(norm01 * 16383.0f);
    if (toSeq) { dbeam_seq_mod_depth.store(md, std::memory_order_release);
                 dbeam_mod_depth.store(0, std::memory_order_release); }
    else       { dbeam_mod_depth.store(md, std::memory_order_release);
                 dbeam_seq_mod_depth.store(0, std::memory_order_release); }
    break; }
  case DbeamRoute::VOLUME: {
    /* [DBEAM-VOL] Inverted volume pedal.  norm01 ≈ 0 when the hand is OFF the
     * sensor (rest) and rises toward 1 as the hand approaches.  We want the bus
     * to SIT at the user's normal level at rest and DIP toward silence as the
     * hand nears, returning on lift:  vol = base × (1 − norm01).
     *   • PRESS edge (rest→engaged): capture the live bus as the baseline — the
     *     true rest level the knob owns, BEFORE the pedal dips it.
     *   • Engaged: drive vol = base × (1 − norm01).  base is frozen.
     *   • LIFT edge (engaged→rest): restore the bus fully to base.
     *   • At rest: keep base synced to the bus so MASTER H.Vol/S.Vol set the rest
     *     level and the pedal never fights the knob.
     * [DBEAM-VOL-DRIFT FIX] The follower's RELEASE is gradual, so norm01 crosses
     * the rest threshold while the bus is still a hair below base.  The old code
     * adopted that still-dipped value as the new baseline EVERY rest tick, so each
     * gesture shrank the level ~1 % (and a route/enable change mid-gesture left the
     * bus stuck low).  Edge-tracking captures the baseline once on PRESS and fully
     * restores it on LIFT — no cumulative drift, no stuck-low bus.
     * routeDbeamExpression() runs on Core 1 (laser task) only, so the per-target
     * engaged flags are single-threaded and need no atomics.                     */
    constexpr float kRestEps = 0.01f;   /* (name avoids the Xtensa EPS macro) */
    static bool s_volEngagedHarp = false;
    static bool s_volEngagedSeq  = false;
    if (toSeq) {
      if (norm01 <= kRestEps) {
        if (s_volEngagedSeq) {   /* lift edge → restore the full user level */
          mixSeqVol.store(dbeamVolBaseSeq.load(std::memory_order_relaxed),
                          std::memory_order_release);
          s_volEngagedSeq = false;
        }
        dbeamVolBaseSeq.store(mixSeqVol.load(std::memory_order_relaxed),
                              std::memory_order_relaxed);  /* knob owns rest level */
      } else {
        if (!s_volEngagedSeq) {  /* press edge → capture true (un-dipped) rest level */
          dbeamVolBaseSeq.store(mixSeqVol.load(std::memory_order_relaxed),
                                std::memory_order_relaxed);
          s_volEngagedSeq = true;
        }
        mixSeqVol.store(dbeamVolBaseSeq.load(std::memory_order_relaxed) * (1.0f - norm01),
                        std::memory_order_release);
      }
    } else {
      if (norm01 <= kRestEps) {
        if (s_volEngagedHarp) {  /* lift edge → restore the full user level */
          mixHarpVol.store(dbeamVolBaseHarp.load(std::memory_order_relaxed),
                           std::memory_order_release);
          s_volEngagedHarp = false;
        }
        dbeamVolBaseHarp.store(mixHarpVol.load(std::memory_order_relaxed),
                               std::memory_order_relaxed); /* knob owns rest level */
      } else {
        if (!s_volEngagedHarp) { /* press edge → capture true (un-dipped) rest level */
          dbeamVolBaseHarp.store(mixHarpVol.load(std::memory_order_relaxed),
                                 std::memory_order_relaxed);
          s_volEngagedHarp = true;
        }
        mixHarpVol.store(dbeamVolBaseHarp.load(std::memory_order_relaxed) * (1.0f - norm01),
                         std::memory_order_release);
      }
    }
    /* OLED dashboard bargraph reflects live D-BEAM volume (local display only). */
    displayDirty.store(true, std::memory_order_relaxed);
    break; }
  default: break;
  }
}

/* ─────────────────────────────────────────────────────────────────────────────
 * checkDbeamThreshold
 * [FIX-C] Reads g_last_good_data.acAmplitude (snapshot under patchMux)
 * instead of g_kalman_ac[].x (unprotected cross-core access).
 * ─────────────────────────────────────────────────────────────────────────────*/
uint8_t IRAM_ATTR checkDbeamThreshold(float thresholdMv, int stringIdx) {
  const int si = (stringIdx < 0)
      ? (int)(dbeamLastStringIdx.load(std::memory_order_relaxed) & 7u)
      : (stringIdx & 7);

  portENTER_CRITICAL(&patchMux);
  const float amplitude = g_last_good_data[si].acAmplitude;
  portEXIT_CRITICAL(&patchMux);

  return (amplitude < thresholdMv) ? LOW : HIGH;
}
