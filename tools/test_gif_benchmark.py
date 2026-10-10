#!/usr/bin/env python3
"""Owned host-submit benchmark contracts; no firmware, timing threshold or FPS oracle."""

import re
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

import test_gif_replay as fixtures


BENCHMARK = REPLAY = None
SCALE = None
COLOR = 0x80402010


def triangle_payload():
    return fixtures.ad((0x4C, 1 << 16), (0x18, 0),
                       (0x40, (31 << 16) | (31 << 48)), (0x47, 0),
                       (0, 3), (1, COLOR), (5, fixtures.xyz(0, 0, 17)),
                       (5, fixtures.xyz(0, 8, 17)), (5, fixtures.xyz(8, 0, 17)))


def field(output, name):
    match = re.search(rf"\b{re.escape(name)}=([^\s]+)", output)
    if not match:
        raise AssertionError(f"missing {name} in {output!r}")
    return match.group(1)


def independent_hashes():
    words = {}
    for y in range(0, 8, SCALE):
        for x in range(0, 8, SCALE):
            if x + y >= 8:
                continue
            for dy in range(SCALE):
                for dx in range(SCALE):
                    words[(y + dy) * 64 + x + dx] = COLOR
    preview = fixtures.hash_words(words.get((y * SCALE) * 64 + x * SCALE, 0)
                                  if x * SCALE < 64 else 0
                                  for y in range(448 // SCALE)
                                  for x in range(640 // SCALE))
    vram = fixtures.hash_words(words.get(index, 0) for index in range(1024 * 1024))
    return preview, vram


class GifBenchmarkTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="astra-gif-benchmark-")
        self.directory = Path(self.temporary.name)
        self.trace = self.directory / "owned.astgif"
        self.data = fixtures.serialized_trace([triangle_payload()])
        self.trace.write_bytes(self.data)

    def tearDown(self):
        self.temporary.cleanup()

    def benchmark(self, samples="1", trace=None, *extra):
        return subprocess.run([str(BENCHMARK), str(trace or self.trace), str(samples),
                               *map(str, extra)], env=fixtures.clean_environment(),
                              text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                              timeout=20, check=False)

    def replay(self, trace=None):
        return subprocess.run([str(REPLAY), str(trace or self.trace)],
                              env=fixtures.clean_environment(), text=True,
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                              timeout=20, check=False)

    def assert_success(self, result, samples, records=1):
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(field(result.stdout, "validated"), "1")
        self.assertEqual(field(result.stdout, "matched"), "1")
        self.assertEqual(field(result.stdout, "sample_scale"), str(SCALE))
        self.assertEqual(field(result.stdout, "warmups"), "1")
        self.assertEqual(field(result.stdout, "samples"), str(samples))
        self.assertEqual(field(result.stdout, "records"), str(records))
        self.assertEqual(field(result.stdout, "gif_packets"), str(records))
        self.assertEqual(field(result.stdout, "rejected"), "0")
        self.assertEqual(field(result.stdout, "pending"), "0")
        self.assertIn("scope=host-gif-submit-throughput-not-guest-core-frame-vita-fps", result.stdout)
        self.assertNotRegex(result.stdout, r"\bfps=")
        timings = re.findall(r"^gif_benchmark_sample index=(\d+) submit_ns=(\d+)$",
                             result.stdout, re.MULTILINE)
        self.assertEqual([int(index) for index, _ in timings], list(range(1, samples + 1)))
        ordered = sorted(int(value) for _, value in timings)
        median = ordered[len(ordered) // 2] if len(ordered) % 2 else \
            (ordered[len(ordered) // 2 - 1] + ordered[len(ordered) // 2]) // 2
        self.assertEqual(int(field(result.stdout, "median_ns")), median)
        self.assertEqual(int(field(result.stdout, "min_ns")), ordered[0])
        self.assertEqual(int(field(result.stdout, "max_ns")), ordered[-1])

    def test_known_triangle_hashes_and_one_or_three_samples(self):
        expected_preview, expected_vram = independent_hashes()
        baseline = self.replay()
        self.assertEqual(baseline.returncode, 0, baseline.stderr)
        scope = {1: "native-pixel-grid", 2: "half-grid", 4: "quarter-grid"}[SCALE]
        self.assertIn(f"gs_raster_grid width={640 // SCALE} height={448 // SCALE} "
                      f"sample_scale={SCALE} scope={scope}-approximate-linear-gs",
                      baseline.stdout)
        for samples in (1, 3):
            with self.subTest(samples=samples):
                result = self.benchmark(samples)
                self.assert_success(result, samples)
                self.assertEqual(field(result.stdout, "payload_bytes"), str(len(triangle_payload())))
                self.assertEqual(field(result.stdout, "triangles"), "1")
                self.assertEqual(field(result.stdout, "FRAME"), "0000000000010000")
                self.assertEqual(field(result.stdout, "framebuffer_hash"), expected_preview)
                self.assertEqual(field(result.stdout, "logical_vram_word_hash"), expected_vram)
                for name in ("FRAME", "framebuffer_hash", "logical_vram_word_hash", "triangles"):
                    self.assertEqual(field(result.stdout, name), field(baseline.stdout, name))

    def test_fragment_boundaries_and_empty_submission_are_preserved(self):
        payload = triangle_payload()
        self.trace.write_bytes(fixtures.serialized_trace([payload[:1], payload[1:17], b"", payload[17:]]))
        result = self.benchmark(3)
        self.assert_success(result, 3, records=4)
        self.assertEqual(field(result.stdout, "payload_bytes"), str(len(payload)))
        self.assertEqual(field(result.stdout, "triangles"), "1")
        expected_preview, expected_vram = independent_hashes()
        self.assertEqual(field(result.stdout, "framebuffer_hash"), expected_preview)
        self.assertEqual(field(result.stdout, "logical_vram_word_hash"), expected_vram)

    def test_zero_record_control_has_zero_rates_and_valid_hashes(self):
        self.trace.write_bytes(fixtures.serialized_trace([]))
        result = self.benchmark(3)
        self.assert_success(result, 3, records=0)
        self.assertEqual(field(result.stdout, "empty_workload"), "1")
        self.assertEqual(field(result.stdout, "payload_bytes"), "0")
        self.assertEqual(field(result.stdout, "triangles"), "0")
        self.assertEqual(field(result.stdout, "payload_bytes_per_second"), "0.000")
        self.assertEqual(field(result.stdout, "submissions_per_second"), "0.000")
        baseline = self.replay()
        self.assertEqual(baseline.returncode, 0, baseline.stderr)
        for name in ("FRAME", "framebuffer_hash", "logical_vram_word_hash"):
            self.assertEqual(field(result.stdout, name), field(baseline.stdout, name))

    def test_fresh_raster_depth_for_every_trial(self):
        # Fresh depth is UINT_MAX: GEQUAL Z=5 rejects. An ALWAYS depth-only
        # draw then stores Z=1. Reusing only Gif.reset would allow the earlier
        # draw next time and leave visible color behind under the later mask.
        geometry = ((5, fixtures.xyz(0, 0, 5)), (5, fixtures.xyz(0, 8, 5)),
                    (5, fixtures.xyz(8, 0, 5)))
        payload = fixtures.ad((0x4C, 1 << 16), (0x18, 0),
                              (0x40, (31 << 16) | (31 << 48)), (0x4E, 0),
                              (0x47, 0x50000), (0, 3), (1, COLOR), *geometry,
                              (0x4C, (0xFFFFFFFF << 32) | (1 << 16)),
                              (0x47, 0x30000),
                              (5, fixtures.xyz(0, 0, 1)), (5, fixtures.xyz(0, 8, 1)),
                              (5, fixtures.xyz(8, 0, 1)))
        self.trace.write_bytes(fixtures.serialized_trace([payload]))
        result = self.benchmark(3)
        self.assert_success(result, 3)
        self.assertEqual(field(result.stdout, "triangles"), "2")
        self.assertEqual(field(result.stdout, "FRAME"), "FFFFFFFF00010000")
        self.assertEqual(field(result.stdout, "framebuffer_hash"),
                         fixtures.hash_words(0 for _ in range((640 // SCALE) * (448 // SCALE))))
        self.assertEqual(field(result.stdout, "logical_vram_word_hash"),
                         fixtures.hash_words(0 for _ in range(1024 * 1024)))

    def test_sequential_texture_settings_do_not_leak_between_draws_or_contexts(self):
        # Every sprite has constant UV and an aligned4x4 rectangle, so both
        # raster scales write the same16 logical words. Values below are hand
        # decoded/filtered colors, not hashes learned from the replay tool.
        tex32 = (0x20002040, 0xA00060C0, 0x4000A080, 0xE000E000)
        tex16 = (0x0000, 0x801F, 0x03E0, 0xFC00)
        block32, block16 = 0x2000, 0x2100

        def upload(block, psm, raw):
            header = fixtures.ad((0x50, (block << 32) | (1 << 48) | (psm << 56)),
                                 (0x51, 0), (0x52, 2 | (2 << 32)), (0x53, 0))
            return header + struct.pack("<QQ", (2 << 58) | 0x8001, 0) + raw.ljust(16, b"\0")

        uploads = upload(block32, 0, struct.pack("<4I", *tex32)) + \
            upload(block16, 2, struct.pack("<4H", *tex16))
        texa_aem = 32 | (1 << 15) | (224 << 32)
        texa_other = 96 | (160 << 32)
        region_clamp = 2 | (2 << 2) | (1 << 24) | (1 << 34)
        region_repeat = 3 | (3 << 2) | (1 << 14) | (1 << 34)
        # context,PSM,TEX1,CLAMP,TEXA,U10.4,V10.4,expected RGBA
        draws = [
            (0, 0, 0x00, 0, texa_aem, 8, 8, tex32[0]),
            (0, 0, 0x20, 0, texa_aem, 16, 16, 0x78008060),
            (0, 0, 0x40, 5, texa_other, 36, 8, tex32[1]),
            (0, 1, 0x60, 0, 0x66, 16, 16, 0x66008060),
            (1, 2, 0x00, 0, texa_aem, 8, 8, 0x00000000),
            (1, 2, 0x60, 0, texa_aem, 16, 16, 0x783E3E3E),
            (1, 2, 0x20, 0, texa_other, 16, 16, 0x803E3E3E),
            (1, 2, 0x00, 0, texa_other, 8, 8, 0x60000000),
            (1, 0, 0x00, region_clamp, 0, 36, 8, tex32[2]),
            (0, 0, 0x00, region_repeat, 0, 36, 8, tex32[3]),
            (0, 1, 0x00, 0, 0x8044, 8, 8, 0x44002040),
            (1, 0, 0x20, 0, texa_other, 16, 16, 0x78008060),
        ]
        # PSM32 at(1,1) filters RGBA to96/128/0/120. PSM16 expanded taps
        # are black/red248/green248/blue248: the half blends give62/62/62.
        # AEM alphas0/224/32/224 filter to120, while changed TEXA without AEM
        # gives96/160/96/160 and128. PSM24 replaces stored alpha before TFX.
        payloads = [uploads]
        for index, (context, psm, tex1, clamp, texa, u, v, _) in enumerate(draws):
            block = block16 if psm == 2 else block32
            tex0 = (block | (1 << 14) | (psm << 20) | (1 << 26) | (1 << 30) |
                    (1 << 34) | (1 << 35))
            uv = u | (v << 16)
            payloads.append(fixtures.ad(
                (0x4C + context, 1 << 16), (0x18 + context, 0),
                (0x40 + context, (63 << 16) | (3 << 48)), (0x47 + context, 0),
                (0x4E + context, 1 << 32), (0x06 + context, tex0),
                (0x14 + context, tex1), (0x08 + context, clamp), (0x3B, texa),
                (0, 0x116 | (context << 9)), (1, 0x3F80000080808080),
                (3, uv), (5, fixtures.xyz(index * 4, 0)),
                (3, uv), (5, fixtures.xyz((index + 1) * 4, 4))))
        self.trace.write_bytes(fixtures.serialized_trace(payloads, terminal_cycle=1000))
        words = {}
        for index, item in enumerate(draws):
            for y in range(4):
                for x in range(index * 4, (index + 1) * 4):
                    words[y * 64 + x] = item[-1]
        for index, value in enumerate(tex32):
            words[block32 * 256 // 4 + (index // 2) * 64 + index % 2] = value
        words[block16 * 256 // 4] = tex16[0] | (tex16[1] << 16)
        words[block16 * 256 // 4 + 32] = tex16[2] | (tex16[3] << 16)
        expected_preview = fixtures.hash_words(
            words.get((y * SCALE) * 64 + x * SCALE, 0) if x * SCALE < 64 else 0
            for y in range(448 // SCALE) for x in range(640 // SCALE))
        expected_vram = fixtures.hash_words(words.get(index, 0) for index in range(1024 * 1024))
        result = self.benchmark(3)
        self.assert_success(result, 3, records=len(payloads))
        self.assertEqual(field(result.stdout, "payload_bytes"), str(sum(map(len, payloads))))
        self.assertEqual(field(result.stdout, "sprites"), str(len(draws)))
        self.assertEqual(field(result.stdout, "triangles"), "0")
        self.assertEqual(field(result.stdout, "image_tags"), "2")
        self.assertEqual(field(result.stdout, "image_bytes"), "32")
        self.assertEqual(field(result.stdout, "local_bytes_written"), "24")
        self.assertEqual(field(result.stdout, "framebuffer_hash"), expected_preview)
        self.assertEqual(field(result.stdout, "logical_vram_word_hash"), expected_vram)
        image = self.directory / "owned-state-transitions.ppm"
        replay = subprocess.run([str(REPLAY), str(self.trace), "--linear-framebuffer", str(image),
                                 "--dispfb", "200", "--linear-size", "64x4"],
                                env=fixtures.clean_environment(), text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                timeout=20, check=False)
        self.assertEqual(replay.returncode, 0, replay.stdout + replay.stderr)
        sampled = [words.get((y * SCALE) * 64 + x * SCALE, 0)
                   for y in range(4 // SCALE) for x in range(64 // SCALE)]
        self.assertEqual(image.read_bytes(), fixtures.ppm_words(sampled, 64 // SCALE, 4 // SCALE))

    def test_strict_sample_count_and_argument_shape(self):
        for value in ("", "0", "101", "-1", "+1", "1x", "1.0", " 1", "1 ",
                      "999999999999999999999999999999999999999999999999999999"):
            with self.subTest(value=value):
                result = self.benchmark(value)
                self.assertEqual(result.returncode, 2)
                self.assertNotIn("gif_benchmark_sample", result.stdout)
        self.assertEqual(self.benchmark(1, None, "extra").returncode, 2)
        self.assertEqual(self.benchmark(1, self.directory / "missing.astgif").returncode, 2)

    def test_corrupt_truncated_trailing_or_oversized_trace_never_starts_timing(self):
        variants = [self.data[:offset] for offset in (0, 7, 8, 39, 40, len(self.data) - 1)]
        variants.extend((self.data + b"trailing", self.data[:-1] + bytes([self.data[-1] ^ 1])))
        corrupt = bytearray(self.data)
        corrupt[40] ^= 1
        variants.append(bytes(corrupt))
        oversized = bytearray(self.data)
        struct.pack_into("<I", oversized, 28, 16 * 1024 * 1024 + 1)
        variants.append(bytes(oversized))
        for index, data in enumerate(variants):
            with self.subTest(index=index):
                self.trace.write_bytes(data)
                result = self.benchmark(3)
                self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
                self.assertNotIn("gif_benchmark_sample", result.stdout)
                self.assertNotIn("matched=1", result.stdout)

    def test_valid_container_with_pending_gif_fails_before_any_sample(self):
        self.trace.write_bytes(fixtures.serialized_trace([b"\0"]))
        result = self.benchmark(3)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("pending=1", result.stderr)
        self.assertNotIn("gif_benchmark_sample", result.stdout)
        self.assertNotIn("matched=1", result.stdout)


if __name__ == "__main__":
    if len(sys.argv) != 4 or sys.argv[3] not in ("1", "2", "4"):
        raise SystemExit("usage: test_gif_benchmark.py BENCHMARK REPLAY SAMPLE_SCALE")
    BENCHMARK, REPLAY = map(Path, sys.argv[1:3])
    SCALE = int(sys.argv[3])
    unittest.main(argv=[sys.argv[0]])
