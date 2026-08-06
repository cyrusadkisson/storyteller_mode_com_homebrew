# System architecture

How the Storyteller MY24 "MODE" system is built, as reconstructed from the
factory firmware package `STORYTELLER_MY24_2_06.pv1`.

## Hardware

- **Display / head unit:** 3sigma / Enovation Controls (EControls) display,
  part number **HV1100-GF-T-CR**. This is a rugged CAN HMI in the same family as
  Murphy PowerView units.
- **SoC:** Renesas R-Car M2 (ARM Cortex-A15).
- **OS:** QNX Neutrino (RTOS).
- **Screen:** 1280×768.
- **Buses:** two CAN channels (`can0`, `can1`), MODBUS, plus SPI/I²C for local
  peripherals (backlight, touch, EEPROM, PMIC, thermal).

The head unit is **not** the thing that switches loads. It is one node on the
CAN bus. The actual switching is done by **Power Distribution Modules (PDMs)**.

## Control model

Everything the user can see or control is a **named state-machine variable**.
The UI logic reads and writes them with `smRead()` / `smWrite()` (see
`config/config0/BinaryData/scripts.as` in the firmware — human-readable). Those
variables are bound to physical channels on the CAN/MODBUS devices.

Naming convention seen in the signal dictionary
(`config/config0/configurationInfo/DeviceInformationAll.pbuff`):

| Prefix | Meaning | Example |
|---|---|---|
| `PDM1.DOn` / `PDM2.DOn` | Power module **D**igital **O**utput = a switched load | `PDM1.DO5.AwningLights` |
| `PDM1.DIn` | **D**igital **I**nput = a physical switch | `PDM1.DI6.RecircPump_BathLight_Switch` |
| `PDM1.AIn` | **A**nalog **I**nput = a sensor | `PDM1.AI4.ElectricalCabTemp` |
| `J1939.Lithionics.*` | Battery BMS telemetry over J1939 | `LITH_Batt_DischargeCapacityRemaining` |
| `cmd_Rix_*` | Rixen hydronic heater commands | `cmd_Rix_SetFanSpeed` |

### Known controllable domains (~86 variables)

- **Lighting:** Cargo, Reading, Cabin, Awning, Bath — brightness, on/off,
  presets 1/2/"magic", and a master switch.
- **Climate:** AC cool/heat setpoints (°C/°F), cabin temp, heater source, galley
  fan, HVAC toggle.
- **Hydronic heat (Rixen):** fan speed, electric vs. furnace source.
- **Hot water**, **inverter** command, **awning** motor + rain-sensor auto-retract.
- **Power:** Lithionics BMS state, amp limit, solar/battery backup
  (`PDM1.DO1.SolarBattBackup`), raw PDM digital outputs DO1–DO8.

## Firmware package format (`.pv1`)

Little-endian container; header magic `d4 c5 67 4e`. A section table begins near
offset `0x298`; each entry is 32 bytes:

```
+0x00 u32 id
+0x04 u32 flagA
+0x08 u32 flagB
+0x0c u64 size      (lo@0x0c, hi@0x10)
+0x14 u64 offset    (lo@0x14, hi@0x18)
+0x1c u32 checksum  (algorithm TBD — not plain zlib crc32)
```

Sections are stored contiguously (each `offset` == previous `offset+size`):

| id | contents |
|---|---|
| 2 | QNX boot image filesystem (IFS) — bootstrap, drivers, `procnto`, libc |
| 3, 4 | zero-filled gzip placeholders |
| 5 | **gzip** (`1f 8b`) → **POSIX tar** = the application (`app/ config/ os/ nvdata/`) |
| 7 | Intel-HEX firmware for a companion MCU (~8 KB, addr 0x0000–0xb000) |

`tools/unpack_pv1.py` parses this and extracts the application tree.

## Application layer (section 5)

The tar holds the running system:

- `app/AppLoader` — the data-driven UI engine. Renders whatever is in
  `config/config0/` (`Configuration.bin` + PNG assets + compiled `scripts.byt`).
- `app/CANPro-manager`, `app/j1939`, `app/PDM-Manager` — the CAN stack that maps
  variables to bus traffic.
- `app/MODBUS-master` / `MODBUS-slave` / `modbus-app` — MODBUS side.
- `app/BTApp`, `app/ConnectedDisplay`, `app/NetworkLauncher` — Bluetooth (BLE +
  SPP) and network apps. **Present but not active on this unit.**
- `config/config0/configurationInfo/DeviceInformationAll.pbuff` — the **signal
  dictionary** (protobuf). The Rosetta Stone for building our own client.

## Connectivity reality (measured)

See `docs/reverse-engineering-log.md`. Short version: USB port is host-only
(update sticks), Wi-Fi/BT radios inactive, **CAN bus is the path**.
