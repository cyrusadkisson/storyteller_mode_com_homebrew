# Wire-level CAN map

The actual bus messages the head unit sends and receives, decoded from
`config/config0/Configuration.bin` (gzip → a custom typed-node tree). This is a
DBC-style database: **30 messages, 129 signals**, each with CAN id, direction,
DLC, and per-signal bit position + scale + offset.

- Reproduce: `python3 tools/parse_can_config.py <Configuration.bin> --csv out.csv`
- Machine-readable: [`../data/can_messages.csv`](../data/can_messages.csv) /
  [`../data/can_messages.json`](../data/can_messages.json)
- Binary format is documented in the header of `tools/parse_can_config.py`.

> **Provenance/IP:** decoded for interoperability; this is our derived table, not
> a copy of the firmware. `Configuration.bin` itself stays git-ignored.

## How to decode a signal

```
engineering_value = raw_bits * scale + offset
```

Bits are LSB-numbered across the 8-byte frame (bit 0 = byte 0 bit 0). Example
that validates the parse: the AC and thermostat temperature signals use
**scale 0.03125 (= 1/32), offset −273** — the standard J1939 temperature
encoding (K→°C). Getting that exact pair out of the binary is strong evidence
the bit/scale extraction is correct.

## What this covers — and what it doesn't

**Covered:** the RV *appliances* on the bus — holding tanks, air conditioning /
HVAC, the Rixen hydronic heater, the Lithionics BMS, and the inverter/charger.

**NOT covered (important):** the **Power Distribution Modules (PDM1/PDM2)** that
switch the lights, pumps, fans, fridge, etc. Those are **not** declared as
messages here — `Configuration.bin` only references a handful of PDM *inputs*
(DI3/DI4/DI5/DI6) by name. PDM output control is driven by J1939 logic inside the
`app/PDM-Manager` binary instead. So to command lights/pumps we still need to
reverse that — analysis is in [`pdm-control.md`](pdm-control.md).
(Note: `0x53f80018`, seen in PDM-Manager, is a memory-mapped SoC register poke —
`in32`/`out32` — **not** a CAN id; disregard the earlier lead.)

## CAN ports

Two channels exist: `CANPort1 → /dev/can0`, `CANPort2 → /dev/can1`. Which message
rides which port is not yet resolved from the config (per-port grouping nodes
exist but aren't decoded). Determine on the bench with a capture.

---

## Rixen hydronic heater — standard 11-bit CAN

| CAN id | Dir | Purpose |
|---|---|---|
| `0x724` | RX | HC_Status — 12 signals: ambient temp ×0.01, target ×0.1, pressure, 8 status bits (heat/pump/furnace/electric/engine/hotwater/preheat), + mode byte |
| `0x725` | RX | HC_Diag |
| `0x726` | RX | HC_Diag2 — 4 temps (inlet/outlet ×0.01, voltage ×0.1) |
| `0x727` | RX | HC_Fault — two 32-bit fault words |
| `0x788` | **TX** | **Commands (multiplexed)** — see below |
| `0x789` | TX | HC_SetIO — 8 single-bit IO + a 32-bit field |

**Commanding the heater (`0x788`).** All heater commands share id `0x788`; byte 0
selects the parameter and the payload differs:

| Command | Payload bits | Encoding |
|---|---|---|
| Set TempTarget | 8..23 | 16-bit |
| Set FanSpeed | 8..15 | 8-bit |
| Set Furnace / Electric / Engine / Hot Water / Preheat | bit 8 | 1-bit on/off |
| Send Ambient Temp | 8..23 | ×0.03125, −273 (°C) |
| Send Eng Run / Prime Fuel Time | 8.. | flag / 8-bit |

This is the cleanest first target for read **and** write validation: watch
`0x724`–`0x727` to confirm decoding, then a single `0x788` frame exercises a write.

## Holding tanks — J1939

| CAN id | PGN | SA | Purpose |
|---|---|---|---|
| `0x19FFB7AF` | `0x1FFB7` | `0xAF` | Fresh-water & gray tank status (one PGN, both tanks) — 4 fields each (level, capacity, etc.) |

## Air conditioning / HVAC — J1939 ("FFC")

| CAN id | PGN | SA | Dir | Purpose |
|---|---|---|---|---|
| `0x19FFE258` | `0x1FFE2` | `0x58` | RX | AC unit status (2 temps ×0.03125 −273) |
| `0x19FEF903` | `0x1FEF9` | `0x03` | TX | Thermostat command (heat/cool setpoints ×0.03125 −273) |
| `0x19FF9C58` | `0x1FF9C` | `0x58` | RX | Ambient status |
| `0x19FEA603` | `0x1FEA6` | `0x03` | TX | Vent control (13 fields) |
| `0x19FEA758` | `0x1FEA7` | `0x58` | RX | Vent status 2 |
| `0x19FDE203` | `0x1FDE2` | `0x03` | TX | Vent control 2 |

## Battery — Lithionics — J1939

| CAN id | PGN | SA | Dir | Purpose |
|---|---|---|---|---|
| `0x18FF8046` | `0x0FF80` | `0x46` | RX | BMS info — 26 signals: 4 data bytes + 22 status bitflags |
| `0x18EF0000` | `0x0EF00` | `0x00` | TX | BMS command (proprietary peer-to-peer, PGN EF00) |
| `0x18EF0046` | `0x0EF00` | `0x46` | RX | BMS response |

SA `0x46` (70) recurs as the head unit's / display's own address.

## Inverter & charger — J1939

| CAN id | PGN | SA | Dir | Purpose |
|---|---|---|---|---|
| `0x19FFD300` | `0x1FFD3` | `0x00` | TX | Inverter command |
| `0x19FFD4E1` | `0x1FFD4` | `0xE1` | RX | Inverter status (RV-C DGN 1FFD4) |
| `0x19FF9500` | `0x1FF95` | `0x00` | TX | Circuit capacity |
| `0x19FFCAE1` | `0x1FFCA` | `0xE1` | RX | Charger AC status |

SA `0xE1` (225) = the inverter/charger device.

---

## J1939 addressing observed

Extended ids decode as `priority(3) | PGN(18) | source-address(8)`. Source
addresses seen: `0x46` head unit, `0xE1` inverter/charger, `0x58` AC unit,
`0xAF` tank monitor, `0x03` thermostat side, `0x00` broadcast/command source.

## Coverage

The map is bus-validated end to end.

- **PDM output protocol** — command frames, mux layout, per-channel levels,
  feedback amps, and the input-spoof control method are in
  [`pdm-control.md`](pdm-control.md).
- **Signal names** — resolved for everything the app uses (tanks, climate,
  vent, battery/inverter). Some unused BMS sub-signals have bit layouts but no
  individual names.
- **Port assignment** — CAN1 = pins 5/6 (loads, tanks, climate, Rixen);
  CAN2 = pins 18/19 (battery, inverter, charger). See
  [`energy-can2.md`](energy-can2.md).

---

## Tank levels decoded and verified against the panel (2026-08-12)

`0x19FFB7AF` (PGN 1FFB7, SA `0xAF`) is **multiplexed on byte 0 = tank instance**
— the DB's `Rx_FWTankStatus` and `Rx_GrayTankStatus` share one CAN id.

| byte | field |
|---|---|
| 0 | **instance** — `0` = fresh water, `2` = gray |
| 1 | level (raw count) |
| 2 | resolution (full-scale count) |

**level % = byte1 / byte2 × 100** — standard RV-C DGN 1FFB7.

Verified live: `00 0B 18` → 11/24 = **45.8 %** fresh, `02 00 18` → 0/24 =
**0 %** gray. The owner confirmed both match the panel exactly.

## What the panel displays, and where each value comes from

The head unit shows six live values. All six are now accounted for:

| Panel display | Source | Bus |
|---|---|---|
| Interior temperature | `0x724` b0‑1 ×0.01, and `0x19FF9C58` | **CAN1** ✅ decoded |
| Fresh tank level | `0x19FFB7AF` instance 0 | **CAN1** ✅ decoded |
| Gray tank level | `0x19FFB7AF` instance 2 | **CAN1** ✅ decoded |
| Battery state of charge | `LITH_Batt_StateOCharge` | **CAN2** |
| Battery pack temperature | `LITH_Batt_Temp` | **CAN2** |
| Power flow | `LITH_Batt_DCCurr` / `DC_Power` | **CAN2** |

The three missing ones are all Lithionics, on `0x18FF8046` / `0x18EF0046`
from SA `0x46` — a source address that has **never appeared** in any CAN1
capture. This is now established two ways: by elimination (absent from CAN1)
and by positive confirmation (everything CAN1 *does* carry has been decoded and
matched against the panel).

### Note on a false lead

`0x14EF111E[FB]` / `0x14EF111F[FB]` byte 7 both read `0x34` (52), which looked
like a plausible state-of-charge given a "50 %+" pack. It is **not** battery
data — no Lithionics frame reaches this bus. Byte 6 of the same frame is also
not supply voltage: the two PDMs reported 147–170 and 106–137 simultaneously,
and two nodes on one 12 V system cannot disagree about it. Both bytes remain
unidentified per‑PDM telemetry.
