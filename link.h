/* ═════════════════════════════════════════════════════════════════════════════
 * link.h — App↔device link constants + session latch
 *
 * MIRROR SPEC (FROZEN): docs/mirror_architecture.md · code_info.h §5A
 * Three axes — LINK / TRANSPORT / PLAYHEAD — method locked; timing polish only.
 * ═════════════════════════════════════════════════════════════════════════════ */
#pragma once
#ifndef LINK_H
#define LINK_H

#include <atomic>
#include <cstdint>
#include "esp_attr.h"

enum class LinkPhase : uint8_t {
  OFFLINE     = 0,
  LIVE        = 1,
  DEGRADED    = 2,
  PERSIST     = 3,
  REBOOT_WAIT = 4,
  SYNC_BURST  = 5,
  BLOB_XFER   = 6,
};

enum class PersistAckPhase : uint8_t {
  FAIL       = 0,
  COMMITTED  = 1,
  REBOOTING  = 2,
};

RTC_NOINIT_ATTR static uint32_t g_linkBootRtc;

/** App↔device link cadence — matches display_refresh_task (~30 Hz). */
static constexpr uint32_t LINK_FRAME_MS = 33u;

/** Device→App link beacon on CMD_PING (v14): HIGH = alive, LOW = explicit idle. */
static constexpr uint16_t LINK_BEACON_HIGH = 16383u;
static constexpr uint16_t LINK_BEACON_LOW  = 0u;

/** App session on device OLED — latched on APP_SYNC_REQ; idle if no App SysEx. */
static constexpr uint32_t APP_SESSION_IDLE_MS = 60000u;

inline std::atomic<bool> g_appSessionLatched{ false };

static inline void linkLatchAppSession() {
  g_appSessionLatched.store(true, std::memory_order_release);
}

static inline void linkReleaseAppSession() {
  g_appSessionLatched.store(false, std::memory_order_release);
}

inline std::atomic<uint8_t>  g_linkPhase{ 0 };
inline std::atomic<uint16_t> g_persistTxnId{ 0 };

static inline void linkInitBootId() {
  g_linkBootRtc++;
  if (g_linkBootRtc == 0u || g_linkBootRtc > 0x3FFu) g_linkBootRtc = 1u;
  g_linkPhase.store((uint8_t)LinkPhase::SYNC_BURST, std::memory_order_release);
}

static inline uint16_t linkBootId() {
  return (uint16_t)(g_linkBootRtc & 0x3FFu);
}

static inline LinkPhase linkGetPhase() {
  return (LinkPhase)g_linkPhase.load(std::memory_order_acquire);
}

static inline void linkSetPhase(LinkPhase p) {
  g_linkPhase.store((uint8_t)p, std::memory_order_release);
}

static inline uint16_t linkEncodePong() {
  const uint16_t phase = (uint16_t)linkGetPhase() & 7u;
  return (uint16_t)((phase << 10) | linkBootId());
}

static inline void linkNoteAppPing(uint16_t v14) {
  (void)v14;
  /* Phase advances on APP_SYNC_REQ completion — not on PING. */
}

static inline void linkOnAppSyncComplete() {
  linkSetPhase(LinkPhase::LIVE);
  linkLatchAppSession();
}

static inline uint32_t linkHeartbeatTimeoutMs() {
  /* Device→App link is BPM + PING beacon @ LINK_FRAME_MS; inbound App idle uses APP_SESSION_IDLE_MS. */
  return APP_SESSION_IDLE_MS;
}

static inline void linkBeginPersist(uint16_t txnId) {
  g_persistTxnId.store(txnId, std::memory_order_release);
  linkSetPhase(LinkPhase::PERSIST);
}

static inline uint16_t linkEncodePersistAck(PersistAckPhase phase, uint16_t txnId) {
  return (uint16_t)(((uint16_t)phase & 7u) << 12) | (txnId & 0x0FFFu);
}

/** v14 low nibble: scope+1 (1..4). Bits 4..15: txn_id (12-bit). */
static inline uint16_t decodePersistTxnV14(uint16_t v14) {
  if (v14 == 0u || v14 >= 16383u) return 0u;
  return (v14 >> 4) & 0x0FFFu;
}

#endif /* LINK_H */
