#include "ps2vita/memory.hpp"
#include "ps2vita/native_iop_services.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

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

class TestFiles final : public ps2vita::NativeFileBackend {
public:
  int open(const char* path, std::uint32_t) override {
    if (std::strcmp(path, "host:NFS\\TEST.BIN") != 0) return -2;
    position = 0;
    ++opens;
    return 42;
  }
  int close(int fd) override {
    if (fd != 42) return -9;
    ++closes;
    return 0;
  }
  int read(int fd, void* out, std::uint32_t count) override {
    if (fd != 42) return -9;
    static constexpr char data[] = "FILEIO";
    const auto rest = position < sizeof(data) - 1u ?
        static_cast<std::uint32_t>(sizeof(data) - 1u - position) : 0u;
    const auto n = rest < count ? rest : count;
    std::memcpy(out, data + position, n);
    position += n;
    return static_cast<int>(n);
  }
  int seek(int fd, std::int32_t offset, std::uint32_t whence) override {
    if (fd != 42) return -9;
    std::int32_t base = whence == 0u ? 0 : whence == 1u ?
        static_cast<std::int32_t>(position) : 6;
    if (base + offset < 0 || base + offset > 6) return -22;
    position = static_cast<std::uint32_t>(base + offset);
    return static_cast<int>(position);
  }
  std::uint32_t position = 0;
  int opens = 0;
  int closes = 0;
};
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

  // FILEIO service after a completed reboot. The host mock supplies actual
  // bytes; no game asset is invented or embedded in the native implementation.
  // Final r10 hardware telemetry: FILEIO client 0x005D0DC0, semaphore 14,
  // server 0x001E3000, NOWAIT mode 1, and an 8-byte receive buffer.
  // The operation and send payload were not captured. TestFiles supplies
  // synthetic operation data; this is not a replay of private game bytes.
  constexpr std::uint32_t kObservedFileioClient = 0x005D0DC0u;
  memory.write32(kObservedFileioClient + 8u, 14u);
  TestFiles files;
  services.set_file_backend(&files);
  packet(memory, 0x80000002u, 0u, kEeReceive, 20u);
  require(services.receive(memory, kReceiver, 20u, kCmdDataEe),
          "post-reboot SIFCMD accepts EE receiver");
  packet(memory, 0x80000002u, 1u, 0u, 16u);
  require(services.receive(memory, kReceiver, 16u, kCmdDataEe),
          "post-reboot SIFRPC reaches ready state");
  memory.write32(kOriginalPacket + 16u, 5u);
  packet(memory, 0x80000009u, 0u, 0u, 64u);
  memory.iop_write32(kReceiver + 20u, kOriginalPacket);
  memory.iop_write32(kReceiver + 28u, kObservedFileioClient);
  memory.iop_write32(kReceiver + 32u, 0x80000001u);
  require(services.receive(memory, kReceiver, 64u, kCmdDataEe) &&
              memory.read32(kObservedFileioClient + 36u) == 0x001E3000u &&
              memory.read32(kObservedFileioClient + 20u) == 0x001E4000u &&
              services.fileio_bind_count() == 1u &&
              services.take_pending_signal_sema() == 14u,
          "FILEIO RPC binds only with an installed native backend");

  constexpr std::uint32_t kFileBuf = 0x001E4000u;
  constexpr std::uint32_t kDestination = 0x5A01u; // deliberately unaligned
  constexpr std::uint32_t kCompletion = 0x5B00u;
  const auto file_call = [&](std::uint32_t function, std::uint32_t size,
                             std::uint32_t mode = 1u,
                             std::uint32_t reply_size = 4u,
                             std::uint32_t reply_address = 0u) {
    memory.write32(kOriginalPacket + 16u, 5u);
    packet(memory, 0x8000000Au, 0u, 0u, 64u);
    memory.iop_write32(kReceiver + 20u, kOriginalPacket);
    memory.iop_write32(kReceiver + 28u, kObservedFileioClient);
    memory.iop_write32(kReceiver + 32u, function);
    memory.iop_write32(kReceiver + 36u, size);
    memory.iop_write32(kReceiver + 40u,
                       reply_address == 0u ? kResult : reply_address);
    memory.iop_write32(kReceiver + 44u, reply_size);
    memory.iop_write32(kReceiver + 48u, mode);
    memory.iop_write32(kReceiver + 52u, 0x001E3000u);
    return services.receive(memory, kReceiver, 64u, kCmdDataEe);
  };
  constexpr char kName[] = "host:NFS\\TEST.BIN";
  memory.iop_write32(kFileBuf, 1u); // FIO_O_RDONLY
  for (unsigned i = 0; i < sizeof(kName); ++i)
    memory.iop_write8(kFileBuf + 4u + i,
                      static_cast<std::uint8_t>(kName[i]));
  require(file_call(0u, 272u) && memory.read32(kResult) == 3u &&
              files.opens == 1 && services.fileio_request_count() == 1u,
          "FILEIO open returns a tracked guest descriptor for real file");
  services.take_pending_signal_sema();
  require(services.last_rpc_call().reject_flags == 0u &&
              services.last_rpc_call().function == 0u &&
              services.last_rpc_call().server == 0x001E3000u &&
              services.last_rpc_call().bound_fileio_client == kObservedFileioClient &&
              services.last_rpc_call().sema == 14u,
          "successful FILEIO call captures the real RPC fields");
  require(!file_call(1u, 4u, 2u) &&
              (services.last_rpc_call().reject_flags & 8u) != 0u &&
              services.last_rpc_call().mode == 2u &&
              services.fileio_request_count() == 1u && files.closes == 0,
          "unsupported FILEIO RPC mode is diagnosed without closing a file");
  // Standard blocking SIF RPC is mode 0, and must work for real file I/O.
  memory.iop_write32(kFileBuf, 3u);
  memory.iop_write32(kFileBuf + 4u, 0u);
  memory.iop_write32(kFileBuf + 8u, 1u);
  require(file_call(4u, 12u, 0u) && memory.read32(kResult) == 0u &&
              services.last_rpc_call().reject_flags == 0u &&
              services.last_rpc_call().mode == 0u &&
              services.fileio_request_count() == 2u,
          "blocking FILEIO RPC seek succeeds without bypassing validation");
  services.take_pending_signal_sema();
  // Physical Vita r10 captured an eight-byte FILEIO reply with mode 1.
  memory.write32(kResult + 4u, 0xA5A5A5A5u);
  require(file_call(4u, 12u, 1u, 8u) &&
              memory.read32(kResult) == 0u &&
              memory.read32(kResult + 4u) == 0u &&
              services.last_rpc_call().recv_size == 8u &&
              services.last_rpc_call().mode == 1u &&
              services.last_rpc_call().sema == 14u &&
              services.last_rpc_call().server == 0x001E3000u &&
              services.last_rpc_call().reject_flags == 0u &&
              services.fileio_request_count() == 3u,
          "eight-byte FILEIO reply returns actual status and zeroed padding");
  require(services.take_pending_signal_sema() == 14u,
          "r10 FILEIO NOWAIT completion signals observed semaphore 14");
  require(!file_call(4u, 12u, 1u, 6u) &&
              (services.last_rpc_call().reject_flags & 4u) != 0u &&
              services.fileio_request_count() == 3u,
          "mis-sized FILEIO response remains an explicit rejection");
  // A valid 4-byte word at the end of EE RAM cannot host an 8-byte reply.
  // Rejection must precede the operation and preserve the existing word.
  constexpr std::uint32_t kLastEeWord = 0x01FFFFFCu;
  memory.write32(kLastEeWord, 0x0BADC0DEu);
  require(!file_call(4u, 12u, 1u, 8u, kLastEeWord) &&
              services.last_rpc_call().reject_flags == 128u &&
              services.fileio_request_count() == 3u &&
              memory.read32(kLastEeWord) == 0x0BADC0DEu,
          "eight-byte FILEIO reply rejects a buffer crossing the EE RAM boundary");

  memory.iop_write32(kFileBuf, 3u);
  memory.iop_write32(kFileBuf + 4u, kDestination);
  memory.iop_write32(kFileBuf + 8u, 6u);
  memory.iop_write32(kFileBuf + 12u, kCompletion);
  memory.write32(kCompletion, 0xFFFFFFFFu);
  require(file_call(2u, 16u) && memory.read32(kResult) == 6u &&
              memory.read8(kDestination) == 'F' &&
              memory.read8(kDestination + 5u) == 'O' &&
              memory.read32(kCompletion) == 0u &&
              services.fileio_total_read() == 6u,
          "FILEIO read transfers actual unaligned bytes and clears callback fragments");
  services.take_pending_signal_sema();
  require(file_call(2u, 16u) && memory.read32(kResult) == 0u,
          "FILEIO read reports real EOF");
  services.take_pending_signal_sema();

  memory.iop_write32(kFileBuf, 3u);
  memory.iop_write32(kFileBuf + 4u, 2u);
  memory.iop_write32(kFileBuf + 8u, 0u);
  require(file_call(4u, 12u) && memory.read32(kResult) == 2u,
          "FILEIO seek uses the backend offset");
  services.take_pending_signal_sema();
  memory.iop_write32(kFileBuf, 3u);
  require(file_call(1u, 4u) && memory.read32(kResult) == 0u &&
              files.closes == 1,
          "FILEIO close releases the tracked descriptor");
  services.take_pending_signal_sema();

  memory.iop_write32(kFileBuf, 3u);
  require(file_call(1u, 4u) &&
              static_cast<std::int32_t>(memory.read32(kResult)) == -9,
          "FILEIO rejects already-closed guest descriptors");
  services.take_pending_signal_sema();
  require(!file_call(12u, 4u) && services.fileio_last_function() == 12u,
          "unsupported FILEIO operations remain explicit fault boundaries");
  // The r11 native Vita trace reached retail libkernl FILEIO 0xFF version
  // negotiation. The generic service must still reject it when no title
  // ABI was supplied (no invented IOP firmware version).
  const auto before_ff = services.fileio_request_count();
  const auto before_open = files.opens;
  const auto before_close = files.closes;
  memory.write32(kResult, 0xC0FFEE01u);
  memory.write32(kResult + 4u, 0xC0FFEE02u);
  require(!file_call(0xFFu, 0u, 1u, 8u) &&
              services.last_rpc_call().function == 0xFFu &&
              services.last_rpc_call().reject_flags == 128u &&
              services.last_rpc_call().sema == 14u &&
              services.fileio_last_function() == 0xFFu &&
              services.fileio_request_count() == before_ff + 1u &&
              memory.read32(kResult) == 0xC0FFEE01u &&
              memory.read32(kResult + 4u) == 0xC0FFEE02u &&
              files.opens == before_open && files.closes == before_close &&
              services.take_pending_signal_sema() == 0u,
          "unconfigured FILEIO 0xFF rejects without reply or file side effects");

  // NFSMW's real ELF carries the tag PsIIlibkernl2800, i.e. four version
  // bytes '2','8','0','0'. Retail sceFsInit consumes a four-byte RPC answer.
  constexpr std::uint32_t kNfsmwLibkernl2800 = 0x30303832u;
  services.set_fileio_compatibility_version(kNfsmwLibkernl2800);
  memory.iop_write32(kFileBuf, 0x12345678u); // version send buffer is opaque
  require(!file_call(0xFFu, 5u, 0u, 4u) &&
              services.last_rpc_call().reject_flags == 128u &&
              memory.read32(kResult) == 0xC0FFEE01u,
          "malformed version RPC must not write a compatibility response");
  require(file_call(0xFFu, 4u, 0u, 4u) &&
              memory.read32(kResult) == kNfsmwLibkernl2800 &&
              services.last_rpc_call().reject_flags == 0u &&
              services.fileio_last_function() == 0xFFu &&
              services.fileio_last_result() ==
                  static_cast<std::int32_t>(kNfsmwLibkernl2800) &&
              files.opens == before_open && files.closes == before_close &&
              services.take_pending_signal_sema() == 14u,
          "retail sceFsInit version RPC completes with title's ABI tag");
  memory.write32(kResult + 4u, 0xC0FFEE02u);
  require(file_call(0xFFu, 8u, 1u, 8u) &&
              memory.read32(kResult) == kNfsmwLibkernl2800 &&
              memory.read32(kResult + 4u) == 0u &&
              services.last_rpc_call().send_size == 8u &&
              services.last_rpc_call().recv_size == 8u &&
              services.last_rpc_call().reject_flags == 0u &&
              services.take_pending_signal_sema() == 14u,
          "r14 Vita RPC with eight-byte request and reply negotiates version");

  services.reset();
  require(services.fileio_request_count() == 0u && files.closes == 1,
          "service reset clears FILEIO diagnostics without double-closing");
  std::puts("Native IOP service handshake tests passed.");
  return 0;
}
