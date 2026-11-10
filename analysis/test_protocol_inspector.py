"""Regression tests with observed TX vectors and adversarial offline fragments."""
import contextlib
import io
import json
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from protocol_inspector import FrameStreamDecoder, inspect_frame, inspect_hex, main


INC = bytes.fromhex("02 55 30 03 38 38 0D")
DEC = bytes.fromhex("02 55 31 03 38 39 0D")
LIG = bytes.fromhex("02 55 20 03 37 38 0D")


class InspectionTests(unittest.TestCase):
    def test_three_recovered_firmware_tx_vectors(self):
        for frame, checksum in [(INC, "88"), (DEC, "89"), (LIG, "78")]:
            with self.subTest(frame=frame.hex()):
                result = inspect_hex(frame.hex(" "))
                self.assertTrue(result["valid"])
                self.assertTrue(result["checksum_valid"])
                self.assertEqual(result["checksum_expected"], checksum)
                self.assertEqual(result["checksum_received"], checksum)

    def test_wrong_checksum_and_non_uppercase_ascii_are_rejected(self):
        self.assertIn("checksum_mismatch", inspect_hex("02 55 30 03 30 30 0D")["errors"])
        self.assertIn("invalid_checksum_ascii", inspect_hex("02 55 30 03 61 61 0D")["errors"])

    def test_missing_delimiters_and_truncation_are_invalid(self):
        for frame in [b"", INC[1:], INC[:-1], INC[:4], b"\x02U0X88\r"]:
            with self.subTest(frame=frame):
                self.assertFalse(inspect_frame(frame)["valid"])

    def test_cli_emits_json_without_hardware_access(self):
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            self.assertEqual(main([INC.hex()]), 0)
        self.assertEqual(json.loads(output.getvalue())["checksum_expected"], "88")


class StreamingTests(unittest.TestCase):
    def test_fragmentation_at_every_split_and_multiple_frames(self):
        capture = INC + DEC + LIG
        for split in range(len(capture) + 1):
            with self.subTest(split=split):
                decoder = FrameStreamDecoder()
                frames = decoder.feed(capture[:split]) + decoder.feed(capture[split:])
                self.assertEqual([f["frame_hex"] for f in frames], [f.hex(" ").upper() for f in [INC, DEC, LIG]])
                self.assertTrue(all(f["valid"] for f in frames))
                self.assertEqual(decoder.finish(), [])

    def test_noise_before_stx_and_resync_after_truncated_frame(self):
        decoder = FrameStreamDecoder()
        frames = decoder.feed(b"noise\r" + INC[:3] + DEC)
        self.assertEqual(len(frames), 1)
        self.assertEqual(frames[0]["frame_hex"], DEC.hex(" ").upper())
        self.assertTrue(frames[0]["valid"])
        self.assertEqual(decoder.resync_count, 1)
        self.assertEqual(decoder.discarded_bytes, 9)

    def test_finish_reports_truncated_capture_and_resets(self):
        decoder = FrameStreamDecoder()
        self.assertEqual(decoder.feed(INC[:-1]), [])
        self.assertEqual(decoder.buffered_bytes, len(INC) - 1)
        result = decoder.finish()
        self.assertEqual(len(result), 1)
        self.assertFalse(result[0]["valid"])
        self.assertIn("truncated_capture", result[0]["errors"])
        self.assertIn("missing_cr", result[0]["errors"])
        self.assertEqual(decoder.finish(), [])
        self.assertTrue(decoder.feed(LIG)[0]["valid"])

    def test_limit_is_enforced_and_next_valid_frame_recovers(self):
        decoder = FrameStreamDecoder(max_frame_bytes=7)
        self.assertEqual(decoder.feed(b"\x02" + b"x" * 6), [])
        self.assertEqual(decoder.buffered_bytes, 7)
        self.assertEqual(decoder.feed(b"x" * 10000), [])
        self.assertEqual(decoder.buffered_bytes, 0)
        self.assertEqual(decoder.oversize_count, 1)
        result = decoder.feed(INC)
        self.assertEqual(len(result), 1)
        self.assertTrue(result[0]["valid"])
        self.assertEqual(decoder.discarded_bytes, 10007)

    def test_wrong_checksum_is_reported_then_collection_continues(self):
        decoder = FrameStreamDecoder()
        frames = decoder.feed(b"\x02U0\x0300\r" + DEC)
        self.assertEqual(len(frames), 2)
        self.assertFalse(frames[0]["valid"])
        self.assertIn("checksum_mismatch", frames[0]["errors"])
        self.assertTrue(frames[1]["valid"])

    def test_binary_payload_and_sum_wrap_without_payload_interpretation(self):
        # Fixed offline fixture: F6 + ASCII '000002' + ETX -> 0x1B.
        result = inspect_hex("02 F6 30 30 30 30 30 32 03 31 42 0D")
        self.assertTrue(result["valid"])
        self.assertEqual(result["length"], 12)
        self.assertEqual(result["payload_hex"], "F6 30 30 30 30 30 32")
        self.assertEqual(result["checksum_expected"], "1B")

    def test_invalid_limits_and_input_types(self):
        with self.assertRaises(ValueError):
            FrameStreamDecoder(max_frame_bytes=5)
        with self.assertRaises(TypeError):
            FrameStreamDecoder(max_frame_bytes=True)
        with self.assertRaises(TypeError):
            FrameStreamDecoder().feed("02")
        with self.assertRaises(ValueError):
            inspect_hex("02 zz")


if __name__ == "__main__":
    unittest.main()
