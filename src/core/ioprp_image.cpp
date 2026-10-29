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

std::uint16_t word16(const std::uint8_t* bytes, std::size_t offset) {
  return std::uint16_t{bytes[offset]} |
         (std::uint16_t{bytes[offset + 1u]} << 8u);
}

// PS2 ELF32 IRX .iopmod (SHT/PT 0x70000080) records a module version.
// It does NOT establish the FILEIO RPC 0xFF response format or authorize
// returning any version to guest code. Never execute private IOP modules.
// References: PS2SDK ps2-irxgen, ps2-bios-book IOP .iopmod layout.
bool inspect_fileio_irx_version(const std::uint8_t* data,
                                std::size_t length,
                                std::uint32_t& version) {
  if (length < 52u || std::memcmp(data, "\x7f""ELF", 4u) != 0 ||
      data[4] != 1u || data[5] != 1u || data[6] != 1u ||
      word16(data, 16u) != 0xFF80u || word16(data, 18u) != 8u)
    return false;
  constexpr std::uint32_t kIopmod = 0x70000080u;
  const auto metadata = [&](std::size_t offset, std::size_t size) -> bool {
    if (size < 36u || offset > length || size > length - offset) return false;
    constexpr char kName[] = "FILEIO";
    for (unsigned i = 0; i < 6u; ++i) {
      auto ch = data[offset + 28u + i];
      if (ch >= 'a' && ch <= 'z') ch = static_cast<std::uint8_t>(ch - 'a' + 'A');
      if (ch != static_cast<std::uint8_t>(kName[i])) return false;
    }
    if (data[offset + 34u] != 0u || data[offset + 35u] != 0u) return false;
    version = word32(data, offset + 24u);
    return true;
  };
  const auto table = [&](std::uint32_t at, std::uint16_t stride,
                         std::uint16_t count, bool sections) -> bool {
    const std::size_t minimum = sections ? 40u : 32u;
    if (!at || !count || count > 512u || stride < minimum || at > length ||
        std::uint64_t{stride} * count > length - at) return false;
    for (std::uint32_t i = 0u; i < count; ++i) {
      const auto entry = std::size_t{at} + std::size_t{i} * stride;
      if (word32(data, entry + (sections ? 4u : 0u)) != kIopmod) continue;
      if (metadata(word32(data, entry + (sections ? 16u : 4u)),
                   word32(data, entry + (sections ? 20u : 16u)))) return true;
    }
    return false;
  };
  return table(word32(data, 32u), word16(data, 46u),
               word16(data, 48u), true) ||
         table(word32(data, 28u), word16(data, 42u),
               word16(data, 44u), false);
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
    if (same(entry, "FILEIO")) result.has_fileio = true;
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
    if (same(entry, "FILEIO"))
      result.fileio_irx_version_valid = inspect_fileio_irx_version(
          bytes + entry.offset, entry.size, result.fileio_irx_version);
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
