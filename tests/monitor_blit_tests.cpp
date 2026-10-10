#include "ps2vita/monitor_blit.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <vector>

namespace {
unsigned checks = 0u, failures = 0u;
constexpr std::size_t kGuard = 19u, kWidth = 960u, kHeight = 544u;
constexpr std::uint32_t kUntouched = 0xA55A37E1u;

void check(bool ok, const char* name) {
  ++checks;
  if (!ok) { ++failures; std::fprintf(stderr, "FAIL: %s\n", name); }
}

std::array<std::uint32_t, 160u * 112u> source_pixels() {
  std::array<std::uint32_t, 160u * 112u> pixels{};
  for (unsigned y = 0u; y < 112u; ++y)
    for (unsigned x = 0u; x < 160u; ++x)
      pixels[y * 160u + x] = ((x * 73u + y * 29u) << 24u) |
          ((y * 31u & 255u) << 16u) | ((x * 11u & 255u) << 8u) | ((x ^ y) & 255u);
  return pixels;
}

void full_surface_oracles() {
  const auto source = source_pixels();
  constexpr std::array<int, 17> offsets{{
      std::numeric_limits<int>::min(), -1000, -448, -447, -443, -4, -3, -1,
      0, 48, 96, 543, 544, 545, 1000, std::numeric_limits<int>::max() - 3,
      std::numeric_limits<int>::max()}};
  for (const std::size_t pitch : {960u, 967u, 1024u}) {
    std::vector<std::uint32_t> storage(kGuard + pitch * kHeight + kGuard);
    for (const auto offset : offsets) {
      std::fill(storage.begin(), storage.end(), kUntouched);
      auto* surface = storage.data() + kGuard;
      const auto result = ps2vita::blit_monitor_160x112(surface, pitch * kHeight,
          pitch, source.data(), offset);
      bool matching = true;
      for (std::size_t y = 0u; y < kHeight; ++y)
        for (std::size_t x = 0u; x < pitch; ++x) {
          const auto source_row = static_cast<std::int64_t>(y) - offset;
          const bool visible = x < kWidth && source_row >= 0 && source_row < 448;
          const auto expected = visible ? source[static_cast<std::size_t>(source_row / 4) * 160u + x / 6u] : kUntouched;
          matching = matching && surface[y * pitch + x] == expected;
        }
      check(result && matching,
          "every visible color/alpha word, clipped row and pitch padding matches independent pixels");
      bool guards = true;
      for (std::size_t at = 0u; at < kGuard; ++at)
        guards = guards && storage[at] == kUntouched &&
            storage[kGuard + pitch * kHeight + at] == kUntouched;
      check(guards, "surface prefix/suffix canaries remain intact for every offset and pitch");
    }
  }
}

void invalid_surfaces() {
  const auto source = source_pixels();
  std::vector<std::uint32_t> pixels(kWidth * kHeight + 2u * kGuard, kUntouched);
  auto* surface = pixels.data() + kGuard;
  const auto unchanged = [&]() {
    bool ok = true;
    for (auto value : pixels) ok = ok && value == kUntouched;
    return ok;
  };
  check(!ps2vita::blit_monitor_160x112(nullptr, kWidth * kHeight, kWidth, source.data()),
      "null destination is rejected");
  check(!ps2vita::blit_monitor_160x112(surface, kWidth * kHeight, kWidth, nullptr) && unchanged(),
      "null source cannot change the surface");
  for (std::size_t pitch : {0u, 1u, 959u})
    check(!ps2vita::blit_monitor_160x112(surface, kWidth * kHeight, pitch, source.data()) && unchanged(),
        "zero/narrow pitch is rejected without division by zero or writes");
  for (std::size_t capacity : {std::size_t{0u}, kWidth, kWidth * kHeight - 1u})
    check(!ps2vita::blit_monitor_160x112(surface, capacity, kWidth, source.data()) && unchanged(),
        "short capacity cannot write a partial/undersized surface");
  for (std::size_t pitch : {std::numeric_limits<std::size_t>::max(),
                          std::numeric_limits<std::size_t>::max() / 544u + 1u})
    check(!ps2vita::blit_monitor_160x112(surface, std::numeric_limits<std::size_t>::max(),
        pitch, source.data()) && unchanged(), "overflowing pitch-times-height is rejected before pointer arithmetic");
}

void reused_surface() {
  auto source = source_pixels();
  std::vector<std::uint32_t> pixels(kWidth * kHeight, kUntouched);
  check(ps2vita::blit_monitor_160x112(pixels.data(), pixels.size(), kWidth, source.data(), 48),
      "initial reused-surface copy succeeds");
  auto expected = pixels;
  for (std::size_t at = 0u; at < source.size(); ++at) source[at] ^= 0xF00F55AAu;
  // An independently calculated overlay retains the first image outside the
  // second visible region; no clear or persistent expanded-row cache is assumed.
  for (std::size_t y = 0u; y < 445u; ++y)
    for (std::size_t x = 0u; x < kWidth; ++x)
      expected[y * kWidth + x] = source[((y + 3u) / 4u) * 160u + x / 6u];
  check(ps2vita::blit_monitor_160x112(pixels.data(), pixels.size(), kWidth, source.data(), -3) &&
      pixels == expected, "later clipped copy reads current source and preserves rows outside its footprint");
  const auto before = pixels;
  check(ps2vita::blit_monitor_160x112(pixels.data(), pixels.size(), kWidth, source.data(), 544) &&
      pixels == before, "fully offscreen valid copy is a no-write success");
}
} // namespace

int main() {
  full_surface_oracles(); invalid_surfaces(); reused_surface();
  std::printf("Monitor blit tests: %u checks, %u failures\n", checks, failures);
  return failures == 0u ? 0 : 1;
}
