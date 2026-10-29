#include "ps2vita/memory.hpp"

#include <cstdint>
#include <cstdio>

namespace {

unsigned checks = 0;
unsigned failures = 0;

void check(bool ok, const char* message) {
  ++checks;
  if (!ok) {
    ++failures;
    std::fprintf(stderr, "FAIL: %s\n", message);
  }
}

void test_sif_bus() {
  ps2vita::Memory memory;

  // IOP control status combines EE-visible bits with fixed SBUS hardware bits.
  check(memory.iop_read32(0x1D000040u) == 0xF0000002u,
        "IOP sees the hardware-ready SBUS control status");
  check(memory.iop_read16(0x1D000040u) == 0x0002u,
        "IOP halfword control read includes hardware-ready bit");
  check(memory.iop_read16(0x1D000042u) == 0xF000u,
        "IOP high-halfword control read includes fixed status bits");
  check(memory.read32(0x1000F240u) == 0u,
        "Fixed IOP status bits do not leak into the EE control register");

  // IOP writes to SMFLAG set bits; EE writes clear them.
  // SIFCMD uses 0x20000 to tell the EE it is initialized.
  memory.iop_write16(0x1D000032u, 0x0002u);
  check(memory.read32(0x1000F230u) == 0x00020000u,
        "IOP upper-halfword SMFLAG write is visible to EE");
  memory.write32(0x1000F230u, 0x00020000u);
  check(memory.iop_read32(0x1D000030u) == 0u,
        "EE clears the same shared SMFLAG bit");

  // EE writes to MSFLAG set bits; IOP writes clear them.
  memory.write32(0x1000F220u, 0x00030000u);
  memory.iop_write16(0x1D000022u, 0x0002u);
  check(memory.read32(0x1000F220u) == 0x00010000u,
        "IOP halfword write clears only requested MSFLAG bits");

  // The IOP publishes its SIFCMD receive address through SMCOM.
  memory.iop_write16(0x1D000010u, 0xABCDu);
  memory.iop_write16(0x1D000012u, 0x1234u);
  check(memory.read32(0x1000F210u) == 0x1234ABCDu,
        "IOP halfword SMCOM writes assemble an EE-visible address");

  // SBUS control halfword toggles must not be silently dropped.
  memory.iop_write16(0x1D000040u, 0x0020u);
  check((memory.iop_read32(0x1D000040u) & 0x00002020u) == 0x00002020u,
        "IOP halfword control write changes the real SBUS control state");
  memory.clear();
  check(memory.iop_read32(0x1D000040u) == 0xF0000002u,
        "Reset restores the hardware control status without stale flags");
  check(memory.read32(0x1000F230u) == 0u,
        "Reset clears stale IOP-to-EE handshake flags");
}

} // namespace

int main() {
  test_sif_bus();
  std::printf("IOP SIF hardware handshake: %u checks, %u failures\n",
              checks, failures);
  return failures == 0u ? 0 : 1;
}
