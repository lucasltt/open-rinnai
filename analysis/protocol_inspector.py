"""Offline inspection of candidate Rinnai serial frames; never accesses hardware.

The checksum rule was recovered from the module's TX firmware. The RX reader
also sums fixed indices 1 through 12, but its full framing has not been verified
against physical captures. Variable ETX-based framing below is proven for TX.
No command builders, serial ports, sockets or transmission functions are present.

Examples:
    python analysis/protocol_inspector.py "02 55 30 03 38 38 0D"
    python analysis/protocol_inspector.py --stream "FF 02 55 31 03 38 39 0D"
"""
from __future__ import annotations

import argparse
import json
from typing import Any

STX = 0x02
ETX = 0x03
CR = 0x0D
MIN_FRAME_BYTES = 6
CHECKSUM_RULE = "TX: sum(frame[1:ETX+1]) & 0xFF; RX firmware sums fixed indices 1..12, RX framing unverified"


def inspect_frame(frame: bytes) -> dict[str, Any]:
    """Check structure and candidate checksum; do not interpret payload fields."""
    if not isinstance(frame, (bytes, bytearray, memoryview)):
        raise TypeError("frame must be bytes-like")
    data = bytes(frame)
    errors: list[str] = []
    if len(data) < MIN_FRAME_BYTES:
        errors.append("frame_too_short")
    if not data or data[0] != STX:
        errors.append("missing_stx")
    if not data or data[-1] != CR:
        errors.append("missing_cr")

    # A complete frame has ETX exactly four bytes before its end. In an
    # incomplete frame, retain the final ETX only to aid offline diagnostics.
    etx_position = len(data) - 4 if data and data[-1] == CR else data.rfind(bytes([ETX]))
    has_etx = etx_position >= 1 and data[etx_position] == ETX
    if not has_etx:
        errors.append("missing_etx_before_checksum")

    expected: str | None = None
    received: str | None = None
    checksum_valid: bool | None = None
    payload: str | None = None
    if has_etx:
        payload = data[1:etx_position].hex(" ").upper()
        expected = f"{sum(data[1:etx_position + 1]) & 0xFF:02X}"
        raw_checksum = data[etx_position + 1:etx_position + 3]
        received = raw_checksum.decode("ascii", errors="replace")
        if len(raw_checksum) != 2 or any(byte not in b"0123456789ABCDEF" for byte in raw_checksum):
            errors.append("invalid_checksum_ascii")
            checksum_valid = False
        else:
            checksum_valid = received == expected
            if not checksum_valid:
                errors.append("checksum_mismatch")

    return {
        "valid": not errors,
        "length": len(data),
        "frame_hex": data.hex(" ").upper(),
        "payload_hex": payload,
        "checksum_expected": expected,
        "checksum_received": received,
        "checksum_valid": checksum_valid,
        "checksum_rule": CHECKSUM_RULE,
        "errors": errors,
    }


def inspect_hex(hex_string: str) -> dict[str, Any]:
    """Inspect a single frame expressed as hexadecimal bytes (spaces allowed)."""
    return inspect_frame(bytes.fromhex(hex_string))


class FrameStreamDecoder:
    """Bounded STX/CR frame collector for offline byte captures.

    feed() accepts arbitrary fragments and returns every CR-terminated candidate,
    including malformed frames. A new STX restarts collection. Overlong frames
    are dropped until another STX is seen. finish() reports a trailing incomplete
    candidate. This framing assumes STX and CR are not escaped payload bytes;
    that assumption must be checked against actual RX captures.
    """

    def __init__(self, max_frame_bytes: int = 4096) -> None:
        if not isinstance(max_frame_bytes, int) or isinstance(max_frame_bytes, bool):
            raise TypeError("max_frame_bytes must be an integer")
        if max_frame_bytes < MIN_FRAME_BYTES:
            raise ValueError(f"max_frame_bytes must be at least {MIN_FRAME_BYTES}")
        self.max_frame_bytes = max_frame_bytes
        self._buffer = bytearray()
        self.discarded_bytes = 0
        self.resync_count = 0
        self.oversize_count = 0

    @property
    def pending_hex(self) -> str:
        return self._buffer.hex(" ").upper()

    @property
    def buffered_bytes(self) -> int:
        return len(self._buffer)

    def feed(self, chunk: bytes) -> list[dict[str, Any]]:
        if not isinstance(chunk, (bytes, bytearray, memoryview)):
            raise TypeError("chunk must be bytes-like")
        results: list[dict[str, Any]] = []
        for byte in bytes(chunk):
            if byte == STX:
                if self._buffer:
                    self.resync_count += 1
                    self.discarded_bytes += len(self._buffer)
                self._buffer = bytearray([STX])
                continue
            if not self._buffer:
                self.discarded_bytes += 1
                continue
            # Check the bound before appending, so even temporary buffer length
            # never exceeds the caller's limit.
            if len(self._buffer) == self.max_frame_bytes:
                self.oversize_count += 1
                self.discarded_bytes += len(self._buffer) + 1
                self._buffer.clear()
                continue
            self._buffer.append(byte)
            if byte == CR:
                results.append(inspect_frame(self._buffer))
                self._buffer.clear()
        return results

    def finish(self) -> list[dict[str, Any]]:
        """Report and clear remaining truncated data at the end of a capture."""
        if not self._buffer:
            return []
        result = inspect_frame(self._buffer)
        result["errors"].append("truncated_capture")
        result["valid"] = False
        self._buffer.clear()
        return [result]


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("hex_bytes", help="Offline capture as hexadecimal bytes")
    parser.add_argument("--stream", action="store_true", help="Decode concatenated or noisy capture")
    parser.add_argument("--max-frame-bytes", type=int, default=4096, help="Stream buffer limit (default: 4096)")
    args = parser.parse_args(argv)
    try:
        data = bytes.fromhex(args.hex_bytes)
        if args.stream:
            decoder = FrameStreamDecoder(args.max_frame_bytes)
            output: Any = {
                "frames": decoder.feed(data) + decoder.finish(),
                "discarded_bytes": decoder.discarded_bytes,
                "resync_count": decoder.resync_count,
                "oversize_count": decoder.oversize_count,
            }
        else:
            output = inspect_frame(data)
    except (TypeError, ValueError) as exc:
        parser.error(str(exc))
    print(json.dumps(output, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
