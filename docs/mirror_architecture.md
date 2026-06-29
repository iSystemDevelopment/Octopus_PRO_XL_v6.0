# Mirror architecture — LINK · TRANSPORT · PLAYHEAD (FROZEN)

**Status:** **Normative and frozen** — the only valid implementation for Octopus linked mode.  
**Scope:** `OctopusApp.html` (Octopus mode) + firmware mirror lane (`midi.cpp`, `groovebox.cpp`, `link.h`, `patches.h`).  
**Policy:** **Method changes forbidden.** Only **timing polish** is allowed (intervals, timeouts, dedup windows, queue budgets). Do not replace, merge, or “simplify” these three axes.

**Related:** `code_info.h` §5A · `docs/link_contract.md` (persist/reboot) · `docs/app_god_rules.md` (playhead invariants) · **`docs/playhead_policy_audit.md`** (DSP + MIDI verification matrix).

**MIDI Controller:** playhead uses `_midiSeq` + local clock — not firmware STEP_SYNC. See audit doc §3.2; this file remains authoritative for **DSP link/mirror** only.

---

## 0. Why three axes

Stress testing proved that mixing concerns causes rot:

| Symptom | Wrong coupling |
|---------|----------------|
| BPM alive, playhead frozen | Import queue flags blocking mirror paint |
| Green ON, mirror dead | Badge tied to BPM instead of session |
| Passive hardware play, mirror stops | `isAppConnected()` gate on STEP_SYNC while latch released |
| Playhead jitter on hover | Layout reads (`getBoundingClientRect`) during playback |

**Rule:** LINK, TRANSPORT, and PLAYHEAD are **independent**. Each has its own send path, receive path, and UI surface. Cross-gating is a bug.

```
┌─────────────────────┐  ┌──────────────────────┐  ┌─────────────────────┐
│  AXIS 1 — LINK      │  │  AXIS 2 — TRANSPORT  │  │  AXIS 3 — PLAYHEAD  │
├─────────────────────┤  ├──────────────────────┤  ├─────────────────────┤
│ Session + USB alive │  │ Shared play/stop/rec │  │ Discrete step bar   │
│ Badge ON / Off      │  │ HW SCALE + App ▶/■   │  │ STEP_SYNC → transform│
│ PING + BPM beacons  │  │ CMD_TRANSPORT echo │  │ Compositor-only play │
│ Orange = import only│  │ 600 ms supervisor    │  │ Never blocked by queue│
└─────────────────────┘  └──────────────────────┘  └─────────────────────┘
```

---

## 1. AXIS 1 — LINK (session + badge)

### 1.1 Purpose

Prove the USB SysEx pipe is open and the App session is established. **Link does not drive the playhead** and **does not prove mirror freshness** beyond “we are in session.”

### 1.2 Dual heartbeat (both required)

| Direction | Mechanism | Period | Refreshes |
|-----------|-----------|--------|-----------|
| App → device | `CMD_PING` (99) via `startHeartbeat()` | **~800 ms** | `lastWebSysexMs` → `isAppConnected()` |
| Device → App | `CMD_BPM` (97) + `CMD_PING` beacon + `CMD_CPU_LOAD` (164) via `sequencer_background_task` | **33 ms** (`LINK_FRAME_MS`) | App `_lastDeviceRxMs`, tempo box, CPU box |

**Hard rules**

- Never `stopHeartbeat()` during save/reboot wait except when `midiOut` is physically gone.
- Outbound PING on connect **before** `APP_SYNC_REQ`.
- `_linkGraceUntil` (12 s connect grace): absorb port blips; do not drop session mid-burst.
- Stale RX during import: **60 s** tolerance while `_syncBurstExpected`; **4.5 s** in normal LIVE.

### 1.3 Badge (two states + import)

| UI | Condition | Meaning |
|----|-----------|---------|
| **Octopus SYNC** (orange) | `_sessionImporting()` ≡ `_syncBurstExpected` | Initial/resync blob import — block SAVE only |
| **Octopus ON** (green) | `_connOnline` | Device echo seen; editing allowed |
| **Octopus Off** (grey) | else | No session |

**Rejected:** “Green = BPM only”, “Green = STEP_SYNC fresh”, `_hwMirrorLive()`, `_refreshLinkState()`, 33 ms link watch loops that repaint playhead.

### 1.4 Session latch (firmware)

- `g_appSessionLatched` set on `APP_SYNC_REQ`; **not** cleared on inbound idle.
- `txSysex()`: when latched → always `txSysexForce()` (mirror lane same class as BPM).
- `isAppConnected()` still uses recent App→device RX for **wire authority** on param echoes; mirror cmds use latch.

---

## 2. AXIS 2 — TRANSPORT (shared state)

### 2.1 Model — two switches, one light

Transport is **one shared state** on the device. Hardware and App are both valid toggles; firmware is authoritative; echoes converge both UIs.

```
Hardware SCALE ──► seq_start/stop ──► CMD_TRANSPORT echo ──► App _setTransport()
App ▶ / ■       ──► CMD_TRANSPORT 1/0 ──► seq_start/stop ──► echo ──► App _setTransport()
```

### 2.2 Firmware

| Event | Echo |
|-------|------|
| Play / stop | `CMD_TRANSPORT` 1 / 0 (+ `CMD_TRIG_MODE` where applicable) |
| Record arm | `CMD_TRANSPORT` 3 / 4 |
| BPM change | `CMD_BPM` (immediate + 33 ms beacon) |
| Safety net | Sync supervisor every **600 ms** re-asserts transport while `g_appSessionLatched \|\| isAppConnected()` |

Supervisor period **must stay above** the 500 ms identical-value dedup window in `txSysexForce`.

While App connected, hardware surface is locked: SCALE = play/stop, OC short = record, ENC = BPM (see `interface.cpp`).

### 2.3 App

| Path | Behavior |
|------|----------|
| **Priority RX** | `CMD_TRANSPORT`, `CMD_TRIG_MODE`, `CMD_BPM` → immediate `applyIncoming` (never queued) |
| **`play()` / `stop()`** | Octopus mode: `txParam(CMD.TRANSPORT, 1/0)` — **not** hint-only |
| **`_setTransport()`** | Single UI truth: play/stop/rec buttons + `isPlaying` / `isRecording` |
| **Play start** | Snapshot `_phLayout` **before** `isPlaying = true` |
| **Play stop** | `_phInvalidateLayout()`; remeasure on next paint |

**Rejected:** App transport as read-only reflectors; `CMD_TRANSPORT_AVAIL`; supervisor at 33 ms (overkill).

---

## 3. AXIS 3 — PLAYHEAD (discrete compositor)

### 3.1 Authority

| Mode | Clock |
|------|-------|
| **Octopus linked** | `CMD_STEP_SYNC` (105) from firmware — **only** source |
| **MIDI Controller** | Local `_startMidiClock()` — STEP_SYNC RX blocked |

Playhead is **discrete** (one column per 16th). No sub-cell PLL glide in production.

### 3.2 Firmware

- Each sequencer step: `seq_ext_push(CMD_STEP_SYNC, step)` → `sequencer_background_task` drains → `txSysex`.
- With session latched: mirror path uses `txSysexForce` (not dropped by `isAppConnected()` gate).
- `CMD_STEP_SYNC` exempt from 500 ms dedup.

### 3.3 App — compositor pipeline (frozen)

| Piece | Role |
|-------|------|
| `#seq-playhead-layer` | Dedicated GPU layer (`contain: strict`) |
| `#seq-playhead` | Single bar; `will-change: transform` |
| `_phLayout` | Cached stage metrics (`clientWidth`/`clientHeight` + CSS column math) |
| `_phApplyStep(s)` | **Only** paint entry |
| `_phMovePlayhead(x,y)` | `translate3d()` only |

**While `isPlaying`**

- Hot path: **transform only** — zero layout reads once `_phLayout` exists.
- One-shot bootstrap measure allowed if grid becomes visible mid-transport (tab switch).
- `repaintGrid()`, hover, header, P-page repaint: **must not** invalidate layout or remeasure.

**While stopped**

- Measure once → transform.
- Invalidate on: stop, grid rebuild, window resize, manual page change (when stopped).

**Page follow**

- `_followPlayheadPage()`: defer `repaintGrid()` to one rAF; do **not** invalidate column geometry (columns are page-independent).

### 3.4 Receive path (frozen)

```javascript
[CMD.STEP_SYNC]: v14 => {
    this.visStep = s;
    this._paintPlayhead(s);   // ALWAYS — never gated on _syncBurstActive
}
```

Priority RX via `_rxIsPriority()` — STEP_SYNC never enters `_rxQueue`.

`_syncBurstActive` (queue > 20): throttles **knob/grid drain budget only** — never blocks playhead.

### 3.4 Rejected playhead approaches

| Rejected | Why |
|----------|-----|
| Per-cell `classList` playhead | O(n) layout; hover jank |
| `getBoundingClientRect()` per step or per cell | Main-thread layout thrash |
| `_pllPositionTick()` sub-cell glide | rAF stutter under UI load |
| Gating paint on `_syncBurstActive` | Import ≠ mirror |
| `_recoverMirrorAfterLoad()` in link timer | Geometry during hover |
| `_phGeom` + `force` remeasure on every STEP_SYNC | Same failure mode |

---

## 4. Allowed changes (timing polish only)

| Knob | Location | Notes |
|------|----------|-------|
| `LINK_FRAME_MS` (33 ms) | `link.h` | Device beacon rate |
| Heartbeat 800 ms | `OctopusApp.html` `startHeartbeat` | App PING |
| Supervisor 600 ms | `groovebox.cpp` | Must remain > 500 ms dedup |
| `_rxQueue` budget 18/40 | `_drainRxQueue` | Import throughput |
| Stale 4500 / 60000 ms | `startHeartbeat` | Link drop tolerance |
| `LINK_CONNECT_GRACE_MS` 12000 | App | Connect grace |
| Dedup exempt list | `txSysexForce` | Mirror cmds only |

**Not allowed:** merging axes, new badge signals, replacing STEP_SYNC with BPM-derived interpolation, removing priority RX, making transport hint-only, reintroducing geometry reads on the play hot path.

---

## 5. Verification checklist

After any timing tweak, re-run:

1. Connect → orange **SYNC** → green **ON**; stays green **5+ min @ 240 BPM** with outbound PING.
2. Hardware play/stop only (no App clicks 10+ min) — playhead + transport buttons track.
3. App ▶/■ — hardware follows; no fight with SCALE.
4. Mouse over grid/header/P-pages during play — **no playhead jitter**.
5. Orange SYNC during import — playhead still paints if already playing.
6. CPU load ⚠ in header — playhead still moves (priority path).
7. App save without reboot; hardware SAVE reboots; link survives.

---

## 6. File map

| File | Responsibility |
|------|----------------|
| `OctopusApp.html` | Priority RX, heartbeat, badge, `_phApplyStep`, transport TX/RX |
| `midi.cpp` | `txSysex` latch → force; dedup exemptions |
| `groovebox.cpp` | STEP_SYNC push; 33 ms beacon; 600 ms supervisor |
| `link.h` | `g_appSessionLatched`, `LINK_FRAME_MS` |
| `patches.h` | `sendFullStateSync`, session latch helpers |
| `code_info.h` | §5A manifest (this doc in firmware tree) |

---

*Frozen 2026-06-25 — Octopus PRO XL v6.3 mirror lane. Method locked; timing polish permitted.*
