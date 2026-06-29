# Sitemap — Octopus DSP Engine shell

Product label in UI: **OCTOPUS DSP ENGINE**  
Runtime: `_appMode === 'dsp'` or `'disconnected'` · `data-app-mode="dsp"`  
Sequencer state object: `app._dspSeq` via `app._dspShell()`

## Header (dsp-only regions)

| Element | ID | Role |
|---------|-----|------|
| Play / Stop / Rec | `dspBtnPlay`, `dspBtnStop`, `dspBtnRec` | Hardware transport (SysEx echo) |
| BPM (read-only) | `dspBpm` | Displays device `CMD.BPM` |
| Session slot | `sessionSlot` | SESSION 1–16 |
| Save / Export / Import | SAVE btn, `octopusImportFile` | Browser session JSON |
| CPU load | `cpuLoad` | Device audio-core % |
| SysEx monitor | MON button → `monitorModal` | Link debug |

**Shared (mode selector only):** `midiOut`, `connBadge` (shows DSP Off / DSP SYNC / DSP ON)

## Tabs → views

| Tab | View ID | Content |
|-----|---------|---------|
| INSTRUMENTS | `#view-synths` | Harp, Seq synth, Drum voices (knob panels lazy-built) |
| MIXER | `#view-mixer` | Master bus, drum scope, laser, D-BEAM, perf split |
| SEQUENCER | `#view-seq` | Pattern grid + PATIMAGE + song editor |

## Sequencer view (`#view-seq`)

| Region | IDs | Notes |
|--------|-----|-------|
| Toolbar | `btnBankA`–`D`, `btnBankChain`, `btnPage1`–`4`, `btnSeqMode` | Bank / page / EDIT |
| LEDs | `led-0` … `led-3` | Page indicators |
| PATIMAGE | `patimageSlot`, synth/drum factory `<select>` | Saves to PATIMAGE store |
| Grid tools | CPY, PST, CLR, RND-S, RND-D, `randDensity` | Active P page only |
| LEN / TRN | `.dsp-len-lbl`, `.dsp-trn-lbl`, `dspSeqLen`, `dspSeqTrn` | Tx `HW_S_LEN`, `TRANSPOSE` |
| Grid stage | `#seq-grid-stage`, `#seq-grid`, `#seq-playhead-layer`, `#seq-playhead` | 16×64 matrix · playhead audit: [playhead_policy_audit.md](playhead_policy_audit.md) §3.1 |
| Song editor | `#song-editor`, `#song-chain-rows`, `songNumSteps` | Chain A→D |

Factory: `SEQ_IDS_DSP` in `OctopusApp.html` maps all ids for `createSeqShell()`.

## In-memory model (`_dspSeq`)

```
gridData[4][16][64]   bankIdx, gridPageIdx, seqLen, visStep
songData[16]          isSongMode, showSongEditor, songSlotIdx, songPlayStep
clipboardGrid         playhead cache (_phLayout, _domGridCache, …)
```

## Persistence

| Store | Key / API | Contents |
|-------|-----------|----------|
| Session slots | `SessionStore` + `OCTOPUS_SESSION_STORE` | Full workspace bundle |
| PATIMAGE | `OCTOPUS_PATIMAGE_STORE` | Grid + chain snapshots |
| Sound images | per-engine keys | Harp / seq / drum / mix knobs |

Serialize path: `SessionStore.serializeSession` → `_dsp(app).gridData`.

## Hardware link

- `APP_SYNC_REQ`, heartbeat `PING`, `_syncBurstExpected`
- Grid TX: `_flushGridToDevice()` → `_dspShell().gridData`
- RX dispatch: `STEP_SYNC`, `HW_S_LEN`, `SONG_*` → `_dspShell()` only
- Outbound firewall: `txParam` no-op when `_appMode === 'midi'`

## Lazy DOM

`_ensureDspKnobs()` builds laser / harp / seq / drum / master panels once on first DSP shell entry.

## Disconnect / off

`setAppMode('disconnected')` → DSP shell visible, tab INSTRUMENTS, badge **DSP Off**, no SysEx burst.
