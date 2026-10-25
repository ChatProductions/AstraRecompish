#!/usr/bin/env python3
"""Contract tests for the minimal AstraIR instruction builder."""

import json
from pathlib import Path
import tempfile
import unittest

from astrair.builder import build_data_instruction
from astrair.analysis_effects import is_hard_barrier
from astrair.analysis_liveness import analyze_block
from astrair.analysis_width import infer_block_results
from astrair.deopt import (
    DeoptMap, Guard, GuardKind, RecoveryKind, RegisterRecovery,
    validate_deopt_metadata,
)
from astrair.ir import Effect, Op, UpperBits, ValueKind
from generate_astrart import (
    Function, defer_gpr_writeback, discover, generate, generate_trace_plan,
    has_delay_slot, select_direct_traces, split_for_event_horizon,
)
from make_aot_chain_elf import build as build_chain_elf
from make_aot_memory_elf import elf as build_memory_elf
from astrair.profile import (
    CensusError, FINGERPRINT_SCHEME, load_execution_profile, source_fingerprint,
)
from astrair.trace import (
    BlockProfile, EdgeProfile, Trace, form_direct_traces, serialize_trace_plan,
)


def r_type(rs: int, rt: int, rd: int, sub: int, function: int) -> int:
    return (rs << 21) | (rt << 16) | (rd << 11) | (sub << 6) | function


def i_type(primary: int, rs: int, rt: int, immediate: int) -> int:
    return (primary << 26) | (rs << 21) | (rt << 16) | (immediate & 0xFFFF)


class AstraIrBuilderTests(unittest.TestCase):
    def test_supported_subset_has_typed_operations(self) -> None:
        cases = {
            0: Op.NOP,
            i_type(0x09, 3, 2, -4): Op.ADDIU,
            r_type(3, 4, 2, 0, 0x21): Op.ADDU,
            r_type(3, 4, 2, 0, 0x24): Op.AND,
            r_type(3, 4, 2, 0, 0x2B): Op.SLTU,
            r_type(3, 4, 2, 0, 0x2D): Op.DADDU,
            r_type(3, 4, 2, 0, 0x2F): Op.DSUBU,
            r_type(3, 4, 2, 0, 0x18): Op.MULT,
            r_type(3, 0, 0, 0, 0x11): Op.MTHI,
            r_type(3, 0, 0, 0, 0x13): Op.MTLO,
            r_type(3, 0, 0, 0, 0x11) | (0x1C << 26): Op.MTHI1,
            r_type(3, 0, 0, 0, 0x13) | (0x1C << 26): Op.MTLO1,
            i_type(0x01, 3, 0x19, 5): Op.MTSAH,
            (0x11 << 26) | (0x04 << 21) | (5 << 16) | (7 << 11): Op.MTC1,
            (0x11 << 26) | (0x06 << 21) | (5 << 16) | (7 << 11): Op.CTC1,
            (0x11 << 26) | (0x10 << 21) | (5 << 16) | (7 << 11) | 0x18: Op.ADDA_S,
            0x0000040F: Op.SYNC,
            r_type(3, 4, 2, 0x12, 0x09) | (0x1C << 26): Op.PAND,
            r_type(3, 4, 2, 0x13, 0x09) | (0x1C << 26): Op.PXOR,
            r_type(3, 4, 2, 0x12, 0x29) | (0x1C << 26): Op.POR,
            r_type(3, 4, 2, 0x10, 0x28) | (0x1C << 26): Op.PADDUW,
            i_type(0x0C, 3, 2, 0x1234): Op.ANDI,
            i_type(0x0D, 3, 2, 0x1234): Op.ORI,
            i_type(0x0F, 0, 2, 0x1234): Op.LUI,
            i_type(0x23, 3, 2, -4): Op.LW,
            i_type(0x28, 3, 2, 1): Op.SB,
            i_type(0x2B, 3, 2, -4): Op.SW,
            i_type(0x1F, 3, 2, -16): Op.SQ,
            i_type(0x37, 3, 2, -8): Op.LD,
            i_type(0x3F, 3, 2, -8): Op.SD,
        }
        for word, expected in cases.items():
            with self.subTest(word=f"0x{word:08x}"):
                instruction = build_data_instruction(0x1000, word)
                self.assertIsNotNone(instruction)
                self.assertEqual(instruction.op, expected)
                self.assertEqual(instruction.pc, 0x1000)
                self.assertEqual(instruction.word, word)

    def test_fields_and_signed_immediate_are_preserved(self) -> None:
        instruction = build_data_instruction(0x2000, i_type(0x09, 7, 9, -4))
        self.assertEqual(
            (instruction.rs, instruction.rt, instruction.rd,
             instruction.immediate),
            (7, 9, 31, -4),
        )

    def test_control_and_unknown_operations_are_not_data_ir(self) -> None:
        self.assertIsNone(build_data_instruction(0x1000, i_type(0x04, 1, 2, 1)))
        self.assertIsNone(build_data_instruction(0x1000, i_type(0x08, 1, 2, -1)))

    def test_effects_are_conservative_trace_barriers(self) -> None:
        pure = build_data_instruction(0x1000, r_type(3, 4, 2, 0, 0x21))
        load = build_data_instruction(0x1004, i_type(0x23, 3, 2, 4))
        store = build_data_instruction(0x1008, i_type(0x2B, 3, 2, 4))
        self.assertEqual(pure.effects, Effect.PURE)
        self.assertFalse(is_hard_barrier(pure))
        self.assertEqual(
            load.effects,
            Effect.MEMORY_READ | Effect.MAY_FAULT | Effect.MAY_TOUCH_MMIO,
        )
        self.assertTrue(is_hard_barrier(load))
        self.assertEqual(
            store.effects,
            Effect.MEMORY_WRITE
            | Effect.MAY_FAULT
            | Effect.MAY_TOUCH_MMIO
            | Effect.MAY_SCHEDULE_EVENT,
        )
        self.assertTrue(is_hard_barrier(store))

    def test_static_result_widths_and_upper_bits(self) -> None:
        cases = (
            (i_type(0x09, 3, 2, -4), 2, ValueKind.S32,
             UpperBits.SIGN_EXTENDED_32),
            (i_type(0x0C, 3, 2, 0x1234), 2, ValueKind.U16,
             UpperBits.ZERO),
            (r_type(3, 4, 2, 0, 0x2B), 2, ValueKind.I1,
             UpperBits.ZERO),
            (i_type(0x37, 3, 2, -8), 2, ValueKind.U64,
             UpperBits.UNKNOWN),
            (r_type(3, 4, 2, 0x12, 0x09) | (0x1C << 26), 2,
             ValueKind.V128, UpperBits.UNKNOWN),
            (r_type(3, 4, 2, 0x10, 0x28) | (0x1C << 26), 2,
             ValueKind.V128, UpperBits.UNKNOWN),
            (i_type(0x2B, 3, 2, 4), 0, ValueKind.NONE,
             UpperBits.UNKNOWN),
        )
        for word, register, kind, upper in cases:
            with self.subTest(word=f"0x{word:08x}"):
                instruction = build_data_instruction(0x1000, word)
                self.assertEqual(instruction.result_register, register)
                self.assertEqual(instruction.result_kind, kind)
                self.assertEqual(instruction.upper_bits, upper)

    def test_write_to_zero_has_no_result(self) -> None:
        instruction = build_data_instruction(0x1000, i_type(0x09, 3, 0, 1))
        self.assertEqual(instruction.result_register, 0)
        self.assertEqual(instruction.result_kind, ValueKind.NONE)

    def test_block_width_inference_propagates_safe_facts(self) -> None:
        words = (
            i_type(0x0F, 0, 2, 0x1234),
            i_type(0x0D, 2, 3, 0x5678),
            i_type(0x0D, 0, 4, 0x00FF),
            r_type(3, 4, 5, 0, 0x24),
        )
        instructions = [
            build_data_instruction(0x1000 + index * 4, word)
            for index, word in enumerate(words)
        ]
        inferred = infer_block_results(instructions)
        self.assertEqual(
            (inferred[1].result_kind, inferred[1].upper_bits),
            (ValueKind.S32, UpperBits.SIGN_EXTENDED_32),
        )
        self.assertEqual(
            (inferred[2].result_kind, inferred[2].upper_bits),
            (ValueKind.U16, UpperBits.ZERO),
        )
        self.assertEqual(
            (inferred[3].result_kind, inferred[3].upper_bits),
            (ValueKind.U16, UpperBits.ZERO),
        )

    def test_register_use_def_and_backward_liveness(self) -> None:
        instructions = [
            build_data_instruction(0x1000, i_type(0x09, 4, 2, 1)),
            build_data_instruction(0x1004, i_type(0x09, 5, 2, 2)),
            build_data_instruction(0x1008, r_type(2, 6, 3, 0, 0x21)),
        ]
        live = analyze_block(instructions, live_out={3})
        self.assertEqual(instructions[0].reads, frozenset({4}))
        self.assertEqual(instructions[0].writes, frozenset({2}))
        self.assertEqual(live[0].dead_writes, frozenset({2}))
        self.assertEqual(live[1].dead_writes, frozenset())
        self.assertEqual(live[2].live_out, frozenset({3}))
        self.assertEqual(live[0].live_in, frozenset({4, 5, 6}))

    def test_store_reads_address_and_value_without_writing_gpr(self) -> None:
        for primary in (0x28, 0x2B, 0x1F, 0x3F):
            with self.subTest(primary=primary):
                store = build_data_instruction(
                    0x1000, i_type(primary, 3, 2, 4))
                self.assertEqual(store.reads, frozenset({2, 3}))
                self.assertEqual(store.writes, frozenset())
                self.assertTrue(is_hard_barrier(store))

    def test_generator_preserves_byte_store_and_aligned_quad_store(self) -> None:
        from generate_astrart import emit_data
        byte_lines = []
        emit_data(byte_lines, 0x1000, i_type(0x28, 3, 2, -1))
        self.assertIn("memory.write8(address_00001000, static_cast<std::uint8_t>(state.gpr[2]));", "\n".join(byte_lines))
        self.assertIn("memory.valid(address_00001000, 1u)", "\n".join(byte_lines))
        quad_lines = []
        emit_data(quad_lines, 0x1004, i_type(0x1F, 3, 2, 7))
        self.assertIn("& ~15u", "\n".join(quad_lines))
        self.assertIn("memory.valid(address_00001004, 16u)", "\n".join(quad_lines))
        self.assertIn("memory.write64(address_00001004 + 8u, state.gpr_hi[2]);", "\n".join(quad_lines))

    def test_accumulator_writes_read_source_without_clobbering_gpr(self) -> None:
        for opcode in (0x11, 0x13):
            for primary in (0, 0x1C):
                word = r_type(3, 0, 0, 0, opcode) | (primary << 26)
                instruction = build_data_instruction(0x1000, word)
                self.assertEqual(instruction.reads, frozenset({3}))
                self.assertEqual(instruction.writes, frozenset())

    def test_mtsah_and_mtc1_read_sources_without_gpr_writes(self) -> None:
        mtsah = build_data_instruction(0x1000, i_type(0x01, 3, 0x19, 5))
        mtc1 = build_data_instruction(
            0x1004, (0x11 << 26) | (0x04 << 21) | (5 << 16) | (7 << 11)
        )
        self.assertEqual(mtsah.reads, frozenset({3}))
        self.assertEqual(mtsah.writes, frozenset())
        self.assertEqual(mtc1.reads, frozenset({5}))
        self.assertEqual(mtc1.writes, frozenset())

    def test_cop1_control_and_accumulator_have_no_gpr_destinations(self) -> None:
        control = build_data_instruction(
            0x1000, (0x11 << 26) | (0x06 << 21) | (5 << 16) | (31 << 11)
        )
        accumulate = build_data_instruction(
            0x1004, (0x11 << 26) | (0x10 << 21) | (1 << 16) | 0x18
        )
        self.assertEqual(control.reads, frozenset({5}))
        self.assertEqual(control.writes, frozenset())
        self.assertEqual(accumulate.reads, frozenset())
        self.assertEqual(accumulate.writes, frozenset())

    def test_deferred_writeback_flushes_every_commit_path(self) -> None:
        source = [
            "AotExit generated(Memory& memory, CpuState& state) {",
            "  std::uint32_t executed = 0;",
            "  std::uint32_t fast = 0;",
            "  state.gpr[2] = state.gpr[4] + state.gpr[0];",
            "  if (fault) {",
            "    commit(memory, state, executed, fast);",
            "    return {};",
            "  }",
            "  commit(memory, state, executed, fast);",
            "  return {};",
            "}",
        ]
        generated = defer_gpr_writeback(source)
        self.assertIn("  std::uint64_t gpr_2 = state.gpr[2];", generated)
        self.assertIn("  std::uint64_t gpr_4 = state.gpr[4];", generated)
        self.assertIn("  gpr_2 = gpr_4 + state.gpr[0];", generated)
        self.assertEqual(generated.count("    state.gpr[2] = gpr_2;"), 1)
        self.assertEqual(generated.count("  state.gpr[2] = gpr_2;"), 1)
        for index, line in enumerate(generated):
            if "commit(memory, state, executed, fast);" in line:
                self.assertIn("state.gpr[2] = gpr_2;", generated[index - 1])

    def test_profile_guided_direct_trace_closes_hot_loop(self) -> None:
        traces = form_direct_traces(
            blocks=(BlockProfile(0x1000, 1000), BlockProfile(0x1010, 950),
                    BlockProfile(0x1020, 50)),
            edges=(EdgeProfile(0x1000, 0x1010, 1000),
                   EdgeProfile(0x1010, 0x1000, 950)),
            direct_successors={0x1000: frozenset({0x1010}),
                               0x1010: frozenset({0x1000})},
        )
        self.assertEqual(traces[0].blocks, (0x1000, 0x1010))
        self.assertTrue(traces[0].closes_loop)

    def test_trace_stops_at_barrier_ambiguity_and_indirect_edge(self) -> None:
        barrier = form_direct_traces(
            (BlockProfile(1, 100), BlockProfile(2, 100, hard_barrier=True),
             BlockProfile(3, 100)),
            (EdgeProfile(1, 2, 100), EdgeProfile(2, 3, 100)),
            {1: frozenset({2}), 2: frozenset({3})},
        )
        self.assertEqual(barrier[0].blocks, (1, 2))
        ambiguous = form_direct_traces(
            (BlockProfile(1, 100), BlockProfile(2, 50), BlockProfile(3, 50)),
            (EdgeProfile(1, 2, 50), EdgeProfile(1, 3, 50)),
            {1: frozenset({2, 3})},
        )
        self.assertEqual(ambiguous[0].blocks, (1,))
        biased_conditional = form_direct_traces(
            (BlockProfile(1, 100), BlockProfile(2, 99), BlockProfile(3, 1)),
            (EdgeProfile(1, 2, 99), EdgeProfile(1, 3, 1)),
            {1: frozenset({2, 3})},
        )
        self.assertEqual(biased_conditional[0].blocks, (1,))
        indirect = form_direct_traces(
            (BlockProfile(1, 100), BlockProfile(2, 100)),
            (EdgeProfile(1, 2, 100),),
            {1: frozenset()},
        )
        self.assertEqual(indirect[0].blocks, (1,))

    def test_trace_order_is_deterministic(self) -> None:
        traces = form_direct_traces(
            (BlockProfile(0x20, 7), BlockProfile(0x10, 7)), (), {})
        self.assertEqual([trace.blocks for trace in traces], [(0x10,), (0x20,)])

    def test_profile_requires_exact_elf_set_fingerprint(self) -> None:
        fingerprint = source_fingerprint((b"first", b"second"))
        document = {
            "schema": "astrarecomp.execution-census",
            "version": 3,
            "source": {
                "kind": "elf-set",
                "fingerprint_scheme": FINGERPRINT_SCHEME,
                "fingerprint_sha256": fingerprint,
            },
            "ee": {
                "blocks": [{"pc": "0x00001000", "entries": 7}],
                "edges": [],
            },
        }
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "census.json"
            path.write_text(json.dumps(document), encoding="utf-8")
            profile = load_execution_profile(path, fingerprint)
            self.assertEqual(profile.blocks, (BlockProfile(0x1000, 7),))
            with self.assertRaisesRegex(CensusError, "fingerprint mismatch"):
                load_execution_profile(path, "0" * 64)
            del document["source"]
            path.write_text(json.dumps(document), encoding="utf-8")
            with self.assertRaisesRegex(CensusError, "source must be an object"):
                load_execution_profile(path, fingerprint)

    def test_source_fingerprint_is_ordered_and_length_delimited(self) -> None:
        self.assertNotEqual(
            source_fingerprint((b"a", b"bc")),
            source_fingerprint((b"ab", b"c")),
        )
        self.assertNotEqual(
            source_fingerprint((b"first", b"second")),
            source_fingerprint((b"second", b"first")),
        )

    def test_trace_plan_serialization_is_deterministic(self) -> None:
        traces = (Trace((0x1000, 0x1010), True), Trace((0x2000,), False))
        first = serialize_trace_plan(traces, "a" * 64)
        second = serialize_trace_plan(traces, "a" * 64)
        self.assertEqual(first, second)
        document = json.loads(first)
        self.assertEqual(document["traces"][0]["blocks"],
                         ["0x00001000", "0x00001010"])
        self.assertTrue(document["traces"][0]["closes_loop"])
        self.assertEqual(document["traces"][0]["guards"], [])
        self.assertEqual(document["deopt_maps"], [])

    def test_guard_requires_complete_deopt_map(self) -> None:
        recovery = RegisterRecovery(2, False, RecoveryKind.TRACE_LOCAL, 3)
        deopt_map = DeoptMap(7, 0x1020, (recovery,))
        guard = Guard(GuardKind.BRANCH_DIRECTION, 0x1010, 1, 0x1020, 7)
        validate_deopt_metadata((guard,), (deopt_map,))
        document = json.loads(serialize_trace_plan(
            (Trace((0x1000,), False, (guard,)),), "b" * 64, (deopt_map,)
        ))
        self.assertEqual(document["traces"][0]["guards"][0]["deopt_id"], 7)
        self.assertEqual(document["deopt_maps"][0]["recoveries"][0], {
            "high_half": False,
            "kind": "trace_local",
            "register": 2,
            "value": 3,
        })
        with self.assertRaisesRegex(ValueError, "missing deopt ID"):
            validate_deopt_metadata((guard,), ())
        mismatched = DeoptMap(7, 0x1030, (recovery,))
        with self.assertRaisesRegex(ValueError, "must match"):
            validate_deopt_metadata((guard,), (mismatched,))

    def test_generator_builds_plan_only_from_matching_profile_and_static_cfg(self) -> None:
        elf = build_chain_elf()
        fingerprint = source_fingerprint((elf,))
        census = {
            "schema": "astrarecomp.execution-census",
            "version": 3,
            "source": {
                "kind": "elf-set",
                "fingerprint_scheme": FINGERPRINT_SCHEME,
                "fingerprint_sha256": fingerprint,
            },
            "ee": {
                "blocks": [
                    {"pc": "0x00003000", "entries": 100},
                    {"pc": "0x00003020", "entries": 100},
                    {"pc": "0x00003008", "entries": 1},
                ],
                "edges": [
                    {"source": "0x00003000", "target": "0x00003020",
                     "transitions": 100},
                    # Observed but indirect JR target: static CFG must reject it.
                    {"source": "0x00003020", "target": "0x00003008",
                     "transitions": 100},
                ],
            },
        }
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            elf_path = root / "chain.elf"
            census_path = root / "census.json"
            elf_path.write_bytes(elf)
            census_path.write_text(json.dumps(census), encoding="utf-8")
            plan = json.loads(generate_trace_plan([elf_path], census_path))
        self.assertEqual(plan["source"]["fingerprint_sha256"], fingerprint)
        self.assertEqual(plan["traces"][0]["blocks"],
                         ["0x00003000", "0x00003020"])
        self.assertEqual(plan["traces"][1]["blocks"], ["0x00003008"])

    def test_generator_emits_only_validated_guard_free_trace_wrappers(self) -> None:
        elf = build_chain_elf()
        fingerprint = source_fingerprint((elf,))
        census = {
            "schema": "astrarecomp.execution-census",
            "version": 3,
            "source": {
                "kind": "elf-set",
                "fingerprint_scheme": FINGERPRINT_SCHEME,
                "fingerprint_sha256": fingerprint,
            },
            "ee": {
                "blocks": [
                    {"pc": "0x00003000", "entries": 100},
                    {"pc": "0x00003020", "entries": 100},
                ],
                "edges": [
                    {"source": "0x00003000", "target": "0x00003020",
                     "transitions": 100},
                ],
            },
        }
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            elf_path = root / "chain.elf"
            census_path = root / "census.json"
            elf_path.write_bytes(elf)
            census_path.write_text(json.dumps(census), encoding="utf-8")
            traces, _ = select_direct_traces([elf_path], census_path)
            traces = tuple(trace for trace in traces if len(trace.blocks) >= 2)
            generated = generate([elf_path], "trace-test", True, traces)
        self.assertIn("AotExit generated_trace_00003000", generated)
        self.assertIn("memory.cycles_until_next_event() <= 5u", generated)
        self.assertIn("return generated_00003000(memory, state);", generated)
        self.assertNotIn("AotExit exit_0 = generated_00003000", generated)
        self.assertIn(
            "{0x00003000u, 0x00003008u, generated_trace_00003000}", generated
        )
        self.assertIn(
            "{0x00003020u, 0x0000302cu, generated_00003020}", generated
        )

        guarded = Trace(
            (0x3000, 0x3020), False,
            (Guard(GuardKind.BRANCH_DIRECTION, 0x3000, 1, 0x3020, 0),),
        )
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "chain.elf"
            path.write_bytes(elf)
            with self.assertRaisesRegex(ValueError, "guarded traces"):
                generate([path], "trace-test", True, (guarded,))

    def test_private_vita_package_guards_code_and_uses_separate_factory(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "chain.elf"
            path.write_bytes(build_chain_elf())
            generated = generate(
                [path], "private-title", factory_name="generated_private_game_aot_package",
                guard_guest_code=True,
            )
        self.assertIn("memory.read32(0x00003000u) !=", generated)
        self.assertIn("generated_private_game_aot_package()", generated)
        self.assertNotIn("generated_phase0_aot_package()", generated)
        self.assertIn("state.pc, 0u};", generated)
        with self.assertRaisesRegex(ValueError, "identifier"):
            generate([path], "private-title", factory_name="invalid.name")

    def test_recompiler_resumes_after_guest_syscall_and_fallback(self) -> None:
        # The native block stops at the syscall; the PC after the BIOS/kernel
        # return is nevertheless a valid candidate for a new native block.
        base = 0x8000
        words = {
            base: i_type(0x09, 0, 2, 42),
            base + 4: 0x0000000C,  # SYSCALL, interpreter only
            base + 8: i_type(0x09, 2, 2, 1),
            base + 12: 0x03E00008,  # jr ra
            base + 16: 0x00000000,
        }
        blocks = discover([base], words)
        self.assertEqual([(b.start, b.end) for b in blocks], [
            (base, base + 8), (base + 8, base + 20)])
        # MFC0 remains interpreter-only but does not truncate discovery.
        words = {
            base: 0x40026000,       # mfc0 v0, status (unsupported AOT)
            base + 4: i_type(0x09, 2, 2, 1),
            base + 8: 0x0000000D,   # BREAK
        }
        self.assertEqual([(b.start, b.end) for b in discover([base], words)], [
            (base, base + 4), (base + 4, base + 12)])

    def test_recompiler_discovers_unsupported_branch_successors(self) -> None:
        base = 0x9000
        words = {
            base: i_type(0x14, 2, 3, 2), # BEQL, backend deoptimizes
            base + 4: 0,                 # delay slot; never an entry
            base + 8: i_type(0x09, 0, 2, 7),
            base + 12: 0x0000000D,       # taken target
        }
        self.assertTrue(has_delay_slot(words[base]))
        blocks = discover([base], words)
        self.assertEqual([b.start for b in blocks], [
            base, base + 8, base + 12])
        self.assertEqual(blocks[0].end, base + 8)
        self.assertEqual(len(discover([base], words, max_leaders=2)), 2)
        with self.assertRaisesRegex(ValueError, "at least one"):
            discover([base], words, max_leaders=0)

    def test_scheduler_splits_before_branch_not_in_its_delay_slot(self) -> None:
        base = 0x1000
        words = {base + 4 * index: 0 for index in range(12)}
        # A branch at the eighth instruction must carry its delay slot into
        # the next resume block instead of being cut after the branch.
        words[base + 7 * 4] = i_type(0x04, 2, 3, 1)
        blocks = split_for_event_horizon(
            [Function(base, base + 9 * 4)], words, 8
        )
        self.assertEqual([(b.start, b.end, b.direct_fallthrough)
                          for b in blocks], [
            (base, base + 7 * 4, True),
            (base + 7 * 4, base + 9 * 4, False),
        ])
        self.assertTrue(all((b.end - b.start) // 4 <= 8 for b in blocks))
        with self.assertRaisesRegex(ValueError, "at least 2"):
            split_for_event_horizon([Function(base, base + 8)], words, 1)

    def test_memory_delay_slot_is_a_trace_barrier(self) -> None:
        elf = build_memory_elf(0xA000, (
            0x08002804,  # j 0xA010
            0x8C020000,  # lw v0, 0(zero) in the delay slot
            0x00000000,
            0x00000000,
            0x0000000D,
        ))
        fingerprint = source_fingerprint((elf,))
        census = {
            "schema": "astrarecomp.execution-census",
            "version": 3,
            "source": {
                "kind": "elf-set",
                "fingerprint_scheme": FINGERPRINT_SCHEME,
                "fingerprint_sha256": fingerprint,
            },
            "ee": {
                "blocks": [
                    {"pc": "0x0000A000", "entries": 100},
                    {"pc": "0x0000A010", "entries": 100},
                ],
                "edges": [{
                    "source": "0x0000A000", "target": "0x0000A010",
                    "transitions": 100,
                }],
            },
        }
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            elf_path = root / "delay-memory.elf"
            census_path = root / "census.json"
            elf_path.write_bytes(elf)
            census_path.write_text(json.dumps(census), encoding="utf-8")
            traces, _ = select_direct_traces([elf_path], census_path)
        self.assertEqual(traces[0].blocks, (0xA000,))


if __name__ == "__main__":
    unittest.main()
