// Differential startup proof for a privately supplied PS2 game ELF.
// Builds only when -DASTRART_GAME_ELF points to a user-owned local file.
#include "ps2vita/aot.hpp"
#include "ps2vita/cpu.hpp"
#include "ps2vita/elf_loader.hpp"
#include "ps2vita/emulator.hpp"
#include "ps2vita/memory.hpp"

#include <array>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <vector>

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: nfsmw_entry_probe <PS2 ELF>\n";
    return 2;
  }
  std::ifstream input(argv[1], std::ios::binary);
  if (!input) {
    std::cerr << "cannot read PS2 ELF\n";
    return 2;
  }
  std::vector<char> image(std::istreambuf_iterator<char>{input}, {});
  ps2vita::Memory interpreter_memory;
  ps2vita::Memory native_memory;
  const auto original = ps2vita::load_elf32(
      image.data(), image.size(), interpreter_memory);
  const auto translated = ps2vita::load_elf32(
      image.data(), image.size(), native_memory);
  if (!original.ok || !translated.ok || original.entry != translated.entry) {
    std::cerr << "ELF could not be loaded into PS2 EE memory\n";
    return 2;
  }

  ps2vita::Cpu interpreter(interpreter_memory);
  interpreter.reset(original.entry);
  auto& oracle = interpreter.state();
  ps2vita::CpuState native{};
  native.pc = original.entry;
  // Non-zero seed catches missing upper halves and incorrect accumulator moves.
  for (unsigned index = 1; index < 32; ++index) {
    const std::uint64_t low = 0xFEDCBA9812345678ull ^
        (static_cast<std::uint64_t>(index) * 0x0101010101010101ull);
    const std::uint64_t high = 0x0123456789ABCDEFull ^
        (static_cast<std::uint64_t>(index) * 0x1111000011110000ull);
    oracle.gpr[index] = native.gpr[index] = low;
    oracle.gpr_hi[index] = native.gpr_hi[index] = high;
  }
  oracle.hi = native.hi = 0x11112222u;
  oracle.lo = native.lo = 0x33334444u;
  oracle.hi1 = native.hi1 = 0x55556666u;
  oracle.lo1 = native.lo1 = 0x77778888u;
  oracle.shift_amount = native.shift_amount = 6;
  oracle.fpu_acc = native.fpu_acc = 0x40000000u;
  for (unsigned index = 0; index < 32; ++index) {
    oracle.fpr[index] = native.fpr[index] = 0x3f800000u + index;
    oracle.fcr[index] = native.fcr[index] = 0x12340000u + index;
  }

  const auto package = ps2vita::generated_phase0_aot_package();
  if (!ps2vita::validate_aot_package(package).ok()) {
    std::cerr << "generated native package failed validation\n";
    return 2;
  }
  if (!ps2vita::find_aot_function(package, original.entry)) {
    std::cerr << "PS2 ELF entry PC missing from generated native package\n";
    return 2;
  }
  const ps2vita::CpuState startup_state = native;
  // The device scheduler yields every eight EE cycles. A startup sequence
  // must therefore resume across multiple small native dispatches.
  const auto exit = ps2vita::dispatch_aot(package, native_memory, native, 10u);
  if ((exit.kind != ps2vita::AotExitKind::Interpreter &&
       exit.kind != ps2vita::AotExitKind::Direct) ||
      exit.instructions != 76u) {
    std::cerr << "native package did not complete 76 startup instructions"
              << " (completed " << exit.instructions << ")\n";
    return 1;
  }
  const auto stop = interpreter.run(exit.instructions);
  bool matched = stop == ps2vita::StopReason::StepLimit &&
      oracle.pc == native.pc && oracle.cycles == native.cycles &&
      oracle.cop0[9] == native.cop0[9] && oracle.gpr == native.gpr &&
      oracle.gpr_hi == native.gpr_hi && oracle.hi == native.hi &&
      oracle.lo == native.lo && oracle.hi1 == native.hi1 &&
      oracle.lo1 == native.lo1 &&
      oracle.shift_amount == native.shift_amount &&
      oracle.fpr == native.fpr &&
      oracle.fcr == native.fcr &&
      oracle.fpu_acc == native.fpu_acc;

  // The real Emulator::run_slice path must actually dispatch native code,
  // not merely work when a test calls execute_aot directly.
  auto emulator = std::make_unique<ps2vita::Emulator>();
  const auto loaded = emulator->load_elf(image.data(), image.size());
  if (!loaded.ok || loaded.entry != original.entry ||
      !emulator->cpu().set_startup_aot_package(&package)) {
    std::cerr << "could not prepare opt-in emulator startup AOT\n";
    return 2;
  }
  emulator->cpu().state() = startup_state;
  const auto integrated_reason = emulator->run_slice(exit.instructions);
  const auto& integrated = emulator->cpu().state();
  const bool integrated_ok = integrated_reason == ps2vita::StopReason::StepLimit &&
      integrated.fast_path_instructions == native.fast_path_instructions &&
      integrated.gpr == oracle.gpr && integrated.gpr_hi == oracle.gpr_hi &&
      integrated.fpr == oracle.fpr && integrated.fcr == oracle.fcr &&
      integrated.fpu_acc == oracle.fpu_acc && integrated.pc == oracle.pc &&
      integrated.cycles == oracle.cycles;
  matched = matched && integrated_ok;
  if (!integrated_ok) {
    std::cerr << "integrated: reason=" << static_cast<int>(integrated_reason)
              << " fast=" << integrated.fast_path_instructions
              << " cycles=" << integrated.cycles
              << " pc=0x" << std::hex << integrated.pc
              << " expected_pc=0x" << oracle.pc << std::dec
              << " expected_fast=" << native.fast_path_instructions << '\n';
  }

  // The first ten native dispatches are only a scheduler-bound prefix, not
  // the extent of the game code. Probe beyond that boundary with a separate
  // interpreter oracle, while leaving unsupported instructions to fallback.
  const auto continuation = ps2vita::dispatch_aot(
      package, native_memory, native, 64u);
  const auto continuation_stop = interpreter.run(continuation.instructions);
  const bool continuation_ok =
      continuation_stop == ps2vita::StopReason::StepLimit &&
      oracle.pc == native.pc && oracle.cycles == native.cycles &&
      oracle.gpr == native.gpr && oracle.gpr_hi == native.gpr_hi &&
      oracle.fpr == native.fpr && oracle.fcr == native.fcr &&
      oracle.fpu_acc == native.fpu_acc &&
      oracle.shift_amount == native.shift_amount &&
      oracle.hi == native.hi && oracle.lo == native.lo &&
      oracle.hi1 == native.hi1 && oracle.lo1 == native.lo1;
  matched = matched && continuation_ok;
  if (!continuation_ok) {
    std::cerr << "continuation mismatch after " << continuation.instructions
              << " native guest instructions, interpreter_stop="
              << static_cast<int>(continuation_stop)
              << " next_pc=0x" << std::hex << native.pc
              << " oracle_pc=0x" << oracle.pc << std::dec << '\n';
  }

  // A title build must not execute stale native instructions when the ELF in
  // ux0:data/ps2vita/boot.elf differs from the one used to compile it.
  auto altered = std::make_unique<ps2vita::Emulator>();
  const auto altered_load = altered->load_elf(image.data(), image.size());
  if (!altered_load.ok ||
      !altered->cpu().set_startup_aot_package(&package)) {
    std::cerr << "cannot prepare code-identity fallback check\n";
    return 2;
  }
  altered->cpu().state() = startup_state;
  altered->memory().write32(original.entry + 4u, 0u); // Legal MIPS NOP.
  const auto altered_stop = altered->run_slice(8u);
  const bool guard_ok = altered_stop == ps2vita::StopReason::StepLimit &&
      altered->cpu().state().fast_path_instructions == 0u &&
      altered->cpu().state().cycles == 8u;
  if (!guard_ok)
    std::cerr << "mismatched game code did not deopt to interpreter\n";
  matched = matched && guard_ok;

  std::cout << (matched ? "PASS" : "FAIL") << ": native vs interpreter "
            << exit.instructions << " guest instructions, entry=0x"
            << std::hex << original.entry << ", fallback=0x" << native.pc
            << std::dec << ", integrated=" << (integrated_ok ? "PASS" : "FAIL")
            << ", extended=" << (continuation_ok ? "PASS" : "FAIL")
            << ", extra=" << continuation.instructions
            << ", final_pc=0x" << std::hex << native.pc << std::dec
            << ", code-guard=" << (guard_ok ? "PASS" : "FAIL")
            << '\n';
  if (!matched) std::cerr << "guest state mismatch; do not use this package on Vita\n";
  return matched ? 0 : 1;
}
