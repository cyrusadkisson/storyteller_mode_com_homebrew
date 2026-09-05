/*
 * app.ino — companion v1: WiFi AP + web UI over the verified control surface.
 *
 *   T-2CAN-FD.  CAN-A (MCP2518FD) on the van's CAN1 (control bus, 250k),
 *   CAN-B (TWAI, GPIO6/7) on CAN2 (energy bus, 250k).
 *
 *   Phone joins the AP, opens http://192.168.4.1 (or http://van.local).
 *
 *   CONTROL SURFACE — wire-verified on this van unless noted:
 *     - wall-switch toggles via the input spoof (impersonate the PDM's F0/F8
 *       digital-input frames; the HU toggles and re-broadcasts, so persistence
 *       is free) — cabin, cargo, aux, water pump, recirc are confirmed.
 *       The awning light uses the same mechanism but is UNCONFIRMED: the
 *       awning is physically absent from this van, so only the HU's command
 *       byte can be watched.        — docs/modewifi-analysis.md §2
 *     - A/C off/cool/heat, compressor, fan auto/low/high, and the COOL
 *       setpoint (0x19FEF903, echoed on 0x19FFE258). The heat setpoint is
 *       decoded but has no control here.   — docs/climate-control.md
 *     - vent lid, fan on/off, air direction, fan speed (0x19FEA603, echoed on
 *       0x19FEA758). Fan run state is COMMANDED, never inferred: status byte 2
 *       oscillates to 0 while the fan runs.
 *     - inverter on/off on CAN2 (0x19FFD3F2, single-shot latch). No status
 *       echo exists, so the AC line voltage is the truth check.
 *   READ-ONLY: per-channel power (feedback amps x the 12 V load bus),
 *   tanks, battery DC status, inverter
 *   AC stats, interior temperature, PDM fault frames, Rixen heater state.
 *
 *   NOT included, each for a measured reason (see the docs): dimming and
 *   reading lights (cannot hold a PDM output from a parallel tap), Rixen
 *   writes (accepted, but the HU reverts them within ~5 s), sink drain
 *   (hold-to-run cannot be sustained). All need cut-and-stand-in.
 *
 *   The panel screen is a ONE-WAY DISPLAY: it never shows what we command.
 *   This UI reads truth from the bus, never from the panel.
 */
#include <Arduino.h>
#include <SPI.h>
#include <WiFi.h>
#include <WebServer.h>
#include <SPIFFS.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include "mcp2518fd_can.h"
#include "pin_config.h"
#include "driver/twai.h"

// ----- WiFi AP ---------------------------------------------------------------
// The board is its own access point, with no screen and no reset button, so
// the credentials are compiled in.
//
// They are deliberately IN THE CLEAR, by owner decision (2026-09-04). The
// ap_secret.h override mechanism and its .gitignore entry were removed in
// favour of a memorable password. The trade is real and accepted: anyone
// within radio range of the van who has read this repository can join this AP
// and operate the lights, pump, A/C and vent. Changing these two strings and
// reflashing is what revokes that.
static const char *AP_SSID = "VanCompanion";
static const char *AP_PASS = "storyteller";      // WPA2 requires >= 8 chars
static const char *MDNS_NAME = "van";         // http://van.local

// ----- board temperature history --------------------------------------------
// The ESP32-S3 has no RTC and millis() resets on every boot, so history is
// RELATIVE: TEMP_HOURS of history in 15-minute buckets (4 per hour), plus the
// bucket in progress = TEMP_HOURS*4+1 bars. A 15-min bucket needs >=15 of its
// 30 possible samples to count.
// Live in RAM, but PERSISTED to SPIFFS on every bucket rotation (see the
// history lifeboat below). This comment used to justify RAM-only storage by
// "thousands of flash write cycles"; that does not survive arithmetic at this
// write rate. ~1.6 KB every 15 minutes is ~154 KB/day into a 3.4 MB partition
// -- roughly 16 erase cycles a year against an endurance near 100,000. The
// real constraint was never wear; it is that the board has no clock and so
// cannot tell how long it was unpowered.
//
// This reads the DIE, not the cavity: it runs above ambient by the chip's own
// dissipation plus the WiFi radio. Absolute accuracy is a few degrees; the
// useful signal is the shape over a day.
#define TEMP_HOURS     48           // history span; UI derives its axis from this
#define TEMP_BARS_HOUR 4            // 15-minute buckets
#define TEMP_BUCKETS   (TEMP_HOURS * TEMP_BARS_HOUR + 1)   // + the one in progress
#define TEMP_SAMPLE_MS 30000        // one reading per 30 s
#define TEMP_MIN_VALID 15           // >=7.5 min of samples or the bucket is void
static bool     tempOK = true;   // Arduino's temperatureRead() needs no setup
static int16_t  tempHist[TEMP_BUCKETS];   // tenths of °C; INT16_MIN = no data
static float    tempAcc = 0;              // current hour accumulator
static uint16_t tempCnt = 0;
// Cabin ambient gets the same treatment: same buckets, same validity rule.
// It comes from the A/C node on CAN1, so it also stops when that bus does --
// hence its own sample counter rather than reusing the die sensor's.
static int16_t  ambHist[TEMP_BUCKETS];
static float    ambAcc = 0;
static uint16_t ambCnt = 0;
// Pack power, same buckets. Stored as whole WATTS (signed: negative is a
// draw, positive a surplus) rather than tenths -- it ranges into the
// hundreds and an int16 of tenths would overflow past 3.2 kW.
static int16_t  pwrHist[TEMP_BUCKETS];
static float    pwrAcc = 0;
static uint16_t pwrCnt = 0;
// Pack temperature from the BMS (CAN2 DC_SOURCE_STATUS_2), same buckets and
// the same tenths-of-C storage as the other two temperature rings. This is the
// only temperature in the system that is measured INSIDE the battery, which is
// why it is worth its own history rather than being folded into the die chart.
static int16_t  ptHist[TEMP_BUCKETS];
static float    ptAcc = 0;
static uint16_t ptCnt = 0;
// Rolling ring for the smoothed time-remaining figure. The hourly buckets are
// too coarse: the compressor and heater cycle several times an hour, which is
// exactly the swing being averaged out. The window GROWS: the figure appears
// at 15 minutes and the window widens to its full hour as samples accrue.
// 720 samples at 5 s = 1 h.
#define PWRWIN_N      720
#define PWRWIN_MS     5000
#define PWRWIN_MIN_FILL 180          // 15 min before the figure means anything
static int16_t  pwrwin[PWRWIN_N];
static uint16_t pwHead = 0, pwFill = 0;
static uint32_t pwAt = 0;
// millis() of the first sample. The row is gated on ELAPSED TIME from here,
// not on a sample count: a count can only ever fall behind the clock -- a
// dropped tick or a stale-battery moment is a sample never taken -- so a
// count-gated row is guaranteed to appear after the 15-minute chart bar it is
// supposed to sit beside. Both now key off the same 15 minutes.
static uint32_t pwFirstMs = 0;
static uint32_t tempLastSample = 0;
// Die over 85 C (185 F) sustained for an hour latches a warning that survives
// until the user dismisses it (client-side, keyed on hotAt). hotAt is the
// board-millis moment the hour threshold was crossed; a later excursion that
// again crosses one hour updates it, un-acking the warning.
static uint32_t hotStreakStart = 0;
static uint16_t hotStreakCnt = 0;
static uint32_t hotAt = 0;                 // 0 = never latched
static uint32_t tempHourStart = 0;
static uint16_t tempFilled = 0;           // buckets rotated so far

#define CANB_TX GPIO_NUM_7
#define CANB_RX GPIO_NUM_6

mcp2518fd CanA(MCP2518_CS);
SemaphoreHandle_t canMux;           // serializes ALL CanA SPI access
WebServer server(80);
DNSServer dns;                      // captive portal: any DNS answer -> our IP

// ----- CAN ids --------------------------------------------------------------
static const uint32_t ID_PDM1_STAT  = 0x14EF111EUL;  // PDM1 -> HU (inputs, feedback)
static const uint32_t ID_PDM2_STAT  = 0x14EF111FUL;  // PDM2 -> HU
static const uint32_t ID_PDM1_CMD   = 0x14EF1E11UL;  // HU -> PDM1 (output levels, ~91 Hz)
static const uint32_t ID_PDM2_CMD   = 0x14EF1F11UL;  // HU -> PDM2
static const uint32_t ID_PDM1_FAULT = 0x14E9111EUL;  // PDM short/overcurrent warning
static const uint32_t ID_PDM2_FAULT = 0x14E9111FUL;
static const uint32_t ID_TANKS      = 0x19FFB7AFUL;  // fresh(0)/gray(2) levels
static const uint32_t ID_AC_CMD     = 0x19FEF903UL;  // thermostat command (we TX)
static const uint32_t ID_AC_STAT    = 0x19FFE258UL;  // AC status echo
static const uint32_t ID_AMBIENT    = 0x19FF9C58UL;  // cabin ambient temp
static const uint32_t ID_VENT_CMD   = 0x19FEA603UL;  // vent control (we TX)
static const uint32_t ID_VENT_STAT  = 0x19FEA758UL;  // vent status
static const uint32_t ID_DC1        = 0x19FFFD46UL;  // battery V / I
static const uint32_t ID_DC2        = 0x19FFFC46UL;  // battery temp / SoC / time-left
static const uint32_t ID_DC3        = 0x19FFFB46UL;  // battery SoH / Ah remaining
static const uint32_t ID_INV_CMD    = 0x19FFD3F2UL;  // inverter on/off (we TX, CAN2)
static const uint32_t ID_INV_AC     = 0x19FFD7E1UL;  // inverter/shore AC line status
// Cell monitor (SA 0x8E), six consecutive PGNs 0x18FF918E..0x18FF968E, each a
// further 0x100 apart. The last four carry four bytes each = sixteen values,
// which is the pack's cell count -- but those sixteen bytes have NEVER been
// seen to differ from one another at any state of charge, so whether they are
// per-cell values or one aggregate replicated sixteen times is UNRESOLVED.
// See docs/energy-can2.md. Captured raw here so the question answers itself:
// cell 9 sits 33 counts below its neighbours at 0.01 V per count, far outside
// the truncation that made the earlier observation ambiguous, so the next
// drawdown decides it. The first two frames have a different shape and may
// carry min/max/average -- if a MIN is in there, the weak cell is visible on
// the bus after all, which would matter more than the sixteen.
#define ID_CELL_BASE 0x18FF918EUL
#define ID_CELL_STEP 0x100UL
#define ID_CELL_N    6
#define CELL_STALE_MS 5000
// Rixen hydronic heater: standard 11-bit ids, not J1939.
// READ-ONLY BY DESIGN. Writes are accepted by the heater (verified 2026-08-26:
// a target change took effect in ~300 ms) but the head unit re-asserts every
// sub-command every ~5-6 s, so holding a value would mean contending with it
// indefinitely -- an oscillating setpoint on a DIESEL BURNER. Not worth it for
// a comfort feature; real control needs cut-and-stand-in. Do not add writes.
static const uint32_t ID_RIX_STATUS = 0x724UL;      // temps + flags
static const uint32_t ID_RIX_CMD    = 0x788UL;      // HU's command stream (we listen)

// ----- switch table (verified 2026-08-24, matches ModeWifi byte-for-byte) ---
struct Sw { uint8_t pdm; uint8_t mux; uint8_t byte; uint8_t shift;
            uint16_t hold; bool holdrun; uint8_t pdmDo; const char *name; };
static const Sw SW[] = {
  {0, 0, 6, 0, 150, false, 4,  "cabin"},        // PDM1 DO4
  {0, 0, 6, 2, 150, false, 2,  "cargo"},        // PDM1 DO2
  {1, 0, 7, 6, 150, false, 12, "pump"},         // PDM1 DO12 (switch lives on PDM2)
  {1, 0, 6, 0, 150, false, 12, "aux"},          // PDM2 DO12 (perimeter)
  {0, 0, 7, 4, 150, false, 6,  "recirc"},       // PDM1 DO6 (momentary, 10 s cycle)
  {0, 0, 7, 6, 150, false, 5,  "awning"},       // PDM1 DO5 (awning LIGHT)
};
// State lookup uses the PDM of the OUTPUT channel, not the switch that drives
// it: aux's switch is a PDM2 input but the perimeter lights are PDM2 DO12;
// the pump switch is a PDM2 input while the pump is PDM1 DO12.
#define SDO(n) (SW[n].pdmDo)
static const uint8_t SW_OUT_PDM[6] = {0, 0, 0, 1, 0, 0};

static const uint8_t MUX[2] = {0xF0, 0xF8};

static uint8_t live[2][2][8];      // last F0/F8 per PDM (spoof base)
static bool    liveSeen[2][2];

// ----- mirrored state --------------------------------------------------------
static uint8_t  doLevel[2][13];    // HU-commanded level per channel (index 1..12)
static float    doAmps[2][13];     // feedback current per channel, ×0.125 A
// The switched DC loads run on a nominal 12 V bus, downstream of the van's
// 48->12 V converter. NOTE the PDM's own FB frame reports ~52.7 V -- that is
// the 48 V PACK, not this rail, and using it would overstate every load by 4x.
// Verified against the panel: the water pump reads 5.375 A here and the panel
// shows ~60 W, i.e. ~11-12 V.
#define LOAD_BUS_V 12.0f
// Battery-compartment fans: PDM2 DO2, named GalleyFanSpeed in the firmware
// dictionary. A standing-feed channel we never write to -- the head unit runs
// it autonomously. The COMMANDED level is the usable signal; its feedback
// current sits on the 0.125 A quantisation boundary and dithers, so it cannot
// tell running from stopped.
#define GALLEY_FAN_PDM 1
#define GALLEY_FAN_DO  2
static uint8_t  faultB[2][2];      // PDM fault frame bytes 2-3
static uint8_t  fwL = 0, fwR = 1, grL = 0, grR = 1;
static float    battV = 0, battA = 0, battT = 0;      // CAN2
static uint16_t battSoC2 = 0;      // SoC ×2
static uint16_t battMin = 0xFFFF, battAh = 0;
static uint8_t  battSoH = 0;
static bool     seenBatt = false, seenTank = false;
// Battery data must never be rendered from a frozen reading. The BMS can
// sleep, the CAN2 tap can fail, and the TWAI controller can go bus-off --
// in all three the last values would otherwise sit on screen looking live.
// Correctness cannot depend on someone noticing a frame counter.
static uint32_t battAt = 0;
#define BATT_STALE_MS 5000
// DC_SOURCE_STATUS_2 carries SoC and pack temperature. It gets its own
// timestamp because battAt is set by any of the three battery frames: without
// this, a run of DC1-only traffic would let an unset battT (0 C = 32 F) be
// averaged into the pack-temperature history as though it were measured.
static uint32_t battTAt = 0;
static uint8_t  cellFrm[ID_CELL_N][8];     // raw, exactly as received
static bool     cellSeen[ID_CELL_N];
static uint32_t cellAt = 0;

// Weak-cell watch. CONFIRMED 2026-09-05 that the sixteen bytes are per-cell
// (docs/energy-can2.md), which makes this the DIRECT signal that the voltage/
// SoC disagreement warning was only ever a proxy for: the BMS opens the
// contactor on the weakest cell, never on the pack average, which is why this
// van dies while the gauge still reads 80 %.
//
// Thresholds are in byte counts, not volts, because that is what the wire
// carries: cell V = 2.00 + byte/100, so byte 100 = 3.00 V. Two independent
// conditions, either of which warns:
//   LOW    -- the weakest cell is approaching the BMS undervoltage floor.
//   SPREAD -- one cell is separating from the pack. A healthy pack sits within
//             1-2 counts; cell 9 read 35 counts down with the system dead.
// Separate trip and clear values so a pack sitting on a threshold cannot flap.
#define CELL_LOW_TRIP      100     // 3.00 V
#define CELL_LOW_CLEAR     105     // 3.05 V
#define CELL_SPREAD_TRIP    10     // 0.10 V
#define CELL_SPREAD_CLEAR    7     // 0.07 V
static bool     cellBad = false;
static uint32_t cellBadAt = 0;
static uint8_t  cellMinV = 0, cellMinIdx = 0, cellSpreadV = 0;

// Fills the stats from the four per-cell frames. False unless ALL four are
// present and fresh -- a spread computed from half the pack is not a spread.
static bool cellStats() {
  if (!cellAt || (millis() - cellAt) >= CELL_STALE_MS) return false;
  int lo = 255, hi = 0, li = 0;
  for (int f = 2; f < ID_CELL_N; f++) {
    if (!cellSeen[f]) return false;
    for (int b = 4; b < 8; b++) {
      uint8_t v = cellFrm[f][b];
      if (v < lo) { lo = v; li = (f - 2) * 4 + (b - 4); }
      if (v > hi) hi = v;
    }
  }
  cellMinV = (uint8_t)lo;
  cellMinIdx = (uint8_t)li;
  cellSpreadV = (uint8_t)(hi - lo);
  return true;
}
// Voltage/SoC disagreement. A pack reading low while the gauge still reads
// high means a CELL is near its floor, not that the pack is empty -- the BMS
// protects per cell, so it opens the contactor on that one cell while the pack
// average still looks healthy. That is exactly how this van has failed three
// times (cell 9 at 2.80 V; see docs/energy-can2.md), and pack voltage and SoC
// are both averages, so this disagreement is the only warning available on the
// bus. Separate trip and clear thresholds so a pack sitting on the line does
// not flap the warning on and off.
#define VSOC_V_TRIP   51.0f
#define VSOC_V_CLEAR  51.3f
#define VSOC_SOC_MIN  50.0f
static bool     vsocBad = false;
static uint32_t vsocAt = 0;                // board-millis of the last trip
// ----- wall clock ------------------------------------------------------------
// The board has no RTC, and millis() restarts at zero every power cycle, so it
// can never know the time by itself. The browser polling /api/state does know,
// and sends it as ?now=<unix seconds>. ONE subtraction turns that into the
// moment this board booted:
//
//     bootEpoch = phoneNow - millis()/1000
//
// After which every millis() value the board holds converts to a real date --
// including values recorded BEFORE any phone connected, because they are all
// offsets from that same boot. That is why log entries carry bootId: at dump
// time an undated entry from THIS session can still be resolved, while one
// from an earlier session (whose boot moment nobody ever learned) honestly
// falls back to milliseconds.
//
// Trusting the phone's clock is the trade. It is network-synced and better
// than anything this board could manage, and a wrong phone writes a wrong
// timestamp. The sanity floor below rejects an obviously unset clock.
#define EPOCH_FLOOR 1700000000UL          // 2023-11-14; earlier is not a clock
static uint32_t bootEpoch = 0;            // 0 = never learned this session
static uint16_t bootId = 0;               // distinguishes this run in the log

static void learnEpoch(uint32_t nowSec) {
  if (nowSec < EPOCH_FLOOR) return;
  // Recomputed on every poll rather than latched: it costs nothing and it
  // absorbs the ~2 s/day the crystal drifts.
  bootEpoch = nowSec - (millis() / 1000);
  // A restore left the saved moment pending; now that the time is known, the
  // outage length is computable. Declared below, so forward-declare it here.
  extern void lbApplyGapIfPending();
  lbApplyGapIfPending();
}

static uint32_t nowEpoch() {
  return bootEpoch ? (bootEpoch + millis() / 1000) : 0;
}


// ----- history lifeboat ------------------------------------------------------
// The four chart histories live in RAM and die with any power cycle -- which
// includes every deliberate swap between laptop USB, a power brick and van
// power. This saves them to SPIFFS on demand, and restores them at boot.
//
// SPIFFS, not NVS: the partition table already carries a 3.4 MB spiffs region
// that nothing else uses, while NVS is 20 KB and has overflowed once already.
// This is bulk data, which is what a filesystem is for.
//
// SCOPE, deliberately narrow -- only the four bucket histories:
//   * The rolling power window (pwrwin) is NOT saved. It is a one-hour mean,
//     and a mean spanning an unknown outage is worse than no mean. It refills
//     in 15 minutes.
//   * The overheat latch is NOT saved. It is a dismissable warning, and losing
//     it across a swap you performed yourself is harmless.
//   * The bucket in progress is NOT saved: at most 15 minutes.
//
// THE GAP. The board has no clock at boot, so it cannot know how long it was
// dark until a phone connects. The save file records the wall-clock moment it
// was written; the first poll that supplies the time triggers the shift, which
// pushes in one empty bucket per 15 minutes of outage. Until that poll the
// restored chart is un-shifted -- honest enough, since it is un-shifted by
// exactly the amount nobody yet knows.
// THERE IS DELIBERATELY NO MANUAL SAVE BUTTON. It looks helpful and is not:
// the saved arrays are only complete up to the last bucket rotation, since the
// bucket in progress is never written. So the true no-data period runs from
// that ROTATION to the next boot, and savedEpoch has to mark the rotation --
// which is exactly what saving on rotation records. A button would stamp a
// later time over identical data, shrinking bootEpoch - savedEpoch and
// UNDERSTATING the gap by up to 15 minutes. It would make the chart claim
// continuity it does not have.
#define LB_PATH  "/history.bin"
#define LB_MAGIC 0x564C4231UL              // "VLB1"
#define LB_VER   1

struct __attribute__((packed)) LbHdr {
  uint32_t magic; uint16_t ver; uint16_t buckets;
  uint32_t savedEpoch;                     // 0 if the clock was never learned
  uint16_t filled; uint16_t pad;
  uint32_t sum;                            // over the payload only
};

static uint32_t lbPendingEpoch = 0;        // set by a restore, cleared by the shift
// The file is read at boot but NOT committed to the live rings until the clock
// arrives. Without a clock the board cannot tell a 30-second swap from a
// week-old file, and committing early would display stale history as current
// for however long it takes someone to open the app. Held here instead, and
// applied -- or discarded as too old -- the moment the time is known.
static int16_t  lbHeld[4][TEMP_BUCKETS];
static bool     lbHave = false;
static uint16_t lbHeldFilled = 0;
static bool     lbMounted = false;

static uint32_t lbSum(const int16_t *p, size_t n) {
  uint32_t s = 2166136261UL;               // FNV-1a, plenty for corruption
  const uint8_t *b = (const uint8_t *)p;
  for (size_t i = 0; i < n * sizeof(int16_t); i++) { s ^= b[i]; s *= 16777619UL; }
  return s;
}

// The four rings, in a fixed order both save and restore agree on.
static int16_t *lbRing(int k) {
  switch (k) {
    case 0: return tempHist;
    case 1: return ambHist;
    case 2: return pwrHist;
    default: return ptHist;
  }
}

// Nothing reports the result anywhere, so it returns a plain bool and builds
// no message. The only caller is the bucket rotation.
static bool lbSave() {
  if (!lbMounted) return false;
  LbHdr h;
  h.magic = LB_MAGIC; h.ver = LB_VER; h.buckets = TEMP_BUCKETS;
  h.savedEpoch = nowEpoch(); h.filled = tempFilled; h.pad = 0;
  h.sum = 0;
  for (int k = 0; k < 4; k++) h.sum ^= lbSum(lbRing(k), TEMP_BUCKETS);

  File f = SPIFFS.open(LB_PATH, FILE_WRITE);
  if (!f) return false;
  bool good = f.write((uint8_t *)&h, sizeof h) == sizeof h;
  for (int k = 0; k < 4 && good; k++)
    good = f.write((uint8_t *)lbRing(k), TEMP_BUCKETS * sizeof(int16_t)) ==
           TEMP_BUCKETS * sizeof(int16_t);
  f.close();
  return good;
}

static void lbRestore() {
  if (!lbMounted || !SPIFFS.exists(LB_PATH)) return;
  File f = SPIFFS.open(LB_PATH, FILE_READ);
  if (!f) return;
  LbHdr h;
  if (f.read((uint8_t *)&h, sizeof h) != sizeof h ||
      h.magic != LB_MAGIC || h.ver != LB_VER || h.buckets != TEMP_BUCKETS) {
    f.close();
    return;                                // stale or foreign: ignore, never trust
  }
  bool good = true;
  for (int k = 0; k < 4 && good; k++)
    good = f.read((uint8_t *)lbHeld[k], TEMP_BUCKETS * sizeof(int16_t)) ==
           TEMP_BUCKETS * sizeof(int16_t);
  f.close();
  if (!good) return;
  uint32_t sum = 0;
  for (int k = 0; k < 4; k++) sum ^= lbSum(lbHeld[k], TEMP_BUCKETS);
  if (sum != h.sum) { Serial.println("lifeboat: checksum mismatch, ignored"); return; }

  // A lifeboat is one-shot. Leaving the file in place meant every later boot
  // restored the same stale history and re-armed a gap from a timestamp that
  // had already been consumed -- which is how a save from an hour earlier kept
  // blanking freshly collected buckets.
  SPIFFS.remove(LB_PATH);

  if (!h.savedEpoch) {
    // No clock at save means the age can never be established. Refusing is the
    // safe half of that trade: unknowably old history shown as current is
    // worse than no history. In practice this cannot happen -- saving is done
    // from the app, and the app sets the clock on every poll.
    Serial.println("lifeboat: file has no saved clock, discarded");
    return;
  }
  lbHeldFilled = h.filled;
  lbPendingEpoch = h.savedEpoch;
  lbHave = true;
  Serial.printf("lifeboat: %u buckets held, waiting for the clock\n",
                (unsigned)h.filled);
}

// Reached from learnEpoch(), which is defined above this point.
void lbApplyGapIfPending();

// Called once the clock is known. Pushes in one empty bucket per 15 minutes of
// outage so the chart shows the hole instead of implying continuity.
// Runs once, the first time the clock is known.
//
// Each bucket is placed by its OWN ABSOLUTE TIME, which is what makes this
// simple. Number the 15-minute buckets of all time: b = epoch / 900. The file
// records the moment its newest bucket closed, so every bucket in it has a
// known number, and every slot in the live ring does too. Placing a bucket is
// then one subtraction, and everything else falls out for free:
//
//   * A gap is whatever never gets written -- no gap to compute or insert.
//   * Anything older than the window lands outside the ring and is dropped.
//   * Data collected since boot is already in the ring; restored buckets only
//     fill slots that are still empty, so nothing can be trampled.
//
// The previous version placed buckets by POSITION and corrected with a shift,
// which is where three separate bugs lived: uptime counted as outage, the
// shift trampling newer data, and a stale file re-applied on every boot.
static void lbApplyGap() {
  uint32_t saved = lbPendingEpoch;
  lbPendingEpoch = 0;
  if (!lbHave) return;
  lbHave = false;
  uint32_t now = nowEpoch();
  if (!now || !saved) return;

  const uint32_t bSaved = saved / 900;      // bucket number of the file's newest
  const uint32_t bNow   = now / 900;        // bucket number of the ring's newest
  if (bNow < bSaved) { Serial.println("lifeboat: file is from the future, discarded"); return; }

  int placed = 0;
  for (int j = 0; j < TEMP_BUCKETS; j++) {
    uint32_t b = bSaved - (TEMP_BUCKETS - 1 - j);   // this bucket's number
    if (bSaved < (uint32_t)(TEMP_BUCKETS - 1 - j)) continue;   // predates epoch
    uint32_t age = bNow - b;                        // in buckets
    if (age >= TEMP_BUCKETS) continue;              // older than the window
    int i = TEMP_BUCKETS - 1 - (int)age;            // where it belongs now
    for (int k = 0; k < 4; k++)
      if (lbRing(k)[i] == INT16_MIN) lbRing(k)[i] = lbHeld[k][j];
    if (lbHeld[0][j] != INT16_MIN) placed++;
  }
  if (tempFilled < TEMP_BUCKETS) {
    uint32_t f = tempFilled + placed;
    tempFilled = (f > TEMP_BUCKETS) ? TEMP_BUCKETS : (uint16_t)f;
  }
  Serial.printf("lifeboat: %d buckets placed, %lu min since the file was written\n",
                placed, (unsigned long)((now - saved) / 60));
}

void lbApplyGapIfPending() { if (lbHave) lbApplyGap(); }

#include "app_log.h"
// The same reasoning applies to CAN1. The head unit re-asserts PDM levels at
// ~91 Hz, so a gap of even a second is abnormal; if the bus goes quiet -- a
// tap works loose, the loom is disturbed -- the last levels would otherwise
// sit on screen indefinitely, showing switch states that no longer reflect
// anything and cannot be acted on. Observed on this van 2026-09-02 with a
// finicky T-tap: the app kept displaying yesterday's lights.
static uint32_t pdmAt = 0;
#define PDM_STALE_MS 3000
static uint8_t  acB1 = 0, acFan = 0;
// Fan (wire-verified 2026-08-25): byte1 high nibble 0=auto, 1=manual;
// byte2 = speed, 0x64 low / 0xC8 high. byte1 0x10 = fan-only (compressor off).
static uint8_t  acFanSpd = 0x64;
void sendAC(uint8_t b1);
void sendAC(uint8_t b1, uint8_t fanSpd);
static uint16_t acHeatRaw = 0x252F, acCoolRaw = 0x2540;
static bool     seenAC = false;
static uint8_t  ventSpeed = 0;                   // derived wire value
// The mode byte is COMPOSED per command from discrete state, never carried as
// one shared mutable word. A shared word meant every command re-sent whatever
// lid/direction bits happened to be in it, which reversed the lid twice.
static bool     lidOpen = false;                 // commanded lid position
static bool     airIn   = false;                 // commanded airflow direction
#define VENT_ENABLE 0x40
static inline uint8_t ventModeByte() {
  return VENT_ENABLE | (lidOpen ? 0x10 : 0) | (airIn ? 0x01 : 0);
}
// Lid is a 3-state machine driven by the status flags: bit 3 = in motion,
// bit 4 = open. The reported position is STALE for the whole ~10-15 s transit,
// so it is only trustworthy while settled -- that is the rule, not an
// ownership special case.
enum LidState { LID_CLOSED = 0, LID_MOVING = 1, LID_OPEN = 2 };
static LidState lidState = LID_CLOSED;
// The "in motion" flag lags a lid command by ~3.6-4.1 s (docs/climate-control.md,
// two independent runs). For that window the vent still reports its OLD
// position, so adopting the report would silently undo the command we just
// sent -- and any later command, composing the mode byte from lidOpen, would
// drive the lid the wrong way. lidCmdAt marks the window.
static uint32_t lidCmdAt = 0;
// Vent status freshness + adoption debounce. Status frames arrive at only
// ~0.25 Hz, and one corrupt frame (a flaky tap) adopted as truth held a wrong
// lid state for minutes. Now: a settled state change needs two consecutive
// agreeing frames, and the lid position is reported UNKNOWN if status stops.
static uint32_t ventAt = 0;
#define VENT_STALE_MS 10000
// Byte 3 bit 2 of the vent status frame. Observed set exactly once, on
// 2026-09-04: the lid was physically CLOSED while bit 4 claimed open, after
// the lid had last been moved from the factory panel. A close command produced
// a brief re-home (the motor squeaked and stopped at once, so the vent's own
// limit sensing was working) and the flag cleared to a confident 0x00. The
// van has had three total power losses, and a vent controller rebooting
// without stored position fits this exactly.
//
// The precise meaning is NOT proven -- one observation. But the safe reading
// is "the vent is not sure where the lid is", and under this project's
// standing rule an uncertain position is reported as unknown, never as a
// position. Commands stay enabled: commanding a close is what re-homes it.
#define VENT_POS_UNSURE 0x04
static bool     ventPosUnsure = false;
static uint8_t  lidPendCnt = 0;
static bool     lidPendOpen = false, lidPendAir = false;
// Fan run state is COMMANDED, never inferred. docs/climate-control.md records
// that status byte 2 "oscillates between the setpoint and 0 on a rough 5-10 s
// cycle" -- so the reported speed cannot answer "is the fan on?". It is the
// setpoint that persists; ventFanOn says whether we are asking it to run.
static bool     ventFanOn = false;
// 0 = UNKNOWN. Never invent a setpoint: a hardcoded 100 against max 200 shows
// as a confident "50%" that is pure fiction until the fan actually runs and
// reports, or the user sets one.
static uint8_t  ventSetSpeed = 0;
// Until WE command the vent, the panel owns it and we must adopt what the bus
// shows -- otherwise a fresh boot reports our defaults (off / out / 50%) as if
// they were fact, while the fan is actually running.
// Ownership is a short WINDOW after our own command, not a permanent mode.
// It exists so a status byte that momentarily reads 0 cannot clobber a
// setpoint we just sent; making it permanent made the panel invisible to the
// app forever after its first command.
static uint32_t ventCmdAt = 0;
#define VENT_OWN_MS 5000               // true once we have commanded
// Byte 2 oscillates to 0 on a ~5-10 s cycle while the fan runs, so a zero
// proves nothing -- but a NONZERO reading proves the fan IS running. Latch
// that, and only believe "stopped" after sustained zeros past that period.
static uint32_t ventLastNonZero = 0;
static uint8_t  ventObsSpeed = 0;
static uint8_t  ventRepSpeed = 0;                // speed the fan REPORTS
static uint8_t  ventRepMode = 0;                 // mode/status the fan reports
static float    ventT = 0;
// Rixen mirrored state (all read from the bus)
static uint16_t rixCurRaw = 0, rixTgtRaw = 0;
static uint8_t  rixFlags = 0;
static uint8_t  rixFan = 0, rixFurnace = 0, rixHotWater = 0;
static bool     seenRix = false, seenRixCmd = false;
static bool     seenVent = false;
static float    invAcV = 0, invHz = 0;
// The AC-line frame comes FROM the inverter node, so it stops arriving when
// the inverter powers down. Without a timestamp the last reading persists and
// the UI reports a dead inverter as running. Data older than AC_STALE_MS is
// treated as "no reading", not as the last one.
static uint32_t invAcAt = 0;
#define AC_STALE_MS 3000
static bool     seenInv = false;
static int8_t   invCmd = -1;         // last commanded inverter state (-1 = never commanded)
static float    ambC = 0;
static bool     seenAmb = false;
static uint32_t ambAt = 0;
static uint32_t cntA = 0, cntB = 0;
static char     lastCmd[56] = "none";   // last /api/cmd that ran, for the footer

static float raw2cF(uint16_t r) { return r * 0.03125f - 273.0f; }           // J1939 temp -> °C
static float raw2fF(uint16_t r) { return raw2cF(r) * 9.0f / 5.0f + 32.0f; } // -> °F
static uint16_t f2acRaw(float f) { return (uint16_t)((((f - 32.0f) * 5.0f / 9.0f) + 273.0f) * 32.0f + 0.5f); }

// ----- commands --------------------------------------------------------------
void spoofPress(uint8_t i) {
  const Sw &s = SW[i];
  uint8_t buf[8];
  if (liveSeen[s.pdm][s.mux]) memcpy(buf, live[s.pdm][s.mux], 8);
  else memset(buf, 0, sizeof buf);
  buf[0] = MUX[s.mux];
  uint8_t clearM = 0x03 << s.shift;
  uint8_t press  = 0x02 << s.shift;
  uint32_t id = s.pdm ? ID_PDM2_STAT : ID_PDM1_STAT;

  uint8_t pb[8];
  memcpy(pb, buf, 8);
  pb[s.byte] = (pb[s.byte] & ~clearM) | press;

  if (s.holdrun) {
    uint32_t t0 = millis();
    while (millis() - t0 < s.hold) {
      xSemaphoreTake(canMux, portMAX_DELAY);
      CanA.sendMsgBuf(id, 1, 0, 8, pb);
      xSemaphoreGive(canMux);
      delay(20);
    }
  } else {
    xSemaphoreTake(canMux, portMAX_DELAY);
    CanA.sendMsgBuf(id, 1, 0, 8, pb);
    xSemaphoreGive(canMux);
    delay(s.hold);
  }
  buf[s.byte] &= ~clearM;
  xSemaphoreTake(canMux, portMAX_DELAY);
  CanA.sendMsgBuf(id, 1, 0, 8, buf);
  xSemaphoreGive(canMux);
  Serial.printf("spoof %s\n", s.name);
}

void sendAC(uint8_t b1) { sendAC(b1, acFanSpd); }
void sendAC(uint8_t b1, uint8_t fanSpd) {
  uint8_t ac[8] = {0x01, b1, fanSpd,
                   (uint8_t)acHeatRaw, (uint8_t)(acHeatRaw >> 8),
                   (uint8_t)acCoolRaw, (uint8_t)(acCoolRaw >> 8), 0x00};
  xSemaphoreTake(canMux, portMAX_DELAY);
  CanA.sendMsgBuf(ID_AC_CMD, 1, 0, 8, ac);
  xSemaphoreGive(canMux);
}

void sendVent(uint8_t speed, uint8_t mode) {
  uint8_t v[8] = {0x02, 0x15, speed, mode, 0x00, 0x00, 0x00, 0x00};
  xSemaphoreTake(canMux, portMAX_DELAY);
  CanA.sendMsgBuf(ID_VENT_CMD, 1, 0, 8, v);
  xSemaphoreGive(canMux);
}

void sendInv(bool on) {
  twai_message_t m = {};
  m.identifier = ID_INV_CMD;
  m.extd = 1;
  m.data_length_code = 8;
  m.data[0] = 0x01;
  m.data[1] = on ? 0x01 : 0x00;
  twai_transmit(&m, pdMS_TO_TICKS(500));
}

// ----- light channels (read-only levels; control is by switch spoof) --------
// Dimming was removed 2026-08-25: it required continuously out-transmitting
// the head unit, which our own bus-safety rules forbid. See the note in
// docs/pdm-control.md. Levels here are STATUS ONLY, read off the HU's own
// broadcasts.
static const uint8_t LIGHT_DO[4] = {4, 2, 3, 5};   // cabin, cargo, reading, awning
// Wall-switch spoof index per light, -1 = no physical switch. Reading lights
// are screen-only on this van, so with injection gone they are status-only.
static const int8_t  LIGHT_SW[4] = {0, 1, -1, 5};
static int lvl2pct(uint8_t l) { return (l * 100 + 63) / 127; }

// ----- web UI ------------------------------------------------------------------
#include "webui.h"

// ----- state mirroring ---------------------------------------------------------
void onCanAFrame(uint32_t id, const uint8_t *b, uint8_t len) {
  if (id == ID_PDM1_FAULT || id == ID_PDM2_FAULT) {      // short/overcurrent (DLC 4)
    if (len >= 4) {
      uint8_t p = (id == ID_PDM2_FAULT);
      faultB[p][0] = b[2]; faultB[p][1] = b[3];
    }
    return;
  }
  if (len < 8) return;

  if (id == ID_PDM1_CMD || id == ID_PDM2_CMD) {          // HU -> PDM levels
    pdmAt = millis();
    uint8_t p = (id == ID_PDM2_CMD);
    if (b[0] == 0xFC) {
      for (int ch = 1; ch <= 6; ch++) doLevel[p][ch] = b[ch];

    }
    else if (b[0] == 0xFD) for (int ch = 7; ch <= 12; ch++) doLevel[p][ch] = b[ch - 6];
    return;
  }
  if (id == ID_PDM1_STAT || id == ID_PDM2_STAT) {        // PDM -> HU
    uint8_t p = (id == ID_PDM2_STAT);
    if (b[0] == 0xF0)      { memcpy(live[p][0], b, 8); liveSeen[p][0] = true; }
    else if (b[0] == 0xF8) { memcpy(live[p][1], b, 8); liveSeen[p][1] = true; }
    // feedback amps: F9/C9/39 = ch 1-6, 0A/CA/FA = ch 7-12; bytes 2..7 ×0.125 A
    uint8_t m = b[0];
    if (m == 0xF9 || m == 0xC9 || m == 0x39)
      for (int ch = 1; ch <= 6; ch++) doAmps[p][ch] = b[ch + 1] * 0.125f;
    else if (m == 0x0A || m == 0xCA || m == 0xFA)
      for (int ch = 7; ch <= 12; ch++) doAmps[p][ch] = b[ch - 5] * 0.125f;
    return;
  }
  if (id == ID_TANKS) {
    seenTank = true;
    if (b[0] == 0x00)      { fwL = b[1]; fwR = b[2]; }
    else if (b[0] == 0x02) { grL = b[1]; grR = b[2]; }
    return;
  }
  if (id == ID_AC_STAT) {
    // NOTE: these variables are both "what we commanded" and "what the A/C
    // reports". That is only safe because this frame is a verbatim ECHO of
    // our command. If the A/C ever reports ACTUAL state here (e.g. fan speed
    // 0 while idle), the setpoint and fan selection will start snapping back
    // -- exactly the bug that hit the vent slider on 2026-08-26. Split
    // commanded/reported before trusting any new field from this frame.
    seenAC = true;
    acB1 = b[1]; acFan = b[2]; acFanSpd = b[2];
    acHeatRaw = b[3] | (b[4] << 8);
    acCoolRaw = b[5] | (b[6] << 8);
    return;
  }
  if (id == ID_VENT_STAT) {
    seenVent = true; ventAt = millis();
    // Report-only for SPEED: the fan reports 0 while idle, and writing that
    // back erased the slider's setpoint.
    // The LID is different -- adopt its actual position into our command word,
    // or a later speed/direction command sends the stale boot default (0x40 =
    // enabled, not open) and closes an open vent.
    ventRepSpeed = b[2]; ventRepMode = b[3];
    bool moving = (ventRepMode & 0x08);
    bool repOpen = (ventRepMode & 0x10);
    ventPosUnsure = (ventRepMode & VENT_POS_UNSURE) != 0;
    // Settling window: from a lid command until the vent visibly starts
    // moving. Clearing it on "report agrees with us" is wrong -- after an
    // OPEN command an already-open report agrees immediately, which would
    // reopen the hole. Only actual motion, or the timeout, ends it.
    bool settling = lidCmdAt && (millis() - lidCmdAt < 6000);
    if (moving) { lidCmdAt = 0; settling = false; }   // motion seen: window done
    lidState = (moving || settling) ? LID_MOVING
                                    : (repOpen ? LID_OPEN : LID_CLOSED);
    // Adopt reported position/direction ONLY when settled -- never while the
    // vent is moving, and never during the ~4 s before it admits it is.
    if (!moving && !settling) {
      // Two consecutive settled frames must agree before a state change is
      // believed -- one glitched frame is not the vent's opinion.
      if (repOpen == lidPendOpen && (ventRepMode & 0x01) == lidPendAir) {
        if (++lidPendCnt >= 2 && (lidOpen != repOpen || airIn != (ventRepMode & 0x01))) {
          lidOpen = repOpen;
          airIn = (ventRepMode & 0x01);
          lidPendCnt = 0;
        }
      } else {
        lidPendOpen = repOpen; lidPendAir = (ventRepMode & 0x01); lidPendCnt = 1;
      }
    }
    bool owned = ventCmdAt && (millis() - ventCmdAt < VENT_OWN_MS);
    if (ventRepSpeed > 0) {
      ventLastNonZero = millis();
      ventObsSpeed = ventRepSpeed;
      // A running fan is the ONE time the real speed is on the bus: the
      // command frame is fire-once and never re-broadcast, and the status byte
      // reads 0 whenever the fan is off. Outside our own command window the
      // panel is authoritative, so track whatever it sets.
      if (!owned) { ventSetSpeed = ventRepSpeed; ventFanOn = true; }
      else if (ventSetSpeed == 0) ventSetSpeed = ventRepSpeed;
    } else if (!owned && ventLastNonZero &&
               millis() - ventLastNonZero > 15000) {
      // Sustained zeros, past the documented 5-10 s oscillation: really off.
      ventFanOn = false;
    }
    ventT = raw2cF(b[4] | (b[5] << 8));
    return;
  }
  if (id == ID_AMBIENT) { seenAmb = true; ambAt = millis(); ambC = raw2cF(b[1] | (b[2] << 8)); return; }
  if (id == ID_RIX_STATUS) {                    // 0x724, LE 16-bit fields
    seenRix = true;
    rixCurRaw = b[0] | (b[1] << 8);             // x0.01 C
    rixTgtRaw = b[2] | (b[3] << 8);             // x0.1  C
    rixFlags  = b[6];                           // bit 3 = heat requested
    return;
  }
  if (id == ID_RIX_CMD) {                       // 0x788, multiplexed on byte 0
    seenRixCmd = true;
    switch (b[0]) {
      case 0x01: rixTgtRaw = b[1] | (b[2] << 8); break;   // target echo
      case 0x02: rixFan = b[1]; break;
      case 0x03: rixFurnace = b[1]; break;
      case 0x06: rixHotWater = b[1]; break;
      default: break;                            // 0x0C = ambient relay, ignored
    }
    return;
  }
}

void onCanBFrame(const twai_message_t &m) {
  if (!m.extd || m.data_length_code < 8) return;
  const uint8_t *d = m.data;
  if (m.identifier == ID_DC1) {
    battV = (d[2] | (d[3] << 8)) * 0.05f;
    int32_t raw = d[4] | (d[5] << 8) | (d[6] << 16) | ((uint32_t)d[7] << 24);
    battA = -(raw - 2000000000LL) / 1000.0f;   // wire + = discharge; we show panel sign (+ = charging)
    seenBatt = true; battAt = millis();
  } else if (m.identifier == ID_DC2) {
    battT = raw2cF(d[2] | (d[3] << 8));
    battSoC2 = d[4];
    battMin = d[5] | (d[6] << 8);
    seenBatt = true; battAt = millis(); battTAt = battAt;
  } else if (m.identifier == ID_DC3) {
    battSoH = d[2];
    battAh = d[3] | (d[4] << 8);
    seenBatt = true; battAt = millis();
  } else if (m.identifier == ID_INV_AC) {
    invAcAt = millis();
    invAcV = (d[1] | (d[2] << 8)) * 0.05f;
    invHz = (d[5] | (d[6] << 8)) / 128.0f;
    seenInv = true;
  } else if (m.identifier >= ID_CELL_BASE &&
             m.identifier <= ID_CELL_BASE + (ID_CELL_N - 1) * ID_CELL_STEP &&
             ((m.identifier - ID_CELL_BASE) % ID_CELL_STEP) == 0) {
    // Stored verbatim and decoded nowhere: the mapping is a guess, and a guess
    // baked into the capture would destroy the evidence it exists to gather.
    uint8_t idx = (uint8_t)((m.identifier - ID_CELL_BASE) / ID_CELL_STEP);
    memcpy(cellFrm[idx], d, 8);
    cellSeen[idx] = true;
    cellAt = millis();
  }
}

// ----- JSON state ----------------------------------------------------------------
// FOUR 121-bucket histories dominate this buffer: temps are ~5 chars each and
// power can be 6 ("-1037,"), so worst case is ~4.3 KB before the rest of the
// state. The J() macro truncates silently once full, which would break the
// whole response rather than just a chart -- so size it with real headroom.
// Grown from 5120 when the pack-temperature history was added; at 5120 the
// four histories plus state could reach the ceiling and silently truncate.
static char jbuf[6656];

// Amp-hours still needed to reach a full pack, from the two figures the BMS
// does publish. Returns -1 when it cannot be derived.
static float ahToFull() {
  float soc = battSoC2 * 0.5f;
  if (battAh == 0 || soc <= 0.0f || soc > 100.0f) return -1.0f;
  return battAh * (100.0f - soc) / soc;
}

void sendState() {
  char *w = jbuf;
  int left = sizeof jbuf;
  int n = snprintf(w, left, "{\"build\":\"%s %s\",", __DATE__, __TIME__);

  // helper macro: append formatted
  #define J(...) do { w += n; left -= n; if (left <= 0) goto out; n = snprintf(w, left, __VA_ARGS__); } while (0)

  if (seenBatt && (millis() - battAt < BATT_STALE_MS)) {
    float soc = battSoC2 * 0.5f;
    // Pack watts is a real calculation: both terms are measured on CAN2 and
    // this IS the pack rail (unlike the DC loads -- see LOAD_BUS_V).
    float battW = battV * battA;
    // Three separate lines so the UI can lay them out; sign convention is a
    // DRAW negative, a surplus (shore, solar, alternator) positive with no
    // + sign. battA already follows this -- the wire value is negated at decode.
    J("\"soc\":%.1f,\"battV\":%.2f,", soc, battV);
    // Temperature is NOT repeated here -- it has its own row and chart in the
    // battery card. Voltage and amp-hours have no other home, so they stay.
    J("\"packline\":\"Pack: %.1fV  %uAh\",", battV, battAh);
    J("\"packf\":%s,",
      (battTAt && (millis() - battTAt < BATT_STALE_MS))
          ? String(battT * 9 / 5 + 32, 0).c_str() : "null");
    // vcell is the pack average, NOT the weak cell -- a collapsed cell reads
    // far below this. It is here because the average is what makes the
    // disagreement legible: 3.11 V/cell average at 80 % is the tell.
    J("\"vsoc\":%d,\"vcell\":%.2f,", vsocBad ? 1 : 0, battV / 16.0f);
    if (vsocBad)
      J("\"vsocago\":%lu,", (unsigned long)((millis() - vsocAt) / 1000));

    J("\"drawline\":\"%.1fA (%.0fW)\",", battA, battW);
    // The BMS sends 0xFFFF whenever it declines to estimate -- which it does
    // at low draw, exactly when the answer is most reassuring. We have amp
    // hours and amps, so compute it rather than showing nothing. Charging has
    // no "time left" to report.
    // ALWAYS emit something: the UI shows both extrapolations permanently, and
    // a dead zone here (|A| below a threshold) made the immediate figure vanish
    // exactly when the van was idling. Near-zero discharge produces a huge
    // number, which the UI renders as "fault" per its >999h rule.
    // Explicit minutes and a direction, not prose for the UI to regex. The
    // two rows are allowed to DISAGREE, including on direction: plug in after
    // a long discharge and the instant row flips to "til full" while the
    // hour-long mean is still negative and reads "til empty". That is correct
    // -- the window corrects itself as the new samples accumulate.
    uint32_t mins = 0;
    const char *dir = "";
    if (battA > 0.0f) {                       // charging: time to a full pack
      float ah = ahToFull();
      if (ah >= 0.0f) { mins = (uint32_t)(ah / battA * 60.0f); dir = "full"; }
    } else {                                  // discharging: time to empty
      if (battMin != 0xFFFF && battMin > 0) mins = battMin;
      else if (battA < 0.0f && battAh > 0) mins = (uint32_t)(battAh / -battA * 60.0f);
      if (mins > 0) dir = "empty";
    }
    J("\"instmin\":%lu,\"instdir\":\"%s\",", (unsigned long)mins, dir);
    J("\"batt\":\"\",");
  } else J("\"soc\":null,\"batt\":\"%s\","
           "\"packline\":\"\",\"drawline\":\"--\","
           "\"instmin\":0,\"instdir\":\"\","
           "\"packf\":null,\"vsoc\":0,",
           seenBatt ? "CAN2 battery data stale" : "no CAN2 battery frames");
  J("\"tempin\":%s,", seenAmb ? String(ambC * 9 / 5 + 32, 1).c_str() : "null");
  // Scoped: a declaration here would cross the J() macro's goto.
  {
    bool canAlive = pdmAt && (millis() - pdmAt < PDM_STALE_MS);
    J("\"can1\":%d,", canAlive ? 1 : 0);
    J("\"gfan\":%d,", canAlive ? doLevel[GALLEY_FAN_PDM][GALLEY_FAN_DO] : -1);
  }
  if (seenInv && (millis() - invAcAt < AC_STALE_MS))
    J("\"invtext\":\"AC line %.0fV %.1fHz\",", invAcV, invHz);
  else if (seenInv) J("\"invtext\":\"no AC line data\",");
  else J("\"invtext\":\"\",");
  {
    // AC line presence is the truth signal, but only while the reading is
    // FRESH. A stale reading means the inverter stopped reporting, which is
    // itself evidence it is off -- never evidence that it is still on.
    bool acFresh = seenInv && (millis() - invAcAt < AC_STALE_MS);
    bool acLive  = acFresh && invAcV > 90.0f;
    // With no fresh data we do not know; report -1 rather than echoing back
    // whatever we last commanded, which is not an observation.
    int invShown = acFresh ? (acLive ? 1 : 0) : -1;
    J("\"invon\":%d,\"aclive\":%d,\"shore\":%d,",
      invShown, acLive ? 1 : 0, (acLive && battA > 0.5f) ? 1 : 0);
  }

  J("\"lights\":[");
  for (int k = 0; k < 4; k++) {
    uint8_t ch = LIGHT_DO[k];
    // Loads only ever consume, so both figures are reported negative.
    J("%s{\"on\":%d,\"pct\":%d,\"amps\":%.3f,\"watts\":%.1f,\"ctl\":%d}", k ? "," : "",
      doLevel[0][ch] > 0 ? 1 : 0, lvl2pct(doLevel[0][ch]),
      -doAmps[0][ch], -doAmps[0][ch] * LOAD_BUS_V,
      LIGHT_SW[k] >= 0 ? 1 : 0);
  }
  J("],");
  J("\"sw\":[");
  for (int i = 0; i < 6; i++) J("%s%d", i ? "," : "", doLevel[SW_OUT_PDM[i]][SDO(i)] > 0 ? 1 : 0);
  J("],\"amps\":[");
  for (int i = 0; i < 6; i++) J("%s%.3f", i ? "," : "", -doAmps[SW_OUT_PDM[i]][SDO(i)]);
  J("],\"watts\":[");
  for (int i = 0; i < 6; i++) J("%s%.1f", i ? "," : "", -doAmps[SW_OUT_PDM[i]][SDO(i)] * LOAD_BUS_V);
  J("],");

  if (seenAC) {
    const char *mode;
    uint8_t op = acB1 & 0x0F;
    if (acB1 == 0x04) mode = "on, compressor off";
    else switch (op) { case 0: mode = "off"; break; case 1: mode = "cool"; break;
                       case 2: mode = "heat"; break; default: mode = "on"; }
    J("\"acmode\":\"%s\",\"coolsp\":%d,", mode, (int)lroundf(raw2fF(acCoolRaw)) - 2);  // panel deadband: wire-2
    J("\"heatsp\":%d,\"acfan\":%d,\"acfanspd\":%d,",
      (int)lroundf(raw2fF(acHeatRaw)) + 2, (acB1 >> 4) & 0x0F, acFan);
  } else J("\"acmode\":\"?\",\"coolsp\":null,\"heatsp\":null,");

  if (seenVent) {
    // If vent status has gone quiet, the lid position is UNKNOWN, not the
    // last value -- stale state presented as fact is the recurring bug class.
    const bool ventFresh = ventAt && (millis() - ventAt < VENT_STALE_MS);
    // vset = the setpoint (survives fan-off); vfan = commanded run state.
    // vrep is the raw reported speed, kept for diagnostics only -- it
    // oscillates to 0 while the fan runs and must not drive any UI state.
    // No status STRING is sent: the UI composes its own wording from these
    // fields, and a second, unused rendering here was free to drift from it.
    J("\"vspeed\":%d,\"vrep\":%d,\"vdir\":%d,"
      "\"vopen\":%d,\"vmoving\":%d,\"vset\":%d,\"vfan\":%d,"
      "\"vown\":%d,\"vobs\":%d,\"vunsure\":%d,",
      ventSpeed, ventRepSpeed, airIn ? 1 : 0,
      (ventFresh && !ventPosUnsure) ? ((lidState == LID_OPEN) ? 1 : 0) : -1,
      ventFresh ? ((lidState == LID_MOVING) ? 1 : 0) : -1,
      ventSetSpeed, ventFanOn ? 1 : 0,
      (ventCmdAt && (millis() - ventCmdAt < VENT_OWN_MS)) ? 1 : 0, ventObsSpeed,
      ventPosUnsure ? 1 : 0);
  } else J("\"vspeed\":0,\"vrep\":0,\"vdir\":0,"
           "\"vopen\":-1,\"vmoving\":0,\"vset\":0,\"vfan\":0,"
           "\"vown\":0,\"vobs\":0,\"vunsure\":0,");

  if (seenRix) {
    // 0x724: current x0.01 C, target x0.1 C. Rixen takes the target with NO
    // deadband (unlike the RV-C thermostat frame, which carries panel +/- 2).
    J("\"rixcur\":%.1f,\"rixtgt\":%.1f,\"rixheat\":%d,",
      (rixCurRaw * 0.01f) * 9.0f / 5.0f + 32.0f,
      (rixTgtRaw * 0.1f) * 9.0f / 5.0f + 32.0f,
      (rixFlags & 0x08) ? 1 : 0);
    J("\"rixfan\":%d,\"rixfurn\":%d,\"rixhw\":%d,", rixFan, rixFurnace, rixHotWater);
  } else J("\"rixcur\":null,\"rixtgt\":null,\"rixheat\":0,"
           "\"rixfan\":0,\"rixfurn\":0,\"rixhw\":0,");
  if (seenTank) J("\"fresh\":%d,\"gray\":%d,", fwR ? fwL * 100 / fwR : 0, grR ? grL * 100 / grR : 0);
  else J("\"fresh\":null,\"gray\":null,");

  {
    // The sixteen per-cell bytes are bytes 4..7 of the last four frames.
    // CONFIRMED per-cell on 2026-09-05: one byte read 0x7B against fifteen
    // 0x7D, matching the phone app exactly (docs/energy-can2.md). Reported raw
    // alongside the spread, which is what makes a diverging cell obvious.
    bool cf = cellAt && (millis() - cellAt < CELL_STALE_MS);
    J("\"cellfresh\":%d,\"cells\":[", cf ? 1 : 0);
    int lo = 255, hi = 0;
    for (int f = 2; f < ID_CELL_N; f++)
      for (int b = 4; b < 8; b++) {
        uint8_t v = cellFrm[f][b];
        J("%s%u", (f == 2 && b == 4) ? "" : ",", v);
        if (cellSeen[f]) { if (v < lo) lo = v; if (v > hi) hi = v; }
      }
    J("],\"cellspread\":%d,", (lo <= hi) ? (hi - lo) : 0);
    // cellmin/cellminc are the SAME numbers the weak-cell warning is raised
    // from, so the UI must display these rather than recompute its own -- a
    // second minimum could name a different cell than the banner does.
    // cellrep is the BMS's own lowest-cell field (0x18FF918E byte 7), carried
    // beside ours as a cross-check; if the two ever disagree, one of the two
    // decodes is wrong and that should be visible rather than averaged away.
    J("\"cellbad\":%d,\"cellmin\":%d,\"cellminc\":%d,\"cellrep\":%d,",
      cellBad ? 1 : 0, cellMinV, cellMinIdx + 1, cellFrm[0][7]);
    if (cellBad)
      J("\"cellbadago\":%lu,", (unsigned long)((millis() - cellBadAt) / 1000));
    J("\"cellraw\":[");
    for (int f = 0; f < ID_CELL_N; f++) {
      J("%s\"", f ? "," : "");
      for (int b = 0; b < 8; b++) J("%02X", cellFrm[f][b]);
      J("\"");
    }
    J("],");
  }

  {
    // Temperature history: TEMP_BUCKETS entries, oldest first, in °F.
    // null marks an hour with too few samples or no data at all.
    J("\"temphist\":[");
    for (int i = 0; i < TEMP_BUCKETS; i++) {
      int16_t v = tempHist[i];
      if (i == TEMP_BUCKETS - 1 && tempCnt >= TEMP_MIN_VALID)
        v = (int16_t)(tempAcc / tempCnt * 10.0f);      // bucket in progress
      if (v == INT16_MIN) J("%snull", i ? "," : "");
      else J("%s%.0f", i ? "," : "", v / 10.0f * 9.0f / 5.0f + 32.0f);
    }
    J("],\"ambhist\":[");
    for (int i = 0; i < TEMP_BUCKETS; i++) {
      int16_t v = ambHist[i];
      if (i == TEMP_BUCKETS - 1 && ambCnt >= TEMP_MIN_VALID)
        v = (int16_t)(ambAcc / ambCnt * 10.0f);
      if (v == INT16_MIN) J("%snull", i ? "," : "");
      else J("%s%.0f", i ? "," : "", v / 10.0f * 9.0f / 5.0f + 32.0f);
    }
    J("],\"pwrhist\":[");
    for (int i = 0; i < TEMP_BUCKETS; i++) {
      int16_t v = pwrHist[i];
      if (i == TEMP_BUCKETS - 1 && pwrCnt >= TEMP_MIN_VALID)
        v = (int16_t)(pwrAcc / pwrCnt);
      if (v == INT16_MIN) J("%snull", i ? "," : "");
      else J("%s%d", i ? "," : "", v);
    }
    J("],\"packhist\":[");
    for (int i = 0; i < TEMP_BUCKETS; i++) {
      int16_t v = ptHist[i];
      if (i == TEMP_BUCKETS - 1 && ptCnt >= TEMP_MIN_VALID)
        v = (int16_t)(ptAcc / ptCnt * 10.0f);
      if (v == INT16_MIN) J("%snull", i ? "," : "");
      else J("%s%.0f", i ? "," : "", v / 10.0f * 9.0f / 5.0f + 32.0f);
    }
    J("],\"tempnow\":%.1f,\"tempfill\":%u,\"temphours\":%d,",
      temperatureRead() * 9.0f / 5.0f + 32.0f, tempFilled, TEMP_HOURS);
    if (hotAt)
      J("\"hotat\":%lu,\"hotago\":%lu,",
        (unsigned long)hotAt, (unsigned long)((millis() - hotAt) / 1000));
    // Mean power over the window, and the time remaining it implies. The ring
    // holds one hour; the row appears once the window spans fifteen minutes.
    // The window SPAN in minutes, capped at the ring's one hour. The UI used to
    // derive this from the sample count, which understates a sparsely sampled
    // window -- 120 samples over a real 15 minutes would have read "10m".
    {
      uint32_t spanMin = pwFirstMs ? ((millis() - pwFirstMs) / 60000UL) : 0;
      if (spanMin > 60) spanMin = 60;
      J("\"pwrwinN\":%u,\"pwrwinMin\":%lu,", pwFill, (unsigned long)spanMin);
    }
    // Ready once the window SPANS 15 minutes, however many samples landed in
    // it. A sparse window is a noisier mean, not a shorter one, and it beats
    // hiding the row indefinitely because the bus dropped frames.
    bool pwReady = pwFirstMs && (millis() - pwFirstMs) >= PWRWIN_MS * PWRWIN_MIN_FILL;
    if (pwReady && pwFill > 0) {
      float sum = 0;
      for (uint16_t i = 0; i < pwFill; i++) sum += pwrwin[i];
      float avgW = sum / pwFill;
      float avgA = (battV > 1.0f) ? avgW / battV : 0.0f;
      // Same shape as the instant row: minutes plus a direction. The window
      // sign is what decides the direction here, so shortly after plugging in
      // this row still reads "til empty" while the instant row reads "til
      // full". They are measuring different spans and are meant to disagree.
      uint32_t wmins = 0;
      const char *wdir = "error";             // filled but underivable
      if (avgA > 0.05f) {                     // net charge over the window
        float ah = ahToFull();
        if (ah >= 0.0f) { wmins = (uint32_t)(ah / avgA * 60.0f); wdir = "full"; }
      } else if (avgA < -0.05f && battAh > 0) {
        wmins = (uint32_t)(battAh / -avgA * 60.0f); wdir = "empty";
      }
      J("\"winmin\":%lu,\"windir\":\"%s\",", (unsigned long)wmins, wdir);
    } else J("\"winmin\":0,\"windir\":\"\",");   // "" = window still filling
  }
  {
    char ft[256];                       // grew when the clock field was added
    uint32_t up = millis() / 1000;
    char ups[24];
    if (up < 7200) snprintf(ups, sizeof ups, "up %lu mins", (unsigned long)(up / 60));
    else snprintf(ups, sizeof ups, "up %luh %02lum", (unsigned long)(up / 3600),
                  (unsigned long)((up % 3600) / 60));
    // Say plainly whether the clock has been learned. Silence here would let
    // a board that never saw a phone look identical to one that did.
    char clk[40];   // roomy: the compiler cannot bound tm_year
    uint32_t ep = nowEpoch();
    if (ep) {
      time_t t = (time_t)ep;
      struct tm tmv;
      gmtime_r(&t, &tmv);
      snprintf(clk, sizeof clk, "%04d-%02d-%02d %02d:%02d:%02dZ",
               tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
               tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    } else snprintf(clk, sizeof clk, "not set");
    snprintf(ft, sizeof ft,
             "%s  |  clock %s  |  CAN1 %lu  CAN2 %lu (%lus ago)  |  last: %s%s%s",
             ups, clk,
             (unsigned long)cntA, (unsigned long)cntB,
             (unsigned long)(battAt ? (millis() - battAt) / 1000 : 0), lastCmd,
             (faultB[0][0] | faultB[0][1]) ? "  PDM1 FAULT" : "",
             (faultB[1][0] | faultB[1][1]) ? "  PDM2 FAULT" : "");
    J("\"foot\":\"%s\"", ft);
  }
  {
    // Raw HU level bytes, both PDMs, DO1..12 in order — the debug view that
    // settles "which byte actually moved" without a second CAN logger.
    char db[200];
    int off = snprintf(db, sizeof db, "levels P1:");
    for (int ch = 1; ch <= 12; ch++) off += snprintf(db + off, sizeof db - off, " %02X", doLevel[0][ch]);
    off += snprintf(db + off, sizeof db - off, "  P2:");
    for (int ch = 1; ch <= 12; ch++) off += snprintf(db + off, sizeof db - off, " %02X", doLevel[1][ch]);
    J(",\"dbg\":\"%s\"", db);
  }
  J("}");
out:
  jbuf[sizeof jbuf - 1] = 0;
  server.send(200, "application/json", jbuf);
  #undef J
}

// ----- command endpoint ---------------------------------------------------------
void sendOK(const char *what) {
  logWrite(3, what);
  snprintf(lastCmd, sizeof lastCmd, "%s @ %lus", what, (unsigned long)(millis() / 1000));
  Serial.printf("cmd: %s\n", what);
  server.send(200, "application/json", "{\"ok\":true}");
}

void onCmd() {
  String c = server.arg("c");
  { String q = "c=" + c; logWrite(2, q.c_str()); }   // before: death mid-action shows as CMD> without CMD OK
  if (c == "toggle") {
    int i = server.arg("i").toInt();
    if (i < 0 || i > 5) { server.send(400, "text/plain", "bad i"); return; }
    spoofPress((uint8_t)i);
    sendOK(SW[i].name);
  } else if (c == "ac") {
    String m = server.arg("mode");
    if (m == "on")        { sendAC(0x01); sendOK("ac on"); }
    else if (m == "off")  { sendAC(0x00); sendOK("ac off"); }
    else if (m == "comp") { sendAC(0x04); sendOK("ac compressor off"); }
    else if (m == "heat") { sendAC(0x02); sendOK("ac heat"); }
    else server.send(400, "text/plain", "bad mode");
  } else if (c == "acfan") {
    // Wire-verified 2026-08-25: byte1 high nibble 0=auto/1=manual, byte2 speed
    // (0x64 low, 0xC8 high). Operating-mode nibble is preserved either way.
    String m = server.arg("m");
    uint8_t opmode = acB1 & 0x0F;
    if (m == "auto")      { acFanSpd = 0x64; sendAC(opmode, acFanSpd); sendOK("ac fan auto"); }
    else if (m == "low")  { acFanSpd = 0x64; sendAC(0x10 | opmode, acFanSpd); sendOK("ac fan low"); }
    else if (m == "high") { acFanSpd = 0xC8; sendAC(0x10 | opmode, acFanSpd); sendOK("ac fan high"); }
    else server.send(400, "text/plain", "bad fan mode");
  } else if (c == "cool") {
    int d = server.arg("d").toInt();           // +/- 1 °F on the DISPLAYED value
    int8_t step = (d >= 0) ? 1 : -1;
    int shown = (int)lroundf(raw2fF(acCoolRaw)) - 2 + step;   // panel deadband
    shown = constrain(shown, 55, 90);          // sane bounds even before first echo
    acCoolRaw = f2acRaw(shown + 2.0f);
    sendAC(acB1 ? acB1 : 0x01);
    sendOK("cool setpoint");
  } else if (c == "vent") {
    //   open=0|1  lid position       fan=0|1   run state
    //   speed=N   setpoint (N=0 stops)   dir=0|1   airflow
    // Each argument touches only its own state; the mode byte is composed at
    // send time, so a speed command can never move the lid.
    bool used = false;
    if (server.hasArg("open")) {
      lidOpen = (server.arg("open") == "1");
      lidCmdAt = millis();               // opens the settling window
      if (!lidOpen) ventFanOn = false;   // closed lid: fan stops, setpoint kept
      used = true;
    }
    if (server.hasArg("dir")) { airIn = (server.arg("dir") == "1"); used = true; }
    if (server.hasArg("speed")) {
      int v = server.arg("speed").toInt();
      if (v > 0) ventSetSpeed = (uint8_t)constrain(v, 1, 200);
      else ventFanOn = false;            // explicit 0 = stop, setpoint preserved
      used = true;
    }
    if (server.hasArg("fan")) {
      ventFanOn = (server.arg("fan") == "1");
      if (ventFanOn && ventSetSpeed == 0) ventSetSpeed = 100;  // must start somewhere
      used = true;
    }
    if (used) {
      ventCmdAt = millis();
      ventSpeed = ventFanOn ? ventSetSpeed : 0;
      sendVent(ventSpeed, ventModeByte());
      char m[80];
      snprintf(m, sizeof m, "vent wire=%u set=%u fan=%d lid=%d air=%d",
               ventSpeed, ventSetSpeed, ventFanOn ? 1 : 0, lidOpen ? 1 : 0, airIn ? 1 : 0);
      sendOK(m);
    }
    else server.send(400, "text/plain", "bad vent");
  } else if (c == "inv") {
    bool on = server.arg("on") == "1";
    sendInv(on);
    invCmd = on ? 1 : 0;
    sendOK(on ? "inverter on" : "inverter off");
  } else {
    server.send(400, "text/plain", "unknown");
  }
}

// ----- setup / loop -------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("companion app: boot");

  for (int i = 0; i < TEMP_BUCKETS; i++) {
    tempHist[i] = INT16_MIN; ambHist[i] = INT16_MIN; pwrHist[i] = INT16_MIN;
    ptHist[i] = INT16_MIN;
  }
  bootId = (uint16_t)(esp_random() & 0xFFFF);   // labels this run in the log
  lbMounted = SPIFFS.begin(true);            // true = format if unformatted
  if (!lbMounted) Serial.println("SPIFFS mount failed; lifeboat disabled");
  else lbRestore();
  Serial.printf("temp sensor: %.1f C\n", temperatureRead());
  tempHourStart = millis();
  tempLastSample = millis();

  logInit();

  canMux = xSemaphoreCreateMutex();
  SPI.begin(MCP2518_SCLK, MCP2518_MISO, MCP2518_MOSI, MCP2518_CS);
  if (CanA.begin(CAN20_250KBPS, MCP2518FD_40MHz) == CAN_OK) Serial.println("canA: CAN1 250k OK");
  else Serial.println("canA: init FAIL");
  CanA.__flgFDF = 0;

  twai_general_config_t g = TWAI_GENERAL_CONFIG_DEFAULT(CANB_TX, CANB_RX, TWAI_MODE_NORMAL);
  twai_timing_config_t t = TWAI_TIMING_CONFIG_250KBITS();
  twai_filter_config_t f = TWAI_FILTER_CONFIG_ACCEPT_ALL();
  if (twai_driver_install(&g, &t, &f) == ESP_OK && twai_start() == ESP_OK)
    Serial.println("canB: CAN2 250k OK");
  else Serial.println("canB: init FAIL");

  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  dns.start(53, "*", WiFi.softAPIP());   // phones that probe for a portal get us
  Serial.printf("AP %s up, ip %s\n", AP_SSID, WiFi.softAPIP().toString().c_str());
  if (MDNS.begin(MDNS_NAME)) Serial.printf("mDNS: http://%s.local\n", MDNS_NAME);

  server.on("/", HTTP_GET, []() {
    server.sendHeader("Cache-Control", "no-store");   // phones LOVE stale pages
    server.send_P(200, "text/html", INDEX_HTML);
  });
  server.on("/api/state", HTTP_GET, []() {
    // The poll that already runs every second is the natural carrier: no extra
    // request, and the clock re-syncs continuously for free.
    if (server.hasArg("now"))
      learnEpoch((uint32_t)strtoul(server.arg("now").c_str(), NULL, 10));
    server.sendHeader("Cache-Control", "no-store");
    sendState();
  });
  server.on("/api/cmd", HTTP_POST, onCmd);
  server.on("/api/log", HTTP_GET, []() {
    server.send(200, "text/plain", logDump());
  });
  server.onNotFound([]() {           // captive-portal probes land on the UI
    server.sendHeader("Location", String("http://") + WiFi.softAPIP().toString() + "/");
    server.send(302, "text/plain", "");
  });
  server.begin();
  Serial.println("http: 80 ready");

  // Core 0 belongs to the WiFi stack (priority 23, unbeatable). Injection
  // timing must not compete with it: CAN owns core 1, above the web server.
  xTaskCreatePinnedToCore(canTask, "can", 4096, NULL, 5, NULL, 1);
  Serial.println("can task: core 1 (off the WiFi core)");
}

// CAN work must not queue behind HTTP: the HU re-asserts every ~11 ms and we
// have to see those frames promptly to mirror state. Sharing the Arduino loop
// with the web server made injections land late or not at all, and the light
// averaged between our level and the HU's -> visible flicker.
// CAN therefore owns core 1 (see xTaskCreatePinnedToCore below); core 0 is the
// WiFi stack's, at priority 23, which nothing here can outrun.
void canTask(void *) {
  for (;;) {
    bool worked = false;
    for (int i = 0; i < 16; i++) {          // drain, don't sip
      uint8_t len, buf[8];
      uint32_t id;
      xSemaphoreTake(canMux, portMAX_DELAY);
      bool have = (CanA.checkReceive() == CAN_MSGAVAIL);
      if (have) { CanA.readMsgBuf(&len, buf); id = CanA.getCanId(); }
      xSemaphoreGive(canMux);
      if (!have) break;
      cntA++;
      onCanAFrame(id, buf, len);
      worked = true;
    }
    twai_message_t msg;
    while (twai_receive(&msg, 0) == ESP_OK) { cntB++; onCanBFrame(msg); worked = true; }
    if (!worked) vTaskDelay(1);
  }
}

// Roll the accumulators into a bucket every 15 minutes. All timing uses unsigned
// subtraction so the 49.7-day millis() rollover is handled -- this box can run
// for months, and a naive comparison would corrupt the history at ~7 weeks.
void tempTick() {
  if (!tempOK) return;
  uint32_t now = millis();
  if ((uint32_t)(now - lastPulseLog) >= 300000UL) {   // 5-min alive pulse
    lastPulseLog = now;
    logWrite(4, "pulse");
  }
  if ((uint32_t)(now - pwAt) >= PWRWIN_MS) {
    // Advance by the interval rather than snapping to now. Snapping discards
    // the leftover every time the loop runs late, so the ring fell behind the
    // wall clock a little on every lap and never caught up. The bucket
    // rotation below has always done it this way; this did not.
    pwAt += PWRWIN_MS;
    if ((uint32_t)(now - pwAt) > PWRWIN_MS * 4) pwAt = now;   // long stall: resync
    if (seenBatt && (uint32_t)(now - battAt) < BATT_STALE_MS) {
      if (!pwFirstMs) pwFirstMs = now;
      pwrwin[pwHead] = (int16_t)(battV * battA);
      pwHead = (pwHead + 1) % PWRWIN_N;
      if (pwFill < PWRWIN_N) pwFill++;
    }
  }
  if ((uint32_t)(now - tempLastSample) >= TEMP_SAMPLE_MS) {
    tempLastSample = now;
    float c = temperatureRead();          // °C, ESP32-S3 internal die sensor
    if (c > -40.0f && c < 125.0f) {       // ignore the out-of-range error value
      tempAcc += c;
      tempCnt++;
    }
    if (c > 85.0f) {                      // >185 F: streak in 30 s samples
      if (hotStreakCnt == 0) hotStreakStart = now;
      hotStreakCnt++;
      if (hotStreakCnt == 120) hotAt = millis();   // one hour: latch/update
    } else hotStreakCnt = 0;
    // Only sample ambient while the reading is fresh; a stale value would
    // otherwise be averaged in as though the cabin were still being measured.
    if (seenAmb && (uint32_t)(now - ambAt) < 30000UL) {
      ambAcc += ambC;
      ambCnt++;
    }
    // Only while the battery data is fresh -- same reasoning as ambient.
    if (seenBatt && (uint32_t)(now - battAt) < BATT_STALE_MS) {
      pwrAcc += battV * battA;
      pwrCnt++;
    }
    // Pack temperature rides on DC2 specifically, so it needs DC2's own
    // freshness rather than the shared battery timestamp.
    if (battTAt && (uint32_t)(now - battTAt) < BATT_STALE_MS) {
      ptAcc += battT;
      ptCnt++;
    }
  }
  // Voltage/SoC disagreement. Evaluated here rather than in sendState so it is
  // detected and logged whether or not a phone is connected -- the shutdowns
  // this exists to catch happen with nobody looking.
  if (seenBatt && (uint32_t)(now - battAt) < BATT_STALE_MS) {
    float socNow = battSoC2 * 0.5f;
    if (!vsocBad && battV < VSOC_V_TRIP && socNow > VSOC_SOC_MIN) {
      vsocBad = true;
      vsocAt = now;
      logWrite(5, "vsoc");
    } else if (vsocBad && (battV > VSOC_V_CLEAR || socNow <= VSOC_SOC_MIN)) {
      vsocBad = false;
      logWrite(6, "vsoc ok");
    }
  }
  // Weak cell. Evaluated here for the same reason as the V/SoC watch: it has to
  // catch a shutdown that happens with nobody looking. The log entry carries
  // the cell number and its raw byte, so the forensics say WHICH cell went.
  if (cellStats()) {
    bool bad = (cellMinV < CELL_LOW_TRIP) || (cellSpreadV >= CELL_SPREAD_TRIP);
    bool fine = (cellMinV > CELL_LOW_CLEAR) && (cellSpreadV <= CELL_SPREAD_CLEAR);
    if (!cellBad && bad) {
      cellBad = true;
      cellBadAt = now;
      // Roomy enough that snprintf cannot truncate (the compiler cannot prove
      // the bounds on these); logWrite copies the first 7 chars into cmd[8],
      // and the longest real value, "c16 255", is exactly 7.
      char msg[16];
      snprintf(msg, sizeof msg, "c%u %u", (unsigned)(cellMinIdx + 1),
               (unsigned)cellMinV);
      logWrite(7, msg);
    } else if (cellBad && fine) {
      cellBad = false;
      logWrite(8, "cell ok");
    }
  }
  if ((uint32_t)(now - tempHourStart) >= 900000UL) {   // 15-minute buckets
    tempHourStart += 900000UL;
    // Shift the ring left; the newest completed bucket lands at the end.
    for (int i = 0; i < TEMP_BUCKETS - 1; i++) tempHist[i] = tempHist[i + 1];
    tempHist[TEMP_BUCKETS - 1] =
        (tempCnt >= TEMP_MIN_VALID) ? (int16_t)(tempAcc / tempCnt * 10.0f)
                                    : INT16_MIN;      // too few samples: void
    tempAcc = 0; tempCnt = 0;
    for (int i = 0; i < TEMP_BUCKETS - 1; i++) ambHist[i] = ambHist[i + 1];
    ambHist[TEMP_BUCKETS - 1] =
        (ambCnt >= TEMP_MIN_VALID) ? (int16_t)(ambAcc / ambCnt * 10.0f)
                                   : INT16_MIN;
    ambAcc = 0; ambCnt = 0;
    for (int i = 0; i < TEMP_BUCKETS - 1; i++) pwrHist[i] = pwrHist[i + 1];
    pwrHist[TEMP_BUCKETS - 1] =
        (pwrCnt >= TEMP_MIN_VALID) ? (int16_t)(pwrAcc / pwrCnt)
                                   : INT16_MIN;
    pwrAcc = 0; pwrCnt = 0;
    for (int i = 0; i < TEMP_BUCKETS - 1; i++) ptHist[i] = ptHist[i + 1];
    ptHist[TEMP_BUCKETS - 1] =
        (ptCnt >= TEMP_MIN_VALID) ? (int16_t)(ptAcc / ptCnt * 10.0f)
                                  : INT16_MIN;
    ptAcc = 0; ptCnt = 0;
    if (tempFilled < TEMP_BUCKETS) tempFilled++;
    // Persist automatically: the rings only change here, so this is the exact
    // moment there is something new to keep. Removes the "did I remember to
    // press save" failure entirely -- an unexpected power loss now costs at
    // most the bucket in progress.
    lbSave();
  }
}

void loop() {
  server.handleClient();
  dns.processNextRequest();
  tempTick();
  vTaskDelay(1);
}
