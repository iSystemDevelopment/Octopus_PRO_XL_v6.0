# Deployment — Octopus PRO XL firmware v6.1.01 + OctopusApp v6.2.07

Production hosting for the **browser App** and **product site**. Firmware is flashed via USB (not covered here).

| Asset | URL | Source file |
|-------|-----|-------------|
| **OctopusApp** | [octopus.isystem.app](https://octopus.isystem.app) | `OctopusApp.html` |
| **Product site** | [octopus-info.isystem.app](https://octopus-info.isystem.app) | `octopus_web.html` → `index.html` |

**Versions:** Firmware **6.1.01** · OctopusApp **6.2.07** (Universal MIDI Controller mode, Octopus hard priority, no-reboot Settings/Motion reset).

---

## 0. VPS + Cloudflare (first-time setup)

Web MIDI **requires HTTPS**. Cloudflare is the usual way to terminate TLS in front of a small VPS.

### DNS (Cloudflare dashboard)

| Type | Name | Content | Proxy |
|------|------|---------|-------|
| A | `octopus` | Your VPS public IPv4 | **Proxied** (orange cloud) |
| A | `octopus-info` | Same VPS IP (or separate) | **Proxied** |

Or CNAME to the VPS hostname if your provider gives one.

### SSL/TLS (Cloudflare → SSL/TLS)

| Setting | Value |
|---------|--------|
| **Encryption mode** | **Full (strict)** if the VPS has a valid origin cert (Let's Encrypt), else **Full** |
| **Always Use HTTPS** | On |
| **Minimum TLS Version** | 1.2 |

Web MIDI works with Cloudflare proxy on — no special WebSocket rules needed (the App is static HTML + browser USB MIDI, not server MIDI).

### Origin VPS (nginx example)

```bash
# Ubuntu/Debian — install once
sudo apt update && sudo apt install -y nginx certbot python3-certbot-nginx

# App site
sudo mkdir -p /var/www/octopus.isystem.app
sudo cp OctopusApp.html /var/www/octopus.isystem.app/index.html

# Product site
sudo mkdir -p /var/www/octopus-info.isystem.app
sudo cp octopus_web.html /var/www/octopus-info.isystem.app/index.html
# + copy logo.jpg, octopus-app-hero.jpg, etc.
```

```nginx
# /etc/nginx/sites-available/octopus.isystem.app
server {
    listen 80;
    server_name octopus.isystem.app;
    root /var/www/octopus.isystem.app;
    index index.html;
    location / {
        try_files $uri $uri/ /index.html;
        add_header Cache-Control "no-cache, must-revalidate";
        add_header X-Content-Type-Options "nosniff";
    }
}
```

Enable site + cert (repeat for `octopus-info.isystem.app`):

```bash
sudo ln -s /etc/nginx/sites-available/octopus.isystem.app /etc/nginx/sites-enabled/
sudo certbot --nginx -d octopus.isystem.app
sudo nginx -t && sudo systemctl reload nginx
```

### Cloudflare cache (important for App updates)

**Caching → Configuration → Cache Rules** (or Page Rule legacy):

- URL `octopus.isystem.app/*` → **Bypass cache** (or Cache Level: Bypass)

Static HTML must not sit in Cloudflare edge cache after you upload v6.2, or users keep an old App until cache expires.

### Verify HTTPS before MIDI test

Open `https://octopus.isystem.app` — padlock valid, no mixed-content warnings. Then allow MIDI in Chrome.

---

## App mode switching (Octopus vs MIDI Controller)

How the live App chooses mode (no manual “mode switch” button):

| Situation | Mode | Badge |
|-----------|------|-------|
| ★ **Octopus** port selected / auto-detected | **Octopus linked** | Octopus ON |
| Any **non-★** port selected | **MIDI Controller** | MIDI OUT |
| No port / empty dropdown | **Disconnected** | Octopus Off |

**Auto-connect rules**

1. If a ★ Octopus USB port exists → App **prefers Octopus** and syncs via SysEx (**Octopus hard priority**).
2. If **no** ★ port → App auto-picks the **first available MIDI output** → **MIDI OUT** mode.
3. **Octopus lockout (v6.2.07+):** while any live ★ Octopus port is connected, the header **MIDI** dropdown **cannot** switch to a non-★ port — the picker reverts to Octopus with a log message. **Unplug Octopus** (or wait until ★ disappears from the list) to use MIDI Controller mode. If both ★ Octopus and a third-party interface are plugged in, ★ always wins auto-connect.

**Reload on connect (v6.2.01+, refined v6.2.07)**

A **full page reload** runs only when the App must swap shells or after a persist/reconnect handshake — **not** on every USB attach:

| Trigger | After reload |
|---------|----------------|
| ★ Octopus ↔ non-★ port change (either direction) | Correct shell: **Octopus ON** (3 tabs) or **MIDI OUT** (2 tabs) |
| Octopus returns after unplug, SAVE, or FULL/BANKS reset | **Octopus ON** + `APP_SYNC_REQ` re-import |
| SETTINGS / MOTION reset (fw ≥ 6.1.01, no USB drop) | Reload → same ★ port → re-pull fresh image |
| Same mode, same class of port (lightweight reconnect) | **No reload** — `_adoptMidiPort()` only |

MIDI patterns are saved to `localStorage` before a reload; Octopus state comes from the device after sync.

**Song mode + patterns (v6.2.03+, MIDI OUT only)**

| Control | Action |
|---------|--------|
| **🔗** (banks row) | Toggle song mode — chain plays banks in order via local clock |
| **PAT / EDIT** | Pattern grid ↔ chain step editor |
| **Chain 01–16** | Select song slot (16 independent chains) |
| **MELODY PATTERNS** | Load factory melody into active bank (steps 1–16) |
| **DRUM PATTERNS** | Load factory drums into active bank (steps 1–16) |

Song chains persist in `localStorage` and **EXP** JSON (`songData`, `isSongMode`).

**When Octopus is unplugged**

- App re-scans ports. If ★ is gone, it may reload into **MIDI OUT** on the next available output (or stay disconnected until you pick a port).
- Plug Octopus back in → auto-pick ★ → reload → **Octopus ON** (MIDI Controller mode is locked out again while ★ is live).

**Transport**

| Mode | Play / Stop / REC / BPM |
|------|-------------------------|
| **Octopus ON** | **Locked** — reflectors only; hardware SCALE / OC / encoder owns transport |
| **MIDI OUT** | **Unlocked** — ▶ ■ REC and BPM field work in the App; local step clock |

---

## 1. OctopusApp (`octopus.isystem.app`)

### Upload / override new version (Ubuntu 22.04, existing nginx)

From your **PC** (repo folder), copy the file over SSH — replace `USER` and `VPS_IP`:

```powershell
# Windows PowerShell
scp OctopusApp.html USER@VPS_IP:/tmp/OctopusApp.html
```

```bash
# On the VPS (SSH in)
sudo cp /tmp/OctopusApp.html /var/www/octopus.isystem.app/index.html
sudo chown www-data:www-data /var/www/octopus.isystem.app/index.html
sudo nginx -t && sudo systemctl reload nginx
```

**Optional backup before overwrite:**

```bash
sudo cp /var/www/octopus.isystem.app/index.html \
  /var/www/octopus.isystem.app/index.html.bak.$(date +%Y%m%d)
```

**Confirm live version:** open `https://octopus.isystem.app` → log line or title must show **OctopusApp v6.2.07**. Hard refresh once (`Ctrl+Shift+R`) if the tab was already open.

**Cloudflare:** if you use edge cache, purge `octopus.isystem.app` after upload (or keep cache bypass rule from §0).

**Social link preview (Facebook, etc.):** `OctopusApp.html` references share images on the product site — `https://octopus-info.isystem.app/octopus-app-hero.jpg` (card image) and `logo.jpg` (favicon). Those files **must** exist on `octopus-info` (see §2). After first share or an App update, refresh Facebook’s cache: [Sharing Debugger](https://developers.facebook.com/tools/debug/) → paste `https://octopus.isystem.app` → **Scrape Again**.

### First-time upload

1. Copy `OctopusApp.html` to the VPS web root for the `octopus` subdomain.
2. Configure the vhost so `/` serves that file (or rename to `index.html` if your stack requires it).
3. **HTTPS is mandatory** — Web MIDI only works in a [secure context](https://developer.mozilla.org/en-US/docs/Web/API/Web_MIDI_API#security_requirements).

### Cache headers (recommended)

Browsers cache HTML aggressively. After each release, users should get the new App without a hard refresh:

```nginx
# Example — nginx location for OctopusApp
location = / {
    # Or path to OctopusApp.html / index.html
    add_header Cache-Control "no-cache, must-revalidate";
    add_header X-Content-Type-Options "nosniff";
}
```

Alternatively bump a query string in bookmarks (`?v=6.2.00`) — not required if `Cache-Control` is set.

### Post-deploy smoke test

Run in **Google Chrome** (or Edge) on **HTTPS**:

| # | Test | Pass criteria |
|---|------|----------------|
| 1 | Page loads | Title shows **OctopusApp v6.2.07** |
| 2 | Octopus mode | With ★ port connected → badge **Octopus ON**; 3 tabs; transport read-only |
| 3 | MIDI mode | Badge **MIDI OUT**; transport unlocked; **🔗** chain + pattern dropdowns |
| 4 | Song mode | 🔗 ON → EDIT chain → Play cycles banks per chain row + repeats |
| 5 | Patterns | MELODY / DRUM dropdown loads factory pattern → Play sends MIDI |
| 6 | Share preview | [FB Debugger](https://developers.facebook.com/tools/debug/) on `https://octopus.isystem.app` shows **octopus-app-hero** image + title |
| 7 | Sequencer | Grid edit, **Play** / **Stop**, playhead moves at BPM |
| 8 | MIDI notes | External synth or loopback receives notes on step advance |
| 9 | INSTRUMENTS | CC knob sends MIDI; PC on change; scopes animate |
| 10 | Persistence | Reload → pattern + song chain restored; **EXP** includes `songData` |
| 11 | Help | **HELP** → **MIDI CONTROLLER** tab documents song mode + patterns |
| 12 | Regression | Reconnect ★ Octopus → full SysEx sync identical to v6.1 behaviour |
| 13 | Octopus lockout | With ★ + another port plugged in, picking non-★ reverts to ★; unplug ★ → MIDI OUT available |

### MIDI Controller production notes

- **No server-side code** — all MIDI runs in the browser via Web MIDI API.
- **No firmware flash** required for v6.2 App features.
- Patterns + **song chains** persist in browser `localStorage` key `octopusapp_midi_session_v1` (not on device NVS).
- Document for users: [User Manual §9.4](./user_manual.md#94-universal-midi-controller-mode-octopusapp-v6207--shipped).

---

## 2. Product site (`octopus-info.isystem.app`)

### Upload

1. `octopus_web.html` → `index.html` (or equivalent entry).
2. Include static assets referenced by the page (`logo.jpg`, `octopus-app-hero.jpg`, etc.) — **required** for OctopusApp social share previews on `octopus.isystem.app`.
3. HTTPS required.

### Post-deploy check

- Nav link **MIDI Mode** → `#midi-mode` section shows **New in OctopusApp v6.2.00** and beginner setup cards.
- **OPEN OCTOPUSAPP** button → `https://octopus.isystem.app`.

---

## 3. GitHub (source of truth)

Tag release when production deploy is verified:

```bash
git tag -a octopusapp-v6.2.07 -m "OctopusApp v6.2.07 — Octopus hard priority + no-reboot Settings/Motion reset"
git push origin octopusapp-v6.2.07
# firmware patch (reflash needed):
git tag -a fw-v6.1.01 -m "Firmware 6.1.01 — SETTINGS/MOTION scoped reset no longer reboots"
git push origin fw-v6.1.01
```

Keep `CHANGELOG.md` [6.2.07] / [6.1.01] entries aligned with the live App + firmware.

---

## 4. What does *not* ship with App v6.2

These remain **firmware / future** work (`code_info.h` §9, `todo.md`):

- Hardware SEQ matrix pages 17–64 on OLED
- OLED P-lock lane editor
- External MIDI OUT via WiFi/BLE coprocessor
- OctopusApp motion-matrix (P-lock) editor in Octopus linked mode

None of these block MIDI Controller mode production.

---

## 5. Rollback

Keep the previous `OctopusApp.html` on the server (e.g. `OctopusApp.v6.1.00.html.bak`). To roll back, restore the file and clear CDN/cache if used.

Octopus linked mode must remain safe — v6.2 does not change firmware protocol.

**PayPal donate link** (optional tip widget): `https://www.paypal.com/donate?hosted_button_id=KX7B76V37PED8` — present in `OctopusApp.html` and `octopus_web.html` after deploy.

---

*© 2026 DIODAC ELECTRONICS / iSystem*
