# Changelog

All notable changes to Octopus PRO XL firmware and OctopusApp are documented here.

Format based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).
Versioning aligns with firmware `SYSTEM_FW_VERSION` in `code_info.h`.

> **Current version: [6.1.00](#610--2026-06-23)** — build + field-test before flashing to production.  
> **Previous release: [6.0.01](#601--2026-06-22).**

## [6.1.00] — 2026-06-23

### Fixed

- **Reset Settings (and all scoped SAVE/LOAD/RESET from App)** — App encodes persist scope as `v14 = ResetScope + 1` (1–4) but firmware decoded `v14 & 3`, so SETTINGS (4) became FULL (0) and FULL (1) became BANKS_PATTERNS. Fixed `decodePersistScopeV14()` in `midi.cpp`.
- **Reset/Save always FAILED in App** — scoped RESET never echoed `CMD_SCOPED_RESET` ACK before reboot (only `CMD_SESSION_SAVE` ACK was sent). `g_persistAckCmd` routes the correct ACK command.
- **Hardware LOAD while App connected** — `handleScopedLoad()` now calls `sendFullStateSync()` + `CMD_SESSION_LOAD` ACK so the App reloads and mirrors NVS state.
- **Wedged persist flags** — `recoverWedgedPersistFlags()` clears stale `g_saveRequest` before new SAVE/RESET attempts.

### Changed

- **Soft Reset removed** — `CMD_SOFT_RESET` (171) ignored on RX; SEQ SETUP menu entry removed; `seqSoftResetWorkingImage()` deleted. Use **RESET → Settings** for factory knob/mixer defaults (persisted + reboot).
- **OctopusApp v6.1.00** — auto-connect on USB; Connect/Disconnect buttons removed; badge **Octopus ON / Octopus Off**; full browser reload after SAVE/LOAD/RESET (and hardware-initiated persist) then re-import via `APP_SYNC_REQ`.
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
