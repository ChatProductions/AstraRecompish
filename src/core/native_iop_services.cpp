#include "ps2vita/native_iop_services.hpp"

#include "ps2vita/memory.hpp"

namespace ps2vita {
namespace {
constexpr std::uint32_t kSubAddress = 0x1D000010u;
constexpr std::uint32_t kSmFlag = 0x1D000030u;
constexpr std::uint32_t kSifInit = 0x00010000u;
constexpr std::uint32_t kCmdInit = 0x00020000u;
constexpr std::uint32_t kBootEnd = 0x00040000u;
constexpr std::uint32_t kSystemCommand = 0x80000000u;
constexpr std::uint32_t kChangeAddress = kSystemCommand | 0u;
constexpr std::uint32_t kInitCommand = kSystemCommand | 2u;
constexpr std::uint32_t kResetCommand = kSystemCommand | 3u;
constexpr std::uint32_t kRpcBind = kSystemCommand | 9u;
constexpr std::uint32_t kRpcCall = kSystemCommand | 10u;
constexpr std::uint32_t kCdvdInitServer = 0x80000592u;
constexpr std::uint32_t kFileioServer = 0x80000001u;
constexpr std::uint32_t kCdvdServerHandle = 0x001E1000u;
constexpr std::uint32_t kCdvdServerBuffer = 0x001E2000u;
constexpr std::uint32_t kFileioServerHandle = 0x001E3000u;
constexpr std::uint32_t kFileioServerBuffer = 0x001E4000u;
constexpr std::uint32_t kFileioMaxRead = 1024u * 1024u;

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

bool ee_buffer_valid(const Memory& memory, std::uint32_t address,
                     std::uint32_t bytes) {
  const auto phys = (address & 0xE0000000u) == 0x80000000u ||
                    (address & 0xE0000000u) == 0xA0000000u
                        ? address & 0x1FFFFFFFu : address;
  const auto ram_phys = ((address & 0xFE000000u) == 0x20000000u ||
                         (address & 0xFE000000u) == 0x30000000u)
                          ? address & 0x01FFFFFFu : phys;
  return ram_phys < Memory::kRamSize &&
         bytes <= Memory::kRamSize - ram_phys && memory.valid(address, bytes);
}
} // namespace

void NativeIopServices::close_all_files() {
  for (auto& slot : open_files_) {
    if (slot.in_use && file_backend_) file_backend_->close(slot.backend_handle);
    slot = {};
  }
}

void NativeIopServices::reset() {
  close_all_files();
  phase_ = Phase::Offline;
  ee_receive_address_ = 0u;
  command_count_ = 0u;
  unsupported_cid_ = 0u;
  unsupported_rpc_sid_ = 0u;
  cdvd_init_calls_ = 0u;
  bound_fileio_client_ = 0u;
  fileio_bind_count_ = fileio_request_count_ = fileio_total_read_ = 0u;
  fileio_last_function_ = 0xFFFFFFFFu;
  fileio_last_result_ = 0;
  fileio_last_path_ = {};
  last_rpc_call_ = {};
  pending_signal_sema_ = 0u;
  bound_cdvd_client_ = 0u;
  reboot_count_ = reboot_mode_ = 0u;
  reboot_completed_ = 0u;
  reboot_arg_ = {};
  verified_ioprp_ = false;
}

bool NativeIopServices::complete_reboot(Memory& memory) {
  if (phase_ != Phase::RebootPending || !verified_ioprp_) return false;
  // Re-create the SIFCMD endpoint as a *native service* after accepting a
  // verified ROMDIR image. No IOP IRX/ELF instructions execute in this path.
  // Unsupported servers after the reboot still stop as unknown commands.
  if (!memory.iop_valid(kReceiveAddress, 128u)) return false;
  for (unsigned i = 0; i < 128u; ++i)
    memory.iop_write8(kReceiveAddress + i, 0u);
  memory.iop_write32(kSubAddress, kReceiveAddress);
  memory.iop_write32(kSmFlag, kSifInit | kCmdInit | kBootEnd);
  ee_receive_address_ = 0u;
  close_all_files();
  bound_cdvd_client_ = 0u;
  bound_fileio_client_ = 0u;
  pending_signal_sema_ = 0u;
  phase_ = Phase::AwaitingInit;
  ++reboot_completed_;
  return true;
}

std::uint32_t NativeIopServices::take_pending_signal_sema() {
  const auto sema = pending_signal_sema_;
  pending_signal_sema_ = 0u;
  return sema;
}

bool NativeIopServices::execute_fileio(
    Memory& memory, std::uint32_t function, std::uint32_t send_size,
    std::uint32_t recvbuf, std::uint32_t recv_size) {
  // PS2SDK fileio-common.h: RPC fno 0=open, 1=close, 2=read,
  // 3=write, 4=lseek. Arguments were copied by the EE to our IOP-side
  // RPC buffer through a separate SIF DMA operation.
  if (!file_backend_ || recv_size != 4u ||
      !ee_pointer_valid(memory, recvbuf, 4u) ||
      send_size > 4096u ||
      !memory.iop_valid(kFileioServerBuffer, send_size)) return false;
  ++fileio_request_count_;
  fileio_last_function_ = function;
  std::int32_t answer = -38; // ENOSYS; not a fabricated successful reply.
  switch (function) {
  case 0u: { // _fio_open_arg: flags (u32) + path[256].
    if (send_size < 260u) return false;
    const auto flags = memory.iop_read32(kFileioServerBuffer);
    std::array<char, 257> path{};
    bool terminated = false;
    for (std::uint32_t i = 0; i < 256u; ++i) {
      const auto ch = memory.iop_read8(kFileioServerBuffer + 4u + i);
      if (ch == 0u) { terminated = true; break; }
      if (ch < 0x20u || ch > 0x7Eu) return false;
      path[i] = static_cast<char>(ch);
    }
    if (!terminated) return false;
    fileio_last_path_ = path;
    // No writes, creates, or truncates on a device containing user data.
    if ((flags & 0x0003u) > 1u ||
        (flags & (0x0100u | 0x0200u | 0x0400u | 0x0800u)))
      answer = -30; // EROFS
    else {
      std::size_t free_slot = open_files_.size();
      for (std::size_t i = 0; i < open_files_.size(); ++i)
        if (!open_files_[i].in_use) { free_slot = i; break; }
      if (free_slot == open_files_.size()) answer = -24; // EMFILE
      else {
        const auto descriptor = file_backend_->open(path.data(), flags);
        answer = descriptor < 0 ? descriptor :
            static_cast<std::int32_t>(free_slot + 3u);
        if (descriptor >= 0)
          open_files_[free_slot] = {descriptor, true};
      }
    }
    break;
  }
  case 1u: { // close(fd)
    if (send_size < 4u) return false;
    const auto fd = memory.iop_read32(kFileioServerBuffer);
    const auto index = fd - 3u;
    if (index >= open_files_.size() || !open_files_[index].in_use)
      answer = -9; // EBADF
    else {
      answer = file_backend_->close(open_files_[index].backend_handle);
      open_files_[index] = {};
    }
    break;
  }
  case 2u: { // _fio_read_arg: fd, EE dst, bytes, EE _fio_read_data.
    if (send_size < 16u) return false;
    const auto fd = memory.iop_read32(kFileioServerBuffer);
    const auto dest = memory.iop_read32(kFileioServerBuffer + 4u);
    const auto count = memory.iop_read32(kFileioServerBuffer + 8u);
    const auto completion = memory.iop_read32(kFileioServerBuffer + 12u);
    if (count > kFileioMaxRead ||
        !ee_buffer_valid(memory, dest, count) ||
        !ee_pointer_valid(memory, completion, 48u)) return false;
    const auto index = fd - 3u;
    if (index >= open_files_.size() || !open_files_[index].in_use) {
      answer = -9;
      break;
    }
    std::array<std::uint8_t, 4096> chunk{};
    std::uint32_t received = 0u;
    while (received < count) {
      const auto n = count - received < chunk.size()
          ? count - received : static_cast<std::uint32_t>(chunk.size());
      const auto got = file_backend_->read(
          open_files_[index].backend_handle, chunk.data(), n);
      if (got < 0) {
        answer = received ? static_cast<std::int32_t>(received) : got;
        break;
      }
      if (static_cast<std::uint32_t>(got) > n) return false;
      if (got == 0) break; // real EOF, no invented data
      if (!memory.copy_in(dest + received, chunk.data(),
                          static_cast<std::uint32_t>(got))) return false;
      received += static_cast<std::uint32_t>(got);
    }
    if (answer == -38) answer = static_cast<std::int32_t>(received);
    // Fileio's EE completion callback copies unaligned header/tail fragments
    // from _fio_read_data. The native transfer already copied all bytes to
    // their final EE destination; indicate no fragments remain.
    if (!memory.zero(completion, 48u)) return false;
    fileio_total_read_ += received;
    break;
  }
  case 3u: // write deliberately unavailable in read-only test builds.
    answer = -30; // EROFS
    break;
  case 4u: { // _fio_lseek_arg: fd, signed offset, whence.
    if (send_size < 12u) return false;
    const auto fd = memory.iop_read32(kFileioServerBuffer);
    const auto offset = static_cast<std::int32_t>(
        memory.iop_read32(kFileioServerBuffer + 4u));
    const auto whence = memory.iop_read32(kFileioServerBuffer + 8u);
    const auto index = fd - 3u;
    if (whence > 2u) answer = -22; // EINVAL
    else if (index >= open_files_.size() || !open_files_[index].in_use)
      answer = -9;
    else answer = file_backend_->seek(
        open_files_[index].backend_handle, offset, whence);
    break;
  }
  default:
    // A new FILEIO function must be studied and implemented explicitly.
    return false;
  }
  fileio_last_result_ = answer;
  memory.write32(recvbuf, static_cast<std::uint32_t>(answer));
  return true;
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
    if (verified_ioprp_) {
      phase_ = Phase::RebootPending;
      return true;
    }
    phase_ = Phase::RebootRequired;
    unsupported_cid_ = cid;
    return false; // No IOP image to validate or start a native replacement.
  }
  case kRpcBind:
    if (packet_size < 36u) return false;
    unsupported_rpc_sid_ = memory.iop_read32(kReceiveAddress + 32u);
    if (phase_ != Phase::RpcReady ||
        (unsupported_rpc_sid_ != kCdvdInitServer &&
         (unsupported_rpc_sid_ != kFileioServer || !file_backend_))) {
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
      const bool fileio = unsupported_rpc_sid_ == kFileioServer;
      // Bind to the service's own native IOP receive buffer. FILEIO exists
      // only with an explicit platform file backend, not as a fake RPC stub.
      memory.write32(client + 36u,
                     fileio ? kFileioServerHandle : kCdvdServerHandle);
      memory.write32(client + 20u,
                     fileio ? kFileioServerBuffer : kCdvdServerBuffer);
      memory.write32(client + 24u, 0u);
      memory.write32(client, 0u); // _request_end frees the pending packet.
      memory.write32(original_pkt + 16u,
                     memory.read32(original_pkt + 16u) & ~1u);
      if (fileio) {
        bound_fileio_client_ = client;
        ++fileio_bind_count_;
      } else bound_cdvd_client_ = client;
      pending_signal_sema_ = sema == UINT32_MAX ? 0u : sema;
      unsupported_rpc_sid_ = 0u;
      return true;
    }
  case kRpcCall: {
    last_rpc_call_ = {};
    last_rpc_call_.packet_size = packet_size;
    last_rpc_call_.bound_fileio_client = bound_fileio_client_;
    if (phase_ != Phase::RpcReady || packet_size < 56u) {
      last_rpc_call_.reject_flags = (phase_ != Phase::RpcReady ? 512u : 0u) |
                                    (packet_size < 56u ? 256u : 0u);
      unsupported_cid_ = cid;
      return false;
    }
    const auto client = memory.iop_read32(kReceiveAddress + 28u);
    const auto rpc_number = memory.iop_read32(kReceiveAddress + 32u);
    const auto send_size = memory.iop_read32(kReceiveAddress + 36u);
    const auto recvbuf = memory.iop_read32(kReceiveAddress + 40u);
    const auto recv_size = memory.iop_read32(kReceiveAddress + 44u);
    const auto mode = memory.iop_read32(kReceiveAddress + 48u);
    const auto server = memory.iop_read32(kReceiveAddress + 52u);
    const auto original_pkt = memory.iop_read32(kReceiveAddress + 20u);
    // Snapshot all packet fields before attempting an RPC. The r7 hardware
    // report proves FILEIO bound, but cannot identify which call guard failed.
    last_rpc_call_.client = client;
    last_rpc_call_.server = server;
    last_rpc_call_.function = rpc_number;
    last_rpc_call_.send_size = send_size;
    last_rpc_call_.recvbuf = recvbuf;
    last_rpc_call_.recv_size = recv_size;
    last_rpc_call_.mode = mode;
    last_rpc_call_.original_packet = original_pkt;
    const bool client_valid = ee_pointer_valid(memory, client, 40u);
    if (client_valid) last_rpc_call_.sema = memory.read32(client + 8u);
    if (bound_fileio_client_ != 0u &&
        (client == bound_fileio_client_ || server == kFileioServerHandle)) {
      last_rpc_call_.fileio_candidate = 1u;
      last_rpc_call_.reject_flags =
          (client != bound_fileio_client_ ? 1u : 0u) |
          (server != kFileioServerHandle ? 2u : 0u) |
          (recv_size != 4u ? 4u : 0u) |
          (mode != 1u ? 8u : 0u) |
          (!ee_pointer_valid(memory, original_pkt, 28u) ? 16u : 0u) |
          (last_rpc_call_.sema == 0u || last_rpc_call_.sema > 64u ? 32u : 0u) |
          (!client_valid ? 64u : 0u);
    }
    if (client == bound_fileio_client_ && client != 0u &&
        server == kFileioServerHandle) {
      if (!client_valid) {
        unsupported_cid_ = cid;
        unsupported_rpc_sid_ = kFileioServer;
        return false;
      }
      const auto sema = last_rpc_call_.sema;
      if (recv_size != 4u || mode != 1u ||
          !ee_pointer_valid(memory, original_pkt, 28u) ||
          sema == 0u || sema > 64u ||
          !execute_fileio(memory, rpc_number, send_size,
                          recvbuf, recv_size)) {
        if (last_rpc_call_.reject_flags == 0u)
          last_rpc_call_.reject_flags = 128u;
        unsupported_cid_ = cid;
        unsupported_rpc_sid_ = kFileioServer;
        return false;
      }
      memory.write32(client, 0u);
      memory.write32(original_pkt + 16u,
                     memory.read32(original_pkt + 16u) & ~1u);
      pending_signal_sema_ = sema;
      unsupported_rpc_sid_ = 0u;
      return true;
    }
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
