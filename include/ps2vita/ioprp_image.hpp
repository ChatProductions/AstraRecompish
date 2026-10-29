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
  // Failure details survive incomplete validation so hardware logs can
  // distinguish a damaged module from a legitimately unpadded final file.
  std::uint32_t failed_entry_index = 0xFFFFFFFFu;
  std::uint32_t failed_actual_end = 0;
  std::uint32_t failed_aligned_end = 0;
  std::uint32_t final_payload_end = 0;
  std::uint32_t final_missing_padding = 0;
  std::uint32_t trailing_bytes = 0;
  bool has_bootconf = false;
  bool has_sifcmd = false;
  bool has_cdvd = false;
  bool has_fileio = false;
  // Read-only IRX .iopmod module identity; NOT the SIFRPC 0xFF reply.
  bool fileio_irx_version_valid = false;
  std::uint32_t fileio_irx_version = 0u;
  std::array<IoprpEntry, kMaxEntries> modules{};
};

IoprpSummary inspect_ioprp_image(const void* image, std::size_t length);

} // namespace ps2vita
