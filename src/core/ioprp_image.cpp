#include "ps2vita/ioprp_image.hpp"

#include <cstring>

namespace ps2vita {
namespace {
constexpr std::size_t kRomdirEntrySize = 16u;
constexpr std::size_t kMaxImageSize = 32u * 1024u * 1024u;

std::uint32_t word32(const std::uint8_t* bytes, std::size_t offset) {
  return std::uint32_t{bytes[offset]} |
         (std::uint32_t{bytes[offset + 1u]} << 8u) |
         (std::uint32_t{bytes[offset + 2u]} << 16u) |
         (std::uint32_t{bytes[offset + 3u]} << 24u);
}
bool same(const IoprpEntry& entry, const char* name) {
  return std::strcmp(entry.name.data(), name) == 0;
}
} // namespace

IoprpSummary inspect_ioprp_image(const void* image, std::size_t length) {
  IoprpSummary result;
  if (image == nullptr || length < 3u * kRomdirEntrySize ||
      length > kMaxImageSize) {
    result.error = "IOPRP image size invalid";
    return result;
  }
  result.image_size = static_cast<std::uint32_t>(length);
  const auto* bytes = static_cast<const std::uint8_t*>(image);
  // First read the entire directory, including its terminator. Knowing which
  // entry is truly last lets us accept images with an unpadded final payload
  // without weakening bounds checks on any earlier module.
  bool terminated = false;
  for (std::size_t i = 0; i < result.modules.size(); ++i) {
    const std::size_t base = i * kRomdirEntrySize;
    if (base + kRomdirEntrySize > length) {
      result.failed_entry_index = static_cast<std::uint32_t>(i);
      result.error = "ROMDIR has no terminating entry";
      return result;
    }
    auto& entry = result.modules[i];
    std::memcpy(entry.name.data(), bytes + base, 10u);
    entry.name[10] = '\0';
    if (entry.name[0] == '\0') { terminated = true; break; }
    for (unsigned j = 0; j < 10u; ++j) {
      const auto ch = static_cast<unsigned char>(entry.name[j]);
      if (ch == 0u) break;
      if ((ch < '0' || ch > '9') && (ch < 'A' || ch > 'Z') &&
          ch != '_' && ch != '-') {
        result.failed_entry_index = static_cast<std::uint32_t>(i);
        result.error = "ROMDIR has invalid module name";
        return result;
      }
    }
    entry.size = word32(bytes, base + 12u);
    ++result.entries;
    if (same(entry, "IOPBTCONF")) result.has_bootconf = true;
    if (same(entry, "SIFCMD")) result.has_sifcmd = true;
    if (same(entry, "CDVDMAN") || same(entry, "CDVDFSV"))
      result.has_cdvd = true;
  }
  if (!terminated || result.entries < 2u ||
      !same(result.modules[0], "RESET") ||
      !same(result.modules[1], "ROMDIR")) {
    result.error = "not an IOPRP ROMDIR image";
    return result;
  }
  const auto directory_bytes = (result.entries + 1u) * kRomdirEntrySize;
  if (result.modules[0].size != 0u ||
      result.modules[1].size < directory_bytes) {
    result.error = "ROMDIR length does not cover directory";
    return result;
  }

  std::uint64_t module_offset = 0u;
  for (std::size_t i = 0; i < result.entries; ++i) {
    auto& entry = result.modules[i];
    entry.offset = static_cast<std::uint32_t>(module_offset);
    const auto actual_end = module_offset + entry.size;
    const auto aligned_end = (actual_end + 15u) & ~std::uint64_t{15u};
    if (actual_end > length) {
      result.failed_entry_index = static_cast<std::uint32_t>(i);
      result.failed_actual_end = static_cast<std::uint32_t>(actual_end);
      result.failed_aligned_end = static_cast<std::uint32_t>(aligned_end);
      result.error = "ROMDIR entry exceeds file bounds";
      return result;
    }
    if (aligned_end > length) {
      // Only the final module may omit up to 15 bytes of 16-byte padding.
      // The payload itself must still be completely inside the file.
      if (i + 1u != result.entries) {
        result.failed_entry_index = static_cast<std::uint32_t>(i);
        result.failed_actual_end = static_cast<std::uint32_t>(actual_end);
        result.failed_aligned_end = static_cast<std::uint32_t>(aligned_end);
        result.error = "ROMDIR non-final aligned entry exceeds file bounds";
        return result;
      }
      result.final_missing_padding =
          static_cast<std::uint32_t>(aligned_end - length);
    }
    module_offset = aligned_end;
    if (i + 1u == result.entries) {
      result.final_payload_end = static_cast<std::uint32_t>(actual_end);
      result.trailing_bytes =
          static_cast<std::uint32_t>(length - actual_end);
    }
  }
  result.valid = true;
  result.error = "ok";
  return result;
}
} // namespace ps2vita
