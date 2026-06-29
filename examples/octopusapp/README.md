# OctopusApp factory tutorial examples

These files document the **built-in tutorials** seeded on first visit to [octopus.isystem.app](https://octopus.isystem.app) (v6.6+).

## What gets loaded automatically

| Asset | Where in UI | Purpose |
|-------|-------------|---------|
| **harp-image** | INSTRUMENTS → HARP → `harp-image ▼` | Acid laser harp — wave, patch 018, FX, ADSR knobs |
| **melody-image** | INSTRUMENTS → SEQ SYNTH → `melody-image ▼` | Magellanic swell lead — pairs with factory melody patterns |
| **drum-image** | INSTRUMENTS → AB 9-09 → `drum-image ▼` | Quantum house kit — 8 voices + FX |
| **mix-image** | MIXER → master bar → `mix-image ▼` | Club master mix — levels, mutes, master FX |
| **PATIMAGE 16** | SEQUENCER → `PATIMAGE 16 ▼` | Master-class snapshot (max PATIMAGE features) |
| **MIDI demo song** | HELP → *Load MIDI demo song* | Browser MIDI session (MIDI Controller mode) |

## Sound-image format (learn by example)

Each sound-image stores:

1. **Preview card** (PNG data URL) — human-readable faceplate  
2. **`blob`** — SessionBundle subset (`knobs` + `refs` + optional `mix`)

```json
{
  "harp": [{
    "name": "EXAMPLE · Acid Laser Harp",
    "dataUrl": "data:image/png;base64,...",
    "blob": {
      "meta": { "schema": 1, "kind": "sound_harp", "label": "..." },
      "knobs": { "0": 0, "1": 50, "5": 6200 },
      "refs": { "harpWave": "0", "harpPatch": "18", "harpFxA": "2" }
    }
  }]
}
```

**Workflow:** tweak sound → **SAVE** (image modal) → name it → recall from dropdown anytime.

## PATIMAGE master class (slot 16)

Demonstrates everything a PATIMAGE can hold:

| Feature | Demo content |
|---------|----------------|
| **4 banks A–D** | Different melody + drum factory patterns per bank |
| **64-step length** | Patterns tiled across P1–P4 |
| **bankMeta** | Per-bank transpose, octave, scale, origin pattern IDs |
| **Song chain** | A×2 → B → C×2 → D (4 chain steps) |
| **transport** | BPM 124, LEN 64, song mode on |

Recall: **SEQUENCER** → `PATIMAGE 16 ▼` → grid + chain load instantly.

## MIDI demo song

See [`midi_demo_song.json`](midi_demo_song.json) — importable structure for Universal MIDI Controller mode:

- Banks A/B with Magellanic Swell + Quantum House / Magnetar grooves  
- 128 BPM, 64 steps, Dorian scale  
- Song chain A×2 → B → A×2  
- ARP on (UpDown, 1/8, 50% gate), MIDI clock out  

In v6.6.01, **HELP → Load MIDI demo song** applies the same grid in MIDI Controller mode (or via PATIMAGE logic in DSP mode).

## Reload tutorials

**HELP** popup → **RELOAD TUTORIAL EXAMPLES** (or clear `localStorage` key `octopusapp_factory_examples_v6.6`).

## Schema reference

- [`docs/session_bundle_v1.md`](../../docs/session_bundle_v1.md) — full JSON schema  
- [`local/operator/v6.3.00.md`](../../local/operator/v6.3.00.md) §13–§14 — SESSION / PATIMAGE workflow *(machine-local, gitignored)*
