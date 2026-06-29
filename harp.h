/* ═════════════════════════════════════════════════════════════════════════════
 * Octopus PRO XL v6.1.01 — Laser Harp Groovebox
 * © 2026 DIODAC ELECTRONICS / iSystem. All Rights Reserved.
 *
 * PROPRIETARY AND CONFIDENTIAL. Unauthorized copying, distribution, modification,
 * or use of this software or firmware, in whole or in part, is strictly prohibited
 * without prior written permission from DIODAC ELECTRONICS.
 * ═════════════════════════════════════════════════════════════════════════════
 * harp.h — v6.1.01  LASER-HARP INSTRUMENT — PUBLIC INTERFACE
 *
 * SOUND FROZEN — docs/dsp_sound_frozen.md (harp.cpp implementation locked).
 *
 * Self-contained laser-harp engine (harp.cpp, private anonymous namespace).
 * Seq/drum use shared register helpers from globals.h; harp does not share
 * synthesis code with groovebox.h.
 *
 * Play modes: POLY8 (8-voice MIDI), STRINGS (1:1 pluck + string vibrato),
 * SOLO (monophonic last-note priority with held-beam stack).
 *
 * D-BEAM: consumes dbeam.h expression only (harpPumpExpression reads addends);
 * does not reimplement the ADC follower.
 *
 * Boundary: harp = synthesis + expression control; laser.h = galvo/PWM render.
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
