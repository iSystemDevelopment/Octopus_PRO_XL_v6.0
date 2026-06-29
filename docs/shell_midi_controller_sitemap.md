# Sitemap — Octopus MIDI Controller shell

**Status: Production Ready** (v6.6.01 — pulpit layout, dual-shell separation verified)  
**Playhead policy:** [playhead_policy_audit.md](playhead_policy_audit.md) §3.2 + checklist **B**

Product label in UI: **OCTOPUS MIDI CONTROLLER**  
Runtime: `_appMode === 'midi'` · `data-app-mode="midi"`  
Sequencer state object: `app._midiSeq` via `app._midiShell()`

## Layout — hardware pulpit (single page)

No separate INSTRUMENTS tab. One view: `#view-midi-seq`.

```
┌─────────────────────────────────────────────────────────────────┐
│ MASTER BAR — brand · SEQUENCER tab · transport · BPM · port    │
├─────────────────────────────────────────────────────────────────┤
│ UTILITY HEADER — 4 groups (full width, space-between)           │
│  [LEN TRN OCT]  [ARP + pat/rate/gate]  [JSON MID IMP]  [CLK…MON]│
├─────────────────────────────────────────────────────────────────┤
│ SEQUENCER CONTROL — BANK · P1–P4 · EDIT · SONG · SCALE · tools   │
├──────────────────────────────┬──────────────────────────────────┤
│ INSTRUMENTS 55% (+30%)       │ SEQUENCER MATRIX 45% (−30%)      │
│ ┌ SEQ SYNTH 24×CC ─────────┐ │ 16×16 grid + playhead            │
│ └──────────────────────────┘ │ song editor (EDIT mode)          │
│ ┌ DRUM NOTE MAP + scope ────┐ │                                  │
│ └──────────────────────────┘ │                                  │
└──────────────────────────────┴──────────────────────────────────┘
```

## Header 1 — Master bar (midi-only elements)

| Element | ID | Role |
|---------|-----|------|
| Play / Stop / Rec | `midiBtnPlay`, `midiBtnStop`, `midiBtnRec` | Local transport |
| BPM | `midiBpm` / `midiBpmLbl` + ◀ ▶ (`stepBpm`) | Editable 40–240 |
| MON | master bar, beside `midiOut` | Activity monitor (not utility row) |
| Port / badge | `midiOut`, `connBadge` | **MIDI OUT** when live |

Tab **INSTRUMENTS** hidden in MIDI mode. Only **SEQUENCER** tab (pulpit page).

## Header 2 — Utility (`midi-hdr-stack`)

| Group | Controls |
|-------|----------|
| PITCH | `midiSeqLenLbl`, `midiSeqLen`, `midiSeqTrnLbl`, `midiSeqTrn`, `midiSeqOctLbl`, `midiSeqOct` |
| ARP | `btnMidiArp`, `midiArpPat`, `midiArpRate`, `midiArpGate` |
| FILE | JSON / MID / IMP (`midiImportFile`) |
| MIDI | `btnMidiClockOut`, `midiMelodyCh`, `midiDrumCh`, `midiMelPc`, `midiDrumPc`, MON |

## Header 3 — Sequencer control (`#midi-seq-matrix-bar`)

| Group | Controls |
|-------|----------|
| Bank | `midiBtnBankA`–`D`, `midiBtnBankChain` |
| Pages + song | `midiBtnPage1`–`4`, LEDs, `midiBtnSeqMode`, **`midiSongSlotSelect`** (SONG 01–16) |
| Patterns + tools | `midiScale`, `midiSynthPat`, `midiDrumPat`, CPY/PST/CLR/RND-S/RND-D, `midiRandDensity` |

## DOM — pulpit body

| Region | IDs |
|--------|-----|
| CC panel (top) | `#midi-seq-knobs` |
| Drum map (bottom) | `#midi-drum-notes`, `midiGlobalPitch`, `#midi-dual-scope-canvas` |
| Grid | `#midi-seq-grid`, `#midi-seq-playhead-layer`, `#midi-seq-playhead` |
| Song editor | `#midi-song-editor`, `#midi-song-chain-rows` |

Factory map: `SEQ_IDS_MIDI` in `OctopusApp.html`.

## Row labels (melody rows 1–8)

Labels follow **SCALE + TRN + OCT + global pitch** via `_midiMelodyNoteForRow(row, true)`.  
Bug fix v6.6: must read `midiSeqTrn` / `midiSeqOct`, not DSP fields.

## Persistence

| Store | API |
|-------|-----|
| MIDI session | `_persistMidiSession()` / `_loadMidiSession()` |
| Factory demo | `MIDI_FACTORY_DEMO_KEY` |

## Connect / disconnect

| Event | Result |
|-------|--------|
| Generic MIDI port (no ★) | `setAppMode('midi')` → pulpit view |
| Unplug / disconnect | `setAppMode('disconnected')` → **Octopus DSP Engine** tab INSTRUMENTS (`#view-synths`) |
| ★ Octopus port | `setAppMode('dsp')` → DSP 3-tab shell |

## Verification checklist

- [ ] Pulpit: ~33% grid right, ~67% instruments left
- [ ] TRN/OCT change updates synth row note names (not stuck D4–D5)
- [ ] MIDI disconnect → DSP INSTRUMENTS (not MIDI grid)
- [ ] SONG dropdown shows SONG 01 … SONG 16
- [ ] DSP grid unchanged after MIDI editing session

## Next

Octopus DSP Engine shell — separate visual pass (see `shell_dsp_engine_sitemap.md`).
