/*
 * companion.ino — the evolving T-2CAN-FD companion firmware.
 *
 * Two-bus core + input-spoof control of the four verified wall switches.
 *
 *   CAN-A (MCP2518FD): CAN1 @ 250k classic — mirrors the live F0/F8 digital-
 *     input frames; on a serial command, impersonates a verified wall-switch
 *     press so the HU toggles the load itself (input spoof, §2 of
 *     docs/modewifi-analysis.md — validated live on cabin 2026-08-24).
 *   CAN-B (TWAI): CAN2 @ 250k listen-only — battery SoC/voltage decode
 *     (docs/energy-can2.md).
 *
 * Verified switches on this van (2026-08-24):
 *   1 cabin      PDM1 F0  byte6 slot0  press 0x02
 *   2 garage     PDM1 F0  byte6 slot1  press 0x08
 *   3 water pump PDM2 F0  byte7 slot3  press 0x80
 *   4 sink drain PDM2 F8  byte6 slot1  press 0x08  (momentary/hold)
 */
#include <Arduino.h>
#include <SPI.h>
#include "mcp2518fd_can.h"
#include "pin_config.h"
#include "driver/twai.h"

#define CANB_TX GPIO_NUM_7
#define CANB_RX GPIO_NUM_6

mcp2518fd CanA(MCP2518_CS);

static const uint32_t ID_PDM1_STAT = 0x14EF111EUL; // PDM1 digital-input status
static const uint32_t ID_PDM2_STAT = 0x14EF111FUL; // PDM2
static const uint32_t ID_DC1 = 0x19FFFD46UL;       // DC_SOURCE_STATUS_1
static const uint32_t ID_DC2 = 0x19FFFC46UL;         // DC_SOURCE_STATUS_2

// A verified switch: PDM (0/1), mux (0=F0,1=F8), payload byte, bit shift,
// hold ms, and whether it's hold-to-run (must sustain the pressed field on the
// bus, because the PDU re-broadcasts its own cleared field at ~25 Hz).
struct Sw { uint8_t pdm; uint8_t mux; uint8_t byte; uint8_t shift; uint16_t hold; bool holdrun; };
static const Sw SW[] = {
  {0, 0, 6, 0, 150, false},  // cabin (latching — one pulse toggles)
  {0, 0, 6, 2, 150, false},  // garage
  {1, 0, 7, 6, 150, false},  // water pump
  {1, 1, 6, 2, 1000, true},  // sink drain — hold-to-run, ~1 s, don't run dry
  {1, 0, 6, 0, 150, false},  // aux — exterior perimeter lighting
  {0, 0, 7, 4, 150, false},  // recirc — momentary; HU runs ~10 s auto-cycle
  {0, 0, 7, 6, 150, false},  // awning light — toggle (verified w/o awning deployed)
};
static const uint8_t MUX[2] = {0xF0, 0xF8};
static const char *SWNAME[7] = {"cabin", "garage", "water pump", "sink drain", "aux", "recirc", "awning light"};

static uint8_t live[2][2][8];      // [pdm][mux]
static bool    liveSeen[2][2];

static uint32_t cntA = 0, cntB = 0;
static float   lastV = 0.0f;
static int32_t lastmA = 0;
static uint8_t lastSoC = 0;
static bool    seenBatt = false;

static uint32_t idOf(uint8_t pdm) { return pdm ? ID_PDM2_STAT : ID_PDM1_STAT; }

void spoofPress(uint8_t i) {
  const Sw &s = SW[i];
  if (!liveSeen[s.pdm][s.mux]) {
    Serial.printf("spoof %s: no live frame yet, using defaults\n", SWNAME[i]);
  }
  uint8_t buf[8];
  memcpy(buf, live[s.pdm][s.mux], 8);
  buf[0] = MUX[s.mux];
  uint8_t clearM = 0x03 << s.shift;
  uint8_t press  = 0x02 << s.shift;
  uint32_t id = idOf(s.pdm);

  uint8_t pressBuf[8];
  memcpy(pressBuf, buf, 8);
  pressBuf[s.byte] = (pressBuf[s.byte] & ~clearM) | press;   // pressed

  if (s.holdrun) {
    // hold-to-run: re-send the pressed field throughout the hold, otherwise
    // the PDU's own ~25 Hz cleared re-broadcast masks it after a few ms
    uint32_t t0 = millis();
    while (millis() - t0 < s.hold) {
      CanA.sendMsgBuf(id, 1, 0, 8, pressBuf);
      delay(20);
    }
  } else {
    CanA.sendMsgBuf(id, 1, 0, 8, pressBuf);
    delay(s.hold);
  }
  Serial.printf("press %s (%s): %02X%02X %02X%02X %02X%02X%02X%02X\n", SWNAME[i],
                (s.pdm ? "PDM2" : "PDM1"), pressBuf[0], pressBuf[1], pressBuf[2], pressBuf[3],
                pressBuf[4], pressBuf[5], pressBuf[6], pressBuf[7]);
  buf[s.byte] &= ~clearM;                            // release
  CanA.sendMsgBuf(id, 1, 0, 8, buf);
  Serial.println("  release");
}

// Vent fan control — 0x19FEA603 (documented in docs/climate-control.md).
// 02 15 <speed> <mode>: speed 0-255 (0=off), mode = bit6 enable(always) +
// bit4 vent open + bit0 air IN. Status on 0x19FEA758.
void sendVent(uint8_t speed, uint8_t mode) {
  uint8_t v[8] = {0x02, 0x15, speed, mode, 0x00, 0x00, 0x00, 0x00};
  CanA.sendMsgBuf(0x19FEA603UL, 1, 0, 8, v);
}

// --- A/C thermostat 0x19FEF903 (docs/climate-control.md) ---
// 01 <b1> 64 <heat LE> <cool LE> 00 ; heat/cool setpoint raw = (C+273)*32
static uint16_t heatSetRaw = 0x252F;
static uint16_t coolSetRaw = 0x2540;
static uint16_t f2acRaw(uint8_t f) { return (uint16_t)((((f - 32) * 5 / 9) + 273) * 32); }
static uint16_t f2rixRaw(uint8_t f) { return (uint16_t)(((f - 32) * 5 / 9) * 10); } // Rixen: x0.1 C

void sendAC(uint8_t b1) {
  uint8_t ac[8] = {0x01, b1, 0x64,
                   (uint8_t)heatSetRaw, (uint8_t)(heatSetRaw >> 8),
                   (uint8_t)coolSetRaw, (uint8_t)(coolSetRaw >> 8), 0x00};
  CanA.sendMsgBuf(0x19FEF903UL, 1, 0, 8, ac);
}

// Rixen heater 0x788 (standard 11-bit): byte0 = mux, payload from byte1
void sendRixen(uint8_t mux, uint8_t a, uint8_t b) {
  uint8_t r[8] = {mux, a, b, 0x00, 0x00, 0x00, 0x00, 0x00};
  CanA.sendMsgBuf(0x788UL, 0, 0, 8, r);
}

// read a decimal number over serial (2 s window); 0 if none
uint16_t readNum() {
  uint16_t v = 0; bool got = false; uint32_t t0 = millis();
  while (millis() - t0 < 2000) {
    if (Serial.available()) {
      char ch = Serial.read();
      if (ch >= '0' && ch <= '9') { v = v * 10 + (ch - '0'); got = true; }
      else if (got) break;
    }
  }
  return v;
}

// water tank levels from 0x19FFB7AF (fresh instance 0, gray instance 2)
static uint8_t fwL = 0, fwR = 1, grL = 0, grR = 1;

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("companion: boot");

  SPI.begin(MCP2518_SCLK, MCP2518_MISO, MCP2518_MOSI, MCP2518_CS);
  if (CanA.begin(CAN20_250KBPS, MCP2518FD_40MHz) != CAN_OK) {
    Serial.println("canA: init FAIL");
  } else {
    Serial.println("canA: CAN1 @ 250k classic OK");
  }
  CanA.__flgFDF = 0;

  twai_general_config_t g = TWAI_GENERAL_CONFIG_DEFAULT(CANB_TX, CANB_RX, TWAI_MODE_NORMAL);
  twai_timing_config_t t = TWAI_TIMING_CONFIG_250KBITS();
  twai_filter_config_t f = TWAI_FILTER_CONFIG_ACCEPT_ALL();
  if (twai_driver_install(&g, &t, &f) == ESP_OK && twai_start() == ESP_OK) {
    Serial.println("canB: CAN2 (TWAI listen-only) OK");
  } else {
    Serial.println("canB: init FAIL");
  }
  Serial.println("cmds: 1-7 sw 8 read 9/0 AC c comp  i/j inv  v vent  h<T> heat  T<T> cool  H heatmode  r<T> rixen  f furn  w water  b batt");
}

void loop() {
  // CAN-A: mirror the live PDU digital-input frames (and count)
  if (CanA.checkReceive() == CAN_MSGAVAIL) {
    uint8_t len, buf[8];
    CanA.readMsgBuf(&len, buf);
    cntA++;
    if (len >= 8 && (CanA.getCanId() == ID_PDM1_STAT || CanA.getCanId() == ID_PDM2_STAT)) {
      uint8_t pdm = (CanA.getCanId() == ID_PDM2_STAT) ? 1 : 0;
      if (buf[0] == 0xF0)      { memcpy(live[pdm][0], buf, 8); liveSeen[pdm][0] = true; }
      else if (buf[0] == 0xF8) { memcpy(live[pdm][1], buf, 8); liveSeen[pdm][1] = true; }
    }
    // water tank levels — 0x19FFB7AF, byte0 = tank instance (0 fresh, 2 gray)
    if (CanA.getCanId() == 0x19FFB7AFUL && len >= 3) {
      if (buf[0] == 0x00)      { fwL = buf[1]; fwR = buf[2]; }
      else if (buf[0] == 0x02) { grL = buf[1]; grR = buf[2]; }
    }
  }

  // CAN-B: CAN2 (read-only)
  twai_message_t msg;
  if (twai_receive(&msg, 0) == ESP_OK) {
    cntB++;
    if (msg.extd && msg.data_length_code >= 8) {
      if (msg.identifier == ID_DC1) {
        lastV = (float)(msg.data[2] | (msg.data[3] << 8)) * 0.05f;
        int32_t raw = msg.data[4] | (msg.data[5] << 8) | (msg.data[6] << 16) | ((uint32_t)msg.data[7] << 24);
        lastmA = raw - 2000000000;
        seenBatt = true;
      } else if (msg.identifier == ID_DC2) {
        lastSoC = msg.data[4];
        seenBatt = true;
      }
    }
  }

  // serial commands
  if (Serial.available()) {
    char c = Serial.read();
    if (c >= '1' && c <= '7') spoofPress(c - '1');
    else if (c == '8') {
      // direct write: reading lights DO3 = 0x26 for ~800 ms, then off.
      // Copy of the exact FC frame the HU uses; the HU's re-broadcast will
      // likely overwrite it in ~11 ms (the drain-style wall) — test anyway.
      uint8_t on[8]  = {0xFC, 0x7F, 0x00, 0x26, 0x00, 0x00, 0x00, 0xFF};
      uint8_t off[8] = {0xFC, 0x7F, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF};
      CanA.sendMsgBuf(0x14EF1E11UL, 1, 0, 8, on);
      Serial.println("direct read: DO3=0x26");
      delay(800);
      CanA.sendMsgBuf(0x14EF1E11UL, 1, 0, 8, off);
      Serial.println("direct read: off");
    }
    else if (c == 'b' || c == 'B') {
      if (seenBatt) Serial.printf("SoC %u.%u%%, %.2f V, %+.2f A\n", lastSoC/2,(lastSoC%2)*5,lastV,(float)lastmA/1000.0f);
      else Serial.println("no battery frame yet");
    }
    else if (c == '9' || c == '0') {
      sendAC(c == '9' ? 0x01 : 0x00);       // A/C on / off
      Serial.println(c == '9' ? "A/C ON" : "A/C OFF");
    }
    else if (c == 'c') {
      sendAC(0x04);                          // compressor AC OFF
      Serial.println("compressor AC OFF");
    }
    else if (c == 'h') { uint16_t f = readNum(); heatSetRaw = f2acRaw((uint8_t)f); sendAC(0x01); Serial.printf("heat setpoint %u F\n", (unsigned)f); }
    else if (c == 'T') { uint16_t f = readNum(); coolSetRaw = f2acRaw((uint8_t)f); sendAC(0x01); Serial.printf("cool setpoint %u F\n", (unsigned)f); }
    else if (c == 'H') { sendAC(0x02); Serial.println("A/C heat mode"); }
    else if (c == 'r') { uint16_t f = readNum(); uint16_t raw = f2rixRaw((uint8_t)f); sendRixen(0x01, raw & 0xFF, raw >> 8); Serial.printf("Rixen target %u F\n", (unsigned)f); }
    else if (c == 'f') { sendRixen(0x03, 0x01, 0x00); Serial.println("Rixen furnace on"); }
    else if (c == 'w') {
      Serial.printf("water: fresh %u/%u=%u%%, gray %u/%u=%u%%\n",
        fwL, fwR, fwR ? fwL * 100 / fwR : 0,
        grL, grR, grR ? grL * 100 / grR : 0);
    }
    else if (c == 'i' || c == 'j') {
      // inverter on/off — 0x19FFD3F2, byte1 01/00, single-shot latch (CAN2)
      twai_message_t m = {0};
      m.identifier = 0x19FFD3F2UL;
      m.extd = 1;
      m.data_length_code = 8;
      m.data[0] = 0x01;
      m.data[1] = (c == 'i') ? 0x01 : 0x00;
      esp_err_t r = twai_transmit(&m, pdMS_TO_TICKS(500));
      twai_status_info_t st;
      twai_get_status_info(&st);
      Serial.printf("inverter %s r=%d state=%d txerr=%u txfail=%u\n",
                    (c == 'i' ? "ON" : "OFF"), r, st.state, st.tx_error_counter, st.tx_failed_count);
    }
    else if (c == 'v') {
      // Replay the live panel vent sequence: open, fan low, max, airflow in,
      // low, off, close. (0x19FEA603, decoded 2026-08-24.)
      Serial.println("vent: open / fan-low / max / airflow-in / low / off / close");
      sendVent(0x00, 0x51); delay(1500);
      sendVent(0x2B, 0x51); delay(1500);
      sendVent(0xC8, 0x51); delay(1500);
      sendVent(0xC8, 0x50); delay(1500);
      sendVent(0x37, 0x50); delay(1500);
      sendVent(0x00, 0x50); delay(1500);
      sendVent(0x00, 0x40);
      Serial.println("vent: sequence done");
    }
  }

  // periodic summary
  static uint32_t lastTick = 0;
  if (millis() - lastTick >= 2000) {
    lastTick = millis();
    Serial.printf("chk A=%lu B=%lu", (unsigned long)cntA, (unsigned long)cntB);
    if (seenBatt) Serial.printf("  SoC %u.%u%% %.2fV", lastSoC/2,(lastSoC%2)*5, lastV);
    Serial.println();
  }
}
