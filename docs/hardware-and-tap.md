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

| Connector | In use? | Function |
|---|---|---|
| **"CONTROL PANEL"** (smaller Deutsch/AMPSEAL-family) | **Yes — the live one** | Power + **CAN** + I/O to the van |
| Two large ~35‑pin connectors | **No — empty** | Expansion (audio/video options Storyteller didn't wire) |
| Round **M12 5‑pin A‑coded** | (optional) | **Ethernet** — *not* CAN (see below) |
| Round connectors w/ red/blue/black; inline gray Deutsch DT | Yes | **Power / accessory** — not CAN |
| "PUSH BUTTON" Deutsch DT (green/white) | Yes | Pushbutton input — not CAN |

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

**As actually wired in this van:** the big connectors are empty, so CAN comes in
through the **"CONTROL PANEL"** connector. The reliable identification is by
wire, not pin number:

> **CAN1 = the green + yellow twisted pair** in the CONTROL PANEL connector —
> **yellow = CAN_H, green = CAN_L** (J1939 standard colors). A second twisted
> pair (teal + gold) is the likely **CAN2**.

Wire colors in a custom harness aren't guaranteed — **meter‑verify before
wiring** (see below).

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

| Part | Note |
|---|---|
| **CANable** USB‑CAN adapter (candleLight → native SocketCAN) | the adapter; `candump`/`cansniffer` work out of the box |
| **T‑tap / Posi‑Tap** connectors for ~18–20 AWG | tap green/yellow without cutting |
| **Multimeter** | confirm the CAN pair before wiring |

Not needed for this path: the M12 cable, or a mating breakout connector.

## Two paths, kept separate

- **CAN (this doc)** — CANable onto the CONTROL PANEL green/yellow pair. Confirmed
  live; all of `can-map.md` applies. **This is the path we're taking.**
- **Ethernet (M12)** — A‑coded 5‑pin cable hand‑wired to RJ45 → IP link to the
  head unit. Unproven (port may be dormant, non‑standard cabling). Later, maybe.

They use different hardware and don't combine.
