#include "ps2vita/gs.hpp"
#include "ps2vita/gs_integer_math.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

namespace ps2vita {
namespace {

constexpr bool kNativeLittleEndian =
#if (defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__) && \
     __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__) || \
    (defined(_MSC_VER) && (defined(_M_IX86) || defined(_M_X64) || \
                          defined(_M_ARM) || defined(_M_ARM64)))
    true;
#else
    false;
#endif

void store_aligned_color32(std::vector<std::uint8_t>& memory,
                           std::uint32_t address, std::uint32_t value) {
  // FRAME base and every native-pixel offset are multiples of four. After
  // wrapping, an aligned word starts at <=0x3FFFFC and cannot straddle VRAM.
  // memcpy is alias-safe; keep each store live and in the original tile order.
  if constexpr (kNativeLittleEndian) {
    std::memcpy(memory.data() + (address & 0x3FFFFFu), &value, sizeof(value));
  } else {
    for (unsigned b = 0; b < 4; ++b)
      memory[(address + b) & 0x3FFFFFu] = value >> (b * 8u);
  }
}

std::int64_t edge(const GsVertex& a, const GsVertex& b, int x, int y) {
  return static_cast<std::int64_t>(x - a.x) * (b.y - a.y) -
         static_cast<std::int64_t>(y - a.y) * (b.x - a.x);
}

std::uint32_t mix_channel(std::uint32_t ca, std::uint32_t cb, std::uint32_t cc,
                          std::int64_t wa, std::int64_t wb, std::int64_t wc,
                          std::int64_t area, unsigned shift) {
  const auto a = static_cast<std::int64_t>((ca >> shift) & 0xFFu);
  const auto b = static_cast<std::int64_t>((cb >> shift) & 0xFFu);
  const auto c = static_cast<std::int64_t>((cc >> shift) & 0xFFu);
  const auto value = (a * wa + b * wb + c * wc) / area;
  return static_cast<std::uint32_t>(std::clamp<std::int64_t>(value, 0, 255)) << shift;
}

std::uint32_t mix_depth(std::uint32_t a, std::uint32_t b, std::uint32_t c,
                       std::uint64_t wa, std::uint64_t wb, std::uint64_t wc,
                       std::uint64_t area) {
  if (area <= std::numeric_limits<std::uint32_t>::max())
    return static_cast<std::uint32_t>((a * wa + b * wb + c * wc) / area);
  // Different latched XYOFFSETs can make a signed12.4 area exceed 32 bits.
  // Split Z rather than overflow a 64-bit product or require host __int128.
  // The weights sum to area; guest fixed bounds make these sums <2^50 and
  // the reconstructed residual <2^51, including full 32-bit guest Z.
  const auto high = (a >> 16u) * wa + (b >> 16u) * wb + (c >> 16u) * wc;
  const auto low = (a & 0xFFFFu) * wa + (b & 0xFFFFu) * wb + (c & 0xFFFFu) * wc;
  return static_cast<std::uint32_t>((high / area) * 65536u +
      ((high % area) * 65536u + low) / area);
}

} // namespace

Gs::Gs() : color_(kWidth * kHeight), depth_(kWidth * kHeight) { clear(0); }

void Gs::set_color_target(std::vector<std::uint8_t>* memory, std::uint64_t frame) {
  color_frame_ = frame;
  const unsigned format = (frame >> 24) & 0x3Fu;
  color_width_ = ((frame >> 16) & 0x3Fu) * 64u;
  color_base_ = (frame & 0x1FFu) * 8192u;
  color_mask_ = static_cast<std::uint32_t>(frame >> 32) |
                (format == 1u ? 0xFF000000u : 0u);
  color_memory_ = memory && memory->size() == 4u * 1024u * 1024u &&
                  color_width_ != 0u && format <= 1u ? memory : nullptr;
}

bool Gs::set_pixel_watch(const std::vector<std::uint32_t>& addresses,
                          PixelObserver observer) {
  if (addresses.size() > 16u || (!addresses.empty() && !observer)) return false;
  for (std::size_t i = 0; i < addresses.size(); ++i) {
    if ((addresses[i] & 3u) != 0u || addresses[i] >= 0x400000u) return false;
    if (std::find(addresses.begin(), addresses.begin() + i, addresses[i]) !=
        addresses.begin() + i) return false;
  }
  pixel_watch_addresses_ = addresses;
  pixel_observer_ = addresses.empty() ? PixelObserver{} : std::move(observer);
  draw_trace_ = {};
  texture_trace_ = {};
  return true;
}

std::uint32_t Gs::target_read(std::uint32_t address) const {
  // Only FRAME pixel addresses and validated aligned watches call this.
  if constexpr (kNativeLittleEndian) {
    std::uint32_t value;
    std::memcpy(&value, color_memory_->data() + (address & 0x3FFFFFu), sizeof(value));
    return value;
  }
  std::uint32_t value = 0;
  for (unsigned b = 0; b < 4; ++b)
    value |= std::uint32_t{(*color_memory_)[(address + b) & 0x3FFFFFu]} << (8u * b);
  return value;
}

void Gs::target_write(int x, int y, std::uint32_t color, std::uint32_t mask) {
  if (static_cast<unsigned>(x * kRasterScale) >= color_width_) return;
  // A bulk unmasked tile operation needs no old destination reads. Measured
  // beneficial for 4x4 tiles, not the separate one-pixel diagnostic: leave
  // that build's mask selection unchanged. Preserve VRAM wrapping and the
  // per-word masked route for FBMSK/PSMCT24/RGB_ONLY preservation.
  if (kRasterScale > 1 && mask == 0u) {
    for (unsigned dy = 0; dy < static_cast<unsigned>(kRasterScale); ++dy)
      for (unsigned dx = 0; dx < static_cast<unsigned>(kRasterScale); ++dx) {
        const auto address = color_base_ +
            ((y * static_cast<unsigned>(kRasterScale) + dy) * color_width_ +
             x * static_cast<unsigned>(kRasterScale) + dx) * 4u;
        store_aligned_color32(*color_memory_, address, color);
      }
    return;
  }
  for (unsigned dy = 0; dy < static_cast<unsigned>(kRasterScale); ++dy)
    for (unsigned dx = 0; dx < static_cast<unsigned>(kRasterScale); ++dx) {
      const auto address = color_base_ +
          ((y * static_cast<unsigned>(kRasterScale) + dy) * color_width_ +
           x * static_cast<unsigned>(kRasterScale) + dx) * 4u;
      const auto value = (color & ~mask) | (target_read(address) & mask);
      store_aligned_color32(*color_memory_, address, value);
    }
}

void Gs::clear(std::uint32_t color, std::uint32_t depth) {
  std::fill(color_.begin(), color_.end(), color);
  std::fill(depth_.begin(), depth_.end(), depth);
  if (color_memory_)
    for (int y = 0; y < kHeight; ++y)
      for (int x = 0; x < kWidth; ++x) target_write(x, y, color, 0u);
}

void Gs::write(int x, int y, std::uint32_t z, std::uint32_t color) {
  if (x < 0 || y < 0 || x >= kWidth || y >= kHeight) return;
  if (x < scissor_left_ || x > scissor_right_ ||
      y < scissor_top_ || y > scissor_bottom_) return;
  const auto index = static_cast<std::size_t>(y * kWidth + x);
  bool write_color = true, write_depth = depth_write_, preserve_alpha = false;
  if ((alpha_test_ & 1u) != 0u) {
    const unsigned alpha = color >> 24;
    const unsigned reference = (alpha_test_ >> 4) & 0xFFu;
    const bool results[] = {false, true, alpha < reference, alpha <= reference,
                            alpha == reference, alpha >= reference,
                            alpha > reference, alpha != reference};
    if (!results[(alpha_test_ >> 1) & 7u]) {
      switch ((alpha_test_ >> 12) & 3u) {
      case 0: return; // KEEP: neither buffer is updated.
      case 1: write_depth = false; break; // FB_ONLY
      case 2: write_color = false; break; // ZB_ONLY
      case 3: write_depth = false; preserve_alpha = true; break; // RGB_ONLY
      }
    }
  }
  const bool pass = depth_test_ == DepthTest::Always ||
      (depth_test_ == DepthTest::GreaterEqual && z >= depth_[index]) ||
      (depth_test_ == DepthTest::Greater && z > depth_[index]) ||
      (depth_test_ == DepthTest::LessEqual && z <= depth_[index]);
  if (pass) {
    if (write_depth) depth_[index] = z;
    if (!write_color) return;
    const auto input_color = color;
    std::uint16_t trace_hits = 0;
    std::array<std::uint32_t, 16> trace_before;
    if (pixel_observer_ && color_memory_ &&
        static_cast<unsigned>(x * kRasterScale) < color_width_) {
      const auto first = color_base_ +
          (y * static_cast<unsigned>(kRasterScale) * color_width_ +
           x * static_cast<unsigned>(kRasterScale)) * 4u;
      for (std::size_t watch = 0; watch < pixel_watch_addresses_.size(); ++watch) {
        const auto address = pixel_watch_addresses_[watch];
        for (unsigned dy = 0; dy < static_cast<unsigned>(kRasterScale); ++dy) {
          const auto delta = (address - (first + dy * color_width_ * 4u)) & 0x3FFFFFu;
          if (delta < static_cast<unsigned>(kRasterScale) * 4u) {
            trace_hits |= static_cast<std::uint16_t>(1u << watch);
            trace_before[watch] = target_read(address);
            break;
          }
        }
      }
    }
    if (blend_enabled_ && (!blend_pabe_ || (color & 0x80000000u) != 0u)) {
      const auto destination = pixel(x, y);
      const auto source = color;
      const unsigned a = blend_equation_ & 3u;
      const unsigned b = (blend_equation_ >> 2) & 3u;
      const unsigned c = (blend_equation_ >> 4) & 3u;
      const unsigned d = (blend_equation_ >> 6) & 3u;
      // Selectors 3 are reserved; only the defined equation subset is modeled.
      if (a < 3u && b < 3u && c < 3u && d < 3u) {
        const int alpha = c == 0u ? source >> 24 : c == 1u ? destination >> 24 :
            (blend_equation_ >> 32) & 0xFFu;
        color &= 0xFF000000u; // Blending modifies RGB, not source alpha.
        for (unsigned shift = 0; shift < 24u; shift += 8u) {
          const int components[] = {int((source >> shift) & 0xFFu),
                                    int((destination >> shift) & 0xFFu), 0};
          const int product = (components[a] - components[b]) * alpha;
          const int scaled = gs_detail::floor_blend_product_7bit(product);
          const int value = scaled + components[d];
          const auto channel = color_clamp_ ? unsigned(std::clamp(value, 0, 255)) :
              static_cast<unsigned>(value) & 0xFFu;
          color |= channel << shift;
        }
      }
    }
    if (preserve_alpha) color = (color & 0x00FFFFFFu) | (pixel(x, y) & 0xFF000000u);
    if (color_memory_) {
      target_write(x, y, color, color_mask_ | (preserve_alpha ? 0xFF000000u : 0u));
      color = pixel(x, y);
    }
    color_[index] = color;
    for (std::size_t watch = 0; trace_hits != 0u &&
         watch < pixel_watch_addresses_.size(); ++watch) {
      if ((trace_hits & (1u << watch)) == 0u) continue;
      const auto address = pixel_watch_addresses_[watch];
      const GsPixelWrite event{address, trace_before[watch], target_read(address),
          input_color, z, x, y, color_frame_, draw_trace_, texture_trace_};
      pixel_observer_(event);
    }
  }
}

void Gs::point(const GsVertex& vertex) {
  write(vertex.x, vertex.y, vertex.z, vertex.color);
}

void Gs::line(GsVertex a, GsVertex b) {
  const int dx = std::abs(b.x - a.x);
  const int dy = std::abs(b.y - a.y);
  const int steps = std::max(dx, dy);
  if (steps == 0) { point(a); return; }
  for (int i = 0; i <= steps; ++i) {
    const auto lerp = [=](std::uint32_t av, std::uint32_t bv) {
      return static_cast<std::uint32_t>((static_cast<std::uint64_t>(av) * (steps - i) +
                                         static_cast<std::uint64_t>(bv) * i) / steps);
    };
    std::uint32_t color = 0;
    for (unsigned shift = 0; shift < 32; shift += 8)
      color |= lerp((a.color >> shift) & 0xFFu, (b.color >> shift) & 0xFFu) << shift;
    write(a.x + (b.x - a.x) * i / steps, a.y + (b.y - a.y) * i / steps,
          lerp(a.z, b.z), color);
  }
}

void Gs::triangle(GsVertex a, GsVertex b, GsVertex c,
                  const TextureSampler& sample, unsigned st_width, unsigned st_height,
                  const FixedTextureSampler& fixed_sample) {
  const bool fixed = a.fixed_xy || b.fixed_xy || c.fixed_xy;
  const int step = fixed ? 16 * kRasterScale : 1;
  auto pa = a, pb = b, pc = c;
  if (fixed) {
    const auto position = [&](GsVertex& point) {
      const auto x = point.fixed_xy ? std::int64_t{point.fixed_x} : std::int64_t{point.x} * step;
      const auto y = point.fixed_xy ? std::int64_t{point.fixed_y} : std::int64_t{point.y} * step;
      if (x < -65535 || x > 65535 || y < -65535 || y > 65535) return false;
      point.x = static_cast<int>(x); point.y = static_cast<int>(y);
      return true;
    };
    if (!position(pa) || !position(pb) || !position(pc)) return;
  }
  std::int64_t area = edge(pa, pb, pc.x, pc.y);
  // A collapsed triangle has no coverage; it is not a line primitive.
  if (area == 0) return;
  if (area < 0) { std::swap(b, c); std::swap(pb, pc); area = -area; }
  // Barycentric weights sum exactly to area. Identical corner attributes
  // need no interpolation/division, even for the wide-area depth path.
  // Guard each attribute independently; Gouraud gradients keep the old math.
  const bool constant_z = a.z == b.z && a.z == c.z;
  const bool constant_color = a.color == b.color && a.color == c.color;
  const auto fp = [](std::uint32_t bits) {
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return static_cast<double>(value);
  };
  const double sa = fp(static_cast<std::uint32_t>(a.st)),
               sb = fp(static_cast<std::uint32_t>(b.st)),
               sc = fp(static_cast<std::uint32_t>(c.st));
  const double ta = fp(static_cast<std::uint32_t>(a.st >> 32)),
               tb = fp(static_cast<std::uint32_t>(b.st >> 32)),
               tc = fp(static_cast<std::uint32_t>(c.st >> 32));
  const double qa = fp(a.q), qb = fp(b.q), qc = fp(c.q);
  // With positive edge() area and screen Y increasing downwards, include
  // left/downward and top/leftward edges; exclude their opposite partners.
  const auto inclusive = [](const GsVertex& from, const GsVertex& to) {
    return to.y > from.y || (to.y == from.y && to.x < from.x);
  };
  const bool include_a = inclusive(pb, pc), include_b = inclusive(pc, pa),
             include_c = inclusive(pa, pb);
  const auto ceiling = [step](int coordinate) {
    return coordinate >= 0 ? (std::int64_t{coordinate} + step - 1) / step :
        std::int64_t{coordinate} / step;
  };
  const auto floor = [step](int coordinate) {
    return coordinate >= 0 ? std::int64_t{coordinate} / step :
        -((-std::int64_t{coordinate} + step - 1) / step);
  };
  const int min_x = static_cast<int>(std::max<std::int64_t>(std::max(0, scissor_left_),
      ceiling(std::min({pa.x, pb.x, pc.x}))));
  const int max_x = static_cast<int>(std::min<std::int64_t>(std::min(kWidth - 1, scissor_right_),
      floor(std::max({pa.x, pb.x, pc.x}))));
  const int min_y = static_cast<int>(std::max<std::int64_t>(std::max(0, scissor_top_),
      ceiling(std::min({pa.y, pb.y, pc.y}))));
  const int max_y = static_cast<int>(std::min<std::int64_t>(std::min(kHeight - 1, scissor_bottom_),
      floor(std::max({pa.y, pb.y, pc.y}))));
  for (int y = min_y; y <= max_y; ++y) {
    for (int x = min_x; x <= max_x; ++x) {
      // Non-AA GS samples lie on integer native anchors, not half centers.
      // Keep all 12.4 vertex fractions until coverage AND interpolation.
      const auto wa = edge(pb, pc, x * step, y * step);
      const auto wb = edge(pc, pa, x * step, y * step);
      const auto wc = edge(pa, pb, x * step, y * step);
      if (wa < 0 || wb < 0 || wc < 0) continue;
      if ((wa == 0 && !include_a) || (wb == 0 && !include_b) ||
          (wc == 0 && !include_c)) continue;
      const auto z = constant_z ? a.z : mix_depth(a.z, b.z, c.z, wa, wb, wc, area);
      std::uint32_t color = a.color;
      if (!constant_color) {
        color = 0;
        for (unsigned shift = 0; shift < 32; shift += 8)
          color |= mix_channel(a.color, b.color, c.color, wa, wb, wc, area, shift);
      }
      if (sample || fixed_sample) {
        // FST UV is unsigned 10.4 fixed point. Interpolate before removing
        // the fractional bits; vertex winding swaps must also swap attributes.
        const auto coordinate = [&](unsigned shift) {
          const auto sum = static_cast<std::int64_t>((a.uv >> shift) & 0x3FFFu) * wa +
                           static_cast<std::int64_t>((b.uv >> shift) & 0x3FFFu) * wb +
                           static_cast<std::int64_t>((c.uv >> shift) & 0x3FFFu) * wc;
          return fixed_sample ? static_cast<std::int64_t>(sum * 4096 / area) :
              static_cast<std::int64_t>(sum / (area * 16));
        };
        if (st_width != 0u && st_height != 0u) {
          // S, T and Q are independently affine in screen space. Divide AFTER
          // interpolation; area cancels between numerator and denominator.
          const double q = qa * wa + qb * wb + qc * wc;
          if (q == 0.0 || !std::isfinite(q)) continue;
          const double u = (sa * wa + sb * wb + sc * wc) * st_width / q;
          const double v = (ta * wa + tb * wb + tc * wc) * st_height / q;
          // Preserve signed STQ until texture addressing. Bound the current
          // logical sampler before any floating-to-integer conversion; this
          // does not model native GS overflow outside its signed 16.16 range.
          const double max = std::numeric_limits<unsigned>::max();
          if (!std::isfinite(u) || !std::isfinite(v) ||
              u < -max || v < -max || u > max || v > max) continue;
          // GS nearest sampling truncates to signed 16.16, then shifts away
          // its fraction arithmetically. Use division for portable negative
          // floor semantics, including tiny negatives which truncate to zero.
          const auto texel = [](double coordinate) {
            const auto fixed = static_cast<std::int64_t>(coordinate * 65536.0);
            return fixed >= 0 ? fixed / 65536 : -((-fixed + 65535) / 65536);
          };
          color = fixed_sample ? fixed_sample(
              static_cast<std::int64_t>(u * 65536.0),
              static_cast<std::int64_t>(v * 65536.0), color) :
              sample(texel(u), texel(v), color);
        } else {
          color = fixed_sample ? fixed_sample(coordinate(0), coordinate(16), color) :
              sample(coordinate(0), coordinate(16), color);
        }
      }
      write(x, y, z, color);
    }
  }
}

std::uint32_t Gs::pixel(int x, int y) const {
  if (x < 0 || y < 0 || x >= kWidth || y >= kHeight) return 0;
  if (color_memory_) {
    if (static_cast<unsigned>(x * kRasterScale) >= color_width_) return 0;
    return target_read(color_base_ +
        (y * static_cast<unsigned>(kRasterScale) * color_width_ +
         x * static_cast<unsigned>(kRasterScale)) * 4u);
  }
  return color_[static_cast<std::size_t>(y * kWidth + x)];
}

const std::uint32_t* Gs::pixels() const {
  if (color_memory_)
    for (int y = 0; y < kHeight; ++y)
      for (int x = 0; x < kWidth; ++x) color_[y * kWidth + x] = pixel(x, y);
  return color_.data();
}

} // namespace ps2vita
