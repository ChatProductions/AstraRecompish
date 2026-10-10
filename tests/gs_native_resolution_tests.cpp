#include "ps2vita/gif.hpp"
#include "ps2vita/framebuffer_dump.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <sstream>
#include <vector>

namespace {

static_assert(ps2vita::Gs::kRasterScale == 1, "link only the native diagnostic library");
static_assert(ps2vita::Gs::kWidth == 640 && ps2vita::Gs::kHeight == 448,
              "native diagnostic viewport");
unsigned checks = 0, failures = 0;
constexpr std::uint32_t kColor = 0x80DDAA77u;
constexpr unsigned kStride = 64u;

void check(bool condition, const char* property) {
  ++checks;
  if (!condition) { ++failures; std::fprintf(stderr, "FAIL: %s\n", property); }
}

class Packet {
public:
  void word64(std::uint64_t value) {
    for (unsigned i = 0; i < 8u; ++i)
      bytes.push_back(static_cast<std::uint8_t>(value >> (8u * i)));
  }
  void ad(unsigned reg, std::uint64_t value) {
    word64(0x1000000000008001ull); word64(0xEull);
    word64(value); word64(reg);
  }
  void setup() {
    ad(0x4Cu, 1ull << 16u); // Linear FRAME 0, native width 64, PSMCT32.
    ad(0x18u, 0u);
    ad(0x40u, (63ull << 16u) | (63ull << 48u));
    ad(0x47u, 0u); ad(0x4Eu, 1ull << 32u); // No depth comparisons/writes.
    ad(1u, (0x3F800000ull << 32u) | kColor);
  }
  void xyz(unsigned x_fixed, unsigned y_fixed) {
    ad(5u, x_fixed | (std::uint64_t{y_fixed} << 16u));
  }
  std::vector<std::uint8_t> bytes;
};

unsigned nonblack(const ps2vita::Gif& gif) {
  unsigned count = 0;
  for (unsigned y = 0; y < 64u; ++y)
    for (unsigned x = 0; x < kStride; ++x)
      count += (gif.read_local32((y * kStride + x) * 4u) & 0xFFFFFFu) != 0u;
  return count;
}

void point_and_watch() {
  ps2vita::Gs gs;
  ps2vita::Gif gif(gs);
  constexpr unsigned address = (7u * kStride + 5u) * 4u;
  std::vector<ps2vita::GsPixelWrite> events;
  check(gs.set_pixel_watch({address, address - 4u, address + 4u},
      [&](const ps2vita::GsPixelWrite& event) { events.push_back(event); }),
      "native physical watchpoints attach");
  Packet packet; packet.setup(); packet.ad(0u, 0u);
  packet.xyz(5u * 16u, 7u * 16u);
  check(gif.submit(packet.bytes.data(), packet.bytes.size()), "owned point accepted");
  check(gs.pixel(5, 7) == kColor && gif.read_local32(address) == kColor,
        "guest point reaches its exact native pixel");
  check(nonblack(gif) == 1u, "point writes one pixel, not a four-by-four tile");
  check(events.size() == 1u && events[0].address == address &&
            events[0].x == 5 && events[0].y == 7 && events[0].before == 0u &&
            events[0].after == kColor,
        "native watch metadata and footprint do not claim adjacent pixels");
  std::ostringstream output;
  check(ps2vita::write_framebuffer_ppm(output, gs) &&
            output.str().compare(0, 15, "P6\n640 448\n255\n") == 0 &&
            output.str().size() == 15u + 640u * 448u * 3u,
        "native PPM dimensions/payload contain true raster samples");
}

void thin_line() {
  ps2vita::Gs gs;
  ps2vita::Gif gif(gs);
  Packet packet; packet.setup(); packet.ad(0u, 1u);
  packet.xyz(5u * 16u, 10u * 16u); packet.xyz(9u * 16u, 10u * 16u);
  check(gif.submit(packet.bytes.data(), packet.bytes.size()) && gif.lines_emitted() == 1u,
        "owned one-pixel horizontal line accepted");
  unsigned mismatches = 0;
  for (unsigned y = 0; y < 64u; ++y)
    for (unsigned x = 0; x < 64u; ++x)
      mismatches += gif.read_local32((y * kStride + x) * 4u) !=
          (y == 10u && x >= 5u && x <= 9u ? kColor : 0u);
  check(mismatches == 0u && nonblack(gif) == 5u,
        "line preserves all five separate pixels and remains one pixel high");
}

void fractional_sprite() {
  ps2vita::Gs gs;
  ps2vita::Gif gif(gs);
  Packet packet; packet.setup(); packet.ad(0u, 6u);
  packet.xyz(12u * 16u + 1u, 12u * 16u + 1u);
  packet.xyz(14u * 16u - 1u, 14u * 16u - 1u);
  check(gif.submit(packet.bytes.data(), packet.bytes.size()) && gif.sprites_emitted() == 1u,
        "owned fractional sprite accepted");
  check(nonblack(gif) == 1u && gs.pixel(13, 13) == kColor,
        "half-open fractional sprite covers native anchor thirteen only");
}

void integer_triangle() {
  ps2vita::Gs gs;
  ps2vita::Gif gif(gs);
  Packet packet; packet.setup(); packet.ad(0u, 3u);
  packet.xyz(4u * 16u, 4u * 16u);
  packet.xyz(8u * 16u, 4u * 16u);
  packet.xyz(4u * 16u, 8u * 16u);
  check(gif.submit(packet.bytes.data(), packet.bytes.size()) && gif.triangles_emitted() == 1u,
        "owned integer triangle accepted");
  unsigned mismatches = 0;
  for (unsigned y = 0; y < 64u; ++y)
    for (unsigned x = 0; x < 64u; ++x) {
      // Independent half-plane oracle for this integer-coordinate triangle.
      const bool covered = x >= 4u && y >= 4u && x + y < 12u;
      mismatches += gif.read_local32((y * kStride + x) * 4u) != (covered ? kColor : 0u);
    }
  check(mismatches == 0u && nonblack(gif) == 10u,
        "integer triangle retains distinct native coverage and top-left edges");
}

std::uint32_t texel(unsigned x, unsigned y) {
  return 0x80000000u | (x * 25u + 1u) | ((y * 27u + 1u) << 8u) |
      ((x + y * 8u + 1u) << 16u);
}

void independent_texels() {
  ps2vita::Gs gs;
  ps2vita::Gif gif(gs);
  Packet packet; packet.setup();
  constexpr unsigned base = 0x100000u;
  packet.ad(0x50u, (std::uint64_t{base / 256u} << 32u) | (1ull << 48u));
  packet.ad(0x51u, 0u); packet.ad(0x52u, 8u | (8ull << 32u)); packet.ad(0x53u, 0u);
  packet.word64((2ull << 58u) | 0x8000u | 16u); packet.word64(0u);
  for (unsigned y = 0; y < 8u; ++y)
    for (unsigned x = 0; x < 8u; x += 2u)
      packet.word64(texel(x, y) | (std::uint64_t{texel(x + 1u, y)} << 32u));
  packet.ad(0x06u, (base / 256u) | (1ull << 14u) | (3ull << 26u) |
      (3ull << 30u) | (1ull << 34u) | (1ull << 35u)); // RGBA DECAL.
  packet.ad(0x14u, 0u); packet.ad(0x08u, 0u);
  packet.ad(0u, 0x116u);
  packet.ad(0x03u, 0u); packet.xyz(16u * 16u, 16u * 16u);
  packet.ad(0x03u, 128u | (128ull << 16u)); packet.xyz(24u * 16u, 24u * 16u);
  check(gif.submit(packet.bytes.data(), packet.bytes.size()), "owned texture sprite accepted");
  for (unsigned y = 0; y < 8u; ++y)
    for (unsigned x = 0; x < 8u; ++x)
      check(gs.pixel(16 + x, 16 + y) == texel(x, y),
            "all sixty-four source texels retain independent native detail");
  check(nonblack(gif) == 64u, "texture rectangle writes exactly its sixty-four native pixels");
}

void narrow_owned_glyph() {
  ps2vita::Gs gs;
  ps2vita::Gif gif(gs);
  // Owned five-row shape, not a BIOS font or an image-processing reconstruction.
  constexpr std::array<unsigned, 5> rows{{7u, 5u, 7u, 4u, 4u}};
  Packet packet; packet.setup(); packet.ad(0u, 0u);
  unsigned expected_count = 0;
  for (unsigned y = 0; y < rows.size(); ++y)
    for (unsigned x = 0; x < 3u; ++x)
      if ((rows[y] & (4u >> x)) != 0u) {
        packet.xyz((21u + x) * 16u, (19u + y) * 16u); ++expected_count;
      }
  check(gif.submit(packet.bytes.data(), packet.bytes.size()), "owned narrow glyph points accepted");
  unsigned mismatches = 0;
  for (unsigned y = 0; y < 64u; ++y)
    for (unsigned x = 0; x < 64u; ++x) {
      const bool covered = x >= 21u && x < 24u && y >= 19u && y < 24u &&
          (rows[y - 19u] & (4u >> (x - 21u))) != 0u;
      mismatches += gif.read_local32((y * kStride + x) * 4u) != (covered ? kColor : 0u);
    }
  check(mismatches == 0u && nonblack(gif) == expected_count,
        "three-pixel-wide owned glyph survives without merging strokes into a block");
}

} // namespace

int main() {
  point_and_watch(); thin_line(); fractional_sprite(); integer_triangle();
  independent_texels(); narrow_owned_glyph();
  std::printf("GS native resolution tests: %u checks, %u failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
