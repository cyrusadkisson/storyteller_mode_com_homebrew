#!/usr/bin/env bash
# Bring up the CANable as a SocketCAN interface, LISTEN-ONLY by default.
#
#   ./tools/can_up.sh                 # can0 @ 250k, listen-only  (SAFE DEFAULT)
#   ./tools/can_up.sh 500000          # different bitrate
#   ./tools/can_up.sh 250000 --tx     # allow transmit (ONLY after validation)
#
# Listen-only matters: in normal mode the adapter ACKs every frame, and if the
# bitrate is wrong it will spew error frames onto a bus that runs the van's DC
# system. In listen-only the transceiver physically cannot drive the bus.
set -euo pipefail

BITRATE="${1:-250000}"
MODE="${2:-}"
IFACE="${IFACE:-can0}"

say() { printf '\033[1m%s\033[0m\n' "$*"; }
die() { printf '\033[31mERROR: %s\033[0m\n' "$*" >&2; exit 1; }

command -v candump >/dev/null || die "can-utils not installed.  sudo apt install -y can-utils"

# --- Is the adapter actually there? ----------------------------------------
if ! lsusb | grep -qiE '1d50:606f|openmoko|candlelight|geschwister'; then
  if lsusb | grep -qiE '16d0:117e|0483:5740' || ls /dev/ttyACM* >/dev/null 2>&1; then
    cat >&2 <<'EOF'
The adapter looks like it is running *slcan* firmware (serial), not candleLight.
It will appear as /dev/ttyACM0 instead of can0.  Bring it up with:

    sudo slcand -o -c -f -s5 /dev/ttyACM0 can0     # -s5 = 250 kbit/s
    sudo ip link set can0 up

(-s4 = 125k, -s5 = 250k, -s6 = 500k).  Then re-run the capture script.
EOF
    exit 1
  fi
  die "No CANable found on USB. Plug it in and re-run.  (lsusb should show 1d50:606f)"
fi
say "✓ CANable detected on USB (candleLight/gs_usb)"

# --- Configure -------------------------------------------------------------
sudo ip link set "$IFACE" down 2>/dev/null || true

# NOTE: no `restart-ms` here. candleLight firmware reports "Device doesn't
# support restart from Bus Off", and it is irrelevant in listen-only anyway —
# you cannot enter bus-off if you never transmit.
if [[ "$MODE" == "--tx" ]]; then
  printf '\033[33m!! TRANSMIT ENABLED — this adapter can now drive the van bus.\033[0m\n'
  sudo ip link set "$IFACE" up type can bitrate "$BITRATE" \
    || die "Failed to bring up $IFACE at ${BITRATE} bps."
else
  sudo ip link set "$IFACE" up type can bitrate "$BITRATE" listen-only on \
    || die "Interface refused listen-only mode. Update the CANable firmware, or accept
       the risk and pass --tx explicitly. Do NOT silently fall back."

  # Trust the readback, not the exit code: confirm the flag is actually set.
  if ! ip -details link show "$IFACE" | grep -qi 'listen-only'; then
    sudo ip link set "$IFACE" down 2>/dev/null || true
    die "listen-only did not take effect — refusing to sniff a live vehicle bus
       with a transmit-capable adapter. Check the firmware."
  fi
fi

say "✓ $IFACE up @ ${BITRATE} bps  ($([[ "$MODE" == "--tx" ]] && echo TRANSMIT || echo 'listen-only, verified'))"
ip -details -statistics link show "$IFACE" | sed 's/^/    /'

cat <<EOF

Next:
  candump -td $IFACE              # live frames, delta timestamps
  cansniffer -c $IFACE            # live view, highlights CHANGING bytes  <-- best for load hunting
  ./tools/can_capture.sh baseline # record to a file

If you see NOTHING within ~5 seconds, the bitrate is probably wrong or you are
on the quiet pair. Try:  ./tools/can_scan_bitrate.sh
EOF
