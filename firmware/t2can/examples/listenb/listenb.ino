/*
 * listenb — check the T-2CAN-FD's CAN-B (ESP32 TWAI) tap onto the van's CAN2.
 *
 * CAN-B is the ESP32-S3's built-in TWAI controller on GPIO7 (TX) / GPIO6 (RX),
 * NOT the MCP2518FD. This runs it in LISTEN-ONLY so a wrong bitrate or a bad
 * tap cannot inject anything onto the CAN2 bus while we find the right speed.
 *
 * Prints every received frame as "IDx [dlc] bytes". Default 250 kbit/s; if the
 * bus stays silent, re-flash with TWAI_TIMING_CONFIG_500KBITS() and retry.
 */
#include <Arduino.h>
#include "driver/twai.h"

#define CANB_TX GPIO_NUM_7
#define CANB_RX GPIO_NUM_6

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("listenb: boot");

  twai_general_config_t g = TWAI_GENERAL_CONFIG_DEFAULT(CANB_TX, CANB_RX, TWAI_MODE_LISTEN_ONLY);
  twai_timing_config_t t = TWAI_TIMING_CONFIG_250KBITS();
  twai_filter_config_t f = TWAI_FILTER_CONFIG_ACCEPT_ALL();

  esp_err_t err = twai_driver_install(&g, &t, &f);
  if (err != ESP_OK) {
    Serial.printf("listenb: twai_driver_install failed err=%d\n", err);
    return;
  }
  err = twai_start();
  if (err != ESP_OK) {
    Serial.printf("listenb: twai_start failed err=%d\n", err);
    return;
  }
  Serial.println("listenb: CAN-B listening (listen-only, 250kbps)");
}

void loop() {
  twai_message_t msg;
  if (twai_receive(&msg, pdMS_TO_TICKS(1000)) == ESP_OK) {
    Serial.printf("%lu RX %08X%c [%d]",
                  (unsigned long)millis(),
                  (uint32_t)msg.identifier,
                  msg.extd ? 'E' : 'S',
                  msg.data_length_code);
    for (int i = 0; i < msg.data_length_code; i++) Serial.printf(" %02X", msg.data[i]);
    Serial.println();
  }
}
