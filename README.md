# Octopus PRO XL v6.1

**Laser Harp Groovebox — Firmware & Companion Application**

![Octopus PRO XL — OctopusApp web console](https://octopus-info.isystem.app/octopus-app-hero.jpg)

| | |
|---|---|
| **Version** | **6.1.00** (current — compile + field-test before flashing to production) |
| **Platform** | ESP32-S3 (dual-core, FreeRTOS, ESP-IDF 5) |
| **Author** | DIODAC ELECTRONICS / [iSystem](https://isystem.app) |
| **Launch App** | **[octopus.isystem.app](https://octopus.isystem.app)** |
| **Product site** | **[octopus-info.isystem.app](https://octopus-info.isystem.app)** |
| **Source code** | **[GitHub](https://github.com/iSystemDevelopment/Octopus_PRO_XL_v6.0)** |
| **Documentation** | [User Manual](./user_manual.md) · [Changelog](./CHANGELOG.md) · [Architecture](./code_info.h) |

---

## Live links

| Resource | URL |
|----------|-----|
| **OctopusApp** (Web MIDI console) | [https://octopus.isystem.app](https://octopus.isystem.app) |
| **Product / info site** | [https://octopus-info.isystem.app](https://octopus-info.isystem.app) |
| **GitHub repository** | [github.com/iSystemDevelopment/Octopus_PRO_XL_v6.0](https://github.com/iSystemDevelopment/Octopus_PRO_XL_v6.0) |
| **Facebook** | [facebook.com/diodac.co.uk](https://www.facebook.com/diodac.co.uk/) |

Production OctopusApp and product site are hosted on VPS subdomains (`octopus.isystem.app` and `octopus-info.isystem.app`).

---

## Overview

Octopus PRO XL is a performance-oriented **laser harp** and **groovebox** with real-time laser projection, polyphonic synthesis, a 64-step sequencer, TR-style drums, D-BEAM expression, and a shared effects bus — operable from the hardware surface or **OctopusApp** over USB MIDI SysEx.

When USB is connected, OctopusApp **auto-links** to the device, imports the full hardware state (knobs, grid, transport), and reloads after SAVE / LOAD / RESET so the UI always mirrors the device. Transport (play, stop, record arm, tempo) remains **hardware-owned** (SCALE / OC short / encoder).

NVS flash uses namespace **`octopus`** (`SETTINGS_VERSION 0x0615` for struct layout). Upgrading from earlier builds migrates saved data automatically from the legacy `octopus_v5` namespace on first boot.

---

## Quick start

### Hardware-only

1. Power via USB.
2. **SCALE** (long) — switch HARP / SEQUENCER dashboard.
3. HARP: **SCALE** = scale; encoder = presets; **OC** (long) = open laser gate.
4. SEQUENCER: encoder = BPM; **SCALE** = play/stop; **OC short** = record arm when App connected.
5. Long encoder press → **SAVE** before power-off.

### With OctopusApp

1. Open **[octopus.isystem.app](https://octopus.isystem.app)** in Chrome or Edge (HTTPS required for Web MIDI).
2. Connect USB — the App **auto-connects** (badge shows **Octopus ON**).
3. Edit patches, grid, mixer, and song chains in the App.
4. Use **hardware** for play/stop, record arm, and tempo.

See [User Manual §3.4 — App-connected mode](./user_manual.md#34-app-connected-mode).

---

## Repository layout

| Path | Description |
|------|-------------|
| `Octopus_PRO_XL_v6.0.ino` | Boot kernel, task scheduling |
| `harp.cpp` / `groovebox.cpp` / `effect.cpp` | Instrument engines |
| `OctopusApp.html` | Web MIDI console → deploy to **octopus.isystem.app** |
| `octopus_web.html` | Product info page → deploy to **octopus-info.isystem.app** |
| `user_manual.md` | End-user documentation |
| `code_info.h` | Developer architecture manifest (`SYSTEM_FW_VERSION`) |

---

## Building firmware

**Requirements:** Arduino IDE 2.x or arduino-cli, **ESP32-S3** board support (ESP-IDF 5.x), ESP32Encoder, Adafruit GFX + Adafruit SH110X.

1. Open `Octopus_PRO_XL_v6.0.ino` (all `.cpp` / `.h` in the same folder).
2. Select ESP32-S3 board matching your module (flash / PSRAM).
3. Use `partitions.csv` and `sdkconfig.defaults` per your board package.
4. Compile and upload via USB.

Consult **`code_info.h`** before changing SysEx commands or persistence.

---

## Deployment

### OctopusApp (`octopus.isystem.app`)

1. Upload `OctopusApp.html` to the VPS web root for the `octopus` subdomain.
2. Serve over **HTTPS** (Web MIDI requires a secure context).
3. No server-side MIDI — all USB MIDI runs in the browser.

### Product site (`octopus-info.isystem.app`)

1. Upload `octopus_web.html` (as `index.html`), `logo.jpg`, `octopus-app-hero.jpg`, and related assets.
2. Serve over HTTPS.

### Firmware

Flash via USB after field-testing `6.1.00` on hardware. Include `partitions.csv` for the 256 KB NVS layout.

---

## Contributing & support

- [CONTRIBUTING.md](./CONTRIBUTING.md) · [SECURITY.md](./SECURITY.md) · [CHANGELOG.md](./CHANGELOG.md)
- GitHub Issues — include firmware **`6.1.00`**, App connected Y/N, repro steps

---

## License

© 2026 **DIODAC ELECTRONICS / iSystem**. Proprietary — see [LICENSE](./LICENSE).
