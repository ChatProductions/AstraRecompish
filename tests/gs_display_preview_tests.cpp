#include "ps2vita/gif.hpp"
#include "ps2vita/gs_display_preview.hpp"
#include "ps2vita/memory.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <vector>

// Owned logical-linear display-buffer diagnostic oracles, NOT golden hardware
// scanout. The helper deliberately samples a fixed region, not DISPLAY extents.
namespace {
unsigned checks = 0u, failures = 0u;
using Status = ps2vita::GsLinearDisplayStatus;
using Preview = ps2vita::GsLinearDisplayPreview;

void check(bool ok, const char* name) {
  ++checks;
  if (!ok) { ++failures; std::fprintf(stderr, "FAIL: %s\n", name); }
}

constexpr std::uint64_t dispfb(unsigned base, unsigned width, unsigned format = 0u,
    unsigned x = 0u, unsigned y = 0u) {
  return base | (std::uint64_t{width} << 9u) | (std::uint64_t{format} << 15u) |
      (std::uint64_t{x} << 32u) | (std::uint64_t{y} << 43u);
}

void selection_cases() {
  constexpr auto first = ps2vita::GsLinearDisplaySelection::decode(1u, 0x1450u, 0u);
  constexpr auto second = ps2vita::GsLinearDisplaySelection::decode(0x66u, 0u, 0x1400u);
  static_assert(first.status == Status::Ready && first.circuit == 1u &&
      first.framebuffer.base_bytes() == 0xA0000u && first.framebuffer.width_pixels() == 640u);
  static_assert(second.status == Status::Ready && second.circuit == 2u &&
      second.framebuffer.base_bytes() == 0u && second.framebuffer.width_pixels() == 640u);
  for (unsigned mode = 0u; mode < 4u; ++mode) {
    for (auto extra : {0ull, 0xFFFCull, 0xFFFFFFFFFFFFFFFCull}) {
      const auto selected = ps2vita::GsLinearDisplaySelection::decode(mode | extra,
          dispfb(0x50u, 10u, 0u, 3u, 7u), dispfb(0xA0u, 20u, 1u, 9u, 11u));
      check(mode == 0u ? selected.status == Status::Disabled && selected.circuit == 0u :
          mode == 3u ? selected.status == Status::MultipleCircuits && selected.circuit == 0u :
          selected.status == Status::Ready && selected.circuit == mode &&
              selected.framebuffer.fbp == (mode == 1u ? 0x50u : 0xA0u) &&
              selected.framebuffer.dbx == (mode == 1u ? 3u : 9u) &&
              selected.framebuffer.dby == (mode == 1u ? 7u : 11u),
          "only PMODE EN1/EN2 select this diagnostic's circuit and origin");
    }
  }
  for (unsigned circuit = 1u; circuit <= 2u; ++circuit)
    for (unsigned format = 0u; format < 32u; ++format) {
      const auto fb = dispfb(511u, 63u, format, 2047u, 2047u);
      const auto selected = ps2vita::GsLinearDisplaySelection::decode(circuit, fb, fb);
      check(selected.status == (format <= 1u ? Status::Ready : Status::UnsupportedFormat) &&
          selected.framebuffer.psm == format, "only PSMCT32/24 are supported diagnostic formats");
      const auto zero = ps2vita::GsLinearDisplaySelection::decode(circuit,
          dispfb(1u, 0u, format), dispfb(2u, 0u, format));
      check(zero.status == Status::ZeroWidth && zero.circuit == circuit,
          "zero width is rejected before any linear sampling");
    }
  const auto reserved = ps2vita::GsLinearDisplaySelection::decode(1u,
      dispfb(17u, 5u, 1u, 23u, 42u) | (0xFFFull << 20u) | (0x3FFull << 54u), 0u);
  check(reserved.status == Status::Ready && reserved.framebuffer.fbp == 17u &&
      reserved.framebuffer.fbw == 5u && reserved.framebuffer.psm == 1u &&
      reserved.framebuffer.dbx == 23u && reserved.framebuffer.dby == 42u,
      "reserved DISPFB bits do not change decoded fields");
}

std::uint8_t byte_pattern(std::uint32_t address) {
  return static_cast<std::uint8_t>((address * 29u) ^ (address >> 9u) ^ (address >> 17u) ^ 0x67u);
}

std::uint32_t byte_word(std::uint64_t address) {
  std::uint32_t word = 0u;
  for (unsigned byte = 0u; byte < 4u; ++byte)
    word |= std::uint32_t{byte_pattern(static_cast<std::uint32_t>(
        (address + byte) % 0x400000u))} << (byte * 8u);
  return word;
}

void whole_region_oracles() {
  struct Case { unsigned base, width, format, x, y; };
  constexpr std::array<Case, 8> cases{{
      {0x50u, 10u, 0u, 0u, 0u}, {0u, 10u, 0u, 0u, 0u},
      {0xA0u, 10u, 1u, 3u, 5u}, {511u, 1u, 0u, 0u, 0u},
      {511u, 63u, 1u, 2047u, 2047u}, {7u, 1u, 1u, 2047u, 2047u},
      {0u, 63u, 0u, 2047u, 2047u}, {0x50u, 10u, 1u, 639u, 255u}}};
  Preview preview;
  for (const auto& item : cases) {
    std::size_t reads = 0u;
    bool physical_aligned = true;
    const auto status = preview.update(2u, 0u,
        dispfb(item.base, item.width, item.format, item.x, item.y),
        [&](std::uint32_t address) {
          ++reads;
          physical_aligned = physical_aligned && address < 0x400000u && (address & 3u) == 0u;
          return byte_word(address);
        });
    bool matching = true;
    for (unsigned y = 0u; y < 112u; ++y)
      for (unsigned x = 0u; x < 160u; ++x) {
        // Independent explicit field arithmetic, not the production address helper.
        const auto address = std::uint64_t{item.base} * 8192u +
            ((std::uint64_t{item.y} + y * 4u) * item.width * 64u + item.x + x * 4u) * 4u;
        const auto expected = 0xFF000000u | (byte_word(address) & 0x00FFFFFFu);
        matching = matching && preview.pixels()[y * 160u + x] == expected;
      }
    check(status == Status::Ready && matching,
        "all 17920 samples match the owned stride/origin/modulo byte oracle");
    check(reads == 17920u && physical_aligned,
        "every sample makes one fresh, physically wrapped aligned word read");
  }

  const auto stable_fb = dispfb(3u, 10u);
  preview.update(1u, stable_fb, 0u, [](std::uint32_t) { return 0x00112233u; });
  check(preview.pixels()[0] == 0xFF112233u && preview.pixels().back() == 0xFF112233u,
      "zero guest alpha is shown as opaque diagnostic RGB");
  preview.update(1u, stable_fb, 0u, [](std::uint32_t) { return 0xEE445566u; });
  bool fresh = true;
  for (auto pixel : preview.pixels()) fresh = fresh && pixel == 0xFF445566u;
  check(fresh, "a later update reads live replacement RGB across the whole region");

  struct Invalid { std::uint64_t mode, fb; Status status; };
  constexpr std::array<Invalid, 4> invalid{{
      {0u, stable_fb, Status::Disabled}, {3u, stable_fb, Status::MultipleCircuits},
      {1u, dispfb(0u, 0u), Status::ZeroWidth},
      {2u, dispfb(0u, 10u, 2u), Status::UnsupportedFormat}}};
  for (const auto& item : invalid) {
    preview.update(1u, stable_fb, 0u, [](std::uint32_t) { return 0x80123456u; });
    std::size_t reads = 0u;
    const auto status = preview.update(item.mode, item.fb, item.fb,
        [&](std::uint32_t) { ++reads; return 0xFFFFFFFFu; });
    bool black = true;
    for (auto pixel : preview.pixels()) black = black && pixel == 0xFF000000u;
    check(status == item.status && black && reads == 0u,
        "unsupported/disabled selection blanks every stale pixel without reading VRAM");
  }
}

struct Packet {
  std::vector<std::uint8_t> bytes;
  void word(std::uint64_t value) {
    for (unsigned byte = 0u; byte < 8u; ++byte) bytes.push_back(static_cast<std::uint8_t>(value >> (byte * 8u)));
  }
  void ad(std::uint64_t value, unsigned reg) {
    word(0x1000000000008001ull); word(0xEull); word(value); word(reg);
  }
  void image(unsigned base, std::uint32_t color) {
    ad((std::uint64_t{base / 256u} << 32u) | (10ull << 48u), 0x50u);
    ad(0u, 0x51u); ad(8u | (8ull << 32u), 0x52u); ad(0u, 0x53u);
    word(0x0800000000008010ull); word(0u);
    for (unsigned pair = 0u; pair < 32u; ++pair) word(color | (std::uint64_t{color} << 32u));
  }
};

std::uint64_t memory_hash(const ps2vita::Gif& gif) {
  std::uint64_t hash = 1469598103934665603ull;
  for (unsigned address = 0u; address < 0x400000u; address += 4u) {
    hash ^= gif.read_local32(address); hash *= 1099511628211ull;
  }
  return hash;
}

void live_buffer_switches() {
  ps2vita::Memory memory;
  ps2vita::Gs gs;
  ps2vita::Gif gif(gs);
  Packet packet;
  packet.image(0xA0000u, 0x80112233u);
  packet.image(0u, 0x80445566u);
  packet.ad(0xA0u | (10ull << 16u), 0x4Cu);
  packet.ad((639ull << 16u) | (447ull << 48u), 0x40u);
  packet.ad(0u, 0x47u); packet.ad(6u, 0u); packet.ad(0x80778899u, 1u);
  packet.ad(0u, 5u); packet.ad(128u | (128ull << 16u), 5u);
  check(gif.submit(packet.bytes.data(), packet.bytes.size()) && gif.sprites_emitted() == 1u &&
      gif.image_tags() == 2u && gif.local_bytes_written() == 512u &&
      gif.pending_bytes() == 0u && gif.packets_rejected() == 0u,
      "owned IMAGE uploads and a third draw target establish three different buffers");
  check(gs.color_frame() == (0xA0u | (10ull << 16u)) &&
      (gs.pixel(0, 0) & 0xFFFFFFu) == 0x778899u,
      "active draw-target preview differs from both display buffers");
  const auto before = memory_hash(gif);
  const auto packed = gif.packed_tags();
  memory.write64(0x12000070u, 0x1450u);
  memory.write64(0x12000090u, 0x1400u);
  Preview preview;
  const auto update = [&]() {
    return preview.update(memory.read64(0x12000000u), memory.read64(0x12000070u),
        memory.read64(0x12000090u), [&](std::uint32_t at) { return gif.read_local32(at); });
  };
  memory.write64(0x12000000u, 1u);
  check(update() == Status::Ready && preview.selection().circuit == 1u &&
      preview.pixels()[0] == 0xFF112233u && preview.pixels()[161] == 0xFF112233u &&
      preview.pixels()[2] == 0xFF000000u,
      "actual PMODE and DISPFB1 select IMAGE RGB rather than the current FRAME");
  memory.write64(0x12000000u, 0x66u);
  check(update() == Status::Ready && preview.selection().circuit == 2u &&
      preview.pixels()[0] == 0xFF445566u,
      "PMODE-only circuit switch selects DISPFB2 without a new draw");
  memory.write64(0x12000090u, 0x1450u);
  check(update() == Status::Ready && preview.pixels()[0] == 0xFF112233u,
      "DISPFB-only base switch takes effect without a new draw");
  Packet replacement; replacement.image(0xA0000u, 0x00AABBCCu);
  check(gif.submit(replacement.bytes.data(), replacement.bytes.size()) &&
      update() == Status::Ready && preview.pixels()[0] == 0xFFAABBCCu,
      "later real IMAGE writes are immediately visible, not a cached texture/frame");
  check(gs.color_frame() == (0xA0u | (10ull << 16u)) &&
      (gs.pixel(0, 0) & 0xFFFFFFu) == 0x778899u && gif.sprites_emitted() == 1u &&
      gif.packed_tags() == packed + 4u,
      "display diagnostics do not rebind or draw into the GS target");
  // Verify read-only copying independently after the intentionally mutating IMAGE.
  const auto after_image = memory_hash(gif);
  update(); update();
  check(memory_hash(gif) == after_image && after_image != before &&
      gif.pending_bytes() == 0u && gif.packets_rejected() == 0u,
      "repeated display copies leave all 4 MiB of VRAM and packet state unchanged");
  memory.clear();
  check(update() == Status::Disabled && preview.pixels()[0] == 0xFF000000u &&
      preview.pixels().back() == 0xFF000000u,
      "reset privileged registers cannot leave an old selected-buffer image on screen");
}
} // namespace

int main() {
  selection_cases(); whole_region_oracles(); live_buffer_switches();
  std::printf("GS display preview tests: %u checks, %u failures\n", checks, failures);
  return failures == 0u ? 0 : 1;
}
