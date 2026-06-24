# Octopus PRO XL v6.1 — User Manual

**Laser Harp Groovebox · Firmware 6.1.00**

| Field | Value |
|-------|-------|
| Document type | End-user & integrator reference |
| Firmware | `6.1.00` · NVS namespace `octopus` · `SETTINGS_VERSION 0x0615` (struct layout) |
| Companion app | [octopus.isystem.app](https://octopus.isystem.app) (Web MIDI / SysEx) |
| Hardware UI | SH1106 OLED · rotary encoder · SCALE · OC |

This manual describes the Octopus PRO XL from initial setup through complete operation of the hardware menu system, companion application, signal routing, and musical tools (sequencer topology, arpeggiator layouts, D-BEAM response curves, and effects character). Menu labels and category order match the on-device OLED display exactly (`display.h`).

For repository overview and build instructions, see [**README.md**](https://github.com/iSystemDevelopment/Octopus_PRO_XL_v6.0/blob/main/README.md). Product site: [**octopus-info.isystem.app**](https://octopus-info.isystem.app). App: [**octopus.isystem.app**](https://octopus.isystem.app). Source: [**GitHub**](https://github.com/iSystemDevelopment/Octopus_PRO_XL_v6.0). [Facebook](https://www.facebook.com/diodac.co.uk/). For developer architecture, see [**code_info.h**](./code_info.h).

---

## Table of contents

1. [Product overview](#1-product-overview)
2. [System architecture](#2-system-architecture)
3. [Connections & initial configuration](#3-connections--initial-configuration)
4. [Control surface reference](#4-control-surface-reference)
5. [Operational dashboards](#5-operational-dashboards)
6. [Menu navigation model](#6-menu-navigation-model)
7. [Technical reference — layouts, curves & effects](#7-technical-reference--layouts-curves--effects)
8. [Main menu reference](#8-main-menu-reference)
9. [OctopusApp companion](#9-octopusapp-companion)
10. [Recommended workflows](#10-recommended-workflows)
11. [Diagnostics & troubleshooting](#11-diagnostics--troubleshooting)
12. [Appendices](#12-appendices)

---

## 1. Product overview

Octopus PRO XL is an embedded performance instrument that unifies **laser harp playing**, **step sequencing**, **drum synthesis**, **proximity expression**, and **programmable laser visuals** on a single ESP32-S3 platform. The design prioritises **standalone operability**: a performer can select scales, browse 128 factory presets, compose 64-step patterns across four banks, chain arrangements in song mode, and persist the entire session to flash — without external software.

When a host computer is available, **OctopusApp** provides a graphical editing environment over USB MIDI SysEx. Transport authority (play, stop, record arm, tempo) remains on the hardware control surface at all times, ensuring predictable live behaviour and eliminating parameter fights between UI layers.

### 1.1 Engine summary

| Engine | Function | Primary I/O |
|--------|----------|-------------|
| **Harp** | 8-string laser instrument, scales, play modes, optional arp | Beam break → note; RGB galvo output |
| **Sequencer** | Melody grid (rows 1–8) + drum grid (rows 9–16), 64 steps | Internal clock; USB SysEx to OctopusApp |
| **Drums** | 8-voice synthesis (kick … perc) | Grid row triggers; kit character presets |
| **D-BEAM** | Hand-proximity expression | ADC → internal synth modulation only |
| **FX bus** | Shared delay/reverb + per-engine inserts | I2S stereo master output |
| **Laser show** | Hue matrix, animation, drum flash | Galvo + RGB (independent of audio MIDI) |

---

## 2. System architecture

### 2.1 Dual-core topology

Processing is partitioned across ESP32-S3 cores to isolate **galvo timing** from **audio DSP**:

```mermaid
flowchart TB
  subgraph Core0["Core 0 — DSP & UI"]
    A[AudioSynth\nI2S + full mix]
    D[dbeam_adc\nADC DMA + Kalman]
    C[ControlPoll\nencoder @ 200 Hz]
    O[OledRender\n~30 Hz]
  end
  subgraph Core1["Core 1 — Laser & I/O"]
    L[LaserSweep\ngalvo timing]
    S[SeqSysexOut\nSTEP_SYNC + supervisor]
    M[MidiUsbRx\nSysEx parser]
    N[NvsWorker\nflash persist]
  end
  C --> O
  M --> A
  D --> A
  S --> M
  A --> OUT[Audio output]
  L --> GALVO[Laser projection]
```

### 2.2 Audio signal path

Each engine passes through **insert FX-A** (modulation/time), **insert FX-B** (dynamics), then summed sends to the **global delay/reverb aux**, **master bus** (tube, DJ filter, EQ, master preset), and finally the I2S DAC.

```mermaid
flowchart LR
  subgraph Sources
    H[Harp synth]
    S[Seq synth]
    DR[Drums]
  end
  subgraph PerEngine["Per-engine chain"]
    IA[FX-A slot]
    IB[FX-B slot]
  end
  subgraph Global
    DS[Delay send]
    RS[Reverb send]
    MB[Master bus]
  end
  H --> IA --> IB
  S --> IA --> IB
  DR --> IA --> IB
  IB --> DS
  IB --> RS
  DS --> MB
  RS --> MB
  IB --> MB
  MB --> DAC[I2S out]
```

### 2.3 Control-plane & persistence

All parameter writes funnel through **`patches.h`** apply-functions so hardware encoder turns, OLED menu edits, and OctopusApp SysEx commands converge on identical atomic state. Session data is stored in **NVS** with four scoped operations:

| Scope | Save | Load | Reset |
|-------|------|------|-------|
| **Full** | Entire device state → NVS + reboot | Reload all blobs (no reboot) | `pend_rst` flag + instant reboot → boot wipe |
| **Banks+Pats** | Pattern grids, user slots, bank deltas → NVS + reboot | Re-seed factory banks + overlay stored blobs | `pend_rst` flag + instant reboot → boot wipe |
| **Motion** | P-lock lanes → NVS + reboot | Clear matrix + overlay motion blob | NvsWorker erase + reboot |
| **Settings** | Mix, MIDI, laser, D-BEAM, songs → NVS + reboot | Reload settings blob only | NvsWorker factory settings + reboot |

**NVS keys:** `settings`, `patterns`, `banks`, `usrnames`, `usrpat`, `usrpatnames`, `motion`, plus control key `pend_rst` (u8) for deferred FULL/BANKS reset.

---

## 3. Connections & initial configuration

### 3.1 Requirements

- Octopus PRO XL (laser module + control enclosure)
- USB data cable (power + communication)
- Chromium-based browser (Chrome, Edge) for OctopusApp
- Optional: USB MIDI host for harp/seq/drum output routing

### 3.2 Power-on sequence

1. Connect USB. Firmware loads persisted settings from NVS and restores the last active dashboard (HARP or SEQUENCER).
2. The laser harp gate defaults to **closed** at boot; open explicitly with **OC long-press** on the HARP dashboard.
3. Open **[octopus.isystem.app](https://octopus.isystem.app)** — the App auto-connects when the Octopus USB MIDI port is detected (**Octopus ON** badge).

### 3.3 Factory reset at boot

To restore factory defaults:

1. Power off the device.
2. Press and hold **OC** and **SCALE** simultaneously.
3. Apply power while holding both buttons for approximately **150 ms** until the reset routine begins.
4. The unit arms a deferred reset, reboots, and on the next boot restores factory state before any audio or laser tasks start.

Runtime **Full Reset** and **Banks+Pats Reset** arm an NVS flag and reboot immediately (under ~200 ms). The actual wipe runs at the start of the next boot — the same safe window as the button combo above. **Settings Reset** and **Motion Clear** complete on the running device via a short NvsWorker commit, then reboot.

Scoped resets are also available under **RESET** in the main menu ([§8.14](#814-reset)).

### 3.4 App-connected mode

When OctopusApp maintains an active Web MIDI connection, the OLED displays **APP CONNECTED**. Hardware input is restricted to:

| Control | Assigned function |
|---------|-------------------|
| **SCALE** (short) | Play / Stop |
| **OC** (short) | Record arm toggle |
| **Encoder turn** | BPM adjustment (40–240) |
| **Encoder long-press** | No action (SAVE/LOAD use the App) |

Parameter editing is performed exclusively in the App during this mode, preventing concurrent modification of the same atomic parameters from two surfaces.

---

## 4. Control surface reference

The instrument exposes three physical controls. Gestures are **context-sensitive** according to dashboard (HARP vs SEQUENCER) and menu depth.

### 4.1 Gesture map

```mermaid
stateDiagram-v2
  [*] --> Dashboard
  Dashboard --> MenuL1: ENC click
  MenuL1 --> MenuL2: ENC click
  MenuL2 --> MenuL3: ENC click
  MenuL2 --> SpecialEditor: ENC click\n(Matrix / Edge / Song)
  MenuL3 --> MenuL2: ENC click
  MenuL2 --> MenuL1: ENC double
  MenuL1 --> Dashboard: ENC double
  Dashboard --> SaveMenu: ENC long
```

### 4.2 Rotary encoder (ENC)

| Gesture | Duration / pattern | Typical function |
|---------|-------------------|------------------|
| Turn | Linear 1:1 detents | Adjust value, scroll menu, browse presets |
| Click | Short press | Enter / confirm / descend menu level |
| Double-click | ~230 ms window | Navigate upward (L3→L2→L1→dashboard) |
| Long-press | ~600 ms | Open scoped **SAVE** menu |

### 4.3 SCALE button

| Gesture | HARP dashboard | SEQUENCER dashboard |
|---------|----------------|---------------------|
| Short | All-notes-off + advance scale | Toggle play / stop |
| Long (~550 ms) | Switch to SEQUENCER dashboard | Switch to HARP dashboard |
| Hold + encoder turn | Harp octave shift (±4 semitones) | — |

### 4.4 OC button (Open / Close)

| Gesture | HARP dashboard | SEQUENCER dashboard |
|---------|----------------|---------------------|
| Short | Cycle play mode: POLY8 → STRINGS → SOLO | Toggle record arm |
| Long (~800 ms) | Toggle laser harp gate (open ↔ close) | — |

### 4.5 Confirmation modals

Destructive operations present a **YES / NO** dialog. The default selection is **NO** to guard against accidental data loss.

- **Encoder turn** — move selection (left = NO, right = YES)
- **Encoder click** — execute selection
- **Encoder double-click** — cancel (equivalent to NO)

---

## 5. Operational dashboards

Long-press **SCALE** toggles between dashboards. Each dashboard provides at-a-glance status and direct encoder shortcuts before entering nested menus.

### 5.1 HARP dashboard

Displays active **scale**, **play mode**, **preset name**, **BPM**, and harp gate state (open / closed / animating).

| Objective | Procedure |
|-----------|-----------|
| Browse factory presets (128 named) | Turn encoder on dashboard |
| Select scale | Short **SCALE** |
| Shift harp octave | **SCALE** held + encoder turn |
| Change play mode | Short **OC** |
| Open / close laser gate | Long **OC** |
| Enter menu at HARP SETUP | Short encoder click |

#### Play mode characteristics

| Mode | Voice allocation | Timbre | Harp arp |
|------|------------------|--------|----------|
| **POLY8** | Up to 8 simultaneous notes (one voice per string) | Standard dual-osc synth | Enabled; latches held notes (poly stack) |
| **STRINGS** | Same polyphony with string-resonance model | Vibrato + staccato release profile | **Disabled automatically** |
| **SOLO** | Single note (king-of-stack priority) | Standard synth on solo voice | Enabled; follows solo voice latch |

```text
POLY8 holding C4 + E4 + G4:     C4 ─┐
                                   E4 ─┼─ all sound together
                                   G4 ─┘

SOLO same input:                  only the most recent note sounds

STRINGS:                          wobble + physical decay model (no arp)
```

### 5.2 SEQUENCER dashboard

Displays active **bank** (A–D), **BPM**, **pattern length**, transport state, and a page-aware step progress indicator.

| Objective | Procedure |
|-----------|-----------|
| Set tempo | Turn encoder |
| Play / stop | Short **SCALE** |
| Arm / disarm record | Short **OC** |
| Enter menu at SEQ SETUP | Short encoder click |

#### Parameter motion recording (P-locks)

1. Short **OC** to arm recording.
2. Short **SCALE** to begin playback.
3. Modify automatable parameters during playback (via menu or pre-connected App).
4. Short **OC** again to disarm.

Each pattern stores up to **four motion lanes** — per-step automation of parameters such as filter cutoff, send levels, or mixer values. Lanes replay during subsequent passes through the pattern.

---

## 6. Menu navigation model

### 6.1 Hierarchy

| Level | OLED content | Encoder turn | Encoder click |
|-------|--------------|--------------|---------------|
| **IDLE** | Dashboard | BPM or preset browse | Enter MENU L1 |
| **MENU L1** | 15 categories (performance order) | Scroll category | Enter MENU L2 |
| **MENU L2** | Items within category | Scroll item | Enter L3 or special editor |
| **MENU L3** | Live parameter value | Adjust value | Return to L2 |

Double-click the encoder at any level to step back toward the dashboard without saving.

### 6.2 Main menu order (on-device)

Categories appear in this sequence on the OLED:

```text
 1. HARP SETUP       9. MASTER
 2. HARP SYNTH       10. D-BEAM
 3. SEQ SETUP        11. MIDI I/O
 4. SEQ MATRIX       12. LASER SHOW
 5. SEQ SYNTH        13. TELEMETRY
 6. SONG             14. RESET
 7. DRUM KIT         15. SAVE
 8. AUX FX           16. LOAD
```

### 6.3 Full-screen editors

Certain L2 selections bypass the standard L3 value screen:

| Menu path | Editor function |
|-----------|-----------------|
| HARP SETUP → Edge Comp | Per-string trigger height (8 bars × 16 scales) |
| SEQ MATRIX → Open Grid | 16×8 step matrix with bank navigation |
| SONG (L2 enter) | Chain row editor (bank + repeats) |
| TELEMETRY (L2 enter) | Live diagnostic scope (7 views — see [§8.13](#813-telemetry)) |

---

## 7. Technical reference — layouts, curves & effects

This section documents the **musical and signal-processing semantics** underlying menu parameters — independent of whether you edit from hardware or OctopusApp.

### 7.1 Sequencer grid topology

The sequencer stores patterns as **64 steps × 16 rows** per bank. Hardware and App present a **16×8 window** at a time; position within the full 64 steps is selected by **page** (App: P1–P4). The hardware matrix editor currently covers **steps 1–16 only** — see [§12.C](#c-planned-upgrades-future-work).

#### Row assignment

```text
 Row (UI)   Engine row   Content
 ─────────────────────────────────────────
  1 –  8    0 –  7       Melody (scale degrees 1–8)
  9 – 16    8 – 15       Drums (Kick … Perc)

 Drum row map:
   9 → Kick    10 → Snare   11 → Clap    12 → HH Closed
  13 → HH Open 14 → Tom-H   15 → Tom-L   16 → Perc
```

#### Bank & page model

```mermaid
flowchart TB
  subgraph BankA["Bank A"]
    P1[Steps 1–16\nPage 1 / SYN or DRM view]
    P2[Steps 17–32]
    P3[Steps 33–48]
    P4[Steps 49–64]
  end
  P1 --- P2 --- P3 --- P4
```

| Concept | Range | Notes |
|---------|-------|-------|
| Banks | A–D (0–3) | Independent pattern + sound snapshot per bank |
| Pattern length | 16 / 32 / 48 / 64 | Steps beyond length are masked (shown crossed on OLED) |
| View S/D | Synth page / Drum page | Hardware matrix shows rows 1–8 or 9–16 |
| Chain index | Fixed 0 | v6.0 UI pins chain 0; song mode handles multi-bank playback |

#### Matrix editor navigation (hardware)

```text
         col 1    col 2   …   col 16
 row 1   [ ]      [■]         [ ]     ← OC/SCALE move vertically
 row 2   [ ]      [ ]         [■]     ← encoder moves horizontally
  …
 row 8   [■]      [ ]         [ ]

 • Horizontal wrap at column 16 → previous/next bank
 • Vertical wrap at row 1/8 → switch SYN/DRM page or adjacent bank
 • Encoder click → toggle step at cursor
```

### 7.2 Arpeggiator pattern reference

Both harp and sequencer arpeggiators share the **`arp::Engine`** core (`arp.h`). Notes are latched on input, sorted ascending for directional patterns, and retriggered at BPM-scaled periods.

#### Rate divisions

**Sequencer arp** — 8 rates (MIDI tick basis at 480 ticks/quarter):

| Index | Label | Ticks | At 120 BPM ≈ |
|-------|-------|-------|--------------|
| 0 | 1/1 | 1920 | 2.0 s |
| 1 | 1/2 | 960 | 1.0 s |
| 2 | 1/4 | 480 | 0.5 s |
| 3 | 1/8 | 240 | 0.25 s |
| 4 | 1/8T | 160 | triplet eighth |
| 5 | 1/16 | 120 | 0.125 s |
| 6 | 1/16T | 80 | triplet sixteenth |
| 7 | 1/32 | 60 | 0.0625 s |

**Harp arp** exposes indices 3–6 only (1/8, 1/8T, 1/16, 1/16T) — musically suited to live harp performance.

#### Gate length

Eight gate steps define note duty cycle as a percentage of each arp period:

```text
Gate index:  0    1    2    3    4    5    6    7
Duty %:     100   75   50   38   25   19   13    6

Visual (one period):  ████████░░░░  ≈ 50% gate
```

#### Pattern layouts (sequencer — 8 patterns)

Given latched notes **C4, E4, G4** (low → high):

| Pattern | Playback order | Diagram |
|---------|----------------|---------|
| **Up** | C → E → G → C … | `C E G C E G` |
| **Down** | G → E → C → G … | `G E C G E C` |
| **UpDn** | C → E → G → E → C … | `C E G E C` (ping-pong, no repeat top) |
| **DnUp** | G → E → C → E → G … | reverse ping-pong |
| **Rnd** | Pseudorandom index | `G C G E …` |
| **AsIs** | Order of input latch | preserves performance order |
| **Up+1** | Up pattern + octave | each step +12 semitones |
| **Dn-1** | Down pattern − octave | each step −12 semitones |

```text
UpDown cycle for 3 notes (indices 0,1,2,1):

  pitch
    G ─────●
    E ───●   ●───
    C ─●           ●─
       1  2  3  4  5  step
```

**Harp arp** maps UI indices 0–3 to **Up, Down, UpDn, Rnd** only. In **POLY8**, the engine latches all held strings; in **SOLO**, the king note defines the motif. Single-beam input expands to a **scale motif** so patterns remain musically distinct.

> **v6.0.01:** Harp arp maps **Up, Down, UpDn, Rnd** (see table above). The arpeggiator engine fixes (pitch-row laser sync, DnUp ping-pong, AsIs latch order) and the harp-arp ↔ App sync fix are in this release — see [CHANGELOG.md](./CHANGELOG.md#601--2026-06-22).

### 7.3 D-BEAM response curves

D-BEAM converts hand proximity (ADC, Kalman-filtered) into a normalised **0…1 expression** value. A **user-selectable curve** shapes sensitivity before routing to the target synth.

#### Curve equations (normalised input *x*)

| Curve | Transfer | Character | ASCII shape |
|-------|----------|-----------|-------------|
| **Linear** | *y = x* | Neutral, proportional | `/` |
| **Inverted** | *y = 1 − x* | Near = quiet, far = loud | `\` |
| **Exponential** | *y = x²* | Slow start, strong at close range | `_/` |
| **Logarithmic** | *y = √x* | Fast initial response, soft top | `/‾` |
| **Sigmoid** | *y = x²(3−2x)* | Soft knee near extremes (smoothstep) | `S` |

```text
Output
1.0 ┤           Linear ----
    │         _--    Sigmoid ···
    │       _-     Exp - - -
    │     _-    Log ·····
    │   _-
0.0 ┼────────────────────── Input (far → near)
    0.0                  1.0
```

#### Routing modes

| Route | Destination | Typical use |
|-------|-------------|-------------|
| **Off** | — | Bypass (Enable also required) |
| **Modulation** | LFO depth / timbral mod | Gestural swells |
| **Volume** | Amplitude (linear forced pre-curve) | Expressive dynamics |
| **Cutoff** | Filter cutoff | Brightness sweep |

**Target** selects **Harp Synth** or **Melody Synth** (sequencer). D-BEAM operates entirely in the **local DSP path** — no MIDI CC is transmitted.

**Calibration procedure:** Set **Offset** with hand clear of sensor; set **Range** at maximum intended playing distance; select curve and route; adjust **Env Atk / Rel** for smoothing.

### 7.4 Effects architecture & character guide

Each engine (harp, seq, drums) has **FX-A** (modulation/time) and **FX-B** (dynamics). All engines share a **global delay/reverb aux** (AUX FX menu) plus a **master bus** preset.

#### Insert FX-A (UI names → DSP mode)

Names are the abbreviated form of `INSERT_FX_PRESETS[]` in `effect.cpp` (1:1 with `kInsertFxNames[]` on the OLED and `INSERT_FX_NAMES` in OctopusApp); the index is the wire value, so the DSP mode below is fixed per row.

| # | Name | Engine type | Character |
|---|------|-------------|-----------|
| 0 | Nebula Taps | Bypass + wet sends | Delay/reverb wash; neutral insert |
| 1 | Snova Chorus | Chorus | Wide stereo swell, moderate depth |
| 2 | Pulsar Mod | Ring mod | Metallic sideband bell tones |
| 3 | Quasar Phase | Phaser | Notched sweep + reverb tail |
| 4 | Chronos Echo | Bypass + delay | Tempo-agnostic echo emphasis |
| 5 | Singul Tube | Distortion | Aggressive tube-style saturation |
| 6 | Jet Flange | Flanger | Jet comb filtering |
| 7 | Astral Shmr | Chorus | Shimmer verb blend |
| 8 | Dark SubRoom | Bypass + verb | Large dark reverb room |
| 9 | Cosmos Tape | Bypass + delay | Tape-style echo |
| 10 | Hyper ResMod | Ring mod | Higher carrier shimmer |
| 11 | Vortex Swirl | Phaser | Comb swirl, moderate mix |
| 12 | **Organic Drive** | Distortion | **Organic drum/melody drive** — warm even harmonics, strong on transients |
| 13 | Aether Gate | Bypass + verb | Bright shimmer gate |
| 14 | Void Satur | Distortion | Heavy saturation + room |
| 15 | Zero Quantum | Ring mod | Extreme carrier/detune |

**Organic Drive** (index 12): optimised for **musical saturation** on drums and melodic sources (drive ~2.6, tone ~0.68, mix ~0.44 in the preset table).

#### Insert FX-B — dynamics

| # | Name | Type | Application |
|---|------|------|-------------|
| 0 | Dyn Byp | Off | Transparent |
| 1 | Glue Comp | Compressor 4:1 | Gentle bus glue |
| 2 | Punch Comp | Compressor 6:1 | Transient emphasis |
| 3 | Soft Lim | Limiter | Smooth peak control |
| 4 | Brick Lim | Limiter | Hard ceiling |
| 5 | Noise Gate | Gate | Suppress bleed |
| 6 | Tight Gate | Gate | Higher threshold gate |
| 7 | Trans Pun | Transient shaper | Attack enhancement |
| 8 | Snap Atk | Transient | Fast snap |
| 9 | Drum Smk | Transient | Drum-focused punch |
| 10 | Bus Glue | Compressor | Mix bus |
| 11 | Vocal Ride | Compressor | Slow release leveling |
| 12 | Harp Sus | Compressor | Sustained harp smoothing |
| 13 | Seq Pump | Compressor 8:1 | Rhythmic ducking / pump |
| 14 | Sub Gate | Gate | Sub-only isolation |
| 15 | Max Safe | Limiter | Safety ceiling |

#### Master FX presets (selection)

| Preset | Role |
|--------|------|
| Aether Bypass | Clean pass-through |
| Galactic Bus | Gentle EQ lift |
| Magnetar Sat / Pulsar Impact | Increasing saturation + EQ |
| LPF Deep Sweep / HPF Prism Sweep | DJ-style filter motion |
| Nova OD / Quantum Tube | Master overdrive tones |
| Centauri Master | Polished final bus |

Master **tube** (TB Drive/Tone/Mix), **DJ filter**, and **EQ** are also available as continuous parameters under **MASTER**.

### 7.5 Song mode playback model

Song mode replaces single-bank loop behaviour with an ordered **chain** of bank references.

```mermaid
flowchart LR
  S1["Step 1\nBank A ×4"] --> S2["Step 2\nBank C ×2"]
  S2 --> S3["Step 3\nBank D ×8"]
  S3 --> S1
```

| Field | Range | Meaning |
|-------|-------|---------|
| Bank | A–D | Pattern bank to play |
| Repeats | 1–15 | Loop count before advancing |
| Song slots | 0–15 | Independent chain storage |
| Max rows | 16 per slot | Append rows in hardware song editor |

Enable song mode and select slot from **OctopusApp** (SEQUENCER toolbar) or persist via saved settings. Hardware **SONG** menu edits row content.

### 7.6 Scale system

Sixteen scales map eight strings to pitch classes. Standard scales share uniform RGB; **Rainbow Major / Minor** assign **per-string colours** and independent edge-compensation tables.

| # | Name | Type |
|---|------|------|
| 01 | Major | Diatonic |
| 02 | Minor | Diatonic |
| 03 | Pentatonic | 5-note |
| 04 | Blues | Blues scale |
| 05–08 | Dorian … Mixolydian | Modes |
| 09 | Locrian | Mode |
| 10–11 | Harmonic / Melodic Minor | Minor variants |
| 12–13 | Spanish / Arabic | Colour scales |
| 14 | Chromatic | Semitone |
| 15–16 | Rainbow Maj / Min | Per-string hue + edge tables |

---

## 8. Main menu reference

Complete operational index for hardware menus. Enter via **encoder click** from dashboard; category order matches OLED.

---

### 8.1 HARP SETUP

Per-scale beam triggering, colour, and idle behaviour (13 items).

| Item | Function | Adjustment notes |
|------|----------|------------------|
| Gate Hold | Note-on debounce (ms) | Increase in noisy environments |
| White Lvl | White-point laser intensity | Per-scale visibility tuning |
| Touch On / Touch Off | Beam confirm thresholds | Balance sensitivity vs false triggers |
| Beam Red / Green / Blue | Scale colour mix | RGB 0–255 per scale |
| Margin | Global trigger margin (DAC) | Primary sensitivity control |
| Stuck Rel | Auto-release timeout (ms) | Safety for blocked beams |
| Edge Comp | Per-string fine trigger (%) | Opens full-screen 8-bar editor |
| Fog Reject | Haze/fog false-trigger filter | Enable in foggy venues |
| Fog Margin | Differential reject threshold | Tune with Fog Reject on |
| Screensvr | Idle animation when harp closed | Independent of LASER SHOW |

**Edge Comp editor:** SCALE = next string; OC = next scale (live); encoder = value; encoder click = factory reset for string; encoder long = save & exit.

---

### 8.2 HARP SYNTH

25-parameter harp synthesizer including arpeggiator.

| Items | Parameters |
|-------|------------|
| 0–13 | Waveform, ADSR, filter, noise, detune, LFO (rate/depth/route), osc2, env→cutoff |
| 14–15 | FX-A / FX-B slot selection |
| 16–17 | Delay / reverb send |
| 18 | Snd Preset — 128-name factory bank |
| 19–20 | Save Slot / Load Slot (64 user slots) |
| 21–24 | H Arp On, Pat, Rate, Gate |

**LFO routes:** Pitch · Filter · Wave · Ptch+Flt · Flt+Wave · Ptch+Wav · All 3 · Tremolo

---

### 8.3 SEQ SETUP

Pattern management and sequencer arp (**13 items**).

| Item | Function |
|------|----------|
| Bank A-D | Active bank select |
| View S/D | Synth vs drum matrix page |
| Transpose | ±12 semitones |
| Length | 16 / 32 / 48 / 64 steps |
| Load Synth / Load Drum | Factory pattern recall |
| Clear | Wipe active bank (confirm) |
| Save Pat / Load Pat | 64 user pattern slots |
| Arp On / Type / Rate / Gate | Sequencer arpeggiator ([§7.2](#72-arpeggiator-pattern-reference)) |

---

### 8.4 SEQ MATRIX

| Item | Function |
|------|----------|
| Open Grid | Enter 16×8 step editor ([§7.1](#71-sequencer-grid-topology)) |

---

### 8.5 SEQ SYNTH

Melody engine parameters — **21 items** (synth core + preset/slots; no harp arp entries). Parameter set mirrors HARP SYNTH items 0–20.

---

### 8.6 SONG

| Item | Function |
|------|----------|
| *(L2 enter)* | Row editor: BANK + REPEATS per step ([§7.5](#75-song-mode-playback-model)) |

**Controls:** encoder turn = edit value; SCALE / click = move cursor; OC = append row; OC+SCALE = delete row.

---

### 8.7 DRUM KIT

40 voice parameters + kit + pitch.

**Voices:** Kick, Snare, Clap, HH-C, HH-O, Tom-H, Tom-L, Perc — each with Tune, Decay, Vol, Noise, Wave (Clap/HH: noise-only).

| Item | Function |
|------|----------|
| Drum Kit | TR-909 · TR-808 · Trap · House |
| Drm Pitch | Global drum tuning (semitone-linear) |

---

### 8.8 AUX FX

Shared delay/reverb and insert routing (14 items): Dly Time, Dly FB, Rev Size, Rev Damp, H/S harp+seq sends, Harp/Seq/Drum FX-A/B selectors.

---

### 8.9 MASTER

24 items: M.Vol, H/S/D Vol, M.Pitch, FX Preset, Drum Rev/Dly, Tube, DJ filter, EQ, Drum FX slots, mutes (L2 click toggle), H/S/D Pan.

---

### 8.10 D-BEAM

Offset, Range, Curve, Enable, Env Atk/Rel, Route, Target — see [§7.3](#73-d-beam-response-curves).

---

### 8.11 MIDI I/O

PB Range, PB Enable (L2 toggle), Harp Ch, Seq Ch, Drum Ch — USB MIDI channel routing.

---

### 8.12 LASER SHOW

Show Mode, MIDI Hue, Base Hue, Anim Mode (Pulse/Chase/Strobe/Wave), Drum Flash, Hue ADSR.

---

### 8.13 TELEMETRY

Seven diagnostic views. From **MENU L1 → TELEMETRY**, scroll **MENU L2** with the encoder, then **encoder click** opens the selected view. While a view is active:

| Control | Function |
|---------|----------|
| Encoder turn | Cycle views 1–7 (wraps) |
| Encoder click | Exit to dashboard |
| Encoder double-click | Exit to dashboard |

| L2 item | On-screen content |
|---------|-------------------|
| **AC Scope** | AC-coupled beam amplitude trace (true RMS above noise floor) |
| **DC Bias** | Average DC servo bias (volts + raw ADC counts, mid-rail ≈ 1.65 V) |
| **DAC Thresh** | Eight per-string AGC threshold bars (12-bit DAC scale 0–4095) |
| **D-BEAM Expr** | Post-curve expression magnitude (0–16383) |
| **SNR** | Signal-to-noise ratio trace (`maMv / noise floor`) |
| **System** | CPU load %, DRAM/PSRAM free, D-BEAM carrier freq, stack low warning |
| **Fog Reject** | Eight amplitude bars with floor + accept lines (margin tuning aid) |

The OLED refreshes continuously (~30 Hz) while any telemetry view is open. **System** stats update every 5 s. Scope traces (AC, D-BEAM, SNR) auto-range to recent peaks.

---

### 8.14 RESET

Four scopes mirror SAVE/LOAD. L2 click → confirm → execute.

| Scope | Runtime behaviour |
|-------|-------------------|
| **Full Reset** | Arms NVS `pend_rst` → immediate reboot → boot kernel wipes all blobs + factory settings |
| **Banks+Pats** | Same deferred path — clears patterns, banks, user slots; keeps settings & motion |
| **Motion Clr** | Clears P-lock matrix in RAM + NVS via NvsWorker → reboot |
| **Settings** | Factory knob/mixer/laser defaults in RAM + NVS via NvsWorker → reboot |

After reboot the OLED returns to the normal dashboard; OctopusApp reloads and re-imports via `APP_SYNC_REQ`.

---

### 8.15 SAVE / 8.16 LOAD

Same four scopes. **SAVE** writes current RAM to NVS via NvsWorker (+ reboot). **LOAD** reloads from NVS into RAM without reboot (App receives full state echo).

---

## 9. OctopusApp companion

Open **[octopus.isystem.app](https://octopus.isystem.app)** in Chrome or Edge over **HTTPS**. The App auto-connects over USB MIDI SysEx (`0x7D` host→device, `0x7C` device→host). After SAVE, LOAD, or RESET the page reloads and re-imports the full device state.

### 9.1 View structure

| Tab | Content |
|-----|---------|
| **INSTRUMENTS** | Laser show, D-BEAM, harp synth, seq synth, drum scope |
| **MIXER** | Levels, pans, mutes, master processing, insert wet controls |
| **SEQUENCER** | 64-step grid (P1–P4), banks, song editor, pattern loaders, arp |

### 9.2 Transport & authority

Header **PLAY / STOP / REC** and **BPM** field are **read-only reflectors**. Hardware **SCALE**, **OC**, and encoder own transport. The App supervisor re-asserts device state to prevent UI drift.

### 9.3 Utility bar

SAVE · LOAD · RESET · SLOTS · CPY/PST · RND-H/RND-D · CLR · mutes · DBEAM · MIDI routing · MON · HELP

---

## 10. Recommended workflows

### 10.1 Live performance (hardware-centric)

1. HARP dashboard → select scale and preset → long **OC** to open gate.
2. Long **SCALE** → SEQUENCER → set BPM → short **SCALE** to start backing.
3. Long **SCALE** → return to harp for solo sections.

### 10.2 Pattern authoring (App + hardware)

1. Connect OctopusApp; edit grid on SEQUENCER tab (P1–P4 pages).
2. Disconnect or use hardware transport; arm record; capture P-locks during playback.
3. **SAVE → Full Save** before power cycle.

### 10.3 Sound library management

1. Sculpt patch in HARP SYNTH or SEQ SYNTH.
2. **Save Slot** → select index → confirm.
3. Name slot in App **SLOTS** modal for recall clarity.

---

## 11. Diagnostics & troubleshooting

| Symptom | Recommended action |
|---------|-------------------|
| No harp audio | Verify gate open (OC long); check H.Vol, Harp Mute |
| Erratic triggering | Enable Fog Reject; adjust Margin / Edge Comp |
| App parameters inactive | Expected in APP CONNECTED mode for non-transport controls — use App UI |
| BPM uneditable in App | By design — adjust on hardware encoder |
| Lost pattern after power | Perform SAVE → Full Save |
| Stuck notes | Short SCALE on HARP dashboard (panic) |
| Factory restore | Boot with OC+SCALE held, or RESET → Full Reset (instant reboot, wipe on next boot) |

---

## 12. Appendices

### A. User storage map

| Store | Count | Contents |
|-------|-------|----------|
| Factory presets | 128 | Indices 0–127 in `userBank[]` / `seqBank[]` |
| User sound slots | 64 × 2 engines | Harp + seq patches at indices 128–191 |
| User pattern slots | 64 | Separate `g_userPat[]` library (not in bank array) |
| Song slots | 16 | Chain programs |
| Pattern banks | 4 | Live working sets A–D |

### B. Wavetable index (25)

Cosmic Saw · Quantum Sq · Pulsar 25% · Stellar Tri · Nebula Organ · Astral Vocal · Chrono Bell · Aether String · Singular Sine · Pulsar 10% · Pulsar 40% · Hyper Glass · Cygnus Tine · Vortex Clav · Void Choir · Reso Quark · Photon Reed · Warp Cello · Nova Harm · Event Growl · Solar Flute · Plasma Pad · Moog Gravity · Meteor Tabla · Deep Drone

### C. Planned upgrades (future work)

Authoritative list: **`code_info.h` §9**. Targets for the next upgrade:

1. **Hardware matrix step pages** — on-device editing for steps 17–64 (App P1–P4 today).
2. **OLED P-lock lane editor** — full-screen motion lane editing on hardware.
3. **External MIDI OUT** — channel-voice output via a WiFi/BLE coprocessor path.
4. **OctopusApp motion-matrix editor** — per-step P-lock editing in the browser.

### D. Document revision

| Version | Date | Notes |
|---------|------|-------|
| 1.0 | 2026-06-20 | Initial v6.0 manual — architecture diagrams, arp/FX/D-BEAM reference |
| 1.1 | 2026-06-23 | v6.1.00 — deferred boot reset (FULL/BANKS+PATS), auto-connect App, 7-view TELEMETRY |

---

*Octopus PRO XL v6.1 — © DIODAC ELECTRONICS / iSystem. Firmware labels: `display.h`. Protocol: `sysex.h`.*
