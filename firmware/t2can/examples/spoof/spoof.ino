/*
 * spoof — T-2CAN-FD input-spoof for the cabin light (phase-3 rehearsal).
 *
 * Mirrors the PDM1 digital-input frame (0x14EF111E, mux F0) so we always
 * copy-live-then-modify. On serial 'p' (or 'P'), impersonates the cabin wall
 * switch — confirmed slot on this van: byte 6 (7th payload byte), bits 0-1:
 *
 *   copy live F0 frame, set field to 0b10, send (press),
 *   wait 150 ms, send the cleared field (release).
 *
 * The HU's state machine sees the press and toggles the cabin itself. This is
 * ModeWifi's "input spoof" (§2 of docs/modewifi-analysis.md) validated live.
 *
 * Fixes baked in from docs/t2can-bench.md: 40 MHz crystal, __flgFDF = 0.
 */
#include <Arduino.h>
#include <SPI.h>
#include "mcp2518fd_can.h"
#include "pin_config.h"

mcp2518fd CanA(MCP2518_CS);

static const uint32_t ID_PDM1_STATUS = 0x14EF111EUL; // PDM1 -> HU, SA 0x1E
static const uint8_t  CABIN_BYTE     = 6;            // 7th payload byte, bits 0-1

static uint8_t lastF0[8] = {0xF0, 0xFF, 0xFF, 0xFF, 0xFF, 0xA4, 0x00, 0x00};
static bool    haveF0    = false;

void spoofCabin(bool pressed) {
  uint8_t buf[8];
  memcpy(buf, lastF0, 8);
  buf[0] = 0xF0;                  // mux = digital inputs 1..6
  buf[CABIN_BYTE] &= ~0x03;       // clear the cabin 2-bit slot
  if (pressed) buf[CABIN_BYTE] |= 0x02;  // 0b10 = pressed
  CanA.sendMsgBuf(ID_PDM1_STATUS, 1, 0, 8, buf);
  Serial.printf("spoof cabin %s: F0 %02X %02X %02X %02X %02X %02X %02X\n",
                pressed ? "PRESS " : "release",
                buf[1], buf[2], buf[3], buf[4], buf[5], buf[6], buf[7]);
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("spoof: boot");
  SPI.begin(MCP2518_SCLK, MCP2518_MISO, MCP2518_MOSI, MCP2518_CS);
  if (CanA.begin(CAN20_250KBPS, MCP2518FD_40MHz) != CAN_OK) {
    Serial.println("can A fd init fail");
  } else {
    Serial.println("can A fd init success (classic, 250k, 40MHz)");
  }
  CanA.__flgFDF = 0;
  Serial.println("send 'p' to press the cabin switch (press + release)");
}

void loop() {
  // mirror the live F0 frame so we always copy-live-then-modify
  if (CanA.checkReceive() == CAN_MSGAVAIL) {
    uint8_t len, buf[8];
    CanA.readMsgBuf(&len, buf);
    if (CanA.getCanId() == ID_PDM1_STATUS && len >= 8 && buf[0] == 0xF0) {
      memcpy(lastF0, buf, 8);
      haveF0 = true;
    }
  }
  if (Serial.available()) {
    char c = Serial.read();
    if (c == 'p' || c == 'P') {
      spoofCabin(true);
      delay(150);
      spoofCabin(false);
      Serial.println(haveF0 ? "spoofed against live F0" : "warning: no live F0 seen, used baseline");
    }
  }
}
