# Climate control — A/C, roof vent and hydronic heat

Climate is a separate subsystem from the PDM loads. Different nodes, a different
source address for the head unit, and — unlike the PDM's continuous broadcast —
an acknowledged request/response protocol.

Scale and offset values come from `Configuration.bin` (see
[`can-map.md`](can-map.md)) and match live capture.

## Nodes

| SA | Node |
|---|---|
| `0x03` | head unit, climate address — it uses `0x11` for the PDMs |
| `0x58` | A/C and vent controller |
| — | Rixen hydronic heater, on standard 11-bit IDs `0x724`–`0x78A` |

## Three different value ranges

Worth fixing in mind early, because mixing them up produces subtle bugs:

| subsystem | fan/level range |
|---|---|
| PDM outputs | 0–127 (`0x00`–`0x7F`) |
| A/C fan | 0–255 |
| Roof vent fan | 0–255; the panel sends up to `0xB9` (185), and the fan stops responding above roughly 200 |

---

# A/C thermostat

## `0x19FEF903` — thermostat command (head unit → A/C)

**Sent on change only**, which is why it never appears in an idle capture.

| bytes | field | scale |
|---|---|---|
| 0 | instance | `01` |
| 1 | operating mode + fan mode | see below |
| 2 | fan speed | `0x64` low → `0xC8` high, range 0–255 |
| 3–4 | **heat setpoint** | × 0.03125, offset −273 → °C |
| 5–6 | **cool setpoint** | × 0.03125, offset −273 → °C |

**Both setpoints travel in every command.** Heat and cool are set together.

### Byte 1

| bits | field | values |
|---|---|---|
| **0–3** | operating mode | `0` off · `1` cool · `2` heat · `4` compressor off, unit on |
| 4–5 | fan mode | `0` auto · `1` manual |
| 6–7 | unused | always `0` |

RV-C also defines `3` auto and `4` fan-only; only the values above have been
seen on this van.

**The compressor is separately controllable.** The A/C screen has its own
compressor switch, and it moves byte 1 between `0x01` (running) and `0x04` (unit
on, cooling element off). The status echo mirrors byte 1, so compressor state is
readable.

## Setpoints carry a ±2 °F deadband

**The head unit does not send the number on the panel.** It builds a deadband
around it, and only in the RV-C thermostat frame:

| value | relationship to the panel |
|---|---|
| thermostat **cool** setpoint | panel **+ 2 °F** |
| thermostat **heat** setpoint | panel **− 2 °F** |
| Rixen target | panel exactly |

> **App rule: cool = displayed + 2, heat = displayed − 2, Rixen = displayed.**
> Do not apply one blanket offset. An app that ignores this shows setpoints that
> disagree with the panel beside it, which reads as a bug whichever number is
> "right".

This is a deadband, not a scale error. Compared in raw counts rather than
rounded °F, the difference is a constant ~35 counts across the range:

| Panel | Wire | Raw count | Correct raw for panel value | Delta |
|---|---|---|---|---|
| 67 °F | 69.0 °F | 9393 | 9358 | 35 |
| 68 °F | 70.0 °F | 9411 | 9376 | 35 |
| 70 °F | 72.0 °F | 9447 | 9412 | 35 |

Each panel degree is 18 raw counts, so the panel steps in whole °F. Watching a
target move from 90 to 78 with the unit in heat shows both rules holding
11 degrees apart — thermostat heat at −2 throughout, Rixen target unmodified,
and the cool setpoint untouched, which confirms the two fields are independent.

The Rixen's trailing `.1 °F` is quantisation: it carries 0.1 °C units, so 78 °F
→ 25.6 °C → 78.08 °F.

## `0x19FFE258` — A/C status

The A/C mirrors mode, fan speed and both setpoints straight back, so **A/C
writes are verifiable**: send, then read the echo.

## `0x18E80358` — acknowledgement

```
00 ff ff ff ff f9 fe 01
^^ control 0x00 = positive ACK        ^^^^^^^^ PGN 0x01FEF9
```

The A/C acknowledges the thermostat command by PGN. Climate writes can be
verified two ways — the ACK and the status echo. PDM load commands have neither;
they are fire-and-forget broadcasts.

## What the A/C decides for itself

The head unit sets mode, setpoint and fan. **The unit's own anti-short-cycle
timer decides when the compressor actually starts**, and that delay — around
30 seconds — produces no CAN traffic at all.

---

# Roof vent

## `0x19FEA603` — vent command (head unit → vent)

```
02 15 B9 50 00 00 00 00
^^ instance (0x02)
   ^^ 0x15, constant
      ^^ fan speed
         ^^ mode bits
```

| byte | field | detail |
|---|---|---|
| 0 | instance | `0x02` |
| 1 | — | `0x15` in every frame |
| **2** | **fan speed** | `00` = off; range 0–255 |
| **3** | **mode bits** | bit 4 = lid open · bit 0 = air direction, 1 = in · bit 6 set in every frame |

**Fire once, not held.** A single frame starts the motion and the controller
drives to position on its own. Every command is acknowledged on `0x18E80358`
(control `0x00`, PGN `0x01FEA6`).

> **Send the full eight bytes.** Both this frame and the A/C's `0x19FEF903` are
> 8 bytes, and both begin with an instance byte that is easy to omit. A 7-byte
> vent command shifts every field by one: the fan does not respond at all. A
> 7-byte A/C command runs the low fan and nothing else. Neither produces an
> error — the frame is simply misread.

A full cycle:

| t | payload | action | status response |
|---|---|---|---|
| 0.00 | `02 15 00 51` | open | closed → moving at 3.60 → open at 14.13 |
| 13.80 | `02 15 7D 51` | fan on, speed 125 | fan = 125 |
| 18.14 | `02 15 41 51` | speed 65 | fan = 65 |
| 23.65 | `02 15 41 50` | air out | open + out at 24.16 |
| 39.18 | `02 15 41 51` | air in | open + in at 39.69 |
| 53.15 | `02 15 00 51` | fan off | fan = 0 |
| 56.67 | `02 15 00 41` | close | |

## `0x19FEA758` — vent status

| byte | field |
|---|---|
| 0–1 | instance / constant, mirroring the command |
| 2 | fan speed — see the caveat below |
| **3** | **state flags** — bit 4 open · bit 3 in motion · bit 2 position unknown · bit 0 air direction |
| **4–5** | temperature, × 0.03125 offset −273 → °C |

Byte 3 in practice:

| value | state |
|---|---|
| `0x00` | closed |
| `0x08` | moving |
| `0x10` | open |

Transit times: **open ~10–11 s, close ~15 s.**

> **Status byte 2 does not report the setpoint reliably.** It oscillates between
> the commanded speed and `0` on a rough 5–10 second cycle while the command byte
> holds steady. Either the fan physically cycles or the byte reports something
> else; unexplained either way.
>
> And **the setpoint is not on the bus while the fan is off** — the command is
> fire-once and never re-broadcast, and status byte 2 reads `00`. An app can only
> learn the speed while the fan runs, or remember what it set.

## The "in motion" flag lags the command by ~4 seconds

```
open  command at 0.00   ->  moving at 3.60
close command at 38.78  ->  moving at 42.89
```

**For about four seconds after a lid command the vent still reports its OLD
position with the motion bit clear** — indistinguishable from "settled" unless
you track that you just commanded it.

This causes a specific, repeatable bug. Any logic that mirrors the reported
position back into its own command state will silently undo the command, and the
next command — composed from that state — drives the lid the wrong way. Seen
twice: a fan-on closed an open vent, and an airflow flip reopened a closing one.

Rules for anything driving this vent:

1. Treat the lid as **three states** — closed, moving, open — not a boolean.
2. Adopt the reported position **only when settled**, and treat the four seconds
   after your own command as "moving" even though the vent does not say so.
3. Compose the mode byte **per command** from discrete state. Carrying one shared
   mutable mode word means every command re-sends whatever lid and direction bits
   happen to be in it.

## Byte 3 bit 2 — the lid position can be unknown

Bit 2 set means **the vent does not know where the lid is**, and bit 4 is then
meaningless. Reported values are `0x14` — bit 4 apparently "open" plus bit 2 —
while the lid is physically closed.

Commanding a close drives the lid to its limit, the vent regains certainty, and
the bit clears for good.

The likely cause is the controller losing stored position across a power
interruption. This is a single observation, so the cause is inferred — but the
correct handling does not depend on being right about it: **when bit 2 is set,
report the position as unknown rather than reporting a position**, and keep the
control enabled, because commanding a close is what fixes it.

---

# Rixen hydronic heater

## `0x724` — heater status

| bytes | scale | meaning |
|---|---|---|
| 0–1 | × 0.01 | current temperature °C |
| 2–3 | × 0.1 | target temperature °C |
| 4–5 | ×1 | unidentified, stable `d403` |
| 6 | bitfield | status flags — bit 3 set when calling for heat |
| 7 | ×1 | unidentified |

16-bit fields are little-endian. `0x78A` bytes 2–3 mirror `0x724` bytes 0–1.

## `0x788` — heater command, multiplexed on byte 0

Ten sub-commands share this ID, selected by byte 0, payload from byte 1.

| mux | sub-command | width | notes |
|---|---|---|---|
| `01` | **Set TempTarget** | 16-bit, 0.1 °C | echoed in `0x724` bytes 2–3 |
| `02` | **Set FanSpeed** | 8-bit | `0` → `15` with the furnace |
| `03` | **Set Furnace** | 8-bit | `0` → `1` on fuel heat |
| `06` | Set Hot Water | 1-bit | present in the stream, never seen changing |
| `0C` | **Send Amb Temp** | 16-bit, × 0.03125 offset −273 | see below |

`0C` is unambiguous: no other sub-command uses the `0.03125 / −273` scaling, and
it tracks `0x724` bytes 0–1 exactly. **The head unit reads ambient temperature
from the A/C (`0x19FF9C58`) and relays it to the heater.**

The remaining sub-commands in the database — Set Electric, Set Engine, Set
Preheat, Send Eng Run, Send Prime Fuel Time — have not been seen changing, so
their mux values are unassigned.

## Heat source selection

The panel's heat page engages the Rixen immediately, and a separate control
switches the source between **fuel**, **elec** and **dual**. On the wire that
hands the job between two entirely separate subsystems:

```
switch to HEAT (fuel)            switch fuel -> elec
  RIXEN[03] 0 -> 1  furnace on     RIXEN[03] 1 -> 0  furnace off
  RIXEN[01] -> 322  target 90F     RIXEN[02] -> 0    fan off
  RIXEN[02] -> 15   fan speed      RIXEN[01] -> 1
  THERMOSTAT mode -> 0 (off)       THERMOSTAT mode -> 2 (heat), fan 0x1E
```

Fuel and elec are mutually exclusive — only one is commanded at a time. `dual`
presumably commands both; not captured.

## Writes are accepted; holding them is the problem

**The Rixen does not filter by sender.** Unlike the PDMs, which obey only
SA `0x11`, it acts on any correctly framed command. What defeats a write is the
head unit re-asserting its own value:

```
t=2.70s   send target 80.1 F
t=3.00s   heater accepts   -> 0x724 target reads 80.1 F   (~300 ms)
t=5.63s   head unit re-asserts 78.1 F
t=6.00s   heater reverts   -> 0x724 target reads 78.1 F
```

**The head unit continuously re-asserts the heater's whole state.** Each `0x788`
sub-command repeats roughly every 5–6 seconds, staggered rather than in one burst
— over 24 seconds, 12 bursts with a mean 2.18 s gap, each carrying one or two
sub-commands. Unlike the roof vent's fire-once command, this frame is repeated.

A one-shot write therefore holds for about three seconds.

> **Why a companion app should read this and not write it.**
>
> Injecting at 1–2 Hz would be cheap on bus load, unlike PDM dimming. The problem
> is that the steady state is **contention, not takeover**: your value and the
> head unit's alternate every few seconds.
>
> On a lighting channel that is flicker. On a **diesel burner** it is a setpoint
> oscillating several times a minute, and the Rixen's internal hysteresis and
> minimum-run behaviour are not characterised here at all. Short-cycling a
> combustion heater is a wear and safety question that cannot be bounded from the
> bus.
>
> Real control needs an inline controller presenting one coherent setpoint, never
> parallel injection.

## What is worth reading

| Source | Field |
|---|---|
| `0x724` b0–1 | current cabin temperature (× 0.01 °C) |
| `0x724` b2–3 | target (× 0.1 °C) — no deadband; the Rixen gets the panel's number |
| `0x724` b6 bit 3 | calling for heat |
| `0x788[02]` | heater fan |
| `0x788[03]` | furnace |
| `0x788[06]` | hot water |

A cross-check worth knowing: `0x788[0C]` ambient matches `0x724` current
temperature to the hundredth, which exercises both decodes at once.

---

# Not established

- **`0x19FDE203`** (Vent Control 2) — never observed, across a session covering
  fan on/off, two speed changes, air direction and open/close. Everything went
  out on `1FEA6`. It may address a second vent this van does not have, or a
  feature the UI does not expose.
- **`0x789`** HC_SetIO (bitfield plus a 32-bit field) — never observed.
- **`0x788` mux `06`** (hot water) — present but never seen changing.
- **Whether A/C heat and Rixen heat share a limit.** The thermostat heat setpoint
  reached 88 °F while the Rixen target reached 90 °F. Consistent with the −2 °F
  deadband, but not tested at the top of the range.
- **`dual` heat mode** — not captured.
