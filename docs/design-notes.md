# Design notes — why it works this way

Why the companion controller works the way it does. None of this is needed to
install or use it; all of it matters if you intend to change it.

---

## Why the CAN bus, and not USB / Wi-Fi / Bluetooth

**The USB port doesn't lead anywhere.** A USB pigtail runs from the back of the
panel into the overhead cabinet, and it looks like the obvious way in. It is a
**host** port — the same kind as on a laptop — there to read firmware updates
and media off a stick. Connecting a computer joins two hosts, which does
nothing; neither side will talk. Tested directly: a Linux laptop on that port
produced no new device at all, on plug or replug. There is no software fix.
A USB *device* port, the kind a phone has, is what would have been needed.

**The Wi-Fi and Bluetooth radios are dormant.** The firmware carries the apps
for both — BLE, Bluetooth serial, a network launcher, `hostapd` — but the panel
offers no setting to enable either, and neither has ever been seen broadcasting.
Waking them means modifying signed factory firmware, which is the exact risk
this project exists to avoid.

**The CAN bus is always live**, and it is the real control surface. Every load
is a named signal on it, and the firmware ships the full signal dictionary
(`DeviceInformationAll.pbuff`) that names them all. That is the map.

---

## Two buses, split by function

| bus | pins | carries |
|---|---|---|
| CAN1 | 5 / 6 | house loads, tanks, climate, vent |
| CAN2 | 18 / 19 | battery, inverter, charger, shore power |

Both run at 250 kbit/s. A companion device needs **both**: state of charge,
pack current, pack temperature and per-cell voltages exist only on CAN2, while
every switchable load lives on CAN1. See
[`hardware-and-tap.md`](hardware-and-tap.md) and
[`energy-can2.md`](energy-can2.md).

---

## Parallel tap, not inline

The controller is a second voice on a shared wire — like adding a light switch
to a room rather than rewiring the first one. The head unit keeps transmitting
throughout, and the factory panel remains fully functional and is the fallback.

That choice is what makes the project safe, and it is also the source of every
limitation below. An **inline** controller — one that cuts the bus and stands in
between — could own a channel outright and do all of these. It could also brick
the van's DC electrical system, which is why it was not built.

### What a parallel tap cannot do

- **Dimming.** Holding a brightness means out-transmitting the head unit
  continuously at roughly 250 Hz, about +50 % bus load. Rejected on bus-safety
  grounds.
- **Reading lights.** They have no digital input to spoof, and a direct write is
  overwritten within ~11 ms.
- **Rixen heater writes.** The heater accepts a command in ~300 ms, but the head
  unit reverts it within ~5 s. Holding one would oscillate a diesel burner's
  setpoint, so the app reads the heater and never commands it.
- **Sink drain.** Hold-to-run. The PDM re-broadcasts the switch as released
  about 25 times a second, so a spoofed press only flickers.
- **Updating the factory screen.** The panel renders its own state. It sees
  spoofed *switch inputs* (so a light toggled in the app appears on the screen),
  but a direct write — A/C, vent — does not move the panel's display.

### Awning: decoded, unverified

The awning is absent from the van these captures come from. The light is exposed
as a switch and the head unit's command byte responds, but no light exists to
observe. The
motor protocol is decoded and untested, and it **latches** — anything driving it
needs a watchdog that writes zero.

---

## Safety, for anyone transmitting on the bus

This documentation is complete enough to command real machinery. That is the
point of it, and also the risk.

- **Motor and pump channels latch.** A value written to the awning motor or a
  pump **stays commanded until something writes `0x00`**. Software that sets a
  value and then crashes, disconnects, or is force-quit leaves a motor driving
  against its end stop or a pump running dry, indefinitely.
- **Momentary vs latching is not predictable** from what a load does — the van
  has two pumps that behave oppositely. It must be observed per channel.
- **Read-only first, always.** Bring the interface up listen-only
  (`tools/can_up.sh` refuses otherwise) and confirm the bus before transmitting.
- The head unit shares this bus with the van's DC electrical system. Treat
  transmitting as a separate, deliberate step, not a continuation of sniffing.

Choose the first transmitted frame so the worst case is a light coming on — not
a motor moving, not a heater igniting, not a pump running.

---

## How channel evidence is recorded

[`data/pdm_channels.csv`](../data/pdm_channels.csv) carries an `evidence` column,
because the difference matters before you drive something:

| value | meaning |
|---|---|
| `observed` | the frame was seen **and** the load responded |
| `frame-only` | the frame is certain; the load could not be watched (the awning) |
| `inferred` | from the firmware dictionary and the byte-equals-DO rule, untested |
| `standing-feed` | a permanently energised circuit, not a control |
| `unknown` | no name in the dictionary |

Anything below `observed` is a lead. Check it on your own van before relying on
it, and especially before writing to it.

---

## The stock system

| | |
|---|---|
| Head unit | 3sigma / Enovation Controls display, part **HV1100-GF-T-CR** |
| OS / SoC | QNX Neutrino on Renesas R-Car M2 (ARM Cortex-A15), 1280×768 |
| UI engine | `AppLoader` — data-driven from a config bundle, not hard-coded |
| Control bus | CAN (J1939 / Enovation "CANPro"), plus MODBUS |
| Loads | Power Distribution Modules (PDM1/PDM2) — lights, pumps, awning, heater |
| Battery | Lithionics BMS, reported over J1939 |
| Firmware pkg | `.pv1` container: QNX boot image + gzip'd tar application + MCU hex |

Full breakdown in [`architecture.md`](architecture.md).

---

## What is deliberately not in this repository

The Storyteller / 3sigma firmware (`*.pv1`) and everything extracted from it —
binaries, `.pbuff`, screen images, `Configuration.bin` — are proprietary,
copyrighted, signed software, and are git-ignored. This repository contains only
original analysis, documentation and tooling. Keep your own firmware copy local;
do not commit or redistribute it.
