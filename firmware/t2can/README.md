# firmware/t2can — the companion-box firmware

Canonical home of the T-2CAN-FD (ESP32-S3) firmware. **This copy is the backup
of record** — the working copy lives in `/tmp/T-2Can` (a clone of
[Xinyuan-LilyGO/T-2Can](https://github.com/Xinyuan-LilyGO/T-2Can)) and is
copied here on milestone days. `/tmp` is scratch space; edit there, land here.

Vendored 2026-08-24 so a `/tmp` cleanup can't take the phase-2 work with it.

## Contents

- `examples/companion/` — **the current firmware.** Two-bus live: CAN-A
  (MCP2518FD) on the van's CAN1, CAN-B (TWAI, GPIO6/7) on CAN2. Input-spoofs
  the six verified wall switches, commands A/C (mode/setpoints/compressor),
  vent fan and inverter, reads tank levels + battery. Serial-command driven.
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
