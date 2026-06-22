/* ═════════════════════════════════════════════════════════════════════════════
 * Octopus PRO XL v6.0.00 — Laser Harp Groovebox
 * © 2026 DIODAC ELECTRONICS / iSystem. All Rights Reserved.
 *
 * PROPRIETARY AND CONFIDENTIAL. Unauthorized copying, distribution, modification,
 * or use of this software or firmware, in whole or in part, is strictly prohibited
 * without prior written permission from DIODAC ELECTRONICS.
 * ═════════════════════════════════════════════════════════════════════════════
 * groovebox.cpp — v6.0.00  INTEGRATED GROOVEBOX ENGINE — IMPLEMENTATION
 *
 * [GB-MERGE] One translation unit for the whole groovebox machine:
 *   • transport + sample-locked step engine + grid editor  (was sequencer.cpp)
 *   • factory pattern ROM + pattern management              (was patterns.h)
 *   • semantic groovebox API                                [GB-API]
 *
 * The seq voice ops and drum DSP: drum_fill_buf stays inline in groovebox.h;
 * seq trigger/release/fill are implemented in this TU (§ SEQ SYNTH ENGINE below).
 * synth_core.h is gone — the sequencer path owns its own DSP here.
 *
 * Layout:
 *   TU-PRIVATE (anonymous namespace)
 *     1. Clock + step constants
 *     2. Lock-free SPSC out-ring
 *     3. Step-position helpers
 *     4. (removed) — external MIDI clock/realtime emitter; transport is now
 *        sample-locked in AudioSynth and the App syncs via SysEx position echoes
 *     5. Song-mode chain advance
 *     6. P-lock motion apply
 *     7. Grid math helpers
 *     8. Factory pattern ROM tables + row map
 *   PUBLIC
 *     9.  Grid cursor state
 *    10.  Grid editor navigation + OLED render
 *    11.  Transport
 *    12.  sequencer_render_block   (IRAM — audio task)
 *    13.  sequencer_background_task (Core 1 drain)
 *    14.  Pattern management
 *    15.  Semantic groovebox API
 * ═════════════════════════════════════════════════════════════════════════════ */
#include "groovebox.h"

#include <Arduino.h>
#include <algorithm>
#include <cstring>
#include <cstdlib>     /* std::abs(int) — nearest-row pitch placement */
#include <esp_timer.h>

#include "patches.h"  /* hwSeqData/hwSongData, applyHarp/Seq/DrumParam,
                       * handleSysexCommand, SCALES_NOTES, patchMux/motionMux,
                       * echoGridRow, recall*/loadFactory*Preset, getPresetName  */
#include "midi.h"     /* txSysex (App position echoes), allNotesOff               */
#include "display.h"  /* display object — seqUI_renderMatrix / renderSettings    */
#include "harp.h"     /* [LASER-SHOW] harpHueNoteOn/Off — melody drives hue ADSR  */
#include "arp.h"      /* [SEQ-ARP] shared arpeggiator engine                      */
#include "settings.h" /* [SOFT-RESET] reset_settings_to_factory()                 */

/* ═══════════════════════════════════════════════════════════════════════════
 * TU-PRIVATE ENGINE INTERNALS
 * ═══════════════════════════════════════════════════════════════════════════ */
namespace {

/* ── 1. Clock + step constants ───────────────────────────────────────────── */
constexpr uint8_t  SEQ_VEL_NORMAL = 85;
constexpr uint8_t  SEQ_VEL_ACCENT = 127;
constexpr uint32_t TICKS_PER_BEAT = 480;
constexpr uint32_t TICKS_PER_STEP = TICKS_PER_BEAT / 4; /* 16th notes */

std::atomic<uint32_t> master_tick_counter{ 0 };

/* [BPM-RACE] millis() of the last live BPM change.  The 600 ms sync supervisor
 * (below) re-asserts CMD_BPM to heal dropped echoes, but while the hardware
 * encoder is actively spinning that re-assert can carry a one-detent-stale value
 * that races the live per-detent echoes → App BPM flicker.  We suppress the
 * supervisor BPM re-assert for a short window after any live change; live echoes
 * still propagate instantly and idle healing resumes once the knob settles. */
static std::atomic<uint32_t> s_lastBpmChangeMs{ 0 };

/* [SEQ-ARP] Runtime engine + per-step velocity latch. */
arp::Engine s_seqArp;
static uint16_t s_seqArpVelQ15 = 0;

/* When a step has only one melody cell, expand to nearby scale degrees so
 * Up/Down/UpDn/Rnd patterns are audible (mirrors harpArpExpandScaleMotif). */
static void seqArpExpandScaleMotif(int8_t* midi, int8_t* rows, int& n) {
  if (n != 1) return;
  const int si    = harpScaleIndex.load(std::memory_order_relaxed) & (NUM_SCALES - 1);
  const int trans = seqTranspose.load(std::memory_order_relaxed);
  const int oct   = (int)octaveShift[1].load(std::memory_order_relaxed);
  const int baseRow = (int)rows[0];
  int8_t nm[arp::MAX_NOTES];
  int8_t nr[arp::MAX_NOTES];
  int cn = 0;
  for (int d = -1; d <= 2 && cn < arp::MAX_NOTES; ++d) {
    const int r = baseRow + d;
    if (r < 0 || r >= 8) continue;
    nr[cn] = (int8_t)r;
    nm[cn] = (int8_t)std::min(127, std::max(0,
        (int)SCALES_NOTES[si][7 - r] + trans + oct * 12));
    ++cn;
  }
  if (cn >= 2) {
    for (int i = 0; i < cn; ++i) {
      midi[i] = nm[i];
      rows[i] = nr[i];
    }
    n = cn;
  }
}

static void seqArpReleaseVoice(bool laser_show) {
  if (!s_seqArp.gate_open) return;
  release_seq_note(arp::SEQ_VOICE);
  seqVoiceOwner[arp::SEQ_VOICE] = HV_FREE;
  if (laser_show && s_seqArp.sounding_row >= 0)
    harpHueNoteOff(s_seqArp.sounding_row);
  s_seqArp.gate_open = false;
  s_seqArp.sounding_midi = -1;
  s_seqArp.sounding_row = -1;
}

static void seqArpCommitLatch(int8_t* midi, int8_t* rows, int n, bool laser_show) {
  seqArpReleaseVoice(laser_show);
  if (n <= 0) {
    s_seqArp.reset();
    s_seqArpVelQ15 = 0;
    return;
  }
  seqArpExpandScaleMotif(midi, rows, n);
  s_seqArp.latchNotes(midi, rows, n);
  s_seqArp.last_period = UINT32_MAX;
}

static void seqArpTick(uint32_t tick, bool laser_show) {
  /* [FIX-ARP-RACE] Apply any pending cross-core reset before touching fields.*/
  s_seqArp.check_reset();
  if (!seqArpEnabled.load(std::memory_order_relaxed)) return;
  if (s_seqArp.count <= 0) return;

  const uint8_t rate_i = std::min<uint8_t>(7u, seqArpRate.load(std::memory_order_relaxed));
  const uint8_t gate_i = std::min<uint8_t>(7u, seqArpGate.load(std::memory_order_relaxed));
  const uint8_t pat = std::min<uint8_t>((uint8_t)(arp::PATTERN_COUNT - 1),
                      seqArpPattern.load(std::memory_order_relaxed));

  /* Re-sync timing when rate/pattern changes mid-run (avoids stuck or burst notes). */
  static uint8_t s_cachedRate = 0xFFu;
  static uint8_t s_cachedPat  = 0xFFu;
  if (rate_i != s_cachedRate) {
    s_cachedRate = rate_i;
    s_seqArp.last_period = UINT32_MAX;
  }
  if (pat != s_cachedPat) {
    s_cachedPat = pat;
    s_seqArp.step_idx = 0;
  }

  const uint32_t period = arp::RATE_TICKS[rate_i];
  const uint32_t cur_period = tick / period;

  if (s_seqArp.gate_open && tick >= s_seqArp.gate_off_tick)
    seqArpReleaseVoice(laser_show);

  if (cur_period == s_seqArp.last_period) return;
  s_seqArp.last_period = cur_period;

  if (s_seqArp.gate_open) seqArpReleaseVoice(laser_show);

  const int8_t midi = s_seqArp.nextNote(pat);
  if (midi < 0) return;

  const int8_t row = s_seqArp.last_play_row;
  const uint16_t vel = s_seqArpVelQ15
                       ? s_seqArpVelQ15
                       : (uint16_t)((uint32_t)SEQ_VEL_NORMAL * (uint32_t)Q15_ONE / 127u);
  trigger_seq_note(arp::SEQ_VOICE, midi_note_to_freq((uint8_t)midi), vel);
  seqVoiceOwner[arp::SEQ_VOICE] = (int16_t)midi;
  s_seqArp.gate_open = true;
  s_seqArp.gate_off_tick = tick + (period * (uint32_t)arp::GATE_PCT[gate_i]) / 100u;
  s_seqArp.sounding_midi = midi;
  s_seqArp.sounding_row  = row;
  if (laser_show) {
    g_showBeamNote[row].store(midi, std::memory_order_relaxed);
    harpHueNoteOn(row);
  }
}

/* ── 2. Dual-tier outbound queue  [TIMING-LOCK / LINK-HEAL] ─────────────────
 * CRITICAL (BPM / TRANSPORT play+rec / STEP_SYNC): coalesced atomic slots —
 * latest value wins, never dropped.  Drained before the bulk ring every loop.
 *
 * BULK ring: motion echoes, BANK, SONG_POS, TRIG_MODE, etc.  Non-blocking;
 * overflow increments g_seq_ext_drops (telemetry via CMD_CPU_LOAD).  Consumer:
 * sequencer_background_task (SeqSysexOut) → txSysex under midiMutex.         */
struct SeqExtEvt {
  uint8_t  cmd;
  uint16_t v14;
};

constexpr size_t SEQ_EXT_QUEUE_SIZE = 256u;
SeqExtEvt             seq_ext_queue[SEQ_EXT_QUEUE_SIZE];
std::atomic<uint16_t> seq_ext_head{ 0 };
std::atomic<uint16_t> seq_ext_tail{ 0 };

std::atomic<bool>     seq_hi_bpm_pending{ false };
std::atomic<uint16_t> seq_hi_bpm_val{ 120 };
std::atomic<bool>     seq_hi_transport_play_pending{ false };
std::atomic<uint16_t> seq_hi_transport_play_val{ 0 };
std::atomic<bool>     seq_hi_transport_rec_pending{ false };
std::atomic<uint16_t> seq_hi_transport_rec_val{ 4 };
std::atomic<bool>     seq_hi_step_pending{ false };
std::atomic<uint16_t> seq_hi_step_val{ 0 };

static inline bool seq_ext_is_critical(uint8_t cmd) {
  return cmd == CMD_BPM || cmd == CMD_STEP_SYNC || cmd == CMD_TRANSPORT;
}

inline void IRAM_ATTR seq_ext_push_critical(uint8_t cmd, uint16_t v14) {
  if (cmd == CMD_BPM) {
    seq_hi_bpm_val.store(v14, std::memory_order_relaxed);
    seq_hi_bpm_pending.store(true, std::memory_order_release);
    return;
  }
  if (cmd == CMD_STEP_SYNC) {
    seq_hi_step_val.store(v14, std::memory_order_relaxed);
    seq_hi_step_pending.store(true, std::memory_order_release);
    return;
  }
  if (cmd == CMD_TRANSPORT) {
    if (v14 == 3u || v14 == 4u) {
      seq_hi_transport_rec_val.store(v14, std::memory_order_relaxed);
      seq_hi_transport_rec_pending.store(true, std::memory_order_release);
    } else {
      seq_hi_transport_play_val.store(v14, std::memory_order_relaxed);
      seq_hi_transport_play_pending.store(true, std::memory_order_release);
    }
  }
}

inline void IRAM_ATTR seq_ext_push(uint8_t cmd, uint16_t v14) {
  if (seq_ext_is_critical(cmd)) {
    seq_ext_push_critical(cmd, v14);
    return;
  }
  /* Coalesce: same cmd already queued → update value in place (motion echoes). */
  const uint16_t t = seq_ext_tail.load(std::memory_order_acquire);
  uint16_t h  = seq_ext_head.load(std::memory_order_relaxed);
  if (t != h) {
    uint16_t prev = (uint16_t)((h - 1u) & (SEQ_EXT_QUEUE_SIZE - 1u));
    if (seq_ext_queue[prev].cmd == cmd) {
      seq_ext_queue[prev].v14 = v14;
      return;
    }
  }
  const uint16_t nh = (uint16_t)((h + 1u) & (SEQ_EXT_QUEUE_SIZE - 1u));
  if (nh == t) {
    g_seq_ext_drops.fetch_add(1u, std::memory_order_relaxed);
    return;
  }
  seq_ext_queue[h] = SeqExtEvt{ cmd, v14 };
  seq_ext_head.store(nh, std::memory_order_release);
}

inline bool IRAM_ATTR seq_ext_pop(SeqExtEvt& out) {
  const uint16_t t = seq_ext_tail.load(std::memory_order_relaxed);
  if (t == seq_ext_head.load(std::memory_order_acquire)) return false;
  out = seq_ext_queue[t];
  seq_ext_tail.store((uint16_t)((t + 1u) & (SEQ_EXT_QUEUE_SIZE - 1u)),
                     std::memory_order_release);
  return true;
}

static void seq_ext_drain_hi_slot(std::atomic<bool>& pending, uint8_t cmd,
                                  std::atomic<uint16_t>& val) {
  while (pending.load(std::memory_order_acquire)) {
    const uint16_t v = val.load(std::memory_order_relaxed);
    pending.store(false, std::memory_order_release);
    txSysex(cmd, v);
  }
}

static void seq_ext_drain_critical() {
  seq_ext_drain_hi_slot(seq_hi_bpm_pending, CMD_BPM, seq_hi_bpm_val);
  /* STEP_SYNC before TRANSPORT — stop burst must paint step before play-off or
   * the App freezes the playhead at the wrong column / hides it on stop.        */
  seq_ext_drain_hi_slot(seq_hi_step_pending, CMD_STEP_SYNC, seq_hi_step_val);
  seq_ext_drain_hi_slot(seq_hi_transport_play_pending, CMD_TRANSPORT,
                        seq_hi_transport_play_val);
  seq_ext_drain_hi_slot(seq_hi_transport_rec_pending, CMD_TRANSPORT,
                        seq_hi_transport_rec_val);
}

/* ── 3. Step-position helper ──────────────────────────────────────────────── */
inline uint16_t IRAM_ATTR get_current_sequencer_step() {
  const uint32_t tick   = master_tick_counter.load(std::memory_order_relaxed);
  const uint32_t safLen = std::max<uint32_t>(1u,
                            (uint32_t)seqLength.load(std::memory_order_relaxed));
  return (uint16_t)((tick / TICKS_PER_STEP) % safLen);
}

/* ── 4. Song-mode chain advance  [TIMING-LOCK / C]  (mutex-free) ──────────────
 * Runs at every pattern wrap (step == 0) from the audio task.  Updates atomics
 * directly and echoes song POSITION through the out-ring — never txSysex on the
 * audio core.  Does NOT echo CMD_BANK (would move the App edit grid). [APP-SEQ-FIX] */
void IRAM_ATTR song_advance_rt() {
  if (!songModeActive.load(std::memory_order_relaxed)) return;

  const uint8_t  slot = activeSongSlot.load(std::memory_order_relaxed) & 15u;
  const SongSlot& song = hwSongData[slot];
  if (song.numSteps == 0u) return;

  const uint8_t curStep = songCurrentStep.load(std::memory_order_relaxed);
  const uint8_t curRpt  = songCurrentRepeat.load(std::memory_order_relaxed);
  const uint8_t stepIdx = (uint8_t)(curStep % song.numSteps);
  const SongStep& step  = song.steps[stepIdx];
  if (step.repeats == 0u) return; /* step inactive */

  const uint8_t nextRpt = (uint8_t)(curRpt + 1u);
  if (nextRpt < step.repeats) {
    songCurrentRepeat.store(nextRpt, std::memory_order_relaxed);
    return; /* still repeating current step */
  }

  const uint8_t nextStep = (uint8_t)((stepIdx + 1u) % song.numSteps);
  const SongStep& ns      = song.steps[nextStep];
  songCurrentRepeat.store(0u, std::memory_order_relaxed);
  songCurrentStep.store(nextStep, std::memory_order_relaxed);
  if (ns.repeats == 0u) return; /* skip inactive */

  const uint8_t nb = (uint8_t)(ns.bank & 15u);
  seqActiveBank.store(nb, std::memory_order_release);
  seqActiveChain.store(0u, std::memory_order_release);
  displayDirty.store(true, std::memory_order_relaxed);

  /* [PER-PATTERN-TRANSPOSE] Adopt the new pattern's stored transpose live (audio
   * core: single-byte read, no lock) so a song chain can sequence transposed
   * patterns.  Echo it so the App's transpose box tracks the chain. */
  const int8_t nt = seqPatternTranspose[nb][0];
  seqTranspose.store((int32_t)nt, std::memory_order_relaxed);

  /* Echo bank + song position so the App grid follows the chain (🔗 song mode). */
  seq_ext_push(CMD_BANK,      (uint16_t)nb);
  seq_ext_push(CMD_TRANSPOSE, (uint16_t)((int)nt + 12));
  seq_ext_push(CMD_SONG_POS, (uint16_t)(((nextStep & 0xFu) << 4) | 0u));
}

/* ── 5. P-lock motion apply  [TIMING-LOCK / motion]  (mutex-free) ─────────────
 * Synth/drum param ranges (cmd 0–63) route through applyHarp/Seq/DrumParam
 * (patchMux only, safe on audio core).  Master-FX automation (cmd ≥ 64) falls
 * back to handleSysexCommand (verified: only bare atomic stores, no mutex).    */
void IRAM_ATTR seq_apply_motion(uint8_t cmd, uint16_t v14) {
  /* Discrete layout params: live atomic only — never persist to pattern or
   * re-enter handleSysexCommand (applySeqTranspose would write pattern cells). */
  if (cmd == CMD_TRANSPOSE) {
    if (v14 <= 24u)
      seqTranspose.store((int32_t)v14 - 12, std::memory_order_relaxed);
    return;
  }
  if (cmd == CMD_HW_H_OCT || cmd == CMD_HW_S_OCT) {
    if (v14 <= 8u) {
      const int eng = (cmd == CMD_HW_H_OCT) ? 0 : 1;
      octaveShift[eng].store(
          std::min<int32_t>(4, std::max<int32_t>(-4, (int32_t)v14 - 4)),
          std::memory_order_relaxed);
    }
    return;
  }
  if (cmd == CMD_HW_S_LEN) {
    if (v14 >= 1u && v14 <= 64u)
      seqLength.store((uint8_t)v14, std::memory_order_relaxed);
    return;
  }
  if (cmd < 16u)      applyHarpParam((int)cmd, v14);
  else if (cmd < 32u) applySeqParam((int)(cmd - 16u), v14);
  else if (cmd < 64u) applyDrumParam((int)(cmd - 32u), v14);
  else                handleSysexCommand(cmd, v14);
}

/* ── 6. Grid math helpers ────────────────────────────────────────────────── */
inline int seqUI_gridBank() {
  return std::min(3, (int)seqActiveBank.load(std::memory_order_relaxed));
}
/* SSOT push — set bank (A-D = 0-3), keep chain pinned at 0.
 * [FIX-BANK-ECHO] Routes through applySeqBank() so txSysex(CMD_BANK) fires on
 * every hardware bank switch (OLED cursor wrap, bank A-D buttons).  Previously
 * stored seqActiveBank directly, so the App never saw hardware bank changes
 * until the next reconnect.  applySeqBank also syncs per-pattern transpose.  */
void seqUI_setBank(int bank) {
  const uint8_t nb = (uint8_t)(((bank % 4) + 4) % 4);
  applySeqBank(nb);                             /* sets atomic + echo + transpose */
  seqActiveChain.store((uint8_t)SEQ_UI_CHAIN, std::memory_order_release); /* pin 0 */
}
/* Step grid toggle — flips one cell of the active [bank][0] slot. */
void IRAM_ATTR toggleHardwareGridStep(uint8_t trackRow, uint8_t stepColumn) {
  if (trackRow >= 16u || stepColumn >= 64u) return;
  const uint8_t bank  = seqActiveBank.load(std::memory_order_relaxed) & 15u;
  const uint8_t chain = seqActiveChain.load(std::memory_order_relaxed) & 3u;
  portENTER_CRITICAL(&patchMux);
  hwSeqData[bank][chain][trackRow] ^= (1ull << stepColumn);
  portEXIT_CRITICAL(&patchMux);
  displayDirty.store(true, std::memory_order_release);
  /* [G2] Live echo edited row to App — runs on UI task, MIDI lock is safe here. */
  echoGridRow(bank, trackRow);
}
/* Step state query — chain 0 (synth rows 0-7 / drum rows 8-15). */
bool seqUI_isStepActiveInternal(int bank, int page, int row, int col) {
  if (bank < 0 || bank > 3 || page < 0 || page > 1) return false;
  if (row < 0 || row > 7 || col < 0 || col > 15) return false;
  const int hwRow = (page == 0) ? row : (row + 8);
  return (hwSeqData[bank][SEQ_UI_CHAIN][hwRow] & (1ull << col)) != 0;
}

/* ── 7. Factory pattern ROM tables + row map (TU-private PROGMEM) ──────────────
 * Row → semitone offset (mirrors sequencer engine_base_pitch[8]); row 0 = top
 * (+12 st), row 7 = root (0 st).  [U1] nearest-row pitch placement.            */
constexpr int kRowSemitone[8] = { 12, 11, 9, 7, 5, 4, 2, 0 };

const SynthPatternROM SYNTH_PATTERNS[NUM_SYNTH_PATS] PROGMEM = {
  { "Acid 303 Singularity",
    { 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0 },
    { 0, 12, 7, 0, 5, 3, 0, -12, 0, 12, 7, 0, 5, 3, 0, -12 },
    { 0, 50, 4500, 1200, 3000, 6200, 11500, 0, 8192, 1200, 3500, 1, 0, 9500, 5, 0 } },
  { "Magellanic Swell Arp",
    { 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 },
    { 0, 4, 7, 11, 12, 7, 4, 0, 2, 5, 9, 11, 14, 11, 7, 2 },
    { 21, 3500, 8000, 12000, 6500, 4100, 4000, 1500, 8400, 400, 8000, 3, 21, 2000, 1, 7 } },
  { "Pulsar Resonance Shift",
    { 1, 0, 1, 1, 0, 1, 1, 0, 1, 1, 0, 1, 1, 0, 1, 1 },
    { 0, 0, 12, 3, 0, 7, 10, 0, 5, 12, 0, 7, 8, 0, 3, 12 },
    { 1, 120, 6000, 4500, 2500, 8500, 13000, 800, 8192, 2500, 6000, 4, 1, 11000, 2, 14 } },
  { "Chronos Warp Staccato",
    { 1, 0, 1, 0, 1, 1, 0, 1, 0, 1, 0, 1, 1, 0, 1, 0 },
    { 12, 0, 7, 0, 0, 5, 0, 3, 0, 12, 0, 7, 8, 0, 2, 0 },
    { 13, 10, 1500, 0, 1500, 9800, 8000, 0, 8600, 4500, 0, 0, 0, 14000, 4, 5 } },
  { "Nebula Ambient Cascade",
    { 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0 },
    { 0, 0, 0, 0, 7, 0, 0, 0, 12, 0, 0, 0, 5, 0, 0, 0 },
    { 14, 6000, 12000, 14000, 9000, 3100, 2000, 3000, 8192, 150, 4000, 2, 14, 0, 0, 13 } },
  { "Event Horizon Growler",
    { 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 },
    { -12, -12, 0, 0, -5, -5, 7, 7, -12, -12, 12, 12, 0, 0, -5, -5 },
    { 19, 80, 5000, 9000, 4000, 2500, 12000, 4000, 8250, 1800, 9000, 6, 1, 6000, 5, 11 } },
  { "Quasar Kinetic Sub",
    { 1, 0, 0, 1, 0, 0, 1, 0, 1, 0, 0, 1, 0, 1, 0, 0 },
    { 0, 0, 0, 3, 0, 0, 5, 0, -12, 0, 0, 7, 0, 2, 0, 0 },
    { 22, 40, 3200, 11000, 2000, 1800, 3000, 0, 8192, 100, 0, 0, 22, 4000, 0, 1 } },
  { "Astral Lyric Tines",
    { 1, 0, 1, 0, 1, 0, 1, 0, 1, 1, 1, 1, 0, 0, 0, 0 },
    { 0, 0, 12, 0, 4, 0, 7, 0, 11, 12, 14, 16, 0, 0, 0, 0 },
    { 12, 20, 2500, 3000, 3500, 7800, 5000, 500, 8300, 1600, 2000, 5, 8, 3000, 7, 9 } },

  /* ── [WS5] Appended from patt.md (idx 8…28).  gate[] + pitch[] are the authored
   * rhythm/melody; preset[] is a DISTINCT hand-tuned patch per pattern (no longer
   * recycled from the cosmic set — each vibe gets its own waveform/ADSR/filter). */
  { "Acid Snake Bassline",
    { 1, 0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 1, 0, 1, 0, 0 },
    { 0, 0, -5, 0, 0, 7, 0, 0, 0, -5, 0, 3, 0, -12, 0, 0 },
    { 0, 40, 3800, 800, 2500, 5500, 12000, 0, 8292, 2000, 4000, 1, 1, 10000, 0, 0 } },
  { "Pentatonic Rise",
    { 1, 0, 0, 1, 0, 0, 1, 0, 1, 0, 0, 1, 0, 0, 1, 0 },
    { 0, 0, 0, 3, 0, 0, 5, 0, 7, 0, 0, 10, 0, 0, 12, 0 },
    { 3, 2800, 7500, 11000, 6000, 5000, 3500, 800, 8500, 600, 7500, 3, 8, 2500, 0, 0 } },
  { "Dorian Modal Groove",
    { 1, 1, 0, 1, 0, 1, 1, 0, 1, 0, 1, 0, 1, 1, 0, 1 },
    { 0, 2, 0, 3, 0, 5, 7, 0, 9, 0, 7, 0, 5, 3, 0, 2 },
    { 7, 200, 4500, 6000, 3500, 6800, 7000, 200, 8192, 1800, 4500, 4, 3, 8000, 0, 0 } },
  { "Funky Bass Pump",
    { 1, 0, 0, 1, 1, 0, 1, 0, 1, 0, 0, 1, 0, 1, 1, 0 },
    { 0, 0, 0, -2, -5, 0, 0, 0, -12, 0, 0, -2, -5, -3, -5, 0 },
    { 22, 60, 3000, 5000, 2000, 3200, 10000, 0, 8192, 1200, 3000, 1, 22, 11000, 0, 0 } },
  { "Lydian Shimmer",
    { 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0 },
    { 0, 0, 4, 0, 6, 0, 7, 0, 11, 0, 12, 0, 11, 0, 7, 0 },
    { 6, 15, 2200, 4000, 4000, 9000, 4500, 300, 8600, 800, 2500, 5, 12, 2000, 0, 0 } },
  { "Phrygian Descend",
    { 1, 1, 0, 0, 1, 0, 1, 1, 0, 1, 0, 0, 1, 0, 1, 0 },
    { 12, 10, 0, 0, 8, 0, 7, 5, 0, 3, 0, 0, 1, 0, 0, 0 },
    { 1, 100, 5500, 3000, 4500, 7200, 11000, 500, 8092, 2200, 5500, 3, 19, 9500, 0, 0 } },
  { "Jazz Minor Bebop",
    { 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 },
    { 0, 2, 3, 5, 7, 9, 11, 12, 11, 9, 7, 6, 5, 3, 2, 0 },
    { 12, 25, 2000, 2500, 3000, 8200, 4800, 400, 8350, 1400, 1800, 0, 13, 3500, 0, 0 } },
  { "Electro Hard Pulse",
    { 1, 0, 0, 1, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 1 },
    { 0, 0, 0, -12, 0, 5, 0, 0, 0, 0, 0, 7, 0, 0, 5, -5 },
    { 0, 30, 2800, 6000, 1800, 4800, 12500, 0, 8292, 3500, 5000, 1, 0, 12000, 0, 0 } },
  { "Dub Bass Riddim",
    { 1, 0, 0, 0, 1, 0, 1, 0, 0, 0, 1, 0, 0, 1, 0, 0 },
    { 0, 0, 0, 0, -5, 0, -5, 0, 0, 0, -7, 0, 0, 0, 0, 0 },
    { 22, 50, 4000, 10000, 3000, 2200, 8000, 0, 7992, 200, 1500, 7, 8, 5000, 0, 0 } },
  { "Chromatic Crawl",
    { 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0 },
    { 0, 0, 1, 0, 2, 0, 3, 0, 4, 0, 3, 0, 2, 0, 1, 0 },
    { 19, 80, 6000, 5000, 4000, 7800, 9000, 600, 8192, 3000, 6500, 4, 1, 10000, 0, 0 } },
  { "Gamelan Bell Cascade",
    { 1, 0, 0, 1, 0, 1, 0, 0, 1, 0, 1, 0, 0, 0, 1, 0 },
    { 0, 0, 0, 7, 0, 4, 0, 0, 12, 0, 7, 0, 0, 0, 4, 0 },
    { 6, 10, 1800, 2000, 3500, 9500, 3000, 200, 8700, 500, 3000, 5, 6, 1500, 0, 0 } },
  { "Blues Scale Walk",
    { 1, 0, 1, 0, 1, 1, 0, 1, 0, 1, 0, 1, 1, 0, 1, 0 },
    { 0, 0, 3, 0, 5, 6, 0, 7, 0, 5, 0, 3, 0, 0, -1, 0 },
    { 13, 40, 2000, 3500, 2500, 6500, 7500, 800, 8250, 2500, 4000, 3, 7, 8000, 0, 0 } },
  { "Techno Hypnosis",
    { 1, 0, 1, 0, 1, 0, 0, 1, 0, 1, 0, 1, 0, 0, 1, 0 },
    { 0, 0, 0, 0, 7, 0, 0, 5, 0, 0, 0, 3, 0, 0, 0, 0 },
    { 0, 35, 4200, 4000, 2800, 5800, 11800, 0, 8192, 2800, 4500, 1, 2, 10500, 0, 0 } },
  { "Eastern Wind Scale",
    { 1, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0, 0, 1, 0, 1, 0 },
    { 0, 0, 1, 4, 0, 0, 5, 0, 7, 8, 0, 0, 7, 0, 4, 0 },
    { 21, 300, 5000, 7000, 5500, 7200, 3500, 1500, 8492, 900, 3500, 0, 21, 4000, 0, 0 } },
  { "Ambient Chord Drift",
    { 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0 },
    { 0, 0, 0, 0, 4, 0, 0, 0, 7, 0, 0, 0, 11, 0, 0, 0 },
    { 14, 5500, 11000, 14000, 8500, 3800, 1800, 2500, 8392, 200, 5000, 2, 14, 0, 0, 0 } },
  { "Rave Saw Blaster",
    { 1, 1, 0, 1, 1, 0, 1, 1, 0, 1, 1, 0, 1, 0, 1, 1 },
    { 0, 3, 0, 5, 7, 0, 7, 5, 0, 3, 0, 0, 12, 0, 7, 5 },
    { 0, 20, 1500, 7000, 1500, 4500, 13000, 0, 8392, 4000, 6000, 6, 0, 11500, 0, 0 } },
  { "Baroque Step Fall",
    { 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 },
    { 7, 5, 3, 2, 0, -1, 0, 2, 3, 2, 0, 2, 4, 3, 2, 0 },
    { 13, 15, 1200, 2500, 2800, 7500, 4200, 100, 8192, 600, 1500, 0, 6, 4500, 0, 0 } },
  { "Polyrhythm Triplet",
    { 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1 },
    { 0, 0, 0, 5, 0, 0, 7, 0, 0, 3, 0, 0, 5, 0, 0, 0 },
    { 12, 12, 1000, 500, 1200, 8800, 5500, 200, 8192, 3200, 2000, 7, 3, 6000, 0, 0 } },
  { "Afro Tumbao Lick",
    { 1, 0, 0, 1, 1, 0, 1, 0, 0, 0, 1, 0, 1, 0, 0, 0 },
    { 0, 0, 0, -5, -3, 0, -5, 0, 0, 0, -7, 0, 0, 0, 0, 0 },
    { 22, 45, 2500, 8000, 2200, 2800, 9500, 300, 8192, 1500, 2500, 1, 24, 9000, 0, 0 } },
  { "Deep Space Signal",
    { 1, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0, 0, 7, 0, 0, 0, 0, 5, 0, 0 },
    { 8, 4000, 10000, 12000, 8000, 4500, 2200, 3500, 8192, 100, 3000, 2, 8, 500, 0, 0 } },
  { "Minimal Phase Loop",
    { 1, 1, 1, 0, 1, 1, 0, 1, 1, 0, 1, 1, 1, 0, 1, 1 },
    { 0, 2, 4, 0, 2, 4, 0, 0, 2, 0, 4, 2, 0, 0, 4, 2 },
    { 1, 500, 6000, 9000, 5000, 5200, 6000, 500, 8192, 800, 5500, 3, 3, 7000, 0, 0 } }
};

const DrumPatternROM DRUM_PATTERNS[NUM_DRUM_PATS] PROGMEM = {
  { "Quantum House Core",
    { 0x1111, 0x0404, 0x0000, 0x4444, 0x0A0A, 0x0000, 0x0000, 0x0202 },
    { 8192, 8500, 14000, 0, 8192, 6000, 11000, 6000, 8192, 4000, 12000, 8192, 8192, 5000, 10000, 12000,
      8192, 9000, 9000, 14000, 8192, 5000, 11000, 2000, 8192, 5000, 11000, 2000, 8192, 4500, 12000, 4000 } },
  { "Magnetar Sat Groove",
    { 0x1111, 0x0444, 0x0404, 0x5555, 0xAAAA, 0x0101, 0x1010, 0x4224 },
    { 9500, 9000, 15000, 0, 7500, 8000, 12000, 9000, 8192, 5500, 13000, 7000, 9000, 4500, 11000, 13000,
      8192, 9500, 10000, 15000, 7000, 6000, 12000, 4000, 8800, 5800, 11500, 3000, 8192, 5000, 13000, 6000 } },
  { "Industrial Pulsar",
    { 0x9999, 0x0404, 0x4040, 0xEEEE, 0x1111, 0x2222, 0x8888, 0x0000 },
    { 6000, 11000, 16383, 2000, 9000, 9500, 13000, 12000, 6500, 8000, 14000, 11000, 11000, 3000, 12000, 14000,
      8192, 8000, 11000, 11000, 6000, 8000, 13000, 8000, 5500, 9000, 13000, 7000, 8192, 4000, 10000, 2000 } },
  { "Liquid Hang Meditation",
    { 0x1000, 0x0000, 0x0202, 0x4444, 0x0808, 0x1111, 0x5555, 0x2222 },
    { 8192, 4000, 10000, 0, 8192, 4000, 0, 0, 8192, 3000, 9000, 4000, 8192, 6000, 8000, 8192,
      8192, 7000, 8000, 10000, 8192, 9500, 14000, 0, 9200, 11000, 15000, 0, 8500, 10000, 13000, 1000 } },
  { "Event Horizon Break",
    { 0x1212, 0x0404, 0x0101, 0x5454, 0x2A2A, 0x0800, 0x0002, 0x1010 },
    { 8192, 8800, 14500, 0, 8192, 6500, 11500, 7000, 8500, 4500, 12500, 9000, 8192, 4800, 10500, 11000,
      8192, 9200, 9500, 13500, 8192, 5500, 10500, 3000, 8192, 5200, 12000, 1000, 8800, 4200, 12500, 5000 } },
  { "Cyberpunk Magnetar",
    { 0x1515, 0x0404, 0x0444, 0xFAFA, 0x0F0F, 0x0202, 0x2020, 0x4444 },
    { 11000, 9800, 16000, 1000, 8500, 7500, 13000, 10000, 8192, 6000, 13500, 8500, 9500, 4000, 12000, 14000,
      8192, 9900, 11000, 16000, 7500, 6500, 12500, 5000, 9000, 6000, 12000, 4000, 8192, 5500, 14000, 7000 } },
  { "Sub-Zero Minimalist",
    { 0x1010, 0x0000, 0x0404, 0x1111, 0x0000, 0x4040, 0x0404, 0x0202 },
    { 7000, 12000, 15000, 0, 8192, 4000, 0, 0, 9000, 3500, 11000, 6000, 7500, 6500, 9000, 10000,
      8192, 8000, 7000, 12000, 8192, 4500, 10000, 1000, 7800, 5000, 11000, 2000, 9000, 3800, 11500, 3000 } },
  { "Cosmic Warehouse Tech",
    { 0x1111, 0x0404, 0x0505, 0x5555, 0xAAAA, 0x1010, 0x0101, 0x2222 },
    { 8500, 8200, 14200, 0, 8000, 6200, 11200, 6500, 8300, 4200, 12200, 8500, 8300, 5200, 10200, 12500,
      8300, 9200, 9200, 14500, 8300, 5200, 11200, 2200, 8300, 5200, 11200, 2200, 8300, 4700, 12200, 4500 } },

  /* ── [WS5] Appended from patt.md (idx 8…28).  tracks[8] step masks + the 8×4
   * drum patch were authored to the DrumPatternROM layout and verified in range
   * (0…16383), so these import as-authored.                                     */
  { "Four-Four Foundation",
    { 0x1111, 0x1010, 0x5555, 0x4000, 0x0000, 0x0000, 0x4444, 0x0001 },
    { 9500, 8500, 14000, 0, 8192, 8000, 13000, 7000, 8192, 5000, 9000, 5000, 6000, 0, 12000, 0,
      0, 0, 0, 0, 0, 0, 0, 0, 7000, 4500, 9000, 3500, 12000, 0, 0, 0 } },
  { "Broken Beat Breaker",
    { 0x0909, 0x2090, 0xAAAA, 0x8080, 0x0208, 0x0800, 0x5555, 0x0000 },
    { 8500, 8000, 13000, 0, 7500, 7000, 12000, 8000, 8192, 4500, 9500, 5000, 6000, 0, 13000, 0,
      7000, 4000, 9000, 0, 8000, 0, 11000, 0, 7000, 4500, 8500, 4000, 0, 0, 0, 0 } },
  { "Trap Bomb Drop",
    { 0x8121, 0x1010, 0xFFFF, 0x4040, 0x0000, 0x0000, 0x0000, 0x0001 },
    { 9500, 7000, 14000, 0, 8192, 9000, 15000, 8000, 8192, 3500, 9000, 4000, 5500, 0, 11000, 0,
      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 12000, 0, 0, 0 } },
  { "Minimal Techno Pulse",
    { 0x1111, 0x0000, 0x5555, 0x0000, 0x0404, 0x0000, 0x0000, 0x0000 },
    { 9000, 8000, 14000, 0, 0, 0, 0, 0, 8192, 4500, 8500, 5000, 0, 0, 0, 0,
      7500, 4000, 10000, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 } },
  { "Jungle Breakstep",
    { 0x1419, 0x4104, 0x7777, 0x8080, 0x0401, 0x0000, 0x4444, 0x0000 },
    { 9000, 8500, 14000, 0, 8192, 7500, 13000, 8500, 8192, 3000, 8000, 4000, 6000, 0, 12000, 0,
      8000, 5000, 11000, 0, 0, 0, 0, 0, 7000, 4000, 9000, 3000, 0, 0, 0, 0 } },
  { "Afro-Beat Engine",
    { 0x4141, 0x1010, 0x5555, 0x0404, 0x1249, 0x0110, 0x2222, 0x0000 },
    { 8500, 8000, 13000, 0, 8192, 8000, 12000, 7000, 8192, 4500, 9000, 5000, 7000, 4000, 11000, 0,
      7500, 5000, 10000, 3000, 8192, 5500, 10000, 4000, 7000, 4000, 9000, 3500, 0, 0, 0, 0 } },
  { "Disco Fever Reel",
    { 0x1111, 0x1010, 0x5555, 0x4444, 0x0404, 0x8080, 0x1111, 0x0001 },
    { 9500, 9000, 15000, 0, 8192, 8500, 13000, 7500, 8192, 4500, 9000, 5000, 7000, 4000, 11000, 0,
      8000, 5000, 11000, 3000, 8000, 5000, 11000, 3000, 8192, 6000, 10000, 5000, 12000, 0, 0, 0 } },
  { "Reggaeton Dembow",
    { 0x1101, 0x0410, 0x5555, 0x4040, 0x0000, 0x0000, 0xAAAA, 0x0000 },
    { 9000, 8000, 13500, 0, 8192, 7500, 12500, 8000, 8192, 4000, 9000, 5000, 6500, 0, 11500, 0,
      0, 0, 0, 0, 0, 0, 0, 0, 6500, 4000, 8500, 3500, 0, 0, 0, 0 } },
  { "Deep House Shuffle",
    { 0x1111, 0x1010, 0x7575, 0x4040, 0x0000, 0x0000, 0x4444, 0x0001 },
    { 9000, 8500, 14000, 0, 8192, 8000, 12500, 7000, 8192, 4000, 9000, 4500, 6000, 0, 11000, 0,
      0, 0, 0, 0, 0, 0, 0, 0, 6500, 4000, 8500, 3500, 11000, 0, 0, 0 } },
  { "Gabber Overdrive",
    { 0xFFFF, 0x1111, 0xAAAA, 0x0000, 0x0000, 0x0101, 0x1010, 0x0000 },
    { 9500, 9000, 16000, 0, 9000, 9000, 15000, 8000, 8192, 3000, 8000, 4000, 0, 0, 0, 0,
      0, 0, 0, 0, 8000, 6000, 12000, 4000, 8000, 6000, 12000, 4000, 0, 0, 0, 0 } },
  { "Half-Time Stomp",
    { 0x0101, 0x0100, 0x5555, 0x4000, 0x0808, 0x0000, 0x4444, 0x0001 },
    { 10000, 9000, 15000, 0, 8192, 8500, 14000, 7000, 8192, 4500, 9000, 5000, 5500, 0, 11000, 0,
      7500, 4500, 11000, 0, 0, 0, 0, 0, 6500, 4000, 8500, 3500, 12000, 0, 0, 0 } },
  { "UK Garage Roll",
    { 0x0909, 0x1010, 0x5D5D, 0x4040, 0x0000, 0x0000, 0x2222, 0x0000 },
    { 9000, 8500, 13500, 0, 8192, 8000, 13000, 7500, 8192, 3500, 8500, 4500, 6000, 0, 11000, 0,
      0, 0, 0, 0, 0, 0, 0, 0, 6500, 4000, 8500, 3000, 0, 0, 0, 0 } },
  { "Latin Clave Groove",
    { 0x5141, 0x1010, 0x5555, 0x0408, 0x1449, 0x4110, 0x2222, 0x0000 },
    { 9000, 8000, 13000, 0, 8192, 7500, 12000, 7000, 8192, 4000, 9000, 5000, 7500, 4000, 11500, 0,
      8000, 5000, 11000, 3000, 8000, 5000, 11000, 3500, 7000, 4000, 9000, 3000, 0, 0, 0, 0 } },
  { "808 Hip-Hop Drive",
    { 0x2121, 0x1010, 0x1111, 0x4040, 0x0000, 0x0000, 0xAAAA, 0x0000 },
    { 10000, 7000, 14000, 0, 8192, 8500, 13500, 7500, 8192, 4500, 9000, 5000, 6000, 0, 11500, 0,
      0, 0, 0, 0, 0, 0, 0, 0, 6500, 3500, 8500, 3000, 0, 0, 0, 0 } },
  { "Neurofunk Grid",
    { 0x1213, 0x4104, 0xDDDD, 0x0808, 0x0041, 0x0000, 0x4444, 0x0000 },
    { 9000, 8500, 14000, 0, 7500, 7000, 12500, 8000, 8192, 3000, 8000, 4000, 6000, 0, 12000, 0,
      8000, 5000, 11000, 0, 0, 0, 0, 0, 7000, 4000, 9000, 3500, 0, 0, 0, 0 } },
  { "Footwork Blaster",
    { 0x1515, 0x1111, 0xFFFF, 0x4040, 0x8888, 0x0000, 0x2222, 0x0101 },
    { 9500, 8000, 14000, 0, 8192, 9000, 14000, 8000, 8192, 3000, 8000, 3500, 5500, 0, 11000, 0,
      7500, 4500, 10000, 0, 0, 0, 0, 0, 7000, 4000, 9000, 3500, 10000, 0, 12000, 0 } },
  { "Glitch Industrial",
    { 0x2185, 0x1212, 0x2929, 0x0000, 0x8444, 0x4081, 0x0000, 0x0000 },
    { 9000, 7500, 14000, 0, 8000, 7000, 12500, 8500, 8192, 3500, 8500, 4500, 0, 0, 0, 0,
      7000, 4500, 10000, 2000, 8000, 5000, 11000, 3000, 0, 0, 0, 0, 0, 0, 0, 0 } },
  { "Bossa Nova Light",
    { 0x4141, 0x1010, 0x5555, 0x0040, 0x0909, 0x1010, 0x2222, 0x0000 },
    { 8500, 8000, 12000, 0, 8192, 7500, 11000, 6500, 8192, 4000, 8500, 4500, 6000, 0, 10000, 0,
      7500, 5000, 10000, 3000, 8192, 5500, 10000, 4500, 6500, 4000, 8500, 3000, 0, 0, 0, 0 } },
  { "Dark Techno Ritual",
    { 0x1111, 0x1010, 0x5555, 0x0000, 0x8888, 0x0100, 0x0000, 0x0001 },
    { 9500, 8500, 14500, 0, 7500, 7000, 13000, 7000, 8192, 4500, 9000, 5000, 0, 0, 0, 0,
      7000, 4500, 10500, 2000, 8500, 6000, 12000, 4000, 0, 0, 0, 0, 12000, 0, 0, 0 } },
  { "Club Tribal Drive",
    { 0x1111, 0x4040, 0x5555, 0x0404, 0x9249, 0x2222, 0x4444, 0x0000 },
    { 9000, 8500, 13500, 0, 8192, 7500, 12000, 7000, 8192, 4000, 9000, 5000, 7000, 4000, 11000, 0,
      7500, 5000, 10000, 3000, 7000, 4500, 9500, 3500, 7000, 4000, 9000, 3000, 0, 0, 0, 0 } },
  { "Slow Burn Doom",
    { 0x0101, 0x4010, 0x1111, 0x0000, 0x8800, 0x0000, 0x0404, 0x0101 },
    { 10000, 9000, 15000, 0, 8192, 8000, 14000, 7000, 8192, 5000, 9000, 5500, 0, 0, 0, 0,
      7500, 4500, 11000, 2000, 0, 0, 0, 0, 7000, 4500, 9500, 3000, 11000, 0, 13000, 0 } }
};

} /* anonymous namespace */

/* ═══════════════════════════════════════════════════════════════════════════
 * PUBLIC STATE + API
 * ═══════════════════════════════════════════════════════════════════════════ */

/* ── 9. Grid cursor state (declared extern in groovebox.h) ───────────────── */
std::atomic<int> seqUI_row{ 0 };
std::atomic<int> seqUI_col{ 0 };
std::atomic<int> seqUI_page{ 0 }; /* 0 = synth rows 0-7, 1 = drum rows 8-15 */

/* ═══════════════════════════════════════════════════════════════════════════
 * 10. GRID EDITOR NAVIGATION + RENDER
 * Vertical scroll wraps synth(0-7) → drum(0-7) → adjacent bank, mirroring the
 * horizontal L/R bank wrap so both axes feel continuous.
 * ═══════════════════════════════════════════════════════════════════════════ */
void seqUI_moveUp() {
  const int row = seqUI_row.load(std::memory_order_relaxed);
  if (row > 0) { seqUI_row.store(row - 1, std::memory_order_relaxed); return; }
  seqUI_row.store(7, std::memory_order_relaxed);
  if (seqUI_page.load(std::memory_order_relaxed) == 1) {
    seqUI_page.store(0, std::memory_order_relaxed);  /* drum top → same-bank synth bottom */
    displayDirty.store(true, std::memory_order_release);
  } else {
    seqUI_page.store(1, std::memory_order_relaxed);  /* synth top → prev-bank drum bottom */
    seqUI_setBank(seqUI_gridBank() - 1);
  }
}

void seqUI_moveDown() {
  const int row = seqUI_row.load(std::memory_order_relaxed);
  if (row < 7) { seqUI_row.store(row + 1, std::memory_order_relaxed); return; }
  seqUI_row.store(0, std::memory_order_relaxed);
  if (seqUI_page.load(std::memory_order_relaxed) == 0) {
    seqUI_page.store(1, std::memory_order_relaxed);  /* synth bottom → same-bank drum top */
    displayDirty.store(true, std::memory_order_release);
  } else {
    seqUI_page.store(0, std::memory_order_relaxed);  /* drum bottom → next-bank synth top */
    seqUI_setBank(seqUI_gridBank() + 1);
  }
}

void seqUI_moveLeft() {
  const int col = seqUI_col.load(std::memory_order_relaxed);
  if (col > 0) { seqUI_col.store(col - 1, std::memory_order_relaxed); return; }
  seqUI_col.store(15, std::memory_order_relaxed);
  seqUI_setBank(seqUI_gridBank() - 1);
}

void seqUI_moveRight() {
  const int col = seqUI_col.load(std::memory_order_relaxed);
  if (col < 15) { seqUI_col.store(col + 1, std::memory_order_relaxed); return; }
  seqUI_col.store(0, std::memory_order_relaxed);
  seqUI_setBank(seqUI_gridBank() + 1);
}

void seqUI_selectBank(int bank) {
  if (bank >= 0 && bank <= 3) seqUI_setBank(bank);
}

void seqUI_toggleStep() {
  const int page = seqUI_page.load(std::memory_order_relaxed) & 1;
  /* [FIX-OOB] Clamp before cast: an out-of-range atomic value (corrupt encoder
   * ISR) would silently wrap to uint8 and index hwSeqData[bank][chain][18+],
   * overwriting adjacent memory.  toggleHardwareGridStep bounds-checks too, but
   * an unsigned-wrap before that call bypasses it.                              */
  const int row = std::max(0, std::min(7,  seqUI_row.load(std::memory_order_relaxed)));
  const int col = std::max(0, std::min(15, seqUI_col.load(std::memory_order_relaxed)));
  const int hwRow = (page == 0) ? row : (row + 8);
  toggleHardwareGridStep((uint8_t)hwRow, (uint8_t)col);
}

/* ── Matrix grid renderer ────────────────────────────────────────────────── */
void seqUI_renderMatrix() {
  constexpr int STEP_W = 128 / 16;
  constexpr int STEP_H = 56 / 8;

  const int bank     = seqUI_gridBank();
  const int page     = seqUI_page.load(std::memory_order_relaxed) & 1;
  const int curRow   = seqUI_row.load(std::memory_order_relaxed);
  const int curCol   = seqUI_col.load(std::memory_order_relaxed);
  const int playhead = (int)seqCurrentStep.load(std::memory_order_relaxed) & 15;
  const int length   = std::max(1, std::min(16,
                                  (int)seqLength.load(std::memory_order_relaxed)));
  const bool playing = seqPlaying.load(std::memory_order_relaxed);

  /* [gbox OPT-7] Prefetch the 8 row words once instead of 128 per-cell function
   * calls; the cell test below is a single bitwise AND. No behaviour change.    */
  uint64_t rowData[8];
  for (int r = 0; r < 8; ++r) {
    const int hwRow = (page == 0) ? r : (r + 8);
    rowData[r] = hwSeqData[bank][SEQ_UI_CHAIN][hwRow];
  }

  for (int row = 0; row < 8; ++row) {
    for (int col = 0; col < 16; ++col) {
      const int x = col * STEP_W;
      const int y = 9 + row * STEP_H;

      if (col >= length) {
        display.drawRect(x + 1, y + 1, STEP_W - 2, STEP_H - 2, SH110X_WHITE);
        display.drawLine(x + 1, y + 1, x + STEP_W - 2, y + STEP_H - 2, SH110X_WHITE);
        continue;
      }

      const bool active   = (rowData[row] & (1ull << col)) != 0;
      const bool isCursor = (row == curRow && col == curCol);

      if (active) display.fillRect(x + 1, y + 1, STEP_W - 2, STEP_H - 2, SH110X_WHITE);
      else        display.drawRect(x + 1, y + 1, STEP_W - 2, STEP_H - 2, SH110X_WHITE);

      if (col == playhead && playing)
        display.drawFastHLine(x + 2, y + STEP_H - 2, STEP_W - 4, SH110X_WHITE);
      if (isCursor)
        display.drawRect(x, y, STEP_W, STEP_H, SH110X_WHITE);
    }
  }

  /* ── Top status bar — title + live cursor read-out CONSOLIDATED ──────────────
   * The R/C/STEP status used to sit on its OWN bottom row (y=57) where it
   * overdrew the lowest grid steps.  It now shares the inverted top bar with the
   * bank/page tag in one compact ≤21-char line ("A SYN R1C01 S01/16"), so it is
   * always visible AND the full 8×16 grid is free of text overlap.  Cursor-aware:
   * the bank/page + R/C reflect wherever the edit cursor is.                     */
  display.fillRect(0, 0, 128, 8, SH110X_WHITE);
  display.setTextColor(SH110X_BLACK);
  display.setCursor(2, 0);
  display.printf("%c %s R%dC%02d S%02d/%d",
                 'A' + bank, page == 0 ? "SYN" : "DRM",
                 curRow + 1, curCol + 1, playhead + 1, length);
  display.setTextColor(SH110X_WHITE);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 11. TRANSPORT
 * ═══════════════════════════════════════════════════════════════════════════ */
/* Song-mode rewind  [SONG-FIX] — public API (groovebox.h); must live outside the
 * anonymous namespace so midi.cpp / seq_start resolve the same symbol.          */
void IRAM_ATTR song_rewind_rt() {
  if (!songModeActive.load(std::memory_order_relaxed)) return;
  songCurrentStep.store(0u, std::memory_order_relaxed);
  songCurrentRepeat.store(0u, std::memory_order_relaxed);

  const uint8_t   slot = activeSongSlot.load(std::memory_order_relaxed) & 15u;
  const SongSlot& song = hwSongData[slot];
  if (song.numSteps == 0u) return;
  const SongStep& s0 = song.steps[0];
  if (s0.repeats == 0u) return; /* first step inactive — keep current bank */

  seqActiveBank.store((uint8_t)(s0.bank & 15u), std::memory_order_release);
  seqActiveChain.store(0u, std::memory_order_release);
  displayDirty.store(true, std::memory_order_relaxed);
  seq_ext_push(CMD_BANK,     (uint16_t)(s0.bank & 15u));
  seq_ext_push(CMD_SONG_POS, 0u);
}

/* Tempo + step timing are owned entirely by the DMA-locked step engine
 * (sequencer_render_block): it accumulates musical ticks from esp_timer deltas
 * scaled by seqBpm, paced by the audio task's per-buffer cadence.  No external
 * clock object is involved — seqBpm is the single source of tempo truth.       */
void seq_start() {
  master_tick_counter.store(0u, std::memory_order_release);
  std::atomic_thread_fence(std::memory_order_release);
  song_rewind_rt();   /* [SONG-FIX] preload the song's first pattern before play */
  seqPlaying.store(true, std::memory_order_release);
  /* Transport echoes via seq_ext ring — see §2 out-ring comment.               */
  seq_ext_push(CMD_TRIG_MODE, 16383u);
  seq_ext_push(CMD_TRANSPORT, 1u);   /* mirror play state to App transport buttons */
  seq_ext_push(CMD_STEP_SYNC, 0u);   /* prime playhead at step 0 before first audio block */
  displayDirty.store(true, std::memory_order_relaxed);
}

void seq_stop() {
  /* Echo final step before clearing play state — App/OLED keep this position when
   * stopped (same as seqCurrentStep on hardware).  seq_start() resets to 0. */
  const uint16_t holdStep = seqCurrentStep.load(std::memory_order_relaxed) & 63u;
  seqPlaying.store(false, std::memory_order_release);
  /* Disarm record inline — not seq_set_recording(): stop must push play-off (0)
   * before record-off (4) in one ordered ring burst (see echoes below).        */
  seqRecording.store(false, std::memory_order_relaxed);
  std::atomic_thread_fence(std::memory_order_release);
  /* [FIX-ARP-RACE] Use request_reset() instead of reset(): seq_stop() may be
   * called from the MIDI task (Core 1) while the audio task (Core 0) reads
   * s_seqArp fields in seqArpTick().  Direct reset() of non-atomic fields is
   * a data race.  request_reset() sets an atomic flag; the audio task applies
   * the actual reset at the start of its next buffer via check_reset().     */
  s_seqArp.request_reset();
  for (int i = 0; i < SEQ_POLYPHONY; ++i) release_seq_note(i);
  allNotesOff();
  seq_ext_push(CMD_STEP_SYNC, holdStep);
  seq_ext_push(CMD_TRIG_MODE, 0u);
  seq_ext_push(CMD_TRANSPORT, 0u);  /* play off  */
  seq_ext_push(CMD_TRANSPORT, 4u);  /* record off — after play off, decoupled   */
  displayDirty.store(true, std::memory_order_relaxed);
}

void seq_pause() {
  seqPlaying.store(false, std::memory_order_release);
  std::atomic_thread_fence(std::memory_order_release);
  seq_ext_push(CMD_TRANSPORT, 2u);
  displayDirty.store(true, std::memory_order_relaxed);
}

void seq_toggle() {
  seqPlaying.load(std::memory_order_relaxed) ? seq_stop() : seq_start();
}

/* Record arm/disarm — canonical mutation path (hardware + App SysEx).
 * Never pair seqRecording.store() with direct txSysex(); use these instead.  */
void seq_set_recording(bool on) {
  if (seqRecording.load(std::memory_order_relaxed) == on) return;
  seqRecording.store(on, std::memory_order_relaxed);
  seq_ext_push(CMD_TRANSPORT, on ? 3u : 4u);
  displayDirty.store(true, std::memory_order_relaxed);
}

void seq_toggle_recording() {
  seq_set_recording(!seqRecording.load(std::memory_order_relaxed));
}

void IRAM_ATTR seq_restart_from_step_zero() {
  /* No-op when stopped — the next seq_start() always resets the counter. */
  if (!seqPlaying.load(std::memory_order_relaxed)) return;
  master_tick_counter.store(0u, std::memory_order_release);
  std::atomic_thread_fence(std::memory_order_release);
  seq_ext_push(CMD_STEP_SYNC, 0u); /* snap App playhead to step 0 instantly */
}

void setSequencerBpm(int32_t bpm) {
  bpm = std::min<int32_t>(240, std::max<int32_t>(40, bpm));
  seqBpm.store(bpm, std::memory_order_relaxed);
  s_lastBpmChangeMs.store(millis(), std::memory_order_relaxed);
  seq_ext_push(CMD_BPM, (uint16_t)bpm); /* coalesced hi slot → SeqSysexOut */
}

void initSequencer() {
  /* DMA-locked engine needs no clock-object init; the step engine reads seqBpm
   * directly each buffer.  Transport starts explicitly via seq_start().  Kept
   * as a named boot phase for the .ino setup sequence.                         */
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 12. STEP ENGINE — runs in audio_synthesis_task, clocked by DMA frame count.
 * ═══════════════════════════════════════════════════════════════════════════ */
void IRAM_ATTR sequencer_render_block(uint32_t frames) {
  static float    fracTicks      = 0.0f;  /* [gbox OPT-1] float — LX7 has no double FPU */
  static uint64_t lastRenderUs   = 0;
  static uint32_t lastStep       = 0xFFFFFFFFu;
  static bool     halfGateFired  = false;
  static uint16_t lastMelodyMask = 0;
  static uint16_t lastDrumMask   = 0;

  const uint64_t nowUs   = (uint64_t)esp_timer_get_time();
  const uint32_t nomUs   = (uint32_t)((uint64_t)frames * 1000000ull / (uint64_t)SAMPLE_RATE);
  const uint32_t elapsedUs = (lastRenderUs == 0)
                               ? 0u
                               : (uint32_t)std::min<uint64_t>(nowUs - lastRenderUs,
                                                              (uint64_t)nomUs * 4u);
  lastRenderUs = nowUs;

  if (!seqPlaying.load(std::memory_order_relaxed)) {
    fracTicks = 0.0f;
    lastStep = 0xFFFFFFFFu;
    halfGateFired = false;
    lastMelodyMask = lastDrumMask = 0;
    /* Safely apply any pending cross-core reset (audio task context). */
    s_seqArp.reset_safe();
    return;
  }

  const uint32_t bpm = (uint32_t)std::min<int32_t>(240, std::max<int32_t>(40,
                          seqBpm.load(std::memory_order_relaxed)));
  /* [gbox OPT-1] (float)TICKS_PER_BEAT * (1/60e6f) folds to a compile-time const;
   * float mantissa (24 bits) resolves the ≤81.92 ticks/call to <1e-5 tick.      */
  fracTicks += (float)elapsedUs * ((float)bpm * (float)(TICKS_PER_BEAT) * (1.0f / 60000000.0f));
  const uint32_t iTicks = (uint32_t)fracTicks;
  fracTicks -= (float)iTicks;
  if (iTicks) master_tick_counter.fetch_add(iTicks, std::memory_order_relaxed);

  const uint32_t absoluteTick = master_tick_counter.load(std::memory_order_relaxed);
  const uint32_t stepPhase    = absoluteTick % TICKS_PER_STEP;
  const uint16_t step         = get_current_sequencer_step();
  seqCurrentStep.store((uint8_t)(step & 0xFFu), std::memory_order_relaxed);

  if (g_saveArmed.load(std::memory_order_acquire)) return;

  const bool arpActive = seqArpEnabled.load(std::memory_order_relaxed)
                         && s_seqArp.count > 0;

  if (!halfGateFired && stepPhase >= TICKS_PER_STEP / 2) {
    halfGateFired = true;
    const bool show = laserShowMode.load(std::memory_order_relaxed);
    if (!arpActive) {
      for (int r = 0; r < 8; ++r) {
        if (lastMelodyMask & (1u << r)) {
          release_seq_note(r);
          seqVoiceOwner[r] = HV_FREE;
          if (show) harpHueNoteOff(r);
        }
      }
      lastMelodyMask = 0;
    }
    lastDrumMask = 0;
  }
  if (stepPhase < TICKS_PER_STEP / 2) halfGateFired = false;

  if (step != (uint16_t)(lastStep & 0xFFFFu)) {
    /* [SONG-FIX] Distinguish the very first downbeat after seq_start (lastStep
     * sentinel) from a true pattern wrap.  Advancing the song chain on the first
     * downbeat skipped any step with repeats==1 before it ever played. */
    const bool firstDownbeat = (lastStep == 0xFFFFFFFFu);
    lastStep = step;

    if (step == 0u && !firstDownbeat) song_advance_rt();

    seq_ext_push(CMD_STEP_SYNC, step);

    const uint8_t bank     = seqActiveBank.load(std::memory_order_relaxed) & 15u;
    const uint8_t chain    = seqActiveChain.load(std::memory_order_relaxed) & 3u;
    const uint8_t gridStep = (uint8_t)(step & 63u);   /* [GRID-64] full 0..63 mask  */
    const uint8_t motStep  = (uint8_t)(step & (MOTION_STEPS_PER_LANE - 1u));
    const uint8_t vel      = (gridStep % 4u == 0u) ? SEQ_VEL_ACCENT : SEQ_VEL_NORMAL;

    uint64_t rowSnap[16];
    portENTER_CRITICAL(&patchMux);
    for (int r = 0; r < 16; ++r) rowSnap[r] = hwSeqData[bank][chain][r];
    portEXIT_CRITICAL(&patchMux);

    {
      uint8_t  motCmds[4];
      uint16_t motVals[4];
      bool     motActive[4] = {};

      portENTER_CRITICAL(&motionMux);
      for (int l = 0; l < 4; ++l) {
        const uint8_t mc = hwMotionData[bank][chain][l].targetCmd;
        if (mc == 255u) continue;
        const uint16_t mv = hwMotionData[bank][chain][l].steps[motStep];
        if (mv == 0xFFFFu) continue;
        /* Legacy P-locks may hold full-scale values on discrete layout cmds — skip. */
        if (mc == CMD_TRANSPOSE && mv > 24u) continue;
        if ((mc == CMD_HW_H_OCT || mc == CMD_HW_S_OCT) && mv > 8u) continue;
        if (mc == CMD_HW_S_LEN && (mv < 1u || mv > 64u)) continue;
        /* [FIX-PLOCK] Validate discrete-layout bank and chain cmds so a corrupt
         * automation entry cannot pass an out-of-range value to applySeqBank /
         * applySeqChain, which index into hwSeqData[bank][chain].              */
        if (mc == CMD_BANK && mv > 15u) continue;
        if (mc == CMD_SEQ_CHAIN && mv > 3u) continue;
        motCmds[l]   = mc;
        motVals[l]   = mv;
        motActive[l] = true;
      }
      portEXIT_CRITICAL(&motionMux);

      for (int l = 0; l < 4; ++l) {
        if (!motActive[l]) continue;
        isMotionPlayback.store(true, std::memory_order_release);
        seq_apply_motion(motCmds[l], motVals[l]);
        isMotionPlayback.store(false, std::memory_order_release);
        /* [MOTION-ECHO] Mirror the played-back P-lock value to the App so the
         * matching knob/button animates live during playback.  targetCmd IS the
         * wire SysEx cmd (harp 0-15 / seq 16-31 / drum 32-63 / FX ≥64), exactly
         * what applyIncoming() expects, so no translation is needed.  Reuses the
         * lock-free out-ring + SeqSysexOut drain (off the audio core, same path
         * as STEP_SYNC) and the App applies it with _suppressTx → no echo loop. */
        seq_ext_push(motCmds[l], motVals[l]);
      }
    }

    uint16_t newMelodyMask = 0;
    if (!mixSeqMute.load(std::memory_order_relaxed)) {
      const int si    = harpScaleIndex.load(std::memory_order_relaxed) & (NUM_SCALES - 1);
      const int trans = seqTranspose.load(std::memory_order_relaxed);
      const int oct   = (int)octaveShift[1].load(std::memory_order_relaxed);
      const int cap = std::min((int)SEQ_POLYPHONY, std::max(1,
                        g_seq_voice_cap.load(std::memory_order_relaxed)));
      const uint16_t vel16 = (uint16_t)((uint32_t)vel * (uint32_t)Q15_ONE / 127u);
      const bool arpOn = seqArpEnabled.load(std::memory_order_relaxed);

      int8_t latchMidi[8];
      int8_t latchRows[8];
      int latchN = 0;

      int sounded = 0;
      for (int row = 0; row < 8; ++row) {
        if (!(rowSnap[row] & (1ull << gridStep))) continue;

        const uint8_t midiNote = (uint8_t)std::min(127, std::max(0,
            (int)SCALES_NOTES[si][7 - row] + trans + oct * 12));

        if (arpOn) {
          if (latchN < 8) {
            latchMidi[latchN]  = (int8_t)midiNote;
            latchRows[latchN]  = (int8_t)row;
            ++latchN;
          }
          continue;
        }

        if (sounded >= cap) break;
        ++sounded;

        trigger_seq_note(row, midi_note_to_freq(midiNote), vel16);
        seqVoiceOwner[row] = (int16_t)midiNote;
        newMelodyMask |= (uint16_t)(1u << row);

        if (laserShowMode.load(std::memory_order_relaxed)) {
          g_showBeamNote[row].store((int8_t)midiNote, std::memory_order_relaxed);
          harpHueNoteOn(row);
        }
      }

      if (arpOn) {
        s_seqArpVelQ15 = vel16;
        const bool show = laserShowMode.load(std::memory_order_relaxed);
        for (int r = 0; r < 8; ++r) {
          release_seq_note(r);
          seqVoiceOwner[r] = HV_FREE;
        }
        seqArpCommitLatch(latchMidi, latchRows, latchN, show);
      }
    }
    lastMelodyMask = newMelodyMask;

    uint16_t newDrumMask = 0;
    if (!mixDrumsMute.load(std::memory_order_relaxed)) {
      for (int row = 0; row < 8; ++row) {
        if (!(rowSnap[row + 8] & (1ull << gridStep))) continue;
        fire_tuned_drum(row, (float)vel / 127.0f);
        newDrumMask |= (uint16_t)(1u << row);
      }
      /* [LASER-SHOW v2] Any drum hit on this step fires a global white flash
       * across the laser fan (depth set by Drum Flash).  Stamp the timestamp the
       * laser core reads to derive a linear-decay flash level — lock-free. */
      if (newDrumMask && laserShowMode.load(std::memory_order_relaxed))
        g_showDrumFlashUs.store((uint32_t)esp_timer_get_time(), std::memory_order_relaxed);
    }
    lastDrumMask = newDrumMask;
  }

  if (seqArpEnabled.load(std::memory_order_relaxed)) {
    seqArpTick(absoluteTick, laserShowMode.load(std::memory_order_relaxed));
  }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 13. OUTBOUND DRAIN — emits jitter-tolerant MIDI/sysex off the audio core.
 * ═══════════════════════════════════════════════════════════════════════════ */
void sequencer_background_task(void* pvParameters) {
  (void)pvParameters;
  while (!g_systemReady.load(std::memory_order_acquire))
    vTaskDelay(pdMS_TO_TICKS(1));

  uint32_t lastSupervisorMs = 0u;
  uint32_t lastPhaseMs      = 0u;
  for (;;) {
    midi_drain_tx_retry();

    /* Critical coalesced slots first — never starved by bulk ring flood. */
    seq_ext_drain_critical();

    /* Bulk ring: bounded per iteration so hi slots + supervisor get CPU. */
    {
      unsigned budget = 12u;
      SeqExtEvt e;
      while (budget && seq_ext_pop(e)) {
        txSysex(e.cmd, e.v14);
        --budget;
      }
    }

    /* ── [v6.0 SYNC-SUPERVISOR] ──────────────────────────────────────────────
     * Re-assert transport STATE every 600 ms (above txSysex 500 ms dedup window).
     * Uses coalesced hi slots so re-asserts cannot be dropped by ring overflow. */
    const uint32_t now = millis();
    if ((uint32_t)(now - lastSupervisorMs) >= 600u && isAppConnected()) {
      lastSupervisorMs = now;
      seq_ext_push(CMD_BPM, (uint16_t)seqBpm.load(std::memory_order_relaxed));
      seq_ext_push(CMD_TRANSPORT, seqPlaying.load(std::memory_order_relaxed) ? 1u : 0u);
      seq_ext_push(CMD_TRANSPORT, seqRecording.load(std::memory_order_relaxed) ? 3u : 4u);
      /* While stopped, re-echo step position so the App playhead stays visible
       * (no PLL glide — just a static column).  Omitted while playing: duplicates
       * reset the PLL anchor and cause visible backward playhead strokes.       */
      if (!seqPlaying.load(std::memory_order_relaxed))
        seq_ext_push(CMD_STEP_SYNC,
                     (uint16_t)(seqCurrentStep.load(std::memory_order_relaxed) & 63u));
      /* bits 0–6: load %; 7–13: bulk ring drops (mod 128); 14: P-lock lane steal */
      const uint16_t loadPct = (uint16_t)g_audio_load_pct.load(std::memory_order_relaxed);
      const uint16_t drops   = (uint16_t)(g_seq_ext_drops.load(std::memory_order_relaxed) & 127u);
      uint16_t cpuV = (uint16_t)((loadPct & 0x7Fu) | (drops << 7));
      if (g_plock_lane_steal.exchange(false, std::memory_order_acq_rel))
        cpuV |= (1u << 14);
      txSysex(CMD_CPU_LOAD, cpuV);
    }

    /* ── [SUBSTEP] Sub-step phase echo (~20 Hz while playing) ─────────────────
     * Tells the App where WITHIN the current step the sample-locked clock is, so
     * its PLL anchors interpolation to the true phase (correcting USB/drain
     * latency) rather than restarting each step at frac 0.  One tick read drives
     * both fields so step and phase are coherent.  Decoupled from the per-step
     * STEP_SYNC path; far below per-step traffic, so it adds no meaningful load.
     * The App treats it as a refinement only (ignored unless its step matches). */
    if ((uint32_t)(now - lastPhaseMs) >= 50u &&
        seqPlaying.load(std::memory_order_relaxed) && isAppConnected()) {
      lastPhaseMs = now;
      const uint32_t tick = master_tick_counter.load(std::memory_order_relaxed);
      const uint32_t len  = std::max<uint32_t>(1u,
                              (uint32_t)seqLength.load(std::memory_order_relaxed));
      const uint8_t  step = (uint8_t)((tick / TICKS_PER_STEP) % len);
      const uint8_t  ph8  = (uint8_t)(((tick % TICKS_PER_STEP) * 256u) / TICKS_PER_STEP);
      txSysex(CMD_STEP_PHASE, (uint16_t)(((uint16_t)(step & 0x3Fu) << 8) | ph8));
    }
    vTaskDelay(pdMS_TO_TICKS(2));
  }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 14. PATTERN MANAGEMENT  (was patterns.h — now with sanitizePatch on loads)
 * ═══════════════════════════════════════════════════════════════════════════ */
void loadFactorySynthPattern(int bankIdx, int chainIdx, int presetIdx) {
  if ((unsigned)bankIdx >= 16u || (unsigned)chainIdx >= 4u ||
      (unsigned)presetIdx >= (unsigned)NUM_SYNTH_PATS) return;
  /* [SEQ-LOAD-FIX] Single source of truth for the displayed/active preset index:
   * set it HERE so the OLED readout, the menu encoder, and App-driven loads all
   * agree (previously only the menu path stored it via a private static). */
  g_lastSynthPreset.store((uint8_t)presetIdx, std::memory_order_relaxed);

  SynthPatternROM pat;
  memcpy_P(&pat, &SYNTH_PATTERNS[presetIdx], sizeof(SynthPatternROM));

  const bool isActive =
    (seqActiveBank.load(std::memory_order_relaxed)  == (uint8_t)bankIdx &&
     seqActiveChain.load(std::memory_order_relaxed) == (uint8_t)chainIdx);

  /* [gbox OPT-6] [U1] nearest-row pitch placement — computed with NO lock held;
   * the critical section below shrinks to 8 stores (+ optional memcpy).        */
  uint64_t rowBits[8] = {};
  for (int step = 0; step < 16; ++step) {
    if (!pat.gate[step]) continue;
    const int p = std::max(-12, std::min(12, (int)pat.pitch[step]));
    int bestRow = 7, bestDst = 999;
    for (int r = 0; r < 8; ++r) {
      const int d = std::abs(p - kRowSemitone[r]);
      if (d < bestDst) { bestDst = d; bestRow = r; }
    }
    rowBits[bestRow] |= (1ull << step);
  }

  portENTER_CRITICAL(&patchMux);
  for (int r = 0; r < 8; ++r) hwSeqData[bankIdx][chainIdx][r] = rowBits[r];

  if (isActive) {
    memcpy(seqLivePatch, pat.preset, sizeof(pat.preset));
    sanitizePatch(seqLivePatch); /* companion preset is engine-safe before it sounds */
    seqLivePatchVersion.store(
      (seqLivePatchVersion.load(std::memory_order_relaxed) + 1u) & 0xFFu,
      std::memory_order_release);
  }
  portEXIT_CRITICAL(&patchMux);
  /* [SEQ-SOUND-PERSIST] The companion preset above changes the LIVE seq sound
   * (seqLivePatch), but NVS saves from the seq atomic mirrors — so without this
   * fan-out the loaded melody's sound is audible yet never persists (it reverts
   * to the stale atomics on reboot).  Mirror the live patch into the atomics so
   * SAVE captures exactly what is playing.  (Atomics are lock-free → outside the
   * patchMux critical section, matching recallSeqPatch.)                        */
  if (isActive) syncSeqAtomicsFromLivePatch();
  std::atomic_thread_fence(std::memory_order_release);
  /* [GAP-3] Echo synth rows 0-7 to App so its grid reflects the loaded pattern.
   * [GAP-5] Echo pattern index so App synthPat dropdown updates.
   * echoGridRow defined in patches.h; only sends if App is connected.    */
  if (isAppConnected()) {
    for (uint8_t r = 0; r < 8u; ++r)
      echoGridRow((uint8_t)(bankIdx & 15), r);
    txSysex(CMD_LOAD_PAT_S, (uint16_t)(presetIdx % NUM_SYNTH_PATS));
  }
}

void loadFactoryDrumPattern(int bankIdx, int chainIdx, int presetIdx) {
  if ((unsigned)bankIdx >= 16u || (unsigned)chainIdx >= 4u ||
      (unsigned)presetIdx >= (unsigned)NUM_DRUM_PATS) return;
  /* [SEQ-LOAD-FIX] Single source of truth for the displayed/active preset index. */
  g_lastDrumPreset.store((uint8_t)presetIdx, std::memory_order_relaxed);

  DrumPatternROM drum;
  memcpy_P(&drum, &DRUM_PATTERNS[presetIdx], sizeof(DrumPatternROM));

  const bool isActive =
    (seqActiveBank.load(std::memory_order_relaxed)  == (uint8_t)bankIdx &&
     seqActiveChain.load(std::memory_order_relaxed) == (uint8_t)chainIdx);

  portENTER_CRITICAL(&patchMux);
  for (int trk = 0; trk < 8; ++trk)
    hwSeqData[bankIdx][chainIdx][trk + 8] = (uint64_t)drum.tracks[trk];
  if (isActive) {
    memcpy(drumLivePatch, drum.preset, sizeof(drum.preset));
    drumLivePatchVersion.store(
      (drumLivePatchVersion.load(std::memory_order_relaxed) + 1u) & 0xFFu,
      std::memory_order_release);
  }
  portEXIT_CRITICAL(&patchMux);
  std::atomic_thread_fence(std::memory_order_release);
  /* [GAP-3] Echo drum rows 8-15 to App so its grid reflects the loaded pattern.
   * [GAP-5] Echo pattern index so App drumPat dropdown updates.           */
  if (isAppConnected()) {
    for (uint8_t r = 8u; r < 16u; ++r)
      echoGridRow((uint8_t)(bankIdx & 15), r);
    txSysex(CMD_LOAD_PAT_D, (uint16_t)(presetIdx % NUM_DRUM_PATS));
  }
}

/* [USER-PAT-SLOTS] Snapshot the ACTIVE bank/chain (melody rows 0–7, drum rows
 * 8–15, companion seq-synth + drum sounds, transpose) into a user library slot,
 * then persist usrpat + usrpatnames via BANKS_PATTERNS scope (no reboot).      */
bool saveActivePatternToUserSlot(uint8_t uidx) {
  uidx %= (uint8_t)NUM_USER_PAT_SLOTS;
  const uint8_t bank  = seqActiveBank .load(std::memory_order_relaxed) & 15u;
  const uint8_t chain = seqActiveChain.load(std::memory_order_relaxed) & 3u;

  UserPatternSlot& slot = g_userPat[uidx];
  portENTER_CRITICAL(&patchMux);
  memcpy(slot.synthRows, &hwSeqData[bank][chain][0], 8u * sizeof(uint64_t));
  memcpy(slot.drumRows,  &hwSeqData[bank][chain][8], 8u * sizeof(uint64_t));
  slot.transpose = seqPatternTranspose[bank][chain];
  memcpy(slot.synthPreset, seqLivePatch, sizeof(slot.synthPreset));
  memcpy(slot.drumPreset,  drumLivePatch, sizeof(slot.drumPreset));
  slot.flags = 1u;
  portEXIT_CRITICAL(&patchMux);

  displayDirty.store(true, std::memory_order_relaxed);
  /* [FIX-L2b] Same fix as saveLiveToUserSlot: replace blocking persist with
   * async NvsWorker so this function does not freeze the MIDI RX task
   * (8 KB stack) for up to 20 s while NVS writes.  Copy is already safe in
   * RAM under patchMux above; persistence happens asynchronously.
   * Echo the slot index immediately so the App marks it occupied.          */
  requestBanksOnlySave();
  if (isAppConnected())
    txSysex(CMD_USR_PAT_SAVE, (uint16_t)uidx);
  return true;
}

/* Recall a user pattern slot into the ACTIVE bank/chain (non-destructive to
 * other banks).  Empty slots are ignored.  Mirrors factory load echo paths.   */
void loadUserPatternToActive(uint8_t uidx) {
  uidx %= (uint8_t)NUM_USER_PAT_SLOTS;
  const UserPatternSlot& src = g_userPat[uidx];
  if (!(src.flags & 1u)) return;

  const uint8_t bank  = seqActiveBank .load(std::memory_order_relaxed) & 15u;
  const uint8_t chain = seqActiveChain.load(std::memory_order_relaxed) & 3u;

  portENTER_CRITICAL(&patchMux);
  memcpy(&hwSeqData[bank][chain][0], src.synthRows, 8u * sizeof(uint64_t));
  memcpy(&hwSeqData[bank][chain][8], src.drumRows,  8u * sizeof(uint64_t));
  seqPatternTranspose[bank][chain] = src.transpose;
  memcpy(seqLivePatch, src.synthPreset, sizeof(src.synthPreset));
  sanitizePatch(seqLivePatch);
  seqLivePatchVersion.store(
    (seqLivePatchVersion.load(std::memory_order_relaxed) + 1u) & 0xFFu,
    std::memory_order_release);
  memcpy(drumLivePatch, src.drumPreset, sizeof(src.drumPreset));
  drumLivePatchVersion.store(
    (drumLivePatchVersion.load(std::memory_order_relaxed) + 1u) & 0xFFu,
    std::memory_order_release);
  portEXIT_CRITICAL(&patchMux);

  syncSeqAtomicsFromLivePatch();
  applySeqTranspose((uint16_t)((int)src.transpose + 12));
  std::atomic_thread_fence(std::memory_order_release);
  displayDirty.store(true, std::memory_order_relaxed);

  if (isAppConnected()) {
    for (uint8_t r = 0; r < 16u; ++r)
      echoGridRow(bank, r);
    txPatchBlob(1u);
    for (uint8_t i = 0; i < 32u; ++i)
      txSysex((uint8_t)(32u + i), drumLivePatch[i]);
    txSysex(CMD_USR_PAT_LOAD, (uint16_t)uidx);
  }
}

void loadFactorySynthPreset(uint8_t presetIdx) {
  if (presetIdx >= (uint8_t)NUM_SYNTH_PATS) return;
  loadFactorySynthPattern(
    (int)(seqActiveBank.load(std::memory_order_relaxed)  & 0x0Fu),
    (int)(seqActiveChain.load(std::memory_order_relaxed) & 0x03u),
    (int)presetIdx);
}

void loadFactoryDrumPreset(uint8_t presetIdx) {
  if (presetIdx >= (uint8_t)NUM_DRUM_PATS) return;
  loadFactoryDrumPattern(
    (int)(seqActiveBank.load(std::memory_order_relaxed)  & 0x0Fu),
    (int)(seqActiveChain.load(std::memory_order_relaxed) & 0x03u),
    (int)presetIdx);
}

void autoCloneSynthPatternToBank(uint8_t bankIdx, uint8_t presetIdx) {
  if (bankIdx >= 16u || presetIdx >= (uint8_t)NUM_SYNTH_PATS) return;
  loadFactorySynthPattern((int)(bankIdx & 0x0Fu), 0, (int)presetIdx);
}

void autoCloneDrumPatternToBank(uint8_t bankIdx, uint8_t presetIdx) {
  if (bankIdx >= 16u || presetIdx >= (uint8_t)NUM_DRUM_PATS) return;
  loadFactoryDrumPattern((int)(bankIdx & 0x0Fu), 0, (int)presetIdx);
}

void clearPatternSlot(uint8_t bankIdx, uint8_t chainIdx) {
  if (bankIdx >= 16u || chainIdx >= 4u) return;
  portENTER_CRITICAL(&patchMux);
  for (int r = 0; r < 16; ++r) hwSeqData[bankIdx][chainIdx][r] = 0;
  portEXIT_CRITICAL(&patchMux);
  displayDirty.store(true, std::memory_order_relaxed);
}

void clearActivePattern() {
  clearPatternSlot(
    seqActiveBank.load(std::memory_order_relaxed)  & 0x0Fu,
    seqActiveChain.load(std::memory_order_relaxed) & 0x03u);
}

void clearAllPatterns() {
  /* [gbox OPT-5] memset over the nested element loops — one word-aligned sweep
   * each, ~50-100× shorter critical sections (audio task spin-wait window).    */
  portENTER_CRITICAL(&patchMux);
  memset(hwSeqData, 0, sizeof(hwSeqData));            /* zero entire step grid */
  portEXIT_CRITICAL(&patchMux);

  /* 0xFF fills BOTH targetCmd (uint8 255 = empty) AND steps[] (uint16 0xFFFF =
   * no automation) in a single sweep — matches the original sentinel fill.     */
  portENTER_CRITICAL(&motionMux);
  memset(hwMotionData, 0xFF, sizeof(hwMotionData));
  portEXIT_CRITICAL(&motionMux);

  std::atomic_thread_fence(std::memory_order_release);
  displayDirty.store(true, std::memory_order_relaxed);
}

void copyPatternSlot(uint8_t srcBank, uint8_t srcChain,
                     uint8_t dstBank, uint8_t dstChain) {
  if (srcBank >= 16u || srcChain >= 4u) return;
  if (dstBank >= 16u || dstChain >= 4u) return;
  if (srcBank == dstBank && srcChain == dstChain) return;
  portENTER_CRITICAL(&patchMux);
  for (int r = 0; r < 16; ++r)
    hwSeqData[dstBank][dstChain][r] = hwSeqData[srcBank][srcChain][r];
  portEXIT_CRITICAL(&patchMux);
  displayDirty.store(true, std::memory_order_relaxed);
}

/* [SEQ-CLEAR] One-shot "blank the active pattern" used by the SEQ SETUP → Clear
 * confirm dialog.  NOT a factory reset: it touches only the ACTIVE bank/chain.
 *   • clears all 16 step rows (synth 0-7 + drum 8-15)
 *   • clears the 4 motion / P-lock lanes for that slot
 *   • resets BOTH the seq-synth and drum SOUND to their factory default image
 *     (preset 0's companion patch) — the "initial knob image" the user asked for
 * Mirrors exactly what loadFactory*Pattern does for the sound (sanitise + version
 * bump + atomic fan-out for NVS), minus the melody/drum grid.  App is resynced:
 * zeroed grid rows + P-lock clear + the two sound blobs.                        */
/* [SOUND-RESET] Reset BOTH live sounds (seq-synth + drum) to the preset-0
 * companion image and mirror them to the App.  Shared by CLEAR and SOFT RESET
 * so the two paths can never drift.  Does NOT touch the step grid or P-locks. */
static void seqResetSoundsToPreset0() {
  /* Seq-synth sound → factory default image (preset 0 companion). */
  SynthPatternROM sp;
  memcpy_P(&sp, &SYNTH_PATTERNS[0], sizeof(SynthPatternROM));
  portENTER_CRITICAL(&patchMux);
  memcpy(seqLivePatch, sp.preset, sizeof(sp.preset));
  sanitizePatch(seqLivePatch);
  seqLivePatchVersion.store(
    (seqLivePatchVersion.load(std::memory_order_relaxed) + 1u) & 0xFFu,
    std::memory_order_release);
  portEXIT_CRITICAL(&patchMux);
  syncSeqAtomicsFromLivePatch(); /* [SEQ-SOUND-PERSIST] keep atomics == live sound */

  /* Drum sound → factory default image (preset 0 companion). */
  DrumPatternROM dp;
  memcpy_P(&dp, &DRUM_PATTERNS[0], sizeof(DrumPatternROM));
  portENTER_CRITICAL(&patchMux);
  memcpy(drumLivePatch, dp.preset, sizeof(dp.preset));
  drumLivePatchVersion.store(
    (drumLivePatchVersion.load(std::memory_order_relaxed) + 1u) & 0xFFu,
    std::memory_order_release);
  portEXIT_CRITICAL(&patchMux);

  /* The "default sound" is preset 0, so the load-preset readouts follow. */
  g_lastSynthPreset.store(0, std::memory_order_relaxed);
  g_lastDrumPreset .store(0, std::memory_order_relaxed);

  std::atomic_thread_fence(std::memory_order_release);
  displayDirty.store(true, std::memory_order_relaxed);

  if (isAppConnected()) {
    txPatchBlob(1u);                                   /* seq-synth knobs        */
    for (uint8_t i = 0; i < 32u; ++i)
      txSysex((uint8_t)(32u + i), drumLivePatch[i]);   /* drum knobs             */
  }
}

void seqClearActiveAndResetSounds() {
  const uint8_t bank  = seqActiveBank .load(std::memory_order_relaxed) & 0x0Fu;
  const uint8_t chain = seqActiveChain.load(std::memory_order_relaxed) & 0x03u;

  /* 1) Steps — all 16 rows of the active pattern. */
  portENTER_CRITICAL(&patchMux);
  for (int r = 0; r < 16; ++r) hwSeqData[bank][chain][r] = 0;
  portEXIT_CRITICAL(&patchMux);

  /* 2) P-locks / motion — the 4 lanes of this slot (255/0xFFFF = empty sentinel). */
  portENTER_CRITICAL(&motionMux);
  for (int l = 0; l < 4; ++l) {
    hwMotionData[bank][chain][l].targetCmd = 255u;
    for (int s = 0; s < (int)MOTION_STEPS_PER_LANE; ++s)
      hwMotionData[bank][chain][l].steps[s] = 0xFFFFu;
  }
  portEXIT_CRITICAL(&motionMux);

  /* 3) Both live sounds → preset-0 image (shared with SOFT RESET). */
  seqResetSoundsToPreset0();

  /* 4) Mirror cleared grid (rows 0-15) + P-locks to the App (sounds echoed above). */
  if (isAppConnected()) {
    for (uint8_t r = 0; r < 16u; ++r) echoGridRow(bank, r);
    txSysex(CMD_CLR_PLOCKS, 16383u);
  }
}

/* [SOFT-RESET] CLEAR extended: return the live SOUNDS to preset-0 and the
 * sequencer WORKING IMAGE (bank/chain/page/length/transpose "dropdown" positions)
 * to their initial defaults.  RAM-only — touches NO NVS and does NOT reboot, and
 * deliberately PRESERVES the pattern grid + P-locks (that is what CLEAR / RESET
 * are for).  A LOAD or power-cycle restores the saved session.                 */
void seqSoftResetWorkingImage() {
  /* [FIX-M2] Stop the sequencer and silence all voices before wiping live state.
   * Without this the step engine fires notes against partially-reset sound params
   * during the ~50 ms it takes reset_settings_to_factory to complete.           */
  seq_stop();
  allNotesOff();

  /* Factory settings → all atomics (master, laser show, D-BEAM, mix, harp scale,
   * FX, …).  RAM-only — does NOT touch banks, patterns, or motion.              */
  reset_settings_to_factory();

  /* Sequencer working image (explicit — matches post-CLEAR initial dropdowns). */
  applySeqBank(0u);
  applySeqChain(0u);
  seqUI_page.store(0, std::memory_order_relaxed);
  applySeqLength(16u);
  applySeqTranspose(12u);

  /* Live sounds → preset-0 image (shared with CLEAR). */
  seqResetSoundsToPreset0();

  syncLivePatchFromAtomics();
  displayDirty.store(true, std::memory_order_relaxed);

  /* Mirror the full RAM image to the App (no reboot — replaces piecemeal echoes). */
  if (isAppConnected())
    requestFullStateSync(true, false);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * § SEQ SYNTH ENGINE  (sequencer-only DSP — replaces synth_core.h)
 *
 * Private voice-arm primitive lives in the anonymous namespace below.
 * gb_seq_fill_buf is file-scope (linkable from audio.cpp).  trigger_seq_note /
 * release_seq_note are the voice boundary; seq_synth_fill_buf wraps gb_seq_fill_buf
 * in audio.cpp for the orchestrator's bool hOn/sOn/dOn flags.
 * ═══════════════════════════════════════════════════════════════════════════ */
namespace {

constexpr uint16_t GB_SEQ_ACCENT_VEL_Q15 = 24000u;

void IRAM_ATTR gb_arm_seq_voice(Voice* v, float freq, uint16_t velQ15) {
  const float mp = masterPitch.load(std::memory_order_relaxed);
  v->step  = (uint32_t)((freq * mp * 512.0f / (float)SAMPLE_RATE) * 8388608.0f);
  v->phase = v->phase2 = 0;
  v->type  = (uint8_t)(seqLivePatch[(int)SynthParam::P_WAVEFORM]  % 25u);
  v->type2 = (uint8_t)(seqLivePatch[(int)SynthParam::P_OSC2_WAVE] % 25u);
  v->velocity    = velQ15;
  v->is_accented = (velQ15 > GB_SEQ_ACCENT_VEL_Q15);
  v->svf_low = v->svf_band = 0;
  v->env_level.store(0u, std::memory_order_relaxed);
  v->env_state = EnvState::ENV_ATTACK;
  std::atomic_thread_fence(std::memory_order_release);
  v->active.store(true, std::memory_order_release);
}

} /* anonymous namespace — gb_arm_seq_voice stays TU-private */

bool IRAM_ATTR gb_seq_fill_buf(int16_t* out_buf, size_t frames) {
  /* [PERF] Idle early-out — see harp_synth_fill_buf for rationale.  The outer
   * per-sample loop (LFO advance, exp2f pitch-mod, wave-morph, SVF coeff slew)
   * must not run when no seq voice is active, or an idle sequencer steals
   * headroom from whatever IS playing and inflates the baseline load. */
  /* [MUTE-GATE] Seq muted → skip the DSP pass and free voices.  sequencer_
   * render_block() still ran this buffer (ticks/song/STEP_SYNC continue), it
   * just armed voices we now silence; the envelope ager lives below, so freeing
   * here prevents an un-advanced voice pile-up that would starve allocation on
   * unmute.  Equivalent click profile to the old gain-0 path. */
  if (mixSeqMute.load(std::memory_order_relaxed)) {
    for (int v = 0; v < SEQ_POLYPHONY; ++v)
      seqVoices[v].active.store(false, std::memory_order_relaxed);
    memset(out_buf, 0, frames * sizeof(int16_t));
    return false;
  }

  bool any_active = false;
  for (int v = 0; v < SEQ_POLYPHONY; ++v) {
    if (seqVoices[v].active.load(std::memory_order_relaxed)) { any_active = true; break; }
  }
  if (!any_active) {
    memset(out_buf, 0, frames * sizeof(int16_t));
    return false;                         
  }

  uint16_t lp[PARAMS_PER_PRESET];
  portENTER_CRITICAL(&patchMux);
  memcpy(lp, seqLivePatch, PARAMS_PER_PRESET * sizeof(uint16_t));
  portEXIT_CRITICAL(&patchMux);

  SynthGlobal& g = seq_synth_g;

  const float lfo_hz = 0.1f + ((float)lp[(int)SynthParam::P_LFO_RATE] / 16383.0f) * 29.9f;
  g.lfo_step  = (uint32_t)((lfo_hz / (float)SAMPLE_RATE) * 4294967296.0f);
  g.lfo_depth = lp[(int)SynthParam::P_LFO_DEPTH];

  const uint32_t cached_pb        = g.pitch_bend_q16.load(std::memory_order_relaxed);
  const int32_t  lfo_depth_cached = (int32_t)g.lfo_depth;
  const uint16_t lfo_route        = lp[(int)SynthParam::P_LFO_ROUTE] % 8u;
  const int32_t  env_cut_amt      = (int32_t)lp[(int)SynthParam::P_ENV_CUT];

  const int svf_passes = g_svf_oversample.load(std::memory_order_relaxed);

  const float atk = std::max(0.005f, (float)lp[(int)SynthParam::P_ATTACK]  / 16383.0f * 2.0f);
  const float dec = std::max(0.005f, (float)lp[(int)SynthParam::P_DECAY]   / 16383.0f * 3.0f);
  const float rel = std::max(0.010f, (float)lp[(int)SynthParam::P_RELEASE] / 16383.0f * 4.0f);
  const float sus = std::min(1.0f, std::max(0.0f,
                                            (float)lp[(int)SynthParam::P_SUSTAIN] / 16383.0f));

  const float    magic        = (float)0x7FFFFFFFu / (float)SAMPLE_RATE;
  const uint32_t c_atk_step   = (uint32_t)(magic / atk);
  const uint32_t c_dec_normal = (uint32_t)(magic / dec);
  const uint32_t c_dec_accent = (uint32_t)(magic / std::max(0.005f, dec * 0.3f));
  const uint32_t c_rel_step   = (uint32_t)(magic / rel);
  const uint32_t c_sus_level  = (uint32_t)(sus * (float)0x7FFFFFFFu);

  /* [gbox OPT-4] Cache exp2f against raw P_DETUNE — Flash libm fires only on a
   * detune knob/preset change, not every buffer. Audio-task-only statics.      */
  static uint16_t s_det_param = 0xFFFFu;
  static float    s_det_mult  = 1.0f;
  static float    s_det_semi  = 0.0f;
  const uint16_t det_raw = lp[(int)SynthParam::P_DETUNE];
  if (det_raw != s_det_param) {
    /* [WS11-FIX] P_DETUNE is a CENTS control (±82 ct, centre 8192): the ratio is
     * exp2(cents/1200), NOT the semitone exp2(cents/12).  The old /12 detuned osc2
     * by up to ~6.8 octaves so detune was effectively unusable.  Matches harp.cpp. */
    s_det_semi  = ((int)det_raw - 8192) / 100.0f;     /* cents */
    s_det_mult  = exp2f(s_det_semi / 1200.0f);        /* cents → frequency ratio */
    s_det_param = det_raw;
  }
  const float detune_semi = s_det_semi;
  const float detune_mult = s_det_mult;
  /* [LOAD-SHED] osc2 unison layer gated off above ~85 % load (matches harp.cpp). */
  const bool osc2_on = g_osc2_enable.load(std::memory_order_relaxed);
  /* [WS11-FIX] osc2 also joins when its waveform differs from osc1 (not only when
   * detuned), so selecting an OSC2 WAVE at centre detune is audible. */
  const bool osc2_wave_diff =
      ((int)lp[(int)SynthParam::P_OSC2_WAVE] % 25) != ((int)lp[(int)SynthParam::P_WAVEFORM] % 25);
  const bool osc2_audible = osc2_on && (detune_semi != 0.0f || osc2_wave_diff);

  for (int v = 0; v < SEQ_POLYPHONY; ++v) {
    if (!seqVoices[v].active.load(std::memory_order_relaxed)) continue;
    seqVoices[v].attack_step   = c_atk_step;
    seqVoices[v].decay_step    = seqVoices[v].is_accented ? c_dec_accent : c_dec_normal;
    seqVoices[v].release_step  = c_rel_step;
    seqVoices[v].sustain_level = c_sus_level;
    seqVoices[v].step2_target  = (uint32_t)((float)seqVoices[v].step * detune_mult);
    seqVoices[v].osc_mix_q15   = osc2_audible ? (Q15_ONE / 2) : 0;
  }

  /* [OPT-A] Cache calc_svf_cutoff_hz against raw P_CUTOFF — same strategy as
   * harp.cpp [OPT-A].  calc_svf_cutoff_hz calls h_sinf() (5th-order poly,
   * ~12 cyc) every buffer; the cutoff only changes on knob/sysex events.   */
  static uint16_t s_seq_cut_param = 0xFFFFu;
  static int32_t  s_seq_cut_hz    = -1;
  {
    const uint16_t cp = lp[(int)SynthParam::P_CUTOFF];
    if (cp != s_seq_cut_param) {
      s_seq_cut_hz    = calc_svf_cutoff_hz((float)cp);
      s_seq_cut_param = cp;
    }
  }
  const int32_t cut_target = s_seq_cut_hz;
  const int32_t res_target = calc_svf_damping(lp[(int)SynthParam::P_RESONANCE]);
  if (g.cut_cur < 0) {
    g.cut_cur = cut_target;
    g.res_cur = res_target;
  } else {
    g.cut_cur += (cut_target - g.cut_cur) >> 3;
    g.res_cur += (res_target - g.res_cur) >> 3;
  }
  const int32_t live_cut   = g.cut_cur;
  const int32_t live_res   = g.res_cur;
  const uint16_t live_noise =
      (uint16_t)(((float)lp[(int)SynthParam::P_NOISE] / 16383.0f) * 32767.0f);
  const int base_wave  = (int)lp[(int)SynthParam::P_WAVEFORM]  % 25;
  const int base_wave2 = (int)lp[(int)SynthParam::P_OSC2_WAVE] % 25;

  /* ── [PERF] Loop-invariants hoisted out of the per-sample loop (see harp.cpp). */
  /* [DBEAM-TGT] D-BEAM expression addends when Target = Melody synth.  These are
   * 0 unless the D-BEAM is routed (Cut/Mod) to this engine; the harp set is
   * forced to 0 in routeDbeamExpression() so only one synth is modulated.     */
  const int32_t cached_dbeam     = dbeam_seq_svf_cutoff.load(std::memory_order_relaxed);
  const int32_t cached_dbeam_lfo = (int32_t)dbeam_seq_mod_depth.load(std::memory_order_relaxed);
  const int32_t lfo_depth_eff =
      std::min<int32_t>(16383,
          std::min<int32_t>(16383, lfo_depth_cached) +
          std::min<int32_t>(16383, cached_dbeam_lfo));
  const bool lfo_pitch   = (lfo_route == 0 || lfo_route == 3 || lfo_route == 5 || lfo_route == 6);
  const bool lfo_filter  = (lfo_route == 1 || lfo_route == 3 || lfo_route == 4 || lfo_route == 6);
  const bool lfo_wave    = (lfo_route == 2 || lfo_route == 4 || lfo_route == 5 || lfo_route == 6);
  const bool lfo_tremolo = (lfo_route == 7);
  const bool lfo_do_pitch = (lfo_pitch && lfo_depth_eff > 0);
  const bool has_bend     = (cached_pb != 65536u);
  /* [OPT-B] Fold pitch-bend into float pitch_mul (same as harp.cpp [OPT-B]).
   * Eliminates per-osc 64-bit uint muls from the inner voice loop.         */
  const float eff_bend_f  = has_bend ? ((float)cached_pb * (1.0f / 65536.0f)) : 1.0f;
  const bool do_pitch_mul = lfo_do_pitch || has_bend;
  const int16_t* const wav_static = WAVE_TABLE_RAM[base_wave];
  const int16_t* const wav2_ptr   = WAVE_TABLE_RAM[base_wave2];

  /* [gbox OPT-3] One acquire load per voice per buffer instead of per sample;
   * voices that finish their envelope clear their bit for remaining samples.   */
  uint8_t live_mask = 0;
  for (int v = 0; v < SEQ_POLYPHONY; ++v)
    if (seqVoices[v].active.load(std::memory_order_acquire))
      live_mask |= (uint8_t)(1u << v);

  for (size_t i = 0; i < frames; ++i) {
    g.lfo_phase += g.lfo_step;
    const uint32_t lp_phase = g.lfo_phase >> 16;
    const int32_t  lfo = (lp_phase < 32768u)
                           ? (int32_t)(lp_phase << 1) - 32768
                           : (int32_t)((65535u - lp_phase) << 1) - 32768;

    /* Fast exp2 poly (|arg| < 0.09 → < 0.01 % error) avoids a per-sample libm call. */
    float lfo_pitch_mul = 1.0f;
    if (lfo_do_pitch) {
      const float xp = (float)((lfo * lfo_depth_eff) >> 15) * (1.0f / (12.0f * 16383.0f));
      lfo_pitch_mul = 1.0f + xp * (0.6931472f + xp * 0.2402265f);
    }

    const int16_t* wav_ptr = wav_static;
    if (lfo_wave) {
      const int wave_morph = (int)((lfo * lfo_depth_eff) >> 20);
      int morph_idx = (base_wave + wave_morph) % 25;
      if (morph_idx < 0) morph_idx += 25;
      wav_ptr = WAVE_TABLE_RAM[morph_idx];
    }

    /* [PERF] Voice-independent part of cutoff + tremolo gain: computed once/sample.
     * (env-tracking cutoff is still added per voice below since it uses each
     *  voice's envelope.) */
    int32_t cut_base = live_cut;
    if (lfo_filter && lfo_depth_eff > 0)
      cut_base = std::min(SVF_CUT_MAX, std::max<int32_t>(0,
                          cut_base + ((lfo * lfo_depth_eff) >> 3)));
    /* [DBEAM-TGT] D-BEAM Cutoff route → melody SVF (constant per-buffer offset). */
    if (cached_dbeam > 0)
      cut_base = std::min(SVF_CUT_MAX, cut_base + cached_dbeam);

    int32_t trem_mul = 32768; /* Q15 unity */
    if (lfo_tremolo && lfo_depth_eff > 0)
      trem_mul = 32768 - (((lfo + 32768) * lfo_depth_eff) >> 16);

    int32_t acc = 0;

    for (int v = 0; v < SEQ_POLYPHONY; ++v) {
      if (!(live_mask & (uint8_t)(1u << v))) continue;

      EnvState es = seqVoices[v].env_state;
      uint32_t el = seqVoices[v].env_level.load(std::memory_order_relaxed);
      const uint32_t sl = seqVoices[v].sustain_level;
      bool act = true;

      switch (es) {
        case EnvState::ENV_ATTACK:
          if (0x7FFFFFFFu - el <= seqVoices[v].attack_step) {
            el = 0x7FFFFFFFu;
            es = EnvState::ENV_DECAY;
          } else {
            el += seqVoices[v].attack_step;
          }
          break;
        case EnvState::ENV_DECAY:
          if (el > sl + seqVoices[v].decay_step) {
            el -= seqVoices[v].decay_step;
          } else {
            el = sl;
            if (sl == 0) { es = EnvState::ENV_IDLE; act = false; }
            else         { es = EnvState::ENV_SUSTAIN; }
          }
          break;
        case EnvState::ENV_SUSTAIN:
          break;
        case EnvState::ENV_RELEASE:
          if (el > seqVoices[v].release_step) {
            el -= seqVoices[v].release_step;
          } else {
            el = 0;
            es = EnvState::ENV_IDLE;
            act = false;
          }
          break;
        default: break;
      }

      seqVoices[v].env_state = es;
      seqVoices[v].env_level.store(el, std::memory_order_relaxed);

      if (!act) {
        seqVoices[v].active.store(false, std::memory_order_release);
        live_mask &= ~(uint8_t)(1u << v);
        continue;
      }

      const int32_t amp_q16 = (int32_t)(el >> 16);

      /* [OPT-B] Single float mul per osc: bend folded via eff_bend_f (constant
       * per buffer), LFO pitch folded from lfo_pitch_mul (per-sample).
       * Eliminates up to 2× 64-bit uint muls per voice per sample.          */
      const float eff_pitch_mul = lfo_do_pitch
                                  ? (lfo_pitch_mul * eff_bend_f)
                                  : eff_bend_f;
      uint32_t mstep = seqVoices[v].step;
      if (do_pitch_mul) mstep = (uint32_t)((float)mstep * eff_pitch_mul);
      seqVoices[v].phase += mstep;
      int32_t raw = synth_interpolate_wavetable(wav_ptr, seqVoices[v].phase);

      if (seqVoices[v].osc_mix_q15 > 0) {
        uint32_t ms2 = seqVoices[v].step2_target;
        if (do_pitch_mul) ms2 = (uint32_t)((float)ms2 * eff_pitch_mul);
        seqVoices[v].phase2 += ms2;
        const int32_t osc2 = synth_interpolate_wavetable(wav2_ptr, seqVoices[v].phase2);
        raw = (raw * (Q15_ONE - seqVoices[v].osc_mix_q15)
               + osc2 * seqVoices[v].osc_mix_q15) >> 15;
      }

      if (live_noise > 0) {
        raw = (raw * (int32_t)(32767 - live_noise) + fast_noise() * (int32_t)live_noise) >> 15;
      }

      /* Cutoff: shared base computed once/sample; env-tracking part is per voice. */
      int32_t cut = cut_base;
      if (env_cut_amt > 0)
        cut = std::min(SVF_CUT_MAX,
                       cut + (int32_t)(((uint64_t)(el >> 16) * env_cut_amt) >> 14));

      for (int s = 0; s < svf_passes; ++s) {
        const int32_t hp = raw
                           - seqVoices[v].svf_low
                           - ((live_res * seqVoices[v].svf_band) >> 15);
        /* [gbox RISK-1] SVF_CUT_MAX(31000) * |hp|(≤81919) ≈ 2.54e9 overflows
         * int32; promote both cut multiplies to int64 before the >>15.        */
        seqVoices[v].svf_band = std::max(-SVF_SAT, std::min(SVF_SAT,
                                 seqVoices[v].svf_band + (int32_t)(((int64_t)cut * hp) >> 15)));
        seqVoices[v].svf_low  = std::max(-SVF_SAT, std::min(SVF_SAT,
                                 seqVoices[v].svf_low + (int32_t)(((int64_t)cut * seqVoices[v].svf_band) >> 15)));
      }

      const int32_t fa = (trem_mul == 32768) ? amp_q16 : ((amp_q16 * trem_mul) >> 15);

      const int64_t contrib =
          ((int64_t)(seqVoices[v].svf_low) * (int32_t)seqVoices[v].velocity >> 15) * fa;
      acc += (int32_t)(contrib >> 16);
    }

    out_buf[i] = engine_soft_clip(acc);
  }
  return true;
}

void IRAM_ATTR trigger_seq_note(int vi, float freq, uint16_t vel) {
  if (vi < 0 || vi >= SEQ_POLYPHONY) return;
  portENTER_CRITICAL(&patchMux);
  /* [gbox OPT-9] seqLivePatch is already sanitized on every preset load/recall;
   * re-sanitize only when its version changed (s_ver is Core-0-private).       */
  static uint8_t s_sanitized_ver = 0xFFu;
  const uint8_t cur_ver = (uint8_t)seqLivePatchVersion.load(std::memory_order_relaxed);
  if (cur_ver != s_sanitized_ver) {
    sanitizePatch(seqLivePatch);
    s_sanitized_ver = cur_ver;
  }
  gb_arm_seq_voice(&seqVoices[vi], freq, vel);
  portEXIT_CRITICAL(&patchMux);
}

void IRAM_ATTR release_seq_note(int vi) {
  if (vi < 0 || vi >= SEQ_POLYPHONY) return;
  portENTER_CRITICAL(&patchMux);
  if (seqVoices[vi].active.load(std::memory_order_relaxed)) {
    const EnvState es = seqVoices[vi].env_state;
    if (es != EnvState::ENV_RELEASE && es != EnvState::ENV_IDLE)
      seqVoices[vi].env_state = EnvState::ENV_RELEASE;
  }
  portEXIT_CRITICAL(&patchMux);
}
