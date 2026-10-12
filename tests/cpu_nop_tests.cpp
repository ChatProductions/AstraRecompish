#include "ps2vita/cpu.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <memory>
#include <vector>

namespace {

constexpr std::uint32_t kPc = 0x80001000u;
unsigned checks = 0u, failures = 0u;

void check(bool condition, const char* description) {
  ++checks;
  if (!condition) {
    ++failures;
    std::fprintf(stderr, "FAIL: %s\n", description);
  }
}

bool equal(const ps2vita::CpuState& actual, const ps2vita::CpuState& expected) {
  return actual.gpr == expected.gpr && actual.gpr_hi == expected.gpr_hi &&
      actual.cop0 == expected.cop0 && actual.fpr == expected.fpr &&
      actual.fcr == expected.fcr && actual.fpu_acc == expected.fpu_acc &&
      actual.vu0_vf == expected.vu0_vf && actual.vu0_vf_hi == expected.vu0_vf_hi &&
      actual.vu0_vi == expected.vu0_vi && actual.vu0_acc == expected.vu0_acc &&
      actual.hi == expected.hi && actual.lo == expected.lo &&
      actual.hi1 == expected.hi1 && actual.lo1 == expected.lo1 &&
      actual.shift_amount == expected.shift_amount && actual.pc == expected.pc &&
      actual.cycles == expected.cycles &&
      actual.fast_path_instructions == expected.fast_path_instructions &&
      actual.aot_trace_entries == expected.aot_trace_entries &&
      actual.aot_trace_horizon_fallbacks == expected.aot_trace_horizon_fallbacks;
}

ps2vita::CpuState seeded_state() {
  ps2vita::CpuState state;
  for (unsigned index = 0u; index < 32u; ++index) {
    state.gpr[index] = 0x0123456789ABCDEFull + index * 0x0101010101010101ull;
    state.gpr_hi[index] = 0xFEDCBA9876543210ull - index * 0x0202020202020202ull;
    state.cop0[index] = 0xA5C30000u ^ (index * 0x01030709u);
    state.fpr[index] = 0x3F000001u + index * 0x010101u;
    state.fcr[index] = 0x13572468u ^ (index * 0x12345u);
    state.vu0_vf[index] = 0x3F80000140000002ull + index * 0x0001000100010001ull;
    state.vu0_vf_hi[index] = 0x4040000340800004ull + index * 0x0002000200020002ull;
    state.vu0_vi[index] = 0x24681357u ^ (index * 0x10203u);
  }
  for (unsigned lane = 0u; lane < 4u; ++lane)
    state.vu0_acc[lane] = 0x41000011u + lane * 0x010101u;
  state.fpu_acc = 0x42A00003u;
  state.hi = 0x1122334455667788ull;
  state.lo = 0x8877665544332211ull;
  state.hi1 = 0x13579BDF2468ACE0ull;
  state.lo1 = 0x02468ACE13579BDFull;
  state.shift_amount = 0x87654321u;
  state.pc = kPc;
  state.cycles = 0x1020304050607080ull;
  state.fast_path_instructions = 0x12345678u;
  state.aot_trace_entries = 0x23456789u;
  state.aot_trace_horizon_fallbacks = 0x3456789Au;
  state.cop0[9] = std::numeric_limits<std::uint32_t>::max();
  // Both zero-register halves are intentionally dirty: normalization belongs
  // to Cpu::step(), not the SPECIAL/SLL decoder or a NOP-specific helper.
  return state;
}

struct Fixture {
  std::unique_ptr<ps2vita::Memory> memory = std::make_unique<ps2vita::Memory>();
  ps2vita::Cpu cpu{*memory};

  Fixture() {
    cpu.reset(kPc);
    cpu.state() = seeded_state();
    memory->write32(kPc, 0u);
  }
};

// Explicit expected architectural retirement; no second Cpu or instruction
// decoder is used as an oracle. Device expectations are checked separately.
void retire_expected(ps2vita::CpuState& state, std::uint32_t next_pc,
                     std::uint32_t interrupt_lines = 0u) {
  state.gpr[0] = 0u;
  state.gpr_hi[0] = 0u;
  state.cop0[13] = (state.cop0[13] & ~0xC00u) | interrupt_lines;
  ++state.cycles;
  ++state.cop0[9];
  state.pc = next_pc;
}

void test_seeded_nop() {
  Fixture fixture;
  fixture.memory->write32(0x3000u, 0x89ABCDEFu);
  const auto generation = fixture.memory->page_generation(0x3000u);
  auto expected = fixture.cpu.state();
  retire_expected(expected, kPc + 4u);

  check(fixture.cpu.step() == ps2vita::StopReason::None,
        "zero encoding retires as an ordinary running instruction");
  check(equal(fixture.cpu.state(), expected),
        "NOP preserves seeded scalar, packed, FPU, VU, COP and diagnostic state");
  check(fixture.cpu.state().gpr[0] == 0u && fixture.cpu.state().gpr_hi[0] == 0u,
        "NOP still normalizes both dirty zero-register halves");
  check(fixture.cpu.state().cop0[9] == 0u,
        "NOP increments and wraps the 32-bit COP0 Count register");
  check(fixture.cpu.stop_reason() == ps2vita::StopReason::None &&
            fixture.cpu.fault_instruction() == 0u && fixture.cpu.fault_address() == 0u,
        "NOP does not create a fault or a semantic fast-path diagnostic");
  check(fixture.memory->read32(0x3000u) == 0x89ABCDEFu &&
            fixture.memory->page_generation(0x3000u) == generation,
        "NOP does not write unrelated ordinary RAM");
  retire_expected(expected, kPc + 8u);
  check(fixture.cpu.step() == ps2vita::StopReason::None &&
            equal(fixture.cpu.state(), expected),
        "consecutive NOP continues through the ordinary retirement path");
}

void test_nop_delay_slot_irq() {
  Fixture fixture;
  fixture.memory->write32(kPc, 0x10000003u); // BEQ zero,zero,kPc+16.
  fixture.memory->write32(kPc + 4u, 0u);    // NOP delay slot.
  fixture.memory->write32(kPc + 16u, 0x24090009u); // Must not execute before IRQ.
  fixture.memory->write32(0x80000200u, 0x240A0001u); // Installed INTC vector.
  fixture.cpu.set_exception_mode(true);
  fixture.cpu.state().cop0[12] = 0x00010401u; // EIE, INTC mask, IE; no EXL/ERL.
  auto expected = fixture.cpu.state();
  retire_expected(expected, kPc + 4u);
  check(fixture.cpu.step() == ps2vita::StopReason::None &&
            equal(fixture.cpu.state(), expected),
        "taken branch reaches its NOP delay slot with exact expected state");

  // The IOP SBUS control raises EE INTC bit 1 without consuming guest cycles.
  fixture.memory->write32(0x1000F010u, 1u << 1u);
  fixture.memory->iop_write32(0x1F801450u, 2u);
  check(fixture.memory->ee_interrupt_lines() == 0x400u,
        "owned SBUS fixture raises an enabled interrupt during the NOP delay slot");
  retire_expected(expected, kPc + 16u, 0x400u);
  check(fixture.cpu.step() == ps2vita::StopReason::None &&
            equal(fixture.cpu.state(), expected),
        "NOP retires the pending taken branch despite its enabled interrupt");

  // The next step takes the interrupt without executing the target ADDIU.
  expected.cop0[13] = (expected.cop0[13] & ~0x8000007Cu) | 0x400u;
  expected.cop0[14] = kPc + 16u;
  expected.cop0[12] |= 2u;
  expected.pc = 0x80000200u;
  ++expected.cycles;
  ++expected.cop0[9];
  check(fixture.cpu.step() == ps2vita::StopReason::None &&
            equal(fixture.cpu.state(), expected),
        "following step takes the interrupt at the branch target without executing it");
  check((fixture.cpu.state().cop0[13] & 0x80000000u) == 0u &&
            fixture.cpu.state().cop0[14] == kPc + 16u,
        "deferred asynchronous interrupt records target EPC without a delay-slot flag");
}

void test_nonzero_sll() {
  struct Case {
    unsigned source, destination, shift;
    std::uint64_t input, result;
  };
  constexpr std::array<Case, 4> cases{{
      {5u, 6u, 0u, 0x0123456789ABCDEFull, 0xFFFFFFFF89ABCDEFull},
      {5u, 6u, 31u, 0xFEDCBA9800000001ull, 0xFFFFFFFF80000000ull},
      {5u, 0u, 7u, 0x0123456789ABCDEFull, 0u},
      {0u, 0u, 31u, 0u, 0u}}};
  for (const auto& item : cases) {
    Fixture fixture;
    const auto instruction = (item.source << 16u) |
        (item.destination << 11u) | (item.shift << 6u);
    check(instruction != 0u, "SLL fixture encoding is distinct from canonical NOP");
    fixture.memory->write32(kPc, instruction);
    fixture.cpu.state().gpr[item.source] = item.input;
    auto expected = fixture.cpu.state();
    if (item.destination != 0u) expected.gpr[item.destination] = item.result;
    retire_expected(expected, kPc + 4u);
    check(fixture.cpu.step() == ps2vita::StopReason::None &&
              equal(fixture.cpu.state(), expected),
          "nonzero SLL retains exact low-word/sign-extension or discarded-destination behavior");
  }
}

void test_halt_before_nop() {
  Fixture fixture;
  fixture.memory->advance(7u);
  const auto expected = fixture.cpu.state();
  const auto event_distance = fixture.memory->cycles_until_next_event();
  fixture.cpu.request_halt();
  check(fixture.cpu.step() == ps2vita::StopReason::Halted,
        "requested halt wins before NOP fetch or execution");
  check(equal(fixture.cpu.state(), expected),
        "halt does not normalize zero, update Cause or consume Count/cycles");
  check(fixture.memory->iop_read32(0x1F8014A0u) == 0u &&
            fixture.memory->cycles_until_next_event() == event_distance,
        "halt before NOP does not cross the pending IOP timer edge");
  check(fixture.cpu.fault_instruction() == 0u && fixture.cpu.fault_address() == 0u,
        "requested halt does not introduce fetch-fault diagnostics");
}

void test_fetch_faults() {
  for (const auto address : {kPc + 1u, 0x13000000u}) {
    Fixture fixture;
    fixture.cpu.state().pc = address;
    fixture.memory->advance(7u);
    auto expected = fixture.cpu.state();
    expected.cop0[13] &= ~0xC00u; // Interrupt polling precedes fetch validation.
    const auto event_distance = fixture.memory->cycles_until_next_event();
    check(fixture.cpu.step() == ps2vita::StopReason::MemoryFault,
          "unaligned or unmapped fetch faults before considering NOP");
    check(equal(fixture.cpu.state(), expected),
          "fetch fault preserves dirty zero and consumes no Count/cycles");
    check(fixture.cpu.fault_address() == address && fixture.cpu.fault_instruction() == 0u,
          "fetch fault records its exact address without inventing an instruction");
    check(fixture.memory->iop_read32(0x1F8014A0u) == 0u &&
              fixture.memory->cycles_until_next_event() == event_distance,
          "fetch fault does not advance the pending device edge");
  }
}

void test_diagnostic_retention() {
  Fixture fixture;
  fixture.memory->write32(kPc, 1u); // SPECIAL/function 1 is unsupported.
  check(fixture.cpu.step() == ps2vita::StopReason::InvalidInstruction &&
            fixture.cpu.fault_instruction() == 1u,
        "nonzero unsupported instruction is not accidentally accepted as NOP");
  auto expected = fixture.cpu.state();
  fixture.memory->write32(kPc, 0u);
  retire_expected(expected, kPc + 4u);
  check(fixture.cpu.step() == ps2vita::StopReason::None &&
            equal(fixture.cpu.state(), expected),
        "NOP resumes at the retained PC after an unsupported instruction");
  check(fixture.cpu.fault_instruction() == 1u && fixture.cpu.fault_address() == 0u,
        "NOP preserves prior diagnostic fields instead of clearing them");
}

void test_timer_progression() {
  constexpr std::array<std::uint32_t, 4> divisors{{1u, 8u, 16u, 256u}};
  for (unsigned selector = 0u; selector < divisors.size(); ++selector) {
    Fixture fixture;
    fixture.memory->iop_write32(0x1F8014A8u, 2u);
    fixture.memory->iop_write32(0x1F8014A4u, (selector << 13u) | 0x10u);
    const auto period = divisors[selector] * 8u;
    fixture.memory->advance(period - 1u); // Prepare exactly one cycle before a tick.
    auto expected = fixture.cpu.state();
    check(fixture.memory->iop_read32(0x1F8014A0u) == 0u,
          "timer remains before its selected prescaler tick before NOP");
    retire_expected(expected, kPc + 4u);
    check(fixture.cpu.step() == ps2vita::StopReason::None &&
              equal(fixture.cpu.state(), expected),
          "NOP consumes precisely its own CPU cycle at the timer tick");
    check(fixture.memory->iop_read32(0x1F8014A0u) == 1u &&
              (fixture.memory->iop_read32(0x1F801070u) & (1u << 16u)) == 0u,
          "NOP advances the normal timer model by one cycle without an early target IRQ");

    fixture.memory->advance(period - 1u);
    retire_expected(expected, kPc + 8u);
    check(fixture.cpu.step() == ps2vita::StopReason::None &&
              equal(fixture.cpu.state(), expected),
          "second NOP retains exact Count/PC despite intervening device-only phase setup");
    check(fixture.memory->iop_read32(0x1F8014A0u) == 2u &&
              (fixture.memory->iop_read32(0x1F801070u) & (1u << 16u)) != 0u &&
              (fixture.memory->iop_read32(0x1F8014A4u) & 0x800u) != 0u,
          "NOP advances Timer 5 through its precise target/interrupt boundary");
  }
}

void test_dma_progression() {
  Fixture fixture;
  constexpr std::array<std::uint8_t, 16> payload{{
      1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u,
      9u, 10u, 11u, 12u, 13u, 14u, 15u, 16u}};
  check(fixture.memory->copy_in(0x2000u, payload.data(), payload.size()),
        "owned GIF DMA source is ordinary RAM");
  fixture.memory->write32(0x1000A010u, 0x2000u);
  fixture.memory->write32(0x1000A020u, 1u);
  fixture.memory->write32(0x1000A000u, 0x100u);
  auto expected = fixture.cpu.state();
  std::vector<std::uint8_t> observed;
  // First NOP discovers the arm; eight further NOPs retire its 8-cycle transfer.
  // This is the existing functional DMA model, not a hardware latency claim.
  for (unsigned cycle = 1u; cycle <= 9u; ++cycle) {
    retire_expected(expected, kPc + cycle * 4u);
    check(fixture.cpu.step() == ps2vita::StopReason::None &&
              equal(fixture.cpu.state(), expected),
          "NOP never skips a cycle while normal DMA discovery/retirement advances");
    const bool completed = fixture.memory->pop_gif_packet(observed);
    check(completed == (cycle == 9u),
          "GIF DMA payload appears only at the existing arm-plus-transfer boundary");
  }
  check(observed == std::vector<std::uint8_t>(payload.begin(), payload.end()),
        "NOP-driven DMA preserves every owned payload byte");
  check(fixture.memory->read32(0x1000A010u) == 0x2010u &&
            fixture.memory->read32(0x1000A020u) == 0u &&
            (fixture.memory->read32(0x1000A000u) & 0x100u) == 0u &&
            (fixture.memory->read32(0x1000E010u) & (1u << 2u)) != 0u,
        "NOP-driven DMA publishes exact completion address/count/status");
  check(!fixture.memory->pop_gif_packet(observed),
        "NOP-driven DMA produces exactly one completed packet");
}

} // namespace

int main() {
  test_seeded_nop();
  test_nop_delay_slot_irq();
  test_nonzero_sll();
  test_halt_before_nop();
  test_fetch_faults();
  test_diagnostic_retention();
  test_timer_progression();
  test_dma_progression();
  std::printf("cpu_nop checks=%u failures=%u\n", checks, failures);
  return failures == 0u ? 0 : 1;
}
