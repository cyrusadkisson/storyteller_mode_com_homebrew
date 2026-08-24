# T-2CAN-FD bench bring-up (2026-08-22/23)

The phase-3 hardware is the **LILYGO T-2CAN-FD** (ESP32-S3-WROOM-1U, two CAN
channels). This doc captures what it took to get it building, flashing, and
verified on the bench, and the two real firmware bugs in the LILYGO stock
example for this exact board that silently break any CAN work.

Status: **benched and verified** — both channels transmit/receive on the board's
own self-loop, channel A's frames are decoded by the Jhoinrch CANable at 500k,
and channel A transmits the van's real 29-bit extended PDM datagrams at the van's
bus speed (250 kbit/s), confirmed by the Jhoinrch (2026-08-24).

## Toolchain (Linux, x86_64)

- **PlatformIO Core 6.1.19** installed headless via [uv](https://docs.astral.sh/uv/)
  `uv tool install platformio` → `~/.local/bin/pio`. The stock
  `get-platformio.py` installer fails on this system because Python 3.14 shipped
  without `venv`/`ensurepip`; rather than `sudo apt install python3.14-venv` we
  used uv, which builds its own Python env without root.
- **Serial flashing**: board enumerates as `Espressif USB JTAG/serial debug unit
  (1d50:606f... no — that's the CANable) ` → actually `303a:1001`, shown as
  `/dev/ttyACM0`.
- **Serial output over USB-C**: `pio device monitor` needs an interactive TTY — in
  this shell it can't run it. To read the board's serial from a script, use a
  pyserial one-liner from the PlatformIO env: 
  
  ```bash
  PY=/home/codexuser/.local/share/uv/tools/platformio/bin/python3
  $PY -c 'import serial,time; s=serial.Serial("/dev/ttyACM0",115200,timeout=0.3); t0=time.time(); o=[]
  while time.time()<t0+6:
      d=s.read(s.in_waiting or 1)
      if d: o.append(d.decode(errors="replace"))
  print("".join(o))'
  ```
- **`dialout` group**: /dev/ttyACM0 is root:dialout, and the user account had to
  be added (`sudo usermod -aG dialout $USER`). **Crucially, logging out/in is not
  enough** — this machine's *systemd user session* predates the group change and
  everything (terminals, Claude Code's shells) hangs off it, so all descendants
  inherited the *stale* group list. Fixed by rebooting the machine.

## Board layout (schematic: `project/T-2Can-Fd_V1.0.pdf` in Xinyuan-LilyGO/T-2Can)

- Two CAN channels. **Each uses a Mornsun TD501MCAN *isolated* CAN transceiver
  module** — it has its own isolated bus side and its own `CANG` (isolated
  bus-side ground).
- Screw terminals per channel: `DGNDx`, `CANHx`, `CANLx`, `SGNDx`. 
  The transceiver's isolated reference (`CANG`) goes through a cap + 1 MΩ to
  board/signal ground, and the connector's 4th pin is `SGNDx`. When wiring to a
  peer on the bench, `SGNDx` is the right bus ground to tie (we wired to
  `SGNDA`).
- Each channel has its **own on-board 120 Ω termination resistor** — so for a
  two-node link, exactly one of you and the peer should be terminated.

## Stock example bugs that block any CAN work (this board)

The repo: `github.com/Xinyuan-LilyGO/T-2Can`, `examples/can/can.ino`, library
`libraries/Longan_CANFD`.

1. **Crystal is 40 MHz, but the library defaults to 20 MHz.**
   `pin_config.h` and the example call `begin(CAN_500K_5M)` (or
   `CAN20_500KBPS`); the library signature is
   `begin(uint32_t speedset, const byte clockset = MCP2518FD_20MHz)`. The
   schematic shows `X1 = 40MHZ±10ppm` on the MCP2518FD's OSC pins. Wrong clock
   means the actual wire bitrate is 2× the configured one. With the Jhoinrch at
   500k and the T-2CAN-FD at 1 Mbit, the Jhoinrch's controller sees every frame
   as errors → ERROR-PASSIVE. Fix:
   ```cpp
   Can_A.begin(CAN20_500KBPS, MCP2518FD_40MHz)
   ```
2. **The library always sends CAN-FD-format frames.**
   `begin()` does `speedset = bittime_compat_to_mcp2518fd(speedset);
   if (speedset > CAN20_1000KBPS) __flgFDF = 1;` — after the compat translation the
   speedset is a big number (`CANFD::BITRATE(500000, 0)` = 500000), which exceeds
   the *enum ordinal* of `CAN20_1000KBPS` (≈ 10-ish). So `__flgFDF` is set to 1
   for every classical bitrate, producing FD-format frames that a classic-only
   controller (the Jhoinrch / MCP2515 family) can't decode. Fix: set it back
   explicitly after begin():
   ```cpp
   Can_A.__flgFDF = 0;
   ```

The T-2CAN-FD firmware after those two patches runs **classic CAN at 500 kbit/s**
and both of these tests pass:

- **Self-loop**: wiring `CANHA↔CANHB`, `CANLA↔CANLB`, `SGNDA↔SGNDB` lets the
  two channels receive each other: channel A receives `0xBB` from channel B,
  channel B receives `0xAA 41 41 41 41 41 41 41 41` from channel A.
- **Jhoinrch link**: wiring channel A into the Jhoinrch (CANHA→CAN_H,
  CANLA→CAN_L, SGNDA→GND, R120 switch ON) and bringing `can0` up at 250 000 kbit/s
  classic, `candump` decodes `0AA [8] 41 41 41 41 41 41 41 41` cleanly.

## Van datagrams at the van's speed (2026-08-24)

The milestone: **prove channel A can emit the van's real frames — the same
datagrams we reverse-engineered off CAN1 — at 250 kbit/s classic, and that the
Jhoinrch decodes them as-is.** This is the bench rehearsal for the van
integration test (T-2CAN-FD spliced into CAN1, impersonating SA 0x11).

Firmware: `examples/bench250k/bench250k.ino` (built with `default_envs = bench250k`).
The van's IDs are 29-bit extended, so the `sendMsgBuf(id, ...)` **second argument
must be 1** (extended frame) — with `0` the MCP2518FD truncates the ID to 11 bits
and the wire shows a garbage standard ID. That bug bit us once; the fix was four
call-site edits from `ext=0` to `ext=1`.

Verified capture (`candump -tz can0` at `bitrate 250000`, two full 6s loop
cycles, zero errors):

```
can0  14EF1E11  [8] FC 00 00 00 7F 00 00 FF   # PDM1 cmd, cabin lights ON  (DO4=0x7F)
can0  14EF1E11  [8] FC 00 00 00 00 7F 00 FF   # PDM1 cmd, cabin lights OFF (DO4=0x00)
can0  14EF1F11  [8] FC 00 00 00 7F 00 00 FF   # PDM2 cmd
can0  14EF111E  [8] FC 00 00 00 00 7F 00 FF   # PDM1 status ID
```

Frames repeat every ~6s at ~1.7s spacing. Wire IDs and payload bytes match the
van's datagrams byte-for-byte. The T-2CAN-FD can now speak the van's language
before ever touching the real bus.

## Misc hard-won notes

- `candump` piped to `head` doesn't print until the process exits — capture to a
  file, or use `candump -L` / read the file afterwards; otherwise you get a blank
  screen even though frames are flowing.
- The T-2CAN-FD is a bare-PCB board (an enclosure still has to be made/bought); no
  cables ship with it — only the antenna.
