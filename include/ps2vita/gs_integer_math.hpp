#pragma once

namespace ps2vita {
namespace gs_detail {

// Internal byte-channel math only, not arbitrary signed-coordinate division.
// Callers supply a,b in [0,255] and fraction in [0,15]. The biased product
// is [271,7921], so unsigned power-of-two division exactly reproduces floor
// without relying on a negative signed shift. Result is still in [0,255].
constexpr int lerp_byte_4bit(int a, int b, unsigned fraction) {
  const int product = (b - a) * static_cast<int>(fraction);
  return a + static_cast<int>(static_cast<unsigned>(product + 4096) / 16u) - 256;
}

// The GS blend product is (byte A - byte B) * byte alpha/FIX, in
// [-65025,65025]. Adding the divisor-aligned bias keeps division nonnegative.
// Cast the quotient to int BEFORE subtracting, preserving negative results.
constexpr int floor_blend_product_7bit(int product) {
  return static_cast<int>(static_cast<unsigned>(product + 65536) / 128u) - 512;
}

} // namespace gs_detail
} // namespace ps2vita
