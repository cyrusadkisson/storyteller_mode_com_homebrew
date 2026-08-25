/*
 * twobus.ino — the two-bus core: CAN-A (MCP2518FD) on CAN1 + CAN-B (TWAI) on
 * CAN2 running in ONE binary. Proves both van buses read simultaneously and
 * decodes the battery state of charge / voltage from CAN2.
 *
 *   CAN-A: MCP2518FD @ 250k classic, 40 MHz crystal, FDF off (control bus).
 *   CAN-B: ESP32 TWAI, listen-only @ 250k (CAN2 is read-only for us forever).
 *
 * Battery decode, verified in docs/energy-can2.md:
 *   0x19FFFD46 bytes 2-3 = DC voltage x 0.05 V
 *              bytes 4-7 = DC current, 32-bit LE, 1 mA, offset -2 000 000 000,
 *                          positive = discharging (negate to match the panel)
 *   0x19FFFC46 byte 4    = state of charge x 0.5 %
 */
#include <Arduino.h>
#include <SPI.h>
#include "mcp2518fd_can.h"
#include "pin_config.h"
#include "driver/twai.h"

#define CANB_TX GPIO_NUM_7
#define CANB_RX GPIO_NUM_6

mcp2518fd CanA(MCP2518_CS);

static const uint32_t ID_DC1 = 0x19FFFD46UL; // DC_SOURCE_STATUS_1 (voltage/current)
static const uint32_t ID_DC2 = 0x19FFFC46UL;  // DC_SOURCE_STATUS_2 (SoC, temp)

static uint32_t cntA = 0, cntB = 0;
static float    lastV  = 0.0f;
static int32_t  lastmA = 0;
static uint8_t  lastSoC = 0;
static bool     seenBatt = false;

void printBattery() {
  Serial.printf("BATT SoC %u.%u%%, %.2f V, %+.2f A (pos=discharge)\n",
                lastSoC / 2, (lastSoC % 2) * 5,
                lastV,
                (float)lastmA / 1000.0f);
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("twobus: boot");

  SPI.begin(MCP2518_SCLK, MCP2518_MISO, MCP2518_MOSI, MCP2518_CS);
  if (CanA.begin(CAN20_250KBPS, MCP2518FD_40MHz) != CAN_OK) {
    Serial.println("canA: init FAIL");
  } else {
    Serial.println("canA: CAN1 @ 250k classic OK");
  }
  CanA.__flgFDF = 0;

  twai_general_config_t g = TWAI_GENERAL_CONFIG_DEFAULT(CANB_TX, CANB_RX, TWAI_MODE_LISTEN_ONLY);
  twai_timing_config_t t = TWAI_TIMING_CONFIG_250KBITS();
  twai_filter_config_t f = TWAI_FILTER_CONFIG_ACCEPT_ALL();
  if (twai_driver_install(&g, &t, &f) == ESP_OK && twai_start() == ESP_OK) {
    Serial.println("canB: CAN2 (TWAI listen-only) OK");
  } else {
    Serial.println("canB: init FAIL");
  }
  Serial.println("twobus: listening both buses (battery line every ~2 s)");
}

void loop() {
  // --- CAN-A / CAN1 (read) ---
  if (CanA.checkReceive() == CAN_MSGAVAIL) {
    uint8_t len, buf[8];
    CanA.readMsgBuf(&len, buf);
    cntA++;
  }

  // --- CAN-B / CAN2 (read-only) ---
  twai_message_t msg;
  if (twai_receive(&msg, 0) == ESP_OK) {
    cntB++;
    if (msg.extd && msg.data_length_code >= 8) {
      if (msg.identifier == ID_DC1) {
        lastV = (float)((msg.data[2] | (msg.data[3] << 8))) * 0.05f;
        int32_t raw = msg.data[4] | (msg.data[5] << 8) | (msg.data[6] << 16) | ((uint32_t)msg.data[7] << 24);
        lastmA = raw - 2000000000;                 // mA, positive = discharge
        seenBatt = true;
      } else if (msg.identifier == ID_DC2) {
        lastSoC = msg.data[4];                      // x 0.5 %
        seenBatt = true;
      }
    }
  }

  // --- periodic summary every ~2 s ---
  static uint32_t lastTick = 0;
  if (millis() - lastTick >= 2000) {
    lastTick = millis();
    Serial.printf("chk A=%lu B=%lu", (unsigned long)cntA, (unsigned long)cntB);
    if (seenBatt) printBattery(); else Serial.println("  (no battery frame yet)");
  }
}
