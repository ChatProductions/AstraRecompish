#!/usr/bin/env python3
"""Owned synthetic parser/matching controls; no firmware or reference assets."""

import contextlib
import dataclasses
import io
import itertools
import json
from pathlib import Path
import tempfile
import unittest
from unittest import mock

from compare_cloud_vertices import (
    Reference, REQUIRED_TSV_FIELDS, STATE_FIELDS, Trace, Triangle, Vertex,
    color_summary, compare, exact_monotonic_alignment, main, parse_astra_triangles, parse_context,
    parse_reference_equal_z, parse_reference_vertices, pcsx2_precision_words,
)


def context_text():
    # Independently specified register fields, including the PCSX2 (*32)
    # display convention for FBP/ZBP and native XYOFFSET units.
    sections = {
        "PRIM": dict(PRIM=4, IIP=1, TME=1, FGE=0, ABE=1, AA1=0, FST=0, CTXT=0, FIX=0),
        "PRMODECONT": dict(AC=1),
        "XYOFFSET": dict(OFX="1728.0000", OFY="1920.5000"),
        "SCISSOR": dict(SCAX0=0, SCAX1=639, SCAY0=0, SCAY1=255),
        "TEST": dict(ATE=0, ATST=0, AREF=0, AFAIL=0, DATE=0, DATM=0, ZTE=1, ZTST=2),
        "TEX0": dict(TBP0="0x3360", TBW=1, PSM="0x2", TW=6, TH=6, TCC=1,
                     TFX=0, CBP=0, CPSM=0, CSM=0, CSA=0, CLD=0),
        "CLAMP": dict(WMS=0, WMT=0, MINU=0, MAXU=0, MINV=0, MAXV=0),
        "ALPHA": dict(A=0, B=2, C=2, D=1, FIX=20),
        "TEXA": dict(TA0=127, AEM=1, TA1=129),
        "FRAME": dict(FBP="0xa00", FBW=10, PSM=0, FBMSK="0x00ff00ff"),
        "ZBUF": dict(ZBP="0x1400", PSM="0x31", ZMSK=1),
        "TEX1": dict(LCM=0, MXL=0, MMAG=1, MMIN=1, MTBA=0, L=0, K="-0.5000"),
        "PABE": dict(PABE=0), "COLCLAMP": dict(CLAMP=1), "FBA": dict(FBA=0),
    }
    return "# owned synthetic context — not firmware\n" + "\n\n".join(
        name + ":\n" + "\n".join(f"    {key}: {value}" for key, value in fields.items())
        for name, fields in sections.items()) + "\n"


def vertex_text():
    return '''flush_reason: "SYNTHETIC"

vertex: # TRIANGLE
  - {X: -3.8750, Y: 9.5000, Z: 142042, U: -0.25, V: 32, R: 0, G: 0, B: 0, A: 128} # 0
  - {X: -49.5000, Y: 40.0000, Z: 142042, U: 32, V: 64, R: 0, G: 0, B: 0, A: 128} # 1
  - {X: 2.2500, Y: 36.7500, Z: 142042, U: 64, V: 64, R: 3, G: 5, B: 13, A: 128} # 2

vertex_stq: # TRIANGLE
  - {S: 999, T: -999, Q: 0, Si: 0x3c0aaa00, Ti: 0x3b8af400, Qi: 0x3c0af500} # 0
  - {S: 0, T: 0, Q: 0, Si: 0000000000, Ti: 0x3c0af400, Qi: 0x3c0af500} # 1
  - {S: 0, T: 0, Q: 0, Si: 0x3c0aaa00, Ti: 0x3c0af400, Qi: 0x3c0af500} # 2
vertex_trace:
  min_xyz: {X: -49.5, Y: 9.5, Z: 142042}
'''


def reference():
    state = parse_context(context_text())
    return Reference(state, parse_reference_vertices(vertex_text(), state, 1))


def triangle(ref=None, sequence=0, packet=1):
    ref = ref or reference()
    return Triangle(sequence, packet, {name: ref.state[name] for name in STATE_FIELDS},
                    tuple(dataclasses.replace(v, uv=0) for v in ref.vertices))


def tsv(records, footer=True, header=None, replay_packets=None):
    header = list(header or REQUIRED_TSV_FIELDS)
    rows = ["\t".join(header)]
    for record in records:
        values = {"sequence": str(record.sequence), "packet": str(record.packet)}
        values.update({name: f"{value:016X}" for name, value in record.state.items()})
        for index, vertex in enumerate(record.vertices):
            for name in ("x", "y", "z", "color", "st", "uv", "q", "xyz"):
                value = getattr(vertex, name)
                values[f"v{index}_{name}"] = str(value) if name in ("x", "y", "z") else (
                    f"{value:08X}" if name in ("color", "q") else f"{value:016X}")
        values.update(ee_cycle="123", next_ee_pc="00100000", extra="opaque_extension")
        rows.append("\t".join(values[name] for name in header))
    if footer:
        packets = max((r.packet for r in records), default=0) if replay_packets is None else replay_packets
        rows.append(f"# complete triangles={len(records)} replay_packets={packets} trace_packets={packets}")
    return "\n".join(rows) + "\n"


class ContextTests(unittest.TestCase):
    def test_field_encoding_and_display_block_convention(self):
        state = parse_context(context_text())
        self.assertEqual(state["prim"], 0x5C)
        self.assertEqual(state["xyoffset"], 0x780800006C00)
        self.assertEqual(state["frame"], 0x00FF00FF000A0050)
        self.assertEqual(state["zbuf"], 0x1310000A0)
        self.assertEqual(state["tex0"], 0x598207360)
        self.assertEqual(state["texa"], 0x810000807F)
        self.assertEqual(state["alpha"], 0x1400000068)
        self.assertEqual(state["tex1"], 0xFF800000060)

    def test_missing_duplicate_out_of_range_and_alignment_rejected(self):
        cases = [context_text().replace("    TA1: 129\n", ""),
                 context_text() + "TEXA:\n    TA0: 0\n",
                 context_text().replace("    TA0: 127", "    TA0: 127\n    TA0: 127"),
                 context_text().replace("    FBP: 0xa00", "    FBP: 0xa01"),
                 context_text().replace("    IIP: 1", "    IIP: 2"),
                 context_text().replace("    OFY: 1920.5000", "    OFY: 1920.51"),
                 context_text().replace("    AC: 1", "    AC: 0")]
        for text in cases:
            with self.subTest(text=text[-80:]), self.assertRaises(ValueError):
                parse_context(text)


class ReferenceParserTests(unittest.TestCase):
    def test_optional_saved_equal_z_condition_is_not_incoming_geometry(self):
        self.assertIsNone(parse_reference_equal_z(vertex_text()))
        for bit in (0, 1):
            self.assertEqual(parse_reference_equal_z(vertex_text() + f"  eq_xyz: {{X: 0, Y: 0, Z: {bit}}}\n"), bool(bit))
        for source in ("  eq_xyz: {X: 0, Y: 0, Z: 1}\n" + vertex_text(),
                       vertex_text() + "  eq_xyz: {X: 0, Y: 0, Z: 2}\n",
                       vertex_text() + "  eq_xyz: {X: 0, Y: 0, Z: 1}\n  eq_xyz: {X: 0, Y: 0, Z: 1}\n"):
            with self.subTest(source=source[-80:]), self.assertRaises(ValueError):
                parse_reference_equal_z(source)

    def test_exact_xyz_signed_host_and_authoritative_stq(self):
        vertices = reference().vertices
        self.assertEqual([v.xyz for v in vertices],
                         [0x22ADA78A06BC2, 0x22ADA7A8868E8, 0x22ADA7A546C24])
        self.assertEqual([(v.x, v.y) for v in vertices], [(0, 2), (-12, 10), (0, 9)])
        self.assertEqual(vertices[0].st, 0x3B8AF4003C0AAA00)
        self.assertEqual(vertices[1].st, 0x3C0AF40000000000)
        self.assertEqual(vertices[0].q, 0x3C0AF500)
        self.assertEqual(vertices[2].color, 0x800D0503)

    def test_truncation_missing_stq_duplicates_and_wrong_counts_rejected(self):
        source = vertex_text()
        cases = [source[:source.index("vertex_stq:")],
                 source[source.index("vertex_stq:"):],
                 source.replace("vertex_stq: # TRIANGLE", "vertex_stq: # TRIANGLE\nvertex_stq: # TRIANGLE"),
                 source.replace("} # 1", "} # 0", 1),
                 source.replace("} # 2", "} # 3", 1),
                 source.replace("Qi: 0x3c0af500} # 2", "Qi: 0x3c0af500 # 2"),
                 source.replace("Si: 0x3c0aaa00", "Si: 0x123", 1),
                 source.replace("R: 3", "R: 256"),
                 source.replace("X: -3.8750", "X: -3.87"),
                 source.replace("Q: 0, Si:", "Q: nan, Si:", 1),
                 source.replace("X: -3.8750", "X: -99999")]
        for text in cases:
            with self.subTest(text=text[-60:]), self.assertRaises(ValueError):
                parse_reference_vertices(text, reference().state, 1)
        with self.assertRaises(ValueError):
            parse_reference_vertices(source, reference().state, 2)


class TraceParserTests(unittest.TestCase):
    def test_header_names_extensions_and_bom_crlf(self):
        header = ["ee_cycle", "next_ee_pc", *reversed(REQUIRED_TSV_FIELDS), "extra"]
        result = parse_astra_triangles("\ufeff" + tsv([triangle()], header=header).replace("\n", "\r\n"))
        self.assertEqual(result.triangles[0].vertices[2].color, 0x800D0503)
        self.assertEqual((result.replay_packets, result.trace_packets), (1, 1))

    def test_missing_footer_rows_counts_and_duplicate_header_rejected(self):
        source = tsv([triangle()])
        cases = [tsv([triangle()], footer=False),
                 source.replace("triangles=1", "triangles=2"),
                 source + "# complete triangles=1 replay_packets=1 trace_packets=1\n",
                 source + source.splitlines()[1] + "\n",
                 source.replace("replay_packets=1", "replay_packets=0"),
                 source.replace("replay_packets=1", "replay_packets=2"),
                 source.replace("v0_x\t", "sequence\t", 1),
                 "\n".join(source.splitlines()[:1] + [source.splitlines()[1].rsplit("\t", 1)[0]] + source.splitlines()[2:]),
                 tsv([triangle(), triangle()]),
                 tsv([triangle(sequence=1)]),
                 tsv([triangle(packet=0)]),
                 tsv([triangle(), triangle(sequence=2)])]
        for text in cases:
            with self.subTest(text=text[-100:]), self.assertRaises(ValueError):
                parse_astra_triangles(text)

    def test_raw_word_case_width_and_xyz_provenance_rejected(self):
        source = tsv([triangle()])
        for old, new in (("800D0503", "800d0503"), ("800D0503", "800D050"),
                         ("00022ADA78A06BC2", "00022ADA78A06BC3"),
                         ("142042", "142043"), ("3C0AF500", "３C0AF500")):
            with self.subTest(old=old), self.assertRaises(ValueError):
                # Changing XYZ by one fixed unit alone might not change the
                # host anchor; choose a visible whole-anchor change instead.
                text = source.replace(old, new, 1)
                if old == "00022ADA78A06BC2":
                    text = source.replace(old, "00022ADA78A06C42", 1)
                parse_astra_triangles(text)

    def test_empty_completed_export_is_valid(self):
        self.assertEqual(parse_astra_triangles(tsv([])).triangles, ())


class MatchingTests(unittest.TestCase):
    def test_full_exact_match_and_color_digest(self):
        ref = reference()
        report = compare(ref, parse_astra_triangles(tsv([triangle(ref)])))
        self.assertEqual(report["match_quality"], "exact_raw_xyz_anchor")
        self.assertTrue(report["all_observed_fields_equal"])
        self.assertEqual(report["reference_colors"], report["actual_colors"])
        self.assertEqual(report["reference_colors"]["black_rgb_vertices"], 2)
        self.assertEqual(report["reference_colors"]["max_rgb"], [3, 5, 13])

    def test_full_rgb_stq_and_state_differences_not_just_anchor(self):
        ref = reference()
        record = triangle(ref)
        vertices = list(record.vertices)
        vertices[1] = dataclasses.replace(vertices[1], color=0x804C1E13, st=123, q=456)
        state = dict(record.state, alpha=0)
        report = compare(ref, Trace((dataclasses.replace(record, state=state, vertices=tuple(vertices)),), 1, 1))
        self.assertEqual(report["vertex_differences"], {"color": 1, "st": 1, "q": 1, "rgb": 1})
        self.assertEqual(report["state_differences"], {"alpha": 1})
        self.assertEqual(report["vertex_examples"][0]["vertex_index"], 1)
        self.assertFalse(report["all_observed_fields_equal"])

    def test_no_exact_fallback_is_opt_in_and_never_equivalence(self):
        ref = reference()
        record = triangle(ref)
        changed = dataclasses.replace(record.vertices[0], xyz=record.vertices[0].xyz + 1)
        record = dataclasses.replace(record, vertices=(changed, *record.vertices[1:]))
        trace = Trace((record,), 1, 1)
        self.assertEqual(compare(ref, trace)["match_quality"], "no_exact_location")
        report = compare(ref, trace, allow_z_fallback=True)
        self.assertEqual(report["match_quality"], "z_only_not_an_exact_location")
        self.assertEqual(report["geometry_alignment"]["unambiguous_matched_triangles"], 0)
        self.assertEqual(report["vertex_differences"], {})
        self.assertEqual(report["unpaired_reference_vertices"], 3)
        self.assertFalse(report["all_observed_fields_equal"])

    def test_inserted_geometry_is_skipped_not_paired_by_array_position(self):
        ref = reference()
        def shifted(delta):
            return tuple(dataclasses.replace(v, xyz=v.xyz + delta, x=v.x + delta // 64) for v in ref.vertices)
        expected = Reference(ref.state, ref.vertices + shifted(64) + shifted(128))
        base = triangle(ref)
        actual = [base,
                  dataclasses.replace(base, sequence=1, vertices=shifted(256)),
                  dataclasses.replace(base, sequence=2, vertices=shifted(64)),
                  dataclasses.replace(base, sequence=3, vertices=shifted(128)),
                  dataclasses.replace(base, sequence=4, vertices=shifted(320))]
        report = compare(expected, Trace(tuple(actual), 1, 1))
        aligned = report["geometry_alignment"]
        self.assertEqual(aligned["lcs_triangles"], 3)
        self.assertEqual(aligned["unambiguous_matched_triangles"], 3)
        self.assertEqual(aligned["candidate_without_exact_match_through_last_possible_match"], 1)
        self.assertEqual(aligned["candidate_without_exact_match_ranges"], [[1, 1]])
        self.assertEqual(aligned["candidate_tail_after_last_possible_match"], 1)
        self.assertEqual(report["vertex_differences"], {})
        self.assertTrue(report["all_paired_fields_equal"])
        self.assertFalse(report["all_observed_fields_equal"])
        changed = list(actual[2].vertices)
        changed[0] = dataclasses.replace(changed[0], color=0xFFFFFFFF, st=123)
        actual[2] = dataclasses.replace(actual[2], vertices=tuple(changed))
        report = compare(expected, Trace(tuple(actual), 1, 1))
        self.assertEqual(report["vertex_differences"], {"color": 1, "st": 1, "rgb": 1})
        self.assertEqual(report["vertex_examples"][0]["vertex_index"], 3)
        self.assertEqual(report["vertex_examples"][0]["triangle_sequence"], 2)

    def test_duplicate_geometry_never_arbitrarily_pairs_color_or_stq(self):
        ref = reference()
        second_vertices = tuple(dataclasses.replace(v, xyz=v.xyz + 64, x=v.x + 1) for v in ref.vertices)
        expected = Reference(ref.state, ref.vertices + second_vertices)
        base = triangle(ref)
        second = dataclasses.replace(base, sequence=1, vertices=second_vertices)
        duplicate = dataclasses.replace(second, sequence=2,
            vertices=tuple(dataclasses.replace(v, color=0xFFFFFFFF, st=999) for v in second_vertices))
        report = compare(expected, Trace((base, second, duplicate), 1, 1))
        aligned = report["geometry_alignment"]
        self.assertEqual(aligned["lcs_triangles"], 2)
        self.assertEqual(aligned["unambiguous_matched_triangles"], 1)
        self.assertEqual(aligned["ambiguous_lcs_ranks"], (2,))
        self.assertEqual(aligned["ambiguous_candidate_pairs"], 2)
        self.assertEqual(aligned["reference_with_only_ambiguous_alignment_ranges"], [[1, 1]])
        self.assertEqual(report["vertex_differences"], {})
        self.assertEqual(report["actual_colors"]["vertices"], 3)
        self.assertFalse(report["all_paired_fields_equal"])

    def test_one_fixed_unit_geometry_difference_is_not_silently_exact(self):
        ref = reference()
        second_vertices = tuple(dataclasses.replace(v, xyz=v.xyz + 64, x=v.x + 1) for v in ref.vertices)
        expected = Reference(ref.state, ref.vertices + second_vertices)
        base = triangle(ref)
        near = list(second_vertices)
        near[2] = dataclasses.replace(near[2], xyz=near[2].xyz + 1)
        report = compare(expected, Trace((base, dataclasses.replace(base, sequence=1, vertices=tuple(near))), 1, 1))
        aligned = report["geometry_alignment"]
        self.assertEqual(aligned["lcs_triangles"], 1)
        self.assertEqual(aligned["reference_without_exact_candidate_ranges"], [[1, 1]])
        self.assertEqual(aligned["reference_unaligned_triangles"], 1)
        self.assertEqual(report["actual_colors"]["vertices"], 3)
        self.assertEqual(report["unpaired_reference_vertices"], 3)

    def test_ambiguous_exact_requires_explicit_candidate_selection(self):
        ref = reference()
        trace = Trace((triangle(ref), triangle(ref, sequence=1)), 1, 1)
        self.assertEqual(compare(ref, trace)["match_quality"], "ambiguous_exact_location")
        report = compare(ref, trace, candidate_index=1)
        self.assertEqual(report["first_sequence"], 1)
        self.assertEqual(report["actual_triangles"], 1)
        with self.assertRaises(ValueError):
            compare(ref, trace, candidate_index=2)

    def test_extra_same_z_vertices_cannot_claim_equal_and_examples_are_bounded(self):
        ref = reference()
        first = triangle(ref)
        second = dataclasses.replace(triangle(ref, sequence=1),
            vertices=tuple(dataclasses.replace(v, xyz=v.xyz + 1, color=0xFFFFFFFF) for v in first.vertices))
        report = compare(ref, Trace((first, second), 1, 1), max_examples=0)
        self.assertIsNone(report["count_matches"])
        self.assertEqual(report["unpaired_actual_vertices"], 0)
        self.assertEqual(report["uncertain_same_z_tail_triangles"], 1)
        self.assertTrue(report["all_paired_fields_equal"])
        self.assertFalse(report["all_observed_fields_equal"])

    def test_unicode_paths_and_cli_wrong_count(self):
        with tempfile.TemporaryDirectory(prefix="cloud-星-") as directory:
            base = Path(directory)
            paths = [base / name for name in ("状態.txt", "頂点.txt", "三角.tsv")]
            for path, text in zip(paths, (context_text(), vertex_text(), tsv([triangle()]))):
                path.write_text(text, encoding="utf-8")
            args = ["--context", str(paths[0]), "--vertices", str(paths[1]),
                    "--triangles", str(paths[2]), "--expected-triangles", "1", "--require-equal"]
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                self.assertEqual(main(args), 0)
            self.assertTrue(json.loads(output.getvalue())["all_observed_fields_equal"])
            with contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(main(args[:-3]), 2)  # Default requires 244, not one.


class AlignmentAlgorithmTests(unittest.TestCase):
    def test_candidate_pair_capacity_is_bounded_and_fail_closed(self):
        with mock.patch("compare_cloud_vertices.MAX_ALIGNMENT_PAIRS", 3), self.assertRaises(ValueError):
            exact_monotonic_alignment((1, 1), (1, 1))

    def test_crossing_candidates_report_ambiguity_not_false_correspondence(self):
        aligned = exact_monotonic_alignment((1, 2), (2, 1))
        self.assertEqual(aligned["reference_with_exact_candidates"], 2)
        self.assertEqual(aligned["lcs_triangles"], 1)
        self.assertEqual(aligned["pairs"], ())
        self.assertEqual(aligned["ambiguous_lcs_ranks"], (1,))
        pinned = exact_monotonic_alignment((1, 2, 1), (1, 1, 2), True)
        self.assertEqual(pinned["pairs"], ((0, 0),))
        self.assertEqual(pinned["lcs_triangles"], 2)
        with self.assertRaises(ValueError):
            exact_monotonic_alignment((1,), (2,), True)

    def test_sparse_lcs_mandatory_pairs_against_exhaustive_owned_sequences(self):
        sequences = [values for length in range(4) for values in itertools.product((0, 1), repeat=length)]
        def all_alignments(left, right, i=0, j=0):
            # Independent tiny exhaustive oracle: every strictly increasing
            # matching-index sequence, including the empty alignment.
            result = {()}
            for a in range(i, len(left)):
                for b in range(j, len(right)):
                    if left[a] == right[b]:
                        result.update(((a, b),) + tail for tail in all_alignments(left, right, a + 1, b + 1))
            return result
        for left, right in itertools.product(sequences, repeat=2):
            with self.subTest(left=left, right=right):
                candidates = all_alignments(left, right)
                longest = max(map(len, candidates))
                best = [set(pairs) for pairs in candidates if len(pairs) == longest]
                mandatory = set.intersection(*best)
                possible = set.union(*best)
                got = exact_monotonic_alignment(left, right)
                self.assertEqual(got["lcs_triangles"], longest)
                self.assertEqual(set(got["pairs"]), mandatory)
                self.assertEqual(set(got["possible_pairs"]), possible)


class GsPrecisionTests(unittest.TestCase):
    def test_independent_low_bit_exponent_and_large_gap_controls(self):
        # Expected literals independently hand-calculated: same exponent
        # clears nine ST mantissa bits; one smaller exponent clears ten.
        self.assertEqual(pcsx2_precision_words(0x3F8003FF, 0x3F0007FF, 0x3F8001AB),
                         (0x3F800200, 0x3F000400, 0x3F800100))
        # Larger ST exponent still clears exactly nine, not fewer bits.
        self.assertEqual(pcsx2_precision_words(0x400003FF, 0xBF8003FF, 0x3F0001AB),
                         (0x40000200, 0xBF800200, 0x3F000100))
        # Very large exponent differences clamp at all 23 mantissa bits,
        # preserving signs and exponents rather than shifting by >=32.
        self.assertEqual(pcsx2_precision_words(0x008001FF, 0x800000FF, 0x7F0001AB),
                         (0x00800000, 0x80000000, 0x7F000100))
        self.assertEqual(pcsx2_precision_words(0, 0x80000000, 0x3F800001),
                         (0, 0x80000000, 0x3F800000))
        self.assertEqual(pcsx2_precision_words(0x007FFFFF, 0x807FFFFF, 0x000001FF),
                         (0x007FFE00, 0x807FFE00, 0x00000100))

    def test_nonfinite_zero_q_and_out_of_range_are_unmodeled(self):
        for words in ((0x7F800000, 0, 0x3F800000), (0, 0xFF800000, 0x3F800000),
                      (0, 0, 0x7FC00001), (0, 0, 0), (0, 0, 0x80000000),
                      (-1, 0, 0x3F800000), (0, 1 << 32, 0x3F800000)):
            with self.subTest(words=words), self.assertRaises(ValueError):
                pcsx2_precision_words(*words)

    def test_explicit_derived_view_keeps_raw_color_and_equality_unchanged(self):
        ref = dataclasses.replace(reference(), trace_equal_z=True)
        base = triangle(ref)
        vertices = tuple(dataclasses.replace(v,
            st=(v.st & 0xFFFFFFFF | 0x7F) | ((v.st >> 32 | 0x7F) << 32), q=v.q | 0x7F)
            for v in base.vertices)
        incoming = dataclasses.replace(base, vertices=vertices)
        trace = Trace((incoming,), 1, 1)
        default = compare(ref, trace)
        report = compare(ref, trace, pcsx2_gs_precision=True)
        view = report["pcsx2_gs_precision"]
        self.assertEqual(default["pcsx2_gs_precision"]["status"], "not_requested")
        self.assertEqual(view["status"], "derived_comparison")
        self.assertEqual(view["gate_basis"], "saved_eq_xyz.Z")
        self.assertEqual(view["raw_word_differences"], {"S": 3, "T": 3, "Q": 3})
        self.assertEqual(view["derived_word_differences"], {"S": 0, "T": 0, "Q": 0})
        self.assertTrue(view["derived_all_compared_words_equal"])
        self.assertEqual(report["vertex_differences"], {"st": 3, "q": 3})
        self.assertFalse(report["all_observed_fields_equal"])
        self.assertEqual(report["actual_colors"], default["actual_colors"])
        self.assertEqual(report["reference_aligned_colors"], report["actual_colors"])
        self.assertEqual(trace.triangles[0].vertices, vertices)  # No normalization mutation.
        changed = list(vertices)
        changed[0] = dataclasses.replace(changed[0], color=0xFFFFFFFF)
        altered = compare(ref, Trace((dataclasses.replace(incoming, vertices=tuple(changed)),), 1, 1),
                          pcsx2_gs_precision=True)
        self.assertEqual(altered["vertex_differences"]["rgb"], 1)
        self.assertTrue(altered["pcsx2_gs_precision"]["derived_all_compared_words_equal"])

    def test_gates_and_unsupported_inputs_are_explicit(self):
        ref = reference()
        for prim in (ref.state["prim"] & ~0x10, ref.state["prim"] | 0x100,
                     ref.state["prim"] & ~7 | 6):
            state = dict(ref.state, prim=prim)
            control = Reference(state, ref.vertices, True)
            report = compare(control, Trace((triangle(control),), 1, 1), pcsx2_gs_precision=True)
            self.assertEqual(report["pcsx2_gs_precision"]["status"], "not_applicable_reference_gate")
        false_z = dataclasses.replace(ref, trace_equal_z=False)
        report = compare(false_z, Trace((triangle(ref),), 1, 1), pcsx2_gs_precision=True)
        self.assertEqual(report["pcsx2_gs_precision"]["status"], "not_applicable_reference_gate")
        inferred = compare(ref, Trace((triangle(ref),), 1, 1), pcsx2_gs_precision=True)
        self.assertEqual(inferred["pcsx2_gs_precision"]["gate_basis"], "inferred_from_constant_reference_raw_Z")
        varying = list(ref.vertices)
        varying[2] = dataclasses.replace(varying[2], z=varying[2].z + 1,
            xyz=(varying[2].xyz & 0xFFFFFFFF) | ((varying[2].z + 1) << 32))
        varying_ref = Reference(ref.state, tuple(varying))
        report = compare(varying_ref, Trace((triangle(varying_ref),), 1, 1), pcsx2_gs_precision=True)
        self.assertEqual(report["pcsx2_gs_precision"]["status"], "not_applicable_reference_gate")
        bad = list(triangle(ref).vertices)
        bad[0] = dataclasses.replace(bad[0], q=0)
        bad[1] = dataclasses.replace(bad[1], q=0x7FC00001)
        report = compare(ref, Trace((dataclasses.replace(triangle(ref), vertices=tuple(bad)),), 1, 1),
                         pcsx2_gs_precision=True)
        self.assertEqual(report["pcsx2_gs_precision"]["normalized_vertices"], 1)
        self.assertEqual(report["pcsx2_gs_precision"]["unmodeled_vertices"], 2)
        self.assertFalse(report["pcsx2_gs_precision"]["derived_all_compared_words_equal"])

    def test_cli_precision_option_never_silently_changes_require_equal(self):
        ref = reference()
        base = triangle(ref)
        incoming = dataclasses.replace(base, vertices=tuple(dataclasses.replace(v,
            st=(v.st & 0xFFFFFFFF | 0x7F) | ((v.st >> 32 | 0x7F) << 32), q=v.q | 0x7F)
            for v in base.vertices))
        with tempfile.TemporaryDirectory(prefix="cloud-precision-") as directory:
            paths = [Path(directory) / name for name in ("context.txt", "vertices.txt", "triangles.tsv")]
            for path, source in zip(paths, (context_text(), vertex_text(), tsv([incoming]))):
                path.write_text(source, encoding="utf-8")
            output = io.StringIO()
            args = ["--context", str(paths[0]), "--vertices", str(paths[1]), "--triangles", str(paths[2]),
                "--expected-triangles", "1", "--pcsx2-gs-precision", "--require-equal"]
            with contextlib.redirect_stdout(output):
                self.assertEqual(main(args), 1)
            report = json.loads(output.getvalue())
            self.assertTrue(report["pcsx2_gs_precision"]["derived_all_compared_words_equal"])
            self.assertFalse(report["all_observed_fields_equal"])


if __name__ == "__main__":
    unittest.main()
