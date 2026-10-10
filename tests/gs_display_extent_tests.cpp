#include "ps2vita/gs_display_preview.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>

// Owned logical-linear source-region oracles, NOT golden hardware scanout.
// DISPLAY extents are requested geometry only: these tests deliberately do not
// invent video-mode detection, fields, display placement, mixing or swizzling.
namespace {
unsigned checks = 0u, failures = 0u;
using Window = ps2vita::GsDisplayWindow;
using Preview = ps2vita::GsLinearDisplayPreview;
using Status = ps2vita::GsLinearDisplayStatus;

void check(bool ok, const char* name) {
  ++checks;
  if (!ok) { ++failures; std::fprintf(stderr, "FAIL: %s\n", name); }
}

constexpr std::uint64_t display(unsigned dw, unsigned dh, unsigned magh = 0u,
    unsigned magv = 0u, unsigned dx = 0u, unsigned dy = 0u) {
  return dx | (std::uint64_t{dy} << 12u) | (std::uint64_t{magh} << 23u) |
      (std::uint64_t{magv} << 27u) | (std::uint64_t{dw} << 32u) |
      (std::uint64_t{dh} << 44u);
}

constexpr std::uint64_t dispfb(unsigned base, unsigned stride, unsigned format = 0u,
    unsigned x = 0u, unsigned y = 0u) {
  return base | (std::uint64_t{stride} << 9u) | (std::uint64_t{format} << 15u) |
      (std::uint64_t{x} << 32u) | (std::uint64_t{y} << 43u);
}

bool equal_window(const Window& a, const Window& b) {
  return a.dx == b.dx && a.dy == b.dy && a.magh == b.magh &&
      a.magv == b.magv && a.dw == b.dw && a.dh == b.dh;
}

Window field_oracle(std::uint64_t value) {
  // Division/remainder extraction is independent of the production shifts/masks.
  return {static_cast<unsigned>(value % 4096u),
      static_cast<unsigned>((value / 4096u) % 2048u),
      static_cast<unsigned>((value / 8388608u) % 16u),
      static_cast<unsigned>((value / 134217728u) % 4u),
      static_cast<unsigned>((value / 4294967296ull) % 4096u),
      static_cast<unsigned>((value / 17592186044416ull) % 2048u)};
}

bool mode_supported(unsigned cmod, unsigned mode) {
  return mode % 2u == 0u || cmod >= 2u;
}

unsigned height_oracle(unsigned dh, unsigned magv, unsigned cmod, unsigned mode) {
  const unsigned height = (dh + 1u) / (magv + 1u);
  if (mode_supported(cmod, mode) && mode % 4u == 3u)
    return height / 2u + height % 2u;
  return height;
}

void decode_and_geometry() {
  constexpr auto common = Window::decode(display(2559u, 511u, 3u));
  static_assert(common.source_width() == 640u &&
      common.source_height(2ull << 13u, 3u) == 256u);
  static_assert(Window::supports_source_mode(0u, 0u) &&
      !Window::supports_source_mode(0u, 1u) &&
      Window::supports_source_mode(3ull << 13u, 1u));
  check(equal_window(Window::decode(0u), {}), "zero DISPLAY decodes every field as zero");
  for (unsigned bit = 0u; bit < 64u; ++bit) {
    const auto one = std::uint64_t{1u} << bit;
    check(equal_window(Window::decode(one), field_oracle(one)),
        "every individual DISPLAY bit has exactly its independent field effect");
    check(equal_window(Window::decode(~one), field_oracle(~one)),
        "every cleared DISPLAY bit has exactly its independent field effect");
  }
  constexpr auto reserved = (7ull << 29u) | (0x1FFull << 55u);
  constexpr auto packed = display(3271u, 1719u, 13u, 2u, 2401u, 1903u);
  check(equal_window(Window::decode(packed), Window::decode(packed | reserved)),
      "all reserved DISPLAY bits leave every decoded field unchanged");

  for (unsigned magh = 0u; magh < 16u; ++magh)
    for (unsigned dw = 0u; dw < 4096u; ++dw) {
      const auto window = Window::decode(display(dw, 0u, magh));
      check(window.source_width() == (dw + 1u) / (magh + 1u),
          "all 65536 DW/MAGH combinations use bounded integer quotient geometry");
    }
  for (unsigned magv = 0u; magv < 4u; ++magv)
    for (unsigned dh = 0u; dh < 2048u; ++dh)
      for (unsigned cmod = 0u; cmod < 4u; ++cmod)
        for (unsigned mode = 0u; mode < 4u; ++mode) {
          const auto window = Window::decode(display(0u, dh, 0u, magv));
          check(window.source_height(std::uint64_t{cmod} << 13u, mode) ==
                  height_oracle(dh, magv, cmod, mode) &&
              Window::supports_source_mode(std::uint64_t{cmod} << 13u, mode) ==
                  mode_supported(cmod, mode),
              "all DH/MAGV/CMOD/INT/FFMD combinations preserve mode gating and odd-height rounding");
        }
}

std::uint32_t word_pattern(std::uint32_t address, std::uint32_t salt = 0u) {
  return ((address * 0x5BD1E995u) ^ (address >> 9u) ^ (address >> 17u) ^ salt) |
      0x00102040u;
}

struct RegionCase {
  unsigned base, stride, format, origin_x, origin_y;
  unsigned dw, dh, magh, magv, dx, dy, cmod, mode;
};

void check_region(Preview& preview, const RegionCase& item, unsigned circuit,
    std::uint64_t display_reserved = 0u, std::uint64_t smode1_reserved = 0u,
    std::uint64_t smode2_reserved = 0u, std::uint32_t salt = 0u) {
  const auto selected_fb = dispfb(item.base, item.stride, item.format,
      item.origin_x, item.origin_y);
  const auto selected_display = display(item.dw, item.dh, item.magh, item.magv,
      item.dx, item.dy) | display_reserved;
  // The inactive circuit deliberately has unrelated and invalid extent/format.
  const auto inactive_fb = dispfb(17u, 0u, 31u, 9u, 11u);
  const auto inactive_display = display(0u, 0u, 15u, 3u, 4079u, 2029u);
  std::array<std::uint32_t, 17920u> addresses{};
  std::size_t reads = 0u;
  bool physical = true;
  const auto status = preview.update_source(circuit | 0xFFFFFFFFFFFFFFFCull,
      circuit == 1u ? selected_fb : inactive_fb,
      circuit == 2u ? selected_fb : inactive_fb,
      circuit == 1u ? selected_display : inactive_display,
      circuit == 2u ? selected_display : inactive_display,
      (std::uint64_t{item.cmod} << 13u) | smode1_reserved,
      item.mode | smode2_reserved,
      [&](std::uint32_t address) {
        if (reads < addresses.size()) addresses[reads] = address;
        ++reads;
        physical = physical && address < 4194304u && address % 4u == 0u;
        return word_pattern(address, salt);
      });
  const unsigned width = (item.dw + 1u) / (item.magh + 1u);
  const unsigned height = height_oracle(item.dh, item.magv, item.cmod, item.mode);
  const auto expected_status = !mode_supported(item.cmod, item.mode) ? Status::UnsupportedMode :
      width == 0u || height == 0u ? Status::EmptyExtent : Status::Ready;
  bool matching = true, address_matching = true;
  std::size_t expected_reads = 0u;
  for (unsigned y = 0u; y < 112u; ++y)
    for (unsigned x = 0u; x < 160u; ++x) {
      std::uint32_t expected = 0xFF000000u;
      if (expected_status == Status::Ready && x * 4u < width && y * 4u < height) {
        const std::uint64_t logical = std::uint64_t{item.base} * 8192u +
            ((std::uint64_t{item.origin_y} + y * 4u) * item.stride * 64u +
                item.origin_x + x * 4u) * 4u;
        const auto physical_address = static_cast<std::uint32_t>(logical % 4194304u);
        address_matching = address_matching && expected_reads < reads &&
            addresses[expected_reads] == physical_address;
        ++expected_reads;
        expected |= word_pattern(physical_address, salt) & 0x00FFFFFFu;
      }
      matching = matching && preview.pixels()[y * 160u + x] == expected;
    }
  check(status == expected_status && preview.selection().status == expected_status &&
          preview.selection().circuit == circuit && matching,
      "every source-region pixel agrees with independent geometry, opaque RGB and black clipping");
  check(reads == expected_reads && physical && address_matching,
      "only in-extent anchors read fresh aligned wrapped addresses in exact raster order");
  check(preview.source_width() == width && preview.source_height() == height &&
          equal_window(preview.source_window(), {item.dx, item.dy, item.magh, item.magv,
              item.dw, item.dh}),
      "selected source metadata is independent of the inactive circuit and reserved fields");
}

void full_region_oracles() {
  constexpr std::array<RegionCase, 16> cases{{
      {0u, 10u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u},
      {3u, 10u, 1u, 7u, 11u, 3u, 3u, 0u, 0u, 4095u, 2047u, 0u, 2u},
      {3u, 10u, 0u, 7u, 11u, 4u, 4u, 0u, 0u, 31u, 47u, 1u, 0u},
      {0x50u, 10u, 0u, 0u, 0u, 2559u, 511u, 3u, 0u, 632u, 50u, 2u, 3u},
      {0xA0u, 10u, 1u, 639u, 255u, 1279u, 895u, 1u, 1u, 11u, 19u, 3u, 1u},
      {7u, 10u, 0u, 3u, 5u, 638u, 446u, 0u, 0u, 13u, 17u, 2u, 0u},
      {511u, 63u, 1u, 2047u, 2047u, 635u, 443u, 0u, 0u, 4095u, 2047u, 3u, 2u},
      {511u, 1u, 0u, 2047u, 2047u, 4095u, 2047u, 0u, 0u, 0u, 0u, 3u, 3u},
      {511u, 1u, 1u, 2047u, 2047u, 4095u, 2047u, 15u, 3u, 4095u, 2047u, 2u, 3u},
      {7u, 63u, 1u, 2047u, 2047u, 0u, 2047u, 15u, 0u, 101u, 203u, 2u, 0u},
      {7u, 63u, 1u, 2047u, 2047u, 4095u, 0u, 0u, 3u, 101u, 203u, 2u, 3u},
      {7u, 63u, 0u, 2047u, 2047u, 0u, 0u, 15u, 3u, 101u, 203u, 0u, 3u},
      {0u, 1u, 0u, 0u, 0u, 12u, 8u, 0u, 0u, 0u, 0u, 2u, 3u},
      {0u, 1u, 0u, 0u, 0u, 12u, 16u, 0u, 1u, 0u, 0u, 3u, 3u},
      {0u, 1u, 0u, 0u, 0u, 4095u, 2047u, 0u, 0u, 0u, 0u, 0u, 1u},
      {0u, 1u, 0u, 0u, 0u, 4095u, 2047u, 0u, 0u, 0u, 0u, 1u, 3u}}};
  Preview preview;
  for (const auto& item : cases)
    for (unsigned circuit = 1u; circuit <= 2u; ++circuit) {
      check_region(preview, item, circuit);
      check_region(preview, item, circuit, (7ull << 29u) | (0x1FFull << 55u),
          ~(3ull << 13u), ~3ull, 0xDEADBEEFu);
    }

  for (unsigned cmod = 0u; cmod < 4u; ++cmod)
    for (unsigned mode = 0u; mode < 4u; ++mode) {
      const RegionCase item{3u, 10u, 0u, 17u, 23u,
          2559u, 511u, 3u, 0u, 31u, 47u, cmod, mode};
      check_region(preview, item, 1u, 0u, ~(3ull << 13u), ~3ull);
    }
}

bool all_black(const Preview& preview) {
  for (const auto pixel : preview.pixels()) if (pixel != 0xFF000000u) return false;
  return true;
}

void precedence_and_live_updates() {
  Preview preview;
  const auto valid_fb = dispfb(17u, 10u);
  const auto full_display = display(639u, 447u);
  const auto empty_display = display(0u, 0u, 15u, 3u);
  const auto fill_stale = [&]() {
    preview.update_source(1u, valid_fb, 0u, full_display, 0u, 0u, 0u,
        [](std::uint32_t) { return 0x00112233u; });
  };
  struct Invalid { std::uint64_t pmode, fb, mode1, mode2; Status expected; };
  constexpr std::array<Invalid, 7> invalid{{
      {0u, dispfb(0u, 0u, 31u), 0u, 3u, Status::Disabled},
      {3u, dispfb(0u, 0u, 31u), 0u, 3u, Status::MultipleCircuits},
      {1u, dispfb(0u, 0u, 31u), 0u, 3u, Status::ZeroWidth},
      {2u, dispfb(0u, 10u, 31u), 0u, 3u, Status::UnsupportedFormat},
      {1u, dispfb(0u, 10u), 0u, 3u, Status::UnsupportedMode},
      {2u, dispfb(0u, 10u), 1ull << 13u, 1u, Status::UnsupportedMode},
      {1u, dispfb(0u, 10u), 2ull << 13u, 3u, Status::EmptyExtent}}};
  for (const auto& item : invalid) {
    fill_stale();
    std::size_t reads = 0u;
    const auto status = preview.update_source(item.pmode, item.fb, item.fb,
        empty_display, empty_display, item.mode1, item.mode2,
        [&](std::uint32_t) { ++reads; return 0xFFFFFFFFu; });
    check(status == item.expected && all_black(preview) && reads == 0u,
        "selection errors precede unsupported mode before empty geometry and clear all stale pixels without reads");
  }

  fill_stale();
  std::size_t reads = 0u;
  preview.update_source(1u, valid_fb, 0u, display(4u, 4u), empty_display, 0u, 0u,
      [&](std::uint32_t) { ++reads; return 0xEE445566u; });
  bool fresh = true;
  for (unsigned y = 0u; y < 112u; ++y)
    for (unsigned x = 0u; x < 160u; ++x)
      fresh = fresh && preview.pixels()[y * 160u + x] ==
          (x < 2u && y < 2u ? 0xFF445566u : 0xFF000000u);
  check(reads == 4u && fresh,
      "a shrink immediately refreshes live RGB and blacks every formerly visible out-of-extent anchor");

  auto stable_pixels = preview.pixels();
  preview.update_source(1u, valid_fb, dispfb(511u, 63u, 31u), display(4u, 4u),
      full_display, 0u, 0u, [](std::uint32_t) { return 0xEE445566u; });
  check(preview.pixels() == stable_pixels && preview.selection().circuit == 1u,
      "changing only inactive DISPLAY/DISPFB leaves the selected source pixels unchanged");
  preview.update_source(2u, valid_fb, dispfb(0u, 1u), display(4u, 4u), display(0u, 0u),
      0u, 0u, [](std::uint32_t) { return 0x0099AABBu; });
  bool switched = preview.pixels()[0] == 0xFF99AABBu;
  for (std::size_t i = 1u; i < preview.pixels().size(); ++i)
    switched = switched && preview.pixels()[i] == 0xFF000000u;
  check(switched && preview.selection().circuit == 2u &&
          preview.source_width() == 1u && preview.source_height() == 1u,
      "PMODE-only switch uses the other circuit's independent DISPLAY extent");

  reads = 0u;
  preview.update(2u, 0u, valid_fb,
      [&](std::uint32_t) { ++reads; return 0x00556677u; });
  bool old_region = true;
  for (const auto pixel : preview.pixels()) old_region = old_region && pixel == 0xFF556677u;
  check(reads == 17920u && old_region && preview.source_width() == 640u &&
          preview.source_height() == 448u && equal_window(preview.source_window(), {}),
      "fixed-region update remains an independent diagnostic and resets prior extent metadata");
}
} // namespace

int main() {
  decode_and_geometry();
  full_region_oracles();
  precedence_and_live_updates();
  std::printf("GS display extent tests: %u checks, %u failures\n", checks, failures);
  return failures == 0u ? 0 : 1;
}
