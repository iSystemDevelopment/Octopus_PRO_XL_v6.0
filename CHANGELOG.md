# Changelog

All notable changes to Octopus PRO XL firmware and OctopusApp are documented here.

Format based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).
Versioning aligns with firmware `SYSTEM_FW_VERSION` in `code_info.h`.

> **Current version: [6.1.00](#610--2026-06-23)** — firmware field baseline.  
> **OctopusApp: [6.2.00](#620--2026-06-23)** — universal MIDI Controller mode (browser-only).  
> **Previous release: [6.0.01](#601--2026-06-22).**

## [6.2.00] — 2026-06-23 (OctopusApp)

Browser-only release. **No firmware changes.** Authoritative build order: [docs/midi_controller_mode.md](./docs/midi_controller_mode.md).

### Added

- **Universal MIDI Controller mode** — when no Octopus hardware is linked, select any Web MIDI output port; App sends standard note/CC/PC instead of Octopus SysEx.
- **Trimmed 2-tab layout** — SEQUENCER (shared grid + local playhead) + INSTRUMENTS (seq synth MIDI panel + drum machine scope); MIXER / laser / D-BEAM / NVS tools unavailable.
- **App-owned transport** — play, stop, editable BPM, local 16th-note step clock; MIDI Start/Stop (`0xFA`/`0xFC`); optional **MIDI clock out** (`0xF8`, 24 PPQN).
- **Grid → MIDI notes** — melody rows use firmware scale map + transpose/octave; drum rows use GM notes (editable per row).
- **INSTRUMENTS panels** — 8 CC knobs, Program Change, per-row drum notes, beat-reactive scope canvases.
- **Dual Help** — separate in-app Help tab for MIDI Controller mode.
- **localStorage** — `octopusapp_midi_session_v1` stores all 4 banks, routing, CC/PC map (separate from Octopus NVS / slot cache).
- **EXP / IMP** — export/import MIDI session as JSON from the routing bar.

### Changed (docs)

- **User manual §9.4** — shipped MIDI Controller beginner guides (macOS / Windows + Chrome, DAW routing).
- **Product site** — MIDI Mode section updated for v6.2.00; in-app Help MIDI CONTROLLER tab.
- **DEPLOYMENT.md** — production upload + smoke-test checklist for App v6.2.00.

### Unchanged

- **Octopus linked mode** — identical to v6.1.00 when ★ Octopus port + SysEx echo is detected.
- **Firmware** — remains **6.1.00**.

## [6.1.00] — 2026-06-23

### Fixed

- **AUX reverb SIZE — ~5 % drift + top-of-range dead zone** — the App (and the firmware's own echo) encode reverb size as `(size/0.95)·16383`, but both decode paths (`applyAuxParam` and `applyMasterParam` in `patches.h`) stored `min(0.95, n)` instead of `n·0.95` — so the bus reverb ran ~5 % larger than the knob showed and the top ~5 % of the knob all collapsed to 0.95. Decoded with `·0.95` now (same fix already applied to AUX delay-feedback). Delay-time, delay-FB and reverb-damp round-trips verified stable.
- **D-BEAM VOLUME pedal — cumulative volume drift** — the inverted volume pedal adopted the *live* bus level as its baseline every rest tick, but the follower's release is gradual so the bus is still slightly dipped when it crosses the rest threshold. Each hand gesture therefore shrank the harp/seq baseline (~1 %+, more on a fast lift), so the level crept down over a set. Also left the bus **stuck low** if D-BEAM was disabled or re-routed mid-gesture. Reworked `routeDbeamExpression()` (`dbeam.cpp`) to capture the baseline once on the **press edge** and fully restore the bus on the **lift edge** — no drift, no stuck-low bus.
- **App "RND" (randomize) did not restart playback** — OctopusApp sent `CMD_SEQ_RESTART` (193) after RND-H / RND-D so a freshly randomised pattern plays from beat 1, but the firmware had **no handler** for it, so the sequencer kept running from its current step. Added `seq_restart_rt()` (`groovebox.cpp`) + dispatch case (`midi.cpp`): the step counter is zeroed without stopping playback (no-op when stopped).
- **CPU-load telemetry mismatch (dead App warnings)** — OctopusApp's `CMD_CPU_LOAD` handler decoded an out-ring drop count (bits 7–13) and a P-lock "steal" flag from **bit 14**, but the firmware sent only the raw load %, and bit 14 is unaddressable in a 14-bit SysEx value, so both warnings could never fire. Firmware now packs **load % (bits 0–6) · drop count (bits 7–12, saturating 0–63) · lanes-full flag (bit 13)**; the App decode and the (previously incorrect) "oldest lane replaced" message were corrected — the firmware *drops* new automation when all 4 P-lock lanes are full, it never steals.
- **FULL / Banks+Pats reset false FAIL + wedged UI** — runtime heavy NVS writes while audio/tasks were active could time out, show FAIL, and skip reboot even when flash was wiped later. Replaced with deferred boot reset (`pend_rst` + instant reboot; wipe before `loadSettings()`).
- **Reset Settings (and all scoped SAVE/LOAD/RESET from App)** — App encodes persist scope as `v14 = ResetScope + 1` (1–4) but firmware decoded `v14 & 3`, so SETTINGS (4) became FULL (0) and FULL (1) became BANKS_PATTERNS. Fixed `decodePersistScopeV14()` in `midi.cpp`.
- **Reset/Save always FAILED in App** — scoped RESET never echoed `CMD_SCOPED_RESET` ACK before reboot (only `CMD_SESSION_SAVE` ACK was sent). `g_persistAckCmd` routes the correct ACK command.
- **Hardware LOAD while App connected** — `handleScopedLoad()` now calls `sendFullStateSync()` + `CMD_SESSION_LOAD` ACK so the App reloads and mirrors NVS state.
- **Wedged persist flags** — `recoverWedgedPersistFlags()` clears stale `g_saveRequest` before new SAVE/RESET attempts.

### Changed

- **Insert FX-A names unified to the cosmic set** — the slot-A preset labels drifted: `effect.cpp INSERT_FX_PRESETS[]` carried the authored cosmic names ("Nebula Taps", "Snova Chorus"…) while the OLED (`kInsertFxNames`) and App (`INSERT_FX_NAMES`) still showed the legacy set ("AcidRoom", "HiChorus"…). The index→DSP mapping was always identical, so this is labels only — now aligned across `display.h`, OctopusApp, and `user_manual.md` to match `effect.cpp` (consistent with the master-FX/wave cosmic branding).
- **Drum scope is now beat-reactive** — the MIXER-tab drum oscilloscope previously free-ran on random noise gated by play state. It now fires a transient on the **actual drum hits** at the live playhead step (drum grid rows 8–15 of the playing bank, kick/snare weighted heavier than hats/perc, downbeat accent, scaled by D.VOL), decaying on empty steps. The canvas was moved to a **definitive, isolated GPU compositor layer** (`will-change`/`contain`/`isolation`) so its per-frame repaints never reflow the mixer UI, and it ticks only while actually on-screen.
- **Legacy grid-row SysEx retired** — the numeric `CMD_GRID_ROW_LO/HI` (162/163) handler is removed (firmware) and no longer sent (App). Its old v14 packing overlapped the 8 step bits with the page field, corrupting steps 4–5 on pages 1–3. Bulk grid sync runs **both directions** through the lossless `SX_SUB_GRID_ROW` (sub 0x05) blob. The wire IDs stay **reserved** (never renumbered); docs in `sysex.h` / `code_info.h` / OctopusApp updated.
- **Soft Reset removed** — `CMD_SOFT_RESET` (171) ignored on RX; SEQ SETUP menu entry removed; `seqSoftResetWorkingImage()` deleted. Use **RESET → Settings** for factory knob/mixer defaults (persisted + reboot).
- **Deferred boot reset (FULL / Banks+Pats)** — runtime reset no longer writes large NVS blobs while audio/tasks run. `settings_arm_pending_reset()` stores `pend_rst` in NVS, ACKs the App, and `esp_restart()` in ~150 ms. `settings_execute_pending_reset_at_boot()` (before `loadSettings()`) applies the wipe in the same safe window as the OC+SCALE combo. SETTINGS/MOTION reset still use NvsWorker + reboot.
- **OctopusApp v6.1.00** — auto-connect on USB; Connect/Disconnect buttons removed; badge **Octopus ON / Octopus Off**; full browser reload after SAVE/LOAD/RESET (and hardware-initiated persist) then re-import via `APP_SYNC_REQ`; reset NACK waits for USB reconnect instead of instant FAIL.
- **Playhead compositor layer** — `#seq-playhead` uses dedicated GPU layer (`will-change`, `contain`) separated from grid cell repaints.
- **NVS namespace** — flash partition namespace renamed to `octopus` (auto-migrates legacy `octopus_v5` on first boot). `SETTINGS_VERSION 0x0615` is the struct wire-layout ID, not the firmware semver.
- **Documentation pass** — all `.cpp` module headers, `code_info.h`, README, `user_manual.md`, and `octopus_web.html` aligned to v6.1.00 as the maintenance baseline.

## [6.0.01] — 2026-06-22

### Fixed (field-test pass)

- **Save / Reset "always FAILED"** — wedge-recovery for stuck persist flags; improved FAILED toast diagnostics.
- **App playhead glitch / mouse-movement glitch** — discrete STEP_SYNC playhead; transport-edge cache invalidation.
- **4 beat-LEDs froze in SONG editor view** — LEDs driven from STEP_SYNC handler.
- **`octopus_web.html`** — sample rate, USB-only, SysEx count, version strings corrected.

### Changed

- **OctopusApp v6.0.01** — PLL playhead retired in favour of discrete hardware-step mirror; task priorities centralized in `globals.h`.

## [6.0.00] — 2026-06-20

### Added

- **v6.0 platform:** ESP32-S3 dual-core architecture; USB-only MIDI (DIN removed).
- **64-step sequencer** with App pages P1–P4; song mode (16 slots × 16 chain steps).
- **Seq + harp arpeggiators**, OrgDrive FX, drum global pitch, fog-reject, D-BEAM local DSP.
- **OctopusApp v6.0** — STEP_SYNC playhead, CPU telemetry, harp/seq ARP UI.
- **Documentation** — README, user manual, product site.

### Changed

- Transport ownership: hardware always owns play/stop/record/BPM; App is read-only reflector.

---

| Resource | URL |
|----------|-----|
| OctopusApp | [https://octopus.isystem.app](https://octopus.isystem.app) |
| Product site | [https://octopus-info.isystem.app](https://octopus-info.isystem.app) |
| GitHub | [github.com/iSystemDevelopment/Octopus_PRO_XL_v6.0](https://github.com/iSystemDevelopment/Octopus_PRO_XL_v6.0) |
