#include "ps2vita/framebuffer_dump.hpp"
#include "ps2vita/gif.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <sstream>
#include <utility>
#include <vector>

// Owned half-grid contracts, not hardware GS coverage or a BIOS font oracle.
// Unlike native rendering, each accepted even-coordinate anchor still writes
// an approximate 2x2 native tile. No default or Vita resolution changes here.
namespace {
static_assert(ps2vita::Gs::kRasterScale == 2, "link only the half-grid library");
static_assert(ps2vita::Gs::kWidth == 320 && ps2vita::Gs::kHeight == 224,
    "half-grid diagnostic viewport");
unsigned checks = 0u, failures = 0u;
constexpr std::uint32_t kColor = 0x80DDAA77u;
constexpr unsigned kStride = 64u, kOffsetX = 0x8003u, kOffsetY = 0x8017u;

void check(bool ok, const char* name) {
  ++checks;
  if (!ok) { ++failures; std::fprintf(stderr, "FAIL: %s\n", name); }
}

class Packet {
public:
  void word(std::uint64_t value, unsigned count = 8u) {
    for (unsigned byte = 0u; byte < count; ++byte)
      bytes.push_back(static_cast<std::uint8_t>(value >> (8u * byte)));
  }
  void ad(unsigned reg, std::uint64_t value) {
    word(0x1000000000008001ull); word(0xEull); word(value); word(reg);
  }
  void setup(unsigned left = 0u, unsigned top = 0u,
      unsigned right = 63u, unsigned bottom = 63u) {
    ad(0x4Cu, 1ull << 16u); // Linear native stride64, PSMCT32, base0.
    ad(0x18u, kOffsetX | (std::uint64_t{kOffsetY} << 32u));
    ad(0x40u, left | (std::uint64_t{right} << 16u) |
        (std::uint64_t{top} << 32u) | (std::uint64_t{bottom} << 48u));
    ad(0x47u, 0u); ad(0x4Eu, 1ull << 32u);
    color(kColor);
  }
  void color(std::uint32_t value) { ad(1u, (0x3F800000ull << 32u) | value); }
  void xyz(int fixed_x, int fixed_y) {
    const auto x = static_cast<unsigned>(fixed_x + static_cast<int>(kOffsetX));
    const auto y = static_cast<unsigned>(fixed_y + static_cast<int>(kOffsetY));
    ad(5u, x | (std::uint64_t{y} << 16u));
  }
  std::vector<std::uint8_t> bytes;
};

template<class Expected>
bool entire_target(const ps2vita::Gif& gif, Expected expected) {
  for (unsigned y = 0u; y < 64u; ++y)
    for (unsigned x = 0u; x < 64u; ++x)
      if (gif.read_local32((y * 64u + x) * 4u) != expected(x, y)) return false;
  return true;
}

void point_line_and_preview() {
  {
    ps2vita::Gs gs;
    ps2vita::Gif gif(gs);
    const std::vector<std::uint32_t> watched{
        (6u * 64u + 4u) * 4u, (6u * 64u + 5u) * 4u,
        (7u * 64u + 4u) * 4u, (7u * 64u + 5u) * 4u,
        (6u * 64u + 3u) * 4u, (7u * 64u + 6u) * 4u};
    std::vector<ps2vita::GsPixelWrite> events;
    check(gs.set_pixel_watch(watched, [&](const ps2vita::GsPixelWrite& item) {
      events.push_back(item);
    }), "half-grid physical watchpoints attach");
    Packet packet; packet.setup(); packet.ad(0u, 0u);
    packet.xyz(5 * 16, 7 * 16);
    check(gif.submit(packet.bytes.data(), packet.bytes.size()) && gif.points_emitted() == 1u,
        "owned half-grid point accepted");
    check(entire_target(gif, [](unsigned x, unsigned y) {
      return x >= 4u && x <= 5u && y >= 6u && y <= 7u ? kColor : 0u;
    }) && gs.pixel(2, 3) == kColor,
        "odd-native point projects to one exact 2x2 tile, not one pixel or a 4x4 tile");
    bool exact_events = events.size() == 4u;
    for (unsigned index = 0u; index < 4u; ++index) {
      const auto found = std::find_if(events.begin(), events.end(), [&](const auto& item) {
        return item.address == watched[index];
      });
      exact_events = exact_events && found != events.end();
      if (found != events.end()) exact_events = exact_events && found->before == 0u &&
          found->after == kColor && found->input_color == kColor && found->x == 2 && found->y == 3;
    }
    check(exact_events, "half-grid watch footprint covers all four tile words and no neighboring pixel");
    std::ostringstream output;
    check(ps2vita::write_framebuffer_ppm(output, gs) &&
        output.str().compare(0, 15, "P6\n320 224\n255\n") == 0 &&
        output.str().size() == 15u + 320u * 224u * 3u,
        "half-grid PPM dimensions contain real 320x224 raster samples");
  }
  {
    ps2vita::Gs gs;
    ps2vita::Gif gif(gs);
    Packet packet; packet.setup(); packet.ad(0u, 1u);
    packet.xyz(5 * 16, 10 * 16); packet.xyz(9 * 16, 10 * 16);
    check(gif.submit(packet.bytes.data(), packet.bytes.size()) && gif.lines_emitted() == 1u,
        "owned half-grid line accepted");
    check(entire_target(gif, [](unsigned x, unsigned y) {
      return x >= 4u && x <= 9u && y >= 10u && y <= 11u ? kColor : 0u;
    }), "line preserves three separate half-grid samples with only their 2x2 tiles");
  }
}

struct XY { int x, y; };
struct Triangle { std::array<XY, 3> vertices; std::uint32_t color = kColor; };

std::int64_t cross(const XY& a, const XY& b, const XY& p) {
  return std::int64_t{b.x - a.x} * (p.y - a.y) -
      std::int64_t{b.y - a.y} * (p.x - a.x);
}

bool coverage(Triangle triangle, unsigned native_x, unsigned native_y) {
  auto& v = triangle.vertices;
  const auto area = cross(v[0], v[1], v[2]);
  if (area == 0) return false;
  if (area < 0) std::swap(v[1], v[2]);
  const XY point{static_cast<int>(native_x * 16u), static_cast<int>(native_y * 16u)};
  // Independent standard positive cross-product oracle at even native anchors.
  // No production bounding-box, edge function or snapped geometry is used.
  for (unsigned edge = 0u; edge < 3u; ++edge) {
    const auto& a = v[edge];
    const auto& b = v[(edge + 1u) % 3u];
    const auto distance = cross(a, b, point);
    const bool inclusive = b.y < a.y || (b.y == a.y && b.x > a.x);
    if (distance < 0 || (distance == 0 && !inclusive)) return false;
  }
  return true;
}

void triangle_case(const std::vector<Triangle>& triangles, unsigned left = 0u,
    unsigned top = 0u, unsigned right = 31u, unsigned bottom = 31u) {
  ps2vita::Gs gs;
  ps2vita::Gif gif(gs);
  Packet packet; packet.setup(left, top, right, bottom); packet.ad(0u, 3u);
  for (const auto& triangle : triangles) {
    packet.color(triangle.color);
    for (const auto& vertex : triangle.vertices) packet.xyz(vertex.x, vertex.y);
  }
  check(gif.submit(packet.bytes.data(), packet.bytes.size()) &&
          gif.triangles_emitted() == triangles.size() && gif.packets_rejected() == 0u &&
          gif.pending_bytes() == 0u,
      "owned half-grid triangle packet preserves complete accepted primitive counts");
  check(entire_target(gif, [&](unsigned x, unsigned y) {
    const unsigned anchor_x = (x / 2u) * 2u, anchor_y = (y / 2u) * 2u;
    std::uint32_t result = 0u;
    if (anchor_x >= left && anchor_x <= right && anchor_y >= top && anchor_y <= bottom)
      for (const auto& triangle : triangles)
        if (coverage(triangle, anchor_x, anchor_y)) result = triangle.color;
    return result;
  }), "every covered/untouched native word matches independent fixed-position half-grid tile coverage");
}

void fractional_triangle_coverage() {
  const std::array<Triangle, 5> cases{{
      {{{{4, 4}, {4, 228}, {228, 4}}}},
      {{{{7, 13}, {23, 245}, {237, 29}}}},
      {{{{-8, -5}, {-8, 180}, {164, -5}}}},
      {{{{28, 28}, {28, 40}, {40, 28}}}},
      {{{{3, 7}, {67, 71}, {131, 135}}}}}};
  for (auto triangle : cases) {
    triangle_case({triangle});
    std::swap(triangle.vertices[1], triangle.vertices[2]);
    triangle_case({triangle});
  }
  triangle_case({cases[0]}, 1u, 1u, 9u, 9u);
  triangle_case({cases[0]}, 9u, 0u, 1u, 31u);
  const Triangle first{{{{4, 12}, {4, 252}, {244, 12}}}, 0x80554433u};
  const Triangle second{{{{244, 12}, {4, 252}, {244, 252}}}, 0x80998877u};
  triangle_case({first, second});
  triangle_case({second, first});
  check(coverage(cases[3], 2u, 2u) && !coverage(cases[3], 0u, 0u) &&
          !coverage(cases[3], 4u, 4u),
      "owned tiny fractional triangle distinguishes half-grid native-two coverage from quarter anchors");
}

void fractional_sprite() {
  ps2vita::Gs gs;
  ps2vita::Gif gif(gs);
  Packet packet; packet.setup(); packet.ad(0u, 6u);
  packet.xyz(12 * 16 + 1, 12 * 16 + 1);
  packet.xyz(15 * 16 - 1, 15 * 16 - 1);
  check(gif.submit(packet.bytes.data(), packet.bytes.size()) && gif.sprites_emitted() == 1u,
      "owned fractional half-grid sprite accepted");
  check(entire_target(gif, [](unsigned x, unsigned y) {
    return x >= 14u && x <= 15u && y >= 14u && y <= 15u ? kColor : 0u;
  }), "fractional sprite covers even native anchor fourteen and exactly its diagnostic 2x2 tile");
}

std::uint32_t texel(unsigned x, unsigned y) {
  return 0x80000000u | (x * 25u + 1u) | ((y * 27u + 1u) << 8u) |
      ((x + y * 8u + 1u) << 16u);
}

void distinct_texture_samples() {
  ps2vita::Gs gs;
  ps2vita::Gif gif(gs);
  constexpr unsigned base = 0x100000u;
  Packet packet; packet.setup();
  packet.ad(0x50u, (std::uint64_t{base / 256u} << 32u) | (1ull << 48u));
  packet.ad(0x51u, 0u); packet.ad(0x52u, 8u | (8ull << 32u)); packet.ad(0x53u, 0u);
  packet.word((2ull << 58u) | 0x8000u | 16u); packet.word(0u);
  for (unsigned y = 0u; y < 8u; ++y)
    for (unsigned x = 0u; x < 8u; ++x) packet.word(texel(x, y), 4u);
  packet.ad(0x06u, (base / 256u) | (1ull << 14u) | (3ull << 26u) |
      (3ull << 30u) | (1ull << 34u) | (1ull << 35u)); // RGBA DECAL.
  packet.ad(0x14u, 0u); packet.ad(0x08u, 0u); packet.ad(0u, 0x116u);
  packet.ad(0x03u, 0u); packet.xyz(16 * 16, 16 * 16);
  packet.ad(0x03u, 128u | (128ull << 16u)); packet.xyz(24 * 16, 24 * 16);
  check(gif.submit(packet.bytes.data(), packet.bytes.size()) && gif.sprites_emitted() == 1u &&
          gif.image_bytes() == 256u && gif.packets_rejected() == 0u && gif.pending_bytes() == 0u,
      "owned nearest-textured half-grid sprite and IMAGE upload accepted");
  check(entire_target(gif, [](unsigned x, unsigned y) {
    return x >= 16u && x < 24u && y >= 16u && y < 24u ?
        texel(((x - 16u) / 2u) * 2u, ((y - 16u) / 2u) * 2u) : 0u;
  }), "all sixteen distinct even-coordinate texture samples retain their separate 2x2 tiles");
  bool source_unchanged = true;
  for (unsigned y = 0u; y < 8u; ++y)
    for (unsigned x = 0u; x < 8u; ++x)
      source_unchanged = source_unchanged &&
          gif.read_local32(base + (y * 64u + x) * 4u) == texel(x, y);
  check(source_unchanged, "sampling preserves all sixty-four original source texture pixels");
}

void wrapped_masked_tiles() {
  constexpr unsigned bytes = 4194304u, base = 511u * 8192u, pitch = 63u * 64u;
  std::vector<std::uint8_t> memory(bytes);
  for (unsigned at = 0u; at < bytes; ++at)
    memory[at] = static_cast<std::uint8_t>((at * 29u) ^ (at / 257u) ^ 0x67u);
  const auto original = memory;
  for (unsigned psm = 0u; psm < 2u; ++psm) {
    memory = original;
    auto expected = original;
    constexpr std::uint32_t explicit_mask = 0x0000FF00u;
    const auto mask = explicit_mask | (psm == 1u ? 0xFF000000u : 0u);
    for (unsigned y = 0u; y < 2u; ++y)
      for (unsigned x = 0u; x < 2u; ++x) {
        const auto word_address = (std::uint64_t{base} +
            (std::uint64_t{y} * pitch + x) * 4u) % bytes;
        for (unsigned byte = 0u; byte < 4u; ++byte) {
          const auto at = static_cast<unsigned>((word_address + byte) % bytes);
          const auto byte_mask = static_cast<std::uint8_t>(mask >> (byte * 8u));
          expected[at] = static_cast<std::uint8_t>(
              (static_cast<std::uint8_t>(kColor >> (byte * 8u)) & ~byte_mask) |
              (original[at] & byte_mask));
        }
      }
    ps2vita::Gs gs;
    gs.set_color_target(&memory, 511u | (63ull << 16u) |
        (std::uint64_t{psm} << 24u) | (std::uint64_t{explicit_mask} << 32u));
    gs.set_depth_state(ps2vita::Gs::DepthTest::Always, false);
    gs.point({0, 0, 0u, kColor});
    check(memory == expected,
        "all 4MiB match independent wrapped half-tile byte writes with FBMSK and PSMCT24 alpha preservation");
  }
}
} // namespace

int main() {
  point_line_and_preview();
  fractional_triangle_coverage();
  fractional_sprite();
  distinct_texture_samples();
  wrapped_masked_tiles();
  std::printf("GS half resolution tests: %u checks, %u failures\n", checks, failures);
  return failures == 0u ? 0 : 1;
}
