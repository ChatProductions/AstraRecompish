#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace ps2vita {

// Inspect the EE executable's expected libkernl SDK compatibility tag.
// This is NOT the version returned by the IOP FILEIO module: the IOP
// response and its compatibility rules must be verified independently.
// Return zero for missing, malformed, or non-decimal tags.
inline std::uint32_t elf_libkernl_expected_version(
    const void* elf, std::size_t length) {
  constexpr char kTag[] = "PsIIlibkernl";
  constexpr std::size_t kTagBytes = sizeof(kTag) - 1u;
  const auto* bytes = static_cast<const std::uint8_t*>(elf);
  if (!bytes || length < kTagBytes + 4u ||
      bytes[0] != 0x7Fu || bytes[1] != 'E' ||
      bytes[2] != 'L' || bytes[3] != 'F') return 0u;
  for (std::size_t i = 0; i <= length - kTagBytes - 4u; ++i) {
    if (std::memcmp(bytes + i, kTag, kTagBytes) != 0) continue;
    std::uint32_t value = 0u;
    for (std::size_t n = 0; n < 4u; ++n) {
      const auto ch = bytes[i + kTagBytes + n];
      if (ch < '0' || ch > '9') return 0u;
      value |= static_cast<std::uint32_t>(ch) << (8u * n);
    }
    return value;
  }
  return 0u;
}

} // namespace ps2vita
