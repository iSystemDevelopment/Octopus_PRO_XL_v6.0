/* ═════════════════════════════════════════════════════════════════════════════
 * Octopus PRO XL v6.1.00 — Laser Harp Groovebox
 * © 2026 DIODAC ELECTRONICS / iSystem. All Rights Reserved.
 *
 * PROPRIETARY AND CONFIDENTIAL. Unauthorized copying, distribution, modification,
 * or use of this software or firmware, in whole or in part, is strictly prohibited
 * without prior written permission from DIODAC ELECTRONICS.
 * ═════════════════════════════════════════════════════════════════════════════
 * audio.cpp — v6.1.00  I2S DSP LOOP + FREERTOS TASK SPAWN
 *
 * Owns the real-time audio path and all Core-0 service tasks:
 *   • init_audio_system()  — heap alloc, fx_init, I2S enable, xTaskCreatePinnedToCore
 *                            (AUTHORITATIVE task table — see code_info.h §1)
 *   • audio_synthesis_task — I2S write loop: sequencer_render_block → harp/seq/drum
 *                            fill → fx_process_multi_buf_safe; adaptive load shedding
 *   • control_surface_task — 200 Hz (5 ms): updateHardwareInterface + stack stats
 *   • display_refresh_task — ~30 Hz (33 ms): renderUIState under i2cMutex
 *   • settings_save_task   — NvsWorker on Core 1: muted save handshake + reboot
 *
 * Laser sweep (laser.cpp) and USB MIDI RX (midi.cpp) are spawned here but run
 * on Core 1; sequencer_background_task (groovebox.cpp) drains the out-ring there.
 * ═════════════════════════════════════════════════════════════════════════════ */
#include "audio.h"
#include "esp_log.h"


#include "laser.h"
#include "midi.h"  

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


void IRAM_ATTR audio_synthesis_task(void* pvParameters) {
  static_assert(DMA_BUFFER_FRAMES <= FX_BUF_SIZE,
    "DMA_BUFFER_FRAMES > FX_BUF_SIZE: increase FX_BUF_SIZE in effect.h");

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
  int      s_shedTier = -1;  

  static constexpr uint32_t AUDIO_IDLE_GUARD_MS = 250u;
  uint32_t lastYieldMs = millis();
  esp_task_wdt_add(NULL); /* monitored + named in any WDT report */

  audioTaskReady.store(true, std::memory_order_release);

  for (;;) {
    esp_task_wdt_reset();

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
    syncDrumSends();

    /* ── 4. DSP frame timing start ──────────────────────────────────────── */
    const int64_t t0 = esp_timer_get_time();

    sequencer_render_block(DMA_BUFFER_FRAMES);

    const bool hOn = harp_synth_fill_buf(audio_raw_harpH, DMA_BUFFER_FRAMES);
    const bool sOn = gb_seq_fill_buf(audio_raw_seqH, DMA_BUFFER_FRAMES);
    const bool dOn = drum_fill_buf      (audio_raw_drumH, DMA_BUFFER_FRAMES);

    fx_process_multi_buf_safe(audio_raw_harpH, audio_raw_seqH, audio_raw_drumH,
                               audio_raw_outH,  DMA_BUFFER_FRAMES, hOn, sOn, dOn);

    static constexpr float kInvPeriodUs = 1.0f / kPeriodUs;
    const float computeUs = (float)(esp_timer_get_time() - t0);
    const float load      = computeUs * kInvPeriodUs;
    const float aCoef     = (load > loadEma) ? 0.5f : 0.02f; /* attack : release */
    loadEma += aCoef * (load - loadEma);
    const int pct = (int)(loadEma * 100.0f);
    g_audio_load_pct.store((uint8_t)std::min(100, std::max(0, pct)));

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
      if (++loHoldCnt > 300u && s_shedTier != 0) { 
        s_shedTier = 0;
        g_seq_voice_cap.store(SEQ_POLYPHONY); g_aux_mode.store(2); g_osc2_enable.store(true);
        if (loadEma < 0.35f) g_svf_oversample.store(2);
      }
    }

    bool yielded = false;
    if (computeUs >= kPeriodUs) {
      vTaskDelay(pdMS_TO_TICKS(1));
      yielded = true;
    }

    /* ── 10. I2S DMA write ───────────────────────────────────────────────── */
    const int64_t wrStart = esp_timer_get_time();
    const esp_err_t r = i2s_channel_write(tx_handle, audio_raw_outH,
                                           DMA_BUFFER_STEREO_BYTES,
                                           &bytes_written, pdMS_TO_TICKS(100));

    if ((esp_timer_get_time() - wrStart) >= 1000) yielded = true;
    if (r != ESP_OK && tx_handle) {
      i2s_channel_disable(tx_handle);
      vTaskDelay(pdMS_TO_TICKS(10));
      i2s_channel_enable(tx_handle);
      yielded = true;
    }

    const uint32_t nowMs = millis();
    if (yielded) {
      lastYieldMs = nowMs;
    } else if ((uint32_t)(nowMs - lastYieldMs) >= AUDIO_IDLE_GUARD_MS) {
      lastYieldMs = nowMs;
      vTaskDelay(pdMS_TO_TICKS(1));
    }
  }
}


void IRAM_ATTR control_surface_task(void* pvParameters) {
  while (!g_systemReady.load(std::memory_order_acquire))
    vTaskDelay(pdMS_TO_TICKS(1));

  TickType_t   lastWake = xTaskGetTickCount();
  const TickType_t kPer = pdMS_TO_TICKS(5);   
  uint32_t lastStackMs   = 0u;
  uint32_t lastSerialMs  = 0u;
  for (;;) {
    updateHardwareInterface();

    const uint32_t nowMs = millis();
    if (nowMs - lastStackMs >= 5000u) {
      lastStackMs = nowMs;
      updateTaskStackStats();
      if (lastSerialMs == 0u || nowMs - lastSerialMs >= 30000u) {
        lastSerialMs = nowMs;
        printInterfaceStats();
      }
    }

    vTaskDelayUntil(&lastWake, kPer);
  }
}


void IRAM_ATTR display_refresh_task(void* pvParameters) {
  while (!g_systemReady.load(std::memory_order_acquire))
    vTaskDelay(pdMS_TO_TICKS(1));

  TickType_t   lastWake = xTaskGetTickCount();
  const TickType_t kPer = pdMS_TO_TICKS(33);   
  uint16_t lastDbeam    = 0u;
  uint32_t lastDbeamDrawMs = 0u;
  bool     prevConnected = false;
  uint8_t  lastStepDrawn = 0xFFu;
  bool     prevSaveReq   = false;   

  for (;;) {
    const bool appConn = isAppConnected();

    if (appConn != prevConnected) {
      prevConnected = appConn;
      displayDirty.store(true, std::memory_order_relaxed);
    }

    bool stepChanged = false;
    if (seqPlaying.load(std::memory_order_relaxed)) {
      const uint8_t curStep = seqCurrentStep.load(std::memory_order_relaxed);
      if (curStep != lastStepDrawn) {
        lastStepDrawn = curStep;
        stepChanged   = true;
      }
    } else {
      lastStepDrawn = 0xFFu;   
    }


    const uint32_t nowMs = millis();
    const uint16_t amp = (uint16_t)dbeamAmplitude.load(std::memory_order_relaxed);

    const uint16_t dbeamDelta = (amp > lastDbeam) ? (uint16_t)(amp - lastDbeam)
                                                  : (uint16_t)(lastDbeam - amp);
    if (dbeamDelta > 256 &&
        (uint32_t)(nowMs - lastDbeamDrawMs) >= 100u) {
      lastDbeam       = amp;
      lastDbeamDrawMs = nowMs;
      displayDirty.store(true, std::memory_order_relaxed);
    }

    const bool scope = (currentScopeView.load(std::memory_order_relaxed) != TelemetryView::OFF);

    static bool toastPrev = false;
    const bool  toastNow  = (int32_t)(g_saveFlashMs.load(std::memory_order_relaxed) - nowMs) > 0;
    const bool  failNow   = (int32_t)(g_saveFailFlashMs.load(std::memory_order_relaxed) - nowMs) > 0;
    if (toastNow || failNow || toastPrev) displayDirty.store(true, std::memory_order_relaxed);
    toastPrev = toastNow;

    const bool saveReqNow = g_saveRequest.load(std::memory_order_acquire);
    if (saveReqNow && !prevSaveReq) displayDirty.store(true, std::memory_order_relaxed);
    prevSaveReq = saveReqNow;

    const bool matrixStep = stepChanged && viewIsSeqMatrix();
    
    const bool edgeEdit = edgeEditOpen.load(std::memory_order_relaxed);
    
    const bool saveArmedNow = g_saveArmed.load(std::memory_order_acquire);
    bool draw = false;
    if (!saveArmedNow)
      draw = displayDirty.exchange(false, std::memory_order_relaxed) || scope || matrixStep || edgeEdit;

    if (draw && xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
      display.clearDisplay();
      if (appConn)
        drawAppConnectedPage();   
      else
        renderUIState();          
      drawSaveToastIfActive();   
      
      displayFlushDiff();
      xSemaphoreGive(i2cMutex);
    } else if (!saveArmedNow && stepChanged && xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
      
      if (renderStepBarRegionIfVisible())
        displayFlushDiff();
      xSemaphoreGive(i2cMutex);
    }

    vTaskDelayUntil(&lastWake, kPer);
  }
}

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

    const float savedVol = masterVol.load(std::memory_order_relaxed);
    masterVol.store(0.0f, std::memory_order_relaxed);
    vTaskDelay(pdMS_TO_TICKS(40));

    const bool haveI2c =
        (i2cMutex && xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(250)) == pdTRUE);

    g_saveArmed.store(true, std::memory_order_release);
    std::atomic_thread_fence(std::memory_order_seq_cst);  

    const uint32_t deadline = millis() + 500u;
    while (!g_loopParked.load(std::memory_order_acquire) && millis() < deadline) {
      esp_task_wdt_reset();
      vTaskDelay(pdMS_TO_TICKS(1));
    }

    const bool parked = g_loopParked.load(std::memory_order_acquire);
   
    masterVol.store(savedVol, std::memory_order_relaxed);

    esp_task_wdt_reset();
    const ResetScope scope =
        (ResetScope)(g_persistScope.load(std::memory_order_relaxed) & 3u);
    const bool ok = settings_save_scoped(scope);
    esp_task_wdt_reset();
    g_persistScope.store((uint8_t)ResetScope::FULL, std::memory_order_relaxed);
    g_saveLastOk.store(ok, std::memory_order_release);
    if (!ok) {
      g_saveFailFlashMs.store(millis() + 1500u, std::memory_order_relaxed);
      displayDirty.store(true, std::memory_order_relaxed);
    } else {
      g_saveFailFlashMs.store(0u, std::memory_order_relaxed);
      g_saveFlashMs.store(millis() + 1200u, std::memory_order_relaxed);
      displayDirty.store(true, std::memory_order_relaxed);
    }

    std::atomic_thread_fence(std::memory_order_release);
    g_loopParked.store(false, std::memory_order_release);
    g_saveArmed .store(false, std::memory_order_release);
    
    g_beamRecover.store(true, std::memory_order_release);
    std::atomic_thread_fence(std::memory_order_release);

    masterVol.store(savedVol, std::memory_order_relaxed);
    g_saveRequest.store(false, std::memory_order_release);
  
    if (haveI2c) xSemaphoreGive(i2cMutex);
    if (g_saveDoneSem) xSemaphoreGive(g_saveDoneSem);

    if (ok && isAppConnected()) {
      const uint8_t ack = g_persistAckCmd.load(std::memory_order_relaxed);
      txSysex(ack ? ack : CMD_SESSION_SAVE, 16383u);
    }

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
  wavetables_init_ram();

  const uint32_t flags = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
  audio_raw_harpH = (int16_t*)heap_caps_malloc(DMA_BUFFER_MONO_BYTES,   flags);
  audio_raw_seqH  = (int16_t*)heap_caps_malloc(DMA_BUFFER_MONO_BYTES,   flags);
  audio_raw_drumH = (int16_t*)heap_caps_malloc(DMA_BUFFER_MONO_BYTES,   flags);
  audio_raw_outH  = (int16_t*)heap_caps_malloc(DMA_BUFFER_STEREO_BYTES, flags);

  if (!audio_raw_harpH || !audio_raw_seqH || !audio_raw_drumH || !audio_raw_outH) {
    return false;
  }
  memset(audio_raw_harpH, 0, DMA_BUFFER_MONO_BYTES);
  memset(audio_raw_seqH,  0, DMA_BUFFER_MONO_BYTES);
  memset(audio_raw_drumH, 0, DMA_BUFFER_MONO_BYTES);
  memset(audio_raw_outH,  0, DMA_BUFFER_STEREO_BYTES);

  /* ── FX chain init ─────────────────────────────────────────────────────── */
  if (!fx_init()) { return false; }

  /* ── I2S hardware ──────────────────────────────────────────────────────── */
  if (init_i2s_hardware() != ESP_OK) { return false;}

  /* Task table — authoritative; code_info.h §1 mirrors this exactly. */
  xTaskCreatePinnedToCore(audio_synthesis_task,    "AudioSynth",  16384,  NULL,    24, &hAudioTask,   0);
  xTaskCreatePinnedToCore(adc_dma_processing_task, "dbeam_adc",   6144,   NULL,    19, &hDBeamTask,   0);
  xTaskCreatePinnedToCore(display_refresh_task,    "OledRender",  16384,  NULL,    18, &hDisplayTask, 0);
  xTaskCreatePinnedToCore(control_surface_task,    "ControlPoll", 8192,   NULL,    17, &hControlTask, 0); 

  xTaskCreatePinnedToCore(laser_sweep_task,          "LaserSweep",  8192,  NULL, 24, &hLaserTask,  1);
  xTaskCreatePinnedToCore(sequencer_background_task, "SeqSysexOut", 4096,  NULL, 12, &hSeqBgTask,  1);
  xTaskCreatePinnedToCore(midi_usb_event_task,       "MidiUsbRx",   8192,  NULL, 6, &hMidiTask,    1);
  xTaskCreatePinnedToCore(settings_save_task,        "NvsWorker",   16384, NULL, 3, &hNvsTask,     1);
  Serial.printf("[Audio] DSP online — %u Hz / %u frames / %u Hz PWM\n",
    SAMPLE_RATE, (unsigned)DMA_BUFFER_FRAMES, LASER_PWM_FREQ_HZ);
  return true;
}
