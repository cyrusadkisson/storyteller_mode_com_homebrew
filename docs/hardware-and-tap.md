# Hardware ID & CAN tap point

Physical identification of the head unit and where to get on the CAN bus,
from inspection of the unit's back panel. (Photos are kept local / git-ignored —
they carry GPS EXIF; this doc is the conclusions.)

## The head unit

- **Display:** Enovation Controls / Murphy **PowerView PV1100‑TCL** (label:
  `MODEL: PV1100-TCL`, `INPUT: 6–36VDC, 33W`, Enovation Controls, Tulsa OK).
  Other labels: `78700635 / BL1100TBWEL`, and a config part `HV1100-GF-T-CR`
  in the firmware.
- **Integrator:** **JET Technologies, Inc.** — "12986‑KIT Control Screen Panel,
  ABS" (order #175241, 7/14/23). JET builds the Storyteller "MODE" panel on the
  Enovation display.
- Documented product → install manual `00-02-1020` is public (ManualsLib /
  Scribd), which is where the connector pinouts come from.

## Connectors on the back (what's what)

Full inventory, walked by hand 2026‑08‑11 (supersedes an earlier, wrong reading
taken from photos — see note at the end of this section):

| # | Connector | In use? | Function |
|---|---|---|---|
| 1 | **23‑pin AMPSEAL** (8/7/8, male) | **YES — the live one** | Power + **both CAN buses** + I/O. **This is the tap.** |
| 2 | 23‑pin AMPSEAL (male) | No — empty | the manual's other (gray) connector: video / RS‑485 |
| 3 | ~35‑pin (male) | No | A/V expansion Storyteller didn't wire |
| 4 | Round, large, USB pigtail | Yes | **USB host** — update/media sticks only; a laptop does not enumerate |
| 5 | Round **M12 5‑pin A‑coded** | No — empty | **Ethernet**, *not* CAN (see below). Possibly dormant. |
| 6 | Round, power button | Yes | Pushbutton — not CAN |

> **Correction:** an earlier pass called the big connectors "two ~35‑pin, both
> empty" and located CAN in a separate small "CONTROL PANEL" connector. Both
> wrong. They are 23‑pin AMPSEALs, one is populated, and "CONTROL PANEL" is a
> **tag around a wire group** inside it — not a connector.

### The M12 is Ethernet, not CAN
Per the PV1100 manual, the round **M12 (5‑pos, A‑coded)** carries Ethernet
(TX+/RX+/TX−/RX−/GND), not CAN. Consequences:
- Off‑the‑shelf "M12 Ethernet" cables are **D‑coded (4‑pin)** or X‑coded (8‑pin)
  and **will not mate** with this A‑coded 5‑pin jack. There is no ready‑made
  M12→RJ45 for it; you'd hand‑wire an A‑coded 5‑pin flying‑leads cable to RJ45.
- The port is an *optional* feature and may be dormant on this unit (like the
  Wi‑Fi/BT, which are inactive). An Ethernet/IP link to the QNX box (telnet/FTP)
  would be attractive, but its viability is **unconfirmed** — treat as a later
  experiment, not the first move.

## CAN — the two ways it's exposed

**Per the manual (Black Connector pinout), for reference:**

| Pin | Signal | Pin | Signal |
|---|---|---|---|
| 5 | CAN1 Low | 18 | CAN2 Low |
| 6 | CAN1 High | 19 | CAN2 High |
| 7 | Battery (+V) | 8 | Ground |

### As actually wired in this van (CONFIRMED 2026‑08‑11)

All 7 wires in the populated 23‑pin AMPSEAL, decoded against the manual:

| Pin | Wire | Signal | Wire group |
|---|---|---|---|
| **5** | **green** | **CAN1 Low** | sleeved |
| **6** | **yellow** | **CAN1 High** | sleeved |
| 7 | red | Battery (+V) | sleeved |
| 8 | gray | Ground | sleeved |
| 15 | red | Ignition Input | sleeved |
| **18** | **green** | **CAN2 Low** | tagged "CONTROL PANEL" |
| **19** | **yellow** | **CAN2 High** | tagged "CONTROL PANEL" |

**Both CAN buses are present.** The accordion‑sleeved group is CAN1 + power +
ignition; the 2‑wire group tagged "CONTROL PANEL" is CAN2.

Why this mapping is trustworthy — three independent things agree:
1. Every one of the 7 wires lands on a documented pin, with **no leftovers**.
2. Colors match J1939 convention (**green = Low, yellow = High**) on *both*
   pairs independently.
3. The groupings are electrically sensible: CAN1 sleeved together with its own
   power and ignition; CAN2 broken out separately to a panel.

> **Reading orientation:** as read from the wire‑entry side, rows appear
> **mirrored** left‑to‑right, and the row order runs bottom‑to‑top relative to
> the standard numbering. The table above is already corrected — don't re‑flip it.

**Tap CAN1 (pins 5/6, the sleeved pair) first.** If it's quiet, CAN2 (18/19) is
right there in the same connector.

## Tap procedure (non‑destructive)

1. **Verify** with a multimeter, van powered: CAN_H and CAN_L each idle
   ~2–3 V to ground (≈2.5 V nominal), with a small differential that flickers as
   traffic flows — *not* 0 V, *not* 12 V. Confirm green/yellow behave this way.
2. **T‑tap** (Posi‑Tap / gel quick‑splice, no cutting) onto the two wires:
   - CANable **CAN_H → yellow**, **CAN_L → green**, **GND → black / chassis**
3. **Termination OFF** on the CANable — the bus is already terminated at its
   ends; a third terminator would unbalance it.
4. **Listen‑only first.** Never transmit until we've confirmed we're reading the
   right bus cleanly.
5. `candump` and **validate the rig on a known frame** — the Rixen heater
   `0x724` (status) / `0x788` (command) from [`can-map.md`](can-map.md) — before
   trusting anything. Then flip one load at a time from the stock screen to
   isolate the PDM command frames (finishes [`pdm-control.md`](pdm-control.md)).
   If this pair is the quiet bus, move to the CAN2 (teal/gold) pair.

## Parts

| Part | Phase | Note |
|---|---|---|
| **CANable** USB‑CAN adapter (candleLight → native SocketCAN) | 1–2 | the listening rig; `candump`/`cansniffer` work out of the box |
| **T‑tap / Posi‑Tap** connectors for ~18–20 AWG | 1–3 | tap green/yellow without cutting |
| **Multimeter** | 1 | confirm the CAN pair before wiring |
| **Waveshare ESP32‑S3‑RS485‑CAN** industrial board | 3 | the companion box — see below |
| **Inline fuse holder + 1 A fuse** | 3 | on the box's +12V tap. Not optional. |

Not needed for this path: the M12 cable, or a mating breakout connector.

## The companion box (phase 3)

**Waveshare ESP32‑S3‑RS485‑CAN** industrial control board (~$22–25, ordered
2026‑08‑10). Chosen over a bare ESP32 + transceiver because the things that
matter in a vehicle are already on it:

| | |
|---|---|
| Power in | **7–36V** screw terminal (raw van 12V direct, no buck converter) + USB‑C 5V |
| CAN | **Isolated**, TVS/surge/ESD protected, screw terminals |
| Termination | 120Ω **NC by default**, enabled via jumper → leave it alone, bus is already terminated |
| Wireless | WiFi + Bluetooth 5 LE (ESP32‑S3 ⇒ BLE only, no BT Classic — irrelevant, iOS can't use SPP anyway) |
| Enclosure | DIN‑rail case included |
| Bonus | isolated RS485 — the van also runs MODBUS |

Rejected: bare ESP32 / XIAO ESP32C6 (**no CAN transceiver** — the SoC has a
controller, but that's only half of it — and no 12V input); Autosport Labs
ESP32‑CAN‑X2 (good board, dual CAN, but $55, JST pigtails, and termination is
**on** by default); Raspberry Pi (SD‑card corruption on unclean power cut is the
classic vehicle failure mode, plus ~30 s boot and 20× the current draw).

### Powering it

- **Bench / phase 2:** USB‑C from the laptop. Nothing to solve yet.
- **In the van / phase 3:** T‑tap **+12V and ground from the same CONTROL PANEL
  connector** you're already tapping for CAN — the red and black wires in that
  bundle. Two more taps in a bundle you're already in.
  - It then wakes and sleeps with the head unit → **no parasitic drain** while
    the van is stored.
  - The **isolated** CAN side means powering from here can't create a ground
    loop with the bus tap.

> **Fuse the +12V leg. 1 A, inline.** 24 AWG is fine for CAN (a signal, ~µA),
> but on an *unfused* 12V tap it becomes the fuse if it ever chafes — buried
> in a harness behind a panel. ~$8 removes the risk entirely.

**Meter the power pair too** — same session as the CAN check. Confirm which wire
actually reads ~12.6V and which is ground. Don't trust colour alone on power.

## Two paths, kept separate

- **CAN (this doc)** — CANable onto the CONTROL PANEL green/yellow pair. Confirmed
  live; all of `can-map.md` applies. **This is the path we're taking.**
- **Ethernet (M12)** — A‑coded 5‑pin cable hand‑wired to RJ45 → IP link to the
  head unit. Unproven (port may be dormant, non‑standard cabling). Later, maybe.

They use different hardware and don't combine.

---

## CONFIRMED: the van uses BOTH CAN buses, split by function (2026-08-11)

Toggling the inverter while capturing CAN1 produced **zero new CAN ids and zero
PDM byte changes**. The bus carried nothing about it at all.

| Bus | Pins | Carries |
|---|---|---|
| **CAN1** | 5/6 (sleeved green/yellow) | PDM1 + PDM2 (all switched loads), holding tanks (SA `AF`), A/C + vent (SA `58`), Rixen heater (std 11-bit) |
| **CAN2** | 18/19 ("CONTROL PANEL" tagged green/yellow) | **Lithionics BMS** (SA `46`), **inverter** (SA `E1`), **charger** (SA `E1`), shore-power circuit capacity |

Evidence that the energy subsystem is on CAN2, not merely idle:

- `0x18EF0046` / `0x18FF8046` (Lithionics BMS) — never seen on CAN1
- `0x19FFD4E1` (Inverter Status) / `0x19FFCAE1` (Charger AC Status) — never seen
- `0x19FFD300` (Inverter Command) — never seen, even while toggling the inverter
- `0x19FF9500` (`CircuitCapacity`, the "branch amps" shore-power limit) — never seen

All four are defined in the firmware's own CAN DB, so they exist; they are
simply on the other pair.

### Consequence for the companion box

**Battery state of charge — probably the single most wanted number on a van
dashboard — is on CAN2.** Any useful companion device needs both buses:

- CAN1 to control lights, pumps, fridge, fans and read tanks/climate
- CAN2 to read battery SoC, inverter and charger state

The **Waveshare ESP32-S3-RS485-CAN has one CAN interface.** Its second port is
RS485, which does not help here. Options, in order of preference:

1. **Autosport Labs ESP32-CAN-X2** (~$55) — two independent CAN controllers on
   one board, 6–20V automotive input. This is now clearly the right part; it
   was the runner-up earlier only because we could not yet show CAN2 was needed.
2. Two Waveshare boards, one per bus, bridged over WiFi. Cheaper if the first
   is already in hand, but two devices to power, mount and keep in sync.
3. Single Waveshare on CAN1 only — full control of loads, **no battery data**.

**Verify before buying:** tap CAN2 with the existing CANable first and confirm
the BMS and inverter are actually there. Moving two T-taps costs nothing.
