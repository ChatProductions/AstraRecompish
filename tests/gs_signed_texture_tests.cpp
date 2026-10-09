#include "ps2vita/gif.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
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

std::uint32_t bits(float value) {
  std::uint32_t word = 0u;
  static_assert(sizeof(word) == sizeof(value), "32-bit float required");
  std::memcpy(&word, &value, sizeof(word));
  return word;
}

constexpr std::uint32_t kTextureBase = 0x100000u;
constexpr unsigned kTextureStride = 64u;
constexpr unsigned kSampleX = 1u, kSampleY = 1u;
constexpr std::uint32_t kWatchAddress =
    (kSampleY * 4u * kTextureStride + kSampleX * 4u) * 4u;

std::uint32_t texel(unsigned u, unsigned v) {
  // Every owned texel is nonblack; the components independently identify U/V.
  return 0x80000000u | (32u + u) | ((64u + v) << 8u) |
      ((u ^ (v << 3u)) << 16u);
}

constexpr std::uint64_t clamp(unsigned umode, unsigned vmode,
                              unsigned umin = 0u, unsigned umax = 0u,
                              unsigned vmin = 0u, unsigned vmax = 0u) {
  return umode | (std::uint64_t{vmode} << 2u) |
      (std::uint64_t{umin} << 4u) | (std::uint64_t{umax} << 14u) |
      (std::uint64_t{vmin} << 24u) | (std::uint64_t{vmax} << 34u);
}

class Packet {
public:
  void word64(std::uint64_t value) {
    for (unsigned byte = 0u; byte < 8u; ++byte)
      bytes.push_back(static_cast<std::uint8_t>(value >> (byte * 8u)));
  }
  void word32(std::uint32_t value) {
    for (unsigned byte = 0u; byte < 4u; ++byte)
      bytes.push_back(static_cast<std::uint8_t>(value >> (byte * 8u)));
  }
  void ad(std::uint64_t value, unsigned reg) {
    word64(0x1000000000008001ull); // One PACKED A+D qword, EOP.
    word64(0xEull);
    word64(value);
    word64(reg);
  }
  std::vector<std::uint8_t> bytes;
};

void append_texture(Packet& packet) {
  // Owned 8x8 PSMCT32 data, transferred to a distinct logical VRAM region.
  packet.ad((std::uint64_t{kTextureBase / 256u} << 32u) | (1ull << 48u), 0x50u);
  packet.ad(0u, 0x51u);
  packet.ad(8u | (8ull << 32u), 0x52u);
  packet.ad(0u, 0x53u);
  packet.word64((2ull << 58u) | 0x8000u | 16u); // IMAGE: 64 pixels/16 qwords.
  packet.word64(0u);
  for (unsigned v = 0u; v < 8u; ++v)
    for (unsigned u = 0u; u < 8u; ++u) packet.word32(texel(u, v));
}

struct Case {
  const char* name;
  // U/V are native texture coordinates BEFORE signed16.16 quantization;
  // S/T are encoded as U*Q/8 and V*Q/8, so perspective division recovers them.
  float u, v, q;
  std::int64_t raw_u, raw_v;
  unsigned mapped_u, mapped_v;
  std::uint64_t addressing = 0u;
  bool reversed = false;
  bool valid = true;
  bool crossing_q = false;
};

Packet make_packet(const Case& item) {
  Packet packet;
  append_texture(packet);
  packet.ad(1ull << 16u, 0x4Cu); // FRAME0: 64 native pixels, PSMCT32.
  packet.ad(1ull << 32u, 0x4Eu); // ZBUF0: writes disabled.
  packet.ad(0x30000u, 0x47u);    // ZTE, ALWAYS; no alpha or destination test.
  packet.ad(0u, 0x18u);        // XYOFFSET0.
  packet.ad((31ull << 16u) | (31ull << 48u), 0x40u);
  packet.ad((kTextureBase / 256u) | (1ull << 14u) | (3ull << 26u) |
                (3ull << 30u) | (1ull << 34u) | (1ull << 35u),
            0x06u);           // TEX0: 8x8, RGBA, DECAL.
  packet.ad(0u, 0x14u);        // TEX1: nearest; no filtering claim in this suite.
  packet.ad(item.addressing, 0x08u);
  packet.ad(0x13u, 0x00u);     // TRIANGLE, TME, STQ; no blending.
  const std::array<std::array<unsigned, 2>, 3> points{{
      {{0u, 0u}}, {{0u, 32u}}, {{32u, 0u}}}};
  for (unsigned vertex = 0u; vertex < 3u; ++vertex) {
    const unsigned index = item.reversed && vertex != 0u ? 3u - vertex : vertex;
    // At host (1,1), weights are48/8/8. A Q pattern1/-3/-3 makes Q=0 there
    // while keeping other fragments finite; attributes travel with winding.
    const float q = item.crossing_q ? (index == 0u ? 1.0f : -3.0f) : item.q;
    const auto st = std::uint64_t{bits(item.u * q / 8.0f)} |
        (std::uint64_t{bits(item.v * q / 8.0f)} << 32u);
    packet.ad(st, 0x02u);
    packet.ad((std::uint64_t{bits(q)} << 32u) | 0x80808080u, 0x01u);
    packet.ad((points[index][0] * 16u) |
                  (std::uint64_t{points[index][1] * 16u} << 16u) |
                  (1ull << 32u),
              0x05u);
  }
  return packet;
}

struct Result {
  std::uint32_t pixel = 0u;
  std::array<std::uint32_t, ps2vita::Gs::kWidth * ps2vita::Gs::kHeight> preview{};
  std::vector<std::uint32_t> vram;
  std::vector<ps2vita::GsPixelWrite> events;
  bool submitted = false;
  std::uint64_t packets = 0u, rejected = 0u, triangles = 0u, image_bytes = 0u;
};

Result run_fixture(const Case& item, bool watch, bool snapshot_vram = false) {
  ps2vita::Gs gs;
  ps2vita::Gif gif(gs);
  Result result;
  if (watch)
    check(gs.set_pixel_watch({kWatchAddress}, [&](const ps2vita::GsPixelWrite& event) {
      result.events.push_back(event);
    }), item.name, "owned physical pixel watch attaches");
  const auto packet = make_packet(item);
  result.submitted = gif.submit(packet.bytes.data(), packet.bytes.size());
  result.pixel = gs.pixel(kSampleX, kSampleY);
  for (int y = 0; y < ps2vita::Gs::kHeight; ++y)
    for (int x = 0; x < ps2vita::Gs::kWidth; ++x)
      result.preview[y * ps2vita::Gs::kWidth + x] = gs.pixel(x, y);
  if (snapshot_vram) {
    result.vram.reserve(1024u * 1024u);
    for (std::uint32_t address = 0u; address < 0x400000u; address += 4u)
      result.vram.push_back(gif.read_local32(address));
  }
  result.packets = gif.packets_submitted();
  result.rejected = gif.packets_rejected();
  result.triangles = gif.triangles_emitted();
  result.image_bytes = gif.image_bytes();
  return result;
}

void mapping_cases() {
  // Primary algorithm basis: PCSX2 v2.8.2 GSDrawScanline.cpp lines1018..1054:
  // perspective division -> signed16.16 -> arithmetic shift -> repeat/clamp.
  // These tests target that bounded logical-nearest subset, not native GS
  // swizzling, bilinear filtering, all float precision, or full raster coverage.
  constexpr float tiny_negative = -0x1p-18f;
  const float infinity = std::numeric_limits<float>::infinity();
  const float nan = std::numeric_limits<float>::quiet_NaN();
  const std::array<Case, 30> cases{{
      {"repeat U minus-quarter", -.25f, 1.25f, 1.0f, -1, 1, 7u, 1u},
      {"repeat U minus-one-quarter", -1.25f, 1.25f, 1.0f, -2, 1, 6u, 1u},
      {"repeat V minus-quarter", 1.25f, -.25f, 1.0f, 1, -1, 1u, 7u},
      {"repeat both negative", -1.25f, -.25f, 1.0f, -2, -1, 6u, 7u},
      {"repeat positive control", 2.25f, 3.25f, 1.0f, 2, 3, 2u, 3u},
      {"Q2 scaled positive ST", 2.25f, 3.25f, 2.0f, 2, 3, 2u, 3u},
      {"repeat positive wrap", 8.25f, 9.25f, 1.0f, 8, 9, 0u, 1u},
      {"repeat negative exact", -1.0f, -8.0f, 1.0f, -1, -8, 7u, 0u},
      {"fixed trunc tiny negative U", tiny_negative, 1.25f, 1.0f, 0, 1, 0u, 1u},
      {"fixed trunc tiny negative V", 1.25f, tiny_negative, 1.0f, 1, 0, 1u, 0u},
      {"Q2 scaled tiny negative ST", tiny_negative, 1.25f, 2.0f, 0, 1, 0u, 1u},
      {"fixed trunc just below minus-one", -1.0f + tiny_negative, 1.25f,
       1.0f, -1, 1, 7u, 1u},
      {"fixed negative one-LSB U", -0x1p-16f, 1.25f, 1.0f, -1, 1, 7u, 1u},
      {"clamp negatives", -.25f, -1.25f, 1.0f, -1, -2, 0u, 0u, clamp(1u, 1u)},
      {"clamp positive overshoot", 9.25f, 8.25f, 1.0f, 9, 8, 7u, 7u, clamp(1u, 1u)},
      {"region clamp minima", -.25f, -1.25f, 1.0f, -1, -2, 2u, 3u,
       clamp(2u, 2u, 2u, 5u, 3u, 6u)},
      {"region repeat negative mask", -1.25f, -.25f, 1.0f, -2, -1, 6u, 3u,
       clamp(3u, 3u, 3u, 4u, 1u, 2u)},
      {"reversed winding negative", -.25f, -1.25f, 1.0f, -1, -2, 7u, 6u,
       0u, true},
      {"negative Q with same ratio", -.25f, 1.25f, -1.0f, -1, 1, 7u, 1u},
      {"large positive bounded repeat", 0x1p31f, 1.25f, 1.0f, 2147483648ll, 1, 0u, 1u},
      {"large negative bounded repeat", -0x1p31f, 1.25f, 1.0f, -2147483648ll, 1, 0u, 1u},
      {"zero Q rejected", 1.25f, 1.25f, 0.0f, 0, 0, 0u, 0u, 0u, false, false},
      {"NaN Q rejected", 1.25f, 1.25f, nan, 0, 0, 0u, 0u, 0u, false, false},
      {"infinite Q rejected", 1.25f, 1.25f, infinity, 0, 0, 0u, 0u, 0u, false, false},
      {"NaN S rejected", nan, 1.25f, 1.0f, 0, 0, 0u, 0u, 0u, false, false},
      {"infinite T rejected", 1.25f, infinity, 1.0f, 0, 0, 0u, 0u, 0u, false, false},
      {"positive out-of-bound rejected", 0x1p32f, 1.25f, 1.0f, 0, 0, 0u, 0u,
       0u, false, false},
      {"negative out-of-bound rejected", -0x1p32f, 1.25f, 1.0f, 0, 0, 0u, 0u,
       0u, false, false},
      {"interpolated zero Q rejected", -.25f, 1.25f, 1.0f, 0, 0, 0u, 0u,
       0u, false, false, true},
      {"interpolated zero Q reversed", -.25f, 1.25f, 1.0f, 0, 0, 0u, 0u,
       0u, true, false, true},
  }};
  for (const auto& item : cases) {
    const auto result = run_fixture(item, true);
    check(result.submitted && result.packets == 1u && result.rejected == 0u &&
              result.triangles == 1u && result.image_bytes == 256u,
          item.name, "owned GIF IMAGE plus one STQ triangle is accepted");
    const auto expected = item.valid ? texel(item.mapped_u, item.mapped_v) : 0u;
    check(result.pixel == expected, item.name,
          "sample matches signed fixed-point mapping or safe rejection");
    if (!item.valid) {
      check(result.events.empty(), item.name,
            "rejected sample does not create an accepted-write trace");
      continue;
    }
    check(result.events.size() == 1u, item.name,
          "covered owned watchpoint has one accepted write");
    const auto* event = result.events.size() == 1u ? &result.events.front() : nullptr;
    check(event && event->texture.valid &&
              static_cast<std::int64_t>(event->texture.raw_u) == item.raw_u &&
              static_cast<std::int64_t>(event->texture.raw_v) == item.raw_v &&
              event->texture.u == item.mapped_u && event->texture.v == item.mapped_v,
          item.name, "trace preserves signed raw coordinates separately from mapped UV");
    check(event && event->address == kWatchAddress && event->before == 0u &&
              event->after == expected && event->texture.raw_texel == expected &&
              event->texture.output_color == expected &&
              event->texture.address == kTextureBase +
                  (item.mapped_v * kTextureStride + item.mapped_u) * 4u,
          item.name, "physical trace identifies the selected owned texel and write");
  }
}

void observer_equality() {
  const Case item{"observer equality", -.25f, -1.25f, 1.0f,
                  -1, -2, 7u, 6u};
  const auto off = run_fixture(item, false, true);
  const auto on = run_fixture(item, true, true);
  check(off.preview == on.preview && off.vram == on.vram,
        item.name, "watching preserves all preview pixels and all 4 MiB logical VRAM");
  check(off.submitted == on.submitted && off.packets == on.packets &&
            off.rejected == on.rejected && off.triangles == on.triangles &&
            off.image_bytes == on.image_bytes && off.events.empty(),
        item.name, "watching preserves packet/draw counters and disabled listener is silent");
}

} // namespace

int main() {
  mapping_cases();
  observer_equality();
  std::printf("GS signed texture tests: %u checks, %u failures\n", checks, failures);
  return failures == 0u ? 0 : 1;
}
