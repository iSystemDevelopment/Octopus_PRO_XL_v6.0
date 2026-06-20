# Octopus PRO XL — Deployment

**Current stable firmware / App label:** `6.0.00` (`SYSTEM_FW_VERSION` in `code_info.h`).

Web assets (`octopus_web.html`, images) may live on the VPS separately from this firmware repository. Upload the product site and App HTML to match the version you have flashed on hardware.

---

## 1. GitHub repository

Public source: [github.com/iSystemDevelopment/Octopus_PRO_XL_v6.0](https://github.com/iSystemDevelopment/Octopus_PRO_XL_v6.0)

Docs rendered on GitHub: `README.md`, `user_manual.md`, `CHANGELOG.md`, `Upgrade.md`.

---

## 2. OctopusApp (`octopus.isystem.app`)

1. Build or copy `OctopusApp.html` from this repo (version string should match firmware `6.0.00`).
2. Upload to the VPS web root for **octopus.isystem.app** (HTTPS required for Web MIDI).
3. Serve as `index.html` or configure nginx `index OctopusApp.html;`.
4. Hard-refresh browsers after deploy (Ctrl+Shift+R).

---

## 3. Product site (`octopus-info.isystem.app`)

1. Deploy `octopus_web.html` as `index.html` (or default document).
2. Upload static assets in the **same directory** (case-sensitive on Linux):

   | File | Notes |
   |------|--------|
   | `logo.jpg` | Nav / hero |
   | `laser.jpg` | Harp section |
   | `lharps.jpg` | Contact section (lowercase) |
   | `octopus-app-hero.jpg` | Hero screenshot |

3. Confirm images return `Content-Type: image/jpeg` — not HTML (broken filename falls through to `index.html` on some nginx configs).

---

## 4. VPS checklist

- TLS certificate valid (Let's Encrypt or Cloudflare).
- Both subdomains point to the correct document roots.
- After upload: test `https://octopus.isystem.app` and `https://octopus-info.isystem.app`.
- Firmware USB MIDI is local to the browser; no server-side MIDI proxy needed.

---

## 5. Firmware

Flash `Octopus_PRO_XL_v6.0.ino` via Arduino IDE / arduino-cli (ESP32-S3, `partitions.csv`, `sdkconfig.defaults`). Serial boot banner should report **`OCTOPUS PRO XL v6.0.00`**.
