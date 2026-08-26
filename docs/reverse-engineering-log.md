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

## 2026-08-11 — FIRST LIVE CAPTURE — PDM protocol found

Tapped CAN1 (pins 5/6, sleeved green/yellow) with the CANable, listen-only @
250 kbps. Bus live and clean.

**Rig validated.** Frames predicted from the firmware appeared verbatim:
Rixen `0x724` status / `0x788` command (multiplexed on byte 0: `01 01`,
`0C 03 26`, `06`, `03`, `02`), plus `0x725/726/728/78A`; holding tanks
**PGN 1FFB7 SA AF**; A/C **PGN 1FFE2 / 1FF9C / FEA7 SA 58**; DM1 (`FECA`)
from both AF and 58. `can-map.md` is confirmed against real wire data.

**The PDM gap is closed (pending load confirmation).** Bus is dominated by
**PGN EF00 (Proprietary A, PF=0xEF, PDU1 point-to-point)**, priority 5,
between a master at **SA 0x11** and two slaves at **SA 0x1E / 0x1F** —
the head unit and PDM1/PDM2.

| CAN ID | SA → DA | Role |
|---|---|---|
| `14EF1E11` / `14EF1F11` | 0x11 → 0x1E/0x1F | **command** (head unit → PDM) |
| `14EF111E` / `14EF111F` | 0x1E/0x1F → 0x11 | status / feedback |

Command frames carry only **two payloads each**, byte 0 = multiplexer
(`FC`, `FD` — likely channels 1–6 and 7–12), bytes 1–6 = per-channel level,
byte 7 = `FF` constant:

```
→ 0x1F:  FC 00 5C 7F 7F 00 00 FF   |  FD 7F 7F 7F 7F 00 00 FF
→ 0x1E:  FC 7F 00 00 40 00 00 FF   |  FD 00 00 00 7F 7F 00 FF
```

Values observed: `00` (off), `7F` (full), `40` (~50%), `5C` (~72%) — i.e. a
0–0x7F level, matching `EnableOutput` / `SetPwmCommandMode` in PDM-Manager.

Status direction is richer, byte 0 multiplexed over `39/F0/F8/FA/FB/FC/FD/FE`,
with `FC` sub-indexed on byte 1 (`F1`…`FC` = 12 items) — per-channel current
feedback is the obvious candidate.

Also present: `14E9111E` / `14E9111F` (DLC 4, `01 00 00 00`…`01 05 00 00`) —
a 0–5 poll/scan cycle.

Note: the head-unit SA seen here is **0x11**, not the 0x46 inferred from
`Configuration.bin`. Different bus/role; do not assume 0x46 on CAN1.

**Still to confirm:** which byte position corresponds to which physical load.
That needs the OFF/ON capture-diff — static analysis cannot supply it.

## 2026-08-11 — Inverter / branch-amps exhausted on CAN1

Three power-system actions in one 15 s capture: inverter ON, branch-amps slider
20 → 15 → 20, inverter OFF. Result on CAN1:

- **zero new CAN ids** (same 18 as every other capture)
- **zero PDM byte changes** — all four command frames flat throughout
- exhaustive grep for `19FFD3` / `19FFD4` / `19FFCA` / `19FF95` / `18EF0046` /
  `18FF8046`: nothing (earlier substring hits were payload bytes, not ids)

Critically, **the inverter and branch-amps controls are on-screen buttons, not
physical switches.** So the head unit necessarily transmitted something, and it
was not on CAN1. Combined with the BMS/inverter/charger ids being defined in the
firmware DB but never observed, the energy subsystem is on **CAN2**.

### Tool fix: multiplexer auto-detection false positive

`can_diff.py` split `0x724` (Rixen status) into phantom sub-messages `[5D]` and
`[95]`, reporting one as "appeared" and the other as "disappeared". Byte 0 there
is a *reading*, not a selector — but it had few distinct values, which was the
whole heuristic.

Fixed by adding the property that actually distinguishes them: **a real
multiplexer cycles.** It revisits its values repeatedly as the sender rotates
through sub-messages; a drifting data byte changes monotonically and rarely
returns. Now requires `transitions >= 2 * distinct_values` **and**
`revisits >= distinct_values`. Verified: `0x724` no longer splits, all genuine
muxes still detected, and the cabin-light result reproduces unchanged.

### Incidental

`0x78A` bytes 2–3 mirror `0x724` bytes 0–1, and `0x788[0C]` byte 1 carries the
same value as `0x19FF9C58` byte 1 (ambient from the A/C, relayed to the Rixen
heater — matching `Rixen Send Amb Temp` in the DB).

## 2026-08-24 — T-2CAN-FD live control session (companion)

Both the A/C and the inverter are controllable from the companion. The panel
UI is a **one-way display**: it never reflects externally-issued commands, so
**power flow / bus state is the source of truth, never the screen.**

**A/C command** — `0x19FEF903` [8] `01 <on/off> 64 2F 25 40 25 00`
(byte 1 = `01` on / `00` off; byte 0 constant `01`). Verified: fan went to
high, cooling element came on, ~1 kW off the inverter. **Bug trap:** byte 1 is
the on/off and the frame is 8 bytes — a premature 7-byte attempt only ran the
low fan. Fan low/high/auto button mapping still pending (capture
`19FEF903` while pressing those).

**Inverter (CAN2)** — `19FFD3F2` [8] `01 <on/off> 00 00 00 00 00 00`
(byte 1 = `01` on / `00` off), single-shot latch. Initial "failure" was purely
the screen not updating; the power-flow confirms it works (idle draw rises, and
the A/C runs off it). ModeWifi has **no inverter control** — his HVAC is fed via
PDM2 output DO9 (`HVAC_POWER`), a DC unit, unlike our 120 V inverter-fed roof.

**Same-session follow-ups (2026-08-24):**

- **Vent verified working** — `0x19FEA603` is an 8-byte frame
  `02 15 <speed> <mode>` (instance `02`, `15` const, speed 0–255, mode = bit6
  enable + bit4 open + bit0 air-in). A first attempt sent it **without the
  leading `02` byte** (7 bytes) and the fan did not respond at all — the same
  offset trap as the A/C command. With the full frame the fan status
  (`0x19FEA758`) echoed the sequence exactly.
- **Compressor is independently controllable** — the A/C screen's compressor
  "AC OFF" switch writes `0x19FEF903` byte 1 = `0x04` (vs `0x01` = on,
  `0x00` = off), and `0x19FFE258` echoes it. This corrects the earlier
  "compressor is autonomous and invisible" conclusion in climate-control.md.

## 2026-08-26 — The companion box is built and in the van

**Hardware:** LILYGO T-2CAN-FD (ESP32-S3), two independent CAN controllers —
MCP2518FD on CAN1, the ESP32's TWAI on CAN2 — both isolated. A dual-CAN board
became necessary once the battery/inverter turned out to live on CAN2.
Firmware vendored in `firmware/t2can/`.

**Working control surface** (all wire-verified on the van):

| Subsystem | Method |
|---|---|
| Cabin, garage, awning light, aux, water pump, recirc | wall-switch **input spoof** — the head unit toggles and holds it |
| Roof A/C: off/cool/heat, compressor, fan auto/low/high, cool setpoint | direct write as SA `0x03`, echoed on `0x19FFE258` |
| Roof vent: lid, fan, airflow, speed | direct write `0x19FEA603` |
| Inverter | single-shot latch on CAN2 |
| Read-only | per-channel levels + feedback amps, tanks, battery/SoC, AC line, temps, PDM faults, Rixen state |

**Deliberately out of scope**, each for a measured reason:

- **Dimming** — holding a level means out-transmitting the head unit at ~250 Hz
  (~+50% bus load). Rejected on bus safety. See `pdm-control.md`.
- **Reading lights** — no digital input exists for DO3 at all, so there is
  nothing to spoof and a direct write is overwritten in ~11 ms.
- **Rixen writes** — accepted by the heater in ~300 ms, but reverted by the
  head unit within ~5 s. Holding one would oscillate a diesel burner's
  setpoint. Read-only instead. See `climate-control.md`.
- **Sink drain, awning motor** — owner decision / hardware removed.

All three would need the **cut-and-stand-in** architecture (box inline, owning
the channel) rather than a parallel tap.

**Corrections:** the head unit re-asserts PDM state at ~91 Hz (not the ~45 Hz
previously recorded); the vent's "in motion" flag lags a lid command by ~4 s;
"a companion app never needs to emulate a switch" is exactly backwards for a
parallel tap.
