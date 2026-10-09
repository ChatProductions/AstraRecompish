#!/usr/bin/env python3
"""Compare a PCSX2 triangle draw dump with Astra's complete triangle TSV.

Example (draw 81 has 244 triangles):
  python tools/compare_cloud_vertices.py --context 00081_context.txt \
      --vertices 00081_vertex.txt --triangles astra-triangles.tsv

PCSX2 X/Y are native, XYOFFSET-relative coordinates. They are reconstructed
with Decimal into guest 12.4 XYZ; printed Si/Ti/Qi, not rounded S/T/Q, supply
the stored floating-point words. PCSX2's Si/Ti/Qi are POST-FLUSH GSVertex
fields, not original GIF input: the constant-Z textured STQ path masks
mantissa bits before dumping. --pcsx2-gs-precision reports an explicitly
derived precision-adjusted view alongside, NEVER in place of, raw words.
A Z-only fallback is opt-in and NEVER establishes
equivalence. No images, firmware, emulator execution, or third-party code is
used. Register bit positions were checked against primary v2.8.2 definitions:
https://github.com/PCSX2/pcsx2/blob/v2.8.2/pcsx2/GS/GSRegs.h

After locating the first exact ordered XYZ triple, comparisons use only
ordered triples present in every longest monotonic exact alignment. Extra
triangles and same-Z tails are not assumed to be culling or draw boundaries.
Ambiguous geometry never supplies arbitrarily paired color/STQ comparisons.

Exit 0 means a comparison was produced (differences may exist), 1 means no
unambiguous location, and 2 means invalid/incomplete input. --require-equal
also returns 1 unless all observed fields and the full vertex count agree.
"""

import argparse
import collections
import csv
import hashlib
import io
import json
import re
import struct
import sys
from dataclasses import dataclass
from decimal import Decimal, InvalidOperation
from pathlib import Path


STATE_FIELDS = ("prim", "xyoffset", "scissor", "test", "zbuf", "tex0",
                "clamp", "frame", "alpha")
OPTIONAL_STATE_FIELDS = ("tex1", "texa", "pabe", "colclamp")
VERTEX_FIELDS = ("x", "y", "z", "color", "st", "uv", "q", "xyz")
REQUIRED_TSV_FIELDS = ("sequence", "packet", *STATE_FIELDS,
                       *(f"v{i}_{name}" for i in range(3) for name in VERTEX_FIELDS))
FOOTER = re.compile(r"# complete triangles=([0-9]+) replay_packets=([0-9]+) trace_packets=([0-9]+)")
NUMBER = re.compile(r"[+-]?(?:[0-9]+(?:\.[0-9]*)?|\.[0-9]+)(?:[eE][+-]?[0-9]+)?")
MAX_TRIANGLES = 100000
MAX_ALIGNMENT_PAIRS = 1000000


@dataclass(frozen=True)
class Vertex:
    x: int
    y: int
    z: int
    color: int
    st: int
    uv: int | None
    q: int
    xyz: int


@dataclass(frozen=True)
class Reference:
    state: dict
    vertices: tuple
    trace_equal_z: bool | None = None


@dataclass(frozen=True)
class Triangle:
    sequence: int
    packet: int
    state: dict
    vertices: tuple


@dataclass(frozen=True)
class Trace:
    triangles: tuple
    replay_packets: int
    trace_packets: int


def uint(text, bits, label, hex_word=False):
    pattern = r"(?:0[xX][0-9a-fA-F]+|[0-9]+)" if hex_word else r"[0-9]+"
    if not re.fullmatch(pattern, text) or len(text) > 32:
        raise ValueError(f"{label}: invalid unsigned integer {text!r}")
    value = int(text, 16 if text.lower().startswith("0x") else 10)
    if value >= 1 << bits:
        raise ValueError(f"{label}: exceeds {bits} bits")
    return value


def decimal(text, label):
    if len(text) > 64 or not NUMBER.fullmatch(text):
        raise ValueError(f"{label}: invalid finite decimal {text!r}")
    try:
        result = Decimal(text)
        if not result.is_finite() or abs(result) > Decimal("1e10"):
            raise ValueError(f"{label}: decimal outside diagnostic bounds")
        return result
    except InvalidOperation as error:
        raise ValueError(f"{label}: invalid decimal") from error


def fixed(text, label, unsigned=True):
    value = decimal(text, label) * 16
    if value != value.to_integral_value() or (unsigned and not 0 <= value <= 65535):
        raise ValueError(f"{label}: not a representable 12.4 coordinate")
    return int(value)


def trunc_div(value, divisor):
    return value // divisor if value >= 0 else -((-value) // divisor)


def parse_context(text):
    sections = {}
    section = None
    for line_number, line in enumerate(text.lstrip("\ufeff").splitlines(), 1):
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        match = re.fullmatch(r"([A-Z][A-Z0-9_]*):", line)
        if match:
            section = match[1]
            if section in sections:
                raise ValueError(f"context line {line_number}: duplicate section {section}")
            sections[section] = {}
            continue
        match = re.fullmatch(r"\s+([A-Z][A-Z0-9_]*):\s+([^#]+?)(?:\s+#.*)?", line)
        if not match or section is None:
            raise ValueError(f"context line {line_number}: malformed field")
        key, value = match[1], match[2].strip()
        if key in sections[section]:
            raise ValueError(f"context line {line_number}: duplicate {section}.{key}")
        sections[section][key] = value

    def get(section_name, key):
        try:
            return sections[section_name][key]
        except KeyError as error:
            raise ValueError(f"context: missing {section_name}.{key}") from error

    def pack(section_name, fields):
        result = 0
        for key, shift, width in fields:
            value = uint(get(section_name, key), width, f"{section_name}.{key}", True)
            result |= value << shift
        return result

    result = {
        "prim": pack("PRIM", [("PRIM", 0, 3), ("IIP", 3, 1), ("TME", 4, 1),
            ("FGE", 5, 1), ("ABE", 6, 1), ("AA1", 7, 1), ("FST", 8, 1),
            ("CTXT", 9, 1), ("FIX", 10, 1)]),
        "xyoffset": fixed(get("XYOFFSET", "OFX"), "OFX") |
                    (fixed(get("XYOFFSET", "OFY"), "OFY") << 32),
        "scissor": pack("SCISSOR", [("SCAX0", 0, 11), ("SCAX1", 16, 11),
                                    ("SCAY0", 32, 11), ("SCAY1", 48, 11)]),
        "test": pack("TEST", [("ATE", 0, 1), ("ATST", 1, 3), ("AREF", 4, 8),
            ("AFAIL", 12, 2), ("DATE", 14, 1), ("DATM", 15, 1),
            ("ZTE", 16, 1), ("ZTST", 17, 2)]),
        "tex0": pack("TEX0", [("TBP0", 0, 14), ("TBW", 14, 6), ("PSM", 20, 6),
            ("TW", 26, 4), ("TH", 30, 4), ("TCC", 34, 1), ("TFX", 35, 2),
            ("CBP", 37, 14), ("CPSM", 51, 4), ("CSM", 55, 1),
            ("CSA", 56, 5), ("CLD", 61, 3)]),
        "clamp": pack("CLAMP", [("WMS", 0, 2), ("WMT", 2, 2), ("MINU", 4, 10),
            ("MAXU", 14, 10), ("MINV", 24, 10), ("MAXV", 34, 10)]),
        "alpha": pack("ALPHA", [("A", 0, 2), ("B", 2, 2), ("C", 4, 2),
                                  ("D", 6, 2), ("FIX", 32, 8)]),
        "texa": pack("TEXA", [("TA0", 0, 8), ("AEM", 15, 1), ("TA1", 32, 8)]),
        "frame": pack("FRAME", [("FBW", 16, 6), ("PSM", 24, 6), ("FBMSK", 32, 32)]),
        "zbuf": pack("ZBUF", [("PSM", 24, 6), ("ZMSK", 32, 1)]),
        "tex1": pack("TEX1", [("LCM", 0, 1), ("MXL", 2, 3), ("MMAG", 5, 1),
            ("MMIN", 6, 3), ("MTBA", 9, 1), ("L", 19, 2)]),
        "pabe": pack("PABE", [("PABE", 0, 1)]),
        "colclamp": pack("COLCLAMP", [("CLAMP", 0, 1)]),
        "fba": pack("FBA", [("FBA", 0, 1)]),
    }
    for section_name, key, target in (("FRAME", "FBP", "frame"), ("ZBUF", "ZBP", "zbuf")):
        block = uint(get(section_name, key), 14, f"{section_name}.{key}", True)
        if block % 32:
            raise ValueError(f"{section_name}.{key}: PCSX2 (*32) block base is not aligned")
        result[target] |= block // 32
    k = fixed(get("TEX1", "K"), "TEX1.K", False)
    if not -2048 <= k <= 2047:
        raise ValueError("TEX1.K: signed fixed-point value exceeds twelve bits")
    result["tex1"] |= (k & 0xFFF) << 32
    if uint(get("PRMODECONT", "AC"), 1, "PRMODECONT.AC") != 1:
        raise ValueError("reference requires PRMODECONT.AC=1 (effective PRIM)")
    if (result["prim"] & 7) not in (3, 4, 5) or result["prim"] & 0x100:
        raise ValueError("reference must be an STQ triangle draw, not FST/other primitives")
    return result


def parse_reference_vertices(text, state, expected_triangles=None):
    rows = {"vertex": [], "vertex_stq": []}
    section = None
    seen = set()
    for line_number, line in enumerate(text.lstrip("\ufeff").splitlines(), 1):
        if not line.strip():
            continue
        if line.startswith("vertex_trace:"):
            if section != "vertex_stq":
                raise ValueError("vertex_trace appeared before vertex_stq")
            section = "summary"
            continue
        if line.startswith("vertex:") or line.startswith("vertex_stq:"):
            match = re.fullmatch(r"(vertex|vertex_stq):\s*# TRIANGLE", line)
            if not match or match[1] in seen or section == "summary":
                raise ValueError(f"vertex line {line_number}: invalid/duplicate section")
            section = match[1]
            if section == "vertex_stq" and "vertex" not in seen:
                raise ValueError("vertex_stq appeared before vertex")
            seen.add(section)
            continue
        if section is None and re.fullmatch(r'flush_reason: "[^"\r\n]*"', line):
            continue
        if section == "summary":
            # Diagnostic min/max lines have no authoritative raw vertex values.
            if re.match(r"\s*-\s*\{", line):
                raise ValueError("vertex row after summary/end of vertex sections")
            continue
        match = re.fullmatch(r"\s*-\s*\{([^{}]+)\}\s*#\s*([0-9]+)\s*", line)
        if not match or section not in rows:
            raise ValueError(f"vertex line {line_number}: malformed row")
        index = uint(match[2], 32, "vertex index")
        if index != len(rows[section]):
            raise ValueError(f"{section}: missing/duplicate/out-of-order index {index}")
        fields = {}
        for item in match[1].split(","):
            pair = re.fullmatch(r"\s*([A-Za-z]+):\s*(\S+)\s*", item)
            if not pair or pair[1] in fields:
                raise ValueError(f"vertex {index}: malformed/duplicate field")
            fields[pair[1]] = pair[2]
        required = set("X Y Z U V R G B A".split()) if section == "vertex" else set("S T Q Si Ti Qi".split())
        if fields.keys() != required:
            raise ValueError(f"{section} {index}: wrong/missing field set")
        rows[section].append(fields)
        if len(rows[section]) > MAX_TRIANGLES * 3:
            raise ValueError("reference vertex count exceeds diagnostic bound")
    count = len(rows["vertex"])
    if seen != rows.keys() or count == 0 or count % 3 or len(rows["vertex_stq"]) != count:
        raise ValueError("reference needs complete, equally sized vertex/STQ sections in triangle triplets")
    if expected_triangles is not None and count != expected_triangles * 3:
        raise ValueError(f"reference contains {count // 3} triangles, expected {expected_triangles}")
    result = []
    offset_x = state["xyoffset"] & 0xFFFF
    offset_y = (state["xyoffset"] >> 32) & 0xFFFF
    for index, (v, stq) in enumerate(zip(rows["vertex"], rows["vertex_stq"])):
        raw_x = fixed(v["X"], f"vertex {index} X", False) + offset_x
        raw_y = fixed(v["Y"], f"vertex {index} Y", False) + offset_y
        if not 0 <= raw_x <= 65535 or not 0 <= raw_y <= 65535:
            raise ValueError(f"vertex {index}: reconstructed XYZ outside guest 16-bit coordinates")
        z = uint(v["Z"], 32, "reference Z")
        color = sum(uint(v[name], 8, f"reference {name}") << shift
                    for name, shift in (("R", 0), ("G", 8), ("B", 16), ("A", 24)))
        for name in ("U", "V"):
            decimal(v[name], f"informational {name}")
        for name in ("S", "T", "Q"):
            decimal(stq[name], f"informational {name}")
        words = []
        for name in ("Si", "Ti", "Qi"):
            word = stq[name]
            if re.fullmatch(r"0[xX][0-9a-fA-F]{8}", word):
                words.append(int(word, 16))
            elif word == "0000000000":
                words.append(0)
            else:
                raise ValueError(f"{name}: expected authoritative 32-bit hex word")
        result.append(Vertex(trunc_div(raw_x - offset_x, 64), trunc_div(raw_y - offset_y, 64),
            z, color, words[0] | (words[1] << 32), None, words[2],
            raw_x | (raw_y << 16) | (z << 32)))
    return tuple(result)


def parse_reference_equal_z(text):
    """Optional saved GS trace condition, kept separate from incoming vertices."""
    values = []
    in_summary = False
    for line in text.lstrip("\ufeff").splitlines():
        if line.startswith("vertex_trace:"):
            in_summary = True
        if re.match(r"\s*eq_xyz:", line):
            match = re.fullmatch(r"\s*eq_xyz:\s*\{X: ([01]), Y: ([01]), Z: ([01])\}\s*", line)
            if not in_summary or not match:
                raise ValueError("malformed/out-of-place saved eq_xyz condition")
            values.append(match[3] == "1")
    if len(values) > 1:
        raise ValueError("duplicate saved eq_xyz condition")
    return values[0] if values else None


def parse_astra_triangles(text):
    lines = text.lstrip("\ufeff").splitlines()
    if not lines:
        raise ValueError("empty triangle TSV")
    header = lines[0].split("\t")
    if len(set(header)) != len(header) or any(not name for name in header):
        raise ValueError("TSV has blank/duplicate header names")
    if not set(REQUIRED_TSV_FIELDS).issubset(header):
        raise ValueError("TSV missing fields: " + ", ".join(sorted(set(REQUIRED_TSV_FIELDS) - set(header))))
    records = []
    footer = None
    for line_number, line in enumerate(lines[1:], 2):
        match = FOOTER.fullmatch(line)
        if match:
            if footer is not None:
                raise ValueError("duplicate TSV completion footer")
            footer = tuple(uint(match[i], 64, "footer count") for i in range(1, 4))
            continue
        if footer is not None:
            raise ValueError("data after TSV completion footer")
        values = next(csv.reader(io.StringIO(line), delimiter="\t", quoting=csv.QUOTE_NONE)) if line else []
        if len(values) != len(header):
            raise ValueError(f"TSV line {line_number}: wrong/truncated field count")
        row = dict(zip(header, values))

        def word(name, width):
            if not re.fullmatch(r"[0-9A-F]{" + str(width) + r"}", row[name]):
                raise ValueError(f"TSV line {line_number}: {name} needs {width} uppercase hex digits")
            return int(row[name], 16)

        def signed(name):
            if not re.fullmatch(r"-?[0-9]+", row[name]) or len(row[name]) > 12:
                raise ValueError(f"TSV line {line_number}: invalid signed decimal {name}")
            value = int(row[name])
            if not -(1 << 31) <= value < 1 << 31:
                raise ValueError(f"TSV {name}: signed coordinate overflow")
            return value

        sequence = uint(row["sequence"], 64, "sequence")
        packet = uint(row["packet"], 64, "packet")
        if (not records and sequence != 0) or packet == 0:
            raise ValueError("complete replay TSV must start at sequence zero with packet ordinals >=1")
        if records and sequence != records[-1].sequence + 1:
            raise ValueError("TSV triangle sequences must be contiguous and increasing")
        if records and packet < records[-1].packet:
            raise ValueError("TSV packet ordinal moved backwards")
        state = {name: word(name, 16) for name in STATE_FIELDS}
        for name in OPTIONAL_STATE_FIELDS:
            if name in row:
                state[name] = uint(row[name], 1, name) if name in ("pabe", "colclamp") else word(name, 16)
        if "ee_cycle" in row:
            uint(row["ee_cycle"], 64, "ee_cycle")
        if "next_ee_pc" in row:
            word("next_ee_pc", 8)
        vertices = []
        for i in range(3):
            prefix = f"v{i}_"
            vertex = Vertex(signed(prefix + "x"), signed(prefix + "y"),
                uint(row[prefix + "z"], 32, "vertex z"), word(prefix + "color", 8),
                word(prefix + "st", 16), word(prefix + "uv", 16),
                word(prefix + "q", 8), word(prefix + "xyz", 16))
            native_x = vertex.xyz & 0xFFFF
            native_y = (vertex.xyz >> 16) & 0xFFFF
            if vertex.z != vertex.xyz >> 32 or vertex.x != trunc_div(native_x - (state["xyoffset"] & 0xFFFF), 64) or vertex.y != trunc_div(native_y - ((state["xyoffset"] >> 32) & 0xFFFF), 64):
                raise ValueError(f"TSV line {line_number}: vertex {i} host/Z fields disagree with raw XYZ")
            vertices.append(vertex)
        records.append(Triangle(sequence, packet, state, tuple(vertices)))
        if len(records) > MAX_TRIANGLES:
            raise ValueError("TSV triangle count exceeds export bound")
    if footer is None or footer[0] != len(records):
        raise ValueError("missing completion footer or wrong declared triangle count")
    if footer[1] > footer[2]:
        raise ValueError("completion footer replay_packets exceeds captured trace_packets")
    if records and records[-1].packet > footer[1]:
        raise ValueError("completion footer replay_packets precedes last triangle packet")
    return Trace(tuple(records), footer[1], footer[2])


def color_summary(vertices):
    colors = [v.color for v in vertices]
    rgba = b"".join(struct.pack("<I", color) for color in colors)
    rgb = b"".join(bytes((color & 255, (color >> 8) & 255, (color >> 16) & 255)) for color in colors)
    return {"vertices": len(colors), "black_rgb_vertices": sum((c & 0xFFFFFF) == 0 for c in colors),
        "unique_rgb_colors": len({c & 0xFFFFFF for c in colors}),
        "unique_rgba_colors": len(set(colors)),
        "max_rgb": [max(((c >> shift) & 255 for c in colors), default=0) for shift in (0, 8, 16)],
        "alpha_histogram": dict(sorted(collections.Counter(c >> 24 for c in colors).items())),
        "rgba_le_sha256": hashlib.sha256(rgba).hexdigest(), "rgb_sha256": hashlib.sha256(rgb).hexdigest()}


def exact_monotonic_alignment(reference_keys, actual_keys, pin_first=False):
    """Sparse ordered-triple LCS; identify pairs shared by ALL maximum alignments.

    A match node (i,j) has strictly smaller reference and actual predecessors.
    Forward/backward longest-chain lengths identify every node on a maximum
    alignment. A rank containing just one such node is mandatory; multiple
    nodes at that rank are ambiguous and are excluded from value comparisons.
    This avoids quadratic dense DP and does not resolve ties using color/STQ.
    """
    if pin_first and (not reference_keys or not actual_keys or reference_keys[0] != actual_keys[0]):
        raise ValueError("pinned geometry anchor is not an exact ordered triple")
    offset = int(pin_first)
    positions = collections.defaultdict(list)
    for j in range(offset, len(actual_keys)):
        positions[actual_keys[j]].append(j)
    groups = []
    pair_count = offset
    reference_hits = {0} if pin_first else set()
    for i in range(offset, len(reference_keys)):
        candidates = positions.get(reference_keys[i], ())
        pair_count += len(candidates)
        if pair_count > MAX_ALIGNMENT_PAIRS:
            raise ValueError("exact geometry candidate pairs exceed bounded alignment capacity")
        if candidates:
            reference_hits.add(i)
        groups.append([[i, j, 0, 0] for j in candidates])

    def query(tree, index):
        best = 0
        while index:
            best = max(best, tree[index])
            index -= index & -index
        return best

    def update(tree, index, value):
        while index < len(tree):
            tree[index] = max(tree[index], value)
            index += index & -index

    size = len(actual_keys)
    tree = [0] * (size + 1)
    longest = 0
    for group in groups:
        for node in group:
            node[2] = 1 + query(tree, node[1])
            longest = max(longest, node[2])
        # Batch updates prevent two matches of the same reference triangle
        # from appearing together in a strictly monotonic alignment.
        for node in group:
            update(tree, node[1] + 1, node[2])
    tree = [0] * (size + 1)
    for group in reversed(groups):
        for node in group:
            node[3] = 1 + query(tree, size - node[1] - 1)
        for node in group:
            update(tree, size - node[1], node[3])
    ranks = collections.defaultdict(list)
    if pin_first:
        ranks[1].append((0, 0))
    for group in groups:
        for i, j, forward, backward in group:
            if forward + backward - 1 == longest:
                ranks[forward + offset].append((i, j))
    mandatory = tuple(nodes[0] for rank, nodes in sorted(ranks.items()) if len(nodes) == 1)
    possible = tuple(pair for rank, nodes in sorted(ranks.items()) for pair in nodes)
    possible_references = {i for i, j in possible}
    mandatory_references = {i for i, j in mandatory}
    return {
        "pairs": mandatory, "possible_pairs": possible,
        "lcs_triangles": longest + offset,
        "exact_candidate_pairs": pair_count,
        "reference_with_exact_candidates": len(reference_hits),
        "reference_without_exact_candidates": tuple(i for i in range(len(reference_keys)) if i not in reference_hits),
        "reference_candidates_excluded_by_monotonicity": tuple(sorted(reference_hits - possible_references)),
        "reference_with_only_ambiguous_alignment": tuple(sorted(possible_references - mandatory_references)),
        "ambiguous_lcs_ranks": tuple(rank for rank, nodes in sorted(ranks.items()) if len(nodes) > 1),
        "ambiguous_candidate_pairs": sum(len(nodes) for nodes in ranks.values() if len(nodes) > 1),
    }


def index_ranges(indices):
    """Compact inclusive ranges without assigning a reason to missing indices."""
    result = []
    for index in sorted(set(indices)):
        if result and index == result[-1][1] + 1:
            result[-1][1] = index
        else:
            result.append([index, index])
    return result


def alignment_runs(pairs, actual):
    runs = []
    for i, j in pairs:
        if runs and i == runs[-1]["reference_end"] + 1 and j == runs[-1]["candidate_end"] + 1:
            runs[-1].update(reference_end=i, candidate_end=j, sequence_end=actual[j].sequence)
        else:
            runs.append({"reference_start": i, "reference_end": i,
                "candidate_start": j, "candidate_end": j,
                "sequence_start": actual[j].sequence, "sequence_end": actual[j].sequence})
    return runs


def pcsx2_precision_words(s, t, q):
    """Independently express the pinned observed finite GS precision rule.

    This is a diagnostic transformation of copies, not GS emulation. Incoming
    zero-Q and nonfinite words need handler/state provenance not available in
    a draw vertex dump, so they fail closed instead of guessing sanitization.
    """
    if any(not 0 <= word <= 0xFFFFFFFF for word in (s, t, q)):
        raise ValueError("STQ word outside 32-bit range")
    if any((word >> 23) & 255 == 255 for word in (s, t, q)):
        raise ValueError("nonfinite STQ")
    if q & 0x7FFFFFFF == 0:
        raise ValueError("zero Q lacks packed-handler provenance")
    q_exponent = (q >> 23) & 255
    values = []
    for word in (s, t):
        exponent = (word >> 23) & 255
        bits = min(23, 9 + max(0, q_exponent - exponent))
        values.append(word & (0xFFFFFFFF ^ ((1 << bits) - 1)))
    return (*values, q & 0xFFFFFF00)


def gs_precision_view(reference, actual, pairs, requested, max_examples):
    uniform_z = len({v.z for v in reference.vertices}) == 1
    prim = reference.state["prim"]
    conditions = {"reference_triangle_class": (prim & 7) in (3, 4, 5),
        "reference_tme": bool(prim & 0x10), "reference_stq_not_fst": not bool(prim & 0x100),
        "saved_trace_equal_z": reference.trace_equal_z, "reference_raw_z_constant": uniform_z}
    view = {"requested": requested, "derived_not_original_gif_words": True,
        "scope": "finite triangle diagnostic; actual draw-boundary/GS trace equality is inferred from reference alignment",
        "source": "https://github.com/PCSX2/pcsx2/blob/v2.8.2/pcsx2/GS/GSState.cpp#L2333-L2467",
        "reference_word_provenance": "post-FlushPrim GSVertex fields, printed with bit_cast before Draw",
        "source_gate": "TME && !FST && (sprite_class || saved_GS_trace_equal_Z)",
        "texture_dimensions_or_filtering_are_not_gates": True,
        "conditions": conditions, "color_is_never_normalized": True,
        "captured_words_unchanged": True, "raw_require_equal_unchanged": True,
        "status": "not_requested"}
    if not requested:
        return view
    eq_z = reference.trace_equal_z if reference.trace_equal_z is not None else uniform_z
    if not all(conditions[key] for key in ("reference_triangle_class", "reference_tme", "reference_stq_not_fst")) or not eq_z:
        view["status"] = "not_applicable_reference_gate"
        return view
    view["gate_basis"] = "saved_eq_xyz.Z" if reference.trace_equal_z is not None else "inferred_from_constant_reference_raw_Z"
    differences = collections.Counter({name: 0 for name in ("S", "T", "Q")})
    raw_differences = collections.Counter()
    skipped = collections.Counter()
    examples = []
    normalized = 0
    differing = 0
    for i, j in pairs:
        record = actual[j]
        actual_prim = record.state["prim"]
        if (actual_prim & 7) not in (3, 4, 5) or not actual_prim & 0x10 or actual_prim & 0x100:
            skipped["actual primitive is not a textured STQ triangle"] += 3
            continue
        for k, got in enumerate(record.vertices):
            expected = reference.vertices[i * 3 + k]
            words = (got.st & 0xFFFFFFFF, got.st >> 32, got.q)
            expected_words = (expected.st & 0xFFFFFFFF, expected.st >> 32, expected.q)
            for name, left, right in zip(("S", "T", "Q"), words, expected_words):
                raw_differences[name] += left != right
            try:
                adjusted = pcsx2_precision_words(*words)
                # Reject nonfinite/zero expected Q as well; this view does
                # not claim a model for exceptional handler paths.
                pcsx2_precision_words(*expected_words)
            except ValueError as error:
                skipped[str(error)] += 1
                continue
            normalized += 1
            fields = [name for name, left, right in zip(("S", "T", "Q"), adjusted, expected_words) if left != right]
            differences.update(fields)
            if fields:
                differing += 1
            if fields and len(examples) < max_examples:
                examples.append({"reference_vertex_index": i * 3 + k, "triangle_sequence": record.sequence,
                    "fields": fields, "incoming_words": [f"{word:08X}" for word in words],
                    "derived_gs_precision_words": [f"{word:08X}" for word in adjusted],
                    "reference_saved_words": [f"{word:08X}" for word in expected_words]})
    view.update(status="derived_comparison", normalized_vertices=normalized,
        unmodeled_vertices=sum(skipped.values()), unmodeled_reasons=dict(skipped),
        raw_word_differences=dict(raw_differences), derived_word_differences=dict(differences),
        derived_all_compared_words_equal=normalized > 0 and normalized == len(pairs) * 3 and not any(differences.values()),
        examples=examples, examples_truncated=differing > len(examples))
    return view


def compare(reference, trace, allow_z_fallback=False, candidate_index=None, max_examples=12, pcsx2_gs_precision=False):
    records = trace.triangles
    count = len(reference.vertices) // 3
    anchor = tuple(v.xyz for v in reference.vertices[:3])
    exact = [i for i, record in enumerate(records) if tuple(v.xyz for v in record.vertices) == anchor]
    target_z = reference.vertices[0].z
    uniform_z = all(v.z == target_z for v in reference.vertices)
    z_runs = []
    if uniform_z:
        start = None
        for i in range(len(records) + 1):
            matches = i < len(records) and all(v.z == target_z for v in records[i].vertices)
            if matches and start is None:
                start = i
            elif not matches and start is not None:
                z_runs.append((start, i - start))
                start = None
    report = {"scope": "raw emitted triangle records, not image/hardware/animation equivalence",
        "reference_triangles": count, "reference_colors": color_summary(reference.vertices),
        "reference_state": {name: f"{value:016X}" for name, value in reference.state.items()},
        "trace_triangles": len(records), "trace_complete": True,
        "raw_stq_scope": "Astra incoming words versus saved post-flush PCSX2 GSVertex words; mismatch alone is not an EE/VU source-error claim",
        "exact_candidates": [{"record_index": i, "sequence": records[i].sequence, "packet": records[i].packet} for i in exact],
        "z_candidates": [{"record_index": i, "sequence": records[i].sequence, "packet": records[i].packet, "triangles": n} for i, n in z_runs],
        "match_quality": "no_exact_location", "all_observed_fields_equal": False}
    candidates = exact
    quality = "exact_raw_xyz_anchor"
    if not candidates:
        if not allow_z_fallback:
            return report
        candidates = [i for i, n in z_runs if n == count]
        quality = "z_only_not_an_exact_location"
    if not candidates:
        return report
    if candidate_index is None and len(candidates) != 1:
        report["match_quality"] = "ambiguous_exact_location" if exact else "ambiguous_z_only_location"
        return report
    selected = 0 if candidate_index is None else candidate_index
    if not 0 <= selected < len(candidates):
        raise ValueError("candidate-index is outside the selected match candidate list")
    start = candidates[selected]
    available = next((n - (start - i) for i, n in z_runs if i <= start < i + n), count) if uniform_z else count
    # A same-Z run is a search domain, NOT a GS context-flush/draw boundary.
    # Reference dumps can omit submitted triangles; align exact geometry
    # monotonically throughout the candidate domain, never by array position.
    actual = records[start:start + available]
    reference_keys = [tuple(v.xyz for v in reference.vertices[i:i + 3]) for i in range(0, count * 3, 3)]
    actual_keys = [tuple(v.xyz for v in record.vertices) for record in actual]
    alignment = exact_monotonic_alignment(reference_keys, actual_keys, quality == "exact_raw_xyz_anchor")
    pairs = alignment["pairs"]
    actual_vertices = tuple(v for i, j in pairs for v in actual[j].vertices)
    reference_vertices = tuple(reference.vertices[i * 3 + k] for i, j in pairs for k in range(3))
    last_possible = max((j for i, j in alignment["possible_pairs"]), default=-1)
    matched_candidate_indices = {j for i, j in alignment["possible_pairs"]}
    gap_ranges = index_ranges(j for j in range(last_possible + 1) if j not in matched_candidate_indices)
    missing_ranges = index_ranges(alignment["reference_without_exact_candidates"])
    runs = alignment_runs(pairs, actual)
    geometry = {name: value for name, value in alignment.items() if name not in (
        "pairs", "possible_pairs", "reference_without_exact_candidates",
        "reference_candidates_excluded_by_monotonicity", "reference_with_only_ambiguous_alignment")}
    excluded_ranges = index_ranges(alignment["reference_candidates_excluded_by_monotonicity"])
    ambiguous_ranges = index_ranges(alignment["reference_with_only_ambiguous_alignment"])
    geometry.update(method="ordered_raw_xyz_triple_sparse_LCS",
        first_anchor_pinned=quality == "exact_raw_xyz_anchor",
        comparison_pairs="pairs present in every maximum monotonic alignment only",
        unambiguous_matched_triangles=len(pairs), compared_vertices=len(actual_vertices),
        reference_unaligned_triangles=count - alignment["lcs_triangles"],
        candidate_triangles=len(actual), candidate_triangles_through_last_possible_match=last_possible + 1,
        candidate_without_exact_match_through_last_possible_match=sum(end - begin + 1 for begin, end in gap_ranges),
        candidate_tail_after_last_possible_match=len(actual) - last_possible - 1,
        reference_without_exact_candidate_ranges=missing_ranges[:max_examples],
        reference_candidates_excluded_by_monotonicity_ranges=excluded_ranges[:max_examples],
        reference_with_only_ambiguous_alignment_ranges=ambiguous_ranges[:max_examples],
        candidate_without_exact_match_ranges=gap_ranges[:max_examples],
        matched_runs=runs[:max_examples],
        range_examples_truncated=max(map(len, (missing_ranges, excluded_ranges, ambiguous_ranges, gap_ranges, runs))) > max_examples,
        skips_are_not_proven_culling=True, draw_boundary_proven=False)
    report.update(match_quality=quality, selected_candidate_index=selected,
        first_sequence=records[start].sequence, first_packet=records[start].packet,
        first_actual_state={name: f"{value:016X}" for name, value in records[start].state.items()},
        actual_triangles=len(pairs), reference_span_complete=len(pairs) == count,
        count_matches=None if available > count or not uniform_z else len(actual) == count and len(pairs) == count,
        count_basis="unambiguous exact geometry alignment; same-Z run is only a search domain",
        same_z_run_triangles=available if uniform_z else None,
        same_z_excess_over_reference_triangles=max(0, available - count) if uniform_z else None,
        uncertain_same_z_tail_triangles=len(actual) - last_possible - 1 if uniform_z else None,
        draw_boundary_proven=False,
        actual_colors=color_summary(actual_vertices), reference_aligned_colors=color_summary(reference_vertices),
        geometry_alignment=geometry,
        unavailable_state_fields=[name for name in OPTIONAL_STATE_FIELDS if name not in records[start].state])
    differences = collections.Counter()
    raw_stq_differences = collections.Counter()
    examples = []
    differing_vertices = 0
    for index, (expected, got) in enumerate(zip(reference_vertices, actual_vertices)):
        reference_triangle, candidate_triangle = pairs[index // 3]
        reference_index = reference_triangle * 3 + index % 3
        for name, left, right in zip(("S", "T", "Q"),
                (expected.st & 0xFFFFFFFF, expected.st >> 32, expected.q),
                (got.st & 0xFFFFFFFF, got.st >> 32, got.q)):
            raw_stq_differences[name] += left != right
        fields = []
        for name in ("xyz", "color", "st", "q", "z"):
            if getattr(expected, name) != getattr(got, name):
                differences[name] += 1
                fields.append(name)
        if expected.color & 0xFFFFFF != got.color & 0xFFFFFF:
            differences["rgb"] += 1
            fields.append("rgb")
        if (expected.x, expected.y) != (got.x, got.y):
            differences["host_xy"] += 1
            fields.append("host_xy")
        if fields:
            differing_vertices += 1
        if fields and len(examples) < max_examples:
            examples.append({"vertex_index": reference_index, "reference_triangle_index": reference_triangle,
                "candidate_triangle_index": candidate_triangle, "triangle_sequence": actual[candidate_triangle].sequence,
                "fields": fields, "reference_xyz": f"{expected.xyz:016X}", "actual_xyz": f"{got.xyz:016X}",
                "reference_color": f"{expected.color:08X}", "actual_color": f"{got.color:08X}",
                "reference_st": f"{expected.st:016X}", "actual_st": f"{got.st:016X}",
                "reference_q": f"{expected.q:08X}", "actual_q": f"{got.q:08X}"})
    state_differences = collections.Counter()
    first_state_differences = {}
    for i, j in pairs:
        record = actual[j]
        for name, value in record.state.items():
            if value != reference.state[name]:
                state_differences[name] += 1
                first_state_differences.setdefault(name, {"sequence": record.sequence,
                    "reference": f"{reference.state[name]:016X}", "actual": f"{value:016X}"})
    report.update(vertex_differences=dict(differences), vertex_examples=examples,
        raw_stq_word_differences=dict(raw_stq_differences),
        pcsx2_gs_precision=gs_precision_view(reference, actual, pairs, pcsx2_gs_precision, max_examples),
        vertex_examples_truncated=differing_vertices > len(examples),
        state_differences=dict(state_differences), first_state_differences=first_state_differences,
        unpaired_reference_vertices=len(reference.vertices) - len(reference_vertices),
        unpaired_actual_vertices=0)
    report["all_paired_fields_equal"] = report["reference_span_complete"] and not differences and not state_differences
    report["all_observed_fields_equal"] = quality == "exact_raw_xyz_anchor" and report["count_matches"] is True and report["all_paired_fields_equal"]
    return report


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--context", required=True, type=Path)
    parser.add_argument("--vertices", required=True, type=Path)
    parser.add_argument("--triangles", required=True, type=Path)
    parser.add_argument("--expected-triangles", type=int, default=244)
    parser.add_argument("--allow-z-fallback", action="store_true")
    parser.add_argument("--candidate-index", type=int)
    parser.add_argument("--max-examples", type=int, default=12)
    parser.add_argument("--require-equal", action="store_true")
    parser.add_argument("--pcsx2-gs-precision", action="store_true",
        help="add explicit derived pinned GS precision view; raw fields and --require-equal stay unchanged")
    args = parser.parse_args(argv)
    if not 1 <= args.expected_triangles <= MAX_TRIANGLES or not 0 <= args.max_examples <= 128:
        parser.error("expected-triangles must be 1..100000; max-examples must be 0..128")
    try:
        state = parse_context(args.context.read_text(encoding="utf-8-sig"))
        reference_text = args.vertices.read_text(encoding="utf-8-sig")
        vertices = parse_reference_vertices(reference_text, state, args.expected_triangles)
        trace = parse_astra_triangles(args.triangles.read_text(encoding="utf-8-sig"))
        report = compare(Reference(state, vertices, parse_reference_equal_z(reference_text)), trace,
            args.allow_z_fallback, args.candidate_index, args.max_examples, args.pcsx2_gs_precision)
    except (OSError, UnicodeError, ValueError) as error:
        print(f"compare_cloud_vertices: {error}", file=sys.stderr)
        return 2
    print(json.dumps(report, indent=2, sort_keys=True))
    found = report["match_quality"] in ("exact_raw_xyz_anchor", "z_only_not_an_exact_location")
    return 0 if found and (not args.require_equal or report["all_observed_fields_equal"]) else 1


if __name__ == "__main__":
    raise SystemExit(main())
