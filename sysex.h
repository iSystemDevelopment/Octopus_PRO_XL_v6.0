/* ═════════════════════════════════════════════════════════════════════════════
 * Octopus PRO XL v6.1.00 — Laser Harp Groovebox
 * © 2026 DIODAC ELECTRONICS / iSystem. All Rights Reserved.
 *
 * PROPRIETARY AND CONFIDENTIAL. Unauthorized copying, distribution, modification,
 * or use of this software or firmware, in whole or in part, is strictly prohibited
 * without prior written permission from DIODAC ELECTRONICS.
 * ═════════════════════════════════════════════════════════════════════════════
 * sysex.h — v6.1.00  OCTOPUS SYSEX WIRE PROTOCOL  (command IDs)
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
 * PATCH BLOB — variable-length full-preset transfer:
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
 * CMD  0–127   sub-byte 0x00
 * CMD 128+     sub-byte 0x01  (wire cmd-base = full_cmd − 128)
 *
 * Block layout:
 *   0– 15  Harp synth params  (P_WAVEFORM … P_SPARE2, SynthParam enum order)
 *  16– 31  Seq synth params   (same layout)
 *  32– 63  Drum params        (8 drums × 4: tune/decay/vol/noise)
 *  64– 96  Master / FX        (vol, pitch, EQ, tube, DJ, D-BEAM, octave…)
 *  97–127  Transport / system (BPM, bank, grid, patterns, FX slots…)
 * 128–137  Laser / wire       (laser show, hue ADSR, MIDI channel routing)
 * 138–147  Seq / mix / D-BEAM (chain, mutes, aux FX, drum wave, D-BEAM route)
 * 148–163  Song / transport / pan / grid bulk
 * 164–194  v6 extensions      (CPU load, play mode, scoped persist, user slots,
 *                               arp, drum insert FX, seq restart; 194 reserved)
 * ─────────────────────────────────────────────────────────────────────────────*/

/* Patch blob sub-byte (variable-length full-preset transfer — see module header). */
static constexpr uint8_t SX_SUB_PATCH_BLOB     = 0x02;
static constexpr uint8_t SX_SUB_USR_SOUND_NAME = 0x03; /* App→device / device→App */
static constexpr uint8_t SX_SUB_USR_PAT_NAME   = 0x04;
/* Grid-row bulk sub-byte (clean bank/row/page encoding — see SX_SUB_GRID_ROW frame). */
static constexpr uint8_t SX_SUB_GRID_ROW       = 0x05;

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
/* CMD_S_SCALE: RESERVED — scale is GLOBAL (harp + seq share harpScaleIndex via
 * CMD_H_SCALE).  No firmware handler and absent from the OctopusApp CMD map; the
 * App routes both scale dropdowns through CMD_H_SCALE.  Kept only so the wire IDs
 * never renumber.  Do not repurpose without a protocol-version bump.            */
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

/* ── Laser / wire (cmd 128–137, sub-byte 0x01, wire value = cmd−128) ── */
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

/* ── Seq / mix / D-BEAM (cmd 138–147, sub-byte 0x01) ── */
static constexpr uint8_t CMD_SEQ_CHAIN = 138;   /* seqActiveChain 0–3                */
static constexpr uint8_t CMD_H_MUTE = 139;      /* mixHarpMute  0=off / 16383=mute   */
static constexpr uint8_t CMD_S_MUTE = 140;      /* mixSeqMute                        */
static constexpr uint8_t CMD_D_MUTE = 141;      /* mixDrumsMute                      */
static constexpr uint8_t CMD_AUX_DLY_FB = 142;  /* masterAuxDlyFb  0–0.95            */
static constexpr uint8_t CMD_AUX_REV_DMP = 143; /* masterAuxRevDamp 0–1              */
static constexpr uint8_t CMD_DRUM_WAVE = 144;   /* drum body waveform: (ch<<5)|widx  */
static constexpr uint8_t CMD_DB_ROUTE = 145;  /* DbeamRoute 0=OFF 1=MOD 2=VOL 3=CUT */
/* 146/147 (DB_CUT_CC/DB_MOD_CC) removed v6.0 — D-BEAM emits no MIDI (local DSP). */

/* ── Song mode + transport (sub-byte 0x01, wire 20–29 for 148–163) ── */
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
/* Per-instrument pan −1..+1 (v14: 0=full L, 8192=centre, 16383=full R) */
static constexpr uint8_t CMD_H_PAN = 159;
static constexpr uint8_t CMD_S_PAN = 160;
static constexpr uint8_t CMD_D_PAN = 161;
/* Grid bulk sync — RETIRED v6.1, IDs RESERVED (never renumber).
 *   The old v14 packing (bank<<12 | row<<8 | page<<4 | byteMask) overlapped the
 *   8 step bits (0-7) with the page field (bits 4-5), corrupting steps 4-5 on
 *   pages 1-3.  Replaced — BOTH directions — by the lossless SX_SUB_GRID_ROW
 *   blob (sub 0x05, see frame below): ESP→App full dump on connect + live
 *   hardware-edit echo, and App→ESP bulk row writes.  No firmware RX handler and
 *   no sender remains; the constants persist only to keep the wire IDs stable.  */
static constexpr uint8_t CMD_GRID_ROW_LO = 162;
static constexpr uint8_t CMD_GRID_ROW_HI = 163;
/* Device→App telemetry, pushed ~600 ms while App connected (App-only display).
 * 14-bit-safe packing:
 *   bits 0-6  : audio-core load %  (0–100)
 *   bits 7-12 : out-ring drop count, saturating 0–63
 *   bit  13   : P-lock lanes-full flag (active pattern's 4 lanes all allocated)
 * (bit 14 is unaddressable in a 14-bit SysEx value — fields must stay ≤ bit 13.) */
static constexpr uint8_t CMD_CPU_LOAD = 164;
/* Harp play-mode (bidirectional): 0=POLY8 1=STRINGS 2=SOLO. Device echoes on
 * hardware OC-cycle so the App mirrors currentPlayMode. */
static constexpr uint8_t CMD_PLAY_MODE = 165;
/* Harp manual tune (bidirectional): v14 centre 8192 = unity; ±1 octave range. */
static constexpr uint8_t CMD_H_PITCH = 166;
/* Laser show animation controls (bidirectional). */
static constexpr uint8_t CMD_LSR_ANIM = 167;
static constexpr uint8_t CMD_LSR_DRUMFLASH = 168;
/* Scoped persistence (App↔device parity for SAVE / LOAD / RESET menus).
 *   CMD_SESSION_SAVE / CMD_SESSION_LOAD / CMD_SCOPED_RESET: v14 = ResetScope + 1
 *     (FULL=1 … SETTINGS=4).  v14=0 NACK; v14=16383 ACK echo (ignored on RX).
 *   CMD_SCOPED_RESET — FULL/BANKS+PATS: arm NVS pend_rst + reboot; wipe on next boot.
 *     SETTINGS/MOTION: applyResetScope + settings_commit_reset_scoped via NvsWorker.
 *   CMD_SEQ_CLEAR    — clear active pattern + reset companion sounds (mirror hardware).
 *   CMD_SOFT_RESET (171) — retired v6.1; ignored on RX. */
static constexpr uint8_t CMD_SCOPED_RESET = 169;
static constexpr uint8_t CMD_SEQ_CLEAR    = 170;
static constexpr uint8_t CMD_SOFT_RESET   = 171; /* retired v6.1 — RX ignored */
/* User sound slots (engine 0=harp, 1=seq; slot 0..63).
 *   CMD_USR_SOUND_SAVE / LOAD — v14 = (engine<<13)|slot.
 *   CMD_USR_SOUND_NAME — rename via NAME BLOB sub 0x03. */
static constexpr uint8_t CMD_USR_SOUND_SAVE = 172;
static constexpr uint8_t CMD_USR_SOUND_LOAD = 173;
static constexpr uint8_t CMD_USR_SOUND_NAME = 174;
/* User pattern slots (slot 0..63; melody+drum grid + companion sounds).
 *   CMD_USR_PAT_SAVE / LOAD — v14 = slot.
 *   CMD_USR_PAT_NAME — rename via NAME BLOB sub 0x04. */
static constexpr uint8_t CMD_USR_PAT_SAVE = 175;
static constexpr uint8_t CMD_USR_PAT_LOAD = 176;
static constexpr uint8_t CMD_USR_PAT_NAME = 177;
/* Pitch-bend mapping + D-BEAM target + drum pitch + arp params (bidirectional). */
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
/* App→device: reset step counter to 0 without stopping playback (post randomise). */
static constexpr uint8_t CMD_SEQ_RESTART = 193;
/* CMD_STEP_PHASE (194) — reserved; not emitted (sub-step PLL echo removed). */
static constexpr uint8_t CMD_STEP_PHASE = 194;
static constexpr uint8_t CMD_COUNT = 195; /* total command count              */

#endif /* SYSEX_H */
