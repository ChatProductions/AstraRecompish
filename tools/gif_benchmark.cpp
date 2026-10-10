#include "ps2vita/gif.hpp"
#include "ps2vita/gif_trace.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <new>
#include <tuple>
#include <utility>
#include <vector>

namespace {

bool parse_samples(const char* text, unsigned& samples) {
  if (*text == '\0') return false;
  samples = 0u;
  for (; *text; ++text) {
    if (*text < '0' || *text > '9') return false;
    const unsigned digit = static_cast<unsigned>(*text - '0');
    if (samples > (100u - digit) / 10u) return false;
    samples = samples * 10u + digit;
  }
  return samples != 0u && samples <= 100u;
}

struct State {
  std::uint64_t framebuffer_hash = 1469598103934665603ull;
  std::uint64_t vram_hash = 1469598103934665603ull;
  std::uint64_t frame = 0u, packets = 0u, rejected = 0u, pending = 0u;
  std::uint64_t sprites = 0u, points = 0u, lines = 0u, triangles = 0u;
  std::uint64_t packed_tags = 0u, reglist_tags = 0u, image_tags = 0u;
  std::uint64_t image_bytes = 0u, local_bytes_written = 0u;

  bool operator==(const State& other) const {
    return std::tie(framebuffer_hash, vram_hash, frame, packets, rejected, pending,
                    sprites, points, lines, triangles, packed_tags, reglist_tags,
                    image_tags, image_bytes, local_bytes_written) ==
        std::tie(other.framebuffer_hash, other.vram_hash, other.frame, other.packets,
                 other.rejected, other.pending, other.sprites, other.points,
                 other.lines, other.triangles, other.packed_tags, other.reglist_tags,
                 other.image_tags, other.image_bytes, other.local_bytes_written);
  }
};

bool run_sample(const std::vector<ps2vita::GifTraceRecord>& records,
                State& state, std::uint64_t& nanoseconds) {
  // Construction, allocation and reset are deliberately outside the interval.
  // Reusing Gif::reset alone would retain Gs depth/cache state between trials.
  ps2vita::Gs gs;
  ps2vita::Gif gif(gs);
  gif.reset();
  bool accepted = true;
  const auto start = std::chrono::steady_clock::now();
  for (const auto& record : records)
    accepted = gif.submit(record.payload.data(), record.payload.size()) && accepted;
  const auto finish = std::chrono::steady_clock::now();
  const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(finish - start).count();
  if (elapsed < 0 || (elapsed == 0 && !records.empty())) {
    std::fputs("graphics benchmark clock did not advance\n", stderr);
    return false;
  }
  nanoseconds = static_cast<std::uint64_t>(elapsed);
  state.frame = gs.color_frame();
  state.packets = gif.packets_submitted(); state.rejected = gif.packets_rejected();
  state.pending = gif.pending_bytes(); state.sprites = gif.sprites_emitted();
  state.points = gif.points_emitted(); state.lines = gif.lines_emitted();
  state.triangles = gif.triangles_emitted(); state.packed_tags = gif.packed_tags();
  state.reglist_tags = gif.reglist_tags(); state.image_tags = gif.image_tags();
  state.image_bytes = gif.image_bytes(); state.local_bytes_written = gif.local_bytes_written();
  if (!accepted || state.rejected != 0u || state.pending != 0u) {
    std::fprintf(stderr, "graphics benchmark submission failed: accepted=%u rejected=%llu pending=%llu\n",
        unsigned(accepted), static_cast<unsigned long long>(state.rejected),
        static_cast<unsigned long long>(state.pending));
    return false;
  }
  // Hash every raw aligned logical-VRAM word and the complete active preview,
  // including alpha. No output, observer or hash work is timed.
  for (int y = 0; y < ps2vita::Gs::kHeight; ++y)
    for (int x = 0; x < ps2vita::Gs::kWidth; ++x) {
      state.framebuffer_hash ^= gs.pixel(x, y);
      state.framebuffer_hash *= 1099511628211ull;
    }
  for (std::uint32_t address = 0u; address < 0x400000u; address += 4u) {
    state.vram_hash ^= gif.read_local32(address);
    state.vram_hash *= 1099511628211ull;
  }
  return true;
}

int benchmark(int argc, char** argv) {
  unsigned samples = 0u;
  if (argc != 3 || argv[1][0] == '\0' || !parse_samples(argv[2], samples)) {
    std::fputs("usage: ps2gif_benchmark TRACE SAMPLES(1..100)\n"
               "host graphics-submit throughput only; not guest/core/frame/Vita FPS\n", stderr);
    return 2;
  }
  std::ifstream input(argv[1], std::ios::binary);
  ps2vita::GifTraceReader reader(input);
  std::vector<ps2vita::GifTraceRecord> records;
  ps2vita::GifTraceRecord record;
  while (reader.next(record)) records.push_back(std::move(record));
  if (!reader.good() || !reader.finished()) {
    std::fprintf(stderr, "graphics benchmark trace validation failed: %s\n", reader.error());
    return 2;
  }
  // Entire bounded container/footer validated before any warmup or sample.
  const auto payload_bytes = reader.payload_bytes();
  State expected;
  std::uint64_t warmup_ns = 0u;
  if (!run_sample(records, expected, warmup_ns)) return 1;
  std::vector<std::uint64_t> timings;
  timings.reserve(samples);
  for (unsigned sample = 1u; sample <= samples; ++sample) {
    State actual;
    std::uint64_t nanoseconds = 0u;
    if (!run_sample(records, actual, nanoseconds)) return 1;
    if (!(actual == expected)) {
      std::fprintf(stderr, "graphics benchmark state diverged from warmup at sample %u\n", sample);
      return 1;
    }
    timings.push_back(nanoseconds);
    std::printf("gif_benchmark_sample index=%u submit_ns=%llu\n", sample,
        static_cast<unsigned long long>(nanoseconds));
  }
  std::sort(timings.begin(), timings.end());
  const auto middle = timings.size() / 2u;
  const auto median = timings.size() % 2u != 0u ? timings[middle] :
      timings[middle - 1u] + (timings[middle] - timings[middle - 1u]) / 2u;
  const bool empty = records.empty();
  const double bytes_per_second = !empty && median != 0u ?
      static_cast<double>(payload_bytes) * 1.0e9 / static_cast<double>(median) : 0.0;
  const double submissions_per_second = !empty && median != 0u ?
      static_cast<double>(records.size()) * 1.0e9 / static_cast<double>(median) : 0.0;
  std::printf("gif_benchmark validated=1 matched=1 scope=host-gif-submit-throughput-not-guest-core-frame-vita-fps "
              "sample_scale=%u warmups=1 samples=%u empty_workload=%u records=%llu payload_bytes=%llu "
              "terminal_ee_cycle=%llu terminal_next_ee_pc=%08X\n",
      unsigned(ps2vita::Gs::kRasterScale), samples, unsigned(empty),
      static_cast<unsigned long long>(reader.record_count()),
      static_cast<unsigned long long>(payload_bytes),
      static_cast<unsigned long long>(reader.terminal_cycle()), reader.terminal_pc());
  std::printf("gif_benchmark_timing median_ns=%llu min_ns=%llu max_ns=%llu "
              "payload_bytes_per_second=%.3f submissions_per_second=%.3f\n",
      static_cast<unsigned long long>(median), static_cast<unsigned long long>(timings.front()),
      static_cast<unsigned long long>(timings.back()), bytes_per_second, submissions_per_second);
  std::printf("gif_benchmark_state FRAME=%016llX framebuffer_hash=%016llX "
              "logical_vram_word_hash=%016llX gif_packets=%llu rejected=%llu pending=%llu "
              "sprites=%llu points=%llu lines=%llu triangles=%llu packed_tags=%llu "
              "reglist_tags=%llu image_tags=%llu image_bytes=%llu local_bytes_written=%llu\n",
      static_cast<unsigned long long>(expected.frame),
      static_cast<unsigned long long>(expected.framebuffer_hash),
      static_cast<unsigned long long>(expected.vram_hash),
      static_cast<unsigned long long>(expected.packets), static_cast<unsigned long long>(expected.rejected),
      static_cast<unsigned long long>(expected.pending), static_cast<unsigned long long>(expected.sprites),
      static_cast<unsigned long long>(expected.points), static_cast<unsigned long long>(expected.lines),
      static_cast<unsigned long long>(expected.triangles), static_cast<unsigned long long>(expected.packed_tags),
      static_cast<unsigned long long>(expected.reglist_tags), static_cast<unsigned long long>(expected.image_tags),
      static_cast<unsigned long long>(expected.image_bytes),
      static_cast<unsigned long long>(expected.local_bytes_written));
  return 0;
}

} // namespace

int main(int argc, char** argv) {
  try {
    return benchmark(argc, argv);
  } catch (const std::bad_alloc&) {
    std::fputs("graphics benchmark allocation failed\n", stderr);
    return 2;
  }
}
