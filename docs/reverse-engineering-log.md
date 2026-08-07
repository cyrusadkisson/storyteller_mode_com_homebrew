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

## 2026-08-06 — Signal dictionary decoded

- Parsed `DeviceInformationAll.pbuff` (protobuf, no schema) straight from the
  wire format. Top-level has three repeated signal tables (fields #14/#15/#16);
  each record is `id`(#1) + `name`(#2) + `unit`(#4, e.g. A/V/W/C/%/RPM/Hz).
- **1758 signals**, ids 1–34478. ~916 are van-control/telemetry; the rest are
  infotainment (Maps/Radio/Media/GoPro/Bluetooth) + system internals.
- Mapped every PDM1/PDM2 output (loads), input (switches), `.Feedback` (per-load
  amps) and `.DiagBits`; lighting/climate/water setpoints; Lithionics battery
  telemetry + BMS warnings; and the Rixen heater — which **embeds raw CAN ids in
  its names** (status `0x724`–`0x727`, commands `0x788`, IO `0x789`). Inverter
  status references RV-C DGN `1FFD4` (so some RV-C framing coexists with CANPro).
- Wrote `docs/signal-dictionary.md` (annotated map) and
  `data/signals.control.csv` (machine-readable subset). Raw `.pbuff` stays
  git-ignored; only our derived analysis is tracked.
- Still missing: wire-level CAN PGN/byte mapping for the non-Rixen signals →
  next mine `CANPro-manager`/`j1939`/`PDM-Manager`/`Configuration.bin`, then
  confirm with a live capture (validate the rig on the known `0x724`/`0x788`
  frames first).

## 2026-08-06 — Wire-level CAN map extracted

- `Configuration.bin` is gzip → a 686 KB custom typed-node tree. Reverse-
  engineered the node format (tags: `08bc6d46` message, `44b7fd36` signal,
  `d9b7f8f2` container, `8f8eefea` value/string; interned strings, absolute
  offset refs). Full format in `tools/parse_can_config.py` header.
- Extracted a DBC: **30 messages, 129 signals** with CAN id, direction, DLC, and
  per-signal start/end bit + scale + offset. Self-validates: AC/thermostat temps
  come out as scale 0.03125 / offset −273 (textbook J1939 temp).
- Covered subsystems: Rixen heater (std 11-bit 0x724–0x789, 0x788 = multiplexed
  command), holding tanks (PGN 1FFB7), AC/HVAC ("FFC", PGN 1FFE2/1FEF9/…),
  Lithionics BMS (PGN FF80 info, EF00 cmd/resp), inverter/charger (1FFD3/1FFD4/
  1FF95/1FFCA). Head-unit SA = 0x46.
- **PDM outputs (lights/pumps) are NOT in this table** — config only names PDM
  inputs; PDM output control lives in `app/PDM-Manager` (J1939). Shared constant
  `0x53f80018` in PDM-Manager + CANPro-manager → check on live bus. This is the
  main remaining gap for controlling loads.
- Wrote `tools/parse_can_config.py`, `data/can_messages.csv`/`.json`,
  `docs/can-map.md`.

## 2026-08-07 — PDM-Manager static analysis

- `PDM-Manager` = stripped ARM/QNX C++. Established the control architecture:
  device classes `PDM::PDM12V`/`PDM24V`/`PDMBase`, config objects
  `conf::PdmOutput`/`PdmInput`/`PdmData`, control primitives `EnableOutput`/
  `DisableOutput` (on/off), `SetPwmCommandMode` (dimming), `SetPositionCommandMode`.
  Transport = QNX `MsgSend` to the `j1939` daemon (no direct J1939 imports).
- **`0x53f80018` is NOT a CAN id** — it's an `in32`/`out32` MMIO register poke.
  Corrected `can-map.md`.
- The PDM command PGN/payload is **not** statically recoverable here: no hardcoded
  PGN constant (checked movw/movt immediates, literal pools, .data.rel.ro), and
  **no ARM disassembler available** (objdump lacks ARM; no capstone/llvm; offline).
  Documented in `docs/pdm-control.md` with the decisive next step: a live CAN
  capture (toggle each load, diff traffic; validate rig on Rixen 0x788 first),
  or run Ghidra/Capstone on a connected machine.
- Tooling gap to close for deeper static RE: install Capstone / use Ghidra.

## 2026-08-07 — Hardware ID & CAN tap point (from back-panel photos)

- Head unit positively ID'd: Enovation/Murphy **PowerView PV1100-TCL**
  (6–36VDC), integrated by **JET Technologies** as the "12986-KIT" panel.
- Pulled the PV1100 install manual (00-02-1020): Black Connector CAN1 = pins
  5(Low)/6(High), CAN2 = 18/19, Battery=7, Ground=8. The round **M12 5-pin
  A-coded jack is ETHERNET, not CAN** (so standard D-coded M12 Ethernet cables
  won't fit; port may be dormant).
- Physical wiring in this van: the two big 35-pin connectors are **empty**; CAN
  comes through the smaller **"CONTROL PANEL"** connector. **CAN1 = green(CAN_L)
  + yellow(CAN_H) twisted pair** (J1939 colors; meter-verify). Red/blue/black
  connectors = power; "PUSH BUTTON" = input.
- Tap plan documented in `docs/hardware-and-tap.md`: T-tap green/yellow →
  CANable (termination off, listen-only), validate on Rixen 0x724/0x788, then
  flip loads to finish the PDM map. Parts: CANable + T-taps + multimeter.
- Photos kept local; `images/` git-ignored (GPS EXIF + interior).
