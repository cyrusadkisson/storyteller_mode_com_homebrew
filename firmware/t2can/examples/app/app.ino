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
#include <DNSServer.h>
#include <ESPmDNS.h>
#include "mcp2518fd_can.h"
#include "pin_config.h"
#include "driver/twai.h"

// ----- WiFi AP ---------------------------------------------------------------
// SET YOUR OWN CREDENTIALS BEFORE FLASHING. Anyone within radio range who has
// read this source can otherwise join and operate the van's lights, pump, A/C
// and vent. There is no screen or reset button on this board, so credentials
// are compiled in; pick something and write it down.
//
// KEEP REAL CREDENTIALS OUT OF THE REPOSITORY. Put them in ap_secret.h, which
// is git-ignored:
//
//     #define AP_SSID_OVERRIDE "YourSSID"
//     #define AP_PASS_OVERRIDE "YourPassword"
//
// Without that file the build uses the placeholders below and warns.
#if __has_include("ap_secret.h")
#include "ap_secret.h"
#endif
#ifndef AP_SSID_OVERRIDE
#define AP_SSID_OVERRIDE "VanCompanion"
#endif
#ifndef AP_PASS_OVERRIDE
#define AP_PASS_OVERRIDE "storyteller"
#warning "Using the placeholder AP password from the public repo -- create ap_secret.h."
#endif
static const char *AP_SSID = AP_SSID_OVERRIDE;
static const char *AP_PASS = AP_PASS_OVERRIDE;
static const char *MDNS_NAME = "van";         // http://van.local

// ----- board temperature history --------------------------------------------
// The ESP32-S3 has no RTC and millis() resets on every boot, so history is
// RELATIVE: 120 completed hourly buckets plus the hour in progress = 121 bars.
// Kept in RAM (~500 bytes) rather than flash -- the board is on permanent DC
// and only loses power on a full system shutdown, where losing history is
// acceptable and the alternative is thousands of flash write cycles.
//
// This reads the DIE, not the cavity: it runs above ambient by the chip's own
// dissipation plus the WiFi radio. Absolute accuracy is a few degrees; the
// useful signal is the shape over a day.
#define TEMP_BUCKETS   121          // 120 whole hours + the current one
#define TEMP_SAMPLE_MS 30000        // one reading per 30 s
#define TEMP_MIN_VALID 30           // >=15 min of samples or the hour is void
static bool     tempOK = true;   // Arduino's temperatureRead() needs no setup
static int16_t  tempHist[TEMP_BUCKETS];   // tenths of °C; INT16_MIN = no data
static float    tempAcc = 0;              // current hour accumulator
static uint16_t tempCnt = 0;
static uint32_t tempLastSample = 0;
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
    seenVent = true;
    // Report-only for SPEED: the fan reports 0 while idle, and writing that
    // back erased the slider's setpoint.
    // The LID is different -- adopt its actual position into our command word,
    // or a later speed/direction command sends the stale boot default (0x40 =
    // enabled, not open) and closes an open vent.
    ventRepSpeed = b[2]; ventRepMode = b[3];
    bool moving = (ventRepMode & 0x08);
    bool repOpen = (ventRepMode & 0x10);
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
      lidOpen = repOpen;
      airIn = (ventRepMode & 0x01);
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
  if (id == ID_AMBIENT) { seenAmb = true; ambC = raw2cF(b[1] | (b[2] << 8)); return; }
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
    seenBatt = true; battAt = millis();
  } else if (m.identifier == ID_DC3) {
    battSoH = d[2];
    battAh = d[3] | (d[4] << 8);
    seenBatt = true; battAt = millis();
  } else if (m.identifier == ID_INV_AC) {
    invAcAt = millis();
    invAcV = (d[1] | (d[2] << 8)) * 0.05f;
    invHz = (d[5] | (d[6] << 8)) / 128.0f;
    seenInv = true;
  }
}

// ----- JSON state ----------------------------------------------------------------
// 121 temperature buckets add ~600 bytes on top of the rest of the state, and
// the J() macro truncates silently once the buffer fills.
static char jbuf[3584];

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
    J("\"soc\":%.1f,\"battV\":%.2f,\"battA\":%.2f,\"battW\":%.0f,", soc, battV, battA, battW);
    J("\"packline\":\"Pack: %.1fV  %.0f°F  %uAh\",", battV, battT * 9 / 5 + 32, battAh);

    J("\"drawline\":\"%.1fA (%.0fW)\",", battA, battW);
    // The BMS sends 0xFFFF whenever it declines to estimate -- which it does
    // at low draw, exactly when the answer is most reassuring. We have amp
    // hours and amps, so compute it rather than showing nothing. Charging has
    // no "time left" to report.
    uint32_t mins = 0;
    if (battMin != 0xFFFF && battMin > 0) mins = battMin;
    else if (battA < -0.05f && battAh > 0) mins = (uint32_t)(battAh / -battA * 60.0f);
    if (mins > 0)
      J("\"lifeline\":\"   \u2022   %02ud %02uh\",",
        (unsigned)(mins / 1440), (unsigned)((mins % 1440) / 60));
    else if (battA > 0.05f)
      J("\"lifeline\":\"   \u2022   charging\",");
    else
      J("\"lifeline\":\"\",");
    J("\"batt\":\"\",");
  } else J("\"soc\":null,\"batt\":\"%s\","
           "\"packline\":\"\",\"drawline\":\"--\",\"lifeline\":\"\",",
           seenBatt ? "CAN2 battery data stale" : "no CAN2 battery frames");
  J("\"tempin\":%s,", seenAmb ? String(ambC * 9 / 5 + 32, 1).c_str() : "null");
  J("\"gfan\":%d,", doLevel[GALLEY_FAN_PDM][GALLEY_FAN_DO]);
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
    char vs[64];
    snprintf(vs, sizeof vs, "%s",
             lidState == LID_MOVING ? "moving"
                                    : (lidState == LID_OPEN ? "open" : "closed"));
    // vset = the setpoint (survives fan-off); vfan = commanded run state.
    // vrep is the raw reported speed, kept for diagnostics only -- it
    // oscillates to 0 while the fan runs and must not drive any UI state.
    J("\"ventst\":\"%s\",\"vspeed\":%d,\"vrep\":%d,\"vdir\":%d,"
      "\"vopen\":%d,\"vmoving\":%d,\"vset\":%d,\"vfan\":%d,"
      "\"vown\":%d,\"vobs\":%d,\"fansp\":\"%s\",",
      vs, ventSpeed, ventRepSpeed, airIn ? 1 : 0,
      (lidState == LID_OPEN) ? 1 : 0, (lidState == LID_MOVING) ? 1 : 0,
      ventSetSpeed, ventFanOn ? 1 : 0,
      (ventCmdAt && (millis() - ventCmdAt < VENT_OWN_MS)) ? 1 : 0, ventObsSpeed,
      ventFanOn ? (airIn ? "air in" : "air out") : "off");
  } else J("\"ventst\":\"?\",\"vspeed\":0,\"vrep\":0,\"vdir\":0,"
           "\"vopen\":0,\"vmoving\":0,\"vset\":0,\"vfan\":0,"
           "\"vown\":0,\"vobs\":0,\"fansp\":\"\",");

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
    // Temperature history: 121 buckets, oldest first, tenths of °F.
    // null marks an hour with too few samples or no data at all.
    J("\"temphist\":[");
    for (int i = 0; i < TEMP_BUCKETS; i++) {
      int16_t v = tempHist[i];
      if (i == TEMP_BUCKETS - 1 && tempCnt >= TEMP_MIN_VALID)
        v = (int16_t)(tempAcc / tempCnt * 10.0f);      // hour in progress
      if (v == INT16_MIN) J("%snull", i ? "," : "");
      else J("%s%.0f", i ? "," : "", v / 10.0f * 9.0f / 5.0f + 32.0f);
    }
    J("],\"tempnow\":%.1f,\"tempfill\":%u,",
      temperatureRead() * 9.0f / 5.0f + 32.0f, tempFilled);
  }
  {
    char ft[200];
    snprintf(ft, sizeof ft, "CAN1 %lu  CAN2 %lu (%lus ago)  |  last: %s%s%s",
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
  snprintf(lastCmd, sizeof lastCmd, "%s @ %lus", what, (unsigned long)(millis() / 1000));
  Serial.printf("cmd: %s\n", what);
  server.send(200, "application/json", "{\"ok\":true}");
}

void onCmd() {
  String c = server.arg("c");
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

  for (int i = 0; i < TEMP_BUCKETS; i++) tempHist[i] = INT16_MIN;
  Serial.printf("temp sensor: %.1f C\n", temperatureRead());
  tempHourStart = millis();
  tempLastSample = millis();

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
    server.sendHeader("Cache-Control", "no-store");
    sendState();
  });
  server.on("/api/cmd", HTTP_POST, onCmd);
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
// have to see those frames promptly to mirror state. Sharing the
// Arduino loop with the web server made injections land late or not at all,
// and the light averaged between our level and the HU's -> visible flicker.
// The CAN work therefore owns core 0; the web server keeps core 1.
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

// Roll the accumulator into a bucket every hour. All timing uses unsigned
// subtraction so the 49.7-day millis() rollover is handled -- this box can run
// for months, and a naive comparison would corrupt the history at ~7 weeks.
void tempTick() {
  if (!tempOK) return;
  uint32_t now = millis();
  if ((uint32_t)(now - tempLastSample) >= TEMP_SAMPLE_MS) {
    tempLastSample = now;
    float c = temperatureRead();          // °C, ESP32-S3 internal die sensor
    if (c > -40.0f && c < 125.0f) {       // ignore the out-of-range error value
      tempAcc += c;
      tempCnt++;
    }
  }
  if ((uint32_t)(now - tempHourStart) >= 3600000UL) {
    tempHourStart += 3600000UL;
    // Shift the ring left; the newest completed hour lands at the end.
    for (int i = 0; i < TEMP_BUCKETS - 1; i++) tempHist[i] = tempHist[i + 1];
    tempHist[TEMP_BUCKETS - 1] =
        (tempCnt >= TEMP_MIN_VALID) ? (int16_t)(tempAcc / tempCnt * 10.0f)
                                    : INT16_MIN;      // too few samples: void
    tempAcc = 0; tempCnt = 0;
    if (tempFilled < TEMP_BUCKETS) tempFilled++;
  }
}

void loop() {
  server.handleClient();
  dns.processNextRequest();
  tempTick();
  vTaskDelay(1);
}
