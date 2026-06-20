/* ═════════════════════════════════════════════════════════════════════════════
 * Octopus PRO XL v6.0.00 — Laser Harp Groovebox
 * © 2026 DIODAC ELECTRONICS / iSystem. All Rights Reserved.
 *
 * PROPRIETARY AND CONFIDENTIAL. Unauthorized copying, distribution, modification,
 * or use of this software or firmware, in whole or in part, is strictly prohibited
 * without prior written permission from DIODAC ELECTRONICS.
 * ═════════════════════════════════════════════════════════════════════════════
 * Octopus_PRO_XL_v6.0.ino — v6.0.00  DUAL-CORE SCHEDULER & BOOT KERNEL
 *
 * Changes vs v5.3.x:
 *  [A] wires.h removed — WebApp authority logic absorbed into patches.h.
 *  [B] laser_sweep_task body removed from .ino — it now lives in laser.cpp.
 *      .ino simply passes the function pointer to xTaskCreatePinnedToCore
 *      inside init_audio_system() (audio.cpp).  All task creation is in one
 *      place: init_audio_system().
 *  [C] setupMCPWM() removed from PHASE 6 — initLaser() (laser.cpp) calls it
 *      internally.  Only initLaser() needs to be called from setup().
 *  [D] updateHarpPatch() / updateSeqPatch() / updateSampleBuffersSync()
 *      removed from PHASE 8 — these functions no longer exist in v5.3.
 *      livePatch arrays are maintained by patches.h apply* functions.
 *  [E] harpMode set to CLOSED at boot (not OPENING); laser animation now
 *      triggered explicitly by the user via the OC long-press gesture.
 *  [F] initMotionMatrix() defined here inline — clears hwMotionData garbage
 *      from BSS segment before the sequencer task starts.
 *
 * Core assignment (v6.0 production):
 *   Core 0: AudioSynth(24, incl. DMA-locked step engine) ControlPoll(5)
 *            OledRender(4)
 *   Core 1: LaserSweep(24) SeqSysexOut(14) MidiUsbRx(10) dbeam_adc(6)
 *            NvsWorker(2) loop(1)
 * ═════════════════════════════════════════════════════════════════════════════ */
#include <Arduino.h>
#include <soc/gpio_struct.h>
#include <pgmspace.h>
#include <atomic>
#include <math.h>
#include "esp_attr.h"
#include "esp_system.h"   /* esp_reset_reason() — boot crash diagnostics */
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "esp_adc/adc_continuous.h"
#include "driver/mcpwm_prelude.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "SPI.h"
#include <Wire.h>
#include "USB.h"
#include "USBCDC.h"
#include "USBMIDI.h"
#include <ESP32Encoder.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>
#include <Preferences.h>

#include "code_info.h"
#include "globals.h"
#include "assets.h"
#include "patches.h"
#include "effect.h"
#include "settings.h"
#include "groovebox.h"  /* [GB-MERGE] seq + drum + patterns + transport (was sequencer.h + patterns.h) */
#include "midi.h"
#include "dbeam.h"
#include "laser.h"
#include "audio.h"
#include "display.h"
#include "interface.h"
/* wires.h removed [A] — WebApp authority logic in patches.h */

USBMIDI MIDI;

/* [F] initMotionMatrix() removed [MOTION-PERSIST] — the motion matrix is now
 * initialised by loadSettings() → persisted_extras_load() (settings.h), which
 * sets the empty sentinels (targetCmd=255, steps=0xFFFF) and overlays any saved
 * P-locks from NVS.  A separate boot-time init pass here would wipe them.       */

/* ═════════════════════════════════════════════════════════════════════════════
 * setup()
 * ═════════════════════════════════════════════════════════════════════════════ */
void setup() {
  delay(500);
  Serial.begin(115200);
  delay(100);

  Serial.println(F("\n╔════════════════════════════════════════╗"));
  Serial.printf("║  OCTOPUS PRO XL v%s — BOOT KERNEL  ║\n", SYSTEM_FW_VERSION);
  Serial.println(F("╚════════════════════════════════════════╝\n"));

  /* ── REBOOT DIAGNOSTIC ───────────────────────────────────────────────────────
   * Decode WHY the chip last reset.  This is the single most useful clue for the
   * "keeps rebooting" symptom — it distinguishes a software panic (task/interrupt
   * watchdog, illegal access) from a POWER fault (brownout) from a normal boot.
   * Capture this line from the serial monitor after a reboot and act on it:
   *   • PANIC / TASK_WDT / INT_WDT → software (scheduling / hard fault) — see code
   *   • BROWNOUT                   → power supply: laser+galvo+audio current spike
   *                                  sags the rail; add bulk capacitance / a stiffer
   *                                  3V3 + 5V supply.  No software change fixes this.
   *   • POWERON / EXT (SW)         → clean boot / manual reset — not a crash. */
  {
    const esp_reset_reason_t rr = esp_reset_reason();
    const char* rs = "?";
    switch (rr) {
      case ESP_RST_POWERON:   rs = "POWERON (clean)";        break;
      case ESP_RST_EXT:       rs = "EXT pin";                break;
      case ESP_RST_SW:        rs = "SW restart";             break;
      case ESP_RST_PANIC:     rs = "PANIC (Guru Meditation — see Backtrace / ipc0 stack)"; break;
      case ESP_RST_INT_WDT:   rs = "INTERRUPT WDT (IRQs disabled too long)"; break;
      case ESP_RST_TASK_WDT:  rs = "TASK WDT (a task/idle starved >5 s)"; break;
      case ESP_RST_WDT:       rs = "OTHER WDT";              break;
      case ESP_RST_BROWNOUT:  rs = "BROWNOUT (power rail sag — HARDWARE)"; break;
      case ESP_RST_DEEPSLEEP: rs = "deep-sleep wake";        break;
      case ESP_RST_SDIO:      rs = "SDIO";                   break;
      default:                rs = "unknown";                break;
    }
    Serial.printf("  [RESET REASON] %s\n", rs);
    if (rr == ESP_RST_BROWNOUT)
      Serial.println(F("  >>> BROWNOUT: this is a POWER problem, not code. "
                       "Check the 3V3/5V supply + add bulk caps near laser/galvo."));
  }

  /* PHASE 1 — Mutexes (must be first) */
  Serial.print(F("  [1] Mutexes ... "));
  midiMutex = xSemaphoreCreateMutex();
  fxMutex = xSemaphoreCreateMutex();
  i2cMutex = xSemaphoreCreateMutex();
  g_saveDoneSem = xSemaphoreCreateBinary();
  if (!midiMutex || !fxMutex || !i2cMutex || !g_saveDoneSem) {
    Serial.println(F("FATAL — heap exhausted"));
    while (1) delay(100);
  }
  Serial.println(F("✓"));

  /* PHASE 2 — Communication buses */
  Serial.print(F("  [2] I2C 1 MHz ... "));
  Wire.begin();
  Wire.setTimeout(50);
  Wire.setClock(1000000);  /* SH1106 verified stable at 1 MHz; fastest frame flush */
  Serial.println(F("✓"));

  Serial.print(F("  [2] USB MIDI ... "));
  USB.PID(0x8209);
  USB.VID(0x303a);
  USB.productName("Octopus PRO S3");
  USB.manufacturerName("DIODAC ELECTRONICS");
  USB.usbClass(02);
  USB.usbSubClass(02);
  USB.begin();
  MIDI.begin();
  Serial.println(F("✓"));

  /* PHASE 3 — OLED display */
  Serial.print(F("  [3] OLED SH1106 ... "));
  Wire.beginTransmission(I2C_ADDR_OLED);
  if (Wire.endTransmission() == 0) {
    hasOLED = true;
    display.begin(I2C_ADDR_OLED, true);
    display.setRotation(0);
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SH110X_WHITE);
    display.setCursor(6, 20);
    display.printf("OCTOPUS PRO XL v%s", SYSTEM_FW_VERSION);
    display.setCursor(6, 36);
    display.println(F("   INITIALIZING..."));
    display.display();
    Serial.println(F("✓"));
  } else {
    hasOLED = false;
    Serial.println(F("not detected — headless mode"));
  }

  /* PHASE 4 — Load settings from NVS */
  Serial.print(F("  [4] NVS settings ... "));
  if (!loadSettings()) {
    Serial.println(F("✓ (factory defaults applied)"));
  } else {
    Serial.println(F("✓"));
  }
  /* NOTE: do NOT call initDrumParameters() here.  loadSettings() →
   * settings_sync_to_ssot() has already written every drum atomic from the
   * loaded (or factory) blob; calling initDrumParameters() now would overwrite
   * those persisted values with placeholders, wiping saved drum tuning on every
   * boot.  The reset paths inside settings_load() still init-then-sync.        */

  /* PHASE 5 — Hardware I/O */
  Serial.print(F("  [5] SPI bus ... "));
  SPI.begin(12, GPIO_NUM_48, 11, -1); /* CLK=12, MISO=48, MOSI=11, no CS */
  Serial.println(F("✓"));

  Serial.print(F("  [5] Hardware interface (encoder, buttons) ... "));
  initHardwareInterface(); /* interface.cpp: ButtonPoll init + factory-reset detect */
  Serial.println(F("✓"));

  Serial.print(F("  [5] MIDI (USB) ... "));
  init_midi_hardware(); /* USB-only: firewall + harp voice-owner init */
  Serial.println(F("✓"));

  /* PHASE 6 — Laser + D-BEAM peripherals */
  Serial.print(F("  [6] Laser MCPWM + SPI DAC ... "));
  initLaser(); /* laser.cpp: setupMCPWM [C] + SPI + galvo park */
  Serial.println(F("✓"));

  Serial.print(F("  [6] D-BEAM DMA ADC ... "));
  initDBeamSensor(); /* [SAVE-FIX8] alloc + config here (original timing, before the
                      * laser task); adc_continuous_start() is deferred to the
                      * D-BEAM task so the shared ADC lock is owned by the task that
                      * stops it in the self-heal (avoids xTaskPriorityDisinherit). */
  Serial.println(F("✓"));

  laserOff();

  Serial.print(F("  [6] String duty init ... "));
  initStringDuty(); /* globals.h: stringDuty[] = DUTY_UNITY */
  Serial.println(F("✓"));

  /* PHASE 7 — DSP patch tables (MUST complete before RT tasks start) ─────── */
  Serial.print(F("  [7] LivePatch sync ... "));
  syncLivePatchFromAtomics();
  Serial.println(F("✓"));

  Serial.print(F("  [7] Hue envelopes ... "));
  initHueEnvelopes();
  Serial.println(F("✓"));

  Serial.print(F("  [7] Sequencer ... "));
  initSequencer();
  Serial.println(F("✓"));

  /* PHASE 8 — Audio system + FreeRTOS tasks (parked on g_systemReady until end) */
  Serial.print(F("  [8] Audio system + tasks ... "));
  if (!init_audio_system()) {
    Serial.println(F("FATAL — DSP init failed"));
    while (1) delay(100);
  }
  Serial.println(F("✓"));

  /* Laser state — set BEFORE g_systemReady so the first laser wake sees OPENING */
  sweepState = SweepState::MOVING;
  currentIndex.store(0, std::memory_order_relaxed);
  stateStartUs = (uint32_t)esp_timer_get_time();
  harpMode.store(HarpMode::OPENING, std::memory_order_relaxed);

  /* Release all RT tasks (audio / laser / ADC / MIDI / OLED / NVS). */
  std::atomic_thread_fence(std::memory_order_release);
  g_systemReady.store(true, std::memory_order_release);

  delay(50); /* let audioTaskReady flip before the READY banner */

  if (hasOLED) {
    if (xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
      display.clearDisplay();
      display.setCursor(6, 20);
      display.printf("OCTOPUS PRO XL v%s", SYSTEM_FW_VERSION);
      display.setCursor(6, 36);
      display.println(F("     READY TO PLAY"));
      display.display();
      xSemaphoreGive(i2cMutex);
    }
  }

  Serial.println(F("\n╔════════════════════════════════════════╗"));
  Serial.println(F("║       ✓  BOOT COMPLETE — READY         ║"));
  Serial.println(F("╚════════════════════════════════════════╝"));
  Serial.printf("Audio Load: %d%%  Master Vol: %.2f  BPM: %d\n",
                (int)g_audio_load_pct.load(std::memory_order_relaxed),
                masterVol.load(std::memory_order_relaxed),
                (int)seqBpm.load(std::memory_order_relaxed));
  Serial.printf("  Free DRAM: %u bytes  |  Free PSRAM: %u bytes\n",
                heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  Serial.println(F("  (sdkconfig.defaults: IPC stack 4096 — do a full Clean+Rebuild after pull)"));
}

/* ══════════════════════════════════════════════════════════════════════════════
 * loop() — Core 1, priority 1 — SAFETY FALLBACK ONLY
 *
 * All real-time work is in FreeRTOS tasks.  loop() runs only when ALL Core 1
 * tasks are in vTaskDelay (effectively idle).  Its only job: if g_saveArmed
 * fires before laser_sweep_task has seen it, set g_loopParked so NvsWorker
 * can proceed.  Otherwise yield to keep the WDT happy.
 * ══════════════════════════════════════════════════════════════════════════════*/
void loop() {
  if (g_saveArmed.load(std::memory_order_acquire)) {
    g_loopParked.store(true, std::memory_order_release);
  }
  vTaskDelay(pdMS_TO_TICKS(50));
}
