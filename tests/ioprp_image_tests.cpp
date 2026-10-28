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
