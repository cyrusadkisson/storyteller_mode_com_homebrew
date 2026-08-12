# storyteller_mode_com_homebrew

A homebrew project to build a better, phone-friendly interface for the
**Storyteller Overland (MY24) "MODE" van** — and, along the way, to document how
the stock system actually works.

The factory touchscreen is slow and unintuitive. Rather than modify the
locked/signed factory firmware (risky — the same computer runs the van's DC
electrical system), this project builds a **parallel companion controller** that
speaks the van's own control bus, leaving the stock system fully intact as a
fallback.

---

# ⚠️ DISCLAIMER — READ BEFORE USING ANY OF THIS

**USE ENTIRELY AT YOUR OWN RISK. NO WARRANTY OF ANY KIND IS OFFERED OR IMPLIED,
INCLUDING AS TO ACCURACY.**

This repository documents an **independent, amateur** reverse‑engineering effort
by a vehicle owner. It is **not** engineering guidance, not a service manual,
not vendor‑sanctioned, and has **not** been reviewed by anyone qualified to
review it. Some of it is certainly wrong.

Acting on this information can **damage your vehicle, destroy expensive
equipment, cause fire, cause serious injury, or kill you.** Specifically:

- **High‑voltage DC.** This van's house system is **48 V nominal (~53 V
  charged)**, not 12 V. A large lithium pack can deliver hundreds of amps into
  a short with no fuse in between. It does not care that you are careful. DC
  arcs do not self‑extinguish the way AC does.
- **Fire.** Improper taps, undersized conductors, and unfused connections are a
  fire risk in a vehicle you may be asleep inside.
- **Machinery.** These commands drive **motors, pumps, heaters and a furnace**.
  A command written to a motor channel **latches** — it keeps driving until
  something writes zero. Crashed software, a dropped connection, or a bug can
  leave an awning motor straining against its stop or a pump running dry.
- **Safety systems.** The bus documented here is shared with the battery
  management, inverter and charger. Interfering with a BMS can defeat
  protections that exist to prevent thermal runaway.
- **Warranty and insurance.** Tapping wiring or transmitting on the vehicle bus
  may **void your vehicle, appliance or battery warranty**, and may affect
  insurance claims. That is between you and them.

**Findings come from exactly one MY24 van.** Other model years, trim levels and
build configurations differ. A channel that is a light on this van may be
something else on yours. **Verify everything against your own vehicle** before
acting on it, and assume the mapping is wrong until you have proven otherwise.

**Do not use this on a vehicle you do not own**, and do not use it to interfere
with anyone else's property.

Nothing here has been verified by transmitting. Everything was obtained
**passively, by listening.** The authors and contributors accept **no liability
whatsoever** for any loss, damage or injury arising from use of this material.
If you are not prepared to own the consequences of your own actions on your own
vehicle, **do not proceed.**

---

## The system (what we're working with)

| | |
|---|---|
| Head unit | 3sigma / Enovation Controls display, part **HV1100‑GF‑T‑CR** |
| OS / SoC | QNX Neutrino on Renesas R‑Car M2 (ARM Cortex‑A15), 1280×768 |
| UI engine | `AppLoader` — data‑driven from a config bundle (not hard‑coded) |
| Control bus | **CAN** (J1939 / Enovation "CANPro"), plus MODBUS |
| Loads | Switched by **Power Distribution Modules** (PDM1/PDM2) — lights, pumps, awning, heater |
| Battery | **Lithionics** BMS, reported over J1939 |
| Firmware pkg | `.pv1` container: QNX boot image + gzip'd tar application + MCU hex |

See [`docs/architecture.md`](docs/architecture.md) for the full breakdown and
[`docs/reverse-engineering-log.md`](docs/reverse-engineering-log.md) for how we
got there.

## Why the CAN bus (and not USB/Wi‑Fi/Bluetooth)

- The cabinet **USB port is host‑only** (for update sticks) — a computer plugged
  in doesn't enumerate.
- The firmware contains **Wi‑Fi/Bluetooth** apps, but those radios are **not
  active** on this unit and there's no UI to enable them.
- The **CAN bus is always live** and is the real control surface. Every load is a
  named signal on it, and the firmware ships the full **signal dictionary**
  ([`DeviceInformationAll.pbuff`]) that names them all. That's our map.

Plan: a small gateway (e.g. ESP32 + CAN transceiver, or Pi + CAN HAT) taps the
bus and bridges it to a phone/web UI. **No firmware flashing → no brick risk.**

## Repo layout

```
docs/     project documentation & reverse‑engineering notes
tools/    scripts that operate on YOUR local copy of the firmware
app/      companion interface (added later)
```

## ⚠️ What is NOT in this repo (intentionally)

The Storyteller/3sigma firmware (`*.pv1`) and everything extracted from it
(binaries, `.pbuff`, screen images, `Configuration.bin`, …) are **proprietary,
copyrighted, signed software** and are **git‑ignored**. This repo contains only
our own analysis, documentation, and tooling. Keep your own firmware copy
locally; do not commit or redistribute it.

## ⚠️ Safety — read this before writing anything to the bus

This documentation is complete enough to **command real machinery**. That is
the point of it, and it is also the risk.

- **Motor and pump channels latch.** A value written to the awning motor or a
  pump **stays commanded until something writes `0x00`**. An app that sets a
  value and then crashes, loses its connection, or is force‑quit will leave a
  motor driving against its end stop, or a pump running dry, indefinitely.
  **Anything that writes to those channels needs a watchdog that zeroes them.**
- **Momentary vs latching is not predictable** from what a load does — this van
  has two pumps that behave oppositely. It must be observed per channel.
- **Read‑only first, always.** Bring the interface up in listen‑only
  (`tools/can_up.sh` refuses otherwise) and confirm you are on the right bus
  before considering transmission.
- The head unit shares this bus with the van's DC electrical system. Treat
  transmitting as a separate, deliberate step — not a continuation of sniffing.

Everything documented here was obtained **passively**, by listening. Nothing in
this repository has been verified by transmitting.

**If you transmit, you own what happens.** Bring the bus up listen-only, prove
you are on the right one, and treat the first transmitted frame as its own
deliberate project with a load chosen so the worst case is a light coming on —
not a motor moving, not a heater igniting, not a pump running.

## Not affiliated

This is an independent, unofficial project by a **Storyteller Overland owner**.
It is **not affiliated with, endorsed by, or supported by** Storyteller
Overland, Enovation Controls / Murphy, JET Technologies, Lithionics, Rixen, or
any other vendor named here. All trademarks belong to their respective owners.

Findings come from one MY24 van. Other model years and build configurations
will differ. Verify against your own vehicle before relying on anything here.
See the **disclaimer at the top of this file** — it is not boilerplate, and the
48 V system in particular is a genuine hazard rather than a formality.

## License

MIT — see [`LICENSE`](LICENSE). The license covers **this repository's own
analysis, documentation and tooling only**. It does not and cannot grant any
rights to the vendor firmware, which is not included here.

## Status

**CAN1 mapped and largely decoded. CAN2 confirmed and being decoded.**

| Area | State |
|---|---|
| `.pv1` firmware | unpacked, structure documented ([`architecture.md`](docs/architecture.md)) |
| Signal dictionary | 1758 signals decoded ([`signal-dictionary.md`](docs/signal-dictionary.md)) |
| Wire-level CAN DB | 30 messages / 129 signals from `Configuration.bin` ([`can-map.md`](docs/can-map.md)) |
| **PDM load control** | **protocol cracked, 10 channels confirmed on the wire** ([`pdm-control.md`](docs/pdm-control.md)) |
| **Climate** | **Rixen, thermostat and vent fully decoded** ([`climate-control.md`](docs/climate-control.md)) |
| Tanks | decoded, verified against the panel |
| **Battery / inverter / charger** | **CAN2 tapped and largely decoded** ([`energy-can2.md`](docs/energy-can2.md)) |
| Companion device | not started |

The van uses **two CAN buses split by function**: CAN1 (pins 5/6) carries house
loads, tanks and climate; CAN2 (pins 18/19) carries the battery, inverter,
charger and shore power. Both are 250 kbit/s. See
[`hardware-and-tap.md`](docs/hardware-and-tap.md) for the tap, and
[`energy-can2.md`](docs/energy-can2.md) for the energy bus.

A companion device needs **both** buses to be useful — state of charge, pack
current and temperature are only on CAN2.

Every claim is labelled by how strongly it is supported — `CONFIRMED` means
observed on the wire, `predicted` means derived but untested. See
[`data/pdm_channels.csv`](data/pdm_channels.csv).

[`DeviceInformationAll.pbuff`]: docs/signal-dictionary.md
