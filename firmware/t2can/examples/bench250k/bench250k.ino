/*
 * bench250k — T-2CAN-FD bench test at the van's actual bus speed.
 *
 * Goal: prove the T-2CAN-FD's CAN-A channel can emit the van's real
 * J1939/RV-C + CANPro frames at 250 kbit/s classic, AND that the Jhoinrch
 * CANable receives them (and vice versa). This is the bench rehearsal for the
 * van integration test — we're not on the van, we're on jumper wires.
 *
 * Uses the fixes from docs/t2can-bench.md:
 *   - MCP2518FD has a 40 MHz crystal -> pass MCP2518FD_40MHz
 *   - Longan_CANFD always sets the FD-format flag -> CanA.__flgFDF = 0
 */
#include <Arduino.h>
#include <SPI.h>
#include "mcp2518fd_can.h"
#include "pin_config.h"

mcp2518fd CanA(MCP2518_CS);

// The van's known IDs. We send them over the bench link to the Jhoinrch.
static const uint32_t ID_PDM1_CMD   = 0x14EF1E11UL;  // HU -> PDM1 command
static const uint32_t ID_PDM2_CMD   = 0x14EF1F11UL;  // HU -> PDM2 command
static const uint32_t ID_PDM1_STATUS= 0x14EF111EUL;  // PDM1 -> HU status

// A valid PDM command: mux FC = DO1..DO6, 0x00 = OFF, 0x7F = full.
// Here: turn cabin lights off (byte index == DO number; DO4 is byte 4).
// FC DO1 DO2 DO3 DO4 DO5 DO6 0xFF
static const uint8_t PDM_CABIN_OFF[8] = {0xFC, 0x00, 0x00, 0x00, 0x00, 0x7F, 0x00, 0xFF};
static const uint8_t PDM_CABIN_ON [8] = {0xFC, 0x00, 0x00, 0x00, 0x7F, 0x00, 0x00, 0xFF};

void setup() {
  Serial.begin(115200);
  Serial.println("bench250k: boot");

  SPI.begin(MCP2518_SCLK, MCP2518_MISO, MCP2518_MOSI, MCP2518_CS);

  // The two bench fixes from docs/t2can-bench.md
  if (CanA.begin(CAN20_250KBPS, MCP2518FD_40MHz) != CAN_OK) {
    Serial.println("can A fd init fail");
  } else {
    Serial.println("can A fd init success (classic, 250k, 40MHz)");
  }
  CanA.__flgFDF = 0;
}

void loop() {
  // 1) Send the PDM command frames at 250k so the Jhoinrch can decode them.
  CanA.sendMsgBuf(ID_PDM1_CMD, 1, 0, 8, (byte*)PDM_CABIN_ON);
  Serial.println("TX: cabin on (ID 0x14EF1E11, FC ... 0x7F)");
  delay(1500);

  CanA.sendMsgBuf(ID_PDM1_CMD, 1, 0, 8, (byte*)PDM_CABIN_OFF);
  Serial.println("TX: cabin off (ID 0x14EF1E11, FC ... 0x00)");
  delay(1500);

  CanA.sendMsgBuf(ID_PDM2_CMD, 1, 0, 8, (byte*)PDM_CABIN_ON);  // just an ID exercise
  Serial.println("TX: PDM2 frame (ID 0x14EF1F11)");
  delay(1500);

  CanA.sendMsgBuf(ID_PDM1_STATUS, 1, 0, 8, (byte*)PDM_CABIN_OFF);  // a status ID exercise
  Serial.println("TX: PDM1 status frame (ID 0x14EF111E)");
  delay(1500);

  // 2) Listen for anything on the wire (e.g. the Jhoinrch transmitting).
  if (CanA.checkReceive() == CAN_MSGAVAIL) {
    uint8_t len;
    uint8_t buf[8];
    CanA.readMsgBuf(&len, buf);
    uint32_t id = CanA.getCanId();
    Serial.print("can A fd received data\n");
    Serial.printf("can A fd receive id: 0x%X\n", id);
    Serial.printf("can A fd receive data length: %d\n", len);
  }
}
