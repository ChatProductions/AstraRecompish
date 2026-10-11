#include "ps2vita/memory.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>

namespace {

unsigned checks = 0u, failures = 0u;
constexpr std::uint32_t kIntcStatus = 0x1000F000u;
constexpr std::uint32_t kIopStatus = 0x1F801070u;
constexpr std::uint32_t kSmode1 = 0x12000010u;
constexpr std::uint32_t kSyncv = 0x12000060u;
constexpr std::uint32_t kTimer3 = 0x10001800u;
constexpr std::uint64_t kEeClock = 294912000ull;
constexpr std::uint32_t kInitialRender = 4498396u;
constexpr std::uint32_t kInitialScanline = 18743u;

void check(bool condition, const char* property) {
  ++checks;
  if (!condition) {
    ++failures;
    std::fprintf(stderr, "FAIL: %s\n", property);
  }
}

void acknowledge_video(ps2vita::Memory& memory) {
  memory.write32(kIntcStatus, 0xCu);
  memory.iop_write32(kIopStatus, ~(1u | (1u << 11u)));
}

void expect_video_edge(ps2vita::Memory& memory, std::uint32_t duration, bool start) {
  acknowledge_video(memory);
  memory.advance(duration - 1u);
  check((memory.read32(kIntcStatus) & 0xCu) == 0u &&
            (memory.iop_read32(kIopStatus) & (1u | (1u << 11u))) == 0u,
        "neither video interrupt arrives before its pending deadline");
  check(memory.cycles_until_next_event() == 1u,
        "scheduler horizon exposes the final cycle before the video edge");
  memory.advance(1u);
  check((memory.read32(kIntcStatus) & 0xCu) == (start ? 4u : 8u) &&
            (memory.iop_read32(kIopStatus) & (1u | (1u << 11u))) ==
                (start ? 1u : (1u << 11u)),
        "video edge reaches EE and IOP together, with the correct start/end identity");
  check(memory.cycles_until_next_event() >= 1u && memory.cycles_until_next_event() <= 8u,
        "video retirement retains a positive bounded scheduler horizon");
}

void select_pal(ps2vita::Memory& memory, unsigned width = 8u,
                std::uint32_t alias = 0u) {
  const auto smode = kSmode1 | alias, sync = kSyncv | alias;
  switch (width) {
  case 1u:
    memory.write8(smode + 1u, 0x60u);
    memory.write8(sync, 1u);
    break;
  case 2u:
    memory.write16(smode, 0x6000u);
    memory.write16(sync, 1u);
    break;
  case 4u:
    memory.write32(smode, 0x6000u);
    memory.write32(sync, 1u);
    break;
  default:
    // Actual BIOS register words, with independently decoded selection bits.
    memory.write64(smode, 0x0000000740816504ull);
    memory.write64(sync, 0x00A9000502101401ull);
    break;
  }
}

void reset_ntsc_contract() {
  ps2vita::Memory memory;
  expect_video_edge(memory, kInitialRender, true);
  expect_video_edge(memory, 421724u, false);
  // The retained NTSC rational carry applies only when scheduling render.
  std::uint32_t previous_extra = 0u;
  for (unsigned field = 1u; field <= 100u; ++field) {
    const auto extra = field * 360u / 2997u;
    expect_video_edge(memory, kInitialRender + extra - previous_extra, true);
    expect_video_edge(memory, 421724u, false);
    previous_extra = extra;
  }
  select_pal(memory);
  memory.advance(197u);
  memory.clear();
  check(memory.read64(kSmode1) == 0u && memory.read64(kSyncv) == 0u,
        "reset clears mode registers and returns to the explicit NTSC fallback");
  expect_video_edge(memory, kInitialRender, true);
  expect_video_edge(memory, 421724u, false);
}

void pal_rational_fields() {
  ps2vita::Memory memory;
  select_pal(memory);
  // A mode write preserves the reset render deadline, not a whole fresh PAL
  // render period. Subsequent phases derive from independent clock/line math.
  expect_video_edge(memory, kInitialRender, true);
  std::uint64_t previous = 0u;
  const auto field_numerator = kEeClock * 2u;
  // Scanline = clock*2/(50*625), blank = 49/2 scanlines. The denominator
  // 62,500 keeps the exact fractional phase; no production helper is called.
  const auto blank_numerator = field_numerator * 49u;
  const auto full_field_numerator = kEeClock * 1250u;
  for (unsigned field = 1u; field <= 125u; ++field) {
    const auto blank_end = ((field - 1u) * full_field_numerator + blank_numerator) / 62500u;
    const auto field_end = field * full_field_numerator / 62500u;
    expect_video_edge(memory, static_cast<std::uint32_t>(blank_end - previous), false);
    expect_video_edge(memory, static_cast<std::uint32_t>(field_end - blank_end), true);
    previous = field_end;
  }
  check(previous == 125u * (kEeClock / 50u),
        "125 PAL fields accumulate the exact master-clock duration without integer drift");
}

void pal_rational_scanlines() {
  ps2vita::Memory memory;
  memory.write32(kTimer3 + 0x10u, 0x83u);
  select_pal(memory);
  memory.advance(kInitialScanline - 1u);
  check(memory.read32(kTimer3) == 0u && memory.cycles_until_next_event() == 1u,
        "PAL selection preserves the initial pending NTSC scanline edge");
  memory.advance(1u);
  check(memory.read32(kTimer3) == 1u, "first preserved scanline clocks Timer3 once");
  std::uint64_t previous = 0u;
  for (unsigned line = 1u; line <= 625u; ++line) {
    const auto cumulative = line * (kEeClock * 2u) / (50u * 625u);
    const auto duration = static_cast<std::uint32_t>(cumulative - previous);
    memory.advance(duration - 1u);
    check(memory.read32(kTimer3) == line && memory.cycles_until_next_event() == 1u,
          "rational PAL scanline remains quiet one cycle before its edge");
    memory.advance(1u);
    check(memory.read32(kTimer3) == line + 1u,
          "rational PAL scanline clocks Timer3 exactly once at its edge");
    previous = cumulative;
  }
  check(previous == 2u * (kEeClock / 50u),
        "625 PAL scanlines exactly span two field durations, not rounded 312-line fields");
}

void width_alias_and_repeated_writes() {
  for (const unsigned width : {1u, 2u, 4u, 8u})
    for (const std::uint32_t alias : {0u, 0xA0000000u, 0x80000000u}) {
      ps2vita::Memory memory;
      memory.advance(97u);
      select_pal(memory, width, alias);
      expect_video_edge(memory, kInitialRender - 97u, true);
      // The first blank has already accumulated its 2/125 fraction. Same
      // selection writes must neither reset that carry nor its live deadline.
      memory.advance(53u);
      select_pal(memory, width, alias);
      memory.write8(kSmode1 + 7u, 0xA5u); // Reserved/unrelated selection bits.
      memory.write64(0x12000020u, 0u);   // INT alone does not determine timing.
      expect_video_edge(memory, 462422u - 53u, false);
      expect_video_edge(memory, 5435818u, true);
      expect_video_edge(memory, 462422u, false);
    }
}

void fallback_and_mode_switch_deadlines() {
  for (unsigned cmod = 0u; cmod <= 3u; ++cmod)
    for (unsigned odd_front_porch = 0u; odd_front_porch <= 1u; ++odd_front_porch) {
      if (cmod == 3u && odd_front_porch != 0u) continue;
      ps2vita::Memory memory;
      memory.write64(kSmode1, std::uint64_t{cmod} << 13u);
      memory.write64(kSyncv, odd_front_porch);
      memory.write64(0x12000020u, 3u); // An INT bit must not invent PAL support.
      expect_video_edge(memory, kInitialRender, true);
      expect_video_edge(memory, 421724u, false);
    }

  ps2vita::Memory memory;
  select_pal(memory);
  expect_video_edge(memory, kInitialRender, true);
  memory.advance(71u);
  memory.write8(kSmode1 + 1u, 0x40u); // Switch to NTSC while PAL blank is pending.
  expect_video_edge(memory, 462422u - 71u, false);
  expect_video_edge(memory, kInitialRender, true);
  expect_video_edge(memory, 421724u, false);
  // Switch during NTSC blank: its existing edge survives, then PAL's first
  // render schedules with a fresh 123/125 remainder and the next blank carries.
  expect_video_edge(memory, kInitialRender, true);
  memory.advance(31u);
  select_pal(memory);
  expect_video_edge(memory, 421724u - 31u, false);
  expect_video_edge(memory, 5435817u, true);
  expect_video_edge(memory, 462423u, false);

  ps2vita::Memory lines;
  lines.write32(kTimer3 + 0x10u, 0x83u);
  lines.advance(101u);
  select_pal(lines);
  lines.advance(kInitialScanline - 101u);
  check(lines.read32(kTimer3) == 1u, "switch leaves the active scanline deadline intact");
  lines.advance(43u);
  lines.write8(kSyncv, 0u); // Unsupported PAL progressive => legacy NTSC fallback.
  lines.advance(18874u - 43u - 1u);
  check(lines.read32(kTimer3) == 1u, "fallback leaves the pending PAL scanline deadline intact");
  lines.advance(1u);
  check(lines.read32(kTimer3) == 2u, "pending PAL scanline still retires after selection changes");
  lines.advance(kInitialScanline - 1u);
  check(lines.read32(kTimer3) == 2u, "next fallback scanline uses the unchanged NTSC cadence");
  lines.advance(1u);
  check(lines.read32(kTimer3) == 3u, "fallback scanline clocks Timer3 at the legacy deadline");
}

std::array<std::uint64_t, 11> snapshot(const ps2vita::Memory& memory) {
  return {{memory.read32(kIntcStatus), memory.iop_read32(kIopStatus),
      memory.read32(kTimer3), memory.read32(kTimer3 + 0x10u),
      memory.read32(kTimer3 + 0x20u), memory.iop_read32(0x1F8014A0u),
      memory.iop_read32(0x1F8014A4u), memory.iop_read32(0x1F8014A8u),
      memory.read64(kSmode1), memory.read64(kSyncv), memory.cycles_until_next_event()}};
}

void advance_split(ps2vita::Memory& memory, std::uint32_t cycles) {
  constexpr std::array<std::uint32_t, 8> quantum{{1u, 3u, 7u, 8u, 509u, 18743u, 65536u, 3000001u}};
  unsigned index = 0u;
  while (cycles != 0u) {
    const auto count = std::min(cycles, quantum[index++ % quantum.size()]);
    memory.advance(count);
    cycles -= count;
  }
}

void split_advance_equivalence() {
  ps2vita::Memory single, split;
  for (auto* memory : {&single, &split}) {
    memory->write32(kTimer3 + 0x10u, 0x83u);
    memory->write32(kTimer3 + 0x20u, 0xFFFFu);
    memory->iop_write32(0x1F8014A4u, 0x2000u);
  }
  const std::array<std::uint32_t, 9> durations{{97u, 4498299u, 53u, 462369u,
      5435818u, 1001u, 12000001u, 18743u, 5898240u}};
  for (unsigned step = 0u; step < durations.size(); ++step) {
    if (step == 1u || step == 3u || step == 7u) {
      select_pal(single); select_pal(split);
    } else if (step == 6u) {
      single.write8(kSmode1 + 1u, 0x20u);
      split.write8(kSmode1 + 1u, 0x20u);
    }
    single.advance(durations[step]);
    advance_split(split, durations[step]);
    check(snapshot(single) == snapshot(split),
          "single and irregular host slices preserve mode, edge, Timer3, IOP phase and event horizon");
  }
}

} // namespace

int main() {
  reset_ntsc_contract();
  pal_rational_fields();
  pal_rational_scanlines();
  width_alias_and_repeated_writes();
  fallback_and_mode_switch_deadlines();
  split_advance_equivalence();
  std::printf("Video timing tests: %u checks, %u failures; scope=PAL-interlaced-cadence-not-scanout\n",
              checks, failures);
  return failures == 0u ? 0 : 1;
}
