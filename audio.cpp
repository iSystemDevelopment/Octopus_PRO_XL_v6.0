/* ═════════════════════════════════════════════════════════════════════════════
 * Octopus PRO XL v6.0.00 — Laser Harp Groovebox
 * © 2026 DIODAC ELECTRONICS / iSystem. All Rights Reserved.
 *
 * PROPRIETARY AND CONFIDENTIAL. Unauthorized copying, distribution, modification,
 * or use of this software or firmware, in whole or in part, is strictly prohibited
 * without prior written permission from DIODAC ELECTRONICS.
 * ═════════════════════════════════════════════════════════════════════════════
 * audio.cpp — v6.0.00
 * ═════════════════════════════════════════════════════════════════════════════ */
#include "audio.h"
/* esp_log.h removed — ESP_LOGI debug calls were leftover agent instrumentation
 * that blocked the NvsWorker task during saves via Serial.flush(). */

/* Ensure laser symbols available even if include order varies */
#include "laser.h"
#include "midi.h"   /* txSysex (App session ACK) */

/* ── Heap buffers for the 3-engine mixer ────────────────────────────────── */
static int16_t* audio_raw_harpH  = nullptr;  /* HARP synth mono   */
static int16_t* audio_raw_seqH   = nullptr;  /* SEQ synth mono    */
static int16_t* audio_raw_drumH  = nullptr;  /* DRUM synth mono   */
static int16_t* audio_raw_outH   = nullptr;  /* master stereo out */

/* ─────────────────────────────────────────────────────────────────────────────
 * init_i2s_hardware
 * ─────────────────────────────────────────────────────────────────────────────*/
esp_err_t init_i2s_hardware() {
  i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  chan_cfg.auto_clear = true;
  esp_err_t err = i2s_new_channel(&chan_cfg, &tx_handle, NULL);
  if (err != ESP_OK) return err;
  i2s_std_config_t std_cfg = {
    .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
    .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                     I2S_SLOT_MODE_STEREO),
    .gpio_cfg = { .bclk = PIN_I2S_BCLK, .ws = PIN_I2S_WS,
                  .dout = PIN_I2S_DOUT,  .din = I2S_GPIO_UNUSED }
  };
  err = i2s_channel_init_std_mode(tx_handle, &std_cfg);
  if (err != ESP_OK) return err;
  return i2s_channel_enable(tx_handle);
}

/* [P0 CLEANUP] song_mode_check_advance() removed — it had no call site.  Song-
 * chain advance is handled in real time by song_advance_rt() (groovebox.cpp),
 * driven from the audio task at pattern wrap.                                */

/* ─────────────────────────────────────────────────────────────────────────────
 * MASTER LIMITING  [A6 / LIM1]
 *
 * The single, transparent master limiter now lives entirely inside
 * MasterFX::process() (effect.h): a mid/side soft-knee limiter that is
 * bit-exact below its threshold (0.8) and asymptotically approaches the
 * ceiling (0.98) above it.  fx_process_multi_buf_safe() already clamps the
 * float→int16 conversion, so the master bus never wraps.
 *
 * The previous second-stage tanh limiter (applySoftLimiter, drive 1.4×) was
 * removed: it coloured and boosted EVERY sample — even quiet passages — which
 * is exactly the non-transparent behaviour we wanted to eliminate.  One
 * transparent limiter is now the whole story.
 * ─────────────────────────────────────────────────────────────────────────────*/

/* ─────────────────────────────────────────────────────────────────────────────
 * audio_synthesis_task — Core 0, priority 24  [A1][A2][A6]
 *
 * This task is the ORCHESTRATOR, not the mixer.  Per-engine synthesis is owned
 * by each instrument module; the multibuffer FX mix is owned by effect.cpp.
 * This task only sequences the per-buffer pipeline and feeds the I2S DMA.
 *
 * Per-DMA-buffer pipeline (512 frames @ 44.1 kHz → ~11.61 ms):
 *   1. FX preset change detection (cold-loaded off the hot path)
 *   2. Sync D-BEAM drum sends into the FX struct (syncDrumSends; self-locks)
 *   3. Sample-locked step engine tick (sequencer_render_block, groovebox.cpp)
 *   4. Fill per-engine MONO buffers:
 *        harp_synth_fill_buf  → audio_raw_harpH   (harp.cpp)
 *        gb_seq_fill_buf      → audio_raw_seqH    (groovebox.cpp)
 *        drum_fill_buf        → audio_raw_drumH   (groovebox.h)
 *   5. Multibuffer FX MIX → stereo: fx_process_multi_buf_safe (effect.cpp)
 *        (3 mono → inserts → aux sends → panned sum → master FX/limiter)
 *   6. Load monitor + adaptive quality scaling [LOAD-SHED]
 *   7. I2S DMA write of the stereo master buffer (audio_raw_outH)
 * ─────────────────────────────────────────────────────────────────────────────*/
void IRAM_ATTR audio_synthesis_task(void* pvParameters) {
  static_assert(DMA_BUFFER_FRAMES <= FX_BUF_SIZE,
    "DMA_BUFFER_FRAMES > FX_BUF_SIZE: increase FX_BUF_SIZE in effect.h");

  /* Block until setup() has synced live patches and raised g_systemReady — prevents
   * Core-0 DSP + IPC driver callbacks from racing incomplete boot state. */
  while (!g_systemReady.load(std::memory_order_acquire))
    vTaskDelay(pdMS_TO_TICKS(1));

  size_t bytes_written = 0;
  int    lastMasterFx  = -1;
  int    lastHarpFxA   = -1, lastHarpFxB  = -1;
  int    lastSeqFxA    = -1, lastSeqFxB   = -1;
  int    lastDrumFxA   = -1, lastDrumFxB  = -1;

  static constexpr float kPeriodUs =
    (1000000.0f * (float)DMA_BUFFER_FRAMES) / (float)SAMPLE_RATE;  /* ~11610 µs @ 44.1 k */

  float    loadEma    = 0.0f;
  uint32_t loHoldCnt  = 0u;
  int      s_shedTier = -1;  /* [aud OPT-1] cached load-shed tier; -1 = uninit  */

  /* [IDLE-GUARD] Core-0 anti-starvation.  AudioSynth is the top priority on
   * Core 0; the lower cooperative tasks (esp_timer@22, dbeam_adc@19, OledRender,
   * ControlPoll) and IDLE0 only run when this task relinquishes the core.  In
   * steady state that happens naturally when i2s_channel_write blocks on a full
   * DMA ring; under > 100 % load the over-budget branch yields.  But in the
   * exact-~100 % band neither fires, so without a guaranteed periodic relinquish
   * IDLE0 can miss the 5 s task-watchdog deadline → Guru Meditation reboot.
   * lastYieldMs tracks the last real relinquish; AUDIO_IDLE_GUARD_MS forces one
   * tick if we have been CPU-bound for too long (click-safe: DMA ring ≈ 30 ms). */
  static constexpr uint32_t AUDIO_IDLE_GUARD_MS = 250u;
  uint32_t lastYieldMs = millis();
  esp_task_wdt_add(NULL); /* monitored + named in any WDT report */

  audioTaskReady.store(true, std::memory_order_release);

  for (;;) {
    esp_task_wdt_reset();

    /* ── 1. FX preset change detection (take midiMutex briefly) ───────── */
    /* FX presets are loaded outside the hot path to avoid stalling DSP.   */
    /* [aud OPT-3] Templated loader (auto) instead of a void(*)(int) pointer, so
     * the compiler can inline each loadHarpFx/loadSeqFx/… call site rather than
     * jumping through a Flash-resident function pointer from the IRAM audio task. */
    auto tryLoad = [&](int& last, int cur, auto loader_fn) {
      if (cur == last || cur < 0 || cur > 15) return;
      if (!midiMutex || xSemaphoreTake(midiMutex, 0) != pdTRUE) return;
      loader_fn(cur);
      last = cur;
      xSemaphoreGive(midiMutex);
    };
    { const int idx = masterFxIndex.load(std::memory_order_relaxed);
      if (idx != lastMasterFx && midiMutex && xSemaphoreTake(midiMutex, 0) == pdTRUE) {
        fx.loadMasterFx(idx); lastMasterFx = idx; xSemaphoreGive(midiMutex);
      }
    }
    tryLoad(lastHarpFxA, harpFxIndex  .load(std::memory_order_relaxed), [](int i){ loadHarpFx (i); });
    tryLoad(lastHarpFxB, harpFxIndexB .load(std::memory_order_relaxed), [](int i){ loadHarpFxB(i); });
    tryLoad(lastSeqFxA,  seqFxIndex   .load(std::memory_order_relaxed), [](int i){ loadSeqFx  (i); });
    tryLoad(lastSeqFxB,  seqFxIndexB  .load(std::memory_order_relaxed), [](int i){ loadSeqFxB (i); });
    tryLoad(lastDrumFxA, drumFxIndexA .load(std::memory_order_relaxed), [](int i){ loadDrumFx (i); });
    tryLoad(lastDrumFxB, drumFxIndexB .load(std::memory_order_relaxed), [](int i){ loadDrumFxB(i); });

    /* ── 2./3. Per-frame FX param sync ──────────────────────────────────────
     * Drum sends are the only per-frame writes needed: they change rapidly
     * from D-BEAM DbeamRoute.  All other FX parameters are maintained by the
     * patches.h apply* functions which write directly to fx struct members
     * and atomics on every parameter change — no per-frame polling required.
     * The effect engine reads aux bus / EQ / tube / DJ params from its own
     * internal state set during those apply* calls.  [aud Fix A]              */
    syncDrumSends();

    /* ── 4. DSP frame timing start ──────────────────────────────────────── */
    const int64_t t0 = esp_timer_get_time();

    /* ── 5. Sample-locked step engine [TIMING-LOCK] ─────────────────────────
     * The sequencer's musical clock now lives here, driven by the exact frame
     * count of this DMA buffer, so tempo is locked to the audio crystal and
     * cannot swing under DSP/FX load.  Advances ticks, fires internal seq+drum
     * voices in-task (no cross-task voice race), and pushes App STEP_SYNC /
     * SONG_POS SysEx echoes to the lock-free out-ring drained by SeqSysexOut. */
    sequencer_render_block(DMA_BUFFER_FRAMES);

    /* ── 6. Fill per-engine mono buffers ────────────────────────────────── */
    /* livePatch arrays are maintained by patches.h apply* functions [A1].
     * No updateHarpPatch/updateSeqPatch calls needed here.                  */
    const bool hOn = harp_synth_fill_buf(audio_raw_harpH, DMA_BUFFER_FRAMES);
    const bool sOn = gb_seq_fill_buf(audio_raw_seqH, DMA_BUFFER_FRAMES);
    const bool dOn = drum_fill_buf      (audio_raw_drumH, DMA_BUFFER_FRAMES);

    /* ── 7. Multi-engine FX mix → stereo output ─────────────────────────── */
    fx_process_multi_buf_safe(audio_raw_harpH, audio_raw_seqH, audio_raw_drumH,
                               audio_raw_outH,  DMA_BUFFER_FRAMES, hOn, sOn, dOn);

    /* ── 8. Master limiting handled inside MasterFX (single transparent
     *       soft-knee limiter); no second-stage limiter needed. [LIM1] ────── */

    /* ── 9. Load monitoring + adaptive quality scaling [LOAD-SHED] ──────────
     * ASYMMETRIC tracking: FAST attack (shed within ~1–2 buffers when load
     * spikes) + SLOW release (recover gently, no quality hunting).  The old
     * single 0.05 EMA needed ~20 buffers (~210 ms) to react — far too slow to
     * stop the underruns that produced the crackle.  Three levers are wired:
     *   g_svf_oversample → SVF filter oversampling (harp.cpp + groovebox.cpp) — biggest win
     *   g_seq_voice_cap  → sequencer polyphony cap (groovebox.cpp render_block)
     *   g_aux_mode       → aux reverb tail length (effect.cpp)  [now live]    */
    /* [aud OPT-2] multiply by precomputed reciprocal (~4 cyc) vs divide (~14). */
    static constexpr float kInvPeriodUs = 1.0f / kPeriodUs;
    const float computeUs = (float)(esp_timer_get_time() - t0);
    const float load      = computeUs * kInvPeriodUs;
    const float aCoef     = (load > loadEma) ? 0.5f : 0.02f; /* attack : release */
    loadEma += aCoef * (load - loadEma);
    const int pct = (int)(loadEma * 100.0f);
    g_audio_load_pct.store((uint8_t)std::min(100, std::max(0, pct)));

    /* GRADUATED LOAD-SHED [LOAD-SHED2]
     * Shed signal = max(instantaneous, EMA): reacts to a single-buffer spike
     * immediately so we cut cost BEFORE the underrun, not after.  Levers are
     * applied in order of LEAST-audible first, so dropped notes (the most
     * audible artefact) are the very last resort:
     *   >0.72  aux reverb tail 3 s → 0.5 s        (barely audible)
     *   >0.80  SVF oversample ×2 → ×1             (subtle filter detail)
     *   >0.85  osc2 detune/unison layer OFF        (thins dense patches)
     *   >0.88  seq poly cap 8 → 6                  (only on dense patterns)
     *   >0.93  seq poly cap → 5
     *   >0.98  seq poly cap → 4                    (emergency)
     * osc2 sheds BEFORE notes are dropped (a missing unison layer is far less
     * audible than a missing note).  Restore only after a sustained calm spell. */
    /* [aud OPT-1] Cache the shed tier; the cross-core lever atomics are written
     * ONLY on a tier transition, not every buffer.  At a steady overload this
     * cuts ~282 redundant atomic stores/s (cache-coherency traffic to Core 1).
     * Behaviour matches the old per-buffer if-chain: each tier sets the same
     * levers it used to, and lower tiers leave higher-tier-only levers as-is.   */
    const float shed = std::max(load, loadEma);
    const int newTier =
        (shed > 0.98f) ? 6 :
        (shed > 0.93f) ? 5 :
        (shed > 0.88f) ? 4 :
        (shed > 0.85f) ? 3 :
        (shed > 0.80f) ? 2 :
        (shed > 0.72f) ? 1 : 0;

    if (newTier > 0) {
      loHoldCnt = 0u;
      if (newTier != s_shedTier) {
        s_shedTier = newTier;
        switch (newTier) {
          case 6: g_svf_oversample.store(1); g_seq_voice_cap.store(4);
                  g_aux_mode.store(1);        g_osc2_enable.store(false); break;
          case 5: g_svf_oversample.store(1); g_seq_voice_cap.store(5);
                  g_aux_mode.store(1);        g_osc2_enable.store(false); break;
          case 4: g_svf_oversample.store(1); g_seq_voice_cap.store(6);
                  g_aux_mode.store(1);        g_osc2_enable.store(false); break;
          case 3: g_svf_oversample.store(1);
                  g_aux_mode.store(1);        g_osc2_enable.store(false); break;
          case 2: g_svf_oversample.store(1); g_aux_mode.store(1);         break;
          case 1:                            g_aux_mode.store(1);         break;
        }
      }
    } else if (loadEma < 0.55f) {
      /* Comfortable: restore full polyphony + normal aux tail.  [HEADROOM-A]
       * SVF ×2 is only re-enabled when the engine is nearly idle (<0.35) so it
       * never flips ×1↔×2 mid-performance (which would colour the filter).  osc2
       * + voice cap restore together after the calm hold so they never hunt. */
      if (++loHoldCnt > 300u && s_shedTier != 0) { /* ~3.2 s of headroom first */
        s_shedTier = 0;
        g_seq_voice_cap.store(SEQ_POLYPHONY); g_aux_mode.store(2); g_osc2_enable.store(true);
        if (loadEma < 0.35f) g_svf_oversample.store(2);
      }
    }
    /* else: 0.55 ≤ loadEma ≤ 0.72 — neither shedding nor restoring; hold state. */

    /* ── Over-budget watchdog yield [WDT-SAFE] ──────────────────────────────
     * CRITICAL: when compute exceeds one I2S period the DMA ring is draining
     * faster than we refill it, so i2s_channel_write() returns IMMEDIATELY
     * (there is always free space) and never blocks.  Without an explicit yield
     * here the priority-24 audio task spins forever, the Core 0 IDLE task never
     * runs, and the Task Watchdog fires → "Guru Meditation" panic under heavy
     * load.  A 1 ms yield feeds the watchdog and lets the just-applied load-shed
     * take effect.  Musical timing is unaffected: the step engine advances by a
     * fixed frame count per buffer regardless of wall-clock, so the groove only
     * stretches smoothly during the overload instead of swinging.            */
    bool yielded = false;
    if (computeUs >= kPeriodUs) {
      vTaskDelay(pdMS_TO_TICKS(1));
      yielded = true;
    }

    /* ── 10. I2S DMA write ───────────────────────────────────────────────── */
    /* [FIX-I2S-TIMEOUT] Timeout reduced 100 ms → 50 ms.  Old value was 8×
     * the buffer period; if the DMA stalls, Core 0 would block for 100 ms
     * starving dbeam_adc(19), OledRender(14), ControlPoll(10) and IDLE0.
     * 50 ms is still 4× the buffer period (ample for any realistic DMA lag)
     * while limiting the stall window to half of the old worst case.          */
    const int64_t wrStart = esp_timer_get_time();
    const esp_err_t r = i2s_channel_write(tx_handle, audio_raw_outH,
                                           DMA_BUFFER_STEREO_BYTES,
                                           &bytes_written, pdMS_TO_TICKS(50));
    /* i2s_channel_write blocks (relinquishing Core 0) only while the DMA ring is
     * full — the normal < 100 % steady state.  A return in well under a frame
     * means the ring was draining (at/over budget) and the core was NOT yielded. */
    if ((esp_timer_get_time() - wrStart) >= 1000) yielded = true;
    if (r != ESP_OK && tx_handle) {
      i2s_channel_disable(tx_handle);
      vTaskDelay(pdMS_TO_TICKS(10));
      i2s_channel_enable(tx_handle);
      yielded = true;
    }
    /* [FIX-PARTIAL-WRITE] ESP_OK does not guarantee a full write; if the DMA
     * ring accepted fewer than the requested bytes the output frame is shorter
     * than the engine produced, causing a progressive audio de-sync that
     * manifests as periodic clicks or pitch drift.  Treat partial writes the
     * same as a driver error: disable/re-enable to resync the ring.           */
    if (r == ESP_OK && bytes_written < DMA_BUFFER_STEREO_BYTES && tx_handle) {
      i2s_channel_disable(tx_handle);
      vTaskDelay(pdMS_TO_TICKS(5));
      i2s_channel_enable(tx_handle);
      yielded = true;
    }

    /* ── 11. [IDLE-GUARD] Guaranteed Core-0 relinquish ──────────────────────
     * If we have not naturally yielded for AUDIO_IDLE_GUARD_MS (the exact-~100 %
     * band), force one tick so esp_timer/dbeam/OLED/control AND IDLE0 are served
     * before the 5 s task-watchdog fires.  Rare in practice — load-shedding pulls
     * sustained load back below budget within a few buffers. */
    const uint32_t nowMs = millis();
    if (yielded) {
      lastYieldMs = nowMs;
    } else if ((uint32_t)(nowMs - lastYieldMs) >= AUDIO_IDLE_GUARD_MS) {
      lastYieldMs = nowMs;
      vTaskDelay(pdMS_TO_TICKS(1));
    }
  }
}

/* ─────────────────────────────────────────────────────────────────────────────
 * control_surface_task — Core 0, priority 16  [ControlPoll]
 *
 * Polls ESP32Encoder::getCount() at 200 Hz.  MUST stay on the same core as
 * ESP32Encoder::isrServiceCpuCore (interface.cpp initHardwareInterface).
 *
 * [A4] When app is connected, only transport and BPM encoder actions reach
 * hardware; all other parameter changes are driven exclusively by the App.
 * ─────────────────────────────────────────────────────────────────────────────*/
void IRAM_ATTR control_surface_task(void* pvParameters) {
  while (!g_systemReady.load(std::memory_order_acquire))
    vTaskDelay(pdMS_TO_TICKS(1));

  TickType_t   lastWake = xTaskGetTickCount();
  const TickType_t kPer = pdMS_TO_TICKS(5);   /* 200 Hz — matches v28.9 */
  uint32_t lastStackMs   = 0u;
  uint32_t lastSerialMs  = 0u;
  for (;;) {
    updateHardwareInterface();

    /* Recurring stack / heap telemetry — OLED STACK_STATS view + Serial log.
     * Stats sample every 5 s; Serial line every 30 s (less spam).           */
    const uint32_t nowMs = millis();
    if (nowMs - lastStackMs >= 5000u) {
      lastStackMs = nowMs;
      updateTaskStackStats();
      /* First sample also logs to Serial; thereafter every 30 s. */
      if (lastSerialMs == 0u || nowMs - lastSerialMs >= 30000u) {
        lastSerialMs = nowMs;
        printInterfaceStats();
      }
    }

    vTaskDelayUntil(&lastWake, kPer);
  }
}

/* ─────────────────────────────────────────────────────────────────────────────
 * display_refresh_task — Core 0, priority 18  [A5]
 *
 * When the App is connected: show a static "APP CONNECTED" splash and do not
 * render the full menu tree.  This avoids parameter display churn, prevents
 * accidental parameter changes from encoder movement on screen, and makes it
 * visually clear that the App is the active control surface.
 * ─────────────────────────────────────────────────────────────────────────────*/
void IRAM_ATTR display_refresh_task(void* pvParameters) {
  while (!g_systemReady.load(std::memory_order_acquire))
    vTaskDelay(pdMS_TO_TICKS(1));

  TickType_t   lastWake = xTaskGetTickCount();
  const TickType_t kPer = pdMS_TO_TICKS(33);   /* 30 Hz */
  uint16_t lastDbeam    = 0u;
  uint32_t lastDbeamDrawMs = 0u;
  bool     prevConnected = false;
  uint8_t  lastStepDrawn = 0xFFu;
  bool     prevSaveReq   = false;   /* [SAVE-FRAME] edge-detect save request */

  for (;;) {
    const bool appConn = isAppConnected();

    /* Force a redraw on connection state change */
    if (appConn != prevConnected) {
      prevConnected = appConn;
      displayDirty.store(true, std::memory_order_relaxed);
    }

    /* [PLAYHEAD] Sequencer step-bar animation.  seqCurrentStep is advanced by the
     * sample-locked clock in the audio task, but NOTHING marked the screen dirty
     * when it changed — so the OLED playhead (DRAW_STEP_BARGRAPH) only moved when
     * some OTHER event happened to redraw (D-BEAM amplitude jitter from a hand on
     * the beam, an encoder turn…), making it look frozen-then-jumpy and out of
     * sync with BPM.  Now a step change requests a CHEAP partial redraw of just
     * the bar band (see below) instead of a full-screen flush — the bar tracks
     * the clock exactly while the 30 Hz cap coalesces fast tempos.  Drives both
     * the SEQ dashboard and the "APP CONNECTED" splash step bar. */
    bool stepChanged = false;
    if (seqPlaying.load(std::memory_order_relaxed)) {
      const uint8_t curStep = seqCurrentStep.load(std::memory_order_relaxed);
      if (curStep != lastStepDrawn) {
        lastStepDrawn = curStep;
        stepChanged   = true;
      }
    } else {
      lastStepDrawn = 0xFFu;   /* re-arm so the first step after PLAY repaints */
    }

    /* D-BEAM amplitude change → dirty (visual feedback even when connected).
     * [PERF] Throttled: a full OLED redraw is a ~9 ms blocking I2C flush on
     * Core 0.  During active D-BEAM play the amplitude jitters every frame, so
     * an unthrottled "redraw on >128 change" pushed up to 30 such flushes/s onto
     * the audio core.  Cap D-BEAM-driven redraws to ~10 Hz with a wider
     * threshold — still smooth for a bargraph, ~20 fewer flushes/s under play.
     * Real state changes (transport/menu/encoder) still redraw immediately via
     * their own displayDirty writes; this only gates the amplitude animation. */
    /* [aud OPT-4] one millis() read per frame, reused by the toast timer below. */
    const uint32_t nowMs = millis();
    const uint16_t amp = (uint16_t)dbeamAmplitude.load(std::memory_order_relaxed);
    /* [aud OPT-5] unsigned delta — no std::abs overload ambiguity. */
    const uint16_t dbeamDelta = (amp > lastDbeam) ? (uint16_t)(amp - lastDbeam)
                                                  : (uint16_t)(lastDbeam - amp);
    /* [DISP-OPT2] D-BEAM amplitude change → partial region update, not full dirty.
     * Previously displayDirty=true triggered clearDisplay + full drawHarpDashboard
     * (20+ GFX calls) even though only the 9-pixel D-BEAM bar strip changed.
     * renderDbeamBarIfVisible() repaints only y=DASH_DBEAM_Y..SCREEN_H-1 (same as
     * renderStepBarRegionIfVisible for the step bar).  The I2C cost is identical
     * (only 1-2 pages change), but clearDisplay + full dashboard render is skipped,
     * saving ~200-400 µs of Core-0 GFX work at up to 10 Hz.                       */
    bool dbeamChanged = false;
    if (dbeamDelta > 256 &&
        (uint32_t)(nowMs - lastDbeamDrawMs) >= 100u) {
      lastDbeam       = amp;
      lastDbeamDrawMs = nowMs;
      dbeamChanged    = true;
    }

    const bool scope = (currentScopeView.load(std::memory_order_relaxed) != TelemetryView::OFF);
    /* [C9b] No more "redraw every frame for 2.5 s after a turn" (manOv): that
     * forced a blocking full-screen I2C flush on every 33 ms frame while the user
     * was browsing, starving the control task.  The encoder / buttons now set
     * displayDirty on every change, so the screen still tracks scrolling (one
     * coalesced redraw per 30 Hz frame) but goes quiet the instant input stops. */
    /* [PERSIST-UI] Keep redrawing while the DONE!/FAILED! toast is on screen, plus ONE frame
     * after it expires so the pill is cleanly erased (otherwise it would linger
     * until the next unrelated redraw). */
    static bool toastPrev = false;
    static bool failPrev  = false;
    const bool  toastNow  = (int32_t)(g_saveFlashMs.load(std::memory_order_relaxed) - nowMs) > 0;
    const bool  failNow   = (int32_t)(g_saveFailFlashMs.load(std::memory_order_relaxed) - nowMs) > 0;
    const bool  statusHold = (int32_t)(g_oledStatusHoldMs.load(std::memory_order_relaxed) - nowMs) > 0;
    static bool statusHoldPrev = false;
    /* Keep redrawing while EITHER pill is up, plus ONE frame after EITHER expires
     * so it is cleanly erased.  [FIX] failPrev mirrors toastPrev — previously only
     * the DONE pill got the trailing erase frame, so a FAILED pill lingered on the
     * OLED forever until some unrelated value redrew the screen. */
    if (toastNow || failNow || toastPrev || failPrev)
      displayDirty.store(true, std::memory_order_relaxed);
    toastPrev = toastNow;
    failPrev  = failNow;
    if (!statusHold && statusHoldPrev) {
      /* oledStatusHold expired — restore APP CONNECTED splash / dashboard. */
      menuState.store(MenuState::IDLE, std::memory_order_relaxed);
      displayDirty.store(true, std::memory_order_relaxed);
    }
    statusHoldPrev = statusHold;

    /* [SAVE-FRAME] On the rising edge of a save request, force ONE redraw NOW so
     * the "SAVING…" pill is painted during the ~40 ms pre-arm window — BEFORE
     * settings_save_task arms g_saveArmed and the flash write disables the cache.
     * The frame then holds frozen (showing SAVING…) through the cache-off write
     * instead of a stale mid-render; the SAVED toast lands right after. */
    const bool saveReqNow = g_saveRequest.load(std::memory_order_acquire);
    if (saveReqNow && !prevSaveReq) displayDirty.store(true, std::memory_order_relaxed);
    prevSaveReq = saveReqNow;

    /* [MATRIX-PH] The SEQ MATRIX playhead is a per-row column underline spanning
     * the full grid height, so it can't ride the cheap bottom-band partial redraw
     * (that only repaints y 53..63).  While the grid is the active view AND the
     * sequencer is playing, force a full matrix re-render on each step so the
     * column tracks the clock live.  Bounded to the step rate; page-diff flush
     * still ships only the pages the moving column touches; goes silent the moment
     * play stops or the user leaves the grid. */
    const bool matrixStep = stepChanged && viewIsSeqMatrix();
    /* [EDGE] The edge-comp editor shows LIVE per-string beam-broken state, so it
     * must redraw every frame (like the telemetry scope) while open. */
    const bool edgeEdit = edgeEditOpen.load(std::memory_order_relaxed);
    /* [SAVE-FIX5] During the NVS write window (g_saveArmed) the flash cache is
     * disabled and the OTHER core is stalled by IDF's IPC.  If the OLED task is
     * mid-I2C transaction at that instant, the Wire driver's internal mutex is
     * released from the wrong context → "assert xTaskPriorityDisinherit".  The
     * SAVING pill was already painted on the pre-arm edge, so freeze ALL OLED
     * I/O while armed (matches the documented "hold frozen through the write").
     * Do NOT consume displayDirty here — the SAVED toast redraw fires after the
     * window when settings_save_task re-sets displayDirty. */
    const bool saveArmedNow = g_saveArmed.load(std::memory_order_acquire);
    bool draw = false;
    if (!saveArmedNow)
      draw = displayDirty.exchange(false, std::memory_order_relaxed) || scope || matrixStep || edgeEdit;

    if (draw && !statusHold && xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
      display.clearDisplay();
      if (appConn)
        drawAppConnectedPage();   /* static splash [A5] — defined in display.cpp */
      else
        renderUIState();          /* full menu / dashboard rendering              */
      drawSaveToastIfActive();    /* [PERSIST-UI] DONE!/FAILED!/WAIT pill over any view */
      /* [PERF] Page-diff flush: only the framebuffer pages that actually changed
       * are pushed over I2C, so a full re-render of a mostly-static screen still
       * costs almost nothing on the bus.  This is what makes BPM/scale/preset/
       * menu/slider/telemetry/D-BEAM updates cheap with no per-element code. */
      displayFlushDiff();
      xSemaphoreGive(i2cMutex);
    } else if (!saveArmedNow && !statusHold && stepChanged && xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
      /* [PERF] Playhead-only path: repaint just the bar band (no full clear, so
       * the GFX render cost stays tiny) then page-diff flush → only the ~2 bar
       * pages hit the bus.  Skips entirely if the view has no step bar. */
      if (renderStepBarRegionIfVisible())
        displayFlushDiff();
      xSemaphoreGive(i2cMutex);
    } else if (!saveArmedNow && !statusHold && dbeamChanged && xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
      /* [DISP-OPT2] D-BEAM region-only path: mirrors the step-bar path above.
       * renderDbeamBarIfVisible() returns false on the SEQ dashboard / menu /
       * app-connected views, keeping those views correct (D-BEAM bar only exists
       * on the HARP dashboard).  On those views the bargraph is part of the next
       * full render triggered by the encoder/transport or the scope. */
      if (renderDbeamBarIfVisible())
        displayFlushDiff();
      xSemaphoreGive(i2cMutex);
    }

    vTaskDelayUntil(&lastWake, kPer);
  }
}

/* ─────────────────────────────────────────────────────────────────────────────
 * settings_save_task — Core 1, priority 3  [NvsWorker]
 *
 * NVS save handshake:
 *   1. g_saveRequest fires → ramp master volume to 0 (click-free)
 *   2. Wait 40 ms for gain smoother + DMA to drain
 *   3. Set g_saveArmed → laser_sweep_task parks Core 1
 *   4. Wait for g_loopParked (≤500 ms); if timeout, proceed anyway with a
 *      warning — the NVS write is safe even if the laser is mid-sweep because
 *      IDF's flash driver suspends the other core correctly; worst case is one
 *      missed beam detection during the ~30–200 ms write window.
 *   5. Write NVS (all four blobs); restore volume; clear flags
 *
 * App CMD_SESSION_SAVE also routes here via g_saveRequest.
 * ─────────────────────────────────────────────────────────────────────────────*/
void settings_save_task(void* pvParameters) {
  (void)pvParameters;
  esp_task_wdt_add(NULL);
  while (!g_systemReady.load(std::memory_order_acquire))
    vTaskDelay(pdMS_TO_TICKS(1));

  for (;;) {
    esp_task_wdt_reset();
    if (!g_saveRequest.load(std::memory_order_acquire)) {
      vTaskDelay(pdMS_TO_TICKS(50));
      continue;
    }

    /* ── Step 1: ramp volume to silence ───────────────────────────────── */
    const float savedVol = masterVol.load(std::memory_order_relaxed);
    masterVol.store(0.0f, std::memory_order_relaxed);
    vTaskDelay(pdMS_TO_TICKS(40));

    /* [SAVE-FIX7] Own the I2C bus for the WHOLE write window. */
    const bool haveI2c =
        (i2cMutex && xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(250)) == pdTRUE);

    /* ── Step 2: arm save window, wait for laser to park ─────────────── */
    g_saveArmed.store(true, std::memory_order_release);
    std::atomic_thread_fence(std::memory_order_seq_cst);

    /* [FIX-WRAP] Use elapsed-time comparison to avoid millis()+500u overflow. */
    const uint32_t parkStartMs = millis();
    while (!g_loopParked.load(std::memory_order_acquire) &&
           (uint32_t)(millis() - parkStartMs) < 500u) {
      esp_task_wdt_reset();
      vTaskDelay(pdMS_TO_TICKS(1));
    }

    const bool parked = g_loopParked.load(std::memory_order_acquire);
    if (!parked)
      Serial.println(F("[NVS] WARN: laser park timeout — saving anyway"));

    /* [SAVE-FIX6] Restore the REAL master volume BEFORE the blob sync so
     * settings_sync_from_ssot() does not persist volume=0. */
    masterVol.store(savedVol, std::memory_order_relaxed);

    /* ── Step 3: write NVS ────────────────────────────────────────────── */
    esp_task_wdt_reset();
    const ResetScope scope =
        (ResetScope)(g_persistScope.load(std::memory_order_relaxed) & 3u);
    const bool ok = settings_save_scoped(scope);
    esp_task_wdt_reset();
    g_persistScope.store((uint8_t)ResetScope::FULL, std::memory_order_relaxed);
    g_saveLastOk.store(ok, std::memory_order_release);
    if (!ok) {
      Serial.println(F("[NVS] Save FAILED — check NVS partition size"));
      g_saveFailFlashMs.store(millis() + 1500u, std::memory_order_relaxed);
      displayDirty.store(true, std::memory_order_relaxed);
    } else {
      g_saveFailFlashMs.store(0u, std::memory_order_relaxed);
      g_saveFlashMs.store(millis() + 1200u, std::memory_order_relaxed);
      displayDirty.store(true, std::memory_order_relaxed);
    }

    /* ── Step 4: clear flags and restore volume ───────────────────────── */
    std::atomic_thread_fence(std::memory_order_release);
    g_loopParked.store(false, std::memory_order_release);
    g_saveArmed .store(false, std::memory_order_release);
    /* [SAVE-FIX13] Request beam re-home AFTER clearing g_saveArmed so the
     * laser's park loop exits first and picks this up on its next iteration. */
    g_beamRecover.store(true, std::memory_order_release);
    std::atomic_thread_fence(std::memory_order_release);

    masterVol.store(savedVol, std::memory_order_relaxed);
    g_saveRequest.store(false, std::memory_order_release);
    Serial.println(F("[BEAM] save complete → beam-recover requested"));

    /* [SAVE-FIX7] Release the I2C bus — OLED tasks may render again. */
    if (haveI2c) xSemaphoreGive(i2cMutex);

    if (g_saveDoneSem) xSemaphoreGive(g_saveDoneSem);

    /* Send ACK to App on success only. On failure the reset_persist_task sends
     * the NACK; for a plain save the App's 90 s timeout unblocks the modal. */
    if (ok && isAppConnected()) txSysex(CMD_SESSION_SAVE, 16383u);

    /* [SAVE-FIX14] Restart after a good commit so laser beams re-init cleanly. */
    if (g_restartAfterSave.exchange(false, std::memory_order_acq_rel)) {
      if (ok) {
        delay(700);
        esp_restart();
      }
    }
  }
}

/* ─────────────────────────────────────────────────────────────────────────────
 * init_audio_system — called once from setup() after hardware and GPIO init
 * ─────────────────────────────────────────────────────────────────────────────*/
bool init_audio_system() {
  /* ── Mirror wavetables to DRAM [PERF] ──────────────────────────────────────
   * MUST run before the audio task starts: the harp/seq/drum oscillators read
   * from WAVE_TABLE_RAM (internal SRAM) instead of PROGMEM flash, removing
   * flash-cache-miss stalls from the per-sample hot path. */
  wavetables_init_ram();

  /* ── Heap buffers (DRAM, DMA-capable) ──────────────────────────────────── */
  const uint32_t flags = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
  audio_raw_harpH = (int16_t*)heap_caps_malloc(DMA_BUFFER_MONO_BYTES,   flags);
  audio_raw_seqH  = (int16_t*)heap_caps_malloc(DMA_BUFFER_MONO_BYTES,   flags);
  audio_raw_drumH = (int16_t*)heap_caps_malloc(DMA_BUFFER_MONO_BYTES,   flags);
  audio_raw_outH  = (int16_t*)heap_caps_malloc(DMA_BUFFER_STEREO_BYTES, flags);

  if (!audio_raw_harpH || !audio_raw_seqH || !audio_raw_drumH || !audio_raw_outH) {
    Serial.printf("[Audio] DRAM alloc failed (need %u bytes)\n",
      (unsigned)(3u * DMA_BUFFER_MONO_BYTES + DMA_BUFFER_STEREO_BYTES));
    /* Free any successful allocations so the heap isn't silently leaked if
     * the system continues (e.g. diagnostic mode with audio disabled).       */
    heap_caps_free(audio_raw_harpH); audio_raw_harpH = nullptr;
    heap_caps_free(audio_raw_seqH);  audio_raw_seqH  = nullptr;
    heap_caps_free(audio_raw_drumH); audio_raw_drumH = nullptr;
    heap_caps_free(audio_raw_outH);  audio_raw_outH  = nullptr;
    return false;
  }
  memset(audio_raw_harpH, 0, DMA_BUFFER_MONO_BYTES);
  memset(audio_raw_seqH,  0, DMA_BUFFER_MONO_BYTES);
  memset(audio_raw_drumH, 0, DMA_BUFFER_MONO_BYTES);
  memset(audio_raw_outH,  0, DMA_BUFFER_STEREO_BYTES);

  /* ── FX chain init ─────────────────────────────────────────────────────── */
  if (!fx_init()) { Serial.println(F("[Audio] fx_init failed")); return false; }

  /* ── I2S hardware ──────────────────────────────────────────────────────── */
  if (init_i2s_hardware() != ESP_OK) {
    Serial.println(F("[Audio] I2S init failed")); return false;
  }

  /* ── Song data ───────────────────────────────────────────────────────────
   * Do NOT re-initialise hwSongData here.  By this point loadSettings() (PHASE
   * 4) has already populated it from NVS (or from the SongSlot defaults on a
   * fresh blob).  Re-defaulting it here would wipe the user's saved songs on
   * every boot.  Sane defaults live in the SongSlot struct (globals.h).        */

  /* ══ CORE 0 — REALTIME AUDIO ISLAND ═══════════════════════════════════════
   * Core 0 is dedicated to the hard-realtime signal chain.  AudioSynth is the
   * ONLY heavy consumer here, and because it is the highest-priority task on the
   * core it always preempts the two cooperative tasks below it — so it already
   * gets ~100 % of Core 0 whenever a buffer needs filling.  The real win of this
   * layout is NEGATIVE work removal: the bursty data-traffic tasks (MIDI USB RX,
   * SeqSysexOut, NVS flash) live on Core 1 so their ISRs + cache thrash no longer
   * stall the audio core mid-buffer.  That isolation is the lowest-glitch setup yet.
   * Live priorities come from the globals.h TASK_PRIO_* constants ([TASK-PRIO-SSOT];
   * raised vs the original layout to cure UI/echo starvation):
   *   24 AudioSynth  — never preempted while an I2S DMA write is pending
   *   19 dbeam_adc   — D-BEAM ADC Kalman.  Its GDMA ISR is installed on Core 1
   *                    (initDBeamSensor runs from setup() on Core 1), so only the
   *                    light processing task lives here.  MUST be on Core 0, not
   *                    Core 1: the ADC commits only while the beam is lit, and the
   *                    Core-1 laser busy-polls through the whole lit dwell — a
   *                    Core-1 ADC task would never run in that window.  At 19 it
   *                    drains the ring in audio's I2S-block slack; below 24 it can
   *                    never delay a buffer fill.
   *   18 OledRender  — moved here from Core 1 so the laser owns Core 1 alone
   *                    (smoother beams).  Its ~9 ms blocking I2C flush is SAFE:
   *                    AudioSynth (24) + dbeam_adc (19) preempt it instantly, so
   *                    the flush is chopped across slack → "sluggish but reliable"
   *                    display, never an underrun.
   *   16 ControlPoll — encoder/buttons; runs in leftover slack.
   * NOTE: OledRender(18) sits ABOVE ControlPoll(16), so during a redraw the
   * screen out-prioritises input — that is why the UI feels heaviest while it
   * repaints.  If input latency ever matters more than redraw speed, drop
   * OledRender below ControlPoll.
   *
   * [C4] Stack sizes are right-sized, not a uniform 8192.  AudioSynth (deep
   * FX/DSP) and OledRender (framebuffer) keep generous stacks.  VERIFY before
   * shipping: the boot log prints each task's stack high-water mark ~5 s after
   * boot (see control_surface_task) — if any task shows < 512 bytes free, bump
   * it back up. */
  /* Priorities come from globals.h TASK_PRIO_* (single source of truth — see the
   * [TASK-PRIO-SSOT] block).  The old inline priority + `// old:N` notes are kept
   * in code_info.h / the comment blocks above; do not re-introduce literals here. */
  xTaskCreatePinnedToCore(audio_synthesis_task, "AudioSynth", 16384, NULL, TASK_PRIO_RT,      &hAudioTask,   0);
  xTaskCreatePinnedToCore(display_refresh_task, "OledRender", 16384, NULL, TASK_PRIO_OLED,    &hDisplayTask, 0);
  xTaskCreatePinnedToCore(control_surface_task, "ControlPoll", 8192, NULL, TASK_PRIO_CONTROL, &hControlTask, 0);
  /* [DBEAM-FIX] Priority 8 → 19.  At 8 this task sat BELOW OledRender(14) and
   * ControlPoll(10), so under full play (audio 65–98 %) it was the lowest-priority
   * task on a saturated core → starved → ADC ring backed up → "D-BEAM dead /
   * blocking ADC".  It MUST stay on Core 0 (opposite the laser): the ADC only
   * commits a sample while the beam is lit (dbeam.cpp), and the laser owns Core 1
   * by busy-polling esp_timer through every lit dwell — so a Core-1 ADC task can
   * never run during the lit window and never commits (the "on core 1 not work"
   * symptom).  On Core 0 the lit window stays open independently; priority 19 lets
   * the ADC drain the ring in audio's I2S-block slack, above the cooperative UI
   * tasks but still below AudioSynth(24) so it can never cause an underrun. */
  xTaskCreatePinnedToCore(adc_dma_processing_task, "dbeam_adc",  6144, NULL, TASK_PRIO_DBEAM, &hDBeamTask,  0);



  /* ══ CORE 1 — LASER + ALL BURSTY DATA TRAFFIC ════════════════════════════
   * LaserSweep owns the core; everything else here is event-driven and latency-
   * tolerant and runs inside LaserSweep's dark-phase breathing windows
   * (LASER_BREATHE_MS, gated to the MOVING phase so a lit beam is never cut).
   * Concentrating the data-traffic tasks here keeps their interrupts and the
   * flash-cache stalls OFF Core 0, which is what protects the audio buffer.
   *   24 LaserSweep — galvo dwell timing; highest so it is never preempted.  It
   *                   busy-polls esp_timer (never blocks in the hot path) and so
   *                   owns the core during every lit dwell — which is exactly why
   *                   dbeam_adc CANNOT live here (it would never run while lit and
   *                   so could never commit a sample).  dbeam_adc is on Core 0.
   *   14 SeqSysexOut— drains the lock-free out-ring → App STEP_SYNC / SONG_POS
   *                   SysEx echoes (takes midiMutex, must stay off the audio core).
   *   12 MidiUsbRx  — USB MIDI in; ~1 ms parse latency is inaudible.  Carries the
   *                   OctopusApp 0x7D SysEx control + optional instrument play-in.
   *    9 NvsWorker  — blocked 99 % of the time; wakes only on a save request. */
   


  xTaskCreatePinnedToCore(laser_sweep_task,          "LaserSweep",  8192, NULL, TASK_PRIO_RT,        &hLaserTask, 1);
  xTaskCreatePinnedToCore(sequencer_background_task, "SeqSysexOut", 4096, NULL, TASK_PRIO_SEQ_SYSEX, &hSeqBgTask, 1);
  xTaskCreatePinnedToCore(midi_usb_event_task,       "MidiUsbRx",   8192, NULL, TASK_PRIO_MIDI_RX,   &hMidiTask,  1);
  
  /* [USB-ONLY] MIDI RX always runs: it polls USB MIDI for OctopusApp 0x7D SysEx
   * control + optional instrument play-in (DIN UART removed in v6.0).            */
  
  /* [TIMING-LOCK] Former "SeqClock": no longer keeps musical time (that moved
   * into AudioSynth, sample-locked).  Now only drains the lock-free out-ring and
   * emits jitter-tolerant STEP_SYNC / SONG_POS SysEx echoes to the App (which take
   * midiMutex and must stay off the audio core).  Lives on Core 1 at priority 14 —
   * above MIDI-RX/dbeam so echoes stay tight, below LaserSweep so beams win.      */
  
  /* [SAVE-FIX2] 16384 (was 8192): four sequential nvs_set_blob calls (settings
   * ~2 KB + patterns ~8 KB + banks ~4 KB + motion ~4 KB) drive 3-4 KB deep into
   * IDF NVS/flash-driver internals each. Back-to-back peak is ~5-6 KB; the 8 KB
   * stack left only 2-3 KB headroom and any IDF minor version change overflowed it
   * → heap corruption → crash on the next alloc. 16 KB is safe for all four blobs
   * in sequence with comfortable margin for future IDF stack growth.
   * Task is NOT IRAM_ATTR — NVS/flash paths execute from flash; marking the task
   * IRAM_ATTR made cache-off windows riskier on some IDF builds. */
  xTaskCreatePinnedToCore(settings_save_task, "NvsWorker", 16384, NULL, TASK_PRIO_NVS, &hNvsTask, 1);
  Serial.printf("[Audio] DSP online — %u Hz / %u frames / %u Hz PWM\n",
    SAMPLE_RATE, (unsigned)DMA_BUFFER_FRAMES, LASER_PWM_FREQ_HZ);
  return true;
}
