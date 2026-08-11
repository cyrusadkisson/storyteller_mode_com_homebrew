#!/usr/bin/env bash
# Record a timestamped candump log into captures/.
#
#   ./tools/can_capture.sh baseline           # record until Ctrl-C
#   ./tools/can_capture.sh galley_lights_on 20  # record for 20 seconds
#
# Workflow for cracking a load (e.g. galley lights):
#   1. ./tools/can_capture.sh galley_OFF 15    <- load off, don't touch anything
#   2. flip the load ON from the stock screen
#   3. ./tools/can_capture.sh galley_ON  15
#   4. ./tools/can_diff.py captures/galley_OFF.log captures/galley_ON.log
set -euo pipefail

NAME="${1:-capture}"
SECS="${2:-}"
IFACE="${IFACE:-can0}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="$ROOT/captures/$(date +%Y%m%d_%H%M%S)_${NAME}.log"

mkdir -p "$ROOT/captures"
ip link show "$IFACE" 2>/dev/null | grep -q 'state UP' \
  || { echo "$IFACE is not up. Run ./tools/can_up.sh first."; exit 1; }

echo "Recording $IFACE -> $OUT"
if [[ -n "$SECS" ]]; then
  echo "  (${SECS}s)"
  timeout "$SECS" candump -L "$IFACE" > "$OUT" || true
else
  echo "  (Ctrl-C to stop)"
  candump -L "$IFACE" > "$OUT" || true
fi

# Stable name alongside the timestamped one, so diffs are easy to type.
ln -sf "$(basename "$OUT")" "$ROOT/captures/${NAME}.log"

frames=$(wc -l < "$OUT")
ids=$(awk -F'[ #]' '{print $3}' "$OUT" | sort -u | wc -l)
echo "  $frames frames, $ids distinct CAN ids"
[[ "$frames" -eq 0 ]] && echo "  !! nothing captured — see ./tools/can_scan_bitrate.sh"
echo "  ./tools/can_identify.py captures/${NAME}.log"
