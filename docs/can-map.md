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
reverse that (next step). Lead: both `PDM-Manager` and `CANPro-manager` contain
the constant `0x53f80018` — check it against a live capture.

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

## Next steps

1. **PDM output protocol** — reverse `app/PDM-Manager` (J1939) to learn how
   lights/pumps/fans are commanded; validate `0x53f80018` on a live bus.
2. **Per-signal names** — messages with many signals (BMS info ×26, vent ×13)
   have bit layouts but not individual names yet; resolve the signal-id → name
   linkage or infer from J1939/RV-C standards + the signal dictionary.
3. **Port assignment** — decode which messages are on can0 vs can1.
4. **Live capture** — validate the whole map on the bus; start with the known
   Rixen `0x724`/`0x788` frames to prove the capture rig before decoding unknowns.
