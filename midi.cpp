/* ═════════════════════════════════════════════════════════════════════════════
 * Octopus PRO XL v6.1.01 — Laser Harp Groovebox
 * © 2026 DIODAC ELECTRONICS / iSystem. All Rights Reserved.
 *
 * PROPRIETARY AND CONFIDENTIAL. Unauthorized copying, distribution, modification,
 * or use of this software or firmware, in whole or in part, is strictly prohibited
 * without prior written permission from DIODAC ELECTRONICS.
 * ═════════════════════════════════════════════════════════════════════════════
 * midi.cpp — v6.1.01  USB MIDI RX/TX + OCTOPUS SYSEX DISPATCH
 *
 * midi_usb_event_task (Core 1, prio 22 — FROZEN): parses USB MIDI stream, routes App SysEx
 * (0x7D) to handleSysexCommand() and channel voice to internal engines.
 * txSysex() / sendFullStateSync() echo device state on 0x7C (dedup + rate limit).
 * Transport play-in note/CC paths call groovebox.h / harp.h directly.
 * ═════════════════════════════════════════════════════════════════════════════ */
#include "interface.h"   /* encodeMasterPitch / decodeMasterPitch */
#include "effect.h"      /* FxChain fx, loadHarpFx etc.           */
#include "harp.h"
#include "dbeam.h"
#include "groovebox.h"
#include "patches.h"
#include "link.h"
#include "settings.h"     /* applyHarpParam, recallHarpPatch, …    */

static uint32_t s_last_rx_ms[256]  = {};
static uint16_t s_last_rx_val[256] = {};
static uint32_t s_last_tx_ms[256]  = {};
static uint16_t s_last_tx_val[256] = {};

/** App encodes persist: low nibble = ResetScope+1 (1..4); bits 4..15 = txn_id. */
static ResetScope decodePersistScopeV14(uint16_t v14) {
  const uint8_t lo = (uint8_t)(v14 & 0xFu);
  if (lo >= 1u && lo <= 4u)
    return (ResetScope)(lo - 1u);
  if (v14 >= 1u && v14 <= 4u)
    return (ResetScope)(v14 - 1u);
  return (ResetScope)(v14 & 3u);
}

MidiParserState g_usb_parse;

static void IRAM_ATTR midiEmitRaw(const uint8_t* msg, uint8_t n) {
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
 * TX helpers — mirror lane (FROZEN: docs/mirror_architecture.md §1.4, §3.2)
 *
 * g_appSessionLatched: STEP_SYNC / TRANSPORT / SONG_POS use txSysexForce path
 * even when isAppConnected() would be false (passive hardware-only performance).
 * Method locked — timing/dedup polish only.
 * ─────────────────────────────────────────────────────────────────────────────*/
void txSysex(uint8_t cmd, uint16_t v14bit) {
  /* Latched session: mirror playhead/transport like BPM — not gated on recent
   * inbound App SysEx (passive hardware-only performance must still see echoes). */
  if (g_appSessionLatched.load(std::memory_order_acquire)) {
    txSysexForce(cmd, v14bit);
    return;
  }
  if (!isAppConnected()) return;
  txSysexForce(cmd, v14bit);
}

void txSysexForce(uint8_t cmd, uint16_t v14bit) {

  if (v14bit > 16383u) v14bit = 16383u;

  const uint32_t now = millis();
  if (cmd != CMD_STEP_SYNC && cmd != CMD_SONG_POS && cmd != CMD_BPM && cmd != CMD_PING
      && cmd != CMD_TRANSPORT && cmd != CMD_TRIG_MODE && cmd != CMD_CPU_LOAD
      && cmd != CMD_DBEAM_AMP
      && cmd != CMD_SESSION_SLOT_ACK && cmd != CMD_SESSION_SAVE) {
    if (s_last_tx_val[cmd] == v14bit && (now - s_last_tx_ms[cmd] < 500u)) return;
  }
  /* Rate limiting for fast-changing parameter groups (never throttle link heartbeat). */
  if (cmd != CMD_BPM && cmd != CMD_PING && cmd != CMD_DBEAM_AMP
      && (cmd < 90u || (cmd >= 112u && cmd <= 120u))) {
    if (now - s_last_tx_ms[cmd] < 10u) return;
  }
  
  if (!midiLock()) return;
  const uint8_t v14hi = (uint8_t)((v14bit >> 7) & 0x7Fu);
  const uint8_t v14lo = (uint8_t)(v14bit & 0x7Fu);
  
  if (cmd <= 127u) {
    const uint8_t f[7] = { 0xF0u, 0x7Cu, 0x00u, cmd, v14hi, v14lo, 0xF7u };
    midiEmitRaw(f, 7);
  } else {
    const uint8_t f[7] = { 0xF0u, 0x7Cu, 0x01u, (uint8_t)(cmd - 128u), v14hi, v14lo, 0xF7u };
    midiEmitRaw(f, 7);
  }
  midiUnlock();

  s_last_tx_ms[cmd]  = now;
  s_last_tx_val[cmd] = v14bit;
}

void txPatchBlob(uint8_t engine) {
  if (engine > 1u) return;
  if (!isAppConnected()) return;          

  uint16_t snap[PARAMS_PER_PRESET];
  portENTER_CRITICAL(&patchMux);
  memcpy(snap, engine == 0u ? harpLivePatch : seqLivePatch, sizeof(snap));
  portEXIT_CRITICAL(&patchMux);

  if (!midiLock()) return;
  uint8_t f[5 + PARAMS_PER_PRESET * 2];
  uint8_t n = 0;
  f[n++] = 0xF0u;
  f[n++] = 0x7Cu;                          
  f[n++] = SX_SUB_PATCH_BLOB;
  f[n++] = engine;
  for (uint8_t i = 0; i < PARAMS_PER_PRESET; ++i) {
    f[n++] = (uint8_t)((snap[i] >> 7) & 0x7Fu);
    f[n++] = (uint8_t)(snap[i] & 0x7Fu);
  }
  f[n++] = 0xF7u;
  midiEmitRaw(f, n);
  midiUnlock();
}

/* Lossless grid row (device→App).  Matches OctopusApp sub 0x05 parser — avoids
 * GRID_ROW_LO/HI v14 bit overlap on steps 4–5 of each 8-step half-row.          */
void txGridRowBlob(uint8_t bank, uint8_t row, uint8_t page, uint8_t lo, uint8_t hi) {
  if (!isAppConnected()) return;
  if (!midiLock()) return;
  const uint8_t f[10] = {
    0xF0u, 0x7Cu, SX_SUB_GRID_ROW,
    (uint8_t)(((bank & 3u) << 4) | (row & 15u)),
    (uint8_t)(page & 3u),
    (uint8_t)(lo & 0x0Fu), (uint8_t)((lo >> 4) & 0x0Fu),
    (uint8_t)(hi & 0x0Fu), (uint8_t)((hi >> 4) & 0x0Fu),
    0xF7u
  };
  midiEmitRaw(f, sizeof(f));
  midiUnlock();
}

/* Device→App: live D-BEAM bargraph — lossless sub 0x06 (OctopusApp _parseDeviceSysex). */
void txDbeamAmpBlob(uint16_t amp) {
  if (amp > 16383u) amp = 16383u;
  if (!midiLock()) return;
  const uint8_t hi = (uint8_t)((amp >> 7) & 0x7Fu);
  const uint8_t lo = (uint8_t)(amp & 0x7Fu);
  const uint8_t f[6] = { 0xF0u, 0x7Cu, SX_SUB_DBEAM_AMP, hi, lo, 0xF7u };
  midiEmitRaw(f, sizeof(f));
  midiUnlock();
}

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

static bool userSoundSlotHasData(uint8_t engine, uint8_t slot) {
  if (engine > 1u || slot >= NUM_USER_SLOTS) return false;
  const int idx = USER_SLOT_BASE + (int)slot;
  const uint16_t* row = (engine == 0u) ? userBank[idx] : seqBank[idx];
  for (int i = 0; i < PARAMS_PER_PRESET; ++i)
    if (row[i] != 0u) return true;
  return false;
}

/* Push user-library metadata to the App after connect / FULL load.
 * Pattern slot GRID content is intentionally omitted: the App SLOTS vault only
 * shows names + occupied flags; loading a slot (CMD_USR_PAT_LOAD) pushes that
 * pattern into the active bank and echoes the live grid. Full sync already
 * carries the session grid (hwSeqData), not the 64-slot pattern library.      */
void txUserLibraryNames() {
  if (!isAppConnected()) return;
  for (uint8_t eng = 0; eng < 2u; ++eng) {
    for (uint8_t i = 0; i < NUM_USER_SLOTS; ++i) {
      if (g_userSlotName[eng][i][0]) txUserSoundNameBlob(eng, i);
      if (userSoundSlotHasData(eng, i))
        txSysex(CMD_USR_SOUND_SAVE, (uint16_t)((eng << 13) | i));
    }
  }
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

void txDrumFxSends() {
  if (!isAppConnected()) return;
  const uint16_t vDly = (uint16_t)(std::min(1.0f, std::max(0.0f,
                          drumDlySend.load(std::memory_order_relaxed))) * 16383.0f);
  const uint16_t vRev = (uint16_t)(std::min(1.0f, std::max(0.0f,
                          drumRevSend.load(std::memory_order_relaxed))) * 16383.0f);
  txSysex(CMD_D_DLY, vDly);
  txSysex(CMD_D_REV, vRev);
}

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

static void IRAM_ATTR noteOnSeq(uint8_t note, uint8_t velocity) {
  const int si = harpScaleIndex.load(std::memory_order_relaxed) & (NUM_SCALES - 1);
  const uint8_t scaleNote = SCALES[si].notes
      ? (uint8_t)SCALES[si].notes[(note & 7) % std::max(1, SCALES[si].numActiveStrings)]
      : note;

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

static void IRAM_ATTR handleControlChange(uint8_t channel, uint8_t cc, uint8_t value) {
  const uint8_t harpCh = wireHarpMidiChannel.load(std::memory_order_relaxed);
  const uint8_t seqCh  = wireSeqMidiChannel .load(std::memory_order_relaxed);
  const uint8_t drumCh = wireDrumMidiChannel.load(std::memory_order_relaxed);

  switch (cc) {
    case 120:
    case 123: allNotesOff(); return;
    case 121: return;
    default: break;
  }

  if (channel == drumCh) {
    const uint16_t v14 = cc7_to_v14_uni(value);
    switch (cc) {
      case 7:  mixDrumsVol .store(v14 / 16383.f); txSysex(CMD_D_VOL, v14); break;
      case 70: 
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
    case 91:
      portENTER_CRITICAL(&patchMux);
      if (isHarp) fx.harpInsert.dly_send = v14 / 16383.f;
      else        fx.seqInsert .dly_send = v14 / 16383.f;
      portEXIT_CRITICAL(&patchMux);
      txSysex(isHarp ? CMD_H_DLY_MIX : CMD_S_DLY_MIX, v14);
      break;
    case 93:
      portENTER_CRITICAL(&patchMux);
      if (isHarp) fx.harpInsert.rev_send = v14 / 16383.f;
      else        fx.seqInsert .rev_send = v14 / 16383.f;
      portEXIT_CRITICAL(&patchMux);
      txSysex(isHarp ? CMD_H_REV_MIX : CMD_S_REV_MIX, v14);
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

static void IRAM_ATTR handleProgramChange(uint8_t channel, uint8_t patch,
                                           uint8_t bankMSB, uint8_t bankLSB) {
  const int bank = ((bankMSB << 7) | bankLSB) & 0x01;
  const int pnum = clampRecallPatchIndex((bank << 7) | (patch & 0x7F));
  const uint8_t harpCh = wireHarpMidiChannel.load(std::memory_order_relaxed);
  const uint8_t seqCh  = wireSeqMidiChannel .load(std::memory_order_relaxed);
  if (channel == harpCh) recallHarpPatch(pnum, ParamSource::UI);      
  if (channel == seqCh)  recallSeqPatch (pnum, ParamSource::UI);      
}

void handleSysexCommand(uint8_t cmd, uint16_t v14) {
  const uint32_t now = millis();
  if (s_last_rx_val[cmd] == v14 && (now - s_last_rx_ms[cmd] < 50u)) return;
  if (cmd < 90u || (cmd >= 112u && cmd <= 120u)) {
    if (now - s_last_rx_ms[cmd] < 5u) return;
  }
  s_last_rx_ms[cmd]  = now;
  s_last_rx_val[cmd] = v14;

  if (cmd < 16u) {
    
    applyHarpParam((int)cmd, v14);
    recordMotionParam(cmd, v14);
    return;
  }
  if (cmd < 32u) {
    applySeqParam((int)(cmd - 16u), v14);
    recordMotionParam(cmd, v14);
    return;
  }
  if (cmd < 64u) {
    applyDrumParam((int)(cmd - 32u), v14);
    recordMotionParam(cmd, v14);
    return;
  }

  switch (cmd) {
    case CMD_M_VOL:   case CMD_H_VOL:   case CMD_S_VOL:   case CMD_D_VOL:
    case CMD_H_PAN:   case CMD_S_PAN:   case CMD_D_PAN:
    case CMD_EQ_L:    case CMD_EQ_H:    case CMD_PITCH: case CMD_DRUM_PITCH:
    case CMD_D_REV:   case CMD_D_DLY:
    case CMD_TB_DRV:  case CMD_TB_TONE: case CMD_TB_MIX:
    case CMD_DJ_FQ:   case CMD_DJ_RES:  case CMD_DJ_MIX:
    case CMD_DB_CURVE: case CMD_DB_OFFSET: case CMD_DB_RANGE: case CMD_DB_ENABLED:
      applyMasterParam(cmd, v14, Origin::APP); break;

    case CMD_HLFO_RT:
      applyHarpParam((int)SynthParam::P_LFO_ROUTE, v14);
      txSysex((uint8_t)(CMD_H_WAVE + (int)SynthParam::P_LFO_ROUTE), v14);
      break;
    case CMD_SLFO_RT:
      applySeqParam((int)SynthParam::P_LFO_ROUTE, v14);
      txSysex((uint8_t)(CMD_S_WAVE + (int)SynthParam::P_LFO_ROUTE), v14);
      break;
    case CMD_H_PATCH:
     
      recallHarpPatch((int)(v14 & 0xFFu), ParamSource::UI);
      txSysex(CMD_H_PATCH, v14 & 0xFFu); 
      break;
    case CMD_S_PATCH:
      recallSeqPatch((int)(v14 & 0xFFu), ParamSource::UI);
      txSysex(CMD_S_PATCH, v14 & 0xFFu); 
      break;
    case CMD_H_SCALE: {
      const uint8_t s = (uint8_t)(v14 & 15u);
      harpScaleIndex.store((int)s, std::memory_order_relaxed);
      txSysex(CMD_H_SCALE, (uint16_t)s);
      break;
    }

    case CMD_PLAY_MODE: {
      const uint8_t pm = (uint8_t)(v14 > 2u ? 0u : v14);
      harpSetPlayMode((PlayMode)pm);    
      txSysex(CMD_PLAY_MODE, (uint16_t)pm);  
      break;
    }

    case CMD_H_FX_IDX:   loadHarpFx  (v14 & 0x0Fu); harpFxIndex .store((int)(v14 & 0xFu)); txInsertFxSends(0u);
                         maybeEchoAuxAfterInsertLoad(); break;
    case CMD_H_FX_IDX_B: loadHarpFxB (v14 & 0x0Fu); harpFxIndexB.store((int)(v14 & 0xFu)); txSysex(CMD_H_FX_IDX_B, v14 & 0xFu); break;
    case CMD_S_FX_IDX:   loadSeqFx   (v14 & 0x0Fu); seqFxIndex  .store((int)(v14 & 0xFu)); txInsertFxSends(1u);
                         maybeEchoAuxAfterInsertLoad(); break;
    case CMD_S_FX_IDX_B: loadSeqFxB  (v14 & 0x0Fu); seqFxIndexB .store((int)(v14 & 0xFu)); txSysex(CMD_S_FX_IDX_B, v14 & 0xFu); break;
    case CMD_D_FX_IDX:   loadDrumFx(v14 & 0x0Fu); drumFxIndexA.store((int)(v14 & 0xFu));
                         txSysex(CMD_D_FX_IDX, v14 & 0xFu); echoDrumInsertParams();
                         maybeEchoAuxAfterInsertLoad(); break;
    case CMD_D_FX_IDX_B: loadDrumFxB (v14 & 0x0Fu); drumFxIndexB.store((int)(v14 & 0xFu)); txSysex(CMD_D_FX_IDX_B, v14 & 0xFu); break;
    case CMD_M_FX_IDX:   fx.loadMasterFx((int)(v14 & 0x0Fu)); txMasterFxParams(); break;

    case CMD_AUX_SCENE_IDX: {
      const int i = (int)(v14 & 0x0Fu);
      loadAuxScene(i);
      txSysex(CMD_AUX_SCENE_IDX, (uint16_t)i);
      echoAuxParams();
      break;
    }
    case CMD_LINK_AUX_PRESET: {
      const bool en = v14 > 8191;
      linkAuxToInsertPreset.store(en, std::memory_order_relaxed);
      txSysex(CMD_LINK_AUX_PRESET, en ? 16383u : 0u);
      displayDirty.store(true, std::memory_order_relaxed);
      break;
    }

    case CMD_H_FX_TIME: case CMD_S_FX_TIME:
    case CMD_H_FX_SIZE: case CMD_S_FX_SIZE:
      applyMasterParam(cmd, v14, Origin::APP); break;
    case CMD_H_FX_MIX:  case CMD_S_FX_MIX:
    case CMD_H_DLY_MIX: case CMD_H_REV_MIX:
    case CMD_S_DLY_MIX: case CMD_S_REV_MIX:
      applyFxSend(cmd, v14, Origin::APP); break;

    case CMD_TRIG_MODE:
      if (v14 > 0u) seq_start(); else seq_stop();
      txSysex(CMD_TRIG_MODE, seqPlaying.load() ? 16383u : 0u);
      break;
    case CMD_BPM:
      setSequencerBpm((int32_t)v14);   
      break;
    case CMD_BANK:
      applySeqBank((uint8_t)(v14 & 15u));
      break;
    case CMD_TRANSPOSE:
      applySeqTranspose(v14);             
      break;
    case CMD_H_PITCH: {  
      const float semis = ((float)v14 - 8192.0f) / 8192.0f * 12.0f; 
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
      if (trk < 16 && step < 64) {
        portENTER_CRITICAL(&patchMux);
        /* [GRID-64] Chain pinned 0 — matches sub-0x05 blob + App grid mirror. */
        hwSeqData[seqActiveBank.load() & 15u][0u][trk]
            ^= (1ull << step);
        portEXIT_CRITICAL(&patchMux);
      }
      break;
    }

    /* CMD_GRID_ROW_LO / CMD_GRID_ROW_HI (162/163) — RETIRED v6.1.  The old v14
     * packing folded the 8 step bits (bits 0-7) over the page field (bits 4-5),
     * so steps 4-5 of pages 1-3 were corrupted.  Bulk grid writes (both
     * directions) now use the lossless SX_SUB_GRID_ROW (sub 0x05) blob parsed in
     * parseMidiByte().  The numeric IDs stay RESERVED (never renumber wire IDs);
     * no device sends them any more, so they fall through to default and are
     * ignored.  Do not re-add a handler here — use the sub-0x05 frame.          */

    case CMD_CLR_PLOCKS: {
      const uint8_t bank  = seqActiveBank .load(std::memory_order_relaxed);
      portENTER_CRITICAL(&motionMux);
      for (int l = 0; l < 4; ++l) {
        hwMotionData[bank][0u][l].targetCmd = 255u;
        for (int s = 0; s < 16; ++s) hwMotionData[bank][0u][l].steps[s] = 0xFFFFu;
      }
      portEXIT_CRITICAL(&motionMux);
      g_motionLanesFull.store(false, std::memory_order_relaxed); /* lanes freed — clear telemetry flag */
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
    case CMD_FETCH:     sendFullStateSync(); break;
    case CMD_HARD_SAVE: requestSessionSave(); break;
    case CMD_PING:
      linkNoteAppPing(v14);
      txSysex(CMD_PING, linkEncodePong());
      break;

    case CMD_LSR_SHOW:  laserShowMode .store(v14 > 0u); break;
    case CMD_MIDI_HUE:  midiHueControl.store(v14 > 0u); break;
  
    case CMD_HUE_BASE:  case CMD_HUE_ATK: case CMD_HUE_DEC:
    case CMD_HUE_SUS:   case CMD_HUE_REL:
    case CMD_LSR_ANIM:  case CMD_LSR_DRUMFLASH:
      applyMasterParam(cmd, v14, Origin::APP); break;

    /* ── MIDI channel routing ────────────────────────────────────────────── */
    case CMD_WIRE_HARP_CH: wireHarpMidiChannel.store(clamp_midi_channel(v14)); break;
    case CMD_WIRE_SEQ_CH:  wireSeqMidiChannel .store(clamp_midi_channel(v14)); break;
    case CMD_WIRE_DRUM_CH: wireDrumMidiChannel.store(clamp_midi_channel(v14)); break;

    case CMD_SEQ_CHAIN:
      applySeqChain(0);  
      break;

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
     
      applyDrumKit((uint8_t)v14, true);
      break;
   
    case CMD_SONG_MODE:
      songModeActive.store((v14 >= 8192u), std::memory_order_release);
      if (v14 >= 8192u) song_rewind_rt();   
      txSysex(CMD_SONG_MODE, v14); break;
    case CMD_SONG_SLOT: {
      const uint8_t sl = (uint8_t)(v14 & 15u);
      activeSongSlot.store(sl, std::memory_order_relaxed);
      song_rewind_rt();  
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
      switch (v14 & 3u) {
        case 0: seq_stop();  break;
        case 1: seq_start(); break;
        case 2: seq_pause(); break;
        case 3:
          seqRecording.store(!seqRecording.load(std::memory_order_relaxed),
                             std::memory_order_relaxed);
          txSysex(CMD_TRANSPORT, seqRecording.load(std::memory_order_relaxed) ? 3u : 4u);
          displayDirty.store(true, std::memory_order_relaxed);
          break;
      }
      break;
    case CMD_TRANSPORT_AVAIL:
      break;
    case CMD_APP_SYNC_REQ:
      sendFullStateSync(); echoSongState();
      linkOnAppSyncComplete();
      txSysex(CMD_APP_SYNC_REQ, 16383u); break;
    case CMD_SESSION_SAVE:
      if (v14 == 16383u) break;
      recoverWedgedPersistFlags();
      {
        const uint16_t txn = decodePersistTxnV14(v14);
        const ResetScope scope = decodePersistScopeV14(v14);
        /* App save: no reboot — keep USB link alive (hardware menu still reboots). */
        if (!requestScopedSave((uint8_t)scope, false)) {
          txSysexForce(CMD_SESSION_SLOT_ACK,
                       linkEncodePersistAck(PersistAckPhase::FAIL, txn));
          txSysexForce(CMD_SESSION_SAVE, 0u);
          break;
        }
        linkBeginPersist(txn);
        settings_mark_dirty();
      }
      break;
    case CMD_SESSION_LOAD:
      /* Retired — NVS restores at boot; scoped LOAD removed from App + hardware menu. */
      break;
    case CMD_SCOPED_RESET:
      if (v14 == 16383u) break;
      linkBeginPersist(decodePersistTxnV14(v14));
      handleScopedReset(decodePersistScopeV14(v14)); break;
    case CMD_SEQ_CLEAR:
      if (v14 == 16383u) break;
      seqClearActiveAndResetSounds(); break;
    case CMD_USR_SOUND_SAVE:
      if (v14 == 16383u) break;
      if (saveLiveToUserSlot((uint8_t)((v14 >> 13) & 1u), (uint8_t)(v14 & 63u)))
        txSysex(CMD_USR_SOUND_SAVE, v14);
      break;
    case CMD_USR_SOUND_LOAD:
      if (v14 == 16383u) break;
      loadUserSlotToLive((uint8_t)((v14 >> 13) & 1u), (uint8_t)(v14 & 63u)); break;
    case CMD_USR_PAT_SAVE:
      if (v14 == 16383u) break;
      saveActivePatternToUserSlot((uint8_t)(v14 & 63u)); break;
    case CMD_USR_PAT_LOAD:
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

    /* App→device: restart the step counter to 0 without stopping playback
     * (sent after RND-H / RND-D so the new random pattern plays from beat 1). */
    case CMD_SEQ_RESTART:
      seq_restart_rt(); break;
    case CMD_SEQ_STEP_PAGE:
      applySeqStepPage((uint8_t)(v14 & 3u));
      break;

    default: break;
  }

  if (cmd >= 64u && cmd < CMD_BPM) {
    switch (cmd) {
    case CMD_HW_H_OCT: case CMD_HW_S_OCT: case CMD_HW_S_LEN:
    case CMD_TRANSPOSE: case CMD_HW_MARGIN: case CMD_HW_GATE: case CMD_HW_WHITE:
      break;
    default:
      recordMotionParam(cmd, v14);
      break;
    }
  }
 
  if (cmd == CMD_AUX_DLY_FB || cmd == CMD_AUX_REV_DMP || cmd == CMD_DRUM_WAVE) {
    recordMotionParam(cmd, v14);
  }
}

/* ─────────────────────────────────────────────────────────────────────────────
 * parseMidiByte — USB MIDI state machine (feeds g_usb_parse).
 * ─────────────────────────────────────────────────────────────────────────────*/
void parseMidiByte(uint8_t b, MidiParserState& ps) {
  if (b >= 0xF8u) return; 

  if (b == 0xF7u && ps.inSysex) {
    ps.inSysex = false;
   
    if (ps.sxPtr >= 6u && ps.sxBuf[0] == 0xF0u && ps.sxBuf[1] == 0x7Du) {
      lastWebSysexMs.store(millis(), std::memory_order_relaxed);
      if (ps.sxBuf[2] == SX_SUB_PATCH_BLOB) {
        if (ps.sxPtr >= (uint16_t)(4u + PARAMS_PER_PRESET * 2u)) {
          const uint8_t base = (ps.sxBuf[3] == 0u) ? CMD_H_WAVE : CMD_S_WAVE;
          for (uint8_t i = 0; i < PARAMS_PER_PRESET; ++i) {
            const uint16_t pv = ((uint16_t)(ps.sxBuf[4u + i * 2u] & 0x7Fu) << 7u)
                               | (ps.sxBuf[5u + i * 2u] & 0x7Fu);
            handleSysexCommand((uint8_t)(base + i), pv);
          }
        }
      } else if (ps.sxBuf[2] == SX_SUB_GRID_ROW) {
        if (ps.sxPtr >= 10u) {
          const uint8_t bank = (ps.sxBuf[3] >> 4) & 3u;
          const uint8_t row  = ps.sxBuf[3] & 15u;
          const uint8_t page = ps.sxBuf[4] & 3u;
          const uint8_t lo = (uint8_t)((ps.sxBuf[5] & 0xFu) | ((ps.sxBuf[6] & 0xFu) << 4));
          const uint8_t hi = (uint8_t)((ps.sxBuf[7] & 0xFu) | ((ps.sxBuf[8] & 0xFu) << 4));
          const int shiftLo = (int)(page * 16u);
          const int shiftHi = shiftLo + 8;
          portENTER_CRITICAL(&patchMux);
          uint64_t m = hwSeqData[bank][0][row];
          m = (m & ~(0xFFull << shiftLo)) | ((uint64_t)lo << shiftLo);
          m = (m & ~(0xFFull << shiftHi)) | ((uint64_t)hi << shiftHi);
          hwSeqData[bank][0][row] = m;
          portEXIT_CRITICAL(&patchMux);
          displayDirty.store(true, std::memory_order_release);
        }
      } else if (ps.sxBuf[2] == SX_SUB_USR_SOUND_NAME) {
        if (ps.sxPtr >= 21u) {
          const uint8_t eng  = ps.sxBuf[3] & 1u;
          const uint8_t slot = ps.sxBuf[4] & 63u;
          char nm[16] = {};
          for (uint8_t i = 0; i < 15u; ++i) nm[i] = (char)(ps.sxBuf[5u + i] & 0x7Fu);
          setUserSlotName(eng, slot, nm[0] ? nm : nullptr);
          settings_persist_blocking(ResetScope::BANKS_PATTERNS);
          txUserSoundNameBlob(eng, slot);
        }
      } else if (ps.sxBuf[2] == SX_SUB_USR_PAT_NAME) {
        if (ps.sxPtr >= 20u) {
          const uint8_t slot = ps.sxBuf[3] & 63u;
          char nm[16] = {};
          for (uint8_t i = 0; i < 15u; ++i) nm[i] = (char)(ps.sxBuf[4u + i] & 0x7Fu);
          setUserPatName(slot, nm[0] ? nm : nullptr);
          settings_persist_blocking(ResetScope::BANKS_PATTERNS);
          txUserPatNameBlob(slot);
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

void sendFullStateSync() { 
  memset(s_last_tx_val, 0xFF, sizeof(s_last_tx_val));   
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
  txSysex(CMD_H_PATCH,   (uint16_t)clampRecallPatchIndex((int)harpPatchIndex.load()));
  txSysex(CMD_S_PATCH,   (uint16_t)clampRecallPatchIndex((int)seqPatchIndex .load()));
  txSysex(CMD_H_SCALE,   (uint16_t)harpScaleIndex.load());
  txSysex(CMD_M_FX_IDX,  (uint16_t)masterFxIndex .load());
  txSysex(CMD_H_FX_IDX,  (uint16_t)harpFxIndex   .load());
  txSysex(CMD_H_FX_IDX_B,(uint16_t)harpFxIndexB  .load());
  txSysex(CMD_S_FX_IDX,  (uint16_t)seqFxIndex    .load());
  txSysex(CMD_S_FX_IDX_B,(uint16_t)seqFxIndexB   .load());

  /* ── D-BEAM basic ────────────────────────────────────────────────────── */
  txSysex(CMD_DB_ENABLED, dbeamEnabled.load() ? 16383u : 0u);

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

  float hDly, hRev, sDly, sRev;
  portENTER_CRITICAL(&patchMux);
  hDly = fx.harpInsert.dly_send; hRev = fx.harpInsert.rev_send;
  sDly = fx.seqInsert .dly_send; sRev = fx.seqInsert .rev_send;
  portEXIT_CRITICAL(&patchMux);
  txSysex(CMD_H_DLY_MIX, (uint16_t)(hDly * 16383.f));
  txSysex(CMD_H_REV_MIX, (uint16_t)(hRev * 16383.f));
  txSysex(CMD_S_DLY_MIX, (uint16_t)(sDly * 16383.f));
  txSysex(CMD_S_REV_MIX, (uint16_t)(sRev * 16383.f));

  txSysex(CMD_HUE_ATK, (uint16_t)((hueAttack .load() / HUE_ATK_MAX_S) * 16383.f));
  txSysex(CMD_HUE_DEC, (uint16_t)((hueDecay  .load() / HUE_DEC_MAX_S) * 16383.f));
  txSysex(CMD_HUE_SUS, (uint16_t)(hueSustain.load() * 16383.f));
  txSysex(CMD_HUE_REL, (uint16_t)((hueRelease.load() / HUE_REL_MAX_S) * 16383.f));
 
  txSysex(CMD_LSR_ANIM, (uint16_t)(((float)(uint8_t)laserShowAnim.load() / 3.f) * 16383.f));
  txSysex(CMD_LSR_DRUMFLASH, (uint16_t)(laserDrumFlash.load() * 16383.f));

  txSysex(CMD_PLAY_MODE, (uint16_t)currentPlayMode.load(std::memory_order_relaxed));

  echoFullSeqState();
  txUserLibraryNames(); 
}

void wireRecordInputNote(uint8_t channel, uint8_t note, uint8_t /*vel*/) {
  const uint8_t seqCh = wireSeqMidiChannel.load(std::memory_order_relaxed);
  if (channel != seqCh) return;
  const uint8_t step  = seqCurrentStep.load(std::memory_order_relaxed) & 63u;
  const uint8_t bank  = seqActiveBank  .load(std::memory_order_relaxed) & 15u;
  portENTER_CRITICAL(&patchMux);
  hwSeqData[bank][0u][note % 8u] |= (1ull << step);
  portEXIT_CRITICAL(&patchMux);
}


bool init_midi_hardware() {
  harpVoiceOwnerInit();
  return true;
}

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
