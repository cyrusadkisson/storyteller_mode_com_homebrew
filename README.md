# storyteller_mode_com_homebrew

A homebrew project to build a better, phone-friendly interface for the
**Storyteller Overland (MY24) "MODE" van** — and, along the way, to document how
the stock system actually works.

The factory touchscreen is slow and unintuitive. Rather than modify the
locked/signed factory firmware (risky — the same computer runs the van's DC
electrical system), this project builds a **parallel companion controller** that
speaks the van's own control bus, leaving the stock system fully intact as a
fallback.

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

## Status

Early. Firmware unpacked and understood; connectivity path decided (CAN tap);
**signal dictionary decoded** ([`docs/signal-dictionary.md`](docs/signal-dictionary.md),
[`data/signals.control.csv`](data/signals.control.csv)); **wire-level CAN map
extracted** — 30 messages / 129 signals from `Configuration.bin`
([`docs/can-map.md`](docs/can-map.md),
[`data/can_messages.csv`](data/can_messages.csv)). Next: reverse the PDM output
protocol (`PDM-Manager`) so lights/pumps are controllable, then a live capture
and gateway hardware.

[`DeviceInformationAll.pbuff`]: docs/signal-dictionary.md
