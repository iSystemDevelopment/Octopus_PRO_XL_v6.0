/* ═════════════════════════════════════════════════════════════════════════════
 * Octopus PRO XL v6.1.01 — Laser Harp Groovebox
 * © 2026 DIODAC ELECTRONICS / iSystem. All Rights Reserved.
 *
 * PROPRIETARY AND CONFIDENTIAL. Unauthorized copying, distribution, modification,
 * or use of this software or firmware, in whole or in part, is strictly prohibited
 * without prior written permission from DIODAC ELECTRONICS.
 * ═════════════════════════════════════════════════════════════════════════════
 * laser.cpp — v6.1.01  GALVO SWEEP + BEAM TRIGGER (Core 1, prio 24)
 *
 * laser_sweep_task: MCPWM galvo timing, LT1016 digital trigger latch, per-string
 * note on/off → harp.cpp, fog reject gate, D-BEAM expression read, hue/laser show.
 * Phase timing: Core-1-local esp_rom_delay_us busy-waits + LASER_BREATHE_MS yield.
 * NVS save: parks dark while NvsWorker writes flash.
 * ═════════════════════════════════════════════════════════════════════════════ */
#include "laser.h"
#include "harp.h"    /* [HARP-SPLIT] harpNoteOn/Off, harpReleaseVoice, hue, vibrato */
#include "dbeam.h"   /* updateDbeamExpression, routeDbeamExpression, s_dbeam_*  */
#include "patches.h" /* appSyncActive, pollSyncHeartbeat                  */
#include "fog.h"     /* [FOG] fogAccept() — differential fog-reject gate       */
#include "esp_timer.h"
#include "esp_task_wdt.h" /* Option A: feed TWDT + let Core-1 IDLE breathe */

/* ─────────────────────────────────────────────────────────────────────────────
 * releaseHarpString — full note-off for one string when the harp closes.
 *
 * Clears the physical string-active flag, frees the voice owner, starts the DSP
 * release envelope (musical fade, not a hard cut/click) and sends the hue ADSR
 * into RELEASE so the beam colour fades with the note.  Used by the CLOSING
 * fan-in (per beam) and as a final safety sweep when the harp reaches CLOSED so
 * NO harp note can keep ringing after the instrument is shut.
 * ─────────────────────────────────────────────────────────────────────────────*/
static inline void IRAM_ATTR releaseHarpString(int s) {
  if (s < 0 || s >= MAX_STRINGS) return;
  stringActive[s & 7] = false;
  harpHueNoteOff(s); /* [HARP-SPLIT] hue → RELEASE */
  noteOffHarp(s);    /* clears physical string-active flag */
  harpNoteOff(s);    /* frees owner + DSP release under patchMux */
}

/* ─────────────────────────────────────────────────────────────────────────────
 * tickScreensaver — gentle idle visual while the harp is CLOSED [opt-in].
 *
 * A single dim, slowly hue-shifting dot bounces across the beam window.  The
 * laser is OFF during every galvo move (blocking settle) so there are no faint
 * connecting lines — just a clean roving point.  Beam detection is NOT run here,
 * so the screensaver can never trigger a note.  Power is held low (≈40 %) so it
 * reads as an ambient idle animation rather than an active beam.
 * ─────────────────────────────────────────────────────────────────────────────*/
static inline void IRAM_ATTR tickScreensaver() {
  const uint32_t nowUs = (uint32_t)esp_timer_get_time();

  /* ── Smooth constant-velocity bounce ──────────────────────────────────────
   * The old version recomputed position from absolute wall-clock every tick and
   * used a sine.  Two problems made the dot stutter "in the middle":
   *   1. After a breathing/scheduling pause, the wall-clock position had moved
   *      on, so the dot JUMPED to catch up — biggest where it moves fastest.
   *   2. A sine peaks its velocity at mid-travel, so the catch-up jumps landed
   *      exactly in the centre.
   * Fix: advance a phase accumulator by a CLAMPED time-delta (a long pause is
   * capped, so it can never cause a catch-up jump), and use near-constant
   * velocity (linear with only a light end-ease) so the centre no longer races. */
  static uint32_t lastUs = 0u;
  static float phase = 0.0f; /* 0..1 across one one-way sweep */
  static int8_t dir = 1;
  if (lastUs == 0u) lastUs = nowUs;
  uint32_t dtUs = nowUs - lastUs;
  lastUs = nowUs;
  if (dtUs > 8000u) dtUs = 8000u; /* clamp out long gaps → no position jump */

  phase += (float)dir * (float)dtUs * (1.0f / 3200000.0f); /* ~3.2 s per sweep */
  if (phase >= 1.0f) {
    phase = 1.0f;
    dir = -1;
  } else if (phase <= 0.0f) {
    phase = 0.0f;
    dir = 1;
  }

  /* 80 % linear + 20 % smoothstep: constant-velocity middle, gently eased ends
   * (no abrupt mechanical reversal, no mid-travel speed-up). */
  const float smooth = phase * phase * (3.0f - 2.0f * phase);
  const float eased = 0.80f * phase + 0.20f * smooth;

  const int32_t winLo = (int32_t)BEAM_WINDOW_MIN;
  const int32_t winHi = (int32_t)BEAM_WINDOW_MAX;
  const int32_t margin = (winHi - winLo) / 20; /* 5 % guard each end */
  const int32_t lo = winLo + margin;
  const int32_t hi = winHi - margin;
  uint16_t pos = (uint16_t)(lo + (int32_t)(eased * (float)(hi - lo)));
  pos = std::min<uint16_t>(std::max<uint16_t>(pos, BEAM_WINDOW_MIN), BEAM_WINDOW_MAX);

  /* Colour: 12 s hue cycle, dimmed to ~40 % so it stays subtle. */
  const float hue = (float)(nowUs % 12000000u) / 12000000.0f;
  uint8_t r, g, b;
  blitHueToRGB(hue, r, g, b);
  r = (uint8_t)(r * 0.40f);
  g = (uint8_t)(g * 0.40f);
  b = (uint8_t)(b * 0.40f);

  galvoMoveDark(pos, 300u, true); /* laser OFF during move, blocking settle */
  resetLaserPhase();
  laserRGB(r, g, b, 0);
  esp_rom_delay_us(ANIM_DWELL_US_L);
  laserOffAndSync();
}

/* ─────────────────────────────────────────────────────────────────────────────
 * setupMCPWM — IDF v5 MCPWM for R/G/B laser channels
 * Period = MCPWM_MAX_DUTY ticks at 10 MHz → carrier per DBEAM_CARRIER_SEL
 *   (globals.h sweep table: 1.0×=9 652 Hz … 2.5×=24 155 Hz). [L1]
 * ─────────────────────────────────────────────────────────────────────────────*/
void setupMCPWM() {
  gpio_reset_pin(PIN_LASER_R);
  gpio_reset_pin(PIN_LASER_G);
  gpio_reset_pin(PIN_LASER_B);

  /* Timer: 10 MHz source, MCPWM_MAX_DUTY-tick period (carrier per DBEAM_CARRIER_SEL) [L1] */
  mcpwm_timer_config_t timer_cfg = {
    .group_id = 0,
    .clk_src = MCPWM_TIMER_CLK_SRC_DEFAULT,
    .resolution_hz = 10000000u,
    .count_mode = MCPWM_TIMER_COUNT_MODE_UP,
    .period_ticks = MCPWM_MAX_DUTY
  };
  if (mcpwm_new_timer(&timer_cfg, &laser_timer) != ESP_OK) return;

  /* Soft sync source — used for phase-aligned dwell starts */
  mcpwm_soft_sync_config_t soft_sync_cfg = {};
  if (mcpwm_new_soft_sync_src(&soft_sync_cfg, &laser_soft_sync) != ESP_OK) return;

  mcpwm_timer_sync_phase_config_t sync_phase = {
    .sync_src = laser_soft_sync,
    .count_value = 0,
    .direction = MCPWM_TIMER_DIRECTION_UP
  };
  if (mcpwm_timer_set_phase_on_sync(laser_timer, &sync_phase) != ESP_OK) return;

  /* Three operators (one per laser colour channel) */
  mcpwm_operator_config_t oper_cfg = { .group_id = 0 };
  if (mcpwm_new_operator(&oper_cfg, &laser_oper_r) != ESP_OK) return;
  if (mcpwm_new_operator(&oper_cfg, &laser_oper_g) != ESP_OK) return;
  if (mcpwm_new_operator(&oper_cfg, &laser_oper_b) != ESP_OK) return;
  mcpwm_operator_connect_timer(laser_oper_r, laser_timer);
  mcpwm_operator_connect_timer(laser_oper_g, laser_timer);
  mcpwm_operator_connect_timer(laser_oper_b, laser_timer);

  /* Comparators — update on timer-zero for glitch-free duty changes */
  mcpwm_comparator_config_t cmpr_cfg = { .flags = { .update_cmp_on_tez = true } };
  if (mcpwm_new_comparator(laser_oper_r, &cmpr_cfg, &laser_cmpr_r) != ESP_OK) return;
  if (mcpwm_new_comparator(laser_oper_g, &cmpr_cfg, &laser_cmpr_g) != ESP_OK) return;
  if (mcpwm_new_comparator(laser_oper_b, &cmpr_cfg, &laser_cmpr_b) != ESP_OK) return;
  mcpwm_comparator_set_compare_value(laser_cmpr_r, 0);
  mcpwm_comparator_set_compare_value(laser_cmpr_g, 0);
  mcpwm_comparator_set_compare_value(laser_cmpr_b, 0);

  /* Generators */
  const mcpwm_generator_config_t gen_r = { .gen_gpio_num = PIN_LASER_R };
  const mcpwm_generator_config_t gen_g = { .gen_gpio_num = PIN_LASER_G };
  const mcpwm_generator_config_t gen_b = { .gen_gpio_num = PIN_LASER_B };
  if (mcpwm_new_generator(laser_oper_r, &gen_r, &laser_gen_r) != ESP_OK) return;
  if (mcpwm_new_generator(laser_oper_g, &gen_g, &laser_gen_g) != ESP_OK) return;
  if (mcpwm_new_generator(laser_oper_b, &gen_b, &laser_gen_b) != ESP_OK) return;

  /* Action: set HIGH on timer empty, set LOW on compare match */
  auto setActions = [](mcpwm_gen_handle_t gen, mcpwm_cmpr_handle_t cmpr) {
    mcpwm_generator_set_action_on_timer_event(gen,
                                              MCPWM_GEN_TIMER_EVENT_ACTION(MCPWM_TIMER_DIRECTION_UP,
                                                                           MCPWM_TIMER_EVENT_EMPTY,
                                                                           MCPWM_GEN_ACTION_HIGH));
    mcpwm_generator_set_action_on_compare_event(gen,
                                                MCPWM_GEN_COMPARE_EVENT_ACTION(MCPWM_TIMER_DIRECTION_UP,
                                                                               cmpr,
                                                                               MCPWM_GEN_ACTION_LOW));
  };
  setActions(laser_gen_r, laser_cmpr_r);
  setActions(laser_gen_g, laser_cmpr_g);
  setActions(laser_gen_b, laser_cmpr_b);

  mcpwm_timer_enable(laser_timer);
  mcpwm_timer_start_stop(laser_timer, MCPWM_TIMER_START_NO_STOP);
}

/* ─────────────────────────────────────────────────────────────────────────────
 * initLaserSPI — [L4] configure SPI ONCE so hot-path can use raw transfers.
 *
 * The transaction is intentionally LEFT OPEN (no endTransaction).
 * Core 1 is the sole SPI user (DAC only); no bus contention possible.
 * All calls to SPI.transfer16() in mcp4922Write proceed without per-call
 * mutex acquisition, saving ~40 µs per galvo step.
 * ─────────────────────────────────────────────────────────────────────────────*/
void initLaserSPI() {
  SPI.begin();
  /* 20 MHz, MSB first, SPI mode 0 — leave transaction open permanently */
  SPI.beginTransaction(SPISettings(20000000, MSBFIRST, SPI_MODE0));

  /* Ensure CS and LDAC are HIGH (idle) before any transfers */
  fastWrite(PIN_DAC_CS, HIGH);
  fastWrite(PIN_DAC_LDAC, HIGH);
}

/* ─────────────────────────────────────────────────────────────────────────────
 * initLaser — call from setup() after mutexes and GPIO
 * ─────────────────────────────────────────────────────────────────────────────*/
void initLaser() {
  initLaserSPI();
  setupMCPWM();
  laserOffAndSync();

  /* Park galvo at centre, set nominal threshold */
  mcp4922Write(GALVO_CENTER, 300u);

  /* Sweep state */
  currentIndex.store(0, std::memory_order_relaxed);
  direction.store(1, std::memory_order_relaxed);
  sweepState = SweepState::MOVING;
  stateStartUs = (uint32_t)esp_timer_get_time();
  beamRevealMask = 0x00u;
}

/* ─────────────────────────────────────────────────────────────────────────────
 * tickAnimation — POPULATE opening fan-out / fan-in closing  [L6]
 *
 * ROOT-CAUSE FIX for faint lines (was v5.1 bug):
 *   Old: galvoMoveDark(pos, thresh, false) → laserWhite(255) immediately
 *        → laser fires while galvo still moving → faint paint stroke.
 *   New: ALL galvo moves in animation use blocking settle (true).
 *        Laser fires ONLY after the galvo is stationary.
 *        resetLaserPhase() before AND after every dwell for clean pulses.
 *
 * POPULATE EFFECT:
 *   beamRevealMask accumulates one pair per ANIM_STEP_INTERVAL_US step.
 *   Between steps, ALL currently-revealed beams are scanned in a tight loop
 *   so they appear simultaneously lit (persistence of vision, ≥100 Hz per beam).
 *   This creates the "fan of beams growing from centre" visual.
 * ─────────────────────────────────────────────────────────────────────────────*/
void IRAM_ATTR tickAnimation() {
  static uint32_t lastAnimUs = 0;
  static int fanStep = 0;
  static uint8_t lastMode = 0xFF;

  const uint32_t nowUs = (uint32_t)esp_timer_get_time();
  const int si = harpScaleIndex.load(std::memory_order_relaxed) & (NUM_SCALES - 1);
  const int totalActive = SCALES[si].numActiveStrings;
  const uint8_t currentMode = (uint8_t)harpMode.load(std::memory_order_relaxed);

  if (totalActive <= 0) return;

  /* ── Mode transition reset ────────────────────────────────────────────── */
  if (currentMode != lastMode) {
    fanStep = 0;
    lastMode = currentMode;
    lastAnimUs = nowUs;
    beamRevealMask = 0x00u;

    if (currentMode == (uint8_t)HarpMode::OPENING) {
      /* Pre-compute DAC thresholds for all strings */
      for (int i = 0; i < totalActive; ++i)
        computeHardwareDACThreshold(i, 0.0f);
      /* Reveal the centre pair on the very first frame */
      const int ci = totalActive / 2;
      const int lSt = ci - 1;
      const int rSt = ci;
      if (lSt >= 0 && lSt < totalActive) beamRevealMask |= (1u << lSt);
      if (rSt >= 0 && rSt < totalActive) beamRevealMask |= (1u << rSt);
    }
    if (currentMode == (uint8_t)HarpMode::CLOSING) {
      /* Start with all beams revealed, remove pairs inward */
      beamRevealMask = (uint8_t)((1u << totalActive) - 1u);
    }
  }

  /* ── OPENING — fan-out from centre ──────────────────────────────────── */
  if (currentMode == (uint8_t)HarpMode::OPENING) {

    /* Step advance: reveal next pair when interval elapsed */
    if (nowUs - lastAnimUs >= ANIM_STEP_INTERVAL_US) {
      lastAnimUs = nowUs;
      ++fanStep;
      const int ci = totalActive / 2;
      const int lSt = ci - 1 - fanStep;
      const int rSt = ci + fanStep;
      if (lSt >= 0) beamRevealMask |= (1u << lSt);
      if (rSt < totalActive) beamRevealMask |= (1u << rSt);

      /* All strings revealed → transition to OPEN */
      const uint8_t fullMask = (uint8_t)((1u << totalActive) - 1u);
      if (beamRevealMask == fullMask || fanStep > totalActive / 2) {
        fanStep = 0;
        beamRevealMask = fullMask;
        currentIndex.store(0, std::memory_order_relaxed);
        direction.store(1, std::memory_order_relaxed);
        sweepState = SweepState::MOVING;
        const uint16_t p0 = SCALES[si].dacPos[0];
        mcp4922Write(p0, getHardwareDACThreshold(0));
        esp_rom_delay_us(getGalvoSettleTime(lastPos, p0));
        lastPos = p0;
        stateStartUs = (uint32_t)esp_timer_get_time();
        harpMode.store(HarpMode::OPEN, std::memory_order_relaxed);
        displayDirty.store(true, std::memory_order_relaxed);
        return;
      }
    }

    /* POPULATE scan: visit every revealed beam in this iteration.
     * blocking galvoMoveDark(true) → zero faint lines [L6, ROOT-CAUSE FIX] */
    for (int i = 0; i < totalActive; ++i) {
      if (!(beamRevealMask & (1u << i))) continue;
      const uint16_t p = SCALES[si].dacPos[i & 7];
      if (p < BEAM_WINDOW_MIN || p > BEAM_WINDOW_MAX) continue;

      galvoMoveDark(p, getHardwareDACThreshold(i), true); /* blocking settle */
      resetLaserPhase();                                  /* align phase [L2] */
      laserWhite(255);
      esp_rom_delay_us(ANIM_DWELL_US_L);
      laserOffAndSync(); /* off + phase reset [L2] */
    }
    return;
  }

  /* ── CLOSING — fan-in toward centre ──────────────────────────────────── */
  if (currentMode == (uint8_t)HarpMode::CLOSING) {

    if (nowUs - lastAnimUs >= ANIM_STEP_INTERVAL_US) {
      lastAnimUs = nowUs;
      /* Silence outer pair */
      const int lSt = fanStep;
      const int rSt = (totalActive - 1) - fanStep;
      /* Release the note as its beam fans in (natural fade per string). */
      if (lSt >= 0 && lSt < totalActive) {
        beamRevealMask &= ~(1u << lSt);
        releaseHarpString(lSt);
      }
      if (rSt >= 0 && rSt < totalActive && rSt != lSt) {
        beamRevealMask &= ~(1u << rSt);
        releaseHarpString(rSt);
      }
      std::atomic_thread_fence(std::memory_order_release);
      ++fanStep;

      if (beamRevealMask == 0u || fanStep > totalActive / 2) {
        fanStep = 0;
        beamRevealMask = 0x00u;
        /* Final safety sweep: ensure EVERY harp voice is released so nothing
         * can keep ringing once the harp is shut (notes off + audio silenced). */
        for (int s = 0; s < MAX_STRINGS; ++s) releaseHarpString(s);
        std::atomic_thread_fence(std::memory_order_release);
        laserOffAndSync();
        harpMode.store(HarpMode::CLOSED, std::memory_order_relaxed);
        displayDirty.store(true, std::memory_order_relaxed);
        return;
      }
    }

    /* Scan remaining beams */
    for (int i = 0; i < totalActive; ++i) {
      if (!(beamRevealMask & (1u << i))) continue;
      const uint16_t p = SCALES[si].dacPos[i & 7];
      if (p < BEAM_WINDOW_MIN || p > BEAM_WINDOW_MAX) continue;
      galvoMoveDark(p, getHardwareDACThreshold(i), true);
      resetLaserPhase();
      laserWhite(255);
      esp_rom_delay_us(ANIM_DWELL_US_L);
      laserOffAndSync();
    }
  }
}

/* ─────────────────────────────────────────────────────────────────────────────
 * laser_sweep_task — Core 1, priority 24  (moved from .ino [L6])
 *
 * Timing machine states:
 *   MOVING   → poll settle time, clear 74HC74, reset phase, fire laser
 *   LASER_ON → poll LASER_STAB_US, enable 74HC74 latch
 *   DWELLING → poll BEAM_DWELL_US, sample beam, advance galvo (dark)
 *
 * Key timing properties:
 *   • Phase waits use Core-1-local esp_rom_delay_us busy-waits (precise µs
 *     timing; esp_timer on Core 0 is starved by AudioSynth under load).
 *   • LASER_BREATHE_MS periodic vTaskDelay hands Core 1 to MidiUsbRx /
 *     SeqSysexOut / NvsWorker without chopping lit beams.
 *   • Phases <60 µs may spin via esp_rom_delay_us (timer arm overhead not worth it)
 *   • All microsecond delays use esp_rom_delay_us (IRAM, not flash)
 *   • BEAM_DWELL_US = 10 × PWM period = 1036 µs (integer cycles [L1])
 * ─────────────────────────────────────────────────────────────────────────────*/
void IRAM_ATTR laser_sweep_task(void* pvParameters) {
  /* Task-local sweep state — no global pollution, no lock needed */
  uint8_t touchCounter[MAX_STRINGS] = {};
  uint8_t releaseCounter[MAX_STRINGS] = {};
  uint32_t noteOnTime[MAX_STRINGS] = {};
  /* [STUCK-FIX] Last time each string was confirmed SOLIDLY held (touchCounter at
   * the hold cap).  A note force-releases if it has not been solidly held for
   * beamStuckReleaseMs (globals.h, runtime-tunable, 0 = disabled) — the absolute
   * fail-safe so a note can never hang even if the comparator emits sparse false
   * breaks while the hand is off. */
  uint32_t lastHoldUs[MAX_STRINGS] = {};
  uint32_t settleTimeUs = 400u;
  /* [SAVE-FIX11] Beam DETECTION suppression window (the scan still runs + beams
   * stay visible).  Used after the NVS-save dark park so the AC-coupled LT1016
   * comparator can re-settle before we trust its output — otherwise it
   * false-fires and latches every string "broken".  The flag keeps the µs
   * comparison valid (detectArmAtUs is only read while the window is active and
   * is always within range of nowUs, so uint32 wraparound can't misfire). */
  bool     detectBlackout = false;
  uint32_t detectArmAtUs  = 0u;

  uint32_t lastWatchdogMs = 0u;
  bool lastLinkState = false;
  int8_t lastScanDir = 1;        /* track ping-pong direction for reversal settle */
  uint8_t reverseSettleCnt = 0u; /* >0 → add GALVO_REVERSE_EXTRA_US this move */

  /* [TELEMETRY] Auto-ranging scope normaliser — peak-follow with slow release so
   * every view fills the display regardless of its absolute units (mV, SNR ratio,
   * DAC counts).  Resets when the view changes.  See the scope-write block below. */
  float         scopePeak     = 1.0f;
  TelemetryView scopePeakView = TelemetryView::OFF;

  esp_task_wdt_add(NULL);
  /* ── [CORE1-LOCAL TIMING] Precise phase waits, no esp_timer dependency ─────
   * Each scan phase is timed with a Core-1-local esp_rom_delay_us busy-wait
   * (see the wait block at the bottom of the loop).  We deliberately do NOT
   * sleep on an esp_timer one-shot: the esp_timer dispatch task lives on Core 0
   * BELOW AudioSynth (prio 22 < 24), so under audio load its callbacks are
   * delayed and the laser would fall back to a 5 ms notify timeout EVERY phase —
   * collapsing the scan rate to uneven, flickering beams.  Busy-waiting on Core 1
   * (which the laser owns) keeps beam timing exact regardless of Core-0 load.
   * A short wall-clock breathe (also at the bottom) still hands Core 1 to IDLE1
   * and the data tasks every LASER_BREATHE_MS so nothing on Core 1 starves and
   * the idle-task watchdog stays fed.                                          */
  uint32_t lastBreatheMs = millis();

  while (!g_systemReady.load(std::memory_order_acquire))
    vTaskDelay(pdMS_TO_TICKS(1));

  for (;;) {

    esp_task_wdt_reset();
    /* [CORE1-RECLAIM] No breathe-yield here — the phase-deadline wait at the
     * bottom of the loop yields Core 1 every phase (IDLE1 + data tasks fed).
     * Keeping the old wall-clock vTaskDelay(1) would re-add the per-frame beam
     * "swing" it was fighting, so it's gone.                                    */


    /* ── NVS save handshake — park beam; NvsWorker runs on Core 0 @ prio 16 ── */
    if (g_saveArmed.load(std::memory_order_acquire)) {
      laserOffAndSync();
      g_loopParked.store(true, std::memory_order_release);
      Serial.println(F("[BEAM] save-park ENTER (laser dark, waiting for NVS write)"));
      while (g_saveArmed.load(std::memory_order_acquire)) {
        esp_task_wdt_reset(); /* keep TWDT fed while parked */
        vTaskDelay(pdMS_TO_TICKS(5));
      }
      Serial.println(F("[BEAM] save-park EXIT (recovery via g_beamRecover)"));
      /* Recovery is handled below in the g_beamRecover branch so it runs
       * IDENTICALLY whether or not we managed to park for this save. */
    }

    /* [SAVE-FIX13] Beam recovery after ANY save — set by settings_save_task once
     * the NVS write completes.  Runs whether or not the laser parked, so the
     * flash-write glitch (cache off + Core-1 IPC stall) is always cleaned up.
     *
     * The dark park / flash write leaves the scan suspended mid-phase: the
     * AC-coupled LT1016 emits an edge the 74HC74 latches as "beam broken", the
     * galvo/currentIndex/direction freeze, and held notes stay asserted — so on
     * resume EVERY string reads blocked until the user manually panics (scale
     * change).  Replicate the proven manual fix automatically: recompute all DAC
     * thresholds, force-write the DAC, full clean re-home, release voices + clear
     * stringActive[]/latch, then blank detection while the comparator re-settles. */
    if (g_beamRecover.exchange(false, std::memory_order_acq_rel)) {
      const int siNow = harpScaleIndex.load(std::memory_order_relaxed) & (NUM_SCALES - 1);

      allNotesOff();                       /* RELEASE held voice envelopes (DSP) */
      /* allNotesOff() does NOT touch stringActive[] (it only releases voices),
       * so clear the physical-hold truth here too. */
      for (int s = 0; s < MAX_STRINGS; ++s) {
        stringActive[s]   = false;
        touchCounter[s]   = 0u;
        releaseCounter[s] = 0u;
        noteOnTime[s]     = 0u;
        lastHoldUs[s]     = 0u;
      }
      /* Recompute comparator thresholds for ALL strings (same as the manual
       * scale-change recovery) → refreshes dac_calibration[].final_dac_voltage. */
      for (int i = 0; i < MAX_STRINGS; ++i) computeHardwareDACThreshold(i, 0.0f);
      /* Force the next DAC write past the dedup cache (chip may have glitched). */
      invalidateDacCache();

      /* Clean scan re-home (mirror of tickAnimation OPENING→OPEN). */
      fastWrite(PIN_PEAK_CLR, LOW);         /* clear 74HC74 latched peak */
      laserOffAndSync();
      currentIndex.store(0, std::memory_order_relaxed);
      direction.store(1, std::memory_order_relaxed);
      const uint16_t p0   = SCALES[siNow].dacPos[0];
      const uint16_t thr0 = getHardwareDACThreshold(0);
      mcp4922Write(p0, thr0);
      esp_rom_delay_us(getGalvoSettleTime(lastPos, p0));
      lastPos      = p0;
      settleTimeUs = 400u;
      sweepState   = SweepState::MOVING;
      stateStartUs = (uint32_t)esp_timer_get_time();
      /* Suppress detection ~1.2 s while the AC-coupled comparator re-settles. */
      detectBlackout = true;
      detectArmAtUs  = stateStartUs + 1200000u;

      Serial.printf("[BEAM] RECOVER: scale=%d  scaleMargin=%u  thr0(DAC)=%u  galvo0=%u  (detect blackout 1.2s)\n",
                    siNow, (unsigned)scaleMargin[siNow], (unsigned)thr0, (unsigned)p0);
      continue;
    }

    /* [STUCK-FIX] Lightweight detection-state reset requested on a PLAY-MODE (or
     * scale) change.  harpAllNotesOff() drops the voices but the laser's
     * per-string physical-hold truth (stringActive[] + counters) lives here and
     * would otherwise survive the switch — a beam held across the change stays
     * "active" (white, no sound) until lifted, and can't retrigger.  Clear it so
     * the new mode starts from a clean slate.  No re-home / blackout: the scan is
     * already live and the comparator never went dark. */
    if (g_beamClearReq.exchange(false, std::memory_order_acq_rel)) {
      for (int s = 0; s < MAX_STRINGS; ++s) {
        stringActive[s]   = false;
        touchCounter[s]   = 0u;
        releaseCounter[s] = 0u;
        noteOnTime[s]     = 0u;
        lastHoldUs[s]     = 0u;
      }
      fastWrite(PIN_PEAK_CLR, LOW);   /* drop any latched peak */
    }

    /* ── Periodic maintenance (1 Hz) ──────────────────────────────────────── */
    const uint32_t nowMs = millis();
    if (nowMs - lastWatchdogMs >= 1000u) {
      lastWatchdogMs = nowMs;
      pollSyncHeartbeat();
      const bool link = appSyncActive.load(std::memory_order_relaxed);
      if (link != lastLinkState) {
        lastLinkState = link;
        displayDirty.store(true);
      }
    }

    /* ── Panic ──────────────────────────────────────────────────────────── */
    if (panicRequested.exchange(false, std::memory_order_relaxed))
      allNotesOff();

    /* ── Harp mode gate ─────────────────────────────────────────────────── */
    const HarpMode hMode = harpMode.load(std::memory_order_relaxed);
    if (hMode == HarpMode::OPENING || hMode == HarpMode::CLOSING) {
      tickAnimation();
      /* [CORE1-RECLAIM] vTaskDelay (NOT taskYIELD): this task is the highest
       * priority on Core 1, so taskYIELD never lets IDLE1 run — IDLE1 then fails
       * to feed the Task WDT during the boot OPENING animation → panic reboot
       * loop.  A 1-tick block yields to IDLE1 every frame (animation cadence is
       * wall-clock ANIM_STEP_INTERVAL_US, so 1 ms here is invisible).            */
      vTaskDelay(pdMS_TO_TICKS(1));
      continue;
    }
    if (hMode == HarpMode::CLOSED) {
      if (laserScreensaver.load(std::memory_order_relaxed)) {
        tickScreensaver();           /* gentle roving dot — no beam detection */
        vTaskDelay(pdMS_TO_TICKS(1)); /* [CORE1-RECLAIM] block → IDLE1 feeds WDT */
      } else {
        laserOffAndSync(); /* dark + safe (default) */
        vTaskDelay(pdMS_TO_TICKS(2));
      }
      continue;
    }

    /* ── Beam scan state machine ────────────────────────────────────────── */
    const uint32_t nowUs = (uint32_t)esp_timer_get_time();
    const int scaleIdx = harpScaleIndex.load(std::memory_order_relaxed) & (NUM_SCALES - 1);
    int ci = currentIndex.load(std::memory_order_relaxed) & 7;
    /* [LASER-SHOW v2] While the show runs the fan is a pure light output: hand
     * detection and D-BEAM expression are gated OFF (no false harp notes, no
     * stale expression), and the galvo visits a FIXED even 8-beam fan instead of
     * the selected scale's positions.  The hue ADSR still advances — but it is
     * driven by the SEQUENCER (groovebox harpHueNoteOn/Off), not by beam breaks. */
    const bool showOn = laserShowMode.load(std::memory_order_relaxed);
    switch (sweepState) {

      /* MOVING: wait for galvo to settle, RESET the 74HC74, then fire laser.
     * The LT1016 is AC-coupled and extremely fast, so it emits a burst of micro-
     * pulses on a beam break.  The 74HC74 latches the FIRST clean peak and holds
     * it for the whole dwell (no machine-gun retrigger).  Resetting here discards
     * any event captured during the galvo move so each string starts clean.      */
      case SweepState::MOVING:
        if (nowUs - stateStartUs >= settleTimeUs) {
          fastWrite(PIN_PEAK_CLR, LOW); /* reset 74HC74 (clear latched peak)    */
          resetLaserPhase();            /* Phase 0 → first pulse is always full [L2] */
          laserForString(ci);           /* Apply colour + hue ADSR envelope          */
          /* [SYNC] Tell the ADC DMA task which string is now illuminated so it
         * attributes the RMS amplitude to g_kalman_ac[ci]/g_last_good_data[ci].
         * Without this, dbeamLastStringIdx stays 0 forever and ALL expression
         * energy piles onto string 0 (strings 1-7 read stale/zero, and their
         * health watchdog trips after 1 s). */
          dbeamLastStringIdx.store((uint8_t)ci, std::memory_order_relaxed);
          /* [LASER-SHOW v2] In show mode keep the ADC gate CLOSED so the beam is
           * lit for colour only — no D-BEAM/expression attribution. */
          dbeamLit.store(!showOn, std::memory_order_relaxed); /* beam ON → ADC may sample ci */
          stateStartUs = (uint32_t)esp_timer_get_time();
          sweepState = SweepState::LASER_ON;
        }
        break;

      /* LASER_ON: wait LASER_STAB_US for the beam + comparator to settle, then ARM
     * the 74HC74 so it is ready to catch a trigger if a hand is on the beam.      */
      case SweepState::LASER_ON:
        if (nowUs - stateStartUs >= LASER_STAB_US) {
          fastWrite(PIN_PEAK_CLR, HIGH); /* arm 74HC74 (release clear)           */
          esp_rom_delay_us(2u);          /* 74HC74 setup+hold margin                    */
          stateStartUs = (uint32_t)esp_timer_get_time();
          sweepState = SweepState::DWELLING;
        }
        break;

      /* DWELLING: sample beam, advance hue ADSR, drive D-Beam, move to next */
      case SweepState::DWELLING:
        if (nowUs - stateStartUs >= BEAM_DWELL_US) {

          /* ── Advance hue ADSR for this string [L5][HARP-SPLIT] ───────── */
          harpHueAdvance(ci);

          /* [SAVE-FIX11] Post-save settle blackout: keep the per-string counters
         * pinned to "clear" and skip note evaluation until the comparator has
         * re-settled.  The scan/visuals above (hue, galvo, expression below)
         * keep running, so the beams look normal — they just don't trigger. */
          if (detectBlackout && (int32_t)(nowUs - detectArmAtUs) >= 0)
            detectBlackout = false; /* window elapsed → resume detection */
          if (detectBlackout || showOn) {
            /* [LASER-SHOW v2] Detection suppressed (post-save blackout OR show
             * mode) — pin the counters clear so a re-arm starts clean. */
            touchCounter[ci]   = 0u;
            releaseCounter[ci] = 0u;
          } else {

          /* ── Beam detection: LT1016 → 74HC74 peak latch → PIN_TRIGGER ────
         * Pull-up holds the line HIGH while armed-and-clear; a caught peak
         * (hand on beam) latches it LOW for the dwell.  LOW = blocked.       */
          const bool beamBrokenRaw = !fastPinRead(PIN_TRIGGER); /* LOW = blocked */
          /* [FOG] Differential fog-reject gate (no-op when disabled): a raw break
           * only counts as a real hand if this string's amplitude stands out from
           * the common-mode fog floor.  Advisory AND — can only suppress, never
           * fabricate a trigger, and never touches the D-BEAM/threshold logic. */
          const bool beamBrokenHW = beamBrokenRaw && fogAccept(ci);
          const uint8_t onNeeded = std::max((uint8_t)1u, scaleTouchConfirm[scaleIdx]);
          uint8_t offNeeded = std::max((uint8_t)1u, scaleReleaseConfirm[scaleIdx]);
          /* [SOLO-FIX] SOLO ping-pong on the 2nd-last note: a steadily-held beam
           * whose reflection momentarily dips would false-release then re-press,
           * re-stealing "king" from the other held beam (audible A/B/A/B).  SOLO
           * is monophonic last-note priority, so snappy per-string release matters
           * far less than rock-solid hold — give it ~4× release hysteresis so brief
           * dropouts can't fake a release.  POLY8/STRINGS keep their crisp release. */
          if (currentPlayMode.load(std::memory_order_relaxed) == PlayMode::SOLO)
            offNeeded = (uint8_t)std::min(60, (int)offNeeded * 4);

          /* ── Note ON / OFF detection — hardened against STUCK NOTES ───────
           * Two independent guarantees, both robust to the AC-coupled LT1016
           * emitting sparse FALSE breaks (the usual reason a note never released):
           *
           *  1. Asymmetric release: an in-progress release is only cancelled by a
           *     *sustained* re-touch (touchCounter rebuilt to holdCap ≥ 2).  The
           *     clear path decrements touchCounter, so an isolated stray break
           *     can't reach holdCap and therefore can't reset releaseCounter —
           *     the release keeps progressing to completion.
           *  2. Absolute fail-safe: if the beam has not been SOLIDLY held for
           *     beamStuckReleaseMs (globals.h, tunable), force the note off
           *     regardless of counters.
           *     A genuine hold refreshes lastHoldUs every dwell, so it is immune;
           *     only a beam that stopped being truly broken can time out.
           *
           * SOLO last-note priority stays in the harp engine's held stack
           * (harp.cpp); stringActive[] remains pure physical-hold truth so lifting
           * the newest beam falls back to an older held beam.                     */
          const uint8_t holdCap = (uint8_t)std::max<int>(2, (int)onNeeded);
          if (beamBrokenHW) {
            if (!stringActive[ci]) {
              if (touchCounter[ci] < onNeeded) ++touchCounter[ci];
              if (touchCounter[ci] >= onNeeded) {
                stringActive[ci]   = true;
                noteOnTime[ci]     = nowUs;
                lastHoldUs[ci]     = nowUs;
                releaseCounter[ci] = 0u;
                harpHueNoteOn(ci);            /* hue ADSR → ATTACK */
                harpPumpExpression();         /* [HARP-SPLIT] D-BEAM pump */
                harpNoteOn(ci, 100);
              }
            } else {
              /* Held: climb toward holdCap.  Only a confirmed sustained re-touch
               * cancels a pending release and refreshes the fail-safe timer. */
              if (touchCounter[ci] < holdCap) ++touchCounter[ci];
              if (touchCounter[ci] >= holdCap) {
                releaseCounter[ci] = 0u;
                lastHoldUs[ci]     = nowUs;   /* solidly held → keep alive */
              }
              harpPumpExpression();
            }

            /* ── Note-off path ───────────────────────────────────────────── */
          } else {
            if (touchCounter[ci] > 0) --touchCounter[ci];

            if (stringActive[ci]) {
              if (releaseCounter[ci] < offNeeded) ++releaseCounter[ci];
              const bool counterDone = (releaseCounter[ci] >= offNeeded);
              const uint32_t stuckUs = beamStuckReleaseMs * 1000u; /* 0 = disabled */
              const bool stuckTimeout =
                  (stuckUs > 0u) &&
                  ((uint32_t)(nowUs - lastHoldUs[ci]) >= stuckUs);
              const uint32_t holdWindow = (uint32_t)beamGateHoldMs * 1000u;
              if ((counterDone || stuckTimeout) &&
                  (uint32_t)(nowUs - noteOnTime[ci]) >= holdWindow) {
                releaseCounter[ci] = touchCounter[ci] = 0u;
                stringActive[ci]   = false;
                harpHueNoteOff(ci);           /* hue ADSR → RELEASE */
                noteOffHarp(ci);
                harpNoteOff(ci);              /* frees owner + DSP release */
              }
            } else {
              releaseCounter[ci] = 0u;
            }
          }
          } /* [SAVE-FIX11] end detect-armed guard */

          /* ── D-Beam expression decay ─────────────────────────────────────
         * Expression is a single GLOBAL (whole-reflection) estimate now, so
         * there is no per-string buffer to decay — just keep advancing it when
         * the current string is idle so the height falls back as the hand lifts.
         * [DBEAM-VOL] Also APPLY the decayed value through the router so the DSP
         * target returns to rest when the hand lifts (volume pedal auto-return,
         * cutoff/mod addends decay back to 0).  Previously routing ran only while
         * a beam was held, so the last value stayed frozen after lift.          */
          if (!showOn && !stringActive[ci]) routeDbeamExpression(updateDbeamExpression(ci));

          /* ── Advance to next string ──────────────────────────────────── */
          advanceIndex();
          ci = currentIndex.load(std::memory_order_relaxed) & 7;

          /* ── Direction-reversal detection (faint endpoint-beam fix) ─────
         * The scan is ping-pong; at each turnaround the galvo physically
         * reverses and overshoots/rings more than a straight one-string step,
         * which left the 2 endpoint beams with a faint tail.  When direction
         * flips, tag the next TWO moves (into the turnaround + out of it) for
         * extra dark settle so those beams come out as clean as the middle six. */
          const int8_t curScanDir = (int8_t)direction.load(std::memory_order_relaxed);
          if (curScanDir != lastScanDir) {
            reverseSettleCnt = 2u;
            lastScanDir = curScanDir;
          }

          const uint16_t nomPos = showOn ? SHOW_DAC_POS[ci] : SCALES[scaleIdx].dacPos[ci];
          uint16_t targetPos = std::min(std::max(nomPos, GALVO_PHYS_MIN), GALVO_PHYS_MAX);

          /* ── STRINGS mode: physical string-vibration emulation [HARP-SPLIT] ─
         * The ADSR-driven wobble math now lives in harp.cpp
         * (harpStringVibratoTarget); laser only decides WHEN to apply it and
         * renders the returned galvo target.  envNorm → 0 on release inside
         * the harp call, so the vibration decays to a stop with the note. */
          if (!showOn && currentPlayMode.load(std::memory_order_relaxed) == PlayMode::STRINGS && hMode == HarpMode::OPEN && !stringActive[ci]) {
            targetPos = harpStringVibratoTarget(ci, nomPos, scaleIdx);
          }

          /* ── Move galvo dark — settle tracked by MOVING state ────────── */
          dbeamLit.store(false, std::memory_order_relaxed); /* beam goes dark → ADC gates off */
          settleTimeUs = getGalvoSettleTime(lastPos, targetPos);
          if (reverseSettleCnt > 0u) { /* turnaround move → let ringing decay */
            settleTimeUs += GALVO_REVERSE_EXTRA_US;
            --reverseSettleCnt;
          }
          galvoMoveDark(targetPos, getHardwareDACThreshold(ci), false); /* non-blocking */

          /* Telemetry scope — auto-range per view (see display.cpp drawTelemetryOscilloscope). */
          const TelemetryView view = currentScopeView.load(std::memory_order_relaxed);
          if (view != TelemetryView::OFF && view != TelemetryView::DC_LEVEL &&
              view != TelemetryView::STACK_STATS && view != TelemetryView::CAL_BASELINE) {
            float sampleVal = 0.0f;
            float floorScale = 1.0f;
            switch (view) {
              case TelemetryView::RAW_AC:
                portENTER_CRITICAL(&patchMux);
                sampleVal = g_last_good_data[ci].acAmplitude - NOISE_FLOOR_MV;
                portEXIT_CRITICAL(&patchMux);
                if (sampleVal < 0.0f) sampleVal = 0.0f;
                floorScale = NOISE_FLOOR_MV + 10.0f; /* mV above subtracted floor → full scale */
                break;
              case TelemetryView::CC_OUT_14BIT:
                sampleVal  = (float)dbeamAmplitude.load(std::memory_order_relaxed);
                floorScale = 16383.0f;        /* fixed 14-bit expression scale */
                break;
              case TelemetryView::SIGNAL_SNR:
                portENTER_CRITICAL(&patchMux);
                sampleVal = g_last_good_data[ci].health.snr;
                portEXIT_CRITICAL(&patchMux);
                floorScale = 1.5f;            /* SNR ratio (typical 0..5) */
                break;
              default: break;
            }

            /* Auto-range: fast attack to new peaks, slow release so old peaks
             * fade; reset when the user switches views. */
            if (view != scopePeakView) { scopePeak = floorScale; scopePeakView = view; }
            const float a = fabsf(sampleVal);
            if (a > scopePeak) scopePeak = a;
            else               scopePeak *= 0.992f;
            const float fs = std::max(scopePeak, floorScale);

            const int wp = scopeWritePtr.load(std::memory_order_relaxed);
            scopeHistory[wp] = (uint8_t)std::min(std::max((a / fs) * 245.f, 0.f), 255.f);
            scopeWritePtr.store((wp + 1) % 128, std::memory_order_relaxed);
          }

          stateStartUs = (uint32_t)esp_timer_get_time();
          sweepState = SweepState::MOVING;
        }
        break;

      default:
        sweepState = SweepState::MOVING;
        break;
    } /* end switch */

    /* ── [CORE1-LOCAL] Precise phase wait + periodic breathe ───────────────────
     * phaseUs is computed HERE from the CURRENT sweepState (post-transition) so
     * the remaining-time wait always matches the state we are about to wait in.
     * The wait is a Core-1-local esp_rom_delay_us busy-wait: exact µs timing that
     * does not depend on the Core-0 esp_timer task (which AudioSynth can starve).
     * On wake the phase has elapsed, so the switch above fires its transition on
     * the next iteration. */
    {
      uint32_t phaseUs = 0u;
      switch (sweepState) {
        case SweepState::MOVING:   phaseUs = settleTimeUs;  break;
        case SweepState::LASER_ON: phaseUs = LASER_STAB_US; break;
        case SweepState::DWELLING: phaseUs = BEAM_DWELL_US; break;
        default: break;
      }
      const uint32_t elapsed = (uint32_t)esp_timer_get_time() - stateStartUs;
      if (phaseUs > elapsed) esp_rom_delay_us(phaseUs - elapsed);

      /* Breathe: hand Core 1 to IDLE1 / MidiUsbRx / SeqSysexOut for
       * one tick every LASER_BREATHE_MS, but ONLY at the start of a dark MOVING
       * phase so a lit beam is never chopped.  This is what feeds the idle-task
       * watchdog on Core 1 and lets the data tasks run; without it the busy-wait
       * above would starve IDLE1 and trip the watchdog → reboot. */
      const uint32_t nowBrMs = millis();
      if (sweepState == SweepState::MOVING &&
          (uint32_t)(nowBrMs - lastBreatheMs) >= LASER_BREATHE_MS) {
        lastBreatheMs = nowBrMs;
        vTaskDelay(1);
      }
    }
  }
}
