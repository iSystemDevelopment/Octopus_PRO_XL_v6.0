# OctopusApp GOD Rules — Playhead, Reload, Mode Separation

**App target:** v6.2.07 · **Firmware:** 6.1.01  
**Scope:** `OctopusApp.html` — Octopus linked mode vs MIDI Controller mode.

---

## Verdict (deep audit)

### Playhead (GOD layer)

| Finding | Severity | Status |
|---------|----------|--------|
| Playhead lived on `#seq-grid-stage` sibling to wrap but shared paint path with grid rebuilds | Medium | **Fixed** — `#seq-playhead-layer` dedicated compositor layer |
| `_remeasurePhGeometry()` read cell `getBoundingClientRect()` — hover/layout could shift Y | High | **Fixed** — pure CSS math from stage rect only |
| `_paintPlayhead()` called `setGridPage()` on page cross → synchronous `repaintGrid()` during STEP_SYNC | High | **Fixed** — `_followPlayheadPage()` defers grid repaint to one rAF |
| Sync burst ended without flushing `_phPendingStep` | Medium | **Fixed** — `_drainRxQueue()` paints pending step when burst clears |
| `_pllPositionTick()` sub-cell glide intentionally dead (discrete STEP_SYNC bar) | OK | Retained as reference only |
| Song editor hides grid stage — playhead correctly deferred via `_phPendingStep` | OK | **Hardened** — `_setSongView(false)` restores pending step |

**Clock authority**

- **Octopus mode:** STEP_SYNC + device TRANSPORT/BPM only. App never starts `_startMidiClock()`.
- **MIDI mode:** `_startMidiClock()` (4 ms interval, not rAF). App blocks STEP_SYNC/TRANSPORT/BPM RX in `applyIncoming()`. MIDI clock 0xF8 optional via CLK button.

### P1–P4 page buttons

| Finding | Severity | Status |
|---------|----------|--------|
| `_updatePageButtons()` set `pointer-events: none` on P2–P4 when `seqLen ≤ 16` | **Root cause of “dead P2–P4”** | **Fixed** — all four pages always clickable |
| Pages beyond LEN had no visual cue | Low | **Fixed** — `beyond-len` class in `repaintGrid()` (CSS existed, never applied) |
| P-page during song view | OK | `userSetGridPage()` exits song editor and opens SEQUENCER tab |

### LOAD / SAVE / RESET / reload contract

| Finding | Severity | Status |
|---------|----------|--------|
| No JS syntax error in inline script (loads not “broken” by parse error) | — | Verified structure OK |
| LOAD modal uses `loadScoped()` correctly (not dead `loadSavedSession()`) | OK | Modal wired |
| LOAD ACK stayed on-page but never reloaded → inconsistent shell vs NVS | High | **Fixed** — drain burst then `_requestPortBootReload()` |
| SETTINGS/MOTION RESET reloaded immediately, aborting inbound sync | High | **Fixed** — same post-sync reload as LOAD |
| SAVE / FULL RESET | OK | Reboot → `_reloadAfterReconnect` → port boot reload |
| Octopus ↔ MIDI port change | OK | `_requestPortBootReload()` |

**Post-sync reload sequence**

1. `_persistFinished(ok)` sets `_syncBurstExpected`, `_pendingPostSyncReload`.
2. RX queue drains grid/knob/song echoes (`_syncBurstActive` during large bursts).
3. When queue empty: flush playhead, log matrix sync, stash port → `location.reload()`.
4. `PORT_BOOT_KEY` in sessionStorage → auto-reconnect → `APP_SYNC_REQ` → full blob import.

### Song ↔ pattern toggle

| Finding | Status |
|---------|--------|
| Song editor defers DOM via `_songEditorDirty` rAF | OK |
| Chain loop (🔗) in MIDI mode uses local `_midiAdvanceSongChain()` | OK |
| Returning to GRID remeasures playhead | **Hardened** |

### MIDI ARP

| Finding | Status |
|---------|--------|
| Octopus SEQ/HARP ARP via SysEx only — hidden in MIDI shell | OK (separation) |
| No outbound MIDI arp for melody rows | **Gap** | **Fixed** — utility bar ARP + PAT/RATE/GATE; persists in session JSON |

### Mode leaks (checklist)

- [ ] Octopus mode: zero `_startMidiClock`, zero `_loadMidiSession` on connect
- [ ] MIDI mode: zero `txSysex` → device (except port picker), STEP_SYNC blocked
- [ ] `_requireOctopusHardware()` gates SAVE/LOAD/RESET
- [ ] `setPlayMode(..., tx=true)` no-op in MIDI mode

---

## GOD invariants (must hold after every tweak)

1. **Playhead paint path:** `_paintPlayhead()` → `_applyPhTransform(translate3d)` only; no per-step `classList` on grid cells.
2. **No layout reads during play** except stage rect on resize/page change (never per-cell hover rects).
3. **GPU isolation:** `#seq-playhead-layer` with `contain: strict`, `will-change: transform` on `#seq-playhead`.
4. **Sync burst:** STEP_SYNC sets `_phPendingStep` while `_syncBurstActive`; paint after drain. `_finishPostSyncIfReady()` triggers reload after LOAD / SETTINGS·MOTION RESET.
5. **Reload triggers:** mode change, SAVE reboot, LOAD/SETTINGS/MOTION RESET after sync drain, EXP/IMP uses in-page apply (MIDI session only).
6. **Re-verify compositor** after any CSS/DOM change touching `#seq-grid-stage`, `#seq-playhead-layer`, `#seq-playhead`, MIXER drum scope, MIDI activity scopes, or `animateVU`.

### P-page grid tools (v6.2.07)

| Tool | Scope |
|------|--------|
| CPY / PST | Active P page (16 steps) within active bank |
| CLR | Active P page — Octopus tx one page of grid-row blobs only |
| RND-H / RND-D | Active P page |
| MELODY / DRUM patterns | Active P page (16 steps) |

Octopus **SEQ SETUP → Clear** on hardware remains a full wipe — not the header **CLR** button.

---

## Manual test fixtures

### P-page grid tools

- [ ] LEN=64: CPY on P2 → switch P3 → PST → P2 unchanged, P3 matches clipboard
- [ ] CLR on P2 only — P1/P3/P4 cells unchanged; sounds unchanged

### Octopus + playhead

- [ ] Connect ★ port, SEQUENCER tab, press PLAY on hardware
- [ ] Move mouse over grid, headers, P1–P4, banks — playhead stays discrete, no stutter
- [ ] Step crosses 16 → auto page follow without hang
- [ ] SONG editor open while playing → switch GRID → playhead at hardware step

### P1–P4

- [ ] LEN=16: P2–P4 clickable, steps dimmed beyond LEN
- [ ] LEN=64: all pages active, playhead follows

### LOAD / RESET

- [ ] LOAD FULL → wait → page reload → matrix/knobs match hardware
- [ ] RESET MOTION (no reboot) → sync → reload → motion cleared in UI
- [ ] SAVE FULL → device reboot → app reload → state intact

### MIDI mode

- [ ] Play + CLK out → DAW receives 0xF8
- [ ] ARP ON, two synth rows on step → arpeggiated melody notes out
- [ ] 🔗 song chain + P2 page during playback
- [ ] EXP → reload → IMP → state restored

### Mode switch

- [ ] Octopus ★ → generic MIDI port: full reload, MIDI shell, no SysEx TX
- [ ] Back to ★: reload, Octopus shell, APP_SYNC pull

---

## Remaining / future

- Hardware-initiated LOAD (device menu) still syncs on-page without forced reload (by design).
- MIDI ARP: no HARP-style poly arp (seq rows 0–7 only); drums unchanged.
- Optional: re-enable compositor glide via `_pllPositionTick` only if tied to hardware BPM PLL (currently disabled).
