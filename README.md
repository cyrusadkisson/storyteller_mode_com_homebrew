# Van Companion

**Control your Storyteller Overland van from your phone.**

The MODE COM touchscreen is good, and it is bolted to the galley. That is fine
until you are somewhere else in the van:

- You start driving and can't remember whether you left the roof vent open.
- You're in a lawn chair outside and the A/C kicks on.
- Your partner is asleep and you left the awning light on.
- You're in bed and the water pump is still running.

This project adds a second way in. A small board taps the van's control bus and
serves a web page over its own Wi-Fi, so the lights, water pump, roof A/C, roof
vent and inverter work from a phone anywhere in or around the van — along with
battery, tank and temperature readings you'd otherwise walk to the screen for.

**Nothing about the factory system changes.** The stock firmware is never
touched, the panel keeps working exactly as it did, and it remains the fallback.
The board only listens and speaks on a wire the van already uses.

> 📷 **Screenshots go here** — `docs/img/app-battery.png`, `app-lights.png`,
> `app-climate.png`. See [`docs/img/README.md`](docs/img/README.md).

---

# ⚠️ Read this first

**Use at your own risk. No warranty, including as to accuracy.** This is amateur
reverse engineering by a van owner, not vendor guidance, and some of it is
certainly wrong.

- **48 V, not 12 V.** The house pack sits near 53 V charged and can push
  hundreds of amps into a short. DC arcs don't self-extinguish.
- **It commands real machinery** — pumps, motors, heaters. Motor channels
  *latch*: they keep driving until something writes zero.
- **Tested on exactly one van:** a 2024 MODE Classic with a **single 8.4 kWh
  Lithionics** pack. Requires a Lithionics BMS.
- **Most Storyteller vans have the dual 16.8 kWh system.** The per-cell
  monitoring assumes one 16-cell module and will be wrong or incomplete on a
  two-battery van. Everything else should still apply. Untested.
- Tapping wiring may **void warranties** and affect insurance claims.
- Don't use it on a vehicle you don't own.

If you're not prepared to own the consequences on your own van, don't proceed.

---

## What it does, and doesn't

**Controls:** cabin / cargo / aux lights, water pump, recirculation pump, roof
A/C (off / cool / heat, compressor, fan speed, temperature setpoint), roof vent
(lid, fan, direction, speed), inverter.

**Shows:** state of charge, power flow, time to full or empty, pack voltage /
current / temperature, per-cell voltages, tank levels, cabin temperature, AC line
voltage and frequency, shore power limit, Rixen heater state, per-channel power
draw, board temperature — most with 48-hour charts.

**Won't do**, and these are limitations of tapping the bus in parallel rather
than oversights — reasons in [`docs/design-notes.md`](docs/design-notes.md):

- **No dimming.** Holding a brightness would need ~50 % more bus traffic. Use
  the panel.
- **No Rixen heater control.** The head unit reverts any command within ~5 s.
  Read-only.
- **The factory screen doesn't follow the app.** It updates for lights and pumps
  (those work by spoofing a wall switch), but not for A/C or vent changes. The
  app *does* follow the screen.
- **No reading lights, no sink drain.** Neither can be held by a parallel tap.
- **Awning is decoded but unverified** — the van it was worked out on doesn't
  have one.

---

## Installation

Budget an afternoon. You need to be comfortable pulling the galley panel,
crimping a wire, and drilling four small holes in a plastic bracket.

### Parts

| | |
|---|---|
| **LILYGO T-2CAN-FD** (ESP32-S3, dual CAN) | *[add AliExpress link]* · *[add Amazon link]* |
| **6 × Posi-Tap** connectors | for 18–22 AWG · 📷 `docs/img/part-positap.jpg` |
| **6 × spade connector pairs** *(optional)* | so the board can be unplugged · 📷 `docs/img/part-spades.jpg` |
| **20–22 AWG wire**, ~2 ft | three colours helps |
| **Zip ties + foam padding** | for mounting |
| **6 ft USB-C to USB-A cable** | power. A **left-angled** USB-A end routes far better, if you can find one. |

### Tools

- Wire cutter / stripper / crimper — 📷 `docs/img/tool-crimper.jpg`
- Phillips screwdriver
- Small flat-head ("electronics") screwdriver, for the board's terminal blocks
- Drill with a **3/16"** bit

---

### 1. Flash the firmware

Do this at a desk, before touching the van. The board only needs USB.

**Set your own Wi-Fi password first.** The board has no screen, so credentials
are compiled in — and the defaults are published in this public repo, meaning
anyone in range who has read it could join and operate your van. Open
`firmware/t2can/examples/app/app.ino`, find these two lines near the top, and
change them:

```c
static const char *AP_SSID = "VanCompanion";
static const char *AP_PASS = "storyteller";      // WPA2 requires >= 8 chars
```

Then install [PlatformIO](https://platformio.org/install/cli) and build:

```bash
git clone https://github.com/cyrusadkisson/storyteller_mode_com_homebrew.git
cd storyteller_mode_com_homebrew
pio run -d firmware/t2can -t upload
```

Platform notes:

- **Linux** — the board appears as `/dev/ttyACM0`. If upload fails with a
  permissions error, add yourself to `dialout`
  (`sudo usermod -aG dialout $USER`) and **reboot** — logging out is not always
  enough, because your systemd user session keeps the old group list.
- **macOS** — appears as `/dev/cu.usbmodem*`. No driver needed.
- **Windows** — appears as a COM port. If PlatformIO can't find it, pass
  `--upload-port COM5` with the right number from Device Manager.

Confirm it worked: open the serial monitor at 115200 baud and you should see

```
companion app: boot
canA: CAN1 250k OK
canB: CAN2 250k OK
AP VanCompanion up, ip 192.168.4.1
```

`CAN … OK` here only means the controllers initialised — the wiring is proven
later, in the van.

### 2. Wire it in

**Turn the system off at the panel first.**

1. Unscrew the galley panel and its enclosure and let it hang. **Don't
   disconnect anything.**
2. Posi-Tap the six wires — CAN1 high/low/ground and CAN2 high/low/ground.
   Which wires, with photos, is in
   [`docs/hardware-and-tap.md`](docs/hardware-and-tap.md).
   📷 `docs/img/install-taps.jpg`
3. Run them to the board's screw terminals: **CAN-A = CAN1** (the accordion-
   sleeved pair), **CAN-B = CAN2** (the pair tagged CONTROL PANEL). **Label both
   ends now** — they are indistinguishable in an hour.
   📷 `docs/img/install-terminals.jpg`
4. *(Optional)* Crimp spade connectors so the board can be removed without
   re-tapping.
5. Plug the USB cable into the board; the other end goes to the pillar behind
   the driver.

> **Don't swap the buses.** The software could be changed to swap them, but
> deliberately isn't: CAN1 is the flakier bus, and the MCP2518FD side (CAN-A)
> recovers from bus-off, while the ESP32's TWAI side (CAN-B) does not.

### 3. Mount it

1. Loosen the MODE COM screen enough to gain room. Don't remove it.
2. Drill four **3/16"** holes in the bracket — 📷 `docs/img/install-holes.jpg`
3. Fish zip ties through — 📷 `docs/img/install-zipties.jpg`
4. Cut the foam pad to the board's footprint — 📷 `docs/img/install-foam.jpg`
5. Seat the board on the foam and cinch it down — 📷 `docs/img/install-mounted.jpg`
6. Route the USB cable down through the driver-side plastics —
   📷 `docs/img/install-usb.jpg`
7. **Test before closing up:** power on, join the `VanCompanion` Wi-Fi, open
   <http://192.168.4.1>, and check the footer shows both CAN counters climbing.
   That is the real proof the taps are good.
8. Re-tighten the screen and refit the panel and enclosure.

**Note on Wi-Fi:** joining the board's access point takes your phone off the
internet, since the board isn't a gateway. That's normal.

---

## Features

> 📷 **Screenshots go here.** Each section below wants one — filenames and a
> checklist are in [`docs/img/README.md`](docs/img/README.md).

**Battery & Power** — charge, live power flow with a 48-hour chart, time to full
or empty, pack temperature and its own chart, inverter on/off, shore power limit.
📷 `docs/img/app-battery.png`

One place the app is simply more correct than the panel: when the BMS declines
to estimate time remaining it sends `0xFFFF`, and the factory screen prints that
sentinel literally as **45d 12h**. The app computes the figure instead.

**Lights & switches** — cabin, cargo, aux, water pump, recirculation, with
per-channel power draw. 📷 `docs/img/app-lights.png`

**Climate** — A/C off / cool / heat, compressor, fan auto / low / high,
temperature setpoint, cabin temperature with a 48-hour chart.
📷 `docs/img/app-climate.png`

**Roof vent** — open / close, fan on/off, airflow direction, speed.
📷 `docs/img/app-vent.png`

**Cell monitor** — all sixteen cell voltages, the weakest one called out, and
the spread across the pack. 📷 `docs/img/app-cells.png`

**Warnings that watch while you're away.** The board logs to flash and keeps
watching with no phone connected:

- **Weak cell** — the BMS opens the contactor on the *weakest cell*, never the
  pack average, which is why these vans can die showing 80 % charge. The app
  warns when a cell nears its floor or drifts from the pack.
- **Voltage / charge disagreement** — a backstop for the same failure, using
  only pack-level data.
- **Board over-temperature** — sustained above 185 °F.

Events are written to flash with real timestamps and survive a total power loss,
so after a shutdown you can read what the pack was doing on the way down:
`http://192.168.4.1/api/log`.

---

## Documentation

| | |
|---|---|
| [`design-notes.md`](docs/design-notes.md) | why CAN, why a parallel tap, what that rules out, bus safety |
| [`hardware-and-tap.md`](docs/hardware-and-tap.md) | which wires to tap, with photos |
| [`architecture.md`](docs/architecture.md) | the stock system, inside and out |
| [`energy-can2.md`](docs/energy-can2.md) | battery, inverter, charger, per-cell decode |
| [`pdm-control.md`](docs/pdm-control.md) | how loads are switched |
| [`climate-control.md`](docs/climate-control.md) | A/C, thermostat, vent, Rixen |
| [`can-map.md`](docs/can-map.md) · [`signal-dictionary.md`](docs/signal-dictionary.md) | the wire-level message and signal maps |
| [`capture-session.md`](docs/capture-session.md) | runbook: getting on your own bus and mapping a load |
| [`t2can-bench.md`](docs/t2can-bench.md) | board bring-up, and two bugs in LILYGO's stock example |

```
docs/       documentation and reverse-engineering notes
firmware/   the companion controller's firmware
tools/      scripts that work on your own local firmware copy
data/       machine-readable CAN map and channel tables
```

---

## Not affiliated

An independent, unofficial project by a Storyteller Overland owner. **Not
affiliated with, endorsed by, or supported by** Storyteller Overland, Enovation
Controls / Murphy, JET Technologies, Lithionics, Rixen, or any other vendor
named here. All trademarks belong to their owners.

## License

MIT — see [`LICENSE`](LICENSE). Covers **this repository's own** analysis,
documentation, tooling and firmware. It grants no rights to vendor firmware,
which is not included here.

Third-party code:

- [`firmware/t2can/libraries/Longan_CANFD/`](firmware/t2can/libraries/Longan_CANFD/)
  — MCP2518FD driver, © Longan Labs, MIT (its own `LICENSE` included). Vendored
  because two bugs in LILYGO's stock example for this board break CAN work; see
  [`t2can-bench.md`](docs/t2can-bench.md).
- `firmware/t2can/libraries/private_library/pin_config.h` — LILYGO's pin
  definitions, unmodified from
  [Xinyuan-LilyGO/T-2Can](https://github.com/Xinyuan-LilyGO/T-2Can). No license
  header of its own; it is 23 `#define`s of GPIO numbers and every sketch needs
  it to compile.

Protocol facts in [`modewifi-analysis.md`](docs/modewifi-analysis.md) were
corroborated against [ModeWifi](https://github.com/changer65535/ModeWifi)
(GPL-3.0), an independent owner's project. **No code from it is used here** —
observations about a shared vehicle bus aren't copyrightable expression, and
mixing GPL-3 code into this MIT repo is deliberately avoided.
