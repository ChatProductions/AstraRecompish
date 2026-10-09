#!/usr/bin/env python3
"""Owned serialized GIF/ROM CLI fixtures; not a Sony/PCSX2 GS-dump oracle."""

import csv
import io
import os
import re
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


REPLAY_BINARY = BIOS_BINARY = None
HASH_SEED = 1469598103934665603
HASH_PRIME = 1099511628211
MASK64 = (1 << 64) - 1


def hash_bytes(data, value=HASH_SEED):
    for byte in data:
        value = ((value ^ byte) * HASH_PRIME) & MASK64
    return value


def serialized_trace(payloads, terminal_cycle=100, terminal_pc=0x12345678):
    output = bytearray(b"ASTRAGF1")
    checksum = HASH_SEED
    total = 0
    for index, payload in enumerate(payloads, 1):
        header = struct.pack("<QQIIQ", index, index * 10, 0x1000 + index * 4,
                             len(payload), 0)
        output += header + payload
        checksum = hash_bytes(header + payload, checksum)
        total += len(payload)
    end = struct.pack("<QQIIQ", 0, terminal_cycle, terminal_pc, 0, 1)
    output += end + struct.pack("<QQQ", len(payloads), total,
                                hash_bytes(end, checksum))
    return bytes(output)


def ad(*entries):
    return struct.pack("<QQ", (1 << 60) | 0x8000 | len(entries), 0xE) + b"".join(
        struct.pack("<QQ", value, register) for register, value in entries)


def xyz(x, y, z=0):
    return x * 16 | (y * 16 << 16) | (z << 32)


def owned_payloads():
    clear = ad((0x4C, 1 << 16), (0x18, 0), (0x40, 0x07FF000007FF0000),
               (0x47, 0), (0x46, 1), (0, 6), (1, 0xFF332211),
               (5, xyz(0, 0)), (5, xyz(8, 8)))
    upload = ad((0x50, (16 << 32) | (1 << 48)), (0x51, 0),
                (0x52, 2 | (2 << 32)), (0x53, 0))
    upload += struct.pack("<QQ4I", (2 << 58) | 0x8001, 0,
                          0xFF0000FF, 0xFF00FF00, 0xFFFF0000, 0xFFFFFFFF)
    tex0 = (16 | (1 << 14) | (1 << 26) | (1 << 30) |
            (1 << 34) | (1 << 35))
    draws = ad((0, 3), (1, 0x3F80000080332211),
               (5, xyz(0, 0, 7)), (5, xyz(8, 0, 8)), (5, xyz(0, 8, 9)),
               # A degenerate drawing kick must also be exported.
               (5, xyz(12, 12, 10)), (5, xyz(12, 12, 10)), (5, xyz(12, 12, 10)),
               # XYZ3 assembles but suppresses the draw/observer callback.
               (5, xyz(0, 0, 1)), (5, xyz(8, 0, 1)), (0xD, xyz(0, 8, 1)),
               (0x4C, 32 | (1 << 16)), (6, tex0), (0x08, 0), (0, 0x116),
               (1, 0x3F80000080808080), (3, 0), (5, xyz(0, 0)),
               (3, 32 | (32 << 16)), (5, xyz(8, 8)))
    tail = ad((0x4C, 1 << 16), (0, 0), (1, 0xFFABCDEF), (5, xyz(12, 12)))
    return [clear, upload, draws, tail]


def clean_environment():
    return {key: value for key, value in os.environ.items()
            if not key.startswith("ASTRA_")}


class GifReplayTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(prefix="astra-gif-replay-")
        cls.directory = Path(cls.temporary.name)
        cls.trace = cls.directory / "owned.astgif"
        cls.data = serialized_trace(owned_payloads())
        cls.trace.write_bytes(cls.data)
        cls.rom = cls.directory / "owned-nop-rom.bin"
        cls.rom.write_bytes(bytes(4 * 1024 * 1024))

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    def replay(self, *arguments, trace=None):
        return subprocess.run([str(REPLAY_BINARY), str(trace or self.trace),
                               *map(str, arguments)], env=clean_environment(),
                              text=True, stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE, timeout=20, check=False)

    def test_complete_replay_and_all_triangle_rows(self):
        image = self.directory / "full.ppm"
        table = self.directory / "triangles.tsv"
        result = self.replay("--framebuffer", image, "--triangles", table)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("trace_validated=1", result.stdout)
        self.assertIn("scope=fresh-instance-reset-starting-serialized-astra-gif",
                      result.stdout)
        self.assertIn("trace_packets=4", result.stdout)
        self.assertIn("replay_packets=4", result.stdout)
        self.assertIn("terminal_ee_cycle=100 terminal_next_ee_pc=12345678", result.stdout)
        self.assertIn("gif_packets=4 rejected=0 pending=0 sprites=2 points=1 lines=0 "
                      "triangles=2 exported_triangles=2", result.stdout)
        self.assertTrue(image.read_bytes().startswith(b"P6\n160 112\n255\n"))
        text = table.read_text()
        self.assertTrue(text.endswith("# complete triangles=2 replay_packets=4 trace_packets=4\n"))
        rows = list(csv.DictReader(io.StringIO("\n".join(
            line for line in text.splitlines() if not line.startswith("#"))), delimiter="\t"))
        self.assertEqual([row["sequence"] for row in rows], ["0", "1"])
        self.assertEqual([row["packet"] for row in rows], ["3", "3"])
        self.assertEqual(rows[0]["ee_cycle"], "30")
        self.assertEqual(rows[0]["next_ee_pc"], "0000100C")
        self.assertEqual(rows[0]["prim"], "0000000000000003")
        self.assertEqual(rows[0]["v0_color"], "80332211")
        self.assertEqual(rows[0]["v0_q"], "3F800000")
        self.assertEqual(rows[0]["v1_xyz"], f"{xyz(8, 0, 8):016X}")
        self.assertEqual((rows[1]["v0_x"], rows[1]["v1_x"], rows[1]["v2_x"]),
                         ("3", "3", "3"))

    def test_packet_prefix_stops_rendering_but_validates_the_whole_trace(self):
        image = self.directory / "prefix.ppm"
        result = self.replay("--stop-packet", "1", "--framebuffer", image)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("stop=packet-prefix trace_packets=4", result.stdout)
        self.assertIn("replay_packets=1", result.stdout)
        self.assertIn("gif_packets=1 rejected=0 pending=0 sprites=1", result.stdout)
        data = image.read_bytes().split(b"\n", 3)[3]
        for y in range(112):
            for x in range(160):
                expected = b"\x11\x22\x33" if x < 2 and y < 2 else b"\0\0\0"
                offset = (y * 160 + x) * 3
                self.assertEqual(data[offset:offset + 3], expected)

    def test_selected_sprite_freezes_source_and_target_then_stops_at_packet_boundary(self):
        target = self.directory / "sprite-target.ppm"
        source = self.directory / "sprite-source.ppm"
        result = self.replay("--sprite", "1", "--sprite-framebuffer", target,
                             "--sprite-source", source)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("stop=sprite-capture-submission-boundary", result.stdout)
        self.assertIn("trace_packets=4", result.stdout)
        self.assertIn("replay_packets=3", result.stdout)
        self.assertIn("points=0 lines=0 triangles=2", result.stdout)
        colors = target.read_bytes().split(b"\n", 3)[3]
        for x, y, rgb in ((0, 0, b"\xff\0\0"), (1, 0, b"\0\xff\0"),
                          (0, 1, b"\0\0\xff"), (1, 1, b"\xff\xff\xff")):
            offset = (y * 160 + x) * 3
            self.assertEqual(colors[offset:offset + 3], rgb)
        self.assertTrue(source.read_bytes().startswith(b"P6\n160 64\n255\n\xff\0\0"))

    def test_split_and_empty_submissions_preserve_pending_state_and_output(self):
        image = self.directory / "split.ppm"
        baseline_image = self.directory / "unsplit.ppm"
        baseline = self.replay("--framebuffer", baseline_image)
        payloads = owned_payloads()
        split = [payloads[0][:17], b"", payloads[0][17:], *payloads[1:]]
        trace = self.directory / "split.astgif"
        trace.write_bytes(serialized_trace(split))
        result = self.replay("--framebuffer", image, trace=trace)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("gif_packets=6 rejected=0 pending=0", result.stdout)
        self.assertEqual(image.read_bytes(), baseline_image.read_bytes())
        for field in ("framebuffer_hash", "logical_vram_word_hash"):
            expected = re.search(rf"{field}=(\w+)", baseline.stdout).group(1)
            self.assertIn(f"{field}={expected}", result.stdout)
        prefix = self.replay("--stop-packet", "1", trace=trace)
        self.assertEqual(prefix.returncode, 0, prefix.stderr)
        self.assertIn("gif_packets=1 rejected=0 pending=17 sprites=0", prefix.stdout)

    def test_corruption_after_an_early_stop_still_fails_closed(self):
        corrupted = self.directory / "bad-footer.astgif"
        corrupted.write_bytes(self.data[:-1] + bytes([self.data[-1] ^ 1]))
        for options in (("--stop-packet", "1"),
                        ("--sprite", "1", "--sprite-framebuffer", self.directory / "bad-sprite.ppm",
                         "--sprite-source", self.directory / "bad-source.ppm")):
            with self.subTest(options=options):
                image = self.directory / "bad-prefix.ppm"
                table = self.directory / "bad-triangles.tsv"
                result = self.replay(*options, "--framebuffer", image,
                                     "--triangles", table, trace=corrupted)
                self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
                self.assertIn("validation failed", result.stderr)
                self.assertNotIn("trace_validated=1", result.stdout)
                self.assertFalse(image.exists())
                self.assertNotIn("# complete", table.read_text())
        self.assertFalse((self.directory / "bad-sprite.ppm").exists())

    def test_truncation_bad_magic_trailing_bytes_and_capacity_are_rejected(self):
        bad = self.directory / "malformed.astgif"
        cases = [b"", b"GS_DUMP!", self.data[:7], self.data[:39], self.data[:-24],
                 self.data[:-1], self.data + b"\0",
                 b"ASTRAGF1" + struct.pack("<QQIIQ", 1, 0, 0, 16 * 1024 * 1024 + 1, 0)]
        for data in cases:
            with self.subTest(length=len(data)):
                bad.write_bytes(data)
                result = self.replay(trace=bad)
                self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
                self.assertNotIn("trace_validated=1", result.stdout)

    def test_strict_options_and_output_aliases_are_rejected_without_writing_input(self):
        image = self.directory / "options.ppm"
        invalid = [("--wat", "x"), ("--framebuffer",), ("--framebuffer", ""),
                   ("--framebuffer", "-"), ("--stop-packet", "0"),
                   ("--stop-packet", "100001"), ("--stop-packet", "+1"),
                   ("--stop-packet", "-1"), ("--stop-packet", " 1"),
                   ("--stop-packet", "1 "), ("--stop-packet", "0x1"),
                   ("--stop-packet", "1junk"), ("--stop-packet", "1", "--stop-packet", "2"),
                   ("--sprite", "18446744073709551616"), ("--sprite", "1"),
                   ("--sprite-source", image), ("--sprite-framebuffer", image),
                   ("--framebuffer", image, "--triangles", image),
                   ("--framebuffer", self.trace),
                   ("--framebuffer", self.directory / "nested" / ".." / self.trace.name)]
        for arguments in invalid:
            with self.subTest(arguments=arguments):
                result = self.replay(*arguments)
                self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
                self.assertIn("usage:", result.stderr)
        self.assertEqual(self.trace.read_bytes(), self.data)
        self.assertFalse(image.exists())

    def test_missing_prefix_or_sprite_and_unsupported_source_are_explicit_errors(self):
        for arguments in (("--stop-packet", "5"),
                          ("--sprite", "9", "--sprite-framebuffer", self.directory / "missing.ppm",
                           "--sprite-source", self.directory / "missing-source.ppm"),
                          ("--sprite", "0", "--sprite-framebuffer", self.directory / "clear.ppm",
                           "--sprite-source", self.directory / "clear-source.ppm")):
            with self.subTest(arguments=arguments):
                result = self.replay(*arguments)
                self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
                self.assertNotIn("trace_validated=1", result.stdout)

    def bios_trace(self, overrides=None, max_steps=8, rom=None, framebuffer=None,
                   census=None):
        environment = clean_environment()
        environment.update(overrides or {})
        arguments = [str(BIOS_BINARY), str(rom or self.rom), "0",
                     str(max_steps), "1", "0", "8"]
        if framebuffer is not None:
            arguments.extend(("0", "0", "0", str(census or "-"), str(framebuffer)))
        return subprocess.run(arguments,
                              env=environment, text=True, stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE, timeout=20, check=False)

    def test_owned_ee_dma_capture_contains_the_exact_delivered_submission(self):
        payload = owned_payloads()[0]
        packet_words = struct.unpack(f"<{len(payload) // 4}I", payload)
        program = [0x3C080001]  # t0=0x10000, an owned RAM packet destination.
        for index, value in enumerate(packet_words):
            program.extend((0x3C090000 | (value >> 16),
                            0x35290000 | (value & 0xFFFF),
                            0xAD090000 | (index * 4)))
        program.extend((0x3C081001, 0x8D09E000, 0x35290001, 0xAD09E000,
                        0x3C090001, 0xAD09A010,
                        0x34090000 | (len(payload) // 16), 0xAD09A020,
                        0x34090101, 0xAD09A000))
        rom = self.directory / "owned-path3-rom.bin"
        image = struct.pack(f"<{len(program)}I", *program)
        rom.write_bytes(image + bytes(4 * 1024 * 1024 - len(image)))
        trace = self.directory / "owned-path3.astgif"
        baseline = self.bios_trace(max_steps=256, rom=rom)
        captured = self.bios_trace({"ASTRA_GIF_TRACE": str(trace)}, max_steps=256, rom=rom)
        self.assertEqual(baseline.returncode, 1, baseline.stderr)
        self.assertEqual(captured.returncode, baseline.returncode, captured.stderr)
        stripped = "\n".join(line for line in captured.stdout.splitlines()
                             if not line.startswith("gif_trace")) + "\n"
        self.assertEqual(stripped, baseline.stdout)
        self.assertIn("gif_packets=1 rejected=0 sprites=1", captured.stdout)
        raw = trace.read_bytes()
        self.assertEqual(raw[:8], b"ASTRAGF1")
        ordinal, cycle, pc, size, flag = struct.unpack_from("<QQIIQ", raw, 8)
        self.assertEqual((ordinal, size, flag), (1, len(payload), 0))
        self.assertGreater(cycle, 0)
        self.assertGreater(pc, 0xBFC00000)
        self.assertEqual(raw[40:40 + size], payload)
        replay = self.replay(trace=trace)
        self.assertEqual(replay.returncode, 0, replay.stderr)
        self.assertIn("trace_packets=1", replay.stdout)
        self.assertIn("terminal_ee_cycle=256 terminal_next_ee_pc=BFC00400", replay.stdout)
        for field in ("framebuffer_hash", "logical_vram_word_hash"):
            expected = re.search(rf"{field}=(\w+)", captured.stdout).group(1)
            self.assertIn(f"{field}={expected}", replay.stdout)

    def test_owned_nop_rom_capture_is_observational_and_replays_terminal_hash(self):
        trace = self.directory / "nop.astgif"
        baseline = self.bios_trace()
        captured = self.bios_trace({"ASTRA_GIF_TRACE": str(trace)})
        self.assertEqual(baseline.returncode, 1, baseline.stderr)
        self.assertEqual(captured.returncode, baseline.returncode, captured.stderr)
        self.assertEqual(trace.read_bytes(), serialized_trace([], terminal_cycle=8,
                                                             terminal_pc=0xBFC00020))
        stripped = "\n".join(line for line in captured.stdout.splitlines()
                             if not line.startswith("gif_trace")) + "\n"
        self.assertEqual(stripped, baseline.stdout)
        replay = self.replay(trace=trace)
        self.assertEqual(replay.returncode, 0, replay.stderr)
        self.assertIn("trace_packets=0", replay.stdout)
        self.assertIn("terminal_ee_cycle=8 terminal_next_ee_pc=BFC00020", replay.stdout)
        for field in ("framebuffer_hash", "logical_vram_word_hash"):
            expected = re.search(rf"{field}=(\w+)", captured.stdout).group(1)
            self.assertIn(f"{field}={expected}", replay.stdout)

    def test_trace_capture_rejects_empty_dash_and_bios_alias_paths(self):
        original = self.rom.read_bytes()
        for path in ("", "-", str(self.rom),
                     str(self.directory / "nested" / ".." / self.rom.name)):
            with self.subTest(path=path):
                result = self.bios_trace({"ASTRA_GIF_TRACE": path})
                self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
                self.assertIn("ASTRA_GIF_TRACE", result.stderr)
                self.assertNotIn("steps=", result.stdout)
        self.assertEqual(self.rom.read_bytes(), original)

    def test_trace_capture_rejects_derived_step_snapshot_aliases_before_any_writes(self):
        framebuffer = self.directory / "collision-frame.ppm"
        framebuffer.write_bytes(b"owned existing final preview")
        paths = [Path(f"{framebuffer}.step-{step}.ppm{suffix}")
                 for step in (1, 8) for suffix in ("", ".linear-display.ppm")]
        for path in paths:
            path.write_bytes(b"owned existing step preview")
        original = {path: path.read_bytes() for path in [framebuffer, *paths]}
        for path in paths:
            for alias in (path, self.directory / "nested" / ".." / path.name):
                with self.subTest(path=alias):
                    result = self.bios_trace(
                        {"ASTRA_GIF_TRACE": str(alias), "ASTRA_GS_FRAME_STEPS": "1,8"},
                        framebuffer=framebuffer)
                    self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
                    self.assertIn("derived GS step snapshot paths", result.stderr)
                    self.assertNotIn("steps=", result.stdout)
                    self.assertNotIn("gif_trace path=", result.stdout)
                    self.assertEqual({item: item.read_bytes() for item in original}, original)

    def test_derived_step_snapshots_cannot_overwrite_the_bios_or_explicit_outputs(self):
        original_rom = self.rom.read_bytes()
        for index, suffix in enumerate(("", ".linear-display.ppm")):
            with self.subTest(bios_suffix=suffix):
                framebuffer = self.directory / f"protected-bios-{index}.ppm"
                rom = Path(f"{framebuffer}.step-1.ppm{suffix}")
                rom.write_bytes(original_rom)
                result = self.bios_trace({"ASTRA_GS_FRAME_STEPS": "1"},
                                         rom=rom, framebuffer=framebuffer)
                self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
                self.assertIn("ASTRA_GS_FRAME_STEPS", result.stderr)
                self.assertNotIn("steps=", result.stdout)
                self.assertEqual(rom.read_bytes(), original_rom)
                self.assertFalse(framebuffer.exists())
        framebuffer = self.directory / "protected-census.ppm"
        census = Path(f"{framebuffer}.step-1.ppm")
        census.write_bytes(b"owned existing census")
        result = self.bios_trace({"ASTRA_GS_FRAME_STEPS": "1"},
                                 framebuffer=framebuffer, census=census)
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertIn("ASTRA_GS_FRAME_STEPS", result.stderr)
        self.assertEqual(census.read_bytes(), b"owned existing census")
        self.assertFalse(framebuffer.exists())

    def test_derived_step_snapshots_reject_existing_hardlink_aliases(self):
        for index, alias_to_final in enumerate((False, True)):
            with self.subTest(alias_to_final=alias_to_final):
                framebuffer = self.directory / f"hardlink-snapshot-{index}.ppm"
                first = Path(f"{framebuffer}.step-1.ppm")
                first.write_bytes(b"owned existing snapshot")
                alias = framebuffer if alias_to_final else Path(f"{framebuffer}.step-8.ppm")
                os.link(first, alias)
                result = self.bios_trace({"ASTRA_GS_FRAME_STEPS": "1,8"},
                                         framebuffer=framebuffer)
                self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
                self.assertIn("ASTRA_GS_FRAME_STEPS", result.stderr)
                self.assertNotIn("steps=", result.stdout)
                self.assertEqual(first.read_bytes(), b"owned existing snapshot")
                self.assertEqual(alias.read_bytes(), b"owned existing snapshot")
                if not alias_to_final:
                    self.assertIn("distinct from one another", result.stderr)
                    self.assertFalse(framebuffer.exists())


if __name__ == "__main__":
    if len(sys.argv) < 3:
        raise SystemExit("usage: test_gif_replay.py PS2GIF_REPLAY_BINARY PS2BIOS_TRACE_BINARY")
    REPLAY_BINARY = Path(sys.argv.pop(1)).resolve()
    BIOS_BINARY = Path(sys.argv.pop(1)).resolve()
    unittest.main()
