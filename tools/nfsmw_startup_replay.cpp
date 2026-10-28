// Host-only startup replay for a privately supplied title ELF.
// No game image or translated game code is committed or packaged with this tool.
#include "ps2vita/emulator.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <memory>
#include <vector>

int main(int argc, char** argv) {
  if (argc != 2 && argc != 3) {
    std::fprintf(stderr, "usage: nfsmw_startup_replay <private boot.elf> [max_cycles]\n");
    return 2;
  }
  std::uint64_t limit = 12000000u;
  if (argc == 3) {
    char* end = nullptr;
    limit = std::strtoull(argv[2], &end, 10);
    if (*argv[2] == '\0' || *end != '\0' || limit == 0u ||
        limit > 100000000u) {
      std::fprintf(stderr, "invalid max_cycles (1 to 100000000)\n");
      return 2;
    }
  }
  std::ifstream file(argv[1], std::ios::binary);
  if (!file) {
    std::fprintf(stderr, "cannot open private ELF\n");
    return 2;
  }
  const std::vector<char> bytes(std::istreambuf_iterator<char>{file}, {});
  auto emulator = std::make_unique<ps2vita::Emulator>();
  const auto loaded = emulator->load_elf(bytes.data(), bytes.size());
  if (!loaded.ok) {
    std::fprintf(stderr, "cannot load ELF: %s\n", loaded.error);
    return 2;
  }
  emulator->enable_bare_elf_startup_syscalls(true);
  std::printf("replay=host-interpreter-only\nentry=%08X\n", loaded.entry);
  std::uint64_t next_progress = 1000000u;
  std::uint32_t last_deci2_count = 0u;
  unsigned sifcmd_wait_samples = 0u;
  while (emulator->cpu().state().cycles < limit) {
    constexpr std::uint32_t kQuantum = 32768u;
    const auto remaining = limit - emulator->cpu().state().cycles;
    const auto budget = static_cast<std::uint32_t>(
        remaining < kQuantum ? remaining : kQuantum);
    const auto stop = emulator->run_slice(budget);
    const auto& state = emulator->cpu().state();
    // The observed NFSMW SIFCMD initialization loop calls SifGetReg(4)
    // repeatedly until the IOP sets SIF_STAT_CMDINIT in SMFLAG. Detect the
    // absent-IOP bootstrap without manufacturing an IOP-ready response.
    const auto smflag = emulator->memory().read32(0x1000F230u);
    if (state.pc >= 0x00496D28u && state.pc <= 0x00496D38u &&
        (smflag & 0x20000u) == 0u && emulator->iop().state().cycles == 0u)
      ++sifcmd_wait_samples;
    else sifcmd_wait_samples = 0u;
    if (sifcmd_wait_samples >= 4u) {
      std::printf("reason=blocked-sifcmd-no-iop\npc=%08X\ncycles=%llu\n"
                  "sif_smflag=%08X\niop_cycles=0\n",
          state.pc, static_cast<unsigned long long>(state.cycles), smflag);
      return 0;
    }
    if (emulator->deci2_debug_print_count() != last_deci2_count) {
      last_deci2_count = emulator->deci2_debug_print_count();
      std::printf("deci2_count=%u\ndeci2_text=%s\n",
          last_deci2_count, emulator->deci2_last_debug_text());
    }
    if (state.cycles >= next_progress) {
      std::fprintf(stderr, "progress_cycles=%llu pc=%08X\n",
          static_cast<unsigned long long>(state.cycles), state.pc);
      next_progress = ((state.cycles / 1000000u) + 1u) * 1000000u;
    }
    if (stop != ps2vita::StopReason::StepLimit) {
      std::printf("reason=%s\npc=%08X\nopcode=%08X\ncycles=%llu\n"
                  "v1=%08X\na0=%08X\na1=%08X\na2=%08X\na3=%08X\n"
                  "v0=%08X\nra=%08X\nsp=%08X\n"
                  "iop_cycles=%llu\n",
          ps2vita::stop_reason_name(stop), state.pc,
          emulator->cpu().fault_instruction(),
          static_cast<unsigned long long>(state.cycles),
          static_cast<std::uint32_t>(state.gpr[3]),
          static_cast<std::uint32_t>(state.gpr[4]),
          static_cast<std::uint32_t>(state.gpr[5]),
          static_cast<std::uint32_t>(state.gpr[6]),
          static_cast<std::uint32_t>(state.gpr[7]),
          static_cast<std::uint32_t>(state.gpr[2]),
          static_cast<std::uint32_t>(state.gpr[31]),
          static_cast<std::uint32_t>(state.gpr[29]),
          static_cast<unsigned long long>(emulator->iop().state().cycles));
      return 0; // A captured stop is the desired diagnostic outcome.
    }
  }
  const auto& state = emulator->cpu().state();
  std::printf("reason=cycle-limit\npc=%08X\ncycles=%llu\n"
              "sif_smflag=%08X\niop_cycles=%llu\n",
              state.pc, static_cast<unsigned long long>(state.cycles),
              emulator->memory().read32(0x1000F230u),
              static_cast<unsigned long long>(emulator->iop().state().cycles));
  return 0;
}
