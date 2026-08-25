# firmware/t2can — the companion-box firmware

Canonical home of the T-2CAN-FD (ESP32-S3) firmware. **This copy is the backup
of record** — the working copy lives in `/tmp/T-2Can` (a clone of
[Xinyuan-LilyGO/T-2Can](https://github.com/Xinyuan-LilyGO/T-2Can)) and is
copied here on milestone days. `/tmp` is scratch space; edit there, land here.

Vendored 2026-08-24 so a `/tmp` cleanup can't take the phase-2 work with it.

## Contents

- `examples/app/` — **the current firmware: the phone UI.** WiFi AP
  `VanCompanion` (pw `storyteller`), captive portal + mDNS `van.local`, single
  page wrapping every verified control (6 switch toggles via input spoof,
  A/C + setpoint + compressor, vent, inverter) over a live state layer
  (per-channel levels + feedback amps, tanks, battery, temps, fault frames).
  `default_envs = app`.
- `examples/companion/` — the serial-command precursor to `app`. Same two-bus
  core and commands, driven over the USB serial console instead of WiFi.
- `examples/bench250k/` — bench milestone: emits the van's real 29-bit PDM
  datagrams at 250k, verified by the CANable.
- `examples/listen/`, `examples/listenb/` — CAN1 / CAN2 dumpers.
- `examples/spoof/`, `examples/twobus/` — earlier one-off milestones.
- `libraries/Longan_CANFD/` — vendored MCP2518FD library (MIT). Needed
  because two stock-firmware bugs are worked around at the call sites:
  40 MHz crystal (`MCP2518FD_40MHz`) and the library forcing CAN-FD frames
  on classical bitrates (set `__flgFDF = 0` after `begin()`). See
  [`docs/t2can-bench.md`](../../docs/t2can-bench.md).
- `platformio.ini` — note the `src_dir = examples/${platformio.default_envs}`
  trap: building `-e <env>` compiles the DEFAULT env's example unless
  `default_envs` is changed first.

Build: `pio run` (PlatformIO Core ≥ 6.1) from `firmware/t2can/`, with
`default_envs` set to whichever example you're flashing.

## Deployment state (2026-08-25)

The box is **installed on the van and running on van USB power**, no longer
tethered to the laptop. It boots unattended: `setup()` brings up both CAN
channels, the AP, mDNS and the web server on every power-up. Nothing needs
starting by hand.

Consequences while it is deployed:

- **No serial console and no flashing** without bringing it back to the
  laptop's USB. The build tag in the page header (top right) is the only way
  to tell which firmware is loaded -- check it before assuming a change is
  live.
- **The laptop cannot reach the app.** It is not on the `VanCompanion` AP;
  only the phone is. `/api/state` is phone-only until someone joins the AP.
- The box lives and dies with that USB outlet. Whether it stays hot while the
  van sleeps is **unverified** -- it decides whether the app is always-on or
  only awake with the outlet.

## Open question: long-run stability

Everything in the app has been verified by hand, with an owner watching, over
sessions of minutes. **It has never run unattended.** No evidence yet on:
WiFi/AP stability over hours, task starvation, or the MCP2518FD wedging.

The footer's `frames A / B` counters are the check: if they are still climbing
after the box has sat powered for a day, it ran clean. If they are frozen (or
the AP is gone), it wedged, and the next step is a serial session on the laptop
to catch the boot/crash log.

## Diagnostic tooling

The CANable (`can0`, slcan/gs_usb) was **disconnected 2026-08-25** once the app
was working. It is no longer part of the running system -- but it stays part of
the toolkit, because the companion box cannot observe its own errors. Every
significant bug found on 2026-08-25 needed an independent view of the wire:

- the FD-frame index bug (levels for DO7-12 landing in slots 1-6),
- the real ~91 Hz HU re-assert rate, which invalidated dimming-by-injection,
- the A/C fan byte map (captured from panel presses),
- the reading-light negative result (no input frame moves at all).

Reconnect it for any protocol work, and especially before attempting
cut-and-stand-in, where an independent view of an inline rewrite is essential.
