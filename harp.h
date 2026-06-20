/* ═════════════════════════════════════════════════════════════════════════════
 * Octopus PRO XL v6.0.00 — Laser Harp Groovebox
 * © 2026 DIODAC ELECTRONICS / iSystem. All Rights Reserved.
 *
 * PROPRIETARY AND CONFIDENTIAL. Unauthorized copying, distribution, modification,
 * or use of this software or firmware, in whole or in part, is strictly prohibited
 * without prior written permission from DIODAC ELECTRONICS.
 * ═════════════════════════════════════════════════════════════════════════════
 * harp.h — v6.0.00  LASER-HARP INSTRUMENT — PUBLIC INTERFACE
 *
 * Dedicated, self-contained laser-harp engine.  Replaces the former
 * synth.h + synth.cpp and the HARP half of the shared synth_core.h:
 *
 *   • Pure laser-harp DSP (8-voice ADSR + dual-osc + SVF + LFO) — implemented
 *     out-of-line in harp.cpp, FULLY ISOLATED in a private anonymous namespace
 *     (its own copies of sinf / noise / wavetable / SVF / soft-clip), so the
 *     harp shares no symbols with the groovebox seq/drum engine (synth_core.h).
 *
 *   • Three play modes, exposed as one semantic vocabulary:
 *       POLY8   — 8-voice polyphony, free-voice allocation (MIDI keyboard).
 *       STRINGS — 1:1 string→voice, plucked envelope (sustain = 0), physical
 *                 string-vibration emulation + laser hue ADSR.
 *       SOLO    — monophonic, bidirectional last-note priority. Newest beam
 *                 sounds; older still-held beams are remembered on a stack and
 *                 the most-recent one re-sounds when the newest is lifted.
 *
 *   • D-BEAM expression is CONSUMED, not reimplemented.  dbeam.cpp stays the
 *     single source of the amplitude-follower expression; harp only calls
 *     updateDbeamExpression()/routeDbeamExpression() (harpPumpExpression) and reads
 *     dbeam_svf_cutoff / dbeam_mod_depth / mixHarpVol in its DSP.
 *
 * BOUNDARY: harp owns synthesis + expression CONTROL (voice DSP, string
 * vibration value, hue ADSR engine).  laser.cpp keeps the hardware rendering
 * (galvo DAC writes, RGB PWM) and calls into this module.
 * ═════════════════════════════════════════════════════════════════════════════ */
#pragma once
#ifndef HARP_H
#define HARP_H

#include <Arduino.h>
#include "globals.h"   /* Voice, SynthGlobal, PlayMode, harpVoices, harpLivePatch */

/* ── Audio entry — one DMA buffer of laser-harp audio (Core 0 audio task). ── */
bool IRAM_ATTR harp_synth_fill_buf(int16_t* buf, size_t frames);

/* ── Voice lifecycle (mode-aware).  vel is 0–127. ──────────────────────────
 * harpNoteOn maps stringIdx through the active scale + harp octave, applies the
 * current play-mode policy (SOLO soft-kill / STRINGS pluck / POLY8), and arms
 * the dedicated string→voice.  harpNoteOff releases the owning voice.          */
void IRAM_ATTR harpNoteOn(int stringIdx, uint8_t vel);
void IRAM_ATTR harpNoteOff(int stringIdx);
void IRAM_ATTR harpReleaseVoice(int voiceIdx);  /* direct voice release         */
void           harpAllNotesOff();

/* ── MIDI channel-instrument path (polyphonic note allocation). ────────────── */
void IRAM_ATTR harpMidiNoteOn(uint8_t note, uint8_t vel);
void IRAM_ATTR harpMidiNoteOff(uint8_t note);

/* ── Play mode (POLY8 / STRINGS / SOLO). ───────────────────────────────────── */
void     harpSetPlayMode(PlayMode m);
PlayMode harpPlayMode();

/* ── D-BEAM expression pump — calls dbeam (no dbeam modification). ──────────── */
void IRAM_ATTR harpPumpExpression();

/* ── STRINGS string-vibration emulation + hue ADSR (control owned by harp). ──
 * laser.cpp renders: it supplies the nominal galvo position and applies the
 * returned target; harp computes the ADSR-driven wobble and runs the hue env.  */
uint16_t IRAM_ATTR harpStringVibratoTarget(int stringIdx, uint16_t nomPos, int scaleIdx);
void     IRAM_ATTR harpHueNoteOn(int stringIdx);
void     IRAM_ATTR harpHueNoteOff(int stringIdx);
void     IRAM_ATTR harpHueAdvance(int stringIdx);

/* ── Telemetry — active harp voices. ───────────────────────────────────────── */
int harpActiveVoiceCount();

#endif /* HARP_H */
