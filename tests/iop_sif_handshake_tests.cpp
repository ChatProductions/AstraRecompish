#include "ps2vita/memory.hpp"
#include "ps2vita/iop_cpu.hpp"

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
  check(memory.read32(0x1000F240u) == 0xF0000102u,
        "EE retains its own fixed SBUS control status, including bit 8");

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

// Exercise the same SBUS path through executed R3000A instructions, not
// just direct host Memory calls. All instructions are synthetic and public.
void test_guest_executed_sif_handshake() {
  ps2vita::Memory memory;
  ps2vita::IopCpu cpu(memory);
  constexpr std::uint32_t program[] = {
      0x3C081D00u, // lui   t0, 0x1D00
      0x35080030u, // ori   t0, t0, 0x0030 (IOP SMFLAG)
      0x24090002u, // addiu t1, zero, 2
      0xA5090002u, // sh    t1, 2(t0) (set SMFLAG bit 17)
      0x3C0A1D00u, // lui   t2, 0x1D00
      0x8D4B0040u, // lw    t3, 0x40(t2) (IOP SBUS control)
      0x00000000u, // nop: commit the R3000A delayed load
  };
  for (std::uint32_t i = 0; i < sizeof(program) / sizeof(program[0]); ++i)
    memory.iop_write32(i * 4u, program[i]);
  cpu.state().pc = 0u;
  check(cpu.run(7u) == ps2vita::IopStopReason::None,
        "Synthetic IOP SIF handshake executes without CPU faults");
  check(cpu.state().pc == 28u && cpu.state().cycles == 7u,
        "Synthetic IOP SIF handshake executes exactly seven instructions");
  check(memory.read32(0x1000F230u) == 0x00020000u,
        "Guest SH to IOP SMFLAG signals EE SIFCMD initialization");
  check(cpu.state().gpr[11] == 0xF0000002u,
        "Guest LW observes IOP-specific SBUS status after load delay");
  check(memory.read32(0x1000F240u) == 0xF0000102u,
        "Guest IOP MMIO reads do not replace the EE SBUS status view");
}

} // namespace

int main() {
  test_sif_bus();
  test_guest_executed_sif_handshake();
  std::printf("IOP SIF hardware handshake: %u checks, %u failures\n",
              checks, failures);
  return failures == 0u ? 0 : 1;
}
