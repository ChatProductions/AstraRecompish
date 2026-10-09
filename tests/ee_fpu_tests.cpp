#include "ps2vita/cpu.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

unsigned checks = 0u;
unsigned failures = 0u;

void check(bool condition, const char* name, const char* property) {
  ++checks;
  if (!condition) {
    ++failures;
    std::fprintf(stderr, "FAIL [%s]: %s\n", name, property);
  }
}

std::uint32_t bits(float value) {
  std::uint32_t result = 0u;
  static_assert(sizeof(result) == sizeof(value), "32-bit float required");
  std::memcpy(&result, &value, sizeof(result));
  return result;
}

constexpr std::uint32_t kEntry = 0x1000u;
constexpr std::uint32_t kInputs = 0x2000u;
constexpr std::uint32_t kOutput = 0x2100u;
constexpr std::uint32_t kSentinel = 0xA55AA55Au;

constexpr std::uint32_t fp(unsigned fn, unsigned fd, unsigned fs,
                           unsigned ft = 0u) {
  return (0x11u << 26u) | (0x10u << 21u) | (ft << 16u) |
      (fs << 11u) | (fd << 6u) | fn;
}

constexpr std::uint32_t sqrt_s(unsigned fd, unsigned ft) {
  // EE Core Instruction Set Manual v6, p376: SQRT.S fd,ft. The input is in
  // bits 20..16, unlike conventional MIPS SQRT's fs encoding; fs must be zero.
  return fp(0x04u, fd, 0u, ft);
}

constexpr std::uint32_t load_fp(unsigned ft, unsigned offset) {
  return (0x31u << 26u) | (4u << 21u) | (ft << 16u) | offset;
}

constexpr std::uint32_t store_fp(unsigned ft) {
  return (0x39u << 26u) | (5u << 21u) | (ft << 16u);
}

void operand_routing(ps2vita::Memory& memory, ps2vita::Cpu& cpu) {
  struct Case {
    const char* name;
    unsigned ft, fd;
    std::uint32_t input, expected;
    unsigned tolerance = 0u;
  };
  constexpr std::array<Case, 9> cases{{
      {"ft12 positive9 to fd20", 12u, 20u, 0x41100000u, 0x40400000u},
      {"ft12 positive5202 to fd2", 12u, 2u, 0x45A29000u, 0x42903FF2u, 1u},
      {"destination aliases ft", 12u, 12u, 0x41100000u, 0x40400000u},
      {"ft0 is a real FPR", 0u, 20u, 0x41800000u, 0x40800000u},
      {"destination is f0", 12u, 0u, 0x43800000u, 0x41800000u},
      {"ft31 positive1", 31u, 2u, 0x3F800000u, 0x3F800000u},
      {"destination and source f0", 0u, 0u, 0x40800000u, 0x40000000u},
      {"positive zero passthrough", 12u, 20u, 0u, 0u},
      {"negative zero passthrough", 12u, 2u, 0x80000000u, 0x80000000u},
  }};
  for (const auto& item : cases) {
    cpu.reset(kEntry);
    auto& state = cpu.state();
    state.fpr.fill(0x42440000u); // Every unrelated FPR contains positive49.
    state.fpr[item.ft] = item.input;
    state.fpu_acc = 0x41200000u;
    state.gpr[4] = 0x123456789ABCDEF0ull;
    state.gpr_hi[4] = 0xFEDCBA9876543210ull;
    const auto before = state;
    auto expected_fpr = before.fpr;
    memory.write32(kEntry, sqrt_s(item.fd, item.ft));
    memory.write32(kOutput, kSentinel);
    const auto reason = cpu.step();
    // The irrational radius case allows an adjacent float; this fixture tests
    // operand routing, not the EE's detailed SQRT rounding behavior.
    const auto actual = state.fpr[item.fd];
    const auto delta = actual > item.expected ? actual - item.expected
                                             : item.expected - actual;
    expected_fpr[item.fd] = actual;
    check(reason == ps2vita::StopReason::None && state.pc == kEntry + 4u &&
              state.cycles == 1u,
          item.name, "one owned SQRT instruction completes");
    check(delta <= item.tolerance,
          item.name, "result uses ft, not the nonzero f0/fs sentinel");
    check(state.fpr == expected_fpr && state.fpu_acc == before.fpu_acc &&
              state.gpr == before.gpr && state.gpr_hi == before.gpr_hi &&
              memory.read32(kOutput) == kSentinel,
          item.name, "only the destination FPR changes");
  }
  // These are ordinary nonnegative finite inputs and signed-zero routing
  // checks. No assertion claims EE rounding precision, negative-input flags,
  // denormal handling, saturation, or the broader FCR31 implementation.
}

void other_unary_operands(ps2vita::Memory& memory, ps2vita::Cpu& cpu) {
  struct Case {
    const char* name;
    unsigned fn;
    std::uint32_t expected;
  };
  constexpr std::array<Case, 3> cases{{
      {"ABS.S still reads fs", 0x05u, 0x41100000u},
      {"MOV.S still reads fs", 0x06u, 0xC1100000u},
      {"NEG.S still reads fs", 0x07u, 0x41100000u},
  }};
  for (const auto& item : cases) {
    for (const unsigned fd : {20u, 12u, 0u}) {
      cpu.reset(kEntry);
      cpu.state().fpr.fill(0x42440000u);
      cpu.state().fpr[12] = 0xC1100000u; // -9 in fs; reserved ft=0 is +49.
      auto expected_fpr = cpu.state().fpr;
      expected_fpr[fd] = item.expected;
      memory.write32(kEntry, fp(item.fn, fd, 12u));
      check(cpu.step() == ps2vita::StopReason::None &&
                cpu.state().fpr == expected_fpr,
            item.name, "normal/aliased/f0 destinations keep conventional fs routing");
    }
  }
}

constexpr std::array<std::uint32_t, 23> radial_program() {
  // Owned instructions implementing a simple radial brightness function;
  // neither these instructions nor any fixture data are copied from a BIOS.
  // Input: radius^2,x,y,center_y,4,96,0,127; output: truncated brightness.
  return {{
      load_fp(12u, 0u),
      sqrt_s(20u, 12u),
      load_fp(1u, 4u),
      load_fp(2u, 8u),
      load_fp(3u, 12u),
      fp(0x01u, 2u, 3u, 2u),   // center_y - y
      fp(0x02u, 1u, 1u, 1u),   // x*x
      fp(0x02u, 2u, 2u, 2u),   // (center_y-y)^2
      fp(0x00u, 12u, 1u, 2u),
      sqrt_s(2u, 12u),
      load_fp(4u, 16u),
      fp(0x02u, 2u, 2u, 4u),   // 4*distance
      fp(0x01u, 0u, 20u, 2u),  // radius - 4*distance
      load_fp(5u, 20u),
      fp(0x02u, 0u, 0u, 5u),
      fp(0x03u, 0u, 0u, 20u),
      load_fp(6u, 24u),
      fp(0x28u, 0u, 0u, 6u),   // max(brightness,0)
      load_fp(7u, 28u),
      fp(0x29u, 0u, 0u, 7u),   // min(brightness,127)
      fp(0x24u, 8u, 0u),       // CVT.W.S: all final values are in range
      store_fp(8u),
      0x0000000Du,              // BREAK: bounded test completion
  }};
}

void radial_brightness(ps2vita::Memory& memory, ps2vita::Cpu& cpu) {
  constexpr auto program = radial_program();
  for (unsigned i = 0u; i < program.size(); ++i)
    memory.write32(kEntry + i * 4u, program[i]);

  constexpr float radius_squared = 5202.0f;
  constexpr float center_y = -5.1f;
  const double radius = std::sqrt(static_cast<double>(radius_squared));
  std::array<unsigned, 128> actual_histogram{};
  std::array<unsigned, 128> expected_histogram{};
  unsigned mismatches = 0u;
  bool bounded_completion = true;
  bool untouched_inputs = true;
  for (unsigned y = 0u; y < 17u; ++y) {
    for (unsigned x = 0u; x < 17u; ++x) {
      const float px = static_cast<float>(6 * static_cast<int>(x) - 45);
      const float py = static_cast<float>(6 * static_cast<int>(y) - 45);
      const std::array<float, 8> inputs{{
          radius_squared, px, py, center_y, 4.0f, 96.0f, 0.0f, 127.0f}};
      for (unsigned word = 0u; word < inputs.size(); ++word)
        memory.write32(kInputs + word * 4u, bits(inputs[word]));
      memory.write32(kOutput, kSentinel);
      cpu.reset(kEntry);
      cpu.state().gpr[4] = kInputs;
      cpu.state().gpr[5] = kOutput;
      cpu.state().fpr[0] = bits(49.0f); // fs=0 is deliberately not the radicand.
      const auto reason = cpu.run(64u);
      bounded_completion = bounded_completion &&
          reason == ps2vita::StopReason::Break &&
          cpu.state().cycles == program.size() &&
          cpu.state().pc == kEntry + (program.size() - 1u) * 4u;
      for (unsigned word = 0u; word < inputs.size(); ++word)
        untouched_inputs = untouched_inputs &&
            memory.read32(kInputs + word * 4u) == bits(inputs[word]);

      // Independent double-precision geometric oracle, not another decode of
      // the guest program. Integer bins are well away from their boundaries.
      // Its scope is ordinary finite arithmetic, not PS2 float bit identity.
      const double dy = static_cast<double>(center_y) - py;
      const double distance = std::hypot(static_cast<double>(px), dy);
      const double raw = 96.0 * (radius - 4.0 * distance) / radius;
      const auto expected = static_cast<unsigned>(std::fmin(127.0,
                                                    std::fmax(0.0, raw)));
      const auto actual = memory.read32(kOutput);
      ++expected_histogram[expected];
      if (actual < actual_histogram.size()) ++actual_histogram[actual];
      if (actual != expected) {
        if (mismatches < 4u)
          std::fprintf(stderr,
              "radial mismatch x=%u y=%u: actual=%u expected=%u\n",
              x, y, actual, expected);
        ++mismatches;
      }
    }
  }
  check(bounded_completion, "owned radial fixture",
        "all 289 guest executions finish within their exact instruction count");
  check(untouched_inputs, "owned radial fixture", "input memory is read-only");
  check(mismatches == 0u, "owned radial fixture",
        "every cell agrees with independently calculated radial brightness");
  check(actual_histogram == expected_histogram, "owned radial fixture",
        "brightness distribution is not flattened to one color");

  constexpr std::array<unsigned, 14> nonzero{{
      5u, 6u, 9u, 13u, 15u, 19u, 24u, 31u, 40u, 43u, 46u, 50u, 69u, 76u}};
  std::array<unsigned, 128> known_histogram{};
  known_histogram[0] = 261u;
  for (const auto value : nonzero) known_histogram[value] = 2u;
  check(expected_histogram == known_histogram, "independent radial oracle",
        "geometry yields 261 black cells and two cells in each nonzero bin");
  check(actual_histogram == known_histogram, "owned radial fixture",
        "guest reproduces the owned radial mask's known distribution");
}

} // namespace

int main() {
  ps2vita::Memory memory;
  ps2vita::Cpu cpu(memory);
  operand_routing(memory, cpu);
  other_unary_operands(memory, cpu);
  radial_brightness(memory, cpu);
  std::printf("EE FPU tests: %u checks, %u failures\n", checks, failures);
  return failures == 0u ? 0 : 1;
}
