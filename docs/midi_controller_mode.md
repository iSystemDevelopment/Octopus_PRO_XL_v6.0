# OctopusApp — Universal MIDI Controller Mode

**Status:** **Shipped** · **OctopusApp v6.2.00** (browser-only; **firmware unchanged at 6.1.00**)

Authoritative build plan for the dual-mode App: **Octopus linked** (current v6.1 behaviour) vs **MIDI controller** (standalone sequencer + CC/PC to any USB MIDI interface).

---

## Goals

| Goal | Detail |
|------|--------|
| **Isolation** | Octopus SysEx sync path and MIDI-controller path never share outbound logic at runtime |
| **Scope** | MIDI mode = **64-step sequencer** + **Program Change** + **CC** only |
| **Layout** | Switchable shell: 2 tabs (SEQUENCER + INSTRUMENTS); no MIXER tab |
| **INSTRUMENTS (MIDI)** | Seq synth panel + drum machine canvas (not studio mixer / laser / D-BEAM) |
| **Transport** | App-owned play/stop/BPM + local step clock; optional MIDI Start/Stop/Clock |
| **Persistence** | Pattern + MIDI map in `localStorage` only — never Octopus NVS |
| **Graphics** | Reuse `#seq-playhead` GPU layer; drum scope compositor unchanged |

---

## Mode state machine

```
disconnected ──► octopusLinked     (★ port + SysEx echo within grace period)
disconnected ──► midiController    (user picks non-Octopus port OR explicit choice)
octopusLinked  ──► midiController  (user changes port — confirm if needed)
midiController ──► octopusLinked   (Octopus plugged + sync OK)
```

**`app._appMode`:** `'octopus'` | `'midi'` | `'disconnected'`

**Golden rule — outbound firewall:** the **parameter** path goes through `txParam(cmd, val)`:

- `octopus` → `_txSysexOctopus(cmd, val)` — gated by `_octopusSysexAllowed()` (the real firewall)
- `midi` → `_txMidiMapped(cmd, val)` — **a deliberate no-op stub**. Octopus SysEx params are *not* re-mapped to CC; they are simply dropped in MIDI mode.

Standard MIDI traffic (the only thing that should leave the port in MIDI mode) does **not** flow through `txParam`. It uses dedicated helpers, each self-gated with `if (this._appMode !== 'midi') return`:

- Notes → `_midiNoteOn` / `_midiScheduleNoteOff`
- CC → `_midiSendCc` · Program Change → `_midiSendProgramChange`
- Transport / clock bytes → `_midiSendRealtime` (`0xFA` / `0xFC` / `0xF8`)

So the two outbound worlds never share a code path: SysEx can only leave via `_txSysexOctopus` (Octopus mode only), and standard MIDI can only leave via the `_midi*` helpers (MIDI mode only).

---

## Mode boundary — full reload is intentional

Switching between Octopus and MIDI (either direction), and reconnecting after an NVS save/reset, performs a **full page reload** (`_requestPortBootReload` → `PORT_BOOT_KEY` in `sessionStorage`). This is by design, not a bug:

- It guarantees no Octopus lifecycle state (sync burst, slot cache, persist modals, heartbeat) survives into a MIDI session, and vice-versa.
- The reload re-runs `init()` → `setupMIDI()` → `onConnect()`, which now branches into **`_onConnectOctopus()`** or **`_onConnectMidi()`** so the two connect paths share only the inbound/port-open preamble — never the Octopus-only setup.
- **Same mode, same class of port** → no reload; `_adoptMidiPort()` runs the lightweight path.
- When an ★ Octopus port appears **during a live MIDI session**, the App now **asks** (`_offerOctopusSwitch`) before reloading into Octopus, instead of hijacking the session silently. Declining keeps MIDI mode; the user can still switch from the port menu.

**Octopus DOM is lazy:** the five Octopus knob panels are built by `_ensureOctopusKnobs()` on first entry into the Octopus shell (`setAppMode('octopus' | 'disconnected')` or the boot fallback), so a MIDI-only session never builds them.

---

## Layout map

### Octopus mode (unchanged)

| Tab | View ID |
|-----|---------|
| INSTRUMENTS | `#view-synths` |
| MIXER | `#view-mixer` |
| SEQUENCER | `#view-seq` |

### MIDI controller mode

| Tab | View ID | Content |
|-----|---------|---------|
| INSTRUMENTS | `#view-midi-inst` | Seq synth MIDI panel + seq activity canvas · drum machine + drum scope canvas |
| SEQUENCER | `#view-seq` (shared) | Same grid; local clock drives playhead |

**CSS:** `<body data-app-mode="octopus|midi">` — hide `.octopus-only` / `.midi-only` panels.

**Header (MIDI):** port dropdown, badge **MIDI OUT**, active transport, editable BPM, CPY/PST/RND/CLR, HELP.  
**Hidden:** SAVE, LOAD, RESET, SLOTS, DBEAM, CPU, master FX, Octopus persist modals.

---

## MIDI mapping (MVP)

| Source | Outbound |
|--------|----------|
| Grid rows 0–7 | Note on/off on **Melody channel** (scale + octave + transpose) |
| Grid rows 8–15 | Note on/off on **Drum channel** (GM map: 36 kick, 38 snare, …) |
| INSTRUMENTS PC | `0xC0` + program per engine |
| INSTRUMENTS CC knobs | `0xB0` + CC + value (defaults: 7 vol, 74 cutoff, 71 res, 1 mod…) |
| Play | Local clock + optional `0xFA` Start |
| Stop | All notes off + optional `0xFC` Stop |
| BPM | Local timer only; optional `0xF8` clock (CLK toggle) |

Settings stored under `localStorage` key `octopusapp_midi_session_v1`.

---

## Build phases (execute in order)

Each phase ends with a **verification checklist** before starting the next.

### Phase 0 — Documentation & scaffolding *(this document)*

**Deliverables**

- [x] `docs/midi_controller_mode.md` (this file)
- [x] README + CHANGELOG + user manual + product site copy
- [x] Dual Help modal in `OctopusApp.html` (Octopus | MIDI Controller tabs)
- [x] `todo.md` phase checklist

**Verify:** Docs readable; Help MIDI tab opens manually; no runtime behaviour change.

---

### Phase 1 — Mode flag + shell swap ✅

**Deliverables**

- [x] `app._appMode` + `setAppMode()` + `data-app-mode` on `<body>`
- [x] Connection → octopus vs midi vs disconnected
- [x] CSS `.octopus-only` / `.midi-only`; MIXER tab hidden in MIDI mode
- [x] Badge Octopus ON / MIDI OUT / Octopus Off
- [x] `onConnect()` skips `APP_SYNC_REQ` + heartbeat in MIDI mode
- [x] `_requireOctopusHardware()`; `txSysex` / `_parseDeviceSysex` guarded
- [x] `#view-midi-inst` placeholder; `switchView()` 2-tab remap
- [x] Help auto-tab by mode

**Verify:** Octopus plug-in = identical v6.1 shell; generic port = 2-tab MIDI layout, no SysEx in MON.

---

### Phase 2 — Outbound router + connection safety ✅

**Deliverables**

- [x] `txParam(cmd, val)` — octopus → `_txSysexOctopus`; midi → `_txMidiMapped` (a permanent no-op stub — standard MIDI uses the `_midi*` helpers, see "Golden rule" above)
- [x] `txSysex` / `txSysexSoon` alias `txParam`
- [x] `allNotesOff(port)` — CC 120/123 all channels; port change, mode exit, `beforeunload`
- [x] Heartbeat beat + `_checkTransportHealth` + tab-focus resync octopus-only
- [x] `_txGridRow`, slot name blobs, `_armPersist`, session APIs octopus-gated

**Verify:** MIDI mode MON shows zero `→` SysEx; switching ports does not leave hung notes on prior interface.

---

### Phase 3 — Local transport + playhead clock ✅

**Deliverables**

- [x] `_applyTransportChrome()` — BPM editable; play/stop/rec active in MIDI mode
- [x] `_midiClockTick()` on a 4 ms `setInterval` (`_startMidiClock`/`_stopMidiClock`) — 16th-note steps at BPM, drives `visStep`; `animateVU` only paints
- [x] Same `_paintPlayhead()` overlay + `_updateBeatLeds()` as Octopus mode
- [x] MIDI Start `0xFA` / Stop `0xFC` + `allNotesOff` on stop
- [x] Incoming STEP_SYNC/TRANSPORT/BPM ignored in MIDI mode

**Verify:** MIDI port → Play → cyan bar advances at BPM; Stop holds position; Octopus mode still STEP_SYNC-driven.

---

### Phase 4 — Sequencer → MIDI notes ✅

**Files:** `OctopusApp.html`

**Tasks**

1. `setCell()` / grid playback: on step advance, fire note on for active cells; note off at step boundary (50% gate default).
2. Pitch from scale + row + octave + transpose (`grid row 0 = lowest scale degree`).
3. Drum rows → GM note map (configurable in phase 6).
4. `toggleCell` updates local grid only (no `CMD.GRID_TOG`) in MIDI mode.
5. CPY/PST/RND/CLR remain local-only.

**Deliverables**

- [x] `SCALE_MIDI_NOTES` + `GM_DRUM_MAP` (mirrors `patches.h` / `midi.h`)
- [x] `_midiPlayStep()` on clock advance + on transport start
- [x] 50% gate via `_midiScheduleNoteOff`; cleared on stop / mode exit
- [x] Melody pitch: `SCALES_NOTES[scale][7-row] + transpose + octave×12`
- [x] `#midiMelodyCh` / `#midiDrumCh` / `#midiScale` in MIDI utility bar
- [x] `setCell`, `pasteGrid`, `randNotes` — no grid SysEx in MIDI mode

**Verify**

- External synth/drum machine receives notes in time with playhead.
- Stop sends all notes off.
- Octopus grid still sends SysEx when linked.

---

### Phase 5 — MIDI INSTRUMENTS view ✅

**Files:** `OctopusApp.html` (new `#view-midi-inst`)

**Tasks**

1. Build `#view-midi-inst` DOM:
   - **Left:** Seq synth — channel, PC, 8× CC knobs, octave, scale, **seq activity canvas** (GPU-isolated).
   - **Right:** Drum machine — channel, per-row note, PC optional, **reuse drum-scope canvas** (move or clone host from mixer).
2. `buildKnobs()` skipped for Octopus panels in MIDI mode; build `MIDI_KNOBS` CC config instead.
3. CC/PC changes call `_midiSendCc` / `_midiSendPc` only.
4. Activity canvases pulse on outbound note/CC (cosmetic).

**Deliverables**

- [x] Split INSTRUMENTS layout: seq synth MIDI + drum machine MIDI panels
- [x] `MIDI_KNOBS` — 8 CCs (VOL/EXPR/MOD/PAN/CUT/RES/ATK/REL) via `buildMidiKnobs()`
- [x] `_midiSendCc` / `_midiSendProgramChange`; `midiMap` state (PC, CC, drum notes)
- [x] Per-row GM drum note editors (8 rows); synced with `_midiDrumNoteForRow()`
- [x] `_midiSeqScope` + `_midiDrumScope` — GPU-isolated activity canvases (mixer scope unchanged)
- [x] Channel/scale/octave synced between utility bar and INSTRUMENTS panel

**Verify**

- INSTRUMENTS tab shows only MIDI panels in MIDI mode.
- Drum scope does not reflow mixer (compositor `contain` / `will-change` preserved).

---

### Phase 6 — Settings persistence + polish ✅

**Files:** `OctopusApp.html`

**Tasks**

1. Save/load `localStorage`: channels, PC, CC map, drum note map, last pattern banks.
2. Optional “Export pattern” JSON (stretch).
3. MIDI clock out toggle (24 PPQN) in routing bar.
4. Version bump **OctopusApp v6.2.00** in header + Help.
5. Update CHANGELOG [6.2.00] shipped; mark this plan complete.

**Deliverables**

- [x] `MIDI_SESSION_KEY` (`octopusapp_midi_session_v1`) — separate from Octopus slot cache
- [x] Debounced `_persistMidiSession()` on grid/map/routing edits + `beforeunload`
- [x] **EXP / IMP** JSON export/import in MIDI routing bar
- [x] **CLK** toggle — 24 PPQN `0xF8` while playing
- [x] **OctopusApp v6.2.00** — `APP_VERSION` constant, title, Help

**Verify**

- Reload browser restores MIDI-mode pattern + maps.
- Octopus session unaffected (separate storage keys).

---

### Phase 7 — Docs & site final pass ✅

**Tasks**

- User manual §9.4 marked **shipped** (remove “planned”).
- `octopus_web.html` badge “New in v6.2”.
- `code_info.h` §9 — move item from future to shipped note in App section.

**Deliverables**

- [x] `user_manual.md` §9.4–§9.5 — beginner guides (macOS/Windows Chrome, DAW routing)
- [x] §10.4 workflow · §11 MIDI troubleshooting
- [x] `OctopusApp.html` Help → **MIDI CONTROLLER** tab (full beginner text)
- [x] `octopus_web.html` — “New in v6.2.00” + setup cards
- [x] `README.md` · `code_info.h` §5 — shipped notes

**Plan status:** Phases 0–7 complete · **production-ready** — deploy per [DEPLOYMENT.md](../DEPLOYMENT.md).

---

## Production deployment

1. Upload `OctopusApp.html` to **octopus.isystem.app** (HTTPS, `Cache-Control: no-cache` on HTML).
2. Upload `octopus_web.html` (+ assets) to **octopus-info.isystem.app**.
3. Run smoke tests in **DEPLOYMENT.md** §1 (Octopus ON regression + MIDI OUT full path).
4. Tag `octopusapp-v6.2.00` on GitHub when live.

**Not required for v6.2:** firmware flash, server-side MIDI, database.

---

## Files touched (summary)

| File | Phases |
|------|--------|
| `OctopusApp.html` | 1–6 |
| `docs/midi_controller_mode.md` | 0, 7 |
| `README.md` | 0, 7 |
| `CHANGELOG.md` | 0, 6, 7 |
| `user_manual.md` | 0, 7 |
| `octopus_web.html` | 0, 7 |
| `todo.md` | 0–7 |
| `code_info.h` | 7 (App note only) |

**Firmware:** none.

---

## Regression guardrails (every phase)

1. Default mode with Octopus USB = **identical** to v6.1.00.
2. Never call `settings_*` or NVS concepts from MIDI mode.
3. Playhead paint path = single `_paintPlayhead()` function.
4. No new dependencies; stay single-file App deploy.
5. Field-test on Windows Chrome/Edge (one tab rule documented).

---

## Post-ship hardening (separation audit follow-up)

Architecture remained sound after ship; these close the lifecycle/hygiene gaps found in the separation audit:

1. **`onConnect()` split** — `_onConnectOctopus()` / `_onConnectMidi()`. The shared preamble does only the inbound/link reset + port open; the Octopus-only lifecycle (`_loadSlotMeta`, persist-modal cleanup, sync burst, `_resetSlotSavedFlagsForSync`, `APP_SYNC_REQ`, heartbeat) lives in the Octopus branch and never runs in MIDI mode.
2. **GPU fix — frozen MIDI scopes.** `_syncBurstExpected` was armed unconditionally in `onConnect()`. In MIDI mode `_parseDeviceSysex()` returns early, so the RX queue never drains and the flag stayed `true` forever — and `animateVU`'s `gpuBusy = _syncBurstActive || _syncBurstExpected` gate then froze the activity scopes. The flag is now armed only when the port is Octopus (`_syncBurstExpected = oct`).
3. **Defense-in-depth setter guards** — `setPlayMode`, `toggleMute`, `toggleDbeam`, `setDrumWave`, `setDrumKit`, `toggleLaserShow`, `toggleMidiHue`, `setHarpWave`, `setSeqWave`, `setHarpOctave`, `setPbRange`, `setPbEnable` now early-return in MIDI mode, so a stray programmatic call can't mutate Octopus shadow state behind the hidden `.octopus-only` UI. (Shared setters — `setSeqOctave`, `setTranspose`, `setGlobalScale`, `loadSynthPat`, `loadDrumPat`, `randNotes` — keep their existing per-mode branches.)
4. **Lazy Octopus DOM** — `_ensureOctopusKnobs()` builds the five knob panels once, on first entry into the Octopus shell (`setAppMode` for `octopus`/`disconnected`, plus a boot fallback). A MIDI-only session never builds them.
5. **Confirm before hijack** — `_offerOctopusSwitch()` asks before reloading into Octopus when an ★ port appears during a live MIDI session (`_isActiveMidiSession()`); boot reconnect and the first-ever connect still auto-win.

---

## Help content locations

| Audience | Location |
|----------|----------|
| In-app | `OctopusApp.html` → Help modal → **MIDI CONTROLLER** tab |
| GitHub | This file + README + user manual §9.4 |
| Product site | [octopus-info.isystem.app](https://octopus-info.isystem.app) → **MIDI Mode** nav → `#midi-mode` |

**Optional support:** [PayPal donate](https://www.paypal.com/donate?hosted_button_id=KX7B76V37PED8) → `diodac.electronics@gmail.com`

---

*© DIODAC ELECTRONICS / iSystem — OctopusApp v6.2.06 MIDI Controller mode (shipped)*
