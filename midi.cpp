/* ═════════════════════════════════════════════════════════════════════════════
 * Octopus PRO XL v6.0.00 — Laser Harp Groovebox
 * © 2026 DIODAC ELECTRONICS / iSystem. All Rights Reserved.
 *
 * PROPRIETARY AND CONFIDENTIAL. Unauthorized copying, distribution, modification,
 * or use of this software or firmware, in whole or in part, is strictly prohibited
 * without prior written permission from DIODAC ELECTRONICS.
 * ═════════════════════════════════════════════════════════════════════════════
 * midi.cpp — v6.0.00  USB MIDI ENGINE  (DIN bus removed)
 *
 * Changes vs v5.1.01:
 *
 *  [R1] ROOT BUG FIX — handleSysexCommand cmd 0–63 now calls applyHarpParam /
 *       applySeqParam / applyDrumParam instead of writing livePatch[] directly.
 *       Previous code updated livePatch[] but left atomic mirrors stale, so the
 *       display, settings save, and interface.cpp always showed the old value
 *       after a WebApp parameter change.  The encoder path (mutateSynth) was
 *       already correct; the WebApp path was not.  Fixes a fundamental
 *       bidirectional sync inconsistency present since v5.0.02.
 *
 *  [R2] CMD_HLFO_RT / CMD_SLFO_RT route through applyHarpParam / applySeqParam
 *       for the same reason.  Atomic + livePatch updated atomically.
 *
 *  [R3] CMD_S_PATCH now calls recallSeqPatch() from patches.h, which updates
 *       seqLivePatch, all seq atomics, version bump, and display dirty in one
 *       call.  The old inline expand-then-syncPatchesToAudio left seq atomics stale.
 *
 *  [R4] handleControlChange setP lambda: uses applyHarpParam / applySeqParam.
 *       Removes the redundant syncPatchesToAudio() call from the CC path.
 *
 *  [R5] handleProgramChange seq path: uses recallSeqPatch().
 *
 *  [R6] 10 new CMD cases added to handleSysexCommand switch (CMD 138–147):
 *       CMD_SEQ_CHAIN, CMD_H/S/D_MUTE, CMD_AUX_DLY_FB, CMD_AUX_REV_DMP,
 *       CMD_DRUM_WAVE, CMD_DB_ROUTE.
 *
 *  [R7] P-lock automation extended to cover three new automatable extended CMDs:
 *       CMD_AUX_DLY_FB, CMD_AUX_REV_DMP, CMD_DRUM_WAVE.
 *  [PLOCK] captureMotionParam on apply* + handleSysexCommand; PARAM_TABLE gate;
 *       64-step lanes (MOTION_STEPS_PER_LANE); RX throttle off while recording.
 *
 *  [R8] sendFullStateSync calls echoFullSeqState() at the end — closes 12
 *       previously missing echoes (BPM, chain, length, transpose, octaves,
 *       mutes, seq patch, FX B-slots, aux params, drum waves, D-BEAM routing).
 *
 *  [R9] wires.h include removed — WebApp authority logic now lives in patches.h.
 *
 *  [R11] CMD_DRUM_WAVE handler: s_drumBodyWaveIdx → drumWaveIdx (renamed in
 *        globals.h v5.3 §10 where it belongs alongside other drum atomics).
 *       dbeam.h added explicitly — midi.cpp calls fire_tuned_drum (groovebox.cpp)
 *       and applyDbeamRouteHW (dbeam.h) directly.
 *
 *  [R10] init_midi_firewall removed v6.0 (throttled TX queue was unused).
 *
 * v6.0:
 *  [USB-ONLY] DIN UART MIDI removed — single USB transport (OctopusApp SysEx +
 *  instrument play-in).  Channel-voice MIDI OUT (txMidiNoteOn/Off/CC) and
 *  txMidiRealtime deleted; D-BEAM no longer emits MIDI.  Single g_usb_parse state.
 * ═════════════════════════════════════════════════════════════════════════════ */
#include "midi.h"
#include "interface.h"   /* encodeMasterPitch / decodeMasterPitch */
#include "effect.h"      /* FxChain fx, loadHarpFx etc.           */
#include "harp.h"        /* [HARP-SPLIT] harpMidiNoteOn/Off (laser harp) */
#include "dbeam.h"       /* applyDbeamRouteHW (D-BEAM local route) */
#include "groovebox.h"   /* [GB-MERGE] fire_tuned_drum, trigger_seq_note,
                          * seq_start/stop, setSequencerBpm,
                          * loadFactorySynthPattern / DrumPattern */
#include "patches.h"
#include "settings.h"     /* applyHarpParam, recallHarpPatch, …    */
/* wires.h removed — authority logic absorbed into patches.h [R9] */

/* [FIX-C1] Async LOAD task — runs on 16 KB stack so the full NVS blob read
 * (patterns + banks, 32+ KB data) does not overflow the 8 KB MidiUsbRx stack.
 * Mirrors the reset_persist_task pattern in interface.cpp.                    */
struct NvsLoadCtx { uint8_t scope; };
static void nvs_load_task(void* p) {
  auto* c = static_cast<NvsLoadCtx*>(p);
  /* [FIX-C3] Check return value; send ACK 16383 or NACK 0 to the App.        */
  const bool ok = settings_load_scoped((ResetScope)c->scope);
  if (ok)
    for (int i = 0; i < MAX_STRINGS; ++i)
      computeHardwareDACThreshold(i, 0.f); /* [FIX-H2] recompute DAC after load */
  sendFullStateSync();
  echoSongState();
  txSysex(CMD_SESSION_LOAD, ok ? 16383u : 0u);
  delete c;
  vTaskDelete(nullptr);
}

/* [MIDI-OPT5] Dedup/rate-limit tables — private to this TU (was inline in midi.h). */
static uint32_t s_last_rx_ms[256]  = {};
static uint16_t s_last_rx_val[256] = {};
static uint32_t s_last_tx_ms[256]  = {};
static uint16_t s_last_tx_val[256] = {};

/* [LINK-HEAL] Yield every N outbound frames during sendFullStateSync so
 * MidiUsbRx / SeqSysexOut are not mutex-starved for hundreds of ms.          */
static bool     s_txSyncYield      = false;
static uint8_t  s_txSyncYieldCount = 0;
static TaskHandle_t hFullSyncTask  = nullptr;
static std::atomic<bool> s_fspEchoSong{ false };
static std::atomic<bool> s_fspSendAck{ false };

void requestFullStateSync(bool echoSongAfter, bool sendSyncAck) {
  s_fspEchoSong.store(echoSongAfter, std::memory_order_relaxed);
  s_fspSendAck.store(sendSyncAck, std::memory_order_relaxed);
  if (hFullSyncTask)
    xTaskNotifyGive(hFullSyncTask);
}

static inline void txSyncYieldTick() {
  if (s_txSyncYield && ++s_txSyncYieldCount >= 8u) {
    s_txSyncYieldCount = 0u;
    vTaskDelay(pdMS_TO_TICKS(1));
  }
}

/* ── USB MIDI parser state ──────────────────────────────────────────────────
 * [USB-ONLY] DIN UART MIDI was removed (v6.0): the only MIDI transport is now
 * USB, carrying OctopusApp 0x7C/0x7D SysEx control + optional instrument play-in.
 * External channel-voice MIDI OUT (notes/CC to outboard gear) is deferred to the
 * planned WiFi/BLE coprocessor and is intentionally absent here.                */
MidiParserState g_usb_parse;

/* ─────────────────────────────────────────────────────────────────────────────
 * midiEmitRaw — sends n bytes to USB MIDI.  Caller must hold midiMutex.
 * Used only by txSysex (App sync/control); channel-voice TX was removed.
 * ─────────────────────────────────────────────────────────────────────────────*/
static void IRAM_ATTR midiEmitRaw(const uint8_t* msg, uint8_t n) {
  /* [ECHO-FIX] Pack the complete SysEx byte-stream into USB-MIDI event packets
   * with the CORRECT Code Index Numbers so the host reassembles it into ONE
   * message:
   *   CIN 0x4 → SysEx start/continue (3 bytes)
   *   CIN 0x7 → SysEx ends with 3 bytes
   *   CIN 0x6 → SysEx ends with 2 bytes
   *   CIN 0x5 → SysEx ends with 1 byte
   * The previous code emitted every byte as USBMIDI::write() (CIN 0x0F, single
   * byte).  TinyUSB sends that verbatim, but Chrome Web MIDI does NOT treat
   * 0x0F packets as part of a SysEx — it delivered seven 1-byte messages, which
   * the App rejects (it requires data.length >= 7).  Result: NO device→App echo
   * (BPM / transport / step-sync / CPU all dead) while App→device still worked.
   * Cable number 0 → header high nibble 0, so header == CIN.                  */
  uint8_t i = 0;
  while ((uint8_t)(n - i) > 3u) {
    midiEventPacket_t p = { 0x04u, msg[i], msg[i + 1u], msg[i + 2u] };
    MIDI.writePacket(&p);
    i += 3u;
  }
  const uint8_t rem = (uint8_t)(n - i);
  if (rem == 0u) return;
  midiEventPacket_t p = { 0u, 0u, 0u, 0u };
  if (rem == 3u)      { p.header = 0x07u; p.byte1 = msg[i]; p.byte2 = msg[i + 1u]; p.byte3 = msg[i + 2u]; }
  else if (rem == 2u) { p.header = 0x06u; p.byte1 = msg[i]; p.byte2 = msg[i + 1u]; }
  else                { p.header = 0x05u; p.byte1 = msg[i]; }
  MIDI.writePacket(&p);
}

/* ─────────────────────────────────────────────────────────────────────────────
 * TX helpers
 * ─────────────────────────────────────────────────────────────────────────────*/
void txSysex(uint8_t cmd, uint16_t v14bit) {
  /* [MIDI-OPT1] Fast exit when no App is listening — avoids ~64 mutex
   * acquisitions/sec during standalone sequencer playback. */
  if (!isAppConnected()) return;

  if (v14bit > 16383u) v14bit = 16383u;

  const uint32_t now = millis();
  /* Deduplication: don't re-send the same value within 500 ms.
   * STEP_SYNC / SONG_POS are clock position — never dedup (playhead stalls if
   * the same step index repeats within 500 ms at slow tempos / short lengths). */
  if (cmd != CMD_STEP_SYNC && cmd != CMD_SONG_POS) {
    if (s_last_tx_val[cmd] == v14bit && (now - s_last_tx_ms[cmd] < 500u)) return;
  }
  /* Rate limiting for fast-changing parameter groups */
  if (cmd < 90u || (cmd >= 112u && cmd <= 120u)) {
    if (now - s_last_tx_ms[cmd] < 10u) return;
  }
  /* [ECHO-FIX] Stamp dedup/rate-limit state ONLY after a successful send.
   * Stamping before the lock meant a failed midiLock() recorded a phantom send,
   * so the retry of the same value got deduped → silent permanent echo loss. */
  if (!midiLock()) return;
  const uint8_t v14hi = (uint8_t)((v14bit >> 7) & 0x7Fu);
  const uint8_t v14lo = (uint8_t)(v14bit & 0x7Fu);
  /* Direction-tagged manufacturer ID for MIDI-loop immunity:
   *   0x7C = device → App (these echoes/replies)
   *   0x7D = App → device (accepted by parseMidiByte)
   * The RX parser accepts ONLY 0x7D, so the firmware ignores its own 0x7C
   * echoes if MIDI OUT is ever looped back to IN (USB-MIDI loopback / dongle).
   * Without this, looped echoes faked an "App connected" heartbeat and could
   * re-toggle transport.  The App listens for 0x7C and still sends 0x7D.        */
  if (cmd <= 127u) {
    const uint8_t f[7] = { 0xF0u, 0x7Cu, 0x00u, cmd, v14hi, v14lo, 0xF7u };
    midiEmitRaw(f, 7);
  } else {
    /* Extended: sub-byte 0x01, wire value = cmd - 128 (0–36 for cmds 128–164) */
    const uint8_t f[7] = { 0xF0u, 0x7Cu, 0x01u, (uint8_t)(cmd - 128u), v14hi, v14lo, 0xF7u };
    midiEmitRaw(f, 7);
  }
  midiUnlock();

  s_last_tx_ms[cmd]  = now;
  s_last_tx_val[cmd] = v14bit;

  txSyncYieldTick();
}

/* ─────────────────────────────────────────────────────────────────────────────
 * txPatchBlob — [BLOB] one atomic SysEx carrying a full 16-param preset.
 *
 * Frame: { 0xF0, 0x7C, SX_SUB_PATCH_BLOB, engine, p0hi,p0lo … p15hi,p15lo, 0xF7 }.
 * Replaces the old 16-message per-param echo on preset recall: the App's knobs
 * now follow a preset change from ANY source (hardware menu, MIDI program-change,
 * or App) in a single, ordered, glitch-free transfer.  The 16-param values are
 * snapshotted under patchMux (the same tiny critical section the recall uses) so
 * the audio core never stalls.  Skipped when no App is listening — a standalone
 * device wastes no bus on blobs nobody reads.  No dedup: a blob is always sent in
 * full (it represents a discrete "load this preset" event, not a streamed knob). */
/* [FIX-GRID-ENC] txGridRow — lossless 10-byte SysEx for one page-half-pair of a row.
 * Sends lo (steps 0-7) and hi (steps 8-15) of the given page in one frame, with
 * each 8-bit byte split across two 4-bit nibbles so all frame bytes are valid MIDI
 * data bytes (< 0x80).  No field overlaps — bank/row/page/lo/hi are independent. */
void txGridRow(uint8_t bank, uint8_t row, uint8_t page, uint8_t lo, uint8_t hi) {
  if (!isAppConnected()) return;
  if (!midiLock()) return;
  const uint8_t f[10] = {
    0xF0u, 0x7Cu, SX_SUB_GRID_ROW,
    (uint8_t)((bank << 4) | (row & 15u)),  /* bank_row: bank 0-3, row 0-15      */
    (uint8_t)(page & 3u),                   /* page 0-3                          */
    (uint8_t)(lo & 0x0Fu),                  /* lo lower nibble (steps 0-3)       */
    (uint8_t)((lo >> 4) & 0x0Fu),           /* lo upper nibble (steps 4-7)       */
    (uint8_t)(hi & 0x0Fu),                  /* hi lower nibble (steps 8-11)      */
    (uint8_t)((hi >> 4) & 0x0Fu),           /* hi upper nibble (steps 12-15)     */
    0xF7u
  };
  midiEmitRaw(f, 10);
  midiUnlock();
  txSyncYieldTick();
}

void txPatchBlob(uint8_t engine) {
  if (engine > 1u) return;
  if (!isAppConnected()) return;          /* nobody to receive it — skip */

  uint16_t snap[PARAMS_PER_PRESET];
  portENTER_CRITICAL(&patchMux);
  memcpy(snap, engine == 0u ? harpLivePatch : seqLivePatch, sizeof(snap));
  portEXIT_CRITICAL(&patchMux);

  if (!midiLock()) return;
  uint8_t f[5 + PARAMS_PER_PRESET * 2];
  uint8_t n = 0;
  f[n++] = 0xF0u;
  f[n++] = 0x7Cu;                          /* device → App */
  f[n++] = SX_SUB_PATCH_BLOB;
  f[n++] = engine;
  for (uint8_t i = 0; i < PARAMS_PER_PRESET; ++i) {
    f[n++] = (uint8_t)((snap[i] >> 7) & 0x7Fu);
    f[n++] = (uint8_t)(snap[i] & 0x7Fu);
  }
  f[n++] = 0xF7u;
  midiEmitRaw(f, n);
  midiUnlock();
  txSyncYieldTick();
}

/* [USER-SLOTS] Echo a user sound-slot or user-pattern name to the App (sub 0x03/0x04).
 * Only custom names are sent (generic USER/PAT labels are generated client-side). */
static void txUserSoundNameBlob(uint8_t engine, uint8_t slot) {
  if (engine > 1u || slot >= NUM_USER_SLOTS) return;
  if (!g_userSlotName[engine][slot][0]) return;
  if (!isAppConnected() || !midiLock()) return;
  uint8_t f[21];
  uint8_t n = 0;
  f[n++] = 0xF0u;
  f[n++] = 0x7Cu;
  f[n++] = SX_SUB_USR_SOUND_NAME;
  f[n++] = engine & 1u;
  f[n++] = slot & 63u;
  for (uint8_t i = 0; i < 15u; ++i)
    f[n++] = (uint8_t)(g_userSlotName[engine][slot][i] & 0x7Fu);
  f[n++] = 0xF7u;
  midiEmitRaw(f, n);
  midiUnlock();
}

static void txUserPatNameBlob(uint8_t slot) {
  if (slot >= NUM_USER_PAT_SLOTS) return;
  if (!g_userPatName[slot][0]) return;
  if (!isAppConnected() || !midiLock()) return;
  uint8_t f[20];
  uint8_t n = 0;
  f[n++] = 0xF0u;
  f[n++] = 0x7Cu;
  f[n++] = SX_SUB_USR_PAT_NAME;
  f[n++] = slot & 63u;
  for (uint8_t i = 0; i < 15u; ++i)
    f[n++] = (uint8_t)(g_userPatName[slot][i] & 0x7Fu);
  f[n++] = 0xF7u;
  midiEmitRaw(f, n);
  midiUnlock();
}

void txUserLibraryNames() {
  if (!isAppConnected()) return;
  for (uint8_t eng = 0; eng < 2u; ++eng)
    for (uint8_t i = 0; i < NUM_USER_SLOTS; ++i)
      if (g_userSlotName[eng][i][0]) txUserSoundNameBlob(eng, i);
  for (uint8_t i = 0; i < NUM_USER_PAT_SLOTS; ++i) {
    if (g_userPatName[i][0]) txUserPatNameBlob(i);
    if (g_userPat[i].flags & 1u) txSysex(CMD_USR_PAT_SAVE, (uint16_t)i);
  }
}

void txMasterFxParams() {
  if (!isAppConnected()) return;
  auto e01 = [](float v) -> uint16_t {
    return (uint16_t)(std::min(1.0f, std::max(0.0f, v)) * 16383.0f);
  };
  auto eEq = [](float db) -> uint16_t {
    return (uint16_t)(((std::min(12.0f, std::max(-12.0f, db)) + 12.0f) / 24.0f) * 16383.0f);
  };
  txSysex(CMD_TB_DRV,  e01(tbDrive.load(std::memory_order_relaxed)));
  txSysex(CMD_TB_TONE, e01(tbTone .load(std::memory_order_relaxed)));
  txSysex(CMD_TB_MIX,  e01(tbMix  .load(std::memory_order_relaxed)));
  txSysex(CMD_DJ_FQ,   e01(djFreq .load(std::memory_order_relaxed)));
  txSysex(CMD_DJ_RES,  e01(djRes  .load(std::memory_order_relaxed)));
  txSysex(CMD_DJ_MIX,  e01(djMix  .load(std::memory_order_relaxed)));
  txSysex(CMD_EQ_L,    eEq(masterEqLow .load(std::memory_order_relaxed)));
  txSysex(CMD_EQ_H,    eEq(masterEqHigh.load(std::memory_order_relaxed)));
}

void txInsertFxSends(uint8_t engine) {
  if (engine > 1u) return;
  if (!isAppConnected()) return;
  float dly, rev;
  portENTER_CRITICAL(&patchMux);
  if (engine == 0u) { dly = fx.harpInsert.dly_send; rev = fx.harpInsert.rev_send; }
  else              { dly = fx.seqInsert .dly_send; rev = fx.seqInsert .rev_send; }
  portEXIT_CRITICAL(&patchMux);
  const uint16_t vDly = (uint16_t)(std::min(1.0f, std::max(0.0f, dly)) * 16383.0f);
  const uint16_t vRev = (uint16_t)(std::min(1.0f, std::max(0.0f, rev)) * 16383.0f);
  txSysex(engine == 0u ? CMD_H_DLY_MIX : CMD_S_DLY_MIX, vDly);
  txSysex(engine == 0u ? CMD_H_REV_MIX : CMD_S_REV_MIX, vRev);
}

/* [DRUM-FX-SYNC] Echo the drum bus dly/rev sends to the App after a drum FX
 * preset recall.  The drum sends live in their own atomics (drumDlySend/
 * drumRevSend, CMD_D_DLY/CMD_D_REV) — loadDrumInsert now publishes the preset's
 * sends through them, so reading the atomics here mirrors the harp/seq path. */
void txDrumFxSends() {
  if (!isAppConnected()) return;
  const uint16_t vDly = (uint16_t)(std::min(1.0f, std::max(0.0f,
                          drumDlySend.load(std::memory_order_relaxed))) * 16383.0f);
  const uint16_t vRev = (uint16_t)(std::min(1.0f, std::max(0.0f,
                          drumRevSend.load(std::memory_order_relaxed))) * 16383.0f);
  txSysex(CMD_D_DLY, vDly);
  txSysex(CMD_D_REV, vRev);
}

/* Echo drum insert-A live params (WET / P1 / P2) after preset recall or tweak. */
void echoDrumInsertParams() {
  if (!isAppConnected()) return;
  float wet, p1, p2;
  portENTER_CRITICAL(&patchMux);
  wet = fx.drumInsert.fx.fx_mix;
  p1  = fx.drumInsert.fx.p1;
  p2  = fx.drumInsert.fx.p2;
  portEXIT_CRITICAL(&patchMux);
  txSysex(CMD_D_FX_WET, (uint16_t)(std::min(1.f, wet) * 16383.f));
  txSysex(CMD_D_FX_P1,  (uint16_t)(std::min(1.f, p1 / 30.f) * 16383.f));
  txSysex(CMD_D_FX_P2,  (uint16_t)(std::min(1.f, p2 / 250.f) * 16383.f));
}

/* [USB-ONLY] External channel-voice MIDI OUT (txMidiNoteOn/Off/CC, txMidiRealtime)
 * was removed with the DIN bus.  The sequencer/drums and D-BEAM drive the internal
 * engines directly; outboard MIDI OUT will return via the WiFi/BLE coprocessor.   */

/* ─────────────────────────────────────────────────────────────────────────────
 * Note handlers
 * ─────────────────────────────────────────────────────────────────────────────*/
/* [HARP-SPLIT] MIDI keyboard path delegates to the dedicated harp engine. */
static void IRAM_ATTR noteOnSeq(uint8_t note, uint8_t velocity) {
  const int si = harpScaleIndex.load(std::memory_order_relaxed) & (NUM_SCALES - 1);
  /* [FIX-NOTE-SEQ] Old code: (note & 7) % numStrings — used lower 3 bits of the
   * MIDI note number as a string index.  This gave non-musical results: pressing
   * C on any octave always triggered string 0, C# string 1, etc., with no regard
   * for pitch proximity.  Octave changes had no effect on string selection.
   * New code: nearest-pitch matching across all octave transpositions of each
   * string's scale note.  Playing C4 on a keyboard finds whichever string's note
   * is closest in semitones to C4, matching musician expectation for a scale-mapped
   * instrument.  Exact matches short-circuit the search immediately.            */
  uint8_t scaleNote = note;                        /* chromatic fallback */
  if (SCALES[si].notes) {
    const int nStr = std::max(1, SCALES[si].numActiveStrings);
    int bestIdx = 0, bestDist = 999;
    for (int r = 0; r < nStr; ++r) {
      const int base = (int)SCALES[si].notes[r];
      for (int oct = -4; oct <= 4; ++oct) {
        const int d = std::abs((int)note - (base + oct * 12));
        if (d < bestDist) { bestDist = d; bestIdx = r; }
        if (d == 0) { r = nStr; break; }  /* exact match — stop searching */
      }
    }
    scaleNote = (uint8_t)SCALES[si].notes[bestIdx];
  }

  const int cap = std::min(std::max(1, (int)g_seq_voice_cap.load(std::memory_order_relaxed)),
                           (int)SEQ_POLYPHONY);
  int voice = -1;
  portENTER_CRITICAL(&patchMux);
  for (int v = 0; v < cap; ++v) {
    if (!seqVoices[v].active.load(std::memory_order_relaxed)) { voice = v; break; }
  }
  if (voice < 0) voice = scaleNote % cap;
  seqVoiceOwner[voice] = (int16_t)note;
  portEXIT_CRITICAL(&patchMux);
  trigger_seq_note(voice, midi_note_to_freq(scaleNote),
                   /* [MIDI-OPT7] /127u per note-on — event-rate, not hot-path */
                   (uint16_t)((uint32_t)velocity * (uint32_t)Q15_ONE / 127u));
}

static void IRAM_ATTR noteOffSeq(uint8_t note) {
  portENTER_CRITICAL(&patchMux);
  for (int v = 0; v < SEQ_POLYPHONY; ++v) {
    if (seqVoiceOwner[v] == (int16_t)note) {
      release_seq_note(v);
      seqVoiceOwner[v] = HV_FREE;
    }
  }
  portEXIT_CRITICAL(&patchMux);
}

static void IRAM_ATTR noteOnDrums(uint8_t note, uint8_t velocity) {
  const float vn = (float)velocity / 127.0f;
  for (int i = 0; i < 8; ++i) {
    if (GM_DRUM_MAP[i] == note) { fire_tuned_drum(i, vn); return; }
  }
  fire_tuned_drum((int)(note % 8), vn);
}
static void IRAM_ATTR noteOffDrums(uint8_t /*note*/) { /* one-shot voices */ }

/* ─────────────────────────────────────────────────────────────────────────────
 * handleControlChange
 *
 * [R4] setP lambda now calls applyHarpParam / applySeqParam so atomics and
 *      livePatch stay in sync via the canonical patches.h path.
 * ─────────────────────────────────────────────────────────────────────────────*/
static void IRAM_ATTR handleControlChange(uint8_t channel, uint8_t cc, uint8_t value) {
  const uint8_t harpCh = wireHarpMidiChannel.load(std::memory_order_relaxed);
  const uint8_t seqCh  = wireSeqMidiChannel .load(std::memory_order_relaxed);
  const uint8_t drumCh = wireDrumMidiChannel.load(std::memory_order_relaxed);

  switch (cc) {
    /* CC 0 (Bank MSB) and CC 32 (Bank LSB) are intercepted in parseMidiByte
     * before this function is called — they never arrive here. [MIDI-BUG1] */
    case 120:
    case 123: allNotesOff(); return;
    case 121: return;
    default: break;
  }

  if (channel == drumCh) {
    const uint16_t v14 = cc7_to_v14_uni(value);
    switch (cc) {
      case 7:  mixDrumsVol .store(v14 / 16383.f); txSysex(CMD_D_VOL, v14); break;
      case 70: /* Sound Variation → live drum-kit select (value zones → 0-3) */
        applyDrumKit((uint8_t)(value >> 5), true);
        break;
      case 91: drumRevSend .store(v14 / 16383.f); txSysex(CMD_D_REV, v14); break;
      case 93: drumDlySend .store(v14 / 16383.f); txSysex(CMD_D_DLY, v14); break;
      default: break;
    }
    return;
  }

  const bool isHarp = (channel == harpCh);
  if (!isHarp && channel != seqCh) return;

  const uint16_t v14 = cc7_to_v14_uni(value);

  /* [R4] Correct both livePatch and atomic via applyHarpParam / applySeqParam */
  auto setP = [&](int idx) {
    const uint8_t cmd = isHarp ? (uint8_t)(CMD_H_WAVE + idx)
                               : (uint8_t)(CMD_S_WAVE + idx);
    if (isHarp) applyHarpParam(idx, v14);
    else        applySeqParam(idx, v14);
    if (checkWireAuthority(cmd, false))
      txSysex(cmd, v14);
  };

  switch (cc) {
    case 1:  setP((int)SynthParam::P_LFO_DEPTH); break;
    case 7:
      { auto& vol = isHarp ? mixHarpVol : mixSeqVol;
        vol.store(v14 / 16383.f);
        txSysex(isHarp ? CMD_H_VOL : CMD_S_VOL, v14); }
      break;
    case 71: setP((int)SynthParam::P_RESONANCE); break;
    case 72: setP((int)SynthParam::P_RELEASE);   break;
    case 73: setP((int)SynthParam::P_ATTACK);    break;
    case 74: setP((int)SynthParam::P_CUTOFF);    break;
    case 75: setP((int)SynthParam::P_DECAY);     break;
    case 76: setP((int)SynthParam::P_LFO_RATE);  break;
    /* [FIX-CC91-93] Harp/seq CC91/CC93 were swapped vs the drum channel:
     *   Old: CC91→dly_send, CC93→rev_send  (wrong — inverted from standard)
     *   New: CC91→rev_send, CC93→dly_send  (matches drums AND MIDI standard)
     * Standard MIDI CC91 = Reverb Send, CC93 = Chorus/Delay Send.  Both are
     * now routed through applyFxSend (the canonical patchMux-guarded path)
     * instead of writing to the FX struct directly, which bypassed consistency
     * checks and could leave the App knobs unsynchronised.                   */
    case 91: /* Reverb Send → rev_send */
      applyFxSend(isHarp ? CMD_H_REV_MIX : CMD_S_REV_MIX, v14, Origin::MIDI);
      break;
    case 93: /* Chorus/Delay Send → dly_send */
      applyFxSend(isHarp ? CMD_H_DLY_MIX : CMD_S_DLY_MIX, v14, Origin::MIDI);
      break;
    default: break;
  }
}

/* ─────────────────────────────────────────────────────────────────────────────
 * handlePitchBend
 * ─────────────────────────────────────────────────────────────────────────────*/
static void IRAM_ATTR handlePitchBend(uint8_t channel, uint16_t pb) {
  if (!pbMapping.enabled.load(std::memory_order_relaxed)) return;
  const int delta = (int)pb - 8192;
  const float range = (delta >= 0)
      ? (float)pbMapping.upSemi.load(std::memory_order_relaxed)
      : (float)pbMapping.downSemi.load(std::memory_order_relaxed);
  const float semis = (delta * (1.0f / 8192.0f)) * range;
  const uint32_t pbq16 = (uint32_t)(exp2f(semis * (1.0f / 12.0f)) * 65536.0f);
  const uint8_t harpCh = wireHarpMidiChannel.load(std::memory_order_relaxed);
  const uint8_t seqCh  = wireSeqMidiChannel .load(std::memory_order_relaxed);
  if (channel == harpCh) harp_synth_g.pitch_bend_q16.store(pbq16, std::memory_order_relaxed);
  if (channel == seqCh)  seq_synth_g .pitch_bend_q16.store(pbq16, std::memory_order_relaxed);
}

/* ─────────────────────────────────────────────────────────────────────────────
 * handleProgramChange
 *
 * [R5] seq path now calls recallSeqPatch() — updates seqLivePatch + atomics.
 * ─────────────────────────────────────────────────────────────────────────────*/
static void IRAM_ATTR handleProgramChange(uint8_t channel, uint8_t patch,
                                           uint8_t bankMSB, uint8_t bankLSB) {
  const int bank = ((bankMSB << 7) | bankLSB) & 0x01;
  const int pnum = std::min(std::max((bank << 7) | (patch & 0x7F), 0), NUM_PATCHES - 1);
  const uint8_t harpCh = wireHarpMidiChannel.load(std::memory_order_relaxed);
  const uint8_t seqCh  = wireSeqMidiChannel .load(std::memory_order_relaxed);
  if (channel == harpCh) recallHarpPatch(pnum, ParamSource::UI);      /* patches.h */
  if (channel == seqCh)  recallSeqPatch (pnum, ParamSource::UI);      /* patches.h [R5] */
}

/* ─────────────────────────────────────────────────────────────────────────────
 * handleSysexCommand — RX firewall + full SSOT dispatch
 *
 * [R1] Param blocks 0–63 now use applyHarpParam / applySeqParam / applyDrumParam
 *      so atomic mirrors stay in sync with livePatch[].
 * [R6] New CMD cases 138–147 added.
 * ─────────────────────────────────────────────────────────────────────────────*/
void handleSysexCommand(uint8_t cmd, uint16_t v14) {
  /* [FIX] Do NOT stamp lastWebSysexMs here.  handleSysexCommand is also invoked
   * INTERNALLY by sequencer motion/P-lock playback (groovebox.cpp) — stamping the
   * heartbeat on those calls faked an "App connected" state the moment Play
   * started a pattern that has recorded motion (no app present).  The heartbeat
   * is refreshed ONLY by the real-input path in parseMidiByte (genuine 0x7D
   * App→device sysex), which is the single authoritative source. */

  /* ── RX firewall ──────────────────────────────────────────────────────
   * Spam filter: drop exact-duplicate values that repeat within a short
   * window (50 ms), but NEVER permanently and NEVER for action commands.
   * Fast continuous parameter groups additionally get a 5 ms minimum interval.
   *
   * Exempt from dedup (always execute):
   *   CMD_GRID_TOG     — fast double-click same cell must toggle ON then OFF;
   *                      dedup traps the second toggle leaving the cell wrong.
   *   CMD_PING         — always needs a reply so the App can detect link health.
   *   CMD_APP_SYNC_REQ — reconnect fires within ms of boot; dedup would drop the
   *                      sync request and leave the App with a blank state.       */
  const uint32_t now = millis();
  const bool noDedup = (cmd == CMD_GRID_TOG ||
                        cmd == CMD_PING      ||
                        cmd == CMD_APP_SYNC_REQ);
  const bool plockRec = seqRecording.load(std::memory_order_relaxed);
  if (!noDedup) {
    if (s_last_rx_val[cmd] == v14 && (now - s_last_rx_ms[cmd] < 50u)) return;
    /* [PLOCK-REC] Skip 5 ms RX throttle while recording — fast App knob drags
     * must not be dropped or automation lanes stay sparse / empty.            */
    if (!plockRec && (cmd < 90u || (cmd >= 112u && cmd <= 120u))) {
      if (now - s_last_rx_ms[cmd] < 5u) return;
    }
  }
  s_last_rx_ms[cmd]  = now;
  s_last_rx_val[cmd] = v14;

  /* ── Synth/drum parameter blocks ─────────────────────────────────────── */
  if (cmd < 16u) {
    applyHarpParam((int)cmd, v14);
    return;
  }
  if (cmd < 32u) {
    applySeqParam((int)(cmd - 16u), v14);
    return;
  }
  if (cmd < 64u) {
    applyDrumParam((int)(cmd - 32u), v14);
    return;
  }

  switch (cmd) {

    /* ── Master / mix / FX scalars + D-BEAM HW config ────────────────────────
     * [P4] All v14↔value scaling lives in applyMasterParam (patches.h) — the
     * single source of truth.  Origin::APP = no echo (the App already knows).  */
    case CMD_M_VOL:   case CMD_H_VOL:   case CMD_S_VOL:   case CMD_D_VOL:
    case CMD_H_PAN:   case CMD_S_PAN:   case CMD_D_PAN:
    case CMD_EQ_L:    case CMD_EQ_H:    case CMD_PITCH: case CMD_DRUM_PITCH:
    case CMD_D_REV:   case CMD_D_DLY:
    case CMD_TB_DRV:  case CMD_TB_TONE: case CMD_TB_MIX:
    case CMD_DJ_FQ:   case CMD_DJ_RES:  case CMD_DJ_MIX:
    case CMD_DB_CURVE: case CMD_DB_OFFSET: case CMD_DB_RANGE: case CMD_DB_ENABLED:
      applyMasterParam(cmd, v14, Origin::APP); break;

    /* ── LFO route — [R2] legacy 86/87 aliases; echo on canonical 11/27 [WS4] */
    case CMD_HLFO_RT:
      applyHarpParam((int)SynthParam::P_LFO_ROUTE, v14);
      txSysex((uint8_t)(CMD_H_WAVE + (int)SynthParam::P_LFO_ROUTE), v14);
      break;
    case CMD_SLFO_RT:
      applySeqParam((int)SynthParam::P_LFO_ROUTE, v14);
      txSysex((uint8_t)(CMD_S_WAVE + (int)SynthParam::P_LFO_ROUTE), v14);
      break;

    /* ── Patch recall ────────────────────────────────────────────────────── */
    case CMD_H_PATCH:
      /* [BLOB] Recall then confirm to the App.  recallHarpPatch now emits the
       * full 16-param patch blob itself (one atomic message), so here we only
       * need the index echo to land the dropdown.  This also closes the old
       * hardware→App gap: a preset changed on the device blobs its params too,
       * because the echo now lives in the recall, not in this App-only path. */
      recallHarpPatch((int)(v14 & 0xFFu), ParamSource::UI);
      txSysex(CMD_H_PATCH, v14 & 0xFFu); /* echo back patch index */
      break;
    case CMD_S_PATCH:
      /* [BLOB] As above — recallSeqPatch emits the seq patch blob; index echo
       * lands the dropdown.  (Old per-param 16-message echo removed.)          */
      recallSeqPatch((int)(v14 & 0xFFu), ParamSource::UI);
      txSysex(CMD_S_PATCH, v14 & 0xFFu); /* echo back patch index */
      break;
    case CMD_H_SCALE: {
      const uint8_t s = (uint8_t)(v14 & 15u);
      harpScaleIndex.store((int)s, std::memory_order_relaxed);
      txSysex(CMD_H_SCALE, (uint16_t)s);
      break;
    }

    /* ── Harp play mode (POLY8 / STRINGS / SOLO) ──────────────────────────── */
    case CMD_PLAY_MODE: {
      const uint8_t pm = (uint8_t)(v14 > 2u ? 0u : v14);
      harpSetPlayMode((PlayMode)pm);     /* flushes notes + marks display dirty  */
      txSysex(CMD_PLAY_MODE, (uint16_t)pm);  /* echo so all clients/HW stay in sync */
      break;
    }

    /* ── FX insert preset indices ─────────────────────────────────────────── */
    /* [WS5] slot-A recalls change the insert send mix → echo so the App's send
     * knobs follow; slot-B (dynamics) has no App knobs, index echo is enough.  */
    case CMD_H_FX_IDX:   loadHarpFx  (v14 & 0x0Fu); harpFxIndex .store((int)(v14 & 0xFu)); txInsertFxSends(0u); break;
    case CMD_H_FX_IDX_B: loadHarpFxB (v14 & 0x0Fu); harpFxIndexB.store((int)(v14 & 0xFu)); txSysex(CMD_H_FX_IDX_B, v14 & 0xFu); break;
    case CMD_S_FX_IDX:   loadSeqFx   (v14 & 0x0Fu); seqFxIndex  .store((int)(v14 & 0xFu)); txInsertFxSends(1u); break;
    case CMD_S_FX_IDX_B: loadSeqFxB  (v14 & 0x0Fu); seqFxIndexB .store((int)(v14 & 0xFu)); txSysex(CMD_S_FX_IDX_B, v14 & 0xFu); break;
    case CMD_D_FX_IDX:   loadDrumFx(v14 & 0x0Fu); drumFxIndexA.store((int)(v14 & 0xFu));
                         txSysex(CMD_D_FX_IDX, v14 & 0xFu); echoDrumInsertParams(); break;
    case CMD_D_FX_IDX_B: loadDrumFxB (v14 & 0x0Fu); drumFxIndexB.store((int)(v14 & 0xFu)); txSysex(CMD_D_FX_IDX_B, v14 & 0xFu); break;
    case CMD_M_FX_IDX:   fx.loadMasterFx((int)(v14 & 0x0Fu)); txMasterFxParams(); break;

    /* ── Aux FX time / size — [P4] master aux atomics via canonical layer ──── */
    case CMD_H_FX_TIME: case CMD_S_FX_TIME:
    case CMD_H_FX_SIZE: case CMD_S_FX_SIZE:
      applyMasterParam(cmd, v14, Origin::APP); break;

    /* ── Per-engine FX insert sends — [P4] patchMux-guarded canonical layer ── */
    case CMD_H_FX_MIX:  case CMD_S_FX_MIX:
    case CMD_H_DLY_MIX: case CMD_H_REV_MIX:
    case CMD_S_DLY_MIX: case CMD_S_REV_MIX:
      applyFxSend(cmd, v14, Origin::APP); break;

    /* ── Sequencer transport ─────────────────────────────────────────────── */
    case CMD_TRIG_MODE:
      if (v14 > 0u) seq_start(); else seq_stop();
      /* seq_start/stop push TRIG_MODE + TRANSPORT via seq_ext ring — no echo here. */
      break;
    case CMD_BPM:
      setSequencerBpm((int32_t)v14);   /* echoes CMD_BPM inside setSequencerBpm */
      break;
    case CMD_BANK:
      applySeqBank((uint8_t)(v14 & 15u));
      break;
    case CMD_TRANSPOSE:
      applySeqTranspose(v14);             /* [PER-PATTERN-TRANSPOSE] writes active cell */
      break;
    case CMD_H_PITCH: {  /* [WS1] harp continuous pitch-bend; v14 8192 = unity */
      const float semis = ((float)v14 - 8192.0f) / 8192.0f * 12.0f; /* ±1 octave */
      harpPitchMult.store(exp2f(semis / 12.0f), std::memory_order_relaxed);
      break;
    }
    case CMD_HW_GATE:
      beamGateHoldMs = std::min<uint32_t>(BEAM_GATE_HOLD_MAX, (uint32_t)v14);
      break;
    case CMD_HW_S_LEN:
      applySeqLength(v14);
      break;
    case CMD_HW_H_OCT:
      applySeqOctaveSet(0, v14);
      break;
    case CMD_HW_S_OCT:
      applySeqOctaveSet(1, v14);
      break;
    case CMD_GRID_TOG: {
      const int trk  = (v14 >> 7) & 0x7Fu;
      const int step = v14 & 0x7Fu;
      /* [GRID-64] trk 0–15, step 0–63 (absolute step index, not page-local). */
      if (trk < 16 && step < 64) {
        portENTER_CRITICAL(&patchMux);
        hwSeqData[seqActiveBank.load() & 15u][seqActiveChain.load() & 3u][trk]
            ^= (1ull << step);
        portEXIT_CRITICAL(&patchMux);
      }
      break;
    }
    /* [G2] Absolute half-row write — legacy v14 format.  Kept for backward compat
     * with clients that still use the old encoding.  New code sends SX_SUB_GRID_ROW
     * (parsed in parseMidiByte) which has no field overlap.
     * [FIX-GRID-DEC] Old encoding: v14 = (bank<<12)|(row<<8)|(page<<4)|byte.
     * Bug: page<<4 occupies bits 4-5, byte occupies bits 0-7 — they OVERLAP.
     * When byte has bits 4-5 set, (v14>>4)&3 reads those bits as page, decoding
     * the wrong page AND spuriously setting step bits from page in the byte field.
     * Fix: extract page from the KNOWN non-overlapping region (bits 4-5 of pageAddr)
     * and then strip the page contamination from the byte.  This cannot fully recover
     * lo/hi when their bits 4-5 were genuinely set (those bits are irretrievably
     * merged), but it prevents phantom steps caused purely by page-bit injection.  */
    case CMD_GRID_ROW_LO:
    case CMD_GRID_ROW_HI: {
      const uint8_t bank  = (uint8_t)((v14 >> 12) & 3u);
      const uint8_t row   = (uint8_t)((v14 >> 8)  & 15u);
      /* Page is in bits 4-5 of the pre-OR'd pageAddr.  The raw bits 4-5 of v14
       * equal (page_actual | byte_bits_4_5) which may be wrong; we use them as a
       * best-effort estimate for the legacy format.                              */
      const uint8_t page  = (uint8_t)((v14 >> 4)  & 3u);
      /* Strip the page contribution from the byte field.  If byte's own bits 4-5
       * were set, they'll be cleared — partial recovery only.  Use SX_SUB_GRID_ROW
       * (txGridRow) for lossless transmission from new firmware and updated App.  */
      const uint8_t byte  = (uint8_t)((v14 & 0xFFu) & ~((uint8_t)(page << 4)));
      const int shift = (int)(page * 16 + (cmd == CMD_GRID_ROW_LO ? 0 : 8));
      portENTER_CRITICAL(&patchMux);
      uint64_t m = hwSeqData[bank][0][row];
      m = (m & ~(0xFFull << shift)) | ((uint64_t)byte << shift);
      hwSeqData[bank][0][row] = m;
      portEXIT_CRITICAL(&patchMux);
      displayDirty.store(true, std::memory_order_release);
      break;
    }
    case CMD_CLR_PLOCKS: {
      const uint8_t bank  = seqActiveBank .load(std::memory_order_relaxed);
      const uint8_t chain = seqActiveChain.load(std::memory_order_relaxed);
      portENTER_CRITICAL(&motionMux);
      for (int l = 0; l < 4; ++l) {
        hwMotionData[bank][chain][l].targetCmd = 255u;
        for (int s = 0; s < (int)MOTION_STEPS_PER_LANE; ++s)
          hwMotionData[bank][chain][l].steps[s] = 0xFFFFu;
      }
      portEXIT_CRITICAL(&motionMux);
      break;
    }
    case CMD_LOAD_PAT_S: {
      const uint8_t aBank  = seqActiveBank .load(std::memory_order_relaxed) & 0x0Fu;
      const uint8_t aChain = seqActiveChain.load(std::memory_order_relaxed) & 0x03u;
      loadFactorySynthPattern(aBank, aChain, v14 % NUM_SYNTH_PATS);
      break;
    }
    case CMD_LOAD_PAT_D: {
      const uint8_t aBank  = seqActiveBank .load(std::memory_order_relaxed) & 0x0Fu;
      const uint8_t aChain = seqActiveChain.load(std::memory_order_relaxed) & 0x03u;
      loadFactoryDrumPattern(aBank, aChain, v14 % NUM_DRUM_PATS);
      break;
    }

    /* ── System ──────────────────────────────────────────────────────────── */
    case CMD_FETCH:     requestFullStateSync(false, false); break;
    case CMD_HARD_SAVE: requestSessionSave(); break;
    case CMD_PING:      txSysex(CMD_PING, 1u); break;

    /* ── Laser show ──────────────────────────────────────────────────────── */
    /* [LASER-SHOW v2] Show Mode and MIDI→Hue are INDEPENDENT toggles (matches the
     * App's two separate buttons) — enabling the show no longer force-arms Hue. */
    case CMD_LSR_SHOW:  laserShowMode .store(v14 > 0u); break;
    case CMD_MIDI_HUE:  midiHueControl.store(v14 > 0u); break;
    /* [P4] Base hue + hue ADSR + anim/drum-flash → canonical layer (single src). */
    case CMD_HUE_BASE:  case CMD_HUE_ATK: case CMD_HUE_DEC:
    case CMD_HUE_SUS:   case CMD_HUE_REL:
    case CMD_LSR_ANIM:  case CMD_LSR_DRUMFLASH:
      applyMasterParam(cmd, v14, Origin::APP); break;

    /* ── MIDI channel routing ────────────────────────────────────────────── */
    case CMD_WIRE_HARP_CH: wireHarpMidiChannel.store(clamp_midi_channel(v14)); break;
    case CMD_WIRE_SEQ_CH:  wireSeqMidiChannel .store(clamp_midi_channel(v14)); break;
    case CMD_WIRE_DRUM_CH: wireDrumMidiChannel.store(clamp_midi_channel(v14)); break;

    /* ── [R6] v5.3 extensions (cmd 138–147) ─────────────────────────────── */
    case CMD_SEQ_CHAIN:
      applySeqChain(0);  /* V5.3-CONS: synth/drum page uses seqUI_page, chain pinned 0 */
      break;
    /* [P4] Mutes + aux delay-fb/reverb-damp → canonical layer (single source). */
    case CMD_H_MUTE:    case CMD_S_MUTE:    case CMD_D_MUTE:
    case CMD_AUX_DLY_FB: case CMD_AUX_REV_DMP:
      applyMasterParam(cmd, v14, Origin::APP); break;
    case CMD_DRUM_WAVE: {
      const uint8_t ch   = (uint8_t)((v14 >> 5) & 7u);
      const uint8_t widx = (uint8_t)(v14 & 31u) % NUM_WAVE_TABLES;
      if (ch < 8u) applyDrumWave(ch, widx);
      break;
    }
    case CMD_DRUM_KIT:
      /* Loads kit tuning into drumLivePatch + atomics and echoes all 32 drum
       * params back so the App knobs follow; also sets kick/hat character.       */
      applyDrumKit((uint8_t)v14, true);
      break;
    /* [v5.3.1] Song mode + transport + app sync (CMDs 148-157) ───────────── */
    case CMD_SONG_MODE:
      songModeActive.store((v14 >= 8192u), std::memory_order_release);
      if (v14 >= 8192u) song_rewind_rt();   /* [SONG-FIX] jump to song start on enable */
      txSysex(CMD_SONG_MODE, v14); break;
    case CMD_SONG_SLOT: {
      const uint8_t sl = (uint8_t)(v14 & 15u);
      activeSongSlot.store(sl, std::memory_order_relaxed);
      song_rewind_rt();   /* [SONG-FIX] new slot → reset chain + load its first bank */
      txSysex(CMD_SONG_SLOT, (uint16_t)sl);
      echoSongState(); break;
    }
    case CMD_SONG_STEP:
      applySongStep(v14); txSysex(CMD_SONG_STEP, v14); break;
    case CMD_SONG_STEPS_N: {
      const uint8_t n  = (uint8_t)std::min<uint16_t>(16u, std::max<uint16_t>(1u, v14));
      hwSongData[activeSongSlot.load() & 15u].numSteps = n;
      txSysex(CMD_SONG_STEPS_N, (uint16_t)n); break;
    }
    case CMD_TRANSPORT:
      /* CMD_TRANSPORT v14 is a discrete opcode (not bit-packed):
       *   0 = stop, 1 = start, 2 = pause, 3 = record ON, 4 = record OFF.
       * Play/pause/stop route through seq_* transport (tick reset, note release,
       * STEP_SYNC priming).  Record routes through seq_set_recording() so the App
       * echo uses the same seq_ext ring as hardware buttons (no direct txSysex).
       * Kept for backward compat — v6 App transport UI is read-only / hardware-owned. */
      switch (v14) {
        case 0: seq_stop();             break;
        case 1: seq_start();            break;
        case 2: seq_pause();            break;
        case 3: seq_set_recording(true);  break;
        case 4: seq_set_recording(false); break;
      }
      break;
    case CMD_TRANSPORT_AVAIL:
      /* [v6.0] Retired: transport is always hardware-owned, so the App can no
       * longer claim it.  Accepted-and-ignored for protocol compatibility with
       * older App builds that still send it on connect.                        */
      break;
    case CMD_APP_SYNC_REQ:
      if (v14 == 16383u) break; /* ACK echo — ignore */
      requestFullStateSync(true, true);
      break;
    case CMD_SESSION_SAVE:
      if (v14 == 16383u) break; /* ACK echo — ignore */
      if (v14 == 0u)     break; /* NACK echo — ignore */
      /* [FIX-H1] requestScopedSave now returns bool; send NACK 0 to App if a save
       * is already in flight so it doesn't silently lose the request.
       * [FIX-H4] Removed dead settings_mark_dirty() — g_saveRequest already armed. */
      if (!requestScopedSave((uint8_t)(v14 & 3u)))
        txSysex(CMD_SESSION_SAVE, 0u); /* NACK — save already in flight */
      break;
    case CMD_SESSION_LOAD:
      if (v14 == 16383u) break; /* ACK echo — ignore */
      if (v14 == 0u)     break; /* NACK echo — ignore */
      /* [FIX-C1] Dispatch LOAD to a 16 KB task — MidiUsbRx has only 8 KB which
       * is too shallow for the full NVS blob read + patterns array.
       * [FIX-C3] ACK/NACK sent from the task after verifying success.
       * [FIX-H2] DAC thresholds recomputed inside the task (was missing). */
      {
        NvsLoadCtx* ctx = new NvsLoadCtx{ (uint8_t)(v14 & 3u) };
        if (xTaskCreatePinnedToCore(nvs_load_task, "NvsLoad", 16384,
                                    ctx, 3, nullptr, 1) != pdPASS) {
          delete ctx;
          txSysex(CMD_SESSION_LOAD, 0u); /* NACK — task create failed */
        }
      }
      break;
    case CMD_SCOPED_RESET:
      /* [RESET v2] App-driven scoped reset (RAM wipe + persist + reboot).  The
       * App owns its own YES/NO confirm; the device just executes.              */
      if (v14 == 16383u) break; /* ACK echo — ignore */
      handleScopedReset((ResetScope)(v14 & 3u)); break;
    case CMD_SEQ_CLEAR:
      /* [CLEAR] Mirror of hardware SEQ SETUP → Clear (active grid + sounds→0). */
      if (v14 == 16383u) break;
      seqClearActiveAndResetSounds(); break;
    case CMD_SOFT_RESET:
      /* [SOFT-RESET] CLEAR extended: all settings + sounds + nav → initial. */
      if (v14 == 16383u) break;
      seqSoftResetWorkingImage(); break;
    case CMD_SEQ_RESTART:
      /* [RND-RESTART] App sends this after RND-H/RND-D to restart playback
       * from step 0 so the new random pattern is heard from the beginning.
       * No-op when stopped (seq_start() always resets counter on play). */
      seq_restart_from_step_zero(); break;
    case CMD_USR_SOUND_SAVE:
      /* [USER-SLOTS] App save: live patch → user slot + persist (no reboot). */
      if (v14 == 16383u) break;
      if (saveLiveToUserSlot((uint8_t)((v14 >> 13) & 1u), (uint8_t)(v14 & 63u)))
        txSysex(CMD_USR_SOUND_SAVE, v14);
      break;
    case CMD_USR_SOUND_LOAD:
      /* [USER-SLOTS] App load: recall user slot → live patch (echoes blob). */
      if (v14 == 16383u) break;
      loadUserSlotToLive((uint8_t)((v14 >> 13) & 1u), (uint8_t)(v14 & 63u)); break;
    case CMD_USR_PAT_SAVE:
      /* [USER-PAT-SLOTS] App save — groovebox echoes occupancy on success. */
      if (v14 == 16383u) break;
      saveActivePatternToUserSlot((uint8_t)(v14 & 63u)); break;
    case CMD_USR_PAT_LOAD:
      /* [USER-PAT-SLOTS] App load: recall user slot → active bank/chain. */
      if (v14 == 16383u) break;
      loadUserPatternToActive((uint8_t)(v14 & 63u)); break;
    case CMD_DB_ROUTE: applyDbeamRouteHW((uint8_t)(v14 & 3u)); break;
    case CMD_PB_RANGE: {
      const uint8_t v = (uint8_t)std::min(24, (int)v14);
      pbMapping.upSemi.store(v, std::memory_order_relaxed);
      pbMapping.downSemi.store(v, std::memory_order_relaxed);
      break;
    }
    case CMD_PB_ENABLE:
      pbMapping.enabled.store(v14 > 0u, std::memory_order_relaxed);
      break;
    case CMD_DB_TARGET:
      applyDbeamTarget((uint8_t)(v14 & 1u));
      dbeamRefreshAfterTargetChange();
      break;

    case CMD_SEQ_ARP_EN:   applySeqArpEnable(v14);  break;
    case CMD_SEQ_ARP_PAT:  applySeqArpPattern(v14); break;
    case CMD_SEQ_ARP_RATE: applySeqArpRate(v14);    break;
    case CMD_SEQ_ARP_GATE: applySeqArpGate(v14);    break;
    case CMD_HARP_ARP_EN:   applyHarpArpEnable(v14);  break;
    case CMD_HARP_ARP_PAT:  applyHarpArpPattern(v14); break;
    case CMD_HARP_ARP_RATE: applyHarpArpRate(v14);    break;
    case CMD_HARP_ARP_GATE: applyHarpArpGate(v14);    break;

    case CMD_D_FX_WET:
    case CMD_D_FX_P1:
    case CMD_D_FX_P2:
      applyDrumInsertParam(cmd, v14, Origin::APP); break;

    default: break;
  }

  /* P-lock capture — PARAM_TABLE.automatable gate; apply* also captures for
   * early-return cmd blocks; this catches FX indices, wire cmds, etc.       */
  captureMotionParam(cmd, v14);
}

/* ─────────────────────────────────────────────────────────────────────────────
 * parseMidiByte — USB MIDI state machine (feeds g_usb_parse).
 * ─────────────────────────────────────────────────────────────────────────────*/
void parseMidiByte(uint8_t b, MidiParserState& ps) {
  if (b >= 0xF8u) return;  /* Realtime bytes ignored — no channel routing */

  /* Sysex end */
  if (b == 0xF7u && ps.inSysex) {
    ps.inSysex = false;
    /* Accept ONLY 0x7D (App → device).  Our own outbound echoes use 0x7C, so a
     * looped-back MIDI stream is ignored here — no false heartbeat, no transport
     * re-toggle.  This is the only place that refreshes lastWebSysexMs.          */
    if (ps.sxPtr >= 6u && ps.sxBuf[0] == 0xF0u && ps.sxBuf[1] == 0x7Du) {
      lastWebSysexMs.store(millis(), std::memory_order_relaxed);
      if (ps.sxBuf[2] == SX_SUB_PATCH_BLOB) {
        /* [BLOB] App→device full preset: engine + 16 params (hi7,lo7). Replay
         * each through handleSysexCommand at cmd = base(0|16)+index — identical
         * to the 16 individual param messages, but from one atomic frame.  (The
         * App currently changes presets by index and never sends this; the path
         * exists for symmetry / future full-patch push.)                        */
        if (ps.sxPtr >= (uint16_t)(4u + PARAMS_PER_PRESET * 2u)) {
          const uint8_t base = (ps.sxBuf[3] == 0u) ? CMD_H_WAVE : CMD_S_WAVE;
          for (uint8_t i = 0; i < PARAMS_PER_PRESET; ++i) {
            const uint16_t pv = ((uint16_t)(ps.sxBuf[4u + i * 2u] & 0x7Fu) << 7u)
                               | (ps.sxBuf[5u + i * 2u] & 0x7Fu);
            handleSysexCommand((uint8_t)(base + i), pv);
          }
        }
      } else if (ps.sxBuf[2] == SX_SUB_USR_SOUND_NAME) {
        /* { F0, 7D, 03, engine, slot, 15×name, F7 } — App rename user sound slot. */
        if (ps.sxPtr >= 21u) {
          const uint8_t eng  = ps.sxBuf[3] & 1u;
          const uint8_t slot = ps.sxBuf[4] & 63u;
          char nm[16] = {};
          for (uint8_t i = 0; i < 15u; ++i) nm[i] = (char)(ps.sxBuf[5u + i] & 0x7Fu);
          setUserSlotName(eng, slot, nm[0] ? nm : nullptr);
          /* [FIX-NAME-SAVE] Use async NvsWorker instead of settings_persist_blocking().
           * The old blocking call held the 8 KB MidiUsbRx task for up to 20 s while
           * the NVS write completed — MIDI events were silently discarded for the
           * entire duration.  requestBanksOnlySave() queues the write asynchronously
           * with no reboot (name rename should never restart the device).          */
          requestBanksOnlySave();
          txUserSoundNameBlob(eng, slot);
        }
      } else if (ps.sxBuf[2] == SX_SUB_USR_PAT_NAME) {
        /* { F0, 7D, 04, slot, 15×name, F7 } — App rename user pattern slot. */
        if (ps.sxPtr >= 20u) {
          const uint8_t slot = ps.sxBuf[3] & 63u;
          char nm[16] = {};
          for (uint8_t i = 0; i < 15u; ++i) nm[i] = (char)(ps.sxBuf[4u + i] & 0x7Fu);
          setUserPatName(slot, nm[0] ? nm : nullptr);
          requestBanksOnlySave(); /* [FIX-NAME-SAVE] async persist — see above */
          txUserPatNameBlob(slot);
        }
      } else if (ps.sxBuf[2] == SX_SUB_GRID_ROW) {
        /* [FIX-GRID-ENC] { F0, 7D, 05, bank_row, page, lo_lo4, lo_hi4, hi_lo4, hi_hi4, F7 }
         * Lossless grid-row bulk write from App — no field overlaps, all MIDI-safe bytes. */
        if (ps.sxPtr >= 10u) {
          const uint8_t bank_row = ps.sxBuf[3];
          const uint8_t bank = (bank_row >> 4) & 3u;
          const uint8_t row  = bank_row & 15u;
          const uint8_t page = ps.sxBuf[4] & 3u;
          const uint8_t lo   = (ps.sxBuf[5] & 0x0Fu) | ((uint8_t)(ps.sxBuf[6] & 0x0Fu) << 4);
          const uint8_t hi   = (ps.sxBuf[7] & 0x0Fu) | ((uint8_t)(ps.sxBuf[8] & 0x0Fu) << 4);
          const int shift_lo = (int)(page * 16u);
          const int shift_hi = (int)(page * 16u + 8u);
          portENTER_CRITICAL(&patchMux);
          hwSeqData[bank][0][row] =
              (hwSeqData[bank][0][row]
               & ~((0xFFull << shift_lo) | (0xFFull << shift_hi)))
              | ((uint64_t)lo << shift_lo)
              | ((uint64_t)hi << shift_hi);
          portEXIT_CRITICAL(&patchMux);
          displayDirty.store(true, std::memory_order_release);
        }
      } else {
        const uint16_t v14 = ((uint16_t)(ps.sxBuf[4] & 0x7Fu) << 7u)
                            | (ps.sxBuf[5] & 0x7Fu);
        if      (ps.sxBuf[2] == 0x00u) handleSysexCommand(ps.sxBuf[3], v14);
        else if (ps.sxBuf[2] == 0x01u) handleSysexCommand((uint8_t)(ps.sxBuf[3] + 128u), v14);
      }
    }
    ps.sxPtr = 0;
    return;
  }

  if ((b & 0x80u) && b != 0xF0u && ps.inSysex) {
    ps.inSysex = false; ps.sxPtr = 0;
  }

  if (ps.inSysex) {
    if (ps.sxPtr < sizeof(ps.sxBuf)) ps.sxBuf[ps.sxPtr++] = b;
    else { ps.inSysex = false; ps.sxPtr = 0; }
    return;
  }

  if (b == 0xF0u) {
    ps.inSysex = true; ps.sxPtr = 0;
    ps.sxBuf[ps.sxPtr++] = b;
    return;
  }

  /* [MIDI-OPT4] Channel atomics only needed for channel-voice messages — skip
   * during SysEx accumulation (5 of 7 bytes per App frame). */
  const uint8_t harpCh = wireHarpMidiChannel.load(std::memory_order_relaxed);
  const uint8_t seqCh  = wireSeqMidiChannel .load(std::memory_order_relaxed);
  const uint8_t drumCh = wireDrumMidiChannel.load(std::memory_order_relaxed);

  if (b & 0x80u) { ps.midiStatus = b; ps.midiData1 = 0xFFu; return; }

  if ((ps.midiStatus & 0xF0u) == 0xC0u) {
    handleProgramChange((uint8_t)((ps.midiStatus & 0x0Fu) + 1u),
                        b, ps.bankMSB, ps.bankLSB);
    return;
  }

  if ((ps.midiStatus & 0xF0u) == 0xB0u && ps.midiData1 == 0u)   { ps.bankMSB = b & 0x7Fu; ps.midiData1 = 0xFFu; return; }
  if ((ps.midiStatus & 0xF0u) == 0xB0u && ps.midiData1 == 32u)  { ps.bankLSB = b & 0x7Fu; ps.midiData1 = 0xFFu; return; }

  if (ps.midiData1 == 0xFFu) { ps.midiData1 = b; return; }

  const uint8_t channel = (uint8_t)((ps.midiStatus & 0x0Fu) + 1u);
  const uint8_t status  = ps.midiStatus & 0xF0u;

  switch (status) {
    case 0xE0u: handlePitchBend(channel, (uint16_t)ps.midiData1 | ((uint16_t)b << 7u)); break;
    case 0xB0u: handleControlChange(channel, ps.midiData1, b); break;
    case 0x90u:
      if (b == 0u) {  /* velocity-0 = note-off */
        if      (channel == harpCh) harpMidiNoteOff(ps.midiData1);
        else if (channel == seqCh)  noteOffSeq(ps.midiData1);
        else if (channel == drumCh) noteOffDrums(ps.midiData1);
      } else {
        if (seqRecording.load(std::memory_order_relaxed))
          wireRecordInputNote(channel, ps.midiData1, b);
        if      (channel == harpCh) harpMidiNoteOn(ps.midiData1, b);
        else if (channel == seqCh)  noteOnSeq(ps.midiData1, b);
        else if (channel == drumCh) noteOnDrums(ps.midiData1, b);
      }
      break;
    case 0x80u:
      if      (channel == harpCh) harpMidiNoteOff(ps.midiData1);
      else if (channel == seqCh)  noteOffSeq(ps.midiData1);
      else if (channel == drumCh) noteOffDrums(ps.midiData1);
      break;
    default: break;
  }
  ps.midiData1 = 0xFFu;
}

/* ─────────────────────────────────────────────────────────────────────────────
 * sendFullStateSync — complete SSOT → WebApp echo
 *
 * [R8] Now calls echoFullSeqState() at the end, which adds:
 *      BPM, active bank/chain, length, transpose, octave shifts, mute states,
 *      seq patch, FX B-slots, aux dly/rev params, drum wave selections,
 *      and complete D-BEAM expression routing state.
 * ─────────────────────────────────────────────────────────────────────────────*/
void sendFullStateSync() {
  const bool prevYield = s_txSyncYield;
  s_txSyncYield        = true;
  s_txSyncYieldCount   = 0;

  /* Force every echo through: a full sync must transmit the complete state even
   * for parameters whose value has not changed since the last echo.  Reset the
   * TX value-dedup cache to an impossible 14-bit value (0xFFFF) so no txSysex
   * below is suppressed by the "same value within 500 ms" guard. [SYNC-FORCE]  */
  memset(s_last_tx_val, 0xFF, sizeof(s_last_tx_val));   /* [MIDI-OPT3] */

  /* [BLOB] Harp/seq synth params now ship as two atomic patch blobs (below);
   * only the drum block still needs a per-param snapshot here. */
  uint16_t dSnap[32];
  portENTER_CRITICAL(&patchMux);
  memcpy(dSnap, drumLivePatch, sizeof(dSnap));
  portEXIT_CRITICAL(&patchMux);

  /* ── Volume / level / EQ ─────────────────────────────────────────────── */
  txSysex(CMD_M_VOL,  (uint16_t)(masterVol  .load() * 16383.f));
  txSysex(CMD_H_VOL,  (uint16_t)(mixHarpVol .load() * 16383.f));
  txSysex(CMD_S_VOL,  (uint16_t)(mixSeqVol  .load() * 16383.f));
  txSysex(CMD_D_VOL,  (uint16_t)(mixDrumsVol.load() * 16383.f));
  txSysex(CMD_H_PAN,  (uint16_t)((mixHarpPan .load() + 1.f) * 0.5f * 16383.f));
  txSysex(CMD_S_PAN,  (uint16_t)((mixSeqPan  .load() + 1.f) * 0.5f * 16383.f));
  txSysex(CMD_D_PAN,  (uint16_t)((mixDrumsPan.load() + 1.f) * 0.5f * 16383.f));
  txSysex(CMD_PITCH,      encodeMasterPitch(masterPitch.load()));
  txSysex(CMD_DRUM_PITCH, encodeMasterPitch(drumPitchMult.load()));
  txSysex(CMD_EQ_L,   (uint16_t)(((masterEqLow .load() + 12.f) / 24.f) * 16383.f));
  txSysex(CMD_EQ_H,   (uint16_t)(((masterEqHigh.load() + 12.f) / 24.f) * 16383.f));
  txSysex(CMD_D_REV,  (uint16_t)(drumRevSend.load() * 16383.f));
  txSysex(CMD_D_DLY,  (uint16_t)(drumDlySend.load() * 16383.f));

  /* ── Tube / DJ ───────────────────────────────────────────────────────── */
  txSysex(CMD_TB_DRV,  (uint16_t)(tbDrive.load() * 16383.f));
  txSysex(CMD_TB_TONE, (uint16_t)(tbTone .load() * 16383.f));
  txSysex(CMD_TB_MIX,  (uint16_t)(tbMix  .load() * 16383.f));
  txSysex(CMD_DJ_FQ,   (uint16_t)(djFreq .load() * 16383.f));
  txSysex(CMD_DJ_RES,  (uint16_t)(djRes  .load() * 16383.f));
  txSysex(CMD_DJ_MIX,  (uint16_t)(djMix  .load() * 16383.f));

  /* ── Patches / scale / FX-A slots ───────────────────────────────────── */
  txSysex(CMD_H_PATCH,   (uint16_t)harpPatchIndex.load());
  txSysex(CMD_S_PATCH,   (uint16_t)seqPatchIndex .load());
  txSysex(CMD_H_SCALE,   (uint16_t)harpScaleIndex.load());
  txSysex(CMD_M_FX_IDX,  (uint16_t)masterFxIndex .load());
  txSysex(CMD_H_FX_IDX,  (uint16_t)harpFxIndex   .load());
  txSysex(CMD_H_FX_IDX_B,(uint16_t)harpFxIndexB  .load());
  txSysex(CMD_S_FX_IDX,  (uint16_t)seqFxIndex    .load());
  txSysex(CMD_S_FX_IDX_B,(uint16_t)seqFxIndexB   .load());

  /* ── D-BEAM basic ────────────────────────────────────────────────────── */
  txSysex(CMD_DB_ENABLED, dbeamEnabled.load() ? 16383u : 0u);

  /* ── All 16 harp + 16 seq synth params — [BLOB] two atomic transfers ──────
   * Replaces 32 individual param messages with two full-patch blobs.  Bonus:
   * the App maps blob index 11 → CMD 11/27 (the LFO-route selects), so the LFO
   * route now syncs device→App; the old loop echoed it at CMD_HLFO_RT(86) /
   * CMD_SLFO_RT(87), which the App's applyIncoming did not handle.            */
  txPatchBlob(0u); /* harp */
  txPatchBlob(1u); /* seq  */

  /* ── All 32 drum params ──────────────────────────────────────────────── */
  for (uint8_t i = 0; i < 32u; ++i) txSysex((uint8_t)(32u + i), dSnap[i]);
  txSysex(CMD_DRUM_KIT, (uint16_t)drumKit.load(std::memory_order_relaxed));
  txSysex(CMD_D_FX_IDX,   (uint16_t)drumFxIndexA.load(std::memory_order_relaxed));
  txSysex(CMD_D_FX_IDX_B, (uint16_t)drumFxIndexB.load(std::memory_order_relaxed));
  echoDrumInsertParams();

  /* ── Laser / wire ────────────────────────────────────────────────────── */
  txSysex(CMD_LSR_SHOW,     laserShowMode .load() ? 16383u : 0u);
  txSysex(CMD_MIDI_HUE,     midiHueControl.load() ? 16383u : 0u);
  txSysex(CMD_HUE_BASE,     (uint16_t)(laserBaseHue.load() * 16383.f));
  txSysex(CMD_WIRE_HARP_CH, wireHarpMidiChannel.load());
  txSysex(CMD_WIRE_SEQ_CH,  wireSeqMidiChannel .load());
  txSysex(CMD_WIRE_DRUM_CH, wireDrumMidiChannel.load());

  /* ── [P1] Insert FX sends (harp/seq dly+rev) — were never echoed ──────── */
  float hDly, hRev, sDly, sRev;
  portENTER_CRITICAL(&patchMux);
  hDly = fx.harpInsert.dly_send; hRev = fx.harpInsert.rev_send;
  sDly = fx.seqInsert .dly_send; sRev = fx.seqInsert .rev_send;
  portEXIT_CRITICAL(&patchMux);
  txSysex(CMD_H_DLY_MIX, (uint16_t)(hDly * 16383.f));
  txSysex(CMD_H_REV_MIX, (uint16_t)(hRev * 16383.f));
  txSysex(CMD_S_DLY_MIX, (uint16_t)(sDly * 16383.f));
  txSysex(CMD_S_REV_MIX, (uint16_t)(sRev * 16383.f));

  /* ── [P1] Hue ADSR (only HUE_BASE was echoed above) ───────────────────────
   * [LASER-SHOW v2] ATK/DEC/REL are stored in SECONDS on per-stage scales, so
   * normalise back to the 0..16383 wire range the App expects.  SUS is 0..1. */
  txSysex(CMD_HUE_ATK, (uint16_t)((hueAttack .load() / HUE_ATK_MAX_S) * 16383.f));
  txSysex(CMD_HUE_DEC, (uint16_t)((hueDecay  .load() / HUE_DEC_MAX_S) * 16383.f));
  txSysex(CMD_HUE_SUS, (uint16_t)(hueSustain.load() * 16383.f));
  txSysex(CMD_HUE_REL, (uint16_t)((hueRelease.load() / HUE_REL_MAX_S) * 16383.f));
  /* [LASER-SHOW v2] Anim mode (0..3 → 0..16383) + drum-flash depth (0..1). */
  txSysex(CMD_LSR_ANIM, (uint16_t)(((float)(uint8_t)laserShowAnim.load() / 3.f) * 16383.f));
  txSysex(CMD_LSR_DRUMFLASH, (uint16_t)(laserDrumFlash.load() * 16383.f));

  /* ── [v6.0] Harp play mode → App POLY/STR/SOLO buttons ───────────────────── */
  txSysex(CMD_PLAY_MODE, (uint16_t)currentPlayMode.load(std::memory_order_relaxed));

  /* ── [R8] Extended state — all previously-missing echoes ─────────────── */
  /* echoFullSeqState() ends with echoFullGrid() (patches.h), so the full 16×16
   * grid bulk dump (CMD_GRID_ROW_LO/HI ×128) is already sent LAST — after every
   * param knob — giving the App a 1:1 mirror of hwSeqData on connect.          */
  echoFullSeqState();
  txUserLibraryNames(); /* [USER-SLOTS] sparse custom names + pat occupancy */

  s_txSyncYield = prevYield;
}

static void full_sync_task(void* pv) {
  (void)pv;
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    sendFullStateSync();
    if (s_fspEchoSong.exchange(false, std::memory_order_acq_rel))
      echoSongState();
    if (s_fspSendAck.exchange(false, std::memory_order_acq_rel))
      txSysex(CMD_APP_SYNC_REQ, 16383u);
  }
}

/* ─────────────────────────────────────────────────────────────────────────────
 * wireRecordInputNote — live MIDI input → step sequencer grid recording
 * ─────────────────────────────────────────────────────────────────────────────*/
void wireRecordInputNote(uint8_t channel, uint8_t note, uint8_t /*vel*/) {
  const uint8_t seqCh = wireSeqMidiChannel.load(std::memory_order_relaxed);
  if (channel != seqCh) return;
  const uint8_t step  = seqCurrentStep.load(std::memory_order_relaxed) & 63u;
  const uint8_t bank  = seqActiveBank  .load(std::memory_order_relaxed) & 15u;
  const uint8_t chain = seqActiveChain .load(std::memory_order_relaxed) & 3u;
  portENTER_CRITICAL(&patchMux);
  hwSeqData[bank][chain][note % 8u] |= (1ull << step);
  portEXIT_CRITICAL(&patchMux);
}

/* ─────────────────────────────────────────────────────────────────────────────
 * init_midi_hardware — harp voice-owner init (USB MIDI only).
 * [USB-ONLY] No UART driver to install anymore; always succeeds.
 * ─────────────────────────────────────────────────────────────────────────────*/
bool init_midi_hardware() {
  /* [MIDI-OPT5/6] Dedup tables are static zero-init; no memset needed on boot. */
  harpVoiceOwnerInit();
  if (!hFullSyncTask) {
    xTaskCreatePinnedToCore(full_sync_task, "FullSyncOut", 8192, nullptr, 8,
                            &hFullSyncTask, 1);
  }
  return true;
}

/* ─────────────────────────────────────────────────────────────────────────────
 * midi_usb_event_task — polls USB MIDI every 1 ms and feeds the USB parser.
 * [USB-ONLY] Carries OctopusApp 0x7D SysEx control + optional instrument play-in.
 * ─────────────────────────────────────────────────────────────────────────────*/
void midi_usb_event_task(void* pvParameters) {
  (void)pvParameters;
  while (!g_systemReady.load(std::memory_order_acquire))
    vTaskDelay(pdMS_TO_TICKS(1));

  for (;;) {
    if (g_saveArmed.load(std::memory_order_acquire)) {
      vTaskDelay(pdMS_TO_TICKS(10)); continue;
    }

    midiEventPacket_t rx;
    while (MIDI.readPacket(&rx)) {
      switch (rx.header & 0x0Fu) {
        case 0x04: case 0x07: case 0x08: case 0x09:
        case 0x0A: case 0x0B: case 0x0E:
          parseMidiByte(rx.byte1, g_usb_parse);
          parseMidiByte(rx.byte2, g_usb_parse);
          parseMidiByte(rx.byte3, g_usb_parse);
          break;
        case 0x05: case 0x0F:
          parseMidiByte(rx.byte1, g_usb_parse);
          break;
        case 0x06: case 0x0C: case 0x0D:
          parseMidiByte(rx.byte1, g_usb_parse);
          parseMidiByte(rx.byte2, g_usb_parse);
          break;
        default: break;
      }
    }

    vTaskDelay(pdMS_TO_TICKS(1));
  }
}
