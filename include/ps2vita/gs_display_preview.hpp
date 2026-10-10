#pragma once

#include "ps2vita/gs_display.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace ps2vita {

enum class GsLinearDisplayStatus {
  Ready, Disabled, MultipleCircuits, ZeroWidth, UnsupportedFormat, UnsupportedMode, EmptyExtent
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

// Logical-linear diagnostic, NOT GS scanout. update() samples a fixed 640x448
// region; update_source() blanks anchors outside DISPLAY's requested source
// extent, including NTSC/PAL INT+FFMD half-height storage. Neither applies DX/DY,
// actual video-mode detection, field selection/weaving, swizzling or PMODE mixing.
// Guest alpha is not a host transparency channel: preview RGB is opaque.
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
    source_window_ = {};
    source_width_ = kWidth * kNativeStep;
    source_height_ = kHeight * kNativeStep;
    return sample(read_word);
  }

  template<class ReadWord>
  GsLinearDisplayStatus update_source(std::uint64_t pmode, std::uint64_t dispfb1,
      std::uint64_t dispfb2, std::uint64_t display1, std::uint64_t display2,
      std::uint64_t smode1, std::uint64_t smode2, ReadWord&& read_word) {
    selection_ = GsLinearDisplaySelection::decode(pmode, dispfb1, dispfb2);
    source_window_ = GsDisplayWindow::decode(selection_.circuit == 1u ? display1 : display2);
    source_width_ = source_window_.source_width();
    source_height_ = source_window_.source_height(smode1, smode2);
    if (selection_.status == GsLinearDisplayStatus::Ready &&
        !GsDisplayWindow::supports_source_mode(smode1, smode2))
      selection_.status = GsLinearDisplayStatus::UnsupportedMode;
    if (selection_.status == GsLinearDisplayStatus::Ready &&
        (source_width_ == 0u || source_height_ == 0u))
      selection_.status = GsLinearDisplayStatus::EmptyExtent;
    return sample(read_word);
  }

  const GsLinearDisplaySelection& selection() const { return selection_; }
  const GsDisplayWindow& source_window() const { return source_window_; }
  unsigned source_width() const { return source_width_; }
  unsigned source_height() const { return source_height_; }
  const std::array<std::uint32_t, kPixels>& pixels() const { return pixels_; }

private:
  template<class ReadWord>
  GsLinearDisplayStatus sample(ReadWord& read_word) {
    if (selection_.status != GsLinearDisplayStatus::Ready) {
      pixels_.fill(0xFF000000u); // Never retain a stale previously selected buffer.
      return selection_.status;
    }
    for (unsigned y = 0u; y < kHeight; ++y)
      for (unsigned x = 0u; x < kWidth; ++x) {
        if (x * kNativeStep >= source_width_ || y * kNativeStep >= source_height_) {
          pixels_[y * kWidth + x] = 0xFF000000u;
          continue;
        }
        const auto address = selection_.framebuffer.linear_pixel_byte_address(
            x * kNativeStep, y * kNativeStep);
        const auto word = read_word(static_cast<std::uint32_t>(address & 0x3FFFFFu));
        pixels_[y * kWidth + x] = 0xFF000000u | (word & 0x00FFFFFFu);
      }
    return selection_.status;
  }

  GsLinearDisplaySelection selection_{};
  GsDisplayWindow source_window_{};
  unsigned source_width_ = 0u, source_height_ = 0u;
  std::array<std::uint32_t, kPixels> pixels_{};
};

} // namespace ps2vita
