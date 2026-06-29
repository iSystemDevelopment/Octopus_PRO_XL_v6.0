# UI flowcharts — Hardware OLED · OctopusApp · MIDI mode

**Build reference:** Firmware **6.1.01** · OctopusApp **6.6.01** · June 2026  
**Normative hardware wireframe:** [`oled_ui_wireframe_v6.6.md`](oled_ui_wireframe_v6.6.md)  
**Normative App inventory:** [`v6.3.00.md`](../v6.3.00.md) §14.4  
**Transport / playhead:** [`mirror_architecture.md`](mirror_architecture.md) (DSP) · [`playhead_policy_audit.md`](playhead_policy_audit.md) (DSP + MIDI verification)

---

## 1. Hardware — menu navigation

Encoder turn = move cursor · click = enter/confirm · double-click = back to parent · **SCALE** / **OC** = context actions on dashboards.

```mermaid
flowchart TD
    BOOT[Power / boot] --> IDLE{Active dashboard}
    IDLE -->|SCALE long| HARP[HARP PLAY dashboard]
    IDLE -->|SCALE long| SEQ[SEQ PLAY dashboard]
    HARP -->|Encoder click / menu key| L1[L1 MAIN MENU]
    SEQ -->|Encoder click / menu key| L1

    L1 --> HPLAY[HARP PLAY]
    L1 --> HTONE[HARP TONE]
    L1 --> SPLAY[SEQ PLAY]
    L1 --> SMAT[SEQ MATRIX]
    L1 --> DRUM[DRUM]
    L1 --> MIX[MIX LIVE]
    L1 --> DBEAM[D-BEAM]
    L1 --> PERF[PERF SLOT]
    L1 --> SYS[SYSTEM]

    HTONE --> L2H[L2: TONE / ENV / FILTER / MOD / ARP]
    L2H --> L3H[L3 parameters]
    L3H -->|double-click| HTONE

    DRUM --> L2D[L2: Kit · Vol · Pitch · 8 waves]
    L2D --> L3D[L3 per voice]
    L3D -->|double-click| DRUM

    MIX --> L2M[L2: H/S/D vol · mutes · M.FX]
    DBEAM --> L2B[L2: Route · Curve · Offset · Range]
    PERF --> L2P[L2: Load PERF 1–8 · Save · Name]
    SYS --> L2S[L2: MIDI I/O · Telemetry · Factory reset]
    L2S --> SVC[Service: Laser show · Fog · Edge]

    HPLAY -->|double-click| IDLE
    SPLAY -->|double-click| IDLE
    SMAT -->|double-click| L1
    L2H -->|double-click| L1
    L2D -->|double-click| L1
    L2M -->|double-click| L1
    L2B -->|double-click| L1
    L2P -->|double-click| L1
    L2S -->|double-click| L1
```

| Gesture | Any screen |
|---------|------------|
| Encoder turn | Move selection / adjust value (L3) |
| Click | Enter submenu or confirm L3 edit |
| Double-click | Back one level (L3→L2→L1→dashboard) |
| SCALE (dashboard) | Scale / panic (HARP) or play-stop (SEQ) |
| OC long (HARP) | Laser gate open/close |
| OC short | Play mode / record arm (context) |

---

## 2. Hardware — performance surfaces & matrix

Two surfaces users live in during a gig; matrix is the patimage faceplate.

```mermaid
flowchart LR
    subgraph HARP_SURFACE["HARP PLAY"]
        HG[Gate OPEN/CLOSE]
        HS[Sound from SongPack list]
        HO[Scale · Octave · POLY/STR/SOLO]
        HB[D-BEAM bar]
    end

    subgraph SEQ_SURFACE["SEQ PLAY"]
        SB[Bank A–D · LEN · BPM · TRSP]
        SM[H/S/D mute indicators]
        SP[Step bar playhead]
        SK[Kit tag in header]
    end

    subgraph MATRIX["SEQ MATRIX"]
        MV[VIEW mode — read-only patimage]
        MT[TWEAK mode — step on/off toggle]
        MW[Wrap: bank A–D vertical · P1–P4 horizontal]
    end

    HARP_SURFACE -->|MENU → HARP TONE| DEEP1[Deep edit L2/L3]
    SEQ_SURFACE -->|MENU → SEQ MATRIX| MATRIX
    MATRIX -->|hold OC / latch| MT
    MT -->|exit + optional Save PERF?| SEQ_SURFACE
    SEQ_SURFACE -->|chain display only| APPCHAIN[Full chain edit in OctopusApp]
```

| Mode | Matrix cells | Edit authority |
|------|----------------|----------------|
| **VIEW** | Show `hwSeqData` / SongPack patimage | Hardware read-only |
| **TWEAK** | Toggle step on/off | RAM → optional PERF save on exit |
| **App SEQUENCER** | Full 64×16 editor | Browser SESSION / PATIMAGE 1–16 |

**Not on hardware L1:** song chain editor, SESSION 1–16 names, full synth walls, App-only automation.

---

## 3. OctopusApp — extended studio flow (v6.6)

Octopus-first shell. MIDI Controller UI deferred in 6.6 but code path remains.

```mermaid
flowchart TD
    OPEN[Open octopus.isystem.app HTTPS] --> SCAN[Web MIDI scan]
    SCAN -->|★ Octopus port| AUTO[Auto-connect Octopus]
    SCAN -->|No ★ port| OFF[Octopus Off shell]
    AUTO --> BADGE{Link badge}
    BADGE -->|importing| SYNC[Octopus SYNC orange]
    BADGE -->|mirror live| ON[Octopus ON green]
    SYNC --> ON

    ON --> TABS[Master bar]

    subgraph MASTER["Master bar"]
        MB[Logo → octopus-info]
        MT[INSTRUMENTS · MIXER · SEQUENCER · TIP · HELP]
        MTR[▶ ■ REC reflectors]
        MS[SESSION 1–16 · SAVE · EXP · IMP]
        MBPM[BPM glow readout]
        MCPU[CPU · MON]
        MCONN[Port · badge · connector]
    end

    TABS --> V1[INSTRUMENTS]
    TABS --> V2[MIXER]
    TABS --> V3[SEQUENCER]

    V1 --> HARP[HARP · D-BEAM · OCT stepper · harp-image]
    V1 --> SEQENG[SEQ synth · OCT/TRN · melody-image]
    V1 --> DRUMV[AB 9-09 · 24 kits · drum-image]

    V2 --> BUS[AB 9-09 bus inline · FX A/B]
    V2 --> STU[Studio console · USB/MIDI→STUDIO]
    V2 --> SCOPE[Scope/VU · dark when stopped]
    V2 --> LASER[Laser · D-BEAM cal · mix-image]

    V3 --> BAR[Seq matrix bar = grid width]
    V3 --> GRID[3/4 grid · STEP_SYNC playhead layer]
    V3 --> SIDE[1/4 sidebar · seq knobs + drum kit]
    V3 --> EDIT[EDIT → Bank manager · PATIMAGE · PACK FOR HW]

  subgraph PERSIST["Browser persistence"]
        SES[SESSION 1–16 SessionBundle]
        PAT[PATIMAGE 1–16]
        IMG[sound-images harp/melody/drum/mix]
    end

    V1 & V2 & V3 --> PERSIST
    ON -->|SysEx echo| MIRROR[Mirror: transport · knobs · grid]
    MIRROR -->|STEP_SYNC only| PH[Playhead compositor layer]
```

| Layer | User action | Storage |
|-------|-------------|---------|
| **SESSION** | SAVE / EXP / IMP | `octopusapp_session_bundle_v1` |
| **PATIMAGE** | SAVE on seq bar / slot 16 demo | `octopusapp_patimage_v1` |
| **Sound image** | IMG + dropdown recall | `octopusapp_sound_images_v1` |
| **Device PERF** | PACK FOR HW → SongPack | Device NVS (not browser) |

Factory tutorials: **HELP → Reload tutorial examples** · **PATIMAGE 16** · see [`examples/octopusapp/README.md`](../examples/octopusapp/README.md).

---

## 4. OctopusApp — MIDI Controller mode

> **v6.6.01:** MIDI Controller pulpit is **shipped** — unplug ★ Octopus or pick a non-★ port. HELP → **OCTOPUS MIDI CONTROLLER** for setup.

```mermaid
flowchart TD
    OPEN[Open App · no ★ Octopus] --> PICK[Pick MIDI output port]
    PICK --> MIDIOUT[Click MIDI OUT or auto-adopt]
    MIDIOUT --> MODE[data-app-mode = midi]

    MODE --> MB2[Master bar: transport active · BPM editable]
    MODE --> UTIL[Utility bar: routing · ARP · CLK]

    subgraph CH["Channels 1–16 per instrument"]
        MCH[Melody CH 1–16]
        DCH[Drums CH 1–16]
        MPC[Program Change optional]
        CC[8 CC knobs → melody CH]
    end

    UTIL --> CH
    MODE --> T1[INSTRUMENTS: Seq MIDI panel + Drum MIDI panel]
    MODE --> T3[SEQUENCER: 64-step grid]

    T3 --> CLK[App-owned MIDI clock optional]
    T3 --> PLAY[▶ starts local clock]
    PLAY --> NOTES[Melody rows → melody CH · Drum rows → drum CH]
    NOTES --> ARP[ARP expands melody per step]
    T3 --> MOTION[REC + purple dots = CC motion per step]

    MODE --> STORE[localStorage midi_session_v1]
    STORE --> EXP2[No SESSION / PATIMAGE / SysEx / laser / D-BEAM]
```

| Setting | Default | Range |
|---------|---------|-------|
| Melody channel | 1 | **1–16** |
| Drum channel | 10 (GM convention) | **1–16** |
| Melody PC | 0 | 0–127 |
| Drum PC | 0 (optional) | 0–127 |
| Drum notes | GM map 36,38,… | per-row editable |

**Octopus vs MIDI:** never both active — ★ port forces Octopus mode and tears down MIDI clock.
