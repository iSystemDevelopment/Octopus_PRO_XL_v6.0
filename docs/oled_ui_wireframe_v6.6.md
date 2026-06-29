# OLED UI wireframe — v6.6.0 (Phase A)

**Status:** ✅ Phase A artifact — normative for Phase D (firmware menu regroup). **Phase D landed** in `interface.h` / `display.h` / `interface.cpp`.  
**Parent:** [`v6.3.00.md`](../v6.3.00.md) §14.3 D, §14.9  
**Display:** SH1106 **128×64** mono (existing `display.cpp` constants)

---

## 1. Design intent

| Surface | Role |
|---------|------|
| **Dashboards** | Live performance at a glance (HARP / SEQ) |
| **SEQ MATRIX** | Patimage **display** + optional **TWEAK** (on/off only) |
| **L1 menu** | 9 performance entries + **SYSTEM** — no librarian |
| **App** | Full edit, SESSION/PATIMAGE 1–16, SongPack export |

Encoder = navigate · click = enter/confirm · double-click = back · SCALE/OC = context actions.

---

## 2. Screen regions (all full-screen views)

```text
128 px wide × 64 px tall

 y=0..9    HEADER     title · link glyph · transport · mode badge
 y=10      divider    ─────────────────────────────────────────
 y=11..55  BODY       dashboard lines · matrix · menu list · L3 value
 y=56..63  STEPBAR    optional playhead bar (dashboard + matrix)
```

Matrix uses **y=9..56** for cells (8×7 px); header overlays **one text line** above grid (see §5).

---

## 3. IDLE dashboards

### 3.1 HARP PLAY (dashboard — `DashboardMode::HARP`)

*Replaces “show laser + pick sound” — not full HARP SETUP.*

```text
┌──────────────────────────────────────────────────────────────┐ 0
│ LASER HARP                              [link]  GATE:OPEN   │ header
├──────────────────────────────────────────────────────────────┤ 10
│▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓│ 12 inv band
│  SOUND: Glass Pad 02          ◄ pack list (encoder)          │
│                                                              │
│  Scale: 01 Major        Oct: +0                              │
│  MODE: POLY8            DBEAM: OFF                           │
│  ░░░░░███████░░░░░░░░   D-BEAM bar                          │
└──────────────────────────────────────────────────────────────┘ 64
```

| Field | Control | Notes |
|-------|---------|-------|
| Sound | Encoder turn | Cycles **SongPack** embedded sounds only (1–N names) |
| Gate | OC long | Open/close laser |
| Mode | OC short | POLY8 → STRINGS → SOLO |
| Scale | SCALE short | Panic + next scale |
| Octave | SCALE + turn | ±4 |

**Not on dashboard:** wave, ADSR, filter → **HARP TONE** menu or App.

---

### 3.2 SEQ PLAY (dashboard — `DashboardMode::SEQUENCER`)

```text
┌──────────────────────────────────────────────────────────────┐ 0
│ SEQ  TRAP                              [link]  ▶/■  REC     │
├──────────────────────────────────────────────────────────────┤ 10
│▓ BANK:B  LEN:32  CHAIN:ON ▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓│ 12
│ TRSP:+0                              Root: C4                 │
│ BPM:120    H■ S  D■   (■ = muted bus)                        │
│ ████████░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░  step bar  │ 56
└──────────────────────────────────────────────────────────────┘ 64
```

| Field | Control | Notes |
|-------|---------|-------|
| Bank | SEQ SETUP or matrix wrap | A–D |
| LEN / BPM / TRSP | SEQ SETUP / encoder on dashboard focus | |
| Chain | Display only | Edited in App Bank Manager |
| Kit tag | Header `TRAP` | From SongPack drum sound |
| Mutes | MASTER or MIX LIVE | H/S/D indicators |

**Enter matrix:** MENU → **SEQ MATRIX** or shortcut from SEQ PLAY L2.

---

## 4. L1 MAIN MENU (v6.6 performance order)

**9 visible slots** scroll on encoder. Double-click → dashboard.

```text
┌──────────────────────────────────────────────────────────────┐
│ MAIN MENU                                                    │
├──────────────────────────────────────────────────────────────┤
│   HARP PLAY                                                  │
│ > SEQ MATRIX                                                 │  cursor
│   DRUM                                                       │
│   ...                                                        │
└──────────────────────────────────────────────────────────────┘
```

| Slot | Label | Enters | Replaces (v6.3 menu) |
|------|-------|--------|---------------------|
| 0 | **HARP PLAY** | HARP dashboard + L2: gate, scale, sound, screensaver | HARP SETUP (performance subset) |
| 1 | **HARP TONE** | L2 groups → L3 params (§4.1) | HARP SYNTH (trimmed) |
| 2 | **SEQ PLAY** | SEQ dashboard + L2: bank, len, trn, arp | SEQ SETUP (performance subset) |
| 3 | **SEQ MATRIX** | **Direct grid** (no “Open Grid” L2) | SEQ MATRIX |
| 4 | **DRUM** | L2: kit, vol, pitch, 8 waves (pack list) | DRUM KIT (trimmed) |
| 5 | **MIX LIVE** | L2: H/S/D vol, mutes, M.FX preset | MASTER (subset) |
| 6 | **D-BEAM** | L2: offset, range, curve, route (trimmed) | D-BEAM |
| 7 | **PERF SLOT** | L2: Load 1–8 · Save · name | SAVE/LOAD librarian |
| 8 | **SYSTEM** | L2: MIDI · Telemetry · Factory reset | MIDI + TELEMETRY + RESET |

**Removed from L1:** SONG (chain in patimage), SEQ SYNTH wall, AUX FX (→ MIX LIVE preset), SAVE 0–127 scopes.

**Hidden (SYSTEM → Service):** LASER SHOW, Edge Comp, Fog reject — not in performance scroll path.

---

### 4.1 HARP TONE — L2 groups (replaces flat 25-item list)

```text
┌──────────────────────────────────────────────────────────────┐
│ HARP TONE                                                    │
├──────────────────────────────────────────────────────────────┤
│   TONE                                                       │
│ > ENVELOPE                                                   │
│   FILTER                                                     │
│   MOD                                                        │
│   ARP                                                        │
└──────────────────────────────────────────────────────────────┘
```

| L2 | L3 items |
|----|----------|
| TONE | Waveform, Detune, Osc2 |
| ENVELOPE | Attack, Decay, Sustain, Release |
| FILTER | Cutoff, Resonance, Env→Cutoff |
| MOD | LFO Rate, LFO Depth, LFO Route |
| ARP | On, Pattern, Rate, Gate |

**App-only (removed from HW):** Save Slot, Load Slot, Snd Preset 0–127 browser.

---

### 4.2 PERF SLOT

```text
┌──────────────────────────────────────────────────────────────┐
│ PERF SLOT                                                    │
├──────────────────────────────────────────────────────────────┤
│   Load PERF 1                                                │
│ > Load PERF 2                                                │
│   ...                                                        │
│   Load PERF 8                                                │
│   Save current → slot…                                       │
│   Name (9 chars)                                             │
└──────────────────────────────────────────────────────────────┘
```

Load → decode **SongPack** → RAM → matrix VIEW. Save → confirm → encode SongPack to NVS.

Slot **0** = autosave (no menu label; power-down only).

---

### 4.3 SYSTEM

```text
┌──────────────────────────────────────────────────────────────┐
│ SYSTEM                                                       │
├──────────────────────────────────────────────────────────────┤
│   MIDI I/O                                                   │
│   Telemetry                                                  │
│   Factory reset                                              │
│   Service…  → LASER SHOW, Edge Comp, Fog (hidden path)      │
└──────────────────────────────────────────────────────────────┘
```

---

## 5. SEQ MATRIX — patimage display (§14.9)

### 5.1 Chrome overlay (y=0..8, single line)

```text
BANK B  P2/4  LEN32  GIG PACK  [VIEW]
│      │       │      │          └── TWEAK* when OC held / latched
│      │       │      └── SongPack meta.name[9]
│      │       └── seqLength
│      └── step page 1–4
└── active bank A–D
```

### 5.2 Grid (16×8 cells, 8×7 px)

```text
     1   2   3   4   5   6   7   8   9  10  11  12  13  14  15  16
 1  [■][ ][■][ ][ ][ ][ ][ ][ ][ ][ ][ ][ ][ ][ ][ ]   melody rows
 2  [ ][■][ ][ ][ ][ ][ ][ ][ ][ ][ ][ ][ ][ ][ ][ ]
 ...
 8  [ ][ ][■][ ][ ][ ][ ][ ][ ][ ][ ][ ][ ][ ][ ][ ]
 9  [■][ ][ ][ ][■][ ][ ][ ][ ][ ][ ][ ][ ][ ][ ][ ]   drum rows
...
16  [ ][ ][ ][ ][ ][ ][ ][■][ ][ ][ ][ ][ ][ ][ ]

Legend:
  [■]  step ON (filled)
  [ ]  step OFF (outline)
  [╳]  beyond LEN (crossed out) — existing firmware
  ◄►   cursor invert border — encoder position
  ___  playhead underline on active column when playing
```

### 5.3 Modes

| Mode | Enter | ENC click | Exit |
|------|-------|-----------|------|
| **VIEW** | Default after PERF load | hint only | — |
| **TWEAK** | Hold **OC** (or SEQ MATRIX → “Edit”) | toggle step on/off | release OC → dirty? → save dialog |
| **FULL** | — | — | App only |

### 5.4 Navigation (wrap — no separate patimage UI)

| Gesture | Action |
|---------|--------|
| ENC ←/→ at col 1/16 | P1↔P2↔P3↔P4; at edge wrap **bank** A↔D |
| ENC ↑/↓ at row 1/8 | melody↔drum page; at edge wrap **bank** |
| ENC click | TWEAK: toggle cell; VIEW: no-op |
| Double-click ENC | → L1 or dashboard |

*Existing implementation: `seqUI_moveLeft/Right/Up/Down` in `groovebox.cpp`.*

---

## 6. Gesture summary (v6.6)

| Context | SCALE short | OC short | OC long | ENC turn | ENC click | ENC dbl |
|---------|-------------|----------|---------|----------|-----------|---------|
| HARP dash | scale+panic | mode | gate | sound pick | menu L1 | — |
| SEQ dash | play/stop | rec arm | — | BPM* | menu L1 | — |
| MATRIX VIEW | play/stop | **latch TWEAK** | — | move cursor | — | back |
| MATRIX TWEAK | play/stop | toggle cell | exit TWEAK | move cursor | toggle | back |
| MENU L1/L2 | — | — | — | scroll | descend | ascend |
| L3 param | — | — | — | adjust value | back L2 | back L1 |

\*BPM on SEQ dashboard when App not connected; App-connected = encoder BPM only (existing policy).

---

## 7. Old → new menu mapping (Phase D checklist)

| v6.3 L1 (15 slots) | v6.6 disposition |
|--------------------|------------------|
| HARP SETUP | Split → HARP PLAY (performance) + SYSTEM→Service |
| HARP SYNTH | → **HARP TONE** (grouped) |
| SEQ SETUP | Split → SEQ PLAY + matrix wrap |
| SEQ MATRIX | → direct enter grid |
| SEQ SYNTH | **Removed L1** — 4 knobs in MIX LIVE or App |
| SONG | **Removed** — App Bank Manager |
| DRUM KIT | → **DRUM** (trimmed) |
| AUX FX | **Removed L1** — room preset in MIX LIVE |
| MASTER | → **MIX LIVE** |
| D-BEAM | Keep (trimmed) |
| MIDI I/O | → SYSTEM |
| LASER SHOW | → SYSTEM→Service |
| TELEMETRY | → SYSTEM |
| RESET | → SYSTEM |
| SAVE | → **PERF SLOT** |

---

## 8. Firmware constants preview (Phase D — do not implement in Phase A)

Proposed `kL1Order_v66[]` (9 entries):

```c
// slot → category id (reuse existing cat ids where possible; cat 16 = PERF TBD)
{ 0, 3, 5, 6, 9, 2, 1, 14, 4 }  // + SYSTEM aggregates 4,11,12 via L2
```

`kL1MenuCount` → **9** (performance). Service screens reached from SYSTEM L2 only.

---

## 9. Phase A sign-off

- [x] L1 performance order defined (§4)
- [x] HARP PLAY + SEQ PLAY dashboards specified (§3)
- [x] SEQ MATRIX VIEW/TWEAK wireframe (§5)
- [x] HARP TONE groups (§4.1)
- [x] PERF SLOT + SYSTEM (§4.2–4.3)
- [x] Old→new mapping table (§7)
- [x] Gesture table (§6)

**Next:** Phase **B** — [`song_pack_v1.md`](song_pack_v1.md) + [`session_bundle_v1.md`](session_bundle_v1.md).
