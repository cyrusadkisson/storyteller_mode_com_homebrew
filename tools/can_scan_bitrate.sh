#!/usr/bin/env bash
# Find the bus bitrate by trying each candidate in LISTEN-ONLY mode and seeing
# which one produces valid frames. Listen-only means a wrong guess cannot
# disturb the bus (no error frames are transmitted).
#
#   ./tools/can_scan_bitrate.sh
set -euo pipefail

IFACE="${IFACE:-can0}"
DWELL="${DWELL:-4}"          # seconds to listen at each rate
RATES=(250000 500000 125000 1000000 100000 50000)

command -v candump >/dev/null || { echo "need can-utils: sudo apt install -y can-utils"; exit 1; }

echo "Scanning ${IFACE} for a live bus (${DWELL}s per rate, listen-only)..."
echo

BEST=""; BESTN=0
for rate in "${RATES[@]}"; do
  sudo ip link set "$IFACE" down 2>/dev/null || true
  if ! sudo ip link set "$IFACE" up type can bitrate "$rate" listen-only on 2>/dev/null; then
    printf '  %-8s  (interface refused this rate)\n' "$rate"
    continue
  fi

  n=$(timeout "$DWELL" candump -n 400 "$IFACE" 2>/dev/null | wc -l || true)
  errs=$(ip -statistics link show "$IFACE" | awk '/RX:/{getline; print $3}')

  printf '  %-8s  frames=%-5s rx_errors=%s' "$rate" "$n" "${errs:-?}"
  if [[ "$n" -gt "$BESTN" ]]; then BEST="$rate"; BESTN="$n"; printf '   <-- best so far'; fi
  printf '\n'
done

sudo ip link set "$IFACE" down 2>/dev/null || true
echo

if [[ -z "$BEST" || "$BESTN" -eq 0 ]]; then
  cat <<'EOF'
No traffic at any rate. That means one of:
  1. You are on the wrong wire pair  -> try the teal/gold pair (CAN2)
  2. CAN_H / CAN_L are swapped       -> swap the two T-tap leads
  3. The T-taps did not bite through the insulation -> re-check with the meter
  4. No shared ground                -> CANable GND must go to van chassis/black
  5. The van is asleep               -> wake the screen, turn something on
EOF
  exit 1
fi

echo "==> Bus is alive at ${BEST} bps (${BESTN} frames in ${DWELL}s)"
echo "    Bring it up with:  ./tools/can_up.sh ${BEST}"
