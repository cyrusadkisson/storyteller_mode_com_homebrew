"""Shadow-inject a PDM command right behind the head unit's own frame.

The head unit re-broadcasts the PDM command frames (~0x14EF1E11) at ~45 Hz, so
any single command we send is overwritten within ~22 ms. This tool listens for
the head unit's frame and sends OUR version immediately after it, inside its
quiet gap, with the same source address (0x11). That is the only source the
PDM has been shown to obey, and timing off the observed frame keeps the
collision odds near zero (a collision would produce a CAN error frame; a few
are self-healing, a battle is not — which is why this is bounded).

SAFETY: the payload is built by COPYING the head unit's live frame and
changing only the requested byte, so the other five channels keep their
current commanded values. Default bound: 45 injections (~1 second).

  python3 tools/can_shadow.py 14EF1E11 FC 4 00          # 45 injections, 2s cap
  python3 tools/can_shadow.py 14EF1E11 FD 6 00 20       # cap 20 injections

Args: <can_id_hex> <mux_byte_hex> <byte_index_1_to_7> <value_hex> [max_injections]
"""

import socket
import struct
import sys
import time

CAN_EFF_FLAG = 0x80000000
CAN_FRAME = struct.Struct("<IB3x8s")  # can_id, dlc, pad, data[8]


def main() -> int:
    if len(sys.argv) not in (5, 6):
        sys.exit(__doc__)
    can_id = int(sys.argv[1], 16)
    mux = int(sys.argv[2], 16)
    byte_index = int(sys.argv[3])  # 1-based payload index
    value = int(sys.argv[4], 16)
    max_inj = int(sys.argv[5]) if len(sys.argv) == 6 else 45
    if not (1 <= byte_index <= 7):
        sys.exit("byte_index must be 1..7")

    rx = socket.socket(socket.AF_CAN, socket.SOCK_RAW, socket.CAN_RAW)
    rx.setsockopt(
        socket.SOL_CAN_RAW,
        socket.CAN_RAW_FILTER,
        struct.pack("=II", can_id | CAN_EFF_FLAG, 0x1FFFFFFF | CAN_EFF_FLAG),
    )
    rx.bind(("can0",))
    rx.settimeout(2.0)

    print(f"shadowing {can_id:#x}[{mux:02X}] byte {byte_index} -> {value:#04x}, "
          f"max {max_inj} injections, 2 s timeout", file=sys.stderr)

    sent = 0
    deadline = time.monotonic() + 2.0
    while sent < max_inj and time.monotonic() < deadline:
        try:
            fid, dlc, data = CAN_FRAME.unpack(rx.recv(CAN_FRAME.size))
        except socket.timeout:
            break
        if dlc < 8 or data[0] != mux:
            continue
        # Copy the live frame; change only the requested byte.
        ours = bytearray(data)
        ours[byte_index] = value
        tx = socket.socket(socket.AF_CAN, socket.SOCK_RAW, socket.CAN_RAW)
        tx.bind(("can0",))
        tx.send(CAN_FRAME.pack(can_id | CAN_EFF_FLAG, 8, bytes(ours)))
        tx.close()
        sent += 1

    print(f"injected {sent} frames", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
