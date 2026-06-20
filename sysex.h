/* ═════════════════════════════════════════════════════════════════════════════
 * Octopus PRO XL v6.0.00 — Laser Harp Groovebox
 * © 2026 DIODAC ELECTRONICS / iSystem. All Rights Reserved.
 *
 * PROPRIETARY AND CONFIDENTIAL. Unauthorized copying, distribution, modification,
 * or use of this software or firmware, in whole or in part, is strictly prohibited
 * without prior written permission from DIODAC ELECTRONICS.
 * ═════════════════════════════════════════════════════════════════════════════
 * sysex.h — v6.0.00  OCTOPUS SYSEX WIRE PROTOCOL  (command IDs)
 *
 * Single source of truth for the OctopusApp ⇄ device SysEx command table.
 * Extracted from midi.h so the protocol identity lives in one small, dependency-
 * free header (midi.h includes this).  The OctopusApp.html `CMD = { … }` JS map
 * MUST stay in lock-step with the constants below.
 *
 * Frame format (7 bytes):  { 0xF0, ID, sub, cmd-base, v14hi, v14lo, 0xF7 }
 *   ID  0x7C = device → App   (all firmware echoes/replies via txSysex)
 *   ID  0x7D = App → device   (the ONLY ID parseMidiByte accepts)
 *   sub 0x00 = cmd   0–127     (cmd-base = cmd)
 *   sub 0x01 = cmd 128–163     (cmd-base = cmd − 128)
 *   v14 = (v14hi << 7) | v14lo   (0–16383)
 *
 * [BLOB] Variable-length PATCH BLOB (one atomic full-preset transfer):
 *   { 0xF0, ID, 0x02, engine, p0hi,p0lo, … p15hi,p15lo, 0xF7 }   (37 bytes)
 *     engine 0 = harp, 1 = seq;  16 params × (hi7,lo7) in SynthParam order.
 *   Replaces the old 16-message per-param echo on preset recall so the App's
 *   knobs follow a preset change (from ANY source — hardware, MIDI PC, App) in
 *   a single, glitch-free message.  BOTH sides map cmd = base(0|16) + index, so
 *   index 11 naturally lands on the LFO-route select (App CMD 11 / 27).
 * The direction-tagged ID gives MIDI-loop immunity: the firmware ignores its own
 * 0x7C, so a looped-back stream cannot fake an App heartbeat or re-toggle
 * transport.  Migration ONLY ever appends new commands (never renumbers).
 * ═════════════════════════════════════════════════════════════════════════════ */
#pragma once
#ifndef SYSEX_H
#define SYSEX_H

#include <cstdint>

/* ─────────────────────────────────────────────────────────────────────────────
 * SysEx command table
 *
 * CMD  0–127   sub-byte 0x00  (backward-compatible with v28.9 OctopusApp)
 * CMD 128–163  sub-byte 0x01  value on wire = cmd - 128
 *
 * Block layout:
 *   0– 15  Harp synth params  (P_WAVEFORM … P_SPARE2, maps to SynthParam enum)
 *  16– 31  Seq synth params   (same layout)
 *  32– 63  Drum params        (8 drums × 4: tune/decay/vol/noise)
 *  64– 96  Master / FX        (vol, pitch, EQ, tube, DJ, D-BEAM, octave…)
 *  97–127  Transport / system (BPM, bank, grid, patterns, FX slots…)
 * 128–137  v5 Laser / wire    (laser show, hue, MIDI channel routing)
 * 138–147  v5.3 Extensions    (seq chain, mutes, aux FX, drum wave, D-BEAM route)
 * 148–163  v5.3.1+ Song / transport / pan / grid bulk
 * ─────────────────────────────────────────────────────────────────────────────*/

/* [BLOB] SysEx sub-byte that marks a variable-length patch blob (see header). */
static constexpr uint8_t SX_SUB_PATCH_BLOB     = 0x02;
static constexpr uint8_t SX_SUB_USR_SOUND_NAME = 0x03; /* App→device / device→App */
static constexpr uint8_t SX_SUB_USR_PAT_NAME   = 0x04;

/* ── Harp synth (cmd 0–15) ──────────────────────────────────────────────── */
static constexpr uint8_t CMD_H_WAVE = 0; /* base — add SynthParam index */

/* ── Seq synth (cmd 16–31) ──────────────────────────────────────────────── */
static constexpr uint8_t CMD_S_WAVE = 16; /* base */

/* ── Drum params (cmd 32–63) — base 32, drum_ch*4 + param ─────────────── */
/* No named constants needed; addressed as 32 + (ch<<2) + param_idx         */

/* ── Master / FX / hardware (cmd 64–96) ────────────────────────────────── */
static constexpr uint8_t CMD_M_VOL = 64;
static constexpr uint8_t CMD_H_VOL = 65;
static constexpr uint8_t CMD_S_VOL = 66;
static constexpr uint8_t CMD_D_VOL = 67;
static constexpr uint8_t CMD_PITCH = 68;
static constexpr uint8_t CMD_EQ_L = 69;
static constexpr uint8_t CMD_EQ_H = 70;
static constexpr uint8_t CMD_D_REV = 71;
static constexpr uint8_t CMD_D_DLY = 72;
static constexpr uint8_t CMD_TB_DRV = 73;
static constexpr uint8_t CMD_TB_TONE = 74;
static constexpr uint8_t CMD_TB_MIX = 75;
static constexpr uint8_t CMD_DJ_FQ = 76;
static constexpr uint8_t CMD_DJ_RES = 77;
static constexpr uint8_t CMD_DJ_MIX = 78;
static constexpr uint8_t CMD_HLFO_R = 79;
static constexpr uint8_t CMD_HLFO_D = 80;
static constexpr uint8_t CMD_SLFO_R = 81;
static constexpr uint8_t CMD_SLFO_D = 82;
static constexpr uint8_t CMD_DB_CURVE = 83;
static constexpr uint8_t CMD_DB_OFFSET = 84;
static constexpr uint8_t CMD_DB_RANGE = 85;
static constexpr uint8_t CMD_HLFO_RT = 86;
static constexpr uint8_t CMD_SLFO_RT = 87;
static constexpr uint8_t CMD_DB_ENABLED = 88;
static constexpr uint8_t CMD_D_FX_IDX_B = 89;
static constexpr uint8_t CMD_HW_H_OCT = 90;
static constexpr uint8_t CMD_HW_S_OCT = 91;
static constexpr uint8_t CMD_HW_GATE = 92;
static constexpr uint8_t CMD_HW_WHITE = 93;
static constexpr uint8_t CMD_HW_S_LEN = 94;
static constexpr uint8_t CMD_TRANSPOSE = 95;
static constexpr uint8_t CMD_HW_MARGIN = 96;

/* ── Transport / system (cmd 97–127) ───────────────────────────────────── */
static constexpr uint8_t CMD_BPM = 97;
static constexpr uint8_t CMD_BANK = 98;
static constexpr uint8_t CMD_PING = 99;
static constexpr uint8_t CMD_GRID_TOG = 100;
static constexpr uint8_t CMD_CLR_PLOCKS = 101;
static constexpr uint8_t CMD_LOAD_PAT_S = 102;
static constexpr uint8_t CMD_LOAD_PAT_D = 103;
static constexpr uint8_t CMD_TRIG_MODE = 104;
static constexpr uint8_t CMD_STEP_SYNC = 105;
static constexpr uint8_t CMD_H_PATCH = 106;
static constexpr uint8_t CMD_S_PATCH = 107;
static constexpr uint8_t CMD_H_SCALE = 108;
static constexpr uint8_t CMD_S_SCALE = 109;
static constexpr uint8_t CMD_D_FX_IDX = 110;
static constexpr uint8_t CMD_H_FX_IDX_B = 111;
static constexpr uint8_t CMD_H_FX_MIX = 112;
static constexpr uint8_t CMD_H_FX_TIME = 113;
static constexpr uint8_t CMD_H_FX_SIZE = 114;
static constexpr uint8_t CMD_H_FX_IDX = 115;
static constexpr uint8_t CMD_S_FX_MIX = 116;
static constexpr uint8_t CMD_S_FX_TIME = 117;
static constexpr uint8_t CMD_S_FX_SIZE = 118;
static constexpr uint8_t CMD_S_FX_IDX = 119;
static constexpr uint8_t CMD_M_FX_IDX = 120;
static constexpr uint8_t CMD_H_DLY_MIX = 121;
static constexpr uint8_t CMD_H_REV_MIX = 122;
static constexpr uint8_t CMD_S_DLY_MIX = 123;
static constexpr uint8_t CMD_S_REV_MIX = 124;
static constexpr uint8_t CMD_FETCH = 125;
static constexpr uint8_t CMD_HARD_SAVE = 126;
static constexpr uint8_t CMD_S_FX_IDX_B = 127;

/* ── v5 Laser / wire (cmd 128–137, sub-byte 0x01, wire value = cmd-128) ── */
static constexpr uint8_t CMD_LSR_SHOW = 128;
static constexpr uint8_t CMD_MIDI_HUE = 129;
static constexpr uint8_t CMD_HUE_BASE = 130;
static constexpr uint8_t CMD_HUE_ATK = 131;
static constexpr uint8_t CMD_HUE_DEC = 132;
static constexpr uint8_t CMD_HUE_SUS = 133;
static constexpr uint8_t CMD_HUE_REL = 134;
static constexpr uint8_t CMD_WIRE_HARP_CH = 135;
static constexpr uint8_t CMD_WIRE_SEQ_CH = 136;
static constexpr uint8_t CMD_WIRE_DRUM_CH = 137;

/* ── v5.3 Extensions (cmd 138–147, sub-byte 0x01, wire value = cmd-128) ── */
static constexpr uint8_t CMD_SEQ_CHAIN = 138;   /* seqActiveChain 0–3                */
static constexpr uint8_t CMD_H_MUTE = 139;      /* mixHarpMute  0=off / 16383=mute   */
static constexpr uint8_t CMD_S_MUTE = 140;      /* mixSeqMute                        */
static constexpr uint8_t CMD_D_MUTE = 141;      /* mixDrumsMute                      */
static constexpr uint8_t CMD_AUX_DLY_FB = 142;  /* masterAuxDlyFb  0–0.95            */
static constexpr uint8_t CMD_AUX_REV_DMP = 143; /* masterAuxRevDamp 0–1              */
static constexpr uint8_t CMD_DRUM_WAVE = 144;   /* drum body waveform: (ch<<5)|widx  */
static constexpr uint8_t CMD_DB_ROUTE = 145;  /* DbeamRoute 0=OFF 1=MOD 2=VOL 3=CUT */
/* 146/147 (DB_CUT_CC/DB_MOD_CC) removed v6.0 — D-BEAM emits no MIDI (local DSP). */

/* ── v5.3.1 Song mode + transport arbitration (sub-byte 0x01, wire 20-29) ── */
static constexpr uint8_t CMD_SONG_MODE = 148;
static constexpr uint8_t CMD_SONG_SLOT = 149;
static constexpr uint8_t CMD_SONG_STEP = 150; /* [step:4][bank:4][chain:2][rpt:4] */
static constexpr uint8_t CMD_SONG_STEPS_N = 151;
static constexpr uint8_t CMD_SONG_POS = 152;        /* echo: [step:4]<<4 | [repeat:4]   */
static constexpr uint8_t CMD_TRANSPORT = 153;       /* 0=stop 1=play 2=pause 3=rec_tog  */
static constexpr uint8_t CMD_TRANSPORT_AVAIL = 154; /* 1=hw_ctrl 0=app_ctrl             */
static constexpr uint8_t CMD_APP_SYNC_REQ = 155;
static constexpr uint8_t CMD_SESSION_SAVE = 156;
static constexpr uint8_t CMD_SESSION_LOAD = 157;
static constexpr uint8_t CMD_DRUM_KIT = 158;  /* drum kit select: 0=909 1=808 2=Trap 3=House */
/* [P6] Per-instrument pan −1..+1 (v14: 0=full L, 8192=centre, 16383=full R) */
static constexpr uint8_t CMD_H_PAN = 159;
static constexpr uint8_t CMD_S_PAN = 160;
static constexpr uint8_t CMD_D_PAN = 161;
/* [G2] Grid bulk sync — absolute half-row state (NOT a toggle, unlike GRID_TOG).
 *   v14 = (bank<<12) | (row<<8) | (page<<4) | byteMask
 *     bank 0-3 (bits 13-12), row 0-15 (bits 11-8), page 0-3 (bits 5-4),
 *     byteMask = 8 step bits within that page's half-row.
 *   CMD_GRID_ROW_LO carries steps page*16+0..7, HI carries page*16+8..15.
 *   Used both directions: ESP→App full dump on connect + live hardware-edit
 *   echo; App→ESP optional bulk row write.                                    */
static constexpr uint8_t CMD_GRID_ROW_LO = 162;
static constexpr uint8_t CMD_GRID_ROW_HI = 163;
/* [v6.0] Device→App telemetry: audio-core CPU load, 0–100 (raw %, not 14-bit
 * scaled).  Pushed by the sync supervisor (~600 ms) while the App is connected
 * so the header readout stays live without flooding the wire. App-only display. */
static constexpr uint8_t CMD_CPU_LOAD = 164;
/* [v6.0] Harp play-mode select (bidirectional): 0=POLY8 1=STRINGS 2=SOLO.
 * App→device sets the mode; device echoes it back (and on hardware OC-cycle)
 * so the App's POLY/STR/SOLO buttons always mirror currentPlayMode.          */
static constexpr uint8_t CMD_PLAY_MODE = 165;
/* [v6.0 WS1] Harp internal pitch-bend / manual tune (bidirectional, CONTINUOUS).
 * Wire v14 0..16383, centre 8192 = unity; ±1 octave: semis = (v14-8192)/8192*12,
 * harpPitchMult = 2^(semis/12).  A continuous tune knob suits the laser-harp far
 * better than discrete transpose/octave switches. */
static constexpr uint8_t CMD_H_PITCH = 166;
/* [LASER-SHOW v2] Projector animation controls (bidirectional).
 *   CMD_LSR_ANIM      — Anim Mode: v14 0..16383 maps to 4 modes (PULSE/CHASE/
 *                       STROBE/WAVE), i.e. mode = round(v14/16383*3).
 *   CMD_LSR_DRUMFLASH — drum-flash depth, v14 0..16383 → 0..1.                 */
static constexpr uint8_t CMD_LSR_ANIM = 167;
static constexpr uint8_t CMD_LSR_DRUMFLASH = 168;
/* [SAVE/LOAD/RESET v2] App↔device parity for the scoped persistence menus.
 *   CMD_SCOPED_RESET — v14 = ResetScope (0 FULL / 1 BANKS_PATTERNS / 2 MOTION /
 *                      3 SETTINGS).  Device wipes RAM + persists + reboots.
 *   CMD_SEQ_CLEAR    — clear active pattern grid + P-locks + reset both sounds
 *                      to preset 0 (mirror of hardware SEQ SETUP → Clear).
 *   CMD_SOFT_RESET   — CLEAR extended: sounds + nav working image → initial
 *                      (RAM-only, no NVS, no reboot).
 * CMD_SESSION_LOAD now also carries the scope in v14 (0 = FULL, back-compatible).*/
static constexpr uint8_t CMD_SCOPED_RESET = 169;
static constexpr uint8_t CMD_SEQ_CLEAR    = 170;
static constexpr uint8_t CMD_SOFT_RESET   = 171;
/* [USER-SLOTS] App↔device parity for the user sound-slot save/load/name menus.
 *   CMD_USR_SOUND_SAVE — v14 = (engine<<13)|slot ; snapshot live patch → user
 *                        slot (128+slot) + persist (banks+names, no reboot).
 *   CMD_USR_SOUND_LOAD — v14 = (engine<<13)|slot ; recall user slot → live patch.
 *     engine 0 = harp, 1 = seq;  slot 0..63 (NUM_USER_SLOTS).
 *   CMD_USR_SOUND_NAME — App→device rename via the variable-length NAME BLOB
 *     (sub 0x03): { 0xF0,ID,0x03, engine, slot, n0..n14, 0xF7 } (15 name bytes).
 *     Reserved here; wired with the App slot browser (Step 12).               */
static constexpr uint8_t CMD_USR_SOUND_SAVE = 172;
static constexpr uint8_t CMD_USR_SOUND_LOAD = 173;
static constexpr uint8_t CMD_USR_SOUND_NAME = 174;
/* [USER-PAT-SLOTS] App↔device parity for the user pattern-slot save/load/name.
 *   CMD_USR_PAT_SAVE — v14 = slot 0..63 ; snapshot active pattern → user slot +
 *                      persist (usrpat+usrpatnames, no reboot).
 *   CMD_USR_PAT_LOAD — v14 = slot 0..63 ; recall user slot → active bank/chain.
 *   CMD_USR_PAT_NAME — App→device rename via NAME BLOB sub 0x04 (Step 12).    */
static constexpr uint8_t CMD_USR_PAT_SAVE = 175;
static constexpr uint8_t CMD_USR_PAT_LOAD = 176;
static constexpr uint8_t CMD_USR_PAT_NAME = 177;
/* [PHASE-3] MIDI I/O pitch-bend mapping + D-BEAM target synth (bidirectional).
 *   CMD_PB_RANGE  — v14 = ±semitones (0..24, symmetric up/down).
 *   CMD_PB_ENABLE — v14 0=OFF, 16383=ON (incoming USB MIDI pitch wheel).
 *   CMD_DB_TARGET — v14 0=Harp synth, 1=Melody synth (DbeamTarget).
 *   CMD_DRUM_PITCH — v14 encodeMasterPitch; drum-only pitch.
 *   CMD_SEQ_ARP_* — v14 enable/pattern/rate/gate (182–185).                   */
static constexpr uint8_t CMD_PB_RANGE   = 178;
static constexpr uint8_t CMD_PB_ENABLE  = 179;
static constexpr uint8_t CMD_DB_TARGET  = 180;
static constexpr uint8_t CMD_DRUM_PITCH = 181; /* v14: encodeMasterPitch — drum-only */
static constexpr uint8_t CMD_SEQ_ARP_EN   = 182; /* v14 0=OFF 16383=ON                    */
static constexpr uint8_t CMD_SEQ_ARP_PAT  = 183; /* v14 0–7 arp::Pattern                  */
static constexpr uint8_t CMD_SEQ_ARP_RATE = 184; /* v14 0–7 arp::Rate                     */
static constexpr uint8_t CMD_SEQ_ARP_GATE = 185; /* v14 0–7 gate duty index               */
static constexpr uint8_t CMD_HARP_ARP_EN   = 186; /* v14 0=OFF 16383=ON                    */
static constexpr uint8_t CMD_HARP_ARP_PAT  = 187; /* v14 0–3 Up/Down/UpDn/Rnd              */
static constexpr uint8_t CMD_HARP_ARP_RATE = 188; /* v14 0–3 1/8…1/16T                     */
static constexpr uint8_t CMD_HARP_ARP_GATE = 189; /* v14 0–3 gate duty index               */
/* Drum insert FX-A live params (slot A SynthFX — independent of D.REV/D.DLY aux). */
static constexpr uint8_t CMD_D_FX_WET = 190; /* v14 0–16383 → fx_mix 0..1               */
static constexpr uint8_t CMD_D_FX_P1  = 191; /* v14 0–16383 → p1 0..30 (rate/depth)     */
static constexpr uint8_t CMD_D_FX_P2  = 192; /* v14 0–16383 → p2 0..250 (depth/swing)   */
static constexpr uint8_t CMD_COUNT = 193; /* total command count              */

#endif /* SYSEX_H */
