#include "ps2vita/gif.hpp"
#include "ps2vita/gs_integer_math.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <type_traits>
#include <utility>
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
  std::uint32_t word;
  static_assert(sizeof(word) == sizeof(value), "32-bit float required");
  std::memcpy(&word, &value, sizeof(word));
  return word;
}

constexpr std::uint32_t kTextureBase = 0x100000u;
constexpr unsigned kStride = 64u;
constexpr int kSample = 4 / ps2vita::Gs::kRasterScale;
constexpr std::uint32_t kWatch = (4u * kStride + 4u) * 4u;
constexpr std::uint64_t kTexa = (224ull << 32u) | 0x8020u;

class Packet {
public:
  void word(std::uint64_t value, unsigned count) {
    for (unsigned byte = 0u; byte < count; ++byte)
      bytes.push_back(static_cast<std::uint8_t>(value >> (byte * 8u)));
  }
  void ad(std::uint64_t value, unsigned reg) {
    word(0x1000000000008001ull, 8u);
    word(0xEull, 8u);
    word(value, 8u);
    word(reg, 8u);
  }
  std::vector<std::uint8_t> bytes;
};

enum class Primitive { StTriangle, UvTriangle, UvSprite };

struct Case {
  const char* name;
  float u, v;
  Primitive primitive = Primitive::StTriangle;
  unsigned format = 0u;
  std::uint64_t tex1 = 0x60u;
  std::uint64_t addressing = 0u;
  float q = 1.0f;
  bool reversed = false;
  unsigned tfx = 1u;
  bool tcc = true;
  std::uint32_t vertex_color = 0x80808080u;
  std::uint64_t texa = kTexa;
  std::uint64_t test = 0x30000u;
  bool varying_uv = false;
  const std::array<std::uint32_t, 64>* texture = nullptr;
};

std::uint32_t raw_texel(unsigned format, unsigned u, unsigned v) {
  if (format == 2u) {
    if (u == 0u && v == 0u) return 0u;
    return ((3u * u + 5u * v) & 31u) |
        (((7u * u + 2u * v) & 31u) << 5u) |
        (((11u * u + 3u * v) & 31u) << 10u) |
        (((u + v) & 1u) << 15u);
  }
  if (format == 1u && u == 0u && v == 0u) return 0xDE000000u;
  return ((17u + 29u * u + 11u * v) & 255u) |
      (((199u + 7u * u - 23u * v) & 255u) << 8u) |
      (((3u + 13u * u + 31u * v) & 255u) << 16u) |
      (((10u + 41u * u + 17u * v) & 255u) << 24u);
}

std::uint32_t decode_texel(unsigned format, std::uint32_t raw,
                           std::uint64_t texa) {
  if (format == 0u) return raw;
  const unsigned ta0 = texa & 255u, ta1 = (texa >> 32u) & 255u;
  const bool aem = (texa & 0x8000u) != 0u;
  if (format == 1u) {
    const auto rgb = raw & 0xFFFFFFu;
    return rgb | ((aem && rgb == 0u ? 0u : ta0) << 24u);
  }
  const unsigned alpha = (raw & 0x8000u) != 0u ? ta1 :
      aem && raw == 0u ? 0u : ta0;
  return ((raw & 31u) << 3u) | (((raw >> 5u) & 31u) << 11u) |
      (((raw >> 10u) & 31u) << 19u) | (alpha << 24u);
}

std::uint64_t addressing(unsigned umode, unsigned vmode, unsigned umin = 0u,
                         unsigned umax = 0u, unsigned vmin = 0u,
                         unsigned vmax = 0u) {
  return umode | (std::uint64_t{vmode} << 2u) |
      (std::uint64_t{umin} << 4u) | (std::uint64_t{umax} << 14u) |
      (std::uint64_t{vmin} << 24u) | (std::uint64_t{vmax} << 34u);
}

std::int64_t floor_div(std::int64_t value, std::int64_t denominator) {
  const auto quotient = value / denominator;
  return quotient - (value < 0 && value % denominator != 0 ? 1 : 0);
}

unsigned map_coordinate(std::int64_t value, unsigned mode,
                        unsigned minimum, unsigned maximum) {
  if (mode == 0u) {
    const auto remainder = value % 8;
    return static_cast<unsigned>(remainder < 0 ? remainder + 8 : remainder);
  }
  if (mode == 1u) return static_cast<unsigned>(std::clamp<std::int64_t>(value, 0, 7));
  if (mode == 2u)
    return static_cast<unsigned>(std::clamp<std::int64_t>(value, minimum, maximum));
  return static_cast<unsigned>((static_cast<std::uint64_t>(value) & minimum) | maximum);
}

std::uint32_t lerp_word(std::uint32_t a, std::uint32_t b, unsigned fraction) {
  std::uint32_t result = 0u;
  for (unsigned shift = 0u; shift < 32u; shift += 8u) {
    // Nonnegative weighted-sum division implements a+floor((b-a)*f/16),
    // including descending channels, without implementation-defined shifts.
    const unsigned av = (a >> shift) & 255u, bv = (b >> shift) & 255u;
    result |= ((av * (16u - fraction) + bv * fraction) / 16u) << shift;
  }
  return result;
}

std::uint32_t texture_function(const Case& item, std::uint32_t color) {
  if (item.tfx == 1u)
    return item.tcc ? color : (color & 0xFFFFFFu) | (item.vertex_color & 0xFF000000u);
  std::uint32_t result = 0u;
  const unsigned vertex_alpha = item.vertex_color >> 24u;
  for (unsigned shift = 0u; shift < 24u; shift += 8u) {
    const unsigned texel = (color >> shift) & 255u;
    const unsigned vertex = (item.vertex_color >> shift) & 255u;
    const unsigned highlight = item.tfx >= 2u ? vertex_alpha : 0u;
    result |= std::min(255u, texel * vertex / 128u + highlight) << shift;
  }
  unsigned alpha = vertex_alpha;
  if (item.tcc) {
    if (item.tfx == 0u) alpha = (color >> 24u) * vertex_alpha / 128u;
    else if (item.tfx == 2u) alpha = (color >> 24u) + vertex_alpha;
    else alpha = color >> 24u;
  }
  return result | (std::min(255u, alpha) << 24u);
}

struct Expected {
  bool linear = false, accepted = true;
  std::int64_t fixed_u = 0, fixed_v = 0, raw_u = 0, raw_v = 0;
  unsigned fu = 0u, fv = 0u;
  std::array<unsigned, 4> u{}, v{};
  std::array<std::uint32_t, 4> raw{}, expanded{}, address{};
  std::uint32_t filtered = 0u, output = 0u;
};

Expected oracle(const Case& item) {
  Expected result;
  // Independent base-level model: signed16.16 truncation, half-texel shift,
  // four independently addressed taps and separable four-bit RGBA filtering.
  // Primary operation order: PCSX2 v2.8.2 GSDrawScanline.cpp lines1018..1112.
  // https://github.com/PCSX2/pcsx2/blob/v2.8.2/pcsx2/GS/Renderers/SW/GSDrawScanline.cpp
  // MXL=0 selects MMAG directly, per PCSX2 GSVertexTrace.cpp; MMIN does not
  // invent a minification decision in this bounded no-mipmap path.
  // https://github.com/PCSX2/pcsx2/blob/v2.8.2/pcsx2/GS/Renderers/Common/GSVertexTrace.cpp#L54-L59
  result.linear = ((item.tex1 >> 2u) & 7u) == 0u && (item.tex1 & 0x20u) != 0u;
  result.fixed_u = static_cast<std::int64_t>(static_cast<double>(item.u) * 65536.0);
  result.fixed_v = static_cast<std::int64_t>(static_cast<double>(item.v) * 65536.0);
  const auto shifted_u = result.fixed_u - (result.linear ? 32768 : 0);
  const auto shifted_v = result.fixed_v - (result.linear ? 32768 : 0);
  result.raw_u = floor_div(shifted_u, 65536);
  result.raw_v = floor_div(shifted_v, 65536);
  result.fu = static_cast<unsigned>((shifted_u - result.raw_u * 65536) / 4096);
  result.fv = static_cast<unsigned>((shifted_v - result.raw_v * 65536) / 4096);
  for (unsigned tap = 0u; tap < 4u; ++tap) {
    const auto u = result.raw_u + (tap & 1u);
    const auto v = result.raw_v + (tap >> 1u);
    result.u[tap] = map_coordinate(u, item.addressing & 3u,
                                   (item.addressing >> 4u) & 1023u,
                                   (item.addressing >> 14u) & 1023u);
    result.v[tap] = map_coordinate(v, (item.addressing >> 2u) & 3u,
                                   (item.addressing >> 24u) & 1023u,
                                   (item.addressing >> 34u) & 1023u);
    result.raw[tap] = item.texture ?
        (*item.texture)[result.v[tap] * 8u + result.u[tap]] :
        raw_texel(item.format, result.u[tap], result.v[tap]);
    result.expanded[tap] = decode_texel(item.format, result.raw[tap], item.texa);
    result.address[tap] = kTextureBase +
        (result.v[tap] * kStride + result.u[tap]) * (item.format == 2u ? 2u : 4u);
  }
  result.filtered = result.linear ?
      lerp_word(lerp_word(result.expanded[0], result.expanded[1], result.fu),
                lerp_word(result.expanded[2], result.expanded[3], result.fu), result.fv)
      : result.expanded[0];
  result.output = texture_function(item, result.filtered);
  if ((item.test & 1u) != 0u) {
    const unsigned alpha = result.output >> 24u;
    const unsigned reference = (item.test >> 4u) & 255u;
    const bool pass[]{false, true, alpha < reference, alpha <= reference,
                      alpha == reference, alpha >= reference,
                      alpha > reference, alpha != reference};
    result.accepted = pass[(item.test >> 1u) & 7u];
  }
  return result;
}

void upload(Packet& packet, unsigned format,
            const std::array<std::uint32_t, 64>* texture = nullptr) {
  // PSMCT24 is viewed from a32-bit upload, preserving raw high bytes that TEXA
  // must replace before filtering. PSMCT16 is uploaded as16-bit logical data.
  const unsigned transfer_format = format == 2u ? 2u : 0u;
  packet.ad((std::uint64_t{kTextureBase / 256u} << 32u) | (1ull << 48u) |
                (std::uint64_t{transfer_format} << 56u), 0x50u);
  packet.ad(0u, 0x51u);
  packet.ad(8u | (8ull << 32u), 0x52u);
  packet.ad(0u, 0x53u);
  const unsigned bytes_per_texel = format == 2u ? 2u : 4u;
  packet.word((2ull << 58u) | 0x8000u | (64u * bytes_per_texel / 16u), 8u);
  packet.word(0u, 8u);
  for (unsigned v = 0u; v < 8u; ++v)
    for (unsigned u = 0u; u < 8u; ++u)
      packet.word(texture ? (*texture)[v * 8u + u] : raw_texel(format, u, v),
                  bytes_per_texel);
}

void append_draw(Packet& packet, const Case& item, std::uint64_t frame = 1ull << 16u,
                  std::uint32_t texture_base = kTextureBase) {
  packet.ad(frame, 0x4Cu);
  packet.ad(1ull << 32u, 0x4Eu);
  packet.ad(item.test, 0x47u);
  packet.ad(0u, 0x18u);
  packet.ad((31ull << 16u) | (31ull << 48u), 0x40u);
  packet.ad((texture_base / 256u) | (1ull << 14u) |
                (std::uint64_t{item.format} << 20u) | (3ull << 26u) |
                (3ull << 30u) | (std::uint64_t{item.tcc} << 34u) |
                (std::uint64_t{item.tfx} << 35u), 0x06u);
  packet.ad(item.tex1, 0x14u);
  packet.ad(item.addressing, 0x08u);
  packet.ad(item.texa, 0x3Bu);
  const bool sprite = item.primitive == Primitive::UvSprite;
  const bool uv = item.primitive != Primitive::StTriangle;
  packet.ad((sprite ? 6u : 3u) | 0x10u | (uv ? 0x100u : 0u), 0x00u);
  const std::array<std::array<unsigned, 2>, 3> triangle{{
      {{0u, 0u}}, {{0u, 32u}}, {{32u, 0u}}}};
  const unsigned count = sprite ? 2u : 3u;
  for (unsigned vertex = 0u; vertex < count; ++vertex) {
    unsigned x = 0u, y = 0u;
    if (sprite) x = y = ((vertex != 0u) != item.reversed) ? 32u : 0u;
    else {
      const unsigned index = item.reversed && vertex != 0u ? 3u - vertex : vertex;
      x = triangle[index][0]; y = triangle[index][1];
    }
    if (uv) {
      // The varying fixture evaluates to U=1.25,V=2.0 at native(4,4).
      // It detects integer-only interpolation even when all endpoints are
      // individually representable as unsigned10.4 attributes.
      const float u = item.varying_uv ? 1.0f + x / 16.0f : item.u;
      const float v = item.varying_uv ? 1.5f + y / 8.0f : item.v;
      packet.ad(static_cast<unsigned>(u * 16.0f) |
                    (std::uint64_t{static_cast<unsigned>(v * 16.0f)} << 16u), 0x03u);
    } else {
      packet.ad(std::uint64_t{bits(item.u * item.q / 8.0f)} |
                    (std::uint64_t{bits(item.v * item.q / 8.0f)} << 32u), 0x02u);
    }
    packet.ad((std::uint64_t{bits(item.q)} << 32u) | item.vertex_color, 0x01u);
    packet.ad(x * 16u | (std::uint64_t{y * 16u} << 16u) | (1ull << 32u), 0x05u);
  }
}

template<class T, class = void>
struct HasFilterTrace : std::false_type {};
template<class T>
struct HasFilterTrace<T, std::void_t<decltype(std::declval<T>().linear)>>
    : std::true_type {};

template<class T>
bool extended_trace_matches(const T& trace, const Expected& expected) {
  if constexpr (HasFilterTrace<T>::value) {
    if (trace.linear != expected.linear) return false;
    if (!expected.linear) return true;
    return trace.fixed_u == expected.fixed_u && trace.fixed_v == expected.fixed_v &&
        trace.fraction_u == expected.fu && trace.fraction_v == expected.fv &&
        trace.tap_u == expected.u && trace.tap_v == expected.v &&
        trace.tap_address == expected.address && trace.tap_raw == expected.raw &&
        trace.tap_expanded == expected.expanded;
  }
  // The preserved pre-feature headers have only the existing tap0 fields.
  // Baseline compilation intentionally skips the additional instrumentation.
  return true;
}

struct Result {
  bool accepted = false;
  std::uint32_t pixel = 0u;
  std::uint64_t triangles = 0u, sprites = 0u, rejected = 0u, image_bytes = 0u;
  std::size_t nonzero_pixels = 0u;
  std::vector<ps2vita::GsPixelWrite> events;
  std::vector<std::uint32_t> preview, vram;
};

Result run(const Case& item, bool watch = true, bool snapshot = false,
           bool inspect_preview = false) {
  ps2vita::Gs gs;
  ps2vita::Gif gif(gs);
  Result result;
  if (watch)
    check(gs.set_pixel_watch({kWatch}, [&](const ps2vita::GsPixelWrite& event) {
      result.events.push_back(event);
    }), item.name, "owned physical pixel watch attaches");
  Packet packet;
  upload(packet, item.format, item.texture);
  append_draw(packet, item);
  result.accepted = gif.submit(packet.bytes.data(), packet.bytes.size());
  result.pixel = gs.pixel(kSample, kSample);
  result.triangles = gif.triangles_emitted();
  result.sprites = gif.sprites_emitted();
  result.rejected = gif.packets_rejected();
  result.image_bytes = gif.image_bytes();
  if (snapshot || inspect_preview) {
    for (int y = 0; y < ps2vita::Gs::kHeight; ++y)
      for (int x = 0; x < ps2vita::Gs::kWidth; ++x) {
        const auto pixel = gs.pixel(x, y);
        result.nonzero_pixels += pixel != 0u;
        if (snapshot) result.preview.push_back(pixel);
      }
  }
  if (snapshot) {
    for (std::uint32_t address = 0u; address < 0x400000u; address += 4u)
      result.vram.push_back(gif.read_local32(address));
  }
  return result;
}

void run_case(const Case& item) {
  const auto expected = oracle(item);
  const auto result = run(item);
  const bool sprite = item.primitive == Primitive::UvSprite;
  check(result.accepted && result.rejected == 0u &&
            result.triangles == (sprite ? 0u : 1u) &&
            result.sprites == (sprite ? 1u : 0u) &&
            result.image_bytes == (item.format == 2u ? 128u : 256u),
        item.name, "owned GIF texture upload and primitive are accepted");
  check(result.pixel == (expected.accepted ? expected.output : 0u), item.name,
        "pixel agrees with independent four-tap/address/filter/TFX oracle");
  check(result.events.size() == (expected.accepted ? 1u : 0u), item.name,
        "alpha rejection/accepted write is observed after filtering and TFX");
  if (!expected.accepted) return;
  const auto* event = result.events.size() == 1u ? &result.events.front() : nullptr;
  check(event && event->texture.valid && event->texture.format == item.format &&
            event->texture.raw_u == expected.raw_u && event->texture.raw_v == expected.raw_v &&
            event->texture.u == expected.u[0] && event->texture.v == expected.v[0] &&
            event->texture.address == expected.address[0] &&
            event->texture.raw_texel == expected.raw[0],
        item.name, "trace identifies independently addressed first tap");
  check(event && event->texture.expanded_texel == expected.filtered &&
            event->texture.output_color == expected.output && event->after == expected.output,
        item.name, "trace keeps filtered RGBA separate from post-TFX output");
  check(event && extended_trace_matches(event->texture, expected), item.name,
        "when supported, fixed coordinates/fractions/all four taps agree");
}

void coverage_cases() {
  const std::array<Case, 14> cases{{
      {"repeat negative quarter", -.25f, .5f},
      {"repeat positive quarter", .25f, .5f},
      {"half-texel center", .5f, .5f},
      {"one-axis half blend", 1.0f, .5f},
      {"two-axis half blend", 1.0f, 1.0f},
      {"two-axis mixed fractions", 1.25f, 2.75f},
      {"repeat V edge", 1.25f, -.25f},
      {"repeat both negative", -.25f, -1.25f},
      {"tiny negative truncates before half shift", -0x1p-18f, .5f},
      {"top-four fraction below LSB", .5f + 0x1p-5f, .5f},
      {"top-four fraction one LSB", .5f + 0x1p-4f, .5f},
      {"repeat high edge", 7.75f, 7.75f},
      {"FST triangle fractions", 1.25f, 2.75f, Primitive::UvTriangle},
      {"FST sprite fractions", 1.25f, 2.75f, Primitive::UvSprite},
  }};
  for (const auto& item : cases) run_case(item);

  Case item{"clamp four edge taps", -.25f, .25f};
  item.addressing = addressing(1u, 1u); run_case(item);
  item = {"region clamp each tap", 2.0f, 3.0f};
  item.addressing = addressing(2u, 2u, 2u, 5u, 3u, 6u); run_case(item);
  item = {"region repeat each tap", -.25f, 1.0f};
  item.addressing = addressing(3u, 3u, 3u, 4u, 1u, 2u); run_case(item);
  item = {"reversed triangle fractions", 1.25f, 2.75f};
  item.reversed = true; run_case(item);
  item = {"reversed FST sprite fractions", 1.25f, 2.75f, Primitive::UvSprite};
  item.reversed = true; run_case(item);
  item = {"Q2 preserves fractional ratio", 1.25f, 2.75f};
  item.q = 2.0f; run_case(item);
  item = {"negative Q preserves fractional ratio", -.25f, 2.75f};
  item.q = -1.0f; run_case(item);
  item = {"nearest unchanged", 1.25f, 2.75f};
  item.tex1 = 0u; run_case(item);
  item = {"MXL0 MMAG1 MMIN0 is linear", 1.25f, 2.75f};
  item.tex1 = 0x20u; run_case(item);
  item = {"MXL0 MMAG0 MMIN1 is nearest", 1.25f, 2.75f};
  item.tex1 = 0x40u; run_case(item);
  item = {"MXL1 existing fallback not LOD claim", 1.25f, 2.75f};
  item.tex1 = 0x64u; run_case(item);
  for (const auto primitive : {Primitive::UvTriangle, Primitive::UvSprite}) {
    item = {primitive == Primitive::UvTriangle ? "varying FST triangle fractions" :
             "varying FST sprite fractions", 1.25f, 2.0f, primitive};
    item.varying_uv = true; run_case(item);
    item.name = primitive == Primitive::UvTriangle ? "reversed varying FST triangle" :
                                                    "reversed varying FST sprite";
    item.reversed = true; run_case(item);
  }
  item = {"nearest FST sprite unchanged", 1.25f, 2.75f, Primitive::UvSprite};
  item.tex1 = 0u; run_case(item);
}

void format_and_order_cases() {
  Case item{"PSM24 TEXA before interpolation", 1.0f, 1.0f};
  item.format = 1u; run_case(item);
  item = {"PSM16 alpha/RGB decode before interpolation", 1.0f, 1.0f};
  item.format = 2u; run_case(item);
  item = {"PSM16 AEM disabled", 1.0f, 1.0f};
  item.format = 2u; item.texa &= ~0x8000ull; run_case(item);
  item = {"filter before MODULATE", 1.25f, 2.75f};
  item.tfx = 0u; item.vertex_color = 0x61495D3Fu; run_case(item);
  const auto model = oracle(item);
  const auto premature = lerp_word(
      lerp_word(texture_function(item, model.expanded[0]),
                texture_function(item, model.expanded[1]), model.fu),
      lerp_word(texture_function(item, model.expanded[2]),
                texture_function(item, model.expanded[3]), model.fu), model.fv);
  check(model.output != premature, item.name,
        "owned values distinguish filter-then-TFX from TFX-per-tap");
  item = {"DECAL RGB keeps vertex alpha", 1.0f, 1.0f};
  item.tcc = false; item.vertex_color = 0x73808080u; run_case(item);
  item = {"filtered alpha GEQUAL passes", 1.0f, 1.0f};
  item.test |= 1u | (5u << 1u) | (30u << 4u); run_case(item);
  item = {"filtered alpha GEQUAL rejects", 1.0f, 1.0f};
  item.test |= 1u | (5u << 1u) | (50u << 4u); run_case(item);
  item = {"MODULATE alpha test is after TFX", 1.0f, 1.0f};
  item.tfx = 0u; item.vertex_color = 0x40808080u;
  item.test |= 1u | (5u << 1u) | (30u << 4u); run_case(item);
}

void perspective_guard_cases() {
  const float nan = std::numeric_limits<float>::quiet_NaN();
  const float infinity = std::numeric_limits<float>::infinity();
  const float huge = std::numeric_limits<float>::max();
  std::array<Case, 16> invalid{{
      {"NaN S guarded", nan, .5f},
      {"NaN T guarded", .5f, nan},
      {"positive infinite S guarded", infinity, .5f},
      {"negative infinite S guarded", -infinity, .5f},
      {"positive infinite T guarded", .5f, infinity},
      {"negative infinite T guarded", .5f, -infinity},
      {"zero Q guarded", 1.25f, 2.75f},
      {"NaN Q guarded", 1.25f, 2.75f},
      {"positive infinite Q guarded", 1.25f, 2.75f},
      {"negative infinite Q guarded", 1.25f, 2.75f},
      {"positive U beyond UINT_MAX guarded", 0x1p32f, .5f},
      {"negative U beyond UINT_MAX guarded", -0x1p32f, .5f},
      {"positive V beyond UINT_MAX guarded", .5f, 0x1p32f},
      {"negative V beyond UINT_MAX guarded", .5f, -0x1p32f},
      {"largest finite U guarded", huge, .5f},
      {"most negative finite V guarded", .5f, -huge},
  }};
  invalid[6].q = 0.0f;
  invalid[7].q = nan;
  invalid[8].q = infinity;
  invalid[9].q = -infinity;
  for (const auto& original : invalid) {
    // Never invoke oracle() for invalid inputs: its fixed-point conversion is
    // intentionally only an oracle for finite, bounded coordinates.
    for (const auto tex1 : {0x20ull, 0ull}) {
      auto item = original;
      item.tex1 = tex1;
      const auto result = run(item, true, false, true);
      check(result.accepted && result.rejected == 0u && result.triangles == 1u &&
                result.sprites == 0u && result.image_bytes == 256u,
            item.name, "linear/nearest guard rejects samples, not the valid GIF packet");
      check(result.pixel == 0u, item.name,
            "linear/nearest invalid STQ does not change the watched pixel");
      check(result.nonzero_pixels == 0u, item.name,
            "constant invalid STQ emits no visible fragment anywhere in the preview");
      check(result.events.empty(), item.name,
            "linear/nearest invalid STQ creates no accepted-write trace");
    }
  }

  // Largest binary32 native coordinate below 2^32: UINT_MAX itself is not
  // representable. Both signs fit the existing logical sampler's experimental
  // +/-UINT_MAX bound and their signed16.16 values safely fit int64_t.
  // This is a safety/mapping contract, not native GS overflow emulation.
  const float inside = std::nextafter(0x1p32f, 0.0f);
  const std::array<Case, 6> bounded{{
      {"bounded positive U near UINT_MAX", inside, .5f},
      {"bounded negative U near UINT_MAX", -inside, .5f},
      {"bounded positive V near UINT_MAX", .5f, inside},
      {"bounded negative V near UINT_MAX", .5f, -inside},
      {"bounded both positive near UINT_MAX", inside, inside},
      {"bounded both negative near UINT_MAX", -inside, -inside},
  }};
  for (const auto& original : bounded)
    for (const auto tex1 : {0x20ull, 0ull}) {
      auto item = original;
      item.tex1 = tex1;
      run_case(item);
    }
}

void exact_rounding_cases() {
  std::array<std::uint32_t, 64> texture{};
  texture.fill(0xFFFFFFFFu);
  texture[1] = 0u;
  Case descending{"descending 255 to zero fraction one", .5f + 1.0f / 16.0f, .5f};
  descending.texture = &texture;
  check(oracle(descending).filtered == 0xEFEFEFEFu, descending.name,
        "255 + floor((0-255)/16) is 239 in every channel, not 240");
  run_case(descending);

  texture.fill(0x80808080u);
  texture[9] = 0x8F8F8F8Fu;
  Case sequential{"sequential U then V rounding", .5f + 3.0f / 16.0f,
                  .5f + 7.0f / 16.0f};
  sequential.texture = &texture;
  // Four taps 128/128/128/143, fu=3 and fv=7: U gives 128/130,
  // then V gives 128. A single four-tap division gives 129 instead.
  check(oracle(sequential).filtered == 0x80808080u &&
            (128u * 256u + 15u * 3u * 7u) / 256u == 129u,
        sequential.name, "owned values distinguish two-stage from one four-tap rounding");
  run_case(sequential);
}

void exhaustive_byte_math() {
  bool lerp_matches = true, lerp_bounds = true, blend_matches = true;
  // Independent positive weighted-sum identity: numerator cannot be negative.
  // Exhaust all 256*256*16 endpoint/fraction combinations, not just products.
  for (int a = 0; a < 256; ++a)
    for (int b = 0; b < 256; ++b)
      for (unsigned fraction = 0; fraction < 16u; ++fraction) {
        const auto expected = (a * static_cast<int>(16u - fraction) +
                               b * static_cast<int>(fraction)) / 16;
        const auto actual = ps2vita::gs_detail::lerp_byte_4bit(a, b, fraction);
        lerp_matches &= actual == expected;
        lerp_bounds &= actual >= 0 && actual <= 255;
      }
  // Independent signed quotient/remainder definition covers every integer
  // in the blend-product interval, including values no byte product reaches.
  for (int product = -65025; product <= 65025; ++product) {
    const auto expected = product / 128 - (product % 128 < 0 ? 1 : 0);
    blend_matches &= ps2vita::gs_detail::floor_blend_product_7bit(product) == expected;
  }
  check(lerp_matches, "exhaustive byte interpolation",
        "all 1,048,576 endpoint/fraction combinations match positive affine division");
  check(lerp_bounds, "exhaustive byte interpolation",
        "horizontal results remain valid byte endpoints for vertical interpolation");
  check(blend_matches, "exhaustive signed blend rounding",
        "all 130,051 bounded products match floor from signed quotient/remainder");
}

void wrapped_target_mask_transitions() {
  constexpr auto name = "wrapped target masked/unmasked transitions";
  constexpr std::uint32_t bytes = 4u * 1024u * 1024u;
  constexpr unsigned scale = ps2vita::Gs::kRasterScale;
  constexpr unsigned pitch = 63u * 64u;
  constexpr std::uint32_t base = 0x1FFu * 8192u;
  constexpr int host_y = scale == 1u ? 1 : 0;
  constexpr std::uint64_t binding = 0x1FFu | (63ull << 16u);
  std::vector<std::uint8_t> expected(bytes);
  for (std::size_t i = 0u; i < expected.size(); ++i)
    expected[i] = static_cast<std::uint8_t>(i * 29u + i / 257u + 7u);
  // Use independent uint64/modulo addressing, not a call into the private
  // target helper. At scale 4, only row 0 lies below the VRAM end; scale 1's
  // chosen native row 1 is already wrapped to byte 0x1F00.
  std::vector<std::uint32_t> addresses;
  for (unsigned dy = 0u; dy < scale; ++dy)
    for (unsigned dx = 0u; dx < scale; ++dx)
      addresses.push_back(static_cast<std::uint32_t>((std::uint64_t{base} +
          (std::uint64_t{static_cast<unsigned>(host_y) * scale + dy} * pitch + dx) * 4u) % bytes));
  check(addresses.size() == scale * scale &&
            addresses.front() == (scale == 1u ? 0x1F00u : base) &&
            (scale == 1u || addresses[scale] == 0x1F00u), name,
        "owned tile crosses VRAM end at quarter scale or starts wrapped at native scale");
  const auto load = [&](const std::vector<std::uint8_t>& memory, std::uint32_t at) {
    std::uint32_t value = 0u;
    for (unsigned byte = 0u; byte < 4u; ++byte)
      value |= std::uint32_t{memory[(std::uint64_t{at} + byte) % bytes]} << (8u * byte);
    return value;
  };
  const auto store = [&](std::vector<std::uint8_t>& memory, std::uint32_t at,
                         std::uint32_t value) {
    for (unsigned byte = 0u; byte < 4u; ++byte)
      memory[(std::uint64_t{at} + byte) % bytes] = static_cast<std::uint8_t>(value >> (8u * byte));
  };
  for (std::size_t i = 0u; i < addresses.size(); ++i) {
    const auto index = static_cast<unsigned>(i);
    store(expected, addresses[i], ((17u + 13u * index) << 24u) |
        ((48u + 9u * index) << 16u) | ((64u + 7u * index) << 8u) | (80u + 11u * index));
  }
  auto off_memory = expected, on_memory = expected;
  ps2vita::Gs off, on;
  off.set_depth_state(ps2vita::Gs::DepthTest::Always, false);
  on.set_depth_state(ps2vita::Gs::DepthTest::Always, false);
  std::vector<ps2vita::GsPixelWrite> events;
  check(on.set_pixel_watch(addresses, [&](const ps2vita::GsPixelWrite& event) {
    events.push_back(event);
  }), name, "each tile word has an aligned physical watch");
  struct Operation {
    std::uint64_t frame;
    std::uint32_t mask, color, z;
  };
  const std::array<Operation, 4> operations{{
      {binding | (0xFF0F00F0ull << 32u), 0xFF0F00F0u, 0xE6B7C8D9u, 0u},
      {binding, 0u, 0x279ABCEDu, std::numeric_limits<std::uint32_t>::max()},
      {binding | (1ull << 24u), 0xFF000000u, 0xF0012345u, 0x12345678u},
      {binding | (0xFFFFFFFFull << 32u), 0xFFFFFFFFu, 0xFEDCBA98u, 42u}}};
  for (unsigned step = 0u; step < operations.size(); ++step) {
    const auto& operation = operations[step];
    if (step == 2u) {
      // Restore distinct destination alpha after the unmasked overwrite so
      // PSMCT24 must preserve EACH word, not a cached tile-origin alpha.
      // These owned backing-memory seeds are not raster operations/events.
      for (std::size_t i = 0u; i < addresses.size(); ++i) {
        const auto value = ((33u + 11u * static_cast<unsigned>(i)) << 24u) | 0x00987654u;
        store(expected, addresses[i], value);
        store(off_memory, addresses[i], value); store(on_memory, addresses[i], value);
      }
    }
    std::vector<std::uint32_t> before, after;
    for (const auto address : addresses) {
      const auto old = load(expected, address);
      // Bit-mask selection expressed independently as conditional bit flips.
      const auto value = old ^ ((old ^ operation.color) & ~operation.mask);
      before.push_back(old); after.push_back(value);
      store(expected, address, value);
    }
    off.set_color_target(&off_memory, operation.frame);
    on.set_color_target(&on_memory, operation.frame);
    on.set_draw_trace({ps2vita::GsTracePrimitive::Point, step, 100u + step,
                       0u, operation.frame});
    events.clear();
    off.point({0, host_y, operation.z, operation.color});
    on.point({0, host_y, operation.z, operation.color});
    check(off_memory == expected && on_memory == expected, name,
          "every tile byte matches owned mask selection and every unused byte is unchanged");
    check(off_memory == on_memory, name,
          "watch off/on leaves all four MiB identical at each mask transition");
    const auto index = static_cast<std::size_t>(host_y * ps2vita::Gs::kWidth);
    check(off.pixel(0, host_y) == after.front() && on.pixel(0, host_y) == after.front() &&
              off.pixels()[index] == after.front() && on.pixels()[index] == after.front(), name,
          "active target pixel and refreshed preview retain the exact origin word");
    check(events.size() == addresses.size(), name,
          "accepted unmasked, partial, PSMCT24 and fully masked operations all notify every watch");
    for (std::size_t i = 0u; i < std::min(events.size(), addresses.size()); ++i) {
      const auto& event = events[i];
      check(event.address == addresses[i] && event.before == before[i] && event.after == after[i] &&
                event.input_color == operation.color && event.z == operation.z &&
                event.x == 0 && event.y == host_y && event.frame == operation.frame &&
                event.draw.frame == operation.frame && event.draw.sequence == step &&
                event.draw.packet == 100u + step && event.draw.kind == ps2vita::GsTracePrimitive::Point &&
                !event.texture.valid, name,
            "watch metadata keeps each word's old/new value, raw input, depth and exact binding");
    }
  }
}

void observer_and_feedback() {
  const Case item{"observer equality", -.25f, 1.25f};
  const auto off = run(item, false, true), on = run(item, true, true);
  check(off.preview == on.preview && off.vram == on.vram, item.name,
        "filter tracing preserves the entire preview and all 4 MiB logical VRAM");
  check(off.events.empty() && off.triangles == on.triangles &&
            off.sprites == on.sprites && off.rejected == on.rejected &&
            off.image_bytes == on.image_bytes,
        item.name, "disabled observer is silent and primitive/transfer counters agree");

  ps2vita::Gs gs;
  ps2vita::Gif gif(gs);
  const Case source{"owned filtered framebuffer feedback", 1.25f, 2.75f};
  const auto expected = oracle(source).output;
  Packet first;
  upload(first, 0u); append_draw(first, source);
  check(gif.submit(first.bytes.data(), first.bytes.size()), source.name,
        "first filtered draw is accepted");
  Case copy = source;
  copy.u = copy.v = 4.5f; // All four taps lie in the uniformly written region.
  Packet second;
  append_draw(second, copy, 64u | (1ull << 16u), 0u);
  check(gif.submit(second.bytes.data(), second.bytes.size()) &&
            gs.pixel(kSample, kSample) == expected &&
            gif.read_local32(64u * 8192u + kWatch) == expected,
        source.name, "later bilinear sampling reads the rendered logical color target");
}

void local_word_reads() {
  constexpr std::uint32_t bytes = 0x400000u;
  const auto pattern = [](std::uint32_t address) {
    return static_cast<std::uint8_t>((address * 73u) ^ (address >> 8u) ^
                                     (address >> 16u) ^ 0xA5u);
  };
  const auto expected = [&](std::uint32_t address) {
    std::uint32_t value = 0u;
    for (unsigned byte = 0u; byte < 4u; ++byte)
      value |= std::uint32_t{pattern(static_cast<std::uint32_t>(
          (std::uint64_t{address} + byte) % bytes))} << (byte * 8u);
    return value;
  };
  ps2vita::Gs gs;
  ps2vita::Gif gif(gs);
  Packet packet;
  packet.ad(8ull << 48u, 0x50u); // 512-wide, 32-bit destination at base zero.
  packet.ad(0u, 0x51u);
  packet.ad(512u | (2048ull << 32u), 0x52u);
  packet.ad(0u, 0x53u);
  for (std::uint32_t position = 0u; position < bytes;) {
    const auto qwords = std::min<std::uint32_t>((bytes - position) / 16u, 0x7FFFu);
    packet.word(0x0800000000008000ull | qwords, 8u);
    packet.word(0u, 8u);
    for (std::uint32_t qword = 0u; qword < qwords; ++qword) {
      for (unsigned half = 0u; half < 2u; ++half) {
        std::uint64_t value = 0u;
        for (unsigned byte = 0u; byte < 8u; ++byte)
          value |= std::uint64_t{pattern(position++)} << (byte * 8u);
        packet.word(value, 8u);
      }
    }
  }
  const char* name = "owned full-VRAM byte-read oracle";
  check(gif.submit(packet.bytes.data(), packet.bytes.size()) &&
            gif.packets_rejected() == 0u && gif.pending_bytes() == 0u &&
            gif.image_bytes() == bytes && gif.local_bytes_written() == bytes &&
            gif.image_tags() == 9u,
        name, "nine legal IMAGE tags seed all 4 MiB from owned byte data");
  std::array<bool, 4> matching{{true, true, true, true}};
  for (std::uint32_t address = 0u; address < bytes; ++address)
    matching[address & 3u] = matching[address & 3u] &&
        gif.read_local32(address) == expected(address);
  for (unsigned alignment = 0u; alignment < matching.size(); ++alignment)
    check(matching[alignment], name,
          alignment == 0u ? "every aligned little-endian word matches independent bytes" :
                           "every unaligned word, including wrap, matches independent bytes");
  constexpr std::array<std::uint32_t, 14> aliases{{
      0x00400000u, 0x00400001u, 0x00400002u, 0x00400003u,
      0x007FFFFCu, 0x007FFFFDu, 0x007FFFFEu, 0x007FFFFFu,
      0xFFC00000u, 0xFFC01234u, 0xFFFFFFFCu, 0xFFFFFFFDu,
      0xFFFFFFFEu, 0xFFFFFFFFu}};
  bool aliases_match = true;
  for (const auto address : aliases)
    aliases_match = aliases_match && gif.read_local32(address) == expected(address);
  check(aliases_match, name,
        "high-address aliases and uint32 boundary wraps preserve byte semantics");

  // A later IMAGE write must immediately replace live words. It cannot be a
  // decoded-texel cache. Overwrite the final qword, then cross-wrap reads into
  // the untouched patterned first bytes and test high-address aliases again.
  Packet overwrite;
  overwrite.ad((0x3FFFull << 32u) | (1ull << 48u), 0x50u);
  overwrite.ad(60ull << 32u, 0x51u);
  overwrite.ad(4u | (1ull << 32u), 0x52u); overwrite.ad(0u, 0x53u);
  overwrite.word(0x0800000000008001ull, 8u); overwrite.word(0u, 8u);
  overwrite.word(0x1716151413121110ull, 8u);
  overwrite.word(0x1F1E1D1C1B1A1918ull, 8u);
  check(gif.submit(overwrite.bytes.data(), overwrite.bytes.size()) &&
            gif.local_bytes_written() == bytes + 16u,
        name, "subsequent IMAGE changes live final-qword bytes");
  bool changed_match = true;
  for (std::uint32_t low = bytes - 20u; low < bytes; ++low) {
    std::uint32_t value = 0u;
    for (unsigned byte = 0u; byte < 4u; ++byte) {
      const auto at = static_cast<std::uint32_t>((std::uint64_t{low} + byte) % bytes);
      const auto data = at >= bytes - 16u ?
          static_cast<std::uint8_t>(0x10u + at - (bytes - 16u)) : pattern(at);
      value |= std::uint32_t{data} << (byte * 8u);
    }
    changed_match = changed_match && gif.read_local32(low) == value &&
        gif.read_local32(low | 0xFFC00000u) == value;
  }
  check(changed_match, name, "aligned and wrapped aliases observe the changed bytes immediately");
  gif.reset();
  bool reset_zero = true;
  for (const auto address : aliases) reset_zero = reset_zero && gif.read_local32(address) == 0u;
  check(reset_zero && gif.local_bytes_written() == 0u, name,
        "reset clears live source words with no persistent decoded cache");
}

} // namespace

int main() {
  coverage_cases();
  format_and_order_cases();
  perspective_guard_cases();
  exact_rounding_cases();
  exhaustive_byte_math();
  wrapped_target_mask_transitions();
  observer_and_feedback();
  local_word_reads();
  std::printf("GIF bilinear tests: %u checks, %u failures; extended_trace=%u scale=%d\n",
              checks, failures, unsigned(HasFilterTrace<ps2vita::GsTextureTrace>::value),
              ps2vita::Gs::kRasterScale);
  return failures == 0u ? 0 : 1;
}
