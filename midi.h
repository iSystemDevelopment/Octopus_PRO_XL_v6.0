/* ═════════════════════════════════════════════════════════════════════════════
 * Octopus PRO XL v6.0.00 — Laser Harp Groovebox
 * © 2026 DIODAC ELECTRONICS / iSystem. All Rights Reserved.
 *
 * PROPRIETARY AND CONFIDENTIAL. Unauthorized copying, distribution, modification,
 * or use of this software or firmware, in whole or in part, is strictly prohibited
 * without prior written permission from DIODAC ELECTRONICS.
 * ═════════════════════════════════════════════════════════════════════════════
 * midi.h — v6.0.00  USB MIDI ENGINE  (DIN bus removed)
 *
 * Changes vs v5.2.00:
 *
 *  [M1] CMD table extended: 10 new constants (138–147) for parameters that
 *       had apply/echo functions in patches.h but no wire identity.
 *       WebApp (USB SysEx) can now read/write every controllable parameter.
 *
 *  [M2] Sub-byte 0x01 range updated to 128–147 (was 128–137).
 *       OctopusApp must parse extended frames up to value 19 (= 147-128).
 *
 * v6.0:
 *  [USB-ONLY] DIN UART MIDI removed — single USB transport.  One MidiParserState
 *  (g_usb_parse).  txSysex → USB only; channel-voice MIDI OUT helpers deleted.
 *
 * Retained from v5.2.00:
 *  [2] Sysex frame format: 7 bytes, sub-byte 0x00 (cmd 0–127) or 0x01
 *      (cmd 128–147):  { 0xF0, ID, sub, cmd-base, v14hi, v14lo, 0xF7 }
 *      DIRECTION-TAGGED ID for MIDI-loop immunity:
 *        ID 0x7D = App → device (the ONLY ID parseMidiByte accepts)
 *        ID 0x7C = device → App (all firmware echoes/replies via txSysex)
 *      The firmware ignores its own 0x7C, so a looped-back stream cannot fake
 *      an App heartbeat or re-toggle transport.  App sends 0x7D, listens 0x7C.
 *  [3] TX firewall — per-cmd deduplication + rate limiting.
 *  [4] RX firewall — same for handleSysexCommand.
 *  [5] Full MIDI state machine parser.
 *  [6] Voice owner tracking (harpVoiceOwner / seqVoiceOwner).
 *  [7] Dynamic channels — wireHarpMidiChannel / wireSeqMidiChannel / wireDrumMidiChannel.
 *  [8] No IDF log — all ESP_LOGE/LOGI/LOGW removed.
 *
 * NOTE: globals.h defines allNotesOff() as static inline — midi.cpp does not
 *       redefine it.  patches.h declares recallHarpPatch / recallSeqPatch.
 * ═════════════════════════════════════════════════════════════════════════════ */
#pragma once
#ifndef MIDI_H
#define MIDI_H

#include <Arduino.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include "globals.h"
#include "sysex.h"    /* CMD_* command table + SysEx frame format (wire protocol) */
#include "USBMIDI.h"

/* ── USB MIDI global object (defined in the .ino) ─────────────────────────── */
extern USBMIDI MIDI;

/* ── GM drum map ──────────────────────────────────────────────────────────── */
static const uint8_t GM_DRUM_MAP[8] = { 36, 38, 39, 42, 46, 50, 43, 56 };

/* ── Voice owner tables ────────────────────────────────────────────────────── */
static constexpr int16_t HV_FREE = -1;
static constexpr int16_t HV_PHYS_BASE = 256;
inline int16_t harpVoiceOwner[HARP_POLYPHONY]; /* -1=free, 0-127=midi, ≥256=physical */
inline int16_t seqVoiceOwner[SEQ_POLYPHONY];   /* -1=free, 0-127=midi note number    */

static inline bool hvIsMidi(int16_t o) {
  return (o >= 0 && o <= 127);
}
static inline bool hvIsPhys(int16_t o) {
  return (o >= HV_PHYS_BASE);
}
static inline void harpVoiceOwnerInit() {
  for (int i = 0; i < HARP_POLYPHONY; ++i) harpVoiceOwner[i] = HV_FREE;
  for (int i = 0; i < SEQ_POLYPHONY; ++i) seqVoiceOwner[i] = HV_FREE;
}

/* ── MIDI parser state ────────────────────────────────────────────────────────
 * [USB-ONLY] Single instance in midi.cpp (g_usb_parse) feeding the USB stream.
 * ─────────────────────────────────────────────────────────────────────────── */
struct MidiParserState {
  bool inSysex = false;
  uint8_t sxBuf[256] = {};
  uint16_t sxPtr = 0;
  uint8_t midiStatus = 0;
  uint8_t midiData1 = 0xFFu;
  uint8_t bankMSB = 0;
  uint8_t bankLSB = 0;
};
extern MidiParserState g_usb_parse;

/* ── Per-command TX/RX dedup tables — defined in midi.cpp (file scope) ─── */

/* ── Mutex helpers ───────────────────────────────────────────────────────── */
/* [FIX-LOCK] Timeout raised 1 ms → 5 ms.
 * Root cause of "can't restart play after stop": seq_start()/seq_stop() call
 * txSysex() from ControlPoll (priority 5) while SeqSysexOut (priority 18) is
 * draining STEP_SYNC — holding midiMutex ~0.5 ms per send.  Under Chrome USB
 * backpressure this can stretch to 3-4 ms.  The 1 ms timeout caused transport
 * echoes to be silently dropped; the supervisor self-heals at 600 ms which felt
 * like "play button frozen".  5 ms is still safe: audio task never calls
 * txSysex directly (uses seq_ext ring), so no audio-task stall risk.          */
static inline bool IRAM_ATTR midiLock() {
  return (midiMutex && xSemaphoreTake(midiMutex, pdMS_TO_TICKS(5)) == pdTRUE);
}
static inline void IRAM_ATTR midiUnlock() {
  if (midiMutex) xSemaphoreGive(midiMutex);
}

/* ── CC / pitch conversion helpers ──────────────────────────────────────── */
static inline uint16_t IRAM_ATTR cc7_to_v14_uni(uint8_t v7) {
  /* [MIDI-OPT2] 16383 = 129×127 exactly → multiply replaces /127 division. */
  return (uint16_t)((uint32_t)(v7 & 0x7Fu) * 129u);
}
static inline uint16_t IRAM_ATTR cc7_to_v14_bi(uint8_t v7) {
  v7 &= 0x7Fu;
  /* Bipolar 7-bit → 14-bit mapping.  Center (unity) = v7=64 → 8192.
   * [FIX-CC-BI] Both halves now use the same linear interpolation formula so
   * the mapping is perfectly symmetric:
   *   lower: v7  0..63  →  0..8191  (was v7*128 = 0..8064, 128-unit gap at center)
   *   upper: v7 64..127 →  8192..16383
   * The old lower formula (v7<<7) produced a dead zone of 128 units between
   * the maximum negative (8064) and the center (8192), making the center of
   * a hardware pitch-bend wheel noticeably non-zero in the App.             */
  if (v7 < 64) return (uint16_t)((uint32_t)v7 * 8191u / 63u);
  return (uint16_t)(8192u + ((uint32_t)(v7 - 64u) * 8191u / 63u));
}
static inline uint8_t clamp_midi_channel(uint16_t v) {
  return (uint8_t)std::min<uint16_t>(16u, std::max<uint16_t>(1u, v));
}

/* encodeMasterPitch / decodeMasterPitch defined in interface.cpp */

/* ── Public API ──────────────────────────────────────────────────────────── */
bool init_midi_hardware();
void midi_usb_event_task(void* pvParameters);
void handleSysexCommand(uint8_t cmd, uint16_t v14);
void parseMidiByte(uint8_t b, MidiParserState& ps);
void wireRecordInputNote(uint8_t channel, uint8_t note, uint8_t vel);
void sendFullStateSync();
void txSysex(uint8_t cmd, uint16_t v14bit);
/* [BLOB] One-shot full-preset echo (engine 0=harp, 1=seq) — all 16 params in a
 * single SysEx.  Called from recallHarpPatch/recallSeqPatch (patches.h) so every
 * preset recall, from any source, updates the App's knobs atomically. */
void txPatchBlob(uint8_t engine);
/* [FIX-GRID-ENC] Lossless grid-row echo using SX_SUB_GRID_ROW (sub 0x05).
 * Replaces the old txSysex(CMD_GRID_ROW_LO/HI, pageAddr|byte) which corrupted
 * step bits whenever page>0 and lo/hi had bits 4-5 set (page bits ORed into the
 * byte field, then decoded as part of the step mask).  New frame packs bank/row/
 * page/lo/hi into dedicated byte positions with no overlap.                    */
void txGridRow(uint8_t bank, uint8_t row, uint8_t page, uint8_t lo, uint8_t hi);
/* [USER-SLOTS] Echo sparse user-slot / user-pattern names to the App (sub 0x03/0x04). */
void txUserLibraryNames();
/* [WS5] FX preset recall echo — push the *resulting* param values to the App so
 * its knobs follow an FX preset change from ANY source (hardware menu, App
 * dropdown, MIDI/recall).  Mirrors the patch-blob fix for synth presets:
 *   txMasterFxParams()      — 8 master knobs (tube DRV/TONE/MIX, DJ FQ/RES/MIX, EQ L/H)
 *   txInsertFxSends(engine) — harp(0)/seq(1) insert dly+rev send knobs
 *   txDrumFxSends()         — drum bus dly/rev sends (CMD_D_DLY/CMD_D_REV).
 * [DRUM-FX-SYNC] loadDrumInsert now publishes a drum FX preset's sends through
 * the drumDlySend/drumRevSend atomics (the drum bus reads those, not the insert
 * field), so this echo moves the correct App knobs — the old "field split" that
 * blocked the drum echo is resolved. */
void txMasterFxParams();
void txInsertFxSends(uint8_t engine);
void txDrumFxSends();
void echoDrumInsertParams();
/* [USB-ONLY] External channel-voice MIDI OUT (txMidiNoteOn/Off/CC, txMidiRealtime)
 * was removed with the DIN bus; outboard MIDI OUT returns via the WiFi/BLE coproc. */

#endif /* MIDI_H */
