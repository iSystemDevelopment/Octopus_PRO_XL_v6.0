# Playhead policy — full verification audit (DSP + MIDI shells)

**Version:** OctopusApp v6.6.01 · dual `SeqShell` architecture  
**Authority:** This document is the **playhead-specific** audit companion to [`mirror_architecture.md`](mirror_architecture.md) (DSP mirror) and [`two_mode_separation.md`](two_mode_separation.md) (shell isolation).

---

## 1. Policy statement

| Rule | DSP Engine | MIDI Controller |
|------|------------|-----------------|
| **Clock authority** | Firmware `CMD.STEP_SYNC` (105) only | App `_midiClockTick()` only |
| **Never cross** | App must not run `_startMidiClock()` | App must ignore `STEP_SYNC` / HW transport RX |
| **DOM** | `#seq-playhead` in `#seq-playhead-layer` | `#midi-seq-playhead` in `#midi-seq-playhead-layer` |
| **State object** | `app._dspSeq` (`visStep`, `_phLayout`, …) | `app._midiSeq` (separate copy) |
| **Paint API** | `_paintPlayhead(s, O)` | `_paintPlayhead(s, M)` |
| **Algorithm** | **Same** `_phApplyStep(S, s)` — compositor `translate3d` only |

**Shared (by design):** global `isPlaying`, `isRecording`, `bpm` (transport bar).  
**Not shared:** `visStep`, grid, playhead element, layout cache, pending step queue.

---

## 2. Architecture (frozen axes)

```
                    ┌─────────────────────────────────────┐
                    │  TRANSPORT (shared isPlaying/bpm)   │
                    └──────────────┬──────────────────────┘
                                   │
          ┌────────────────────────┼────────────────────────┐
          ▼                        ▼                        ▼
    DSP: HW echo            MIDI: local              LINK: badge only
    STEP_SYNC → O           _midiClockTick → M       (no playhead)
          │                        │
          └────────────┬───────────┘
                       ▼
              _phApplyStep(S, s)  ← one implementation, S selects shell
                       │
              translate3d on S._playheadEl
```

**Rejected forever:** per-cell CSS playhead, BPM-interpolated bar, geometry read on every STEP_SYNC while playing, gating STEP_SYNC on `_syncBurstActive`.

---

## 3. Per-shell inventory

### 3.1 DSP Engine (`_dspSeq` / `SEQ_IDS_DSP`)

| Item | Value |
|------|--------|
| View gate | `#view-seq.active` + not `showSongEditor` |
| Stage | `#seq-grid-stage` |
| Playhead id | `seq-playhead` |
| Step source | `[CMD.STEP_SYNC]` → `O.visStep = s` → `_paintPlayhead(s, O)` |
| RX path | Priority — **never** `_rxQueue` |
| Page follow | Auto unless `O._gridPageUserLock` |
| Beat LEDs | `O.ids.led(i)` |
| Notes fired | Firmware (App read-only for scope/VU) |

### 3.2 MIDI Controller (`_midiSeq` / `SEQ_IDS_MIDI`)

| Item | Value |
|------|--------|
| View gate | `#view-midi-seq.active` + not `showSongEditor` |
| Stage | `#midi-seq-grid-stage` |
| Playhead id | `midi-seq-playhead` |
| Step source | `_midiClockTick` → `M.visStep++` → `_paintPlayhead(s, M)` |
| Clock | `setInterval(4 ms)` — **not** rAF (hover-safe) |
| Step period | `_midiStepMs()` = `60000 / (bpm × 4)` (16th notes) |
| Page follow | Same `_followPlayheadPage(M, …)` |
| Beat LEDs | `midi-led-0` … `midi-led-3` |
| Notes fired | `_midiPlayStep(s)` same tick as playhead advance |

---

## 4. Paint pipeline (both shells)

```
Entry points:
  DSP  — STEP_SYNC dispatch, stop transport, page switch rAF, animateVU pending drain
  MIDI — _midiClockTick, play start, stop transport, page switch rAF, animateVU pending drain

_phApplyStep(S, s):
  1. Clamp s to [0, seqLen-1]
  2. If ! _seqGridVisible(S) → S._phPendingStep = s (defer)
  3. If page ≠ gridPageIdx → follow (repaint grid, even while playing) or lock → defer/hide
  4. If playing:
       — if layout cache absent (first grid reveal mid-play) → measure ONCE
         (bootstrap edge case); only defer if still unmeasurable (grid 0-size)
       — transform only when stepChanged (no remeasure hot path)
  5. If stopped → measure once, show bar without ph-playing

_phMeasureLayout(S):
  — clientWidth/clientHeight only (no getBoundingClientRect)
  — skipped while playing if layout cache valid (±12 px tolerance)
  — bootstrap-only while playing: measures only when NO valid cache exists; it
    never invalidates a live layout (that is still _phInvalidateLayout, stop-gated)

animateVU (every rAF):
  — For EACH shell, ONLY when stopped: flush S._phPendingStep if grid visible
  — While playing the per-step source (STEP_SYNC / _midiClockTick) paints and the
    §4 step-4 bootstrap measure self-heals layout on first reveal — no rAF drain needed
  — Does NOT advance MIDI clock (clock is setInterval)
```

### 4.1 Grid repaint during playback (v6.6.01+ — both shells)

The grid **cells** (not the playhead) follow these rules while `isPlaying`:

| Trigger | Behavior while playing | Why |
|---------|------------------------|-----|
| User clicks a cell | DOM toggled directly in `setCell` (independent of `repaintGrid`) | clicked cell lights instantly |
| Auto P-page follow (`_followPlayheadPage`) | **repaints** the new page's cells (per-page-crossing, not per-step) | bar + cells must agree (A7) |
| Remote/echoed grid edit (DSP only, sub-0x05) | repaint **deferred**: sets `O._gridModelDirty`, no DOM write | protect the playhead hot path |
| Stop (`_setTransport`) | reconciles `O._gridModelDirty` → one `repaintGrid` | echoed edits made during play appear |

**Invariant:** `_gridModelDirty` is **kept set** while playing (never cleared without a repaint), so deferred echoed edits are not lost — they reconcile exactly once on stop. The MIDI shell has no echo path, so only the user-click and page-follow rows apply to it.

---

## 5. Mode guards (must not regress)

| Guard | Location | Effect |
|-------|----------|--------|
| MIDI blocks HW step | `applyIncoming` | `STEP_SYNC`, `TRANSPORT`, `BPM`, `SONG_POS` return early |
| DSP ignores MIDI clock | `_startMidiClock` / `_midiClockTick` | `_appMode !== 'midi'` |
| STEP_SYNC → DSP only | `_buildDispatch` `[CMD.STEP_SYNC]` | uses `_dspShell()` only |
| Priority RX | `_rxIsPriority` + `_enqueueIncoming` | STEP_SYNC never queued |
| Burst throttle | `_drainRxQueue` budget | **Does not** block STEP_SYNC |
| Shell param required | `repaintGrid(S)`, `buildSequencer(S)` | no root `app.gridData` |

---

## 6. Full verification checklist

### A. DSP Engine — hardware mirror

- [ ] **A1** Connect ★ Octopus → orange **DSP SYNC** → green **DSP ON** while playing.
- [ ] **A2** SEQUENCER tab + PLAY (hardware or App) → cyan bar moves **discrete** step-to-step (not smooth scroll).
- [ ] **A3** MONITOR (filter on): `STEP_SYNC` arrives at step rate; playhead stays aligned with hardware OLED step bar.
- [ ] **A4** Hover grid / headers / knobs 30 s while playing → **no playhead jitter**.
- [ ] **A5** Orange SYNC import burst (queue > 20) while playing → playhead **still moves**.
- [ ] **A6** CPU ⚠ in header while playing → playhead **still moves**.
- [ ] **A7** LEN=64, all P1–P4 clickable; playhead auto-follows page when lock off.
- [ ] **A8** Manual P-page click (lock on) → playhead hidden on grid until return to playing page; `visStep` still tracks in `O.visStep`.
- [ ] **A9** EDIT song editor → playhead deferred (`_seqGridVisible` false); return to grid → bar at `O.visStep`.
- [ ] **A10** Stop → bar rests at stopped step; layout cache invalidated.
- [ ] **A11** Window resize while **stopped** → bar repositions; while **playing** → no layout thrash.
- [ ] **A12** Mixer drum scope + VU follow `O.visStep` only (read-only).

### B. MIDI Controller — local clock

- [ ] **B1** Generic MIDI port → **MIDI OUT** badge; pulpit visible.
- [ ] **B2** ▶ Play → cyan bar on **right 33% grid** advances at BPM (16th steps).
- [ ] **B3** `_midiPlayStep` — notes/MIDI out align with bar step (no ahead/behind by >1 step).
- [ ] **B4** ■ Stop → bar holds; `M.visStep` unchanged; clock timer cleared.
- [ ] **B5** BPM change while playing → step period updates (no stale 120 BPM drift).
- [ ] **B6** Hover CC knobs / drum map while playing → playhead **steady** (4 ms timer, not rAF).
- [ ] **B7** Song mode 🔗 + chain → bank switch at chain boundary; playhead + `M.visStep` reset per `_midiResetSongPlayback`.
- [ ] **B8** EDIT song editor → playhead deferred; grid return → bar at `M.visStep`.
- [ ] **B9** P-page lock same as DSP policy on **M** shell.
- [ ] **B10** Unplug MIDI → DSP INSTRUMENTS; **M.visStep** preserved in memory; **O.visStep** unaffected.

### C. Cross-shell isolation

- [ ] **C1** Edit MIDI bank A → connect ★ DSP → DSP bank A **unchanged** (different `gridData`).
- [ ] **C2** DSP playing on hardware → switch impossible without port change; port change reloads shell.
- [ ] **C3** After MIDI session, DSP playhead uses **O.visStep** from STEP_SYNC, not `M.visStep`.
- [ ] **C4** `animateVU` pending drain: only visible shell paints; inactive shell pending cleared when its view shown.
- [ ] **C5** Both playheads use identical CSS (`#seq-playhead`, `#midi-seq-playhead`).

### D. Regression traps (fail if observed)

| Symptom | Likely cause |
|---------|----------------|
| BPM moves, bar frozen | STEP_SYNC gated on burst/import |
| Bar smooth-slides between steps | BPM interpolation (forbidden) |
| Hover stutter (DSP) | cell classList playhead |
| Hover stutter (MIDI) | clock moved to rAF only |
| MIDI bar invisible | missing playhead CSS on `midi-seq-playhead` |
| Wrong grid lights with bar | shared `gridData` or wrong `S` in `repaintGrid` |
| D4–D5 only row labels | `_midiMelodyNoteForRow` reading `dspSeqTrn` |
| Bar blank after revealing grid mid-play (A9/B8) | bootstrap measure removed from `_phApplyStep` **playing** branch (must measure once when layout cache absent) |
| Grid cells stuck on old page while bar moves (A7) | `_followPlayheadPage` rAF repaint gated on `!isPlaying` (must repaint on page-follow even while playing) |
| Echoed hardware grid edits vanish during play (DSP) | `_gridModelDirty` cleared without repaint while playing; must stay set and reconcile on stop |

---

## 7. Code map (`OctopusApp.html`)

| Symbol | Role |
|--------|------|
| `createSeqShell` | Per-shell playhead + grid state factory |
| `_phApplyStep` / `_paintPlayhead` | Single paint implementation |
| `_seqGridVisible(S)` | View + song-editor gate |
| `_followPlayheadPage` | Auto P-page + rAF grid repaint (repaints even while playing) |
| `_rxIsPriority` | STEP_SYNC bypass queue |
| `[CMD.STEP_SYNC]` dispatch | DSP `visStep` + paint |
| `_midiClockTick` | MIDI `visStep` + paint + notes |
| `_setTransport` | Start/stop clocks; invalidate both layouts on stop |
| `animateVU` | Pending step drain for **both** shells — **stopped only** (per-step source paints while playing) |

---

## 8. Related docs

- [`mirror_architecture.md`](mirror_architecture.md) — link / transport / playhead frozen method (DSP)
- [`app_god_rules.md`](app_god_rules.md) — invariant checklist (§ playhead)
- [`shell_dsp_engine_sitemap.md`](shell_dsp_engine_sitemap.md) — DSP DOM ids
- [`shell_midi_controller_sitemap.md`](shell_midi_controller_sitemap.md) — MIDI DOM ids (Production Ready)
- [`transport_mirror.md`](transport_mirror.md) — pointer to mirror_architecture (deprecated detail)

---

## 9. Sign-off

| Shell | Policy owner | Verification |
|-------|--------------|--------------|
| Octopus DSP Engine | Firmware STEP_SYNC | Checklist **A** + **C** |
| Octopus MIDI Controller | App `_midiClockTick` | Checklist **B** + **C** |

**Both shells share `_phApplyStep` only — never share clock, DOM node, or `visStep`.**

---

## 10. Connection method — DSP Engine hardware handshake

### 10.1 Idle state (no Octopus connected)

`body[data-conn-state="disconnected"]` → **INSTRUMENTS IDLE** overlay shows.  
All instrument panels are dimmed (`opacity: 0.18`, `pointer-events: none`).  
The idle overlay polls detected ★ port names and updates `#idle-port-hint`.

### 10.2 First interaction — Ctrl+Shift+R equivalent

When a ★ Octopus port is detected for the **first time in a browser session**:

1. `_onConnectOctopus()` calls `_maybeFirstConnectionRefresh(port)`.
2. Guard key `oct_first_connect_v1` is checked in `sessionStorage`.
3. If **not set**: key is written, port ID is stashed via `_stashPortBoot(portId, 'dsp')`, and `_scheduleAppReload()` fires a `location.reload()` (≡ Ctrl+Shift+R).
4. On the reloaded page: `_readPortBoot()` finds the stashed port → auto-connects directly to `_onConnectOctopus()` without re-triggering the refresh guard (key is already set).
5. Normal sync sequence resumes: `CMD.PING` → `CMD.APP_SYNC_REQ` → orange **SYNC** badge → green **ON**.

**Why:** WebMIDI handles can become stale if the tab was previously connected to a different port or the device rebooted. The hard-refresh ensures the browser re-enumerates ports from a clean state — matching the Ctrl+Shift+R recommendation in the CONNECTION & SYNC help text.

### 10.3 Auto-connect sequence (after hard-refresh or fresh load)

```
WebMIDI access granted
  └─ _pickAutoOutput() → ★ port found?
       ├─ YES → _connectMidiPort() → _onConnectOctopus()
       │          → setAppMode('dsp') [conn-state = 'dsp']
       │          → CMD.PING + CMD.APP_SYNC_REQ
       │          → orange SYNC badge (burst active)
       │          → green ON badge (first STEP_SYNC received)
       └─ NO  → setAppMode('disconnected') [conn-state = 'disconnected']
                → INSTRUMENTS IDLE overlay shown
                → _startReconnectPoll() watches for ★ port arrival
```

### 10.4 Disconnect → IDLE

Any of the following triggers `setAppMode('disconnected')`:

| Trigger | Path |
|---------|------|
| USB unplug | `onstatechange` → `_octopusFallbackToMidi()` → `setAppMode('disconnected')` |
| Heartbeat timeout | `_octopusHeartbeatTick()` → fallback → `setAppMode('disconnected')` |
| Manual port deselect | `_connectMidiPort()` with no live port |

On disconnect: `body[data-conn-state="disconnected"]` is set → idle overlay appears, instrument panels dim, reconnect poll starts.

### 10.5 Invariants

- `_maybeFirstConnectionRefresh` must **never** be called in MIDI Controller mode.
- The `oct_first_connect_v1` key lives in `sessionStorage` (cleared on tab close); a new browser session always performs the hard-refresh.
- `data-conn-state` is set exclusively inside `setAppMode()` — no other function writes `document.body.dataset.connState`.
