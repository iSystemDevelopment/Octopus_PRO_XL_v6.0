# v6.1.00 — shipped (firmware)

Completed — see CHANGELOG.md.

---

# OctopusApp v6.2.06 — Universal MIDI Controller ✅ PRODUCTION-READY

Phases **0–7** complete (shipped at v6.2.00). Deploy checklist: **[DEPLOYMENT.md](./DEPLOYMENT.md)**.

> **Current `APP_VERSION`: `6.2.06`.** Post-ship work (6.2.01–6.2.06) is App-only,
> firmware still **6.1.00**: song mode + playhead refactor, shared-room scene presets,
> Link Aux toggle, and the mode-separation hardening pass below.

## Ship checklist (before / after upload)

- [ ] `OctopusApp.html` uploaded to **octopus.isystem.app** (HTTPS)
- [ ] `octopus_web.html` + assets on **octopus-info.isystem.app**
- [ ] HTML `Cache-Control: no-cache` (or users hard-refresh once)
- [ ] Smoke test: title **v6.2.06**, badge **MIDI OUT**, Play → MIDI notes
- [ ] Smoke test: ★ Octopus → **Octopus ON**, SysEx sync unchanged
- [ ] Smoke test (MIDI mode): activity scopes animate while playing (GPU gate fix)
- [ ] Smoke test: ★ Octopus appearing mid-MIDI-session prompts before reload
- [ ] Optional: git tag `octopusapp-v6.2.06`

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
- [x] Confirm (`_offerOctopusSwitch`) before hijacking a live MIDI session into Octopus
- [x] Docs: `_txMidiMapped` clarified as a no-op stub; reload documented as the intentional mode boundary

---

# Firmware — future work (`code_info.h` §9)

*Separate from MIDI Controller — next hardware/firmware session.*

- [ ] Hardware SEQ MATRIX step pages (steps 17–64 on OLED; App P1–P4 today)
- [ ] OLED P-lock lane editor (record/playback exists; no on-device lane UI)
- [ ] External MIDI OUT via WiFi/BLE coprocessor (USB = SysEx + play-in only)
- [ ] OctopusApp motion-matrix editor (per-step P-lock editing in browser)
