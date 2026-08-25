/*
 * listen — T-2CAN-FD as a passive observer of the van's CAN1.
 *
 * Boots CAN-A at the van's speed (250 kbit/s classic, 40 MHz crystal, FDF off)
 * and dumps every received frame to USB serial in candump-like shape.
 * Transmits NOTHING — this is the first thing that ever goes on the real bus.
 *
 * Cross-check its output against the Jhoinrch's candump of the same bus: both
 * should show the same frames. Also catches whether the T-2CAN-FD's tap into
 * the van is electrically sound before any injection is ever attempted.
 */
#include <Arduino.h>
#include <SPI.h>
#include "mcp2518fd_can.h"
#include "pin_config.h"

mcp2518fd CanA(MCP2518_CS);

void setup() {
  Serial.begin(115200);
  Serial.println("listen: firmware ready");

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
  if (CanA.checkReceive() == CAN_MSGAVAIL) {
    uint8_t len;
    uint8_t buf[8];
    CanA.readMsgBuf(&len, buf);
    uint32_t id = CanA.getCanId();
    Serial.printf("RX %08X%c [%d]", id, CanA.isExtendedFrame() ? 'E' : ' ', len);
    for (uint8_t i = 0; i < len; i++) Serial.printf(" %02X", buf[i]);
    Serial.println();
  }
}
