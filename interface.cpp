/* ═════════════════════════════════════════════════════════════════════════════
 * Octopus PRO XL v6.0.00 — Laser Harp Groovebox
 * © 2026 DIODAC ELECTRONICS / iSystem. All Rights Reserved.
 *
 * PROPRIETARY AND CONFIDENTIAL. Unauthorized copying, distribution, modification,
 * or use of this software or firmware, in whole or in part, is strictly prohibited
 * without prior written permission from DIODAC ELECTRONICS.
 * ═════════════════════════════════════════════════════════════════════════════
 * interface.cpp — v6.0.00  HARDWARE CONTROL SURFACE
 *
 * Changes vs v5.1.01:
 *
 *  [C1] mutateSynth REMOVED.  Was a local lambda that wrote to livePatch and
 *       atomics directly, bypassing the canonical applyHarpParam/applySeqParam
 *       path from patches.h.  All synth parameter edits now go through
 *       applyHarpParam(pIdx, v14) or applySeqParam(pIdx, v14), which write
 *       livePatch + atomic + userBank atomically in one critical section.
 *       Encoder delta is applied in v14 space using encodeHarpParam(pIdx).
 *
 *  [C2] case 5 SEQ SETUP uses applySeqBank / applySeqChain / applySeqLength /
 *       applySeqTranspose from patches.h — echo gap closed.
 *
 *  [C3] case 4 MIDI I/O — pitch-bend range/enable + wire MIDI channels.
 *       D-BEAM Route moved to D-BEAM menu (case 1, l2=6); PB echoes CMD 178/179.
 *
 *  [C4] case 7 "SEQ SETTINGS" (dead, 2-item duplicate of MIDI routing)
 *       replaced with "AUX FX" (14 items) covering:
 *         aux delay time/feedback, aux reverb size/damp (all 4 bus params)
 *         harp/seq insert dly+rev sends, harp/seq/drum FX-A/FX-B slots.
 *       This closes the masterAuxDlyFb and masterAuxRevDamp menu gaps.
 *
 *  [C5] case 9 DRUM KIT — 41 items (5 params × 8 drums + kit selector).
 *       Uses applyDrumParam() for tune/decay/vol/noise and applyDrumWave()
 *       for wave index — all canonical paths with echo.
 *
 *  [C6] case 2 MASTER: l2=18/19/20 = harp/seq/drum mute toggles.
 *       Uses applyMute(cmd, !current) from patches.h — echo included.
 *
 *  [C7] case 2 l2=5 (Master FX preset): masterFxIndex.store() was missing.
 *       Fixed — atomic now updated alongside fx.loadMasterFx() call.
 *
 *  [C8] Encoder read is PURE LINEAR 1:1 (no acceleration) via
 *       EncoderPoll.getDelta() — matches the known-good v28.9 build exactly.
 *
 *  [C9b] uiSyncPending deferred-recall mechanism RESTORED (smooth v28.9 feel).
 *       HARP dashboard IDLE-mode patch browse only bumps harpPatchIndex per
 *       detent (one relaxed store, OLED name tracks instantly); the heavy
 *       recallHarpPatch() (livePatch memcpy + atomic fan-out under patchMux)
 *       runs once, ~160 ms after the encoder rests.  This removes per-detent
 *       patchMux contention and the forced full-screen redraw stream that made
 *       fast scrolling stutter while buttons (single events) stayed instant.
 *
 *  [C10] #include "wires.h" removed — checkWireAuthority() comes from
 *        patches.h which absorbed wires.h.
 *
 * Retained fixes from v5.1.01: encodeMasterPitch/decodeMasterPitch range,
 * symmetric pitch-bend [FIX-B], static preset index [FIX-C],
 * display.begin() order [FIX-V], no spurious outer patchMux [FIX-X].
 * ═════════════════════════════════════════════════════════════════════════════ */
#include "interface.h"
#include <cmath>
#include "groovebox.h"       /* seq_start/stop/toggle, seq_toggle_recording,
                             *     setSequencerBpm, seqUI_*, loadFactory*Pattern */
#include "display.h"         /* display obj, handleTelemetryPageEncoder  */
#include "dbeam.h"           /* applyDbeamRouteHW                       */
#include "fog.h"             /* [FOG] fogRejectEnabled / fogRejectMargin */
#include "laser.h"
#include "harp.h"            /* harpSetPlayMode (HARP OC-cycle play mode) */
#include "effect.h"          /* fx, loadHarpFx, loadSeqFx, loadDrumFx   */
#include "settings.h"        /* saveSettingsSafe, resetToFactoryDefaults */
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "patches.h"         /* applyHarpParam, applySeqParam, applyDrumParam,
                                applyDrumWave, applySeqBank, applySeqChain,
                                applySeqLength, applySeqTranspose, applyMute,
                                applyDbeamRoute, applyDbeamTarget,
                                checkWireAuthority, encodeHarpParam, encodeSeqParam,
                                norm_to_v14, recallHarpPatch              */
#include "midi.h"            /* sendFullStateSync, txSysex                */
/* #include "wires.h" — removed [C10]: absorbed into patches.h           */

/* ── Object instantiations ─────────────────────────────────────────────────── */
ButtonPoll      btnEnc;
ButtonPoll      btnOC;
ButtonPoll      btnScale;
EncoderPoll     encPoll;
ESP32Encoder    encoder;           /* hardware quadrature encoder — used by getDelta() */
InterfaceStats  interface_stats  = {};
DisplayI2CStats display_stats    = {};

/* ═══════════════════════════════════════════════════════════════════════════
 * SECTION 1 — PITCH ENCODE / DECODE  (range fix from v5.1.01 preserved)
 * ═══════════════════════════════════════════════════════════════════════════ */
uint16_t encodeMasterPitch(float pitch) {
  pitch = std::min(MASTER_PITCH_MAX, std::max(MASTER_PITCH_MIN, pitch));
  /* [PD-2] Linear in semitones ±24 (±2 oct); unity (1.0×) at v14 = 8192. */
  const float semis = 12.0f * log2f(pitch);
  const float t     = (semis / 48.0f) + 0.5f;
  return (uint16_t)(std::min(1.0f, std::max(0.0f, t)) * 16383.0f + 0.5f);
}
float decodeMasterPitch(uint16_t v14) {
  if (v14 > 16383u) v14 = 16383u;
  const float semis = ((float)v14 / 16383.0f - 0.5f) * 48.0f;
  return std::min(MASTER_PITCH_MAX,
                  std::max(MASTER_PITCH_MIN, exp2f(semis / 12.0f)));
}

/* ═══════════════════════════════════════════════════════════════════════════
 * SECTION 2 — FAST GPIO INIT
 * ═══════════════════════════════════════════════════════════════════════════ */
void init_fast_gpio() {
  /* Output pins: PEAK_CLR, DAC_LDAC, DAC_CS */
  const gpio_config_t out = {
    .pin_bit_mask = (1ULL<<PIN_PEAK_CLR)|(1ULL<<PIN_DAC_LDAC)|(1ULL<<PIN_DAC_CS),
    .mode         = GPIO_MODE_OUTPUT,
    .pull_up_en   = GPIO_PULLUP_DISABLE,
    .pull_down_en = GPIO_PULLDOWN_DISABLE,
    .intr_type    = GPIO_INTR_DISABLE
  };
  gpio_config(&out);
  fastWrite(PIN_PEAK_CLR, HIGH);
  fastWrite(PIN_DAC_LDAC, HIGH);
  fastWrite(PIN_DAC_CS,   HIGH);

  /* Input pin: TRIGGER (PULLUP — laser beam-break input) */
  const gpio_config_t in = {
    .pin_bit_mask = (1ULL << PIN_TRIGGER),
    .mode         = GPIO_MODE_INPUT,
    .pull_up_en   = GPIO_PULLUP_ENABLE,
    .pull_down_en = GPIO_PULLDOWN_DISABLE,
    .intr_type    = GPIO_INTR_DISABLE
  };
  gpio_config(&in);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * SECTION 3 — HARDWARE INIT
 * ═══════════════════════════════════════════════════════════════════════════ */
void initHardwareInterface() {
  init_fast_gpio();

  btnEnc  .init(PIN_ENC_BTN);
  btnOC   .init(PIN_BTN_OC);
  btnScale.init(PIN_BTN_SCALE);

  /* Firmer debounce on the two physical action buttons — they were triggering
   * too easily/fast.  Encoder button stays at the default snappy debounce. */
  btnOC   .debounceMs = DEBOUNCE_SLOW_MS;
  btnScale.debounceMs = DEBOUNCE_SLOW_MS;

  delay(100);

  /* Factory-reset hold detection: both OC + SCALE held for 150 ms at boot */
  {
    bool combo = true;
    for (int i = 0; i < 30; ++i) {
      if (fastRead(PIN_BTN_OC) || fastRead(PIN_BTN_SCALE)) { combo = false; break; }
      delay(5);
    }
    if (combo) handleFactoryReset();
  }

  /* Encoder — ESP32Encoder PCNT peripheral (library ≥ 0.12, ESP32 Arduino 3.x).
   *
   * vs encoder_example.txt (interrupt demo):
   *   • Example: ESP32Encoder(true, enc_cb) + attachSingleEdge → 1 count/detent
   *     + optional ISR callback (LED toggle).  Counting is STILL done in PCNT
   *     hardware; enc_cb is optional notification, not the counter.
   *   • Ours:   ESP32Encoder() default + attachFullQuad → 4 counts/detent,
   *     divided by ENC_PPR (=4) in EncoderPoll::getDelta → same 1:1 feel.
   *   • We do NOT need always_interrupt=true: polling getCount() at 200 Hz is
   *     the library's intended slow-poll path; hardware accumulates between polls.
   *
   * Cross-core rule (library docs): pin the PCNT ISR service to the SAME core
   * that calls getCount().  control_surface_task lives on Core 0 (ControlPoll),
   * so isrServiceCpuCore must match a core that is NOT saturated by AudioSynth.
   * Core 1 hosts LaserSweep but the encoder ISR is microseconds — safe there and
   * it keeps PCNT ISR traffic off Core 0 (reduces ipc0 / audio-core contention). */
  ESP32Encoder::useInternalWeakPullResistors = puType::up;
  ESP32Encoder::isrServiceCpuCore            = 1u;   /* Core 1 — off the audio island */
  encoder.attachFullQuad((int)PIN_ENC_A, (int)PIN_ENC_B);
  encoder.setFilter(1023);                           /* max glitch filter */
  encoder.clearCount();
  encPoll.lastCount  = encoder.getCount();           /* int64 sync, not assumed 0 */
  encPoll.lastMoveMs = 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * SECTION 4 — PARAMETER MUTATION HELPERS
 *
 * These helpers work in 14-bit v14 space using encodeHarpParam/encodeSeqParam
 * from patches.h to get the current value, then call the canonical apply*
 * functions.  mutateSynth lambda is REMOVED — all paths go through patches.h.
 * ═══════════════════════════════════════════════════════════════════════════ */

/* stepForParam — return v14 step size for the given SynthParam index.
 * Index params (waveform, lfo_route, osc2_wave) step by 1 unit.
 * Float params step by ~1% per detent (164 in v14 = 0.01 normalised).    */
static inline int32_t stepForParam(int pIdx) {
  if (pIdx == (int)SynthParam::P_WAVEFORM  ||
      pIdx == (int)SynthParam::P_LFO_ROUTE ||
      pIdx == (int)SynthParam::P_OSC2_WAVE) return 1;
  return 164;   /* ~1% per detent in [0, 16383] space */
}

/* maxV14ForParam — upper bound in v14 space */
static inline uint16_t maxV14ForParam(int pIdx) {
  if (pIdx == (int)SynthParam::P_WAVEFORM  ||
      pIdx == (int)SynthParam::P_OSC2_WAVE)  return 24;
  if (pIdx == (int)SynthParam::P_LFO_ROUTE) return  7;
  return 16383;
}

/* mutateHarp — read current v14, apply delta, call applyHarpParam [C1]
 * [WS4] Hardware ALWAYS applies (live performance must work with the App open).
 * Wire echo is gated so the App is not spammed when it is the active editor.
 * LFO route echoes on cmd 11 (SynthParam index), not legacy 86 — matches App. */
static inline void mutateHarp(int pIdx, int16_t delta) {
  const uint8_t cmd = (uint8_t)(CMD_H_WAVE + pIdx);

  const int32_t step = stepForParam(pIdx);
  const uint16_t maxV = maxV14ForParam(pIdx);
  const uint16_t cur  = encodeHarpParam(pIdx);

  uint16_t v14;
  if (step == 1)
    v14 = (uint16_t)wrapIndex((int)cur, (int)delta, (int)maxV + 1);
  else
    v14 = (uint16_t)std::min<int32_t>(maxV, std::max<int32_t>(0,
                        (int32_t)cur + (int32_t)delta * step));

  applyHarpParam(pIdx, v14);   /* writes livePatch + atomic + userBank */
  if (checkWireAuthority(cmd, true))
    txSysex(cmd, v14);
}

/* mutateSeq — same for seq synth [C1] */
static inline void mutateSeq(int pIdx, int16_t delta) {
  const uint8_t cmd = (uint8_t)(CMD_S_WAVE + pIdx);

  const int32_t step = stepForParam(pIdx);
  const uint16_t maxV = maxV14ForParam(pIdx);
  const uint16_t cur  = encodeSeqParam(pIdx);

  uint16_t v14;
  if (step == 1)
    v14 = (uint16_t)wrapIndex((int)cur, (int)delta, (int)maxV + 1);
  else
    v14 = (uint16_t)std::min<int32_t>(maxV, std::max<int32_t>(0,
                        (int32_t)cur + (int32_t)delta * step));

  applySeqParam(pIdx, v14);
  if (checkWireAuthority(cmd, true))
    txSysex(cmd, v14);
}

/* hwKnobEchoCapture — menu encoder echo + P-lock capture (Fix 1 hardware path). */
static inline void hwKnobEchoCapture(uint8_t cmd, uint16_t v14) {
  txSysex(cmd, v14);
  captureMotionParam(cmd, v14);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * SECTION 5 — updateHardwareParameter
 *
 * Dispatches encoder delta to the correct SSOT parameter path.
 * l1 = menu category (0–15), l2 = item within category, delta = turn amount
 * (already velocity-scaled by EncoderPoll.getDelta).
 * ═══════════════════════════════════════════════════════════════════════════ */
void updateHardwareParameter(uint8_t l1, uint8_t l2, int16_t delta) {
  if (l1 >= (uint8_t)kL1Count || (int)l2 >= l2CountFor((int)l1)) return;

  const int scale = harpScaleIndex.load(std::memory_order_relaxed) & (NUM_SCALES - 1);

  switch (l1) {

  /* ── 0: HARP SETUP — beam + calibration ─────────────────────────────── */
  case 0:
    /* [FIX-ATOM-LOCK] l2=10/11/12 are std::atomic<> stores that don't need patchMux.
     * Split: non-atomic hw fields (0-8) under patchMux; atomic params (10-12) outside.
     * Case 7 (margin) still exits patchMux early to call computeHardwareDACThreshold
     * outside the lock (that function acquires patchMux itself).               */
    if (l2 >= 10) {
      /* Atomics — no spinlock required */
      switch (l2) {
      case 10: fogRejectEnabled.store(delta > 0 ? true : (delta < 0 ? false
                    : !fogRejectEnabled.load(std::memory_order_relaxed)),
                    std::memory_order_relaxed); break;
      case 11: fogRejectMargin.store(constrain(
                    fogRejectMargin.load(std::memory_order_relaxed) + delta * 25,
                    FOG_MARGIN_MIN, FOG_MARGIN_MAX), std::memory_order_relaxed); break;
      case 12: laserScreensaver.store(delta > 0 ? true : (delta < 0 ? false
                    : !laserScreensaver.load(std::memory_order_relaxed)),
                    std::memory_order_relaxed); break;
      }
      break;
    }
    portENTER_CRITICAL(&patchMux);
    switch (l2) {
    case 0: beamGateHoldMs = (uint32_t)constrain(
                (int32_t)beamGateHoldMs + delta * 25, 0, (int32_t)BEAM_GATE_HOLD_MAX); break;
    case 1: scaleWhiteLevel[scale]     = (uint8_t)constrain((int)scaleWhiteLevel[scale]     + delta*4, 0, 255); break;
    case 2: scaleTouchConfirm[scale]   = (uint8_t)constrain((int)scaleTouchConfirm[scale]   + delta,   CONFIRM_MIN, CONFIRM_MAX); break;
    case 3: scaleReleaseConfirm[scale] = (uint8_t)constrain((int)scaleReleaseConfirm[scale] + delta,   CONFIRM_MIN, CONFIRM_MAX); break;
    case 4: scaleR[scale] = (uint8_t)constrain((int)scaleR[scale] + delta*5, 0, 255); break;
    case 5: scaleG[scale] = (uint8_t)constrain((int)scaleG[scale] + delta*5, 0, 255); break;
    case 6: scaleB[scale] = (uint8_t)constrain((int)scaleB[scale] + delta*5, 0, 255); break;
    case 7: scaleMargin[scale] = (uint16_t)constrain((int)scaleMargin[scale] + delta*10, 0, 2000);
            portEXIT_CRITICAL(&patchMux);
            for (int i = 0; i < MAX_STRINGS; ++i) computeHardwareDACThreshold(i, 0.f);
            return;
    /* [STUCK-FIX] Anti-stuck fail-safe timeout (ms, 0 = disabled). Global, not per-scale. */
    case 8: beamStuckReleaseMs = (uint32_t)constrain(
                (int32_t)beamStuckReleaseMs + delta * 25, 0, (int32_t)BEAM_STUCK_RELEASE_MAX); break;
    }
    portEXIT_CRITICAL(&patchMux);
    break;

  /* ── 1: D-BEAM ───────────────────────────────────────────────────────── */
  case 1:
    switch (l2) {
    /* [WS4-FIX] Apply locally ALWAYS (live performance), then gate ONLY the wire
     * echo so a connected App can't fight the hardware.  Previously these did
     * `if (!checkWireAuthority(...)) return;` BEFORE the apply, so Offset/Range
     * silently refused to move whenever the App heartbeat was active — unlike
     * every other parameter (and contrary to the WS4 "apply then gate" rule). */
    case 0: dbeamHWCfg.offsetAdc = constrain(dbeamHWCfg.offsetAdc + delta*5, 0, 4095);
            if (checkWireAuthority(CMD_DB_OFFSET, true))
              txSysex(CMD_DB_OFFSET, (uint16_t)dbeamHWCfg.offsetAdc);
            break;
    case 1: dbeamHWCfg.rangeAdc  = constrain(dbeamHWCfg.rangeAdc + delta*10, 20, 4095);
            if (checkWireAuthority(CMD_DB_RANGE, true))
              txSysex(CMD_DB_RANGE, (uint16_t)dbeamHWCfg.rangeAdc);
            break;
    case 2: { uint8_t c = (uint8_t)wrapIndex((int)currentDbeamCurve.load(), (int)delta, 5);
              currentDbeamCurve.store((DBEAMCurve)c, std::memory_order_relaxed);
              txSysex(CMD_DB_CURVE, c); break; }
    case 3: { bool e = delta>0 ? true : (delta<0 ? false : !dbeamEnabled.load());
              dbeamEnabled.store(e, std::memory_order_release);
              txSysex(CMD_DB_ENABLED, e ? 16383u : 0u); break; }
    /* Branch B envelope tuning — local hardware knobs (no App sync needed). */
    case 4: { float v = dbeamExprAttack.load(std::memory_order_relaxed) + (float)delta*0.01f;
              v = std::min(DBEAM_EXPR_ATTACK_MAX, std::max(DBEAM_EXPR_ATTACK_MIN, v));
              dbeamExprAttack.store(v, std::memory_order_relaxed); break; }
    case 5: { float v = dbeamExprRelease.load(std::memory_order_relaxed) + (float)delta*0.001f;
              v = std::min(DBEAM_EXPR_RELEASE_MAX, std::max(DBEAM_EXPR_RELEASE_MIN, v));
              dbeamExprRelease.store(v, std::memory_order_relaxed); break; }
    /* [DBEAM] Route (moved here from MIDI) + Target synth selector. */
    case 6: { uint8_t m = (uint8_t)wrapIndex((int)currentDbeamRoute.load(), (int)delta, 4);
              applyDbeamRoute(m); break; }
    case 7: { uint8_t t = (uint8_t)wrapIndex((int)currentDbeamTarget.load(), (int)delta, 2);
              applyDbeamTarget(t);
              dbeamRefreshAfterTargetChange(); break; }
    }
    break;

  /* ── 2: MASTER — vols, pitch, EQ, inserts, mutes ────────────────────── */
  case 2:
    switch (l2) {
    /* [IDM-OPT3] every safe_atomic_store now returns the clamped value (nv);
     * txSysex uses it directly instead of reloading the atomic. */
    case 0:  { const float nv = safe_atomic_store(masterVol,   masterVol.load(),   (float)delta*0.01f, 0.f,1.f);
               applyMasterParam(CMD_M_VOL, (uint16_t)(nv*16383.f), Origin::HW); break; }
    case 1:  { const float nv = safe_atomic_store(mixHarpVol,  mixHarpVol.load(),  (float)delta*0.01f, 0.f,1.f);
               applyMasterParam(CMD_H_VOL, (uint16_t)(nv*16383.f), Origin::HW); break; }
    case 2:  { const float nv = safe_atomic_store(mixSeqVol,   mixSeqVol.load(),   (float)delta*0.01f, 0.f,1.f);
               applyMasterParam(CMD_S_VOL, (uint16_t)(nv*16383.f), Origin::HW); break; }
    case 3:  { const float nv = safe_atomic_store(mixDrumsVol, mixDrumsVol.load(), (float)delta*0.01f, 0.f,1.f);
               applyMasterParam(CMD_D_VOL, (uint16_t)(nv*16383.f), Origin::HW); break; }
    case 4:  { const float nv = safe_atomic_store(masterPitch, masterPitch.load(),
                               (float)delta*0.05f, MASTER_PITCH_MIN, MASTER_PITCH_MAX);
               applyMasterParam(CMD_PITCH, encodeMasterPitch(nv), Origin::HW); break; }
    case 5:  { int v = wrapIndex((int)masterFxIndex.load(), (int)delta, 16);
               masterFxIndex.store(v, std::memory_order_relaxed);
               fx.loadMasterFx(v); hwKnobEchoCapture(CMD_M_FX_IDX, (uint16_t)v);
               txMasterFxParams(); break; }
    case 6:  { const float nv = safe_atomic_store(drumRevSend, drumRevSend.load(), (float)delta*0.01f, 0.f,1.f);
               applyMasterParam(CMD_D_REV, (uint16_t)(nv*16383.f), Origin::HW); break; }
    case 7:  { const float nv = safe_atomic_store(drumDlySend, drumDlySend.load(), (float)delta*0.01f, 0.f,1.f);
               applyMasterParam(CMD_D_DLY, (uint16_t)(nv*16383.f), Origin::HW); break; }
    case 8:  { const float nv = safe_atomic_store(tbDrive, tbDrive.load(), (float)delta*0.01f, 0.f,1.f);
               applyMasterParam(CMD_TB_DRV, (uint16_t)(nv*16383.f), Origin::HW); break; }
    case 9:  { const float nv = safe_atomic_store(tbTone,  tbTone.load(),  (float)delta*0.01f, 0.f,1.f);
               applyMasterParam(CMD_TB_TONE, (uint16_t)(nv*16383.f), Origin::HW); break; }
    case 10: { const float nv = safe_atomic_store(tbMix,   tbMix.load(),   (float)delta*0.01f, 0.f,1.f);
               applyMasterParam(CMD_TB_MIX, (uint16_t)(nv*16383.f), Origin::HW); break; }
    case 11: { const float nv = safe_atomic_store(djFreq,  djFreq.load(),  (float)delta*0.01f, 0.f,1.f);
               applyMasterParam(CMD_DJ_FQ, (uint16_t)(nv*16383.f), Origin::HW); break; }
    case 12: { const float nv = safe_atomic_store(djRes,   djRes.load(),   (float)delta*0.01f, 0.f,1.f);
               applyMasterParam(CMD_DJ_RES, (uint16_t)(nv*16383.f), Origin::HW); break; }
    case 13: { const float nv = safe_atomic_store(djMix,   djMix.load(),   (float)delta*0.01f, 0.f,1.f);
               applyMasterParam(CMD_DJ_MIX, (uint16_t)(nv*16383.f), Origin::HW); break; }
    case 14: { const float nv = safe_atomic_store(masterEqLow,  masterEqLow.load(),  (float)delta*0.5f, -12.f,12.f);
               applyMasterParam(CMD_EQ_L, (uint16_t)(((nv+12.f)/24.f)*16383.f), Origin::HW); break; }
    case 15: { const float nv = safe_atomic_store(masterEqHigh, masterEqHigh.load(), (float)delta*0.5f, -12.f,12.f);
               applyMasterParam(CMD_EQ_H, (uint16_t)(((nv+12.f)/24.f)*16383.f), Origin::HW); break; }
    case 16: { int i = wrapIndex((int)drumFxIndexA.load(), (int)delta, 16);
               drumFxIndexA.store(i, std::memory_order_relaxed);
               loadDrumFx(i); hwKnobEchoCapture(CMD_D_FX_IDX, (uint16_t)i);
               echoDrumInsertParams(); break; }
    case 17: { int i = wrapIndex((int)drumFxIndexB.load(), (int)delta, 16);
               drumFxIndexB.store(i, std::memory_order_relaxed);
               loadDrumFxB(i); hwKnobEchoCapture(CMD_D_FX_IDX_B, (uint16_t)i); break; }
    /* [C6] Mute toggles: encoder turn or single click applies toggle */
    case 18: applyMute(CMD_H_MUTE, !mixHarpMute .load(std::memory_order_relaxed)); break;
    case 19: applyMute(CMD_S_MUTE, !mixSeqMute  .load(std::memory_order_relaxed)); break;
    case 20: applyMute(CMD_D_MUTE, !mixDrumsMute.load(std::memory_order_relaxed)); break;
    case 21: { const float nv = safe_atomic_store(mixHarpPan, mixHarpPan.load(), (float)delta * 0.02f, -1.f, 1.f);
               applyMasterParam(CMD_H_PAN, (uint16_t)((nv + 1.f) * 0.5f * 16383.f), Origin::HW); break; }
    case 22: { const float nv = safe_atomic_store(mixSeqPan, mixSeqPan.load(), (float)delta * 0.02f, -1.f, 1.f);
               applyMasterParam(CMD_S_PAN, (uint16_t)((nv + 1.f) * 0.5f * 16383.f), Origin::HW); break; }
    case 23: { const float nv = safe_atomic_store(mixDrumsPan, mixDrumsPan.load(), (float)delta * 0.02f, -1.f, 1.f);
               applyMasterParam(CMD_D_PAN, (uint16_t)((nv + 1.f) * 0.5f * 16383.f), Origin::HW); break; }
    }
    break;

  /* ── 3: HARP SYNTH — all 14 params + FX slots + sends ───────────────── */
  case 3:
    switch (l2) {
    /* SynthParam indices 0–13 (skip 11 which is lfo_route — handled below) */
    case 0:  case 1:  case 2:  case 3:  case 4:
    case 5:  case 6:  case 7:  case 8:  case 9:
    case 10: case 11: case 12: case 13:
      mutateHarp((int)l2, delta); break;
    case 14: { int i = wrapIndex((int)harpFxIndex.load(),  (int)delta, 16);
               harpFxIndex.store(i, std::memory_order_relaxed);
               loadHarpFx(i);  hwKnobEchoCapture(CMD_H_FX_IDX, (uint16_t)i);
               txInsertFxSends(0u); break; }
    case 15: { int i = wrapIndex((int)harpFxIndexB.load(), (int)delta, 16);
               harpFxIndexB.store(i, std::memory_order_relaxed);
               loadHarpFxB(i); hwKnobEchoCapture(CMD_H_FX_IDX_B, (uint16_t)i); break; }
    case 16: { portENTER_CRITICAL(&patchMux);
               float v = std::min(1.f, std::max(0.f, fx.harpInsert.dly_send + delta*0.02f));
               fx.harpInsert.dly_send = v;
               portEXIT_CRITICAL(&patchMux);
               applyFxSend(CMD_H_DLY_MIX, (uint16_t)(v*16383.f), Origin::HW); break; }
    case 17: { portENTER_CRITICAL(&patchMux);
               float v = std::min(1.f, std::max(0.f, fx.harpInsert.rev_send + delta*0.02f));
               fx.harpInsert.rev_send = v;
               portEXIT_CRITICAL(&patchMux);
               applyFxSend(CMD_H_REV_MIX, (uint16_t)(v*16383.f), Origin::HW); break; }
    case 18: { /* [WS5b] sound-bank preset recall (same 128-name bank as the
               * HARP dashboard browse).  Immediate recall — menu pace is slow
               * enough that the deferred-commit path is unnecessary here.      */
               const int cur = std::min(harpPatchIndex.load() & (NUM_PATCHES - 1),
                                        NUM_NAMED_PRESETS - 1);
               const int nx  = wrapIndex(cur, (int)delta, NUM_NAMED_PRESETS);
               recallHarpPatch(nx, ParamSource::UI); /* emits patch blob to App */
               txSysex(CMD_H_PATCH, (uint16_t)nx);   /* echo index → App dropdown */
               break; }
    /* [USER-SLOTS] 19 Save Slot — turn picks target slot (commit on ENC press);
     *              20 Load Slot — turn picks AND recalls the slot immediately.   */
    case 19: { uint8_t c = (uint8_t)wrapIndex((int)userSlotCursor[0].load(std::memory_order_relaxed),
                              (int)delta, NUM_USER_SLOTS);
               userSlotCursor[0].store(c, std::memory_order_relaxed);
               displayDirty.store(true, std::memory_order_relaxed); break; }
    case 20: { uint8_t c = (uint8_t)wrapIndex((int)userSlotCursor[0].load(std::memory_order_relaxed),
                              (int)delta, NUM_USER_SLOTS);
               userSlotCursor[0].store(c, std::memory_order_relaxed);
               loadUserSlotToLive(0u, c); break; }
    case 21: { const bool en = delta > 0 ? true : (delta < 0 ? false : !harpArpEnabled.load());
               applyHarpArpEnable(en ? 16383u : 0u); break; }
    case 22: { const int p = wrapIndex((int)harpArpPattern.load(), (int)delta, 4);
               applyHarpArpPattern((uint16_t)p); break; }
    case 23: { const int r = wrapIndex((int)harpArpRate.load(), (int)delta, 4);
               applyHarpArpRate((uint16_t)r); break; }
    case 24: { const int g = wrapIndex((int)harpArpGate.load(), (int)delta, 4);
               applyHarpArpGate((uint16_t)g); break; }
    }
    break;

  /* ── 4: MIDI I/O — pitch bend, channels (D-BEAM Route moved to D-BEAM) ─── */
  case 4:
    switch (l2) {
    /* Pitch bend (incoming USB MIDI) — echoes CMD_PB_RANGE / CMD_PB_ENABLE */
    case 0: { const uint8_t v = (uint8_t)constrain(
                (int)pbMapping.upSemi.load(std::memory_order_relaxed) + delta, 0, 24);
              pbMapping.upSemi.store(v, std::memory_order_relaxed);
              pbMapping.downSemi.store(v, std::memory_order_relaxed);
              if (isAppConnected()) txSysex(CMD_PB_RANGE, (uint16_t)v); break; }
    case 1: { /* [FIX-PB-FALL] break was inside the if block — if delta==0 execution
               * fell through to case 2 and silently corrupted wireHarpMidiChannel.
               * Break is now unconditional, outside the if body. */
              if (delta != 0) {
                const bool en = !pbMapping.enabled.load(std::memory_order_relaxed);
                pbMapping.enabled.store(en, std::memory_order_relaxed);
                if (isAppConnected()) txSysex(CMD_PB_ENABLE, en ? 16383u : 0u);
              }
              break; }
    case 2: { uint8_t v = (uint8_t)(wrapIndex((int)wireHarpMidiChannel.load()-1,(int)delta,16)+1);
              wireHarpMidiChannel.store(v, std::memory_order_release);
              txSysex(CMD_WIRE_HARP_CH, (uint16_t)v); break; }
    case 3: { uint8_t v = (uint8_t)(wrapIndex((int)wireSeqMidiChannel.load() -1,(int)delta,16)+1);
              wireSeqMidiChannel.store(v, std::memory_order_release);
              txSysex(CMD_WIRE_SEQ_CH,  (uint16_t)v); break; }
    case 4: { uint8_t v = (uint8_t)(wrapIndex((int)wireDrumMidiChannel.load()-1,(int)delta,16)+1);
              wireDrumMidiChannel.store(v, std::memory_order_release);
              txSysex(CMD_WIRE_DRUM_CH, (uint16_t)v); break; }
    }
    break;

  /* ── 5: SEQ SETUP — bank, chain, length, transpose, load presets ──────── */
  /* [C2] All changes use applySeq* from patches.h — echo gap closed.        */
  case 5:
    switch (l2) {
    /* [V5.3-CONS] Bank = A-D (0-3); item 1 = synth/drum view PAGE (chain pinned 0) */
    case 0: applySeqBank   ((uint8_t)wrapIndex((int)seqActiveBank.load(), (int)delta, 4)); break;
    case 1: seqUI_page.store(wrapIndex((int)seqUI_page.load(), (int)delta, 2),
                            std::memory_order_relaxed);
            displayDirty.store(true, std::memory_order_relaxed); break;
    /* [TRANSPOSE-FIX] Clamp the SIGNED target to ±12 BEFORE handing it to
     * applySeqTranspose (which expects v14 = transpose+12).  The old
     * (uint16_t)(cur+delta+12) cast turned a sub-zero step at −12 into a huge
     * value that clamped to +12 — a boundary wrap. */
    case 2: { const int t = std::min(12, std::max(-12, (int)seqTranspose.load() + (int)delta));
              applySeqTranspose((uint16_t)(t + 12)); break; }
    case 3: applySeqLength   ((uint16_t)((int)seqLength.load() + delta));    break;
    /* [SEQ-LOAD-FIX] Read the live preset index (single source of truth) instead
     * of a private static — keeps the readout honest and scrolling continuous
     * even after an App-driven load. loadFactory* stores the new index itself. */
    case 4: { const int cur  = (int)g_lastSynthPreset.load(std::memory_order_relaxed);
              const int next = wrapIndex(cur, (int)delta, NUM_SYNTH_PATS);
              loadFactorySynthPattern(seqActiveBank.load() & 15u, SEQ_UI_CHAIN, next); break; }
    case 5: { const int cur  = (int)g_lastDrumPreset.load(std::memory_order_relaxed);
              const int next = wrapIndex(cur, (int)delta, NUM_DRUM_PATS);
              loadFactoryDrumPattern(seqActiveBank.load() & 15u, SEQ_UI_CHAIN, next); break; }
    case 6: break;  /* [SEQ-CLEAR] action item — handled on ENC click (opens confirm) */
    case 7: break;  /* [SOFT-RESET] action item */
    case 8: { uint8_t c = (uint8_t)wrapIndex((int)userPatCursor.load(std::memory_order_relaxed),
                              (int)delta, NUM_USER_PAT_SLOTS);
              userPatCursor.store(c, std::memory_order_relaxed);
              displayDirty.store(true, std::memory_order_relaxed); break; }
    /* [USER-PAT-SLOTS] 9 Load Pat — turn picks AND recalls immediately (mirror Load Slot). */
    case 9: { uint8_t c = (uint8_t)wrapIndex((int)userPatCursor.load(std::memory_order_relaxed),
                              (int)delta, NUM_USER_PAT_SLOTS);
              userPatCursor.store(c, std::memory_order_relaxed);
              loadUserPatternToActive(c); break; }
    case 10: { const bool en = delta > 0 ? true : (delta < 0 ? false : !seqArpEnabled.load());
               applySeqArpEnable(en ? 16383u : 0u); break; }
    case 11: { const int p = wrapIndex((int)seqArpPattern.load(), (int)delta, 8);
               applySeqArpPattern((uint16_t)p); break; }
    case 12: { const int r = wrapIndex((int)seqArpRate.load(), (int)delta, 8);
               applySeqArpRate((uint16_t)r); break; }
    case 13: { const int g = wrapIndex((int)seqArpGate.load(), (int)delta, 8);
               applySeqArpGate((uint16_t)g); break; }
    }
    break;

  /* ── 6: SEQ MATRIX — grid navigation is intercepted in updateHardwareInterface
   * before reaching here (l1==6 handler returns early when mstate != MENU_L1).
   * This case is unreachable in normal operation; kept as a safe no-op. */
  case 6:
    break;

  /* ── 7: AUX FX — [C4] replaces dead SEQ SETTINGS ─────────────────────── */
  /* Bus params use applyAuxParam (patches.h) for proper echo.               */
  case 7:
    switch (l2) {
    /* Aux delay bus — applyAuxParam keeps hardware + App in sync */
    case 0: { const float nv = std::min(1.5f, std::max(0.f, masterAuxDlyTime.load() + delta*0.02f));
              applyAuxParam(CMD_H_FX_TIME, norm_to_v14(nv/1.5f)); break; }
    case 1: applyAuxParam(CMD_AUX_DLY_FB,  (uint16_t)std::min<int32_t>(16383, std::max<int32_t>(0,
                (int32_t)norm_to_v14(masterAuxDlyFb.load()/0.95f) + delta*164))); break;
    /* Aux reverb bus */
    case 2: { const float nv = std::min(0.95f, std::max(0.f, masterAuxRevSize.load() + delta*0.01f));
              applyAuxParam(CMD_H_FX_SIZE, norm_to_v14(nv/0.95f)); break; }
    case 3: applyAuxParam(CMD_AUX_REV_DMP, (uint16_t)std::min<int32_t>(16383, std::max<int32_t>(0,
                (int32_t)norm_to_v14(masterAuxRevDamp.load()) + delta*164)));     break;
    /* Harp/seq insert sends */
    case 4: { portENTER_CRITICAL(&patchMux);
               float v = std::min(1.f, std::max(0.f, fx.harpInsert.dly_send + delta*0.02f));
               fx.harpInsert.dly_send = v; portEXIT_CRITICAL(&patchMux);
               applyFxSend(CMD_H_DLY_MIX, (uint16_t)(v*16383.f), Origin::HW); break; }
    case 5: { portENTER_CRITICAL(&patchMux);
               float v = std::min(1.f, std::max(0.f, fx.harpInsert.rev_send + delta*0.02f));
               fx.harpInsert.rev_send = v; portEXIT_CRITICAL(&patchMux);
               applyFxSend(CMD_H_REV_MIX, (uint16_t)(v*16383.f), Origin::HW); break; }
    case 6: { portENTER_CRITICAL(&patchMux);
               float v = std::min(1.f, std::max(0.f, fx.seqInsert.dly_send + delta*0.02f));
               fx.seqInsert.dly_send = v; portEXIT_CRITICAL(&patchMux);
               applyFxSend(CMD_S_DLY_MIX, (uint16_t)(v*16383.f), Origin::HW); break; }
    case 7: { portENTER_CRITICAL(&patchMux);
               float v = std::min(1.f, std::max(0.f, fx.seqInsert.rev_send + delta*0.02f));
               fx.seqInsert.rev_send = v; portEXIT_CRITICAL(&patchMux);
               applyFxSend(CMD_S_REV_MIX, (uint16_t)(v*16383.f), Origin::HW); break; }
    /* FX slot indices */
    case 8:  { int i = wrapIndex((int)harpFxIndex.load(),  (int)delta,16);
               harpFxIndex.store(i, std::memory_order_relaxed);
               loadHarpFx(i);  hwKnobEchoCapture(CMD_H_FX_IDX, (uint16_t)i);
               txInsertFxSends(0u); break; }
    case 9:  { int i = wrapIndex((int)harpFxIndexB.load(), (int)delta,16);
               harpFxIndexB.store(i, std::memory_order_relaxed);
               loadHarpFxB(i); hwKnobEchoCapture(CMD_H_FX_IDX_B, (uint16_t)i); break; }
    case 10: { int i = wrapIndex((int)seqFxIndex.load(),   (int)delta,16);
               seqFxIndex.store(i, std::memory_order_relaxed);
               loadSeqFx(i);   hwKnobEchoCapture(CMD_S_FX_IDX, (uint16_t)i);
               txInsertFxSends(1u); break; }
    case 11: { int i = wrapIndex((int)seqFxIndexB.load(),  (int)delta,16);
               seqFxIndexB.store(i, std::memory_order_relaxed);
               loadSeqFxB(i);  hwKnobEchoCapture(CMD_S_FX_IDX_B, (uint16_t)i); break; }
    case 12: { int i = wrapIndex((int)drumFxIndexA.load(), (int)delta,16);
               drumFxIndexA.store(i, std::memory_order_relaxed);
               loadDrumFx(i); hwKnobEchoCapture(CMD_D_FX_IDX, (uint16_t)i);
               echoDrumInsertParams(); break; }
    case 13: { int i = wrapIndex((int)drumFxIndexB.load(), (int)delta,16);
               drumFxIndexB.store(i, std::memory_order_relaxed);
               loadDrumFxB(i); hwKnobEchoCapture(CMD_D_FX_IDX_B, (uint16_t)i); break; }
    }
    break;

  /* ── 8: SEQ SYNTH — mirror of case 3 for seq engine ──────────────────── */
  case 8:
    switch (l2) {
    case 0:  case 1:  case 2:  case 3:  case 4:
    case 5:  case 6:  case 7:  case 8:  case 9:
    case 10: case 11: case 12: case 13:
      mutateSeq((int)l2, delta); break;
    case 14: { int i = wrapIndex((int)seqFxIndex.load(),  (int)delta,16);
               seqFxIndex.store(i, std::memory_order_relaxed);
               loadSeqFx(i);  hwKnobEchoCapture(CMD_S_FX_IDX, (uint16_t)i);
               txInsertFxSends(1u); break; }
    case 15: { int i = wrapIndex((int)seqFxIndexB.load(), (int)delta,16);
               seqFxIndexB.store(i, std::memory_order_relaxed);
               loadSeqFxB(i); hwKnobEchoCapture(CMD_S_FX_IDX_B, (uint16_t)i); break; }
    case 16: { portENTER_CRITICAL(&patchMux);
               float v = std::min(1.f, std::max(0.f, fx.seqInsert.dly_send + delta*0.02f));
               fx.seqInsert.dly_send = v; portEXIT_CRITICAL(&patchMux);
               applyFxSend(CMD_S_DLY_MIX, (uint16_t)(v*16383.f), Origin::HW); break; }
    case 17: { portENTER_CRITICAL(&patchMux);
               float v = std::min(1.f, std::max(0.f, fx.seqInsert.rev_send + delta*0.02f));
               fx.seqInsert.rev_send = v; portEXIT_CRITICAL(&patchMux);
               applyFxSend(CMD_S_REV_MIX, (uint16_t)(v*16383.f), Origin::HW); break; }
    case 18: { /* [WS5b] SEQ MELODY sound-bank preset recall — the path the seq
               * engine was missing on hardware.  Browses the 128-name bank and
               * recalls from seqBank[] (recallSeqPatch emits the seq patch blob). */
               const int cur = std::min(seqPatchIndex.load() & (NUM_PATCHES - 1),
                                        NUM_NAMED_PRESETS - 1);
               const int nx  = wrapIndex(cur, (int)delta, NUM_NAMED_PRESETS);
               recallSeqPatch(nx, ParamSource::UI);
               txSysex(CMD_S_PATCH, (uint16_t)nx);   /* echo index → App dropdown */
               break; }
    /* [USER-SLOTS] mirror of HARP SYNTH 19/20 for the seq engine (engine 1). */
    case 19: { uint8_t c = (uint8_t)wrapIndex((int)userSlotCursor[1].load(std::memory_order_relaxed),
                              (int)delta, NUM_USER_SLOTS);
               userSlotCursor[1].store(c, std::memory_order_relaxed);
               displayDirty.store(true, std::memory_order_relaxed); break; }
    case 20: { uint8_t c = (uint8_t)wrapIndex((int)userSlotCursor[1].load(std::memory_order_relaxed),
                              (int)delta, NUM_USER_SLOTS);
               userSlotCursor[1].store(c, std::memory_order_relaxed);
               loadUserSlotToLive(1u, c); break; }
    }
    break;

  /* ── 9: DRUM KIT — 5 params × 8 drums = 40 items + Kit (40) + Pitch (41) ─
   * l2 = drum_ch * 5 + param_idx
   * param 0=tune  1=decay  2=vol  3=noise  4=wave
   * All float params use applyDrumParam (canonical path).
   * Wave uses applyDrumWave (wraps through 25 waveforms).                 */
  case 9: {
    if ((int)l2 == 40) { /* Drum Kit selector — load tuning + set character + echo */
      const uint8_t cur = drumKit.load(std::memory_order_relaxed);
      applyDrumKit((uint8_t)wrapIndex((int)cur, (int)delta, (int)DrumKitId::COUNT), true);
      break;
    }
    if ((int)l2 == 41) { /* Drum global pitch — independent of M.TUNE */
      const float nv = safe_atomic_store(drumPitchMult, drumPitchMult.load(std::memory_order_relaxed),
                                         (float)delta * 0.05f, MASTER_PITCH_MIN, MASTER_PITCH_MAX);
      txSysex(CMD_DRUM_PITCH, encodeMasterPitch(nv));
      break;
    }
    const int ch    = (int)l2 / 5;
    const int param = (int)l2 % 5;
    if (ch >= 8) break;
    if (param < 4) {
      /* Params 0-3: tune/decay/vol/noise — float [0,1] → v14 */
      const int dIdx = ch * 4 + param;
      const uint8_t cmd = (uint8_t)(32 + dIdx);
      /* Read current v14 from atom */
      const std::atomic<float>* srcs[4] = {
        &drumTune[ch], &drumDecay[ch], &drumVolume[ch], &drumNoiseMix[ch]
      };
      const uint16_t cur = norm_to_v14(srcs[param]->load(std::memory_order_relaxed));
      const uint16_t v14 = (uint16_t)std::min<int32_t>(16383,
                              std::max<int32_t>(0, (int32_t)cur + delta*164));
      applyDrumParam(dIdx, v14);
      if (checkWireAuthority(cmd, true))
        txSysex(cmd, v14);
    } else {
      /* Param 4: wave index — wraps 0-24 */
      if (ch == 2 || ch == 3 || ch == 4) break; /* CLAP/HAT = noise only */
      const uint8_t cur = drumWaveIdx[ch].load(std::memory_order_relaxed);
      applyDrumWave(ch, (uint8_t)wrapIndex((int)cur, (int)delta, NUM_WAVE_TABLES));
    }
    break;
  }

  /* ── 10: LASER SHOW ──────────────────────────────────────────────────────
   * [LASER-SHOW v2] Show Mode and MIDI→Hue are INDEPENDENT toggles (match the
   * App's two buttons).  HUE ADSR times scale to SECONDS (ATK 0..2, DEC 0..3,
   * REL 0..4) and the SysEx echo normalises back to 0..16383.  Screensaver moved
   * to HARP SETUP (it is a closed-harp idle behaviour, not a show control).     */
  case 10:
    switch (l2) {
    case 0: { bool v = delta>0?true:(delta<0?false:!laserShowMode.load());
              laserShowMode.store(v, std::memory_order_release);
              txSysex(CMD_LSR_SHOW, v?16383u:0u); break; }
    case 1: { bool v = delta>0?true:(delta<0?false:!midiHueControl.load());
              midiHueControl.store(v, std::memory_order_release);
              txSysex(CMD_MIDI_HUE, v?16383u:0u); break; }
    case 2: { const float nv = safe_atomic_store(laserBaseHue,laserBaseHue.load(),(float)delta*0.01f,0.f,1.f);
              txSysex(CMD_HUE_BASE,(uint16_t)(nv*16383.f)); break; }
    case 3: { int m = wrapIndex((int)(uint8_t)laserShowAnim.load(), (int)delta, 4);
              laserShowAnim.store((LaserShowAnim)m, std::memory_order_release);
              txSysex(CMD_LSR_ANIM,(uint16_t)(((float)m/3.f)*16383.f)); break; }
    case 4: { const float nv = safe_atomic_store(laserDrumFlash,laserDrumFlash.load(),(float)delta*0.01f,0.f,1.f);
              txSysex(CMD_LSR_DRUMFLASH,(uint16_t)(nv*16383.f)); break; }
    case 5: { const float nv = safe_atomic_store(hueAttack, hueAttack.load(), (float)delta*0.02f,0.005f,HUE_ATK_MAX_S);
              txSysex(CMD_HUE_ATK,(uint16_t)((nv/HUE_ATK_MAX_S)*16383.f)); break; }
    case 6: { const float nv = safe_atomic_store(hueDecay,  hueDecay.load(),  (float)delta*0.03f,0.005f,HUE_DEC_MAX_S);
              txSysex(CMD_HUE_DEC,(uint16_t)((nv/HUE_DEC_MAX_S)*16383.f)); break; }
    case 7: { const float nv = safe_atomic_store(hueSustain,hueSustain.load(),(float)delta*0.01f,0.f,1.f);
              txSysex(CMD_HUE_SUS,(uint16_t)(nv*16383.f)); break; }
    case 8: { const float nv = safe_atomic_store(hueRelease,hueRelease.load(),(float)delta*0.04f,0.005f,HUE_REL_MAX_S);
              txSysex(CMD_HUE_REL,(uint16_t)((nv/HUE_REL_MAX_S)*16383.f)); break; }
    }
    break;

  case 11: break;  /* TELEMETRY — view-only; encoder handled in updateHardwareInterface */
  default: break;
  }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * SECTION 6 — ENCODER BUTTON CLASSIFIER  (delta-cancel guard preserved)
 * ═══════════════════════════════════════════════════════════════════════════ */
BtnEvent pollEncoderButton(uint32_t now, int32_t delta) {
  return btnEnc.poll(now, ENC_LONG_MS, ENC_DBL_MS, delta);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * SECTION 7 — MAIN CONTROL LOOP (200 Hz)
 * ═══════════════════════════════════════════════════════════════════════════ */

/* [CONFIRM] Run the action a confirm dialog was raised for (YES path).  Central
 * dispatch so the modal stays generic and reusable (RESET / SAVE can add cases). */
static void confirmDispatch(ConfirmAction a, uint8_t arg) {
  switch (a) {
    case ConfirmAction::SEQ_CLEAR:  seqClearActiveAndResetSounds();              break;
    case ConfirmAction::SAVE:       handleScopedSave ((ResetScope)(arg & 3u));   break;
    case ConfirmAction::RESET:      handleScopedReset((ResetScope)(arg & 3u));   break;
    case ConfirmAction::LOAD:       handleScopedLoad ((ResetScope)(arg & 3u));   break;
    case ConfirmAction::SOFT_RESET: seqSoftResetWorkingImage();                  break;
    case ConfirmAction::USR_SOUND_SAVE:                                          /* [USER-SLOTS] */
      if (saveLiveToUserSlot((uint8_t)((arg >> 6) & 1u), (uint8_t)(arg & 63u)) && isAppConnected())
        txSysex(CMD_USR_SOUND_SAVE, (uint16_t)(arg & 0x3FFFu));
      break;
    case ConfirmAction::USR_PAT_SAVE:                                            /* [USER-PAT-SLOTS] */
      saveActivePatternToUserSlot((uint8_t)(arg & 63u));                         break;
    case ConfirmAction::NONE:
    default: break;
  }
}

/* [SAVE-MENU] Open the scoped SAVE menu (L1=14).  Long-press never persists
 * directly any more — it always lands here so the user picks a scope and the
 * YES/NO confirm guards the actual write.  Clears any editor/scope overlay so
 * the menu is the front-most surface. */
static inline void openSaveMenu() {
  currentScopeView.store(TelemetryView::OFF, std::memory_order_relaxed);
  edgeEditOpen.store(false, std::memory_order_relaxed);
  currentMenuL1.store(14, std::memory_order_relaxed);   /* SAVE category (raw id) */
  currentMenuL2.store(0,  std::memory_order_relaxed);
  menuState.store(MenuState::MENU_L2, std::memory_order_relaxed);
  displayDirty.store(true, std::memory_order_relaxed);
}

void updateHardwareInterface() {
  const uint32_t now = millis();
  btnEnc  .update(now);
  btnOC   .update(now);
  btnScale.update(now);

  /* [SAVE-FIX3] Freeze the control surface while a session save is in flight.
   * ControlPoll (Core 0) was still toggling grid steps / mutating userBank via
   * patchMux while NvsWorker (Core 1) snapshotted patterns/banks → corruption
   * or TWDT panic.  Keep debounce state fresh; skip all gesture handling. */
  static uint32_t s_saveStuckSince = 0u;
  if (saveInProgress()) {
    if (s_saveStuckSince == 0u) s_saveStuckSince = now;
    else {
      /* FULL scoped reset can write ~37 KB — allow longer than a normal save. */
      const uint32_t limitMs = g_resetInProgress.load(std::memory_order_relaxed)
                                   ? 45000u : 25000u;
      if ((uint32_t)(now - s_saveStuckSince) > limitMs) {
        saveForceUnlock();
        s_saveStuckSince = 0u;
        g_saveFailFlashMs.store(now + 1500u, std::memory_order_relaxed);
        displayDirty.store(true, std::memory_order_relaxed);
        Serial.println(F("[NVS] WARN: save handshake timeout — unlocked"));
      }
    }
    return;
  }
  s_saveStuckSince = 0u;

  /* ── Encoder read — PURE LINEAR 1:1 (no acceleration), matches the
   *    known-good v28.9 build's getDelta math exactly [C8] ──────────────── */
  const int16_t delta = encPoll.getDelta(encoder.getCount(), now);
  if (delta != 0) lastEncoderTurnMs.store(now, std::memory_order_relaxed);

  /* ── [C9b] Deferred HARP recall commit ──────────────────────────────────
   * While the encoder is being turned we only update harpPatchIndex (cheap).
   * Once it has rested ~160 ms we run the one heavy recallHarpPatch() that
   * does the livePatch memcpy + atomic fan-out under patchMux.  Doing this
   * exactly once per "scroll gesture" instead of once per detent is what
   * keeps fast browsing smooth and lock-free.                               */
  if (uiSyncPending.load(std::memory_order_relaxed) &&
      (uint32_t)(now - lastEncoderTurnMs.load(std::memory_order_relaxed)) > 160u) {
    uiSyncPending.store(false, std::memory_order_relaxed);
    if (activeDashboard.load() == DashboardMode::HARP)
      recallHarpPatch(harpPatchIndex.load(std::memory_order_relaxed), ParamSource::UI);
  }

  const BtnEvent  ev       = pollEncoderButton(now, (int32_t)delta);
  /* OC + SCALE never emit DOUBLE → fire SINGLE instantly on release (no wait). */
  const BtnEvent  evOC     = btnOC.poll(now, OC_LONG_MS, 250u, 0, false);
  const BtnEvent  evScale  = btnScale.poll(now, SCALE_LONG_MS, 200u, 0, false);
  const bool      scopeOn  = (currentScopeView.load() != TelemetryView::OFF);
  const int       l1       = currentMenuL1.load(std::memory_order_relaxed);
  const MenuState mstate   = menuState.load(std::memory_order_relaxed);

  /* ── [v6.0] APP-CONNECTED control surface ───────────────────────────────────
   * While the App is connected the OLED shows the static "APP CONNECTED" splash
   * and the App is master for all PARAMETER editing.  The physical surface is
   * locked to a fixed TRANSPORT role (the hardware ALWAYS owns transport), so
   * the buttons can never fall dead and the App can't fight over shared state:
   *   SCALE single   → play / stop        (seq_toggle echoes CMD_TRANSPORT)
   *   OC single      → record-arm toggle  (seq_toggle_recording → ring echo 3/4)
   *   ENC turn       → BPM                 (setSequencerBpm echoes CMD_BPM)
   *   ENC long-press → save settings (NVS)
   * Menu navigation + parameter editing are suppressed (early return).  Gated on
   * isAppConnected() — the SAME predicate that draws the splash — so the control
   * mode and the screen are always in lock-step.                                */
  /* ── [CONFIRM] YES/NO modal — owns ALL input while open ─────────────────────
   * Placed before the App-connected transport branch so the modal still receives
   * encoder/click input (and renders over the splash) when the App is connected.
   *   ENC turn  → move selection (right = YES, left = NO)
   *   ENC click → commit (runs confirmDispatch on YES); closes either way
   *   ENC double→ cancel.  Defaults to NO so a stray click can't wipe.          */
  if (confirmOpen.load(std::memory_order_relaxed)) {
    if      (delta > 0) { confirmSel.store(1, std::memory_order_relaxed); displayDirty.store(true); }
    else if (delta < 0) { confirmSel.store(0, std::memory_order_relaxed); displayDirty.store(true); }
    if (ev == BtnEvent::SINGLE) {
      const bool          yes = confirmSel.load(std::memory_order_relaxed) != 0;
      const ConfirmAction a   = confirmActionId.load(std::memory_order_relaxed);
      const uint8_t       arg = confirmArg.load(std::memory_order_relaxed);
      confirmOpen    .store(false, std::memory_order_relaxed);
      confirmActionId.store(ConfirmAction::NONE, std::memory_order_relaxed);
      if (yes) confirmDispatch(a, arg);
      displayDirty.store(true);
    } else if (ev == BtnEvent::DOUBLE) {            /* cancel */
      confirmOpen    .store(false, std::memory_order_relaxed);
      confirmActionId.store(ConfirmAction::NONE, std::memory_order_relaxed);
      displayDirty.store(true);
    }
    return;
  }

  if (isAppConnected()) {
    bool dirty = false;
    if (ev == BtnEvent::LONG)                 /* ENC long → protected FULL save (App owns scope UI) */
      openConfirm(ConfirmAction::SAVE, (uint8_t)ResetScope::FULL);
    if (delta != 0) {                           /* ENC turn  → BPM */
      setSequencerBpm((int32_t)constrain(seqBpm.load() + delta, 40, 240));
      dirty = true;
    }
    if (evScale == BtnEvent::SINGLE) {          /* SCALE     → play/stop */
      seq_toggle();                             /* echoes CMD_TRANSPORT itself */
      dirty = true;
    }
    if (evOC == BtnEvent::SINGLE)               /* OC short  → record toggle */
      seq_toggle_recording();
    if (dirty) displayDirty.store(true, std::memory_order_relaxed);
    return;
  }

  /* ── [EDGE] Edge-comp 8-bar editor (full-screen, telemetry-style) ───────────
   * Entered from HARP SETUP → "Edge Comp".  Owns ALL input while open:
   *   SCALE single → select next string (0→7, wraps)
   *   OC single    → select next scale (0→15, wraps) AND apply it live so margin,
   *                  RGB, touch, and edge-comp rows all match the scale under edit
   *   ENC turn     → set selected string's edge-comp % (EDGE_COMP_PCT_MIN..MAX)
   *   ENC single   → reset selected string to its factory value
   *   ENC double   → exit (no save)   ENC long → save + exit
   * edgeComp[] is shared with Core 1 (computeHardwareDACThreshold) → writes are
   * done under patchMux; thresholds recompute OUTSIDE the lock (compute fn takes
   * patchMux itself, same as the Margin case).                                */
  if (edgeEditOpen.load(std::memory_order_relaxed)) {
    int  sel       = edgeEditSel.load(std::memory_order_relaxed) & 7;
    int  escale    = edgeEditScale.load(std::memory_order_relaxed) & (NUM_SCALES - 1);
    bool dirty     = false;
    bool recompute = false;
    /* [EDGE-NAV] Edit whichever set the REVIEWED scale uses (rainbow has its own).
     * OC scrolls scales AND selects them live (harpScaleIndex) so per-scale margin,
     * RGB, touch, and edge-comp values all apply while tuning. */
    uint8_t* const       edge    = edgeCompFor(escale);
    const uint8_t* const factory = edgeFactoryFor(escale);

    if (ev == BtnEvent::DOUBLE) {                 /* exit, no save */
      edgeEditOpen.store(false, std::memory_order_relaxed);
      displayDirty.store(true, std::memory_order_relaxed);
      return;
    }
    if (ev == BtnEvent::LONG) {                    /* exit editor → SAVE menu (protected) */
      edgeEditOpen.store(false, std::memory_order_relaxed);
      openSaveMenu();
      return;
    }
    if (evScale == BtnEvent::SINGLE) {             /* next string, wrap */
      sel = (sel + 1) & 7;
      edgeEditSel.store(sel, std::memory_order_relaxed);
      dirty = true;
    }
    if (evOC == BtnEvent::SINGLE) {                /* [EDGE-NAV] next scale, wrap + live select */
      escale = (escale + 1) & (NUM_SCALES - 1);
      edgeEditScale.store(escale, std::memory_order_relaxed);
      harpScaleIndex.store(escale, std::memory_order_relaxed);
      txSysex(CMD_H_SCALE, (uint16_t)escale);
      g_beamClearReq.store(true, std::memory_order_release);
      recompute = dirty = true;
    }
    if (ev == BtnEvent::SINGLE) {                  /* reset selected to factory */
      portENTER_CRITICAL(&patchMux);
      edge[sel] = factory[sel];
      portEXIT_CRITICAL(&patchMux);
      recompute = dirty = true;
    }
    if (delta != 0) {                              /* adjust selected level */
      portENTER_CRITICAL(&patchMux);
      edge[sel] = (uint8_t)constrain((int)edge[sel] + (int)delta,
                          (int)EDGE_COMP_PCT_MIN, (int)EDGE_COMP_PCT_MAX);
      portEXIT_CRITICAL(&patchMux);
      recompute = dirty = true;
    }
    if (recompute)
      for (int i = 0; i < MAX_STRINGS; ++i) computeHardwareDACThreshold(i, 0.f);
    if (dirty) displayDirty.store(true, std::memory_order_relaxed);
    return;
  }

  /* ── SCALE long: toggle dashboard ─────────────────────────────────────── */
  if (evScale == BtnEvent::LONG) {
    activeDashboard.store(
      activeDashboard.load() == DashboardMode::HARP
      ? DashboardMode::SEQUENCER : DashboardMode::HARP);
    menuState.store(MenuState::IDLE);
    displayDirty.store(true);
  }

  /* ── SEQ MATRIX grid editor (l1=6, already inside L2) ──────────────────
   * Before the generic ENC-long handler — matrix has its own post-save flow. */
  if (l1 == 6 && mstate != MenuState::MENU_L1) {
    if (ev == BtnEvent::LONG) {                    /* exit matrix → SAVE menu (protected) */
      openSaveMenu();
      return;
    }
    /* Encoder = horizontal (L/R + wrap); OC/SCALE = vertical (U/D + wrap). */
    bool moved = false;
    if      (delta > 0) { seqUI_moveRight(); moved = true; }
    else if (delta < 0) { seqUI_moveLeft();  moved = true; }
    if (btnOC.justFell)    { seqUI_moveUp();   moved = true; }
    if (btnScale.justFell) { seqUI_moveDown(); moved = true; }
    if (ev == BtnEvent::SINGLE)  { seqUI_toggleStep(); moved = true; }
    if (ev == BtnEvent::DOUBLE)  menuState.store(MenuState::MENU_L1);
    if (moved || ev != BtnEvent::NONE) displayDirty.store(true);
    return;
  }

  /* ── ENC long: open the protected SAVE menu (never a blind save) ────────────
   * Long-press always lands in the scoped SAVE menu (L1=14); the user selects a
   * scope and the YES/NO confirm guards the actual NVS write + reboot.          */
  if (ev == BtnEvent::LONG) {
    openSaveMenu();
    return;
  }

  /* ── SONG row editor (l1=13, already inside L2) ─────────────────────────
   * Row-per-step value-box editor (matches the App song editor + user spec):
   *   ENC turn   → change the value in the selected box (BANK 0-3 / REPEATS 1-15)
   *   SCALE      → advance the box cursor (BANK→REPEATS→next row→…, wraps)
   *   OC         → append a new row (numSteps++, up to SONG_MAX_STEPS)
   *   OC+SCALE   → delete the current row (hold OC, tap SCALE)
   *   ENC double → back out to MENU_L1
   *   ENC long   → save (handled globally above: g_saveRequest + IDLE)
   * Edits write hwSongData directly (same lock-free model as applySongStep) and
   * echo the whole slot to the App so hardware and web stay 1:1.                */
  if (l1 == 13 && mstate != MenuState::MENU_L1) {
    static bool songSuppressOC = false;   /* eat the OC release after a combo */
    const uint8_t slot = activeSongSlot.load(std::memory_order_relaxed) & 15u;
    SongSlot&     song = hwSongData[slot];
    if (song.numSteps == 0u) {            /* guarantee at least one editable row */
      song.steps[0].bank = 0u; song.steps[0].chain = 0u; song.steps[0].repeats = 1u;
      song.numSteps = 1u;
    }
    int  row     = songUI_row.load(std::memory_order_relaxed);
    int  box     = songUI_box.load(std::memory_order_relaxed);
    if (row >= song.numSteps) row = song.numSteps - 1;
    bool changed = false;

    /* ENC double = exit to category list */
    if (ev == BtnEvent::DOUBLE) { menuState.store(MenuState::MENU_L1); displayDirty.store(true); return; }

    /* OC + SCALE combo = delete current row (needs ≥2 rows) */
    if (btnScale.justFell && btnOC.isPressed()) {
      if (song.numSteps > 1u) {
        for (int i = row; i < song.numSteps - 1; ++i) song.steps[i] = song.steps[i + 1];
        song.numSteps--;
        if (row >= song.numSteps) row = song.numSteps - 1;
        changed = true;
      }
      songSuppressOC = true;              /* OC is still down → ignore its release */
    }
    /* SCALE alone = advance box cursor (flat across rows, wraps).  The
     * !btnOC.isPressed() guard stops the SCALE *release* of a delete combo from
     * also nudging the cursor. */
    else if ((evScale == BtnEvent::SINGLE && !btnOC.isPressed()) || ev == BtnEvent::SINGLE) {
      int flat = row * SONG_UI_BOXES + box + 1;
      const int total = song.numSteps * SONG_UI_BOXES;
      flat %= total;
      row = flat / SONG_UI_BOXES; box = flat % SONG_UI_BOXES;
      displayDirty.store(true);
    }

    /* OC alone = append a new row (default bank A, 1 repeat) */
    if (evOC == BtnEvent::SINGLE) {
      if (songSuppressOC) { songSuppressOC = false; }
      else if (song.numSteps < SONG_MAX_STEPS) {
        const int n = song.numSteps;
        song.steps[n].bank = 0u; song.steps[n].chain = 0u; song.steps[n].repeats = 1u;
        song.numSteps++;
        row = n; box = 0;
        changed = true;
      }
    }

    /* ENC turn = edit the value under the cursor */
    if (delta != 0) {
      SongStep& st = song.steps[row];
      if (box == 0) st.bank = (uint8_t)wrapIndex((int)st.bank, (int)delta, 4);
      else          st.repeats = (uint8_t)std::min(15, std::max(1, (int)st.repeats + (int)delta));
      changed = true;
    }

    songUI_row.store(row, std::memory_order_relaxed);
    songUI_box.store(box, std::memory_order_relaxed);
    if (changed) { echoSongState(); displayDirty.store(true); }
    if (ev != BtnEvent::NONE || evOC != BtnEvent::NONE || evScale != BtnEvent::NONE)
      displayDirty.store(true);
    return;
  }

  /* ── Encoder turn (not pressed) ──────────────────────────────────────── */
  if (delta != 0 && !btnEnc.isPressed()) {
    displayDirty.store(true);
    if (btnScale.isPressed()) {
      /* SCALE + turn = harp octave shift */
      applySeqOctave(0, (int16_t)delta);   /* patches.h — with txSysex echo */
    } else if (scopeOn) {
      handleTelemetryPageEncoder((int16_t)delta);
    } else {
      switch (mstate) {
      case MenuState::IDLE:
        if (activeDashboard.load() == DashboardMode::SEQUENCER)
          setSequencerBpm((int32_t)constrain(seqBpm.load() + delta, 40, 240));
        else {
          /* HARP dashboard: browse patches — [C9b] DEFERRED recall (restores the
           * smooth v28.9 feel).  Per-detent we only bump harpPatchIndex (lock-free,
           * one relaxed store) so the OLED name tracks instantly; the expensive
           * livePatch memcpy + atomic fan-out under patchMux is deferred until the
           * encoder rests (see the uiSyncPending commit block at the top of this
           * function).  This keeps fast scrolling free of patchMux contention and
           * stops the display from being forced into a full redraw every detent.
           * Cap to NUM_NAMED_PRESETS so hardware browsing matches the App dropdown
           * (slots 128-255 are unnamed expansion fillers).                         */
          const int cur  = std::min(harpPatchIndex.load() & (NUM_PATCHES - 1),
                                    NUM_NAMED_PRESETS - 1);
          const int next = wrapIndex(cur, (int)delta, NUM_NAMED_PRESETS);
          harpPatchIndex.store(next, std::memory_order_relaxed);
          uiSyncPending .store(true, std::memory_order_relaxed);
          displayDirty  .store(true, std::memory_order_relaxed);
        }
        break;
      case MenuState::MENU_L1: {
        /* [I7] step through the regrouped *display* order, not the raw id:
         * id → slot, advance the slot, slot → id. */
        const int slot = l1SlotForCat(currentMenuL1.load());
        currentMenuL1.store(l1CatForSlot(wrapIndex(slot, (int)delta, kL1Count)));
        break; }
      case MenuState::MENU_L2: {
        const int span = l2CountFor(currentMenuL1.load());
        if (span > 0)
          currentMenuL2.store(wrapIndex(currentMenuL2.load(), (int)delta, span));
        break; }
      case MenuState::MENU_L3:
        updateHardwareParameter(
          (uint8_t)currentMenuL1.load(),
          (uint8_t)currentMenuL2.load(),
          (int16_t)delta);
        break;
      }
    }
  }

  /* ── ENC single: navigate deeper ─────────────────────────────────────── */
  if (ev == BtnEvent::SINGLE) {
    displayDirty.store(true);
    if (scopeOn) {
      currentScopeView.store(TelemetryView::OFF);
    } else {
      switch (mstate) {
      case MenuState::IDLE:
        currentMenuL1.store(
          activeDashboard.load() == DashboardMode::SEQUENCER ? 5 : 0);
        menuState.store(MenuState::MENU_L1);
        break;
      case MenuState::MENU_L1:
        if (currentMenuL1.load() == 11) {
          currentScopeView.store(TelemetryView::RAW_AC);
          menuState.store(MenuState::IDLE);
        } else {
          currentMenuL2.store(0);
          menuState.store(MenuState::MENU_L2);
        }
        break;
      case MenuState::MENU_L2: {
        /* Inline toggle items that need no L3 value entry */
        const int cl1 = currentMenuL1.load(), cl2 = currentMenuL2.load();
        if (cl1 == 12) {
          /* RESET is destructive (RAM wipe + reboot) → YES/NO gate, scope as arg. */
          openConfirm(ConfirmAction::RESET, (uint8_t)cl2);
        } else if (cl1 == 14) {
          /* SAVE menu — same scopes as RESET, persist + reboot → YES/NO gate. */
          openConfirm(ConfirmAction::SAVE, (uint8_t)cl2);
        } else if (cl1 == 15) {
          /* LOAD menu — same scopes, RAM-only reload (no reboot) → YES/NO gate. */
          openConfirm(ConfirmAction::LOAD, (uint8_t)cl2);
        } else if (cl1 == 4 && cl2 == 1) {
          const bool en = !pbMapping.enabled.load(std::memory_order_relaxed);
          pbMapping.enabled.store(en, std::memory_order_relaxed);
          if (isAppConnected()) txSysex(CMD_PB_ENABLE, en ? 16383u : 0u);
        } else if (cl1 == 2 && cl2 >= 18 && cl2 <= 20) {
          /* Mute toggle from menu confirm */
          switch (cl2) {
            case 18: applyMute(CMD_H_MUTE, !mixHarpMute .load()); break;
            case 19: applyMute(CMD_S_MUTE, !mixSeqMute  .load()); break;
            case 20: applyMute(CMD_D_MUTE, !mixDrumsMute.load()); break;
          }
        } else if (cl1 == 0 && cl2 == 9) {
          /* [EDGE] HARP SETUP → Edge Comp opens the full-screen 8-bar editor
           * instead of a numeric L3 value screen. */
          edgeEditSel.store(0, std::memory_order_relaxed);
          edgeEditScale.store(harpScaleIndex.load(std::memory_order_relaxed) & (NUM_SCALES - 1),
                              std::memory_order_relaxed); /* [EDGE-NAV] start on live scale */
          edgeEditOpen.store(true, std::memory_order_relaxed);
        } else if (cl1 == 5 && cl2 == 6) {
          /* [SEQ-CLEAR] Clear is destructive → raise the YES/NO confirm modal. */
          openConfirm(ConfirmAction::SEQ_CLEAR);
        } else if (cl1 == 5 && cl2 == 7) {
          /* [SOFT-RESET] CLEAR extended (sounds + nav → initial) → YES/NO gate. */
          openConfirm(ConfirmAction::SOFT_RESET);
        } else if (cl1 == 5 && cl2 == 8) {
          /* [USER-PAT-SLOTS] Save Pat → YES/NO confirm (overwrite slot + NVS). */
          const uint8_t idx = userPatCursor.load(std::memory_order_relaxed) & 63u;
          openConfirm(ConfirmAction::USR_PAT_SAVE, idx);
        } else {
          menuState.store(MenuState::MENU_L3);
        }
        break; }
      case MenuState::MENU_L3: {
        /* [USER-SLOTS] Save Slot (HARP/SEQ SYNTH idx 19) commits on ENC press →
         * YES/NO confirm (overwrites a slot + writes NVS).  Everything else just
         * steps back to the L2 list. */
        const int cl1 = currentMenuL1.load(), cl2 = currentMenuL2.load();
        if ((cl1 == 3 || cl1 == 8) && cl2 == 19) {
          const uint8_t eng = (cl1 == 8) ? 1u : 0u;
          const uint8_t idx = userSlotCursor[eng].load(std::memory_order_relaxed) & 63u;
          openConfirm(ConfirmAction::USR_SOUND_SAVE, (uint8_t)((eng << 6) | idx));
        } else {
          menuState.store(MenuState::MENU_L2);
        }
        break; }
      }
    }
  }

  /* ── ENC double: navigate back ────────────────────────────────────────── */
  if (ev == BtnEvent::DOUBLE) {
    displayDirty.store(true);
    if (scopeOn) {
      currentScopeView.store(TelemetryView::OFF);
    } else {
      switch (mstate) {
      case MenuState::IDLE:
        currentMenuL1.store(activeDashboard.load() == DashboardMode::SEQUENCER ? 5 : 0);
        menuState.store(MenuState::MENU_L1); break;
      case MenuState::MENU_L2:
        menuState.store(MenuState::MENU_L1); break;
      case MenuState::MENU_L3:
        menuState.store(MenuState::MENU_L2); break;
      /* [NAV] Double-click now backs all the way out: L3→L2→L1→IDLE, so the user
       * can reach the dashboard without a long-press (which also saves). */
      case MenuState::MENU_L1:
        menuState.store(MenuState::IDLE); break;
      }
    }
  }

  /* ── OC event handling ─────────────────────────────────────────────────── */
  /* Long: HARP = open/close toggle */
  if (evOC == BtnEvent::LONG) {
    if (activeDashboard.load() == DashboardMode::HARP) {
      const HarpMode m = harpMode.load();
      harpMode.store(m == HarpMode::OPEN ? HarpMode::CLOSING : HarpMode::OPENING);
      displayDirty.store(true);
    }
  }
  /* Short: SEQ = record toggle; HARP = cycle play mode */
  if (evOC == BtnEvent::SINGLE) {
    if (activeDashboard.load() == DashboardMode::SEQUENCER)
      seq_toggle_recording();                   /* echo + displayDirty via groovebox API */
    else {
      displayDirty.store(true);
      int pm = (int)currentPlayMode.load() + 1;
      if (pm > 2) pm = 0;
      harpSetPlayMode((PlayMode)pm);              /* flush notes + dirty display */
      txSysex(CMD_PLAY_MODE, (uint16_t)pm);       /* mirror to App POLY/STR/SOLO  */
    }
  }

  /* ── SCALE event handling ─────────────────────────────────────────────── */
  /* Short: SEQ = play/stop; HARP = panic + next scale */
  if (evScale == BtnEvent::SINGLE) {
    displayDirty.store(true);
    if (activeDashboard.load() == DashboardMode::SEQUENCER) {
      /* [v6.0] Hardware ALWAYS owns transport — seq_toggle() echoes CMD_TRANSPORT
       * itself, so the App's (read-only) transport buttons follow.            */
      seq_toggle();
    } else {
      panicRequested.store(true, std::memory_order_relaxed);
      const int next = (harpScaleIndex.load() + 1) % NUM_SCALES;
      harpScaleIndex.store(next);
      txSysex(CMD_H_SCALE, (uint16_t)next);
      for (int i = 0; i < MAX_STRINGS; ++i) computeHardwareDACThreshold(i, 0.f);
      /* [STUCK-FIX] Clear the laser's physical-hold detection state so a beam
       * held across the scale change can't carry over as a stuck string. */
      g_beamClearReq.store(true, std::memory_order_release);
    }
  }

  if (delta != 0 || ev != BtnEvent::NONE)
    displayDirty.store(true);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * SECTION 8 — FACTORY / SCOPED RESET
 * ═══════════════════════════════════════════════════════════════════════════ */

/* OLED status lines — only give i2cMutex when take succeeded (mutex misuse
 * on a failed take was triggering xTaskPriorityDisinherit / Guru Meditation). */
static void oledStatusLines(const char* line1, const char* line2) {
  if (!hasOLED) return;
  /* [SAVE-FIX5] Never touch the I2C bus while a flash write is armed: the cache
   * is off and the other core is IPC-stalled, so a Wire transaction here can
   * release the driver's internal mutex from the wrong context (assert
   * xTaskPriorityDisinherit).  Status text can wait for the ~tens of ms write. */
  if (g_saveArmed.load(std::memory_order_acquire)) return;
  if (xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(200)) != pdTRUE) return;
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SH110X_WHITE);
  display.setCursor(10, 20);
  display.print(line1);
  if (line2) {
    display.setCursor(10, 40);
    display.print(line2);
  }
  display.display();
  xSemaphoreGive(i2cMutex);
}

/* Hold a status screen briefly, then hand the OLED back to display_refresh_task
 * (APP CONNECTED splash when the App link is live).  oledStatusLines writes
 * directly to the framebuffer and bypasses renderUIState — without this restore
 * step "RESET FAILED" / "SAVE BUSY" would stick forever.                     */
static void oledStatusHold(const char* line1, const char* line2,
                           uint32_t holdMs = 2000u) {
  /* Set the hold flag BEFORE drawing so display_refresh_task cannot slip in
   * between oledStatusLines() and the store and overwrite the message. */
  g_oledStatusHoldMs.store(millis() + holdMs, std::memory_order_relaxed);
  oledStatusLines(line1, line2);
}

static void oledStatusRestoreNow() {
  g_oledStatusHoldMs.store(0u, std::memory_order_relaxed);
  menuState.store(MenuState::IDLE, std::memory_order_relaxed);
  displayDirty.store(true, std::memory_order_relaxed);
}

/* applyResetScope() already ran — reload the matching NVS scope back into RAM. */
static void rollbackResetRam(ResetScope scope) {
  (void)settings_load_scoped(scope);
  for (int i = 0; i < MAX_STRINGS; ++i)
    computeHardwareDACThreshold(i, 0.f);
}

static ResetScope s_resetPersistScope = ResetScope::FULL;

/* Runtime scoped reset: persist via NvsWorker (laser park + 16 KB stack) then
 * restart.  Runs off ControlPoll so the control surface stays responsive.     */
static void reset_persist_task(void*) {
  while (saveInProgress()) vTaskDelay(pdMS_TO_TICKS(10));

  /* Wipe RAM here (not in handleScopedReset) so a task-create failure cannot
   * leave the device in a half-reset state with no NVS commit. */
  applyResetScope(s_resetPersistScope);

  if (g_saveDoneSem) xSemaphoreTake(g_saveDoneSem, 0);

  g_persistScope.store((uint8_t)s_resetPersistScope, std::memory_order_release);
  g_resetAckPending.store(true, std::memory_order_release);
  g_restartAfterSave.store(true, std::memory_order_release);
  g_saveRequest.store(true, std::memory_order_release);
  displayDirty.store(true, std::memory_order_relaxed);

  const bool got =
      g_saveDoneSem &&
      (xSemaphoreTake(g_saveDoneSem, pdMS_TO_TICKS(45000)) == pdTRUE);

  if (!got || !g_saveLastOk.load(std::memory_order_acquire)) {
    g_restartAfterSave.store(false, std::memory_order_release);
    g_resetAckPending.store(false, std::memory_order_release);
    rollbackResetRam(s_resetPersistScope);
    /* sendFullStateSync is only useful when the App is actively connected. */
    if (isAppConnected()) sendFullStateSync();
    /* NACK must be sent unconditionally — isAppConnected() may return false
     * if the NVS write took >4.5 s (MidiUsbRx is blocked during the flash
     * write so heartbeats can't be processed, causing the 4.5 s window to
     * expire).  The App MUST receive a NACK to unlock its modal regardless
     * of whether the connection was reported as live at this exact moment. */
    txSysex(CMD_SCOPED_RESET, 0u);
    g_resetInProgress.store(false, std::memory_order_release);
    oledStatusHold("RESET FAILED", "RESTORED RAM", 2000u);
    vTaskDelay(pdMS_TO_TICKS(2100));
    oledStatusRestoreNow();
  }
  /* On success NvsWorker calls esp_restart() — this task never returns. */
  vTaskDelete(nullptr);
}

/* handleScopedReset — apply RAM wipe, persist, restart.
 * Boot OC+SCALE combo: blocking 16 KB save task (NvsWorker not running yet). */
void handleScopedReset(ResetScope scope) {
  if (g_resetInProgress.exchange(true, std::memory_order_acq_rel)) {
    oledStatusHold("RESET BUSY", nullptr, 1500u);
    if (isAppConnected()) txSysex(CMD_SCOPED_RESET, 0u);
    return;
  }
  if (saveInProgress()) {
    g_resetInProgress.store(false, std::memory_order_release);
    oledStatusHold("SAVE BUSY", nullptr, 1500u);
    if (isAppConnected()) txSysex(CMD_SCOPED_RESET, 0u);
    return;
  }

  const char* line1 = "RESET";
  switch (scope) {
    case ResetScope::FULL:           line1 = "FULL RESET";    break;
    case ResetScope::BANKS_PATTERNS: line1 = "BANKS+PATTERNS"; break;
    case ResetScope::MOTION:         line1 = "MOTION CLEAR";  break;
    case ResetScope::SETTINGS:       line1 = "SETTINGS RESET"; break;
  }
  oledStatusHold(line1, "PLEASE WAIT...", 60000u);

  /* [FIX-M3] Silence voices and stop the sequencer before wiping RAM. */
  allNotesOff();
  seq_stop();

  if (!g_systemReady.load(std::memory_order_acquire)) {
    applyResetScope(scope);
    if (!settings_persist_blocking(scope)) {
      g_resetInProgress.store(false, std::memory_order_release);
      rollbackResetRam(scope);
      oledStatusHold("RESET FAILED", "RESTORED RAM", 2000u);
      if (isAppConnected()) txSysex(CMD_SCOPED_RESET, 0u);
      vTaskDelay(pdMS_TO_TICKS(2100));
      oledStatusRestoreNow();
      return;
    }
    vTaskDelay(pdMS_TO_TICKS(300));
    if (isAppConnected()) txSysex(CMD_SCOPED_RESET, 16383u);
    esp_restart();
    return;
  }

  s_resetPersistScope = scope;
  if (xTaskCreatePinnedToCore(reset_persist_task, "RstSave", 8192, nullptr, 5,
                              nullptr, 1) != pdPASS) {
    g_resetInProgress.store(false, std::memory_order_release);
    /* 2000 ms hold auto-expires via display_refresh_task; no vTaskDelay needed
     * here (this runs on MidiUsbRx — blocking 2.1 s drops incoming MIDI). */
    oledStatusHold("RESET FAILED", "TASK ERROR", 2000u);
    if (isAppConnected()) txSysex(CMD_SCOPED_RESET, 0u);
  }
}

/* handleScopedSave — menu / App scoped save (no restart). */
void handleScopedSave(ResetScope scope) {
  if (!requestScopedSave((uint8_t)scope))
    oledStatusHold("SAVE BUSY", nullptr, 1500u);
}

/* handleScopedLoad — menu / App scoped reload from NVS.  RAM-only (no flash
 * write) so there is NO reboot: the loaded state goes live immediately.
 * [FIX-L3] Sends sendFullStateSync() + ACK to App if connected — the comment
 * "only reachable while App is disconnected" was aspirational, not enforced;
 * if the App is connected it must receive the new state or stays stale.         */
void handleScopedLoad(ResetScope scope) {
  if (saveInProgress() || g_resetInProgress.load(std::memory_order_acquire)) {
    oledStatusHold("BUSY", nullptr, 1500u);
    if (isAppConnected()) txSysex(CMD_SESSION_LOAD, 0u);
    return;
  }

  const char* line1 = "LOAD";
  switch (scope) {
    case ResetScope::FULL:           line1 = "FULL LOAD";     break;
    case ResetScope::BANKS_PATTERNS: line1 = "BANKS+PATTERNS"; break;
    case ResetScope::MOTION:         line1 = "MOTION LOAD";   break;
    case ResetScope::SETTINGS:       line1 = "SETTINGS LOAD"; break;
  }
  oledStatusHold(line1, "PLEASE WAIT...", 60000u);

  const bool ok = settings_load_scoped(scope);

  /* Re-apply the bits hardware can't pick up from atomics on its own. */
  for (int i = 0; i < MAX_STRINGS; ++i) computeHardwareDACThreshold(i, 0.f);

  oledStatusHold(ok ? "LOAD OK" : "NOTHING SAVED", nullptr, 900u);

  /* [FIX-L3] Re-sync the App if it is connected — previously skipped because the
   * hardware LOAD was assumed to only run while the App is disconnected, but there
   * is no gate enforcing that.  Without this echo the App shows stale state.    */
  if (ok && isAppConnected()) {
    sendFullStateSync();
    echoSongState();
    txSysex(CMD_SESSION_LOAD, 16383u); /* ACK so App can log "load complete" */
  }

  /* [FIX-DELAY] Use vTaskDelay so Core-0 cooperative tasks run during the
   * status-message hold window.  Arduino delay() delegates to vTaskDelay on
   * ESP32 Arduino, but this makes the intent explicit and avoids confusion. */
  vTaskDelay(pdMS_TO_TICKS(900));
  oledStatusRestoreNow();
  menuState.store(MenuState::IDLE, std::memory_order_relaxed);
  displayDirty.store(true, std::memory_order_relaxed);
}

/* handleFactoryReset — boot-time OC+SCALE combo entry point.  FULL scope. */
void handleFactoryReset() {
  handleScopedReset(ResetScope::FULL);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * TASK STACK / HEAP TELEMETRY — recurring sample for OLED + Serial
 * ═══════════════════════════════════════════════════════════════════════════ */
void updateTaskStackStats() {
  auto hwm = [](TaskHandle_t h) -> uint16_t {
    if (!h) return 0u;
    return (uint16_t)(uxTaskGetStackHighWaterMark(h) * sizeof(StackType_t));
  };

  TaskStackStats s{};
  s.audio   = hwm(hAudioTask);
  s.midi    = hwm(hMidiTask);
  s.dbeam   = hwm(hDBeamTask);
  s.seq     = hwm(hSeqBgTask);
  s.control = hwm(hControlTask);
  s.display = hwm(hDisplayTask);
  s.laser   = hwm(hLaserTask);
  s.nvs     = hwm(hNvsTask);
  s.dramFree  = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  s.psramFree = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

  /* [FIX-MINFREE] Include midi in the scan array — it was isolated outside the
   * loop for no reason, making the logic inconsistent and easy to miss. */
  uint16_t minF = 0xFFFFu;
  const uint16_t vals[] = { s.audio, s.midi, s.dbeam, s.seq, s.control,
                            s.display, s.laser, s.nvs };
  for (uint16_t v : vals)
    if (v > 0u && v < minF) minF = v;
  s.minFree = (minF == 0xFFFFu) ? 0u : minF;

  g_stackStats = s;

  if (currentScopeView.load(std::memory_order_relaxed) == TelemetryView::STACK_STATS)
    displayDirty.store(true, std::memory_order_relaxed);
}

void printInterfaceStats() {
  const TaskStackStats& s = g_stackStats;
  Serial.printf("[Telemetry] load=%u%% stackB free Au:%u Mi:%u dB:%u Sq:%u "
                "Ct:%u Ol:%u Lz:%u Nv:%u min:%u DRAM:%u PSRAM:%u\n",
                (unsigned)g_audio_load_pct.load(std::memory_order_relaxed),
                (unsigned)s.audio, (unsigned)s.midi, (unsigned)s.dbeam,
                (unsigned)s.seq, (unsigned)s.control, (unsigned)s.display,
                (unsigned)s.laser, (unsigned)s.nvs, (unsigned)s.minFree,
                (unsigned)s.dramFree, (unsigned)s.psramFree);
  if (s.minFree > 0u && s.minFree < 512u)
    Serial.println(F("[Telemetry] WARNING: task stack < 512 B free — bump sizes"));
}
