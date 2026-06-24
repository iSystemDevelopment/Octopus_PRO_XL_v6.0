/* ═════════════════════════════════════════════════════════════════════════════
 * Octopus PRO XL v6.1.00 — Laser Harp Groovebox
 * © 2026 DIODAC ELECTRONICS / iSystem. All Rights Reserved.
 *
 * PROPRIETARY AND CONFIDENTIAL. Unauthorized copying, distribution, modification,
 * or use of this software or firmware, in whole or in part, is strictly prohibited
 * without prior written permission from DIODAC ELECTRONICS.
 * display.cpp — v6.1.00  OLED UI — SINGLE WRITER
 *
 * renderUIState() is the only runtime draw entry (display_refresh_task, Core 0).
 * Dashboards: LASER HARP, SEQUENCER, APP CONNECTED splash.  Menus L1/L2/L3,
 * song editor, SEQ matrix delegate, telemetry scopes, transport glyphs.
 * displayFlushDiff() — page-dirty I2C updates under i2cMutex.
 * ═════════════════════════════════════════════════════════════════════════════ */
#include "display.h"
#include "interface.h"
#include <cmath>
#include "arp.h"
#include "globals.h"
#include "effect.h"
#include "assets.h"
#include "dbeam.h"
#include "groovebox.h"
#include "patches.h"
#include "fog.h"

/* ── File-scope helpers ──────────────────────────────────────────────────── */
static const char kChainTokens[4] = { 'A', 'B', 'C', 'D' };

/* Panel is a 1.3" SH1106 128x64 — the SH1106G driver applies the SH1106's
 * 2-pixel column offset internally.  (An SH1107 driver leaves the panel blank.) */
Adafruit_SH1106G display = Adafruit_SH1106G(128, 64, &Wire);

/* Forward decl — drawSeqDashboard (above) uses the transport glyph defined later. */
static void drawTransportGlyph(int16_t x, int16_t y, bool playing,
                               bool recording, uint16_t color);

static uint8_t s_oledShadow[SCREEN_W * (SCREEN_H / 8)];
static bool    s_oledShadowValid = false;

void displayFlushDiff() {
  if (!hasOLED) return;
  uint8_t* buf = display.getBuffer();
  if (!buf) {                       /* safety: no accessible buffer → full flush */
    display.display();
    s_oledShadowValid = false;
    return;
  }
  constexpr int kPages  = SCREEN_H / 8;   /* 8 pages of 8 px                     */
  constexpr int kColOfs = 2;              /* SH1106 128-panel centering offset   */

  for (int page = 0; page < kPages; ++page) {
    const uint8_t* src = buf          + page * SCREEN_W;
    uint8_t*       dst = s_oledShadow + page * SCREEN_W;
    if (s_oledShadowValid && memcmp(src, dst, SCREEN_W) == 0)
      continue;                           /* page unchanged → skip the bus       */

    /* Command stream (control 0x00): set page + column window. */
    Wire.beginTransmission(I2C_ADDR_OLED);
    Wire.write((uint8_t)0x00);
    Wire.write((uint8_t)(0xB0 | page));            /* page address               */
    Wire.write((uint8_t)(0x00 | (kColOfs & 0x0F)));/* lower column nibble         */
    Wire.write((uint8_t)(0x10 | (kColOfs >> 4)));  /* higher column nibble        */
    Wire.endTransmission();

    /* Data stream in 64-byte Wire chunks (reduces I2C overhead per page). */
    static constexpr int kChunkSz = 64;
    for (int col = 0; col < SCREEN_W; col += kChunkSz) {
      Wire.beginTransmission(I2C_ADDR_OLED);
      Wire.write((uint8_t)0x40);
      Wire.write(src + col, (size_t)std::min(kChunkSz, SCREEN_W - col));
      Wire.endTransmission();
    }
    memcpy(dst, src, SCREEN_W);
  }
  s_oledShadowValid = true;
}

/* drawSaveToastIfActive — SAVING / RESETTING / SAVED / FAIL pill after NVS. */
void drawSaveToastIfActive() {
  const bool resetting = g_resetInProgress.load(std::memory_order_acquire);
  const bool saving = !resetting &&
      g_saveRequest.load(std::memory_order_acquire);
  const bool failed = !resetting && !saving &&
      (int32_t)(g_saveFailFlashMs.load(std::memory_order_relaxed) - millis()) > 0;
  const bool saved  = !resetting && !saving && !failed &&
      (int32_t)(g_saveFlashMs.load(std::memory_order_relaxed) - millis()) > 0;
  if (!resetting && !saving && !saved && !failed) return;

  const int16_t w = resetting ? 52 : (saving ? 54 : (failed ? 40 : 46)), h = 13;
  const int16_t x = (SCREEN_W - w) / 2, y = 1;
  display.fillRoundRect(x, y, w, h, 3, SH110X_WHITE);
  display.setTextColor(SH110X_BLACK);
  display.setTextSize(1);
  display.setCursor(x + (failed ? 8 : (resetting ? 4 : 6)), y + 3);
  display.print(resetting ? F("RESET") : (saving ? F("SAVING") : (failed ? F("FAIL!") : F("SAVED"))));
  display.setTextColor(SH110X_WHITE);
}

/* pgm_read preset name safely into a stack buffer */
static inline void readPresetName(int pi, char* buf) {
  pi = std::max(0, std::min(NUM_PATCHES - 1, pi));
  for (int i = 0; i < 15; ++i)
    buf[i] = (char)pgm_read_byte(&PRESET_NAMES[pi][i]);
  buf[15] = '\0';
}

/* ═══════════════════════════════════════════════════════════════════════════
 * TELEMETRY — system report (TelemetryView::STACK_STATS)
 * g_stackStats + g_audio_load_pct refreshed every 5 s by updateTaskStackStats().
 * ═══════════════════════════════════════════════════════════════════════════ */
static void drawStackStatsTelemetry() {
  const TaskStackStats& s = g_stackStats;
  const unsigned cpu = (unsigned)g_audio_load_pct.load(std::memory_order_relaxed);

  display.setTextSize(1);
  display.setTextColor(SH110X_WHITE);
  display.setCursor(0, 0);
  display.print(F("SYSTEM REPORT"));
  display.setCursor(92, 0);
  display.printf("%u%%", cpu);
  display.drawFastHLine(0, 10, SCREEN_W, SH110X_WHITE);

  display.setCursor(0, 14);
  display.printf("CPU LOAD : %u%%", cpu);
  display.setCursor(0, 24);
  display.printf("DRAM FREE: %u B", (unsigned)s.dramFree);
  display.setCursor(0, 34);
  display.printf("PSRAM    : %u B", (unsigned)s.psramFree);
  display.setCursor(0, 44);
  display.printf("MOD FREQ : %.2f kHz", (float)DBEAM_CARRIER_FREQ_HZ / 1000.0f);

  display.setCursor(0, 54);
  display.print(s.minFree < 512u ? F("!! STACK LOW") : F("Turn enc to cycle"));
}

/* ═══════════════════════════════════════════════════════════════════════════
 * TELEMETRY — DC SERVO BIAS value box (encoder view 2)
 *
 * Numeric DC servo bias readout (volts + raw ADC counts).
 * ═══════════════════════════════════════════════════════════════════════════ */
static void drawDcValueBox() {
  /* Average the snapshot DC level across all strings (patchMux-protected). */
  uint32_t sum = 0; int n = 0;
  portENTER_CRITICAL(&patchMux);
  for (int i = 0; i < MAX_STRINGS; ++i) {
    const int dc = g_last_good_data[i].dcLevel;
    if (dc > 0) { sum += (uint32_t)dc; ++n; }
  }
  portEXIT_CRITICAL(&patchMux);
  const float counts = n ? (float)sum / (float)n : 0.0f;
  const float volts  = counts * (3.3f / 4096.0f);

  display.setTextSize(1);
  display.setTextColor(SH110X_WHITE);
  display.setCursor(0, 0);   display.print(F("DC SERVO BIAS"));
  display.setCursor(92, 0);  display.print(F("CPU0"));
  display.drawFastHLine(0, 10, SCREEN_W, SH110X_WHITE);

  /* Big voltage read-out */
  display.setTextSize(2);
  display.setCursor(8, 22);
  display.printf("%.2f V", volts);

  /* Detail line: raw counts + ideal mid-rail target */
  display.setTextSize(1);
  display.setCursor(8, 45);
  display.printf("RAW:%4d  MID:1.65V", (int)counts);
  display.setCursor(8, 55);
  display.print(F("Turn enc to cycle"));
}

/* ═══════════════════════════════════════════════════════════════════════════
 * TELEMETRY OSCILLOSCOPE
 *
 * Plot area y=12..63. Graticule divisions sized for beam amplitude views.
 * (16 px = bigger time/div feel) with a dashed centre line marking the 1.65 V
 * mid-rail.  RAW_AC is drawn AC-coupled: zero signal rests on the centre line
 * and the envelope is mirrored above/below it (true AC look).  The unipolar
 * views (threshold / CC / SNR) stay bottom-referenced as magnitudes.
 * ═══════════════════════════════════════════════════════════════════════════ */
static constexpr int SCOPE_TOP = 12;
static constexpr int SCOPE_BOT = 63;
static constexpr int SCOPE_MID = (SCOPE_TOP + SCOPE_BOT) / 2;   /* ≈37 */

/* DAC AGC threshold — 8 per-string bars (fixed 12-bit scale 0..4095). */
static void drawDacThresholdBars() {
  display.setTextSize(1);
  display.setTextColor(SH110X_WHITE);
  display.setCursor(0, 0);  display.print(F("DAC AGC THRESH"));

  uint16_t vals[8];
  uint16_t mx = 0;
  for (int s = 0; s < 8; ++s) {
    vals[s] = getHardwareDACThreshold(s);
    if (vals[s] > mx) mx = vals[s];
  }
  display.setCursor(86, 0); display.printf("mx%4u", mx);
  display.drawFastHLine(0, 10, SCREEN_W, SH110X_WHITE);

  constexpr float scale = 4095.0f;   /* fixed 12-bit DAC full scale (0..4095) */
  constexpr int top  = 13;     /* bar-area top  */
  constexpr int base = 55;     /* baseline (labels below) */
  constexpr int colW = 16;     /* 128 / 8 strings */
  constexpr int barW = 10;     /* centred bar within each column */

  for (int s = 0; s < 8; ++s) {
    const int x0 = s * colW;
    const int cx = x0 + (colW - barW) / 2;
    int h = (int)(((float)vals[s] / scale) * (float)(base - top) + 0.5f);
    if (h < 0)            h = 0;
    if (h > base - top)   h = base - top;
    display.drawRect(x0 + 1, top, colW - 2, base - top, SH110X_WHITE); /* cell frame */
    if (h > 0) display.fillRect(cx, base - h, barW, h, SH110X_WHITE);  /* level fill */
    display.setCursor(x0 + 5, 57);                                     /* 1..8 label */
    display.printf("%d", s + 1);
  }
}

/* Edge-comp editor — 8 per-string vertical bars (telemetry-style). */
static void drawEdgeCompEditor() {
  display.setTextSize(1);
  display.setTextColor(SH110X_WHITE);
  /* Header: active scale name (OC scrolls); selected string value right-aligned. */
  const int      escale = edgeEditScale.load(std::memory_order_relaxed) & (NUM_SCALES - 1);
  const uint8_t* edge   = edgeCompFor(escale);
  const int      sel    = edgeEditSel.load(std::memory_order_relaxed) & 7;
  display.setCursor(0, 0);
  display.print(SCALES[escale].name);
  char val[6];
  snprintf(val, sizeof val, "%u%%", (unsigned)edge[sel]);
  display.setCursor(SCREEN_W - (int)strlen(val) * 6, 0);
  display.print(val);
  display.drawFastHLine(0, 10, SCREEN_W, SH110X_WHITE);

  constexpr int top  = 13;
  constexpr int base = 55;
  constexpr int colW = 16;     /* 128 / 8 strings */
  constexpr int barW = 10;
  const float   span = (float)(EDGE_COMP_PCT_MAX - EDGE_COMP_PCT_MIN);

  /* ×1.00 reference line */
  int yRef = base - (int)(((100.0f - (float)EDGE_COMP_PCT_MIN) / span)
                          * (float)(base - top) + 0.5f);
  if (yRef < top)  yRef = top;
  if (yRef > base) yRef = base;
  for (int x = 0; x < SCREEN_W; x += 4) display.drawPixel(x, yRef, SH110X_WHITE);

  for (int s = 0; s < 8; ++s) {
    const int  x0     = s * colW;
    const int  cx     = x0 + (colW - barW) / 2;
    const bool broken = stringActive[s];   /* live physical-hold truth */
    int h = (int)((((float)edge[s] - (float)EDGE_COMP_PCT_MIN) / span)
                  * (float)(base - top) + 0.5f);
    if (h < 0)          h = 0;
    if (h > base - top) h = base - top;

    display.drawRect(x0 + 1, top, colW - 2, base - top, SH110X_WHITE);
    if (broken) {                                   /* beam broken NOW → invert */
      display.fillRect(x0 + 2, top + 1, colW - 4, base - top - 2, SH110X_WHITE);
      if (h > 0) display.fillRect(cx, base - h, barW, h, SH110X_BLACK);
    } else {
      if (h > 0) display.fillRect(cx, base - h, barW, h, SH110X_WHITE);
    }

    if (s == sel) {                                 /* selected → inverted label */
      display.fillRect(x0 + 3, 56, 9, 9, SH110X_WHITE);
      display.setTextColor(SH110X_BLACK);
    }
    display.setCursor(x0 + 5, 57);
    display.printf("%d", s + 1);
    display.setTextColor(SH110X_WHITE);
  }
}

/* Fog reject — 8 per-string bars (g_fogAmp[], auto-ranged with floor/accept lines). */
static void drawFogRejectBars() {
  display.setTextSize(1);
  display.setTextColor(SH110X_WHITE);
  display.setCursor(0, 0);  display.print(F("FOG REJECT"));

  const bool  on     = fogRejectEnabled.load(std::memory_order_relaxed);
  const int   margin = fogRejectMargin .load(std::memory_order_relaxed);
  float amp[8];
  float mx = 0.0f;
  for (int s = 0; s < 8; ++s) {
    amp[s] = g_fogAmp[s].load(std::memory_order_relaxed);
    if (amp[s] > mx) mx = amp[s];
  }
  const float floorAmp = fogFloor();
  const float accept   = floorAmp + (float)margin;

  char hdr[14];
  if (on) snprintf(hdr, sizeof hdr, "ON m%d", margin);
  else    snprintf(hdr, sizeof hdr, "OFF");
  display.setCursor(SCREEN_W - (int)strlen(hdr) * 6, 0);  display.print(hdr);
  display.drawFastHLine(0, 10, SCREEN_W, SH110X_WHITE);

  constexpr int top  = 13;
  constexpr int base = 55;
  constexpr int colW = 16;
  constexpr int barW = 10;
  /* Auto-range to the tallest bar (min 300 mV) so the differential fills the
   * screen; the floor/accept lines ride the SAME scale so they stay truthful. */
  const float scale = std::max(mx, std::max(accept, 300.0f));

  auto yFor = [&](float v) {
    int y = base - (int)((v / scale) * (float)(base - top) + 0.5f);
    if (y < top)  y = top;
    if (y > base) y = base;
    return y;
  };

  const int yFloor  = yFor(floorAmp);
  const int yAccept = yFor(accept);
  for (int x = 0; x < SCREEN_W; x += 4) display.drawPixel(x, yFloor, SH110X_WHITE);  /* dotted floor */
  if (on)
    for (int x = 0; x < SCREEN_W; x += 2) display.drawPixel(x, yAccept, SH110X_WHITE); /* dashed accept */

  for (int s = 0; s < 8; ++s) {
    const int  x0   = s * colW;
    const int  cx   = x0 + (colW - barW) / 2;
    const bool pass = on && (amp[s] >= accept);
    int h = (int)((amp[s] / scale) * (float)(base - top) + 0.5f);
    if (h < 0)          h = 0;
    if (h > base - top) h = base - top;

    display.drawRect(x0 + 1, top, colW - 2, base - top, SH110X_WHITE);
    if (pass) {                                   /* would-pass → invert cell */
      display.fillRect(x0 + 2, top + 1, colW - 4, base - top - 2, SH110X_WHITE);
      if (h > 0) display.fillRect(cx, base - h, barW, h, SH110X_BLACK);
    } else if (h > 0) {
      display.fillRect(cx, base - h, barW, h, SH110X_WHITE);
    }
    display.setCursor(x0 + 5, 57);
    display.printf("%d", s + 1);
  }
}

static void drawTelemetryOscilloscope() {
  display.setTextSize(1);
  display.setTextColor(SH110X_WHITE);
  const TelemetryView av = currentScopeView.load(std::memory_order_relaxed);
  if (av == TelemetryView::STACK_STATS)  { drawStackStatsTelemetry(); return; }
  if (av == TelemetryView::DC_LEVEL)     { drawDcValueBox();          return; }
  if (av == TelemetryView::CAL_BASELINE) { drawDacThresholdBars();    return; }
  if (av == TelemetryView::FOG_REJECT)   { drawFogRejectBars();       return; }

  /* CAL_BASELINE handled above (8 per-string bars); the remaining views share
   * the auto-ranged scope trace below. */
  display.setCursor(0, 0);
  switch (av) {
  case TelemetryView::RAW_AC:       display.print(F("TRUE RMS SCOPE")); break;
  case TelemetryView::CC_OUT_14BIT: display.print(F("DBEAM EXPR"));   break;
  case TelemetryView::SIGNAL_SNR:   display.print(F("SIGNAL SNR"));  break;
  default: return;
  }
  display.setCursor(92, 0); display.print(F("CPU0"));
  display.drawFastHLine(0, 10, SCREEN_W, SH110X_WHITE);

  const bool acCoupled = (av == TelemetryView::RAW_AC);

  /* Graticule — vertical division lines every 16 px (larger time/div). */
  for (int x = 0; x < SCREEN_W; x += 16)
    for (int y = SCOPE_TOP; y <= SCOPE_BOT; y += 4)
      display.drawPixel(x, y, SH110X_WHITE);
  if (acCoupled) {
    /* Dashed 1.65 V mid-rail reference line */
    for (int x = 0; x < SCREEN_W; x += 4)
      display.drawPixel(x, SCOPE_MID, SH110X_WHITE);
  } else {
    /* Baseline at the bottom for magnitude views */
    display.drawFastHLine(0, SCOPE_BOT, SCREEN_W, SH110X_WHITE);
  }

  int rp = scopeWritePtr.load(std::memory_order_relaxed);
  const int plotH = SCOPE_BOT - SCOPE_TOP;
  auto mapY = [acCoupled, plotH](uint8_t v) -> int {
    if (acCoupled) {
      const int half = plotH / 2;
      const int dev = (int)(((float)v * (float)half) / 255.f);
      return std::min(SCOPE_BOT, std::max(SCOPE_TOP, SCOPE_MID - dev));
    }
    return std::min(SCOPE_BOT, std::max(SCOPE_TOP,
                    SCOPE_BOT - (int)(((float)v * (float)plotH) / 255.f)));
  };

  int px = 0, py = mapY(scopeHistory[rp]);
  for (int x = 1; x < SCREEN_W; ++x) {
    rp = (rp + 1) % 128;
    const int cy = mapY(scopeHistory[rp]);
    display.writeLine(px, py, x, cy, SH110X_WHITE);
    if (acCoupled) {   /* mirror envelope below the mid-rail for AC look */
      display.writeLine(px, 2 * SCOPE_MID - py, x, 2 * SCOPE_MID - cy, SH110X_WHITE);
    }
    px = x; py = cy;
  }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * D-BEAM BARGRAPH
 * ═══════════════════════════════════════════════════════════════════════════ */
static void drawDbeamBargraph() {
  display.drawRect(0, DASH_DBEAM_Y, SCREEN_W, SCREEN_H - DASH_DBEAM_Y, SH110X_WHITE);
  if (!dbeamEnabled.load(std::memory_order_relaxed)) {
    display.setCursor(26, DASH_DBEAM_Y + 1);
    display.print(F("- BYPASSED -"));
    return;
  }
  const uint16_t amp = (uint16_t)std::min<uint32_t>(16383u,
                       dbeamAmplitude.load(std::memory_order_relaxed));
  const int fw = (int)(((uint32_t)amp * 126u) / 16383u);
  if (fw > 0) display.fillRect(1, DASH_DBEAM_Y + 1, fw,
                                SCREEN_H - DASH_DBEAM_Y - 2, SH110X_WHITE);
  /* D-BEAM route label overlay */
  const DbeamRoute mode = currentDbeamRoute.load(std::memory_order_relaxed);
  if (mode != DbeamRoute::OFF) {
    display.setTextColor(fw > 96 ? SH110X_BLACK : SH110X_WHITE);
    display.setCursor(SCREEN_W - 22, DASH_DBEAM_Y + 1);
    display.print(safeDbeamRouteName((int)mode));
    display.setTextColor(SH110X_WHITE);
  }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * SEQUENCER DASHBOARD
 *
 * Layout (128×64):
 *   y= 0  SEQ  [kit]              transport glyph (●/▶/■)
 *   y=10  divider
 *   y=12  [BANK:A  LEN:16]        inverted highlight band
 *   y=30  TRSP:+0                 root note (right)
 *   y=41  BPM:120  H■ S■ D■       muted buses only
 *   y=56  full-width 16-cell playhead (+ page pips when LEN>16)
 * ═══════════════════════════════════════════════════════════════════════════ */
/* Tracker-style absolute root note (e.g. "C-4") from base MIDI + octave shift. */
static inline const char* midiRootNote(int baseMidi, int shift) {
  static const char* const NN[12] =
    { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
  static char nb[6];
  int m = baseMidi + 12 * shift;
  if (m < 0)   m = 0;
  if (m > 127) m = 127;
  const int pc  = m % 12;
  const int oct = m / 12 - 1;            /* MIDI 60 → C-4 */
  if (pc == 0) snprintf(nb, sizeof(nb), "C-%d", oct);     /* keep the classic dash form */
  else         snprintf(nb, sizeof(nb), "%s%d", NN[pc], oct);
  return nb;
}

static void drawSeqDashboard() {
  display.setTextSize(1);
  display.setTextColor(SH110X_WHITE);

  /* Header: title + active drum-kit tag + recording/playing status */
  display.setCursor(0, HEADER_Y); display.print(F("SEQ"));
  {
    const uint8_t dk = drumKit.load(std::memory_order_relaxed);
    display.setCursor(4 * CHAR_W, HEADER_Y);
    display.print((dk < (uint8_t)DrumKitId::COUNT) ? DRUM_KIT_NAMES[dk] : "?");
  }
  {
    /* Graphic transport glyph only (● rec / ▶ play / ■ stop) — the bracket text
     * tag was retired now that the icon carries the state. */
    const bool rec  = seqRecording.load(std::memory_order_relaxed);
    const bool play = seqPlaying  .load(std::memory_order_relaxed);
    drawTransportGlyph(SCREEN_W - 8, HEADER_Y, play, rec, SH110X_WHITE);
  }
  display.drawFastHLine(0, DIVIDER_Y, SCREEN_W, SH110X_WHITE);

  /* Highlight band: bank, chain name, length */
  display.fillRect(0, DASH_PATCH_Y, SCREEN_W, DASH_PATCH_H, SH110X_WHITE);
  display.setTextColor(SH110X_BLACK);
  display.setCursor(LABEL_X, DASH_PATCH_Y + 4);
  /* Banks A-D; active pattern chain pinned to SEQ_UI_CHAIN (0). */
  const int  bank  = std::min(3, (int)seqActiveBank.load(std::memory_order_relaxed) & 15);
  const int  len   = (int)seqLength.load(std::memory_order_relaxed);
  display.printf("BANK:%c  LEN:%02d", 'A' + bank, len);
  display.setTextColor(SH110X_WHITE);

  /* Transpose + root note */
  display.setCursor(0, DASH_LINE2_Y);
  display.printf("TRSP:%+d", (int)seqTranspose.load(std::memory_order_relaxed));
  const char* sroot = midiRootNote(60, (int)octaveShift[1].load(std::memory_order_relaxed));
  display.setCursor(SCREEN_W - CHAR_W * (int)strlen(sroot), DASH_LINE2_Y);
  display.print(sroot);

  /* Line 3 — BPM first; H/S/D + filled square only when that bus is muted */
  display.setCursor(0, DASH_LINE3_Y);
  display.printf("BPM:%d", (int)seqBpm.load(std::memory_order_relaxed));
  int muteX = display.getCursorX() + CHAR_W;

  if (mixHarpMute.load(std::memory_order_relaxed)) {
    display.setCursor(muteX, DASH_LINE3_Y);
    display.print(F("H"));
    DRAW_MINIBAR(muteX + CHAR_W, DASH_LINE3_Y, 8, true);
    muteX += CHAR_W + 8 + CHAR_W;
  }
  if (mixSeqMute.load(std::memory_order_relaxed)) {
    display.setCursor(muteX, DASH_LINE3_Y);
    display.print(F("S"));
    DRAW_MINIBAR(muteX + CHAR_W, DASH_LINE3_Y, 8, true);
    muteX += CHAR_W + 8 + CHAR_W;
  }
  if (mixDrumsMute.load(std::memory_order_relaxed)) {
    display.setCursor(muteX, DASH_LINE3_Y);
    display.print(F("D"));
    DRAW_MINIBAR(muteX + CHAR_W, DASH_LINE3_Y, 8, true);
  }

  /* Full-width page-aware playhead (matches App P1-P4 paging for >16-step patterns) */
  DRAW_STEP_BARGRAPH(0, 56, SCREEN_W,
                     (uint16_t)seqCurrentStep.load(std::memory_order_relaxed),
                     (uint16_t)len);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * HARP DASHBOARD
 *
 * Layout (128×64):
 *   y= 0  LASER HARP
 *   y=10  divider
 *   y=12  [#001 Preset Name     ] inverted highlight (with scrolling name)
 *   y=30  Scale: 01 Major  Oct:+0
 *   y=41  MODE: POLY8  DBEAM: OFF
 *   y=55  ████▓▓▓░░░░░░░░░       D-BEAM bargraph
 * ═══════════════════════════════════════════════════════════════════════════ */
static void drawHarpDashboard() {
  const uint32_t now = millis();
  display.setTextSize(1);
  display.setTextColor(SH110X_WHITE);

  /* Header */
  display.setCursor(0, HEADER_Y); display.print(F("LASER HARP"));
  display.drawFastHLine(0, DIVIDER_Y, SCREEN_W, SH110X_WHITE);

  /* Highlight band: patch index + scrolling preset name */
  display.fillRect(0, DASH_PATCH_Y, SCREEN_W, DASH_PATCH_H, SH110X_WHITE);
  display.setTextColor(SH110X_BLACK);
  const int pi = harpPatchIndex.load(std::memory_order_relaxed) & (NUM_PATCHES - 1);
  char pname[16];
  readPresetName(pi, pname);
  display.setCursor(LABEL_X, DASH_PATCH_Y + 2);
  display.printf("#%03d ", pi + 1);
  /* Scrolling preset name — fits in remaining width */
  drawScrollingText(LABEL_X + 28, DASH_PATCH_Y + 2,
                    SCREEN_W - LABEL_X - 28, pname, now);
  display.setTextColor(SH110X_WHITE);

  /* Line 2 — scale name + root note, right-anchored */
  const int si = harpScaleIndex.load(std::memory_order_relaxed) & (NUM_SCALES - 1);
  display.setCursor(0, DASH_LINE2_Y);
  display.print(SCALES[si].name);
  const char* root = midiRootNote(SCALES_NOTES[si][0],
                                  (int)octaveShift[0].load(std::memory_order_relaxed));
  display.setCursor(SCREEN_W - CHAR_W * (int)strlen(root), DASH_LINE2_Y);
  display.print(root);

  /* Line 3 — play mode + fixed-position BEAM ON/OFF */
  const uint8_t pm = (uint8_t)std::min<uint8_t>(2u,
    (uint8_t)currentPlayMode.load(std::memory_order_relaxed));
  display.setCursor(0, DASH_LINE3_Y);
  display.print(kPMNames[pm]);
  display.setCursor(SCREEN_W - CHAR_W * 8, DASH_LINE3_Y);   /* fixed anchor */
  display.printf("BEAM:%s",
                 dbeamEnabled.load(std::memory_order_relaxed) ? "ON" : "OFF");

  drawDbeamBargraph();
}

/* ═══════════════════════════════════════════════════════════════════════════
 * SCROLLABLE LIST — menu L1 / L2 navigation with centred highlight
 * ═══════════════════════════════════════════════════════════════════════════ */
static void drawScrollList(const char* title, const char* const* arr,
                            int count, int sel) {
  if (!arr || count <= 0) return;
  display.setTextSize(1);
  display.setTextColor(SH110X_WHITE);
  display.setCursor(0, HEADER_Y); display.print(title);
  display.drawFastHLine(0, DIVIDER_Y, SCREEN_W, SH110X_WHITE);

  sel = std::max(0, std::min(count - 1, sel));
  /* Always keep selection at row 2 (centre of 5-row window) when list scrolls */
  const bool scrolls = (count > LIST_MAX_ITEMS);
  const int  row0    = scrolls ? 2 : sel;
  const int  top     = sel - row0;

  for (int r = 0; r < LIST_MAX_ITEMS; ++r) {
    int idx;
    if (scrolls) idx = (((top + r) % count) + count) % count;
    else         idx = top + r;
    if (!scrolls && (idx < 0 || idx >= count)) continue;
    const int vy = LIST_START_Y + r * LIST_ITEM_H;
    if (r == row0) {
      display.fillRect(0, vy - 1, SCREEN_W, LIST_ITEM_H, SH110X_WHITE);
      display.setTextColor(SH110X_BLACK);
    } else {
      display.setTextColor(SH110X_WHITE);
    }
    display.setCursor(LABEL_X, vy);
    display.print(arr[idx]);
    display.setTextColor(SH110X_WHITE);
  }
}

static void drawMenuL1() {
  /* L1 menu uses regrouped display order (kL1Order). */
  const int slot = l1SlotForCat(
                     (int)currentMenuL1.load(std::memory_order_relaxed));
  drawScrollList("MAIN MENU", kL1NamesOrdered, kL1Count, slot);
}
static void drawMenuL2() {
  const int l1 = std::max(0, std::min(kL1Count - 1,
                 (int)currentMenuL1.load(std::memory_order_relaxed)));
  drawScrollList(safeL1Name(l1), l2TableFor(l1), l2CountFor(l1),
                 (int)currentMenuL2.load(std::memory_order_relaxed));
}

/* ═══════════════════════════════════════════════════════════════════════════
 * formatParamValueString — human-readable value for L3 edit screen.
 * ═══════════════════════════════════════════════════════════════════════════ */
static void formatParamValueString(int l1, int l2, char* out, size_t maxB) {
  if (!out || maxB == 0) return;
  out[0] = '\0';
  /* Guard: l2 must be within bounds for this l1 */
  if (l1 < 0 || l1 >= kL1Count || l2 < 0 || l2 >= l2CountFor(l1)) return;
  const int si = harpScaleIndex.load(std::memory_order_relaxed) & (NUM_SCALES - 1);

  switch (l1) {

  /* ── 0: HARP SETUP ────────────────────────────────────────────────────── */
  case 0:
    switch (l2) {
    case 0: snprintf(out, maxB, "%u ms", (unsigned)beamGateHoldMs); return;
    case 1: snprintf(out, maxB, "%u",    scaleWhiteLevel[si]);      return;
    case 2: snprintf(out, maxB, "%u",    scaleTouchConfirm[si]);    return;
    case 3: snprintf(out, maxB, "%u",    scaleReleaseConfirm[si]);  return;
    case 4: snprintf(out, maxB, "R %u",  scaleR[si]);               return;
    case 5: snprintf(out, maxB, "G %u",  scaleG[si]);               return;
    case 6: snprintf(out, maxB, "B %u",  scaleB[si]);               return;
    /* [margin] Show which scale this per-scale margin belongs to. */
    case 7: snprintf(out, maxB, "%u  %s", scaleMargin[si], SCALES[si].name); return;
    /* Anti-stuck release timeout (0 = disabled). */
    case 8: if (beamStuckReleaseMs == 0u) snprintf(out, maxB, "OFF");
            else snprintf(out, maxB, "%u ms", (unsigned)beamStuckReleaseMs);
            return;
    /* [FOG] Fog-reject enable + differential margin. */
    case 10: snprintf(out, maxB, "%s", fogRejectEnabled.load(std::memory_order_relaxed) ? "ON" : "OFF"); return;
    case 11: snprintf(out, maxB, "%d mV", fogRejectMargin.load(std::memory_order_relaxed)); return;
    /* Closed-harp idle screensaver toggle. */
    case 12: snprintf(out, maxB, "%s", laserScreensaver.load(std::memory_order_relaxed) ? "ON" : "OFF"); return;
    } break;

  /* ── 1: D-BEAM ────────────────────────────────────────────────────────── */
  case 1:
    switch (l2) {
    case 0: snprintf(out, maxB, "%d",   dbeamHWCfg.offsetAdc); return;
    case 1: snprintf(out, maxB, "%d",   dbeamHWCfg.rangeAdc);  return;
    case 2: snprintf(out, maxB, "%s",   safeDBCurveName(
                (int)currentDbeamCurve.load(std::memory_order_relaxed))); return;
    case 3: snprintf(out, maxB, "%s",   dbeamEnabled.load() ? "ON" : "OFF"); return;
    case 4: snprintf(out, maxB, "%.2f", dbeamExprAttack.load(std::memory_order_relaxed));  return;
    case 5: snprintf(out, maxB, "%.3f", dbeamExprRelease.load(std::memory_order_relaxed)); return;
    case 6: snprintf(out, maxB, "%s",   safeDbeamRouteNameFull((int)currentDbeamRoute.load(std::memory_order_relaxed))); return;
    case 7: snprintf(out, maxB, "%s",   safeDbeamTargetName((int)currentDbeamTarget.load(std::memory_order_relaxed))); return;
    } break;

  /* ── 2: MASTER ─────────────────────────────────────────────────────────── */
  case 2:
    switch (l2) {
    case 0:  snprintf(out, maxB, "%d%%",   (int)(masterVol.load()   *100.f)); return;
    case 1:  snprintf(out, maxB, "%d%%",   (int)(mixHarpVol.load()  *100.f)); return;
    case 2:  snprintf(out, maxB, "%d%%",   (int)(mixSeqVol.load()   *100.f)); return;
    case 3:  snprintf(out, maxB, "%d%%",   (int)(mixDrumsVol.load() *100.f)); return;
    case 4:  snprintf(out, maxB, "x%.2f", masterPitch.load());               return;
    case 5:  snprintf(out, maxB, "%s",    safeMasterFxName(masterFxIndex.load() & 15)); return;
    case 6:  snprintf(out, maxB, "%d%%",  (int)(drumRevSend.load()  *100.f)); return;
    case 7:  snprintf(out, maxB, "%d%%",  (int)(drumDlySend.load()  *100.f)); return;
    case 8:  snprintf(out, maxB, "%d%%",  (int)(tbDrive.load()      *100.f)); return;
    case 9:  snprintf(out, maxB, "%d%%",  (int)(tbTone.load()       *100.f)); return;
    case 10: snprintf(out, maxB, "%d%%",  (int)(tbMix.load()        *100.f)); return;
    case 11: snprintf(out, maxB, "%d%%",  (int)(djFreq.load()       *100.f)); return;
    case 12: snprintf(out, maxB, "%d%%",  (int)(djRes.load()        *100.f)); return;
    case 13: snprintf(out, maxB, "%d%%",  (int)(djMix.load()        *100.f)); return;
    case 14: snprintf(out, maxB, "%.1fdB",masterEqLow.load());               return;
    case 15: snprintf(out, maxB, "%.1fdB",masterEqHigh.load());              return;
    case 16: snprintf(out, maxB, "%s",    safeFxName(drumFxIndexA.load() & 15)); return;
    case 17: snprintf(out, maxB, "%s",    safeDynName(drumFxIndexB.load() & 15)); return;
    case 18: snprintf(out, maxB, "%s",    mixHarpMute .load() ? "MUTED" : "ON"); return;
    case 19: snprintf(out, maxB, "%s",    mixSeqMute  .load() ? "MUTED" : "ON"); return;
    case 20: snprintf(out, maxB, "%s",    mixDrumsMute.load() ? "MUTED" : "ON"); return;
    case 21: snprintf(out, maxB, "%+d%%",  (int)(mixHarpPan .load() * 100.f)); return;
    case 22: snprintf(out, maxB, "%+d%%",  (int)(mixSeqPan  .load() * 100.f)); return;
    case 23: snprintf(out, maxB, "%+d%%",  (int)(mixDrumsPan.load() * 100.f)); return;
    } break;

  /* ── 3: HARP SYNTH ───────────────────────────────────────────────────── */
  case 3:
    switch (l2) {
    case 0:  snprintf(out, maxB, "%s",     safeWaveName((int)harpWaveform.load())); return;
    case 1:  snprintf(out, maxB, "%.3fs",  harpAttack.load());     return;
    case 2:  snprintf(out, maxB, "%.3fs",  harpDecay.load());      return;
    case 3:  snprintf(out, maxB, "%d%%",   (int)(harpSustain.load()   *100.f)); return;
    case 4:  snprintf(out, maxB, "%.3fs",  harpRelease.load());    return;
    case 5:  snprintf(out, maxB, "%dHz",   (int)(harpCutoff.load()*10000.f+40.f)); return;
    case 6:  snprintf(out, maxB, "%d%%",   (int)(harpResonance.load() *100.f)); return;
    case 7:  snprintf(out, maxB, "%d%%",   (int)(harpNoise.load()     *100.f)); return;
    case 8:  snprintf(out, maxB, "%+.2f",  harpDetune.load());     return;
    case 9:  snprintf(out, maxB, "%.1fHz", harpLfoRate.load()*30.f); return;
    case 10: snprintf(out, maxB, "%d%%",   (int)(harpLfoDepth.load()  *100.f)); return;
    case 11: snprintf(out, maxB, "%s",     safeLfoRouteName((int)harpLfoRoute.load())); return;
    case 12: snprintf(out, maxB, "%s",     safeWaveName((int)harpOsc2Wave.load())); return;
    case 13: snprintf(out, maxB, "%d%%",   (int)(harpEnvCutAmount.load()*100.f)); return;
    case 14: snprintf(out, maxB, "%s",     safeFxName(harpFxIndex.load()  & 15)); return;
    case 15: snprintf(out, maxB, "%s",     safeDynName(harpFxIndexB.load() & 15)); return;
    case 16: { portENTER_CRITICAL(&patchMux); float v = fx.harpInsert.dly_send;
               portEXIT_CRITICAL(&patchMux); snprintf(out, maxB, "%d%%", (int)(v*100.f)); return; }
    case 17: { portENTER_CRITICAL(&patchMux); float v = fx.harpInsert.rev_send;
               portEXIT_CRITICAL(&patchMux); snprintf(out, maxB, "%d%%", (int)(v*100.f)); return; }
    case 18: { const int pi = harpPatchIndex.load(std::memory_order_relaxed) & (NUM_PATCHES - 1);
               char nm[16]; readPresetName(pi, nm);
               snprintf(out, maxB, "#%03d %s", pi + 1, nm); return; }
    case 19: case 20: { char nm[16];
               userSlotName(0u, userSlotCursor[0].load(std::memory_order_relaxed), nm, sizeof(nm));
               snprintf(out, maxB, "%s", nm); return; }
    case 21: snprintf(out, maxB, "%s", harpArpEnabled.load() ? "ON" : "OFF"); return;
    case 22: snprintf(out, maxB, "%s", arp::harpPatternName(harpArpPattern.load())); return;
    case 23: snprintf(out, maxB, "%s", arp::harpRateName(harpArpRate.load())); return;
    case 24: snprintf(out, maxB, "%d%%", (int)arp::GATE_PCT[arp::harpGateIndex(
                std::min<uint8_t>(3u, harpArpGate.load()))]); return;
    } break;

  /* ── 4: MIDI I/O ──────────────────────────────────────────────────────── */
  case 4:
    switch (l2) {
    case 0: snprintf(out, maxB, "+/-%d semi", (int)pbMapping.upSemi.load(std::memory_order_relaxed)); return;
    case 1: snprintf(out, maxB, "%s", pbMapping.enabled.load(std::memory_order_relaxed) ? "ON" : "OFF"); return;
    case 2: snprintf(out, maxB, "Ch %d", (int)wireHarpMidiChannel.load()); return;
    case 3: snprintf(out, maxB, "Ch %d", (int)wireSeqMidiChannel.load());  return;
    case 4: snprintf(out, maxB, "Ch %d", (int)wireDrumMidiChannel.load()); return;
    } break;

  /* ── 5: SEQ SETUP ─────────────────────────────────────────────────────── */
  case 5:
    switch (l2) {
    case 0: snprintf(out, maxB, "Bank %c",
                kChainTokens[std::min(3, (int)seqActiveBank.load() & 15)]); return;
    case 1: snprintf(out, maxB, "View %s",
                seqUI_page.load(std::memory_order_relaxed) ? "Drum" : "Synth"); return;
    case 2: snprintf(out, maxB, "%+d semi",  (int)seqTranspose.load()); return;
    case 3: snprintf(out, maxB, "%d steps",  (int)seqLength.load());    return;
    case 4: snprintf(out, maxB, "Syn %d/%d",
                (int)g_lastSynthPreset.load() + 1, NUM_SYNTH_PATS); return;
    case 5: snprintf(out, maxB, "Drm %d/%d",
                (int)g_lastDrumPreset.load()  + 1, NUM_DRUM_PATS);  return;
    case 6: snprintf(out, maxB, "Press ENC"); return;  /* Clear — confirm on ENC */
    case 7: case 8: {
      char nm[16];
      userPatName((uint8_t)userPatCursor.load(std::memory_order_relaxed), nm, sizeof(nm));
      snprintf(out, maxB, "%s", nm);
      return;
    }
    case 9: snprintf(out, maxB, "%s", seqArpEnabled.load() ? "ON" : "OFF"); return;
    case 10: snprintf(out, maxB, "%s", arp::patternName(seqArpPattern.load())); return;
    case 11: snprintf(out, maxB, "%s", arp::rateName(seqArpRate.load())); return;
    case 12: snprintf(out, maxB, "%d%%", (int)arp::GATE_PCT[std::min(7, (int)seqArpGate.load())]); return;
    } break;

  /* ── 6: SEQ MATRIX ────────────────────────────────────────────────────── */
  case 6: snprintf(out, maxB, "Press ENC"); return;

  /* ── 7: AUX FX ───────────────────────────────────────────────────────── */
  case 7:
    switch (l2) {
    case 0: snprintf(out, maxB, "%.2fs",  masterAuxDlyTime.load()); return;
    case 1: snprintf(out, maxB, "%d%%",   (int)(masterAuxDlyFb.load() /0.95f*100.f)); return;
    case 2: snprintf(out, maxB, "%.2f",   masterAuxRevSize.load()); return;
    case 3: snprintf(out, maxB, "%d%%",   (int)(masterAuxRevDamp.load() *100.f)); return;
    case 4: { portENTER_CRITICAL(&patchMux); float v = fx.harpInsert.dly_send;
               portEXIT_CRITICAL(&patchMux); snprintf(out, maxB, "%d%%", (int)(v*100.f)); return; }
    case 5: { portENTER_CRITICAL(&patchMux); float v = fx.harpInsert.rev_send;
               portEXIT_CRITICAL(&patchMux); snprintf(out, maxB, "%d%%", (int)(v*100.f)); return; }
    case 6: { portENTER_CRITICAL(&patchMux); float v = fx.seqInsert.dly_send;
               portEXIT_CRITICAL(&patchMux); snprintf(out, maxB, "%d%%", (int)(v*100.f)); return; }
    case 7: { portENTER_CRITICAL(&patchMux); float v = fx.seqInsert.rev_send;
               portEXIT_CRITICAL(&patchMux); snprintf(out, maxB, "%d%%", (int)(v*100.f)); return; }
    case 8:  snprintf(out, maxB, "%s", safeFxName(harpFxIndex.load()  & 15)); return;
    case 9:  snprintf(out, maxB, "%s", safeDynName(harpFxIndexB.load() & 15)); return;
    case 10: snprintf(out, maxB, "%s", safeFxName(seqFxIndex.load()   & 15)); return;
    case 11: snprintf(out, maxB, "%s", safeDynName(seqFxIndexB.load()  & 15)); return;
    case 12: snprintf(out, maxB, "%s", safeFxName(drumFxIndexA.load() & 15)); return;
    case 13: snprintf(out, maxB, "%s", safeDynName(drumFxIndexB.load() & 15)); return;
    case 14: snprintf(out, maxB, "%s", safeAuxSceneName(auxSceneIndex.load() & 15)); return;
    case 15: snprintf(out, maxB, "%s", linkAuxToInsertPreset.load() ? "ON" : "OFF"); return;
    } break;

  /* ── 8: SEQ SYNTH ─────────────────────────────────────────────────────── */
  case 8:
    switch (l2) {
    case 0:  snprintf(out, maxB, "%s",    safeWaveName((int)seqWaveform.load())); return;
    case 1:  snprintf(out, maxB, "%.3fs", seqAttack.load());    return;
    case 2:  snprintf(out, maxB, "%.3fs", seqDecay.load());     return;
    case 3:  snprintf(out, maxB, "%d%%",  (int)(seqSustain.load()   *100.f)); return;
    case 4:  snprintf(out, maxB, "%.3fs", seqRelease.load());   return;
    case 5:  snprintf(out, maxB, "%dHz",  (int)(seqCutoff.load()*10000.f+40.f)); return;
    case 6:  snprintf(out, maxB, "%d%%",  (int)(seqResonance.load() *100.f)); return;
    case 7:  snprintf(out, maxB, "%d%%",  (int)(seqNoise.load()     *100.f)); return;
    case 8:  snprintf(out, maxB, "%+.2f", seqDetune.load());    return;
    case 9:  snprintf(out, maxB, "%.1fHz",seqLfoRate.load()*30.f); return;
    case 10: snprintf(out, maxB, "%d%%",  (int)(seqLfoDepth.load()  *100.f)); return;
    case 11: snprintf(out, maxB, "%s",    safeLfoRouteName((int)seqLfoRoute.load())); return;
    case 12: snprintf(out, maxB, "%s",    safeWaveName((int)seqOsc2Wave.load())); return;
    case 13: snprintf(out, maxB, "%d%%",  (int)(seqEnvCutAmount.load()*100.f)); return;
    case 14: snprintf(out, maxB, "%s",    safeFxName(seqFxIndex.load()  & 15)); return;
    case 15: snprintf(out, maxB, "%s",    safeDynName(seqFxIndexB.load() & 15)); return;
    case 16: { portENTER_CRITICAL(&patchMux); float v = fx.seqInsert.dly_send;
               portEXIT_CRITICAL(&patchMux); snprintf(out, maxB, "%d%%", (int)(v*100.f)); return; }
    case 17: { portENTER_CRITICAL(&patchMux); float v = fx.seqInsert.rev_send;
               portEXIT_CRITICAL(&patchMux); snprintf(out, maxB, "%d%%", (int)(v*100.f)); return; }
    case 18: { const int pi = seqPatchIndex.load(std::memory_order_relaxed) & (NUM_PATCHES - 1);
               char nm[16]; readPresetName(pi, nm);
               snprintf(out, maxB, "#%03d %s", pi + 1, nm); return; }
    case 19: case 20: { char nm[16];
               userSlotName(1u, userSlotCursor[1].load(std::memory_order_relaxed), nm, sizeof(nm));
               snprintf(out, maxB, "%s", nm); return; }
    } break;

  /* ── 9: DRUM KIT — 42 items: 5 params × 8 voices + kit (40) + pitch (41) ─ */
  case 9: {
    if (l2 == 40) { /* Drum Kit selector */
      const uint8_t k = drumKit.load(std::memory_order_relaxed);
      snprintf(out, maxB, "%s", (k < (uint8_t)DrumKitId::COUNT) ? DRUM_KIT_NAMES[k] : "?");
      return;
    }
    if (l2 == 41) { snprintf(out, maxB, "x%.2f", drumPitchMult.load()); return; }
    const int ch    = l2 / 5;
    const int param = l2 % 5;
    switch (param) {
    case 0: snprintf(out, maxB, "%.3f",  drumTune    [ch].load()); return;
    case 1: snprintf(out, maxB, "%.3f",  drumDecay   [ch].load()); return;
    case 2: snprintf(out, maxB, "%d%%",  (int)(drumVolume  [ch].load()*100.f)); return;
    case 3: snprintf(out, maxB, "%d%%",  (int)(drumNoiseMix[ch].load()*100.f)); return;
    case 4:
      /* CLAP/HAT: no wavetable */
      if (ch == 2 || ch == 3 || ch == 4) { snprintf(out, maxB, "N/A"); return; }
      snprintf(out, maxB, "%s",
               safeWaveName(drumWaveIdx[ch].load(std::memory_order_relaxed)));
      return;
    }
    break;
  }

  /* ── 10: LASER SHOW ───────────────────────────────────────────────────── */
  case 10:
    switch (l2) {
    case 0: snprintf(out, maxB, "%s",    laserShowMode.load()  ? "ACTIVE":"MUTED"); return;
    case 1: snprintf(out, maxB, "%s",    midiHueControl.load() ? "ACTIVE":"OFF");   return;
    case 2: snprintf(out, maxB, "%ddeg", (int)(laserBaseHue.load()*360.f));         return;
    case 3: snprintf(out, maxB, "%s",    kLaserAnimNames[(uint8_t)laserShowAnim.load() & 3]); return;
    case 4: snprintf(out, maxB, "%d%%",  (int)(laserDrumFlash.load()*100.f));        return;
    case 5: snprintf(out, maxB, "%.3fs", hueAttack.load());  return;
    case 6: snprintf(out, maxB, "%.3fs", hueDecay.load());   return;
    case 7: snprintf(out, maxB, "%d%%",  (int)(hueSustain.load()*100.f));           return;
    case 8: snprintf(out, maxB, "%.3fs", hueRelease.load()); return;
    } break;

  /* ── 11: TELEMETRY — L2 opens scope; values shown live on scope pages ─── */
  case 11:
    snprintf(out, maxB, "Press ENC");
    return;

  default: break;
  }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * getSliderPct — normalise any parameter to [0,1] for DRAW_SLIDER_POT.
 * Returns 0.5 for toggle/special params.
 * ═══════════════════════════════════════════════════════════════════════════ */
static float getSliderPct(int l1, int l2) {
  if (l1 < 0 || l1 >= kL1Count || l2 < 0 || l2 >= l2CountFor(l1)) return 0.5f;
  const int si = harpScaleIndex.load(std::memory_order_relaxed) & (NUM_SCALES - 1);

  switch (l1) {
  case 0:  /* HARP SETUP */
    switch (l2) {
    case 0: return (float)beamGateHoldMs / (float)BEAM_GATE_HOLD_MAX;
    case 1: return scaleWhiteLevel[si]    / 255.f;
    /* [BAR-FIX] Touch On/Off span CONFIRM_MIN..CONFIRM_MAX (1..15); normalise over
     * the full range (was /7, which overshot 100% for values > 8). */
    case 2: return (float)(std::max(0,(int)scaleTouchConfirm[si]   - CONFIRM_MIN)) / (float)(CONFIRM_MAX - CONFIRM_MIN);
    case 3: return (float)(std::max(0,(int)scaleReleaseConfirm[si] - CONFIRM_MIN)) / (float)(CONFIRM_MAX - CONFIRM_MIN);
    case 4: return scaleR[si] / 255.f;
    case 5: return scaleG[si] / 255.f;
    case 6: return scaleB[si] / 255.f;
    case 7: return scaleMargin[si] / 2000.f;
    case 8: return (float)beamStuckReleaseMs / (float)BEAM_STUCK_RELEASE_MAX;
    /* [FOG] case 10 (enable) is a toggle widget; case 11 margin bar. */
    case 10: return fogRejectEnabled.load(std::memory_order_relaxed) ? 1.f : 0.f;
    case 11: return (float)fogRejectMargin.load(std::memory_order_relaxed) / (float)FOG_MARGIN_MAX;
    case 12: return laserScreensaver.load(std::memory_order_relaxed) ? 1.f : 0.f;
    } break;

  case 1:  /* D-BEAM */
    switch (l2) {
    case 0: return (float)dbeamHWCfg.offsetAdc / 4095.f;
    case 1: return ((float)dbeamHWCfg.rangeAdc - 20.f) / 4075.f;
    case 2: return (float)((int)currentDbeamCurve.load() % 5) / 4.f;
    /* case 3: toggle — falls through to default 0.5 */
    case 4: return (dbeamExprAttack.load(std::memory_order_relaxed) - DBEAM_EXPR_ATTACK_MIN)
                   / (DBEAM_EXPR_ATTACK_MAX - DBEAM_EXPR_ATTACK_MIN);
    case 5: return (dbeamExprRelease.load(std::memory_order_relaxed) - DBEAM_EXPR_RELEASE_MIN)
                   / (DBEAM_EXPR_RELEASE_MAX - DBEAM_EXPR_RELEASE_MIN);
    case 6: return (float)((int)currentDbeamRoute.load() & 3) / 3.f;
    case 7: return (float)((int)currentDbeamTarget.load() & 1);
    } break;

  case 2:  /* MASTER */
    switch (l2) {
    case 0:  return masterVol.load();
    case 1:  return mixHarpVol.load();
    case 2:  return mixSeqVol.load();
    case 3:  return mixDrumsVol.load();
    case 4:  { const float semis = 12.f * log2f(std::max(MASTER_PITCH_MIN,
                          std::min(MASTER_PITCH_MAX, masterPitch.load())));
               return (semis + 24.f) / 48.f; }
    case 5:  return (float)(masterFxIndex.load() & 15) / 15.f;
    case 6:  return drumRevSend.load();
    case 7:  return drumDlySend.load();
    case 8:  return tbDrive.load();
    case 9:  return tbTone.load();
    case 10: return tbMix.load();
    case 11: return djFreq.load();
    case 12: return djRes.load();
    case 13: return djMix.load();
    case 14: return (masterEqLow.load()  + 12.f) / 24.f;
    case 15: return (masterEqHigh.load() + 12.f) / 24.f;
    case 16: return (float)(drumFxIndexA.load() & 15) / 15.f;
    case 17: return (float)(drumFxIndexB.load() & 15) / 15.f;
    case 21: return (mixHarpPan .load() + 1.f) * 0.5f;
    case 22: return (mixSeqPan  .load() + 1.f) * 0.5f;
    case 23: return (mixDrumsPan.load() + 1.f) * 0.5f;
    /* 18/19/20 = toggles, fall to 0.5 */
    } break;

  case 4:  /* MIDI I/O — pitch bend + wire channels */
    switch (l2) {
    case 0: return pbMapping.upSemi.load(std::memory_order_relaxed) / 24.f;
    case 1: return pbMapping.enabled.load(std::memory_order_relaxed) ? 1.f : 0.f;
    case 2: return ((float)wireHarpMidiChannel.load(std::memory_order_relaxed) - 1.f) / 15.f;
    case 3: return ((float)wireSeqMidiChannel.load(std::memory_order_relaxed)  - 1.f) / 15.f;
    case 4: return ((float)wireDrumMidiChannel.load(std::memory_order_relaxed) - 1.f) / 15.f;
    } break;

  case 5:  /* SEQ SETUP — discrete items normalised so the bar tracks the encoder */
    switch (l2) {
    case 0: return (float)(std::min(3, (int)seqActiveBank.load() & 15)) / 3.f;     /* Bank A-D */
    case 1: return seqUI_page.load(std::memory_order_relaxed) ? 1.f : 0.f;          /* View S/D */
    case 2: return ((float)seqTranspose.load() + 12.f) / 24.f;                      /* ±12 semi */
    case 3: return ((float)seqLength.load() - 1.f) / 63.f;                          /* 1..64    */
    case 4: return (NUM_SYNTH_PATS > 1) ? (float)g_lastSynthPreset.load() / (float)(NUM_SYNTH_PATS - 1) : 0.f;
    case 5: return (NUM_DRUM_PATS  > 1) ? (float)g_lastDrumPreset .load() / (float)(NUM_DRUM_PATS  - 1) : 0.f;
    case 6: return 1.f;                                                             /* Clear (action) */
    case 7: case 8: return (NUM_USER_PAT_SLOTS > 1)
                ? (float)userPatCursor.load(std::memory_order_relaxed)
                  / (float)(NUM_USER_PAT_SLOTS - 1) : 0.f;
    case 9: return seqArpEnabled.load() ? 1.f : 0.f;
    case 10: return (float)std::min(7, (int)seqArpPattern.load()) / 7.f;
    case 11: return (float)std::min(7, (int)seqArpRate.load()) / 7.f;
    case 12: return (float)std::min(7, (int)seqArpGate.load()) / 7.f;
    } break;

  case 3:  /* HARP SYNTH */
    switch (l2) {
    case 0:  return (float)std::max(0,(int)harpWaveform.load()) / 24.f;
    case 1:  return harpAttack.load();
    case 2:  return harpDecay.load();
    case 3:  return harpSustain.load();
    case 4:  return harpRelease.load();
    case 5:  return harpCutoff.load();
    case 6:  return harpResonance.load();
    case 7:  return harpNoise.load();
    case 8:  return (harpDetune.load() + 1.f) * 0.5f;
    case 9:  return harpLfoRate.load();
    case 10: return harpLfoDepth.load();
    case 11: return (float)harpLfoRoute.load() / 7.f;
    case 12: return (float)std::max(0,(int)harpOsc2Wave.load()) / 24.f;
    case 13: return harpEnvCutAmount.load();
    case 14: return (float)(harpFxIndex.load()  & 15) / 15.f;
    case 15: return (float)(harpFxIndexB.load() & 15) / 15.f;
    case 16: { portENTER_CRITICAL(&patchMux); float v = fx.harpInsert.dly_send;
               portEXIT_CRITICAL(&patchMux); return v; }
    case 17: { portENTER_CRITICAL(&patchMux); float v = fx.harpInsert.rev_send;
               portEXIT_CRITICAL(&patchMux); return v; }
    case 18: return (float)(std::min(harpPatchIndex.load() & (NUM_PATCHES - 1),
                            NUM_NAMED_PRESETS - 1)) / (float)(NUM_NAMED_PRESETS - 1);
    case 19: case 20: return (float)userSlotCursor[0].load(std::memory_order_relaxed)
                             / (float)(NUM_USER_SLOTS - 1);
    case 21: return harpArpEnabled.load() ? 1.f : 0.f;
    case 22: return (float)std::min(3, (int)harpArpPattern.load()) / 3.f;
    case 23: return (float)std::min(3, (int)harpArpRate.load()) / 3.f;
    case 24: return (float)std::min(3, (int)harpArpGate.load()) / 3.f;
    } break;

  case 7:  /* AUX FX */
    switch (l2) {
    case 0: return masterAuxDlyTime.load() / 1.5f;
    case 1: return masterAuxDlyFb.load()   / 0.95f;
    case 2: return masterAuxRevSize.load() / 0.95f;
    case 3: return masterAuxRevDamp.load();
    case 4: { portENTER_CRITICAL(&patchMux); float v = fx.harpInsert.dly_send;
              portEXIT_CRITICAL(&patchMux); return v; }
    case 5: { portENTER_CRITICAL(&patchMux); float v = fx.harpInsert.rev_send;
              portEXIT_CRITICAL(&patchMux); return v; }
    case 6: { portENTER_CRITICAL(&patchMux); float v = fx.seqInsert.dly_send;
              portEXIT_CRITICAL(&patchMux); return v; }
    case 7: { portENTER_CRITICAL(&patchMux); float v = fx.seqInsert.rev_send;
              portEXIT_CRITICAL(&patchMux); return v; }
    case 8:  return (float)(harpFxIndex.load()  & 15) / 15.f;
    case 9:  return (float)(harpFxIndexB.load() & 15) / 15.f;
    case 10: return (float)(seqFxIndex.load()   & 15) / 15.f;
    case 11: return (float)(seqFxIndexB.load()  & 15) / 15.f;
    case 12: return (float)(drumFxIndexA.load() & 15) / 15.f;
    case 13: return (float)(drumFxIndexB.load() & 15) / 15.f;
    } break;

  case 8:  /* SEQ SYNTH — same as HARP SYNTH */
    switch (l2) {
    case 0:  return (float)std::max(0,(int)seqWaveform.load()) / 24.f;
    case 1:  return seqAttack.load();
    case 2:  return seqDecay.load();
    case 3:  return seqSustain.load();
    case 4:  return seqRelease.load();
    case 5:  return seqCutoff.load();
    case 6:  return seqResonance.load();
    case 7:  return seqNoise.load();
    case 8:  return (seqDetune.load() + 1.f) * 0.5f;
    case 9:  return seqLfoRate.load();
    case 10: return seqLfoDepth.load();
    case 11: return (float)seqLfoRoute.load() / 7.f;
    case 12: return (float)std::max(0,(int)seqOsc2Wave.load()) / 24.f;
    case 13: return seqEnvCutAmount.load();
    case 14: return (float)(seqFxIndex.load()  & 15) / 15.f;
    case 15: return (float)(seqFxIndexB.load() & 15) / 15.f;
    case 16: { portENTER_CRITICAL(&patchMux); float v = fx.seqInsert.dly_send;
               portEXIT_CRITICAL(&patchMux); return v; }
    case 17: { portENTER_CRITICAL(&patchMux); float v = fx.seqInsert.rev_send;
               portEXIT_CRITICAL(&patchMux); return v; }
    case 18: return (float)(std::min(seqPatchIndex.load() & (NUM_PATCHES - 1),
                            NUM_NAMED_PRESETS - 1)) / (float)(NUM_NAMED_PRESETS - 1);
    case 19: case 20: return (float)userSlotCursor[1].load(std::memory_order_relaxed)
                             / (float)(NUM_USER_SLOTS - 1);
    } break;

  case 9:  /* DRUM KIT — 40 voice params + Kit (40) + Pitch (41) */
    { if (l2 == 40)
        return (float)drumKit.load(std::memory_order_relaxed) /
               (float)((int)DrumKitId::COUNT - 1);
      if (l2 == 41) {
        const float semis = 12.f * log2f(std::max(MASTER_PITCH_MIN,
                            std::min(MASTER_PITCH_MAX, drumPitchMult.load())));
        return (semis + 24.f) / 48.f;
      }
      const int ch    = l2 / 5;
      const int param = l2 % 5;
      if (ch < 8 && l2 < 40) {
        switch (param) {
        case 0: return drumTune    [ch].load();
        case 1: return drumDecay   [ch].load();
        case 2: return drumVolume  [ch].load();
        case 3: return drumNoiseMix[ch].load();
        case 4: return (ch == 2 || ch == 3 || ch == 4) ? 0.5f :
                       (float)(drumWaveIdx[ch].load() % kWavesCount) / (kWavesCount - 1.f);
        }
      }
    } break;

  case 10: /* LASER SHOW */
    switch (l2) {
    case 2: return laserBaseHue.load();
    case 3: return (float)((uint8_t)laserShowAnim.load() & 3) / 3.f;
    case 4: return laserDrumFlash.load();
    case 5: return hueAttack.load()  / HUE_ATK_MAX_S;
    case 6: return hueDecay.load()   / HUE_DEC_MAX_S;
    case 7: return hueSustain.load();
    case 8: return hueRelease.load() / HUE_REL_MAX_S;
    } break;

  default: break;
  }
  return 0.5f;   /* safe fallback for toggles and special params */
}

/* ═══════════════════════════════════════════════════════════════════════════
 * drawMenuL3 — parameter edit screen
 *
 * isToggle — true when L3 bar should not track a continuous value.
 *      Value string rendered via drawScrollingText.
 * ═══════════════════════════════════════════════════════════════════════════ */
/* [DB-CURVE] D-BEAM response-curve transfer function (input 0..1 → output 0..1).
 * Mirrors the math in applyDBEAMCurve() (dbeam.cpp) so the on-screen preview is
 * exactly the shape the DSP applies. */
static inline float dbeamCurveEval(int curve, float x) {
  switch (curve) {
    case 1: return 1.0f - x;                    /* INVERTED    */
    case 2: return x * x;                       /* EXPONENTIAL */
    case 3: return sqrtf(x);                    /* LOGARITHMIC */
    case 4: return x * x * (3.0f - 2.0f * x);   /* SIGMOID     */
    default: return x;                          /* LINEAR      */
  }
}

/* [DB-CURVE] 16-bar transfer-function preview for the D-BEAM Curve item, so the
 * user can SEE how each curve reshapes hand-height → expression before picking
 * one (replaces the meaningless enum slider on this item). */
static void drawDbeamCurvePreview(int curve) {
  /* ── Top: FIXED math transfer function (the curve's shape) ─────────────── */
  const int gx = 6, gy = 20, gw = SCREEN_W - 12, gh = 21;
  display.drawRect(gx, gy, gw, gh, SH110X_WHITE);
  const int innerX = gx + 1, innerY = gy + 1, innerW = gw - 2, innerH = gh - 2;

  const int N  = 16;
  const int bw = innerW / N;                    /* ~7 px per bar */
  for (int i = 0; i < N; ++i) {
    const float x = (i + 0.5f) / (float)N;
    float y = dbeamCurveEval(curve, x);
    if (y < 0.f) y = 0.f;
    if (y > 1.f) y = 1.f;
    int h = (int)(y * (float)innerH + 0.5f);
    if (h == 0 && y > 0.f) h = 1;               /* keep a visible stub */
    if (h > innerH) h = innerH;
    const int bx = innerX + i * bw;
    if (h > 0) display.fillRect(bx, innerY + innerH - h, bw - 1, h, SH110X_WHITE);
  }

  /* ── Bottom: LIVE post-curve output (dbeamAmplitude = curve already applied),
   * so you wave a hand and watch the actual beam response after the curve. The
   * D-BEAM amplitude-change watcher in audio.cpp keeps this animating (~10 Hz). */
  const uint16_t amp = (uint16_t)std::min<uint32_t>(16383u,
                       dbeamAmplitude.load(std::memory_order_relaxed));
  const int fw = (int)(((uint32_t)amp * (uint32_t)(SCREEN_W - 2)) / 16383u);
  display.drawRect(0, 44, SCREEN_W, 6, SH110X_WHITE);   /* live track frame */
  if (fw > 0) display.fillRect(1, 45, fw, 4, SH110X_WHITE);

  display.setCursor(LABEL_X, 55);
  /* [DBEAM-VOL] Curve is bypassed (forced Linear) while the VOLUME pedal owns
   * the expression — make that explicit so the preview isn't misleading. */
  if (currentDbeamRoute.load(std::memory_order_relaxed) == DbeamRoute::VOLUME)
    display.printf("%s (VOL=Lin) Bk", safeDBCurveName(curve));
  else
    display.printf("%s LIVE ENC=Bk", safeDBCurveName(curve));
}

static void drawMenuL3() {
  const uint32_t now = millis();
  const int l1 = std::max(0, std::min(kL1Count - 1,
                 (int)currentMenuL1.load(std::memory_order_relaxed)));
  const int l2 = std::max(0, std::min(l2CountFor(l1) - 1,
                 (int)currentMenuL2.load(std::memory_order_relaxed)));

  display.setTextSize(1);
  display.setTextColor(SH110X_WHITE);

  /* Header: category name + "EDIT" */
  display.setCursor(0, HEADER_Y);
  display.print(safeL1Name(l1));
  display.setCursor(SCREEN_W - (4 * CHAR_W), HEADER_Y);
  display.print(F("EDIT"));
  display.drawFastHLine(0, DIVIDER_Y, SCREEN_W, SH110X_WHITE);

  /* Centred parameter name — scroll if too long */
  const char* key = safeL2Name(l1, l2);
  const int   klen = (int)strlen(key);
  const int   ktw  = klen * CHAR_W;
  if (ktw <= SCREEN_W) {
    display.setCursor((SCREEN_W - ktw) / 2, 13);
    display.print(key);
  } else {
    drawScrollingText(0, 13, SCREEN_W, key, now, 0);
  }

  /* [DB-CURVE] D-BEAM Curve (l1=1, l2=2): show the transfer-function preview
   * instead of the generic value + enum slider. */
  if (l1 == 1 && l2 == 2) {
    drawDbeamCurvePreview((int)currentDbeamCurve.load(std::memory_order_relaxed) % 5);
    return;
  }

  /* Value string — scrolling if needed, e.g. long waveform names */
  char valStr[24] = {};
  formatParamValueString(l1, l2, valStr, sizeof(valStr));
  drawScrollingText(LABEL_X, 26, SCREEN_W - LABEL_X * 2, valStr, now, 1);

  /* Widget — toggle switch for boolean params, slider for everything else */
  /* Mute toggles at l1==2, l2 18-20 */
  const bool isToggle = (l1 == 1  && l2 == 3)                   /* D-BEAM enable */
                     || (l1 == 0  && (l2 == 10 || l2 == 12))    /* [FOG] Fog Reject + Screensaver */
                     || (l1 == 2  && l2 >= 18 && l2 <= 20)      /* Mutes         */
                     || (l1 == 4  && l2 == 1)                    /* PB Enable (reindexed) */
                     || (l1 == 10 && (l2 == 0 || l2 == 1));     /* Laser show/midihue toggles */
  if (isToggle) {
    bool st = false;
    if (l1 == 0 && l2 == 10)       st = fogRejectEnabled.load(std::memory_order_relaxed);
    if (l1 == 0 && l2 == 12)       st = laserScreensaver.load(std::memory_order_relaxed);
    if (l1 == 1)                    st = dbeamEnabled .load();
    if (l1 == 2 && l2 == 18)       st = mixHarpMute  .load();
    if (l1 == 2 && l2 == 19)       st = mixSeqMute   .load();
    if (l1 == 2 && l2 == 20)       st = mixDrumsMute .load();
    if (l1 == 4)                    st = pbMapping.enabled.load(std::memory_order_relaxed);
    if (l1 == 10 && l2 == 0)       st = laserShowMode .load();
    if (l1 == 10 && l2 == 1)       st = midiHueControl.load();
    DRAW_SWITCH_TOGGLE(LABEL_X, 41, "State", st);
  } else {
    const float pct = getSliderPct(l1, l2);
    DRAW_SLIDER_POT(LABEL_X, 41, SCREEN_W - (LABEL_X * 2), pct, 0.f, 1.f);
  }

  /* [offrange] Live D-BEAM amplitude preview while editing Offset/Range, so the
   * effect of the mapping is visible in real time.  Drawn as a thin bar on the
   * row above the instruction line (the slider above shows the setting value). */
  if (l1 == 1 && (l2 == 0 || l2 == 1 || l2 == 4 || l2 == 5)) {
    const uint16_t amp = (uint16_t)std::min<uint32_t>(16383u,
                         dbeamAmplitude.load(std::memory_order_relaxed));
    const int fw = (int)(((uint32_t)amp * (SCREEN_W - 2)) / 16383u);
    display.drawFastHLine(0, 51, SCREEN_W, SH110X_WHITE);
    if (fw > 0) display.fillRect(1, 52, fw, 3, SH110X_WHITE);
    display.setCursor(LABEL_X, 56);
    display.print(F("LIVE  ENC=Back"));
    return;
  }

  /* Instruction line — keep ≤ 19 chars = 114 px to stay inside 128 px */
  display.setCursor(LABEL_X, 55);
  if ((l1 == 3 || l1 == 8) && l2 == 19)      display.print(F("Turn=Pick ENC=Save"));
  else if ((l1 == 3 || l1 == 8) && l2 == 20) display.print(F("Turn=Load  ENC=Back"));
  else                                       display.print(F("Turn=Edit  ENC=Back"));
}

/* ═══════════════════════════════════════════════════════════════════════════
 * PUBLIC ENTRY POINT
 * ═══════════════════════════════════════════════════════════════════════════ */

/* drawTransportGlyph — tiny live transport icon (6×7 px).
 *   recording → ● filled disc   playing → ▶ triangle   stopped → ■ square
 * `color` for header fill (BLACK) or dark dashboard (WHITE). Recording wins over play. */
static void drawTransportGlyph(int16_t x, int16_t y, bool playing,
                               bool recording = false,
                               uint16_t color = SH110X_BLACK) {
  if (recording) {
    display.fillCircle(x + 3, y + 3, 3, color);         /* record disc            */
  } else if (playing) {
    for (int i = 0; i < 4; ++i)                         /* right-pointing triangle */
      display.drawFastVLine(x + i, y + i, 7 - 2 * i, color);
  } else {
    display.fillRect(x, y, 6, 6, color);                /* stop square            */
  }
}

/* ─────────────────────────────────────────────────────────────────────────────
 * drawAppConnectedPage — static splash while OctopusApp is connected.
 *
 * The App is the full control surface; the OLED only mirrors live transport
 * state (no parameter echo churn, no data-entry temptation).  Transport is
 * hardware-owned even while connected: SCALE=play/stop, OC short=record,
 * ENC turn=BPM.  ENC long is ignored.  Layout (128×64):
 *   y0-9   header  "APP CONNECTED"     + transport glyph (●/▶/■, right)
 *   y14    BPM 120   BANK A
 *   y25    Voices/Song/Pattern context  ·  Len
 *   y36    BEAM  <D-BEAM amplitude bar>      (separate row — never overdrawn)
 *   y56    full-width page-aware step playhead
 * ─────────────────────────────────────────────────────────────────────────────*/
void drawAppConnectedPage() {
  display.setTextSize(1);
  display.setTextColor(SH110X_WHITE);

  /* Header bar — inverted, with a live transport glyph at the right edge */
  display.fillRect(0, 0, SCREEN_W, HEADER_H, SH110X_WHITE);
  display.setTextColor(SH110X_BLACK);
  display.setCursor(LABEL_X, 1);
  display.print(F("APP CONNECTED"));
  drawTransportGlyph(SCREEN_W - 10, 1,
                     seqPlaying.load(std::memory_order_relaxed),
                     seqRecording.load(std::memory_order_relaxed));
  display.setTextColor(SH110X_WHITE);

  /* Line 1 — clock + bank */
  display.setCursor(0, 14);
  display.printf("BPM %-3d  BANK %c",
    (int)seqBpm.load(std::memory_order_relaxed),
    (char)('A' + std::min(3, (int)seqActiveBank.load() & 15)));

  /* Line 2 — context: live voices / song position / idle pattern + length */
  display.setCursor(0, 25);
  const int nVoices = seq_active_voice_count();
  const int slen    = (int)seqLength.load(std::memory_order_relaxed);
  if (songModeActive.load(std::memory_order_relaxed)) {
    display.printf("SONG %d   step %d",
      (int)activeSongSlot.load() + 1,
      (int)songCurrentStep.load() + 1);
  } else if (nVoices > 0) {
    display.printf("Voices %d   Len %d", nVoices, slen);
  } else {
    display.printf("Pat %02d     Len %d",
      (int)g_lastSynthPreset.load(),
      slen);
  }

  /* Line 3 — D-BEAM amplitude on its OWN row (no longer overdraws the step bar) */
  display.setCursor(0, 37);
  display.print(F("BEAM"));
  if (dbeamEnabled.load(std::memory_order_relaxed)) {
    const uint16_t amp = (uint16_t)dbeamAmplitude.load(std::memory_order_relaxed);
    DRAW_BAR_GRAPH(30, 36, SCREEN_W - 30, 7, (float)amp, 16383.f);
  } else {
    display.setCursor(34, 37);
    display.print(F("- bypassed -"));
  }

  /* Bottom — full-width page-aware playhead (matches App P1-P4 paging) */
  DRAW_STEP_BARGRAPH(0, 56, SCREEN_W,
                     (uint16_t)seqCurrentStep.load(std::memory_order_relaxed),
                     (uint16_t)slen);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * drawSongEditor — hardware SONG row editor (L1 = 13)
 * One row per song step: [>] Snn  BANK x  x<rpt>.  SCALE moves the box cursor,
 * the encoder edits the boxed value, OC appends a row, OC+SCALE deletes one.
 * The '>' marker flags the step the song engine is currently playing.
 * ═══════════════════════════════════════════════════════════════════════════ */
static void drawSongEditor() {
  const uint8_t   slot   = activeSongSlot.load(std::memory_order_relaxed) & 15u;
  const SongSlot& song   = hwSongData[slot];
  const int       n      = std::max(1, (int)song.numSteps);
  const int       curRow = std::max(0, std::min(n - 1,
                              songUI_row.load(std::memory_order_relaxed)));
  const int       curBox = songUI_box.load(std::memory_order_relaxed) % SONG_UI_BOXES;
  const bool      active = songModeActive.load(std::memory_order_relaxed);
  const int       play   = (int)songCurrentStep.load(std::memory_order_relaxed);

  /* Header bar */
  display.fillRect(0, 0, 128, 8, SH110X_WHITE);
  display.setTextColor(SH110X_BLACK);
  display.setCursor(2, 0);
  display.printf("SONG %X  ST%d  %s", slot, n, active ? "ON" : "off");
  display.setTextColor(SH110X_WHITE);

  /* Visible window of up to 6 rows, scrolled to keep the cursor on screen */
  const int firstRow = (curRow < 6) ? 0 : (curRow - 5);
  for (int i = 0; i < 6; ++i) {
    const int r = firstRow + i;
    if (r >= n) break;
    const int y = 10 + i * 9;
    const SongStep& st = song.steps[r];

    if (active && r == play) { display.setCursor(0, y); display.print(">"); }
    display.setCursor(6, y);
    display.printf("S%02d", r + 1);

    /* BANK box at x=40 */
    const int bx = 40;
    if (r == curRow && curBox == 0) {
      display.fillRect(bx - 1, y - 1, 41, 9, SH110X_WHITE);
      display.setTextColor(SH110X_BLACK);
    }
    display.setCursor(bx, y);
    display.printf("BANK %c", 'A' + (st.bank & 15u));
    display.setTextColor(SH110X_WHITE);

    /* REPEATS box at x=92 */
    const int rx = 92;
    if (r == curRow && curBox == 1) {
      display.fillRect(rx - 1, y - 1, 23, 9, SH110X_WHITE);
      display.setTextColor(SH110X_BLACK);
    }
    display.setCursor(rx, y);
    display.printf("x%02d", st.repeats);
    display.setTextColor(SH110X_WHITE);
  }
}

/* YES/NO confirm modal — encoder selects, ENC commits (interface.cpp). */
static void drawConfirmDialog() {
  const uint8_t       sel    = confirmSel.load(std::memory_order_relaxed);
  const ConfirmAction a      = confirmActionId.load(std::memory_order_relaxed);
  const uint8_t       rawArg = confirmArg.load(std::memory_order_relaxed);
  const uint8_t       arg    = rawArg & 3u;
  /* Order matches ResetScope: FULL / BANKS_PATTERNS / MOTION / SETTINGS. */
  static const char* const kScope[4] = { "Full", "Banks+Pats", "Motion", "Settings" };
  const char*         title = "CONFIRM";
  const char*         line1 = "";
  const char*         line2 = "";
  const char*         line3 = "";
  char                slotName[16] = {};
  switch (a) {
    case ConfirmAction::SEQ_CLEAR:
      title = "CLEAR";
      line1 = "Clear pattern +";
      line2 = "reset sounds?";
      break;
    case ConfirmAction::SAVE:
      title = "SAVE";
      line1 = kScope[arg];
      line2 = "write to NVS?";
      line3 = "(reboots)";
      break;
    case ConfirmAction::RESET:
      title = "RESET";
      line1 = kScope[arg];
      line2 = "wipe + reboot?";
      line3 = "(cannot undo)";
      break;
    case ConfirmAction::LOAD:
      title = "LOAD";
      line1 = kScope[arg];
      line2 = "reload from NVS?";
      break;
    case ConfirmAction::USR_SOUND_SAVE:
      title = "SAVE SOUND";
      userSlotName((uint8_t)((rawArg >> 6) & 1u), (uint8_t)(rawArg & 63u),
                   slotName, sizeof(slotName));
      line1 = slotName;
      line2 = "overwrite slot?";
      break;
    case ConfirmAction::USR_PAT_SAVE:
      title = "SAVE PATTERN";
      userPatName((uint8_t)(rawArg & 63u), slotName, sizeof(slotName));
      line1 = slotName;
      line2 = "overwrite slot?";
      break;
    default: break;
  }

  const int bx = 6, by = 6, bw = SCREEN_W - 12, bh = SCREEN_H - 12;
  display.fillRect(bx, by, bw, bh, SH110X_BLACK);
  display.drawRect(bx, by, bw, bh, SH110X_WHITE);

  display.setTextSize(1);
  display.setTextColor(SH110X_WHITE);
  display.setCursor(bx + (bw - (int)strlen(title) * 6) / 2, by + 3);
  display.print(title);
  display.drawFastHLine(bx + 2, by + 12, bw - 4, SH110X_WHITE);

  display.setCursor(bx + 5, by + 15);
  display.print(line1);
  display.setCursor(bx + 5, by + 23);
  display.print(line2);
  display.setCursor(bx + 5, by + 31);
  display.print(line3);

  const int btnY = by + bh - 13, btnW = 34, btnH = 11;
  const int noX  = bx + 9;
  const int yesX = bx + bw - 9 - btnW;

  if (sel == 0) { display.fillRect(noX, btnY, btnW, btnH, SH110X_WHITE); display.setTextColor(SH110X_BLACK); }
  else          { display.drawRect(noX, btnY, btnW, btnH, SH110X_WHITE); display.setTextColor(SH110X_WHITE); }
  display.setCursor(noX + (btnW - 2 * 6) / 2, btnY + 2);
  display.print("NO");

  if (sel != 0) { display.fillRect(yesX, btnY, btnW, btnH, SH110X_WHITE); display.setTextColor(SH110X_BLACK); }
  else          { display.drawRect(yesX, btnY, btnW, btnH, SH110X_WHITE); display.setTextColor(SH110X_WHITE); }
  display.setCursor(yesX + (btnW - 3 * 6) / 2, btnY + 2);
  display.print("YES");

  display.setTextColor(SH110X_WHITE);
}

void renderUIState() {
  if (!hasOLED) return;
  display.setTextSize(1);
  display.setTextColor(SH110X_WHITE);

  /* Confirm modal owns the screen while open. */
  if (confirmOpen.load(std::memory_order_relaxed)) {
    drawConfirmDialog();
    return;
  }

  const int       l1  = (int)currentMenuL1.load(std::memory_order_relaxed);
  const MenuState mst = menuState.load(std::memory_order_relaxed);

  /* SEQ MATRIX: grid editor — delegate to groovebox.cpp (seqUI_renderMatrix) */
  if (l1 == 6 && mst != MenuState::MENU_L1) {
    seqUI_renderMatrix();
    return;
  }
  /* SONG: row editor (l1 = 13) — special page like the matrix */
  if (l1 == 13 && mst != MenuState::MENU_L1) {
    drawSongEditor();
    return;
  }
  /* Edge-comp full-screen editor. */
  if (edgeEditOpen.load(std::memory_order_relaxed)) {
    drawEdgeCompEditor();
    return;
  }
  /* AUX FX (l1=7) uses standard drawMenuL3. */

  /* Telemetry oscilloscope */
  if (currentScopeView.load(std::memory_order_relaxed) != TelemetryView::OFF) {
    drawTelemetryOscilloscope();
    return;
  }

  /* App connected: static page only — no menu tree */
  if (isAppConnected()) {
    drawAppConnectedPage();
    return;
  }

  switch (mst) {
  case MenuState::IDLE:
    (activeDashboard.load() == DashboardMode::SEQUENCER)
      ? drawSeqDashboard() : drawHarpDashboard();
    break;
  case MenuState::MENU_L1: drawMenuL1(); break;
  case MenuState::MENU_L2: drawMenuL2(); break;
  case MenuState::MENU_L3: drawMenuL3(); break;
  }
}

/* viewHasStepBar — is the bottom (0,56) step playhead actually on screen right
 * now?  Mirrors renderUIState()'s dispatch.  Used to gate the cheap partial
 * redraw so we never repaint a band that the current view doesn't own. */
static bool viewHasStepBar() {
  if (confirmOpen.load(std::memory_order_relaxed)) return false; /* modal owns screen */
  if (currentScopeView.load(std::memory_order_relaxed) != TelemetryView::OFF)
    return false;                                   /* telemetry scope owns screen */
  if (isAppConnected()) return true;                /* APP splash shows the bar    */
  const int       l1  = (int)currentMenuL1.load(std::memory_order_relaxed);
  const MenuState mst = menuState.load(std::memory_order_relaxed);
  if (l1 == 6  && mst != MenuState::MENU_L1) return false;  /* SEQ matrix page    */
  if (l1 == 13 && mst != MenuState::MENU_L1) return false;  /* SONG editor page   */
  if (edgeEditOpen.load(std::memory_order_relaxed)) return false; /* EDGE COMP page */
  if (mst != MenuState::IDLE) return false;                 /* any menu level     */
  return activeDashboard.load() == DashboardMode::SEQUENCER; /* SEQ dashboard only */
}

/* viewIsSeqMatrix — is the SEQ MATRIX grid editor the on-screen view right now?
 * The matrix playhead is a per-row column underline (spans the full height), so
 * it can't ride the cheap bottom-band partial redraw; instead the audio-core
 * display task forces a full matrix re-render on each step change while this is
 * true AND the sequencer is playing.  Page-diff flush still ships only the pages
 * the moving column touches, and it goes quiet the instant play stops or the
 * user leaves the grid — so the cost is bounded to the step rate. */
bool viewIsSeqMatrix() {
  if (!hasOLED) return false;
  if (currentScopeView.load(std::memory_order_relaxed) != TelemetryView::OFF) return false;
  if (isAppConnected()) return false;                 /* app splash owns the screen */
  const int       l1  = (int)currentMenuL1.load(std::memory_order_relaxed);
  const MenuState mst = menuState.load(std::memory_order_relaxed);
  return (l1 == 6 && mst != MenuState::MENU_L1);
}

/* renderStepBarRegionIfVisible — partial playhead refresh (SEQ MATRIX bar band).
 * Returns true if drawn (caller should flush); false if view has no step bar. */
bool renderStepBarRegionIfVisible() {
  if (!hasOLED || !viewHasStepBar()) return false;
  display.setTextSize(1);
  display.setTextColor(SH110X_WHITE);
  display.fillRect(0, 53, SCREEN_W, 11, SH110X_BLACK);   /* page strip + bar band */
  DRAW_STEP_BARGRAPH(0, 56, SCREEN_W,
                     (uint16_t)seqCurrentStep.load(std::memory_order_relaxed),
                     (uint16_t)seqLength.load(std::memory_order_relaxed));
  return true;
}
