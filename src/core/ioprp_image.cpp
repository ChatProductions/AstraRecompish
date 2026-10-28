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
  std::uint64_t module_offset = 0u;
  bool terminated = false;
  for (std::size_t i = 0; i < result.modules.size(); ++i) {
    const std::size_t base = i * kRomdirEntrySize;
    if (base + kRomdirEntrySize > length) {
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
        result.error = "ROMDIR has invalid module name";
        return result;
      }
    }
    entry.size = word32(bytes, base + 12u);
    if (module_offset > length || entry.size > length - module_offset) {
      result.error = "ROMDIR entry exceeds file bounds";
      return result;
    }
    entry.offset = static_cast<std::uint32_t>(module_offset);
    module_offset += (std::uint64_t{entry.size} + 15u) & ~std::uint64_t{15u};
    if (module_offset > length) {
      result.error = "ROMDIR aligned entry exceeds file bounds";
      return result;
    }
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
  if (result.modules[1].offset != 0u ||
      result.modules[1].size < directory_bytes) {
    result.error = "ROMDIR length does not cover directory";
    return result;
  }
  result.valid = true;
  result.error = "ok";
  return result;
}
} // namespace ps2vita
