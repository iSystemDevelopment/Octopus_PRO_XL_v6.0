# SongPack v1 — normative binary schema (device)

**Status:** Normative for v6.6 hardware PERF slots (§14).  
**Endianness:** little-endian (ESP32).  
**CRC:** CRC-32 IEEE (same polynomial as `PatternsBlob.crc32` in `settings.h`); CRC field zeroed during calculation.

---

## 1. Purpose

A **SongPack** is a **self-contained performance blob** the groovebox can play **without USB / App**:

- One **PatternImage** (banks A–D + chain)
- **Three embedded SoundImages** (harp, seq, drum) actually used by that pattern
- **Mix + transport** subset for live performance
- Optional motion / D-beam (default **off**)

User flow: App **“Pack for hardware”** → binary → device **PERF slot 1–8**. OLED shows pack-scoped sound names (not full 0–127 library).

**Not included:** 16 SESSION slots, PATIMAGE librarian, full motion matrix, MIDI map, laser factory cal, factory RESET.

---

## 2. File layout overview

```
┌─────────────────────────────────────────────────────────────┐
│ SongPackHeader          32 bytes                            │
├─────────────────────────────────────────────────────────────┤
│ Section: PATN  PatternImageV1                             │
│ Section: SNDH  SoundImageHarpV1                           │
│ Section: SNDS  SoundImageSeqV1                            │
│ Section: SNDD  SoundImageDrumV1                           │
│ Section: MIXL  MixLiveV1                                  │
│ Section: TRNS  TransportV1                                │
│ Section: MOTN  MotionSparseV1     (optional, flag bit0)   │
│ Section: DBEM  DBeamPerfV1        (optional, flag bit1)   │
└─────────────────────────────────────────────────────────────┘
```

Each section:

```
fourcc   char[4]     /* ASCII tag, e.g. "PATN" */
length   uint32      /* payload bytes following */
payload  uint8[length]
```

**Section order is fixed** (table above). Decoder may stop after `TRNS` if flags omit optional sections.

**Total size (typical):** 1.5–4 KB without motion; ≤ ~12 KB with sparse motion (128 lanes max).

---

## 3. SongPackHeader (32 bytes)

| Offset | Size | Field | Value / notes |
|--------|------|-------|----------------|
| 0 | 4 | `magic` | `0x4F505350` (`"OPSP"`) |
| 4 | 2 | `schema_ver` | `1` |
| 6 | 2 | `flags` | see §3.1 |
| 8 | 4 | `total_len` | entire blob size including header |
| 12 | 4 | `crc32` | over bytes `[16 .. total_len)`; store `0` while hashing |
| 16 | 10 | `name` | display name, NUL-padded, max 9 chars + NUL |
| 26 | 4 | `fw_compat` | `0x00060006` = v6.6.0 minimum decoder |
| 30 | 2 | `reserved` | `0` |

### 3.1 `flags`

| Bit | Name | Default | Meaning |
|-----|------|---------|---------|
| 0 | `INCLUDE_MOTION` | 0 | `MOTN` section present |
| 1 | `INCLUDE_DBEAM` | 0 | `DBEM` section present |
| 2–15 | — | 0 | reserved |

---

## 4. Section payloads

### 4.1 `PATN` — PatternImageV1

Mirrors §1.2 PatternImage in `v6.3.00.md` and firmware `hwSeqData` layout for banks A–D, chain 0.

| Offset (rel.) | Size | Field |
|---------------|------|-------|
| 0 | 2 | `version` = `1` |
| 2 | 2 | `bank_count` = `4` |
| 4 | 1 | `chain_count` = `1` |
| 5 | 3 | `_pad` = 0 |

**Per bank** (`bank_count` times):

| Field | Size | Notes |
|-------|------|-------|
| `grid[16]` | 16 × `uint64` | rows 0–7 melody, 8–15 drums; bit *s* = step *s* |
| `transpose` | `int8` | −12..+12 |
| `octave` | `int8` | −4..+4 |
| `scale` | `uint8` | scale index |
| `origin_mel` | `uint8` | 0..30 or `0xFF` |
| `origin_drum` | `uint8` | 0..30 or `0xFF` |
| `sound_harp` | `uint8` | **index inside pack** 0..2 (§4.5), not NVS slot |
| `sound_seq` | `uint8` | same |
| `sound_drum` | `uint8` | same |
| `_pad` | `uint8` | 0 |

**Pattern-global** (after all banks):

| Field | Size | Notes |
|-------|------|-------|
| `bpm` | `uint16` | 40..300 |
| `seq_len` | `uint8` | 1..64 |
| `chain_enabled` | `uint8` | 0/1 |
| `active_bank` | `uint8` | 0..3 |
| `grid_page` | `uint8` | 0..3 (P1–P4) |
| `chain_num_steps` | `uint8` | 1..16 |
| `_pad` | `uint8` | 0 |
| `chain[16]` | 16 × 4 B | `SongStep` LE: `bank:u8, chain:u8, repeats:u8, pad:u8` |
| `name[10]` | 10 | patimage label |

**Struct size:** `8 + 4×(128+8) + 8 + 64 + 10` = **610 bytes** payload (+ 8 B section header).

**Load behaviour:** copy into `hwSeqData[0..3][0][*]`; apply `seqPatternTranspose`; set chain from `chain[]`.

---

### 4.2 `SNDH` / `SNDS` — SoundImage synth (harp / seq)

| Field | Size | Notes |
|-------|------|-------|
| `version` | `uint16` | `1` |
| `flags` | `uint16` | bit0 = valid |
| `params[16]` | 16 × `uint16` | `PARAMS_PER_PRESET` live patch |
| `fx_a`, `fx_b` | 2 × `uint8` | FX slot indices |
| `send_a`, `send_b` | 2 × `uint8` | 0..127 |
| `arp_enable` | `uint8` | seq + harp |
| `arp_pat`, `arp_rate`, `arp_gate` | 3 × `uint8` | |
| `wave_idx` | `uint8` | ROM wave 0..119 |
| `_pad` | `uint8` | |
| `name[10]` | 10 | |

**Payload size:** 54 bytes (+ 8 B section header).

---

### 4.3 `SNDD` — SoundImage drum

| Field | Size | Notes |
|-------|------|-------|
| `version` | `uint16` | `1` |
| `flags` | `uint16` | bit0 = valid |
| `drum_patch[32]` | 32 × `uint16` | `drumLivePatch` |
| `kit_id` | `uint8` | |
| `fx_a`, `fx_b` | 2 × `uint8` | |
| `send_a`, `send_b` | 2 × `uint8` | |
| `_pad` | 4 | |
| `dwaves[8]` | 8 × `uint8` | per-voice wave |
| `name[10]` | 10 | |

**Payload size:** 88 bytes (+ 8 B section header).

---

### 4.4 `MIXL` — MixLiveV1

Performance mixer only (dashboard + MASTER menu subset).

| Field | Size | Notes |
|-------|------|-------|
| `version` | `uint16` | `1` |
| `master_vol` | `uint8` | |
| `harp_vol`, `seq_vol`, `drum_vol` | 3 × `uint8` | |
| `harp_mute`, `seq_mute`, `drum_mute` | 3 × `uint8` | 0/1 |
| `master_fx_idx` | `uint8` | |
| `_pad` | `uint8` | |

**Payload size:** 12 bytes.

---

### 4.5 `TRNS` — TransportV1

| Field | Size | Notes |
|-------|------|-------|
| `version` | `uint16` | `1` |
| `transpose` | `int8` | global melody transpose |
| `seq_oct` | `int8` | |
| `song_slot_idx` | `uint8` | active chain index 0..15 |
| `is_song_mode` | `uint8` | 0/1 |
| `_pad` | 4 | |

**Payload size:** 10 bytes. (`bpm`, `seq_len`, `active_bank`, `grid_page` live in `PATN`.)

**Pack sound indices:** `PATN.bank.sound_*` are **0=harp, 1=seq, 2=drum** embedded sections, not device NVS IDs.

---

### 4.6 `MOTN` — MotionSparseV1 (optional)

Same sparse encoding as `MotionBlob` in `settings.h`:

| Field | Size |
|-------|------|
| `version` | `uint16` = 1 |
| `count` | `uint16` | valid lanes ≤ 128 |
| `lanes[count]` | count × 20 B | `MotionLaneOverride`: bank, chain, lane, targetCmd, steps[16] × uint16 |

Decoder expands into `hwMotionData[bank][chain][lane]`.

---

### 4.7 `DBEM` — DBeamPerfV1 (optional)

| Field | Size | Notes |
|-------|------|-------|
| `version` | `uint16` | `1` |
| `offset_adc` | `int32` | |
| `range_adc` | `int32` | |
| `gain` | `float32` | IEEE754 |

**Payload size:** 14 bytes.

---

## 5. Device storage — PERF slots

| Item | Value |
|------|--------|
| Slot count | **8** (PERF 1–8 on OLED) |
| NVS key pattern | `perf_pack_%u` (`%u` = 1..8) |
| Autosave | separate **slot 0** working RAM flush (not a SongPack); unchanged from §8.5 |
| Max blob size | 16 KB per slot (budget within 256 KB NVS partition) |

**Active pack:** index `active_perf_slot` in RAM; matrix VIEW/TWEAK reads loaded `PATN`.

**Dirty TWEAK:** matrix edits set `pack_dirty`; on exit prompt **“Save to PERF N?”** → re-encode **PATN** (+ optional delta), not full SessionBundle.

---

## 6. Transfer protocol (App ↔ device)

### 6.1 Primary path — `SESSION_BLOB_CHUNK` (blob **0x06**)

Used for chunked import/export (§6 in `v6.3.00.md`). SongPack is one blob type inside the chunk stream.

**Chunk envelope** (payload of blob 0x06):

| Field | Size | Notes |
|-------|------|-------|
| `txn_id` | `uint16` | matches §12 link phase |
| `phase` | `uint8` | 0=BEGIN, 1=DATA, 2=END, 3=ABORT |
| `blob_kind` | `uint8` | `2` = SongPack v1 (reserve `0`=SessionImage, `1`=legacy) |
| `perf_slot` | `uint8` | 1..8 target PERF slot |
| `chunk_idx` | `uint16` | 0-based |
| `total_chunks` | `uint16` | |
| `data[]` | remainder | max **512 B** per chunk (stay under SysEx 4.5 s watchdog) |

**BEGIN:** `data` carries `total_len`, `crc32` expected, `flags`.  
**DATA:** raw SongPack bytes in order.  
**END:** device verifies CRC + `total_len`, commits to NVS, ACK via `SESSION_SLOT_ACK (199)` with `phase=PERSIST`.

### 6.2 Future — dedicated SysEx (optional P4)

`SONG_PACK_WRITE` / `SONG_PACK_READ` may alias the same binary layout; v6.6 uses blob 0x06 only.

---

## 7. Encoder rules (App)

Input: current patimage (from PATIMAGE slot or live grid) + `refs.soundRefs` or per-bank `sound_*`.

1. Resolve three `SoundImage` snapshots (live RAM if modified, else library slot).
2. Build `PATN` with `sound_*` = 0,1,2 pack indices.
3. Emit `SNDH`, `SNDS`, `SNDD` in that order.
4. Snapshot mixer + transport from UI.
5. Include `MOTN` only if user checks **“Include motion”** (default off).
6. Include `DBEM` only if user checks **“Include D-beam”** (default off).
7. Compute `total_len`, CRC, write header.

**Strip:** unused banks beyond `bank_count`, session-only MIDI map, 16 song slots not in active chain (still encode full `chain[16]` in PATN for chain editor parity).

---

## 8. Decoder rules (firmware)

1. Verify `magic`, `schema_ver`, `crc32`, `total_len`.
2. Parse sections in order; unknown `fourcc` → skip if `length` sane, else abort.
3. Load `SND*` into pack-scoped RAM mirrors (`packSoundHarp`, etc.) — **not** NVS 0–127 library.
4. Apply `PATN` → `hwSeqData`; refresh matrix VIEW.
5. Apply `MIXL`, `TRNS`.
6. Populate OLED sound picker from three embedded names only (T22).
7. Optional sections per flags.

---

## 9. Size budget

| Component | Bytes (typical) |
|-----------|-----------------|
| Header | 32 |
| PATN | ~618 |
| SNDH + SNDS + SNDD | ~180 |
| MIXL + TRNS | ~30 |
| MOTN | 0–~2560 (sparse) |
| **Total** | **~900–3500** |

Fits comfortably in 16 KB PERF cap with motion.

---

## 10. Compatibility

| `schema_ver` | Notes |
|--------------|-------|
| 1 | This document |

| `fw_compat` | Meaning |
|-------------|---------|
| `0x00060006` | v6.6.0+ |

P4 may add `bank_count=16` in PATN v2; v6.6 decoder rejects `bank_count > 4`.

---

## 11. Test mapping (§14.7)

| Test | SongPack requirement |
|------|----------------------|
| T21 | Round-trip pack → PERF → standalone play |
| T22 | Only 3 embedded sound names in HW picker |
| T23 | Harp dashboard uses pack sound list |
| T25 | No SESSION 16 on OLED (PERF ≠ SESSION) |

---

*Changelog:*  
- 2026-06-25 — Initial normative binary schema (Phase B, §14).
