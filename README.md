# storyteller_mode_com_homebrew

A homebrew project to control a **Storyteller Overland (MY24) "MODE" van** from
a phone — and, along the way, to document how the stock system actually works.

The factory panel is fixed in the galley. This adds a second way in: a
**parallel companion controller** that speaks the van's own control bus, so
lights, water pump, roof A/C, vent and inverter can be operated from a phone
anywhere in or around the van, and battery, tank and temperature readings can
be seen without walking to the screen.

It does not modify the locked, signed factory firmware — that would be risky,
since the same computer runs the van's DC electrical system. The stock panel
keeps working exactly as it did and remains the fallback.

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

**This project transmits on the vehicle bus.** The mapping was built by
listening, but the companion controller commands lights, pumps, the roof A/C,
the vent and the inverter for real, and this repository documents how. The
authors and contributors accept **no liability whatsoever** for any loss,
damage or injury arising from use of this material. If you are not prepared to
own the consequences of your own actions on your own vehicle, **do not
proceed.**

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

- **The USB port doesn't lead anywhere.** There is a USB pigtail running from
  the back of the panel up into the overhead cabinet, and it looks like the
  obvious way in — but it is a **host** port, the same kind as on a laptop.
  It exists to read firmware updates and media off a **USB stick**. Plugging a
  computer into it connects two hosts together, which does nothing: neither
  side will talk. Tested directly — a Linux laptop on that port produced no
  new device at all, on plug or replug. There is no software fix; it is what
  the port is wired to be. (A USB device port, the kind a phone has, is what
  would have been needed.)
- **The Wi‑Fi and Bluetooth radios are dormant.** The firmware carries the
  apps for both — BLE, Bluetooth serial, a network launcher, `hostapd` — but
  the panel offers no setting to switch either on, and none has ever been seen
  broadcasting. Waking them would mean modifying the signed factory firmware,
  which is the exact risk this project avoids.
- The **CAN bus is always live** and is the real control surface. Every load is a
  named signal on it, and the firmware ships the full **signal dictionary**
  ([`DeviceInformationAll.pbuff`]) that names them all. That's our map.

The companion controller is a **LILYGO T-2CAN-FD** (ESP32-S3, two independent
CAN interfaces) that taps both buses and serves a phone web UI over its own
WiFi access point. The stock firmware is never modified — **no flashing, no
brick risk.** See [`hardware-and-tap.md`](docs/hardware-and-tap.md).

> **Change the AP password before you flash.** The board has no screen or reset
> button, so the SSID and password are compiled in
> ([`app.ino`](firmware/t2can/examples/app/app.ino), top of file) and the
> default is published in this repo. Anyone in radio range who has read the
> source could otherwise join and operate the van. The build prints a warning
> until you change it.

## Repo layout

```
docs/      project documentation & reverse-engineering notes
tools/     scripts that operate on YOUR local copy of the firmware
firmware/  the companion controller's own firmware (T-2CAN-FD)
data/      machine-readable CAN map and channel tables
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
analysis, documentation, tooling and firmware only**. It does not and cannot
grant any rights to the vendor firmware, which is not included here.

Third-party code included:

- [`firmware/t2can/libraries/Longan_CANFD/`](firmware/t2can/libraries/Longan_CANFD/)
  — MCP2518FD driver, © Longan Labs, MIT (its own `LICENSE` is included).
  Vendored because two bugs in the stock example for this board break CAN
  work; see [`docs/t2can-bench.md`](docs/t2can-bench.md).
- `firmware/t2can/libraries/private_library/pin_config.h` — LILYGO's pin
  definitions for this board, carried unmodified from
  [Xinyuan-LilyGO/T-2Can](https://github.com/Xinyuan-LilyGO/T-2Can). It has no
  license header of its own; it is 23 `#define`s of GPIO numbers — the board's
  physical wiring — and every sketch needs it to compile.

Protocol facts in [`docs/modewifi-analysis.md`](docs/modewifi-analysis.md) were
corroborated against [ModeWifi](https://github.com/changer65535/ModeWifi)
(GPL-3.0), an independent owner's project. **No code from it is used or
included here** — observations about a shared vehicle bus are not copyrightable
expression, and mixing GPL-3 code into this MIT repo is deliberately avoided.

## Status

**The companion controller is built and running in the van.** It taps both CAN
buses, serves a phone UI over its own WiFi AP, and commands the loads listed
below. The stock system is untouched and remains the fallback.

| Area | State |
|---|---|
| `.pv1` firmware | unpacked, structure documented ([`architecture.md`](docs/architecture.md)) |
| Signal dictionary | 1758 signals decoded ([`signal-dictionary.md`](docs/signal-dictionary.md)) |
| Wire-level CAN DB | 30 messages / 129 signals from `Configuration.bin` ([`can-map.md`](docs/can-map.md)) |
| PDM load control | protocol cracked, channels confirmed on the wire ([`pdm-control.md`](docs/pdm-control.md)) |
| Climate | Rixen, thermostat and vent fully decoded ([`climate-control.md`](docs/climate-control.md)) |
| Tanks | decoded, verified against the panel |
| Battery / inverter / charger | CAN2 tapped and decoded ([`energy-can2.md`](docs/energy-can2.md)) |
| **Companion controller** | **built, flashed, in the van** ([`firmware/t2can/`](firmware/t2can/)) |

### What the controller does

| Works | How |
|---|---|
| Cabin, cargo, aux, water pump, recirc | spoofs the wall-switch input; the head unit then toggles and holds the state itself |
| Roof A/C — off/cool/heat, compressor, fan auto/low/high, cool setpoint | direct write, echoed back by the A/C. **Cool setpoint only** — the heat setpoint is decoded from the bus but neither shown nor settable; use the panel for it. |
| Roof vent — lid, fan, airflow, speed | direct write, echoed by the vent |
| Inverter | single-shot latch on CAN2 |
| Read-only display | per-channel power draw, tank levels, battery voltage/current/SoC/temperature and time remaining, AC line voltage and frequency, cabin temperature, PDM fault flags, Rixen heater state |

The time-remaining figure is one place this app is simply more correct than
the panel: the BMS reports `0xFFFF` when it declines to estimate, and the
stock screen renders that sentinel literally as **45d 12h**. See
[`energy-can2.md`](docs/energy-can2.md).

**Deliberately not included**, each for a measured reason:

- **Dimming** — holding a brightness means out-transmitting the head unit
  continuously (~250 Hz, roughly +50% bus load). Rejected on bus-safety
  grounds; brightness stays on the factory panel.
- **Reading lights** — they have no digital input to spoof, and a direct write
  is overwritten within ~11 ms.
- **Rixen heater writes** — the heater accepts them in ~300 ms, but the head
  unit reverts them within ~5 s. Holding one would oscillate a diesel burner's
  setpoint, so the app reads the heater and does not command it.
- **Sink drain** — hold-to-run, and a parallel tap cannot sustain a
  hold-to-run switch: the PDM re-broadcasts it as released ~25 times a second,
  so a spoofed press only ever flickers. Left as a manual control by owner
  decision.
- **Awning lights and motor** — the awning is physically removed from this
  van, so neither has been confirmed working. The app exposes the light as a
  switch and the head unit's command byte responds, but no light exists to
  see; on a van that has one, verify it before trusting it. The motor protocol
  is decoded but untested, and it **latches** — anything driving it needs a
  watchdog that writes zero.

All of those would need an **inline** ("cut-and-stand-in") controller that owns
the channel outright, rather than a parallel tap.

### Buses

The van uses **two CAN buses split by function**: CAN1 (pins 5/6) carries house
loads, tanks and climate; CAN2 (pins 18/19) carries the battery, inverter,
charger and shore power. Both are 250 kbit/s. A companion device needs **both**
— state of charge, pack current and temperature are only on CAN2. See
[`hardware-and-tap.md`](docs/hardware-and-tap.md) for the tap and
[`energy-can2.md`](docs/energy-can2.md) for the energy bus.

Every claim is labelled by how strongly it is supported:
`CONFIRMED` means the frame was observed **and** the load was seen to respond;
`CONFIRMED-FRAME` means the frame is certain but the load could not be observed
(the awning channels — the hardware is absent from this van); `predicted` means
derived from the firmware but untested. See
[`data/pdm_channels.csv`](data/pdm_channels.csv).

[`DeviceInformationAll.pbuff`]: docs/signal-dictionary.md
