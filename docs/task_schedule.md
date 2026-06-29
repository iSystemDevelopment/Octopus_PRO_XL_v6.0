# FreeRTOS task schedule (FROZEN)

**Status:** Normative — validated on hardware (heavy patches, full matrix, App preset/knob streaming).  
**Authority:** `init_audio_system()` in `audio.cpp` only.  
**Policy:** **Do not change cores, priorities, or stack sizes** without re-running the full validation matrix below. Timing tweaks inside task bodies are OK; **schedule refactoring is forbidden**.

---

## 1. Why this schedule exists

Earlier builds placed **NvsWorker** on Core 1 @ prio 4 and **MidiUsbRx** @ prio 6. Under load:

- USB SysEx (App knobs, preset dropdowns, grid rows) queued behind laser + low-prio MIDI
- Audible **zipper noise** and multi-second UI lag when selecting synth presets
- **Note clicks**, shallow sound, and higher reported CPU — param/motion echoes arrived too late
- Saves on Core 1 competed with MIDI drain during laser breathe windows

The current schedule fixes this by:

1. **Raising Core 1 I/O** — `SeqSysexOut` @ 23, `MidiUsbRx` @ 22 (just under `LaserSweep` @ 24)
2. **Moving NvsWorker to Core 0 @ 16** — flash writes no longer starve USB parsing on Core 1; still **below** `AudioSynth` @ 24 (audio never preempted)

Field result (user validation): ~**10% lower CPU** on heavy patches, **smoother knobs/dropdowns**, **deeper/cleaner sound**, **no note clicks** on stress patch (100% matrix, BPM 80, LFO-pitch 8 Hz, glass pad + accordion riff). **DSP engines were not changed** — see `docs/dsp_sound_frozen.md`.

---

## 2. Authoritative table

| Core | Prio | Task | Stack | Role |
|------|------|------|-------|------|
| **0** | 24 | AudioSynth | 16384 | I2S + sequencer + synth + FX |
| **0** | 19 | dbeam_adc | 6144 | ADC DMA + Kalman + expression |
| **0** | 18 | OledRender | 16384 | OLED ~30 Hz |
| **0** | 17 | ControlPoll | 8192 | Encoder/buttons 200 Hz |
| **0** | 16 | **NvsWorker** | 16384 | NVS save handshake (muted audio window) |
| **1** | 24 | LaserSweep | 8192 | Galvo sweep + triggers |
| **1** | **23** | **SeqSysexOut** | 4096 | STEP_SYNC drain + sync supervisor + 33 ms beacon |
| **1** | **22** | **MidiUsbRx** | 8192 | USB MIDI + App SysEx RX |
| **1** | 1 | loop() | — | Safety fallback only |

Priorities are **per-core** (AudioSynth 24 on Core 0 does not preempt LaserSweep 24 on Core 1).

Encoder PCNT ISR: Core 1 (`interface.cpp`) — keeps IRQ traffic off the audio island.

---

## 3. Hard rules

| Rule | Reason |
|------|--------|
| **Never** move NvsWorker back to Core 1 prio 4 | Saves + MIDI starvation; session shift risk |
| **Never** drop MidiUsbRx below ~20 on Core 1 | App knob/preset zipers return |
| **Never** drop SeqSysexOut below ~20 on Core 1 | STEP_SYNC / mirror lane stalls |
| **Never** raise any Core 0 task above AudioSynth 24 | I2S dropouts / clicks |
| **Never** duplicate task spawn tables | Single source: `audio.cpp` |

`globals.h` mirrors handles only — comments must match this doc.

During save: `g_saveArmed` still freezes sequencer UI paths; laser yields dark phases on Core 1 (feeds **MidiUsbRx / SeqSysexOut**, not NvsWorker — NvsWorker runs on Core 0).

---

## 4. Rejected schedules

| Schedule | Why rejected |
|----------|----------------|
| NvsWorker Core 1 prio 4 | MIDI/SysEx starvation; save vs laser contention |
| MidiUsbRx prio 6 | Huge App param delay; zipper noise |
| SeqSysexOut prio 12 | Mirror/playhead/backlog under load |
| NvsWorker Core 0 prio ≥ 20 | Could preempt ControlPoll/OLED during long NVS writes |

---

## 5. Validation checklist (re-run after any schedule change)

- [ ] 100% matrix, BPM 80, LFO-pitch 8 Hz — no clicks, stable accordion/poly riff
- [ ] Heavy FX patch — CPU box ~10% lower vs old schedule (same patch)
- [ ] App: synth preset dropdown — positions update smoothly, no multi-second lag
- [ ] App: knob drag on INSTRUMENTS — smooth, no zipper stepping
- [ ] SAVE session — no grid/playhead shift; beam recovers after commit
- [ ] 240 BPM + full grid 5+ min — mirror lane still OK (`mirror_architecture.md`)
- [ ] Stack high-water > 512 B free on all tasks (Serial / STACK_STATS telemetry)

---

*Frozen 2026-06-25 — Octopus PRO XL v6.3 task schedule.*
