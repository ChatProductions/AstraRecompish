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

using Sample = std::array<int, 2>;

std::vector<Sample> target_samples(unsigned base, unsigned width) {
  const unsigned visible_width = std::min(width, 640u);
  const int last_x = static_cast<int>(visible_width / kScale) - 1;
  const int last_y = ps2vita::Gs::kHeight - 1;
  std::vector<Sample> samples;
  const auto add = [&](int x, int y) {
    if (x < 0 || x > last_x || y < 0 || y > last_y) return;
    const Sample sample{x, y};
    if (std::find(samples.begin(), samples.end(), sample) == samples.end())
      samples.push_back(sample);
  };
  for (const int y : {0, 1, last_y})
    for (const int x : {0, 1, last_x}) add(x, y);
  add(std::min(7, last_x), std::min(5, last_y));

  // Derive edge cases from the independent 64-bit linear address, rather
  // than duplicating the production row increment or power-of-two wrap.
  for (unsigned y = 0; y < 448u; ++y) {
    if (y != 0u && address(base, width, 0u, y) / kBytes !=
        address(base, width, 0u, y - 1u) / kBytes) {
      for (const int x : {0, last_x}) {
        add(x, static_cast<int>((y - 1u) / kScale));
        add(x, static_cast<int>(y / kScale));
      }
    }
    for (unsigned x = 0; x < visible_width; ++x) {
      const auto at = address(base, width, x, y) % kBytes;
      if (at != 0u && at != kBytes - 4u) continue;
      const int sample_x = static_cast<int>(x / kScale);
      const int sample_y = static_cast<int>(y / kScale);
      add(sample_x - 1, sample_y);
      add(sample_x, sample_y);
      add(sample_x + 1, sample_y);
    }
  }
  return samples;
}

void check_target(bool ok, const char* label, unsigned base, unsigned fbw,
                  unsigned operation) {
  char detail[160];
  std::snprintf(detail, sizeof(detail), "%s (base=%u FBW=%u operation=%u)",
                label, base, fbw, operation);
  check(ok, detail);
}

void stride_and_wrap_matrix() {
  auto expected = seed(), memory = expected;
  ps2vita::Gs gs;
  gs.set_depth_state(ps2vita::Gs::DepthTest::Always, false);
  struct Operation { std::uint32_t mask, color; unsigned format; bool rgb_only; };
  const std::array<Operation, 7> operations{{
      {0u, 0x01FEE180u, 0u, false},
      {0u, 0xFE102345u, 0u, false},
      {0x5AA53CC3u, 0x817E01FFu, 0u, false},
      {0xFFFFFFFFu, 0xFFFFFFFFu, 0u, false},
      {0u, 0xEA735291u, 1u, false},
      {0x00FF00FFu, 0x43987654u, 1u, false},
      {0x00000FF0u, 0x08ABCDEFu, 0u, true},
  }};
  for (const unsigned base : {0u, 511u})
    for (const unsigned fbw : {1u, 2u, 10u, 63u}) {
      const unsigned width = fbw * 64u;
      const auto samples = target_samples(base, width);
      for (unsigned operation = 0; operation < operations.size(); ++operation) {
        const auto& op = operations[operation];
        // Every word has its own destination alpha. Some selected tiles
        // overlap; seed them in order before applying any color operation.
        for (unsigned i = 0; i < samples.size(); ++i)
          for (unsigned row = 0; row < kScale; ++row)
            for (unsigned column = 0; column < kScale; ++column) {
              const auto alpha = (17u + i * 61u + row * 43u + column * 29u) & 255u;
              const auto rgb = (0x00765432u ^ (i * 0x10203u + row * 0x504u + column)) &
                  0x00FFFFFFu;
              const auto at = address(base, width, samples[i][0] * kScale + column,
                                      samples[i][1] * kScale + row);
              store(expected, at, (alpha << 24u) | rgb);
              store(memory, at, (alpha << 24u) | rgb);
            }
        const auto frame = base | (std::uint64_t{fbw} << 16u) |
            (std::uint64_t{op.format} << 24u) | (std::uint64_t{op.mask} << 32u);
        const auto mask = op.mask | (op.format == 1u || op.rgb_only ? 0xFF000000u : 0u);
        gs.set_color_target(&memory, frame);
        gs.set_alpha_test(op.rgb_only ? 1u | (3u << 12u) : 0u);
        // Reverse the order on alternate operations to exercise subsequent
        // writes to already-live destination words as well as first writes.
        for (unsigned step = 0; step < samples.size(); ++step) {
          const unsigned i = operation % 2u ? samples.size() - 1u - step : step;
          const auto color = op.color ^ (i * 0x01030507u);
          tile(expected, base, width, samples[i][0], samples[i][1], color, mask);
          gs.point({samples[i][0], samples[i][1], 0u, color});
        }
        check_target(memory == expected, "stride/wrap writes match every VRAM byte",
                     base, fbw, operation);
        check_target(preview(gs, expected, base, width), "stride/wrap preview matches oracle",
                     base, fbw, operation);
      }
      // Clear intentionally ignores FBMSK and PSMCT24 alpha preservation.
      // Keep the preceding RGB_ONLY state active to cover that bypass too.
      gs.set_color_target(&memory, base | (std::uint64_t{fbw} << 16u) |
          (1ull << 24u) | (0xFFFFFFFFull << 32u));
      constexpr auto clear_color = 0xA7D34985u;
      gs.clear(clear_color);
      for (int y = 0; y < ps2vita::Gs::kHeight; ++y)
        for (int x = 0; x < ps2vita::Gs::kWidth; ++x)
          tile(expected, base, width, x, y, clear_color, 0u);
      check_target(memory == expected, "PSMCT24/full-mask clear matches every VRAM byte",
                   base, fbw, operations.size());
      check_target(preview(gs, expected, base, width), "clear preview matches oracle",
                   base, fbw, operations.size());
    }
}

void unsupported_bindings_use_host_buffer() {
  auto memory = seed(), small = memory, large = memory;
  small.pop_back(); large.push_back(0xA5u);
  const auto before = memory, small_before = small, large_before = large;
  ps2vita::Gs gs;
  gs.set_depth_state(ps2vita::Gs::DepthTest::Always, false);
  struct Binding { std::vector<std::uint8_t>* memory; std::uint64_t frame; };
  const std::array<Binding, 6> bindings{{
      {nullptr, 511u | (63ull << 16u)},
      {&memory, 511u},
      {&memory, 511u | (1ull << 16u) | (2ull << 24u)},
      {&memory, 511u | (63ull << 16u) | (63ull << 24u)},
      {&small, 511u | (1ull << 16u)},
      {&large, 511u | (63ull << 16u)},
  }};
  for (const auto& binding : bindings) {
    // Rebind from a valid target so a stale cached pointer cannot pass.
    gs.set_color_target(&memory, 511u | (63ull << 16u));
    gs.set_color_target(binding.memory, binding.frame);
    gs.point({2, 3, 0u, 0xCAFEBABEu});
    check(memory == before && small == small_before && large == large_before,
          "unsupported point binding cannot write any supplied memory");
    check(gs.color_frame() == binding.frame && gs.pixel(2, 3) == 0xCAFEBABEu &&
          gs.pixels()[3 * ps2vita::Gs::kWidth + 2] == 0xCAFEBABEu,
          "unsupported binding retains host point and preview behavior");
    gs.clear(0xD18E274Bu);
    check(memory == before && small == small_before && large == large_before,
          "unsupported clear binding cannot write any supplied memory");
    const auto* pixels = gs.pixels();
    check(std::all_of(pixels, pixels + ps2vita::Gs::kWidth * ps2vita::Gs::kHeight,
                      [](auto color) { return color == 0xD18E274Bu; }),
          "unsupported binding clear fills the host buffer");
  }
}

void valid_target_pointer_switches() {
  auto expected_a = seed(), memory_a = expected_a;
  auto expected_b = seed();
  for (auto& byte : expected_b) byte ^= 0xA5u;
  auto memory_b = expected_b;
  ps2vita::Gs gs;
  gs.set_depth_state(ps2vita::Gs::DepthTest::Always, false);
  const auto draw = [&](auto& memory, auto& expected, unsigned base, unsigned fbw,
                        unsigned format, std::uint32_t mask, std::uint32_t color,
                        unsigned native_x, unsigned native_y) {
    const auto frame = base | (std::uint64_t{fbw} << 16u) |
        (std::uint64_t{format} << 24u) | (std::uint64_t{mask} << 32u);
    const int x = static_cast<int>(native_x / kScale);
    const int y = static_cast<int>(native_y / kScale);
    gs.set_color_target(&memory, frame);
    tile(expected, base, fbw * 64u, x, y, color,
         mask | (format == 1u ? 0xFF000000u : 0u));
    gs.point({x, y, 0u, color});
    check(memory_a == expected_a, "valid target switch preserves every A VRAM byte");
    check(memory_b == expected_b, "valid target switch preserves every B VRAM byte");
    check(preview(gs, expected, base, fbw * 64u), "valid target switch preview uses bound buffer");
  };
  // Switch A -> B -> A, with distinct byte seeds and different base, stride,
  // format and mask. Both A operations include the same last physical word,
  // so the final masked write must also consume A's live destination bytes.
  draw(memory_a, expected_a, 511u, 1u, 0u, 0u, 0xF1E2D3C4u, 63u, 31u);
  draw(memory_b, expected_b, 0u, 63u, 0u, 0x5AA53CC3u, 0x09ABCDEFu, 255u, 260u);
  draw(memory_a, expected_a, 511u, 10u, 1u, 0x00FF00FFu, 0x3388EE44u, 127u, 3u);
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
  wrapped_masks_and_watches(); stride_and_wrap_matrix();
  unsupported_bindings_use_host_buffer(); valid_target_pointer_switches();
  blend_reads_live_origin(); same_draw_feedback();
  std::printf("gs_target_access scale=%u checks=%u failures=%u\n", kScale, checks, failures);
  return failures ? 1 : 0;
}
