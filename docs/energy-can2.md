# CAN2 — battery, inverter and charger

The van runs **two CAN buses split by function**. This documents the second one.

| Bus | Pins | Carries |
|---|---|---|
| CAN1 | 5 / 6 | PDM loads, tanks, climate — see [`pdm-control.md`](pdm-control.md), [`climate-control.md`](climate-control.md) |
| **CAN2** | **18 / 19** | **battery, inverter, charger, shore power** |

Established the hard way: the Lithionics and inverter source addresses appear in
the firmware's CAN DB but **never once** in dozens of CAN1 captures, including
while operating the inverter from the panel. Tapping pins 18/19 found them
immediately. See [`hardware-and-tap.md`](hardware-and-tap.md).

Also 250 kbit/s. 29 ids, ~49 frames/s — much quieter than CAN1's ~450.

### T-2CAN-FD CAN-B on CAN2 (2026-08-24)

The companion board's **B channel (ESP32 TWAI, GPIO 6/7)** is now physically
tapped into CAN2 (pins 18/19, the second green/yellow pair; its own isolated
SGND tap on the van ground — the two channels are independently isolated).
Verified live with a listen-only probe at 250 kbit/s: BMS (`0x46`) status
frames and inverter/charger (`0xE1`) frames are on the wire exactly as
documented above, plus `0x19FFD7E1` as a ~15 Hz heartbeat. This completes the
two-bus hardware — CAN-A ↔ CAN1 (control), CAN-B ↔ CAN2 (energy).

## Nodes

Identified from J1939 address claims (`0xEEFF`), which carry the 64-bit NAME:

| SA | NAME decode | Node |
|---|---|---|
| `0x46` | mfg **1069**, function 137, ECU instance **1** | **Lithionics BMS** |
| `0x8E` | mfg **1069**, function 144, ECU instance **2** | **cell monitor** — same manufacturer |
| `0xE1` | — | inverter / charger |
| `0xF2` | — | shore-power circuit capacity |

`0x46` and `0x8E` share a manufacturer code and differ only in function and ECU
instance, so they are two devices in the same battery system.

---

## Battery — standard RV-C DC_SOURCE_STATUS

These are **not** proprietary. They decode with the published RV-C scale factors,
and every field below was cross-checked against the Lithionics phone app or
against arithmetic on another frame.

### `0x19FFFD46` — DC_SOURCE_STATUS_1

| bytes | field | encoding |
|---|---|---|
| 0 | instance | `01` |
| 1 | device priority | `78` |
| **2–3** | **DC voltage** | × 0.05 V |
| **4–7** | **DC current** | 32-bit LE, 1 mA/bit, **offset −2 000 000 000** |

Verified: `0x0428` = 1064 × 0.05 = **53.20 V**, which is exactly the 16 cells ×
3.325 V reported by the app.

> **SIGN CONVENTION: positive = current OUT of the pack (discharging).**
> Confirmed against the panel with shore power disconnected — the wire read
> `+0.70 A` at 53.20 V = **37.2 W**, and the panel simultaneously displayed
> **−37 W**. Magnitude exact, sign inverted relative to the panel's display.
>
> An app must **negate this** to match what the panel shows the owner.
> (Earlier revisions of this document labelled small positive values as
> "charging". That was wrong — they were small discharges.)

### `0x19FFFC46` — DC_SOURCE_STATUS_2

| bytes | field | encoding |
|---|---|---|
| **2–3** | **temperature** | × 0.03125, offset −273 → °C |
| **4** | **state of charge** | × 0.5 % |
| **5–6** | **time remaining** | minutes (`FF FF` when idle/unknown) |

Verified: `0xC8` × 0.5 = **100 %**, matching a freshly-charged pack. Time
remaining confirmed by arithmetic — at 19.5 A it read `0x020A` = **522 min**,
and 173 Ah ÷ 19.5 A = **8.9 h = 533 min**. Two independently decoded frames
agreeing.

### `0x19FFFB46` — DC_SOURCE_STATUS_3

| bytes | field | encoding |
|---|---|---|
| **2** | state of health | × 0.5 % |
| **3–4** | **capacity remaining** | Ah |

Verified: `0xAD` = **173 Ah** at 100 %. The app reported 96.1 Ah at 55 %, which
is internally consistent. **This is an ~8.4 kWh, 48 V nominal pack.**

### Other Lithionics frames (SA `0x46`)

`0x18FF8046`, `0x18FF8146`, `0x18FF8246`, `0x18FF8346`, `0x19FEA546` — partially
decoded. `0x18FF8146` bytes 1–2 and 3–4 both carry × 0.05 V values matching the
pack and bus voltages; `0x18FF8346` bytes 1–2 = `0x0480` × 0.05 = **57.6 V**,
matching the app's "CAN Charger Voltage". `0x19FEC746/C946/CA46` are J1939
diagnostics (DM1/DM2/DM3).

---

## Cell monitor (SA `0x8E`) — structure identified, mapping UNCONFIRMED

Six frames, `0x18FF918E` … `0x18FF968E`, all sharing a three-byte prefix:

```
01 02 10 …
^^ instance
   ^^ ECU instance 2 — matches this node's address claim
      ^^ 0x10 = 16 = the pack's cell count
```

Four of the six have an identical shape, and four bytes each gives sixteen:

```
0x18FF938E   01 02 10 00 | 84 84 84 84      cells  1-4  ?
0x18FF948E   01 02 10 00 | 84 84 84 84      cells  5-8  ?
0x18FF958E   01 02 10 00 | 84 84 84 84      cells  9-12 ?
0x18FF968E   01 02 10 00 | 84 84 84 84      cells 13-16 ?
```

Proposed scaling: **cell V = 2.00 + byte/100**, so `0x84` = 132 → **3.32 V**.
Sixteen of those gives 53.1 V, against **53.20 V** decoded independently from
`DC_SOURCE_STATUS_1`. Agreement to within rounding, by two unrelated routes.

`0x18FF918E` and `0x18FF928E` have a different shape (values `4E`/`4F` = 78/79
among the 132s) and are probably min/max/average plus temperatures.

> **This mapping is NOT confirmed — and confidence went DOWN on 2026-08-12.**
>
> At 62 % SOC under a 1.4 kW load, the **phone app showed a mix of 3.25 V and
> 3.26 V across the pack, while all sixteen bus bytes read an identical
> `0x7D`.** The bus did not reproduce a spread the app could see.
>
> Two explanations, and they are not equally comfortable:
>
> 1. **Resolution.** The field is 0.01 V per count and truncates, so cells at
>    3.254 and 3.256 land on the same byte. Consistent with pack voltage of
>    52.20 V = 3.2625 V/cell, right on the rounding boundary.
> 2. **These may not be per-cell values at all.** A min/max/average replicated
>    across the frame would produce data indistinguishable from what we have.
>    **These sixteen bytes have never once been observed differing from each
>    other**, at any state of charge.
>
> Until they are seen to diverge, "sixteen cells" remains a structural guess
> supported only by the count byte (`0x10` = 16) and by sixteen × the decoded
> value landing near pack voltage.

**Drawn-down test done (2026-08-12) — cell 9 is fine.** With the pack finally
pulled down, the app showed cell 9 in line with the rest of the pack. The
2026-08-11/12 shutdown was genuine low-SOC starvation on a stale SOC gauge, not
a failing cell. The per-cell ordering question above is now moot for practical
purposes: the pack has no misbehaving cell to track, and fine imbalance below
~0.01 V remains invisible on the bus anyway.

**Standing practice (manual):** check the per-cell screen in the Lithionics
phone app occasionally, e.g. after any deep discharge. No automation needed.

---

## Inverter / charger (SA `0xE1`)

### `0x19FFD7E1` — AC line status (10 Hz, the busiest frame)

| bytes | field | encoding | observed |
|---|---|---|---|
| **1–2** | **AC RMS voltage** | × 0.05 V | **119.8 – 120.9 V** |
| 3–4 | AC current | offset ~32000, scale unconfirmed | tracks load inversely |
| **5–6** | **frequency** | ÷ 128 Hz | **59.91 – 60.09 Hz** |

Two fields with unrelated scale factors both landing on textbook North American
shore power is strong evidence the decode is right.

### `0x19FEA3E1`

Bytes 3–4 = × 0.05 V = **53.20 V**, matching the pack voltage from the BMS via a
different node. **Byte 5 tracks AC load** — `14` at idle, rising to `116` with
the A/C running. Likely AC current × 0.1 (= 11.6 A, plausibly under the 15 A
shore limit), **unconfirmed**.

### Shore-power limit ("branch amps") — CONFIRMED, in three frames

Changing the panel setting from 20 A to 15 A moved all three:

| frame | byte | 20 → 15 |
|---|---|---|
| `0x19FF95F2` | 3 | `0x14` → `0x0F` |
| `0x19FF96E1` | 3 | `0x14` → `0x0F` |
| `0x19FFC9E1` | 7 | `0x14` → `0x0F` |

### The inverter and charger STATUS frames are stubs — do not rely on them

Checked byte-by-byte across **four captures including a 20 A load event** (A/C
compressor start, shore limit change). These five frames **never changed a
single byte**:

| frame | payload | nominal meaning |
|---|---|---|
| `0x19FFD4E1` | `01 01 FF FF FF FF FF FF` | inverter status |
| `0x19FFC7E1` | `01 FF FF FF FF FF 00 FF` | charger status |
| `0x19FFCAE1` | `01 00 00 00 7D 00 00 FF` | charger AC status 2 |
| `0x18FECAE1` | `05 42 FF FF FF FF FF FF` | diagnostics |
| `0x19FECAE1` | `05 42 FF FF FF FF FF FF` | diagnostics |

They are broadcast at 2 Hz and are **almost entirely `FF`, which is RV-C's
"not available"**. The device announces itself on these DGNs without populating
them.

> **Consequence for a companion app:** reading `0x19FFD4E1` for inverter state
> returns `01` forever regardless of what the inverter is doing. Do not build a
> UI on these. **All live power data is in `0x19FFD7E1` (AC), `0x19FEA3E1` (DC)
> and the BMS frames.**

The constant `0x7D` in `0x19FFCAE1` byte 4 never varies and is presumably a
nameplate rating rather than a measurement.

### `0x19FFD7E1` bytes 3–4 — AC current, offset-encoded

Observed across the load test, with byte 4 constant at `0x7C`:

| byte 3 | 16-bit LE | condition |
|---|---|---|
| `F9` | 31993 | idle |
| `E4`, `E3` | 31972, 31971 | A/C fan only |
| `DC` | 31964 | |
| `70`, `5D`, `5A`, `56` | 31856 … 31830 | compressor running |

It moves **inversely** with load, so it is offset-encoded around zero. Swing is
**163 counts** from idle to full A/C — the right magnitude for the ~15 A shore
limit in force at the time, but the exact scale factor is **unconfirmed**
(163 counts would be 16.3 A at 0.1 A/bit, slightly over the set limit).

*Test to pin it down: set branch amps to a very different value, load the system
to the limit, and see whether the loaded reading tracks it.*

---

## Measured: what the air conditioner actually costs

Captured live, on shore power with the limit at 15 A and the pack at 100 %:

Sign convention below: **positive = discharge**, i.e. current leaving the pack.

| t | pack current | event |
|---|---|---|
| 0–12 s | 0.5 – 0.7 A | idle |
| 12.75 s | 2.5 A | A/C switched on (fan) |
| 13.25 s | 4.4 A | |
| **22.25 s** | **19.6 A** | **compressor starts** |
| 22.75 s | **26.0 A** | inrush peak |
| 23–39 s | 17 – 19.9 A | settled |

The owner independently reported hearing the compressor start ~15 s before the
end of a 40 s capture. It is at t = 22.25.

**The pack supplies ~20 A even while on shore power.** With the limit at 15 A,
shore can deliver ~1800 W; the A/C needs more, and the inverter makes up the
difference from the battery. That is what the branch-amps setting is for.

**Off shore power the pack supplies all of it** — on the order of 50 A.

### Voltage sag baseline — record this

| condition | pack voltage |
|---|---|
| idle, 100 % SOC | 53.20 V |
| 19.8 A, 100 % SOC | **52.85 V** |

**0.35 V of sag under ~20 A at full charge, ≈ 0.022 V per cell.** That is
healthy, and it is the reference point.

> **The diagnostic:** repeat this measurement with the pack at 40–50 %. If sag
> under a comparable load is dramatically worse, that is the weak cell showing
> itself — and there is now a before-and-after rather than a guess.

This also explains a BMS shutdown the owner experienced: an A/C start on a pack
that was genuinely near-empty (the SOC gauge was reading ~55 % while cell
voltages indicated far less) with one cell already at the floor. The BMS
alarmed and opened the contactor, which is correct behaviour.

---

## Solar — RESOLVED (2026-08-12)

No solar controller node was found on CAN2 — the four source addresses are the
BMS (`0x46`), cell monitor (`0x8E`), inverter/charger (`0xE1`) and circuit
capacity (`0xF2`). The controller on this van is standalone, and that is fine:
the array works, and that is all that needed answering.

**The array is 175 W nominal; ~50–80 W was observed in full sun — but the pack
was near full at the time**, and a nearly-full LiFePO₄ pack tapers. So 50–80 W
is what the battery would *accept*, not a ceiling on what the array can
produce. True array output is still unmeasured, and it is fine for it to stay
that way — the panel works, which is all that needed answering.

There was no bus mystery to solve: solar flows into the pack and the BMS
measures pack current, so solar is visible indirectly as (pack current + house
load) whenever shore power and the alternator are out of the picture. Nothing
further to decode.

### Standing loads

| condition | draw |
|---|---|
| shore off, head unit on, no loads | **37 W** |
| van fully shut down | lower, not yet isolated |

At 164 Ah × 53 V that 37 W idle is roughly ten days of standing time.

### Formerly-open measurements — both closed (2026-08-12)

True solar output and the cell-9 divergence question both needed a drawn-down
pack. Normal use provided it: **solar resolved** (175 W array, ~50–80 W real,
nothing on the bus to decode), **cell 9 resolved** (in line with the pack — the
earlier "failing cell" suspicion did not survive a real drawdown). No open
measurement items remain on CAN2.
