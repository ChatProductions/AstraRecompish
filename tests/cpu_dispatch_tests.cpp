#include "ps2vita/cpu.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

namespace {
unsigned checks = 0u, failures = 0u;
void check(bool condition, const char* label) {
  ++checks;
  if (!condition) { ++failures; std::fprintf(stderr, "FAIL: %s\n", label); }
}
bool equal(const ps2vita::CpuState& a, const ps2vita::CpuState& b) {
  return a.gpr == b.gpr && a.gpr_hi == b.gpr_hi && a.cop0 == b.cop0 &&
      a.fpr == b.fpr && a.fcr == b.fcr && a.fpu_acc == b.fpu_acc &&
      a.vu0_vf == b.vu0_vf && a.vu0_vf_hi == b.vu0_vf_hi &&
      a.vu0_vi == b.vu0_vi && a.vu0_acc == b.vu0_acc &&
      a.hi == b.hi && a.lo == b.lo && a.hi1 == b.hi1 && a.lo1 == b.lo1 &&
      a.shift_amount == b.shift_amount && a.pc == b.pc && a.cycles == b.cycles;
}
ps2vita::StopReason steps(ps2vita::Cpu& cpu, unsigned budget) {
  for (unsigned i = 0u; i < budget; ++i) {
    const auto reason = cpu.step();
    if (reason != ps2vita::StopReason::None) return reason;
  }
  return ps2vita::StopReason::StepLimit;
}

void classifier() {
  auto m = std::make_unique<ps2vita::Memory>();
  check(m->instruction_reads_side_effect_free(0x80001000u, 28u), "KSEG0 RAM span");
  check(m->instruction_reads_side_effect_free(0xA0001FF0u, 28u), "KSEG1 contiguous page crossing");
  check(m->instruction_reads_side_effect_free(0x1000u, 28u), "unmapped low RAM span");
  check(m->instruction_reads_side_effect_free(0x70000000u, 28u), "direct scratch span");
  check(!m->instruction_reads_side_effect_free(0x70003FF0u, 28u), "scratch limit crossing");
  check(!m->instruction_reads_side_effect_free(0xA1FFFFF0u, 28u), "RAM limit crossing");
  check(!m->instruction_reads_side_effect_free(0xFFFFFFF0u, 28u), "virtual wrap rejected");
  check(!m->instruction_reads_side_effect_free(0x80000000u, 0u), "empty span rejected");
  check(!m->instruction_reads_side_effect_free(0x80000000u, 4097u), "large multi-page span rejected");
  check(!m->instruction_reads_side_effect_free(0xBFC00000u, 28u), "unloaded ROM rejected");
  std::vector<std::uint8_t> bios(ps2vita::Memory::kBiosSize, 0u);
  check(m->load_bios(bios.data(), bios.size()), "owned ROM loaded");
  check(m->instruction_reads_side_effect_free(0xBFC00000u, 28u), "loaded ROM span");
  check(!m->instruction_reads_side_effect_free(0xBFFFFFF0u, 28u), "ROM limit crossing");
  for (const auto address : {0x10000000u, 0x1000F440u, 0x12000000u, 0xBA000000u,
                            0x1F402000u, 0x1F801000u, 0xFFFE0000u, 0x1FA00000u})
    check(!m->instruction_reads_side_effect_free(address, 28u), "MMIO span rejected without reads");
  constexpr std::uint32_t alias = 0x00400000u;
  // Paired 4 KiB pages, first ordinary RAM and second hardware.
  m->write_tlb(0u, 0u, alias, (2u << 6) | 2u, (0x10000u << 6) | 2u);
  check(m->instruction_reads_side_effect_free(alias + 0x100u, 28u), "TLB RAM span");
  check(!m->instruction_reads_side_effect_free(alias + 0x1000u, 28u), "TLB MMIO span");
  check(!m->instruction_reads_side_effect_free(alias + 0xFF0u, 28u), "TLB RAM to MMIO boundary");
  m->write_tlb(0u, 0u, alias, (2u << 6) | 2u, (9u << 6) | 2u);
  check(!m->instruction_reads_side_effect_free(alias + 0xFF0u, 28u), "noncontiguous TLB pages");
  m->write_tlb(0u, 0u, alias, (2u << 6) | 2u, (3u << 6) | 2u);
  check(m->instruction_reads_side_effect_free(alias + 0xFF0u, 28u), "contiguous TLB pages");
}

void dispatch() {
  for (const unsigned quantum : {1u, 2u, 7u, 8u, 14u, 31u}) {
    auto a = std::make_unique<ps2vita::Memory>();
    auto b = std::make_unique<ps2vita::Memory>();
    const std::array<std::uint32_t, 8> program{{
        0x24080007u, 0x25080001u, 0x01084821u, 0xAC090000u,
        0x8C0A0000u, 0x1500FFFBu, 0u, 0x0000000Du}};
    for (unsigned i = 0; i < program.size(); ++i) {
      a->write32(0x1000u + i * 4u, program[i]);
      b->write32(0x1000u + i * 4u, program[i]);
    }
    ps2vita::Cpu fast(*a), oracle(*b);
    fast.reset(0x80001000u); oracle.reset(0x80001000u);
    for (unsigned slice = 0; slice < 12u; ++slice) {
      check(fast.run(quantum) == steps(oracle, quantum), "ordinary stop reason matches step");
      check(equal(fast.state(), oracle.state()), "ordinary architectural state matches step");
      check(a->read32(0u) == b->read32(0u), "ordinary stores match step");
    }
  }
}

void zero_fill_and_halt() {
  const std::array<std::uint32_t, 8> program{{
      (0x1Fu << 26) | (16u << 21) | (2u << 16),
      0x26100010u, (16u << 21) | (4u << 16) | (2u << 11) | 0x2Bu,
      0u, 0u, 0x1440FFFAu, (0x1Cu << 26) | (2u << 11) | (0x12u << 6) | 0x29u,
      0x0000000Du}};
  for (const unsigned quantum : {1u, 6u, 7u, 8u, 14u, 35u, 113u}) {
    auto a = std::make_unique<ps2vita::Memory>();
    auto b = std::make_unique<ps2vita::Memory>();
    for (unsigned i = 0; i < program.size(); ++i) {
      a->write32(0x1FF0u + i * 4u, program[i]);
      b->write32(0x1FF0u + i * 4u, program[i]);
    }
    for (unsigned i = 0x4000u; i < 0x4100u; i += 4u) {
      a->write32(i, 0x89ABCDEFu); b->write32(i, 0x89ABCDEFu);
    }
    ps2vita::Cpu fast(*a), oracle(*b);
    fast.reset(0x80001FF0u); oracle.reset(0x80001FF0u);
    fast.state().gpr[16] = oracle.state().gpr[16] = 0x4000u;
    fast.state().gpr[4] = oracle.state().gpr[4] = 0x4100u;
    for (unsigned slice = 0; slice < 120u; ++slice) {
      const auto result = fast.run(quantum);
      check(result == steps(oracle, quantum), "zero-fill stop reason matches step");
      check(equal(fast.state(), oracle.state()), "zero-fill architectural state matches step");
      bool same_ram = true;
      for (unsigned i = 0x4000u; i < 0x4100u; i += 4u)
        same_ram = same_ram && a->read32(i) == b->read32(i);
      check(same_ram, "zero-fill partial RAM matches step");
      if (result != ps2vita::StopReason::StepLimit) break;
    }
    fast.reset(0x80001FF0u);
    fast.state().gpr[16] = 0x4000u; fast.state().gpr[4] = 0x4100u;
    a->write32(0x4000u, 0x89ABCDEFu);
    fast.request_halt();
    check(fast.run(14u) == ps2vita::StopReason::Halted && fast.state().cycles == 0u &&
          a->read32(0x4000u) == 0x89ABCDEFu, "halt precedes native zero-fill stores");
  }
}

void mmio_fetch() {
  for (const unsigned quantum : {0u, 1u, 7u, 8u}) {
    auto a = std::make_unique<ps2vita::Memory>();
    auto b = std::make_unique<ps2vita::Memory>();
    a->write32(0x1000F430u, 0x80210000u);
    b->write32(0x1000F430u, 0x80210000u);
    ps2vita::Cpu fast(*a), oracle(*b);
    fast.reset(0xB000F440u); oracle.reset(0xB000F440u);
    check(fast.run(quantum) == steps(oracle, quantum), "MMIO fetch stop reason matches step");
    check(equal(fast.state(), oracle.state()), "MMIO fetched instruction matches step");
    for (unsigned i = 0u; i < 3u; ++i)
      check(a->read32(0x1000F440u) == b->read32(0x1000F440u),
            "MMIO has no decoder/pattern speculative response reads");
  }
}
} // namespace

int main() {
  classifier(); dispatch(); zero_fill_and_halt(); mmio_fetch();
  std::printf("cpu_dispatch checks=%u failures=%u\n", checks, failures);
  return failures != 0u;
}
