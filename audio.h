/* ═════════════════════════════════════════════════════════════════════════════
 * Octopus PRO XL v6.1.01 — Laser Harp Groovebox
 * © 2026 DIODAC ELECTRONICS / iSystem. All Rights Reserved.
 *
 * PROPRIETARY AND CONFIDENTIAL. Unauthorized copying, distribution, modification,
 * or use of this software or firmware, in whole or in part, is strictly prohibited
 * without prior written permission from DIODAC ELECTRONICS.
 * ═════════════════════════════════════════════════════════════════════════════ */
#pragma once
#ifndef AUDIO_H
#define AUDIO_H

#include <Arduino.h>
#include <atomic>
#include <cmath>
#include <algorithm>
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "globals.h"
#include "harp.h"      
#include "groovebox.h"  
#include "effect.h"
#include "interface.h"
#include "display.h"
#include "settings.h"
#include "midi.h"
#include "patches.h"

/* ═══════════════════════════════════════════════════════════════════════════
 * AUDIO MODULE — I2S output + FreeRTOS task declarations (bodies in audio.cpp)
 *
 * Task spawn: init_audio_system() in audio.cpp — FROZEN schedule in docs/task_schedule.md
 * Buffer size: DMA_BUFFER_FRAMES (512) mono/stereo scratch in audio.cpp.
 * ═══════════════════════════════════════════════════════════════════════════ */
static constexpr size_t DMA_BUFFER_MONO_BYTES   = DMA_BUFFER_FRAMES * sizeof(int16_t);
static constexpr size_t DMA_BUFFER_STEREO_BYTES = DMA_BUFFER_FRAMES * 2u * sizeof(int16_t);

static inline void IRAM_ATTR syncDrumSends() {
  const float rev = drumRevSend.load(std::memory_order_relaxed);
  const float dly = drumDlySend.load(std::memory_order_relaxed);
  portENTER_CRITICAL(&patchMux);
  fx.drumInsert.rev_send = rev;
  fx.drumInsert.dly_send = dly;
  portEXIT_CRITICAL(&patchMux);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * FORWARD DECLARATIONS FOR TASK FUNCTIONS (defined in audio.cpp)
 * ═══════════════════════════════════════════════════════════════════════════ */
bool init_audio_system();       
void IRAM_ATTR audio_synthesis_task(void* pvParameters);
void IRAM_ATTR control_surface_task(void* pvParameters);
void IRAM_ATTR display_refresh_task(void* pvParameters);
void settings_save_task(void* pvParameters);

/* I2S hardware init */
esp_err_t init_i2s_hardware();

#endif /* AUDIO_H */
