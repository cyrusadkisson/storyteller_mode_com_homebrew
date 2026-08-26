# Signal dictionary

The complete list of named signals the head unit uses internally, decoded from
`config/config0/configurationInfo/DeviceInformationAll.pbuff` (a protobuf; no
`.proto` schema ships, so it was parsed from the wire format — see
`tools/` and the reverse-engineering log).

Each signal is an **id → name → unit** triple. The numeric id is what the UI
scripting (`smRead`/`smWrite`) and the CAN/MODBUS managers use to move a value
around inside the system. This is the map we build the companion controller on.

- **1758** total signals, ids `1`–`34478`.
- **~916** are van-control / telemetry relevant (the rest are infotainment —
  Maps/Radio/Media/GoPro/Bluetooth — and system internals).
- Machine-readable export of the control-relevant subset:
  [`../data/signals.control.csv`](../data/signals.control.csv)
  (columns: `id,name,unit,role,subsystem`).

> **Scope / provenance note.** These names and ids are reverse-engineered from
> the factory config for interoperability. This file is our analysis, not a copy
> of the firmware. The raw `.pbuff` itself is git-ignored and not redistributed.

## How to read the names

| Pattern | Meaning |
|---|---|
| `PDMn.DOx.Name` | Power Distribution Module *n*, **D**igital **O**utput *x* — a **switched load** (controllable) |
| `PDMn.DIx.Name` | **D**igital **I**nput *x* — a physical **switch** (read-only) |
| `PDMn.AIx.Name` | **A**nalog **I**nput *x* — a **sensor** (read-only) |
| `….DOxx_Name.Feedback` `[A]` | Measured **current draw** on that output — confirms a load is actually on and how hard it's working |
| `….DiagBits` / `.Diagbits` / `.DB` | Per-output **diagnostic/fault** status |
| `Name.On` / `Name.Off` | Discrete **command** actions (set the output on/off) |
| `CE …` / `CND …` / `SCE …` | **C**onditional **E**xpressions / conditions / scripted events — the UI's *logic*, not physical points. Useful to understand behavior; not things you drive directly. |
| bare names, e.g. `CargoLightsBrightness` | **logical setpoints / state variables** the UI reads & writes |
| unit column (`A V W C % RPM Hz L hPa …`) | engineering unit; `raw`/blank = dimensionless/enumerated |

The same physical load typically appears as several signals: the raw output
(`PDM1.DO2.CargoLights`), its on/off commands, its `.Feedback` current, its
`.DiagBits`, and one or more logical vars (`CargoLightsBrightness`). For control
we mostly care about the **output** and the **logical setpoint**; for a good UI
we also surface **`.Feedback`** (real state/amps) and **`.DiagBits`** (faults).

---

## Power Distribution Module 1 (PDM1) — primary loads

| Output | id | Load |
|---|---|---|
| DO1 | 552 | Solar / battery backup relay |
| DO2 | 553 | Cargo lights |
| DO3 | 555 | Reading lights |
| DO4 | 557 | Cabin lights |
| DO5 | 559 | Awning lights |
| DO6 | 561 | Recirc pump / bath light |
| DO7 | 637 | Awning enable |
| DO10 | 536 | Exhaust fan |
| DO11 | 590 | Furnace power |
| DO12 | 592 | Water pump |

Each output also has: `.Feedback` `[A]` (ids 776–787, measured current),
`.DiagBits`, and `.On`/`.Off` command signals (ids ~1017–1102).

**PDM1 physical switches (read-only inputs):** DI3 CargoLightsSwitch (577),
DI4 CabinLightSwitch (578), DI5 AwningLightSwitch (579),
DI6 RecircPump_BathLight_Switch (580), DI11 AwningInSwitch (563),
DI12 AwningOutSwitch (547). **Sensor:** AI4 ElectricalCabTemp `[V]` (694).

## Power Distribution Module 2 (PDM2) — secondary loads

| Output | id | Load |
|---|---|---|
| DO2 | 1208 | Galley fan speed (PWM; min/max speed + temp mapping at 1206/1214–1216) |
| DO3 | 534 | Refrigerator |
| DO4 | 693 | 12 V / USB outlets |
| DO5 | 692 | Awning motor (+ `.Reverse` 1209) |
| DO7 | 523 | Tank-monitor power |
| DO8 | 526 | Switch power |
| DO9 | 528 | AC gateway power |
| DO10 | 530 | 12 V speaker |
| DO11 | 521 | Sink pump `[%]` |
| DO12 | 539 | Aux 1 (spare, ramped) |

`.Feedback` `[A]` ids 790–798; `.On`/`.Off` ids ~1005–1211.
**PDM2 switches:** DI4 Aux1Switch (540), DI5 WaterPump (520),
DI6 MasterLightSwitch (545), DI7 FurnaceTrigger (546), DI9 SinkSwitch (538).

## Lighting (logical setpoints)

| id | Signal |
|---|---|
| 569 | CabinLightBrightness |
| 572 | ReadingLightBrightness |
| 573 | CargoLightsBrightness |
| 574 | AwningLightBrightness |

Plus master switches, per-zone on/off, and two user "presets" + a "magic" preset
(the `…Preset1/2/Magic` vars, ids in the 1214–1499 range). The preset logic is
in the firmware's `scripts.as` (`SC Lighting`).

## Climate / HVAC

| id | Signal | Unit |
|---|---|---|
| 632 / 633 | AC setpoint heat / cool | °C |
| 629 / 630 | AC setpoint heat / cool | °F |
| 622 / 623 | Tx AC setpoint heat / cool | °C |
| 619 / 620 | Rx AC setpoint heat / cool | °C |
| 621 | Tx Thermostat command | |
| 661 | Rx ambient temp | °C |
| 1197 | Cab_Temp | °C |

`Tx…`/`Rx…` = the transmit/receive sides of the thermostat conversation on the
bus. Scripted climate behavior: `SC Climate Controls` (1419), `SC Magic Climate`
(1421), `SC Climate Off` (1418).

## Hydronic heater (Rixen) — **includes raw CAN frame ids**

This subsystem exposes its CAN identifiers directly in the signal names:

| CAN id | Direction | Purpose |
|---|---|---|
| `0x724` | Rx | HC_Status (1170) |
| `0x725` | Rx | HC_Diag (1179) |
| `0x726` | Rx | HC_Diag2 (1176) |
| `0x727` | Rx | HC_Fault (1173) |
| `0x788` | **Tx (commands)** | Set TempTarget (1149), FanSpeed (1152), Furnace (1155), Electric (1158), Engine (1161), Hot Water (1164), Preheat (1167) |
| `0x789` | Tx | HC_SetIO (1182) |

Status/telemetry (read): `rix_Status_*` ids 1065–1076 — ambient/target temp `[C]`,
air pressure `[hPa]`, humidity `[%]`, and flags for Heat/Pump/Furnace/Electric/
Engine/HotWater/Preheat. Diagnostics: `rix_Diag*` 1084–1093 (inlet/outlet temp,
flame sensor, voltage, fuel, runtime, glow).

> These `0x7xx` ids are 11-bit standard CAN identifiers — a concrete anchor for
> the first live capture: watch `0x724`–`0x727` to decode heater state, and
> `0x788` to learn the command frame layout.

## Water & tanks

| id | Signal |
|---|---|
| 600 / 679 | Fresh-water tank status / level |
| 604 | Fresh-water tank size |
| 609 / 678 | Gray tank status / level |
| 608 | Gray tank size |
| 660 / 1184 | Hot water active / status |
| 708 | Tank-level alarm |
| 521 / 592 | Sink pump / water pump (see PDMs) |
| 1208 | Galley fan speed (temp-mapped) |

## Battery — Lithionics (J1939)

Telemetry (read-only), ids 836–844:

| id | Signal | Unit |
|---|---|---|
| 838 | DC voltage | V |
| 837 | DC current | A |
| 836 | DC power | W |
| 842 | State of charge | % |
| 841 | State of health | % |
| 840 | Discharge capacity remaining | |
| 843 | Battery temp | C |
| 844 | Time remaining | min |
| 1141 | Power flow (converted) | W |

Plus a large set of BMS warning flags `Lith_BMS_Warn_*` (ids 1003–1026):
high/low voltage, overcurrent, hot/cold temp, contactor flutter, precharge error,
charge disable, battery protection, "NeverDie" reserve, power-off, etc. Commands
exist too (`Lith_On` 1339, `Lith_Off` 1340, `Lith_Send` 1338) — **treat writes to
the BMS with extreme caution.**

## Inverter

`Inverter_Command` (1187/1341), `Inverter_En_Request` (975),
`FFC_1FFD4_Inverter Status` (1192) — the `1FFD4` is an RV-C DGN, indicating some
RV-C framing on the bus alongside the Enovation CANPro scheme.

## Faults (J1939 diagnostics DM1 / DM2)

~184 signals under `DM1.*`/`Dm1.*` (active faults) and `DM2.*`/`Dm2.*` (stored
faults) — standard J1939 diagnostic message decode (`.Header.Source`,
`.SPN`, `.FMI`, lamp status, etc.). Useful for surfacing "something's wrong"
in a nicer way than the stock screen does.

---

## What this gives us

Every signal's id, human name, unit, logical device and channel, and — for the
Rixen heater and the inverter — actual CAN ids/DGNs.

The dictionary names the signals but does not say which frame carries them.
That wire-level mapping was recovered separately, by live capture:
[`pdm-control.md`](pdm-control.md) for the PDM loads,
[`climate-control.md`](climate-control.md) for the heater, thermostat and vent,
and [`energy-can2.md`](energy-can2.md) for the Lithionics pack, inverter and
charger.
