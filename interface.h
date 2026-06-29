/* ═════════════════════════════════════════════════════════════════════════════
 * Octopus PRO XL v6.1.01 — Laser Harp Groovebox
 * © 2026 DIODAC ELECTRONICS / iSystem. All Rights Reserved.
 *
 * PROPRIETARY AND CONFIDENTIAL. Unauthorized copying, distribution, modification,
 * or use of this software or firmware, in whole or in part, is strictly prohibited
 * without prior written permission from DIODAC ELECTRONICS.
 * ═════════════════════════════════════════════════════════════════════════════
 * interface.h — v6.1.01  HARDWARE INTERFACE + MENU ARCHITECTURE
 *
 * GPIO map, ButtonPoll/EncoderPoll, menu L1/L2 tables, gesture timing.
 * Implementation: interface.cpp (control_surface_task @ 200 Hz, Core 0).
 *
 * ── HARDWARE LAYOUT ──────────────────────────────────────────────────────────
 *
 *   All 3 buttons (PIN_ENC_BTN, PIN_BTN_OC, PIN_BTN_SCALE) are INPUT_PULLUP.
 *   Active state = LOW (pressed).  fastRead() returns true for HIGH (open).
 *
 *   SCALE  short: SEQ = play/stop toggle   HARP = panic + next scale
 *   SCALE  long:  toggle dashboard (SEQ ↔ HARP)
 *   SCALE + turn: harp octave shift (±4)
 *
 *   OC     short: SEQ = record toggle      HARP = cycle play mode
 *   OC     long:  HARP = open/close harp
 *
 *   ENC    short: menu navigate down
 *   ENC    double: menu navigate back
 *   ENC    long:  save settings to NVS
 *   ENC + turn (in SEQ MATRIX): step toggle / grid navigation
 *
 * ═════════════════════════════════════════════════════════════════════════════ */
#pragma once
#ifndef INTERFACE_H
#define INTERFACE_H

#include <Arduino.h>
#include <algorithm>
#include <cmath>
#include "globals.h"
#include "driver/gpio.h"

/* ═══════════════════════════════════════════════════════════════════════════
 * SECTION 1 — BARE-METAL GPIO
 * ═══════════════════════════════════════════════════════════════════════════ */

/* fastRead — direct register read, ~2 CPU cycles.
 * Returns true for HIGH (logic 1).  For INPUT_PULLUP buttons:
 *   fastRead(PIN_BTN_OC) == true  → button OPEN (not pressed)
 *   fastRead(PIN_BTN_OC) == false → button PRESSED (active LOW)         */
static inline bool IRAM_ATTR fastRead(uint8_t pin) {
  if (pin < 32) return (REG_READ(GPIO_IN_REG) >> pin) & 1u;
  else return (REG_READ(GPIO_IN1_REG) >> (pin - 32)) & 1u;
}

/* fastWrite — set/clear via W1TS/W1TC (write-one-to-set/clear), ~2 cycles.
 * Never produces a glitch: the hardware does a single atomic register write.
 * level=HIGH → pin driven HIGH;  level=LOW → pin driven LOW.             */
static inline void IRAM_ATTR fastWrite(uint8_t pin, bool level) {
  if (level) {
    if (pin < 32) REG_WRITE(GPIO_OUT_W1TS_REG, 1UL << pin);
    else REG_WRITE(GPIO_OUT1_W1TS_REG, 1UL << (pin - 32));
  } else {
    if (pin < 32) REG_WRITE(GPIO_OUT_W1TC_REG, 1UL << pin);
    else REG_WRITE(GPIO_OUT1_W1TC_REG, 1UL << (pin - 32));
  }
}

/* Backward-compatibility alias (used by .ino laser kernel) */
static inline bool IRAM_ATTR fastPinRead(uint8_t pin) {
  return fastRead(pin);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * SECTION 2 — TIMING CONSTANTS
 * ═══════════════════════════════════════════════════════════════════════════ */
static constexpr uint32_t DEBOUNCE_MS = 50;
/* SCALE + OPEN/CLOSE are firm, deliberate gestures (play/stop, scale change,
 * open/close, mute) — give them a longer debounce so a single press can't bounce
 * into a double/too-fast action.  The encoder button keeps the snappy 50 ms. */
static constexpr uint32_t DEBOUNCE_SLOW_MS = 90;
static constexpr uint32_t ENC_LONG_MS = 600;
static constexpr uint32_t ENC_DBL_MS = 230;   /* double-click window; lower = snappier single */
static constexpr uint32_t OC_LONG_MS = 800;
static constexpr uint32_t SCALE_LONG_MS = 550;
static constexpr uint32_t OC_SCALE_COMBO_MS = 300; /* both held within this → mute */
static constexpr int32_t ENC_PPR = 4;              /* pulses per physical detent   */

/* RETIRED — encoder is linear 1:1 (EncoderPoll.getDelta); constants kept unused. */
static constexpr uint32_t ENC_FAST_MS = 80;
static constexpr uint32_t ENC_MEDIUM_MS = 150;

/* ═══════════════════════════════════════════════════════════════════════════
 * SECTION 3 — BUTTON POLL
 *
 * Unified event state machine for all three hardware buttons.
 * All buttons are INPUT_PULLUP → pressed = state LOW = fastRead() returns false.
 * Call update() once per control loop iteration, then poll() for the event.
 * ═══════════════════════════════════════════════════════════════════════════ */
enum class BtnEvent : uint8_t { NONE,
                                SINGLE,
                                DOUBLE,
                                LONG };

struct ButtonPoll {
  /* ── Config ─────────────────────────────────────────────────────────── */
  uint8_t pin = 0;

  /* ── Debounced state ─────────────────────────────────────────────────── */
  bool state = true; /* true = HIGH = not pressed (PULLUP)   */
  uint32_t debMs = 0;
  uint32_t debounceMs = DEBOUNCE_MS; /* per-button; raise for firm gestures */

  /* ── Edge events (valid for one update() call) ───────────────────────── */
  bool justFell = false; /* HIGH→LOW = button just pressed       */
  bool justRose = false; /* LOW→HIGH = button just released      */

  /* ── Long-press tracking ─────────────────────────────────────────────── */
  uint32_t pressMs = 0;
  bool longFired = false;

  /* ── Click-classification state machine ─────────────────────────────── */
  enum class CS : uint8_t { IDLE,
                            WAIT_UP,
                            WAIT_DBL,
                            WAIT_DBL_UP,
                            WAIT_LONG_UP } cs{ CS::IDLE };
  uint32_t upMs = 0; /* timestamp of last release            */

  /* ── GPIO config (call once from setup()) ───────────────────────────── */
  void init(uint8_t p) {
    pin = p;
    const gpio_config_t cfg = {
      .pin_bit_mask = (1ULL << pin),
      .mode = GPIO_MODE_INPUT,
      .pull_up_en = GPIO_PULLUP_ENABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&cfg);
    state = true;
    debMs = pressMs = upMs = 0;
    justFell = justRose = longFired = false;
    cs = CS::IDLE;
  }

  /* ── update() — call every control loop iteration ───────────────────── */
  void update(uint32_t now) {
    justFell = justRose = false;
    const bool reading = fastRead(pin);
    if (reading != state && (now - debMs > debounceMs)) {
      state = reading;
      debMs = now;
      if (!state) {
        justFell = true;
        pressMs = now;
        longFired = false;
      } else {
        justRose = true;
        upMs = now;
      }
    }
  }

  bool isPressed() const {
    return !state;
  }

  /* Long press with one-shot guard — call every loop iteration.
   * Returns true exactly ONCE when the hold threshold is reached.         */
  bool checkLong(uint32_t now, uint32_t threshMs) {
    if (isPressed() && !longFired && (now - pressMs) >= threshMs) {
      longFired = true;
      return true;
    }
    return false;
  }

  /* ── poll() — classify click event ──────────────────────────────────── */
  /* Returns BtnEvent once the gesture is resolved.
   * cancelOnTurn: set delta != 0 to cancel pending click (encoder-button
   * guard: don't fire SINGLE if the encoder was also turned).
   * wantDouble: when false, SINGLE fires immediately on release (no double-
   *   click wait).  Use for buttons that never emit DOUBLE — removes the
   *   perceptible ~dblMs latency on every press.                          */
  BtnEvent poll(uint32_t now, uint32_t longMs, uint32_t dblMs,
                int32_t cancelDelta = 0, bool wantDouble = true) {
    if (cancelDelta != 0) {
      cs = CS::IDLE;
      return BtnEvent::NONE;
    }
    switch (cs) {
      case CS::IDLE:
        if (justFell) { cs = CS::WAIT_UP; }
        break;

      case CS::WAIT_UP:
        if (justRose) {
          if (!wantDouble) { cs = CS::IDLE; return BtnEvent::SINGLE; } /* instant */
          cs = CS::WAIT_DBL;
        } else if ((now - pressMs) >= longMs && !longFired) {
          longFired = true;
          cs = CS::WAIT_LONG_UP;
          return BtnEvent::LONG;
        }
        break;

      case CS::WAIT_DBL:
        if (justFell) {
          cs = CS::WAIT_DBL_UP;
        } else if ((now - upMs) >= dblMs) {
          cs = CS::IDLE;
          return BtnEvent::SINGLE;
        }
        break;

      case CS::WAIT_DBL_UP:
        if (justRose) {
          cs = CS::IDLE;
          return BtnEvent::DOUBLE;
        }
        break;

      case CS::WAIT_LONG_UP:
        if (justRose) cs = CS::IDLE;
        break;
    }
    return BtnEvent::NONE;
  }
};

/* ═══════════════════════════════════════════════════════════════════════════
 * SECTION 4 — ENCODER POLL (PURE LINEAR 1:1)
 * ═══════════════════════════════════════════════════════════════════════════ */
struct EncoderPoll {
  int64_t  lastCount    = 0;   /* must match ESP32Encoder::getCount() width */
  uint32_t lastMoveMs   = 0;   /* retained for ABI; unused in linear mode */

  /* getDelta — read accumulated encoder turns since last call.
   * delta > 0 = clockwise, delta < 0 = counter-clockwise.
   *
   * PURE LINEAR 1:1 — one detent = one step, no velocity acceleration.  The
   * /ENC_PPR division keeps the sub-detent remainder (lastCount advances by
   * whole detents only), so no counts are ever lost between polls even when
   * control_surface_task is briefly starved on Core 0.                        */
  int16_t getDelta(int64_t rawCount, uint32_t now) {
    (void)now;
    const int64_t diff = (rawCount - lastCount) / ENC_PPR;
    if (diff == 0) return 0;
    lastCount += diff * ENC_PPR;
    return (int16_t)std::clamp(diff, (int64_t)INT16_MIN, (int64_t)INT16_MAX);
  }
};

/* ═══════════════════════════════════════════════════════════════════════════
 * SECTION 5 — MENU CONSTANTS
 *
 * Moved here from display.h — interface.cpp needs them; display.h retains
 * only the human-readable label string arrays for rendering.
 *
 * L1 category layout (0–15):
 *   0 HARP SETUP    — gate, beam thresholds, RGB, margin, stuck-release, edge-comp
 *   1 D-BEAM        — offset, range, curve, enable, env, route, target
 *   2 MASTER        — vols, pitch, EQ, tube, DJ, mutes, pan
 *   3 HARP SYNTH    — SynthParams + FX slots + sends + user slots + harp arp
 *   4 MIDI I/O      — pitch-bend range/enable + wire MIDI channels
 *   5 SEQ SETUP     — bank, view, length, transpose, presets, clear, user pat, arp
 *   6 SEQ MATRIX    — grid editor (special: no L2 scroll)
 *   7 AUX FX        — aux bus + insert sends + FX slots
 *   8 SEQ SYNTH     — SynthParams + FX + user slots (no harp arp items)
 *   9 DRUM KIT      — 5 params × 8 drums + kit + drum pitch
 *  10 LASER SHOW    — show toggle, hue ADSR, anim
 *  11 TELEMETRY     — 7 live scope pages (L2 pick → ENC opens view; turn cycles)
 *  12 RESET         — full / banks+patterns / motion / settings → reboot
 *  13 SONG          — row-per-step song editor (special: no L2 scroll) — hidden L1 v6.6
 *  14 PERF SLOT     — SongPack PERF 1–8 load/save (was SAVE scoped persist)
 * ═══════════════════════════════════════════════════════════════════════════ */
static constexpr int kL1CatCount = 16;  /* fixed category ids 0..15 (15=retired LOAD) */
static constexpr int kL1MenuCount = 9;  /* visible MAIN MENU items (v6.6 performance order) */

/* Per-category L2 item counts */
static constexpr int kL2HarpSetupCount = 13; /* gate/white/touch/RGB/margin/stuck/edge/fog/screensaver */
static constexpr int kL2DBeamCount = 8;     /* offset/range/curve/enable/env atk/rel/route/target */
static constexpr int kL2MasterCount = 24;   /* vol/FX/mutes + H/S/D pan */
static constexpr int kL2SynthCount = 25;    /* synth params + FX + user slots + harp arp */
static constexpr int kL2MidiCount = 5;
static constexpr int kL2SeqSetupCount = 13; /* bank … arp gate (incl. clear + user pat) */
static constexpr int kL2AuxFxCount = 16;
static constexpr int kL2SeqSynthCount = 21; /* synth params + FX + user slots (no harp arp) */
static constexpr int kL2DrumsCount = 42;    /* 5×8 + kit + drum pitch */
static constexpr int kL2LaserShowCount = 9;
static constexpr int kL2TelemetryCount = 7; /* matches TelemetryView RAW_AC..FOG_REJECT */
static constexpr int kL2ResetCount = 4;
static constexpr int kL2SongCount = 1;
static constexpr int kL2SystemCount = 3;   /* SYSTEM hub: MIDI / Telemetry / Reset */
static constexpr int kL2PerfSlotCount = 3; /* PERF load / save / name */
static constexpr int kL2SeqMatrixCount = 0; /* direct grid — no L2 picker */

static inline int l2CountFor(int l1) {
  switch (l1) {
    case 0: return kL2HarpSetupCount;
    case 1: return kL2DBeamCount;
    case 2: return kL2MasterCount;
    case 3: return kL2SynthCount;
    case 4: return kL2MidiCount;
    case 5: return kL2SeqSetupCount;
    case 6: return kL2SeqMatrixCount;
    case 7: return kL2AuxFxCount;
    case 8: return kL2SeqSynthCount;
    case 9: return kL2DrumsCount;
    case 10: return kL2LaserShowCount;
    case 11: return kL2TelemetryCount;
    case 12: return kL2ResetCount;
    case 13: return kL2SongCount;
    case 14: return kL2PerfSlotCount;
    case 15: return 0; /* LOAD retired */
    default: return 0;
  }
}

/* L1 DISPLAY ORDER — v6.6 performance surface (§14.3 D, docs/oled_ui_wireframe_v6.6.md §4).
 * Category ids stay fixed; only scroll order + labels change. */
static constexpr int kL1Order[kL1MenuCount] = {
  0,  /* HARP PLAY  — cat 0 HARP SETUP */
  3,  /* HARP TONE  — cat 3 HARP SYNTH */
  5,  /* SEQ PLAY   — cat 5 SEQ SETUP  */
  6,  /* SEQ MATRIX */
  9,  /* DRUM       — cat 9 DRUM KIT   */
  2,  /* MIX LIVE   — cat 2 MASTER     */
  1,  /* D-BEAM     */
  14, /* PERF SLOT  — cat 14 (was SAVE) */
  4,  /* SYSTEM     — cat 4 MIDI hub   */
};

/* category id → display slot. Hidden cats (7,8,10–13) use slot 0 (not in L1 scroll). */
static constexpr int kCatToSlot[kL1CatCount] = {
  0,  /* cat  0 HARP PLAY  → slot 0 */
  6,  /* cat  1 D-BEAM     → slot 6 */
  5,  /* cat  2 MIX LIVE   → slot 5 */
  1,  /* cat  3 HARP TONE  → slot 1 */
  8,  /* cat  4 SYSTEM     → slot 8 */
  2,  /* cat  5 SEQ PLAY   → slot 2 */
  3,  /* cat  6 SEQ MATRIX  → slot 3 */
  0,  /* cat  7 AUX FX     — hidden (App / MIX LIVE) */
  0,  /* cat  8 SEQ SYNTH  — hidden (App) */
  4,  /* cat  9 DRUM       → slot 4 */
  0,  /* cat 10 LASER SHOW — hidden (service) */
  0,  /* cat 11 TELEMETRY  — via SYSTEM */
  0,  /* cat 12 RESET      — via SYSTEM */
  0,  /* cat 13 SONG       — hidden (App Bank Manager) */
  7,  /* cat 14 PERF SLOT  → slot 7 */
  0   /* cat 15 LOAD       — retired */
};
static_assert(kL1Order[0] == 0 && kL1Order[1] == 3 && kL1Order[2] == 5 &&
              kL1Order[3] == 6 && kL1Order[4] == 9 && kL1Order[5] == 2 &&
              kL1Order[6] == 1 && kL1Order[7] == 14 && kL1Order[8] == 4,
              "kL1Order v6.6 performance slots");
static_assert(kCatToSlot[0] == 0 && kCatToSlot[1] == 6 && kCatToSlot[2] == 5 &&
              kCatToSlot[3] == 1 && kCatToSlot[4] == 8 && kCatToSlot[5] == 2 &&
              kCatToSlot[6] == 3 && kCatToSlot[9] == 4 && kCatToSlot[14] == 7,
              "kCatToSlot inverse for visible L1 categories");

static inline int l1SlotForCat(int cat) {
  if (cat >= 0 && cat < kL1CatCount) return kCatToSlot[cat];
  return 0;
}
/* display slot → category id (wraps the slot first) */
static inline int l1CatForSlot(int slot) {
  slot = ((slot % kL1MenuCount) + kL1MenuCount) % kL1MenuCount;
  return kL1Order[slot];
}

/* Drum channel names and parameter names for display.h */
static constexpr const char* kDrumChName[8] = {
  "Kick", "Snare", "Clap", "HH-C", "HH-O", "Tom-H", "Tom-L", "Perc"
};
static constexpr const char* kDrumParamName[5] = {
  "Tune", "Decay", "Vol", "Noise", "Wave"
};
/* Aux menu labels: kL2AuxFx[16] in display.h (SSOT). */
/* Master menu mute names (l2 = 18/19/20) */
static constexpr const char* kMuteName[3] = { "Harp Mute", "Seq Mute", "Drm Mute" };

/* ═══════════════════════════════════════════════════════════════════════════
 * SECTION 6 — UTILITY TEMPLATES
 * ═══════════════════════════════════════════════════════════════════════════ */

/* wrapIndex — signed wrap in [0, size).  Handles negative wrap correctly. */
template<typename T>
static inline T wrapIndex(T current, T delta, T size) {
  if (size <= 0) return 0;
  T r = (current + delta) % size;
  return (r < 0) ? r + size : r;
}

/* safe_atomic_store — clamp-then-store, rejects NaN/Inf; returns clamped value. */
static inline float safe_atomic_store(std::atomic<float>& a, float cur,
                                      float delta, float lo, float hi) {
  const float nv = std::min(hi, std::max(lo, cur + delta));
  if (!isnan(nv) && !isinf(nv)) { a.store(nv, std::memory_order_relaxed); return nv; }
  return a.load(std::memory_order_relaxed);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * SECTION 7 — EXTERN DECLARATIONS
 * ═══════════════════════════════════════════════════════════════════════════ */
extern ButtonPoll btnEnc;
extern ButtonPoll btnOC;
extern ButtonPoll btnScale;

extern EncoderPoll encPoll;

struct InterfaceStats {
  uint32_t encoder_updates, interface_loops, factory_resets;
};
struct DisplayI2CStats {
  uint32_t init_failures, bus_resets;
};
extern InterfaceStats interface_stats;
extern DisplayI2CStats display_stats;

/* ═══════════════════════════════════════════════════════════════════════════
 * SECTION 8 — FUNCTION DECLARATIONS
 * ═══════════════════════════════════════════════════════════════════════════ */
enum class ResetScope : uint8_t; /* defined in settings.h */
void init_fast_gpio();
void initHardwareInterface();
void updateHardwareInterface();
void updateHardwareParameter(uint8_t l1, uint8_t l2, int16_t delta);
void handleFactoryReset();
void handleScopedReset(ResetScope scope);
void handleScopedSave(ResetScope scope);  /* persist + reboot */
void updateTaskStackStats();  /* sample uxTaskGetStackHighWaterMark → g_stackStats */
void printInterfaceStats();   /* recurring Serial telemetry (stack + heap)       */

/* Encoder button classifier (delta-cancel guard on double-click back-nav). */
BtnEvent pollEncoderButton(uint32_t now, int32_t delta);

/* Pitch encode/decode: ratio [MASTER_PITCH_MIN, MAX] ↔ v14 semitone-linear ±24 st */
uint16_t encodeMasterPitch(float pitch);
float decodeMasterPitch(uint16_t v14);

#endif /* INTERFACE_H */
