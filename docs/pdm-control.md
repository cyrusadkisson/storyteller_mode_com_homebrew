# PDM output control — lights, pumps and fans

The Power Distribution Modules switch the van's physical loads. This is the
protocol they speak, the map of which byte drives which load, and what a
companion device can and cannot do with it.

Unlike the RV appliances in [`can-map.md`](can-map.md), none of this is declared
in the firmware's configuration file. It comes from watching the bus.

---

## Addressing

PDM traffic is **PGN EF00** ("Proprietary A", PF `0xEF`, PDU1 point-to-point) at
priority 5. One master, two slaves:

| SA | Node |
|---|---|
| `0x11` | head unit — the master |
| `0x1E` | PDM1 |
| `0x1F` | PDM2 |

| CAN ID | Direction | Role |
|---|---|---|
| `0x14EF1E11` | `0x11` → `0x1E` | command to PDM1 |
| `0x14EF1F11` | `0x11` → `0x1F` | command to PDM2 |
| `0x14EF111E` | `0x1E` → `0x11` | status from PDM1 |
| `0x14EF111F` | `0x1F` → `0x11` | status from PDM2 |
| `0x14E9111E` / `0x14E9111F` | PDM → HU | fault frames (PGN E900) |

**The head unit re-asserts its entire output state at ~91 Hz** (about every
11 ms), in bursts rather than at a steady period. It does not send on change.
That single fact shapes everything a companion device can do.

## Framing

**Byte 0 is a multiplexer.** The same CAN ID carries several different messages
selected by byte 0. Demultiplex before diffing, or payloads smear together and
real transitions become invisible. `tools/can_diff.py` does this and reports
frames as `0x14EF1E11[FC]`.

**Command frames** use mux `FC` and `FD`:

| mux | bytes 1–6 | byte 7 |
|---|---|---|
| `FC` | outputs DO1–DO6, one level each | `FF` |
| `FD` | outputs DO7–DO12, one level each | `FF` |

**Byte index equals DO number.** The firmware dictionary names the cabin lights
`PDM1.DO4.Cabinlights`, and they sit on byte 4.

**Status frames** use mux `39`, `C9`, `F0`, `F8`, `FA`, `FB`, `FC`, `FD`, `FE`.
Which mux carries a given channel's echo varies — the cabin light reports in
`C9`, the water pump in `0A`. Do not assume one status frame covers every
output.

Inside the digital-input frames (`F0` / `F8`), the 2-bit switch slots live in
bytes 6–7. **Bytes 4–5 are a 10-bit analog supply reading**,
`((b4 & 3)·256 + b5) · 5/1024` volts, which drifts with the rail. It is not a
switch and will move on its own.

Per-channel current feedback arrives on mux `F9`/`C9`/`39` (channels 1–6) and
`0A`/`CA`/`FA` (channels 7–12), bytes 2–7, at **0.125 A per count**.

Fault frames carry short and overcurrent warnings: any non-zero in bytes 2–3
means a channel has faulted.

---

## Output levels

Command bytes are **levels from `0x00` to `0x7F`**, not flags. A brightness
sweep of the cabin light produced 91 distinct values from `0x06` to `0x7F`
while every other byte in the frame held still.

| value | meaning |
|---|---|
| `0x00` | off |
| `0x06` | the panel slider's minimum, roughly 5 % |
| `0x40` | the panel's default brightness — where all four lights switch on |
| `0x7F` | full |

> **Off is not the bottom of the range.** The slider bottoms out at `0x06`, and
> `0x00` is a separate off state. A UI mapping 0–100 % straight onto `0x00`–`0x7F`
> will never actually switch a light off and will have a dead zone below ~5 %.
> Treat off as its own command.

Whether a channel is dimmable is a property of the load, not the frame. The
format is identical either way: the water pump only ever goes `00` → `7F`, while
the lights walk the whole range.

A **soft-start ramp lives inside the PDM**, not in the command stream. Commands
snap straight to their target value; the fade happens downstream. Anything
writing a level gets the ramp for free.

---

## The channel map

Full table with per-channel evidence: [`data/pdm_channels.csv`](../data/pdm_channels.csv).

**PDM1 — SA `0x1E`, command ID `0x14EF1E11`**

| mux | byte | DO | Load |
|---|---|---|---|
| FC | 1 | DO1 | SolarBattBackup (standing feed, reads `7F`) |
| FC | 2 | DO2 | CargoLights |
| FC | 3 | DO3 | ReadingLights |
| FC | 4 | DO4 | Cabinlights |
| FC | 5 | DO5 | AwningLights |
| FC | 6 | DO6 | RecircPump / BathLight |
| FD | 1 | DO7 | AwningEnabled |
| FD | 2–3 | DO8–9 | unnamed in the dictionary |
| FD | 4 | DO10 | ExhaustFan (standing feed) |
| FD | 5 | DO11 | FurnacePower (standing feed) |
| FD | 6 | DO12 | WaterPump |

**PDM2 — SA `0x1F`, command ID `0x14EF1F11`**

| mux | byte | DO | Load |
|---|---|---|---|
| FC | 2 | DO2 | GalleyFanSpeed (standing feed) |
| FC | 3 | DO3 | Refrigerator (standing feed) |
| FC | 5 | DO5 | AwningMotor — unnamed in the dictionary |
| FD | 1 | DO7 | TankMonitorPWR |
| FD | 2 | DO8 | SwitchPower |
| FD | 3 | DO9 | ACGatewayPower |
| FD | 5 | DO11 | SinkPump |

Six channels have been driven and watched to respond: cargo, reading, cabin and
awning lights, the recirculation pump, and the water pump. The rest carry the
dictionary's name for their DO number, which is an inference from the
byte-equals-DO rule.

That rule holds up independently. Every channel the map shows as powered is a
subsystem separately visible on the bus:

| Channel | Reads | Cross-check |
|---|---|---|
| PDM2 DO7 TankMonitorPWR | `7F` | tank frames live, PGN 1FFB7 SA `AF` |
| PDM2 DO9 ACGatewayPower | `7F` | A/C frames live, PGN 1FFE2 SA `58` |
| PDM1 DO11 FurnacePower | `7F` | Rixen frames live on `0x724`/`0x788` |
| PDM2 DO3 Refrigerator | `7F` | the fridge is running |
| PDM2 DO2 GalleyFanSpeed | `5C`, drifting | behaves as a speed, not a switch |

**Check a channel before you rely on it.** The alignment is sound; an individual
name is only as good as the dictionary entry behind it. The CSV's `evidence`
column says which is which: `observed` means the load itself responded,
`inferred` means the byte position follows the rule but nothing was watched.

### Status frame layout (PDM1, mux `C9`)

Byte 1 packs two bits per channel, descending as the DO number ascends:

| bits | channel |
|---|---|
| 6–7 | DO3 ReadingLights |
| 4–5 | DO4 Cabinlights |
| 2–3 | DO5 AwningLights |
| 0–1 | DO6 |

DO2 produces no isolated echo in this byte, so the lower channels report
elsewhere in the frame. The water pump reports in mux `0A`, byte 1, bits 0–1.

---

## Channel behaviour

Three distinctions matter, and none of them can be predicted from what the load
is. The van has two pumps that behave oppositely.

### Latching, momentary, or a standing feed

| channel | behaviour |
|---|---|
| Lights DO2–DO5 (PDM1) | latching level, `0x00`–`0x7F` |
| WaterPump DO12 (PDM1) | latching switch |
| **SinkPump DO11 (PDM2)** | **momentary** — runs only while written |
| **AwningMotor DO5 (PDM2)** | **latching**, bidirectional |
| Refrigerator, ExhaustFan, GalleyFan, FurnacePower, SolarBattBackup | standing feeds |

**Standing feeds are not controls.** They sit at a constant value with no
corresponding control on the touchscreen. The head unit energises the circuit
and the appliance manages itself — the fridge is operated by a dial on the
fridge, and turning it produces no CAN traffic because the power was already
there. Do not write to these. Cutting a fridge feed from a phone is a good way
to spoil food.

### The awning motor is signed and bidirectional

One channel, direction encoded in the value:

| value | meaning |
|---|---|
| `0x00` | stop |
| `0x7F` | +127 — full one way (out) |
| `0x80` | −128 — full the other way (in) |

Intermediate signed values presumably give proportional speed; only the extremes
have been seen. `DO7 AwningEnabled` must be set first, and it latches too.

This is what the `SetPositionCommandMode` primitive in the PDM firmware is for —
it makes no sense for a light or a pump.

> **The awning is absent from the van these captures came from.** The frames are
> the head unit's own command stream, captured while pressing the panel's awning
> controls, but no motor or light was ever seen to respond. ModeWifi places the
> awning *lights* on PDM2 DO5 instead, and that disagreement is unresolved.
> Verify against your own van before driving anything here.

### Safety

> **A latching channel stays commanded until something writes `0x00`.**
>
> Software that sets a motor or pump running and then crashes, loses its
> connection, or is force-quit leaves it running indefinitely — an awning motor
> driving against its end stop, a pump running dry. **Anything that writes to
> these channels needs a watchdog that zeroes them.**
>
> A momentary channel runs for exactly as long as you keep writing a non-zero
> level, so it needs hold semantics plus a failsafe timeout.
>
> **Assume momentary until shown otherwise for anything that moves or pumps.**

---

## Scenes and the master switch

**Presets need no new protocol.** A preset is the head unit writing stored levels
into the ordinary command frame:

```
Preset 1  ->  0x14EF1E11 : FC 7F 7F 7F 7F 7F 40 FF
```

All four lights to full, recirculation to `0x40`. Capture each preset once to
read off its levels.

**Presets are stateless.** The head unit computes which preset is "active" by
comparing current levels against the stored values, not by remembering which
button was pressed — moving the master dimmer up until the lights reach full
lights up the Preset 1 indicator on the panel. An app can highlight the active
scene by comparing the current frame against each preset, with no state
tracking.

**The master switch is destructive.** Engaging it overwrites all four PDM1
lighting channels with a flat `0x40` and switches the recirculation pump on:

| | DO2 Cargo | DO3 Reading | DO4 Cabin | DO5 Awning | DO6 Recirc |
|---|---|---|---|---|---|
| before | `4A` | `52` | `4D` | `55` | `00` |
| after | `40` | `40` | `40` | `40` | `40` |

Per-channel brightness is not preserved and not restored. PDM2 is unaffected, so
master covers PDM1 lighting only.

**The master dimmer is a flat level, not a proportional scaler.** All four
lighting channels move in exact lockstep to one identical value; the
recirculation pump never moves. There is never a per-channel ratio for it to
preserve, because engaging master destroys the ratios first. Moving the slider
while master is off produces no CAN traffic at all — it is a UI-side value until
master engages.

---

## Controlling loads from a parallel tap

A device tapped across the bus is a second voice on a shared wire. What that
allows is narrower than the map above suggests.

### Writes must come from SA `0x11`

**The PDMs obey only the head unit's source address.** Bursts of 200 frames from
SA `0x12` produced no reaction at all — the status echo never showed a single
off sample. A foreign address is ignored regardless of rate.

Useful side effect: even a 200-frame burst from a different ID produces **zero
bus errors**. Different IDs arbitrate; they never collide into bit errors.

### Nothing persists by itself

The head unit re-asserts every ~11 ms, so a write from a parallel tap holds for
at most one cycle. Injecting behind the head unit's own frame lands a *single*
change reliably — enough to switch something, not enough to hold a level.

**Holding a level means out-transmitting the head unit** at roughly 250 Hz, about
+50 % total bus load, sustained, as same-ID/different-data frames. That is the
one condition on CAN that reliably produces error frames, and it is not a
reasonable thing to do to a live vehicle bus for a comfort feature.

The same physics is the safety net: a crashed companion device simply stops
transmitting, and the stock system carries on unaffected.

### Copy the frame, change one byte

Every command frame carries six channels. Mirror the live frame and change only
the byte you mean to. A blind payload constant switches off standing channels —
`SolarBattBackup` reads `7F` on the `FC` frame and would be cut.

### The method that actually works: spoof the switch input

Instead of commanding an output as the head unit, **press the physical switch on
the wire.** Copy the PDM's digital-input status frame (`0x14EF111E`, mux `F0` or
`F8`), set the switch's 2-bit field to `0b10` for about 100 ms, then release —
sent from the **PDM's** address (`0x1E`), which the head unit trusts for input
data.

The head unit then changes its own state and holds it in its own 91 Hz
broadcast. Persistence without fighting the chatter, and no sustained
transmission. This is how the companion controller toggles lights and pumps.
Details in [`modewifi-analysis.md`](modewifi-analysis.md).

### What a parallel tap cannot reach

**Dimming.** Requires holding a level. See above.

**Reading lights (PDM1 DO3).** They have no input path. Toggling them from the
panel moves the output byte `00` → `7F` → `00`, but **no digital-input frame
moves anywhere on either PDM** — verified across a 20-second full-bus capture of
8,993 frames, diffing every ID that changed. The panel's reading-light control is
internal to the head unit, so there is nothing to impersonate, and direct writes
are overwritten in ~11 ms.

The distinction is worth stating precisely, because it is easy to get wrong:

| Load | Why it is or is not controllable |
|---|---|
| A/C, roof vent | Separate J1939 nodes (SA `0x03`, `0x58`) that latch a command themselves. One frame, permanent. |
| Cabin, cargo, awning light, recirc, water pump | PDM outputs **with a physical switch**. Spoof the input; the head unit toggles and holds. |
| Reading lights | PDM output with **no input path**. Nothing to spoof. |

"No physical switch" is not by itself the reason — the A/C has no switch either
and works fine. The reason is that a PDM output needs *someone* to hold it, and
for DO3 the only holder is the head unit, which takes no external instruction.

If reading lights are wanted, the routes are an inline controller owning DO3
outright, or spoofing the master switch (PDM2 DI6) — which writes DO3 but hits
all four lights and flattens them to `0x40`, making it an all-lights scene
rather than a reading-light toggle.

### Response is smoothed at the load

A one-second off burst was visible on the wire but easy to miss by eye. Two
seconds to full brightness was unmistakable. Short injections average out at the
load; **one to two seconds is the practical minimum for a state change to look
real.**

---

## How the head unit builds these frames

`PDM-Manager` is a stripped ARM/QNX C++ program. Its control primitives map
directly onto what the bus shows:

| Method | Effect |
|---|---|
| `EnableOutput` / `DisableOutput` | output on / off |
| `SetPwmCommandMode` | the `0x00`–`0x7F` level — dimming |
| `SetPositionCommandMode` | the signed bidirectional motor channel |

It does not build CAN frames itself. It hands requests to a separate `j1939`
daemon over QNX message passing:

```
UI variable (PDM1.DO2.CargoLights)
   -> PDM-Manager (EnableOutput / SetPwmCommandMode)
      -> MsgSend to the j1939 daemon
         -> CAN frame to the PDM's address
```

The PDM layout is data-driven, loaded from configuration objects
(`conf::PdmOutput`, `conf::PdmInput`, `conf::PdmData`), which is consistent with
the dictionary's `PDM1.DOx` / `PDM1.DIx` naming. Each PDM claims its own J1939
address at runtime.

The command PGN and payload layout are not recoverable from the binary — the
target is derived from config plus address claim at runtime rather than stored as
a constant. That is why the protocol above comes from the wire.
