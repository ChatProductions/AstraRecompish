#include "ps2vita/gs.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <vector>

// Independent byte oracle for logical linear VRAM, not hardware GS swizzling
// or native raster coverage. Compile this fixture separately for each grid.
namespace {
constexpr std::uint64_t kBytes = 4u * 1024u * 1024u;
constexpr unsigned kScale = ps2vita::Gs::kRasterScale;
unsigned checks = 0, failures = 0;

void check(bool ok, const char* label) {
  ++checks;
  if (!ok) { ++failures; std::fprintf(stderr, "FAIL: %s\n", label); }
}

std::uint32_t load(const std::vector<std::uint8_t>& memory, std::uint64_t address) {
  std::uint32_t value = 0;
  for (unsigned byte = 0; byte < 4; ++byte)
    value += std::uint32_t{memory[(address + byte) % kBytes]} << (8u * byte);
  return value;
}

void store(std::vector<std::uint8_t>& memory, std::uint64_t address, std::uint32_t value) {
  for (unsigned byte = 0; byte < 4; ++byte)
    memory[(address + byte) % kBytes] = static_cast<std::uint8_t>(value >> (8u * byte));
}

std::uint64_t address(unsigned base, unsigned width, unsigned x, unsigned y) {
  return std::uint64_t{base} * 8192u + (std::uint64_t{y} * width + x) * 4u;
}

void tile(std::vector<std::uint8_t>& memory, unsigned base, unsigned width,
          int x, int y, std::uint32_t color, std::uint32_t mask) {
  if (static_cast<unsigned>(x) * kScale >= width) return;
  for (unsigned row = 0; row < kScale; ++row)
    for (unsigned column = 0; column < kScale; ++column) {
      const auto at = address(base, width, x * kScale + column, y * kScale + row);
      const auto old = load(memory, at);
      store(memory, at, old ^ ((old ^ color) & ~mask));
    }
}

std::vector<std::uint8_t> seed() {
  std::vector<std::uint8_t> memory(kBytes);
  for (std::size_t i = 0; i < memory.size(); ++i)
    memory[i] = static_cast<std::uint8_t>(i * 37u + i / 251u + 19u);
  return memory;
}

bool preview(ps2vita::Gs& gs, const std::vector<std::uint8_t>& expected,
             unsigned base, unsigned width) {
  const auto* pixels = gs.pixels();
  for (int y = 0; y < ps2vita::Gs::kHeight; ++y)
    for (int x = 0; x < ps2vita::Gs::kWidth; ++x) {
      const auto color = static_cast<unsigned>(x) * kScale >= width ? 0u :
          load(expected, address(base, width, x * kScale, y * kScale));
      if (gs.pixel(x, y) != color || pixels[y * ps2vita::Gs::kWidth + x] != color)
        return false;
    }
  return true;
}

void wrapped_masks_and_watches() {
  auto expected = seed(), memory = expected, watched = expected;
  ps2vita::Gs gs, diagnostic;
  gs.set_depth_state(ps2vita::Gs::DepthTest::Always, false);
  diagnostic.set_depth_state(ps2vita::Gs::DepthTest::Always, false);
  constexpr unsigned base = 511u, width = 64u;
  const int x = 64u / kScale - 1u, y = 32u / kScale - 1u;
  std::vector<std::uint32_t> watches;
  for (unsigned row = 0; row < kScale; ++row)
    for (unsigned column = 0; column < kScale; ++column)
      watches.push_back(static_cast<std::uint32_t>(address(base, width,
          x * kScale + column, y * kScale + row) % kBytes));
  check(watches.back() == 0x3FFFFCu, "tile includes last aligned VRAM word");
  std::vector<ps2vita::GsPixelWrite> events;
  check(diagnostic.set_pixel_watch(watches, [&](const auto& event) { events.push_back(event); }),
        "all tile words watched, including last word");
  struct Operation { std::uint32_t mask, color; unsigned format; bool rgb_only; };
  const std::array<Operation, 7> operations{{
      {0u, 0xCB80100Fu, 0u, false},
      {0x5AA53CC3u, 0x12FEDCBAu, 0u, false},
      {0xFFFFFFFFu, 0xFFFFFFFFu, 0u, false},
      {0u, 0xEF0180FFu, 1u, false},
      {0x00FF00FFu, 0x43987654u, 1u, false},
      {0u, 0x08ABCDEFu, 0u, true},
      {0x0000FFFFu, 0x76543210u, 0u, false},
  }};
  for (const auto& op : operations) {
    // Distinct destination alpha requires per-word preservation, not origin caching.
    for (unsigned i = 0; i < watches.size(); ++i) {
      const auto value = ((31u + i * 13u) << 24u) | (0x00112233u + i * 0x10203u);
      store(expected, watches[i], value);
      store(memory, watches[i], value);
      store(watched, watches[i], value);
    }
    const auto before = expected;
    const auto frame = base | (1ull << 16u) | (std::uint64_t{op.format} << 24u) |
        (std::uint64_t{op.mask} << 32u);
    const auto mask = op.mask | (op.format == 1u || op.rgb_only ? 0xFF000000u : 0u);
    tile(expected, base, width, x, y, op.color, mask);
    gs.set_color_target(&memory, frame); diagnostic.set_color_target(&watched, frame);
    // ATE + ATST=NEVER + AFAIL=RGB_ONLY, or alpha testing disabled.
    gs.set_alpha_test(op.rgb_only ? 1u | (3u << 12u) : 0u);
    diagnostic.set_alpha_test(op.rgb_only ? 1u | (3u << 12u) : 0u);
    events.clear();
    gs.point({x, y, 0x87654321u, op.color});
    diagnostic.point({x, y, 0x87654321u, op.color});
    check(memory == expected && watched == expected, "entire VRAM matches masked byte oracle");
    check(memory == watched, "observer cannot change any output byte");
    bool exact = events.size() == watches.size();
    for (unsigned i = 0; i < events.size() && i < watches.size(); ++i)
      exact = exact && events[i].address == watches[i] && events[i].before == load(before, watches[i]) &&
          events[i].after == load(expected, watches[i]) && events[i].input_color == op.color &&
          events[i].frame == frame && events[i].x == x && events[i].y == y &&
          events[i].z == 0x87654321u;
    check(exact, "watch before/after is exact for every word and every mask");
    check(preview(gs, expected, base, width), "every preview pixel matches byte-address oracle");
  }
  gs.set_alpha_test(0); gs.set_color_target(&memory, base | (1ull << 16u));
  tile(expected, base, width, 0, 32u / kScale, 0x80ABCDEFu, 0u);
  gs.point({0, int(32u / kScale), 0u, 0x80ABCDEFu});
  check(memory == expected && load(memory, 0) == 0x80ABCDEFu,
        "following row wraps exactly to VRAM byte zero");
  gs.point({int(64u / kScale), 0, 0u, 0x01020304u});
  check(memory == expected, "narrow FRAME stride rejects out-of-width sample");
  gs.clear(0x76543210u);
  for (int row = 0; row < ps2vita::Gs::kHeight; ++row)
    for (int column = 0; column < ps2vita::Gs::kWidth; ++column)
      tile(expected, base, width, column, row, 0x76543210u, 0u);
  check(memory == expected, "clear follows exact wrapped tile footprints");
}

int floor128(int product) {
  const int quotient = product / 128;
  return quotient - (product < 0 && product % 128 != 0 ? 1 : 0);
}

void blend_reads_live_origin() {
  auto expected = seed(), memory = expected;
  ps2vita::Gs gs;
  gs.set_depth_state(ps2vita::Gs::DepthTest::Always, false);
  gs.set_color_target(&memory, 511u | (63ull << 16u));
  constexpr unsigned width = 63u * 64u;
  const int x = 7, y = 5;
  // (Cs - Cd) * Ad / 128 + Cd, with negative non-integral products.
  const std::uint64_t equation = (1u << 2u) | (1u << 4u) | (1u << 6u);
  for (const bool clamp : {false, true})
    for (const bool pabe : {false, true})
      for (const auto source : {0x19802003u, 0xCBE1D2F3u}) {
        const auto destination = load(expected, address(511u, width, x * kScale, y * kScale));
        auto result = source;
        if (!pabe || (source & 0x80000000u)) {
          result &= 0xFF000000u;
          for (unsigned shift = 0; shift < 24; shift += 8) {
            const int src = (source >> shift) & 255u, dst = (destination >> shift) & 255u;
            const int value = floor128((src - dst) * int(destination >> 24u)) + dst;
            const auto channel = clamp ? unsigned(std::clamp(value, 0, 255)) : unsigned(value) % 256u;
            result |= channel << shift;
          }
        }
        tile(expected, 511u, width, x, y, result, 0u);
        gs.set_blend_state(true, equation, pabe, clamp);
        gs.point({x, y, 0u, source});
        check(memory == expected, "blending consumes live exact destination origin");
      }
  check(preview(gs, expected, 511u, width), "wide wrapped preview reads exact words");
}

void same_draw_feedback() {
  auto expected = seed(), memory = expected;
  ps2vita::Gs gs;
  gs.set_depth_state(ps2vita::Gs::DepthTest::Always, false);
  gs.set_color_target(&memory, 1ull << 16u);
  unsigned samples = 0;
  gs.triangle({0, 0}, {2, 0}, {0, 2}, [&](auto, auto, auto) {
    ++samples;
    return load(memory, 0u) + 1u;
  });
  // Three anchors under the top/left rule: (0,0), (1,0), (0,1), with the
  // opposite diagonal excluded. Only the first overwrites source word zero;
  // all later samples must see that new word.
  for (int y = 0; y < 2; ++y)
    for (int x = 0; x < 2 - y; ++x)
      tile(expected, 0u, 64u, x, y, load(expected, 0u) + 1u, 0u);
  check(samples == 3u && memory == expected, "texture callback observes same-draw aliased writes");
}
} // namespace

int main() {
  wrapped_masks_and_watches(); blend_reads_live_origin(); same_draw_feedback();
  std::printf("gs_target_access scale=%u checks=%u failures=%u\n", kScale, checks, failures);
  return failures ? 1 : 0;
}
