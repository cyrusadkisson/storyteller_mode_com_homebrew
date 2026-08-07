# PDM output control (lights, pumps, fans) — how it works

The Power Distribution Modules switch the physical loads. Unlike the RV
appliances (see [`can-map.md`](can-map.md)), the PDM output commands are **not**
declared in `Configuration.bin`. This documents what static analysis of the
`app/PDM-Manager` binary established, and — honestly — where static analysis
runs out and a live capture takes over.

## Architecture (established)

`PDM-Manager` is a stripped ARM/QNX C++ program. Its symbols and strings reveal:

- **Device classes:** `PDM::PDM12V`, `PDM::PDM24V`, `PDM::PDMBase` — the van has
  12 V and 24 V PDM variants.
- **Config objects:** `conf::PdmOutput`, `conf::PdmInput`, `conf::PdmData` — the
  PDM layout is data-driven, loaded from the configuration (consistent with the
  signal dictionary's `PDM1.DOx` / `PDM1.DIx` naming).
- **Control primitives (the actual per-output API):**
  | Method | Effect |
  |---|---|
  | `EnableOutput` / `DisableOutput` | turn an output **on / off** |
  | `SetPwmCommandMode` | **PWM / dimming** control (this is how the dimmable lights get their brightness) |
  | `SetPositionCommandMode` | position / analog-style output control |
- **Transport:** a `pdmJ1939` / `j1939 External API`. PDM-Manager does **not**
  import J1939 send functions directly — it uses QNX message passing
  (`MsgSend`/`MsgReceive`/`MsgReply`) to hand a request to the separate `j1939`
  daemon, which does the actual CAN framing. So the command path is:

  ```
  UI variable (e.g. PDM1.DO2.CargoLights)
     -> PDM-Manager (EnableOutput / SetPwmCommandMode on a PDM12V/PDM24V object)
        -> MsgSend to j1939 daemon
           -> CAN frame to the PDM's J1939 address, on can0/can1
  ```

Each PDM claims its own J1939 source address via address claim (the config
carries a J1939 NAME table: `Port, SourceAddress, IdentityNumber,
ManufacturerCode, ECUInstance, Function, …`).

## Where static analysis stops (and why)

The exact **command PGN and payload byte/bit layout** for a PDM output could not
be recovered statically here, for concrete reasons:

1. **No hardcoded PGN.** The command PGN/target is derived from the PDM config
   objects + J1939 address claim at runtime, not stored as a code constant.
   Scans of `PDM-Manager`'s immediates (`movw`/`movt`), literal pools, and
   initialized data turned up no clean PGN/CAN-id constant for output commands.
2. **No ARM disassembler available in this environment** (system `objdump` lacks
   ARM; no `llvm-objdump`/Capstone/`pip`, box is offline), so the frame-building
   code inside `EnableOutput`/`SetPwmCommandMode` couldn't be traced.

This is expected: `PDM-Manager` is compiled logic, not a declarative table like
`Configuration.bin` was.

### Correction to an earlier lead
`0x53f80018` (previously flagged in `can-map.md` as a CAN constant to check) is
**not** a CAN id. Its strings are `out32 0x53f80018` / `in32 0x53f80018` — a
memory-mapped **SoC register** access (hardware poke), unrelated to the bus.

## Finishing this: two ways

**A. Live capture (decisive, and needed anyway).** With a CAN adapter on the bus:
1. Log both channels (`candump can0` / `can1` under SocketCAN).
2. Prove the rig against a **known** frame first — the Rixen heater on `0x788`
   and status `0x724` from `can-map.md`.
3. Toggle **one** load at a time from the stock screen (e.g. cargo lights on/off,
   then dim it) and diff the capture. The frame that appears is the PDM command;
   on/off vs. a ramping value distinguishes `EnableOutput` from
   `SetPwmCommandMode`. Repeat per output to build the PDM command table.

**B. Proper disassembly (fills in the theory before the bench).** On a
connected machine, load `PDM-Manager` into Ghidra or `capstone`/`llvm-objdump`
(ARM), find the `EnableOutput` / `SetPwmCommandMode` methods and the struct
passed to `MsgSend`, and read off the PGN/priority/dest and payload template.
This is a tooling gap in the current environment, not a dead end.

## Status

Architecture and control primitives: **known**. Exact PDM command frame:
**pending** a live capture or an ARM disassembler. Everything needed to command
the RV appliances (heater, AC, inverter) is already in `can-map.md`; the PDMs
(lights/pumps/fans) are the one remaining protocol to pin down.
