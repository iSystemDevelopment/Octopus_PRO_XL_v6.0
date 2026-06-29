# OctopusApp GOD Rules — Playhead, Reload, Mode Separation

**Status:** Playhead + mirror rules are **frozen** with [`docs/mirror_architecture.md`](mirror_architecture.md). **Full dual-shell verification:** [`docs/playhead_policy_audit.md`](playhead_policy_audit.md). Timing polish only — no method refactoring.

**App target:** v6.3 · **Firmware:** 6.1.01+  
**Scope:** `OctopusApp.html` — Octopus linked mode vs MIDI Controller mode.

---

## Verdict (deep audit)

### Playhead (GOD layer) — FROZEN METHOD

| Invariant | Implementation |
|-----------|----------------|
| Single paint entry | `_paintPlayhead(s, S)` → `_phApplyStep(S, s)` → `_phMovePlayhead()` (`translate3d`) |
| Layout cache | Per-shell `S._phLayout` — CSS math from stage `clientWidth`/`clientHeight` only |
| Playing hot path | **Transform only** — no `getBoundingClientRect`, no per-cell reads |
| Stopped path | One measure → transform; invalidate on stop / rebuild / resize |
| Clock (DSP) | `CMD_STEP_SYNC` priority RX on `_dspSeq` — **never** gated on `_syncBurstActive` |
| Clock (MIDI) | `_midiClockTick` on `_midiSeq` — 4 ms interval, not rAF |
| Layer (DSP) | `#seq-playhead-layer` + `#seq-playhead` |
| Layer (MIDI) | `#midi-seq-playhead-layer` + `#midi-seq-playhead` (same CSS policy) |
| Page cross | `_followPlayheadPage()` — one deferred rAF `repaintGrid()`; geometry unchanged |
| Hidden grid | `_phPendingStep` + flush in `_finishPostSyncIfReady` / `animateVU` |
| Discrete bar | No `_pllPositionTick()` — sub-cell glide **rejected** for production |

**Rejected permanently:** `_phGeom`, `_remeasurePhGeometry()`, hover rects, PLL glide, paint blocked by import queue.

### Transport — FROZEN METHOD

| Invariant | Implementation |
|-----------|----------------|
| Shared state | Hardware SCALE + App ▶/■ both use `CMD_TRANSPORT` |
| App TX | `play()` / `stop()` → `txParam(CMD.TRANSPORT, 1/0)` in Octopus mode |
| App RX | Priority path → `_setTransport()` |
| Layout at play edge | Measure `_phLayout` before `isPlaying = true`; invalidate on stop |
| Firmware heal | 600 ms supervisor re-asserts transport (see `groovebox.cpp`) |

**Rejected:** hint-only transport buttons, separate App-only clock, `_transportHint()` as the only App action.

### Link / badge — FROZEN METHOD

| Invariant | Implementation |
|-----------|----------------|
| App heartbeat | `startHeartbeat()` — PING ~800 ms |
| Device beacon | BPM + PING + CPU @ 33 ms |
| Orange SYNC | `_syncBurstExpected` only (`_sessionImporting()`) |
| Green ON | `_connOnline` (device echo seen) |
| Import vs mirror | `_syncBurstActive` throttles queue drain — **never** blocks STEP_SYNC |

---

## Clock authority

- **Octopus mode:** STEP_SYNC + device TRANSPORT/BPM only. App never starts `_startMidiClock()`.
- **MIDI mode:** `_startMidiClock()` (4 ms interval). App blocks STEP_SYNC/TRANSPORT/BPM RX in `applyIncoming()`.

---

## GOD invariants (must hold after every tweak)

1. **Three axes stay separate** — LINK, TRANSPORT, PLAYHEAD (see `mirror_architecture.md`).
2. **Playhead paint:** compositor transform only on the hot path; no per-step grid `classList`.
3. **No layout reads during play** except one-shot bootstrap if `_phLayout` missing when grid appears.
4. **STEP_SYNC:** always `_paintPlayhead(s, O)` on `_dspShell()` — never gated on `_syncBurstActive`.
5. **Priority RX:** STEP_SYNC, TRANSPORT, TRIG_MODE, BPM, PING, CPU_LOAD bypass `_rxQueue`.
6. **Reload triggers:** mode change, SAVE/RESET reboot after USB reconnect; EXP/IMP in-page (MIDI session only).

---

## Manual test fixtures

### Octopus + playhead + transport

- [ ] Connect ★ port → orange SYNC → green ON; 5+ min @ 240 BPM
- [ ] Hardware PLAY — playhead discrete; hover grid/headers — no stutter
- [ ] Hardware-only 10+ min — playhead + buttons still track
- [ ] App ▶/■ — hardware follows
- [ ] Step crosses 16 → auto page follow without hang
- [ ] SONG editor while playing → GRID → playhead at hardware step
- [ ] Orange SYNC import while playing — playhead still moves

### P1–P4

- [ ] LEN=64: all pages clickable; playhead follows; beyond-LEN dimmed

### Mode switch

- [ ] Octopus ★ → generic MIDI: full reload, no SysEx TX
- [ ] Back to ★: APP_SYNC pull

### MIDI Controller playhead (local clock)

See checklist **B** + **C** in [`playhead_policy_audit.md`](playhead_policy_audit.md).

---

*DSP mirror spec: `docs/mirror_architecture.md`. Full playhead audit (both shells): `docs/playhead_policy_audit.md`.*
