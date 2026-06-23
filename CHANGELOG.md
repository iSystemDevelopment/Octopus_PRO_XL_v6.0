# Changelog

All notable changes to Octopus PRO XL firmware and OctopusApp are documented here.

Format based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).
Versioning aligns with firmware `SYSTEM_FW_VERSION` in `code_info.h`.

> **Current version: [6.0.01](#601--2026-06-22)** (`SYSTEM_FW_VERSION` bumped) — build + field-test before flashing to production.  
> **Previous field-tested rollback: [6.0.00](#600--2026-06-20).**

## [6.0.01] — 2026-06-22

### Fixed (field-test pass)

- **Save / Reset "always FAILED"** — the App SAVE handler and `handleScopedReset()` NACK'd on an early `saveInProgress()` / `g_resetInProgress` guard *before* the wedge-recovery in `requestScopedSave()` could run, so a stuck `g_saveArmed`/`g_resetInProgress` (crash/abort mid-write) made every save **and** reset NACK forever. Recovery is now a shared `recoverWedgedPersistFlags()` (`globals.h`) called FIRST at every persist entry point. The App's FAILED toast now also logs the real NACK causes (re-flash with partition table + NVS erase, low heap, wrong partition) and points at the device UART0 `[NVS] … FAILED err=…` line. *Note: a genuine NVS write error is environmental — confirm via that serial line.*
- **App playhead glitch / mouse-movement glitch / not 1:1 with hardware** — three fixes: (1) on a transport edge the cell-cache counters (`_playLast`/`_playPage`/`_playCol`/`_playAbsStep`/`_phRect`) were left on the last step, so a same-step restart hit the cached fast-path with a **stale rect** and painted a cell off from the hardware step — they now reset to `-1` on every transport edge (a fresh measure → exact hardware cell); (2) the `CMD_STEP_PHASE` sub-step nudge was **reverted** (it snapped the PLL anchor on late STEP_SYNC → per-step forward jiggle); (3) the per-frame rAF sub-cell **glide was removed** — driving the bar every animation frame made it hostage to main-thread jank, so moving the mouse (browser hit-test + `:hover` recalc + grid repaints between frames) delayed the rAF callback and stuttered the bar. The playhead is now **DISCRETE**: its cell is set only on a `CMD_STEP_SYNC` event (prompt + infrequent), making it immune to mouse/rAF jank and exactly **1:1 with the device's own discrete 16-box bar**.
- **4 beat-LEDs froze in SONG editor view** — `_updateBeatLeds()` only ran inside `_paintPlayhead()`, which bails early when the grid is hidden (chain editor). The LEDs (step-only, no grid geometry) are now driven straight from the `STEP_SYNC` handler, so they keep sequencing in the SONG view.
- **`octopus_web.html`** — sample rate shown as **44.1 kHz (44,100 Hz)**, P4 row notes 44.1 kHz · 24-bit, S3 = 16-bit; corrected stale "USB-C **+ DIN**" (DIN removed in v6.0 → USB-only), "190 SysEx" → 195, OctopusApp v6.0.00 → v6.0.01, build date.

### Fixed

- **FX engine** — Master EQ shelf/peaking dB→linear (`10^(dB/40)`); shared aux tail drains after sends go to zero; safe output when FX chain uninitialized; compressor GR clamp + denormal flush; CPU tail-only path when engines idle.
- **Click / crackle** — Harp/seq/drum mute uses envelope release instead of hard voice kill; click-free seq voice retrigger; harp SOLO legato (phase reset on pitch change); poly headroom before soft-clip (√N scaling).
- **Stuck notes** — Cross-core `stringActiveMask`; per-buffer `harpReconcileStuckNotesLocked()` for SOLO/POLY8; beam release hysteresis; panic + MIDI CC120/123 full harp/laser state reset; held-beam silent recovery.
- **Arpeggiator (`arp.h`)** — `pitch_rows[]` kept in sync with sorted notes (correct laser row for Up/Down patterns); **DnUp** ping-pong sequence corrected; **AsIs** uses latch-order rows only.
- **Harp ARP ↔ App sync** — entering **STRINGS** (hardware OC-cycle or App `CMD_PLAY_MODE`) force-disables the harp arpeggiator AND now echoes `CMD_HARP_ARP_EN=0`, so the App's HARP ARP toggle no longer lingers **ON** after a device-side play-mode change (`harp.cpp harpSetPlayMode` `[ARP-ECHO-FIX]`).

### Changed

- **OctopusApp v6.0.01** — help/changelog aligned with firmware; playhead wrap fix for 1-step patterns; LEN change re-syncs playhead while playing.
- **OctopusApp v6.0.01 — PLL playhead (shipped)** — cyan grid playhead glides at 60 fps via a phase-locked tick clock slaved to `CMD_STEP_SYNC` (hardware anchors each step, App interpolates sub-step position via `_pllPositionTick`); hard-snaps only on page change / pattern wrap; CSS transform transition retired. Closes the v6.0.01 playhead roadmap item.
- **Task priorities centralized** — the raised RTOS priorities now live in one place (`globals.h` `TASK_PRIO_*` constants, `[TASK-PRIO-SSOT]`) and are referenced at every `xTaskCreatePinnedToCore` site (`audio.cpp`, `midi.cpp`), so the values can never drift from the docs again (the priority raise itself preserved relative ordering — realtime tasks still dominate each core, so no behavioral change). Stale task-handle/core comments in `globals.h` corrected.
- **Docs / comment accuracy** — `audio.h` + `audio.cpp` + `code_info.h` task maps corrected to the live RTOS priorities (OledRender 18, ControlPoll 16, SeqSysexOut 14, MidiUsbRx 12, NvsWorker 9; NvsWorker stack 16 KB); 44.1 kHz buffer-period comments fixed (`audio.cpp`, `effect.h`, `harp.cpp` — were 48 kHz); SysEx command table documented through CMD 0–194 (CMD_COUNT 195) incl. PLAY_MODE/H_PITCH/drum-insert FX/SEQ_RESTART/STEP_PHASE and the sub-0x02..0x05 variable-length frames; retired `CMD_GRID_ROW_LO/HI` decoders removed from the App (grid sync uses lossless sub-0x05 frames); `CMD_S_SCALE` marked reserved (scale is global via `CMD_H_SCALE`).

## [6.0.00] — 2026-06-20

### Added

- **v6.0 platform:** ESP32-S3 dual-core architecture; USB-only MIDI (DIN removed).
- **64-step sequencer** with App pages P1–P4; song mode (16 slots × 16 chain steps).
- **Seq arpeggiator** — 8 patterns, 8 rates, gate; motif expansion for single-note steps.
- **Harp arpeggiator** — 4 patterns, 4 rates; POLY8/SOLO; scale motif expansion.
- **OrgDrive** insert FX preset (organic drum/melody distortion, FX-A index 12).
- **Drum global pitch** (D.PITCH), fog-reject gate, D-BEAM local DSP routing.
- **OctopusApp v6.0** — STEP_SYNC playhead, CPU telemetry, harp/seq ARP UI, drum scope.
- **Documentation** — `README.md`, `user_manual.md`, [octopus-info.isystem.app](https://octopus-info.isystem.app) product site.

### Changed

- Transport ownership: hardware always owns play/stop/record/BPM; App is read-only reflector.
- Factory sound bank repacked (128 named presets); `SETTINGS_VERSION 0x0615`.
- Harp engine split to `harp.cpp`; parameter SSOT consolidated in `patches.h`.

### Fixed

- Seq/harp transpose and octave cross-talk from SysEx and P-lock motion playback.
- Harp ARP latch for POLY8/SOLO; pattern/rate/gate discrete encoding.
- Seq ARP pattern/rate/gate clamping (`clampDiscreteUi`); single-note motif expansion.
- OctopusApp playhead detachment from grid DOM; drum scope smooth rendering.

---

## Release links

| Asset | Location |
|-------|----------|
| Product site | [https://octopus-info.isystem.app](https://octopus-info.isystem.app) |
| OctopusApp | [https://octopus.isystem.app](https://octopus.isystem.app) |
| Source | [GitHub](https://github.com/iSystemDevelopment/Octopus_PRO_XL_v6.0) |
| Facebook | [facebook.com/diodac.co.uk](https://www.facebook.com/diodac.co.uk/) |
| User manual | [user_manual.md](./user_manual.md) |
