#include "ps2vita/emulator.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

namespace {
bool number(const char* text, std::uint64_t limit, std::uint64_t& value) {
  value = 0u;
  if (*text == '\0') return false;
  for (; *text; ++text) {
    if (*text < '0' || *text > '9') return false;
    const auto digit = static_cast<unsigned>(*text - '0');
    if (value > (limit - digit) / 10u) return false;
    value = value * 10u + digit;
  }
  return value != 0u && value <= limit;
}

struct Hash {
  std::uint64_t value = 1469598103934665603ull;
  template<class T> void add(T word) {
    // Defined little-endian byte order; never hash padding or host pointers.
    for (unsigned i = 0; i < sizeof(T); ++i) {
      value ^= static_cast<std::uint8_t>(word);
      value *= 1099511628211ull;
      word >>= 8u;
    }
  }
  template<class T, std::size_t N> void add(const std::array<T, N>& words) {
    for (const auto& word : words) add(word);
  }
};

struct Result {
  std::uint64_t cpu = 0u, ram = 0u, iop_ram = 0u, vu = 0u, graphics = 0u;
  bool operator==(const Result& other) const {
    return cpu == other.cpu && ram == other.ram && iop_ram == other.iop_ram &&
        vu == other.vu && graphics == other.graphics;
  }
};

Result digest(const ps2vita::Emulator& emu) {
  Hash cpu, ram, iop_ram, vu, graphics;
  const auto& ee = emu.cpu().state();
  cpu.add(ee.gpr); cpu.add(ee.gpr_hi); cpu.add(ee.cop0);
  cpu.add(ee.fpr); cpu.add(ee.fcr); cpu.add(ee.fpu_acc);
  cpu.add(ee.vu0_vf); cpu.add(ee.vu0_vf_hi); cpu.add(ee.vu0_vi); cpu.add(ee.vu0_acc);
  cpu.add(ee.hi); cpu.add(ee.lo); cpu.add(ee.hi1); cpu.add(ee.lo1);
  cpu.add(ee.shift_amount); cpu.add(ee.pc); cpu.add(ee.cycles);
  cpu.add(emu.cpu().fault_instruction()); cpu.add(emu.cpu().fault_address());
  const auto& iop = emu.iop().state();
  cpu.add(iop.gpr); cpu.add(iop.cop0); cpu.add(iop.hi); cpu.add(iop.lo);
  cpu.add(iop.pc); cpu.add(iop.cycles);
  cpu.add(emu.iop().fault_instruction()); cpu.add(emu.iop().fault_address());
  // Only side-effect-free storage, via unmapped KSEG1 aliases for EE RAM.
  // Deliberately do not digest MMIO reads or private object representations.
  for (std::uint32_t a = 0; a < ps2vita::Memory::kRamSize; a += 4u)
    ram.add(emu.memory().read32(0xA0000000u + a));
  for (std::uint32_t a = 0; a < ps2vita::Memory::kIopRamSize; a += 4u)
    iop_ram.add(emu.memory().iop_read32(a));
  const auto& v = emu.vif1().vu1();
  vu.add(v.state().vf); vu.add(v.state().vi); vu.add(v.state().acc);
  vu.add(v.state().i); vu.add(v.state().q); vu.add(v.state().mac); vu.add(v.state().pc);
  vu.add(v.pairs_executed()); vu.add(v.cycles_executed());
  vu.add(v.path1_tags_queued()); vu.add(v.path1_tags_rejected());
  for (std::uint32_t a = 0; a < 0x4000u; a += 4u) {
    vu.add(emu.memory().vu1_micro_word(static_cast<std::uint16_t>(a)));
    vu.add(emu.memory().vu1_data_word(static_cast<std::uint16_t>(a)));
  }
  graphics.add(emu.gs().color_frame());
  const auto& gif = emu.gif();
  graphics.add(gif.packets_submitted()); graphics.add(gif.packets_rejected());
  graphics.add(static_cast<std::uint64_t>(gif.pending_bytes()));
  graphics.add(gif.triangles_emitted()); graphics.add(gif.sprites_emitted());
  graphics.add(gif.points_emitted()); graphics.add(gif.lines_emitted());
  graphics.add(gif.packed_tags()); graphics.add(gif.reglist_tags()); graphics.add(gif.image_tags());
  graphics.add(gif.image_bytes()); graphics.add(gif.local_bytes_written());
  graphics.add(emu.vif1().packets_submitted()); graphics.add(emu.vif1().packets_rejected());
  for (int y = 0; y < ps2vita::Gs::kHeight; ++y)
    for (int x = 0; x < ps2vita::Gs::kWidth; ++x) graphics.add(emu.gs().pixel(x, y));
  for (std::uint32_t a = 0; a < 0x400000u; a += 4u) graphics.add(gif.read_local32(a));
  return {cpu.value, ram.value, iop_ram.value, vu.value, graphics.value};
}

int self_test() {
  std::vector<std::uint8_t> bios(ps2vita::Memory::kBiosSize, 0u);
  // Owned two-instruction BEQ-zero loop plus NOP delay slot, used by both
  // reset cores. No external firmware or downloaded fixture is required.
  bios[0] = 0xFFu; bios[1] = 0xFFu; bios[3] = 0x10u;
  Result expected;
  bool first = true;
  for (const unsigned quantum : {1u, 7u, 8u, 2048u}) {
    auto emu = std::make_unique<ps2vita::Emulator>();
    if (!emu->load_bios(bios.data(), bios.size()) || !emu->boot_bios()) return 1;
    constexpr unsigned cycles = 8192u;
    while (emu->cpu().state().cycles < cycles) {
      const auto count = std::min<std::uint64_t>(quantum, cycles - emu->cpu().state().cycles);
      if (emu->run_slice(static_cast<unsigned>(count)) != ps2vita::StopReason::StepLimit) return 1;
    }
    if (emu->cpu().state().pc != 0xBFC00000u || emu->cpu().state().cycles != cycles ||
        emu->iop().state().cycles != cycles / 8u) return 1;
    const auto state = digest(*emu);
    if (!first && !(state == expected)) return 1;
    expected = state; first = false;
  }
  std::puts("bios_benchmark_self_test matched=1 quantums=1,7,8,2048 firmware=owned-branch-loop");
  return 0;
}

int benchmark(int argc, char** argv) {
  std::uint64_t cycles = 0u, samples = 0u, quantum = 2048u;
  if (argc < 4 || argc > 5 || !number(argv[2], 10000000000ull, cycles) ||
      !number(argv[3], 25u, samples) ||
      (argc == 5 && !number(argv[4], 1000000u, quantum))) {
    std::fputs("usage: ps2bios_benchmark BIOS MODELED_EE_CYCLES SAMPLES(1..25) [SLICE_QUANTUM(1..1000000)]\n"
               "fresh boot, timed Emulator::run_slice only; not guest instruction rate or Vita FPS\n", stderr);
    return 2;
  }
  std::ifstream input(argv[1], std::ios::binary);
  const std::vector<std::uint8_t> bios((std::istreambuf_iterator<char>(input)), {});
  if (bios.empty() || input.bad()) {
    std::fputs("could not read BIOS\n", stderr);
    return 2;
  }
  std::vector<std::uint64_t> timings;
  Result expected;
  for (std::uint64_t sample = 0; sample < samples; ++sample) {
    auto emu = std::make_unique<ps2vita::Emulator>();
    if (!emu->load_bios(bios.data(), bios.size()) || !emu->boot_bios()) {
      std::fputs("BIOS load/boot failed\n", stderr);
      return 2;
    }
    ps2vita::StopReason reason = ps2vita::StopReason::StepLimit;
    const auto start = std::chrono::steady_clock::now();
    while (emu->cpu().state().cycles < cycles && reason == ps2vita::StopReason::StepLimit) {
      const auto remaining = cycles - emu->cpu().state().cycles;
      reason = emu->run_slice(static_cast<std::uint32_t>(std::min(quantum, remaining)));
    }
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - start).count();
    if (ns <= 0 || reason != ps2vita::StopReason::StepLimit || emu->cpu().state().cycles != cycles) {
      std::fprintf(stderr, "BIOS benchmark stopped reason=%s cycle=%llu pc=%08X\n",
          ps2vita::stop_reason_name(reason), static_cast<unsigned long long>(emu->cpu().state().cycles),
          emu->cpu().state().pc);
      return 1;
    }
    // Hashing, BIOS input, reset, allocation and reporting are outside timing.
    const auto state = digest(*emu);
    if (sample != 0u && !(state == expected)) {
      std::fputs("BIOS benchmark digest diverged between fresh boots\n", stderr);
      return 1;
    }
    expected = state;
    timings.push_back(static_cast<std::uint64_t>(ns));
    std::printf("bios_benchmark_sample index=%llu run_slice_ns=%llu ee_pc=%08X iop_pc=%08X "
                "fast_path_instructions=%llu vu_pairs=%llu gif_packets=%llu vif_packets=%llu "
                "SMODE1=%016llX SYNCV=%016llX\n",
        static_cast<unsigned long long>(sample + 1u), static_cast<unsigned long long>(ns),
        emu->cpu().state().pc, emu->iop().state().pc,
        static_cast<unsigned long long>(emu->cpu().state().fast_path_instructions),
        static_cast<unsigned long long>(emu->vif1().vu1().pairs_executed()),
        static_cast<unsigned long long>(emu->gif().packets_submitted()),
        static_cast<unsigned long long>(emu->vif1().packets_submitted()),
        static_cast<unsigned long long>(emu->memory().read64(0x12000010u)),
        static_cast<unsigned long long>(emu->memory().read64(0x12000060u)));
    std::fflush(stdout);
  }
  std::sort(timings.begin(), timings.end());
  const auto middle = timings.size() / 2u;
  const auto median = timings.size() % 2u ? timings[middle] :
      timings[middle - 1u] + (timings[middle] - timings[middle - 1u]) / 2u;
  std::printf("bios_benchmark scope=host-whole-system-run-slice-not-vita-fps modeled_ee_cycles=%llu "
              "quantum=%llu warmups=0 samples=%llu median_ns=%llu min_ns=%llu max_ns=%llu "
              "cpu_hash=%016llX ram_hash=%016llX iop_ram_hash=%016llX vu_hash=%016llX graphics_hash=%016llX\n",
      static_cast<unsigned long long>(cycles), static_cast<unsigned long long>(quantum),
      static_cast<unsigned long long>(samples), static_cast<unsigned long long>(median),
      static_cast<unsigned long long>(timings.front()), static_cast<unsigned long long>(timings.back()),
      static_cast<unsigned long long>(expected.cpu), static_cast<unsigned long long>(expected.ram),
      static_cast<unsigned long long>(expected.iop_ram), static_cast<unsigned long long>(expected.vu),
      static_cast<unsigned long long>(expected.graphics));
  return 0;
}
} // namespace

int main(int argc, char** argv) {
  try {
    if (argc == 2 && std::string(argv[1]) == "--self-test") return self_test();
    return benchmark(argc, argv);
  } catch (const std::bad_alloc&) {
    std::fputs("BIOS benchmark allocation failed\n", stderr);
    return 2;
  }
}
