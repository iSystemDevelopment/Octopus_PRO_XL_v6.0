# Deployment — Octopus PRO XL firmware v6.1.01 + OctopusApp v6.6.01

Production hosting for the **browser App** and **product site**. Firmware is flashed via USB (not covered here).

| Asset | URL | Source file |
|-------|-----|-------------|
| **OctopusApp** | [octopus.isystem.app](https://octopus.isystem.app) | `OctopusApp.html` |
| **Product site** | [octopus-info.isystem.app](https://octopus-info.isystem.app) | `octopus_web.html` → `index.html` |

**Versions:** Firmware **6.1.01** · OctopusApp **6.6.01** (dual-shell: Octopus DSP Engine + MIDI Controller). UI flowcharts: [`docs/ui_flowcharts.md`](docs/ui_flowcharts.md).

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

# App site (this VPS uses /var/www/octopus — not octopus.isystem.app folder name)
sudo mkdir -p /var/www/octopus
sudo cp OctopusApp.html /var/www/octopus/index.html
sudo cp manifest.webmanifest sw.js icon-192.png icon-512.png logo.jpg /var/www/octopus/

# Product site
sudo mkdir -p /var/www/octopus-info.isystem.app
sudo cp octopus_web.html /var/www/octopus-info.isystem.app/index.html
# + copy logo.jpg, octopus-app-hero.jpg, etc.
```

Use the ready-made config in the repo (recommended):

```bash
sudo cp deploy/nginx-octopus.isystem.app.conf /etc/nginx/sites-available/octopus.isystem.app
sudo nginx -t && sudo systemctl reload nginx
```

Or paste manually — web root **`/var/www/octopus`**:

```nginx
# /etc/nginx/sites-available/octopus.isystem.app

server {
    listen 80;
    listen [::]:80;
    server_name octopus.isystem.app;
    return 301 https://$host$request_uri;
}

server {
    listen 443 ssl http2;
    listen [::]:443 ssl http2;
    server_name octopus.isystem.app;

    root /var/www/octopus;
    index index.html;

    access_log /var/log/nginx/octopus.access.log;
    error_log  /var/log/nginx/octopus.error.log;

    ssl_certificate     /etc/letsencrypt/live/octopus.isystem.app/fullchain.pem;
    ssl_certificate_key /etc/letsencrypt/live/octopus.isystem.app/privkey.pem;
    include /etc/letsencrypt/options-ssl-nginx.conf;
    ssl_dhparam /etc/letsencrypt/ssl-dhparams.pem;

    add_header X-Content-Type-Options "nosniff" always;

    location = /sw.js {
        try_files /sw.js =404;
        default_type application/javascript;
        add_header Cache-Control "no-cache, must-revalidate";
    }

    location = /manifest.webmanifest {
        try_files /manifest.webmanifest =404;
        default_type application/manifest+json;
        add_header Cache-Control "no-cache, must-revalidate";
    }

    location = /logo.jpg {
        try_files /logo.jpg =404;
        add_header Cache-Control "public, max-age=86400";
    }

    location = /icon-192.png {
        try_files /icon-192.png =404;
        default_type image/png;
        add_header Cache-Control "public, max-age=86400";
    }

    location = /icon-512.png {
        try_files /icon-512.png =404;
        default_type image/png;
        add_header Cache-Control "public, max-age=86400";
    }

    location = /sitemap.xml {
        try_files /sitemap.xml =404;
        default_type application/xml;
    }

    location = /sitemap.xsl {
        try_files /sitemap.xsl =404;
        default_type application/xml;
    }

    location = /robots.txt {
        try_files /robots.txt =404;
        default_type text/plain;
    }

    location / {
        try_files $uri $uri/ /index.html;
        add_header Cache-Control "no-cache, must-revalidate";
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

### Desktop install (PWA)

OctopusApp is a **Progressive Web App**. Users install from the browser — no separate `.exe` or raw HTML handout.

| File | Purpose |
|------|---------|
| `manifest.webmanifest` | App name, icons, `standalone` window |
| `sw.js` | Offline shell cache (bump `CACHE` in `sw.js` each release) |
| `icon-192.png` / `icon-512.png` | Square PWA icons (`node scripts/gen-pwa-icons.mjs` from `logo.jpg`) |
| `logo.jpg` | Legacy / social fallback (optional on app root) |

Deploy to the app site root: `index.html`, `manifest.webmanifest`, `sw.js`, `icon-192.png`, `icon-512.png`.

**Verify before telling users to install:** open `https://octopus.isystem.app/manifest.webmanifest` — you must see JSON (`{ "name": "OctopusApp…`), not the app HTML. Same for `https://octopus.isystem.app/sw.js` (JavaScript). If you see HTML, the files are missing and nginx is falling back to `index.html`.

On release: update the `CACHE` string in `sw.js` so installed clients pick up the new App.

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
scp octopus-consent-seo.js USER@VPS_IP:/tmp/octopus-consent-seo.js
```

```bash
# On the VPS (SSH in)
sudo cp /tmp/OctopusApp.html /var/www/octopus.isystem.app/index.html
sudo cp /tmp/octopus-consent-seo.js /var/www/octopus.isystem.app/octopus-consent-seo.js
sudo cp seo/sitemap-octopus-app.xml /var/www/octopus/sitemap.xml
sudo cp seo/sitemap.xsl /var/www/octopus/sitemap.xsl
sudo cp seo/robots-octopus-app.txt /var/www/octopus/robots.txt
sudo chown www-data:www-data /var/www/octopus.isystem.app/index.html /var/www/octopus.isystem.app/octopus-consent-seo.js /var/www/octopus.isystem.app/sitemap.xml /var/www/octopus.isystem.app/robots.txt
sudo nginx -t && sudo systemctl reload nginx
```

**Cookie consent + SEO (v6.6+):** `octopus-consent-seo.js` must sit beside `index.html` (same pattern as `isystem.app`). Set your **Google Analytics 4** ID in `OctopusSiteKit.init({ ga4MeasurementId: 'G-…' })` inside `OctopusApp.html` before deploy. Analytics loads **only** after the user accepts analytics cookies (Consent Mode v2). Optional: `googleSiteVerification` in `init()` for Search Console.

**Optional backup before overwrite:**

```bash
sudo cp /var/www/octopus.isystem.app/index.html \
  /var/www/octopus.isystem.app/index.html.bak.$(date +%Y%m%d)
```

**Confirm live version:** open `https://octopus.isystem.app` → log line or title must show **OctopusApp v6.6.01**. Hard refresh once (`Ctrl+Shift+R`) if the tab was already open.

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
| 1 | Page loads | Title shows **OctopusApp v6.6.01** |
| 2 | Octopus mode | With ★ port connected → badge **Octopus ON**; 3 tabs; transport read-only |
| 3 | MIDI mode | Badge **MIDI OUT**; transport unlocked; **🔗** chain + pattern dropdowns |
| 4 | Song mode | 🔗 ON → EDIT chain → Play cycles banks per chain row + repeats |
| 5 | Patterns | MELODY / DRUM dropdown loads factory pattern → Play sends MIDI |
| 6 | Share preview | [FB Debugger](https://developers.facebook.com/tools/debug/) on `https://octopus.isystem.app` shows **octopus-app-hero** image + title |
| 7 | Sequencer | Grid edit, **Play** / **Stop**, playhead moves at BPM |
| 8 | MIDI notes | External synth or loopback receives notes on step advance |
| 9 | INSTRUMENTS | CC knob sends MIDI; PC on change; SEQ ACT / DRUM ACT scopes animate; **no panel scrollbars** |
| 10 | P-page tools | LEN=64 → copy P2, paste P3 → P2 unchanged; CLR clears active P page only |
| 11 | Persistence | Reload → pattern + song chain restored; **EXP** includes `songData` |
| 12 | Help | **HELP** → **MIDI CONTROLLER** tab documents song mode + patterns + P-page tools |
| 13 | Regression | Reconnect ★ Octopus → full SysEx sync identical to v6.1 behaviour |
| 14 | Octopus lockout | With ★ + another port plugged in, picking non-★ reverts to ★; unplug ★ → MIDI OUT available |
| 15 | MIXER scope | Octopus ON → MIXER tab → drum scope visible, no vertical micro-scrollbar |

### MIDI Controller production notes

- **No server-side code** — all MIDI runs in the browser via Web MIDI API.
- **No firmware flash** required for v6.2 App features.
- Patterns + **song chains** persist in browser `localStorage` key `octopusapp_midi_session_v1` (not on device NVS).
- Document for users: [User Manual §9.4](./user_manual.md#94-universal-midi-controller-mode-octopusapp-v6207--shipped).

---

## 2. Product site (`octopus-info.isystem.app`)

### Upload

1. `octopus_web.html` → `index.html` (or equivalent entry).
2. `octopus-consent-seo.js` → same web root (cookie banner + JSON-LD; shared with OctopusApp).
3. `seo/sitemap-octopus-info.xml` → `sitemap.xml` · `seo/sitemap.xsl` → `sitemap.xsl` · `seo/robots-octopus-info.txt` → `robots.txt`.
4. Include static assets referenced by the page (`logo.jpg`, `octopus-app-hero.jpg`, `octopus-midi-mode.png`, etc.) — **required** for product page MIDI section and OctopusApp social share previews.
5. HTTPS required. Set `ga4MeasurementId` in `OctopusSiteKit.init()` inside `octopus_web.html` when GA4 property is ready.

### Post-deploy check

- Cookie banner appears on first visit; **Manage Cookies** in footer reopens preferences.
- Nav link **MIDI Mode** → `#midi-mode` section.
- **OPEN OCTOPUSAPP** button → `https://octopus.isystem.app`.
- [Google Rich Results Test](https://search.google.com/test/rich-results) on product URL (Product + Organization schema).

---

## 3. GitHub (source of truth)

Tag release when production deploy is verified:

```bash
git tag -a octopusapp-v6.6.01 -m "OctopusApp v6.6.01 + firmware 6.1.01 companion — dual-shell studio, D-BEAM telemetry, CLR P-locks"
git push origin octopusapp-v6.6.01
# firmware patch (reflash needed):
git tag -a fw-v6.1.01 -m "Firmware 6.1.01 — SETTINGS/MOTION scoped reset no longer reboots"
git push origin fw-v6.1.01
```

Keep `CHANGELOG.md` [6.6.01] / [6.1.01] entries aligned with the live App + firmware.

---

## 4. What does *not* ship with App v6.2

These remain **firmware / future** work (`code_info.h` §9; see `local/operator/todo.md` on your machine):

- OLED P-lock lane editor
- External MIDI OUT via WiFi/BLE coprocessor
- OctopusApp motion-matrix (P-lock) editor in Octopus linked mode

None of these block MIDI Controller mode production.

**Hardware SEQ MATRIX step pages (steps 17–64)** — shipped in current firmware source (`groovebox.cpp` `seqUI_stepPage`). Smoke test on device: set LEN 64 → SEQ MATRIX → encoder past column 16 shows **P2/4** and steps 17–32.

### 4.1 Link issues (fixed App v6.2.08 / fw)

1. Tab close — App `_teardownOctopusLink()` stops PING (was leaving heartbeat running).
2. Same-port USB replug — `_octopusResync()` when `onstatechange` keeps the same port id.
3. Harp→seq volume — `dbeamVolumeRestoreEngagedBuses()` on D-BEAM VOLUME route exit.

---

## 5. Rollback

Keep the previous `OctopusApp.html` on the server (e.g. `OctopusApp.v6.1.00.html.bak`). To roll back, restore the file and clear CDN/cache if used.

Octopus linked mode must remain safe — v6.2 does not change firmware protocol.

**PayPal donate link** (optional tip widget): `https://www.paypal.com/donate?hosted_button_id=KX7B76V37PED8` — present in `OctopusApp.html` and `octopus_web.html` after deploy.

---

*© 2026 DIODAC ELECTRONICS / iSystem*
