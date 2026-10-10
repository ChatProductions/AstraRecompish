#pragma once

#include "ps2vita/gs_display.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace ps2vita {

enum class GsLinearDisplayStatus {
  Ready, Disabled, MultipleCircuits, ZeroWidth, UnsupportedFormat
};

struct GsLinearDisplaySelection {
  GsLinearDisplayStatus status = GsLinearDisplayStatus::Disabled;
  unsigned circuit = 0u;
  GsDisplayFramebuffer framebuffer{};

  static constexpr GsLinearDisplaySelection decode(std::uint64_t pmode,
      std::uint64_t dispfb1, std::uint64_t dispfb2) {
    const auto enabled = pmode & 3u;
    if (enabled == 0u) return {};
    if (enabled == 3u) return {GsLinearDisplayStatus::MultipleCircuits, 0u, {}};
    const unsigned circuit = enabled == 1u ? 1u : 2u;
    const auto fb = GsDisplayFramebuffer::decode(circuit == 1u ? dispfb1 : dispfb2);
    if (fb.fbw == 0u) return {GsLinearDisplayStatus::ZeroWidth, circuit, fb};
    if (fb.psm > 1u) return {GsLinearDisplayStatus::UnsupportedFormat, circuit, fb};
    return {GsLinearDisplayStatus::Ready, circuit, fb};
  }
};

// Fixed-region diagnostic, NOT GS scanout. Samples a 640x448 logical-linear
// region from the selected DISPFB origin, at four-native-pixel intervals.
// DISPLAY extents/magnification, fields, swizzling and PMODE mixing are NOT
// applied. Guest alpha is not a host transparency channel: preview RGB is opaque.
// Rows use DISPFB's stride; addresses wrap at 4 MiB, including narrow-stride
// aliases and regions extending beyond the displayed image's true dimensions.
class GsLinearDisplayPreview {
public:
  static constexpr unsigned kWidth = 160u, kHeight = 112u, kNativeStep = 4u;
  static constexpr std::size_t kPixels = kWidth * kHeight;

  template<class ReadWord>
  GsLinearDisplayStatus update(std::uint64_t pmode, std::uint64_t dispfb1,
      std::uint64_t dispfb2, ReadWord&& read_word) {
    selection_ = GsLinearDisplaySelection::decode(pmode, dispfb1, dispfb2);
    if (selection_.status != GsLinearDisplayStatus::Ready) {
      pixels_.fill(0xFF000000u); // Never retain a stale previously selected buffer.
      return selection_.status;
    }
    for (unsigned y = 0u; y < kHeight; ++y)
      for (unsigned x = 0u; x < kWidth; ++x) {
        const auto address = selection_.framebuffer.linear_pixel_byte_address(
            x * kNativeStep, y * kNativeStep);
        const auto word = read_word(static_cast<std::uint32_t>(address & 0x3FFFFFu));
        pixels_[y * kWidth + x] = 0xFF000000u | (word & 0x00FFFFFFu);
      }
    return selection_.status;
  }

  const GsLinearDisplaySelection& selection() const { return selection_; }
  const std::array<std::uint32_t, kPixels>& pixels() const { return pixels_; }

private:
  GsLinearDisplaySelection selection_{};
  std::array<std::uint32_t, kPixels> pixels_{};
};

} // namespace ps2vita
