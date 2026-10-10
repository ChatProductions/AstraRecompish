#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace ps2vita {

// Exact monitor-only 160x112 -> 960x448 nearest-neighbor expansion. Destination
// is a 960x544 surface, with pitch/capacity measured in uint32 words. Source
// must hold 160*112 words and must not overlap destination. No guest state or
// color/alpha conversion. Invalid surface arguments are a no-write failure.
inline bool blit_monitor_160x112(std::uint32_t* destination,
    std::size_t capacity, std::size_t pitch, const std::uint32_t* source,
    int y_offset = 48) {
  constexpr std::size_t width = 960u, height = 544u;
  if (destination == nullptr || source == nullptr || pitch < width ||
      capacity / pitch < height) return false;

  // Write each visible row contiguously. No rectangle call per source pixel,
  // scratch allocation/copy, or readback from the Vita CDRAM framebuffer.
  for (unsigned sy = 0u; sy < 112u; ++sy) {
    const auto top = std::int64_t{y_offset} + sy * 4u;
    const auto first = std::max<std::int64_t>(0, top);
    const auto last = std::min<std::int64_t>(height, top + 4);
    if (first >= last) continue;
    const auto* row_source = source + sy * 160u;
    for (auto y = first; y < last; ++y) {
      auto* row = destination + static_cast<std::size_t>(y) * pitch;
      for (unsigned sx = 0u; sx < 160u; ++sx)
        std::fill_n(row + sx * 6u, 6u, row_source[sx]);
    }
  }
  return true;
}

} // namespace ps2vita
