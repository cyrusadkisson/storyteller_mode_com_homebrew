# ModeWifi (changer65535) — analysis of a parallel effort

An independent owner, **Christopher Clay Hanger**, published
[`github.com/changer65535/ModeWifi`](https://github.com/changer65535/ModeWifi)
(GPL-3.0, March 2025): an ESP32 MCP2515 node that taps CAN1 near the Rixen
heater (DT splitters), serves a web UI ("Storyland Commode", mDNS `cm.local`)
from a WiFi AP, and drives lights, pumps, awning, A/C and reads tanks/amps on
**his** Storyteller. Found via the Storyteller Overland Insiders Facebook
group, 2026-08-14; cloned locally for study (not vendored here — GPL-3 vs our
MIT; the *facts* below are protocol observations, not copied code).

**Why it matters us:** (1) independent corroboration of our entire CAN map;
(2) a second, complementary **control strategy** that solves the persistence
problem we found; (3) working reference hardware/software for phase 3.

## 1. ID map — full agreement (strip the 0x80000000 extended-flag bit)

| His define | ID | Ours |
|---|---|---|
| `PDM1_COMMAND` | `0x14EF1E11` | ✓ PDM1 command |
| `PDM2_COMMAND` | `0x14EF1F11` | ✓ PDM2 command |
| `PDM1_MESSAGE` / `PDM2_MESSAGE` | `0x14EF111E` / `…1F` | ✓ PDM status |
| `THERMOSTAT_COMMAND_1` | `0x19FEF903` | ✓ climate command |
| `THERMOSTAT_AMBIENT_STATUS` | `0x19FF9C58` | ✓ |
| `THERMOSTAT_STATUS_1` | `0x19FFE258` | ✓ |
| `ROOFFAN_CONTROL` / `_STATUS` | `0x19FEA603` / `0x19FEA758` | ✓ |
| `TANK_LEVEL` | `0x19FFB7AF` | ✓ |
| `ACK_CODE` | `0x18E80358` | ✓ J1939 ACK |
| `PDM1_SHORT` / `PDM2_SHORT` | `0x14E9111E` / `…1F` | PGN **E900** — new to us, unexamined |

His DO map matches ours (WaterPump DO12, GalleyFan PDM2/DO2, Fridge PDM2/DO3,
TankMonitor PDM2/DO7, SwitchPower PDM2/DO8, Macerator PDM2/DO11 = our
confirmed SinkPump, Aux PDM2/DO12 = our confirmed Aux1). **Additions we didn't
have:** PDM1 **DI8 = "Engine Running"** (ignition!), PDM2 **DO4 = 12V/USB**
outlet. Discrepancies: he places Awning Lights on PDM2 DO5 — we have it
CONFIRMED on PDM1 DO5, and PDM2 DO5 is the awning MOTOR on our van. His van
may differ (he also has a second fan node at **SA 0xC1**:
`0x19FEA7C1`/`0x19FECAC1` — not seen on ours). Per the README disclaimer:
verify per-van.

## 2. His control strategy — the input spoof (solves persistence)

We proved the PDM obeys output commands **only from the head unit (SA 0x11)**
and that one-shot writes are overwritten by the HU's re-broadcast within
~11 ms (measured 2026-08-25; the ~45 Hz / 22 ms figure recorded earlier was
wrong — mux `FC` actually runs at ~91 Hz). His solution is to never command outputs at all:

1. The PDM broadcasts **digital-input status** frames: `0x14EF111E` mux
   **`F0`** (DI1–6) and **`F8`** (DI7–12), carrying the wall-switch states as
   **2-bit fields**.
2. To "press" a button he **copies the live F0/F8 frame, sets the switch's
   2-bit field to `0b10`, re-sends it with the same CAN id (i.e. impersonating
   the PDM, SA 0x1E)**, waits ~100 ms, then sends the cleared frame (release).
3. The head unit's own state machine sees the press, toggles the load, and
   keeps re-broadcasting the new state itself. **Persistence comes free.**

Trade-offs (visible in his own FB posts): stateless — a spoofed toggle doesn't
say on/off, so his UI infers state from **feedback current** (see §3);
dimmable lights can't be dimmed this way ("wall switches only toggle"); awning
hold-to-run semantics require holding the spoofed press.

His switch→bit mapping (2-bit slots, slot n = bits (2n, 2n+1) of the byte):

| Button | Frame | Byte | Slot | His DI name |
|---|---|---|---|---|
| Cabin | PDM1 F0 | 6 | 0 (bits 0–1) | DI4 |
| Cargo | PDM1 F0 | 6 | 1 (bits 2–3) | DI3 |
| Recirc | PDM1 F0 | 7 | 2 (bits 4–5) | DI6 |
| Awning light | PDM1 F0 | 7 | 3 (bits 6–7) | DI5 |
| Awning enable | PDM1 F8 | 6 | 3 | DI7 |
| Awning out | PDM1 F8 | 7 | 2 | DI2 |
| Awning in | PDM1 F8 | 7 | 3 | DI1 |
| Water pump | PDM2 F0 | 7 | 3 | DI5 |
| Aux | PDM2 F0 | 6 | 0 | DI4 |
| Drain (sink) | PDM2 F8 | 6 | 1 | DI9 |

Note the **reversed pairing within each byte** (low slot = higher DI number) —
same ordering family we found in status mux `C9`. **Unverified against our
van** — all of this must be diff-confirmed on our own captures before use.

**Verified on our van 2026-08-24 (T-2CAN-FD + Jhoinrch):**
- **Cabin (DI4)** — confirmed: PDM1 F0, byte 6 (7th payload byte), slot 0
  (bits 0–1), pressed = `0b10`. F8 frame never moved during the toggle, as
  predicted for a PDM1 DI.
- **Input spoof validated live**: copied the live F0 frame, set the cabin slot
  to `0b10`, re-sent as SA 0x1E (~150 ms), then cleared. The HU reacted within
  ~0.5 s and commanded its own DO4 from `0x00` to `0x13` and held it; the cabin
  light turned on. The release frame (slot cleared) is indistinguishable from
  the PDU's own baseline F0 stream — no extra bus footprint.
**Four switches now verified against the live van (clean toggles, 2026-08-24):**

| Switch | Frame | Byte | Slot | Press value | His table |
|---|---|---|---|---|---|
| Cabin | PDM1 F0 | 6 | 0 (bits 0–1) | `0x02` | ✓ DI4 |
| Garage (Cargo) | PDM1 F0 | 6 | 1 (bits 2–3) | `0x08` | ✓ DI3 |
| Water pump | PDM2 F0 | 7 | 3 (bits 6–7) | `0x80` | ✓ DI5 |
| Sink drain (Drain) | PDM2 F8 | 6 | 1 (bits 2–3) | `0x08` | ✓ DI9, hold-to-run, **parked manual** |
| Aux | PDM2 F0 | 6 | 0 (bits 0–1) | `0x02` | ✓ DI4 — exterior perimeter lighting |
| Recirc | PDM1 F0 | 7 | 2 (bits 4–5) | `0x20` | ✓ DI1 — momentary; HU auto-timer ~10 s |
| Awning light | PDM1 F0 | 7 | 3 (bits 6–7) | `0x80` | ✓ DI5 — toggle; verified w/o awning deployed |

Every slot so far matches his table byte-for-byte — his van ≠ this van was the
open question, and the answer so far is "no difference." Input-spoof round-trip
also proven: spoofed press turned the cabin on, a second press turned it off,
both directions held by the HU's own re-broadcast.

Remaining unverified on this van: recirc, awning light, awning enable/out/in,
Aux. (An earlier mis-toggle capture was discarded, not trusted.)

**Hold-to-run limitation (2026-08-24, wire-verified):** the input-spoof cannot
sustain a *hold-to-run* switch on this van. The sink drain (PDM2 F8, byte 6,
slot 1) physically requires the field held ~0.8 s while the PDU itself
re-broadcasts it as released at ~25 Hz — so a parallel tap can only ever put a
flickering `0b10` on the wire (confirmed: 50 press frames in 1 s, HU never
commanded DO5). Toggle switches (cabin/garage/water pump) work fine off a
single pulse. Control of hold-to-run loads (sink drain now; awning motor
untested, do not assume) requires the **cut-and-stand-in** architecture
(rewrite the panel's commands in flight) — parked as a phase-3 milestone.
Drain parked manual-only by owner decision.

Delta worth chasing: the live F0 byte-5 value moved between sessions (`A4` →
`A5`), another digital input is changing — the F0 stream is dynamic, exactly why
copy-live-then-modify matters. **Resolved by re-audit (2026-08-24): not a
switch.** His code decodes F0/F8 bytes 4–5 as a 10-bit analog supply reading:
`((byte4 & 0x03) * 256 + byte5) * 5 / 1024` V. The `A4` → `A5` drift was the
rail voltage tick, nothing else.

## 3. Status mux map (his, for `0x14EF111E` — supersedes our notes)

| mux | meaning (his) |
|---|---|
| `F0` / `F8` | digital inputs 1–6 / 7–12 (wall switches) |
| `F9` / `C9` / `39` | **per-channel feedback amps, outputs 1–6**: bytes 2–7 = ch 1–6, **× 0.125 A** |
| `0A` / `CA` / `FA` | **feedback amps, outputs 7–12** (same layout) |
| `FB` | **supply voltage** = (data[6] + data[7]·256) / 256 V |
| `FC` | "motor model handshake" (his handleMessage134) — differs from our F1..FC sub-index note; unresolved |
| `FD` | analog inputs 3–4 + output diagnostics |
| `FE` | heartbeat |

The **feedback-amps** frames are the payoff: they are the `.Feedback[A]`
signals from the dictionary *on the wire*, and they make any app **stateful**
(0 A ⇒ the load is truly off) — exactly the piece his toggle model lacked and
ours needs.

## 4. A/C writes (his)

Copy the last observed `0x19FEF903`, modify, re-send **as the same id**
(climate SA `0x03`): `data[1]` = `(fanMode<<4)+operatingMode`, `data[2]` = fan
speed, `data[5..6]` = cool setpoint (LE, ×0.03125 −273 °C). Matches our decode
exactly (his fan-speed comment says 0–125 vs our observed 0–255; treat as his
UI cap, not protocol). His trace shows `0x19FEF903` **repeating** during
operation — so the HU may re-broadcast the thermostat command periodically
when climate is active, not strictly on-change; recheck.

## 5. Synthesis for phase 3 (the design this suggests)

| Need | Method | Source |
|---|---|---|
| Toggle a load, persistently | **input spoof** (F0/F8, +`0b10`, 100 ms, release) | his |
| Set a brightness / level | **direct write as SA 0x11** (shadow-inject to hold) | ours |
| A/C, vent | direct write as SA `0x03` (on-change → persists) | both |
| Know true state | **feedback amps** (F9/C9/39 + 0A/CA/FA, ×0.125 A) + status bits | his + ours |
| Safety | copy-live-frame-then-modify, always; never blind constants | both |

Plus: ESP32+MCP2515-class hardware is proven on this bus by two independent
builds now; his web-UI-over-ESP32-AP model matches the "WiFi AP + web page
beats BLE" call we already made.

Contact: the author offered publicly (March 2025) to share and collaborate in
the Insiders group thread.

## 6. Full re-audit (2026-08-24) — what his code actually does

Every file read end-to-end. Corrections and additions to the above:

- **His working transmit set is exactly three IDs**: the PDM input spoof
  (`0x14EF111E/1F`), the A/C command (`0x19FEF903`), and the vent command
  (`0x19FEA603`). He **never** sends Rixen `0x788`, never writes a PDM command
  frame, and has **no dimmer code at all** — the brightness sliders in his web
  UI have no handler behind them and his README lists dimming under "cannot".
  So dimming is undocumented by everyone; our shadow-injection work is the only
  path.
- **PDM fault frames (new to us):** `0x14E9111E` / `0x14E9111F` (PGN `E900`)
  are per-PDM **short/overcurrent warnings** — he alerts when bytes 2–3 are
  non-zero. Free fault detection for our app.
- **His bus runs at 500 kbps.** Ours is wire-verified 250k many times over —
  a per-van/per-tap difference, not a conflict, but worth knowing before
  anyone tries his firmware image as-is on our van.
- **His AC setpoint code has a byte-order bug** (`acCommand` writes the
  setpoint big-endian; his own captured frames and `acSetTemp` are
  little-endian). His AC on/off/fan still work; it only confirms our LE
  handling is the right one.
- **Vent position nibble doesn't work** ("always full open or closed") — same
  limitation we hit; only open/close/speed/direction are honored.
- **His awning uses the input spoof** (PDM1 F8 byte 6 slot 3 = enable, byte 7
  slots 2/3 = out/in, 100 ms presses). Our awning is physically removed, so
  these stay unverified here.
- His van has a **second roof fan at SA 0xC1** — not present on ours.
