# Two-shell contract — OctopusApp v6.6.01+

One HTML file, **two product shells**. Never mix UI, state, or persistence between them.

| Product name (UI) | Runtime mode | `data-app-mode` | CSS gate |
|-------------------|--------------|-----------------|----------|
| **Octopus DSP Engine** | `dsp` or `disconnected` | `dsp` | `.dsp-only` |
| **Octopus MIDI Controller** | `midi` | `midi` | `.midi-only` |

`disconnected` uses the **DSP Engine** chrome (preview / no ★ port) but does not sync hardware.

## Sitemaps (authoritative)

- [shell_dsp_engine_sitemap.md](shell_dsp_engine_sitemap.md) — views, DOM ids, `_dspSeq`, SysEx, sessions
- [shell_midi_controller_sitemap.md](shell_midi_controller_sitemap.md) — views, DOM ids, `_midiSeq`, local clock, MIDI session
- [playhead_policy_audit.md](playhead_policy_audit.md) — **full verification audit** (playhead policy, DSP + MIDI)

## Principle

```
┌─────────────────────────────────────────────────────────┐
│  Shared chrome ONLY: brand, port picker, conn badge     │
│  (mode selector — not pattern/transport state)          │
├──────────────────────┬──────────────────────────────────┤
│  Octopus DSP Engine  │  Octopus MIDI Controller         │
│  _dspSeq             │  _midiSeq                        │
│  dsp-only DOM        │  midi-only DOM                   │
│  SysEx mirror        │  Standard MIDI out               │
│  SESSION / PATIMAGE  │  localStorage MIDI session key   │
└──────────────────────┴──────────────────────────────────┘
```

**Not shared:** grid, banks, song chains, playhead, toolbar buttons, LEN/TRN fields, transport buttons, BPM input, scopes, knob panels, persistence keys.

## Body mode flag

Set only in `setAppMode()`:

```text
<body data-app-mode="dsp">    <!-- DSP Engine (default, disconnected) -->
<body data-app-mode="midi">   <!-- MIDI Controller -->
```

```css
body[data-app-mode="midi"] .dsp-only { display: none !important; }
body[data-app-mode="dsp"] .midi-only { display: none !important; }
```

## Connect path

```text
onConnect()
  ├─ _portIsOctopus() → setAppMode('dsp')   → APP_SYNC, heartbeat
  └─ else             → setAppMode('midi')  → local clock, _loadMidiSession
```

Port change DSP ↔ MIDI: `_requestPortBootReload(portId, 'dsp'|'midi')` → full page reload (clean shell).

Disconnect MIDI: `setAppMode('disconnected')` → DSP shell, tab **INSTRUMENTS**.

## Runtime guards

| Path | Guard |
|------|--------|
| SysEx TX, NVS persist, PATIMAGE | `_appMode === 'dsp'` + `_dspSysexAllowed()` |
| Local MIDI clock, CC map, `.mid` export | `_appMode === 'midi'` |
| Active sequencer handlers | `app._shell()` → `_dspSeq` or `_midiSeq` |

## What not to do

- Do not bind both shells to one `#seq-grid` or one `gridData` on `app`.
- Do not use one BPM / PLAY / STOP element for both modes.
- Do not arm `_syncBurstExpected` in MIDI mode.
- Do not call `repaintGrid()` without passing the target shell.
