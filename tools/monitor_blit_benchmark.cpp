#include "ps2vita/monitor_blit.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <vector>

// Owned host monitor-copy microbenchmark, NOT emulation speed or Vita FPS.
// No firmware, VBlank, screen clear or guest rendering in the timed region.
#if defined(_MSC_VER)
#define BLIT_NOINLINE __declspec(noinline)
#else
#define BLIT_NOINLINE __attribute__((noinline))
#endif

namespace {
constexpr std::size_t kWidth = 960u, kHeight = 544u;
constexpr int kOffset = 48;
constexpr unsigned kTrials = 5u;
using Source = std::array<std::uint32_t, 160u * 112u>;
using Copy = void (*)(std::uint32_t*, const std::uint32_t*);

// Preserve the old generated ARM routing: a separate rectangle call for every
// source pixel. noinline keeps host measurements of this explicit route; it
// does not predict compiler heuristics or CDRAM performance on a device.
BLIT_NOINLINE void rectangle(std::uint32_t* dst, int x, int y, std::uint32_t color) {
  for (int yy = y < 0 ? 0 : y; yy < y + 4 && yy < 544; ++yy)
    for (int xx = x < 0 ? 0 : x; xx < x + 6 && xx < 960; ++xx)
      dst[yy * 960 + xx] = color;
}

BLIT_NOINLINE void legacy(std::uint32_t* dst, const std::uint32_t* source) {
  for (int sy = 0; sy < 112; ++sy)
    for (int sx = 0; sx < 160; ++sx)
      rectangle(dst, sx * 6, kOffset + sy * 4, source[sy * 160 + sx]);
}

BLIT_NOINLINE void candidate(std::uint32_t* dst, const std::uint32_t* source) {
  ps2vita::blit_monitor_160x112(dst, kWidth * kHeight, kWidth, source, kOffset);
}

Source make_source() {
  Source source{};
  for (std::size_t at = 0u; at < source.size(); ++at)
    source[at] = static_cast<std::uint32_t>(at * 2654435761u) ^ 0x5F12AB87u;
  return source;
}

bool run(Copy copy, unsigned frames, std::uint64_t& elapsed) {
  auto source = make_source();
  std::vector<std::uint32_t> destination(kWidth * kHeight, 0xDEADBEEFu);
  const auto begin = std::chrono::steady_clock::now();
  for (unsigned frame = 0u; frame < frames; ++frame) {
    source[0] = 0x12345678u ^ (frame * 17u);
    copy(destination.data(), source.data());
    // Observable live result, plus complete independent image validation below.
    const volatile std::uint32_t* first = destination.data() + kOffset * kWidth;
    if (*first != source[0]) return false;
  }
  const auto end = std::chrono::steady_clock::now();
  const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin).count();
  if (ns < 0) return false;
  elapsed = static_cast<std::uint64_t>(ns);
  bool matching = true;
  for (std::size_t y = 0u; y < kHeight; ++y)
    for (std::size_t x = 0u; x < kWidth; ++x) {
      const auto expected = y >= 48u && y < 496u ?
          source[((y - 48u) / 4u) * 160u + x / 6u] : 0xDEADBEEFu;
      matching = matching && destination[y * kWidth + x] == expected;
    }
  return matching;
}

bool parse_frames(const char* text, unsigned& frames) {
  if (*text == '\0') return false;
  unsigned value = 0u;
  for (; *text; ++text) {
    if (*text < '0' || *text > '9') return false;
    const auto digit = static_cast<unsigned>(*text - '0');
    if (value > (5000u - digit) / 10u) return false;
    value = value * 10u + digit;
  }
  if (value == 0u || value > 5000u) return false;
  frames = value; return true;
}
} // namespace

int main(int argc, char** argv) {
  unsigned frames = 256u;
  if (argc > 2 || (argc == 2 && !parse_frames(argv[1], frames))) {
    std::fputs("usage: monitor_blit_benchmark [FRAMES_PER_TRIAL: 1..5000]\n", stderr);
    return 2;
  }
  std::uint64_t warmup = 0u;
  if (!run(legacy, 1u, warmup) || !run(candidate, 1u, warmup)) return 1;
  std::array<std::uint64_t, kTrials> baseline{}, refined{};
  for (unsigned trial = 0u; trial < kTrials; ++trial) {
    const bool ok = trial % 2u == 0u ?
        run(legacy, frames, baseline[trial]) && run(candidate, frames, refined[trial]) :
        run(candidate, frames, refined[trial]) && run(legacy, frames, baseline[trial]);
    if (!ok) { std::fputs("complete-image copy oracle failed\n", stderr); return 1; }
    std::printf("trial=%u frames=%u rectangle_ns=%llu row_write_ns=%llu exact=1\n", trial + 1u, frames,
        static_cast<unsigned long long>(baseline[trial]), static_cast<unsigned long long>(refined[trial]));
  }
  std::sort(baseline.begin(), baseline.end()); std::sort(refined.begin(), refined.end());
  std::printf("scope=host-monitor-copy-only trials=5 warmup=1 frames_per_trial=%u "
      "rectangle_median_ns=%llu row_write_median_ns=%llu exact=1 not_vita_fps=1\n", frames,
      static_cast<unsigned long long>(baseline[2]), static_cast<unsigned long long>(refined[2]));
  return 0;
}
