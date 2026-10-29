#include "ps2vita/ioprp_image.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {
void require(bool condition, const char* explanation) {
  if (!condition) { std::fprintf(stderr, "FAIL: %s\n", explanation); std::exit(1); }
}

void entry(std::vector<std::uint8_t>& bytes, std::size_t index,
           const char* name, std::uint32_t size) {
  const auto offset = index * 16u;
  std::memcpy(bytes.data() + offset, name, std::strlen(name));
  for (unsigned i = 0; i < 4u; ++i)
    bytes[offset + 12u + i] = static_cast<std::uint8_t>(size >> (i * 8u));
}
}

int main() {
  // Synthetic ROMDIR with the layout used by IOPRP images. No game modules.
  std::vector<std::uint8_t> image(0xB0u, 0u);
  entry(image, 0u, "RESET", 0u);
  entry(image, 1u, "ROMDIR", 0x70u);
  entry(image, 2u, "EXTINFO", 16u);
  entry(image, 3u, "IOPBTCONF", 16u);
  entry(image, 4u, "SIFCMD", 16u);
  entry(image, 5u, "CDVDFSV", 16u);
  const auto result = ps2vita::inspect_ioprp_image(image.data(), image.size());
  require(result.valid && result.entries == 6u && result.has_bootconf &&
              result.has_sifcmd && result.has_cdvd &&
              result.modules[5].offset == 0xA0u &&
              result.modules[5].size == 16u,
          "ROMDIR parser validates and locates known IOP service modules");

  // The actual IOPRP from a Vita run was an odd number of bytes. A final
  // module's data may reach EOF without containing its 16-byte padding.
  // This cannot be permitted for modules with a successor.
  entry(image, 5u, "CDVDFSV", 15u);
  image.resize(0xAFu);
  const auto unpadded = ps2vita::inspect_ioprp_image(image.data(), image.size());
  require(unpadded.valid && unpadded.entries == 6u &&
              unpadded.final_payload_end == 0xAFu &&
              unpadded.final_missing_padding == 1u &&
              unpadded.trailing_bytes == 0u,
          "final unpadded payload should remain valid when all bytes exist");

  // A truncated byte of actual data is corruption, not missing padding.
  entry(image, 5u, "CDVDFSV", 16u);
  const auto truncated = ps2vita::inspect_ioprp_image(image.data(), image.size());
  require(!truncated.valid && truncated.failed_entry_index == 5u &&
              truncated.failed_actual_end == 0xB0u,
          "truncation inside a final payload must be rejected");
  image.resize(0xB0u, 0u);

  // IOPBTCONF isn't an essential directory entry in every valid ROMDIR.
  // Validation reports that absence; HLE services have separate requirements.
  image[3u * 16u + 3u] = 'X';
  const auto no_bootconf =
      ps2vita::inspect_ioprp_image(image.data(), image.size());
  require(no_bootconf.valid && !no_bootconf.has_bootconf &&
              no_bootconf.has_sifcmd && no_bootconf.has_cdvd,
          "valid ROMDIR without IOPBTCONF is reported accurately");
  image[3u * 16u + 3u] = 'B';

  image.resize(0xAFu);
  entry(image, 4u, "SIFCMD", 29u);
  const auto missing_middle_padding =
      ps2vita::inspect_ioprp_image(image.data(), image.size());
  require(!missing_middle_padding.valid &&
              missing_middle_padding.failed_entry_index == 4u &&
              missing_middle_padding.failed_actual_end == 0xADu &&
              missing_middle_padding.failed_aligned_end == 0xB0u,
          "non-final payload cannot borrow a final-entry padding exception");
  image.resize(0xB0u, 0u);
  entry(image, 4u, "SIFCMD", 16u);

  entry(image, 1u, "ROMDIR", 16u);
  require(!ps2vita::inspect_ioprp_image(image.data(), image.size()).valid,
          "undersized directory cannot masquerade as a valid IOPRP");
  entry(image, 1u, "ROMDIR", 0x70u);
  entry(image, 5u, "CDVDFSV", 0xFFFFFFF0u);
  require(!ps2vita::inspect_ioprp_image(image.data(), image.size()).valid,
          "module lengths must stay inside the supplied image");
  require(!ps2vita::inspect_ioprp_image(nullptr, 0u).valid,
          "missing image is rejected without any memory reads");
  std::puts("IOPRP ROMDIR parsing tests passed.");
  return 0;
}
