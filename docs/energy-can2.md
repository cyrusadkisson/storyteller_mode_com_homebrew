# CAN2 — battery, inverter and charger

The van runs two CAN buses split by function. This one carries everything to do
with energy.

| Bus | Pins | Carries |
|---|---|---|
| CAN1 | 5 / 6 | PDM loads, tanks, climate — [`pdm-control.md`](pdm-control.md), [`climate-control.md`](climate-control.md) |
| **CAN2** | **18 / 19** | **battery, inverter, charger, shore power** |

Both run at 250 kbit/s. CAN2 is much quieter — 29 IDs at roughly 49 frames/s,
against CAN1's ~450.

The Lithionics and inverter source addresses appear in the firmware's CAN
database but never on CAN1, including while operating the inverter from the
panel. They are only on pins 18/19. Wiring is in
[`hardware-and-tap.md`](hardware-and-tap.md).

### Two things called "the app"

Both appear below and they are easy to confuse:

- **the phone app** — Lithionics' own Bluetooth app, which talks directly to the
  BMS
- **the companion app** — this project's web UI, served by a board on CAN2

Where a value is checked against both, that is two independent routes to the
same number.

### The system

A 48 V nominal house pack: sixteen LiFePO₄ cells in series, about 3.3 V each,
53.2 V charged. Nameplate **173 Ah ≈ 8.4 kWh**. A Lithionics BMS sits between
the cells and the van and can open a **contactor** — a large relay — to protect
the pack. When it opens the van loses all house power instantly.

**The BMS protects per cell, not per pack.** That fact explains the failure in
Part 3, and it is the single most useful thing on this page.

> Most Storyteller vans have the dual 16.8 kWh system: two packs, 32 cells. The
> frame decoding here comes from a single-pack van. Pack-level frames should be
> identical; the cell monitor almost certainly is not. See
> [Part 4](#part-4--not-yet-established).

---

# Part 1 — What is on the bus

## Nodes

From J1939 address claims (`0xEEFF`), which carry the 64-bit NAME:

| SA | NAME decode | Node |
|---|---|---|
| `0x46` | mfg **1069**, function 137, ECU instance **1** | Lithionics BMS |
| `0x8E` | mfg **1069**, function 144, ECU instance **2** | cell monitor |
| `0xE1` | — | inverter / charger |
| `0xF2` | — | shore-power circuit capacity |

`0x46` and `0x8E` share a manufacturer code and differ only in function and ECU
instance: two devices inside the same battery system.

## The frames worth reading

| frame | carries |
|---|---|
| `0x19FFFD46` | pack voltage, pack current |
| `0x19FFFC46` | pack temperature, state of charge, time remaining |
| `0x19FFFB46` | state of health, capacity remaining |
| `0x18FF938E`–`0x18FF968E` | the sixteen individual cell voltages |
| `0x18FF918E` | lowest and highest cell |
| `0x19FFD7E1` | AC line voltage, current, frequency |
| `0x19FF95F2`, `0x19FF96E1`, `0x19FFC9E1` | shore-power limit |
| `0x19FFD4E1`, `0x19FFC7E1`, and three more | nothing — see [stub frames](#stub-frames) |

## Battery — standard RV-C `DC_SOURCE_STATUS`

These decode with the published RV-C scale factors. Nothing proprietary.

### `0x19FFFD46` — voltage and current

| bytes | field | encoding |
|---|---|---|
| 0 | instance | `01` |
| 1 | device priority | `78` |
| **2–3** | **DC voltage** | × 0.05 V |
| **4–7** | **DC current** | 32-bit LE, 1 mA/bit, **offset −2 000 000 000** |

`0x0428` = 1064 × 0.05 = **53.20 V**, which is sixteen cells at the 3.325 V the
phone app showed at the same moment.

> ### Sign convention
>
> **On the wire, positive means current flowing OUT of the pack — discharging.**
>
> With shore power disconnected the wire reads **+0.70 A** at 53.20 V = 37.2 W,
> while the panel displays **−37 W**. Same magnitude, opposite sign.
>
> **Negate it to match what the panel shows.** The companion app does, so its
> displayed values are the opposite sign to the raw frame: a draw is negative on
> screen and positive on the wire.

### `0x19FFFC46` — temperature, charge, time remaining

| bytes | field | encoding |
|---|---|---|
| **2–3** | **temperature** | × 0.03125, offset −273 → °C |
| **4** | **state of charge** | × 0.5 % |
| **5–6** | **time remaining** | minutes; `FF FF` means unknown |

`0xC8` × 0.5 = 100 % on a full pack. Time remaining checks out arithmetically:
at 19.5 A it reads `0x020A` = 522 min, and 173 Ah ÷ 19.5 A = 8.9 h = 533 min.

> **The factory panel misreads the unknown sentinel.** The BMS sends `FF FF`
> whenever it declines to estimate, which it does at low draw — exactly when the
> answer would be most reassuring. The panel prints that literally as
> **45d 12h**, because `0xFFFF` minutes is 45.51 days. Seen with the pack at
> ~70 % and a 31 W draw, where the correct figure is 8d 16h.
>
> **45 days on the panel means "unknown", not six weeks.** The companion app
> computes the figure from amp-hours and current instead.

### `0x19FFFB46` — health and remaining capacity

| bytes | field | encoding |
|---|---|---|
| **2** | state of health | × 0.5 % |
| **3–4** | **capacity remaining** | Ah |

`0xAD` = 173 Ah at 100 %. At 95 % it reads 164.5 Ah, and 164.5 / 0.95 = 173.2.

**This is capacity remaining, not pack size.** Total capacity can be derived as
`Ah ÷ (SoC/100)`, which is how the companion app computes time-to-full without
hard-coding a nameplate figure — useful on a pack whose real capacity has
dropped.

### Other Lithionics frames (SA `0x46`)

`0x18FF8146` bytes 1–2 and 3–4 carry × 0.05 V values matching pack and bus
voltage. `0x18FF8346` bytes 1–2 = `0x0480` × 0.05 = **57.6 V**, matching the
phone app's "CAN Charger Voltage". `0x19FEC746` / `C946` / `CA46` are J1939
diagnostics (DM1/DM2/DM3). `0x18FF8046`, `0x18FF8246` and `0x19FEA546` are
undecoded.

---

## Cell monitor (SA `0x8E`)

Six frames, `0x18FF918E` … `0x18FF968E`, sharing a three-byte prefix:

```
01 02 10 …
^^ instance
   ^^ ECU instance 2 — matches this node's address claim
      ^^ 0x10 = 16 = the pack's cell count
```

### The sixteen cell voltages

| frame | cells | payload |
|---|---|---|
| `0x18FF938E` | 1–4 | bytes 4–7 |
| `0x18FF948E` | 5–8 | bytes 4–7 |
| `0x18FF958E` | 9–12 | bytes 4–7 |
| `0x18FF968E` | 13–16 | bytes 4–7 |

**`cell V = 2.00 + byte/100`.** Resolution is 0.01 V, and it truncates.

With one weak cell in the pack, the bus and the phone app read the same thing:

```
01 02 10 00 7D 7D 7D 7D     0x18FF938E   cells  1-4
01 02 10 00 7D 7D 7D 7D     0x18FF948E   cells  5-8
01 02 10 00 7B 7D 7D 7D     0x18FF958E   cells  9-12
01 02 10 00 7D 7D 7D 7D     0x18FF968E   cells 13-16
```

`0x7B` = 123 → 3.23 V, `0x7D` = 125 → 3.25 V, against the app's 3.23 V for cell
9 and 3.25 V for the rest.

> **A uniform pack tells you nothing about this frame.** At high state of charge
> sixteen healthy cells sit within one count of each other, and one count is also
> what truncation can hide. Sixteen identical bytes are the normal reading, not
> evidence of anything. The decode is only testable when a cell is genuinely
> apart from the others.

### `0x18FF918E` — the summary frame

```
01 02 10 00 47 76 77 67
            ^^ ^^ ^^ ^^
            |  |  |  +-- byte 7: lowest cell    0x67 = 3.03 V
            |  |  +----- byte 6: highest cell   0x77 = 3.19 V
            |  +-------- byte 5: tracks the median, not the mean
            +----------- byte 4: probably temperature, byte − 40 = °C
```

**Byte 7 is the most useful byte on this node.** It is the minimum cell voltage,
broadcast continuously, and it is the quantity the BMS opens the contactor on.
Watching for a failing cell takes one byte, not sixteen.

Byte 5 read 3.18 V where the true mean was 3.173 and the median was 3.18 — one
observation, so treat it as a lead. Byte 4 read `0x47` = 71 → 31 °C with the pack
at 86 °F, and `0x45` = 69 → 29 °C at ~84 °F: consistent across two readings,
unproven.

### `0x18FF928E`

```
01 02 10 40 00 00 4A 84
         ^^        ^^
         |         +-- 0x4A = 74 → 34 °C, plausibly BMS temperature
         +------------ 0x40 matched the phone app's "Last Code 40"
```

The phone app reports BMS temperature several degrees above cell temperature,
which fits byte 6 being the BMS's own sensor. Both are single observations. The
rest of this frame is undecoded.

---

## Inverter and charger (SA `0xE1`)

### `0x19FFD7E1` — AC line status

The busiest frame on this bus, about 10 Hz.

| bytes | field | encoding | observed |
|---|---|---|---|
| **1–2** | **AC RMS voltage** | × 0.05 V | 119.8 – 120.9 V |
| 3–4 | AC current | offset-encoded; scale not established | tracks load inversely |
| **5–6** | **frequency** | ÷ 128 Hz | 59.91 – 60.09 Hz |

Two fields with unrelated scale factors both landing on textbook North American
shore power.

Bytes 3–4 sit around an offset and move *inversely* with load. Across a load
test with byte 4 constant at `0x7C`:

| byte 3 | 16-bit LE | condition |
|---|---|---|
| `F9` | 31993 | idle |
| `E4`, `E3` | 31972, 31971 | A/C fan only |
| `DC` | 31964 | |
| `70`, `5D`, `5A`, `56` | 31856 … 31830 | compressor running |

Swing is 163 counts idle to full — the right magnitude for the 15 A shore limit
in force, but the scale factor is not established. At 0.1 A/bit that would be
16.3 A, slightly over the limit that was set.

*To settle it: set branch amps to a very different value, load to the limit, and
see whether the loaded reading tracks it.*

### `0x19FEA3E1` — DC side

Bytes 3–4 = × 0.05 V = 53.20 V, matching pack voltage via a different node.
Byte 5 tracks AC load — `14` at idle, `116` with the A/C running. Probably AC
current × 0.1 (11.6 A), not established.

### Shore-power limit — "branch amps"

The panel setting that caps draw from a pedestal. Changing it from 20 A to 15 A
moves the same value in three frames:

| frame | byte | 20 → 15 |
|---|---|---|
| `0x19FF95F2` | 3 | `0x14` → `0x0F` |
| `0x19FF96E1` | 3 | `0x14` → `0x0F` |
| `0x19FFC9E1` | 7 | `0x14` → `0x0F` |

Plain integer amps. **Reading it is solved; setting it is not** — all three are
reports from the circuit-capacity and inverter nodes. Whatever the panel sends
to change the setting has not been captured.

### Stub frames

Five frames never change a single byte, checked across four captures including a
20 A load event:

| frame | payload | nominal meaning |
|---|---|---|
| `0x19FFD4E1` | `01 01 FF FF FF FF FF FF` | inverter status |
| `0x19FFC7E1` | `01 FF FF FF FF FF 00 FF` | charger status |
| `0x19FFCAE1` | `01 00 00 00 7D 00 00 FF` | charger AC status 2 |
| `0x18FECAE1` | `05 42 FF FF FF FF FF FF` | diagnostics |
| `0x19FECAE1` | `05 42 FF FF FF FF FF FF` | diagnostics |

They broadcast at 2 Hz and are almost entirely `FF`, which in RV-C means "not
available". The device announces itself on these DGNs without populating them.

> **Reading `0x19FFD4E1` for inverter state returns `01` forever, whatever the
> inverter is doing.** All live power data is in `0x19FFD7E1` (AC),
> `0x19FEA3E1` (DC) and the BMS frames.

The constant `0x7D` in `0x19FFCAE1` byte 4 is presumably a nameplate rating.

---

# Part 2 — What the van draws

Sign convention here is the wire's: positive is discharge.

## Standing load

| condition | draw |
|---|---|
| shore off, head unit on, no loads | **37 W** (~0.7 A) |
| van fully shut down | lower, not measured |

At 173 Ah × 53 V, a 37 W idle is roughly **ten days** of standing time.

## What the air conditioner costs

On shore power, limit at 15 A, pack at 100 %:

| t | pack current | event |
|---|---|---|
| 0–12 s | 0.5 – 0.7 A | idle |
| 12.75 s | 2.5 A | A/C on, fan only |
| 13.25 s | 4.4 A | |
| **22.25 s** | **19.6 A** | compressor starts |
| 22.75 s | **26.0 A** | inrush peak |
| 23–39 s | 17 – 19.9 A | settled |

**The pack supplies ~20 A even on shore power.** With the limit at 15 A shore can
deliver about 1800 W; the A/C needs more, and the inverter makes up the
difference from the battery. That is what the branch-amps setting is for, and why
it matters on a pack with a weak cell.

**Off shore power the pack supplies all of it** — 26.6 A ≈ 1350 W steady, and on
the order of 50 A during compressor inrush.

## Voltage sag

| condition | pack voltage |
|---|---|
| idle, 100 % | 53.20 V |
| 19.8 A, 100 % | **52.85 V** |

**0.35 V of sag under ~20 A at full charge, about 0.022 V per cell.** That is a
healthy pack, and it is a useful number to record on your own van while it is
known good. Repeat it at 40–50 %: markedly worse sag under a comparable load is
a weak cell showing itself, and you will have a before-and-after rather than a
guess.

## Solar

No solar controller appears on CAN2; the four source addresses are the ones in
the Nodes table. The controller is standalone.

Solar is still visible indirectly: it flows into the pack, and the BMS measures
pack current, so with shore power and the alternator out of the picture it shows
up as (pack current + house load).

A 175 W array read 50–80 W in full sun with the pack near full — but a nearly
full LiFePO₄ pack tapers, so that is what the battery would accept, not a ceiling
on the array.

---

# Part 3 — A failed cell

This is the section worth reading even if you never touch the bus, because the
symptom is *"the battery died at 80 %"* and nothing on the factory screen
explains it.

## What it looks like

The BMS opens the contactor when **any single cell** reaches its undervoltage
floor. Both numbers you can see are averages:

- **Pack voltage** is the sum of sixteen cells. One cell 0.3 V down moves it by
  0.3 V — indistinguishable from a slightly lower state of charge.
- **State of charge** describes what the pack as a whole holds. It has no way to
  say "and one of them is empty".

So both stay reassuring right up to the moment everything goes dark:

```
pack 50.4 V, gauge 80 %   <- looks fine
one cell at its floor      <- what the BMS is watching
contactor opens            <- total power loss
```

The gauge is not lying. 80 % is a fair description of what the other fifteen
cells hold. **Usable capacity is set by the weakest cell alone**, not by the
amp-hours reported.

## Spotting it

A weak cell hides at high state of charge and only shows itself under load or
near empty. One example pack, tracked over four days:

| pack state | weak cell | the other fifteen | gap |
|---|---|---|---|
| 95 %, resting | 3.23 V | 3.25 V | 1 count |
| 80 %, after 27 min of A/C | 3.03 V | 3.18 – 3.19 V | 16 counts |
| dead, resting | 2.80 V | 3.11 – 3.15 V | 33 counts |

At 95 % it is a single count — indistinguishable from rounding. **Check when the
pack is worked, not when it is full.**

Other signs in the same pack: the **balance map read `0000`**, meaning the BMS
was not even attempting to correct the imbalance, and highest recorded
temperature was 206 °F.

## A shutdown, start to finish

One event logged end to end on CAN2. Times UTC. Sign is the companion app's, so
negative is a draw.

| UTC | event | pack | current |
|---|---|---|---|
| 15:50:17 | **roof A/C switched on** | 51.30 V | −0.7 A |
| 15:50:53 | voltage/charge warning | 50.90 V | −21.3 A |
| 15:54 – 16:14 | A/C running | 50.60 → **50.40 V** | −25.7 → −27.0 A |
| 16:13:25 | weak-cell warning, naming cell 9 at 3.05 V | 50.40 V | −26.7 A |
| **~16:17** | **BMS opens the contactor** | — | −26.6 → **0.0 A** |
| 16:19 – 16:44 | dead; pack resting, BMS still transmitting | 50.60 V | 0.0 A |
| 16:48:47 | charging begins, warnings clear | 51.40 V | **+28.9 A** |

**The trigger is the air conditioner running off the inverter.** Standing load is
−0.8 A. The A/C pulls **−26.6 A ≈ 1350 W straight from the pack**. Under that
load the weak cell reached its floor in 27 minutes, while the pack average sat at
50.4 V and the gauge read 80 %.

**Warning was available 27 minutes ahead**, from pack-level frames alone, and
4 minutes ahead from the cell frames with the cell named.

> **How to tell a dead van from a quiet one.** The BMS keeps transmitting after
> the contactor opens, so the bus still shows a plausible pack voltage. The
> giveaway is **current reading exactly 0.0 A while a standing load should be
> drawing 0.8 A.** Voltage alone will fool you.

## Reading cell voltages around a shutdown

Two readings that look contradictory and are not:

```
16:13   3.05 V   under 26.6 A load
  |     (~4 more minutes of load — the cell falls to its cutoff, unsampled)
~16:17  contactor opens
  |     (at rest, the cell recovers)
16:33   3.03 V   at rest, 0.0 A
```

The later reading is *lower* despite the load being gone. Two effects run
opposite: removing the load **raises** terminal voltage by the IR drop, while the
~1.8 Ah taken out during those last four minutes **lowers** it. Near the knee of
a LiFePO₄ curve — where a failing cell lives — a little charge moves voltage a
lot, so the depletion outweighed the recovery. Two counts is close to the 0.01 V
measurement floor in any case.

> **What would be different:** a cell falling *while genuinely at rest*, across a
> stretch with no current at all. That is self-discharge, and it means an
> internal short rather than a tired cell.
>
> **To tell them apart: two cell readings about fifteen minutes apart while the
> system is dead.** Rising is ordinary recovery. Flat or falling is a short.

## What to do about it

- **A shutdown can arrive at any indicated state of charge.** Do not plan around
  the gauge.
- **Running the roof A/C off the inverter ends in a shutdown in roughly half an
  hour.** On shore power it is a different situation, because the pack is not
  supplying the load — so the branch-amps setting is worth getting right.
- The fix is a pack service conversation with Storyteller or Lithionics. There is
  no software remedy for a failed cell.

## Watching for it automatically

Two warnings the companion controller runs on the board itself, so they work with
no phone connected, and writes to a flash log that survives a total power loss:

| warning | trips on | why |
|---|---|---|
| **weak cell** | lowest cell < 3.00 V, **or** pack spread ≥ 0.10 V | the quantity the BMS itself trips on |
| **voltage / charge** | pack < 51.0 V while SoC > 50 % | pack-level frames only, so it still works if the cell frames go quiet |

Both use split trip and clear thresholds so a pack sitting on the line does not
flap. The second is the weaker signal — an inference about a cell where the bus
reports the cell directly — but it costs nothing and covers a different failure.

---

# Part 4 — Not yet established

| item | state |
|---|---|
| **Dual-battery vans** | Most Storyteller vans have two 8.4 kWh packs, 32 cells. Pack-level frames should be unchanged; the cell monitor almost certainly differs. The phone app's module page has columns headed 1 and 2, so the protocol anticipates a second module, but it has not been observed. |
| **Setting branch amps** | Reading it is solved. The frame the panel sends to change it has not been captured. |
| `0x18FF918E` byte 5 | Tracks the median in one observation. Needs a pack that is not uniform. |
| `0x18FF918E` byte 4 | Temperature on two consistent readings. |
| `0x18FF928E` | Mostly undecoded. |
| `0x19FFD7E1` bytes 3–4 | AC current scale factor. |
| `0x19FEA3E1` byte 5 | Probably AC current × 0.1. |
