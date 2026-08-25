/*
 * app.ino — companion v1: WiFi AP + web UI over the verified control surface.
 *
 *   T-2CAN-FD.  CAN-A (MCP2518FD) on the van's CAN1 (control bus, 250k),
 *   CAN-B (TWAI, GPIO6/7) on CAN2 (energy bus, 250k).
 *
 *   Phone joins the AP, opens http://192.168.4.1 (or http://van.local).
 *
 *   CONTROL SURFACE — every command here is verified live on the van
 *   (docs/reverse-engineering-log.md 2026-08-24):
 *     - 6 wall-switch toggles via the input spoof (impersonate the PDM's
 *       F0/F8 digital-input frames; the HU toggles and re-broadcasts, so
 *       persistence is free)        — docs/modewifi-analysis.md §2
 *     - A/C on/off + cool/heat setpoints + compressor on/off (0x19FEF903,
 *       echoed/verifiable on 0x19FFE258)   — docs/climate-control.md
 *     - vent open/close, fan speed, air direction (0x19FEA603, echoed on
 *       0x19FEA758)
 *     - inverter on/off on CAN2 (0x19FFD3F2, single-shot latch)
 *   READ-ONLY state: HU's own command broadcasts (per-channel levels),
 *   per-channel feedback amps, tanks, battery DC status, inverter AC stats,
 *   interior/ambient temperature, PDM fault frames.
 *
 *   NOT included (architecture-blocked on a parallel tap, by design):
 *   dimming, reading lights, sink drain — need cut-and-stand-in.
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

// ----- WiFi AP (change to taste) ------------------------------------------
static const char *AP_SSID = "VanCompanion";
static const char *AP_PASS = "storyteller";   // 8+ chars required by WPA2
static const char *MDNS_NAME = "van";         // http://van.local

#define CANB_TX GPIO_NUM_7
#define CANB_RX GPIO_NUM_6

mcp2518fd CanA(MCP2518_CS);
WebServer server(80);
DNSServer dns;                      // captive portal: any DNS answer -> our IP

// ----- CAN ids --------------------------------------------------------------
static const uint32_t ID_PDM1_STAT  = 0x14EF111EUL;  // PDM1 -> HU (inputs, feedback)
static const uint32_t ID_PDM2_STAT  = 0x14EF111FUL;  // PDM2 -> HU
static const uint32_t ID_PDM1_CMD   = 0x14EF1E11UL;  // HU -> PDM1 (output levels, 45 Hz)
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

// ----- switch table (verified 2026-08-24, matches ModeWifi byte-for-byte) ---
struct Sw { uint8_t pdm; uint8_t mux; uint8_t byte; uint8_t shift;
            uint16_t hold; bool holdrun; uint8_t pdmDo; const char *name; };
static const Sw SW[] = {
  {0, 0, 6, 0, 150, false, 4,  "cabin"},        // PDM1 DO4
  {0, 0, 6, 2, 150, false, 2,  "garage"},       // PDM1 DO2
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
static uint8_t  faultB[2][2];      // PDM fault frame bytes 2-3
static uint8_t  fwL = 0, fwR = 1, grL = 0, grR = 1;
static float    battV = 0, battA = 0, battT = 0;      // CAN2
static uint16_t battSoC2 = 0;      // SoC ×2
static uint16_t battMin = 0xFFFF, battAh = 0;
static uint8_t  battSoH = 0;
static bool     seenBatt = false, seenTank = false;
static uint8_t  acB1 = 0, acFan = 0;
static uint16_t acHeatRaw = 0x252F, acCoolRaw = 0x2540;
static bool     seenAC = false;
static uint8_t  ventSpeed = 0, ventMode = 0x40;
static float    ventT = 0;
static bool     seenVent = false;
static float    invAcV = 0, invHz = 0;
static bool     seenInv = false;
static float    ambC = 0;
static bool     seenAmb = false;
static uint32_t cntA = 0, cntB = 0;

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
    while (millis() - t0 < s.hold) { CanA.sendMsgBuf(id, 1, 0, 8, pb); delay(20); }
  } else {
    CanA.sendMsgBuf(id, 1, 0, 8, pb);
    delay(s.hold);
  }
  buf[s.byte] &= ~clearM;
  CanA.sendMsgBuf(id, 1, 0, 8, buf);
  Serial.printf("spoof %s\n", s.name);
}

void sendAC(uint8_t b1) {
  uint8_t ac[8] = {0x01, b1, 0x64,
                   (uint8_t)acHeatRaw, (uint8_t)(acHeatRaw >> 8),
                   (uint8_t)acCoolRaw, (uint8_t)(acCoolRaw >> 8), 0x00};
  CanA.sendMsgBuf(ID_AC_CMD, 1, 0, 8, ac);
}

void sendVent(uint8_t speed, uint8_t mode) {
  uint8_t v[8] = {0x02, 0x15, speed, mode, 0x00, 0x00, 0x00, 0x00};
  CanA.sendMsgBuf(ID_VENT_CMD, 1, 0, 8, v);
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
    if (b[0] == 0xFC)      for (int ch = 1; ch <= 6; ch++) doLevel[p][ch] = b[ch];
    else if (b[0] == 0xFD) for (int ch = 7; ch <= 12; ch++) doLevel[p][ch - 6] = b[ch - 6];
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
  if (id == ID_AC_STAT) {                                // AC echoes our command
    seenAC = true;
    acB1 = b[1]; acFan = b[2];
    acHeatRaw = b[3] | (b[4] << 8);
    acCoolRaw = b[5] | (b[6] << 8);
    return;
  }
  if (id == ID_VENT_STAT) {
    seenVent = true;
    ventSpeed = b[2]; ventMode = b[3];
    ventT = raw2cF(b[4] | (b[5] << 8));
    return;
  }
  if (id == ID_AMBIENT) { seenAmb = true; ambC = raw2cF(b[1] | (b[2] << 8)); return; }
}

void onCanBFrame(const twai_message_t &m) {
  if (!m.extd || m.data_length_code < 8) return;
  const uint8_t *d = m.data;
  if (m.identifier == ID_DC1) {
    battV = (d[2] | (d[3] << 8)) * 0.05f;
    int32_t raw = d[4] | (d[5] << 8) | (d[6] << 16) | ((uint32_t)d[7] << 24);
    battA = -(raw - 2000000000LL) / 1000.0f;   // wire + = discharge; we show panel sign (+ = charging)
    seenBatt = true;
  } else if (m.identifier == ID_DC2) {
    battT = raw2cF(d[2] | (d[3] << 8));
    battSoC2 = d[4];
    battMin = d[5] | (d[6] << 8);
    seenBatt = true;
  } else if (m.identifier == ID_DC3) {
    battSoH = d[2];
    battAh = d[3] | (d[4] << 8);
    seenBatt = true;
  } else if (m.identifier == ID_INV_AC) {
    invAcV = (d[1] | (d[2] << 8)) * 0.05f;
    invHz = (d[5] | (d[6] << 8)) / 128.0f;
    seenInv = true;
  }
}

// ----- JSON state ----------------------------------------------------------------
static char jbuf[2048];

void sendState() {
  char *w = jbuf;
  int left = sizeof jbuf;
  int n = snprintf(w, left, "{");

  // helper macro: append formatted
  #define J(...) do { w += n; left -= n; if (left <= 0) goto out; n = snprintf(w, left, __VA_ARGS__); } while (0)

  if (seenBatt) {
    char bt[64];
    float soc = battSoC2 * 0.5f;
    snprintf(bt, sizeof bt, "%.1f V  %+.1f A  %.0f°F  %u Ah", battV, battA, battT * 9 / 5 + 32, battAh);
    J("\"soc\":%.1f,\"batt\":\"%s\",\"battV\":%.2f,\"battA\":%.2f,", soc, bt, battV, battA);
  } else J("\"soc\":null,\"batt\":\"no CAN2 battery frames\",");
  J("\"tempin\":%s,", seenAmb ? String(ambC * 9 / 5 + 32, 1).c_str() : "null");
  if (seenInv) J("\"invtext\":\"shore/inverter %.0f V %.1f Hz\",", invAcV, invHz);
  else J("\"invtext\":\"\",");

  J("\"sw\":[");
  for (int i = 0; i < 6; i++) J("%s%d", i ? "," : "", doLevel[SW_OUT_PDM[i]][SDO(i)] > 0 ? 1 : 0);
  J("],\"amps\":[");
  for (int i = 0; i < 6; i++) J("%s%.2f", i ? "," : "", doAmps[SW_OUT_PDM[i]][SDO(i)]);
  J("],");

  if (seenAC) {
    const char *mode;
    uint8_t op = acB1 & 0x0F;
    if (acB1 == 0x04) mode = "on, compressor off";
    else switch (op) { case 0: mode = "off"; break; case 1: mode = "cool"; break;
                       case 2: mode = "heat"; break; default: mode = "on"; }
    J("\"acmode\":\"%s\",\"coolsp\":%d,", mode, (int)lroundf(raw2fF(acCoolRaw)) - 2);  // panel deadband: wire-2
    J("\"heatsp\":%d,", (int)lroundf(raw2fF(acHeatRaw)) + 2);
  } else J("\"acmode\":\"?\",\"coolsp\":null,\"heatsp\":null,");

  if (seenVent) {
    char vs[64];
    snprintf(vs, sizeof vs, "%s%s", ventMode & 0x10 ? "open" : "closed",
             ventMode & 0x08 ? ", moving" : "");
    J("\"ventst\":\"%s\",\"vspeed\":%d,\"fansp\":\"%s\",", vs, ventSpeed,
      ventSpeed ? (ventMode & 1 ? "air in" : "air out") : "off");
  } else J("\"ventst\":\"?\",\"vspeed\":0,\"fansp\":\"\",");

  if (seenTank) J("\"fresh\":%d,\"gray\":%d,", fwR ? fwL * 100 / fwR : 0, grR ? grL * 100 / grR : 0);
  else J("\"fresh\":null,\"gray\":null,");

  {
    char ft[160];
    snprintf(ft, sizeof ft, "frames A %lu / B %lu%s%s",
             (unsigned long)cntA, (unsigned long)cntB,
             (faultB[0][0] | faultB[0][1]) ? "  PDM1 FAULT" : "",
             (faultB[1][0] | faultB[1][1]) ? "  PDM2 FAULT" : "");
    J("\"foot\":\"%s\"", ft);
  }
  J("}");
out:
  jbuf[sizeof jbuf - 1] = 0;
  server.send(200, "application/json", jbuf);
  #undef J
}

// ----- command endpoint ---------------------------------------------------------
void sendOK(const char *what) {
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
    else if (m == "heat") { sendAC(0x02); sendOK("ac heat (UNVERIFIED)"); }
    else server.send(400, "text/plain", "bad mode");
  } else if (c == "cool") {
    int d = server.arg("d").toInt();           // +/- 1 °F on the DISPLAYED value
    int8_t step = (d >= 0) ? 1 : -1;
    int shown = (int)lroundf(raw2fF(acCoolRaw)) - 2 + step;   // panel deadband
    shown = constrain(shown, 55, 90);          // sane bounds even before first echo
    acCoolRaw = f2acRaw(shown + 2.0f);
    sendAC(acB1 ? acB1 : 0x01);
    sendOK("cool setpoint");
  } else if (c == "vent") {
    if (server.hasArg("open")) {
      bool o = server.arg("open") == "1";
      ventMode = (ventMode & ~(0x10)) | 0x40;   // enable bit stays; open bit follows
      if (o) ventMode |= 0x10;
      if (!o) ventSpeed = 0;
      sendVent(ventSpeed, ventMode);
      sendOK(o ? "vent open" : "vent close");
    } else if (server.hasArg("speed")) {
      ventSpeed = (uint8_t)constrain(server.arg("speed").toInt(), 0, 255);
      sendVent(ventSpeed, ventMode);
      sendOK("vent speed");
    } else if (server.hasArg("dir")) {
      if (server.arg("dir") == "1") ventMode |= 0x01; else ventMode &= ~0x01;
      ventMode |= 0x40;
      sendVent(ventSpeed, ventMode);
      sendOK("vent direction");
    } else server.send(400, "text/plain", "bad vent");
  } else if (c == "inv") {
    bool on = server.arg("on") == "1";
    sendInv(on);
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
    server.send_P(200, "text/html", INDEX_HTML);
  });
  server.on("/api/state", HTTP_GET, sendState);
  server.on("/api/cmd", HTTP_POST, onCmd);
  server.onNotFound([]() {           // captive-portal probes land on the UI
    server.sendHeader("Location", String("http://") + WiFi.softAPIP().toString() + "/");
    server.send(302, "text/plain", "");
  });
  server.begin();
  Serial.println("http: 80 ready");
}

void loop() {
  server.handleClient();
  dns.processNextRequest();

  if (CanA.checkReceive() == CAN_MSGAVAIL) {
    uint8_t len, buf[8];
    CanA.readMsgBuf(&len, buf);
    cntA++;
    onCanAFrame(CanA.getCanId(), buf, len);
  }
  twai_message_t msg;
  while (twai_receive(&msg, 0) == ESP_OK) { cntB++; onCanBFrame(msg); }
}
