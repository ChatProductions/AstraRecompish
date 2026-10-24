// Differential startup proof for a privately supplied PS2 game ELF.
// Builds only when -DASTRART_GAME_ELF points to a user-owned local file.
#include "ps2vita/aot.hpp"
#include "ps2vita/cpu.hpp"
#include "ps2vita/elf_loader.hpp"
#include "ps2vita/memory.hpp"

#include <array>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
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
  for (unsigned index = 0; index < 32; ++index) {
    oracle.fpr[index] = native.fpr[index] = 0x3f800000u + index;
  }

  const auto package = ps2vita::generated_phase0_aot_package();
  if (!ps2vita::validate_aot_package(package).ok()) {
    std::cerr << "generated native package failed validation\n";
    return 2;
  }
  if (package.entry_count != 1 || package.entries[0].guest_start != original.entry) {
    std::cerr << "expected one native entry at PS2 ELF entry PC\n";
    return 2;
  }
  const auto exit = ps2vita::execute_aot(package, native_memory, native);
  if (exit.kind != ps2vita::AotExitKind::Interpreter || !exit.instructions) {
    std::cerr << "native package did not yield to interpreter safely\n";
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
      oracle.fpr == native.fpr;

  std::cout << (matched ? "PASS" : "FAIL") << ": native vs interpreter "
            << exit.instructions << " guest instructions, entry=0x"
            << std::hex << original.entry << ", fallback=0x" << native.pc
            << std::dec << '\n';
  if (!matched) std::cerr << "guest state mismatch; do not use this package on Vita\n";
  return matched ? 0 : 1;
}
