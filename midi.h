/* ═════════════════════════════════════════════════════════════════════════════
 * Octopus PRO XL v6.1.01 — Laser Harp Groovebox
 * © 2026 DIODAC ELECTRONICS / iSystem. All Rights Reserved.
 *
 * PROPRIETARY AND CONFIDENTIAL. Unauthorized copying, distribution, modification,
 * or use of this software or firmware, in whole or in part, is strictly prohibited
 * without prior written permission from DIODAC ELECTRONICS.
 * ═════════════════════════════════════════════════════════════════════════════
 * midi.h — v6.1.01  USB MIDI I/O + SYSEX DISPATCH API
 *
 * SysEx command IDs and frame format: sysex.h (OctopusApp CMD map must match).
 * USB-only: g_usb_parse → parseMidiByte → handleSysexCommand.
 * Full state: sendFullStateSync(); echoes via txSysex() / txPatchBlob().
 * ═════════════════════════════════════════════════════════════════════════════ */
#pragma once
#ifndef MIDI_H
#define MIDI_H

#include <Arduino.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include "globals.h"
#include "sysex.h"   
#include "USBMIDI.h"

/* ── USB MIDI global object (defined in the .ino) ─────────────────────────── */
extern USBMIDI MIDI;

/* ── GM drum map ──────────────────────────────────────────────────────────── */
static const uint8_t GM_DRUM_MAP[8] = { 36, 38, 39, 42, 46, 50, 43, 56 };

/* ── Voice owner tables ────────────────────────────────────────────────────── */
static constexpr int16_t HV_FREE = -1;
static constexpr int16_t HV_PHYS_BASE = 256;
inline int16_t harpVoiceOwner[HARP_POLYPHONY]; 
inline int16_t seqVoiceOwner[SEQ_POLYPHONY];   

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

/* USB parser state — single instance g_usb_parse in midi.cpp (USB stream only). */
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


/* ── Mutex helpers ───────────────────────────────────────────────────────── */
static inline bool IRAM_ATTR midiLock() {
  return (midiMutex && xSemaphoreTake(midiMutex, pdMS_TO_TICKS(1)) == pdTRUE);
}
static inline void IRAM_ATTR midiUnlock() {
  if (midiMutex) xSemaphoreGive(midiMutex);
}

/* ── CC / pitch conversion helpers ──────────────────────────────────────── */
static inline uint16_t IRAM_ATTR cc7_to_v14_uni(uint8_t v7) {
  /* 16383 = 129×127 — multiply avoids /127 division. */
  return (uint16_t)((uint32_t)(v7 & 0x7Fu) * 129u);
}
static inline uint16_t IRAM_ATTR cc7_to_v14_bi(uint8_t v7) {
  v7 &= 0x7Fu;
  if (v7 < 64) return (uint16_t)((uint32_t)v7 << 7);
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

void txPatchBlob(uint8_t engine);
void txGridRowBlob(uint8_t bank, uint8_t row, uint8_t page, uint8_t lo, uint8_t hi);

void txUserLibraryNames();

void txMasterFxParams();
void txInsertFxSends(uint8_t engine);
void txDrumFxSends();
void echoDrumInsertParams();

#endif /* MIDI_H */
