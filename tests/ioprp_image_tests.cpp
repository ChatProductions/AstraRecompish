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
void put16(std::vector<std::uint8_t>& b, std::size_t at,
           std::uint16_t v) {
  b[at] = static_cast<std::uint8_t>(v);
  b[at + 1u] = static_cast<std::uint8_t>(v >> 8u);
}
void put32(std::vector<std::uint8_t>& b, std::size_t at,
           std::uint32_t v) {
  for (unsigned i = 0; i < 4u; ++i)
    b[at + i] = static_cast<std::uint8_t>(v >> (8u * i));
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


  // Entirely synthetic FILEIO IRX, including a bounded .iopmod header.
  std::vector<std::uint8_t> irx_image(0x140u, 0u);
  entry(irx_image, 0u, "RESET", 0u);
  entry(irx_image, 1u, "ROMDIR", 0x40u);
  entry(irx_image, 2u, "FILEIO", 0x100u);
  constexpr std::size_t kIrX = 0x40u;
  constexpr std::size_t kSection = kIrX + 0x68u;
  constexpr std::size_t kMetadata = kIrX + 0xA0u;
  std::memcpy(irx_image.data() + kIrX, "\x7f""ELF", 4u);
  irx_image[kIrX + 4u] = 1u;
  irx_image[kIrX + 5u] = 1u;
  irx_image[kIrX + 6u] = 1u;
  put16(irx_image, kIrX + 16u, 0xFF80u);
  put16(irx_image, kIrX + 18u, 8u);
  put32(irx_image, kIrX + 32u, 0x40u);
  put16(irx_image, kIrX + 46u, 40u);
  put16(irx_image, kIrX + 48u, 2u);
  put32(irx_image, kSection + 4u, 0x70000080u);
  put32(irx_image, kSection + 16u, 0xA0u);
  put32(irx_image, kSection + 20u, 36u);
  put32(irx_image, kMetadata + 24u, 0x0208u);
  std::memcpy(irx_image.data() + kMetadata + 28u, "FILEIO", 6u);
  const auto irx = ps2vita::inspect_ioprp_image(irx_image.data(), irx_image.size());
  require(irx.valid && irx.has_fileio && irx.fileio_irx_version_valid &&
              irx.fileio_irx_version == 0x0208u,
          "synthetic FILEIO .iopmod version is read without executing IOP code");
  put32(irx_image, kSection + 16u, 0xF0u);
  require(!ps2vita::inspect_ioprp_image(irx_image.data(), irx_image.size())
               .fileio_irx_version_valid,
          "out-of-bounds FILEIO .iopmod remains unknown");
  put32(irx_image, kSection + 16u, 0xA0u);
  irx_image[kMetadata + 28u] = 'X';
  require(!ps2vita::inspect_ioprp_image(irx_image.data(), irx_image.size())
               .fileio_irx_version_valid,
          "wrong module name cannot certify FILEIO version");
  irx_image[kMetadata + 28u] = 'F';
  put32(irx_image, kIrX + 32u, 0u);
  put32(irx_image, kIrX + 28u, 0x34u);
  put16(irx_image, kIrX + 42u, 32u);
  put16(irx_image, kIrX + 44u, 1u);
  put32(irx_image, kIrX + 0x34u, 0x70000080u);
  put32(irx_image, kIrX + 0x38u, 0xA0u);
  put32(irx_image, kIrX + 0x44u, 36u);
  require(ps2vita::inspect_ioprp_image(irx_image.data(), irx_image.size())
               .fileio_irx_version_valid,
          "IRX program-header .iopmod version is recognized");
  put32(irx_image, kIrX + 0x38u, 0xFFFFu);
  require(!ps2vita::inspect_ioprp_image(irx_image.data(), irx_image.size())
               .fileio_irx_version_valid,
          "invalid IRX program-header bounds cannot certify FILEIO version");

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
