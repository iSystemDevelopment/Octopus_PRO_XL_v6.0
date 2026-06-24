# v6.1.00 — shipped (firmware)

Completed — see CHANGELOG.md.

---

# OctopusApp v6.2.00 — Universal MIDI Controller ✅ PRODUCTION-READY

Phases **0–7** complete. Deploy checklist: **[DEPLOYMENT.md](./DEPLOYMENT.md)**.

## Ship checklist (before / after upload)

- [ ] `OctopusApp.html` uploaded to **octopus.isystem.app** (HTTPS)
- [ ] `octopus_web.html` + assets on **octopus-info.isystem.app**
- [ ] HTML `Cache-Control: no-cache` (or users hard-refresh once)
- [ ] Smoke test: title **v6.2.00**, badge **MIDI OUT**, Play → MIDI notes
- [ ] Smoke test: ★ Octopus → **Octopus ON**, SysEx sync unchanged
- [ ] Optional: git tag `octopusapp-v6.2.00`

## Phase 0 — Documentation & scaffolding ✅
## Phase 1 — Mode flag + shell swap ✅
## Phase 2 — Outbound router + safety ✅
## Phase 3 — Local transport + playhead ✅
## Phase 4 — Grid → MIDI notes ✅
## Phase 5 — `#view-midi-inst` UI ✅
## Phase 6 — localStorage + ship v6.2.00 ✅
## Phase 7 — Docs final pass ✅

---

# Firmware — future work (`code_info.h` §9)

*Separate from MIDI Controller — next hardware/firmware session.*

- [ ] Hardware SEQ MATRIX step pages (steps 17–64 on OLED; App P1–P4 today)
- [ ] OLED P-lock lane editor (record/playback exists; no on-device lane UI)
- [ ] External MIDI OUT via WiFi/BLE coprocessor (USB = SysEx + play-in only)
- [ ] OctopusApp motion-matrix editor (per-step P-lock editing in browser)
