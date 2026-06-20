/* ═════════════════════════════════════════════════════════════════════════════
 * Octopus PRO XL v6.0.00 — Laser Harp Groovebox
 * © 2026 DIODAC ELECTRONICS / iSystem. All Rights Reserved.
 *
 * PROPRIETARY AND CONFIDENTIAL. Unauthorized copying, distribution, modification,
 * or use of this software or firmware, in whole or in part, is strictly prohibited
 * without prior written permission from DIODAC ELECTRONICS.
 * ═════════════════════════════════════════════════════════════════════════════
 * audio.h — v6.0.00  DECLARATIONS + TASK MANIFEST
 *
 * Changes vs v5.2.03:
 *
 *  [A1] updateHarpPatch / updateSeqPatch / updateSampleBuffersSync REMOVED.
 *       These functions duplicated the atomics→livePatch conversion that
 *       patches.h applyHarpParam / applySeqParam / applyDrumParam already
 *       perform atomically under patchMux on every encoder/MIDI/WebApp write.
 *       The audio task calling them every frame was an expensive no-op.
 *       v6.0 DSP: harp_synth_fill_buf, gb_seq_fill_buf, drum_fill_buf read
 *       always current because patches.h is the sole writer.
 *
 *  [A2] Full FX parameter snapshot added: mixHarpVol, mixSeqVol, mixDrumsVol,
 *       masterVol, all 4 mutes, aux bus (time/fb/size/damp), drum send levels,
 *       all insert send levels, tube/DJ inserts, master EQ — all snapshotted
 *       under a single brief patchMux window at the start of every DSP frame.
 *       Previously only drumRevSend/drumDlySend were read; all other mix and
 *       FX parameters were silently ignored in the audio path.
 *
 *  [A3] Song mode advance is sample-locked in the audio task: song_advance_rt()
 *       runs at pattern wrap from sequencer_render_block() (groovebox.cpp).
 *       SeqSysexOut (Core 1) only drains STEP_SYNC / SONG_POS echoes to the App.
 *
 *  [A4] Transport ownership [v6.0].  Transport (play/stop/record/BPM) is ALWAYS
 *       hardware-owned.  While the App is connected (isAppConnected()),
 *       updateHardwareInterface() locks the surface to a fixed transport role
 *       (SCALE=play/stop, OC long=record, ENC=BPM, ENC long=save) and the App's
 *       transport buttons are read-only reflectors.  The old transportAvailable
 *       hand-off was retired; the sync supervisor (groovebox.cpp) keeps the App
 *       converged on BPM/play/record/CPU load.
 *
 *  [A5] App connectivity.  isAppConnected() returns true within the heartbeat
 *       window.  display_refresh_task shows the APP CONNECTED splash when true;
 *       full menu and parameter display when offline.
 *
 *  [A6] Master limiting [LIM1].  A single transparent soft-knee limiter lives
 *       inside MasterFX::process() (effect.h).  The old second-stage tanh
 *       saturator in this task was removed — it coloured every sample, even quiet
 *       passages.  fx_process_multi_buf_safe() clamps float→int16 on output.
 *
 * ── TASK MAP (actual pinning — see init_audio_system in audio.cpp) ─────────
 *
 * Core 0 (audio island — I2S + DSP; bursty traffic kept OFF this core):
 *   Priority  Task            Stack   Notes
 *   ────────  ──────────────  ─────   ─────────────────────────────────────
 *      24     AudioSynth      16384   I2S DMA + full DSP pipeline +
 *                                     sample-locked sequencer (no uClock)
 *      19     dbeam_adc        6144   ADC Kalman drain (MUST be Core 0 — laser
 *                                     owns Core 1 during lit dwell)
 *      14     OledRender      16384   OLED @ 30 Hz (preempted by audio/ADC)
 *      10     ControlPoll      8192   encoder + buttons @ 200 Hz
 *
 * Core 1 (laser + bursty data — flash/MIDI/SysEx cache stalls stay off Core 0):
 *   Priority  Task            Stack   Notes
 *   ────────  ──────────────  ─────   ─────────────────────────────────────
 *      24     LaserSweep       8192   galvo one-shot timer sleep [v6.0 Option B]
 *      12     SeqSysexOut      4096   STEP_SYNC / SONG_POS / sync supervisor
 *       6     MidiUsbRx        8192   USB MIDI parser (App SysEx + play-in)
 *       3     NvsWorker        4096   NVS save on demand
 *
 * WHY audio on Core 0 and laser on Core 1:
 *   Core 1 runs Arduino loop() at priority 1 and also needs to host the
 *   laser task at priority 24.  Placing the audio task on Core 1 would make
 *   it share a core with the galvo timing machine, and any FreeRTOS tick
 *   preemption of the laser by the audio would cause visible timing glitches
 *   in the beam fan.  Core 0 is kept clear of any time-deterministic hardware
 *   tasks; its only hard-real-time job is the I2S DMA write, which the OS
 *   naturally handles via the I2S DMA ISR.
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
#include "harp.h"       /* [HARP-SPLIT] laser-harp instrument (was synth.h) */
#include "groovebox.h"  /* [GB-MERGE] seq + drum + patterns + transport   */
#include "effect.h"
#include "interface.h"
#include "display.h"
#include "settings.h"
#include "midi.h"
#include "patches.h"

/* ═══════════════════════════════════════════════════════════════════════════
 * BUFFER CONSTANTS
 * Consistent with globals.h: DMA_BUFFER_FRAMES = 512, SAMPLE_RATE = 44100.
 * ═══════════════════════════════════════════════════════════════════════════ */
static constexpr size_t DMA_BUFFER_MONO_BYTES   = DMA_BUFFER_FRAMES * sizeof(int16_t);
static constexpr size_t DMA_BUFFER_STEREO_BYTES = DMA_BUFFER_FRAMES * 2u * sizeof(int16_t);

/* ═══════════════════════════════════════════════════════════════════════════
 * PER-FRAME DRUM-SEND SYNC  [aud BUG-1/BUG-2/ARCH-1 · Fix A]
 *
 * The old AudioFrameParams snapshot ([A2]) was abandoned dead code: it loaded
 * ~20 atomics + read 4 fx fields under patchMux every buffer, but the populated
 * struct was never consumed by any DSP/FX call (they read the atomics directly).
 * The ONLY per-frame write actually needed is mirroring the D-BEAM-driven drum
 * send atomics into the fx struct.  All other FX params are maintained directly
 * by the patches.h apply* functions on each encoder/MIDI/App event — no polling.
 *
 * This strips the work to its single real job: 2 relaxed loads + a 2-write
 * critical section (down from 18 wasted loads + 4 dead in-lock reads).
 * ═══════════════════════════════════════════════════════════════════════════ */
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
bool init_audio_system();       /* call from setup() after hardware init    */

/* Individual task functions (passed to xTaskCreatePinnedToCore)
 * IRAM_ATTR here MUST match the definitions in audio.cpp so the linker
 * placement is unambiguous (declaration/definition attribute parity). */
void IRAM_ATTR audio_synthesis_task(void* pvParameters);
void IRAM_ATTR control_surface_task(void* pvParameters);
void IRAM_ATTR display_refresh_task(void* pvParameters);
void settings_save_task(void* pvParameters);

/* I2S hardware init */
esp_err_t init_i2s_hardware();

#endif /* AUDIO_H */
