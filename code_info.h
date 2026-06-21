/* ═════════════════════════════════════════════════════════════════════════════
 * Octopus PRO XL v6.0.00 — Laser Harp Groovebox
 * © 2026 DIODAC ELECTRONICS / iSystem. All Rights Reserved.
 *
 * PROPRIETARY AND CONFIDENTIAL. Unauthorized copying, distribution, modification,
 * or use of this software or firmware, in whole or in part, is strictly prohibited
 * without prior written permission from DIODAC ELECTRONICS.
 * ═════════════════════════════════════════════════════════════════════════════
 * code_info.h — v6.0.00  ARCHITECTURE MANIFEST, SSOT BLUEPRINT & DEVELOPER GUIDE
 *
 * PURPOSE: This file contains NO executable code.  It is the authoritative
 * reference document for any developer (human or AI) maintaining or extending
 * the Octopus PRO XL firmware and OctopusApp.
 *
 * ─────────────────────────────────────────────────────────────────────────────
 * VERSION IDENTIFIERS
 * ─────────────────────────────────────────────────────────────────────────────
 * SYSTEM_FW_VERSION "6.0.00"
 * SYSTEM_BUILD_DATE "2026-06-20"
 * SYSTEM_ARCH_TAG   "ESP32-S3-DUALCORE-IDF5-FREERTOS"
 * SETTINGS_VERSION  0x0615   (0x0614 seq arp; 0x0615 harp arp)
 * CMD_COUNT         190       (sysex indices 0-189; … 182-185 seq arp;
 *                              186-189 harp arp)
 *
 * ─────────────────────────────────────────────────────────────────────────────
 * WHAT CHANGED IN v6.0 (vs v5.3.x)
 * ─────────────────────────────────────────────────────────────────────────────
 *   • [USB-ONLY] DIN UART MIDI removed; the only transport is USB SysEx + play-in.
 *   • [D-BEAM-LOCAL] D-BEAM is a pure internal-DSP controller — it emits NO MIDI.
 *     The CC route commands (146 DB_CUT_CC / 147 DB_MOD_CC) were retired.
 *   • [HARP-MODULE] harp engine split into harp.h/harp.cpp; synth_core.h removed.
 *   • [P4 CONTROL-PLANE] the v14↔value mapping for master/mix/FX scalars now lives
 *     ONLY in patches.h (applyMasterParam / applyFxSend); midi.cpp + interface.cpp
 *     funnel through it so the App-RX and encoder paths can never drift.
 *   • [FACTORY-BANK] assets.cpp soundbank repacked via SP() (128 authored presets,
 *     NUM_FACTORY_PATCHES); loaders clamp the browse index so a blank slot is
 *     unreachable.  SETTINGS_VERSION bumped → user-bank delta store re-seeds.
 *   • [SEQ-64] sequencer grid is 64 steps (hwSeqData is uint64_t); the App pages
 *     it P1-P4 (16 steps each).  STEP_SYNC carries the absolute step (0-63).
 *   • [SEQ-ARP] melody arpeggiator on the seq synth (8 patterns, 8 rates, gate).
 *   • [HARP-ARP] harp arpeggiator in POLY8/SOLO (4 patterns, 4 rates, gate);
 *     auto-disabled in STRINGS mode; RMS-style poly headroom (×32768/√N).
 *   • [DRUM-PITCH] global drum pitch (D.PITCH) — semitone-linear, independent of
 *     master pitch; persisted + App mixer row.
 *   • [FOG-REJECT] optional fog/haze false-trigger gate (fog.h); NVS + OLED scope.
 *   • [SONG-MODE] full song/chain engine + NVS persistence + hardware OLED editor.
 *   • [CORE1-RECLAIM] the laser sweep sleeps on a one-shot esp_timer instead of
 *     busy-polling, freeing Core 1.
 *   • [TRANSPORT-OWNERSHIP] transport (play/stop/record/BPM) is ALWAYS owned by
 *     the hardware surface; the App is a read-only reflector kept in sync by a
 *     supervisor.  The old transportAvailable hand-off was retired (see §5).
 *   • [APP-v6] OctopusApp: STEP_SYNC-only playhead, page auto-follow repaint,
 *     CPU-load telemetry (CMD 164), retired legacy CMD map entries cleaned up.
 * ═════════════════════════════════════════════════════════════════════════════
 */
#pragma once
#ifndef CODE_INFO_H
#define CODE_INFO_H

#define SYSTEM_FW_VERSION "6.0.00"
#define SYSTEM_BUILD_DATE "2026-06-20"
#define SYSTEM_ARCH_TAG   "ESP32-S3-DUALCORE-IDF5-FREERTOS"

/* ═══════════════════════════════════════════════════════════════════════════
 * 1. CORE ASSIGNMENT
 *
 *   Core 0 — DSP core (no galvo ISR, no SPI DMA contention):
 *     Priority  Task              Stack    Role
 *     ────────  ────────────────  ──────   ─────────────────────────────────
 *        24     AudioSynth        16384    I2S DMA + full DSP pipeline
 *        10     MidiUsbRx(Core1)   8192    USB MIDI parser (App SysEx + play-in)
 *        19     dbeam_adc          6144    ADC DMA + Kalman filter
 *        14     SeqSysexOut(Core1) 4096    drains seq out-ring → App SysEx echoes (STEP_SYNC/
 *                                          SONG_POS) + sync supervisor (BPM/play/rec re-assert)
 *        10     ControlPoll        6144    encoder + buttons @ 200 Hz + TX drain
 *         4     OledRender         8192    OLED @ 30 Hz
 *
 *   Core 1 — Laser core (dedicated to galvo timing):
 *     Priority  Task              Stack    Role
 *     ────────  ────────────────  ──────   ─────────────────────────────────
 *        24     LaserSweep         6144    galvo sweep, trigger detect, vibrato
 *         2     NvsWorker         16384    NVS save on demand (16 KB — four blobs)
 *         1     loop()             —       Arduino fallback (safety only)
 *
 *   Stack high-water marks are sampled every 5 s and printed to Serial (and the
 *   OLED STACK_STATS telemetry page) by control_surface_task — verify > 512 B
 *   free per task before shipping a build.
 *
 *   WHY audio on Core 0: Sharing Core 1 with the laser timing machine would
 *   cause FreeRTOS tick preemption of the galvo sweep → visible beam jitter.
 *   Core 0 is clear of any timing-deterministic hardware; I2S DMA is handled
 *   by the IDF interrupt controller independently of the FreeRTOS scheduler.
 *
 * ═══════════════════════════════════════════════════════════════════════════
 * 2. THREE-LEVEL PARAMETER MODEL (SSOT)
 *
 *   Level 1  g_settings (settings.h)    — NVS blob (load/save only)
 *   Level 2  std::atomic<T> (globals.h) — UI reads/writes, encoder/MIDI/App
 *   Level 3  livePatch[] (globals.h)    — DSP reads (harp.cpp, groovebox.cpp)
 *
 *   Direction A — Boot load:
 *     NVS → g_settings → atomics → livePatch
 *     Call: settings_sync_to_ssot() → syncLivePatchFromAtomics()
 *     (NVS flash is initialised by the Arduino-ESP32 core; there is no manual
 *      init_nvs_flash() call — it was removed in v5.3 as dead code.)
 *
 *   Direction B — Encoder / App write:
 *     applyHarpParam(pIdx, v14)  → writes atomic + livePatch + userBank atomically
 *     applySeqParam(pIdx, v14)   → same for seq synth
 *     applyDrumParam(idx, v14)   → same for drum
 *     applyDrumWave(ch, widx)    → writes drumWaveIdx[]
 *     applyMute(cmd, state)      → writes mute atomic + txSysex echo (encoder path)
 *     [P4] applyMasterParam(cmd, v14, Origin) → CANONICAL master/mix/FX scalar map
 *          (vol/pan/EQ/pitch/tube/DJ/aux/hue/mute/D-BEAM cfg).  The v14↔value
 *          scaling lives HERE ONLY.  Origin::APP = no echo; HW/MIDI = echo back.
 *     [P4] applyFxSend(cmd, v14, Origin)      → per-engine FX insert sends, under
 *          patchMux.  Both are the single home for the mapping across the whole
 *          App control surface (midi.cpp App-RX funnels through them).
 *
 *   Direction C — NVS save:
 *     settings_sync_from_ssot() → settings_save_scoped() on NvsWorker (16 KB stack)
 *     Triggered by ENC long-press, SAVE menu, CMD_SESSION_SAVE, or factory reset.
 *     Boot/first-load reseed uses settings_persist_blocking() (same 16 KB NvsBlk task)
 *     — never call settings_save_scoped() from setup() or MidiUsbRx (~8 KB stacks).
 *
 *   RULE: The audio task (Core 0) NEVER writes to livePatch arrays.
 *         It reads them under patchMux.  Only patches.h apply* / recall* / load*
 *         functions (and syncLivePatchFromAtomics) write livePatch — never from
 *         the audio loop.
 *
 *   RULE (SSOT for SAVE): NVS saves the seq SOUND from the seq ATOMIC mirrors
 *         (settings_sync_from_ssot), NOT from seqLivePatch[].  Therefore ANY code
 *         that writes seqLivePatch[] directly MUST immediately fan it back out via
 *         syncSeqAtomicsFromLivePatch() (patches.h) — else the audible sound and
 *         the saved sound diverge (seq synth sound silently fails to persist).
 *         Covered writers: recallSeqPatch, loadFactoryPreset, applySeqParam,
 *         loadFactorySynthPattern (companion preset).
 *
 * ═══════════════════════════════════════════════════════════════════════════ 
 * 3. SYSEX COMMAND TABLE (CMD 0–189, canonical as of v6.0.00)
 *
 *   Wire format: [0xF0] [ID] [sub] [cmd_wire] [hi7] [lo7] [0xF7]
 *   ID = 0x7D  App → device  (only ID the firmware RX accepts)
 *   ID = 0x7C  device → App  (all firmware echoes — ignored by firmware RX so a
 *              looped MIDI stream can't fake an App heartbeat / re-toggle play)
 *   sub=0x00: cmd 0-127   (cmd_wire = cmd)
 *   sub=0x01: cmd 128-164 (cmd_wire = cmd - 128)
 *   v14 = (hi7 << 7) | lo7  ∈ [0, 16383]
 *
 *   ── Harp synth (0–13) — SynthParam direct index ─────────────────────────
 *    0 H_WAVE       harpWaveform      0–24 wavetable index
 *    1 H_ATTK       harpAttack        0–16383 (→ 0.0–2.0 s)
 *    2 H_DECY       harpDecay         0–16383 (→ 0.0–3.0 s)
 *    3 H_SUST       harpSustain       0–16383 (→ 0–100%)
 *    4 H_RELS       harpRelease       0–16383 (→ 0.0–4.0 s)
 *    5 H_CUT        harpCutoff        0–16383 (→ 30–14000 Hz)
 *    6 H_RES        harpResonance     0–16383 (→ 0.0–10.0)
 *    7 H_NSE        harpNoise         0–16383 (→ 0–100%)
 *    8 H_DET        harpDetune        0–16383 centred 8192 (±400 ct)
 *    9 H_LRATE      harpLfoRate       0–16383 (→ 0.1–30.0 Hz)
 *   10 H_LDPTH      harpLfoDepth      0–16383 (→ 0–100%)
 *   11 HLFO_RT      harpLfoRoute      0–7  (route matrix)
 *   12 H_OSC2       harpOsc2Wave      0–24 wavetable index
 *   13 H_ENVC       harpEnvCutAmount  0–16383 (→ 0–100%)
 *   14–15           spare
 *
 *   ── Seq synth (16–29) — SynthParam index + 16 ───────────────────────────
 *   16 S_WAVE       seqWaveform       0–24
 *   17 S_ATTK       seqAttack         0–16383
 *   18 S_DECY       seqDecay          0–16383
 *   19 S_SUST       seqSustain        0–16383
 *   20 S_RELS       seqRelease        0–16383
 *   21 S_CUT        seqCutoff         0–16383
 *   22 S_RES        seqResonance      0–16383
 *   23 S_NSE        seqNoise          0–16383
 *   24 S_DET        seqDetune         0–16383 centred 8192
 *   25 S_LRATE      seqLfoRate        0–16383
 *   26 S_LDPTH      seqLfoDepth       0–16383
 *   27 SLFO_RT      seqLfoRoute       0–7
 *   28 S_OSC2       seqOsc2Wave       0–24
 *   29 S_ENVC       seqEnvCutAmount   0–16383
 *   30–31           spare
 *
 *   ── Drum (32–63) — ch*4 + param ─────────────────────────────────────────
 *   32+ch*4+0  DRUM_TUNE   drumTune[ch]     ch=0..7
 *   32+ch*4+1  DRUM_DECY   drumDecay[ch]
 *   32+ch*4+2  DRUM_VOL    drumVolume[ch]
 *   32+ch*4+3  DRUM_NOIS   drumNoiseMix[ch]
 *
 *   ── Master / FX (64–96) ──────────────────────────────────────────────────
 *   64 M_VOL         masterVol
 *   65 H_VOL         mixHarpVol
 *   66 S_VOL         mixSeqVol
 *   67 D_VOL         mixDrumsVol
 *   68 PITCH         masterPitch (v14 semitone-linear ±24 st; 8192 = unity ×1.0)
 *   69 EQ_L          masterEqLow (8192=0 dB, ±12 dB)
 *   70 EQ_H          masterEqHigh
 *   71 D_REV         drumRevSend
 *   72 D_DLY         drumDlySend
 *   73 TB_DRV        tbDrive
 *   74 TB_TONE       tbTone
 *   75 TB_MIX        tbMix
 *   76 DJ_FQ         djFreq
 *   77 DJ_RES        djRes
 *   78 DJ_MIX        djMix
 *   79 HLFO_R        (obsolete alias → same as cmd 9)
 *   80 HLFO_D        (obsolete alias → same as cmd 10)
 *   81 SLFO_R        (obsolete alias → same as cmd 25)  [App compat]
 *   82 SLFO_D        (obsolete alias → same as cmd 26)  [App compat]
 *   83 DB_CURVE      currentDbeamCurve  0=Lin 1=Inv 2=Exp 3=Log 4=Sig
 *   84 DB_OFFSET     dbeamHWCfg.offsetAdc
 *   85 DB_RANGE      dbeamHWCfg.rangeAdc
 *   86 HLFO_RT       harpLfoRoute       0–7    [App backward compat for cmd 11]
 *   87 SLFO_RT       seqLfoRoute        0–7    [App backward compat for cmd 27]
 *   88 DB_ENABLED    dbeamEnabled
 *   89 D_FX_IDX_B    drumFxIndexB
 *   90 HW_H_OCT      octaveShift[0]
 *   91 HW_S_OCT      octaveShift[1]
 *   92 HW_GATE       beamGateHoldMs
 *   93 HW_WHITE      scaleWhiteLevel[scale]
 *   94 HW_S_LEN      seqLength  (v14 / 16383 * 64)
 *   95 TRANSPOSE     seqTranspose (v14 / 16383 * 24 - 12)
 *   96 HW_MARGIN     scaleMargin[scale]
 *
 *   ── Sequencer transport + grid (97–127) ──────────────────────────────────
 *   97  BPM          seqBpm (raw BPM 40–240, no scaling)
 *   98  BANK         seqActiveBank  v14 / 1024 → 0–15
 *   99  PING         heartbeat reply → txSysex(PING, millis())
 *   100 GRID_TOG     toggle hwSeqData step: (row << 7) | col
 *   101 CLR_PLOCKS   clear hwMotionData[bank][chain]
 *   102 LOAD_PAT_S   load factory synth pattern index into current bank/chain
 *   103 LOAD_PAT_D   load factory drum pattern index into current bank/chain
 *   104 TRIG_MODE    play/stop (>0=play, 0=stop)  [backward compat; use 153]
 *   105 STEP_SYNC    echo: current step index (0-based)
 *   106 H_PATCH      harpPatchIndex  recall from userBank[]
 *   107 S_PATCH      seqPatchIndex   recall from seqBank[]
 *   108 H_SCALE      harpScaleIndex  0–15
 *   109 S_SCALE      (reserved — seq uses same scale as harp)
 *   110 D_FX_IDX     drumFxIndexA
 *   111 H_FX_IDX_B   harpFxIndexB
 *   112 H_FX_MIX     (unused — legacy)
 *   113 H_FX_TIME    masterAuxDlyTime (shared bus)
 *   114 H_FX_SIZE    masterAuxRevSize (shared bus)
 *   115 H_FX_IDX     harpFxIndex
 *   116 S_FX_MIX     (unused — legacy)
 *   117 S_FX_TIME    masterAuxDlyTime (same as 113)
 *   118 S_FX_SIZE    masterAuxRevSize (same as 114)
 *   119 S_FX_IDX     seqFxIndex
 *   120 M_FX_IDX     masterFxIndex + loadMasterFx()
 *   121 H_DLY_MIX    fx.harpInsert.dly_send
 *   122 H_REV_MIX    fx.harpInsert.rev_send
 *   123 S_DLY_MIX    fx.seqInsert.dly_send
 *   124 S_REV_MIX    fx.seqInsert.rev_send
 *   125 FETCH        request full state sync (deprecated — use CMD 155)
 *   126 HARD_SAVE    save preset to userBank slot (deprecated — use CMD 156)
 *   127 S_FX_IDX_B   seqFxIndexB
 *
 *   ── Laser + wire (128–137, sub-byte 0x01, wire 0–9) ─────────────────────
 *   128 LSR_SHOW     laserShowMode      (independent of MIDI_HUE — v2)
 *   129 MIDI_HUE     midiHueControl
 *   130 HUE_BASE     laserBaseHue       0..1 → hue wheel
 *   131 HUE_ATK      hueAttack          0..16383 → 0..2 s
 *   132 HUE_DEC      hueDecay           0..16383 → 0..3 s
 *   133 HUE_SUS      hueSustain         0..1 → 0..100 %
 *   134 HUE_REL      hueRelease         0..16383 → 0..4 s
 *   167 LSR_ANIM     laserShowAnim      0..3: Pulse/Chase/Strobe/Wave  [v2]
 *   168 LSR_DRUMFLASH laserDrumFlash    0..1 drum-flash depth          [v2]
 *   169 SCOPED_RESET handleScopedReset  v14=ResetScope; RAM wipe+persist+reboot
 *   170 SEQ_CLEAR    seqClearActive…    clear active grid+P-locks+sounds→preset0
 *   171 SOFT_RESET   seqSoftReset…      factory settings + sounds + seq nav → initial (RAM-only); sendFullStateSync
 *   172 USR_SND_SAVE saveLiveToUserSlot v14=(eng<<13)|slot; live→slot 128+slot +persist
 *   173 USR_SND_LOAD loadUserSlotToLive v14=(eng<<13)|slot; recall user slot→live
 *   174 USR_SND_NAME setUserSlotName    NAME BLOB (sub 0x03) eng,slot,15 chars (Step 12)
 *   175 USR_PAT_SAVE saveActivePatternToUserSlot v14=slot; active→user pat +persist
 *   176 USR_PAT_LOAD loadUserPatternToActive v14=slot; recall user pat→active bank
 *   177 USR_PAT_NAME setUserPatName       NAME BLOB (sub 0x04) slot,15 chars (Step 12)
 *   178 PB_RANGE     pbMapping up/downSemi 0..24 semitones (symmetric)
 *   179 PB_ENABLE    pbMapping.enabled     0=OFF 16383=ON
 *   180 DB_TARGET    currentDbeamTarget    0=Harp 1=Melody synth
 *   181 DRUM_PITCH   drumPitchMult         same semitone-linear encode as cmd 68
 *   182 SEQ_ARP_EN   seqArpEnabled         0=OFF 16383=ON
 *   183 SEQ_ARP_PAT  seqArpPattern         0–7 (Up…Down−Oct)
 *   184 SEQ_ARP_RATE seqArpRate            0–7 (1/1…1/32)
 *   185 SEQ_ARP_GATE seqArpGate            0–7 gate duty index
 *   186 HARP_ARP_EN  harpArpEnabled        0=OFF 16383=ON (POLY8/SOLO only)
 *   187 HARP_ARP_PAT harpArpPattern        0–3 (Up/Down/UpDn/Rnd)
 *   188 HARP_ARP_RATE harpArpRate          0–3 (1/8…1/16T)
 *   189 HARP_ARP_GATE harpArpGate          0–3 gate duty index
 *   157 SESSION_LOAD settings_load_scoped v14=ResetScope (0=FULL); RAM reload, no reboot
 *   135 WIRE_HARP_CH wireHarpMidiChannel  1–16
 *   136 WIRE_SEQ_CH  wireSeqMidiChannel   1–16
 *   137 WIRE_DRUM_CH wireDrumMidiChannel  1–16
 *
 *   ── v5.3 extensions (138–147, sub-byte 0x01, wire 10–19) ────────────────
 *   138 SEQ_CHAIN    seqActiveChain   pinned 0 [V5.3-CONS] (App always sends 0)
 *   139 H_MUTE       mixHarpMute      bool
 *   140 S_MUTE       mixSeqMute       bool
 *   141 D_MUTE       mixDrumsMute     bool
 *   142 AUX_DLY_FB   masterAuxDlyFb   0–16383 (→ 0.0–0.95)
 *   143 AUX_REV_DMP  masterAuxRevDamp 0–16383 (→ 0.0–1.0)
 *   144 DRUM_WAVE    drumWaveIdx[ch]  v14: [ch:3][widx:5] (ch<<5 | widx&31)
 *   145 DB_ROUTE     currentDbeamRoute 0=OFF 1=Mod 2=Vol 3=Cut
 *       (D-BEAM Route lives in the D-BEAM menu; Target = Harp/Melody synth is a
 *        local hardware/NVS control — dbeam.target, no sysex command)
 *   (146/147 DB_CUT_CC/DB_MOD_CC removed v6.0 — D-BEAM is local DSP, emits no MIDI)
 *
 *   ── v5.3.1 song mode + transport (148–157, sub-byte 0x01, wire 20–29) ───
 *   148 SONG_MODE    songModeActive  0=pattern, ≥8192=song
 *   149 SONG_SLOT    activeSongSlot  0–15
 *   150 SONG_STEP    applySongStep(): [step:4][bank:4][chain:2][rpt:4]
 *   151 SONG_STEPS_N hwSongData[slot].numSteps  1–16
 *   152 SONG_POS     echo only: [(step<<8)|repeat] >> 1  (fits v14)
 *   153 TRANSPORT    RX (App→ESP, legacy/external): 0=stop 1=play 2=pause
 *                    3=rec_toggle.  ECHO (ESP→App): 0=stop 1=play 2=pause,
 *                    3=record ON, 4=record OFF (play + record are decoupled).
 *                    [v6.0] The App is a read-only reflector; it no longer sends.
 *   154 TRANSPORT_AVAIL  [v6.0 RETIRED] accepted-and-ignored (transport is always
 *                    hardware-owned).  Kept only for old App-build compatibility.
 *   155 APP_SYNC_REQ App requests full state echo (sendFullStateSync + echoSongState)
 *   156 SESSION_SAVE App requests NVS save → triggers g_saveRequest
 *   157 SESSION_LOAD App requests NVS load + full echo
 *   158 DRUM_KIT     applyDrumKit(): 0=TR-909 1=TR-808 2=Trap 3=House. Loads the
 *                    kit tuning into drumLivePatch+atomics (echoes all 32 drum
 *                    params) and sets kick pitch-sweep + hat base character.
 *                    Persisted in DrumSettings.kit (SETTINGS_VERSION 0x0532).
 *                    Live-switchable from an external controller via MIDI CC 70
 *                    (Sound Variation) on the drum channel: kit = value>>5 (0-3).
 *                    handleControlChange() drum-channel block, midi.cpp. The active
 *                    kit name is shown in the OLED SEQ dashboard header + App.
 *
 *   ── v6.0 pan + grid bulk + telemetry (159–164, sub-byte 0x01, wire 31–36) ─
 *   159 H_PAN        mixHarpPan   v14: 0=full L, 8192=centre, 16383=full R
 *   160 S_PAN        mixSeqPan    (same encoding)
 *   161 D_PAN        mixDrumsPan  (same encoding)
 *   162 GRID_ROW_LO  absolute half-row state, steps page*16+0..7  (bulk, not toggle)
 *                    v14 = (bank<<12)|(row<<8)|(page<<4)|byteMask
 *   163 GRID_ROW_HI  absolute half-row state, steps page*16+8..15 (same packing)
 *   164 CPU_LOAD     [v6.0] device→App ONLY: audio-core load, raw 0–100 % (not
 *                    14-bit scaled).  Pushed by the sync supervisor (~600 ms) while
 *                    the App is connected; drives the header CPU readout. Never RX.
 *
 * ═══════════════════════════════════════════════════════════════════════════
 * 4. ADDING A NEW GLOBAL PARAMETER (step-by-step)
 *
 *   STEP 1  globals.h     Add inline atomic: inline std::atomic<float> myParam{ 0.f };
 *   STEP 2  midi.h        Add CMD constant:  static constexpr uint8_t CMD_MY_PARAM = N;
 *   STEP 3  midi.cpp      Add case in handleSysexCommand(): apply + txSysex echo
 *   STEP 4  settings.h    Add to appropriate struct + both sync functions
 *   STEP 5  patches.h     Add applyMyParam(v14) function
 *   STEP 6  interface.cpp Add to appropriate case in updateHardwareParameter()
 *   STEP 7  display.h/cpp Add to kL2Xxx[] label array + formatParamValueString
 *   STEP 8  OctopusApp    Add to CMD object + knob config or control element
 *
 *   INVARIANTS that must never be broken:
 *   • Only patches.h apply* functions write to livePatch[].
 *   • Audio task reads livePatch[] but never writes it.
 *   • Critical sections (patchMux) must complete in < 5 µs.
 *   • All NVS writes must go through the save handshake (g_saveRequest flag).
 *   • [SAVE-FIX14] Every runtime save REBOOTS the device after a good commit
 *     (requestScopedSave sets g_restartAfterSave → settings_save_task ACKs the
 *     App + flashes the SAVED toast, then esp_restart() after 700 ms).  This is a
 *     workaround for the laser beams latching "broken" after an NVS flash write;
 *     the boot init reliably re-homes the beams and the just-saved settings
 *     reload from NVS.  If you ever need a no-reboot save, the root cause (likely
 *     AC-coupled LT1016 settle / MCP4922 glitch during the cache-off write) must
 *     be solved first — see Upgrade.md WS11 item 7.
 *   • [v6.0] Transport (play/stop/record/BPM) is ALWAYS hardware-owned; the App
 *     only reflects it.  Never re-introduce an App-claims-transport hand-off.
 *
 * ═══════════════════════════════════════════════════════════════════════════
 * 5. OCTOPUSAPP ↔ FIRMWARE SYNC PROTOCOL
 *
 *   CONNECTION SEQUENCE:
 *   1. App opens the MIDI connection (selects the device output).
 *   2. App pins Bank A / chain 0, then sends CMD_APP_SYNC_REQ=155.
 *   3. ESP calls sendFullStateSync() + echoSongState() → echoes every parameter,
 *      song program, the full 64-step grid, AND the transport snapshot (BPM, play,
 *      record 3/4, current step if playing) so the App mirror is complete at once.
 *   4. App becomes MASTER for all PARAMETER editing; the App is a read-only
 *      reflector for TRANSPORT (see below).
 *   5. App PINGs every ~1 s to hold the link "connected".
 *
 *   DISCONNECTION (heartbeat timeout):
 *   1. pollSyncHeartbeat() (≤4.5 s) detects no App SysEx → appSyncActive=false.
 *   2. ESP OLED returns to the full dashboard/menu; the hardware surface goes
 *      back to its normal (menu-navigating) role.  Transport ownership does NOT
 *      change — it was hardware-owned all along.
 *
 *   TRANSPORT OWNERSHIP  [v6.0 — hardware is the single owner]:
 *   • play/stop/record/BPM are ALWAYS driven by the physical surface.  While the
 *     App is connected (isAppConnected(), same predicate that draws the splash),
 *     the buttons+encoder are locked to a fixed transport role in
 *     updateHardwareInterface():  SCALE single = play/stop, OC short = record-arm
 *     toggle, ENC turn = BPM, ENC long = save.  Menu/param editing is suppressed.
 *   • The ESP echoes transport state on every change (seq_start/stop/pause →
 *     CMD_TRANSPORT 0/1/2; record → CMD_TRANSPORT 3=on/4=off; BPM → CMD_BPM) and
 *     a SYNC SUPERVISOR (sequencer_background_task, every 600 ms while connected —
 *     just above the txSysex 500 ms dedup window so an unchanged re-assert isn't
 *     swallowed) re-asserts BPM + play + record so a dropped echo self-heals.  The per-step
 *     STEP_SYNC stream (emitted while playing) drives the App playhead.  The same
 *     supervisor tick also pushes CMD_CPU_LOAD (164) — live audio-core load % for
 *     the App header readout.
 *   • The App's transport buttons are read-only reflectors — they send nothing.
 *   • CMD_TRANSPORT_AVAIL=154 is retired (accepted-and-ignored for old App builds).
 *
 *   PATTERN LOAD RULES:
 *   • CMD_LOAD_PAT_S / CMD_LOAD_PAT_D load ONLY into current seqActiveBank/Chain.
 *   • App must set the desired bank/chain BEFORE sending LOAD_PAT_*.
 *   • Manual step editing in App affects ONLY the current bank/chain.
 *   • RND (randomize notes) is App-side only; it edits the app grid then sends
 *     individual GRID_TOG commands for changed cells.
 *
 *   APP CONNECTED DISPLAY:
 *   • When isAppConnected()==true, ESP OLED shows static "APP CONNECTED" splash.
 *   • Full menu tree, encoder parameter editing, and display echo are suppressed
 *     to avoid visual noise and data traffic conflicts.
 *   • BPM, play state, and song/bank position remain visible on the splash.
 *
 *   SESSION SAVE/LOAD:
 *   • App sends CMD_SESSION_SAVE=156 → firmware triggers NVS save handshake.
 *   • App sends CMD_SESSION_LOAD=157 → firmware loads NVS, syncs atomics,
 *     echoes full state back to App.  App resync is automatic.
 *
 * ═══════════════════════════════════════════════════════════════════════════
 * 6. SONG MODE
 *
 *   Data structure (globals.h):
 *     SongSlot hwSongData[16]  — 16 song programs ("CHAINS" in the App)
 *       SongStep steps[16]     — up to 16 chained steps per song
 *         uint8_t bank         — which pattern bank A-D (0–3) [V5.3-CONS]
 *         uint8_t chain        — pinned 0 [V5.3-CONS]; each slot holds BOTH the
 *                                synth (rows 0-7) and drums (rows 8-15)
 *         uint8_t repeats      — repeat count (0=inactive, 1–15)
 *       uint8_t numSteps       — how many steps are active
 *
 *   CMD_SONG_STEP (150) encoding in v14 (14 bits):
 *     [13:10] step_index (0–15)
 *     [ 9: 6] bank       (0–3 used; A-D) [V5.3-CONS]
 *     [ 5: 4] chain      (always 0)      [V5.3-CONS]
 *     [ 3: 0] repeats    (0=inactive, 1–15=repeat count)
 *
 *   SONG_MODE design
 *     hwSongData[16] = 16 programs, each SongSlot has steps[16] + numSteps
 *     SongStep { bank, chain, repeats } repeats=0 → inactive
 *     song_mode_check_advance() called at seqCurrentStep==0 (pattern wrap)
 *     
 *   Song advance logic (groovebox.cpp):
 *     At every seqCurrentStep==0 (pattern wrap), song_mode_check_advance()
 *     increments repeat counter.  When repeats exhausted, loads next step's
 *     bank/chain and echoes CMD_SONG_POS.
 *
 * ═══════════════════════════════════════════════════════════════════════════
 * 7. SEQUENCER GRID MODEL  [V5.3-CONS — consistency revision]
 *
 *   ONE pattern slot = hwSeqData[bank][0] holding 16 rows:
 *     rows 0–7   → SYNTH melody (sequencer voices, 8-note polyphony)
 *     rows 8–15  → DRUMS       (8 TR-909 voices: KICK,SNARE,CLAP,HH-C,HH-O,
 *                               TOM-H,TOM-L,PERC)
 *   The engine (groovebox.cpp) plays the synth rows AND the drum rows of the
 *   SAME slot together every step — they are never separate "chains".
 *
 *   Banks A/B/C/D  = hwSeqData banks 0–3.  The legacy 4-deep `chain` dimension
 *   is PINNED to 0 everywhere (App, hardware grid editor, hardware menu, song
 *   advance).  "Synth vs Drum" on the OLED matrix is a DISPLAY PAGE
 *   (seqUI_page in groovebox.cpp), NOT a change of seqActiveChain.
 *
 *   WHY: pre-V5.3 the grid editor abused seqActiveChain as a synth/drum page
 *   toggle.  Scrolling to the drum rows flipped chain 0→1, moving playback to a
 *   different (empty) slot and silencing the synth — you could never hear a
 *   pattern's synth + drums together on hardware.  All three surfaces
 *   (App grid, hardware grid, hardware menu/dashboard) now address chain 0,
 *   so edits made anywhere are heard everywhere identically.
 *
 *   Files kept in lock-step:  groovebox.cpp/.h (engine + OLED matrix model/nav,
 *   seqUI_page · song advance pins chain 0) · interface.cpp (SEQ SETUP menu:
 *   Bank A-D, View S/D · grid editor) · display.cpp/.h (dashboard "BANK:x",
 *   "View Synth/Drum") · OctopusApp.html (A-D buttons, chainIdx=0).
 *
 * ═══════════════════════════════════════════════════════════════════════════
 * 8. REAL-TIME AUDIO ROBUSTNESS  (live-performance guarantees)
 *
 *   Target scenario: laser-harp + sequencer melody + drum patterns + inserts +
 *   aux reverb/delay + master FX, all running at once, BPM rock-steady, no
 *   crackle, no note-on/off clicks.  How each guarantee is met:
 *
 *   A. TEMPO STABILITY (no BPM drift under load)
 *      The step clock is a microsecond accumulator (esp_timer_get_time) inside
 *      sequencer_render_block (groovebox.cpp), clocked by the audio task once
 *      per DMA buffer.  It integrates REAL elapsed time scaled by seqBpm, with a
 *      fractional-tick carry, so even if a buffer is briefly delayed the next
 *      one emits the ticks that were owed; average tempo cannot drift.  Tempo is
 *      the seqBpm atomic alone — uClock was removed (it drove no output).  This
 *      is the fix for the pre-v5 "tempo becomes unstable under load" behaviour.
 *
 *   B. NO UNDERRUNS / LAG
 *      • audio_synthesis_task blocks on i2s_channel_write → the DMA ring paces
 *        the loop (back-pressure), it can never free-run or busy-spin.
 *      • Default I2S DMA ring (6 × 240 frames ≈ 30 ms) absorbs scheduling jitter.
 *      • Adaptive quality scaling (audio.cpp): a load EMA drives graceful
 *        degradation — at >80/88/95 % it drops SVF oversampling 2→1, seq voice
 *        cap 8→5→4, and aux mode, then restores after ~6 s of <60 % load.
 *      • Denormal flushing on every FX state variable (biquads, combs, allpass,
 *        delay writes) prevents Xtensa FPU subnormal stalls — a classic hidden
 *        cause of periodic crackle.
 *      • safe_sinf() (IRAM polynomial) is used in the per-sample hot path so the
 *        DSP never touches Flash sinf(); Flash sinf is only called from
 *        update_params() on a parameter change, and is skipped entirely during
 *        the NVS-save window (g_saveArmed mutes + returns before update_params).
 *
 *   C. NO CLICKS ON NOTE ON / OFF
 *      • Synth voices: minimum 5 ms attack ramp from zero (no on-click) and
 *        minimum 10 ms release ramp to zero (no off-click) — harp.cpp/groovebox.cpp.
 *      • Voice steal in SOLO mode is a soft-kill (→ ENV_RELEASE), never a hard
 *        zero.  Laser-harp voices use a DETERMINISTIC 1:1 string→voice map
 *        (vi = stringIdx % HARP_POLYPHONY, both = 8) so trigger and release are
 *        always symmetric — no stuck notes / hung sustain (harp.cpp).  STRINGS
 *        mode forces sustain to true zero so plucks fully decay and free the
 *        voice.  The MIDI-note path (noteOnHarpMidi) tracks ownership and
 *        prefers a free voice, falling back to a soft steal.
 *      • Sequencer 50 % gate releases notes via ENV_RELEASE at mid-step.
 *      • Drums are one-shot with a linear-decay envelope to zero (no off edge).
 *      • NVS save handshake: master muted, ~22 ms drain, then Core 1 parks
 *        before the flash write — no save glitch (audio.cpp settings_save_task).
 *
 *   D. POLYPHONY (confirmed)
 *      8 harp + 8 sequencer + 8 drum = 24 simultaneous voices.  The sequencer
 *      maps grid row → voice (row r → seqVoices[r]); all 8 melody rows can
 *      sound at once.  Under heavy load g_seq_voice_cap may shed to 5/4 voices
 *      (new notes dropped silently rather than stealing a ringing voice).
 *
 *   E. MIX / GAIN STAGING (effect.h fx_process_multi_buf_safe)
 *      per-instrument mute+vol → insert A→B (serial) → drum bus saturation →
 *      aux send (harp+seq+drum) to shared delay+reverb (3-s tail gate) →
 *      sum = 0.333·(dry h+s+d) + 0.6·aux_wet → MasterFX (DJ filter, EQ shelves,
 *      tube, mid/side soft-knee limiter) → master vol.
 *      The 0.333 dry scale keeps three full-scale sources at unity headroom;
 *      a SINGLE transparent soft-knee limiter in MasterFX is the only master
 *      stage (the former tanh applySoftLimiter was removed — it coloured even
 *      quiet signals).  Per-engine output uses engine_soft_clip() (harp.cpp
 *      / groovebox.cpp): linear below ±26000, asymptotic above, so dense chords
 *      compress transparently instead of hard-clipping.
 *
 *   DESIGN NOTES (intentional, not bugs):
 *      • Per-engine headroom is now soft (engine_soft_clip), and the master
 *        limiter acts on the SUM, forming a transparent two-stage safety so the
 *        int16 output never wraps even with 24 voices + FX.
 *      • Insert-FX preset .name strings in effect.h are internal descriptors;
 *        the user-facing names live in display.h kInsertFxNames[] and are
 *        mirrored verbatim by OctopusApp — index→DSP mapping is identical on
 *        both sides, so a slot picked in the App loads the same effect on hw.
 *
 *   F. MIDI ENGINE ROBUSTNESS (midi.cpp / midi.h)
 *      • [USB-ONLY] DIN UART MIDI was removed (v6.0): the only transport is USB,
 *        carrying OctopusApp 0x7C/0x7D SysEx control + optional instrument
 *        play-in.  External channel-voice MIDI OUT (notes/CC) and D-BEAM MIDI are
 *        gone — the sequencer/drums and D-BEAM drive the internal engines
 *        directly; outboard MIDI OUT returns later via the WiFi/BLE coprocessor.
 *        Frees GPIO 42/39 (former DIN TX/RX).
 *      • RX spam filter (handleSysexCommand): identical values repeated within
 *        50 ms are dropped, but never permanently — re-sent action/refresh
 *        commands (FETCH, APP_SYNC_REQ, transport, save) always take effect.
 *      • sendFullStateSync() resets the TX value-dedup cache so a full sync
 *        transmits the complete state even for unchanged parameters. [SYNC-FORCE]
 *      • seedFactoryBanks() (patches.h) copies PROGMEM SOUND_BANK → RAM
 *        userBank[]/seqBank[] at boot; without it a user-bank recall would
 *        load a closed-filter/silent patch.  [S10] User edits ON TOP of the
 *        factory seed now persist: settings_save() writes a sparse "banks" NVS
 *        blob (only slots that differ from factory) + a full "patterns" blob
 *        (hwSeqData); persisted_extras_load() restores both at boot/SESSION_LOAD.
 *
 *   G. DISPLAY / ENCODER SURFACE (interface.cpp / display.cpp / audio.cpp)
 *      • Single-writer OLED: ALL runtime drawing goes through renderUIState()
 *        (display.cpp), driven only by display_refresh_task (audio.cpp) under
 *        i2cMutex.  No other task touches the SH1107 at runtime, so the I2C bus
 *        is never contended.  Pre-task boot splashes draw before the scheduler
 *        starts; the post-boot "READY" splash and handleFactoryReset() both take
 *        i2cMutex so they cannot collide with an in-flight render transaction.
 *        handleFactoryReset() holds the mutex through esp_restart() so its
 *        "PLEASE WAIT" message is not overwritten. [DISP-LOCK]
 *      • Encoder: ESP32Encoder full-quad / ENC_PPR=4 per detent; EncoderPoll
 *        keeps the sub-detent remainder and applies EMA-smoothed velocity
 *        ×1/×2/×4 (jitter-immune ramp).  Read
 *        only from control_surface_task (200 Hz, Core 0 prio 10).
 *      • Gesture classifier (ButtonPoll::poll): SINGLE / DOUBLE / LONG with a
 *        delta-cancel guard — turning the encoder while a click is pending
 *        cancels the click, so a turn never doubles as a button event.
 *      • SEQ MATRIX (l1==6) is a self-contained editor branch with an early
 *        return; its DOUBLE = back gesture stores MENU_L1 (NOT IDLE).  Both the
 *        editor branch and renderUIState treat (l1==6 && mst!=MENU_L1) as
 *        "in the grid", so IDLE there would trap the UI with no escape. [GRID-ESC]
 *
 *   H. D-BEAM — STRICT 3-WAY SEPARATION  ⚠ DO NOT MIX  (laser.cpp / dbeam.cpp)
 *      The D-BEAM has THREE fully independent signal paths.  They must NEVER be
 *      cross-wired — every past "kakophony"/stuck-note/jitter regression came
 *      from collapsing two of them together.  Keep them apart:
 *
 *        1) TRIGGER (note on/off) — PURELY DIGITAL, ZERO ADC.
 *           LT1016 comparator → 74HC74 D-flip-flop peak latch → PIN_TRIGGER,
 *           read once per dwell in laser_sweep_task.  Never reads the ADC, never
 *           calls checkDbeamThreshold() in the hot path.  This is what makes
 *           triggering rock-solid regardless of ambient light or ADC state.
 *
 *        2) DYNAMIC THRESHOLD / TELEMETRY — Branch A, PER-STRING ADC.
 *           Per-string boxcar → per-string Kalman (g_kalman_ac[si]) →
 *           g_last_good_data[si], gated by dbeamLit + dbeamLastStringIdx so only
 *           the lit string's reflection is attributed to it.  Feeds the DAC
 *           threshold calibration + the telemetry scope.  NEVER the trigger.
 *
 *        3) EXPRESSION (CC / filter / pitch) — Branch B, CONTINUOUS ENVELOPE.
 *           ONE peak envelope follower (g_expr_env) fed by EVERY lit dwell across
 *           all strings (on-time only — dark gaps feed nothing, so the stream is
 *           continuous and never flushed to 0).  Fast ATTACK rises to the nearest
 *           hand; slow RELEASE rides over the 7/8 no-hand dwells.  This replaced
 *           the old 8-string AVERAGE, which diluted the one hand-string with the
 *           7 no-hand baselines → low, coverage-dependent, jittery values that
 *           were unusable for expression.  ATTACK/RELEASE are runtime-adjustable
 *           SSOT atomics (dbeamExprAttack 0.20–0.50, dbeamExprRelease 0.007–0.020
 *           in globals.h), editable via D-BEAM menu (Env Atk / Env Rel) with a
 *           live preview bar, and NVS-persisted (DBeamSettings.expr_*).
 *
 *        4) FOG REJECT — Branch C, ISOLATED COPY (fog.h).  OPT-IN, default OFF.
 *           The dbeam ADC task publishes a COPY of g_kalman_ac[si].x into its own
 *           array (g_fogAmp[], via fogPublishAmp()).  fogAccept(ci) compares the
 *           tested string to the 2nd-smallest amplitude (the common-mode fog
 *           floor) + a user margin.  The laser trigger ANDs it with the digital
 *           latch (beamBrokenRaw && fogAccept), so it can only SUPPRESS a stray
 *           fog break — never fabricate one — and writes back to NOTHING.  When
 *           disabled it returns true → branch 1 behaves exactly as before.  Tuned
 *           via HARP SETUP → Fog Reject / Fog Margin, NVS-persisted (laser.fog_*).
 *           TELEMETRY → FOG REJECT view (scope page 7) plots g_fogAmp[] as 8 bars
 *           with a dotted FLOOR line + dashed ACCEPT (floor+margin) line; bars that
 *           clear ACCEPT invert = they pass the gate → live margin-tuning aid.
 *
 *      RULE OF THUMB: averaging across strings = expression poison; per-string
 *      buckets = threshold only; the digital latch = the only trigger source;
 *      fog reject reads its OWN copy and gates, never feeds back.
 *
 * ═══════════════════════════════════════════════════════════════════════════
 * 9. KNOWN LIMITATIONS & FUTURE WORK
 *
 *   • OctopusApp CHAIN editor (song mode) builds bank A→D play order with
 *     repeats; per-step P-lock motion editing in the App is still TODO.
 *   • I2S DMA audio buffers are internal DRAM (DMA requires it); FX scratch +
 *     delay/reverb lines use PSRAM-first allocation ([H4] effect.h) with a DRAM
 *     fallback.  Wavetables (assets.cpp) are PROGMEM/IROM (cache-mapped).
 *   • Hardware OLED grid edits only steps 0–15; the engine supports up to 64
 *     steps (set via App / Length) — steps 16–63 are not visible on the OLED.
 *   • Motion playback: P-lock recording + playback live in
 *     sequencer_background_task.  App motion editor is future work.
 *   • NVS persistence [S10/S11]: four blobs in namespace "octopus_v5":
 *     "settings" (~2 KB), "patterns" (~8 KB), sparse "banks" (~4 KB), sparse
 *     "motion" (~4 KB).  ~8-9 NVS pages needed → 256 KB NVS partition placed
 *     AFTER the app (app factory @ 0x10000; NVS @ 0x3B0000; coredump @ 0x3F0000).
 *     The pre-app gap (0x9000..0x10000) is too small (28 KB).  Scoped SAVE/RESET
 *     (ResetScope): FULL / BANKS_PATTERNS / MOTION / SETTINGS — reset applies
 *     RAM wipe then NvsWorker persist + restart; SAVE menu persists only.
 *     Boot factory reset: hold OC+SCALE 150 ms during initHardwareInterface().
 * ═══════════════════════════════════════════════════════════════════════════ */

#endif /* CODE_INFO_H */
