# CAN capture session — runbook

Follow this in order, in the van. Each step has a pass/fail check; **don't move
on until the current step passes.** The goal of session #1 is *listening only*:
prove we're on the right bus, then crack the PDM output frames that
[`pdm-control.md`](pdm-control.md) couldn't recover statically.

**Nothing here transmits.** The adapter is in listen-only mode the whole time,
so it physically cannot drive the bus that runs the van's DC system.

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

Kernel driver `gs_usb` is already present on this laptop. Nothing else to install.

## 1. Find the wires (van powered, screen awake)

Back of the head unit, **"CONTROL PANEL"** connector — the small one; the two
big 35-pin connectors are empty. Look for the **green + yellow twisted pair**.

Meter, black probe on van chassis / a black wire, DC volts:

| Wire | Expected | Meaning |
|---|---|---|
| yellow (CAN_H) | ~2.5–3.5 V, flickering | good |
| green (CAN_L) | ~1.5–2.5 V, flickering | good |
| either | 0 V flat | wrong wire, or bus asleep |
| either | ~12 V | that's power — **do not tap it** |

Both sitting near 2.5 V and twitching = live CAN. A second pair (teal/gold) is
the likely CAN2 — that's the fallback if this pair turns out quiet.

While the meter is already out, also find **+12V and ground** in this same
bundle (the red and black wires — confirm, don't assume) and write down which is
which. That's where the companion box gets its power in phase 3; see
[`hardware-and-tap.md`](hardware-and-tap.md). Nothing to wire today.

> If green/yellow don't behave like this, stop and re-read
> [`hardware-and-tap.md`](hardware-and-tap.md) rather than guessing.

## 2. Tap it

T-taps onto the pair — **no cutting**, they clamp over the insulation.

```
CANable CAN_H  ->  YELLOW
CANable CAN_L  ->  GREEN
CANable GND    ->  van chassis / black wire     <- do not skip this
```

The user's own jumper leads carry a mnemonic: **sky = high**, **ocean = low**,
**ground = black**.

```
WHITE jumper  <->  van YELLOW  (CAN_H)
BLUE  jumper  <->  van GREEN   (CAN_L)
BLACK jumper  <->  van BLACK   (ground)
```

- **Termination switch OFF.** The bus is already terminated at both ends; a
  third terminator unbalances it.
- Ground is not optional — without a shared reference the transceiver sees
  garbage and you'll chase a phantom bitrate problem.
- Your jumper wire is the white 24 AWG guitar hookup wire. It's thinner than the
  red TICONN spades' 22–18 AWG range, so **fold the stripped end back on itself**
  to fill the barrel before crimping, then tug-test each one. Mark one end —
  both jumpers are white and you do not want to discover a swap later.

## 3. Bring the interface up

```bash
cd ~/storyteller_mode_com
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
failure. Judge it on the J1939 matches instead. If *nothing* matches, you're
probably on CAN2; move the taps to the teal/gold pair and repeat.

Note the `chg` column: it flags which byte positions actually changed during the
capture. Constant bytes are padding or config; changing bytes are live data.

## 5. Crack the loads — one at a time

This is the actual objective. For each load (galley lights, water pump, awning,
fan…):

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

- Keep every `captures/*.log` — they're git-ignored (raw bus data from the
  user's van), but they're the raw evidence behind every conclusion.
- Record confirmed frames in [`can-map.md`](can-map.md) and close out the open
  question in [`pdm-control.md`](pdm-control.md).

## What session #2 looks like (not now)

Only once the map is confirmed: re-run `can_up.sh 250000 --tx` and send a single
frame to a harmless load with the van in a safe state. Transmitting is a real
step up in risk — the same computer runs the van's DC electrical system — so it
gets its own session, its own plan, and a load chosen so that the worst case is
a light turning on.
