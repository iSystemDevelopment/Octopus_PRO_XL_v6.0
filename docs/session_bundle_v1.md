# SessionBundle v1 — normative JSON schema (App)

**Status:** Normative for v6.6 App browser sessions (§13, §14).  
**Supersedes:** ad-hoc `octopusapp_octopus_sessions_v1` blobs (migrated on first load).  
**Not used on device:** hardware stores binary `SongPack` v1 for PERF slots; full studio state stays in the App.

---

## 1. Purpose

`SessionBundle` is the **single JSON schema** for:

| Use | Slots | Storage key |
|-----|-------|-------------|
| **SESSION** workspace | 1–16 (slot index 0 = SESSION 1 autosave) | `octopusapp_session_bundle_v1` |
| **PATIMAGE** pattern library | 1–16 | `octopusapp_patimage_v1` |
| **Sound / mix presets** | 1–16 per engine | `octopusapp_sound_{harp\|seq\|drum\|mix}_v1` |
| **File export** | user pick | `octopus_session_N.json` |

All stores hold **references and snapshots** (IDs, grids, knob values). **No audio blobs.**

---

## 2. Top-level store envelope

### 2.1 SESSION store (`octopusapp_session_bundle_v1`)

```json
{
  "storeVersion": 1,
  "slots": [ /* SessionBundle | null × 16 */ ]
}
```

| Field | Type | Notes |
|-------|------|-------|
| `storeVersion` | `uint8` | `1` |
| `slots` | `array[16]` | index `0` = SESSION 1 (autosave); `1`…`15` = SESSION 2–16 |

### 2.2 PATIMAGE store (`octopusapp_patimage_v1`)

Same envelope; each slot is a **PatimageSubset** (§5.2).

### 2.3 Sound stores

```json
{
  "storeVersion": 1,
  "slots": [ /* SoundSlotSubset | null × 16 */ ]
}
```

Keys: `octopusapp_sound_harp_v1`, `octopusapp_sound_seq_v1`, `octopusapp_sound_drum_v1`, `octopusapp_sound_mix_v1`.

---

## 3. SessionBundle object (full)

```typescript
interface SessionBundle {
  meta: SessionMeta;
  gridData: GridBank[4];
  songData: SongSlot[16];
  transport: TransportState;
  knobs: Record<string, number>;   // CMD id (decimal string) → value
  motion?: MotionMatrix;
  mix?: MixState;
  midi?: MidiMapState;
  dbeam?: DBeamState;
  refs: RefBlock;
}
```

### 3.1 `meta` — required

| Field | Type | Required | Notes |
|-------|------|----------|-------|
| `schema` | `number` | yes | always `1` |
| `kind` | `string` | yes | `"session"` \| `"patimage"` \| `"sound_harp"` \| `"sound_seq"` \| `"sound_drum"` \| `"sound_mix"` |
| `app` | `string` | yes | App build id, e.g. `"6.6.01"` |
| `savedAt` | `string` | yes | ISO-8601 UTC |
| `label` | `string` | no | user name, max 32 chars (UI); device names stay ≤9 |

### 3.2 `gridData` — required for session / patimage

Four banks **A–D** (indices `0`–`3`). Each bank is **16 rows × 64 steps** booleans.

```typescript
type GridBank = GridRow[16];
type GridRow = boolean[64];   // step 0..63; pages P1–P4 = steps 0–15, 16–31, …
```

**Wire alignment:** App `gridData[bank][row]` maps to firmware `hwSeqData[bank][chain0][row]` as `uint64` (bit *n* = step *n*). Rows `0`–`7` = melody; `8`–`15` = drums.

**Per-bank metadata** (session / patimage only):

```typescript
interface BankMeta {
  transpose: number;    // int, −12..+12
  octave: number;     // int, −4..+4
  scale: number;      // uint8, scale index
  originMel: number;  // 0..30 factory seed, 255 = none
  originDrum: number; // 0..30, 255 = none
  soundHarp: number;  // SoundImage slot 0..127
  soundSeq: number;
  soundDrum: number;
}
```

Full session: `bankMeta: BankMeta[4]` sibling to `gridData` (required in v1 once Step 2 ships; optional during scaffold migration).

### 3.3 `songData` — required for session / patimage

Sixteen chain programs (legacy `hwSongData[16]`; musically owned by PatternImage in v6.6).

```typescript
interface SongStep {
  bank: number;    // 0..3 (A–D) in v6.6 UI
  chain: number;   // 0..3; v6.6 uses chain 0 only
  repeats: number; // 1..15; 0 = inactive (normalize to 1 on load)
}

interface SongSlot {
  numSteps: number;  // 1..16
  steps: SongStep[16];
}
```

**PatimageSubset** uses **one** active chain: `songSlotIdx` + `songData[songSlotIdx]` only; full session keeps all 16.

### 3.4 `transport` — required

| Field | Type | Range | Firmware |
|-------|------|-------|----------|
| `bpm` | `number` | 40..300 | `BPM` |
| `seqLen` | `number` | 1..64 | `HW_S_LEN` |
| `transpose` | `number` | −12..+12 | `TRANSPOSE` |
| `seqOct` | `number` | −4..+4 | `HW_S_OCT` |
| `bankIdx` | `number` | 0..3 | `BANK` |
| `gridPageIdx` | `number` | 0..3 | grid page P1–P4 |
| `isSongMode` | `boolean` | — | chain loop 🔗 |
| `songSlotIdx` | `number` | 0..15 | active chain editor slot |

### 3.5 `knobs` — required (may be `{}`)

Map of **SysEx command id** (decimal string key) → **integer value** as last sent to device.

Source: App `knobStates` snapshot (`_snapshotKnobs()`). Keys are `String(CMD.*)` for every registered knob.

### 3.6 `motion` — optional (session only)

Sparse P-lock lanes; mirrors `MotionBlob` / `hwMotionData`. **Not** in PATIMAGE or SongPack by default.

```typescript
interface MotionLane {
  bank: number;       // 0..15 (v6.6 session uses 0..3)
  chain: number;      // 0..3
  lane: number;       // 0..3
  targetCmd: number;  // 0..254; 255 = empty (omit lane)
  steps: number[16]; // 0..65535; 65535 = no automation on step
}

interface MotionMatrix {
  version: 1;
  lanes: MotionLane[];  // max 128 allocated lanes
}
```

### 3.7 `mix` — optional (session / sound_mix)

Performance mixer subset (not in PatternImage):

| Field | Type | Notes |
|-------|------|-------|
| `masterVol` | `number` | 0..127 |
| `harpVol`, `seqVol`, `drumVol` | `number` | bus levels |
| `harpMute`, `seqMute`, `drumMute` | `boolean` | H/S/D |
| `masterFxIdx` | `number` | master FX preset |

### 3.8 `midi` — optional

Same logical fields as SysEx blob **0x07** (`docs/midi_map_ssot.md` when present). Omitted = device defaults at recall.

### 3.9 `dbeam` — optional

Laser / D-beam performance offsets; session only unless user exports with `include_dbeam` in SongPack.

### 3.10 `refs` — required

```typescript
interface RefBlock {
  harpWave: number;      // 0..119 ROM
  seqWave: number;
  harpPatch: number;     // 0..127 library (v6.3+ unified slots)
  seqPatch: number;
  drumKit: number;
  drumFxA: number;
  drumFxB: number;
  masterFx: number;
  dwaves: number[8];     // per-drum wave indices
  patternRef?: number;   // 0..127 device PatternImage slot, if linked
  soundRefs?: {
    harp: number;
    seq: number;
    drum: number;
  };
}
```

**Rule:** values are **slot indices**, not embedded `SoundImage` payloads. Optional future field `embeddedSounds` (§8.3) may add inline copies for offline export; default off.

---

## 4. Validation rules

1. `meta.schema === 1` or reject import.
2. `gridData.length === 4`; each row `length === 16`; each step row `length === 64`.
3. `songData.length === 16`.
4. `knobs` keys must parse as integers; values within each CMD’s documented min/max.
5. `transport.seqLen` clamped 1..64 on apply.
6. `songData[*].steps[*].repeats` clamped 1..15 on apply (inactive steps: set `repeats: 1` + unused tail).
7. Unknown top-level keys **ignored** on read (forward compatible).
8. Missing optional sections → defaults (no motion, device mix unchanged).

---

## 5. Subsets (same schema family)

### 5.1 Sound slot subset

```json
{
  "meta": { "schema": 1, "kind": "sound_harp", "app": "6.6.01", "savedAt": "…" },
  "knobs": { "42": 80 },
  "refs": { "harpWave": 3, "harpPatch": 12 }
}
```

Engine-specific: only knobs + refs for that engine (+ `mix` for `sound_mix`).

### 5.2 Patimage subset

```json
{
  "meta": { "schema": 1, "kind": "patimage", "app": "6.6.01", "savedAt": "…", "label": "VERSE" },
  "gridData": [ /* 4 banks */ ],
  "bankMeta": [ /* 4 */ ],
  "transport": { "bpm": 120, "seqLen": 32, "transpose": 0, "seqOct": 0, "bankIdx": 0, "gridPageIdx": 0, "isSongMode": true, "songSlotIdx": 0 },
  "songData": [ /* 16 slots; only songSlotIdx chain required */ ],
  "refs": { "patternRef": 7 }
}
```

---

## 6. Migration from `octopusapp_octopus_sessions_v1`

Legacy store shape:

```json
{ "v": 1, "slots": [ { "v": 1, "app", "savedAt", "gridData", … } | null × 16 ] }
```

**On first `SessionStore.read()`:**

1. If `octopusapp_session_bundle_v1` exists → use it.
2. Else if legacy key exists → for each non-null slot:
   - set `meta.schema = 1`, `meta.kind = "session"`
   - copy fields 1:1 where names match
   - wrap in new envelope `{ storeVersion: 1, slots }`
   - write new key; keep legacy key read-only backup (do not delete until v6.7).
3. Empty slots remain `null`.

Field renames: none in v1 (legacy `_serializeOctopusSession` already matches §3).

---

## 7. Apply + push order (App → device)

When `pushHw === true` (SESSION 2–16 recall, optional PATIMAGE recall):

1. Grid rows — `_txGridRow` × 4 banks × 4 pages × 16 rows  
2. Song chains — `SONG_STEPS_N` + `SONG_STEP` for all 16 slots (or patimage active slot only)  
3. Knobs — `txSysexSoon(cmd, value)` for each `knobs` entry  
4. Refs — `H_WAVE`, `S_WAVE`, patch indices, drum kit, `M_FX_IDX`, `dwaves[]`  
5. Transport — `BANK`, `HW_S_LEN`, `TRANSPOSE`, `HW_S_OCT`, `BPM`, chain mode flags  

No `location.reload()`. No device reboot for browser-only save.

---

## 8. File export / import

| Item | Value |
|------|--------|
| Extension | `.json` |
| Suggested name | `octopus_session_<N>.json` or `octopus_session_<label>.json` |
| Root object | **SessionBundle** (not the 16-slot store envelope) |
| Import target | SESSION slots **2–16** only (slot 1 reserved for autosave) |
| MIME | `application/json` |

Import flow: validate §4 → confirm slot → `slots[i] = bundle` → `apply(bundle, pushHw)`.

---

## 9. Example (minimal session)

```json
{
  "meta": {
    "schema": 1,
    "kind": "session",
    "app": "6.6.01",
    "savedAt": "2026-06-25T12:00:00.000Z",
    "label": "Demo"
  },
  "gridData": [
    [
      [false, true, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false],
      "… 15 more rows …"
    ],
    "… banks B–D …"
  ],
  "bankMeta": [
    { "transpose": 0, "octave": 0, "scale": 0, "originMel": 255, "originDrum": 255, "soundHarp": 0, "soundSeq": 0, "soundDrum": 0 }
  ],
  "songData": [
    {
      "numSteps": 2,
      "steps": [
        { "bank": 0, "chain": 0, "repeats": 4 },
        { "bank": 1, "chain": 0, "repeats": 4 },
        { "bank": 0, "chain": 0, "repeats": 1 },
        "… pad to 16 …"
      ]
    },
    "… 15 more song slots …"
  ],
  "transport": {
    "bpm": 120,
    "seqLen": 16,
    "transpose": 0,
    "seqOct": 0,
    "bankIdx": 0,
    "gridPageIdx": 0,
    "isSongMode": false,
    "songSlotIdx": 0
  },
  "knobs": { "10": 64, "11": 100 },
  "refs": {
    "harpWave": 0,
    "seqWave": 0,
    "harpPatch": 0,
    "seqPatch": 0,
    "drumKit": 0,
    "drumFxA": 0,
    "drumFxB": 0,
    "masterFx": 0,
    "dwaves": [0, 0, 0, 0, 0, 0, 0, 0]
  }
}
```

*(Example truncates repeated arrays for readability; on-disk files must be complete.)*

---

## 10. Relation to SongPack v1

| SessionBundle | SongPack v1 |
|---------------|-------------|
| Full studio, 16 chains, motion, MIDI map | One performance: 1 patimage + 3 sounds |
| Browser `localStorage` / JSON file | Device NVS binary, PERF 1–8 |
| SESSION / PATIMAGE / sound libraries | “Pack for hardware” encoder input |

Encoder: App selects active patimage + referenced sounds + performance mix → [`song_pack_v1.md`](song_pack_v1.md).

---

## 11. Versioning

| `meta.schema` | Meaning |
|---------------|---------|
| `1` | This document |

Increment only when breaking required fields or grid topology. Additive optional fields do **not** require bump if §4 rule 7 applies.

---

*Changelog:*  
- 2026-06-25 — Initial normative schema (Phase B, §13 Step 1).
