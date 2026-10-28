#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace ps2vita {

// Read-only ROMDIR directory metadata. Never executes proprietary IOP modules.
struct IoprpEntry {
  std::array<char, 11> name{};
  std::uint32_t offset = 0;
  std::uint32_t size = 0;
};

struct IoprpSummary {
  static constexpr std::size_t kMaxEntries = 128u;
  bool valid = false;
  const char* error = "not inspected";
  std::uint32_t image_size = 0;
  std::uint32_t entries = 0;
  bool has_bootconf = false;
  bool has_sifcmd = false;
  bool has_cdvd = false;
  std::array<IoprpEntry, kMaxEntries> modules{};
};

IoprpSummary inspect_ioprp_image(const void* image, std::size_t length);

} // namespace ps2vita
