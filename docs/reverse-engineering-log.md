# Reverse-engineering log

Chronological record of findings. Keep new entries at the bottom.

## 2026-08-06 — First look at the firmware

- Input file: `STORYTELLER_MY24_2_06.pv1` (72,074,078 bytes), downloaded from
  Storyteller's website. `file(1)` reports `data`; not a desktop app — it's the
  head-unit **firmware update package**.
- Header magic `d4 c5 67 4e`. Entropy map: first ~3 MB readable, then a large
  high-entropy region (looked encrypted, was actually compressed), then a small
  Intel-HEX tail.
- Parsed the section table at `0x298` (32-byte entries). Sections chain
  contiguously and cover the whole file. See `docs/architecture.md` for layout.
- Section 5 starts `1f 8b 08` → **gzip**, not encryption. Decompresses
  68,830,183 → 135,782,400 bytes = a **GNU tar** starting with `app/Bootloader…`.
- Extracted 820 files: the whole application. Identified platform as
  **3sigma/Enovation QNX** on **Renesas R-Car M2**, display **HV1100-GF-T-CR**,
  build **STORYTELLER_MY24_2_06B**.
- Boot script (`proc/boot/.script` in the IFS) mounts the app filesystem from
  eMMC (`/fs/etfs`, qnx6). `app/start.sh` brings up CAN (`dev-can-m2` on
  can0/can1), MODBUS, audio, USB, touch, then launches `AppLoader`.
- Control model is `smRead`/`smWrite` on named variables (readable source in
  `config/config0/BinaryData/scripts.as`). ~86 variables enumerated.
- Signal dictionary present: `DeviceInformationAll.pbuff` names every
  parameter (`PDM1.DO5.AwningLights`, `J1939.Lithionics.*`, …).

## 2026-08-06 — Connectivity testing (laptop plugged into van)

Goal: find a live channel from a computer/phone to the unit.

- **Cabinet USB port:** user plugged a Linux laptop into it. `lsusb`, `ip`,
  `udevadm monitor` on a replug → **nothing enumerated**. Every USB device seen
  was the laptop's own (fingerprint reader, camera, Bluetooth, Logitech dongle).
  Conclusion: the port is a **USB host** port for update/media **sticks**, not a
  device/link port. Host-to-host over plain USB does nothing.
- **Wi-Fi / Bluetooth:** firmware contains `BTApp` (BLE + SPP/RFCOMM),
  `ConnectedDisplay` (JSON over BLE), `NetworkLauncher`, `hostapd`,
  `wpa_supplicant` (TI WL18xx). **But** the unit shows no Wi-Fi/BT settings UI
  and the user has never observed any Wi-Fi/BT presence. Treat the radios as
  inactive/unpopulated. Enabling them would require firmware changes (invasive).
- **Decision:** the always-on control surface is the **CAN bus**. Build a
  companion controller that taps CAN and bridges to a phone/web UI. Stock system
  untouched → no brick risk. Firmware package appears signed, reinforcing "don't
  reflash."

## Next steps

1. Decode `DeviceInformationAll.pbuff` into a human-readable signal map
   (name → device/channel → type/range). Offline, risk-free.
2. Extract CAN addressing (PGNs / source addresses / MODBUS registers) from
   `CANPro-manager` / `Configuration.bin` where present; confirm with one live
   bus capture.
3. Choose gateway hardware (ESP32 + CAN transceiver vs. Pi + CAN HAT vs.
   USB-CAN) and locate a physical CAN-H/CAN-L tap point.
4. Read-only sniff first; then carefully validate a single output before
   building the UI.
