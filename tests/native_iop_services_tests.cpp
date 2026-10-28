#include "ps2vita/memory.hpp"
#include "ps2vita/native_iop_services.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace {
void require(bool ok, const char* description) {
  if (!ok) { std::fprintf(stderr, "FAIL: %s\n", description); std::exit(1); }
}

void packet(ps2vita::Memory& memory, std::uint32_t cid,
            std::uint32_t opt, std::uint32_t arg, unsigned size) {
  const auto p = ps2vita::NativeIopServices::kReceiveAddress;
  memory.iop_write32(p, size);
  memory.iop_write32(p + 4, 0u);
  memory.iop_write32(p + 8, cid);
  memory.iop_write32(p + 12, opt);
  memory.iop_write32(p + 16, arg);
}
} // namespace

int main() {
  ps2vita::Memory memory;
  ps2vita::NativeIopServices services;
  constexpr std::uint32_t kSmFlagEe = 0x1000F230u;
  constexpr std::uint32_t kSubAddrEe = 0x1000F210u;
  constexpr std::uint32_t kCmdDataEe = 0x2000u;
  constexpr std::uint32_t kEeReceive = 0x2800u;
  constexpr std::uint32_t kSregs = 0x3000u;
  constexpr std::uint32_t kReceiver =
      ps2vita::NativeIopServices::kReceiveAddress;

  memory.write32(0x005CD800u, 0x1234ABCDu);
  require(memory.valid(0x205CD800u, 4u) &&
              memory.read32(0x205CD800u) == 0x1234ABCDu &&
              memory.read32(0x305CD800u) == 0x1234ABCDu &&
              !memory.valid(0x225CD800u),
          "R5900 UNCACHED_SEG/UCAB_SEG alias exactly the installed EE RAM");

  require(services.phase() == ps2vita::NativeIopServices::Phase::Offline &&
              memory.read32(kSmFlagEe) == 0u,
          "no SIF readiness without an IOP service module");
  packet(memory, 0x80000002u, 0u, kEeReceive, 20u);
  require(!services.receive(memory, kReceiver, 20u, kCmdDataEe),
          "offline receiver cannot complete an EE command");

  services.boot(memory);
  require(services.phase() == ps2vita::NativeIopServices::Phase::AwaitingInit &&
              memory.read32(kSubAddrEe) == kReceiver &&
              memory.read32(kSmFlagEe) == 0x30000u,
          "boot models IOP SIFCMD receiver and IOP-owned SMFLAG");
  memory.write32(kSmFlagEe, 0x20000u);
  require(memory.read32(kSmFlagEe) == 0x10000u,
          "EE SIF writes can only clear IOP SMFLAG bits");
  services.boot(memory);

  packet(memory, 0x80000002u, 0u, kEeReceive, 20u);
  require(services.receive(memory, kReceiver, 20u, kCmdDataEe) &&
              services.phase() == ps2vita::NativeIopServices::Phase::CmdConnected &&
              services.ee_receive_address() == kEeReceive,
          "validated INIT_CMD(0) records the EE packet receiver");

  // EE SIFCMD stores the SREG array pointer at cmd_data + 28 (PS2SDK).
  memory.write32(kCmdDataEe + 28u, kSregs);
  packet(memory, 0x80000002u, 1u, 0u, 16u);
  require(services.receive(memory, kReceiver, 16u, kCmdDataEe) &&
              services.phase() == ps2vita::NativeIopServices::Phase::RpcReady &&
              memory.read32(kSregs) == 1u,
          "RPC init command delivers SIF_CMD_SET_SREG(RPCINIT,1)");

  constexpr std::uint32_t kClient = 0x5000u;
  constexpr std::uint32_t kOriginalPacket = 0x5400u;
  constexpr std::uint32_t kResult = 0x5600u;
  memory.write32(kClient + 8u, 2u); // CreateSema returned ID 2.
  memory.write32(kOriginalPacket + 16u, 5u); // allocated RPC packet
  packet(memory, 0x80000009u, 0u, 0u, 64u);
  memory.iop_write32(kReceiver + 20u, kOriginalPacket);
  memory.iop_write32(kReceiver + 28u, kClient);
  memory.iop_write32(kReceiver + 32u, 0x80000592u);
  require(services.receive(memory, kReceiver, 64u, kCmdDataEe) &&
              memory.read32(kClient + 36u) == 0x001E1000u &&
              memory.read32(kClient + 20u) == 0x001E2000u &&
              memory.read32(kOriginalPacket + 16u) == 4u &&
              services.take_pending_signal_sema() == 2u &&
              services.take_pending_signal_sema() == 0u,
          "CDVDFSV Init server bind completes the RPC packet and semaphore");
  memory.write32(kOriginalPacket + 16u, 5u);
  packet(memory, 0x8000000Au, 0u, 0u, 64u);
  memory.iop_write32(kReceiver + 20u, kOriginalPacket);
  memory.iop_write32(kReceiver + 28u, kClient);
  memory.iop_write32(kReceiver + 32u, 0u); // CdInit function 0
  memory.iop_write32(kReceiver + 36u, 4u); // one 32-bit mode
  memory.iop_write32(kReceiver + 40u, kResult);
  memory.iop_write32(kReceiver + 44u, 16u);
  memory.iop_write32(kReceiver + 48u, 1u); // request an END response
  memory.iop_write32(kReceiver + 52u, 0x001E1000u);
  require(services.receive(memory, kReceiver, 64u, kCmdDataEe) &&
              services.cdvd_init_calls() == 1u &&
              memory.read32(kResult) == 1u &&
              services.take_pending_signal_sema() == 2u,
          "CDVD Init RPC acknowledges driver startup (not game disc access)");

  packet(memory, 0x80000009u, 0u, 0u, 64u);
  memory.iop_write32(kReceiver + 32u, 0x80000001u);
  require(!services.receive(memory, kReceiver, 64u, kCmdDataEe) &&
              services.unsupported_rpc_sid() == 0x80000001u,
          "unimplemented fileio RPC SID is an explicit boundary");

  packet(memory, 0x80000003u, 0u, 9u, 104u);
  memory.iop_write32(kReceiver + 20u, 0u); // reboot mode
  constexpr char kRebootPath[] = "rom0:UDNL";
  for (unsigned i = 0; i < 9u; ++i)
    memory.iop_write8(kReceiver + 24u + i,
                      static_cast<std::uint8_t>(kRebootPath[i]));
  require(!services.receive(memory, kReceiver, 104u, kCmdDataEe) &&
              services.phase() == ps2vita::NativeIopServices::Phase::RebootRequired &&
              services.reboot_count() == 1u &&
              services.reboot_mode() == 0u &&
              services.reboot_arg()[8] == 'L',
          "IOP reset packet is decoded, but no fictitious reboot occurs");

  services.reset();
  require(services.phase() == ps2vita::NativeIopServices::Phase::Offline &&
              services.command_count() == 0u &&
              services.unsupported_rpc_sid() == 0u,
          "service reset clears handshake and diagnostic state");

  services.boot(memory);
  packet(memory, 0x80000002u, 1u, 0u, 16u);
  memory.write32(kSregs, 0u);
  require(!services.receive(memory, kReceiver, 16u, kCmdDataEe) &&
              memory.read32(kSregs) == 0u,
          "RPC cannot become ready before completing SIFCMD init");
  packet(memory, 0x80000002u, 0u, kEeReceive, 20u);
  require(services.receive(memory, kReceiver, 20u, kCmdDataEe),
          "second SIFCMD handshake completes normally");
  packet(memory, 0x80000002u, 1u, 0u, 16u);
  memory.write32(kCmdDataEe + 28u, 0x1000F230u);
  require(!services.receive(memory, kReceiver, 16u, kCmdDataEe) &&
              memory.read32(kSregs) == 0u,
          "RPC notification never writes to a forged MMIO SREG pointer");

  services.boot(memory);
  services.permit_reboot_with_verified_image(true);
  memory.write32(kCmdDataEe + 28u, kSregs);
  packet(memory, 0x80000002u, 0u, kEeReceive, 20u);
  require(services.receive(memory, kReceiver, 20u, kCmdDataEe),
          "verified-image scenario completes SIFCMD initialization");
  packet(memory, 0x80000002u, 1u, 0u, 16u);
  require(services.receive(memory, kReceiver, 16u, kCmdDataEe),
          "verified-image scenario completes RPC initialization");
  packet(memory, 0x80000003u, 0u, 9u, 104u);
  memory.iop_write32(kReceiver + 20u, 0u);
  for (unsigned i = 0; i < 9u; ++i)
    memory.iop_write8(kReceiver + 24u + i,
                      static_cast<std::uint8_t>(kRebootPath[i]));
  require(services.receive(memory, kReceiver, 104u, kCmdDataEe) &&
              services.phase() == ps2vita::NativeIopServices::Phase::RebootPending &&
              services.complete_reboot(memory) &&
              services.phase() == ps2vita::NativeIopServices::Phase::AwaitingInit &&
              services.reboot_completed() == 1u &&
              (memory.read32(kSmFlagEe) & 0x70000u) == 0x70000u,
          "verified IOPRP enables a bounded native IOP service restart");
  std::puts("Native IOP service handshake tests passed.");
  return 0;
}
