# Changelog

All notable changes to Octopus PRO XL firmware and OctopusApp are documented here.

Format based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).
Versioning aligns with firmware `SYSTEM_FW_VERSION` in `code_info.h`.

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
| Source | [GitHub](https://github.com/iSystemApp/Octopus-PRO-XL-v6.0) |
| User manual | [user_manual.md](./user_manual.md) |
