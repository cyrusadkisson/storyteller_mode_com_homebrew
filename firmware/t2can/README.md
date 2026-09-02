# firmware/t2can — the companion controller's firmware

The T-2CAN-FD (ESP32-S3) firmware, vendored in full: the app, the earlier
milestone sketches, and the MCP2518FD library it depends on. Derived from
[Xinyuan-LilyGO/T-2Can](https://github.com/Xinyuan-LilyGO/T-2Can).

The library is vendored rather than pulled from upstream because two bugs in
LILYGO's stock example for this board silently break CAN work on it — see
[`docs/t2can-bench.md`](../../docs/t2can-bench.md).

Build with `pio run` from this directory; flash with `pio run -t upload`.

## Contents

- `examples/app/` — **the current firmware: the phone UI.** Serves a WiFi AP
  with a captive portal and mDNS `van.local`, on a single page covering the
  switch toggles (input spoof), A/C, roof vent and inverter, over a live state
  layer (per-channel current, tanks, battery, temperatures, fault frames).
  `default_envs = app`. **Set your own WiFi credentials before flashing** —
  copy `examples/app/ap_secret.h.example` to `ap_secret.h` and edit it. That
  file is git-ignored, so real credentials stay out of the repository; without
  it the build falls back to the published placeholders and warns.
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

Requires PlatformIO Core ≥ 6.1. Set `default_envs` to whichever example you
are flashing — note the `src_dir` trap described above.

## Running it in the van

The board boots unattended: `setup()` brings up both CAN channels, the AP,
mDNS and the web server on every power-up. Nothing needs starting by hand, so
it can live on a van USB outlet.

Consequences while it is deployed rather than on a laptop:

- **No serial console and no flashing** without re-tethering it to USB. The
  build tag in the page header (top right) is the only way to tell which
  firmware is loaded — check it before assuming a change is live.
- **A laptop cannot reach the app** unless it joins the `VanCompanion` AP;
  `/api/state` is otherwise phone-only.
- The box lives and dies with that outlet. Whether a given van's USB stays hot
  in storage is worth checking — it decides between always-on and a small
  parasitic drain.

## Open question: long-run stability

Everything in the app has been verified by hand, over sessions of minutes,
with someone watching. Long unattended runs are **untested**: no evidence yet
on WiFi/AP stability over hours, task starvation, or the MCP2518FD wedging.

The footer's `frames A / B` counters are the check: if they are still climbing
after the box has sat powered for a day, it ran clean. If they are frozen (or
the AP is gone), it wedged, and the next step is a serial session on the laptop
to catch the boot/crash log.

## Diagnostic tooling

The CANable (`can0`, slcan/gs_usb) is not part of the running system — the
T-2CAN reads both buses itself. It stays part of the toolkit because **the
companion box cannot observe its own errors**, and that is the failure mode
that costs the most time. Bugs found only by watching the wire independently:

- an FD-frame index bug putting DO7-12 levels in slots 1-6, while the box's
  own state reported everything healthy,
- the real ~91 Hz head-unit re-assert rate, which invalidated
  dimming-by-injection,
- the A/C fan byte map, captured from panel presses,
- proof that the Rixen accepts writes and only loses them to the HU's
  re-assert,
- proof that the reading lights have no digital input at all.

Reconnect it for any protocol work, and especially before attempting
cut-and-stand-in, where an independent view of an inline rewrite is essential.
