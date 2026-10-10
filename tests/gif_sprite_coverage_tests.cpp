#include "ps2vita/gif.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

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

constexpr std::uint32_t kBackground = 0x80332211u;
constexpr std::uint32_t kSolid = 0x80E5B769u;
constexpr int kOrigin = 0x8000;

struct Rectangle {
  const char* name;
  // Signed offsets from XYOFFSET, in native GS 12.4 fixed-point units.
  int x0, y0, x1, y1;
  // SCISSOR has native integer coordinates, with inclusive maximum edges.
  unsigned left = 0u, top = 0u, right = 2047u, bottom = 2047u;
};

struct Uv {
  // Independent unsigned 10.4 texture endpoints; never pre-round them.
  unsigned u0, v0, u1, v1;
};

class Packet {
public:
  void word64(std::uint64_t value) {
    for (unsigned byte = 0u; byte < 8u; ++byte)
      bytes.push_back(static_cast<std::uint8_t>(value >> (byte * 8u)));
  }
  void ad(std::uint64_t value, std::uint64_t reg) {
    word64(0x1000000000008001ull); // One PACKED A+D qword, EOP.
    word64(0xEull);
    word64(value);
    word64(reg);
  }
  std::vector<std::uint8_t> bytes;
};

std::uint64_t xyz(int x, int y) {
  return static_cast<std::uint64_t>((kOrigin + x) & 0xFFFF) |
      (static_cast<std::uint64_t>((kOrigin + y) & 0xFFFF) << 16u);
}

std::uint32_t texel(unsigned u, unsigned v) {
  // Different components identify both sampled axes without a captured oracle.
  return 0x80000000u | u | (v << 8u) | ((u ^ v) << 16u);
}

void append_texture(Packet& packet, bool linear_empty) {
  // An owned 64x64 PSMCT32 nearest/DECAL texture in the existing linear model.
  constexpr std::uint64_t base = 0x1000ull; // 0x100000 byte address.
  packet.ad((base << 32u) | (1ull << 48u), 0x50u);
  packet.ad(0u, 0x51u);
  packet.ad(64u | (64ull << 32u), 0x52u);
  packet.ad(0u, 0x53u);
  packet.word64(0x0800000000008400ull); // IMAGE, 1024 qwords.
  packet.word64(0u);
  for (unsigned v = 0u; v < 64u; ++v)
    for (unsigned u = 0u; u < 64u; u += 2u)
      packet.word64(texel(u, v) | (std::uint64_t{texel(u + 1u, v)} << 32u));
  packet.ad(base | (1ull << 14u) | (6ull << 26u) | (6ull << 30u) |
                (1ull << 34u) | (1ull << 35u), 0x06u);
  // Linear is used only for empty-footprint guards; bilinear color/rounding
  // has its own independent fixture. Normal color cases remain nearest.
  packet.ad(linear_empty ? 1u << 5u : 0u, 0x14u);
  packet.ad(5u, 0x08u); // Clamp both axes to the 64x64 texture.
}

bool covered(const Rectangle& rect, int x, int y) {
  // Raster samples represent native anchors (scale*x, scale*y), not centers
  // of a tile. Test the original half-open rectangle at those anchors.
  // This independent inequality oracle does not reproduce any ceil helper.
  const int native_fixed_x = x * 16 * ps2vita::Gs::kRasterScale;
  const int native_fixed_y = y * 16 * ps2vita::Gs::kRasterScale;
  return std::min(rect.x0, rect.x1) <= native_fixed_x &&
      native_fixed_x < std::max(rect.x0, rect.x1) &&
      std::min(rect.y0, rect.y1) <= native_fixed_y &&
      native_fixed_y < std::max(rect.y0, rect.y1) &&
      rect.left <= static_cast<unsigned>(x * ps2vita::Gs::kRasterScale) &&
      static_cast<unsigned>(x * ps2vita::Gs::kRasterScale) <= rect.right &&
      rect.top <= static_cast<unsigned>(y * ps2vita::Gs::kRasterScale) &&
      static_cast<unsigned>(y * ps2vita::Gs::kRasterScale) <= rect.bottom;
}

unsigned texture_coordinate(unsigned first, unsigned second,
                            int origin, int end, int anchor) {
  // Affine interpolation uses the original signed geometry and raw UV bits;
  // clipping or ceil-rounded bounds must not replace its origin or span.
  const long double fraction = static_cast<long double>(anchor - origin) /
                               static_cast<long double>(end - origin);
  const long double value = (static_cast<long double>(first) + fraction *
      (static_cast<long double>(second) - first)) / 16.0L;
  return static_cast<unsigned>(std::clamp(std::floor(value), 0.0L, 63.0L));
}

void run_case(const Rectangle& rect, const Uv* uv = nullptr,
              bool linear_empty = false) {
  ps2vita::Gs gs;
  ps2vita::Gif gif(gs);
  gif.enable_triangle_trace(true);
  gif.capture_sprite_framebuffer_at(0u);
  gs.clear(kBackground);
  Packet packet;
  if (uv != nullptr) append_texture(packet, linear_empty);
  // FBW=0 intentionally retains the standalone preview target. Coverage is
  // independent of shared-memory target aliasing and native GS swizzling.
  packet.ad(0u, 0x4Cu);
  packet.ad(0u, 0x47u); // No alpha/depth rejection or depth writes.
  packet.ad(1ull << 32u, 0x4Eu);
  packet.ad(static_cast<std::uint64_t>(kOrigin) |
                (static_cast<std::uint64_t>(kOrigin) << 32u), 0x18u);
  packet.ad(rect.left | (std::uint64_t{rect.right} << 16u) |
                (std::uint64_t{rect.top} << 32u) |
                (std::uint64_t{rect.bottom} << 48u), 0x40u);
  packet.ad((0x3F800000ull << 32u) | kSolid, 0x01u);
  packet.ad(uv != nullptr ? 0x116u : 6u, 0x00u);
  if (uv != nullptr)
    packet.ad(uv->u0 | (std::uint64_t{uv->v0} << 16u), 0x03u);
  packet.ad(xyz(rect.x0, rect.y0), 0x05u);
  if (uv != nullptr)
    packet.ad(uv->u1 | (std::uint64_t{uv->v1} << 16u), 0x03u);
  packet.ad(xyz(rect.x1, rect.y1), 0x05u);
  check(gif.submit(packet.bytes.data(), packet.bytes.size()) &&
            gif.packets_submitted() == 1u && gif.packets_rejected() == 0u &&
            gif.pending_bytes() == 0u && gif.sprites_emitted() == 1u &&
            gif.triangles_emitted() == 0u,
        rect.name, "one complete owned GIF sprite is parsed");
  unsigned mismatches = 0u;
  unsigned coverage = 0u, nonzero = 0u;
  std::uint64_t hash = 1469598103934665603ull;
  bool captured_pixels_match = gif.sprite_framebuffer_capture().size() ==
      static_cast<std::size_t>(ps2vita::Gs::kWidth * ps2vita::Gs::kHeight);
  for (int y = 0; y < ps2vita::Gs::kHeight; ++y) {
    for (int x = 0; x < ps2vita::Gs::kWidth; ++x) {
      std::uint32_t expected = kBackground;
      if (covered(rect, x, y)) {
        ++coverage;
        expected = uv == nullptr ? kSolid : texel(
            texture_coordinate(uv->u0, uv->u1, rect.x0, rect.x1,
                               x * 16 * ps2vita::Gs::kRasterScale),
            texture_coordinate(uv->v0, uv->v1, rect.y0, rect.y1,
                               y * 16 * ps2vita::Gs::kRasterScale));
      }
      const auto actual = gs.pixel(x, y);
      hash ^= actual; hash *= 1099511628211ull;
      nonzero += (actual & 0xFFFFFFu) != 0u;
      if (captured_pixels_match)
        captured_pixels_match = gif.sprite_framebuffer_capture()[
            static_cast<std::size_t>(y * ps2vita::Gs::kWidth + x)] == actual;
      if (actual != expected) {
        if (mismatches < 3u)
          std::fprintf(stderr, "  [%s] pixel(%d,%d): %08X, expected %08X\n",
                       rect.name, x, y, actual, expected);
        ++mismatches;
      }
    }
  }
  check(mismatches == 0u, rect.name,
        "all raster anchors have independently expected coverage/color");
  check(!linear_empty || coverage == 0u, rect.name,
        "linear empty-footprint control never enters the nearest-color oracle");
  check(gif.sprite_framebuffer_captured() && captured_pixels_match, rect.name,
        "even an empty sprite runs the complete framebuffer-capture footer");
  check(gif.sprite_records().size() == 1u &&
            gif.sprite_records()[0].sequence == 0u &&
            gif.sprite_records()[0].target_hash == hash &&
            gif.sprite_records()[0].target_nonzero_rgb == nonzero,
        rect.name, "empty/nonempty sprite trace footer preserves ordinal and target state");
  if (mismatches != 0u)
    std::fprintf(stderr, "  [%s] %u framebuffer mismatches\n", rect.name, mismatches);
}

} // namespace

int main() {
  // Primary basis: PCSX2 v2.8.2 software DrawSprite preserves fractional
  // geometry, ceil-rounds both rectangle edges, then uses exclusive maxima:
  // https://github.com/PCSX2/pcsx2/blob/v2.8.2/pcsx2/GS/Renderers/SW/GSRasterizer.cpp#L1022-L1109
  // Run on both grids: these remain logical-model checks, not PS2 goldens.
  const Rectangle rectangles[]{
      {"integer full target control", 0, 0, 10240, 7168},
      {"fractional full-width right edge", 0, 0, 10239, 512},
      {"fractional right and bottom edges", 0, 0, 10239, 7167},
      {"positive fractional lower edges", 1, 1, 511, 511},
      {"negative fractional lower edges", -1, -1, 511, 511},
      {"negative rectangle off screen", -129, -129, -1, -1},
      {"negative to positive fractional span", -65, -65, 65, 65},
      {"reversed fractional rectangle", 10239, 511, 1, 1},
      {"positive fractional reversed X edge", 511, 1, 1, 511},
      {"positive fractional reversed Y edge", 1, 511, 511, 1},
      {"aligned inclusive scissor control", 0, 0, 512, 512, 4u, 4u, 11u, 11u},
      {"unaligned inclusive scissor", 0, 0, 512, 512, 5u, 5u, 9u, 9u},
      {"invalid native scissor remains empty", 0, 0, 512, 512, 10u, 10u, 8u, 8u},
      {"fractional rectangle between anchors", 1, 1, 63, 63},
      {"zero-width rectangle remains empty", 64, 0, 64, 512},
      {"zero-height rectangle remains empty", 0, 64, 512, 64}};
  for (const auto& rectangle : rectangles) run_case(rectangle);

  const Uv matching{8u, 8u, 520u, 520u};
  const Rectangle fractional{"UV keeps fractional geometry origin/span", 8, 8, 520, 520};
  run_case(fractional, &matching);
  const Rectangle clipped{"UV clipping keeps original geometry origin/span",
                          8, 8, 520, 520, 12u, 12u, 23u, 23u};
  run_case(clipped, &matching);
  const Uv reverse_x{520u, 8u, 8u, 520u};
  const Rectangle reversed_x{"UV reversed X retains attribute endpoints", 520, 8, 8, 520};
  run_case(reversed_x, &reverse_x);
  const Uv reverse_xy{520u, 520u, 8u, 8u};
  const Rectangle reversed_xy{"UV reversed XY retains attribute endpoints", 520, 520, 8, 8};
  run_case(reversed_xy, &reverse_xy);
  const Uv fractional_uv{8u, 8u, 504u, 504u};
  const Rectangle integer_uv{"UV fractional operands survive interpolation", 0, 0, 512, 512};
  run_case(integer_uv, &fractional_uv);
  const Uv shifted{8u, 8u, 520u, 520u};
  const Rectangle negative_uv{"UV negative geometry retains signed origin", -8, -8, 504, 504};
  run_case(negative_uv, &shifted);
  // At host X=1, raw U is 15.5 fixed units, i.e. 0.96875 texels. Rounding
  // the negative delta before adding U0 would instead produce raw U=16 and
  // select texel 1. Form the complete affine value before nearest sampling.
  const Uv descending{16u, 0u, 15u, 0u};
  const Rectangle descending_uv{"UV descending slope crosses nearest boundary", 0, 0, 128, 128};
  run_case(descending_uv, &descending);

  const Uv reverse_y{8u, 16383u, 520u, 0u};
  const Rectangle reversed_y{"UV reversed Y preserves high raw endpoint and row rounding",
                              8, 520, 520, 8};
  run_case(reversed_y, &reverse_y);
  const Rectangle reversed_y_clipped{"UV reversed Y keeps unclipped vertical span",
                                      8, 520, 520, 8, 5u, 5u, 23u, 23u};
  run_case(reversed_y_clipped, &reverse_y);
  const Uv wide_span_uv{0u, 1024u, 512u, 0u};
  const Rectangle wide_span{"UV signed raw vertical bounds retain row coordinate",
                             0, -32768, 512, 32767};
  run_case(wide_span, &wide_span_uv);
  const Uv vertical_boundary{0u, 16u, 0u, 15u};
  run_case({"UV descending V crosses nearest boundary", 0, 0, 128, 128},
           &vertical_boundary);

  const Rectangle textured_empty[]{
      {"textured zero-width footer", 64, 0, 64, 512},
      {"textured zero-height footer", 0, 64, 512, 64},
      {"textured zero-area footer", 64, 64, 64, 64},
      {"textured invalid X scissor footer", 0, 0, 512, 512, 10u, 0u, 8u, 31u},
      {"textured invalid Y scissor footer", 0, 0, 512, 512, 0u, 10u, 31u, 8u},
      {"textured negative X footprint footer", -129, 0, -1, 512}};
  for (const auto& rectangle : textured_empty)
    for (const bool linear : {false, true}) run_case(rectangle, &reverse_y, linear);

  std::printf("gif_sprite_coverage_tests: %u checks, %u failures\n", checks, failures);
  return failures == 0u ? 0 : 1;
}
