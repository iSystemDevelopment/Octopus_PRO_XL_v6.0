/* ═════════════════════════════════════════════════════════════════════════════
 * Octopus PRO XL v6.0.00 — Laser Harp Groovebox
 * © 2026 DIODAC ELECTRONICS / iSystem. All Rights Reserved.
 *
 * PROPRIETARY AND CONFIDENTIAL. Unauthorized copying, distribution, modification,
 * or use of this software or firmware, in whole or in part, is strictly prohibited
 * without prior written permission from DIODAC ELECTRONICS.
 * ═════════════════════════════════════════════════════════════════════════════
 * interface.h — v6.0.00  HARDWARE INTERFACE + MENU ARCHITECTURE
 *
 * Changes vs v5.1.01:
 *
 *  [I1] fastRead() / fastWrite() — free functions with bare-metal register
 *       access added alongside the existing FastButton member fastRead().
 *       Use anywhere in the codebase — .ino laser kernel, dbeam.cpp, etc.
 *       Always IRAM_ATTR; ~2 clock cycles vs ~40 for Arduino digitalRead().
 *
 *  [I2] ButtonPoll — replaces FastButton with a unified event state machine
 *       that handles SINGLE, DOUBLE, and LONG press for ALL three buttons
 *       (encoder, OC, SCALE).  pollEncoderButton() is preserved as a thin
 *       wrapper that adds the delta-reset guard (no click on encoder turn).
 *
 *  [I3] EncoderPoll — encoder read in one struct.  PURE LINEAR 1:1 (one detent
 *       = one step, no velocity acceleration) for predictable feel.  /ENC_PPR
 *       keeps the sub-detent remainder so no counts are lost between polls.
 *       Hardware counting is done by the ESP32Encoder PCNT peripheral (same as
 *       the library's interrupt example — the optional enc_cb there is NOT what
 *       increments the counter).  We poll getCount() from control_surface_task.
 *
 *  [I4] Menu constants moved here from display.h — kL1Count, l2CountFor(),
 *       and per-category item counts are interface logic, not display logic.
 *       display.h retains the human-readable label arrays for rendering.
 *
 *  [I5] Menu structure (v6.0):
 *       Case 7  "SEQ SETTINGS" (dead) → "AUX FX" (14 items).
 *       Case 9  DRUM KIT: 41 items (5 params × 8 drums + kit selector).
 *       Case 2  MASTER: +3 mute items (harp/seq/drum).
 *       kL1Count = 16 (incl. SONG/SAVE/LOAD at slots 12–15).
 *
 *  [I6] OC+SCALE combo for mute toggle added (hold both < 300 ms):
 *       Press OC while SCALE held = toggle harp/seq/drum mute per context.
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

/* Encoder velocity thresholds (ms between detents) */
static constexpr uint32_t ENC_FAST_MS = 80;    /* interval < 80  → ×5 acceleration */
static constexpr uint32_t ENC_MEDIUM_MS = 150; /* interval < 150 → ×2              */

/* ═══════════════════════════════════════════════════════════════════════════
 * SECTION 3 — BUTTON POLL  [I2]
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
 * SECTION 4 — ENCODER POLL (PURE LINEAR 1:1)  [I3]
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
   * control_surface_task is briefly starved on Core 1.                        */
  int16_t getDelta(int64_t rawCount, uint32_t now) {
    (void)now;
    const int64_t diff = (rawCount - lastCount) / ENC_PPR;
    if (diff == 0) return 0;
    lastCount += diff * ENC_PPR;
    return (int16_t)std::clamp(diff, (int64_t)INT16_MIN, (int64_t)INT16_MAX);
  }
};

/* ═══════════════════════════════════════════════════════════════════════════
 * SECTION 5 — MENU CONSTANTS  [I4]
 *
 * Moved here from display.h — interface.cpp needs them; display.h retains
 * only the human-readable label string arrays for rendering.
 *
 * L1 category layout (0–15):
 *   0 HARP SETUP    — gate, beam thresholds, RGB, margin, stuck-release, edge-comp
 *   1 D-BEAM        — offset, range, curve, enable, env, route, target
 *   2 MASTER        — vols, pitch, EQ, tube, DJ, mutes
 *   3 HARP SYNTH    — all 14 SynthParams + FX slots + sends
 *   4 MIDI I/O      — pitch-bend range/enable + wire MIDI channels
 *   5 SEQ SETUP     — bank, chain, len, transpose, presets, user pat slots
 *   6 SEQ MATRIX    — grid editor (special: no L2 scroll)
 *   7 AUX FX        — aux bus + insert sends + FX slots  [I5]
 *   8 SEQ SYNTH     — all 14 SynthParams + FX slots + sends
 *   9 DRUM KIT      — 5 params × 8 drums + kit selector (41 items)
 *  10 LASER SHOW    — show toggle, hue ADSR
 *  11 TELEMETRY     — scope views (special: no L2 scroll)
 *  12 RESET         — full / banks+patterns / motion / settings  [I6]
 *                     (YES/NO confirm → RAM wipe + persist + reboot)
 *  13 SONG          — row-per-step song editor (special: no L2 scroll) [SONG]
 *  14 SAVE          — full / banks+patterns / motion / settings  [WS10]
 *                     (YES/NO confirm → persist + reboot, resumes in place)
 *  15 LOAD          — full / banks+patterns / motion / settings  [LOAD-MENU]
 *                     (YES/NO confirm → RAM-only reload, no reboot)
 * Note: SEQ SETUP also hosts Clear + Soft Reset (RAM-only working-image reset).
 * ═══════════════════════════════════════════════════════════════════════════ */
static constexpr int kL1Count = 16; /* +RESET (12); +SONG (13); +SAVE (14); +LOAD (15) */

/* Per-category L2 item counts */
static constexpr int kL2HarpSetupCount = 13; /* gate/white/touch/RGB/margin/stuck/edge/fog/screensaver */
static constexpr int kL2DBeamCount = 8;     /* offset/range/curve/enable/env atk/rel/route/target */
static constexpr int kL2MasterCount = 24;   /* +3 pan vs v5.4 (was 21)  */
static constexpr int kL2SynthCount = 25;    /* … + [HARP-ARP] 4 items at idx 21–24 */
static constexpr int kL2MidiCount = 5;      /* PB range/enable + 3 channels (Route moved to D-BEAM) */
static constexpr int kL2SeqSetupCount = 14; /* + Arp On/Type/Rate/Gate */
static constexpr int kL2SeqMatrixCount = 1; /* grid (single entry → open)  */
static constexpr int kL2AuxFxCount = 14;    /* aux bus + sends + FX slots  */
static constexpr int kL2SeqSynthCount = 21; /* same layout as harp synth + Snd Preset + [USER-SLOTS] Save/Load Slot */
static constexpr int kL2DrumsCount = 42;    /* 5 params × 8 drums + Kit + Drm Pitch */
static constexpr int kL2LaserShowCount = 9;   /* show/midihue/base/anim/drumflash/hue ADSR×4 */
static constexpr int kL2TelemetryCount = 4;
static constexpr int kL2ResetCount = 4; /* [I6] full/banks+pat/motion/settings */
static constexpr int kL2SongCount = 1;  /* [SONG] single entry → opens row editor */
static constexpr int kL2SaveCount = 4;  /* [WS10] mirrors RESET scopes */
static constexpr int kL2LoadCount = 4;  /* [LOAD-MENU] mirrors SAVE/RESET scopes */

static inline int l2CountFor(int l1) {
  switch (l1) {
    case 0: return kL2HarpSetupCount;
    case 1: return kL2DBeamCount;
    case 2: return kL2MasterCount;
    case 3: return kL2SynthCount;
    case 4: return kL2MidiCount;
    case 5: return kL2SeqSetupCount;
    case 6: return kL2SeqMatrixCount;
    case 7: return kL2AuxFxCount; /* AUX FX [I5] */
    case 8: return kL2SeqSynthCount;
    case 9: return kL2DrumsCount; /* expanded [I5] */
    case 10: return kL2LaserShowCount;
    case 11: return kL2TelemetryCount;
    case 12: return kL2ResetCount; /* RESET [I6] */
    case 13: return kL2SongCount;  /* SONG  [SONG] */
    case 14: return kL2SaveCount;  /* SAVE  [WS10] */
    case 15: return kL2LoadCount;  /* LOAD  [LOAD-MENU] */
    default: return 0;
  }
}

/* ── [I7] L1 DISPLAY ORDER ──────────────────────────────────────────────────
 * The category *id* (stored in currentMenuL1) is FIXED — every switch/special-
 * case (mutateParam, l2CountFor, l1==6 grid, l1==13 SONG, l1==11 TELEMETRY …)
 * keys off it.  Only the *visible* order in MAIN MENU is regrouped, so related
 * categories sit together.  kL1Order maps display-slot → category id; the menu
 * renderer and the L1 encoder step convert between the two.
 *   slot:  0           1          2         3           4          5
 *          HARP SETUP  HARP SYNTH SEQ SETUP SEQ MATRIX  SEQ SYNTH  SONG
 *   slot:  6           7          8         9           10         11
 *          DRUM KIT    AUX FX     MASTER    D-BEAM      MIDI I/O   LASER SHOW
 *   slot:  12          13          14
 *          TELEMETRY   RESET       SAVE                                                     */
static constexpr int kL1Order[kL1Count] = {
  0,  /* HARP SETUP */
  3,  /* HARP SYNTH */
  5,  /* SEQ SETUP  */
  6,  /* SEQ MATRIX */
  8,  /* SEQ SYNTH  */
  13, /* SONG       */
  9,  /* DRUM KIT   */
  7,  /* AUX FX     */
  2,  /* MASTER     */
  1,  /* D-BEAM     */
  4,  /* MIDI I/O   */
  10, /* LASER SHOW */
  11, /* TELEMETRY  */
  12, /* RESET      */
  14, /* SAVE       */
  15  /* LOAD       */
};

/* [IDM-OPT4] category id → display slot, precomputed inverse of kL1Order.
 * Replaces the old O(14) linear scan run on every encoder tick (200 Hz) and
 * every drawMenuL1 (30 Hz) with a single table index.  The static_assert keeps
 * it provably in sync with kL1Order: kL1Order[kCatToSlot[c]] == c for all c. */
static constexpr int kCatToSlot[kL1Count] = {
  0,   /* cat  0 HARP SETUP → slot  0 */
  9,   /* cat  1 D-BEAM     → slot  9 */
  8,   /* cat  2 MASTER     → slot  8 */
  1,   /* cat  3 HARP SYNTH → slot  1 */
  10,  /* cat  4 MIDI I/O   → slot 10 */
  2,   /* cat  5 SEQ SETUP  → slot  2 */
  3,   /* cat  6 SEQ MATRIX → slot  3 */
  7,   /* cat  7 AUX FX     → slot  7 */
  4,   /* cat  8 SEQ SYNTH  → slot  4 */
  6,   /* cat  9 DRUM KIT   → slot  6 */
  11,  /* cat 10 LASER SHOW → slot 11 */
  12,  /* cat 11 TELEMETRY  → slot 12 */
  13,  /* cat 12 RESET      → slot 13 */
  5,   /* cat 13 SONG       → slot  5 */
  14,  /* cat 14 SAVE       → slot 14 */
  15   /* cat 15 LOAD       → slot 15 */
};
static_assert(
  kL1Order[kCatToSlot[0]]  == 0  && kL1Order[kCatToSlot[1]]  == 1  &&
  kL1Order[kCatToSlot[2]]  == 2  && kL1Order[kCatToSlot[3]]  == 3  &&
  kL1Order[kCatToSlot[4]]  == 4  && kL1Order[kCatToSlot[5]]  == 5  &&
  kL1Order[kCatToSlot[6]]  == 6  && kL1Order[kCatToSlot[7]]  == 7  &&
  kL1Order[kCatToSlot[8]]  == 8  && kL1Order[kCatToSlot[9]]  == 9  &&
  kL1Order[kCatToSlot[10]] == 10 && kL1Order[kCatToSlot[11]] == 11 &&
  kL1Order[kCatToSlot[12]] == 12 && kL1Order[kCatToSlot[13]] == 13 &&
  kL1Order[kCatToSlot[14]] == 14 && kL1Order[kCatToSlot[15]] == 15,
  "kCatToSlot must be the exact inverse of kL1Order");

static inline int l1SlotForCat(int cat) {
  if (cat >= 0 && cat < kL1Count) return kCatToSlot[cat];
  return 0;
}
/* display slot → category id (wraps the slot first) */
static inline int l1CatForSlot(int slot) {
  slot = ((slot % kL1Count) + kL1Count) % kL1Count;
  return kL1Order[slot];
}

/* Drum channel names and parameter names for display.h */
static constexpr const char* kDrumChName[8] = {
  "Kick", "Snare", "Clap", "HH-C", "HH-O", "Tom-H", "Tom-L", "Perc"
};
static constexpr const char* kDrumParamName[5] = {
  "Tune", "Decay", "Vol", "Noise", "Wave"
};
/* Aux FX parameter names (case 7) */
static constexpr const char* kAuxFxName[14] = {
  "Dly Time", "Dly FB", "Rev Size", "Rev Damp",
  "H.Dly Snd", "H.Rev Snd", "S.Dly Snd", "S.Rev Snd",
  "Harp FX-A", "Harp FX-B", "Seq FX-A", "Seq FX-B",
  "Drum FX-A", "Drum FX-B"
};
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

/* safe_atomic_store — clamp-then-store, rejects NaN/Inf.
 * [IDM-OPT3] Returns the clamped value so call sites can feed txSysex()
 * directly instead of reloading the atomic (saves ~1 atomic load per case,
 * ~20 per encoder tick across updateHardwareParameter). On NaN/Inf the store
 * is skipped and the (unchanged) current value is returned. */
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
enum class ResetScope : uint8_t; /* full def in settings.h [I6] */
void init_fast_gpio();
void initHardwareInterface();
void updateHardwareInterface();
void updateHardwareParameter(uint8_t l1, uint8_t l2, int16_t delta);
void handleFactoryReset();
void handleScopedReset(ResetScope scope); /* [I6] menu-driven scoped reset + restart */
void handleScopedSave(ResetScope scope);  /* [WS10] menu/App scoped save (no restart) */
void handleScopedLoad(ResetScope scope);  /* [LOAD-MENU] menu/App scoped reload (no reboot) */
void updateTaskStackStats();  /* sample uxTaskGetStackHighWaterMark → g_stackStats */
void printInterfaceStats();   /* recurring Serial telemetry (stack + heap)       */

/* Encoder button classifier (preserves delta-cancel guard from v5.1) */
BtnEvent pollEncoderButton(uint32_t now, int32_t delta);

/* Pitch encode/decode: ratio [MASTER_PITCH_MIN, MAX] ↔ v14 semitone-linear ±24 st */
uint16_t encodeMasterPitch(float pitch);
float decodeMasterPitch(uint16_t v14);

#endif /* INTERFACE_H */
