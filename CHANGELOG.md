# Changelog

All notable changes to Octopus PRO XL firmware and OctopusApp are documented here.

Format based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).
Versioning aligns with firmware `SYSTEM_FW_VERSION` in `code_info.h`.

> **Current version: [6.1.01](#6101--2026-06-25)** — firmware (reflash for companion telemetry).  
> **OctopusApp: [6.6.01](#6601--2026-06-29)** — dual-shell studio + MIDI Controller final.  
> **Previous App release: [6.6.00](#6600--2026-06-25).**

## [6.6.01] — 2026-06-29 (OctopusApp + firmware companion)

Major dual-shell release. Pairs with firmware **6.1.01** (reflash recommended for D-BEAM telemetry and harp patch echo). Deploy `OctopusApp.html` to **octopus.isystem.app**.

### Added (OctopusApp)

- **D-BEAM live bargraph** — MIXER tab expression meter from device→App `SX_SUB_DBEAM_AMP` (0x06) telemetry.
- **Connect-on-Play modal** — ▶ without a MIDI port shows a short connect guide (★ Octopus DSP or MIDI Controller).
- **HELP v6.6.01** — DSP + MIDI tabs updated for CLR/P-locks, reconnect `APP_SYNC_REQ`, WAVE sync, studio VU, motion REC.
- **WAVE 3-way sync** — harp / melody waveform dropdown ↔ knob ↔ hardware echo (`_syncWaveUi`).
- **Studio VU** — master stereo meters with traditional green / yellow / red bands.
- **Keyboard shortcuts** — DSP: `1`/`2`/`3` tabs, `Space` play/stop, `R` record. MIDI: `Space`/`R`.

### Changed (OctopusApp)

- **CLR → hardware** — sequencer **CLR** clears active P-page grid rows **and** sends `CMD_CLR_PLOCKS` so device P-lock lanes wipe with the App.
- **REC auto-play** — arming record from stop auto-sends play first (DSP + MIDI) so motion / P-lock capture always runs against a live transport.
- **USB auto-reconnect** — stale WebMIDI handles refresh after cable replug; same port id re-adopted.
- **MIDI Controller** — pulpit layout shipped in UI (HELP tab); no longer deferred in v6.6 shell.

### Added (firmware companion — reflash)

- **`txDbeamAmpBlob()`** — D-BEAM amplitude on 33 ms link beacon + `echoDbeamExprState()`.
- **`CMD_DBEAM_AMP` (196)** + **`SX_SUB_DBEAM_AMP` (0x06)**; `txSysex` dedup exemption.
- **`recallHarpPatch()`** — echoes `CMD_H_PATCH` to App; dashboard encoder browse sends `H_PATCH` while turning.

### Fixed

- **SysEx parser** — App accepted only frames `length >= 7`, dropping 6-byte D-BEAM blobs.
- **`recallHarpPatch` compile** — removed forward reference to `isAppConnected()` (`txSysex` gates internally).

## [6.6.00] — 2026-06-25 (OctopusApp)

Dual-shell studio redesign (INSTRUMENTS · MIXER · SEQUENCER). SESSION/PATIMAGE browser libraries, 24 drum kits, UV drum scope, SEQ SYNTH 24×CC macro-groups, iOS breakpoints, first-connection hard-refresh.

## [6.1.01] — 2026-06-25 (Firmware)

Firmware patch — scoped-reset reboot policy plus a same-day **drum-voice refinement** pass for more classic-sounding metals, clap and snare. Pairs with OctopusApp [6.6.01](#6601--2026-06-29). Reflash required to get the no-reboot behaviour, drum voicing, and v6.6.01 companion telemetry; the shipped App tolerates both old and new firmware.

### Added

- **Hardware SEQ MATRIX step pages** — `seqUI_stepPage` (0–3) pages the OLED 16×8 matrix across the full pattern (up to 64 steps, bounded by `seqLength`). Encoder L/R at column 0/15 advances step page before bank wrap; `seqUI_toggleStep()` uses absolute column `col + stepPage×16`; status bar `P%n/%d` + absolute step `S%02d/%d`; playhead underline on the current page only; steps beyond LEN crossed out (parity with OctopusApp P1–P4).

### Changed

- **SETTINGS / MOTION scoped reset no longer reboots the ESP.** They were already applied live by `applyResetScope()`, but `settings_save_task` (`audio.cpp`) still called `esp_restart()` afterwards. The `if (isReset)` branch now sends the `CMD_SCOPED_RESET` ACK and reboots **only** for `ResetScope::FULL` / `BANKS_PATTERNS`; for SETTINGS / MOTION it skips the restart and `continue`s. The App reloads on the ACK and re-pulls the fresh settings/motion image via `APP_SYNC_REQ`. SAVE and FULL / BANKS+PATS reset keep their reboot.
- **Drum synthesis — classic snare / clap / metal-hat voicing.** Reworked the three noise-heavy voices in `groovebox.h` and added per-kit character tables in `globals.h`: snare is now a **body wavetable + bandpassed rattle + short click transient** (`KIT_SNARE_BODY_LO/HI`, `KIT_SNARE_SNAP_FC`, `KIT_SNARE_RATTLE_DELTA`, `KIT_SNARE_PITCH_MUL`, `KIT_SNARE_CLICK`, `KIT_SNARE_DECAY_SCALE`, `KIT_SNARE_WAVE`); clap is a **per-kit bandpass triple-burst with a Noise-knob layer** (`KIT_CLAP_BURST1/2/3`, `KIT_CLAP_FILTER_LP/HP`) — the Noise knob now drives the clap layer level (previously ignored); hats use a **per-kit 6-osc metal amplitude** (`KIT_HAT_METAL_AMP`) and tuned base frequencies. `applyDrumKit()` also loads the kit snare wavetable (`drumWaveIdx[1]`) and echoes it via `CMD_DRUM_WAVE` so App knobs follow. Updated `DrumSettings` factory defaults + `DRUM_KIT_*` tables to match. Knob mapping per voice (snare: Tune = snap brightness / Noise = wires; clap: Tune = filter centre / Noise = layer; hats: Tune = HPF cutoff / Noise = wash) is documented in `code_info.h` §8.I.
- **Drum pitch normalisation.** Snare body + hats normalise through `DRUM_PITCH_FACTORY` (×0.60) so the default Drm Pitch keeps classic TR voicing while kick/toms/perc still track the global knob directly.
- **D-BEAM VOLUME — harp/seq independence** — leaving the VOLUME route or switching D-BEAM target no longer restores the untouched engine's `mixSeqVol` / `mixHarpVol` from a stale entry snapshot; only buses the pedal actually dipped are restored (`dbeamVolumeRestoreEngagedBuses()`).

### Unchanged

- SAVE (`requestScopedSave` → `g_restartAfterSave` → reboot ~700 ms) and the FULL / BANKS+PATS deferred boot-reset path are unchanged.
- **NVS layout** — the drum pass changes default *values* only, not the `AllSettings` struct layout, so `SETTINGS_VERSION` stays **`0x0616`** (no migration; existing saves keep their stored drum knobs).

## [6.2.11] — 2026-06-25 (OctopusApp + firmware)

### Removed

- **Scoped LOAD** — App LOAD button/modal, `CMD_SESSION_LOAD` handler, hardware MAIN MENU → LOAD category. NVS restores at boot; factory patterns load from sequencer dropdowns (`LOAD_PAT_S/D`).
- **App SLOTS vault** — header button and popup removed (hardware Save/Load Slot menus unchanged).
- **BOOT_READY handshake (v6.2.10)** — reverted; SAVE/RESET again use `_reloadAfterReconnect` after USB returns.

### Fixed

- **SAVE/RESET App reload** — restored pre–v6.2.10 reconnect → full page reload path (no 2 s boot delay).

## [6.2.10] — 2026-06-25 (reverted in 6.2.11)

Superseded — BOOT_READY / in-page LOAD drain removed.

## [6.2.09] — 2026-06-25 (OctopusApp + firmware)

### Fixed

- **Sequencer matrix P2–P4** — `toggleCell()` read `gridData[r][col]` instead of absolute step `page×16+col`; steps 17–64 appeared dead or toggled the wrong cell.
- **MOTION / SETTINGS reset hang (hardware)** — runtime reset now uses deferred `pend_rst` + reboot + boot-time wipe (same reliable kernel as FULL/BANKS), replacing the NvsWorker path that could stall on "PLEASE WAIT…".
- **App RESET policy** — all four scopes reboot the ESP and reload the App (SAVE already did).

### Firmware (reflash)

- `pend_rst` values 3=MOTION, 4=SETTINGS; `handleScopedReset()` always defers at runtime.

## [6.2.08] — 2026-06-25 (OctopusApp)

Browser-only link fixes, pairs with firmware [6.1.01](#6101--2026-06-25) (reflash for D-BEAM fix #3).

### Fixed

- **OLED stuck APP CONNECTED after tab close** — `_teardownOctopusLink()` stops the heartbeat worker, reconnect poller, and silent AudioContext on `beforeunload`/`pagehide` (was leaving PING running).
- **Stale UI after USB replug / same-port reconnect** — `_octopusResync()` when WebMIDI `onstatechange` keeps the same port id; `_connectMidiPort()` no longer skips sync when `_connOnline` is false.

## [6.2.07] — 2026-06-25 (OctopusApp)

Browser-only, pairs with firmware [6.1.01](#6101--2026-06-25).

### Added

- **Octopus hard priority (MIDI lockout)** — while any live ★ Octopus port exists, the App locks to Octopus mode: auto-switch to Octopus (no decline prompt) and refuse selection of non-Octopus ports (reverts the picker with a message). MIDI Controller mode is reachable only with no Octopus connected. (This supersedes the v6.2.06 confirm-to-switch dialog.)

### Changed

- **Settings / Motion reset = no device reboot** (with firmware ≥ 6.1.01) — `resetScoped()` stores the scope; `_resetReboots()` gates the `CMD.SCOPED_RESET` ACK path: FULL/BANKS take the reboot-wait path, SETTINGS/MOTION reload the App and re-pull from the device. Confirm dialog + Help + RESET popup text now state the per-scope behaviour.
- **Playhead (GOD layer)** — `#seq-playhead-layer` dedicated compositor; geometry from stage CSS only (no per-cell layout reads on hover); page-cross defers grid repaint to one rAF so STEP_SYNC never stutters.
- **P1–P4 always active** — all four page buttons stay clickable at any LEN; steps beyond pattern length show dimmed (`beyond-len`). Manual P click sets **user lock** — playhead keeps running but won't auto-switch the grid view until you pick another P page.
- **Grid tools = active P page only** — **CPY**, **PST**, **CLR**, **RND-H**, **RND-D**, and factory **MELODY/DRUM PATTERNS** affect only the 16 steps on the selected P page (not the full 64-step bank). Octopus **CLR** clears the local page and tx's one page of grid-row blobs — it does **not** send hardware SEQ_CLEAR or reset sounds.
- **LOAD / SETTINGS·MOTION RESET reload** — after NVS sync burst drains, the App reloads and re-imports via `APP_SYNC_REQ` (consistent shell vs device blobs).
- **MIDI melody ARP** — utility-bar **ARP** + PAT/RATE/GATE; expands active synth rows on each step; persists in session JSON.
- **MIDI motion record** — **REC** captures CC knob moves per step (purple dots on grid); replays on Play.
- **UI layout** — header version badge removed (version in page title, startup log, and Help); MIXER drum scope and MIDI seq activity scope fit their panels with no micro-scrollbars; seq CC knobs use a compact 4×2 grid in INSTRUMENTS.

### Fixed

- **P2–P4 appeared dead** when LEN=16 — `pointer-events: none` removed; all pages always editable.
- **Parse error blocked App boot** — duplicate `page` binding in `_clearGridPageOctopusTx()`.
- **Song-mode logic fixtures** — hardened the song ↔ pattern toggle and chain-loop playback: the song editor defers DOM via the `_songEditorDirty` rAF, returning to the GRID re-measures the playhead, and a pending STEP_SYNC step is restored when the grid stage is hidden. In MIDI Controller mode the 🔗 chain advances locally through `_midiAdvanceSongChain()` / `_midiResetSongPlayback()` (repeats + bank-per-row, no device SysEx). Inbound `SONG_POS` joins STEP_SYNC/TRANSPORT/BPM in the MIDI-mode RX block so a stray echo can't move the local playhead. Authoritative checklist: [docs/app_god_rules.md](./docs/app_god_rules.md).

### Docs

- **Doc/codebase consistency pass** — aligned `code_info.h` (manifest now describes v6.1.01; SysEx table header `0–195`; added missing entries 165 `PLAY_MODE`, 166 `H_PITCH`, 171 `SOFT_RESET`, 190–193 drum FX / `SEQ_RESTART`; new §8.I drum-voice synthesis section), corrected the stale `SETTINGS_VERSION 0x0615 → 0x0616` in `README.md`, refreshed the in-app **DRUM KITS** Help with the new classic voicing, and removed a duplicate snare comment in `settings.h`. Verified App ↔ firmware 1:1 for the `CMD` map, scale tables (`SCALE_MIDI_NOTES` ↔ `SCALES_NOTES`), `GM_DRUM_MAP`, and drum-kit names.

## [6.2.06] — 2026-06-25 (OctopusApp)

Browser-only. **No firmware changes** (firmware stays 6.1.00). Mode-separation audit follow-up — closes the lifecycle/hygiene gaps left after the v6.2.00 dual-mode ship. Octopus linked mode is unchanged in behaviour. Rolls up the App-only 6.2.01–6.2.05 work (song mode + playhead refactor, shared-room scene presets, Link Aux toggle).

### Fixed

- **MIDI Controller activity scopes were frozen** — `onConnect()` armed `_syncBurstExpected = true` unconditionally. In MIDI mode `_parseDeviceSysex()` returns early, so the RX queue never drains and the flag never cleared; `animateVU`'s `gpuBusy` gate (`_syncBurstActive || _syncBurstExpected`) then permanently skipped `_tickDrumScope()`, so the seq/drum activity scopes never animated. The sync-burst gate is now armed **only** for an Octopus port (`_syncBurstExpected = oct`), and MIDI connect (`_onConnectMidi()`) clears it.
- **Octopus shell could show empty knob panels** — with lazy DOM, the disconnected / WebMIDI-unavailable states (both render the Octopus shell) are now covered by `_ensureOctopusKnobs()` via `setAppMode` and a boot fallback.

### Changed

- **`onConnect()` split** into `_onConnectOctopus()` / `_onConnectMidi()` — the shared preamble does only the inbound/link reset and port open; NVS slot cache, persist-modal cleanup, sync burst, `APP_SYNC_REQ` and heartbeat run **only** in the Octopus branch.
- **Lazy Octopus DOM** — the five knob panels (laser/harp/seq/drum/master) build on first entry into the Octopus shell via `_ensureOctopusKnobs()`; a MIDI-only session no longer builds them.
- **Defense-in-depth** — Octopus-only setters (`setPlayMode`, `toggleMute`, `toggleDbeam`, `setDrumWave`, `setDrumKit`, `toggleLaserShow`, `toggleMidiHue`, `setHarpWave`, `setSeqWave`, `setHarpOctave`, `setPbRange`, `setPbEnable`) early-return in MIDI mode.

### Added

- **Octopus hard priority (MIDI lockout)** — while any live ★ Octopus port exists, the App is locked to Octopus mode: it auto-switches to Octopus (no decline prompt) and refuses selection of non-Octopus ports (reverts the picker with a message). MIDI Controller mode is reachable only when no Octopus is connected. `_pickAutoOutput` already ranks ★ ports first, so a simultaneously-connected third-party interface never wins.

### Docs

- `docs/midi_controller_mode.md` — "Golden rule" rewritten (notes/CC/PC use the `_midi*` helpers; `_txMidiMapped` is a permanent no-op stub), reload documented as the intentional mode boundary, "Octopus hard priority" + "Post-ship hardening" sections, and a "Pending (firmware-gated) — scoped-reset reboot policy" note; corrected `_midiClockTick` (4 ms `setInterval`, not `animateVU`).

### Known / pending (firmware-gated)

- **SETTINGS / MOTION reset still reboots the ESP** — resolved the same day in firmware [6.1.01](#6101--2026-06-25) + App [6.2.07](#6207--2026-06-25).

## [6.2.00] — 2026-06-23 (OctopusApp)

Browser-only release. **No firmware changes.** Authoritative build order: [docs/midi_controller_mode.md](./docs/midi_controller_mode.md).

### Added

- **Universal MIDI Controller mode** — when no Octopus hardware is linked, select any Web MIDI output port; App sends standard note/CC/PC instead of Octopus SysEx.
- **Trimmed 2-tab layout** — SEQUENCER (shared grid + local playhead) + INSTRUMENTS (seq synth MIDI panel + drum machine scope); MIXER / laser / D-BEAM / NVS tools unavailable.
- **App-owned transport** — play, stop, editable BPM, local 16th-note step clock; MIDI Start/Stop (`0xFA`/`0xFC`); optional **MIDI clock out** (`0xF8`, 24 PPQN).
- **Grid → MIDI notes** — melody rows use firmware scale map + transpose/octave; drum rows use GM notes (editable per row).
- **INSTRUMENTS panels** — 8 CC knobs, Program Change, per-row drum notes, beat-reactive scope canvases.
- **Dual Help** — separate in-app Help tab for MIDI Controller mode.
- **localStorage** — `octopusapp_midi_session_v1` stores all 4 banks, routing, CC/PC map (separate from Octopus NVS / slot cache).
- **EXP / IMP** — export/import MIDI session as JSON from the routing bar.

### Changed (docs)

- **User manual §9.4** — shipped MIDI Controller beginner guides (macOS / Windows + Chrome, DAW routing).
- **Product site** — MIDI Mode section updated for v6.2.00; in-app Help MIDI CONTROLLER tab.
- **DEPLOYMENT.md** — production upload + smoke-test checklist for App v6.2.00.

### Unchanged

- **Octopus linked mode** — identical to v6.1.00 when ★ Octopus port + SysEx echo is detected.
- **Firmware** — remains **6.1.00**.

## [6.1.00] — 2026-06-23

### Fixed

- **AUX reverb SIZE — ~5 % drift + top-of-range dead zone** — the App (and the firmware's own echo) encode reverb size as `(size/0.95)·16383`, but both decode paths (`applyAuxParam` and `applyMasterParam` in `patches.h`) stored `min(0.95, n)` instead of `n·0.95` — so the bus reverb ran ~5 % larger than the knob showed and the top ~5 % of the knob all collapsed to 0.95. Decoded with `·0.95` now (same fix already applied to AUX delay-feedback). Delay-time, delay-FB and reverb-damp round-trips verified stable.
- **D-BEAM VOLUME pedal — cumulative volume drift** — the inverted volume pedal adopted the *live* bus level as its baseline every rest tick, but the follower's release is gradual so the bus is still slightly dipped when it crosses the rest threshold. Each hand gesture therefore shrank the harp/seq baseline (~1 %+, more on a fast lift), so the level crept down over a set. Also left the bus **stuck low** if D-BEAM was disabled or re-routed mid-gesture. Reworked `routeDbeamExpression()` (`dbeam.cpp`) to capture the baseline once on the **press edge** and fully restore the bus on the **lift edge** — no drift, no stuck-low bus.
- **App "RND" (randomize) did not restart playback** — OctopusApp sent `CMD_SEQ_RESTART` (193) after RND-H / RND-D so a freshly randomised pattern plays from beat 1, but the firmware had **no handler** for it, so the sequencer kept running from its current step. Added `seq_restart_rt()` (`groovebox.cpp`) + dispatch case (`midi.cpp`): the step counter is zeroed without stopping playback (no-op when stopped).
- **CPU-load telemetry mismatch (dead App warnings)** — OctopusApp's `CMD_CPU_LOAD` handler decoded an out-ring drop count (bits 7–13) and a P-lock "steal" flag from **bit 14**, but the firmware sent only the raw load %, and bit 14 is unaddressable in a 14-bit SysEx value, so both warnings could never fire. Firmware now packs **load % (bits 0–6) · drop count (bits 7–12, saturating 0–63) · lanes-full flag (bit 13)**; the App decode and the (previously incorrect) "oldest lane replaced" message were corrected — the firmware *drops* new automation when all 4 P-lock lanes are full, it never steals.
- **FULL / Banks+Pats reset false FAIL + wedged UI** — runtime heavy NVS writes while audio/tasks were active could time out, show FAIL, and skip reboot even when flash was wiped later. Replaced with deferred boot reset (`pend_rst` + instant reboot; wipe before `loadSettings()`).
- **Reset Settings (and all scoped SAVE/LOAD/RESET from App)** — App encodes persist scope as `v14 = ResetScope + 1` (1–4) but firmware decoded `v14 & 3`, so SETTINGS (4) became FULL (0) and FULL (1) became BANKS_PATTERNS. Fixed `decodePersistScopeV14()` in `midi.cpp`.
- **Reset/Save always FAILED in App** — scoped RESET never echoed `CMD_SCOPED_RESET` ACK before reboot (only `CMD_SESSION_SAVE` ACK was sent). `g_persistAckCmd` routes the correct ACK command.
- **Hardware LOAD while App connected** — `handleScopedLoad()` now calls `sendFullStateSync()` + `CMD_SESSION_LOAD` ACK so the App reloads and mirrors NVS state.
- **Wedged persist flags** — `recoverWedgedPersistFlags()` clears stale `g_saveRequest` before new SAVE/RESET attempts.

### Changed

- **Insert FX-A names unified to the cosmic set** — the slot-A preset labels drifted: `effect.cpp INSERT_FX_PRESETS[]` carried the authored cosmic names ("Nebula Taps", "Snova Chorus"…) while the OLED (`kInsertFxNames`) and App (`INSERT_FX_NAMES`) still showed the legacy set ("AcidRoom", "HiChorus"…). The index→DSP mapping was always identical, so this is labels only — now aligned across `display.h`, OctopusApp, and `user_manual.md` to match `effect.cpp` (consistent with the master-FX/wave cosmic branding).
- **Drum scope is now beat-reactive** — the MIXER-tab drum oscilloscope previously free-ran on random noise gated by play state. It now fires a transient on the **actual drum hits** at the live playhead step (drum grid rows 8–15 of the playing bank, kick/snare weighted heavier than hats/perc, downbeat accent, scaled by D.VOL), decaying on empty steps. The canvas was moved to a **definitive, isolated GPU compositor layer** (`will-change`/`contain`/`isolation`) so its per-frame repaints never reflow the mixer UI, and it ticks only while actually on-screen.
- **Legacy grid-row SysEx retired** — the numeric `CMD_GRID_ROW_LO/HI` (162/163) handler is removed (firmware) and no longer sent (App). Its old v14 packing overlapped the 8 step bits with the page field, corrupting steps 4–5 on pages 1–3. Bulk grid sync runs **both directions** through the lossless `SX_SUB_GRID_ROW` (sub 0x05) blob. The wire IDs stay **reserved** (never renumbered); docs in `sysex.h` / `code_info.h` / OctopusApp updated.
- **Soft Reset removed** — `CMD_SOFT_RESET` (171) ignored on RX; SEQ SETUP menu entry removed; `seqSoftResetWorkingImage()` deleted. Use **RESET → Settings** for factory knob/mixer defaults (persisted + reboot).
- **Deferred boot reset (FULL / Banks+Pats)** — runtime reset no longer writes large NVS blobs while audio/tasks run. `settings_arm_pending_reset()` stores `pend_rst` in NVS, ACKs the App, and `esp_restart()` in ~150 ms. `settings_execute_pending_reset_at_boot()` (before `loadSettings()`) applies the wipe in the same safe window as the OC+SCALE combo. SETTINGS/MOTION reset still use NvsWorker + reboot.
- **OctopusApp v6.1.00** — auto-connect on USB; Connect/Disconnect buttons removed; badge **Octopus ON / Octopus Off**; full browser reload after SAVE/LOAD/RESET (and hardware-initiated persist) then re-import via `APP_SYNC_REQ`; reset NACK waits for USB reconnect instead of instant FAIL.
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
