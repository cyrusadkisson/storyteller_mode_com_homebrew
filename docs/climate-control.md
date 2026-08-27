# Climate control — decoded

Climate is a **separate subsystem** from the PDM loads. Different nodes,
different source address for the head unit, and — unlike the PDM's continuous
broadcast — an **acknowledged** request/response protocol.

All scale/offset values below come from `Configuration.bin` (see
[`can-map.md`](can-map.md)) and were **confirmed against live capture** on
2026‑08‑11, cross‑checked against the owner's independently stated cabin
temperature of 90 °F.

## Nodes

| SA | Node |
|---|---|
| `0x03` | head unit, **climate** address (note: it uses `0x11` for the PDMs) |
| `0x58` | A/C + vent controller |
| — | Rixen hydronic heater, on standard 11‑bit ids `0x724`–`0x78A` |

## Rixen hydronic heater

### `0x724` — HC_Status (RX)

| bytes | scale | meaning | observed |
|---|---|---|---|
| 0–1 | ×0.01 | **current temperature °C** | `5d0c` = 31.65 °C = 89.0 °F → `950c` = 32.21 °C = **90.0 °F** |
| 2–3 | ×0.1 | **target temperature °C** | `6400` = 10.0 °C = 50 °F → `4201` = 32.2 °C = **90.0 °F** |
| 4–5 | ×1 | (unidentified, stable `d403`) | |
| 6 | bitfield ×8 | status flags — bit 3 set when heat requested | `00` → `08` |
| 7 | ×1 | (unidentified) | |

16‑bit fields are **little‑endian**.

`0x78A` bytes 2–3 mirror `0x724` bytes 0–1 (a status echo).

### `0x788` — command, multiplexed on byte 0

Ten sub‑commands share this id, selected by byte 0. Payload starts at byte 1.

| mux | sub‑command | width | confirmed |
|---|---|---|---|
| `01` | **Set TempTarget** | 16‑bit, units 0.1 °C | ✅ raw 100 = 50 °F → raw 322 = 90 °F, echoed in `0x724` b2‑3 |
| `02` | **Set FanSpeed** | 8‑bit | ✅ tracks the furnace: `0` → `15` on, `0` when switched away |
| `03` | **Set Furnace** | 8‑bit | ✅ `0` → `1` on fuel heat, `1` → `0` on switch to electric |
| `06` | Set Hot Water *(inferred)* | 1‑bit | present in the stream, **never observed changing** |
| `0C` | **Send Amb Temp** | 16‑bit, ×0.03125, offset −273 → °C | ✅ 31.66 °C → 32.22 °C = 90.0 °F |

`0C` is confirmed beyond doubt: no other sub‑command carries the
`0.03125 / −273` scaling, and the decode lands exactly on the observed cabin
temperature. Its value tracks `0x724` bytes 0–1 — **the head unit reads ambient
from the A/C (`0x19FF9C58`) and relays it to the heater.**

The remaining sub‑commands from the DB — Set Electric, Set Engine, Set Preheat,
Send Eng Run, Send Prime Fuel Time — have not yet been observed changing, so
their mux values are unassigned.

## A/C thermostat

### `0x19FEF903` — Tx Thermostat_Command (head unit `0x03` → A/C)

**Sent on change only**, which is why it never appeared in idle captures.

| bytes | field | scale | observed |
|---|---|---|---|
| 0 | instance | — | `01` |
| 1 | mode nibble + 2×2‑bit fields | — | `10` |
| 2 | fan / mode | — | `00` |
| 3–4 | **heat setpoint** | ×0.03125, −273 → °C | 48.0 °F → **88.0 °F** |
| 5–6 | **cool setpoint** | ×0.03125, −273 → °C | 75.0 °F → **72.0 °F** |

Both setpoints travel in every command — heat and cool are set together.

### Byte 1 decoded (2026‑08‑11)

| bits | field | observed |
|---|---|---|
| **0–3** | **operating mode** | `0` = OFF, `1` = COOL — confirmed by switching the A/C on and off |
| 4–5 | fan mode | `0` → `1` at the moment the fan was set by hand → **auto / manual** |
| 6–7 | (unused so far) | always `0` |

RV‑C defines the remaining modes as `2` heat, `3` auto, `4` fan‑only. Only `0`
and `1` have been observed on this van.

### Byte 2 = fan speed

`0x64` (low) → `0xC8` (high). A **0–255** range, like the vent — *not* the
PDM's 0–127.

### `0x19FFE258` Rx AC Unit Status echoes the command

The A/C mirrors mode, fan speed and both setpoints straight back. So — like the
vent — **A/C writes are verifiable**: send, then read the echo.

### The compressor is autonomous — and invisible

An 85 s capture in which the owner *heard* the compressor start ~30 s in
contained **exactly one climate frame change**: the A/C being switched off at
t = 77. Nothing at the 30 s mark, on any climate id.

Cabin was 91 °F against a 72 °F setpoint, so the unit was always going to run;
the ~30 s delay is its own anti‑short‑cycle timer, not a command.

> **The head unit sets mode, setpoint and fan. The A/C decides when to run the
> compressor, and does not report it.** A companion app can show mode, setpoints,
> fan speed and cabin temperature — but **cannot show whether the compressor is
> actually engaged.**

**Correction 2026-08-24:** the compressor **is** independently controllable on
this van. The A/C screen has a separate compressor "AC OFF"/"AC ON" switch, and
pressing it changes `0x19FEF903` **byte 1**: `0x01` = A/C on (compressor runs),
`0x04` = compressor AC OFF (unit stays on, cooling element off), `0x00` = unit
off. The status echo `0x19FFE258` mirrors byte 1, so compressor state *is*
readable (unlike the earlier conclusion). The earlier "invisible" conclusion
likely missed this because the compressor-on/off UI was not exercised.

### CONFIRMED: the panel displays 2 °F lower than it transmits

**Verified live 2026‑08‑12.** With the A/C running, the panel displayed a cool
setpoint of **70 °F** while `0x19FEF903` bytes 5–6 carried **72.0 °F**. The
owner read the number off the screen at the moment of capture, so this is not
a misremembered value.

**The offset is specific to the RV‑C thermostat frame.** The Rixen does not do
it: a target of 90 °F set on the panel produced exactly `90.0 °F` in
`0x788[01]`. So this is the head unit's thermostat translation, not its
temperature handling in general.

> **A companion app must compensate**, or it will display setpoints that
> disagree with the panel beside it — which reads as a bug regardless of which
> number is "right".

### It is a constant offset, not a scale error (2026‑08‑12)

Tested across three setpoints, with the panel value read off the screen each
time:

| Panel | Wire | Offset | Raw count | Correct raw for panel value | Delta |
|---|---|---|---|---|---|
| 67 °F | 69.0 °F | **+2.0** | 9393 | 9358 | 35 |
| 68 °F | 70.0 °F | **+2.0** | 9411 | 9376 | 35 |
| 70 °F | 72.0 °F | **+2.0** | 9447 | 9412 | 35 |

Compared in **raw counts** rather than the rounded °F conversion, the error is
~35 counts at every setpoint. A scale error would grow with temperature; this
does not.

Each panel degree = **18 raw counts** (`9393 → 9411 → 9447`), so the panel steps
in whole °F.

> **App rule: `displayed = wire − 2 °F`** for the A/C cool setpoint.

Likely deliberate rather than a bug — offsetting the command so the A/C's own
hysteresis band centres on the requested temperature.

**Limit of the evidence:** the test spans 3 °F. A scale error small enough to
stay under half a degree across that range cannot be strictly excluded.

**Still unknown: does the offset apply to the heat setpoint?** The heat field
read 88 °F during the Rixen session, but the panel's A/C heat value was not
recorded at the time.

### `0x18E80358` — J1939 Acknowledgement (A/C `0x58` → head unit `0x03`)

```
00 ff ff ff ff f9 fe 01
^^ control = 0x00 = positive ACK      ^^^^^^^^ PGN 0x01FEF9 = 1FEF9
```

The A/C **acknowledges the Thermostat_Command by PGN**. Consequence for the
companion app: **climate writes are verifiable.** Send a command, watch for the
ACK. PDM load commands have no such handshake — they are fire‑and‑forget
broadcasts.

## Vent

### `0x19FEA603` — Tx Vent Control (head unit `0x03` → vent, SA `0x58`)

**Fully decoded 2026‑08‑11** from a 60 s session covering fan on, two speed
changes, air direction, fan off and close. **Eight commands, eight ACKs.**

```
02 15 B9 50 00 00 00 00
^^ instance (0x02)
   ^^ 0x15 constant
      ^^ FAN SPEED
         ^^ MODE BITS
```

| byte | field | detail |
|---|---|---|
| 0 | instance | `0x02` |
| 1 | — | `0x15`, constant in every frame |
| **2** | **fan speed** | `00` = off. Observed `2B`, `7D`, `B9` — exceeds `0x7F`, so a full **0–255** range (unlike the PDM's 0–127) |
| **3** | **mode bits** | **bit 4** = vent open(1)/closed(0) · **bit 0** = air direction, in(1) · **bit 6** set in every frame (enable / manual flag) |

Observed sequence:

| t | payload | action |
|---|---|---|
| 0.00 | `02 15 B9 50` | fan on |
| 6.36 | `02 15 2B 50` | speed down |
| 10.05 | `02 15 7D 50` | speed up |
| 15.90 | `02 15 7D 51` | air **in** (bit 0 set) |
| 33.90 | `02 15 00 51` | fan off (speed → 0) |
| 38.78 | `02 15 00 41` | **close** (bit 4 cleared) |

Only two bits of byte 3 ever moved, which is what makes the assignment safe.

**Confirmed on a second independent run (2026-08-11)** covering the full cycle
including *both* air directions:

| t | payload | action | status response |
|---|---|---|---|
| 0.00 | `02 15 00 51` | open | CLOSED → MOVING at 3.60 → OPEN at 14.13 (**~10 s**) |
| 13.80 | `02 15 7D 51` | fan on, speed 125 | fan=125 |
| 18.14 | `02 15 41 51` | speed 65 | fan=65 |
| 23.65 | `02 15 41 **50**` | **air OUT** | status `OPEN+OUT` at 24.16 |
| 39.18 | `02 15 41 **51**` | **air IN** | status `OPEN+IN` at 39.69 |
| 53.15 | `02 15 00 51` | fan off | fan=0 |
| 56.67 | `02 15 00 41` | close | *(capture ended)* |

Both states of bit 0 are now directly observed — the earlier "out is inferred"
caveat is resolved.

**Fire‑once, not held.** A single frame starts the motion; the controller drives
to position on its own. Every command acknowledged by `0x18E80358`
(`control 0x00`, PGN `0x01FEA6`).

### `0x19FEA758` — Rx Vent Status 2

| byte | field |
|---|---|
| 0–1 | instance / constant, mirroring the command |
| 2 | fan speed — echoes the command, but **oscillates between the setpoint and `0` on a rough 5–10 s cycle** while the command byte holds steady. Either the fan physically cycles, or this reports something other than the setpoint. **Unexplained.** |
| **3** | **state flags** — bit 4 = open · **bit 3 = in motion** · bit 0 = air direction |
| **4–5** | **temperature**, ×0.03125 offset −273 → 32–34 °C (≈90–93 °F), drifts while the fan runs |

Close transit, fully timed:

```
38.78  close command sent
42.89  byte3 = 0x09   bit 3 set  -> in motion
53.41  byte3 = 0x01   bit 4 clear -> closed      (~15 s)
```

Open transit: ~10–11 s, measured on two independent runs.

### The "in motion" flag lags the command by ~4 s

Visible in both runs above, and the cause of two app bugs:

```
open  command at 0.00  ->  MOVING at 3.60   (3.6 s)
close command at 38.78 ->  MOVING at 42.89  (4.1 s)
```

**For ~4 s after a lid command the vent still reports its OLD position, with
bit 3 clear** — indistinguishable from "settled" unless you track that you just
commanded it. Any logic that mirrors the reported position into its own command
state will silently undo the command, and the next command sent (composed from
that state) drives the lid the wrong way. Observed twice: a fan-on closed an
open vent, and an airflow flip reopened a closing one.

Consequences for anything driving this vent:

1. Treat the lid as **three states** — closed / moving / open — not a boolean.
2. Adopt the reported position **only when settled**, and treat the ~4 s after
   your own command as "moving" even though the vent does not say so yet.
3. Compose the mode byte **per command** from discrete state (lid, direction).
   Carrying one shared mutable mode word means every command re-sends whatever
   lid/direction bits happen to be in it.

### Fan speed byte 2 — practical range

The protocol range is 0-255 and the panel was observed sending up to `0xB9`
(185). The fan stops responding above roughly **200**, so the app's slider is
capped there. Note also that **the setpoint is not on the bus when the fan is
off**: the command frame is fire-once and never re-broadcast, and status byte 2
reads `00`. A companion app can only learn the speed while the fan runs, or
remember what it set.

## Still open

- `0x19FDE203` (Vent Control 2) — **still never observed**, despite a session
  covering fan on/off, two speed changes, air direction and open/close. All of
  those went out on `1FEA6`. It may address a second vent this van does not
  have, or a feature not exposed in this UI.
- The thermostat heat setpoint reached **88 °F** while the Rixen target reached
  **90 °F**. Either an intermediate value was captured mid‑adjustment, or A/C
  heat and Rixen heat have different limits. Unresolved.
- `0x788` mux `02` / `03` / `06` assignments are inferred from the DB ordering,
  not confirmed by decode. Toggling fan speed and furnace separately would
  settle them.
- `0x789` HC_SetIO (bitfield + 32‑bit field) never observed.


---

## Heat: source selection and the deadband (2026-08-12)

### Mode 2 = HEAT confirmed

`0x19FEF903` byte 1 low nibble reached **`2`** when the overhead unit was set to
heat. Observed values on this van are now `0` off, `1` cool, `2` heat.

### "Fuel / elec / dual" is a source selector between two systems

The panel's heat page engages the **Rixen** immediately, and a second control
switches the source between `fuel`, `elec` and `dual`. On the wire that hands
the job between two entirely separate subsystems:

```
switch to HEAT (fuel)          switch fuel -> elec
  RIXEN[03] 0 -> 1  furnace ON   RIXEN[03] 1 -> 0  furnace OFF
  RIXEN[01] -> 322  target 90F   RIXEN[02] -> 0    fan off
  RIXEN[02] -> 15   fan speed    RIXEN[01] -> 1
  THERMOSTAT mode -> 0 (OFF)     THERMOSTAT mode -> 2 (HEAT), fan 0x1E
```

The two are **mutually exclusive** in fuel and elec — only one is commanded at a
time. `dual` presumably commands both; not yet captured.

### Rixen sub-commands 01 / 02 / 03 confirmed

Previously inferred from the DB's ordering, now proven by watching them change
together with a known user action:

| mux | sub-command | evidence |
|---|---|---|
| `01` | **Set TempTarget** | `1` → `322` = 90.0 °F, matching the displayed target |
| `02` | **Set FanSpeed** | `0` → `15` with the furnace, → `0` when switched away |
| `03` | **Set Furnace** | `0` → `1` on fuel heat, `1` → `0` on switch to elec |

### The 2 °F offset is a DEADBAND, not a correction

With a displayed target of **90 °F**:

| value | wire | relationship |
|---|---|---|
| Rixen target (`0x788[01]`) | 90.0 °F | **panel + 0** — no offset |
| Thermostat **heat** setpoint | 88.0 °F | **panel − 2** |
| Thermostat **cool** setpoint | 70.0 °F | **panel 68 + 2** |

The head unit is not applying a fixed correction — it builds a **±2 °F deadband**
around the requested temperature, and **only in the RV‑C thermostat frame**. The
Rixen receives the number unmodified.

This also explains the 88 °F seen during the previous session's Rixen test,
which had been logged as unexplained: the target then was also 90.

> **App rule (revised):** cool setpoint = displayed + 2; heat setpoint =
> displayed − 2; Rixen target = displayed. Do **not** apply one blanket offset.

### CONFIRMED by observed transition (2026-08-12)

Target moved from 90 to 78 with the overhead unit in heat. Both rules appear in
the same frames, at two different temperatures 11 degrees apart:

| t | panel | thermostat heat | Rixen target |
|---|---|---|---|
| 2.21 | 89 *(passing through)* | **87.0 °F** = −2 | **89.1 °F** = +0 |
| 5.55 | **78** | **76.0 °F** = −2 | **78.1 °F** = +0 |

Predicted 76.0, observed 76.0. The cool setpoint held at 70.0 throughout,
untouched — consistent with it being an independent field.

The Rixen's trailing `.1 °F` is quantisation, not error: it carries 0.1 °C
units, so 78 °F → 25.6 °C → 78.08 °F.

**The deadband is now observed, not inferred.** Nothing further outstanding on
the setpoint encoding.

## Rixen writes are accepted — the obstacle is persistence

The heater acts on our `0x788[01]` target immediately; what defeats a write is
the head unit re-asserting its own value a few seconds later.

Measured with a single frame (`cansend can0 788#010B010000000000`, target
80.1 °F) against a `candump` of `0x724` + `0x788`:

```
t=2.70s   we send target 80.1 F
t=3.00s   heater ACCEPTS -> 0x724 target reads 80.1 F   (~300 ms)
t=5.63s   head unit re-asserts 78.1 F
t=6.00s   heater reverts  -> 0x724 target reads 78.1 F
```

Conclusions:

- **The Rixen does not filter by sender.** Unlike the PDM (which obeys only
  SA 0x11), it acted on our frame immediately. Our decode and framing are
  correct as documented.
- **The head unit continuously re-asserts the heater's whole state.** Each
  `0x788` sub-command repeats roughly every **5-6 s**, staggered rather than in
  one burst (measured over 24 s: 12 bursts, mean 2.18 s between bursts, each
  carrying one or two sub-commands). The command frame is NOT fire-once, unlike
  the roof vent's `0x19FEA603`.
- A one-shot write therefore holds for ~3 s. Holding a value means contending
  with the head unit indefinitely.

### Why the app stays read-only

Injection at ~1-2 Hz would be cheap on bus load (unlike PDM dimming's ~250 Hz)
— but the steady state is **contention, not takeover**: our value and the head
unit's alternate every few seconds. On a lighting channel that is flicker; on a
**diesel burner** it is a setpoint oscillating several times a minute, and the
Rixen's internal hysteresis and minimum-run behaviour are **not characterised**
by us at all. Short-cycling a combustion heater is a wear and safety question
we cannot bound from the bus.

**Decision (owner, 2026-08-26): Rixen is read-only in the companion app.**
Real control would need **cut-and-stand-in** (box inline, presenting one
coherent setpoint), never parallel injection.

### What the app reads

| Source | Field |
|---|---|
| `0x724` b0-1 | current cabin temp (x0.01 °C) |
| `0x724` b2-3 | target (x0.1 °C) — no deadband; the Rixen gets the panel's number unmodified |
| `0x724` b6 bit 3 | calling for heat |
| `0x788[02]` | heater fan |
| `0x788[03]` | furnace |
| `0x788[06]` | hot water |

Live sample decoded 2026-08-26: current 83.0 °F, target 78.1 °F, all outputs
off; `0x788[0C]` ambient relay matched `0x724` current to the hundredth,
independently confirming both decodes.
