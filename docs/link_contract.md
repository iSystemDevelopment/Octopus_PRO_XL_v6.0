# Octopus PRO XL — Link contract (v6.3.00)

**Status:** Normative for persist/reboot/reconnect.  
**Mirror (playhead + transport + badge):** **frozen** in [`docs/mirror_architecture.md`](mirror_architecture.md) — that doc is authoritative for sync display; this doc covers session lifecycle only.  
**Scope:** Octopus App mode only (not MIDI Controller mode).  
**Related:** `code_info.h` §5A · `patches.h` · `OctopusApp.html`.

---

## 1. Problem statement

Heartbeat (PING every ~800 ms) keeps the OLED on “APP CONNECTED” but does **not** answer:

- Did NVS commit finish before reboot?
- Is this USB reconnect the reboot we ordered?
- Is inbound SysEx still flowing (one-way USB)?
- Will a multi-second blob transfer survive the 4.5 s watchdog?
- Will sound/pattern save accidentally trigger a session reload?

v6.2.x failures were **ambiguous link phase**, not missing PING. v6.2.10 `CMD_BOOT_READY` is **rejected** (unsolicited boot spam). v6.3 uses a **bidirectional phase machine** with transaction IDs.

---

## 2. App rules (simplified — no phase machine)

- **Dual heartbeat (both required):** App `CMD.PING` ~800 ms (`startHeartbeat`); device `CMD_BPM` + PING beacon + CPU @ 33 ms. See `mirror_architecture.md` §1.2.
- **PING = App→device session keepalive:** Never `stopHeartbeat()` during save/reboot wait except when `midiOut` is gone.
- **Do not null `midiOut` in `_prepareForDeviceReboot()`** — keep port reference so PING continues through USB re-enumeration (send errors absorbed by try/catch).
- **ACK paths:** `SESSION_SLOT_ACK`, `SESSION_SAVE 16383`, or `SCOPED_RESET 16383` → `_persistFinished(true, true)` when `_persistBusy`.
- **Fail fast:** persist timeout **8 s**; link stale **4.5 s** (9 s while `_persistAwaitReboot`).
- **`boot_id` in PONG** (optional): surprise reboot → `APP_SYNC_REQ` resync only when not in persist.

Legacy §2 phase table — **deferred to v6.3.1**; do not implement `_linkPhase` in App until session blob work lands.

---

## 2 legacy. Link phases (reference only — not in App v6.2.12)

| Phase | Code | Set by | Heartbeat / timeout rule |
|-------|------|--------|--------------------------|
| `OFFLINE` | 0 | default | none |
| `LIVE` | 1 | post-sync burst done | PING 800 ms; RX stale → `DEGRADED` @ 4.5 s |
| `DEGRADED` | 2 | stale transport echo | auto `APP_SYNC_REQ`; remain until burst drains |
| `PERSIST` | 3 | App arms session/factory save | **ignore** heartbeat miss; wait for `SESSION_SLOT_ACK` |
| `REBOOT_WAIT` | 4 | ACK ok + `will_reboot=1` | stop PING; reconnect poller 1 s; timeout **45 s** |
| `SYNC_BURST` | 5 | connect / resync | block user persist; drain RX queue before `LIVE` |
| `BLOB_XFER` | 6 | JSON import/export | RX limit **120 s**; PING continues; must not FAIL xfer |

**Hard rule:** PING is the connection BPM — **never** `stopHeartbeat()` during save/reboot wait. Only pause PING when `midiOut` is physically gone (USB re-enumerating); reconnect poller runs in parallel.

**Timeouts (App):** persist fail @ **8 s**; link stale @ **4.5 s** (9 s while awaiting reboot USB).

---

## 3. PING / PONG (CMD 99)

Reuse `CMD_PING = 99`. No new command ID.

### 3.1 App → device (PING)

```
v14 = (app_phase << 10) | (seq & 0x3FF)
```

- `app_phase`: 0–6 (table above)
- `seq`: monotonic 10-bit counter

### 3.2 Device → App (PONG)

Same command ID on reply:

```
v14 = (dev_phase << 10) | (boot_id & 0x3FF)
```

- `dev_phase`: firmware link phase (0–6)
- `boot_id`: incremented in `RTC_NOINIT_ATTR` at start of each `setup()` (survives soft reboot; resets on full power cycle)

### 3.3 App decisions

| Condition | Action |
|-----------|--------|
| `boot_id` changed **and** App in `REBOOT_WAIT` with matching `txn_id` | Proceed to reload **once** |
| `boot_id` changed **without** expected reboot | `SYNC_BURST` only (hardware reset / crash recovery) |
| PONG seq echo mismatch (optional v6.3.1) | `DEGRADED` |

### 3.4 Firmware

- Refresh `lastWebSysexMs` on **any** App SysEx (unchanged).
- During `PERSIST` / `BLOB_XFER`: do **not** drop `appSyncActive` solely because PING paused; set `dev_phase` accordingly.

---

## 4. Transaction IDs (session + factory)

| Field | Width | Notes |
|-------|-------|-------|
| `txn_id` | 16-bit | App random per save/load/factory |
| Carried in | `SESSION_SLOT_SAVE` (197), `SESSION_SLOT_LOAD` (198), `FACTORY_RESET` (200), blob 0x06 header |

### SESSION_SLOT_ACK (199)

```
v14 = (phase << 12) | txn_id
```

| `phase` | Meaning |
|---------|---------|
| 0 | FAIL |
| 1 | COMMITTED (no reboot — reserved) |
| 2 | REBOOTING |

`txn_id`: low 12 bits in v14 (full 16-bit in blob ack frame allowed in v6.3.1).

App reload gate: ACK `phase=REBOOTING` **and** `txn_id` match **and** subsequent PONG shows new `boot_id`.

---

## 5. Reconnect layers (all three required)

| Layer | Mechanism | When |
|-------|-----------|------|
| **A** | Heartbeat + PONG phase | Normal performance |
| **B** | 1 s reconnect poller (`_midiAccess` scan by name) | USB re-enumeration after reboot |
| **C** | `sessionStorage` port id + mode, 120 s TTL | Reload after session save only |

After session reload: connect → `APP_SYNC_REQ` → `SYNC_BURST` → `LIVE`. No partial UI from pre-reboot RAM.

Sound/pattern save: remain in `LIVE`; echo changed engine only; **never** set reload flags.

---

## 6. Blob transfer (JSON / session export)

1. Enter `BLOB_XFER` before first chunk.
2. Chunk frame (blob **0x06**): `{ txn_id, seq, total, crc32, payload[] }`.
3. Firmware ACKs each chunk or NACKs with `seq` for retry.
4. App: 3 retries per chunk, 120 s total timeout.
5. Exit `BLOB_XFER` after CRC-ok ACK → `LIVE`.

---

## 7. Implementation checklist

### Firmware (`midi.cpp`, `patches.h`, `Octopus_PRO_XL_v6.0.ino`)

- [x] `linkPhase` atomic
- [x] `boot_id` in RTC memory, increment in `setup()`
- [x] PONG encodes `dev_phase` + `boot_id`
- [x] `SESSION_SLOT_ACK` encodes `phase` + `txn_id`
- [x] During `PERSIST`: suppress non-critical echoes; still PONG + ACK
- [x] After session load at boot: `SYNC_BURST` until first `APP_SYNC_REQ` completes

### App (`OctopusApp.html`)

- [x] Single `_linkPhase` state machine (replace `_persistAwaitReboot`, `_reloadAfterReconnect`)
- [x] PING encodes `app_phase` + `seq`
- [x] `_checkTransportHealth` only in `LIVE` / `DEGRADED`
- [x] Persist modal phase text: “Committing…”, “Waiting for reboot…”, “Syncing…”
- [x] Phase-based timeouts (not mixed 5 s / 90 s)

---

## 8. Explicit rejections

| Rejected | Reason |
|----------|--------|
| Unsolicited `CMD_BOOT_READY` | Cannot distinguish expected vs surprise reboot |
| In-page sync drain for session | v6.2.10 dropdown/state drift |
| Scoped LOAD over USB | Removed v6.2.11 |
| Bare ok/fail ACK without `txn_id` | Double reload / missed reload |

---

## 9. Test plan (link-specific)

| ID | Test |
|----|------|
| T9 | Session save: ACK + matching `txn_id` → one reload only |
| T10 | Session save: USB gone before ACK → FAIL; no reload; reconnect → LIVE resync |
| T11 | Sound/pattern save in `LIVE` → no reload; UI intact |
| T12 | JSON import ~40 KB in `BLOB_XFER` → completes without timeout FAIL |

---

*Source: `v6.3.00.md` §12 · decision 8.11*
