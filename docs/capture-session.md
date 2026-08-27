# CAN capture — runbook

How to get on this van's bus and read it, and how the load mapping in
[`pdm-control.md`](pdm-control.md) was produced. Follow it in order; each step
has a pass/fail check.

Everything here is **listen-only**. Transmitting is a separate, deliberate act
— see the safety notes in the [README](../README.md) — but the map has to come
first, and it comes from listening.

---

## 0. Before you leave the house

```bash
sudo apt install -y can-utils     # needs your password
```

Verify (with the CANable plugged into the laptop, nothing else connected):

```bash
lsusb | grep 1d50            # expect: 1d50:606f  (candleLight)
```

If instead you see `/dev/ttyACM0` and no `1d50:606f`, the adapter shipped with
**slcan** firmware — `can_up.sh` will detect that and print the alternate
command. Not a problem, just a different bring-up.

The `gs_usb` kernel driver ships with any current Linux kernel — no module to
install. If `lsusb` shows the adapter but no `can0` appears after bring-up,
that driver is the thing to check.

## 1. Find the wires (van powered, screen awake)

Back of the head unit, the populated **23-pin AMPSEAL** connector. Both CAN
buses are in it — see [`hardware-and-tap.md`](hardware-and-tap.md) for the full
pinout. CAN1 is the **green + yellow** pair in the accordion-sleeved group;
CAN2 is the **green + yellow** pair tagged "CONTROL PANEL".

Meter, black probe on van chassis / a ground wire, DC volts:

| Wire | Expected | Meaning |
|---|---|---|
| yellow (CAN_H) | ~2.5–3.5 V, flickering | good |
| green (CAN_L) | ~1.5–2.5 V, flickering | good |
| either | 0 V flat | wrong wire, or bus asleep |
| either | ~12 V | that's power — **do not tap it** |

Both sitting near 2.5 V and twitching = live CAN.

> If green/yellow don't behave like this, stop and re-read
> [`hardware-and-tap.md`](hardware-and-tap.md) rather than guessing.

## 2. Tap it

T-taps onto the pair — **no cutting**, they clamp over the insulation.

```
CANable CAN_H  ->  YELLOW
CANable CAN_L  ->  GREEN
CANable GND    ->  van ground (gray, pin 8) / chassis    <- do not skip this
```

- **Termination switch OFF.** The bus is already terminated at both ends; a
  third terminator unbalances it.
- **Ground is not optional** — without a shared reference the transceiver sees
  garbage and you will chase a phantom bitrate problem.

## 3. Bring the interface up

```bash
cd /path/to/this/repo
./tools/can_up.sh                    # can0 @ 250k, listen-only
candump -td can0                     # frames should scroll immediately
```

**Nothing scrolling?** Don't start swapping wires at random:

```bash
./tools/can_scan_bitrate.sh          # tries 250k/500k/125k/1M/100k/50k, safely
```

It prints the diagnosis (wrong pair / swapped H-L / bad crimp / no ground /
bus asleep). If it finds a rate, use it: `./tools/can_up.sh <rate>`.

## 4. Validate the rig — do this before trusting anything

```bash
./tools/can_capture.sh baseline 30    # 30s, everything in its normal state
./tools/can_identify.py captures/baseline.log
```

You want to see:

- **`RIG VALIDATED`** — the Rixen heater frames (`0x724`–`0x789`) are present.
  Those ids came out of the firmware, so seeing them on the wire proves we're
  decoding the right bus at the right bitrate.
- A healthy number of `*` rows (exact matches against `data/can_messages.json`)
  and J1939 PGNs that look like the ones in [`can-map.md`](can-map.md).

If the heater is simply asleep, no Rixen frames appear — that alone isn't
failure. Judge it on the J1939 matches instead. If *nothing* matches you are
probably on the other bus; move the taps to the other green/yellow pair in the
same connector and repeat.

Note the `chg` column: it flags which byte positions actually changed during the
capture. Constant bytes are padding or config; changing bytes are live data.

## 5. Map the loads — one at a time

This is the method that produced [`pdm-control.md`](pdm-control.md), and the
way to check a channel on a van that differs from this one. For each load
(galley lights, water pump, awning, fan…):

```bash
./tools/can_capture.sh galley_OFF 15     # load OFF, hands off everything else
#   -> now flip ONLY that load ON from the stock screen
./tools/can_capture.sh galley_ON 15      # load ON

./tools/can_diff.py captures/galley_OFF.log captures/galley_ON.log
```

Rows marked `**` are clean transitions — a byte that held one value with the
load off and a different single value with it on. That's your control bit; the
`bits` column tells you which bit within the byte.

**Discipline that makes this work:**

- **Change exactly one thing per capture pair.** Two loads at once and the diff
  is unreadable.
- **Confirm by repeating.** Run OFF→ON→OFF and check the same byte tracks the
  load both ways. A single capture can coincide with a temperature tick.
- Note in `docs/reverse-engineering-log.md` which physical load you flipped —
  months from now `0x18FF5021 bit 3` means nothing without that.
- `cansniffer -c can0` is great for a live feel: it highlights bytes as they
  change, so you can often *see* the frame react as you tap the screen.

Work through the PDM outputs listed in
[`signal-dictionary.md`](signal-dictionary.md) — `PDM1.DO*` / `PDM2.DO*`. Also
worth capturing: a dimmer sweep (finds the PWM byte, which will be a smooth
ramp rather than a bit flip) and a tank-level change.

## 6. Wrap up

- Keep every `captures/*.log` — they're git-ignored (raw bus data from a
  private vehicle), but they are the raw evidence behind every conclusion.
- Record confirmed frames in [`can-map.md`](can-map.md) and
  [`pdm-control.md`](pdm-control.md).

## Before transmitting

Transmitting is a real step up in risk — the same computer runs the van's DC
electrical system. Bring the interface up with `can_up.sh 250000 --tx` only
once the map is confirmed, and choose a first target so the worst case is a
light coming on: not a motor moving, not a heater igniting, not a pump running
dry. What the companion controller does and does not command, and why, is in
the [README](../README.md).
