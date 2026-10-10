#include "ps2vita/gif.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

unsigned checks = 0u, failures = 0u;

void check(bool condition, const char* name, const char* property) {
  ++checks;
  if (!condition) {
    ++failures;
    std::fprintf(stderr, "FAIL [%s]: %s\n", name, property);
  }
}

constexpr int kScale = ps2vita::Gs::kRasterScale;
constexpr int kStep = 16 * kScale;
constexpr int kGrid = 32 / kScale;
constexpr std::uint32_t kSolid = 0x80654321u;
constexpr std::uint32_t kTextureBase = 0x100000u;
constexpr unsigned kInheritedOffset = std::numeric_limits<unsigned>::max();
constexpr std::array<std::array<int, 2>, 4> kWatch{{
    {{4, 4}}, {{8, 8}}, {{12, 4}}, {{4, 12}}}};

std::uint32_t bits(float value) {
  std::uint32_t result;
  std::memcpy(&result, &value, sizeof(result));
  return result;
}

std::uint32_t texel(unsigned u, unsigned v) {
  return 0x80000000u | (32u + u) | ((64u + v) << 8u) |
      ((8u * v + u) << 16u);
}

struct Vertex {
  // Signed GS 12.4 coordinates AFTER this vertex's XYOFFSET subtraction.
  int x, y;
  std::uint32_t color = kSolid, z = 100u;
  float u = .5f, v = .5f, q = 1.0f;
  unsigned offset_x = kInheritedOffset, offset_y = kInheritedOffset;
};

using Triangle = std::array<Vertex, 3>;
enum class Texture { None, Uv, Stq };

struct Case {
  const char* name;
  std::vector<Triangle> triangles;
  bool gouraud = false;
  Texture texture = Texture::None;
  unsigned offset_x = 0x8003u, offset_y = 0x8017u;
  unsigned left = 0u, top = 0u, right = 31u, bottom = 31u;
};

std::int64_t edge(const Vertex& a, const Vertex& b, int x, int y) {
  return std::int64_t{x - a.x} * (b.y - a.y) -
      std::int64_t{y - a.y} * (b.x - a.x);
}

bool inclusive(const Vertex& a, const Vertex& b) {
  return b.y > a.y || (b.y == a.y && b.x < a.x);
}

struct Sample {
  bool covered = false;
  std::uint32_t color = 0u, z = 0u, vertex_color = 0u;
  std::int64_t u = 0, v = 0;
};

Sample oracle(const Case& item, Triangle triangle, int native_x, int native_y) {
  Sample result;
  if (native_x < static_cast<int>(item.left) || native_y < static_cast<int>(item.top) ||
      native_x > static_cast<int>(item.right) || native_y > static_cast<int>(item.bottom))
    return result;
  auto area = edge(triangle[0], triangle[1], triangle[2].x, triangle[2].y);
  if (area == 0) return result;
  const auto flat_color = triangle[2].color;
  if (area < 0) {
    std::swap(triangle[1], triangle[2]);
    area = -area;
  }
  // Independent small half-plane oracle at INTEGER native anchors. It never
  // rounds vertices to host coordinates and never uses a raster bounding box.
  // This tests the logical non-AA contract, not full GS coverage precision.
  const std::array<std::int64_t, 3> weights{{
      edge(triangle[1], triangle[2], native_x * 16, native_y * 16),
      edge(triangle[2], triangle[0], native_x * 16, native_y * 16),
      edge(triangle[0], triangle[1], native_x * 16, native_y * 16)}};
  const std::array<bool, 3> included{{
      inclusive(triangle[1], triangle[2]), inclusive(triangle[2], triangle[0]),
      inclusive(triangle[0], triangle[1])}};
  for (unsigned i = 0u; i < 3u; ++i)
    if (weights[i] < 0 || (weights[i] == 0 && !included[i])) return result;
  result.covered = true;
  // Subtracting a common minimum is an exact affine identity independent of
  // the core's split16 overflow fix. Owned huge-area cases have Z deltas at
  // most65535; ordinary cases have small area, so these products fit uint64.
  const auto minimum = std::min({triangle[0].z, triangle[1].z, triangle[2].z});
  std::uint64_t z_delta = 0u;
  for (unsigned i = 0u; i < 3u; ++i)
    z_delta += std::uint64_t{triangle[i].z - minimum} *
        static_cast<std::uint64_t>(weights[i]);
  result.z = minimum + static_cast<std::uint32_t>(z_delta / area);
  result.vertex_color = flat_color;
  if (item.gouraud) {
    result.vertex_color = 0u;
    for (unsigned shift = 0u; shift < 32u; shift += 8u) {
      std::uint64_t sum = 0u;
      for (unsigned i = 0u; i < 3u; ++i)
        sum += ((triangle[i].color >> shift) & 255u) *
            static_cast<std::uint64_t>(weights[i]);
      result.vertex_color |= static_cast<std::uint32_t>(sum / area) << shift;
    }
  }
  result.color = result.vertex_color;
  if (item.texture == Texture::None) return result;
  if (item.texture == Texture::Uv) {
    std::uint64_t u = 0u, v = 0u;
    for (unsigned i = 0u; i < 3u; ++i) {
      u += static_cast<unsigned>(triangle[i].u * 16.0f) *
          static_cast<std::uint64_t>(weights[i]);
      v += static_cast<unsigned>(triangle[i].v * 16.0f) *
          static_cast<std::uint64_t>(weights[i]);
    }
    result.u = static_cast<std::int64_t>(u / (area * 16));
    result.v = static_cast<std::int64_t>(v / (area * 16));
  } else {
    double u = 0.0, v = 0.0, q = 0.0;
    for (unsigned i = 0u; i < 3u; ++i) {
      // Match the owned guest float words, not an unrounded host attribute.
      const float s = triangle[i].u * triangle[i].q / 8.0f;
      const float t = triangle[i].v * triangle[i].q / 8.0f;
      u += static_cast<double>(s) * static_cast<double>(weights[i]);
      v += static_cast<double>(t) * static_cast<double>(weights[i]);
      q += static_cast<double>(triangle[i].q) * static_cast<double>(weights[i]);
    }
    // All owned coordinates/Q are positive and bounded here; signed fixed
    // rounding and invalid-STQ safety have their own regression suites.
    result.u = static_cast<std::int64_t>(u * 8.0 / q * 65536.0) / 65536;
    result.v = static_cast<std::int64_t>(v * 8.0 / q * 65536.0) / 65536;
  }
  result.color = texel(static_cast<unsigned>(result.u) & 7u,
                       static_cast<unsigned>(result.v) & 7u);
  return result;
}

class Packet {
public:
  void word(std::uint64_t value, unsigned count = 8u) {
    for (unsigned byte = 0u; byte < count; ++byte)
      bytes.push_back(static_cast<std::uint8_t>(value >> (8u * byte)));
  }
  void ad(std::uint64_t value, unsigned reg) {
    word(0x1000000000008001ull); word(0xEull); word(value); word(reg);
  }
  std::vector<std::uint8_t> bytes;
};

void upload(Packet& packet) {
  packet.ad((std::uint64_t{kTextureBase / 256u} << 32u) | (1ull << 48u), 0x50u);
  packet.ad(0u, 0x51u); packet.ad(8u | (8ull << 32u), 0x52u); packet.ad(0u, 0x53u);
  packet.word((2ull << 58u) | 0x8000u | 16u); packet.word(0u);
  for (unsigned v = 0u; v < 8u; ++v)
    for (unsigned u = 0u; u < 8u; ++u) packet.word(texel(u, v), 4u);
}

unsigned offset(const Vertex& vertex, const Case& item, bool y) {
  const unsigned local = y ? vertex.offset_y : vertex.offset_x;
  return local == kInheritedOffset ? (y ? item.offset_y : item.offset_x) : local;
}

std::uint64_t xyz(const Vertex& vertex, const Case& item) {
  const auto x = static_cast<unsigned>(vertex.x + static_cast<int>(offset(vertex, item, false)));
  const auto y = static_cast<unsigned>(vertex.y + static_cast<int>(offset(vertex, item, true)));
  return x | (std::uint64_t{y} << 16u) | (std::uint64_t{vertex.z} << 32u);
}

Packet make_packet(const Case& item) {
  Packet packet;
  if (item.texture != Texture::None) upload(packet);
  packet.ad(1ull << 16u, 0x4Cu); packet.ad(1ull << 32u, 0x4Eu);
  packet.ad(0x30000u, 0x47u);
  packet.ad(item.left | (std::uint64_t{item.right} << 16u) |
                (std::uint64_t{item.top} << 32u) | (std::uint64_t{item.bottom} << 48u),
            0x40u);
  if (item.texture != Texture::None) {
    packet.ad((kTextureBase / 256u) | (1ull << 14u) | (3ull << 26u) |
                  (3ull << 30u) | (1ull << 34u) | (1ull << 35u), 0x06u);
    packet.ad(0u, 0x14u); packet.ad(0u, 0x08u);
  }
  packet.ad(3u | (item.gouraud ? 8u : 0u) |
                (item.texture == Texture::None ? 0u : 0x10u) |
                (item.texture == Texture::Uv ? 0x100u : 0u), 0x00u);
  for (const auto& triangle : item.triangles)
    for (const auto& vertex : triangle) {
      packet.ad(offset(vertex, item, false) |
                    (std::uint64_t{offset(vertex, item, true)} << 32u), 0x18u);
      if (item.texture == Texture::Uv)
        packet.ad(static_cast<unsigned>(vertex.u * 16.0f) |
                      (std::uint64_t{static_cast<unsigned>(vertex.v * 16.0f)} << 16u), 0x03u);
      else if (item.texture == Texture::Stq)
        packet.ad(bits(vertex.u * vertex.q / 8.0f) |
                      (std::uint64_t{bits(vertex.v * vertex.q / 8.0f)} << 32u), 0x02u);
      packet.ad(vertex.color | (std::uint64_t{bits(vertex.q)} << 32u), 0x01u);
      packet.ad(xyz(vertex, item), 0x05u);
    }
  return packet;
}

template<class T, class = void>
struct HasFixedXy : std::false_type {};

template<class T>
struct HasFixedXy<T, std::void_t<decltype(std::declval<T>().fixed_xy),
    decltype(std::declval<T>().fixed_x), decltype(std::declval<T>().fixed_y)>>
    : std::true_type {};

template<class T>
bool fixed_trace(const T& actual, const Vertex& expected) {
  if constexpr (HasFixedXy<T>::value)
    return actual.fixed_xy && actual.fixed_x == expected.x && actual.fixed_y == expected.y;
  return true; // Matching pre-feature headers deliberately omit appended fields.
}

template<class T>
std::array<bool, 5> direct_guard_controls(const std::array<T, 3>& vertices) {
  std::array<bool, 5> results{{true, true, true, true, true}};
  if constexpr (HasFixedXy<T>::value) {
    for (unsigned control = 0u; control < results.size(); ++control) {
      auto owned = vertices;
      for (auto& vertex : owned) {
        vertex.fixed_xy = true;
        vertex.fixed_x = vertex.x * kStep;
        vertex.fixed_y = vertex.y * kStep;
      }
      if (control < 4u) {
        // One unit beyond each documented raw coordinate guard, not an
        // enormous arithmetic value which could overflow a test helper.
        if (control < 2u) owned[0].fixed_x = control == 0u ? 65536 : -65536;
        else owned[0].fixed_y = control == 2u ? 65536 : -65536;
      } else {
        // Mixed API fallback multiplies legacy host units in int64 before
        // rejecting the raw range. This must not overflow signed int first.
        owned[0].fixed_xy = false;
        owned[0].x = std::numeric_limits<int>::max();
      }
      ps2vita::Gs gs;
      gs.triangle(owned[0], owned[1], owned[2]);
      for (int y = 0; y < 9; ++y)
        for (int x = 0; x < 9; ++x) results[control] &= gs.pixel(x, y) == 0u;
    }
  }
  return results;
}

template<class T>
bool unused_fixed_neutral(const std::array<T, 3>& vertices, const ps2vita::Gs& plain) {
  if constexpr (HasFixedXy<T>::value) {
    auto owned = vertices;
    for (auto& vertex : owned) {
      vertex.fixed_xy = false;
      vertex.fixed_x = std::numeric_limits<int>::min();
      vertex.fixed_y = std::numeric_limits<int>::max();
    }
    ps2vita::Gs gs;
    gs.triangle(owned[0], owned[1], owned[2]);
    for (int y = 0; y < 9; ++y)
      for (int x = 0; x < 9; ++x)
        if (gs.pixel(x, y) != plain.pixel(x, y)) return false;
  }
  return true;
}

struct Result {
  bool accepted = false;
  std::uint64_t rejected = 0u, triangles = 0u, image_bytes = 0u, vram_hash = 0u;
  std::vector<std::uint32_t> pixels;
  std::vector<ps2vita::GsPixelWrite> events;
  std::vector<ps2vita::GifTriangleRecord> records;
  std::size_t nondegenerate = 0u;
};

Result run(const Case& item, bool watched = true, bool hash_vram = false) {
  ps2vita::Gs gs;
  ps2vita::Gif gif(gs);
  Result result;
  gif.enable_triangle_trace(true);
  gif.set_triangle_observer([&](const ps2vita::GifTriangleRecord& record) {
    result.records.push_back(record);
  });
  if (watched) {
    std::vector<std::uint32_t> addresses;
    for (const auto& point : kWatch)
      addresses.push_back(static_cast<std::uint32_t>((point[1] * 64 + point[0]) * 4));
    check(gs.set_pixel_watch(addresses, [&](const ps2vita::GsPixelWrite& event) {
      result.events.push_back(event);
    }), item.name, "owned physical watchpoints attach");
  }
  const auto packet = make_packet(item);
  result.accepted = gif.submit(packet.bytes.data(), packet.bytes.size());
  result.rejected = gif.packets_rejected(); result.triangles = gif.triangles_emitted();
  result.image_bytes = gif.image_bytes();
  result.nondegenerate = gif.nondegenerate_triangle_records().size();
  for (int y = 0; y < kGrid; ++y)
    for (int x = 0; x < kGrid; ++x) result.pixels.push_back(gs.pixel(x, y));
  if (hash_vram) {
    result.vram_hash = 1469598103934665603ull;
    for (std::uint32_t address = 0u; address < 0x400000u; address += 4u) {
      result.vram_hash ^= gif.read_local32(address);
      result.vram_hash *= 1099511628211ull;
    }
  }
  return result;
}

void run_case(const Case& item) {
  const auto result = run(item);
  check(result.accepted && result.rejected == 0u && result.triangles == item.triangles.size() &&
            result.image_bytes == (item.texture == Texture::None ? 0u : 256u),
        item.name, "owned GIF packet and triangle count are accepted");
  bool pixels_match = true, coverage_match = true;
  for (int y = 0; y < kGrid; ++y)
    for (int x = 0; x < kGrid; ++x) {
      Sample final;
      for (const auto& triangle : item.triangles) {
        const auto sample = oracle(item, triangle, x * kScale, y * kScale);
        if (sample.covered) final = sample;
      }
      const auto actual = result.pixels[static_cast<std::size_t>(y * kGrid + x)];
      pixels_match &= actual == final.color;
      // Zero RGBA is still an accepted color write. Its coverage is not
      // visible in a zero-cleared buffer; exact watch events check those
      // operations separately below.
      coverage_match &= (actual != 0u) == (final.covered && final.color != 0u);
    }
  check(coverage_match, item.name, "entire bounded sample grid matches oracle color-visible coverage");
  check(pixels_match, item.name, "covered pixels retain fixed-position Gouraud/FST/STQ interpolation");

  std::size_t expected_nondegenerate = 0u;
  for (const auto& triangle : item.triangles)
    expected_nondegenerate += edge(triangle[0], triangle[1], triangle[2].x, triangle[2].y) != 0;
  check(result.nondegenerate == expected_nondegenerate, item.name,
        "nondegenerate trace selection uses raw geometry even if projected area collapses");
  check(result.records.size() == item.triangles.size(), item.name,
        "triangle observer retains each original vertex triple");
  bool legacy_match = result.records.size() == item.triangles.size();
  bool fixed_match = legacy_match;
  for (std::size_t t = 0u; t < std::min(result.records.size(), item.triangles.size()); ++t)
    for (unsigned i = 0u; i < 3u; ++i) {
      const auto& actual = result.records[t].vertices[i];
      const auto& expected = item.triangles[t][i];
      legacy_match &= actual.x == expected.x / kStep && actual.y == expected.y / kStep &&
          actual.z == expected.z && actual.color == expected.color &&
          result.records[t].xyz[i] == xyz(expected, item);
      fixed_match &= fixed_trace(actual, expected);
    }
  check(legacy_match, item.name, "legacy trace projection/color/depth/rawXYZ remains compatible");
  check(fixed_match, item.name, "when supported, signed fractional positions are latched per vertex");

  std::vector<ps2vita::GsPixelWrite> remaining = result.events;
  bool events_match = true;
  for (std::size_t t = 0u; t < item.triangles.size(); ++t)
    for (const auto& point : kWatch) {
      const auto expected = oracle(item, item.triangles[t], point[0], point[1]);
      if (!expected.covered) continue;
      const auto address = static_cast<std::uint32_t>((point[1] * 64 + point[0]) * 4);
      const auto found = std::find_if(remaining.begin(), remaining.end(), [&](const auto& event) {
        return event.address == address && event.draw.sequence == t;
      });
      if (found == remaining.end()) { events_match = false; continue; }
      events_match &= found->z == expected.z && found->input_color == expected.color &&
          found->after == expected.color && found->x == point[0] / kScale &&
          found->y == point[1] / kScale;
      if (item.texture != Texture::None)
        events_match &= found->texture.valid && found->texture.raw_u == expected.u &&
            found->texture.raw_v == expected.v &&
            found->texture.vertex_color == expected.vertex_color;
      remaining.erase(found);
    }
  check(events_match && remaining.empty(), item.name,
        "watchpoints have exactly the owned writes with exact affine depth/texture provenance");
}

Case reversed(Case item, const char* name) {
  item.name = name;
  for (auto& triangle : item.triangles) std::swap(triangle[1], triangle[2]);
  return item;
}

void geometry_cases() {
  Case item{"fractional positive origin", {{{{4, 4}, {4, 228}, {228, 4}}}}};
  run_case(item); run_case(reversed(item, "fractional positive origin reversed"));
  item = {"asymmetric fractional edges", {{{{7, 13}, {23, 245}, {237, 29}}}}};
  run_case(item);
  item = {"negative positions after fractional XYOFFSET", {{{{-8, -5}, {-8, 180}, {164, -5}}}}};
  run_case(item); run_case(reversed(item, "negative positions reversed"));
  item = {"fractional SCISSOR minima and inclusive maxima", {{{{-4, -4}, {-4, 252}, {252, -4}}}}};
  item.left = item.top = 1u; item.right = item.bottom = 9u; run_case(item);
  item = {"invalid SCISSOR window", {{{{4, 4}, {4, 228}, {228, 4}}}}};
  item.left = 9u; item.right = 1u; run_case(item);
  item = {"subquarter triangle around native two", {{{{28, 28}, {28, 40}, {40, 28}}}}};
  run_case(item);
  const auto tiny = run(item, false);
  check(std::count_if(tiny.pixels.begin(), tiny.pixels.end(), [](auto pixel) { return pixel != 0u; }) ==
            (kScale == 1 ? 1 : 0), item.name,
        "tiny raw triangle covers one native anchor and no quarter-grid anchors");
  item = {"subquarter triangle around native four", {{{{60, 60}, {60, 76}, {76, 60}}}}};
  run_case(item);
  item = {"raw collinear triangle", {{{{3, 7}, {67, 71}, {131, 135}}}}};
  run_case(item);

  const Vertex a{4, 12}, b{4, 252}, c{244, 12}, d{244, 252};
  item = {"fractional shared diagonal", {{a, b, c}, {c, b, d}}};
  run_case(item); run_case(reversed(item, "fractional shared diagonal reversed"));
}

void interpolation_cases() {
  Case item{"fractional Gouraud and depth", {{{
      {4, 12, 0x10203040u, 100u}, {4, 252, 0x50607080u, 500u},
      {244, 12, 0x90A0B0C0u, 900u}}}}, true};
  run_case(item); run_case(reversed(item, "fractional Gouraud and depth reversed"));
  item.name = "fractional FST interpolation";
  item.texture = Texture::Uv;
  item.triangles[0][0].u = .5f; item.triangles[0][0].v = .5f;
  item.triangles[0][1].u = 6.5f; item.triangles[0][1].v = 4.5f;
  item.triangles[0][2].u = 4.5f; item.triangles[0][2].v = 6.5f;
  run_case(item); run_case(reversed(item, "fractional FST reversed"));
  item.name = "fractional STQ independently affine Q";
  item.texture = Texture::Stq;
  item.triangles[0][1].q = 2.0f; item.triangles[0][2].q = .5f;
  run_case(item); run_case(reversed(item, "fractional STQ reversed"));
}

void large_depth_cases() {
  const auto maximum = std::numeric_limits<std::uint32_t>::max();
  const Vertex a{-65535, -65535, kSolid, maximum, .5f, .5f, 1.0f, 65535u, 65535u};
  const Vertex b{-65535, 65535, kSolid, maximum, .5f, .5f, 1.0f, 65535u, 0u};
  const Vertex c{65535, 0, kSolid, maximum, .5f, .5f, 1.0f, 0u, 0u};
  Case item{"large area constant maximum depth with changed XYOFFSET", {{a, b, c}}};
  check(edge(a, b, c.x, c.y) == 17179344900ll, item.name,
        "owned raw area exceeds UINT_MAX while every XYZ/XYOFFSET field fits");
  check(oracle(item, item.triangles[0], 4, 4).z == maximum, item.name,
        "equal maximum corner depth remains exactly maximum at a covered anchor");
  run_case(item);
  item.name = "large area varying near-maximum depth";
  item.triangles[0][1].z = maximum - 65535u;
  item.triangles[0][2].z = maximum - 32768u;
  // At native(4,4), weights are4282253505/4299030465/8598060930.
  // Exact rational evaluation gives this hand-value independently of uint64
  // products of the original near-maximum depths.
  check(oracle(item, item.triangles[0], 4, 4).z == 4294934495u, item.name,
        "large varying depth agrees with independent hand rational value");
  run_case(item); run_case(reversed(item, "large varying depth reversed"));
}

struct Attributes {
  const char* name;
  std::array<std::uint32_t, 3> color, z;
};

std::array<Attributes, 11> attribute_patterns() {
  constexpr auto maximum = std::numeric_limits<std::uint32_t>::max();
  return {{{"constant zero Z varying RGBA", {{0x00402010u, 0x80706080u, 0xFFE0C0FFu}},
            {{0u, 0u, 0u}}},
           {"constant maximum Z varying RGBA", {{0x00402010u, 0x80706080u, 0xFFE0C0FFu}},
            {{maximum, maximum, maximum}}},
           {"varying full-range Z constant RGBA", {{kSolid, kSolid, kSolid}},
            {{0u, maximum, maximum - 1u}}},
           {"both attributes constant zero", {{0u, 0u, 0u}}, {{0u, 0u, 0u}}},
           {"both attributes constant maximum", {{maximum, maximum, maximum}},
            {{maximum, maximum, maximum}}},
           {"both attributes constant ordinary", {{kSolid, kSolid, kSolid}}, {{123u, 123u, 123u}}},
           {"only red channel varies", {{0x80654300u, 0x806543FFu, 0x80654301u}},
            {{77u, 77u, 77u}}},
           {"only green channel varies", {{0x80650021u, 0x8065FF21u, 0x80650121u}},
            {{0u, maximum, 123u}}},
           {"only blue channel varies", {{0x80004321u, 0x80FF4321u, 0x80014321u}},
            {{maximum, maximum, maximum}}},
           {"only alpha channel varies", {{0x00654321u, 0xFF654321u, 0x01654321u}},
            {{0u, maximum, 1u}}},
           {"varying full-range Z and RGBA", {{0u, maximum, 0x0180FE7Fu}},
            {{maximum, 0u, maximum / 2u}}}}};
}

Triangle attribute_triangle(const Attributes& attributes) {
  Triangle triangle{{{4, 4}, {4, 508}, {508, 4}}};
  for (unsigned i = 0u; i < triangle.size(); ++i) {
    triangle[i].color = attributes.color[i]; triangle[i].z = attributes.z[i];
    triangle[i].u = .5f + 2.0f * i; triangle[i].v = 4.5f - 2.0f * i;
    triangle[i].q = i == 1u ? 2.0f : 1.0f;
  }
  return triangle;
}

void attribute_guard_cases() {
  const auto patterns = attribute_patterns();
  for (const auto& attributes : patterns) {
    Case item{attributes.name, {attribute_triangle(attributes)}, true};
    run_case(item);
    run_case(reversed(item, attributes.name));
  }
  Case successive{"successive constant/varying attribute guard transitions", {}, true};
  for (const auto& attributes : patterns)
    successive.triangles.push_back(attribute_triangle(attributes));
  for (const auto texture : {Texture::None, Texture::Uv, Texture::Stq}) {
    successive.texture = texture;
    run_case(successive); run_case(reversed(successive, successive.name));
  }
  successive.texture = Texture::None;
  const auto off = run(successive, false, true), on = run(successive, true, true);
  check(off.pixels == on.pixels && off.vram_hash == on.vram_hash && off.events.empty(),
        successive.name, "observation preserves the guard-transition raster and complete VRAM hash");
  check(!on.events.empty(), successive.name,
        "guard transitions produce accepted watch operations even for zero color");
}

template<class T>
void direct_fixed_position(T& vertex, int x, int y) {
  if constexpr (HasFixedXy<T>::value) {
    vertex.fixed_xy = true; vertex.fixed_x = x; vertex.fixed_y = y;
  }
}

std::uint64_t word_hash(const std::vector<std::uint32_t>& words) {
  std::uint64_t value = 1469598103934665603ull;
  for (const auto word : words) { value ^= word; value *= 1099511628211ull; }
  return value;
}

void standalone_attribute_guards() {
  const auto patterns = attribute_patterns();
  for (const bool fixed : {false, true})
    for (const bool reverse : {false, true}) {
      std::vector<std::uint64_t> unsampled_hashes;
      // Each Gs instance sees all transitions without clear/reset between
      // draws. Identity callbacks isolate incoming interpolated vertex RGBA
      // from texture lookup and preserve it, including the alpha channel.
      for (unsigned mode = 0u; mode < 3u; ++mode) {
        ps2vita::Gs gs;
        gs.set_depth_state(ps2vita::Gs::DepthTest::Always, false);
        gs.set_scissor(0, 0, 8, 8);
        std::vector<std::uint32_t> expected_pixels(81u, 0u);
        for (std::size_t draw = 0u; draw < patterns.size(); ++draw) {
          const auto& attributes = patterns[draw];
          std::array<ps2vita::GsVertex, 3> vertices{{{0, 0}, {0, 8}, {8, 0}}};
          Triangle expected_triangle;
          for (unsigned i = 0u; i < vertices.size(); ++i) {
            auto& vertex = vertices[i];
            vertex.color = attributes.color[i]; vertex.z = attributes.z[i];
            vertex.uv = 32u | (48ull << 16u); // Constant U2/V3.
            vertex.q = bits(1.0f);
            const int x = vertex.x * kStep + (fixed ? 4 : 0);
            const int y = vertex.y * kStep + (fixed ? 4 : 0);
            if (fixed) direct_fixed_position(vertex, x, y);
            expected_triangle[i] = {x, y, vertex.color, vertex.z};
          }
          if (reverse) { std::swap(vertices[1], vertices[2]);
                         std::swap(expected_triangle[1], expected_triangle[2]); }
          Case item{attributes.name, {expected_triangle}, true};
          item.right = item.bottom = 8u * kScale;
          std::vector<std::uint32_t> expected_samples, sampled_colors;
          for (int y = 0; y < 9; ++y)
            for (int x = 0; x < 9; ++x) {
              const auto sample = oracle(item, expected_triangle, x * kScale, y * kScale);
              if (sample.covered) {
                expected_pixels[static_cast<std::size_t>(y * 9 + x)] = sample.color;
                expected_samples.push_back(sample.vertex_color);
              }
            }
          bool coordinates_match = true;
          const auto sampler = [&](std::int64_t u, std::int64_t v, std::uint32_t color) {
            coordinates_match &= u == (mode == 2u ? 2ll * 65536 : 2ll) &&
                                 v == (mode == 2u ? 3ll * 65536 : 3ll);
            sampled_colors.push_back(color);
            return color;
          };
          if (mode == 0u) gs.triangle(vertices[0], vertices[1], vertices[2]);
          else if (mode == 1u) gs.triangle(vertices[0], vertices[1], vertices[2], sampler);
          else gs.triangle(vertices[0], vertices[1], vertices[2], {}, 0u, 0u, sampler);
          std::vector<std::uint32_t> pixels;
          for (int y = 0; y < 9; ++y)
            for (int x = 0; x < 9; ++x) pixels.push_back(gs.pixel(x, y));
          const auto hash = word_hash(pixels);
          check(pixels == expected_pixels && hash == word_hash(expected_pixels), item.name,
                "legacy/fixed successive draws match independent per-channel affine colors and hash");
          check(coordinates_match && (mode == 0u ? sampled_colors.empty() :
                                      sampled_colors == expected_samples), item.name,
                "nearest/fixed identity callbacks receive exact ordered vertex RGBA including alpha");
          if (mode == 0u) unsampled_hashes.push_back(hash);
          else check(hash == unsampled_hashes[draw], item.name,
                     "identity sampler preserves the unsampled per-draw raster hash");
        }
      }
    }
}

void observer_and_legacy_controls() {
  const Case item{"fractional observer neutrality", {{{{7, 13}, {23, 245}, {237, 29}}}}};
  const auto off = run(item, false, true), on = run(item, true, true);
  check(off.pixels == on.pixels && off.vram_hash == on.vram_hash && off.events.empty(),
        item.name, "pixel observation preserves the sample grid and all logical VRAM words");
  check(off.accepted == on.accepted && off.triangles == on.triangles &&
            off.rejected == on.rejected && off.image_bytes == on.image_bytes,
        item.name, "pixel observation preserves packet/primitive/transfer counters");

  ps2vita::Gs gs;
  const std::array<ps2vita::GsVertex, 3> vertices{{
      {0, 0, 100u, 0x10203040u}, {0, 8, 500u, 0x50607080u},
      {8, 0, 900u, 0x90A0B0C0u}}};
  gs.triangle(vertices[0], vertices[1], vertices[2]);
  Case legacy{"standalone integer Gs triangle remains in host units", {{{
      {0, 0, vertices[0].color, vertices[0].z},
      {0, 8 * kStep, vertices[1].color, vertices[1].z},
      {8 * kStep, 0, vertices[2].color, vertices[2].z}}}}, true};
  legacy.right = legacy.bottom = 32u * kScale;
  bool matches = true;
  for (int y = 0; y < 9; ++y)
    for (int x = 0; x < 9; ++x)
      matches &= gs.pixel(x, y) == oracle(legacy, legacy.triangles[0], x * kScale, y * kScale).color;
  check(matches, legacy.name, "default-false fixed fields preserve legacy coverage and interpolation");
  check(unused_fixed_neutral(vertices, gs), legacy.name,
        "when supported, unused extreme fixed fields do not affect an unflagged triangle");
  const auto guards = direct_guard_controls(vertices);
  for (unsigned i = 0u; i < guards.size(); ++i)
    check(guards[i], "direct fixed triangle safety",
          "when supported, out-of-range fixed/mixed positions reject without drawing");
}

} // namespace

int main() {
  geometry_cases(); interpolation_cases(); large_depth_cases(); observer_and_legacy_controls();
  attribute_guard_cases(); standalone_attribute_guards();
  std::printf("GIF subpixel triangle tests: %u checks, %u failures; fixed_xy=%u scale=%d\n",
              checks, failures, unsigned(HasFixedXy<ps2vita::GsVertex>::value), kScale);
  return failures == 0u ? 0 : 1;
}
