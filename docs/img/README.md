# Documentation images

Images referenced from the main [`README.md`](../../README.md) live here. This
directory is a deliberate exception to the repo-wide image ignore rule — the
private van photos still stay in `images/`, which is git-ignored.

**Strip EXIF before adding anything.** Phone photos carry GPS coordinates, and
this repository is public:

```bash
exiftool -all= docs/img/*.jpg          # or: mogrify -strip docs/img/*.jpg
```

## Expected files

The README has visible placeholders wherever one of these is missing. Replace
each placeholder line with a normal image reference once the file is added.

| file | what it should show |
|---|---|
| `app-battery.png` | Battery & Power section — charge, power flow chart, pack temp |
| `app-lights.png` | Lights & switches |
| `app-climate.png` | Climate — A/C mode, fan, setpoint, cabin chart |
| `app-vent.png` | Roof vent controls |
| `app-cells.png` | Cell monitor with the weak cell marked |
| `part-t2can.jpg` | The LILYGO T-2CAN-FD board |
| `part-positap.jpg` | Posi-Tap connectors |
| `part-spades.jpg` | Spade connector pairs |
| `tool-crimper.jpg` | Wire cutter / stripper / crimper |
| `install-taps.jpg` | The six tapped wires, labelled |
| `install-terminals.jpg` | Wires landed on the board's CAN terminals |
| `install-holes.jpg` | The four drilled mounting holes |
| `install-zipties.jpg` | Zip ties fished through the holes |
| `install-foam.jpg` | Foam pad cut to the board |
| `install-mounted.jpg` | Board mounted in place |
| `install-usb.jpg` | USB cable routed down the driver-side pillar |
