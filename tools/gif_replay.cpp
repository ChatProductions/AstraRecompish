#include "ps2vita/framebuffer_dump.hpp"
#include "ps2vita/gif.hpp"
#include "ps2vita/gif_trace.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <string>
#include <vector>

namespace {

constexpr std::uint64_t kTriangleExportLimit = 100000u;

struct Options {
  const char* trace = nullptr;
  const char* framebuffer = nullptr;
  const char* sprite_framebuffer = nullptr;
  const char* sprite_source = nullptr;
  const char* triangles = nullptr;
  bool have_sprite = false;
  std::uint64_t sprite = 0, stop_packet = 0;
};

bool parse_count(const char* text, std::uint64_t maximum, std::uint64_t& value) {
  if (*text == '\0') return false;
  value = 0;
  for (; *text; ++text) {
    if (*text < '0' || *text > '9') return false;
    const auto digit = static_cast<unsigned>(*text - '0');
    if (digit > maximum || value > (maximum - digit) / 10u) return false;
    value = value * 10u + digit;
  }
  return true;
}

bool same_path(const char* left, const char* right) {
  std::error_code error;
  if (std::filesystem::equivalent(left, right, error)) return true;
  const auto first = std::filesystem::weakly_canonical(left, error);
  if (error) return std::string(left) == right;
  const auto second = std::filesystem::weakly_canonical(right, error);
  return error ? std::string(left) == right : first == second;
}

bool parse_options(int argc, char** argv, Options& options) {
  if (argc < 2 || argv[1][0] == '\0' || argv[1][0] == '-') return false;
  options.trace = argv[1];
  bool have_stop_packet = false;
  for (int index = 2; index < argc; index += 2) {
    if (index + 1 == argc) return false;
    const std::string name(argv[index]);
    const char* value = argv[index + 1];
    if (name == "--sprite") {
      if (options.have_sprite ||
          !parse_count(value, std::numeric_limits<std::uint64_t>::max(), options.sprite))
        return false;
      options.have_sprite = true;
    } else if (name == "--stop-packet") {
      if (have_stop_packet || !parse_count(value,
              ps2vita::gif_trace_detail::max_records, options.stop_packet) ||
          options.stop_packet == 0u) return false;
      have_stop_packet = true;
    } else {
      if (*value == '\0' || std::string(value) == "-") return false;
      const char** target = name == "--framebuffer" ? &options.framebuffer :
          name == "--sprite-framebuffer" ? &options.sprite_framebuffer :
          name == "--sprite-source" ? &options.sprite_source :
          name == "--triangles" ? &options.triangles : nullptr;
      if (!target || *target) return false;
      *target = value;
    }
  }
  if (options.have_sprite != (options.sprite_framebuffer != nullptr) ||
      options.have_sprite != (options.sprite_source != nullptr)) return false;
  const std::vector<const char*> paths{options.trace, options.framebuffer,
      options.sprite_framebuffer, options.sprite_source, options.triangles};
  for (std::size_t index = 1; index < paths.size(); ++index) {
    if (!paths[index]) continue;
    for (std::size_t previous = 0; previous < index; ++previous)
      if (paths[previous] && same_path(paths[index], paths[previous])) return false;
  }
  return true;
}

void hex_word(std::ostream& output, std::uint64_t value, int digits = 16) {
  output << std::uppercase << std::hex << std::setw(digits) << std::setfill('0')
         << value << std::dec;
}

void triangle_header(std::ostream& output) {
  output << "sequence\tpacket\tee_cycle\tnext_ee_pc\tprim\txyoffset\tscissor\t"
            "test\tzbuf\ttex0\tclamp\tframe\talpha";
  for (unsigned vertex = 0; vertex < 3u; ++vertex)
    for (const char* field : {"x", "y", "z", "color", "st", "uv", "q", "xyz"})
      output << "\tv" << vertex << '_' << field;
  output << '\n';
}

void triangle_row(std::ostream& output, const ps2vita::GifTriangleRecord& triangle,
                   const ps2vita::GifTraceRecord& submission) {
  output << triangle.sequence << '\t' << submission.ordinal << '\t'
         << submission.ee_cycle << '\t';
  hex_word(output, submission.next_ee_pc, 8);
  for (const auto value : {triangle.prim, triangle.xyoffset, triangle.scissor,
                          triangle.test, triangle.zbuf, triangle.tex0,
                          triangle.clamp, triangle.frame, triangle.alpha}) {
    output << '\t';
    hex_word(output, value);
  }
  for (unsigned index = 0; index < 3u; ++index) {
    const auto& vertex = triangle.vertices[index];
    output << '\t' << vertex.x << '\t' << vertex.y << '\t' << vertex.z << '\t';
    hex_word(output, vertex.color, 8); output << '\t';
    hex_word(output, vertex.st); output << '\t';
    hex_word(output, vertex.uv); output << '\t';
    hex_word(output, vertex.q, 8); output << '\t';
    hex_word(output, triangle.xyz[index]);
  }
  output << '\n';
}

bool write_pixels(const char* path, const std::vector<std::uint32_t>& pixels,
                   unsigned width, unsigned height) {
  if (pixels.size() != std::size_t{width} * height) return false;
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output << "P6\n" << width << ' ' << height << "\n255\n";
  for (const auto pixel : pixels) {
    const char rgb[]{static_cast<char>(pixel & 0xFFu),
                     static_cast<char>((pixel >> 8u) & 0xFFu),
                     static_cast<char>((pixel >> 16u) & 0xFFu)};
    output.write(rgb, sizeof(rgb));
  }
  output.close();
  return static_cast<bool>(output);
}

} // namespace

int main(int argc, char** argv) {
  Options options;
  if (!parse_options(argc, argv, options)) {
    std::fputs("usage: ps2gif_replay TRACE [--framebuffer FILE] "
               "[--sprite N --sprite-framebuffer FILE --sprite-source FILE] "
               "[--stop-packet N] [--triangles FILE]\n"
               "counts are unsigned decimal; stop-packet is 1..100000; "
               "output paths must be distinct from each other and TRACE\n", stderr);
    return 2;
  }
  std::ifstream input(options.trace, std::ios::binary);
  ps2vita::GifTraceReader reader(input);
  if (!reader.good()) {
    std::fprintf(stderr, "invalid serialized Astra GIF trace: %s\n", reader.error());
    return 2;
  }
  ps2vita::Gs gs;
  ps2vita::Gif gif(gs);
  gif.reset();
  if (options.have_sprite) gif.capture_sprite_framebuffer_at(options.sprite);
  std::ofstream triangles;
  std::uint64_t exported_triangles = 0;
  bool triangle_export_failed = false;
  ps2vita::GifTraceRecord submission;
  if (options.triangles) {
    triangles.open(options.triangles, std::ios::trunc);
    if (!triangles) {
      std::fprintf(stderr, "could not open triangle TSV: %s\n", options.triangles);
      return 2;
    }
    triangle_header(triangles);
    gif.set_triangle_observer([&](const ps2vita::GifTriangleRecord& triangle) {
      if (triangle_export_failed) return;
      if (exported_triangles == kTriangleExportLimit) {
        triangle_export_failed = true;
        return;
      }
      triangle_row(triangles, triangle, submission);
      ++exported_triangles;
      if (!triangles) triangle_export_failed = true;
    });
  }

  std::uint64_t replayed_packets = 0, replayed_bytes = 0;
  std::uint64_t replay_ee_cycle = 0;
  std::uint32_t replay_next_pc = 0;
  const char* stop_reason = "end-of-trace";
  bool stopped = false, accepted = true;
  while (reader.next(submission)) {
    if (stopped) continue; // Still validate every later record and the footer.
    accepted = gif.submit(submission.payload.data(), submission.payload.size()) && accepted;
    ++replayed_packets;
    replayed_bytes += submission.payload.size();
    replay_ee_cycle = submission.ee_cycle;
    replay_next_pc = submission.next_ee_pc;
    if (triangle_export_failed) {
      stopped = true;
      stop_reason = "triangle-export-failed";
    } else if (options.have_sprite && gif.sprite_framebuffer_captured()) {
      stopped = true;
      stop_reason = "sprite-capture-submission-boundary";
    } else if (options.stop_packet && replayed_packets == options.stop_packet) {
      stopped = true;
      stop_reason = "packet-prefix";
    }
  }
  gif.set_triangle_observer({});
  if (!reader.good() || !reader.finished()) {
    std::fprintf(stderr, "serialized Astra GIF trace validation failed: %s; "
                        "any triangle TSV is incomplete\n", reader.error());
    return 2;
  }
  if (triangle_export_failed) {
    std::fprintf(stderr, "triangle TSV incomplete: write failure or more than "
                        "100000 triangles; trace footer was validated\n");
    return 2;
  }
  if (options.stop_packet && reader.record_count() < options.stop_packet) {
    std::fputs("requested stop-packet exceeds trace record count\n", stderr);
    return 2;
  }
  if (options.have_sprite && !gif.sprite_framebuffer_captured()) {
    std::fprintf(stderr, "sprite sequence %llu was not reached in the replayed prefix\n",
        static_cast<unsigned long long>(options.sprite));
    return 2;
  }
  if (options.have_sprite && gif.sprite_texture_capture().empty()) {
    std::fputs("selected sprite has no supported bounded PSMCT32/24 source capture\n", stderr);
    return 2;
  }
  if (options.triangles) {
    triangles << "# complete triangles=" << exported_triangles
              << " replay_packets=" << replayed_packets
              << " trace_packets=" << reader.record_count() << '\n';
    triangles.close();
    if (!triangles) {
      std::fprintf(stderr, "could not finish triangle TSV: %s\n", options.triangles);
      return 2;
    }
  }
  if (options.framebuffer) {
    std::ofstream output(options.framebuffer, std::ios::binary | std::ios::trunc);
    const bool written = ps2vita::write_framebuffer_ppm(output, gs);
    output.close();
    if (!written || !output) {
      std::fprintf(stderr, "could not write replay framebuffer: %s\n", options.framebuffer);
      return 2;
    }
  }
  if (options.have_sprite &&
      (!write_pixels(options.sprite_framebuffer, gif.sprite_framebuffer_capture(),
                     ps2vita::Gs::kWidth, ps2vita::Gs::kHeight) ||
       !write_pixels(options.sprite_source, gif.sprite_texture_capture(),
                     ps2vita::Gif::kSpriteSourceProbeWidth,
                     ps2vita::Gif::kSpriteSourceProbeHeight))) {
    std::fputs("could not write selected sprite framebuffer/source PPM\n", stderr);
    return 2;
  }

  std::uint64_t framebuffer_hash = 1469598103934665603ull;
  std::uint64_t vram_hash = 1469598103934665603ull;
  std::size_t nonzero_rgb = 0;
  for (int y = 0; y < ps2vita::Gs::kHeight; ++y)
    for (int x = 0; x < ps2vita::Gs::kWidth; ++x) {
      const auto pixel = gs.pixel(x, y);
      framebuffer_hash ^= pixel;
      framebuffer_hash *= 1099511628211ull;
      nonzero_rgb += (pixel & 0xFFFFFFu) != 0u;
    }
  for (std::uint32_t address = 0; address < 0x400000u; address += 4u) {
    vram_hash ^= gif.read_local32(address);
    vram_hash *= 1099511628211ull;
  }
  std::printf("gif_replay trace_validated=1 scope=fresh-instance-reset-starting-serialized-astra-gif "
              "not-pcsx2-dump-not-hardware-timeline stop=%s "
              "trace_packets=%llu trace_payload_bytes=%llu replay_packets=%llu "
              "replay_payload_bytes=%llu replay_ee_cycle=%llu replay_next_ee_pc=%08X "
              "terminal_ee_cycle=%llu terminal_next_ee_pc=%08X\n",
      stop_reason, static_cast<unsigned long long>(reader.record_count()),
      static_cast<unsigned long long>(reader.payload_bytes()),
      static_cast<unsigned long long>(replayed_packets),
      static_cast<unsigned long long>(replayed_bytes),
      static_cast<unsigned long long>(replay_ee_cycle), replay_next_pc,
      static_cast<unsigned long long>(reader.terminal_cycle()), reader.terminal_pc());
  std::printf("gif_packets=%llu rejected=%llu pending=%zu sprites=%llu "
              "points=%llu lines=%llu triangles=%llu exported_triangles=%llu\n",
      static_cast<unsigned long long>(gif.packets_submitted()),
      static_cast<unsigned long long>(gif.packets_rejected()), gif.pending_bytes(),
      static_cast<unsigned long long>(gif.sprites_emitted()),
      static_cast<unsigned long long>(gif.points_emitted()),
      static_cast<unsigned long long>(gif.lines_emitted()),
      static_cast<unsigned long long>(gif.triangles_emitted()),
      static_cast<unsigned long long>(exported_triangles));
  std::printf("framebuffer_hash=%016llX nonzero_rgb_pixels=%zu/%u FRAME=%016llX "
              "logical_vram_word_hash=%016llX word_count=1048576\n",
      static_cast<unsigned long long>(framebuffer_hash), nonzero_rgb,
      ps2vita::Gs::kWidth * ps2vita::Gs::kHeight,
      static_cast<unsigned long long>(gs.color_frame()),
      static_cast<unsigned long long>(vram_hash));
  if (options.have_sprite)
    std::printf("gif_sprite_capture sequence=%llu framebuffer=%s source=%s\n",
        static_cast<unsigned long long>(options.sprite),
        options.sprite_framebuffer, options.sprite_source);
  return accepted ? 0 : 1;
}
