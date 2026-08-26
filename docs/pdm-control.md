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

---

# CONFIRMED ON THE WIRE (2026-08-11)

The open question above is **answered**. A live capture plus an OFF→ON→OFF
capture-diff recovered the PDM command protocol that static analysis could not.

## Addressing

**PGN EF00 ("Proprietary A", PF=0xEF, PDU1 point-to-point), priority 5.**
One master and two slaves:

| SA | Node |
|---|---|
| `0x11` | head unit (master) |
| `0x1E` | PDM (slave) |
| `0x1F` | PDM (slave) |

| CAN ID | Direction | Role |
|---|---|---|
| `0x14EF1E11` | 0x11 → 0x1E | **command** |
| `0x14EF1F11` | 0x11 → 0x1F | **command** |
| `0x14EF111E` | 0x1E → 0x11 | status / feedback |
| `0x14EF111F` | 0x1F → 0x11 | status / feedback |

## Framing

**Byte 0 is a multiplexer.** The same CAN id carries several different
messages selected by byte 0 — you *must* demultiplex or the payloads smear
together and real transitions become invisible. (`tools/can_diff.py` does this
automatically; it reports `0x14EF1E11[FC]`.)

- **Command** frames use mux `FC` and `FD` — almost certainly channels 1–6 and
  7–12. Bytes 1–6 = per-channel level, byte 7 = `FF` constant.
- **Status** frames use `39 / C9 / F0 / F8 / FA / FB / FC / FD / FE`, with `FC`
  further sub-indexed on byte 1 (`F1`…`FC`, 12 items → per-channel feedback).
  Within `F0`/`F8` (the digital-input frames), the 2-bit switch slots live in
  bytes 6–7, while **bytes 4–5 are a 10-bit analog supply reading**
  (`((b4 & 3)·256 + b5)·5/1024` V) — so that byte drifts with the rail and must
  not be mistaken for a changing switch (decode from the ModeWifi code,
  re-audit 2026-08-24).
- **Fault frames:** `0x14E9111E` / `0x14E9111F` (PGN `E900`, one per PDM) are
  short/overcurrent warnings — any non-zero in bytes 2–3 means a channel
  faulted. (Also from the ModeWifi re-audit.)

Levels observed in command bytes: `00` (off), `40`, `5C`, `7F`. The non-power-
of-two values mean this is a **level (0x00–0x7F), not a bitfield** — consistent
with `SetPwmCommandMode` in the binary.

## First confirmed channel

| | |
|---|---|
| Command | `0x14EF1E11`, mux `FC`, **byte 4** — `0x00` off, `0x40` on |
| Status | `0x14EF111E`, mux `C9`, byte 1 bits 4–5 (`0x30`) and byte 5 bit 1 (`0x02`) |
| Method | OFF→ON→OFF; all three bytes tracked the load in both directions |

Channel map lives in [`data/pdm_channels.csv`](../data/pdm_channels.csv).

## Incidental finding — RETRACTED

An earlier pass claimed status mux `FB` byte 6 was supply voltage, because it
fell when the cabin light switched on. **The water-pump test showed it rising
when a load switched on** — the opposite direction. One correlation, called too
early. `FB` byte 6 drifts on its own; its meaning is **unknown**.

## What this does NOT yet establish

- Which physical load each byte drives (one confirmed so far; repeat the
  OFF→ON→OFF diff per load).
- Whether `0x40` is a brightness level or just this load's "on" constant —
  a dimmer sweep will settle it.
- Whether writing these frames actually actuates a load. **Nothing has been
  transmitted yet.** That is session #2 and carries real risk.

---

# THE FULL LOAD MAP (2026-08-11)

One confirmed channel plus the firmware's own signal dictionary decodes the
whole thing.

## The key that unlocked it

The dictionary names the cabin lights **`PDM1.DO4.Cabinlights`**. The live
capture put them on **byte 4**. So:

> **byte index = DO number**, and **PDM at SA `0x1E` is PDM1**, `0x1F` is PDM2.
> Command mux `FC` carries DO1–DO6 in bytes 1–6; mux `FD` carries DO7–DO12.

## Why this is trustworthy without testing every load

Five independent cross-checks: every channel the map says is powered on is a
subsystem we can *separately* see transmitting on the bus.

| Channel | Reads | Corroboration |
|---|---|---|
| PDM2 DO7 TankMonitorPWR | `7F` on | tank frames live, PGN 1FFB7 SA `AF` |
| PDM2 DO9 ACGatewayPower | `7F` on | A/C frames live, PGN 1FFE2 SA `58` |
| PDM1 DO11 FurnacePower | `7F` on | Rixen heater frames live, `0x724`/`0x788` |
| PDM2 DO3 Refrigerator | `7F` on | fridge is running |
| PDM2 DO2 GalleyFanSpeed | `5C`, drifting | behaves like a *speed*, not a switch |

A wrong byte→channel alignment would not produce five coincidences that all
agree with the rest of the bus.

## The map

Full table: [`data/pdm_channels.csv`](../data/pdm_channels.csv) — 24 channels,
15 named, 1 confirmed on the wire, 9 unnamed in the firmware dictionary.

**PDM1 — SA `0x1E`, command id `0x14EF1E11`**

| mux | byte | DO | Load |
|---|---|---|---|
| FC | 1 | DO1 | SolarBattBackup |
| FC | 2 | DO2 | CargoLights |
| FC | 3 | DO3 | ReadingLights |
| FC | **4** | **DO4** | **Cabinlights** ← CONFIRMED |
| FC | 5 | DO5 | AwningLights |
| FC | 6 | DO6 | RecircPump / BathLight |
| FD | 1 | DO7 | AwningEnabled |
| FD | 2–3 | DO8–9 | *(unnamed)* |
| FD | 4 | DO10 | ExhaustFan |
| FD | 5 | DO11 | FurnacePower |
| FD | 6 | DO12 | WaterPump |

**PDM2 — SA `0x1F`, command id `0x14EF1F11`**

| mux | byte | DO | Load |
|---|---|---|---|
| FC | 2 | DO2 | GalleyFanSpeed |
| FC | 3 | DO3 | Refrigerator |
| FD | 1 | DO7 | TankMonitorPWR |
| FD | 2 | DO8 | SwitchPower |
| FD | 3 | DO9 | ACGatewayPower |
| — | others | — | *(unnamed in dictionary)* |

## Status of each claim

- **CONFIRMED (1):** Cabinlights — OFF→ON→OFF, command and status echo both tracked.
- **PREDICTED (14):** byte position from the dictionary, on/off state matches
  observed value. Strong, but each still deserves a one-shot OFF→ON diff.
- **UNKNOWN (9):** no name in the firmware dictionary. Only discoverable by
  toggling and watching, or by leaving them alone.

**Values are levels, not flags:** `00` off, `7F` full, with `40` and `5C`
observed in between — matching `SetPwmCommandMode`. Dimming is available on
channels that support it.

## Second confirmation — the `FD` group (2026-08-11)

**WaterPump = PDM1 DO12 = `0x14EF1E11` mux `FD` byte 6**, `0x00` → `0x7F`.
Status echo on `0x14EF111E` mux **`0A`** byte 1 bits 0–1 (`3C` → `3F`).

Chosen specifically because it sits on the *other* mux frame — the `FD`
(DO7–DO12) group was entirely untested and a wrong split would have shown up
here. It didn't. Both mux groups are now verified against physical loads.

Note it goes straight to full scale with no intermediate value, unlike the
cabin lights (`0x40`). Consistent with a pump being a plain switch while
lighting channels carry a brightness level.

Note also that status echoes live in **different mux frames per channel group**
(`C9` for the cabin light, `0A` for the pump) — don't assume one status frame
covers every output.

## Dimming confirmed on hardware (2026-08-11)

25-second capture while sweeping the cabin-light brightness slider by hand:

- `0x14EF1E11[FC]` byte 4 took **91 distinct values, `0x06` → `0x7F`**
- **every other byte of that frame stayed constant** (byte1 `7F`, bytes 2/3/5/6
  `00`, byte7 `FF`) — the sweep is isolated to the one channel

So the command byte is a **continuous level, 0x00–0x7F (0–127)**. This settles
`SetPwmCommandMode` from the binary against real hardware.

> **Off is not the bottom of the range.** The slider's minimum is `0x06`
> (~5%), and `0x00` is a separate off state. A UI that maps a 0–100% slider
> straight onto 0x00–0x7F will never actually switch the light off, and will
> have a dead zone below ~5%. Treat off as its own command.

Pump vs. light is the useful contrast: WaterPump only ever goes `00` → `7F`
(a switch), while Cabinlights walk the whole range (a dimmer). Which channels
are dimmable has to be established per channel — the frame format is identical
either way.

## Map proven — 5 of 5 (2026-08-11)

Every channel tested landed on the byte the map predicted. No misses.

| Load | DO | Predicted byte | Observed | Value |
|---|---|---|---|---|
| CargoLights | DO2 | `[FC]` 2 | `[FC]` 2 | `00`→`40` |
| ReadingLights | DO3 | `[FC]` 3 | `[FC]` 3 | `00`→`40` |
| Cabinlights | DO4 | `[FC]` 4 | `[FC]` 4 | `00`→`40`, sweeps `06`–`7F` |
| AwningLights | DO5 | `[FC]` 5 | `[FC]` 5 | `00`→`40` |
| WaterPump | DO12 | `[FD]` 6 | `[FD]` 6 | `00`→`7F` |

Both mux groups, five distinct physical loads, derived purely from
`byte index == DO number` plus the firmware dictionary. The remaining
`predicted` rows in `pdm_channels.csv` rest on the same rule and should be
treated as reliable, though each is still marked honestly as untested.

All four lights switch on at **`0x40`** — the panel's remembered default
brightness, not a per-channel constant.

### Status frame layout (mux `C9`)

Byte 1 of `0x14EF111E[C9]` packs **2 bits per channel, high-to-low as the DO
number ascends**:

| bits | channel |
|---|---|
| 6–7 | DO3 ReadingLights |
| 4–5 | DO4 Cabinlights |
| 2–3 | DO5 AwningLights |
| 0–1 | DO6 |

Established from three independent loads landing on three adjacent bit-pairs
in the right order. DO2 (Cargo) produced no isolated echo in this byte, so the
lower channels live elsewhere in the frame; the water pump (DO12) reports in a
**different mux entirely** (`0A` byte 1 bits 0–1). Do not assume one status
frame covers all outputs.

**All four lighting channels (DO2 Cargo, DO3 Reading, DO4 Cabin, DO5 Awning)
have a brightness slider in the stock UI** — confirmed by the owner. So all
four carry a 0x00–0x7F level, and the cabin-light sweep result generalises to
the set. WaterPump (DO12) remains the only confirmed plain on/off channel.

## Presets decoded, and a sixth channel confirmed (2026-08-11)

Pressing **Preset 1** produced a single simultaneous change in
`0x14EF1E11[FC]` at t=1.91s of the capture:

| byte | channel | before | after |
|---|---|---|---|
| 1 | DO1 SolarBattBackup | `7F` | `7F` (unchanged) |
| 2 | DO2 CargoLights | `00` | **`7F`** |
| 3 | DO3 ReadingLights | `00` | **`7F`** |
| 4 | DO4 Cabinlights | `00` | **`7F`** |
| 5 | DO5 AwningLights | `00` | **`7F`** |
| 6 | DO6 RecircPump_BathLight | `00` | **`40`** |

The `FD` frame did not change at all.

The owner independently reported "all lights went full blast and the hot water
circ button lit up" — matching the decode channel-for-channel, including
**DO6**, which had never been tested. That is six confirmed channels.

**Presets need no new protocol.** A preset is the head unit writing stored
levels into the same command frame we already decoded:

```
Preset 1  ->  0x14EF1E11 : FC 7F 7F 7F 7F 7F 40 FF
```

A companion app can implement scenes by writing one frame. The same will apply
to Preset 2 and "chill mode" — capture each once to read off its stored levels.

> **Caveat (2026-08-25):** "writing one frame" sets the levels for exactly one
> HU cycle (~11 ms) and is then overwritten, same as any other direct output
> write. Scenes are only durable via the **input spoof** (press the switches
> the scene would press) or **cut-and-stand-in**. The captured preset levels
> below are still the right *reference* for what a scene should look like.

Note `0x7F` here vs `0x40` when switching a light on manually: `0x40` is the
panel's remembered per-light default, `0x7F` is what Preset 1 stores. Both are
just levels on the same scale.

## Physical switches are PDM *inputs* (2026-08-11)

The van's physical buttons are **PDM digital inputs (DI)**, not a separate
control path:

| Physical switch | Dictionary entry |
|---|---|
| "master", near driver | `PDM2.DI6.MasterLightSwitch` |
| galley aux1 | `PDM2.DI4.Aux1Switch` |
| galley awning lights | `PDM1.DI5.AwningLightSwitch` |
| galley awning arm | `PDM1.DI7.AwningEnable` |
| galley awning out / in | `PDM1.DI12` / `PDM1.DI11` |
| cargo / cabin lights | `PDM1.DI3` / `PDM1.DI4` |
| water pump, sink, furnace | `PDM2.DI5` / `DI9` / `DI7` |

Pressing one does **not** emit a new message type. The PDM reports the input,
the head unit runs its scene logic, and then writes the ordinary output frame
we already decoded.

**Emulating the switch input is how a parallel tap controls a load.** Writing
the output directly does not persist — the head unit overwrites it within
~11 ms — and holding it would mean out-transmitting the head unit. Spoofing
the input instead makes the HU change its own mind and hold the new state
itself. See `modewifi-analysis.md` §2. The corollary is the reading-light
finding below: a channel with **no** digital input cannot be controlled from a
parallel tap at all.

## Aux1 confirmed — perimeter lights (2026-08-11)

`0x14EF1F11[FD]` byte 6, `0x00` ↔ `0x7F`. The owner pressed the galley aux1
button three times inside one 15 s capture, giving OFF→ON→OFF→ON — a
self-contained confirmation cycle. No other byte on either PDM moved.

**The command snaps straight to `0x7F`** with no intermediate values, despite
the dictionary marking Aux1 as a `%` channel with `Aux1 Ramp` / `TM Aux1Ramp`
variables. So the soft-start ramp lives **inside the PDM**, not in the head
unit's command stream. A companion app writes the target level and gets the
fade for free; it does not need to animate the value.

## Master toggle is destructive (2026-08-11)

Master switch ON, captured with the four lights deliberately at different
levels:

| | DO2 Cargo | DO3 Reading | DO4 Cabin | DO5 Awning | DO6 Recirc |
|---|---|---|---|---|---|
| before | `4A` | `52` | `4D` | `55` | `00` |
| after | `40` | `40` | `40` | `40` | `40` |

**The master overwrites every channel with a flat `0x40`.** Per-channel
brightness is not preserved and not restored — the stock system simply discards
it. It also switches DO6 RecircPump on.

`0x14EF1F11[FD]` byte 6 (Aux1 / perimeter lights) was **unaffected**, so master
covers PDM1 lighting only.

### Opportunity for the companion app

This is a genuine improvement the app can make over the factory UI with no new
protocol: **snapshot the six PDM1 lighting bytes before a master-off and write
them back on master-on.** Same single frame, but the room comes back the way
you left it.

Still to capture: master **OFF** (presumably writes `00` across the same
channels — unverified), and the master **dimmer slider**, which decides whether
the master is a proportional scaler or another flat overwrite.

## Master dimmer slider (2026-08-11) — partially inconclusive

Sweep produced 99 distinct states on `0x14EF1E11[FC]`, ramping smoothly
`0x40 → 0x06 → 0x7F` and back. Findings:

- **All four lighting channels move in exact lockstep**, identical values
  throughout.
- **`DO6 RecircPump` never moves.** So the master *dimmer* covers only the four
  lights, while the master *toggle* also writes recirc. They are not the same
  control surface.
- `DO1 SolarBattBackup` untouched; PDM2 untouched.

**RESOLVED: the master is a flat level, not a scaler.** (2026-08-11)

The obvious test — spread the channels to different levels, then nudge master —
turns out to be impossible, and *why* it is impossible is the answer:

1. Engaging master writes `0x40` to all four channels, destroying the ratios
   before the dimmer can act on them.
2. Moving the master dimmer while master is **off** produces **no CAN traffic
   at all** — verified over a 20 s capture with the frame held constant at
   `7F 00 7F 23 00 00`. The slider is a UI-side value until master engages.
3. Moving it while master is **on** drives all four channels in lockstep to one
   identical value.

4. Re-tested deliberately (`master_on_scale`): master ON flattened
   `00 00 00 00` → `40 40 40 40 40`, then the dimmer swept all four in
   lockstep. The flatten has now been observed from **two different prior
   states** (`4A 52 4D 55` and all-zero), so it is unconditional.

There is never a per-channel ratio for the master to preserve. It applies a
single level to all four lights. An app should model it that way — or, better,
implement a real scaler, which the factory system does not have.

## Presets are stateless — confirmed independently

While sweeping the master up, the owner observed the panel light up **Preset 1**
the moment the lights reached full, and drop it again on the way back down.

At t=4.27 the frame read `7F 7F 7F 7F` with `DO6` at `0x40` — exactly the
Preset 1 levels decoded earlier. So the head unit computes `ActivePreset` by
**comparing current levels against stored preset values**, not by tracking which
button was last pressed.

Two consequences for the companion app:
1. Our Preset 1 decode is independently confirmed by observed UI behaviour.
2. Highlighting the "active" scene needs no state tracking — just compare the
   current frame against each stored preset.

## Momentary vs latching channels (2026-08-11)

Not every channel is a toggle. **`PDM2 DO11 SinkPump` is momentary** — the panel
writes `0x7F` only while the button is physically held and `0x00` the moment it
is released. Confirmed by the owner: "it's a must hold button, never stays
fully on."

This was initially misread as a UI double-fire when the capture showed
`00 → 7F → 00 → 7F` with a 160 ms gap. That gap was the owner briefly lifting
their finger.

> **Safety note for the companion app.** Command frames are broadcast
> continuously (~91 Hz). A momentary load stays on for exactly as long as the
> app keeps writing a non-zero level. An app that writes `0x7F` once and moves
> on will leave a **drain pump running indefinitely**. Momentary channels must
> replicate hold semantics, and should carry a failsafe timeout so a dropped
> connection or a crashed UI cannot leave a pump running.

Which other channels are momentary is **not yet established**. `WaterPump`
(PDM1 DO12) behaved as a latching toggle in its capture, but the awning motor
channels are obvious momentary candidates. Assume momentary until shown
otherwise for anything that moves or pumps.

## Bidirectional motor channel — the awning (2026-08-11)

**Prediction that failed:** PDM1 `DO8`/`DO9`, sitting right after
`DO7 AwningEnabled`, were the obvious candidates for awning motor out/in.
They never moved. The motor is on **PDM2 `DO5`** — a channel with no name in
the firmware dictionary at all.

Sequence captured (arm → hold out → hold in → awning lights):

| t | channel | value | action |
|---|---|---|---|
| 1.78 | PDM1 DO7 AwningEnabled | `00` → `7F` | arm on (latching) |
| 3.97 → 9.96 | **PDM2 DO5** | `00` → **`7F`** → `00` | OUT on, then off — 6.0 s apart |
| 11.96 → 15.96 | **PDM2 DO5** | `00` → **`80`** → `00` | IN on, then off — 4.0 s apart |
| 17.85 → 19.55 | PDM1 DO5 AwningLights | `00` → `26` → `00` | lights on/off |

### One channel, direction encoded in the value

`0x7F` = **+127**, `0x80` = **−128** in two's complement. This is a **signed
bidirectional command**, not two separate outputs:

| value | meaning |
|---|---|
| `0x00` | stop |
| `0x7F` | full one direction (out) |
| `0x80` | full other direction (in) |

Intermediate signed values presumably give proportional speed, though only the
extremes were observed.

**This retroactively explains `SetPositionCommandMode`** — the fourth control
primitive found in `PDM-Manager` during static analysis, alongside
`EnableOutput` / `DisableOutput` / `SetPwmCommandMode`. It made no sense for a
light or a pump. It is this: a bidirectional motor channel.

**LATCHING, not momentary.** The owner pressed once to start and once to stop —
the intervals above are the gap between two presses, not a hold. (Initially
misrecorded as momentary by analogy with the sink pump; corrected on the
owner's account of what they actually did.)

Gated: `DO7 AwningEnabled` must be set first, and it latches too — it stayed
`0x7F` after the sequence.

> **This makes a failsafe more important, not less.** A latching motor channel
> stays commanded until something writes `0x00`. An app that sets `0x7F` and
> then loses its connection, crashes, or is force-quit leaves a **large piece of
> machinery driving against its end stop indefinitely**. Any write to this
> channel needs a watchdog that zeroes it.
>
> The awning is physically removed from this van, so nothing moved here. On a
> fitted van it would have.

### Channel behaviour is per-channel and must be observed

| channel | behaviour |
|---|---|
| Lights DO2–DO5 (PDM1) | latching level, 0x00–0x7F |
| WaterPump DO12 (PDM1) | latching switch |
| **SinkPump DO11 (PDM2)** | **momentary** — hold to run |
| **AwningMotor DO5 (PDM2)** | **latching**, bidirectional |

Momentary vs latching is **not** predictable from the channel type — two pumps,
opposite behaviour. It has to be observed per channel.

## Not every channel is a control (2026-08-11)

Several channels sit at a constant value and have **no corresponding control on
the touchscreen**. Per the owner:

| channel | reads | reality |
|---|---|---|
| PDM2 DO3 Refrigerator | `7F` constant | operated by a **manual dial** on the fridge; no panel control |
| PDM2 DO2 GalleyFanSpeed | `60`, drifts | no panel control |
| PDM1 DO10 ExhaustFan | `7F` constant | no panel control |

These are **standing power feeds**, not command channels. The head unit energises
the circuit and the appliance manages itself downstream. Turning the fridge on
with its dial produces no CAN change, because the power was already present.

Marked `standing-feed` in `pdm_channels.csv`. **A companion app should not write
to them** — there is no user-facing behaviour to gain, and cutting a fridge feed
from a phone UI is a good way to spoil food.

This is a useful category distinction: *a channel existing in the map does not
mean it is something to expose in a UI.*

---

# FIRST TRANSMISSION (2026-08-12) — we can command loads

The project's last core unknown is closed. We transmitted on the live bus and
the PDM obeyed. Method, findings, and the design consequences:

## Setup

CANable on CAN1 (pins 5/6), brought up TX-capable via `./tools/can_up.sh --tx`
(script verifies TX mode by readback — `listen-only` is a *sticky* flag that
survives down/up unless explicitly cleared with `listen-only off`; an earlier
revision of the script lied about this).

**Baseline facts:** `0x14EF1E11[FC]` broadcasts continuously at **~91 Hz**
(~11 ms), in bursts rather than at a steady period. The head unit does not send
on change — it *re-asserts its entire state* on that cycle. Whole bus ~500 fps.
There is **no dependable quiet gap to inject into**, which is what rules out
holding a value from a parallel tap.

## Test 1 — no-op (PASSED)

Sent the live frame back byte-for-byte identical
(`cansend can0 14EF1E11#FC7F000026000000FF`). TX counter +1, every error
counter frozen. Proves the adapter can drive the bus without disturbing it.

## Test 2 — foreign source address (NEGATIVE, and important)

Sent "cabin off" bursts from **SA 0x12** (`0x14EF1E12`): first 30 frames over
0.3 s, then 200 frames over 2 s. Result: the PDM's status echo
(`0x14EF111E[C9]` byte 1 bits 4–5) stayed `0x30` throughout — **never a single
off sample**. No physical reaction.

**The PDM obeys ONLY source address `0x11` (the head unit).** A foreign
address is completely ignored, no matter the rate. (Upside of the test: even a
200-frame burst from a different id produced **zero bus errors** — different ids
arbitrate, they never collide into bit errors.)

## Test 3 — shadow injection, SA 0x11 (SUCCESS)

Tool: [`tools/can_shadow.py`](../tools/can_shadow.py). Listens for the head
unit's own frame, then transmits immediately **behind** it, from SA 0x11. The
gap is ~11 ms and bursty, so this lands a *single* change reliably but cannot
**hold** one — see "Dimming abandoned" below. Payload = copy of the live frame with exactly one
byte changed (byte 4 = Cabinlights). Timing off the observed frame keeps
collision odds near zero — same-id/different-data overlap is the one thing on
CAN that produces error frames.

- 45 injections, cabin → `0x00`: status byte 1 went `0x00` (off) on **12 of 75**
  samples during the window. The PDM's own broadcast confirmed the channel was
  switching off. **Zero bus errors.**
- 90 injections, cabin → `0x7F` (full): **visibly reacted** — the owner saw the
  cabin lights jump, unmistakably. **Zero bus errors.**

Totals for the day: ~370 transmitted frames, `bus-errors 0`, `error-warn/pass`
unchanged, no restarts.

## What this means for the companion box (phase 3)

1. **Writes must impersonate SA 0x11.** Only that address is obeyed.
2. **Nothing persists by itself.** The head unit re-asserts every ~11 ms, so a
   one-shot write holds for at most one cycle. Holding a state would mean injecting
   continuously at a rate that out-runs the HU — **rejected on bus-safety
   grounds, see "Dimming abandoned"**. The same physics is the safety net: a
   crashed box stops transmitting and the stock system is unaffected.
3. **Copy-then-modify is mandatory.** Every command frame carries six channels;
   only ever change the one byte you mean to, mirroring the rest of the live
   frame. Blind payload constants would switch off standing channels
   (SolarBattBackup reads `7F` on the FC frame).
4. **Physical response appears smoothed.** A ~1 s off burst was wire-visible but
   eyeball-missed; 2 s to full brightness was unmissable. Short injections get
   averaged out at the load. Hold times of ~1–2 s look like the practical
   minimum for a state to feel "real."
5. **Never fight the head unit at high rate from a foreign id** expecting the
   PDM to hear you (it won't), and never blindly spam SA 0x11 without the
   shadow timing, for collision-error reasons.

## A second strategy: spoof the wall-switch inputs (from ModeWifi)

Independent owner project ModeWifi confirms this entire map and adds a
complementary control method: instead of commanding outputs as the head unit,
**press the physical switches on the wire** — copy the PDM's digital-input
status frame (`0x14EF111E` mux `F0`/`F8`), set the switch's 2-bit field to
`0b10` for ~100 ms, release — sent from the **PDM's** address (SA `0x1E`),
which the head unit trusts for input data. The head unit then changes its own
state and holds it in its own ~91 Hz broadcast. Persistence without fighting
the chatter. Full analysis: [`modewifi-analysis.md`](modewifi-analysis.md),
including the **per-channel feedback-amps** frames (mux `F9/C9/39` and
`0A/CA/FA`, bytes 2–7 × 0.125 A) that make an app truly stateful.

## Dimming abandoned for the parallel tap (2026-08-25)

Tried and removed from the companion app. Shadow injection works for a
*momentary* change (bench Test 3 above) but cannot **hold** a level from a
parallel tap:

- The HU re-asserts `FC` at ~91 Hz, not the ~45 Hz assumed, and in bursts.
  One injection per received frame leaves the HU's own value standing for most
  of the duty cycle, so the lamp alternates between the old and new level —
  observed on the van as fast flicker, at three separate injection timings.
- Making our value dominate requires transmitting continuously at a higher
  rate than the HU (~250 Hz was the working figure), i.e. roughly +50 % total
  bus load, sustained, as same-id/different-data frames against the head unit.
  That is precisely the condition this document warns produces bus errors,
  and it is not an acceptable risk on a live vehicle bus for a comfort feature.

**Decision (owner, 2026-08-25): dimming is out of scope for the parallel tap.**
Brightness stays on the factory panel. The app shows each light's level
read-only. If dimming is ever wanted from the app it needs the
**cut-and-stand-in** architecture (box inline, owning the channel outright) —
a wiring change, not a firmware change.

Note this does not affect on/off: the wall-switch **input spoof** toggles
lights with no injection at all, and the HU holds the state itself. Only the
reading lights are unreachable that way (no physical switch on this van).

## Reading lights (DO3) — unreachable from a parallel tap (2026-08-25)

Measured directly. Owner toggled the reading lights ON, waited ~3 s, toggled
OFF, during a 20 s full-bus capture (8993 frames).

**Result:**

- `0x14EF1E11[FC]` byte 3 (DO3) moved `00` -> `7F` -> `00`. The load was
  commanded exactly as expected.
- **No digital-input frame moved.** PDM1 `F0` bytes 6-7 stayed `00 00` across
  the whole capture; PDM2 `F0`/`F8` unchanged. (PDM1 `F0` byte 5 drifted
  `A2`->`A3`, which is the 10-bit analog supply reading, not a switch.)
- A whole-bus diff of every id that changed payload during the window shows
  the change carried **only** on `0x14EF1E11` (the HU's own output command).
  The other movers are feedback-amps/analog muxes reacting to the load,
  tank/engine telemetry, and Rixen `0x788` chatter.

**Conclusion: the panel's reading-light control is internal to the head unit.**
It never appears on CAN1 as an input, so there is nothing to impersonate. The
output is re-asserted ~91x/second by the HU, so writing it does not persist.
Reading lights therefore **cannot be controlled from a parallel tap** —
this is a property of the channel, not a gap in the decode.

Note the distinction, since it is easy to state wrongly:

| Load | Why it is (or is not) controllable |
|---|---|
| A/C, roof vent | Separate J1939 nodes (SA `0x03`, `0x58`). They latch a command themselves — one frame, permanent. |
| Cabin, garage, awning light, aux, pump, recirc | PDM outputs **with a physical switch**. Spoof the input, the HU toggles and holds it. |
| Reading lights (DO3) | PDM output with **no input path at all**. Nothing to spoof; direct writes are overwritten in ~11 ms. |

"No physical switch" is not by itself the reason — the A/C has no switch either
and works fine. The reason is that a PDM output needs *someone* to hold it, and
for DO3 the only holder is the head unit, which takes no external instruction.

**Routes if it is ever wanted:** (1) **cut-and-stand-in** — box inline, owning
DO3 outright; (2) spoof the **master switch** (PDM2 DI6), which per the master
capture above does write DO3 — but master hits all four lights and flattens
them to `0x40`, so it is an all-lights scene, not a reading-light toggle.
