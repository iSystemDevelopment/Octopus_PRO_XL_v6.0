/* ═════════════════════════════════════════════════════════════════════════════
 * Octopus PRO XL v6.1.01 — Laser Harp Groovebox
 * © 2026 DIODAC ELECTRONICS / iSystem. All Rights Reserved.
 *
 * PROPRIETARY AND CONFIDENTIAL. Unauthorized copying, distribution, modification,
 * or use of this software or firmware, in whole or in part, is strictly prohibited
 * without prior written permission from DIODAC ELECTRONICS.
 * ═════════════════════════════════════════════════════════════════════════════
 * interface.cpp — v6.1.01  HARDWARE CONTROL SURFACE
 *
 * Encoder + OC/SCALE buttons @ 200 Hz (control_surface_task, Core 0).
 *   • initHardwareInterface() — GPIO, encoder ISR on Core 1, boot factory-reset hold
 *   • updateHardwareInterface() — gestures, menus, App-connected transport surface
 *   • updateHardwareParameter() — encoder edits → patches.h apply* paths
 * Menu tree L1 0–15 (HARP SETUP … SONG).  All parameter writes funnel through
 * patches.h; display via displayDirty + renderUIState (display.cpp).
 * ═════════════════════════════════════════════════════════════════════════════ */
#include "interface.h"
#include "link.h"
#include <cmath>
#include "groovebox.h"       /* seq transport, grid UI, factory patterns */
#include "display.h"
#include "dbeam.h"
#include "fog.h"
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
/* wire authority + apply* paths live in patches.h */

/* ── Object instantiations ─────────────────────────────────────────────────── */
ButtonPoll      btnEnc;
ButtonPoll      btnOC;
ButtonPoll      btnScale;
EncoderPoll     encPoll;
ESP32Encoder    encoder;           /* hardware quadrature encoder — used by getDelta() */
InterfaceStats  interface_stats  = {};
DisplayI2CStats display_stats    = {};

/* ═══════════════════════════════════════════════════════════════════════════
 * SECTION 1 — PITCH ENCODE / DECODE (semitone-linear master pitch)
 * ═══════════════════════════════════════════════════════════════════════════ */
uint16_t encodeMasterPitch(float pitch) {
  pitch = std::min(MASTER_PITCH_MAX, std::max(MASTER_PITCH_MIN, pitch));
  /* Linear in semitones ±24 (±2 oct); unity (1.0×) at v14 = 8192. */
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

/* mutateHarp — apply delta via applyHarpParam. Hardware always applies locally
 * (live performance with App open); wire echo gated by checkWireAuthority.
 * LFO route echoes on cmd 11 (SynthParam index), not legacy 86. */
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

/* mutateSeq — same pattern for seq synth. */
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

/* ═══════════════════════════════════════════════════════════════════════════
 * SECTION 5 — updateHardwareParameter
 *
 * Dispatches encoder delta to the correct SSOT parameter path.
 * l1 = menu category (0–15), l2 = item within category, delta = turn amount
 * (1:1 detent steps from EncoderPoll.getDelta).
 * ═══════════════════════════════════════════════════════════════════════════ */
void updateHardwareParameter(uint8_t l1, uint8_t l2, int16_t delta) {
  if (l1 >= (uint8_t)kL1CatCount || (int)l2 >= l2CountFor((int)l1)) return;

  const int scale = harpScaleIndex.load(std::memory_order_relaxed) & (NUM_SCALES - 1);

  switch (l1) {

  /* ── 0: HARP SETUP — beam + calibration ─────────────────────────────── */
  case 0:
    portENTER_CRITICAL(&patchMux);
    switch (l2) {
    case 0: beamGateHoldMs = (uint32_t)constrain(
                (int32_t)beamGateHoldMs + delta * 25, 0, (int32_t)BEAM_GATE_HOLD_MAX); break;
    case 1: scaleWhiteLevel[scale]      = (uint8_t)constrain((int)scaleWhiteLevel[scale]     + delta*4, 0, 255); break;
    case 2: scaleTouchConfirm[scale]    = (uint8_t)constrain((int)scaleTouchConfirm[scale]   + delta,   CONFIRM_MIN, CONFIRM_MAX); break;
    case 3: scaleReleaseConfirm[scale]  = (uint8_t)constrain((int)scaleReleaseConfirm[scale] + delta,   CONFIRM_MIN, CONFIRM_MAX); break;
    case 4: scaleR[scale] = (uint8_t)constrain((int)scaleR[scale] + delta*5, 0, 255); break;
    case 5: scaleG[scale] = (uint8_t)constrain((int)scaleG[scale] + delta*5, 0, 255); break;
    case 6: scaleB[scale] = (uint8_t)constrain((int)scaleB[scale] + delta*5, 0, 255); break;
    /* Anti-stuck fail-safe timeout (ms, 0 = disabled). Global, not per-scale. */
    case 8: beamStuckReleaseMs = (uint32_t)constrain(
                (int32_t)beamStuckReleaseMs + delta * 25, 0, (int32_t)BEAM_STUCK_RELEASE_MAX); break;
    case 7: scaleMargin[scale] = (uint16_t)constrain((int)scaleMargin[scale] + delta*10, 0, 2000);
            portEXIT_CRITICAL(&patchMux);
            for (int i = 0; i < MAX_STRINGS; ++i) computeHardwareDACThreshold(i, 0.f);
            return;
    /* Fog-reject enable + differential margin (global atomics). */
    case 10: fogRejectEnabled.store(delta > 0 ? true : (delta < 0 ? false
                  : !fogRejectEnabled.load(std::memory_order_relaxed)),
                  std::memory_order_relaxed); break;
    case 11: fogRejectMargin.store(constrain(
                  fogRejectMargin.load(std::memory_order_relaxed) + delta * 25,
                  FOG_MARGIN_MIN, FOG_MARGIN_MAX), std::memory_order_relaxed); break;
    /* Closed-harp idle screensaver (HARP SETUP item, not LASER SHOW). */
    case 12: laserScreensaver.store(delta > 0 ? true : (delta < 0 ? false
                  : !laserScreensaver.load(std::memory_order_relaxed)),
                  std::memory_order_relaxed); break;
    }
    portEXIT_CRITICAL(&patchMux);
    break;

  /* ── 1: D-BEAM ───────────────────────────────────────────────────────── */
  case 1:
    switch (l2) {
    /* Apply locally always; gate wire echo when App owns the parameter. */
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
    /* D-BEAM route + target synth selector. */
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
    case 0:  { const float nv = safe_atomic_store(masterVol,   masterVol.load(),   (float)delta*0.01f, 0.f,1.f);
               txSysex(CMD_M_VOL,  (uint16_t)(nv*16383.f)); break; }
    case 1:  { const float nv = safe_atomic_store(mixHarpVol,  mixHarpVol.load(),  (float)delta*0.01f, 0.f,1.f);
               txSysex(CMD_H_VOL,  (uint16_t)(nv*16383.f)); break; }
    case 2:  { const float nv = safe_atomic_store(mixSeqVol,   mixSeqVol.load(),   (float)delta*0.01f, 0.f,1.f);
               txSysex(CMD_S_VOL,  (uint16_t)(nv*16383.f)); break; }
    case 3:  { const float nv = safe_atomic_store(mixDrumsVol, mixDrumsVol.load(), (float)delta*0.01f, 0.f,1.f);
               txSysex(CMD_D_VOL,  (uint16_t)(nv*16383.f)); break; }
    case 4:  { const float nv = safe_atomic_store(masterPitch, masterPitch.load(),
                               (float)delta*0.05f, MASTER_PITCH_MIN, MASTER_PITCH_MAX);
               txSysex(CMD_PITCH, encodeMasterPitch(nv)); break; }
    case 5:  { int v = wrapIndex((int)masterFxIndex.load(), (int)delta, 16);
               masterFxIndex.store(v, std::memory_order_relaxed);
               fx.loadMasterFx(v); txSysex(CMD_M_FX_IDX, (uint16_t)v);
               txMasterFxParams(); break; }
    case 6:  { const float nv = safe_atomic_store(drumRevSend, drumRevSend.load(), (float)delta*0.01f, 0.f,1.f);
               txSysex(CMD_D_REV,  (uint16_t)(nv*16383.f)); break; }
    case 7:  { const float nv = safe_atomic_store(drumDlySend, drumDlySend.load(), (float)delta*0.01f, 0.f,1.f);
               txSysex(CMD_D_DLY,  (uint16_t)(nv*16383.f)); break; }
    case 8:  { const float nv = safe_atomic_store(tbDrive, tbDrive.load(), (float)delta*0.01f, 0.f,1.f);
               txSysex(CMD_TB_DRV,  (uint16_t)(nv*16383.f)); break; }
    case 9:  { const float nv = safe_atomic_store(tbTone,  tbTone.load(),  (float)delta*0.01f, 0.f,1.f);
               txSysex(CMD_TB_TONE, (uint16_t)(nv*16383.f)); break; }
    case 10: { const float nv = safe_atomic_store(tbMix,   tbMix.load(),   (float)delta*0.01f, 0.f,1.f);
               txSysex(CMD_TB_MIX,  (uint16_t)(nv*16383.f)); break; }
    case 11: { const float nv = safe_atomic_store(djFreq,  djFreq.load(),  (float)delta*0.01f, 0.f,1.f);
               txSysex(CMD_DJ_FQ,   (uint16_t)(nv*16383.f)); break; }
    case 12: { const float nv = safe_atomic_store(djRes,   djRes.load(),   (float)delta*0.01f, 0.f,1.f);
               txSysex(CMD_DJ_RES,  (uint16_t)(nv*16383.f)); break; }
    case 13: { const float nv = safe_atomic_store(djMix,   djMix.load(),   (float)delta*0.01f, 0.f,1.f);
               txSysex(CMD_DJ_MIX,  (uint16_t)(nv*16383.f)); break; }
    case 14: { const float nv = safe_atomic_store(masterEqLow,  masterEqLow.load(),  (float)delta*0.5f, -12.f,12.f);
               txSysex(CMD_EQ_L, (uint16_t)(((nv+12.f)/24.f)*16383.f)); break; }
    case 15: { const float nv = safe_atomic_store(masterEqHigh, masterEqHigh.load(), (float)delta*0.5f, -12.f,12.f);
               txSysex(CMD_EQ_H, (uint16_t)(((nv+12.f)/24.f)*16383.f)); break; }
    case 16: { int i = wrapIndex((int)drumFxIndexA.load(), (int)delta, 16);
               drumFxIndexA.store(i, std::memory_order_relaxed);
               loadDrumFx(i); txSysex(CMD_D_FX_IDX, (uint16_t)i);
               echoDrumInsertParams();
               maybeEchoAuxAfterInsertLoad(); break; }
    case 17: { int i = wrapIndex((int)drumFxIndexB.load(), (int)delta, 16);
               drumFxIndexB.store(i, std::memory_order_relaxed);
               loadDrumFxB(i); txSysex(CMD_D_FX_IDX_B, (uint16_t)i); break; }
    /* Mute toggles: encoder turn applies toggle. */
    case 18: applyMute(CMD_H_MUTE, !mixHarpMute .load(std::memory_order_relaxed)); break;
    case 19: applyMute(CMD_S_MUTE, !mixSeqMute  .load(std::memory_order_relaxed)); break;
    case 20: applyMute(CMD_D_MUTE, !mixDrumsMute.load(std::memory_order_relaxed)); break;
    case 21: { const float nv = safe_atomic_store(mixHarpPan, mixHarpPan.load(), (float)delta * 0.02f, -1.f, 1.f);
               txSysex(CMD_H_PAN, (uint16_t)((nv + 1.f) * 0.5f * 16383.f)); break; }
    case 22: { const float nv = safe_atomic_store(mixSeqPan, mixSeqPan.load(), (float)delta * 0.02f, -1.f, 1.f);
               txSysex(CMD_S_PAN, (uint16_t)((nv + 1.f) * 0.5f * 16383.f)); break; }
    case 23: { const float nv = safe_atomic_store(mixDrumsPan, mixDrumsPan.load(), (float)delta * 0.02f, -1.f, 1.f);
               txSysex(CMD_D_PAN, (uint16_t)((nv + 1.f) * 0.5f * 16383.f)); break; }
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
               loadHarpFx(i);  txSysex(CMD_H_FX_IDX,   (uint16_t)i);
               txInsertFxSends(0u);
               maybeEchoAuxAfterInsertLoad(); break; }
    case 15: { int i = wrapIndex((int)harpFxIndexB.load(), (int)delta, 16);
               harpFxIndexB.store(i, std::memory_order_relaxed);
               loadHarpFxB(i); txSysex(CMD_H_FX_IDX_B, (uint16_t)i); break; }
    case 16: { portENTER_CRITICAL(&patchMux);
               float v = std::min(1.f, std::max(0.f, fx.harpInsert.dly_send + delta*0.02f));
               fx.harpInsert.dly_send = v;
               portEXIT_CRITICAL(&patchMux);
               if (checkWireAuthority(CMD_H_DLY_MIX, true))
                 txSysex(CMD_H_DLY_MIX, (uint16_t)(v*16383.f)); break; }
    case 17: { portENTER_CRITICAL(&patchMux);
               float v = std::min(1.f, std::max(0.f, fx.harpInsert.rev_send + delta*0.02f));
               fx.harpInsert.rev_send = v;
               portEXIT_CRITICAL(&patchMux);
               if (checkWireAuthority(CMD_H_REV_MIX, true))
                 txSysex(CMD_H_REV_MIX, (uint16_t)(v*16383.f)); break; }
    case 18: { /* Factory preset recall (128-name bank). */
               const int cur = std::min(harpPatchIndex.load() & (NUM_PATCHES - 1),
                                        NUM_NAMED_PRESETS - 1);
               const int nx  = wrapIndex(cur, (int)delta, NUM_NAMED_PRESETS);
               recallHarpPatch(nx, ParamSource::UI);
               break; }
    /* 19 Save Slot — turn picks slot (commit on ENC); 20 Load Slot recalls on turn. */
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
    case 1: if (delta != 0) {
              const bool en = !pbMapping.enabled.load(std::memory_order_relaxed);
              pbMapping.enabled.store(en, std::memory_order_relaxed);
              if (isAppConnected()) txSysex(CMD_PB_ENABLE, en ? 16383u : 0u); break; }
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

  /* ── 5: SEQ SETUP ──────────────────────────────────────────────────────── */
  case 5:
    switch (l2) {
    case 0: applySeqBank   ((uint8_t)wrapIndex((int)seqActiveBank.load(), (int)delta, 4)); break;
    case 1: seqUI_page.store(wrapIndex((int)seqUI_page.load(), (int)delta, 2),
                            std::memory_order_relaxed);
            displayDirty.store(true, std::memory_order_relaxed); break;
    /* Clamp signed transpose to ±12 before applySeqTranspose (v14 = t + 12). */
    case 2: { const int t = std::min(12, std::max(-12, (int)seqTranspose.load() + (int)delta));
              applySeqTranspose((uint16_t)(t + 12)); break; }
    case 3: applySeqLength   ((uint16_t)((int)seqLength.load() + delta));    break;
    /* Load Synth/Drum use g_last*Preset (live index, not a stale static). */
    case 4: { const int cur  = (int)g_lastSynthPreset.load(std::memory_order_relaxed);
              const int next = wrapIndex(cur, (int)delta, NUM_SYNTH_PATS);
              loadFactorySynthPattern(seqActiveBank.load() & 15u, SEQ_UI_CHAIN, next); break; }
    case 5: { const int cur  = (int)g_lastDrumPreset.load(std::memory_order_relaxed);
              const int next = wrapIndex(cur, (int)delta, NUM_DRUM_PATS);
              loadFactoryDrumPattern(seqActiveBank.load() & 15u, SEQ_UI_CHAIN, next); break; }
    case 6: break;  /* Clear — handled on ENC click (confirm dialog) */
    case 7: { uint8_t c = (uint8_t)wrapIndex((int)userPatCursor.load(std::memory_order_relaxed),
                              (int)delta, NUM_USER_PAT_SLOTS);
              userPatCursor.store(c, std::memory_order_relaxed);
              displayDirty.store(true, std::memory_order_relaxed); break; }
    /* Load Pat — turn picks slot and recalls immediately. */
    case 8: { uint8_t c = (uint8_t)wrapIndex((int)userPatCursor.load(std::memory_order_relaxed),
                              (int)delta, NUM_USER_PAT_SLOTS);
              userPatCursor.store(c, std::memory_order_relaxed);
              loadUserPatternToActive(c); break; }
    case 9: { const bool en = delta > 0 ? true : (delta < 0 ? false : !seqArpEnabled.load());
               applySeqArpEnable(en ? 16383u : 0u); break; }
    case 10: { const int p = wrapIndex((int)seqArpPattern.load(), (int)delta, 8);
               applySeqArpPattern((uint16_t)p); break; }
    case 11: { const int r = wrapIndex((int)seqArpRate.load(), (int)delta, 8);
               applySeqArpRate((uint16_t)r); break; }
    case 12: { const int g = wrapIndex((int)seqArpGate.load(), (int)delta, 8);
               applySeqArpGate((uint16_t)g); break; }
    }
    break;

  /* ── 6: SEQ MATRIX — grid navigation (handled in updateHardwareInterface) */
  case 6:
    if      (delta > 0) seqUI_moveRight();
    else if (delta < 0) seqUI_moveLeft();
    break;

  /* ── 7: AUX FX ───────────────────────────────────────────────────────── */
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
               txSysex(CMD_H_DLY_MIX, (uint16_t)(v*16383.f)); break; }
    case 5: { portENTER_CRITICAL(&patchMux);
               float v = std::min(1.f, std::max(0.f, fx.harpInsert.rev_send + delta*0.02f));
               fx.harpInsert.rev_send = v; portEXIT_CRITICAL(&patchMux);
               txSysex(CMD_H_REV_MIX, (uint16_t)(v*16383.f)); break; }
    case 6: { portENTER_CRITICAL(&patchMux);
               float v = std::min(1.f, std::max(0.f, fx.seqInsert.dly_send + delta*0.02f));
               fx.seqInsert.dly_send = v; portEXIT_CRITICAL(&patchMux);
               txSysex(CMD_S_DLY_MIX, (uint16_t)(v*16383.f)); break; }
    case 7: { portENTER_CRITICAL(&patchMux);
               float v = std::min(1.f, std::max(0.f, fx.seqInsert.rev_send + delta*0.02f));
               fx.seqInsert.rev_send = v; portEXIT_CRITICAL(&patchMux);
               txSysex(CMD_S_REV_MIX, (uint16_t)(v*16383.f)); break; }
    /* FX slot indices */
    case 8:  { int i = wrapIndex((int)harpFxIndex.load(),  (int)delta,16);
               harpFxIndex.store(i, std::memory_order_relaxed);
               loadHarpFx(i);  txSysex(CMD_H_FX_IDX,   (uint16_t)i);
               txInsertFxSends(0u);
               maybeEchoAuxAfterInsertLoad(); break; }
    case 9:  { int i = wrapIndex((int)harpFxIndexB.load(), (int)delta,16);
               harpFxIndexB.store(i, std::memory_order_relaxed);
               loadHarpFxB(i); txSysex(CMD_H_FX_IDX_B, (uint16_t)i); break; }
    case 10: { int i = wrapIndex((int)seqFxIndex.load(),   (int)delta,16);
               seqFxIndex.store(i, std::memory_order_relaxed);
               loadSeqFx(i);   txSysex(CMD_S_FX_IDX,   (uint16_t)i);
               txInsertFxSends(1u);
               maybeEchoAuxAfterInsertLoad(); break; }
    case 11: { int i = wrapIndex((int)seqFxIndexB.load(),  (int)delta,16);
               seqFxIndexB.store(i, std::memory_order_relaxed);
               loadSeqFxB(i);  txSysex(CMD_S_FX_IDX_B, (uint16_t)i); break; }
    case 12: { int i = wrapIndex((int)drumFxIndexA.load(), (int)delta,16);
               drumFxIndexA.store(i, std::memory_order_relaxed);
               loadDrumFx(i); txSysex(CMD_D_FX_IDX, (uint16_t)i);
               echoDrumInsertParams();
               maybeEchoAuxAfterInsertLoad(); break; }
    case 13: { int i = wrapIndex((int)drumFxIndexB.load(), (int)delta,16);
               drumFxIndexB.store(i, std::memory_order_relaxed);
               loadDrumFxB(i); txSysex(CMD_D_FX_IDX_B, (uint16_t)i); break; }
    case 14: { const int i = wrapIndex(auxSceneIndex.load(std::memory_order_relaxed) & 15,
                                        (int)delta, 16);
               loadAuxScene(i);
               txSysex(CMD_AUX_SCENE_IDX, (uint16_t)i);
               echoAuxParams(); break; }
    case 15: { const bool en = delta > 0 ? true : (delta < 0 ? false
                          : !linkAuxToInsertPreset.load(std::memory_order_relaxed));
               linkAuxToInsertPreset.store(en, std::memory_order_relaxed);
               txSysex(CMD_LINK_AUX_PRESET, en ? 16383u : 0u);
               displayDirty.store(true, std::memory_order_relaxed); break; }
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
               loadSeqFx(i);  txSysex(CMD_S_FX_IDX,   (uint16_t)i);
               txInsertFxSends(1u);
               maybeEchoAuxAfterInsertLoad(); break; }
    case 15: { int i = wrapIndex((int)seqFxIndexB.load(), (int)delta,16);
               seqFxIndexB.store(i, std::memory_order_relaxed);
               loadSeqFxB(i); txSysex(CMD_S_FX_IDX_B, (uint16_t)i); break; }
    case 16: { portENTER_CRITICAL(&patchMux);
               float v = std::min(1.f, std::max(0.f, fx.seqInsert.dly_send + delta*0.02f));
               fx.seqInsert.dly_send = v; portEXIT_CRITICAL(&patchMux);
               txSysex(CMD_S_DLY_MIX, (uint16_t)(v*16383.f)); break; }
    case 17: { portENTER_CRITICAL(&patchMux);
               float v = std::min(1.f, std::max(0.f, fx.seqInsert.rev_send + delta*0.02f));
               fx.seqInsert.rev_send = v; portEXIT_CRITICAL(&patchMux);
               txSysex(CMD_S_REV_MIX, (uint16_t)(v*16383.f)); break; }
    case 18: { /* Seq melody factory preset recall (128-name bank). */
               const int cur = std::min(seqPatchIndex.load() & (NUM_PATCHES - 1),
                                        NUM_NAMED_PRESETS - 1);
               const int nx  = wrapIndex(cur, (int)delta, NUM_NAMED_PRESETS);
               recallSeqPatch(nx, ParamSource::UI);
               txSysex(CMD_S_PATCH, (uint16_t)nx);   /* echo index → App dropdown */
               break; }
    /* User slots — mirror of HARP SYNTH 19/20 (engine 1). */
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
   * Show Mode and MIDI→Hue are independent toggles. Hue ADSR in seconds.
   * Screensaver lives in HARP SETUP (closed-harp idle, not projector show). */
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
  case 14: break;  /* PERF name — stub Phase G */
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

/* confirmDispatch — run confirmed action (YES path). */
static void confirmDispatch(ConfirmAction a, uint8_t arg) {
  switch (a) {
    case ConfirmAction::SEQ_CLEAR:  seqClearActiveAndResetSounds();              break;
    case ConfirmAction::SAVE:       handleScopedSave ((ResetScope)(arg & 3u));   break;
    case ConfirmAction::RESET:      handleScopedReset((ResetScope)(arg & 3u));   break;
    case ConfirmAction::USR_SOUND_SAVE:
      if (saveLiveToUserSlot((uint8_t)((arg >> 6) & 1u), (uint8_t)(arg & 63u)) && isAppConnected())
        txSysex(CMD_USR_SOUND_SAVE, (uint16_t)(arg & 0x3FFFu));
      break;
    case ConfirmAction::USR_PAT_SAVE:
      saveActivePatternToUserSlot((uint8_t)(arg & 63u));                         break;
    case ConfirmAction::NONE:
    default: break;
  }
}

/* Open PERF SLOT menu (L1=14). ENC long-press routes here. */
static inline void openPerfSlotMenu() {
  currentScopeView.store(TelemetryView::OFF, std::memory_order_relaxed);
  edgeEditOpen.store(false, std::memory_order_relaxed);
  menuSystemMidiSub.store(false, std::memory_order_relaxed);
  menuResetFromSystem.store(false, std::memory_order_relaxed);
  currentMenuL1.store(14, std::memory_order_relaxed);
  currentMenuL2.store(0,  std::memory_order_relaxed);
  menuState.store(MenuState::MENU_L2, std::memory_order_relaxed);
  displayDirty.store(true, std::memory_order_relaxed);
}

void updateHardwareInterface() {
  const uint32_t now = millis();
  btnEnc  .update(now);
  btnOC   .update(now);
  btnScale.update(now);

  /* Freeze control surface while save is in flight (avoid patchMux vs NvsWorker race). */
  static uint32_t s_saveStuckSince = 0u;
  if (saveInProgress()) {
    if (s_saveStuckSince == 0u) s_saveStuckSince = now;
    else if ((uint32_t)(now - s_saveStuckSince) > 25000u) {
      saveForceUnlock();
      s_saveStuckSince = 0u;
      g_saveFailFlashMs.store(now + 1500u, std::memory_order_relaxed);
      displayDirty.store(true, std::memory_order_relaxed);
      Serial.println(F("[NVS] WARN: save handshake timeout — unlocked"));
    }
    return;
  }
  s_saveStuckSince = 0u;

  /* Encoder read — pure linear 1:1 (no acceleration). */
  const int16_t delta = encPoll.getDelta(encoder.getCount(), now);
  if (delta != 0) lastEncoderTurnMs.store(now, std::memory_order_relaxed);

  /* Deferred harp preset recall — commit ~160 ms after encoder stops (dashboard browse). */
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

  /* YES/NO modal — owns all input while open (before App-connected branch). */
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

  /* Edge-comp full-screen editor (HARP SETUP → Edge Comp). */
  if (edgeEditOpen.load(std::memory_order_relaxed)) {
    int  sel       = edgeEditSel.load(std::memory_order_relaxed) & 7;
    int  escale    = edgeEditScale.load(std::memory_order_relaxed) & (NUM_SCALES - 1);
    bool dirty     = false;
    bool recompute = false;
    /* OC scrolls scales and selects live (harpScaleIndex) for margin/RGB/edge rows. */
    uint8_t* const       edge    = edgeCompFor(escale);
    const uint8_t* const factory = edgeFactoryFor(escale);

    if (ev == BtnEvent::DOUBLE) {                 /* exit, no save */
      edgeEditOpen.store(false, std::memory_order_relaxed);
      displayDirty.store(true, std::memory_order_relaxed);
      return;
    }
    if (ev == BtnEvent::LONG) {                    /* exit editor → PERF SLOT menu */
      edgeEditOpen.store(false, std::memory_order_relaxed);
      openPerfSlotMenu();
      return;
    }
    if (evScale == BtnEvent::SINGLE) {             /* next string, wrap */
      sel = (sel + 1) & 7;
      edgeEditSel.store(sel, std::memory_order_relaxed);
      dirty = true;
    }
    if (evOC == BtnEvent::SINGLE) {                /* next scale, wrap + live select */
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
    if (ev == BtnEvent::LONG) {                    /* exit matrix → PERF SLOT menu */
      openPerfSlotMenu();
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

  /* ── ENC long: open PERF SLOT menu (never a blind save) ───────────────────
   * Long-press lands in PERF SLOT (L1=14); Load/Save wired in Phase G.        */
  if (ev == BtnEvent::LONG) {
    openPerfSlotMenu();
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
          /* HARP dashboard: browse presets (deferred recall via uiSyncPending). */
          const int cur  = std::min(harpPatchIndex.load() & (NUM_PATCHES - 1),
                                    NUM_NAMED_PRESETS - 1);
          const int next = wrapIndex(cur, (int)delta, NUM_NAMED_PRESETS);
          harpPatchIndex.store(next, std::memory_order_relaxed);
          if (isAppConnected())
            txSysex(CMD_H_PATCH, (uint16_t)next);
          uiSyncPending .store(true, std::memory_order_relaxed);
          displayDirty  .store(true, std::memory_order_relaxed);
        }
        break;
      case MenuState::MENU_L1: {
        /* L1 menu uses regrouped display order (kL1Order / kCatToSlot). */
        const int slot = l1SlotForCat(currentMenuL1.load());
        currentMenuL1.store(l1CatForSlot(wrapIndex(slot, (int)delta, kL1MenuCount)));
        break; }
      case MenuState::MENU_L2: {
        const int span = l2CountForCat(currentMenuL1.load());
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
        currentMenuL2.store(0);
        menuState.store(MenuState::MENU_L2);
        break;
      case MenuState::MENU_L2: {
        /* Inline toggle items that need no L3 value entry */
        const int cl1 = currentMenuL1.load(), cl2 = currentMenuL2.load();
        if (cl1 == 12) {
          openConfirm(ConfirmAction::RESET, (uint8_t)cl2);
        } else if (cl1 == 14) {
          if (cl2 == 2) {
            menuState.store(MenuState::MENU_L3);
          } else {
            Serial.println(cl2 == 0 ? F("[PERF] Load — Phase G") : F("[PERF] Save — Phase G"));
          }
        } else if (cl1 == 4 && !menuSystemMidiSub.load(std::memory_order_relaxed)) {
          if (cl2 == 0) {
            menuSystemMidiSub.store(true, std::memory_order_relaxed);
            currentMenuL2.store(0, std::memory_order_relaxed);
          } else if (cl2 == 1) {
            currentScopeView.store(TelemetryView::RAW_AC);
            menuState.store(MenuState::IDLE);
          } else if (cl2 == 2) {
            menuResetFromSystem.store(true, std::memory_order_relaxed);
            currentMenuL1.store(12, std::memory_order_relaxed);
            currentMenuL2.store(0, std::memory_order_relaxed);
          }
        } else if (cl1 == 4 && menuSystemMidiSub.load(std::memory_order_relaxed) && cl2 == 1) {
          const bool en = !pbMapping.enabled.load(std::memory_order_relaxed);
          pbMapping.enabled.store(en, std::memory_order_relaxed);
          if (isAppConnected()) txSysex(CMD_PB_ENABLE, en ? 16383u : 0u);
        } else if (cl1 == 2 && cl2 >= 18 && cl2 <= 20) {
          switch (cl2) {
            case 18: applyMute(CMD_H_MUTE, !mixHarpMute .load()); break;
            case 19: applyMute(CMD_S_MUTE, !mixSeqMute  .load()); break;
            case 20: applyMute(CMD_D_MUTE, !mixDrumsMute.load()); break;
          }
        } else if (cl1 == 0 && cl2 == 9) {
          edgeEditSel.store(0, std::memory_order_relaxed);
          edgeEditScale.store(harpScaleIndex.load(std::memory_order_relaxed) & (NUM_SCALES - 1),
                              std::memory_order_relaxed);
          edgeEditOpen.store(true, std::memory_order_relaxed);
        } else if (cl1 == 5 && cl2 == 6) {
          openConfirm(ConfirmAction::SEQ_CLEAR);
        } else if (cl1 == 5 && cl2 == 7) {
          const uint8_t idx = userPatCursor.load(std::memory_order_relaxed) & 63u;
          openConfirm(ConfirmAction::USR_PAT_SAVE, idx);
        } else if (cl1 == 11) {
          const int span = l2CountFor(11);
          const int pick = (span > 0) ? (cl2 % span) : 0;
          currentScopeView.store((TelemetryView)(pick + 1));
          menuState.store(MenuState::IDLE);
        } else {
          menuState.store(MenuState::MENU_L3);
        }
        break; }
      case MenuState::MENU_L3: {
        /* Save Slot (idx 19) → YES/NO confirm; other L3 items step back to L2. */
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
      case MenuState::MENU_L2: {
        const int cl1 = currentMenuL1.load(std::memory_order_relaxed);
        if (cl1 == 12 && menuResetFromSystem.load(std::memory_order_relaxed)) {
          menuResetFromSystem.store(false, std::memory_order_relaxed);
          currentMenuL1.store(4, std::memory_order_relaxed);
          currentMenuL2.store(2, std::memory_order_relaxed);
        } else if (cl1 == 4 && menuSystemMidiSub.load(std::memory_order_relaxed)) {
          menuSystemMidiSub.store(false, std::memory_order_relaxed);
          currentMenuL2.store(0, std::memory_order_relaxed);
        } else {
          menuState.store(MenuState::MENU_L1);
        }
        break; }
      case MenuState::MENU_L3:
        menuState.store(MenuState::MENU_L2); break;
      /* Double-click backs out: L3→L2→L1→IDLE. */
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
    displayDirty.store(true);
    if (activeDashboard.load() == DashboardMode::SEQUENCER)
      seqRecording.store(!seqRecording.load());
    else {
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
      /* Hardware owns transport — seq_toggle echoes CMD_TRANSPORT. */
      seq_toggle();
    } else {
      panicRequested.store(true, std::memory_order_relaxed);
      const int next = (harpScaleIndex.load() + 1) % NUM_SCALES;
      harpScaleIndex.store(next);
      txSysex(CMD_H_SCALE, (uint16_t)next);
      for (int i = 0; i < MAX_STRINGS; ++i) computeHardwareDACThreshold(i, 0.f);
      /* Clear laser hold state on scale change (avoid stuck string). */
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
  /* Skip I2C while NVS flash is armed (cache off / IPC stall). */
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

/* handleScopedReset — runtime: arm NVS pend_rst + reboot (wipe on next boot).
 * Boot OC+SCALE / pre-scheduler: synchronous apply + commit + reboot.          */
void handleScopedReset(ResetScope scope) {
  const bool runtime = g_systemReady.load(std::memory_order_acquire);

  if (runtime) {
    const char* line1 = "RESET";
    switch (scope) {
      case ResetScope::FULL:           line1 = "FULL RESET";     break;
      case ResetScope::BANKS_PATTERNS: line1 = "BANKS+PATTERNS"; break;
      case ResetScope::MOTION:         line1 = "MOTION CLEAR";   break;
      case ResetScope::SETTINGS:       line1 = "SETTINGS RESET"; break;
    }
    oledStatusLines(line1, "REBOOT...");
    if (!settings_arm_pending_reset(scope)) {
      g_saveFailFlashMs.store(millis() + 1500u, std::memory_order_relaxed);
      displayDirty.store(true, std::memory_order_relaxed);
      if (isAppConnected()) {
        txSysexForce(CMD_SESSION_SLOT_ACK,
                     linkEncodePersistAck(PersistAckPhase::FAIL,
                                          g_persistTxnId.load(std::memory_order_relaxed)));
        txSysexForce(CMD_SCOPED_RESET, 0u);
      }
      linkSetPhase(LinkPhase::LIVE);
      return;
    }
    if (isAppConnected()) {
      txSysexForce(CMD_SESSION_SLOT_ACK,
                   linkEncodePersistAck(PersistAckPhase::REBOOTING,
                                        g_persistTxnId.load(std::memory_order_relaxed)));
      txSysexForce(CMD_SCOPED_RESET, 16383u);
    }
    delay(150);
    esp_restart();
    return;
  }

  /* Pre-scheduler boot path (OC+SCALE combo) — no pend_rst needed. */
  const char* line1 = "FULL RESET";
  oledStatusLines(line1, "PLEASE WAIT...");
  applyResetScope(scope);
  settings_commit_reset_scoped(scope);
  if (isAppConnected()) {
    txSysexForce(CMD_SESSION_SLOT_ACK,
                 linkEncodePersistAck(PersistAckPhase::REBOOTING,
                                      g_persistTxnId.load(std::memory_order_relaxed)));
    txSysexForce(CMD_SCOPED_RESET, 16383u);
  }
  delay(300);
  esp_restart();
}

/* handleScopedSave — menu SAVE: persist + reboot (~700 ms). */
void handleScopedSave(ResetScope scope) {
  requestScopedSave((uint8_t)scope);
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

  uint16_t minF = 0xFFFFu;
  const uint16_t vals[] = { s.audio, s.dbeam, s.seq, s.control,
                            s.display, s.laser, s.nvs };
  for (uint16_t v : vals)
    if (v > 0u && v < minF) minF = v;
  if (s.midi > 0u && s.midi < minF) minF = s.midi;
  s.minFree = (minF == 0xFFFFu) ? 0u : minF;

  g_stackStats = s;

  if (currentScopeView.load(std::memory_order_relaxed) != TelemetryView::OFF)
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
