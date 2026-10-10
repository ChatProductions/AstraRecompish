#!/usr/bin/env python3
"""Owned native-grid replay checks; not a PS2 hardware/BIOS framebuffer oracle."""

import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

import test_gif_replay as fixtures


NATIVE_BINARY = QUARTER_BINARY = None


class NativeReplayTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="astra-native-replay-")
        self.directory = Path(self.temporary.name)
        self.trace = self.directory / "owned.astgif"
        self.trace.write_bytes(fixtures.serialized_trace(
            [*fixtures.owned_payloads(), fixtures.linear_payload()]))

    def tearDown(self):
        self.temporary.cleanup()

    def replay(self, *arguments, binary=None, trace=None):
        return subprocess.run([str(binary or NATIVE_BINARY), str(trace or self.trace),
                               *map(str, arguments)], env=fixtures.clean_environment(),
                              text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                              timeout=20, check=False)

    def test_crop_has_independent_pixels_and_unmodified_active_output(self):
        active = self.directory / "native-active.ppm"
        plain = self.replay("--framebuffer", active)
        self.assertEqual(plain.returncode, 0, plain.stderr)
        self.assertTrue(active.read_bytes().startswith(b"P6\n640 448\n255\n"))
        image = self.directory / "native-crop.ppm"
        active_crop = self.directory / "native-with-crop.ppm"
        result = self.replay("--linear-framebuffer", image,
                             "--dispfb", f"{fixtures.linear_dispfb():X}",
                             "--linear-size", "8x8", "--framebuffer", active_crop)
        self.assertEqual(result.returncode, 0, result.stderr)
        words = [fixtures.linear_pixel(x, y) for y in range(3, 11) for x in range(5, 13)]
        self.assertEqual(image.read_bytes(), fixtures.ppm_words(words, 8, 8))
        self.assertIn("native_size=8x8 sample_scale=1 output_size=8x8", result.stdout)
        self.assertIn(f"word_hash={fixtures.hash_words(words)}", result.stdout)
        self.assertIn("scope=native-pixel-grid-approximate-linear-gs", result.stdout)
        self.assertEqual(active.read_bytes(), active_crop.read_bytes())
        for field in ("framebuffer_hash", "logical_vram_word_hash"):
            expected = re.search(rf"{field}=(\w+)", plain.stdout).group(1)
            self.assertIn(f"{field}={expected}", result.stdout)

    def test_point_is_not_a_four_by_four_replication(self):
        color = 0x8055AA33
        payload = fixtures.ad((0x4C, 1 << 16), (0x18, 0),
                              (0x40, (63 << 16) | (15 << 48)), (0x47, 0),
                              (0, 0), (1, color), (5, fixtures.xyz(5, 7)))
        trace = self.directory / "one-pixel.astgif"
        trace.write_bytes(fixtures.serialized_trace([payload]))
        native_image = self.directory / "one-native.ppm"
        result = self.replay("--linear-framebuffer", native_image,
                             "--dispfb", "200", "--linear-size", "64x16", trace=trace)
        self.assertEqual(result.returncode, 0, result.stderr)
        expected = [color if (x, y) == (5, 7) else 0
                    for y in range(16) for x in range(64)]
        self.assertEqual(native_image.read_bytes(), fixtures.ppm_words(expected, 64, 16))
        self.assertIn("nonblack_rgb_samples=1/1024", result.stdout)
        quarter_image = self.directory / "one-quarter.ppm"
        quarter = self.replay("--linear-framebuffer", quarter_image,
                              "--dispfb", "200", "--linear-size", "64x16",
                              trace=trace, binary=QUARTER_BINARY)
        self.assertEqual(quarter.returncode, 0, quarter.stderr)
        expected_quarter = [color if (x, y) == (1, 1) else 0
                            for y in range(4) for x in range(16)]
        self.assertEqual(quarter_image.read_bytes(), fixtures.ppm_words(expected_quarter, 16, 4))
        self.assertIn("scope=quarter-grid-approximate-linear-gs", quarter.stdout)

    def test_one_pixel_size_boundary_and_scale_specific_validation(self):
        image = self.directory / "five-by-three.ppm"
        options = ("--linear-framebuffer", image, "--dispfb",
                   f"{fixtures.linear_dispfb():X}", "--linear-size", "5x3")
        result = self.replay(*options)
        self.assertEqual(result.returncode, 0, result.stderr)
        expected = [fixtures.linear_pixel(x, y) for y in range(3, 6) for x in range(5, 10)]
        self.assertEqual(image.read_bytes(), fixtures.ppm_words(expected, 5, 3))
        sentinel = image.read_bytes()
        quarter = self.replay(*options, binary=QUARTER_BINARY)
        self.assertEqual(quarter.returncode, 2)
        self.assertEqual(image.read_bytes(), sentinel)
        last = 511 | (1 << 9) | (63 << 32) | (31 << 43)
        boundary = self.replay("--linear-framebuffer", image, "--dispfb", f"{last:X}",
                               "--linear-size", "1x1")
        self.assertEqual(boundary.returncode, 0, boundary.stderr)
        self.assertEqual(image.read_bytes(), fixtures.ppm_words([0], 1, 1))
        for dispfb, size in ((last + (1 << 43), "1x1"), (last, "2x1")):
            bad = self.replay("--linear-framebuffer", image, "--dispfb", f"{dispfb:X}",
                              "--linear-size", size)
            self.assertEqual(bad.returncode, 2, bad.stdout + bad.stderr)
            self.assertEqual(image.read_bytes(), fixtures.ppm_words([0], 1, 1))

    def test_native_triangle_export_uses_native_grid_and_preserves_raw_xyz(self):
        table = self.directory / "native.tsv"
        result = self.replay("--triangles", table)
        self.assertEqual(result.returncode, 0, result.stderr)
        import csv
        rows = list(csv.DictReader((line for line in table.read_text().splitlines()
                                    if not line.startswith("#")), delimiter="\t"))
        self.assertEqual(rows[0]["v1_x"], "8")
        self.assertEqual(rows[0]["v2_y"], "8")
        self.assertEqual(rows[0]["v1_xyz"], f"{fixtures.xyz(8, 0, 8):016X}")
        self.assertTrue(table.read_text().endswith(
            "# complete triangles=2 replay_packets=5 trace_packets=5\n"))

    def test_corruption_after_prefix_still_protects_native_outputs(self):
        data = self.trace.read_bytes()
        self.trace.write_bytes(data[:-1] + bytes([data[-1] ^ 1]))
        image = self.directory / "protected.ppm"
        image.write_bytes(b"owned prior output")
        result = self.replay("--stop-packet", "1", "--linear-framebuffer", image,
                             "--dispfb", "200", "--linear-size", "1x1")
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertIn("validation failed", result.stderr)
        self.assertEqual(image.read_bytes(), b"owned prior output")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        raise SystemExit("usage: test_gif_native_replay.py NATIVE_REPLAY QUARTER_REPLAY")
    NATIVE_BINARY, QUARTER_BINARY = map(lambda value: Path(value).resolve(), sys.argv[1:])
    unittest.main(argv=[sys.argv[0]])
