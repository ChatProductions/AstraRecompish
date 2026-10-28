#include "ps2vita/native_iop_services.hpp"

#include "ps2vita/memory.hpp"

namespace ps2vita {
namespace {
constexpr std::uint32_t kSubAddress = 0x1D000010u;
constexpr std::uint32_t kSmFlag = 0x1D000030u;
constexpr std::uint32_t kSifInit = 0x00010000u;
constexpr std::uint32_t kCmdInit = 0x00020000u;
constexpr std::uint32_t kSystemCommand = 0x80000000u;
constexpr std::uint32_t kChangeAddress = kSystemCommand | 0u;
constexpr std::uint32_t kInitCommand = kSystemCommand | 2u;
constexpr std::uint32_t kResetCommand = kSystemCommand | 3u;
constexpr std::uint32_t kRpcBind = kSystemCommand | 9u;
constexpr std::uint32_t kRpcCall = kSystemCommand | 10u;
constexpr std::uint32_t kCdvdInitServer = 0x80000592u;
constexpr std::uint32_t kCdvdServerHandle = 0x001E1000u;
constexpr std::uint32_t kCdvdServerBuffer = 0x001E2000u;

bool ee_pointer_valid(const Memory& memory, std::uint32_t address,
                      std::uint32_t bytes) {
  const auto phys = (address & 0xE0000000u) == 0x80000000u ||
                    (address & 0xE0000000u) == 0xA0000000u
                        ? address & 0x1FFFFFFFu : address;
  const auto ram_phys = ((address & 0xFE000000u) == 0x20000000u ||
                         (address & 0xFE000000u) == 0x30000000u)
                          ? address & 0x01FFFFFFu : phys;
  return (address & 3u) == 0u && ram_phys < Memory::kRamSize &&
         bytes <= Memory::kRamSize - ram_phys && memory.valid(address, bytes);
}
} // namespace

void NativeIopServices::reset() {
  phase_ = Phase::Offline;
  ee_receive_address_ = 0u;
  command_count_ = 0u;
  unsupported_cid_ = 0u;
  unsupported_rpc_sid_ = 0u;
  cdvd_init_calls_ = 0u;
  pending_signal_sema_ = 0u;
  bound_cdvd_client_ = 0u;
  reboot_count_ = reboot_mode_ = 0u;
  reboot_arg_ = {};
}

std::uint32_t NativeIopServices::take_pending_signal_sema() {
  const auto sema = pending_signal_sema_;
  pending_signal_sema_ = 0u;
  return sema;
}

void NativeIopServices::boot(Memory& memory) {
  reset();
  // HLE equivalent of installing the IOP SIFMAN/SIFCMD modules: register a
  // real IOP receive buffer and publish the IOP-owned readiness status.
  // EE writes cannot set SMFLAG, so both changes go through the IOP bus.
  if (!memory.iop_valid(kReceiveAddress, 128u)) return;
  for (unsigned i = 0; i < 128u; ++i)
    memory.iop_write8(kReceiveAddress + i, 0u);
  memory.iop_write32(kSubAddress, kReceiveAddress);
  memory.iop_write32(kSmFlag, kSifInit | kCmdInit);
  phase_ = Phase::AwaitingInit;
}

bool NativeIopServices::receive(Memory& memory, std::uint32_t destination,
                                std::uint32_t transfer_size,
                                std::uint32_t ee_cmd_data) {
  if (phase_ == Phase::Offline) return false;
  // Other IOP RAM transfers are payloads (including prospective RPC input),
  // not SIFCMD packets; no made-up command handler is invoked for them.
  if (destination != kReceiveAddress) return true;
  if (transfer_size < 16u) return false;
  const auto size_word = memory.iop_read32(kReceiveAddress);
  const auto packet_size = size_word & 0xFFu;
  if (packet_size < 16u || packet_size > 112u ||
      packet_size > transfer_size) return false;
  const auto cid = memory.iop_read32(kReceiveAddress + 8u);
  const auto opt = memory.iop_read32(kReceiveAddress + 12u);
  ++command_count_;
  switch (cid) {
  case kInitCommand: {
    if (packet_size < 16u || (opt != 0u && opt != 1u)) return false;
    if (opt == 0u) {
      if (packet_size < 20u) return false;
      const auto address = memory.iop_read32(kReceiveAddress + 16u);
      if (!ee_pointer_valid(memory, address, 128u)) return false;
      ee_receive_address_ = address;
      phase_ = Phase::CmdConnected;
      return true;
    }
    // The SIFRPC EE init packet (opt=1) requires a *completed* SIFCMD
    // handshake. Deliver the documented SIF_CMD_SET_SREG(RPCINIT,1)
    // notification through the guest's registered SREG array. An unvalidated
    // or missing destination never receives a fabricated ready response.
    if (phase_ != Phase::CmdConnected && phase_ != Phase::RpcReady)
      return false;
    if (!ee_pointer_valid(memory, ee_cmd_data, 32u)) return false;
    const auto sregs = memory.read32(ee_cmd_data + 28u);
    if (!ee_pointer_valid(memory, sregs, 32u * 4u)) return false;
    memory.write32(sregs, 1u);
    phase_ = Phase::RpcReady;
    return true;
  }
  case kChangeAddress: {
    if (packet_size < 20u) return false;
    const auto address = memory.iop_read32(kReceiveAddress + 16u);
    if (!ee_pointer_valid(memory, address, 128u)) return false;
    ee_receive_address_ = address;
    phase_ = Phase::CmdConnected;
    return true;
  }
  case kResetCommand: {
    if (packet_size < 24u || phase_ != Phase::RpcReady) return false;
    const auto length = memory.iop_read32(kReceiveAddress + 16u);
    reboot_mode_ = memory.iop_read32(kReceiveAddress + 20u);
    if (length > 79u || 24u + length > packet_size) return false;
    reboot_arg_ = {};
    for (std::uint32_t i = 0; i < length; ++i) {
      const auto ch = memory.iop_read8(kReceiveAddress + 24u + i);
      reboot_arg_[i] = ch >= 0x20u && ch <= 0x7Eu
          ? static_cast<char>(ch) : '?';
    }
    ++reboot_count_;
    phase_ = Phase::RebootRequired;
    unsupported_cid_ = cid;
    // The reset packet is recognized, but a reboot requires the actual IOP
    // module image/ROM or a replacement module loader. Do not pretend the
    // CDVD service survived a reboot or set BOOTEND without starting it.
    return false;
  }
  case kRpcBind:
    if (packet_size < 36u) return false;
    unsupported_rpc_sid_ = memory.iop_read32(kReceiveAddress + 32u);
    if (phase_ != Phase::RpcReady ||
        unsupported_rpc_sid_ != kCdvdInitServer) {
      unsupported_cid_ = cid;
      return false;
    }
    {
      const auto client = memory.iop_read32(kReceiveAddress + 28u);
      const auto original_pkt = memory.iop_read32(kReceiveAddress + 20u);
      if (!ee_pointer_valid(memory, client, 40u) ||
          !ee_pointer_valid(memory, original_pkt, 28u)) return false;
      const auto sema = memory.read32(client + 8u);
      if (sema != UINT32_MAX && (sema == 0u || sema > 64u)) return false;
      // IOP's SIFRPC END reply attaches a real native CDVD service object
      // to the EE client. This object exists only inside this HLE service.
      memory.write32(client + 36u, kCdvdServerHandle);
      memory.write32(client + 20u, kCdvdServerBuffer);
      memory.write32(client + 24u, 0u);
      memory.write32(client, 0u); // _request_end frees the pending packet.
      memory.write32(original_pkt + 16u,
                     memory.read32(original_pkt + 16u) & ~1u);
      bound_cdvd_client_ = client;
      pending_signal_sema_ = sema == UINT32_MAX ? 0u : sema;
      unsupported_rpc_sid_ = 0u;
      return true;
    }
  case kRpcCall: {
    if (phase_ != Phase::RpcReady || packet_size < 56u) return false;
    const auto client = memory.iop_read32(kReceiveAddress + 28u);
    const auto rpc_number = memory.iop_read32(kReceiveAddress + 32u);
    const auto send_size = memory.iop_read32(kReceiveAddress + 36u);
    const auto recvbuf = memory.iop_read32(kReceiveAddress + 40u);
    const auto recv_size = memory.iop_read32(kReceiveAddress + 44u);
    const auto mode = memory.iop_read32(kReceiveAddress + 48u);
    const auto server = memory.iop_read32(kReceiveAddress + 52u);
    const auto original_pkt = memory.iop_read32(kReceiveAddress + 20u);
    if (client != bound_cdvd_client_ || client == 0u ||
        server != kCdvdServerHandle || rpc_number != 0u ||
        send_size != 4u || recv_size != 16u || mode != 1u ||
        !ee_pointer_valid(memory, recvbuf, recv_size) ||
        !ee_pointer_valid(memory, original_pkt, 28u)) {
      unsupported_cid_ = cid;
      return false;
    }
    const auto sema = memory.read32(client + 8u);
    if (sema == 0u || sema > 64u) return false;
    // CDVDFSV's first RPC (fno 0) initializes the optical-drive service,
    // *not* the disc filesystem. Return an initialized service with unknown
    // module version numbers. No game sector reads/searches are invented.
    memory.write32(recvbuf, 1u); // CdInitPkt.m_init_result
    memory.write32(recvbuf + 4u, 0u); // version unknown without IOP ROM
    memory.write32(recvbuf + 8u, 0u);
    memory.write32(recvbuf + 12u, 0u);
    memory.write32(client, 0u);
    memory.write32(original_pkt + 16u,
                   memory.read32(original_pkt + 16u) & ~1u);
    pending_signal_sema_ = sema;
    ++cdvd_init_calls_;
    return true;
  }
  default:
    unsupported_cid_ = cid;
    return false;
  }
}

} // namespace ps2vita
