# Octopus PRO XL v6.1

**Laser Harp Groovebox — Firmware & Companion Application**

![Octopus PRO XL — OctopusApp web console](https://octopus-info.isystem.app/octopus-app-hero.jpg)

| | |
|---|---|
| **Firmware** | **6.1.01** (SETTINGS/MOTION reset without reboot — flash via USB) |
| **OctopusApp** | **6.2.07** (Web MIDI + MIDI Controller mode — deploy `OctopusApp.html`) |
| **Platform** | ESP32-S3 (dual-core, FreeRTOS, ESP-IDF 5) |
| **Author** | DIODAC ELECTRONICS / [iSystem](https://isystem.app) |
| **Launch App** | **[octopus.isystem.app](https://octopus.isystem.app)** |
| **Product site** | **[octopus-info.isystem.app](https://octopus-info.isystem.app)** |
| **Source code** | **[GitHub](https://github.com/iSystemDevelopment/Octopus_PRO_XL_v6.0)** |
| **Documentation** | [User Manual](./user_manual.md) · [Changelog](./CHANGELOG.md) · [Architecture](./code_info.h) · [MIDI Controller Mode](./docs/midi_controller_mode.md) |

---

## Live links

| Resource | URL |
|----------|-----|
| **OctopusApp** (Web MIDI console) | [https://octopus.isystem.app](https://octopus.isystem.app) |
| **Product / info site** | [https://octopus-info.isystem.app](https://octopus-info.isystem.app) |
| **GitHub repository** | [github.com/iSystemDevelopment/Octopus_PRO_XL_v6.0](https://github.com/iSystemDevelopment/Octopus_PRO_XL_v6.0) |
| **Facebook** | [facebook.com/diodac.co.uk](https://www.facebook.com/diodac.co.uk/) |
| **Support (optional)** | [PayPal donate](https://www.paypal.com/donate?hosted_button_id=KX7B76V37PED8) → DIODAC ELECTRONICS |

Production OctopusApp and product site are hosted on VPS subdomains (`octopus.isystem.app` and `octopus-info.isystem.app`).

---

## Support the project

Octopus PRO XL firmware and **OctopusApp** are free to use. If they help your workflow, you can leave an optional tip via **[PayPal](https://www.paypal.com/donate?hosted_button_id=KX7B76V37PED8)** (`diodac.electronics@gmail.com`). Links also appear in OctopusApp (**TIP** chip, footer) and on [octopus-info.isystem.app](https://octopus-info.isystem.app#contact).

---

## Overview

Octopus PRO XL is a performance-oriented **laser harp** and **groovebox** with real-time laser projection, polyphonic synthesis, a 64-step sequencer, TR-style drums, D-BEAM expression, and a shared effects bus — operable from the hardware surface or **OctopusApp** over USB MIDI SysEx.

When USB is connected, OctopusApp **auto-links** to the device, imports the full hardware state (knobs, grid, transport), and reloads after SAVE / LOAD / RESET so the UI always mirrors the device. **Full** and **Banks+Pats** reset arm an NVS flag and reboot immediately; the wipe runs on the next boot before audio starts. **Settings** and **Motion** reset apply live with **no reboot** (firmware ≥ 6.1.01) — the App just reloads and re-pulls the fresh image. A connected ★ Octopus has hard priority: the App locks to Octopus mode (MIDI Controller mode is available only with no Octopus plugged in). Transport (play, stop, record arm, tempo) remains **hardware-owned** (SCALE / OC short / encoder).

NVS flash uses namespace **`octopus`** (`SETTINGS_VERSION 0x0616` for struct layout). Upgrading from earlier builds migrates saved data automatically from the legacy `octopus_v5` namespace on first boot.

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

### Universal MIDI Controller mode *(OctopusApp v6.2.07 — shipped)*

With **no Octopus hardware** connected, OctopusApp runs as a **standalone MIDI controller**: pick any USB MIDI interface from the port dropdown, compose on the 64-step grid, and send **standard MIDI** (notes, Control Change, Program Change) to external synths, drum machines, or a DAW. The App owns transport and tempo in this mode; laser, D-BEAM, NVS SAVE/LOAD, and SysEx sync stay **Octopus-only**.

| Mode | Badge | Tabs | Transport |
|------|-------|------|-----------|
| **Octopus linked** | Octopus ON | INSTRUMENTS · MIXER · SEQUENCER | Hardware-owned |
| **MIDI controller** | MIDI OUT | INSTRUMENTS (seq + drums) · SEQUENCER | App-owned |

**Beginner setup:** [User Manual §9.4](./user_manual.md#94-universal-midi-controller-mode-octopusapp-v6200--shipped) (macOS / Windows / DAW). Build reference: **[docs/midi_controller_mode.md](./docs/midi_controller_mode.md)**.

---

## Repository layout

| Path | Description |
|------|-------------|
| `Octopus_PRO_XL_v6.0.ino` | Boot kernel, task scheduling |
| `harp.cpp` / `groovebox.cpp` / `effect.cpp` | Instrument engines |
| `OctopusApp.html` | Web MIDI console → deploy to **octopus.isystem.app** |
| `octopus_web.html` | Product info page → deploy to **octopus-info.isystem.app** |
| `user_manual.md` | End-user documentation |
| `docs/midi_controller_mode.md` | OctopusApp MIDI Controller mode (shipped v6.2) |
| `DEPLOYMENT.md` | Production deploy + smoke tests (App v6.2, product site) |
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

See **[DEPLOYMENT.md](./DEPLOYMENT.md)** for production checklists and cache headers.

### OctopusApp (`octopus.isystem.app`) — **v6.2.07**

1. Upload `OctopusApp.html` to the VPS web root for the `octopus` subdomain.
2. Serve over **HTTPS** (Web MIDI requires a secure context).
3. Set `Cache-Control: no-cache` on the HTML entry (or users hard-refresh after updates).
4. Run the smoke tests in DEPLOYMENT.md §1 (Octopus ON + MIDI OUT + regression).

No server-side MIDI — all USB MIDI runs in the browser.

### Product site (`octopus-info.isystem.app`)

1. Upload `octopus_web.html` (as `index.html`), `logo.jpg`, `octopus-app-hero.jpg`, and related assets.
2. Serve over HTTPS.

### Firmware

Flash via USB after field-testing **6.1.01** on hardware. Include `partitions.csv` for the 256 KB NVS layout.

---

## Contributing & support

- [CONTRIBUTING.md](./CONTRIBUTING.md) · [SECURITY.md](./SECURITY.md) · [CHANGELOG.md](./CHANGELOG.md)
- GitHub Issues — firmware **`6.1.01`**, OctopusApp **`6.2.07`**, App connected Y/N, repro steps

---

## License

© 2026 **DIODAC ELECTRONICS / iSystem**. Proprietary — see [LICENSE](./LICENSE).
