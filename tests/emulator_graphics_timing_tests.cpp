#include "ps2vita/emulator.hpp"
#include "ps2vita/execution_budget.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

unsigned checks = 0;
unsigned failures = 0;

void check(bool condition, bool chain, std::uint32_t phase,
           std::uint32_t slice, const char* name) {
  ++checks;
  if (!condition) {
    ++failures;
    std::fprintf(stderr, "FAIL [%s phase=%u slice=%u]: %s\n",
                 chain ? "chain" : "normal", phase, slice, name);
  }
}

constexpr std::uint32_t kEntry = 0x1000u;
constexpr std::uint32_t kTag = 0x2FF0u;
constexpr std::uint32_t kPayload = 0x3000u;
constexpr std::uint32_t kOutput = 0x4000u;
constexpr std::uint32_t kVuOffset = 0x1230u;
constexpr std::array<std::uint32_t, 8> kUnpack{{
    0u, 0x01000101u, 0u, 0x6C010123u,
    0x3F800000u, 0xBF000000u, 0x00000001u, 0xDEADBEEFu}};

std::array<std::uint32_t, 20> make_program(bool chain) {
  // The guest, not this host test, enables DMA, programs its source/count,
  // kicks VIF1, polls STR, reads VU1 memory and stores the observed value.
  return {{
      0x3C081001u, // lui t0, 0x1001: signed SW offsets address EE hardware
      0x34090001u, // ori t1, zero, DMAE
      0xAD09E000u, // sw t1, D_CTRL(t0)
      0x34093000u, // ori t1, zero, payload
      0xAD099010u, // sw t1, D1_MADR(t0)
      0x34090002u, // ori t1, zero, QWC=2
      0xAD099020u, // sw t1, D1_QWC(t0)
      0x34092FF0u, // ori t1, zero, tag
      0xAD099030u, // sw t1, D1_TADR(t0)
      chain ? 0x34090105u : 0x34090101u, // source chain or normal, DIR=1, STR=1
      0xAD099000u, // sw t1, D1_CHCR(t0)
      0x8D099000u, // poll: lw t1, D1_CHCR(t0)
      0x31290100u, // andi t1, t1, STR
      0x1520FFFDu, // bne t1, zero, poll
      0u,         // branch delay slot
      0x3C0A1100u, // lui t2, 0x1100
      0x354AC000u, // ori t2, t2, VU1 data bank
      0x8D4B1230u, // lw t3, uploaded VU1 qword's X lane
      0xAC0B4000u, // sw t3, result(zero), before host graphics service can hide it
      0x0000000Du  // break: bounded host-test completion, not a PS2 exit service
  }};
}

std::vector<std::uint8_t> make_elf(bool chain, std::uint32_t phase) {
  const auto program = make_program(chain);
  constexpr std::size_t segment_size = kOutput + 16u - kEntry;
  std::vector<std::uint8_t> elf(0x100u + segment_size);
  const auto put = [&](std::size_t offset, std::uint32_t value, unsigned bytes) {
    for (unsigned byte = 0; byte < bytes; ++byte)
      elf[offset + byte] = static_cast<std::uint8_t>(value >> (byte * 8u));
  };
  put(0u, 0x464C457Fu, 4u); put(4u, 0x010101u, 3u);
  put(16u, 2u, 2u); put(18u, 8u, 2u); put(20u, 1u, 4u);
  put(24u, kEntry, 4u); put(28u, 52u, 4u);
  put(40u, 52u, 2u); put(42u, 32u, 2u); put(44u, 1u, 2u);
  put(52u, 1u, 4u); put(56u, 0x100u, 4u);
  put(60u, kEntry, 4u); put(64u, kEntry, 4u);
  put(68u, static_cast<std::uint32_t>(segment_size), 4u);
  put(72u, static_cast<std::uint32_t>(segment_size), 4u);
  put(76u, 7u, 4u); put(80u, 16u, 4u);
  for (std::size_t word = 0; word < program.size(); ++word)
    put(0x100u + (phase + word) * 4u, program[word], 4u);
  // END tag: two inline qwords. Normal mode must ignore these tag bytes.
  put(0x100u + kTag - kEntry, 0x70000002u, 4u);
  for (std::size_t word = 0; word < kUnpack.size(); ++word)
    put(0x100u + kPayload - kEntry + word * 4u, kUnpack[word], 4u);
  return elf;
}

struct Result {
  ps2vita::CpuState cpu{};
  std::uint32_t output = 0u;
};

bool same_ee_architecture(const ps2vita::CpuState& lhs,
                          const ps2vita::CpuState& rhs) {
  // Compare fields explicitly, not structure padding or host AOT telemetry.
  return lhs.gpr == rhs.gpr && lhs.gpr_hi == rhs.gpr_hi &&
      lhs.cop0 == rhs.cop0 && lhs.fpr == rhs.fpr && lhs.fcr == rhs.fcr &&
      lhs.fpu_acc == rhs.fpu_acc && lhs.vu0_vf == rhs.vu0_vf &&
      lhs.vu0_vf_hi == rhs.vu0_vf_hi && lhs.vu0_vi == rhs.vu0_vi &&
      lhs.vu0_acc == rhs.vu0_acc && lhs.hi == rhs.hi && lhs.lo == rhs.lo &&
      lhs.hi1 == rhs.hi1 && lhs.lo1 == rhs.lo1 &&
      lhs.shift_amount == rhs.shift_amount && lhs.pc == rhs.pc &&
      lhs.cycles == rhs.cycles;
}

Result run_fixture(bool chain, std::uint32_t phase, std::uint32_t slice,
                   bool paced = false) {
  ps2vita::Emulator emulator;
  const auto elf = make_elf(chain, phase);
  const auto loaded = emulator.load_elf(elf.data(), elf.size());
  check(loaded.ok && loaded.entry == kEntry && loaded.segments == 1u,
        chain, phase, slice, "owned EE ELF loads");
  auto& memory = emulator.memory();
  memory.write32(kOutput, 0xA55AA55Au);
  for (unsigned lane = 0; lane < 4u; ++lane)
    memory.vu1_store_data_word(kVuOffset + lane * 4u, 0x13579BDFu);
  check(emulator.run_slice(0u) == ps2vita::StopReason::StepLimit &&
            emulator.cpu().state().cycles == 0u &&
            memory.read32(kOutput) == 0xA55AA55Au,
        chain, phase, slice, "zero budget does not execute guest instructions");
  auto reason = ps2vita::StopReason::StepLimit;
  constexpr std::uint32_t limit = 4096u;
  while (reason == ps2vita::StopReason::StepLimit &&
         emulator.cpu().state().cycles < limit) {
    const auto before = emulator.cpu().state().cycles;
    const auto remaining = limit - static_cast<std::uint32_t>(before);
    if (!paced) {
      reason = emulator.run_slice(std::min(slice, remaining));
    } else {
      std::uint64_t clock = 0u;
      const auto budget = ps2vita::run_execution_budget({2u, slice, 4u},
          [&]() { return clock; }, [&](std::uint32_t quantum) {
            const auto left = limit - static_cast<std::uint32_t>(
                emulator.cpu().state().cycles);
            reason = emulator.run_slice(std::min(quantum, left));
            ++clock;
            return reason == ps2vita::StopReason::StepLimit &&
                emulator.cpu().state().cycles < limit;
          });
      check(budget.quanta > 0u && budget.quanta <= 2u &&
                budget.stop == (reason == ps2vita::StopReason::StepLimit ?
                    ps2vita::ExecutionBudgetStop::Elapsed :
                    ps2vita::ExecutionBudgetStop::CallbackStopped),
            chain, phase, slice, "paced ELF obeys its soft budget and stops at guest BREAK");
    }
    if (emulator.cpu().state().cycles == before) break;
  }
  const auto& cpu = emulator.cpu().state();
  check(reason == ps2vita::StopReason::Break && cpu.cycles < limit,
        chain, phase, slice, "STR poll reaches its bounded guest completion");
  // The existing transport model discovers the kick on its SW cycle and
  // completes its two qwords 16 modeled cycles later. The polling loop then
  // finishes at cycle 36 plus the entry NOP phase. This is a model oracle,
  // not measured PS2 timing.
  check(cpu.cycles == 36u + phase && cpu.pc == kEntry + (19u + phase) * 4u &&
            cpu.cop0[9] == 36u + phase && cpu.fast_path_instructions == 0u,
        chain, phase, slice, "guest cycle/PC/count result is exact without a fast fill");
  check(cpu.gpr[8] == 0x10010000u && cpu.gpr[9] == 0u &&
            cpu.gpr[10] == ps2vita::Memory::kVu1DataBase &&
            cpu.gpr[11] == kUnpack[4] && cpu.gpr[0] == 0u,
        chain, phase, slice, "guest registers contain the uploaded word after STR clears");
  check(memory.read32(kOutput) == kUnpack[4], chain, phase, slice,
        "guest saved the uploaded value before stopping, not the stale poison");
  bool all_lanes = true;
  for (unsigned lane = 0; lane < 4u; ++lane)
    all_lanes = all_lanes &&
        memory.vu1_data_word(kVuOffset + lane * 4u) == kUnpack[4u + lane];
  check(all_lanes, chain, phase, slice, "UNPACK preserves all four raw lanes");
  check(emulator.vif1().packets_submitted() == 1u &&
            emulator.vif1().packets_rejected() == 0u &&
            emulator.vif1().vectors_unpacked() == 1u &&
            emulator.vif1().vu1().pairs_executed() == 0u,
        chain, phase, slice, "one completed DMA is serviced exactly once without running VU");
  check(memory.read32(0x10009010u) == kPayload + 32u &&
            memory.read32(0x10009020u) == 0u &&
            (memory.read32(0x10009000u) & 0x100u) == 0u &&
            (memory.read32(0x1000E010u) & 2u) != 0u &&
            memory.read32(0x10009030u) == (chain ? kPayload + 32u : kTag),
        chain, phase, slice, "DMA register completion agrees with the consumed source");
  std::vector<std::uint8_t> packet;
  check(!memory.pop_vif1_packet(packet) && !memory.pop_gif_packet(packet) &&
            emulator.gif().pending_bytes() == 0u,
        chain, phase, slice, "no completed transport or GIF payload remains queued");
  return {cpu, memory.read32(kOutput)};
}

struct BiosResult {
  Result ee{};
  ps2vita::IopState iop{};
};

BiosResult run_bios_fixture(bool chain, std::uint32_t phase,
                            std::uint32_t slice) {
  ps2vita::Emulator emulator;
  // An owned, minimal reset ROM tests the BIOS scheduling route. It does not
  // contain Sony firmware and does not claim accuracy of the original intro.
  std::vector<std::uint8_t> rom(ps2vita::Memory::kBiosSize);
  auto program = make_program(chain);
  program.back() = 0x1000FFFFu; // beq zero, zero, self; next ROM word is NOP
  const auto put = [&](std::size_t offset, std::uint32_t value) {
    for (unsigned byte = 0; byte < 4u; ++byte)
      rom[offset + byte] = static_cast<std::uint8_t>(value >> (byte * 8u));
  };
  for (std::size_t word = 0; word < program.size(); ++word)
    put((phase + word) * 4u, program[word]);
  put(0x1000u, 0x1000FFFFu); // Separate harmless IOP self-branch/NOP loop.
  check(emulator.load_bios(rom.data(), rom.size()) && emulator.boot_bios(),
        chain, phase, slice, "owned reset ROM boots the BIOS scheduler route");
  // Reset clears hidden branch/load state before selecting the isolated IOP
  // entry. It never runs the EE DMA program as IOP code.
  emulator.iop().reset();
  emulator.iop().state().pc = 0xBFC01000u;
  auto& memory = emulator.memory();
  // ROM loading does not initialize RAM. These are fixture input bytes, not
  // host submissions: only the EE's stores above can initiate their transfer.
  memory.write32(kTag, 0x70000002u);
  for (std::size_t word = 0; word < kUnpack.size(); ++word)
    memory.write32(kPayload + static_cast<std::uint32_t>(word * 4u), kUnpack[word]);
  memory.write32(kOutput, 0xA55AA55Au);
  for (unsigned lane = 0; lane < 4u; ++lane)
    memory.vu1_store_data_word(kVuOffset + lane * 4u, 0x13579BDFu);
  auto reason = ps2vita::StopReason::StepLimit;
  constexpr std::uint32_t limit = 64u;
  while (reason == ps2vita::StopReason::StepLimit &&
         emulator.cpu().state().cycles < limit) {
    const auto before = emulator.cpu().state().cycles;
    const auto remaining = limit - static_cast<std::uint32_t>(before);
    reason = emulator.run_slice(std::min(slice, remaining));
    if (emulator.cpu().state().cycles == before) break;
  }
  const auto& cpu = emulator.cpu().state();
  const auto& iop = emulator.iop().state();
  check(reason == ps2vita::StopReason::StepLimit && cpu.cycles == limit &&
            cpu.pc == 0xBFC00000u + (20u + phase) * 4u &&
            cpu.cop0[9] == limit && cpu.fast_path_instructions == 0u,
        chain, phase, slice, "BIOS guest reaches its terminal branch within 64 EE cycles");
  check(cpu.gpr[8] == 0x10010000u && cpu.gpr[9] == 0u &&
            cpu.gpr[10] == ps2vita::Memory::kVu1DataBase &&
            cpu.gpr[11] == kUnpack[4] && cpu.gpr[0] == 0u,
        chain, phase, slice, "BIOS guest registers observe uploaded VU data after STR clears");
  check(memory.read32(kOutput) == kUnpack[4], chain, phase, slice,
        "BIOS guest saved uploaded data instead of stale poison");
  bool all_lanes = true;
  for (unsigned lane = 0; lane < 4u; ++lane)
    all_lanes = all_lanes &&
        memory.vu1_data_word(kVuOffset + lane * 4u) == kUnpack[4u + lane];
  check(all_lanes, chain, phase, slice, "BIOS route preserves all UNPACK raw lanes");
  check(emulator.vif1().packets_submitted() == 1u &&
            emulator.vif1().packets_rejected() == 0u &&
            emulator.vif1().vectors_unpacked() == 1u &&
            emulator.vif1().vu1().pairs_executed() == 0u,
        chain, phase, slice, "BIOS route services exactly one VIF packet without running VU");
  check(memory.read32(0x10009010u) == kPayload + 32u &&
            memory.read32(0x10009020u) == 0u &&
            (memory.read32(0x10009000u) & 0x100u) == 0u &&
            (memory.read32(0x1000E010u) & 2u) != 0u &&
            memory.read32(0x10009030u) == (chain ? kPayload + 32u : kTag),
        chain, phase, slice, "BIOS route publishes consumed DMA source registers");
  std::vector<std::uint8_t> packet;
  check(!memory.pop_vif1_packet(packet) && !memory.pop_gif_packet(packet) &&
            emulator.gif().pending_bytes() == 0u,
        chain, phase, slice, "BIOS route leaves no completed packet queued");
  check(iop.cycles == 8u && iop.pc == 0xBFC01000u &&
            emulator.iop().stop_reason() == ps2vita::IopStopReason::None &&
            iop.gpr[0] == 0u,
        chain, phase, slice, "BIOS route preserves eight-cycle EE/IOP scheduling ratio");
  return {{cpu, memory.read32(kOutput)}, iop};
}

struct GraphicsSnapshot {
  ps2vita::CpuState ee{};
  ps2vita::IopState iop{};
  std::vector<std::uint32_t> touched_ram, vu_data, vram, preview;
  std::array<std::uint32_t, 10> hardware{};
  std::uint32_t iop_counter = 0u;
  std::uint64_t frame = 0u, vif_packets = 0u, gif_packets = 0u, sprites = 0u;
};

bool same_snapshot(const GraphicsSnapshot& a, const GraphicsSnapshot& b) {
  return same_ee_architecture(a.ee, b.ee) && a.iop.gpr == b.iop.gpr &&
      a.iop.cop0 == b.iop.cop0 && a.iop.hi == b.iop.hi && a.iop.lo == b.iop.lo &&
      a.iop.pc == b.iop.pc && a.iop.cycles == b.iop.cycles &&
      a.touched_ram == b.touched_ram && a.vu_data == b.vu_data &&
      a.vram == b.vram && a.preview == b.preview && a.hardware == b.hardware &&
      a.iop_counter == b.iop_counter && a.frame == b.frame &&
      a.vif_packets == b.vif_packets && a.gif_packets == b.gif_packets &&
      a.sprites == b.sprites;
}

std::array<GraphicsSnapshot, 2> run_paced_graphics(bool chain,
    std::uint32_t phase, std::uint32_t quantum) {
  ps2vita::Emulator emulator;
  auto& memory = emulator.memory();
  auto original = make_program(chain);
  std::vector<std::uint32_t> program(original.begin(), original.end() - 1);
  // After the VIF upload/readback, guest stores kick a real PATH3 DMA sprite.
  // No host Gif::submit or Memory::advance is used by either execution policy.
  const std::array<std::uint32_t, 12> suffix{{
      0x34095000u, 0xAD09A010u, // D2_MADR = 0x5000
      0x3409000Bu, 0xAD09A020u, // D2_QWC = 11
      0x34090101u, 0xAD09A000u, // normal forward DMA, STR
      0x8D09A000u, 0x31290100u, 0x1520FFFDu, 0u, // poll completion
      0x1000FFFFu, 0u // bounded fixture uses a harmless terminal branch/NOP
  }};
  program.insert(program.end(), suffix.begin(), suffix.end());
  std::vector<std::uint8_t> rom(ps2vita::Memory::kBiosSize);
  const auto put = [&](std::size_t offset, std::uint32_t value) {
    for (unsigned byte = 0u; byte < 4u; ++byte)
      rom[offset + byte] = static_cast<std::uint8_t>(value >> (8u * byte));
  };
  for (std::size_t word = 0u; word < program.size(); ++word)
    put((word + phase) * 4u, program[word]);
  // An independent IOP loop makes delayed loads, branches and RAM writes live.
  const std::array<std::uint32_t, 7> iop_program{{
      0x34082000u, 0x8D090000u, 0u, 0x25290001u, 0xAD090000u,
      0x1000FFFBu, 0u}};
  for (std::size_t word = 0u; word < iop_program.size(); ++word)
    put(0x1000u + word * 4u, iop_program[word]);
  check(emulator.load_bios(rom.data(), rom.size()) && emulator.boot_bios(),
        chain, phase, quantum, "paced graphics owned reset ROM boots");
  emulator.iop().reset();
  emulator.iop().state().pc = 0xBFC01000u;
  memory.iop_write32(0x2000u, 0u);
  memory.write32(kTag, 0x70000002u);
  for (std::size_t word = 0u; word < kUnpack.size(); ++word)
    memory.write32(kPayload + static_cast<std::uint32_t>(word * 4u), kUnpack[word]);
  memory.write32(kOutput, 0xA55AA55Au);
  constexpr std::uint32_t color = 0x80654321u;
  const std::array<std::uint64_t, 22> packet{{
      0x100000000000800Aull, 0xEull,
      32u | (10ull << 16u), 0x4Cu, 128u | (1ull << 32u), 0x4Eu,
      1u, 0x1Au, 0u, 0x18u,
      31ull << 16u | 31ull << 48u, 0x40u, 0u, 0x47u,
      6u, 0u, color, 1u, 3ull << 32u, 5u,
      128u | (128ull << 16u) | (3ull << 32u), 5u}};
  for (std::size_t word = 0u; word < packet.size(); ++word)
    memory.write64(0x5000u + static_cast<std::uint32_t>(word * 8u), packet[word]);
  constexpr std::array<std::uint32_t, 10> registers{{
      0x10009000u, 0x10009010u, 0x10009020u, 0x10009030u,
      0x1000A000u, 0x1000A010u, 0x1000A020u, 0x1000E000u,
      0x1000E010u, 0x1000F000u}};
  std::array<GraphicsSnapshot, 2> results;
  std::uint64_t clock = 0u;
  std::uint32_t frame = 0u;
  for (unsigned stage = 0u; stage < results.size(); ++stage) {
    // An odd endpoint and a subsequent 17-cycle continuation expose hidden
    // EE branch / IOP delayed-load and scheduling-phase differences as well.
    const std::uint32_t limit = stage == 0u ? 257u : 274u;
    while (emulator.cpu().state().cycles < limit) {
      const auto before = emulator.cpu().state().cycles;
      auto last_reason = ps2vita::StopReason::StepLimit;
      if (quantum == 0u) {
        last_reason = emulator.run_slice(limit - static_cast<std::uint32_t>(before));
        check(last_reason == ps2vita::StopReason::StepLimit,
              chain, phase, quantum, "fixed-total graphics run stays live");
      } else {
        // Alternate elapsed-budget and quantum-cap frames. Clock time never
        // advances the guest; only ordinary run_slice calls perform work.
        const auto microseconds = (++frame & 1u) != 0u ? 2u : 5u;
        const auto result = ps2vita::run_execution_budget({microseconds, quantum, 3u},
            [&]() { return clock; }, [&](std::uint32_t request) {
              const auto left = limit - static_cast<std::uint32_t>(
                  emulator.cpu().state().cycles);
              last_reason = emulator.run_slice(std::min(request, left));
              ++clock;
              return last_reason == ps2vita::StopReason::StepLimit &&
                  emulator.cpu().state().cycles < limit;
            });
        check(result.quanta > 0u && result.quanta <= 3u &&
                  (result.stop == ps2vita::ExecutionBudgetStop::Elapsed ||
                   result.stop == ps2vita::ExecutionBudgetStop::QuantumLimit ||
                   result.stop == ps2vita::ExecutionBudgetStop::CallbackStopped),
              chain, phase, quantum, "paced graphics frame makes bounded progress");
      }
      // A broken scheduler must fail this fixture, not hang the test runner.
      if (last_reason != ps2vita::StopReason::StepLimit ||
          emulator.cpu().state().cycles == before) break;
    }
    // StepLimit is the run_slice return value, not a sticky CPU fault. A
    // normally completed instruction still leaves Cpu::stop_reason as None.
    check(emulator.cpu().state().cycles == limit &&
              emulator.cpu().stop_reason() == ps2vita::StopReason::None &&
              emulator.iop().stop_reason() == ps2vita::IopStopReason::None &&
              emulator.iop().state().cycles == limit / 8u,
          chain, phase, quantum, "paced EE endpoint and IOP horizon are exact");
    check(memory.read32(kOutput) == kUnpack[4] &&
              memory.iop_read32(0x2000u) > 0u && emulator.gs().pixel(0, 0) == color,
          chain, phase, quantum, "VIF readback, live IOP stores and guest PATH3 pixels occur");
    check(emulator.vif1().packets_submitted() == 1u &&
              emulator.vif1().packets_rejected() == 0u &&
              emulator.gif().packets_submitted() == 1u &&
              emulator.gif().packets_rejected() == 0u &&
              emulator.gif().sprites_emitted() == 1u && emulator.gif().pending_bytes() == 0u,
          chain, phase, quantum, "paced graphics services each DMA exactly once");
    std::vector<std::uint8_t> pending;
    check(!memory.pop_vif1_packet(pending) && !memory.pop_gif_packet(pending),
          chain, phase, quantum, "paced completed packet queues are empty");
    auto& out = results[stage];
    out.ee = emulator.cpu().state(); out.iop = emulator.iop().state();
    out.iop_counter = memory.iop_read32(0x2000u);
    out.frame = emulator.gs().color_frame();
    out.vif_packets = emulator.vif1().packets_submitted();
    out.gif_packets = emulator.gif().packets_submitted();
    out.sprites = emulator.gif().sprites_emitted();
    for (std::uint32_t address = 0u; address < 0x6000u; address += 4u)
      out.touched_ram.push_back(memory.read32(address));
    for (unsigned address = 0u; address < 0x4000u; address += 4u)
      out.vu_data.push_back(memory.vu1_data_word(static_cast<std::uint16_t>(address)));
    for (std::uint32_t address = 0u; address < 0x400000u; address += 4u)
      out.vram.push_back(emulator.gif().read_local32(address));
    for (int y = 0; y < ps2vita::Gs::kHeight; ++y)
      for (int x = 0; x < ps2vita::Gs::kWidth; ++x)
        out.preview.push_back(emulator.gs().pixel(x, y));
    for (std::size_t i = 0u; i < registers.size(); ++i)
      out.hardware[i] = memory.read32(registers[i]);
  }
  return results;
}

} // namespace

int main() {
  // Whole-run equality catches host batching changing guest-visible state.
  // No artificial FLUSHE, manual VIF submission or host service call is used.
  constexpr std::array<std::uint32_t, 6> slices{{1u, 2u, 7u, 8u, 64u, 4096u}};
  for (const bool chain : {false, true}) {
    // A six-NOP phase moves completion to cycle 33 and the guest LW to 40.
    // Servicing only every eight cycles would still read before servicing
    // the packet at cycle 40. The device event, not merely a small fixed
    // batching quantum, must provide the visibility boundary.
    for (const std::uint32_t phase : {0u, 6u}) {
      const auto baseline = run_fixture(chain, phase, slices[0]);
      for (std::size_t index = 1u; index < slices.size(); ++index) {
        const auto result = run_fixture(chain, phase, slices[index]);
        check(same_ee_architecture(result.cpu, baseline.cpu) &&
                  result.output == baseline.output,
              chain, phase, slices[index],
              "architectural result is invariant under host slice size");
      }
      for (const auto quantum : {1u, 7u, 2000u}) {
        const auto paced = run_fixture(chain, phase, quantum, true);
        check(same_ee_architecture(paced.cpu, baseline.cpu) &&
                  paced.output == baseline.output,
              chain, phase, quantum, "paced terminal ELF stops without an extra guest slice");
      }
      const auto bios_baseline = run_bios_fixture(chain, phase, slices[0]);
      for (std::size_t index = 1u; index < slices.size(); ++index) {
        const auto result = run_bios_fixture(chain, phase, slices[index]);
        check(same_ee_architecture(result.ee.cpu, bios_baseline.ee.cpu) &&
                  result.ee.output == bios_baseline.ee.output &&
                  result.iop.gpr == bios_baseline.iop.gpr &&
                  result.iop.cop0 == bios_baseline.iop.cop0 &&
                  result.iop.hi == bios_baseline.iop.hi &&
                  result.iop.lo == bios_baseline.iop.lo &&
                  result.iop.pc == bios_baseline.iop.pc &&
                  result.iop.cycles == bios_baseline.iop.cycles,
              chain, phase, slices[index],
              "BIOS EE/IOP architectural result is invariant under host slice size");
      }
      const auto graphics_baseline = run_paced_graphics(chain, phase, 0u);
      for (const auto quantum : {1u, 7u, 2000u}) {
        const auto paced = run_paced_graphics(chain, phase, quantum);
        check(same_snapshot(paced[0], graphics_baseline[0]) &&
                  same_snapshot(paced[1], graphics_baseline[1]),
              chain, phase, quantum,
              "paced/fixed runs preserve EE/IOP, touched RAM, all VU data/VRAM and continuation");
      }
    }
  }
  std::printf("emulator_graphics_timing_tests: %u checks, %u failures\n",
              checks, failures);
  return failures == 0u ? 0 : 1;
}
