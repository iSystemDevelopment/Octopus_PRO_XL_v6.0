# v6.1.00 — shipped (firmware)

Completed — see CHANGELOG.md.

---

# OctopusApp v6.2.07 · Firmware v6.1.01 — Universal MIDI Controller ✅ PRODUCTION-READY

Phases **0–7** complete (shipped at v6.2.00). Deploy checklist: **[DEPLOYMENT.md](./DEPLOYMENT.md)**.

> **Current `APP_VERSION`: `6.2.07` · `SYSTEM_FW_VERSION`: `6.1.01`.**
> App post-ship work (6.2.01–6.2.07): song mode + playhead refactor, shared-room
> scene presets, Link Aux toggle, mode-separation hardening, Octopus hard priority,
> P-page grid tools, GOD playhead layer, MIDI ARP/motion, scope layout polish.
> Firmware 6.1.01: SETTINGS/MOTION scoped reset no longer reboots the ESP;
> hardware SEQ MATRIX step pages (`seqUI_stepPage`) — see CHANGELOG [6.1.01].

## Ship checklist (before / after upload)

- [ ] `OctopusApp.html` uploaded to **octopus.isystem.app** (HTTPS)
- [ ] `octopus_web.html` + assets on **octopus-info.isystem.app**
- [ ] HTML `Cache-Control: no-cache` (or users hard-refresh once)
- [ ] (Firmware) Flash **v6.1.01** for no-reboot SETTINGS/MOTION reset + SEQ MATRIX paging
- [ ] Smoke test: title **v6.2.07**, badge **MIDI OUT**, Play → MIDI notes
- [ ] Smoke test: ★ Octopus → **Octopus ON**, SysEx sync unchanged
- [ ] Smoke test (MIDI mode): activity scopes animate while playing (GPU gate fix)
- [ ] Smoke test (MIDI INSTRUMENTS): seq CC panel + SEQ ACT scope — **no vertical scrollbar**
- [ ] Smoke test (Octopus MIXER): drum scope fits grid — **no vertical micro-scrollbar**
- [ ] Smoke test: P-page CPY/PST/CLR — copy P2 → paste P3 leaves P2 unchanged
- [ ] Smoke test: ★ Octopus connected → non-Octopus port selection refused (hard priority)
- [ ] Smoke test: RESET → Settings/Motion = App reloads, **no** device reboot; FULL/Banks = reboot
- [ ] Smoke test: SEQ MATRIX LEN 64 → encoder past col 16 → **P2/4**, steps 17–32 editable
- [ ] Optional: git tag `octopusapp-v6.2.07` · `fw-v6.1.01`

## Phase 0 — Documentation & scaffolding ✅
## Phase 1 — Mode flag + shell swap ✅
## Phase 2 — Outbound router + safety ✅
## Phase 3 — Local transport + playhead ✅
## Phase 4 — Grid → MIDI notes ✅
## Phase 5 — `#view-midi-inst` UI ✅
## Phase 6 — localStorage + ship v6.2.00 ✅
## Phase 7 — Docs final pass ✅

## Hardening — mode-separation audit follow-up ✅ (v6.2.06)

Details: **[docs/midi_controller_mode.md](./docs/midi_controller_mode.md)** → *Post-ship hardening*.

- [x] Split `onConnect()` → `_onConnectOctopus()` / `_onConnectMidi()` (no Octopus lifecycle bleed into MIDI)
- [x] **GPU fix** — `_syncBurstExpected` no longer stuck `true` in MIDI mode (was freezing the activity scopes via `animateVU`'s `gpuBusy` gate)
- [x] Defense-in-depth `_appMode === 'midi'` guards on Octopus-only setters
- [x] Lazy Octopus knob DOM (`_ensureOctopusKnobs()`) — MIDI-only session skips it
- [x] **Octopus hard priority / MIDI lockout** — while a ★ Octopus port is connected, auto-switch to Octopus + refuse non-Octopus port selection (MIDI mode only with no Octopus present)
- [x] Docs: `_txMidiMapped` clarified as a no-op stub; reload documented as the intentional mode boundary

## Reset reboot policy ✅ (firmware v6.1.01 + App v6.2.07)

Details: [docs/midi_controller_mode.md](./docs/midi_controller_mode.md) → *Scoped-reset reboot policy*.

- [x] **SETTINGS / MOTION reset without ESP reboot** — `audio.cpp`: `if (isReset)` sends ACK, then reboots only for FULL/BANKS and `continue`s for SETTINGS/MOTION (no `esp_restart`); `OctopusApp.html`: scope-aware `CMD.SCOPED_RESET` ACK (`_resetReboots()` → reload + re-pull for SETTINGS/MOTION, reboot-wait for FULL/BANKS).
- [x] SAVE and FULL / BANKS+PATS reset keep their reboot.
- [x] Confirm dialog, Help modal, RESET popup, `code_info.h`, `octopus_web.html` updated per-scope.

## Playhead & P-page polish ✅ (App v6.2.07)

Details: **[docs/app_god_rules.md](./docs/app_god_rules.md)** · **[CHANGELOG.md](./CHANGELOG.md)** [6.2.07].

- [x] `#seq-playhead-layer` — dedicated compositor; hover-safe geometry
- [x] P1–P4 always clickable; `beyond-len` dim; manual P = user lock (no auto page steal)
- [x] CPY/PST/CLR/RND + pattern load → **active P page only**
- [x] LOAD / SETTINGS·MOTION RESET → post-sync drain → page reload
- [x] MIDI ARP (utility bar) + motion record on REC
- [x] MIXER drum scope + MIDI seq scope — fit panel, no scrollbars
- [x] Header version badge removed (Help + title + log)
- [x] **Reflash firmware v6.1.01** to devices (the no-reboot behaviour needs the new firmware; App tolerates old firmware too).

---

# Firmware — future work (`code_info.h` §9)

*Separate from MIDI Controller — next hardware/firmware session.*

## Shipped (documented 2026-06-25)

- [x] **Hardware SEQ MATRIX step pages** — `seqUI_stepPage` in `groovebox.cpp`; OLED `P1/4`…`P4/4`; encoder L/R paging; absolute steps 1–64; parity with App P1–P4. Docs: `code_info.h`, `user_manual.md` §7.1/§8.4, `CHANGELOG.md` [6.1.01].

## Known open issues — verify / fix (low priority)

*Adversarial verification pending. Also in `user_manual.md` §12.D and `DEPLOYMENT.md` §4.1.*

1. **Hardware stays APP CONNECTED after App tab closes** — `beforeunload` / `pagehide` call `allNotesOff()` but not `stopHeartbeat()`; firmware should drop link after `APP_HEARTBEAT_TIMEOUT_MS` (4500 ms) when PING stops. **Hypothesis:** worker/tab lifecycle or background audio keep-alive delays timeout.
2. **Connect or switch to Octopus — App doesn't pull live state / reload** — `_onConnectOctopus()` sends `APP_SYNC_REQ` but UI may show stale blobs if `_connOnline` / `_syncBurstExpected` think already linked (possibly related to #1).
3. **Harp → sequencer crosstalk** — breaking laser beams changes sequencer sound setup; engines should be independent (`applyHarpParam` / `applySeqParam`, `patchMux`, D-BEAM `applyDbeamRoute`). LASER SHOW uses `harpHueNoteOn/Off` from seq arp path — investigate mistaken param routing.

## Remaining §9 future work

- [ ] OLED P-lock lane editor (record/playback exists; no on-device lane UI)
- [ ] External MIDI OUT via WiFi/BLE coprocessor (USB = SysEx + play-in only)
- [ ] OctopusApp motion-matrix editor (per-step P-lock editing in browser)
