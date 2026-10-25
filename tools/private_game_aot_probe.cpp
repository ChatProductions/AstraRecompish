// Linkage and state differential for the PRIVATE, game-specific native AOT.
// GitHub CI runs this against a redistributable three-instruction MIPS ELF.
// For a real title, build with ASTRART_GAME_ELF pointed at a private local file.
#include "ps2vita/aot.hpp"
#include "ps2vita/cpu.hpp"
#include "ps2vita/elf_loader.hpp"
#include "ps2vita/memory.hpp"

#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <vector>

namespace ps2vita {
const AotPackage& generated_private_host_game_aot_package();
}

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: private_game_aot_probe <ELF>\n";
    return 2;
  }
  std::ifstream input(argv[1], std::ios::binary);
  if (!input) {
    std::cerr << "cannot read local ELF\n";
    return 2;
  }
  const std::vector<char> elf(std::istreambuf_iterator<char>{input}, {});
  auto interpreted_memory = std::make_unique<ps2vita::Memory>();
  auto native_memory = std::make_unique<ps2vita::Memory>();
  const auto oracle_elf = ps2vita::load_elf32(
      elf.data(), elf.size(), *interpreted_memory);
  const auto native_elf = ps2vita::load_elf32(
      elf.data(), elf.size(), *native_memory);
  if (!oracle_elf.ok || !native_elf.ok ||
      oracle_elf.entry != native_elf.entry) {
    std::cerr << "ELF loader failed\n";
    return 2;
  }

  const auto& package = ps2vita::generated_private_host_game_aot_package();
  if (!ps2vita::validate_aot_package(package).ok() ||
      !ps2vita::find_aot_function(package, native_elf.entry)) {
    std::cerr << "private title AOT package missing or invalid\n";
    return 1;
  }

  ps2vita::Cpu interpreter(*interpreted_memory);
  interpreter.reset(oracle_elf.entry);
  ps2vita::CpuState compiled{};
  compiled.pc = native_elf.entry;
  const auto exit = ps2vita::dispatch_aot(
      package, *native_memory, compiled, 4u);
  if (exit.instructions == 0u || compiled.fast_path_instructions == 0u) {
    std::cerr << "private AOT never executed generated guest code\n";
    return 1;
  }
  const auto oracle_stop = interpreter.run(exit.instructions);
  const bool stop_matches =
      exit.kind == ps2vita::AotExitKind::Stop
          ? oracle_stop == exit.reason
          : oracle_stop == ps2vita::StopReason::StepLimit;
  const auto& oracle = interpreter.state();
  const bool matched =
      stop_matches && oracle.pc == compiled.pc &&
      oracle.cycles == compiled.cycles && oracle.cop0 == compiled.cop0 &&
      oracle.gpr == compiled.gpr && oracle.gpr_hi == compiled.gpr_hi &&
      oracle.hi == compiled.hi && oracle.lo == compiled.lo &&
      oracle.hi1 == compiled.hi1 && oracle.lo1 == compiled.lo1 &&
      oracle.shift_amount == compiled.shift_amount &&
      oracle.fpr == compiled.fpr && oracle.fcr == compiled.fcr &&
      oracle.fpu_acc == compiled.fpu_acc &&
      interpreted_memory->read32(0x2000u) == native_memory->read32(0x2000u);
  if (!matched) {
    std::cerr << "native first block differs from interpreted state"
              << " (pc " << std::hex << compiled.pc
              << " vs " << oracle.pc << std::dec << ")\n";
    return 1;
  }

  // A title-specific build must refuse to run compiled code against changed
  // instructions; fallback must occur before any architectural side effect.
  auto changed_memory = std::make_unique<ps2vita::Memory>();
  const auto changed_elf = ps2vita::load_elf32(
      elf.data(), elf.size(), *changed_memory);
  if (!changed_elf.ok) return 2;
  const auto original = changed_memory->read32(native_elf.entry);
  changed_memory->write32(native_elf.entry, original ^ 1u);
  ps2vita::CpuState changed{};
  changed.pc = native_elf.entry;
  const auto guard = ps2vita::execute_aot(
      package, *changed_memory, changed);
  if (guard.kind != ps2vita::AotExitKind::Interpreter ||
      guard.instructions != 0u || changed.cycles != 0u ||
      changed.pc != native_elf.entry) {
    std::cerr << "modified ELF did not fall back before native execution\n";
    return 1;
  }

  std::cout << "PASS: private generated AOT executed "
            << compiled.fast_path_instructions
            << " native guest instructions (" << exit.instructions
            << " total); PC/state oracle and code guard match\n";
  return 0;
}
