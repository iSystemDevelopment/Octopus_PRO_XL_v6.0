# Octopus PRO XL v6.0 — Upgrade Roadmap

Single source of truth for the next round of work. We **plan & polish here first**,
then implement clean — no scattered visions. Edit freely; mark items as you decide.

## Ground rules / context already in place
- **Display page-diff flush** (`displayFlushDiff` in `display.cpp`) is live: the
  renderer draws whole frames into RAM, only changed 128-byte pages hit I2C.
  Any new UI element is automatically a cheap region update — no per-element code.
- **Play-mode switch** (POLY/STR/SOLO) is bidirectional firmware↔app (`CMD_PLAY_MODE`).
- USB-MIDI SysEx protocol: `0x7C` device→app, `0x7D` app→device. Command IDs in
  `sysex.h`; mirrored in `OctopusApp.html` `CMD{}` map. Next free ID = **193**.

---

## Active upgrade anchor — **v6.0.01**

**Production baseline today:** firmware + OctopusApp **6.0.00** (stable; field-tested rollback).

**Next release target:** **6.0.01** — **all new TODO items added after this anchor belong to v6.0.01** and ship together unless explicitly marked otherwise. Do not bump `SYSTEM_FW_VERSION`, `SETTINGS_VERSION`, or App title/help strings until the 6.0.01 bundle is tested and flashed.

Cross-reference: [CHANGELOG.md §6.0.01 (planned)](./CHANGELOG.md#601--2026-06-20-planned) · [README.md](./README.md) (stable 6.0.00 note).

### TODO — v6.0.01: App playhead mirror (PLL slaved tick clock)

**Problem (field test).** The hardware OLED **16-box playhead** (`DRAW_STEP_BARGRAPH` in
`display.cpp`) is sample-locked to the groovebox clock and is visually the most
accurate indicator — **master / valid**. The OctopusApp playhead is a **read-only
mirror** but today can feel jumpy, drift, or glitch when the USB/Web MIDI link is
busy. Users still expect **smooth** mirror motion even though transport and step
timing remain hardware-owned.

**Architecture (agreed).**

| Role | Owner |
|------|--------|
| Step clock, note triggers, OLED bar | **Hardware** (`seqCurrentStep` in audio task) |
| Cyan grid playhead in browser | **App** — visual mirror only, never the sequencer |

The App must **not** run an independent BPM sequencer or rely on heavy wrap/snap
heuristics. Instead: **hardware ticks, web interpolates between them** (PLL).

**Fix — phase-locked local `tick_clock()` (App-only MVP).**

1. **Anchor on hardware** (minimal SysEx — already in 6.0.00):
   - `CMD_STEP_SYNC` (105) each step change → store `{ anchorStep, anchorTime }`
   - `CMD_BPM` + transport (`CMD_TRANSPORT` play/stop/rec) → start/stop mirror;
     `stepMs = 60000 / bpm / 4` (16th notes)
   - `CMD_HW_S_LEN` / bank → wrap via math: `visualStep % len`, page = `step >> 4`

2. **One `tick_clock()` in rAF** while `isPlaying`:
   - `visualStep = anchorStep + (performance.now() - anchorTime) / stepMs`
   - Map `visualStep` → column (0–15) + grid page P1–P4
   - Position `#seq-playhead` via `transform: translateX(col * pitch)` — detached
     from grid DOM rebuilds (page follow repaints cells, not the overlay element)

3. **Re-anchor on each `STEP_SYNC`**:
   - Small drift (e.g. &lt;25 ms): update anchor quietly
   - Large drift / missed tick / step jump: hard snap to hardware step
   - STOP or link loss: hide playhead, stop clock

4. **Retire / simplify** in `OctopusApp.html`:
   - CSS `transition` glide as the **primary** clock (optional cosmetic only)
   - `_phPendingStep`-only drive when PLL clock is running
   - LEN=1 / pattern-wrap / `stepDelta > 1` edge cases → single modulo on
     `visualStep` instead of special-case handlers

5. **Optional firmware (6.0.01+ if PLL alone is not enough):**
   - Sub-step phase 0…1 in STEP_SYNC (or low-rate echo) for in-step glide when
     USB batches messages. **Not required for MVP** if re-anchor every step suffices.

6. **Acceptance tests:**
   - OLED 16-box bar and App cyan bar on same step at 60–180 BPM; LEN 16/32/64
   - SEQUENCER tab + MON: regular `← STEP_SYNC` while badge ONLINE
   - LEN=1 loop: no flicker / false wrap
   - P1–P4 page follow: overlay survives grid repaint
   - Mid-play disconnect: playhead stops; reconnect + PLAY: first STEP_SYNC anchors

**Files:** `OctopusApp.html` (primary); confirm `groovebox.cpp` / `midi.cpp` never
dedupe `STEP_SYNC`; note mirror model in `code_info.h`.

**Status:** 📋 **TODO — v6.0.01** (supersedes partial Workstream 7 §7.4 glide work;
see also CHANGELOG 6.0.01 playhead + FX/harp items).

---

## Product documentation & headers (v6.0.00) ✅

- `octopus_web.html` — full v6.0 product page (USB-only, ARP, fog reject, transport model, 190 SysEx cmds).
- `code_info.h` — manifest updated (build 2026-06-20, seq/harp ARP, drum pitch, app notes).
- All firmware `.h/.cpp/.ino` — standard proprietary header block (DIODAC ELECTRONICS).
- `OctopusApp.html` / `octopus_web.html` — HTML proprietary notice comments.

## App production cleanup (OctopusApp.html) ✅

- Playhead: STEP_SYNC-only clock (no web timer); rAF priority pump; transport unified via `_setTransport`.
- Fixed page auto-follow — grid repaints when playhead crosses P1–P4 (was stale cells + head mismatch).
- Playhead same-step no-op + removed redundant `_playLast` reset on play-start.
- Retired CMD map entries removed (FETCH, HARD_SAVE, H/S_FX_MIX, S_SCALE, TRANSPORT_AVAIL).
- ARP: discrete wheel steps, ON/OFF toggle buttons, PAT/RATE fmt uses `Math.round`; discrete index snap fix.
- Mixer: drum FX A/B dropdowns; insert WET/TIME/DEPTH knobs (CMD 190–192); decoupled from D.REV/D.DLY.
- Master console: MIX knobs spaced; TUBE/DJ/EQ grouped tighter.

## Production cleanup (v6.0.00) ✅

- Removed agent-debug `save_agent_log` from NVS path.
- Removed unused `gb*` semantic API wrappers and `seq_synth_fill_buf` shim.
- Fixed harp arp declaration order compile error (`s_harpArp` before `harp_synth_fill_buf`).
- SysEx echo gaps closed: bank, length, scale, chain pin-0, drum wave, FX slot-B.
- CPU: STRINGS wobble skipped when no active voices.

## Roadmap item 4 — Harp ARP ✅

**Goal.** Simplified BPM-synced arpeggiator for **POLY8** and **SOLO** only. **STRINGS**
excluded — auto-disables on STRINGS mode switch.

**Behavior.**
- Latch held note(s) on beam break; arp retriggers on **HARP_SOLO_VOICE**.
- Clock: elapsed-time integration in `harp_synth_fill_buf` scaled by `seqBpm`.
- POLY8: last triggered string; SOLO: king-of-stack.

**Patterns (4):** Up, Down, Up-Down, Random.

**Rates (4):** 1/8, 1/8T, 1/16, 1/16T (default **1/16**).

**Gate (4):** 100%, 50%, 25%, 13% (default **50%**).

**SysEx:** `CMD_HARP_ARP_EN/PAT/RATE/GATE` (186–189). Persisted in `SequencerSettings` —
**`SETTINGS_VERSION 0x0614 → 0x0615`**.

**UI.** App ARP knobs in harp synth panel; hardware HARP SYNTH items 21–24.

## Roadmap item 4.5 — Harp poly headroom ✅

**Goal.** More equal perceived level between 1 string and 2–8 strings in POLY8.

**Fix.** Replaced linear `sum ÷ N` with RMS-style `sum × (32768/√N)` (Q15 lookup)
before `h_soft_clip` in `harp.cpp`.

## App mixer layout ✅

- **TR-909 row 3:** D.PITCH, drum insert sends (D.REV/D.DLY), FX A/B slot knobs.
- **Master console:** H+Seq insert sends (H.DLY/H.REV/S.DLY/S.REV) + shared AUX bus
  (DLY T/FB, REV SZ/DP). Drum sends removed from master row.
- **Harp header:** OCT moved between SOLO and LFO Route dropdown.

## Roadmap item 3 — ArpEngine + Seq ARP ✅

**Goal.** BPM-synced melody arpeggiator for the seq synth (grid rows 0–7). Shared
`arp.h` core; harp adapter deferred to item #4.

**Behavior.**
- On each **step** with active melody rows → latch notes (chord from all lit rows).
- **Arp voice** = seq voice 7; retriggers via `trigger_seq_note` / gate-off release.
- **50% step gate bypassed** while ARP is on (arp gate controls note length).
- Drums (rows 8–15) unchanged.

**Patterns (8):** Up, Down, Up-Down, Down-Up, Random, As-Played, Up+Oct, Down−Oct.

**Rates (8):** 1/1 … 1/32 (default **1/16**) — subdivisions of `master_tick_counter`.

**Gate (8):** 100% … 6% of arp period (default **50%**).

**SysEx:** `CMD_SEQ_ARP_EN/PAT/RATE/GATE` (182–185). Persisted in `SequencerSettings` —
**`SETTINGS_VERSION 0x0613 → 0x0614`**.

**UI.** App ARP knobs in seq synth panel; hardware SEQ SETUP items 10–13.

## Roadmap item 1 — Drum global pitch ✅

**Goal.** Separate drum pitch from Master Tune (M.TUNE). Drums default to **×0.60**
(independent of `masterPitch`); harp + seq still follow M.TUNE only.

**Firmware.**
- New atomic `drumPitchMult` (`globals.h`, default `0.60f`).
- `groovebox.h` drum trigger reads `drumPitchMult` instead of `masterPitch`.
- Persisted as `DrumSettings.pitch_mult` — **`SETTINGS_VERSION 0x0612 → 0x0613`**
  (factory reseed on flash update).
- New SysEx **`CMD_DRUM_PITCH` (181)** — reuses `encodeMasterPitch` / `decodeMasterPitch`;
  `applyMasterParam` + `sendFullStateSync` echo; `PARAM_TABLE` entry.

**App.** Drum strip knob **D.PITCH** (group GLOBAL, cmd 181, def v14=1529 = ×0.60).

**Hardware.** DRUM KIT menu item **Drm Pitch** (l2=41); `kL2DrumsCount` 41→42.

## Roadmap item 2 — Product decisions ✅

### 2a — Harp tuning model (locked, no new code)
- **No discrete harp semitone transpose.** The laser harp uses **BEND** (`CMD_H_PITCH`
  166, continuous ±1 octave) for live manual tune — not a ±12 semi step control.
- **Octave** stays on `CMD_HW_H_OCT` (App OCT input + hardware `SCALE`+turn chord).
- **Hardware harp-transpose menu:** still **deferred** — no clean slot without an
  intrusive interface change; App + BEND + octave cover the use case.

### 2b — Global harp poly headroom (shipped)
- **Decision:** normalise the harp bus by **active voice count** before
  `h_soft_clip` — one voice = unchanged level; N simultaneous voices each contribute
  1/N so dense chords stop driving the soft-clipper into grit/crackle.
- **Implementation:** `harp.cpp` — per-buffer `active_n` count; `acc /= active_n`
  when `active_n > 1`. No preset or NVS changes; no `MIX_TRIM_HARP` bump yet
  (re-audit on hardware if poly chords feel quiet vs seq).

### 2c — M.TUNE knob feel (shipped)
- **Decision:** SysEx v14 encoding is **linear in semitones** (±24 st = ±2 oct),
  **unity centred at v14 = 8192** — replaces the old linear-in-ratio map (unity at
  ~20% travel).
- **Scope:** `encodeMasterPitch` / `decodeMasterPitch` in `interface.cpp` — also
  used by **D.PITCH** / **Drm Pitch** for consistent feel. Stored NVS floats
  (`master_pitch`, `pitch_mult`) unchanged (still ratio); only wire/knob mapping
  changes.

## Decisions locked (from review)
- **Transpose model:** per-engine, symmetric for Harp + Seq (Option A).
  *Octave and Transpose are SEPARATE controls* (octave = ±N×12 semis in one jump;
  transpose = fine ±semitones). Keep **Master Pitch** as a global "Master Tune".
- **Transpose range:** **±12 semitones** per engine.
- **LOAD scope:** expose the existing **single saved session + 16 song slots +
  factory patterns**. No multi-slot NVS rework for now.
- **Implementation order:** (1) pitch/transpose unify → (2) motion batched echo →
  (3) LOAD popup. Plus slider-knob bug review and pattern import (below).

---

## Phase 2 · Step 10 — User sound slots ✅ (firmware)

**Goal.** Save your tweaked sound into a named slot and recall it later, reusing
the existing 256-slot banks (no new memory region).

**Data model.** Factory presets stay in slots `0..127` (`NUM_NAMED_PRESETS`); the
unnamed expansion region `128..191` becomes **64 user slots per engine** (harp =
`userBank`, seq = `seqBank`), `USER_SLOT_BASE=128`, `NUM_USER_SLOTS=64`. The sparse
`banks` delta store already persists any non-factory slot, so saved slots persist
for free (cap = `MAX_BANK_OVR=64`/bank, shared with edited factory presets).

**Names.** App-editable, sparse: `g_userSlotName[2][64][16]` in RAM, persisted only
when renamed via the new `usrnames` NVS blob (`UserNamesBlob`, CRC-checked, mirrors
the bank delta style). Empty name → generic `USER NN`. Names travel with banks across
SAVE/LOAD/RESET (FULL + BANKS_PATTERNS) and reload at boot in `persisted_extras_load`.

**Save/load.** `saveLiveToUserSlot(engine, idx)` snapshots the LIVE patch → slot and
persists via the proven `settings_persist_blocking(BANKS_PATTERNS)` worker (RAM-live,
no reboot). `loadUserSlotToLive` reuses `recallHarpPatch`/`recallSeqPatch` (atomics +
App blob echo) — load is non-destructive.

**Hardware UI.** Two new items on the shared synth menu (so BOTH `HARP SYNTH` l1=3 and
`SEQ SYNTH` l1=8 get them symmetrically), after `Snd Preset`: **`Save Slot`** (idx 19)
and **`Load Slot`** (idx 20). `kL2SynthCount 19→21`. Load Slot recalls live on encoder
turn (like Snd Preset); Save Slot picks the target on turn and commits on ENC press via
the reusable **confirm modal** (`ConfirmAction::USR_SOUND_SAVE`, title "SAVE SOUND",
shows the slot name). Per-engine `userSlotCursor[2]`.

**SysEx (App parity, Step 12 wires the UI).** `CMD_USR_SOUND_SAVE 172`,
`CMD_USR_SOUND_LOAD 173` (`v14=(engine<<13)|slot`), `CMD_USR_SOUND_NAME 174`
(reserved — variable-length NAME blob, sub `0x03`). `CMD_COUNT 172→175`. RX handlers in
`midi.cpp`. No `SETTINGS_VERSION` bump needed — `usrnames` is an additive, optional blob
(absent on older devices = all-generic names, non-fatal).

---

## Phase 2 · Step 11 — User pattern slots ✅ (firmware)

**Goal.** 64 named melody+drum pattern library slots, separate from the 16-bank
session grid and the 29 factory ROM patterns.

**Data model.** `UserPatternSlot` in RAM (`g_userPat[64]`): synth rows 0–7 +
drum rows 8–15 (`uint64_t` step bitmasks), companion `seqLivePatch` +
`drumLivePatch`, per-pattern transpose. Sparse **`usrpat`** NVS blob
(`UserPatBlob`, CRC-checked, only slots with `flags&1` persisted). Names in sparse
**`usrpatnames`** blob (empty → generic `PAT NN`). Both travel with FULL +
BANKS_PATTERNS SAVE/LOAD/RESET and reload at boot in `persisted_extras_load`.

**Save/load.** `saveActivePatternToUserSlot(idx)` snapshots the **active**
bank/chain (grid + both companion sounds + transpose) → slot, persists via
`settings_persist_blocking(BANKS_PATTERNS)` (no reboot). `loadUserPatternToActive`
writes into the active bank/chain, syncs seq atomics, echoes grid + sound blobs to
App. Empty slots are ignored on load.

**Hardware UI.** Two new **SEQ SETUP** items (idx 8/9): **`Save Pat`** /
**`Load Pat`**. `kL2SeqSetupCount 8→10`. Encoder scrolls `userPatCursor`;
Save Pat opens confirm (`ConfirmAction::USR_PAT_SAVE`, title "SAVE PATTERN");
Load Pat recalls immediately (like Load Slot for sounds).

**SysEx (App parity, Step 12 wires the UI).** `CMD_USR_PAT_SAVE 175`,
`CMD_USR_PAT_LOAD 176` (`v14=slot 0..63`), `CMD_USR_PAT_NAME 177` (reserved —
NAME blob sub `0x04`). `CMD_COUNT 175→178`. RX in `midi.cpp`. No
`SETTINGS_VERSION` bump — additive optional blobs.

---

## Phase 2 · Step 12 — App slot browser ✅

**Goal.** 1:1 App UI for the Step 10/11 user libraries: browse 64 slots, save/load
with confirm, rename (persists to device NVS).

**App (`OctopusApp.html`).** Header **SLOTS** button opens a modal with three tabs:
Harp Sounds · Seq Sounds · Patterns. 8×8 vault grid (slot 01–64), rename field,
**SAVE TO SLOT** (confirm dialog matching hardware) and **LOAD FROM SLOT**.
Local `localStorage` caches names + session occupancy; device echoes custom names
(sub `0x03`/`0x04`) and pattern occupancy (`CMD_USR_PAT_SAVE`) on full sync.

**Firmware additions (name path).** `SX_SUB_USR_SOUND_NAME 0x03`,
`SX_SUB_USR_PAT_NAME 0x04` in `sysex.h`; RX in `midi.cpp` →
`setUserSlotName` / `setUserPatName` + `BANKS_PATTERNS` persist;
`txUserLibraryNames()` on `sendFullStateSync()` echoes sparse custom names +
occupied pattern slots to the App.

**SysEx wired in App.** `CMD 172–177` in `CMD{}`; save/load/rename handlers;
inbound sub `0x03`/`0x04` name blobs in `onmidimessage`.

---

## Phase 2 · Step 13 — Verify / docs ✅

**Preset pass 3 (lead / pad / vintage / FX).** Same soft-clip headroom strategy as
passes 1–2; targeted fixes from audition:

| # | Preset | Issue | Fix |
|---|--------|-------|-----|
| 039 | Pulse Lead | ADSR felt wrong (pad-like swell) | Softer attack, shorter decay, lower sustain; disable LFO; warm cutoff; matched osc2 pulse |
| 060 | Cloud Pad | Crackle | Sine-only dual layer (was Plasma osc2); lower sus/rel/cutoff |
| 093 | Juno Pad | Digital crackle | Headroom + warmer cutoff; lighter LFO; square+detuned square osc2 |
| 094 | Mini Lead | Digital crackle | Lower cutoff/res; softer attack; shorter decay |
| 095 | Prophet Pad | Digital crackle | Headroom + cutoff/res/LFO trim; pulse+pulse osc2 |
| 119 | Dark Matter | Too much noise | Osc → square+sine; noise 0.15→0.04; less res/env-mod |

`SETTINGS_VERSION 0x0611 → 0x0612` (factory reseed on flash).

**Phase 2 complete.** Steps 10–12 (user sound slots, user pattern slots, App SLOTS
browser) + preset revision passes documented above.

---

## Phase 3 · Wiring audit + comment hygiene ✅

Cross-check of encoder→slider math, App↔firmware SysEx parity, and stale comments.

### Functional parity fixes
| Area | Fix |
|------|-----|
| **LOAD_PAT echo (App)** | Dropdown sync now uses full `SYNTH_PATTERNS` / `DRUM_PATTERNS` length (29), not hard-coded `< 8`. |
| **MIDI I/O PB** | New `CMD_PB_RANGE` (178) + `CMD_PB_ENABLE` (179); hardware encoder + ENC-click echo; App routing bar controls; `echoPbMapping()` on connect sync. |
| **D-BEAM Target** | New `CMD_DB_TARGET` (180); `applyDbeamTarget()` + `echoDbeamExprState()` echo; App TARGET SYNTH selector. |
| **HW_H_OCT (App)** | `applyIncoming` handler + harp OCT input + `setHarpOctave()`. |
| **User slot load echo** | `loadUserSlotToLive()` emits `CMD_USR_SOUND_LOAD` when App connected; App handles `USR_SOUND_LOAD` / `USR_PAT_LOAD` for SLOTS vault refresh. |
| **Load Pat UX** | SEQ SETUP **Load Pat** (l2=9) recalls on encoder turn (mirrors **Load Slot**); ENC click opens L3 only. |

### Encoder / slider math
| Area | Fix |
|------|-----|
| **MIDI I/O sliders** | `getSliderPct()` case 4 added (PB range/enable + wire channels). |
| **AUX FX bus** | Encoder cases 0/2 route through `applyAuxParam()` (SSOT with App `AUX_ENC`). |
| **PB Enable toggle** | Encoder case 1 toggles on any turn (was `delta > 0` only). |

### Comment / doc accuracy
- `interface.cpp` / `interface.h` — L1 count 16, MIDI I/O vs D-BEAM route, DRUM KIT 41 items.
- `display.h` / `display.cpp` — DRUM KIT item count.
- `settings.h` — header `SETTINGS_VERSION 0x0612`.
- `Octopus_PRO_XL_v6.0.ino` + `OctopusApp.html` — boot/title v6.0.00.
- `midi.cpp` — removed stale `[GAP-2] missing` on lines that already `txSysex`.
- `patches.h` — Section 10A updated (AUX CMDs wired); D-BEAM target echo documented.
- `globals.h` — preset index migration note simplified.
- `code_info.h` — CMD table 0–180.
- `Upgrade.md` WS5b — `kL2SynthCount = 21` (was 19).

**Deferred (document only):** drum insert send unification; split harp/seq synth menu tables; defensive `kL2Song[]` table.

---

## Phase 3b · App UX + wiring fixes ✅

| Issue | Fix |
|-------|-----|
| **Header overflow** | CPU readout + MON/HELP moved to the utility bar (MIDI ROUTING row); top header no longer pushes tools off-screen. |
| **WAVE / DET knobs** | Primary **WAVE** knob wired to `CMD 0/16` (was OSC2-only); dropdown + knob stay in sync via `setHarpWave` / `setSeqWave`. OSC2 relabelled. |
| **D-BEAM Target** | `dbeamRefreshAfterTargetChange()` re-applies live expression after target change (App + hardware); panel copy updated. |
| **Playhead wrap/start** | Snap on multi-step jumps; play-start primes step 0; transition uses full `transition` property. |
| **MIX_TRIM_DRUM** | `2.60 → 3.00` (`globals.h`). |
| **Seq synth “random” changes** | Usually P-lock motion echo during playback or **Load Pat** companion preset — intentional; check motion lanes if unexpected. |

Next: **Harp ARP** (roadmap #4, simplified POLY8/SOLO), then dead-code / CPU pass.

---

## Phase 2 · Preset revision — sine/sub family ✅ (pass 2)

Same soft-clip-headroom root cause as pass 1, exposed worst on **pure sine / sub**
patches (saturation distortion has no bright harmonics to hide behind). Conservative
headroom + beating + voice-pileup trims:
- `10 Sine Bloom` sus 80→72, rel 1200→950, det .04→.03, cut 7000→6000
- `27 Sub Sine`   sus 80→70 (single sine sub → just headroom)
- `33 Dark Sub`   sus 75→68, det .06→.03 (less beat), env>cut .10→.08
- `51 Dream Pad`  sus 88→78, rel 1800→1300, cut 5000→4200, LFO depth .07→.05

`SETTINGS_VERSION 0x0610 → 0x0611` (reseed). NB: recurring crackle across sustained
poly patches points at a systemic gain-stage (8 harp voices accumulate with no
÷polyphony before `h_soft_clip`); a global headroom scale would fix it bank-wide but
changes overall loudness — flagged for a decision rather than applied unilaterally.

## Phase 2 · Preset revision — glass-pad family ✅ (pass 1)

**Why.** `#001 Glass Pad` cracked + spiked CPU. Root cause (engine-verified, not
guessed): on the harp it is a *sustained polyphonic* patch with (a) a long 1400 ms
release → many **dual-oscillator** voices stay alive → CPU pile-up, and (b) two
stacked bright **Hyper Glass** (wave 11) oscillators at an open cutoff (6500) + high
sustain (85) continuously drive the mix soft-clipper (`h_soft_clip`) into gritty
saturation, and the rich top aliases at higher notes → audible "crackle". Resonance
is already hard-capped below self-oscillation by `sanitizePatch`, so the safe,
high-value levers are **cutoff** (tame harsh top), **release/sustain** (voice overlap
+ headroom) and a **clean osc2 layer**.

**Glass Pad (00).** `SP(11, 600,1800,85,1400, 6500, .18, det .06, o2=11)` →
`SP(11, 350,1500,78, 950, 4600, .14, det .10, o2=8)`. Shorter release (fewer
overlapping voices = lower CPU), lower sustain (soft-clip headroom), warmer cutoff
(kills aliasing/grit), and osc2 swapped from a 2nd glass to a clean **sine** sub-layer
(wide 10 ct chorus) — lush but no harsh stacking.

**Direct siblings (same wave-11 / very-bright sustained-pad failure mode), conservative
cutoff/headroom trims, character preserved:**
- `49 Cyber Pad`  cut 7500→5800, sus 85→82, rel 1800→1500, res .22→.18
- `58 Shimmer`    cut 9000→6800, res .25→.18  (GlassHarm top was extreme)
- `80 Aether`     cut 7000→5400, rel 2200→1800, res .18→.16, nse .10→.08
- `83 Space FX`   cut 8500→6200, rel 2000→1800, res .25→.20, nse .15→.12
- `119 Glitch Pad`cut 7500→6000, res .30→.22, nse .20→.18  (kept edgy, it's an FX)

Basses/leads/plucks/stabs left as-authored — short release + filtered, no soft-clip
grit or voice pile-up, so no crackle/CPU risk. Remaining bank is parameter-safe;
will iterate on any specific preset flagged after audition.

`SETTINGS_VERSION 0x060F → 0x0610` so the corrected factory presets reseed on update.

---

## SAVE / RESET / LOAD / CLEAR / SOFT-RESET — Phase 1 ✅ DONE

Deep refactor of the persistence + destructive-action surface for bug-free, 1:1
hardware↔App consistency. `SETTINGS_VERSION 0x060E → 0x060F`; `CMD_COUNT 169 → 172`.

**Reusable confirm modal (extended).** `ConfirmAction{NONE, SEQ_CLEAR, SOFT_RESET,
SAVE, RESET, LOAD}` + `confirmArg` (scope). `drawConfirmDialog()` renders per-action
titles + up to 3 lines incl. a "(reboots)/(cannot undo)" warning. `confirmDispatch()`
routes every action. Modal moved ABOVE the App-connected branch so it receives input
(and renders over the splash) even while the App is connected.

**No more blind saves.** Every long-press save site (generic ENC-long, edge-comp
editor, SEQ-matrix editor) + the App-connected ENC-long now open the protected SAVE
menu / FULL-save confirm via `openSaveMenu()` — nothing persists without an explicit
YES. RESET (`l1==12`) and SAVE (`l1==14`) menu selections are confirm-gated.

**New LOAD menu (`l1==15`, 4 scopes).** `settings_load_scoped(scope)` does a RAM-only
reload (no flash write → no laser-latch glitch → NO reboot), mirroring SAVE/RESET
scopes 1:1. BANKS_PATTERNS load re-adopts the active pattern's stored transpose.

**Soft reset.** `seqSoftResetWorkingImage()` — CLEAR extended: both sounds → preset 0
and the SEQ working image (bank A / chain 0 / synth view / length 16 / transpose 0)
→ initial, RAM-only, preserves the grid + P-locks, no NVS, no reboot. On SEQ SETUP
(after Clear) and the App RESET popup. Shared `seqResetSoundsToPreset0()` keeps CLEAR
and SOFT RESET from drifting.

**Resume-after-save.** `SequencerSettings` gained `dashboard / ui_page / last_synth_pat
/ last_drum_pat`; captured at save, restored at boot so the post-save reboot powers up
exactly where the user saved.

**Scope audit (SEQ MATRIX / SONG / PATTERN).** Documented the subsystem→blob/scope map:
step grids + per-pattern transpose → `patterns`; P-locks → `motion`; bank deltas →
`banks`; SONG + globals + resume image → `settings`. Scopes are orthogonal by design.

**App 1:1 parity.** New `CMD_SCOPED_RESET (169) / CMD_SEQ_CLEAR (170) / CMD_SOFT_RESET
(171)`; `CMD_SESSION_LOAD` now carries scope (0=FULL, back-compatible). App gains a
RESET popup (4 scopes + SOFT RESET), scoped LOAD buttons, and CLR now mirrors hardware
Clear (grid + P-locks + sounds→preset 0 via `CMD_SEQ_CLEAR`). Every destructive App
action is YES/NO confirmed. SAVE/RESET note the reboot-resumes-in-place behaviour.

---

## Workstream 1 — Unified pitch / transpose / octave  ✅ DONE (hw harp-trn menu deferred)

### What shipped
- **Harp = continuous pitch-bend knob (REVISED per user).** Discrete transpose/
  octave switches don't suit the laser-harp; instead the harp gets an *internal
  pitch-bend / manual tune* knob for on-the-fly tuning. `harpPitchMult` float
  (`globals.h`, 1.0 = unity), applied per-voice in `h_arm_voice` (× masterPitch,
  `harp.cpp`). `CMD_H_PITCH = 166` (continuous: v14 8192 = unity, ±1 octave,
  mult = 2^(semis/12)) — handler in `midi.cpp`, echo in `echoFullSeqState`
  (`patches.h`), persisted as `SequencerSettings.harp_pitch` (**SETTINGS_VERSION →
  0x0605**). App: a **BEND** knob in the Harp panel (`KNOBS.harpSynth`, cmd 166);
  auto-syncs via the generic `knobStates` path (no bespoke handler). The harp's
  hardware `SCALE`+turn octave + dashboard root note are left intact (pre-existing).
- **Seq = octave + transpose in the seq-melody header (as requested).** Seq panel
  header gets **OCT (±4)** + **TRN (±12)** (`setSeqOctave` + existing
  `setTranspose`); global `seqTrn` box kept in sync with the new `seqEngTrn`.
  `applyIncoming` reflects `HW_S_OCT` + `TRANSPOSE`.
- **Master Tune:** PITCH knob relabelled **M.TUNE** (global master tune).

### Master Pitch encoding — VERIFIED + updated (PD-2)
- App cmd 68 / firmware `encodeMasterPitch` / `decodeMasterPitch`: **semitone-linear**
  ±24 st, unity at **v14 = 8192** (was linear-in-ratio, unity at ~3277). ✅
- OPTIONAL polish (done in PD-2): unity-centred semitone feel for M.TUNE + D.PITCH.

### Deferred
- **Hardware harp-transpose control:** harp octave is a `SCALE`+turn chord on the
  device (no harp setup menu); there is no clean slot for a harp-transpose chord/
  menu without an intrusive interface change. Harp transpose is **app-controlled**
  for now (still persists + syncs). Follow-up: add a `SCALE`+`OC`/menu path if HW
  parity is wanted.

---

## Workstream 1 (original notes)

### Current state (the redundancy)
| Control | State var | Range | Affects | Firmware | App |
|---|---|---|---|---|---|
| Master Pitch | `masterPitch` (float) | 0.25–4.0 (±2 oct, continuous) | Harp + Seq voices | menu PITCH + `CMD_PITCH` | M.TUNE knob (MIX, cmd 68) |
| Drum Pitch | `drumPitchMult` (float) | 0.25–4.0 (same encode as M.TUNE) | **Drums only** | DRUM KIT l2=41 + `CMD_DRUM_PITCH` (181) | D.PITCH knob (drum strip) |
| Seq Transpose | `seqTranspose` | ±12 semi | **Seq only** | menu + `CMD_TRANSPOSE` (95) | header `seqTrn` box |
| Harp Octave | `octaveShift[0]` | ±4 oct | Harp only | `CMD_HW_H_OCT` (90) | App OCT input (Phase 3) |
| Seq Octave | `octaveShift[1]` | ±4 oct | Seq only | menu + `CMD_HW_S_OCT` (91) | **none** |

Problems: Harp has no semitone transpose; octave absent from app; lone header
transpose silently = seq only; Master Pitch overlaps conceptually.

### Target
- **Harp:** Octave (±4) + Transpose (±12 semi, NEW).
- **Seq:** Octave (±4) + Transpose (±12 semi, existing).
- **Master Tune:** keep `masterPitch` as global, relabel in UI for clarity.
- Expose all four engine controls **identically** in app engine-panel headers and
  firmware menus, one consistent command pattern.

### Work
- Firmware: add `harpTranspose` atomic (±12); apply in `harpNoteOn` note mapping
  (`harp.cpp`) alongside `octaveShift[0]`. New `CMD_H_TRANSPOSE` (id 166); rename/keep
  `CMD_TRANSPOSE` as seq. `applyHarpTranspose` + echo in `patches.h`. Persist in
  `SequencerSettings` (`settings.h`, bump version) — add `harp_transpose`.
- App: move transpose into **Harp** and **Seq** panel headers; add **Octave**
  selectors for both (wire `CMD_HW_H_OCT`/`CMD_HW_S_OCT`); relabel PITCH→"Master Tune".
  Reflect echoes in `applyIncoming`.
- Display: harp dashboard already shows `Oct:`; add `Trsp:` line. Seq dashboard
  already shows both.

### OPEN — verify (flagged during review) — RESOLVED (PD-2)
- **Master Pitch encoding:** App + firmware now share **semitone-linear** v14
  (±24 st, unity at 8192). No ratio-linear drift.

---

## Workstream 2 — Motion-record echo to app  ✅ DONE

### What shipped (simpler than the batched-SysEx plan — no new protocol)
- During motion playback, each applied P-lock lane is mirrored to the app via the
  EXISTING lock-free out-ring: `groovebox.cpp` render block now calls
  `seq_ext_push(motCmds[l], motVals[l])` right after `seq_apply_motion(...)`.
- Key realisation: `MotionLane.targetCmd` **is** the wire SysEx cmd (harp 0-15,
  seq 16-31, drum 32-63, FX ≥64 — confirmed `CMD_H_WAVE=0`/`CMD_S_WAVE=16`), so it
  needs **no translation**. The app already routes every such cmd through
  `applyIncoming`, where the generic `knobStates[cmd]` updater animates the knob
  (and explicit handlers cover wave/FX selects + LFO routes). No `CMD_MOTION_BATCH`
  was needed — the per-step volume (≤4 lanes + STEP_SYNC) is well within the
  128-deep ring + `SeqSysexOut` drain, and stays off the audio core.
- No echo loop: the app applies incoming with `_suppressTx = true`.

### Decisions taken
- One push per active lane per step (not coalesced) — matches STEP_SYNC cadence,
  trivial bandwidth.
- Playback only (recording path is gated by `isMotionPlayback`/the record flow;
  the echo sits inside the `isMotionPlayback` apply block).
- `txSysex` dedup is fine: changing motion values aren't deduped; a held value is
  suppressed (knob already there) — correct.

---

## Workstream — P-lock recording fixes (Fix 1–3)  ✅ DONE

### Fix 1 — Hardware encoder path records motion
- **Bug:** `recordMotionParam()` was only reachable from `handleSysexCommand()`
  (App SysEx). Menu encoders called `applyHarpParam` / `applySeqParam` /
  `updateHardwareParameter` directly — sound changed but no P-locks were written.
- **Fix:** `captureMotionParam()` hook on all canonical apply paths
  (`applyHarpParam`, `applySeqParam`, `applyDrumParam`, `applyMasterParam`,
  `applyFxSend`, `applyAuxParam`, `applyDrumWave`).  Hardware MASTER / AUX FX /
  insert-send menus route through `applyMasterParam` / `applyFxSend` or
  `hwKnobEchoCapture()` for FX-slot indices.

### Fix 2 — PARAM_TABLE-gated recording + extended CMD range
- **Bug:** Recording used hard-coded cmd ranges (0–63, 64–96, three extended CMDs).
  Many `automatable: true` params (FX sends 110–124, FX mix/time/size, FX indices)
  were never captured.  `PARAM_TABLE.automatable` was documentation-only.
- **Fix:** `shouldRecordMotionCmd()` + `captureMotionParam()` in `patches.h`.
  `handleSysexCommand()` ends with one capture call for switch-case CMDs (FX
  indices, wire aliases).  Discrete layout CMDs (transpose, octave, length, …)
  stay blocklisted.  **RX throttle:** 5 ms dedup is skipped while `seqRecording`
  so fast App knob drags are not dropped mid-take.

### Fix 3 — 64-step P-lock depth
- **Bug:** `MotionLane.steps[16]` + `seqCurrentStep & 15` aliased steps 0/16/32/48
  on patterns longer than 16.
- **Fix:** `MOTION_STEPS_PER_LANE = 64`, `MOTION_VERSION = 0x0002`.  Playback uses
  full `step & 63`.  **Note:** v1 motion NVS blobs no longer load (version mismatch
  — re-record or scoped MOTION reset).  Still **4 lanes max** per bank/chain slot.

---

## Workstream 3 — LOAD popup (app)  ✅ DONE

### What shipped
- The header **LOAD** button now opens a `#loadModal` popup (reuses the existing
  `.modal` style) with the three sources the firmware already exposes:
  - **Saved session** → `CMD_SESSION_LOAD` (firmware `loadSettings()` +
    `sendFullStateSync()` + `echoSongState()`, midi.cpp:696 — App repaints fully).
  - **Song chain slot 1–16** → `setSongSlot()` → `CMD_SONG_SLOT`.
  - **Factory pattern** (melody/drum) → `loadSynthPat()`/`loadDrumPat()` →
    `CMD_LOAD_PAT_S` / `CMD_LOAD_PAT_D`, loaded into the **active bank** (label in
    the modal shows the current bank).
- Modal selects are populated in `populateDropdowns()` (`loadSynthPat`/`loadDrumPat`
  mirror the toolbar dropdowns; `loadSongSlot` mirrors the 16 chain slots). The
  old immediate-fire `sessionLoad()` is kept and called from the popup's session row.

### Current NVS reality (single namespace, one snapshot each)
- `settings` blob = `AllSettings` (master, seq config, fx, dbeam, **16 song slots**).
- `patterns` blob = `hwSeqData` snapshot. `banks` blob. `motion` blob (sparse).
- No multiple named user saves; songs = 16 slots inside settings.
- Existing cmds: `CMD_SESSION_SAVE`/`CMD_SESSION_LOAD` (156/157), `CMD_HARD_SAVE`
  (126), `CMD_LOAD_PAT_S`/`CMD_LOAD_PAT_D` (102/103 — load **factory** patterns).

### Target — simple LOAD popup offering
- **Load Saved Session** (settings+patterns+banks+motion) → `CMD_SESSION_LOAD`.
- **Load Song** slot 1–16 → `CMD_SONG_SLOT`.
- **Load Factory Pattern** (synth/drum) → `CMD_LOAD_PAT_S/D`.

### OPEN
- Does `CMD_SESSION_LOAD` already fully repopulate the app on load (needs a state
  resync echo)? Verify the device emits `sendFullStateSync()` after a session load.
- Popup UI placement (header button near transport?).

---

## Workstream 4 — Slider/knob control bugs (app UI)  ✅ DONE

### Root causes found (mixer / drum-kit / AUX)
1. **AUX bus knob `def` values wrong** — DLY T / DLY FB / REV SZ used linear v14
   guesses (5700, 7373, 8192…) instead of the firmware curves (`/1.5` for delay
   time, `/0.95` for capped fb/size). Knobs looked “broken” because the widget
   started far from the real stored value until a full sync landed.
2. **Drum TUN/DEC formatters wrong** — showed `%` but firmware stores 0..1 floats
   (hardware menu shows `0.500` / `0.600`). VOL/NSE as `%` is correct.
3. **Drum voice `def` values wrong** — every voice used generic 8192/13000/0 instead
   of the TR-909 kit table (`DRUM_KIT_*` in `globals.h`).
4. **`knobStates[cmd]` single-slot** — building master after drums meant only the
   last panel’s widget received echoes for shared cmds (AUX 71/72/113/114/142/143).
5. **Knobs never initialized from `def`** — all started at visual 0 until the first
   echo; felt like controls “did nothing” on load.
6. **Drum aux send readout missing** — wire strip showed HARP/SEQ insert sends but
   not DRUM (`CMD_D_DLY` / `CMD_D_REV`).

### What shipped (`OctopusApp.html`)
- `AUX_ENC` + `DRUM_KIT_909_V14` helpers (1:1 with firmware encode tables).
- Fixed all AUX + drum knob `def`/`fmt` values; TUN/DEC → `0.000` display.
- **AUX group added to drum panel** (D.REV, D.DLY, DLY T, DLY FB, REV SZ, REV DP)
   so the mixer drum row is self-contained; master console keeps the same knobs.
- `knobStates[cmd]` → **array** + `_setKnob(cmd,v14)` updates every duplicate widget.
- Knobs paint factory defaults at build time; pattern loaders use `_setKnob`.
- Wire strip: **DRUM** dly/rev % readout added.

### Round 2 — Harp LFO route, LFO knobs, Tube DRV (user report)
1. **LFO route hardware→App broken** — encoder echoed legacy `CMD_HLFO_RT`
   (86) / `CMD_SLFO_RT` (87); App only listened on **11 / 27**. Fixed: firmware
   now echoes canonical `base+index`; App accepts **both** 11/27 and 86/87.
2. **LFO knobs “dead” on hardware with App open** — `mutateHarp`/`mutateSeq`
   returned *before* `applyHarpParam` when `checkWireAuthority` blocked the
   echo. Hardware now **always applies** locally; only the SysEx echo is gated.
3. **Tube DRV audibly dead** — `MasterFx::process` only ran a glue compressor;
   `tube_tone` was unused. Added **tanh saturation** stage (DRV/TONE/MIX wet).

### Round 3 — MAIN MENU regrouped order (user report)
- **Symptom:** “Main Menu position isn’t going in order” — related categories
  were scattered (HARP SETUP/SYNTH split by D-BEAM/MASTER; SEQ items split by
  AUX FX), so scrolling felt random even though it stepped 0→13 correctly.
- **Constraint:** the L1 *id* is hardcoded in ~25 places (`mutateParam` switch,
  `l2CountFor`, `l1==6` grid, `l1==13` SONG, `l1==11` TELEMETRY, `store(5)`…),
  so renumbering would be high-risk.
- **Fix (display-only remap):** category ids stay fixed; added `kL1Order[]`
  (slot→id) + `l1SlotForCat`/`l1CatForSlot` (`interface.h`) and
  `kL1NamesOrdered[]` (`display.h`). `drawMenuL1` centres on the slot; the L1
  encoder steps through slots. New visible order, grouped by instrument:
  HARP SETUP · HARP SYNTH · SEQ SETUP · SEQ MATRIX · SEQ SYNTH · SONG ·
  DRUM KIT · AUX FX · MASTER · D-BEAM · MIDI I/O · LASER SHOW · TELEMETRY · RESET.

### Deferred → after WS4 verified (user request)
- **SP()-style ROM authoring** for FX slots (MASTER / A / B) and melody/drum
  **Patterns** (Workstream 5 + patch-blob wire) — next up once UI knobs verified.

---

## Workstream 5 — SP()-style FX authoring + atomic FX sync + pattern import  ✅ DONE

### Part A — SP()-style FX ROM authoring (`effect.cpp`)
- Added `IFX(...)` / `MFX(...)` / `DYN(...)` macros (explicit `(float)` casts, no
  token-paste fragility) mirroring the `SP()` synth macro. The three 16-entry FX
  tables (`INSERT_FX_PRESETS`, `MASTER_FX_PRESETS`, `DYNAMICS_PRESETS`) now read
  as one aligned line per preset with a single documented param-order header.
  Values are byte-identical to before — pure authoring/readability refactor.

### Part B — Atomic FX→App knob sync on preset recall
- **Bug (same class as the synth patch-blob fix):** recalling a MASTER FX preset
  changes tube DRV/TONE/MIX + DJ FQ/RES/MIX + EQ L/H, and an insert-A preset
  changes the dly/rev sends — but only the *index* was echoed, so the App's knobs
  went **stale** after any FX recall.
- **Fix:** `txMasterFxParams()` (8 master knobs) and `txInsertFxSends(engine)`
  (harp/seq dly+rev) in `midi.cpp` (decl `midi.h`), encoded byte-identically to
  the matching `sendFullStateSync` lines. Called from every recall site:
  - `interface.cpp` hardware menu (MASTER FX, HARP/SEQ SYNTH + AUX-FX slot turns)
  - `midi.cpp` RX (`CMD_M_FX_IDX`, `CMD_H_FX_IDX`, `CMD_S_FX_IDX`)
  The App already routes those param CMDs to their knobs via the generic
  `_setKnob` path, so no App change was needed.
- **Intentionally skipped:** drum insert-A send echo. The drum bus reads
  `fx.drumInsert.dly_send/rev_send` while the App's drum-send knobs drive the
  *separate* `drumDlySend`/`drumRevSend` atomics (`CMD_D_DLY`/`CMD_D_REV`) — a
  pre-existing field split. Echoing would move the wrong knob. Flagged for a
  future pass to unify the two.

### Part C — Pattern import from `patt.md` (APPEND, per user)
- `patt.md` held **21 synth + 21 drum** authored patterns. Appended after the
  existing 8+8 cosmic set → **`NUM_SYNTH_PATS` / `NUM_DRUM_PATS` = 29** (idx 8…28),
  in both `groovebox.cpp` and the `OctopusApp.html` mirror (dropdowns auto-size).
- **Correction pass applied:**
  - *Synth:* `patt.md`'s 3rd row was per-step data (e.g. waveform `300`, cutoff
    values) — **invalid** as a `SynthPatternROM.preset[16]` patch. Kept the
    authored `gate[]`/`pitch[]` (all in `int8` range) and **replaced** each
    `preset[]` with a vibe-matched, in-range factory patch borrowed from the 8
    cosmic rows (acid/swell/reson/staccato/ambient/growl/sub/tines).
  - *Drum:* `tracks[8]` masks + the 8×4 `preset[32]` matched the
    `DrumPatternROM` layout and were in range (0…16383) → imported as authored.

### Part D — [WS5b] Hardware sound-bank preset uploader for SEQ SYNTH (MELODY)
- **Gap reported:** the SEQ SYNTH menu only exposed waves/params/FX — no way to
  load a patch from the 128-name `assets` sound bank on hardware. The HARP browses
  it from the dashboard IDLE encoder, but the SEQUENCER dashboard IDLE encoder is
  taken by BPM, so the seq engine had **no** hardware preset path (only App
  `CMD_S_PATCH` / MIDI PC / pattern recall).
- **Fix:** added a trailing **"Snd Preset"** item (idx 18) plus **Save/Load Slot**
  (idx 19/20) to the shared synth menu (`kL2Synth[21]`, `kL2SynthCount` = 21). In L3 it browses
  the 128 named presets and recalls:
  - SEQ → `recallSeqPatch(nx, UI)` (seq patch blob to App) + `CMD_S_PATCH` echo.
  - HARP → `recallHarpPatch(nx, UI)` + `CMD_H_PATCH` echo (a menu twin of the
    existing dashboard browse — harmless, and now symmetric).
  - Display: name shown as `#NNN <PresetName>`; L3 bar = idx / 127.
  Touched `display.h` (label), `interface.h` (counts), `interface.cpp`
  (mutateParam cases 3+8 idx 18), `display.cpp` (formatter + value-bar 3+8).

### Follow-up (deferred)
- Unify drum insert-A sends with `drumDlySend`/`drumRevSend` so a drum FX preset
  recall can also echo its sends to the App.
- Optional: hand-tune distinct synth patches per appended pattern (currently a
  small set of shared factory patches keyed by vibe).

---

## Workstream 6 — Mixer / bus loudness normalisation  ✅ DONE (needs ear-tuning)

### Problem
Drums quiet, seq mid, harp loud. Root cause is structural, not the volume knobs:
harp/seq voices accumulate + soft-clip near full int16 scale, while the drum
engine carries ~16× of 8-voice headroom (the `>>19` per-voice scale in
`groovebox.h`), so it peaks ~0.27 in normalised float vs harp ~0.75.

### Fix (shipped) — pure float gain-staging, no engine DSP changed
- Added **per-bus calibration trims** applied BEFORE the user volume knobs, so
  inserts + aux sends all see the balanced level:
  - `globals.h`: `MIX_TRIM_HARP=1.00`, `MIX_TRIM_SEQ=1.35`, `MIX_TRIM_DRUM=2.60`.
- Replaced the flat `* 0.333f` bus sum with `MIX_BUS_SUM=0.38` (≈ +1.2 dB) — but
  the real loudness win is drums/seq finally contributing instead of sitting far
  under. Applied in `effect.cpp` (gain stage `hG/sG/dG` + the L/R sum).
- Safety: `fx_clampf` (±1.0 per bus) + final int16 clamp are the backstop;
  equal-power center pan keeps worst-case 3-bus peak ≈ 0.81 + aux wet, under clip.

### TODO — tune by ear on hardware
- Drums still quiet → raise `MIX_TRIM_DRUM` (try 3.0–3.4). **Applied 3.0** in Phase 3b.
- Seq off → nudge `MIX_TRIM_SEQ`.
- More overall level → raise `MIX_BUS_SUM` toward ~0.45 (watch dense hits); back
  off if the master distorts.
- OPTIONAL alt: fix drum quietness natively (relax the `>>19`) so drums need less
  trim — riskier (8-voice overflow margin), deferred in favour of float trims.

---

## Workstream 7 — Firmware↔App SYNC, transport & playhead 1:1  (GAP RESEARCH)

> **v6.0.01:** Playhead mirror rework is anchored above — **PLL slaved `tick_clock()`**
> replaces STEP_SYNC-only + CSS-glide as primary motion. Items 7.2–7.5 still apply;
> item 7.4 implementation plan is superseded by the v6.0.01 TODO.

Goal: device and app perfectly mirror each other; transport buttons reflect
hardware ops; BPM streams as both a value AND a smooth tick; app playhead glides
on the clock, decoupled from the grid DOM. Findings below are grounded in code
(file:line). Implement in the ordered steps at the end.

### 7.1 — SEQ MATRIX long-press SAVE "not working"
- `interface.cpp:671` global ENC-LONG save runs BEFORE the matrix branch, so
  `g_saveRequest` *does* fire in the matrix → NVS save (`settings_save()` writes
  settings+patterns+banks+motion atomically, `settings.h:993-999`) should occur.
- **Real problem:** the save block does `menuState.store(IDLE)` (`interface.cpp:674`),
  but the matrix branch (`interface.cpp:679`) treats *any* state `!= MENU_L1` as
  "in the grid" (the renderer too, `display.cpp` renderUIState). So after save the
  UI stays trapped in the matrix with **no SAVED feedback** → looks like nothing
  happened / didn't save. Also a live save dips master volume ~40 ms (click-free
  handshake, `audio.cpp:421`).
- **Fix:** handle ENC-LONG *explicitly inside* the matrix branch: fire save, show
  a visible "SAVED" confirmation that renders in matrix view, and stay in the grid
  (don't store IDLE). Verify `g_loopParked` confirms within the 500 ms deadline
  while `seqPlaying`.

### 7.2 — Transport buttons don't reflect hardware (recurring)
- App handler `OctopusApp.html:2771-2793`: `CMD.TRIG_MODE` and `CMD.TRANSPORT`
  toggle `btnPlay` (active-green) and `btnRec` (active-red).
- **Bug:** `btnStop` is **never** highlighted anywhere — on stop, play un-lights
  but stop never lights, so the surface looks "dead". Play/rec also rely on two
  redundant echoes (TRIG_MODE + TRANSPORT) which can race.
- Firmware echoes: `seq_stop` sends TRIG_MODE 0 + TRANSPORT 0 + TRANSPORT 4
  (`groovebox.cpp:417-419`); `seq_start` TRIG_MODE 16383 + TRANSPORT 1
  (`groovebox.cpp:405-406`); supervisor re-asserts play+rec every 600 ms
  (`groovebox.cpp:617-623`, above the 500 ms dedup so it self-heals).
- **Fix (app):** single source of truth — derive `{stopped, playing, recording}`
  from CMD.TRANSPORT (treat 0/2 = stopped, 1 = playing, 3/4 = rec on/off) and set
  all three buttons (`btnPlay`/`btnStop`/`btnRec`) together each update. Make
  `btnStop` active when not playing. Stop double-driving from TRIG_MODE (keep it
  only as a legacy fallback that routes into the same setter).

### 7.3 — BPM stream (value box + tick)
- App `CMD.BPM` → updates `#seqBpm` + `this.bpm` (`OctopusApp.html` applyIncoming
  ~2647); firmware echoes on change (`setSequencerBpm`, `groovebox.cpp:439`) and
  every 600 ms (supervisor). Value path looks OK.
- **Gap:** `this.bpm` is not used to time the playhead glide (see 7.4). Tie BPM +
  `seqLen` into the playhead step-duration so motion is clock-accurate.

### 7.4 — App playhead: smooth, detached from grid DOM
- Today: single overlay `#seq-playhead` positioned via CSS `gridColumn`
  (`OctopusApp.html:3276`), a child of `#seq-grid`, driven only by `CMD.STEP_SYNC`
  (`:2855-2860`, no web timer — good clock authority). It JUMPS one column per
  tick and auto-follows pages via `setGridPage` (grid work) → jumpy + DOM-coupled.
- STEP_SYNC is never deduped (`midi.cpp:115`), so ticks arrive per step.
- **Fix:** detach the playhead to an absolute overlay positioned by
  `transform: translateX(col * cellW)` (not `gridColumn`), and add a CSS transition
  whose duration = one step interval derived from `this.bpm` + `seqLen`
  (`stepMs = 60000 / bpm / 4`). Resync/snap position on each STEP_SYNC tick
  (phase-lock), so it glides between ticks and never drifts. Avoid full grid
  rebuilds on page-follow (move only the overlay; repaint cells without destroying
  the playhead element).

### 7.5 — Length / Bank / Song-mode 1:1
- App handles `CMD.HW_S_LEN` (`:2755`), `CMD.BANK` (setBank), `CMD.SONG_MODE/SLOT/
  STEP/STEPS_N/POS` (`:2816-2853`). Firmware `CMD_HW_S_LEN` handler
  (`midi.cpp:471`) stores length but **does not echo** on App→device set (App
  already knows; OK) — but verify **hardware** length/bank changes echo (menu uses
  `applySeqLength`/`applySeqBank`, "echo gap closed" per `interface.cpp:15`).
- **Action:** confirm `sendFullStateSync()` includes HW_S_LEN, BANK, SEQ_CHAIN,
  SONG_MODE/SLOT/STEPS so a connect/LOAD repopulates them 1:1. Add any missing
  echo (esp. length on hardware menu change, song mode toggle).

### 7.6 — State resync on connect / NVS LOAD (echo everything once)  ✅ VERIFIED
- Connect path `CMD_APP_SYNC_REQ` (155) → `sendFullStateSync()` + `echoSongState()`
  (`midi.cpp:616-618`); it force-resets the TX dedup cache (`midi.cpp:742`).
- `sendFullStateSync()` (`midi.cpp:737`) ends with `echoFullSeqState()`
  (`patches.h:872`), which **already** echoes the transport snapshot
  (CMD_TRIG_MODE + CMD_TRANSPORT play AND rec, `patches.h:880-882`), BPM, bank,
  chain, length, transpose, song mode/slot/steps. Coverage confirmed good.
- `CMD_SESSION_LOAD` (`midi.cpp:622-627`) calls `loadSettings()` then
  `sendFullStateSync()` + `echoSongState()` → app repaints fully after LOAD. ✅
- The fix needed was on the APP side (7.2/7.4): the echoes arrived but `btnStop`
  was never lit and the playhead teleported. Both fixed.
- Tie-in DONE: motion-playback echo (Workstream 2) now animates app knobs live.

### 7.7 — Extra gaps to verify during implementation
- `CMD.TRANSPORT` value 2 (pause) currently maps to "stopped" in the app — fine,
  but confirm no pause path is expected.
- Dedup edge: any state that only echoes once and can be missed (no supervisor
  re-assert) — e.g. mutes, FX indices — confirm they ride `sendFullStateSync`.
- Playhead page auto-follow vs. user manually paging during playback (decide who
  wins; today hardware forces the page).

### Ordered fix steps (do in this sequence)
0. **v6.0.01 playhead PLL mirror** (see **Active upgrade anchor — v6.0.01** above):
   `tick_clock()` + STEP_SYNC re-anchor; simplify wrap logic.
1. **App transport SOT** (7.2): one setter drives play/stop/rec buttons from
   CMD.TRANSPORT; fix missing `btnStop` highlight.
2. **App playhead glide** (7.4 + 7.3): ~~detach overlay to transform + BPM-timed CSS
   transition, phase-lock to STEP_SYNC~~ → **superseded by v6.0.01 PLL `tick_clock()`**.
3. **Matrix save** (7.1): explicit ENC-LONG in matrix branch + SAVED toast + stay
   in grid; verify handshake while playing.
4. **Full-sync coverage** (7.5 + 7.6): audit `sendFullStateSync()`; add missing
   length/bank/song/transport echoes; call it after CMD_SESSION_LOAD.
5. **Regression pass:** hardware-drive every transport/length/bank/song/BPM change
   and confirm the app mirrors 1:1; connect/LOAD repaints fully.

---

## Workstream 8 — UI polish (monitor, matrix, dashboards, telemetry)  ✅ DONE

- **App SysEx monitor** (`OctopusApp.html`): now traces real `TX→`/`RX←` frames
  with decoded command names into a bounded 300-line ring buffer; only touches
  the DOM while open; FILTER button hides high-rate streams (PING/STEP/CPU/
  SONG_POS); CLEAR button. Previously it only printed status text and grew
  `innerText` unbounded.
- **SEQ MATRIX** (`groovebox.cpp`): the R/C/STEP read-out moved off its bottom
  row (where it overdrew the lowest grid steps) into the inverted top bar,
  consolidated with the bank/page tag in one compact ≤21-char cursor-aware line
  ("A SYN R1C01 S01/16"). Full 8×16 grid is now overlap-free.
- **Post-save workflow** (`interface.cpp`): ENC long-press in the matrix saves
  AND advances to SEQ SETUP (bank/chain/length) — ReBirth-style loop: build grid
  → save → set up / chain the next pattern. (Was: dropped to IDLE, looked dead.)
- **Harp dashboard** (`display.cpp`): dropped the "Scl:" prefix; octave now shows
  the absolute root note ("C-4", `octaveNote()`) instead of "Oct:+0"; BEAM ON/OFF
  is anchored at a fixed column so the variable-width play-mode name can't shove
  it around. **Seq dashboard**: octave → root note too.
- **Telemetry DAC AGC THRESHOLD** (`display.cpp`): replaced the broken single
  cycling scope trace with **8 per-string vertical bargraphs** (`drawDacThreshold
  Bars()`), auto-ranged to the tallest (floor 512) with a numeric max read-out
  and 1–8 string labels. `getHardwareDACThreshold(s)` is a pure read, safe to
  poll for all strings.

### Leftover cleanup (consistency pass)
- `laser.cpp`: CAL_BASELINE removed from the scope-ring write (now `!= CAL_BASELINE`
  in the guard, dead `case` deleted) — the DAC view reads thresholds directly, so
  the ring no longer does wasted work for it.
- `display.cpp`: dead `case CAL_BASELINE` in the scope header switch removed (the
  view returns early into `drawDacThresholdBars()`).

### App — session-preserving DISCONNECT (connection box)
- First dropdown entry is now "⏏ Disconnect"; `disconnect()` stops the heartbeat
  (ESP times out → hardware regains transport) and clears `midiOut` WITHOUT
  touching in-app state, so reconnecting just re-syncs. `_userDisconnected` is
  sticky so a device rescan won't auto-reconnect behind the user's back.
- **Connect/disconnect transport — DECIDED (non-destructive, never stops play).**
  Firmware already never ties transport to the link (`pollSyncHeartbeat`: "the
  hardware always owns it"), so a disconnect leaves the pattern running. The only
  data-messing path was `onConnect()` force-pinning Bank A — REMOVED. Connect now
  only sends `APP_SYNC_REQ` and ADOPTS the device's live state (bank, transport,
  BPM, full grid, running playhead via `STEP_SYNC`). Net: disconnect→reconnect
  never stops or reshuffles a mid-performance pattern; the UI just re-attaches.

### Matrix in-grid playhead — DONE (live per-step animation)
- The grid already drew a moving column underline (`groovebox.cpp` ~357,
  `col == playhead && playing`); it only looked frozen because the matrix view
  was off the per-step redraw path. Added `viewIsSeqMatrix()` (`display.cpp`) and
  folded `stepChanged && viewIsSeqMatrix()` into the display task's full-draw
  trigger (`audio.cpp`). The matrix re-renders once per step → column tracks the
  clock live; page-diff flush ships only the touched pages; goes silent the moment
  play stops or the user leaves the grid (cost bounded to the step rate). Note the
  grid `page` is the SYN/DRM layer, not a step page — all 16 cols always show, so
  no page-mismatch on the underline.

### App — LIVE-PERFORMANCE background resilience + GPU compositing
- **Problem:** browsers throttle background-tab timers (≥1 s, and ~1/min under
  "intensive throttling" after ~5 min hidden). The 1 s heartbeat `PING` would slip
  past the ESP's 4.5 s link timeout → the link drops mid-show when the user
  backgrounds the tab.
- **Fix (two layers):**
  1. Silent `AudioContext` keep-alive (`_audioKeepAlive`): a zero-gain oscillator
     keeps the tab in the "audible" state → exempt from intensive throttling.
  2. Heartbeat ticker moved into an inline Web Worker (`_makeHbWorker`): off the
     main-thread event loop, immune to main-thread congestion; PING @ 800 ms.
  3. `visibilitychange`: on re-show, immediate `PING` + `APP_SYNC_REQ` so the UI
     snaps back to the device's live state (playhead/transport/BPM) with no flap.
- **GPU:** VU meters now animate `transform:scaleX` (compositor only) instead of
  `width` (layout+paint each rAF frame); playhead already GPU-composited
  (`transform:translateX` + `will-change`). rAF auto-pauses when hidden → no CPU
  burn in background.
- **ESP CPU health (indirect):** a rock-solid heartbeat removes connection
  *flapping*; each splash⇄dashboard toggle forced a full-screen redraw
  (`audio.cpp` ~334), so eliminating flaps removes those Core-0 redraw storms.
  (Checked: the "APP CONNECTED" splash DOES draw a live BEAM bargraph, so the
  D-BEAM-driven redraw is intentionally kept — not wasted work.)

### OPEN / follow-ups
- Root-note assumes engines are rooted on C, base octave 4. Adjust if a scale's
  actual root letter should be reflected.
- Root-note assumes engines are rooted on C, base octave 4. Adjust if a scale's
  actual root letter should be reflected.

---

## Workstream 9 — Atomic patch-blob transfer (preset loading)  ✅ DONE

### Why
On-device preset loads were already instant (`recallHarpPatch`/`recallSeqPatch`
do one `memcpy_P`/`memcpy`). The *loading problem* lived on the **wire to the App**:
- App→device preset change echoed **16 separate per-param messages** (slow,
  individually dedup-able/droppable, partial-load flicker).
- **Hardware→App preset change echoed NOTHING** — `recallHarpPatch/SeqPatch`
  updated the atomics but never told the App, so device-side preset changes left
  the App's knobs stale. (FX presets were already fine: index-only `INSERT_FX_PRESETS`
  ROM load via `CMD_*_FX_IDX`.)

### What shipped — the patch blob
- New variable-length SysEx sub-frame **`SX_SUB_PATCH_BLOB = 0x02`** (`sysex.h`):
  `{ 0xF0, ID, 0x02, engine, p0hi,p0lo … p15hi,p15lo, 0xF7 }` (37 bytes) — engine
  0=harp / 1=seq, 16 params in `SynthParam` order. One atomic, ordered, glitch-free
  transfer instead of 16 messages.
- **TX** `txPatchBlob(engine)` (`midi.cpp`, declared `midi.h`): snapshots the
  livePatch under `patchMux`, skips when no App is connected, no dedup (a blob is a
  discrete "load this preset" event). **Called from inside `recallHarpPatch` /
  `recallSeqPatch`** (`patches.h`), so EVERY recall — hardware menu, MIDI
  program-change, or App — now echoes the full preset. Closes the hardware→App gap.
- **RX** (`midi.cpp parseMidiByte`): an inbound `0x02` blob replays each param
  through `handleSysexCommand(base+i, v)` — identical to 16 individual messages but
  from one frame. Dormant for now (App changes presets by index), kept for symmetry.
- **App** (`OctopusApp.html` `onmidimessage`): decodes `0x02` → loops
  `applyIncoming(base+i, v)`. Both sides map **cmd = base(0|16) + index**, so the
  per-param `CMD_H_PATCH`/`CMD_S_PATCH` handlers shrank to *recall + index echo*,
  and `sendFullStateSync` now ships two `txPatchBlob` calls instead of 32 messages.

### Bonus fix
- Index 11 (LFO route) lands on App **CMD 11 / 27** (`HLFO_RT`/`SLFO_RT`), which
  `applyIncoming` *does* handle. The old loop echoed it at `CMD_HLFO_RT(86)` /
  `CMD_SLFO_RT(87)` — which the App ignored — so **device→App LFO-route sync was
  silently broken and is now fixed** by the blob path.

### Note re: "SP(...) for FX presets / patterns"
- `SP(...)` is a **compile-time ROM authoring macro** for `SOUND_BANK` — not a
  transport. FX presets already use the same index-ROM model (`INSERT_FX_PRESETS`,
  `DYNAMICS_PRESETS`, `MASTER_FX_PRESETS`). Patterns (WS5) should be authored the
  same way (ROM tables, index load). The blob above is the *wire* counterpart that
  makes the App mirror those one-shot loads atomically.

---

## Workstream 6: SOLO play-mode last-note priority + fallback ✅ DONE
**Symptom:** harp notes "stuck"/dead in SOLO — lifting the newest beam while older
beams are still physically held produced silence instead of the older note resuming.

**Root cause:** the laser scan (`laser.cpp`) implemented SOLO priority by
**force-clearing** `stringActive[s]=false` and setting `stringSuppressed[s]=true`
for every stolen beam, then calling `harpNoteOff(s)`. That destroyed the
physical-hold truth: once a held beam was suppressed it stayed muted until its own
finger lifted, so there was **no record to fall back to** — releasing the king
left silence, and the suppression bookkeeping could desync into hanging notes.

**Fix — priority owned by the harp engine, not the laser:**
- `harp.cpp` now keeps a **SOLO held stack** (`g_soloStr/g_soloVel/g_soloN`,
  oldest→newest, all under `patchMux`). `harpNoteOn` pushes (re-touch promotes to
  newest) and arms the shared `HARP_SOLO_VOICE`; `harpNoteOff` pops, and if the
  lifted beam **was the sounding king**, re-arms the most-recent beam still held
  ("older becomes latest"). Empty stack → release the voice. `harpAllNotesOff`
  clears the stack so a mode switch can't leave a stale king.
- `laser.cpp` no longer clears/suppresses stolen beams in SOLO — `stringActive[]`
  stays **pure physical-hold truth**. A held beam already has `stringActive==true`,
  so the next sweep hits the `else if (stringActive[ci])` branch and never
  re-fires → no machine-gun ping-pong, no suppression flag needed
  (`stringSuppressed[]` removed entirely).
- Bidirectional by construction: priority follows beam-break *order*, not pitch,
  so the hand is tracked sweeping up or down.
- **Render:** `harpSoloKing` (atomic, `globals.h`) tracks the sounding beam and
  tells `laser.h laserForString` which beam carries the SOLO voice's envelope
  (white→scale-colour hue). Non-king beams are NOT dimmed (see WS11 item 10 —
  dimming hurt retrigger + bidirectional staccato).

---

## Workstream 7b — STRINGS-mode ADSR follows the patch  ✅ DONE
**Symptom:** in STRINGS the note decayed to silence even while the beam stayed
broken (finger held), instead of sustaining and only ringing out on release.

**Root cause:** `harp.cpp` forced `eff_sus = pluck_phys ? 0u : c_sus_level` —
STRINGS hard-zeroed the sustain level, so every note behaved as a fixed pluck
regardless of the sound-bank patch.

**Fix:** `eff_sus = c_sus_level` in all modes. STRINGS now holds at the patch's
sustain while the beam is broken and enters RELEASE (at the patch's release time,
via `harpNoteOff` → `ENV_RELEASE`) when the beam clears. A plucky sound is now a
property of the *patch* (sustain≈0 / short decay), i.e. "depending on the sound-
bank preset setup". The micro-pitch vibrato (`pluck_phys` → `g.vib_step`) is kept.

## Workstream 5 follow-up — drum FX preset send sync  ✅ DONE
**Symptom (carry-over from WS5):** recalling a drum FX (slot A) preset didn't move
the App's drum send knobs, and the preset's own sends had no audible effect.

**Root cause:** the drum bus reads its dly/rev send from the `drumDlySend`/
`drumRevSend` **atomics** — `snapAudioParams` overwrites `fx.drumInsert.*_send`
from those atomics every buffer — unlike harp/seq where the insert field IS the
live value. So a preset's `dl_mix`/`rv_mix` were clobbered and never echoed.

**Fix:** new `loadDrumFxLive(i)` (`effect.h`) used only at *explicit user recall*
sites (hardware menu cases 16/12 in `interface.cpp`, App `CMD_D_FX_IDX` RX in
`midi.cpp`): it loads the preset then publishes its sends INTO the
`drumDlySend`/`drumRevSend` atomics, followed by `txDrumFxSends()` (`midi.cpp`) to
echo `CMD_D_DLY`/`CMD_D_REV` to the App. The audio-task/NVS paths keep calling
plain `loadDrumFx` (which leaves the atomics — and thus saved sends — untouched),
so there is no boot/session-load clobber. The old "field split" note in `midi.h`
is resolved.

## Workstream 8b — FX engine deep optimisation (FX.md)  ✅ DONE
Implemented the `FX.md` report against `effect.cpp` / `effect.h`. All changes are
bit-faithful to the prior audio (sample order inside every stateful filter is
unchanged) — they cut redundant work, not the sound.

- **BUG-1 (tail gate domain):** `FX_TAIL_PEAK_THRESH` was `20.0f` (int16 domain)
  but `peakOut` is the post-master FLOAT peak [0,1]. The gate could never hold, so
  long pad/reverb tails were chopped at ~280 ms. Fixed to `20.0f/32767.0f`
  (≈ -64 dBFS) — directly addresses the earlier "tail cut too early" report.
- **OPT-1 (single-pass merge):** the 3 separate convert/insert/sum passes over the
  buffer collapsed into ONE loop; the 3 × 512-float DRAM scratch buffers
  (`fx_buf_hL/sL/dL`) are GONE (channels live in registers). ~33 % fewer loop
  iterations and ~16 MB/s less DRAM traffic. Hot-path guard is now
  `if (!fx.initialized) return;` (new `FxChain::initialized` flag set in `init()`).
  `fx_free_buffers()` kept as a no-op for API/teardown compatibility. **BUG-2**
  (missing `fx_buf_sL` null guard) is subsumed — the scratch guard is gone.
- **OPT-2 (MasterFX/InsertSlot const hoist):** tube-sat (`comp_sat/tilt/w`) and the
  safe compressor/gate thresholds (`comp_thr_safe`, `dyn_thr_safe`) are computed in
  `update_params()` / `loadDynPreset()` (≤ once per buffer / on preset load) instead
  of every sample.
- **OPT-3 (pan cache):** equal-power pan coeffs recompute only when a pan knob moves
  (static cache + dirty check) — skips 6 `cosf`/`sinf` per buffer.
- **OPT-4 (reverb/delay setParams):** comb feedback + delay-tap samples are latched
  once per buffer via `SharedReverb::setParams`/`SharedDelay::setParams`
  (`SharedAux::setParams` fans out), removing a per-sample multiply/clamp.
- **OPT-5/6 (FxDelayLine):** `readFrac` now does ONE address computation (was two
  `read()` calls re-masking the same index); added null-check-free
  `write_unsafe/read_unsafe/readFrac_unsafe` used by `SynthFX::process_mono` (safe
  because the hot path bails out unless `fx.initialized`).
- **OPT-7:** `out_buf` index multiply → `out_p` pointer increment.
- **OPT-8:** `peakOut` tracked only while the aux ring is armed (`trackPeak`).
- **OPT-9 (DrumFX cross-core):** N/A in this codebase — `DrumFX::drive/tone` are
  never written from a sysex handler (no other engine touches them), so there is no
  cross-core race. Skipped intentionally. The `buf_bytes` unused-variable note is
  also resolved (the whole scratch-alloc block was removed by OPT-1).

## Workstream 9 — Harp engine deep optimisation (harp.md)  ✅ DONE
Implemented the full `harp.md` report against `harp.cpp` / `display.cpp`. Audio
character is unchanged — redundant work and one silent overflow bug are fixed.

- **RISK-1 (SVF int32 overflow):** `SVF_CUT_MAX = 31000` exceeds the safe bound
  (26 207) for `cut * hp` in int32. Worst-case `|hp| ≈ 81 919` → product ≈ 2.54e9
  overflows signed int32 (max 2.147e9), corrupting the shifted SVF coefficient and
  destabilising the filter instead of clamping cleanly. Fixed by promoting both
  `cut * hp` and `cut * svf_band` multiplies to `int64_t` before the `>> 15`.
- **OPT-1 (h_noise CAS):** replaced the lock-free `compare_exchange_weak` PRNG seed
  with a plain `static uint32_t` LCG. `h_noise()` is Core-0-only (audio task); the
  CAS was up to 4096 locked RMW cycles/buffer when noise was active and never
  contended.
- **OPT-2 (active bitmask):** per-buffer `live_mask` built with 8 acquire loads;
  inner loop uses a bitwise test instead of 4096 acquire loads/buffer. Voices that
  go idle clear their bit for the rest of the buffer.
- **OPT-3 (detune cache):** `exp2f(detune_semi/12)` cached against raw `P_DETUNE`;
  Flash libm call fires only on detune knob/preset change.
- **OPT-4 (do_pitch_mul hoist):** moved out of the per-voice loop (per-buffer const).
- **OPT-5 (accumulation clarity):** explicit `vel_weighted` / `contrib` parenthesis.
- **OPT-6 (hue ADSR cache):** `hue_steps_update()` caches attack/decay/release step
  sizes + sustain level; dirty-checked against the four hue atomics (~125 Hz laser
  task, not audio hot path).
- **OPT-7 (vibrato phase accumulator):** per-string `s_vib_phase[]` replaces
  `nowUs % period` integer division (~20–40 cycles per laser call eliminated).
- **OPT-8 (osc2 skip):** `step2_target` write skipped when `osc2_active` is false.
- **OPT-9 (MIDI freq LUT):** 128-entry DRAM table via magic-static init; note-on no
  longer calls Flash-resident `exp2f`.
- **OPT-10 (lfo_depth clamp):** per-operand clamp before add (defensive; both sources
  are already bounded to [0,16383] today).

## Optional fixtures  ✅ DONE (hw harp-trn assessed / deferred)

- **Dashboard root note:** `display.cpp` `octaveNote()` → `midiRootNote(baseMidi,
  shift)` derives the letter from the scale's first MIDI note + octave shift. All
  current scales root on 60 (C-4) so the rendered text is unchanged; future non-C
  scales will display correctly.
- **Hardware harp-transpose menu:** assessed and **deferred** (same rationale as WS1).
  `kL2Synth[]` is shared between HARP SYNTH and SEQ SYNTH — a harp-only transpose
  item would also appear on the seq synth menu unless the L2 tables are split (an
  intrusive interface change). Harp manual tuning is already covered by the continuous
  **BEND** knob (`harpPitchMult`, cmd 166, app + NVS + echo); harp octave remains on
  the existing SCALE+turn chord. No new menu slot added.
- **Hand-tuned pattern presets:** the 21 appended synth patterns (idx 8…28 in
  `groovebox.cpp` / `OctopusApp.html`) no longer recycle 8 cosmic presets — each
  carries a distinct vibe-matched patch (waveform/ADSR/filter/LFO tuned to the
  pattern name: acid, bell, dub, pad, etc.).

## Backlog — display region updates (gradual)
Page-diff already makes everything cheap; if any element still needs a *targeted*
repaint to cut GFX render cost (like the playhead), candidates: D-BEAM bargraph,
telemetry trace. Low priority now.

---

## Workstream 10 — SAVE system redesign (menu + granular targets)  ✅ SHIPPED (core)

### Shipped this pass
- **(c) SAVING… frame** ✅ `drawSaveToastIfActive()` — SAVING… pill on `g_saveRequest` rising edge, SAVED pill after commit.
- **Idle-dashboard long-press save** ✅ ENC-LONG → `requestSessionSave()` → NvsWorker full session write.
- **SAVE menu (L1=14)** ✅ Four scoped targets mirroring RESET: Full / Banks+Pats / Motion / Settings. Confirm with ENC single at MENU_L2; routes through `requestScopedSave()` → `settings_save_scoped()`.
- **Scoped NVS writes** ✅ `settings_save_scoped(ResetScope)` writes only the blobs for that scope; FULL writes all four in one commit.
- **16 KB persist path** ✅ `settings_persist_blocking()` — dedicated NvsBlk task used by boot first-load, corrupt reseed, factory reset (pre-NvsWorker), and `saveSettingsSafe()`. Fixes stack overflow when `settings_load()` previously called `settings_save()` inline from setup/MIDI tasks.
- **256 KB NVS partition (after app) + coredump** ✅ `partitions.csv` — `factory` app @ `0x10000` (esptool always flashes there), then **256 KB NVS @ `0x3B0000`** + **64 KB coredump @ `0x3F0000`**. The four blobs need ~8-9 NVS pages — they do NOT fit the 28 KB pre-app gap, so NVS lives after the app. coredump partition captures panics.

### (b) Dirty-blob split — DESIGN (deferred)
- **Why deferred:** `nvs_set_blob` already compare-skips byte-identical data, so
  the **flash-erase window (the cache-off freeze) is already minimal** for routine
  saves. A dirty-split only saves CPU prep (8 KB pattern memcpy + CRC, etc.) during
  a window where audio is already silenced → low reward.
- **Risk to respect:** correctness MUST fail safe. A missed dirty-set = a silently
  dropped save = data loss. So the design is "dirty defaults TRUE; cleared only on
  confirmed commit; set TRUE at every mutation entry."
- **Plan when done:**
  - Add `patterns_dirty` / `banks_dirty` / `motion_dirty` atomics (init true).
  - Set them at the centralised write points: `seqUI_toggleStep` / grid App writes
    / `loadFactory*Pattern` / `clearAllPatterns` / `copyPatternSlot` (patterns);
    bank ops (banks); motion record + `CLR_PLOCKS` (motion).
  - In `settings_save()`, skip a blob's malloc+memcpy+CRC+set_blob when its flag is
    clear; clear flags only inside the `ok` branch (mirror `settings_dirty`).
  - Settings blob always writes (cheap, always-relevant SSOT).

### SAVE MENU — the rethink (open design)
**Goal:** long-press opens a SAVE MENU instead of an immediate full-session save,
offering granular targets. Current NVS is a SINGLE namespace with ONE snapshot per
blob — granular *named* saves need a storage rework, so this is the crux to design.

**Proposed entries (to confirm):**
1. **Save Sound Bank preset** — persist the live harp/seq patch into a USER preset
   slot. NOTE: sound bank is currently compile-time ROM (`SP()` macro); user preset
   slots need NEW NVS storage (multi-slot blob or per-slot keys).
2. **Save FX** — master + insert FX state into a user FX slot (same multi-slot need).
3. **Save Seq Melody & Drum** — the active pattern/bank grid (already in `patterns`
   blob; could expose explicit slot save vs the current implicit active-slot model).
4. **Save Song** — the 16-slot song chain (already in `settings`; expose per-slot).
5. **Save Full Session** — current behaviour (everything), kept as the quick option.

**Open questions (decide before coding):**
- **Storage model:** stay single-snapshot (overwrite) or add multi-slot NVS (named
  user banks)? Multi-slot is the bigger lift but is what "Save preset" implies.
- **Gesture:** long-press → MENU (loses the current instant-save muscle memory) vs
  keep long-press = quick full-save and add the menu under a new chord (e.g.
  SCALE+ENC long). Leaning: **keep instant full-save; add menu via a distinct chord.**
- **NVS wear:** more granular saves = more writes; confirm wear-levelling headroom.
- **App parity:** mirror the save targets to OctopusApp, or hardware-only?

---

## Workstream 11 — Hardware bug pass (from v6.0 field test)  🐞 IN PROGRESS
Reported after the clean compile + flash (90% load, no glitches, smooth telemetry,
100% trigger). Status after fixes.md pass:

1. **D-BEAM randomly stuck → needs restart.** ✅ **dbeamLit stuck-timer** added
   (`dbeam.cpp`, 50 ms watchdog). Instrumentation TBD if hang persists.
2. **AUX FX sliders dead: DLY FB + Tube DRV.** ✅ **AUX_DLY_FB decode** fixed
   in `patches.h` (App + encoder paths). ✅ **TB_DRV App default** set to 0.
3. **SOLO mode ping-pong (2nd-last instead of latest).** ✅ Already fixed — no
   change needed (release hysteresis + solo stack in harp.cpp/laser.cpp).
4. **App playhead holds on 1 step.** ✅ **advancePlayhead** refetch after page
   turn (`OctopusApp.html`).
5. **SAVE/RESET crash + nothing persisted.** ✅ **NvsWorker 16 KB**, per-row
   `patterns_save_h`, `settings_persist_blocking()` for all sync saves, 256 KB NVS
   partition, scoped RESET/SAVE menu wired through NvsWorker handshake.
6. **SEQ synth melody sound not persisting.** ✅ Root cause: `loadFactorySynthPattern`
   (`groovebox.cpp`) loaded a melody's *companion sound preset* straight into
   `seqLivePatch[]` but never fanned it out to the seq atomic mirrors — and NVS
   saves from the atomics (`settings_sync_from_ssot`), not `seqLivePatch`. So the
   loaded sound was audible yet reverted on reboot. Added the shared
   `syncSeqAtomicsFromLivePatch()` helper (`patches.h`); now called from
   `loadFactorySynthPattern` and reused by `recallSeqPatch`. Every writer of
   `seqLivePatch[]` now keeps the atomics == live sound, so SAVE captures what
   is actually playing.
7. **All beams break after a runtime SAVE (stay stuck until manual scale change).**
   ⚠️ **WORKED AROUND via reboot-after-save [SAVE-FIX14] — root cause unresolved.**
   Working theory is the **AC-coupled LT1016 comparator**: the laser is dark for the
   whole NVS write (the save park), and after such a long dark period the comparator's
   coupling needs a few hundred ms of steady illumination to re-settle. Resume the
   scan immediately and it false-fires, latching the 74HC74 "broken" on every string —
   stuck until something happens *later*. That's the only reason a manual SCALE CHANGE
   appeared to "fix" it: the user did it seconds afterward, once the coupling had
   already settled. Two-part fix in `laser.cpp` save-unpark:
   - **Clean re-home** [SAVE-FIX10]: `computeHardwareDACThreshold()` for all strings +
     galvo → string 0, `currentIndex`/`direction` reset, 74HC74 latch clear, held
     voices released *and* `stringActive[]` explicitly cleared (note: `allNotesOff()`
     only releases voice envelopes — it does NOT clear `stringActive[]`).
   - **Detection blackout** [SAVE-FIX11]: suppress beam detection for ~1.2 s after
     unpark while the scan keeps running (beams stay visible, latch cleared each
     cycle) so the comparator's post-dark false-fires can't latch/hold any note.
     Detection auto-resumes once the window elapses — recovery is now automatic.
   - **Forced threshold refresh + dedup invalidation** [SAVE-FIX12]: `mcp4922Write`
     has a dedup cache (`g_lastDACWord`) that skips the SPI write when the value
     matches the last one. If the flash write glitches the MCP4922 threshold
     register but the dedup word still matches, the DAC is never refreshed and the
     comparator runs against a stale threshold → every beam reads "broken". Unpark
     now recomputes `computeHardwareDACThreshold()` for ALL strings *and* calls
     `invalidateDacCache()` so the threshold/galvo are physically re-written on
     resume. A one-shot `beam_rehome` serial breadcrumb logs `thr0` to confirm the
     recompute ran (0 ⇒ margin corrupted; sane ⇒ comparator/AC settle).
   - **Save-side recovery handshake** [SAVE-FIX13]: decoupled recovery from the
     laser noticing the park — `settings_save_task` sets `g_beamRecover` after
     every write and the laser runs the re-home whether or not it parked.
   - ⚠️ **NONE of the in-place recoveries fully worked** — beams still latched
     "broken" within ~2 s after a save (and Arduino `Serial` is on UART0, not the
     USB port, since `USB.begin()` claims USB for MIDI, so the diagnostic
     breadcrumbs were never visible). **Accepted workaround [SAVE-FIX14]: reboot
     after every successful save.** `requestScopedSave()` now sets
     `g_restartAfterSave`; `settings_save_task` ACKs the App + flashes the SAVED
     toast, then `esp_restart()`s after 700 ms. The just-written NVS settings
     reload on boot (nothing lost) and the proven boot init brings the beams up
     cleanly. SAVE-FIX10–13 are left in as a harmless safety net. **Root cause
     (likely AC-coupled comparator settle / flash-write DAC glitch) remains
     unresolved** — revisit if a no-reboot save is ever needed.
8. **SCALE / OPEN-CLOSE buttons trigger too fast (bounce → double action).** ✅
   Per-button debounce added (`interface.h ButtonPoll::debounceMs`); SCALE + OC
   raised to `DEBOUNCE_SLOW_MS` (90 ms) in `initHardwareInterface()`, encoder
   button stays at the snappy 50 ms `DEBOUNCE_MS`.
9. **Beam HUE should track the sound preset's envelope (was preset-independent).**
   ✅ Rewrote the harp-mode colour path in `laser.h laserForString()` so the
   active beam goes **WHITE on beam-break and fades back to the scale colour as
   the preset amp envelope falls** — the ADSR *is* the hue timeline:
   - **POLY8 / SOLO:** full white while the note is held; on release it fades to
     the scale colour over the preset's **RELEASE** time.
   - **STRINGS:** white tracks `envNorm` directly, so **DECAY** (and release)
     shape the fade — mimicking a plucked string losing energy. String vibration
     + micro-pitch continue to come from the synth (`harp.cpp`, `pluck_phys`).

   Voice selection is mode-aware: POLY8/STRINGS read the fixed per-string voice
   (`vi = string % 8`); SOLO reads the single shared `HARP_SOLO_VOICE` and only on
   the sounding **king** beam. The old separate global hue ADSR
   (`stringHueEnv`/`hueAttack…`) is no longer used for harp play modes (still used
   by LASER SHOW). `harpNoteOff` no longer clears `harpSoloKing` on the final
   release, so the king beam can show its release fade (the `envNorm` gate hides it
   once the voice goes idle; cleared on the next arm / `harpAllNotesOff`).

   **Smoothing refinement:** the white↔scale-colour blend now eases through a
   per-string one-pole smoother (`s_white[]`, ~30 ms) toward `envNorm`, identical
   for all three modes, so the colour is a continuous gradient (no stepping) and
   lands softly on the scale colour after the voice idles. Brightness is held
   constant — the beam never dims to black during the transition (only the hue
   changes).
10. **SOLO dimmed non-king beams — hurt retrigger + bidirectional staccato.** ✅
    Removed the non-king dim block in `laser.h laserForString()`; all beams now
    render at full brightness in SOLO. `harpSoloKing` is still tracked as the
    render hint for which beam carries the sounding voice's envelope.
11. **Random STUCK NOTES (note never released, beam never cleared).** ✅ Deep
    revision of the beam-detect state machine in `laser.cpp laser_sweep_task`:
    - **Asymmetric release:** a stray HW break no longer decrements
      `releaseCounter`. A pending release is cancelled only by a *sustained*
      re-touch (`touchCounter` rebuilt to `holdCap = max(2, onNeeded)`), and the
      clear path decrements `touchCounter`, so isolated false peaks from the
      AC-coupled LT1016 can't keep a note alive forever.
    - **Absolute fail-safe:** `lastHoldUs[]` records when a string was last
      SOLIDLY held; if it hasn't been for `beamStuckReleaseMs` (default 350 ms,
      `globals.h`) the note is force-released regardless of counters. A genuine
      hold refreshes the timer every dwell, so only a beam that stopped being truly
      broken times out. `0` disables the timeout.
    - **Mode/scale-change desync:** `harpSetPlayMode` + the manual scale change now
      set `g_beamClearReq`; the laser clears `stringActive[]`/counters/latch so a
      beam held across the change can't carry over as a silent stuck "active"
      string (`harpAllNotesOff` only drops the voices, not the laser hold state).
12. **`beamStuckReleaseMs` exposed in HARP SETUP + persisted.** ✅ New menu item
    `HARP SETUP → Stuck Rel` (L2 index 8) adjusts the anti-stuck timeout live via
    the encoder (0–`BEAM_STUCK_RELEASE_MAX` = 2000 ms, 25 ms/tick, `0` shows
    `OFF`). Saved in NVS via `SequencerSettings.beam_stuck_ms`; `SETTINGS_VERSION`
    bumped `0x0606 → 0x0607` (one-time factory reset on update). Same wire pattern
    as `beamGateHoldMs` (`Gate Hold`) but global, not per-scale.
13. **HARP SETUP audit — Touch On/Off bar fix.** ✅ The `Touch On`/`Touch Off`
    progress bars divided by `7` while `CONFIRM_MAX = 15`, so the bar overshot
    100% for values > 8. Now normalised over the full `CONFIRM_MIN..CONFIRM_MAX`
    range. Verified the other HARP SETUP params (`Gate Hold`, `White Lvl`,
    `Beam R/G/B`, `Margin`) — encoder clamp, on-screen value, bar fraction, and
    NVS save/load all correct (per-scale arrays persist every scale).
14. **Edge compensation made editable (per-string) + new 8-bar editor page.** ✅
    Was a hard-wired `constexpr EDGE_COMP_NORMAL/RAINBOW[8]` in `dbeam.h`, applied
    as a per-string multiplier on the beam-detect DAC threshold in
    `computeHardwareDACThreshold()` (`final = scaleMargin × edge × colour`; lower
    threshold = more sensitive, so outer strings get < 1.0 to fix their weaker
    reflection). Now a runtime, NVS-persisted global array `edgeComp[8]` (percent,
    100 = ×1.00, range `EDGE_COMP_PCT_MIN..MAX` = 40..150), seeded from
    `EDGE_COMP_FACTORY_PCT[]`. Decisions: ONE global set (frame geometry, not
    scale) SHARED across all scales incl. rainbow (the baked rainbow asymmetry is
    no longer applied to detection; rainbow RGB colour weighting is unchanged).
    - **Editor:** `HARP SETUP → Edge Comp` opens a full-screen telemetry-style
      page (`drawEdgeCompEditor`, modelled on the DAC AGC bars). 8 vertical bars =
      per-string comp; dashed ×1.00 reference line; SCALE steps the selected
      string (0→7 wrap); ENC turn sets its %; OC resets the selected string to
      factory; ENC double exits, ENC long saves+exits. Any string whose beam is
      broken right now renders **inverted** (live `stringActive[]`), so you can
      wave a hand and see which outer strings actually register. Page redraws
      every frame while open (`audio.cpp` draw gate).
    - **Persistence:** `LaserSettings.edge_comp[8]`; `SETTINGS_VERSION`
      `0x0607 → 0x0608` (one-time factory reset on update). Threshold recomputed
      for all 8 strings on every edit (edits under `patchMux`, recompute outside).
15. **D-BEAM menu revision — Offset/Range apply bug.** ✅ `updateHardwareParameter`
    case 1 (D-BEAM) Offset/Range did `if (!checkWireAuthority(...)) return;` BEFORE
    applying, so they silently refused to move while the App heartbeat was active
    (every other param follows the WS4 rule: apply locally always, gate only the
    wire echo). Reordered to apply-then-gate-echo. Audited all 6 items
    (`Offset`/`Range`/`Curve`/`Enable`/`Env Atk`/`Env Rel`): encoder clamp, value
    text, slider/toggle, and NVS save/load (`DBeamSettings`) all verified correct.
16. **D-BEAM Curve transfer-function preview + live bar.** ✅ `D-BEAM → Curve`
    renders a 16-bar transfer-function graph (`drawDbeamCurvePreview`,
    input 0..1 → output via `dbeamCurveEval`, mirroring `applyDBEAMCurve`) instead
    of a meaningless enum slider, so the user sees how each curve (Lin/Inv/Exp/
    Log/Sig) reshapes hand-height → expression while turning the encoder. Below
    the fixed math graph is a **LIVE** horizontal bar fed by `dbeamAmplitude`
    (the post-curve output), so waving a hand shows the actual beam response after
    the curve is applied — animated by the existing ~10 Hz D-BEAM redraw watcher.
17. **Menu nav — double-click backs out to IDLE.** ✅ ENC double-click at
    `MENU_L1` was a no-op (`default: break`), so the dashboard was only reachable
    via long-press (which also saves). Now `L3→L2→L1→IDLE`, giving a clean
    save-free way back to the dashboard.

18. **D-BEAM Route relocated + Harp/Melody target + routing fixes.** ✅
    - **Moved `Route` from MIDI → D-BEAM submenu** (it controls a D-BEAM, not a
      MIDI channel). MIDI I/O now holds only PB range/enable + 3 channels
      (`kL2MidiCount` 6→5); D-BEAM gains `Route` and `Target` (`kL2DBeamCount`
      6→8). MIDI adjust/format/confirm/isToggle cases reindexed accordingly.
    - **New `Target` selector — Harp Synth ↔ Melody Synth.** `DbeamTarget`
      enum + `currentDbeamTarget` atomic + `dbeam.target` NVS field. The route
      FUNCTION (Mod/Vol/Cut) is shared; Target only chooses the destination
      engine. Melody-synth addends `dbeam_seq_svf_cutoff` / `dbeam_seq_mod_depth`
      are consumed in `sequencer_render_block` exactly as the harp consumes its
      pair (Cut → SVF cutoff offset, Mod → LFO depth add); Vol → `mixSeqVol`.
    - **Routing bug fix (stuck modulation).** `applyDbeamRoute` / `applyDbeamRouteHW`
      previously cleared the DSP addends only on `OFF`, so switching e.g. Cut→Mod
      left `dbeam_svf_cutoff` frozen and the filter stuck open. Both now clear ALL
      four addends (harp + seq) on every route/target change; `routeDbeamExpression`
      additionally zeros the non-selected synth's addend each buffer, so only one
      engine is ever modulated. Also fixed: routing was only APPLIED while a beam
      was held (laser.cpp idle path updated the follower but never re-applied it),
      so any target stayed frozen at its last value after the hand lifted. The
      idle path now applies the decayed expression too, so Cut/Mod addends fall
      back to 0 on lift. `SETTINGS_VERSION` 0x0608 → 0x0609.
    - **VOLUME route is now a true inverted volume pedal.** It rests at the
      user's normal bus level and DIPS toward silence as the hand approaches the
      sensor (`vol = base × (1 − expr)`), auto-returning when the hand lifts.
      Baselines (`dbeamVolBaseHarp/Seq`) are snapshotted on entering the route,
      continuously re-adopted from the live bus while the hand is off (so MASTER
      H.Vol/S.Vol still set the rest level without fighting the pedal), restored
      on leaving the route or switching Target, and substituted at save time so a
      mid-dip is never persisted. (Old behaviour: `vol = expr`, i.e. silent at
      rest and stuck at the last level on lift.)  The user **Curve is forced
      Linear** while VOLUME is routed (the pedal already inverts; an Inverted
      curve would otherwise cancel it) — the Curve preview shows `(VOL=Lin)`.
    - **VOLUME no-sound fix.** `applyExpressionHysteresis()` returns a 0..4095 mV
      value; `applyDBEAMCurve()` is what normalises it to 0..1. The first cut of
      the force-Linear logic bypassed `applyDBEAMCurve()` entirely, so the pedal
      saw ~4095 and drove `base×(1−4095) → negative → 0` (silent at any hand
      position). Fixed by giving `applyDBEAMCurve(raw, forceLinear)` a flag that
      skips only the curve SHAPE while keeping noise-floor/normalise/gain.
    - **CUTOFF route mapped to a 20–90 % window** (was 0–100 %). The 20 % floor
      stops the filter snapping open from fully-closed as the hand enters (the
      click), and the 90 % ceiling trims the abrupt top of the sweep.

19. **DAC AGC THRESH bargraph + per-scale edge-comp.** ✅
    - **Fixed full-scale telemetry.** The `DAC AGC THRESH` view was auto-ranged
      (`scale = max(tallest, 512)`), so the highest bar always pinned to the
      ceiling and hid the real headroom. Now drawn against a fixed 12-bit range
      (0–4095) — bars show true absolute thresholds. (The intentional `edgeComp`
      *valley* — lower thresholds at the edges so every string trips at the same
      physical height — is therefore visible and correct, not a bug.)
    - **Separate rainbow edge-comp set.** `edgeComp[]` is used by mono-colour
      scales; the rainbow scale now has its own `edgeCompRb[]`, because each
      rainbow string is a different colour with its own physical trigger height.
      `computeHardwareDACThreshold` picks the set via the active scale's
      `isRainbow`; the HARP SETUP → Edge Comp editor edits whichever set the
      current scale uses (header shows `EDGE COMP RB` for the rainbow set). Both
      persist (`laser.edge_comp` / `laser.edge_comp_rb`). `SETTINGS_VERSION`
      0x0609 → 0x060A.

20. **Fog rejection (common-mode rejection) — isolated module `fog.h`.** ✅
    - **Problem.** In haze/fog, scattered laser light raises the reflected
      amplitude on *all* strings together, producing false beam-breaks / scratchy
      stuck notes. A real hand instead spikes *one* string far above the rest.
    - **Method 2 (differential / common-mode rejection).** New self-contained
      module `fog.h`. The D-BEAM ADC task publishes a **copy** of each string's
      Kalman amplitude (`g_kalman_ac[si].x`) into the module's own branch array
      `g_fogAmp[]` via `fogPublishAmp()` — it writes back to nothing. `fogAccept(ci)`
      accepts a beam-break as a real hand only when that string's amplitude clears
      the **common-mode fog floor** (the 2nd-smallest string amplitude, so it
      ignores one dead channel and doesn't rise with polyphony) by a user margin.
    - **Strict isolation (by request).** The laser trigger does
      `beamBrokenHW = beamBrokenRaw && fogAccept(ci)` — an advisory AND that can
      only ever *suppress* a stray fog break, never fabricate a trigger, and never
      touches `g_kalman_ac[]`, the D-BEAM expression, or the DAC threshold. When
      disabled (default) `fogAccept` returns `true`, so the existing trigger path
      is byte-for-byte unchanged.
    - **UI / persistence.** Two new HARP SETUP items: **Fog Reject** (on/off
      toggle) and **Fog Margin** (0–3000 mV, the differential the hand must clear).
      `kL2HarpSetupCount` 10 → 12. Both persist (`laser.fog_reject` /
      `laser.fog_margin`). `SETTINGS_VERSION` 0x060A → 0x060B (one-time factory
      reset on update).
    - **TELEMETRY → FOG REJECT view.** New scope page (`TelemetryView::FOG_REJECT`,
      page 7 — encoder cycle bound bumped to it). Plots the isolated `g_fogAmp[]`
      branch as 8 auto-ranged bars with a **dotted FLOOR** line (`fogFloor()`, the
      2nd-smallest string) and, when enabled, a **dashed ACCEPT** line
      (floor + margin). Bars that clear ACCEPT render **inverted** = they pass the
      gate, so the gap between the dotted floor and a hand's bar is the live margin
      headroom — wave a hand and tune `Fog Margin` until only true hands cross.
      `fogAccept()` refactored to share `fogFloor()` with the view (single source).

21. **EDGE COMP: per-scale review via OC + rainbow seed retune.** ✅
    - **OC scrolls scales in the editor.** The Edge-Comp 8-bar editor now uses the
      **OC button** to step through all 16 scales (`edgeEditScale`, independent of
      the live harp scale); the header shows the reviewed scale name (+ `RB` tag),
      and edits land on that scale's set — mono scales share `edgeComp[]`, rainbow
      uses `edgeCompRb[]`. *Reset-to-factory* moved from OC → **encoder single
      press** (OC was repurposed for scale select). New `scaleIsRainbow(int)` /
      `edgeCompFor(int)` / `edgeFactoryFor(int)` helpers resolve the set for an
      arbitrary scale (the live-scale `activeEdge*()` helpers are unchanged and
      still drive `computeHardwareDACThreshold`).
    - **Rainbow factory seed retuned.** `EDGE_COMP_FACTORY_PCT_RB[]` / `edgeCompRb[]`
      / `laser.edge_comp_rb[]` reseeded to S1..S8 = **40/110/100/100/142/54/47/75 %**
      so every rainbow string trips at the same physical height with the current
      per-string rainbow colours. `SETTINGS_VERSION` 0x060B → 0x060C (one-time
      factory reset applies the new seed).

22. **EDGE COMP: fully independent per-scale tables (16×8) + name navigation.** ✅
    - **Per-scale model.** The two-set model (one mono + one rainbow) was replaced
      with `edgeComp[NUM_SCALES][8]` — every scale carries its own 8 values, since
      trigger height tracks beam colour (red ≠ blue even between mono scales), and
      the rainbow scales vary colour per string. `computeHardwareDACThreshold()`
      reads `edgeComp[scaleIdx][stringIdx]`; a single `EDGE_COMP_DEFAULT_ROWS` macro
      feeds the factory constant (`EDGE_COMP_FACTORY[16][8]`), the runtime array,
      and the NVS struct default (mono scales seed the geometric valley, scales
      15/16 the rainbow per-colour set). Helpers `edgeCompFor(s)`/`edgeFactoryFor(s)`
      and `activeEdge*()` resolve a scale's row. Scale change already recomputes all
      thresholds, so a scale's table applies live the moment it's selected.
    - **Editor navigation.** The OC button still scrolls scales; the editor now
      shows the **full scale name** in the header (uncluttered — it changes as you
      scroll, so you read the scale instead of guessing), with the selected
      string's value compact + right-aligned so long names never clip. Each scale's
      edits land on its own row; reset-to-factory restores that scale's factory row.
    - **Persistence.** `laser.edge_comp` is now `[NUM_SCALES][8]` (replaces the old
      `edge_comp[8]` + `edge_comp_rb[8]`). `SETTINGS_VERSION` 0x060C → 0x060D
      (one-time factory reset on update).

23. **LASER SHOW v2 — projector mode refactor.** ✅
    - **Hardware-honest model.** The galvo is a single-axis fan (MCP4922 ch A =
      X position, ch B = comparator threshold), so the "projector" is the 8-beam
      fan animated by colour/brightness, not XY vector graphics. While the show
      runs, the fan visits a **fixed even 8-beam layout** (`SHOW_DAC_POS[8]`),
      independent of the selected harp scale.
    - **Pure-output mode.** Hand detection, D-BEAM expression and ADC attribution
      are gated OFF in show mode (`dbeamLit` stays low, detection counters pinned,
      no `routeDbeamExpression`) — no false harp notes while projecting. The hue
      ADSR still advances, but it is driven by the **sequencer** (melody note-on/off
      via `harpHueNoteOn/Off`), not by beam breaks.
    - **Animation engine** (`laserForString`, `LaserShowAnim`): per-beam hue from
      the latched melody note (when MIDI→Hue on) + Base Hue anchor; brightness =
      per-beam hue-ADSR envelope shaped by **Anim Mode** — `PULSE` (note pulses its
      beam), `CHASE` (bright dot tracks the sequencer step), `STROBE` (fixed-rate
      fan gate), `WAVE` (ambient travelling sinusoid via `safe_sinf`).
    - **Drum flash.** Any drum hit on a step stamps `g_showDrumFlashUs`; the laser
      core derives a linear-decay white flash (depth = **Drum Flash**, 0 disables)
      added across the whole fan. Lock-free single-cell, no per-frame decay state.
    - **Decoupled toggles.** Show Mode and MIDI→Hue are now INDEPENDENT (match the
      App's two buttons); enabling the show no longer force-arms MIDI→Hue (removed
      the firmware coupling in `interface.cpp` + `midi.cpp` RX).
    - **HUE ADSR scaling fixed firmware↔menu↔App.** Times are stored in SECONDS on
      per-stage scales (`HUE_ATK_MAX_S` 2 s, `HUE_DEC_MAX_S` 3 s, `HUE_REL_MAX_S`
      4 s; SUS = 0..100 %). `applyMasterParam` (`n*max`), the SysEx echo (`/max`),
      the menu clamps/bars and the App knobs all agree now (previously the App read
      2–4× off the real timing).
    - **Menu/UI consistency.** LASER SHOW menu = Show Mode, MIDI Hue, Base Hue,
      **Anim Mode**, **Drum Flash**, Hue Atk/Dec/Sus/Rel (9 items). **Screensaver**
      moved to HARP SETUP (it is a closed-harp idle behaviour, not a show control).
      App `laser-show` panel gains ANIM + DRUM FLASH knobs. New SysEx
      `CMD_LSR_ANIM` (167) / `CMD_LSR_DRUMFLASH` (168), `CMD_COUNT` 167 → 169.
    - **Persistence.** `LaserSettings` gains `anim_mode` + `drum_flash`; hue ADSR
      load clamps widened to the new per-stage ranges. `SETTINGS_VERSION`
      0x060D → 0x060E (one-time factory reset on update).

24. **SEQ SETUP — deep bug-fix pass + CLEAR + reusable confirm modal.** ✅
    - **Sliders track the encoder.** `getSliderPct()` had no `case 5`, so every
      SEQ SETUP bar was pinned at the 0.5 default. Added per-item normalisation
      (Bank /3, View 0/1, Transpose (t+12)/24, Length (n-1)/63, Load Synth/Drum =
      preset/(N-1)).
    - **Load Synth / Load Drum index honesty.** `loadFactorySynthPattern()` /
      `loadFactoryDrumPattern()` now set `g_lastSynthPreset` / `g_lastDrumPreset`
      themselves (single source of truth), so the OLED readout, the menu encoder
      and App-driven `CMD_LOAD_PAT_S/D` loads all agree. The menu dropped its stale
      private `static` index (which started at 0 and silently diverged) and now
      reads the live value, so scrolling is continuous from the real position.
    - **Transpose boundary.** `(uint16_t)(cur+delta+12)` wrapped a sub-zero step at
      −12 into a huge value that clamped to +12. Now the signed target is clamped
      to ±12 before the wire encode.
    - **CLEAR item + `seqClearActiveAndResetSounds()`.** New SEQ SETUP → "Clear":
      blanks the ACTIVE bank/chain (all 16 step rows + the 4 P-lock/motion lanes)
      and resets BOTH the seq-synth and drum SOUND to their factory default image
      (preset 0's companion patch — sanitised, version-bumped, atomic fan-out for
      NVS). Not a factory reset — other banks/settings/calibration are untouched.
      App is resynced (zeroed grid rows + P-lock clear + both sound blobs).
    - **Reusable YES/NO confirm modal.** New generic `ConfirmAction` +
      `openConfirm()` / `confirmDispatch()` (globals.h / interface.cpp) with a
      centred popup (`drawConfirmDialog`). Encoder selects (right = YES, left =
      NO), ENC click commits, ENC double cancels, defaults to NO. CLEAR raises it;
      RESET / SAVE can reuse it later by adding an enum + a dispatch case.
