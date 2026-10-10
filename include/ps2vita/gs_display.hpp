#pragma once

#include <cstdint>

namespace ps2vita {

struct GsDisplayWindow {
  std::uint32_t dx = 0, dy = 0, magh = 0, magv = 0, dw = 0, dh = 0;

  static constexpr GsDisplayWindow decode(std::uint64_t value) {
    return {static_cast<std::uint32_t>(value & 0xFFFu),
            static_cast<std::uint32_t>((value >> 12u) & 0x7FFu),
            static_cast<std::uint32_t>((value >> 23u) & 0xFu),
            static_cast<std::uint32_t>((value >> 27u) & 0x3u),
            static_cast<std::uint32_t>((value >> 32u) & 0xFFFu),
            static_cast<std::uint32_t>((value >> 44u) & 0x7FFu)};
  }

  constexpr std::uint32_t source_width() const { return (dw + 1u) / (magh + 1u); }
  static constexpr bool supports_source_mode(std::uint64_t smode1, std::uint64_t smode2) {
    const auto cmod = (smode1 >> 13u) & 3u;
    return (smode2 & 1u) == 0u || cmod == 2u || cmod == 3u;
  }
  constexpr std::uint32_t source_height(std::uint64_t smode1, std::uint64_t smode2) const {
    const auto height = (dh + 1u) / (magv + 1u);
    // CMOD 2/3 identifies NTSC/PAL analogue output. Do not assume raw INT
    // also identifies interlaced digital output: those modes remain unsupported.
    // Source geometry only, not field selection/weaving or timed GS scanout.
    return supports_source_mode(smode1, smode2) && (smode2 & 3u) == 3u ?
        (height + 1u) / 2u : height;
  }
};

struct GsDisplayFramebuffer {
  std::uint32_t fbp = 0, fbw = 0, psm = 0, dbx = 0, dby = 0;

  static constexpr GsDisplayFramebuffer decode(std::uint64_t value) {
    return {static_cast<std::uint32_t>(value & 0x1FFu),
            static_cast<std::uint32_t>((value >> 9u) & 0x3Fu),
            static_cast<std::uint32_t>((value >> 15u) & 0x1Fu),
            static_cast<std::uint32_t>((value >> 32u) & 0x7FFu),
            static_cast<std::uint32_t>((value >> 43u) & 0x7FFu)};
  }

  // DISPFB FBP and drawing FRAME FBP both select 32 256-byte blocks.
  // PCSX2 v2.8.2 GSRegDISPFB::Block() and GIFRegFRAME::Block() agree.
  constexpr std::uint32_t base_bytes() const { return fbp * 8192u; }
  constexpr std::uint32_t width_pixels() const { return fbw * 64u; }

  // Diagnostic linear-VRAM approximation for PSMCT32/24 only. This is not
  // native GS page/block swizzling or display-circuit sampling.
  constexpr std::uint64_t linear_pixel_byte_address(std::uint32_t x,
                                                    std::uint32_t y) const {
    return base_bytes() +
           (static_cast<std::uint64_t>(dby + y) * width_pixels() + dbx + x) * 4u;
  }
};

} // namespace ps2vita
