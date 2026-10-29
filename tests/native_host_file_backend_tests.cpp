#include "native_host_file_backend.hpp"
#include "ps2vita/memory.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
void require(bool pass, const char* what) {
  if (!pass) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    std::exit(1);
  }
}

void exercise_real_fileio_rpc(ps2vita::HostReadOnlyFileBackend& backend) {
  // End-to-end, synthetic SIFCMD -> SIFRPC -> FILEIO test. This deliberately
  // uses the r10 envelope, not an invented private game operation payload.
  ps2vita::Memory memory;
  ps2vita::NativeIopServices services;
  constexpr std::uint32_t kReceiver = ps2vita::NativeIopServices::kReceiveAddress;
  constexpr std::uint32_t kCmdData = 0x2000u;
  constexpr std::uint32_t kEeReceive = 0x2800u;
  constexpr std::uint32_t kSregs = 0x3000u;
  constexpr std::uint32_t kClient = 0x005D0DC0u;
  constexpr std::uint32_t kPacket = 0x5400u;
  constexpr std::uint32_t kReply = 0x5600u;
  constexpr std::uint32_t kData = 0x5800u;
  constexpr std::uint32_t kCompletion = 0x5900u;
  constexpr std::uint32_t kFileArgs = 0x001E4000u;
  const auto command = [&](std::uint32_t cid, std::uint32_t opt,
                           std::uint32_t arg, std::uint32_t bytes) {
    memory.iop_write32(kReceiver, bytes);
    memory.iop_write32(kReceiver + 4u, 0u);
    memory.iop_write32(kReceiver + 8u, cid);
    memory.iop_write32(kReceiver + 12u, opt);
    memory.iop_write32(kReceiver + 16u, arg);
  };
  services.boot(memory);
  services.set_file_backend(&backend);
  command(0x80000002u, 0u, kEeReceive, 20u);
  require(services.receive(memory, kReceiver, 20u, kCmdData),
          "real-file RPC test completes SIFCMD handshake");
  memory.write32(kCmdData + 28u, kSregs);
  command(0x80000002u, 1u, 0u, 16u);
  require(services.receive(memory, kReceiver, 16u, kCmdData),
          "real-file RPC test completes SIFRPC handshake");
  memory.write32(kClient + 8u, 14u);
  memory.write32(kPacket + 16u, 5u);
  command(0x80000009u, 0u, 0u, 64u);
  memory.iop_write32(kReceiver + 20u, kPacket);
  memory.iop_write32(kReceiver + 28u, kClient);
  memory.iop_write32(kReceiver + 32u, 0x80000001u);
  require(services.receive(memory, kReceiver, 64u, kCmdData) &&
              services.fileio_bind_count() == 1u &&
              memory.read32(kClient + 36u) == 0x001E3000u &&
              services.take_pending_signal_sema() == 14u,
          "real-file FILEIO bind uses observed server and semaphore");
  const auto call = [&](std::uint32_t fno, std::uint32_t send_size,
                        std::uint32_t reply_size = 8u) {
    memory.write32(kPacket + 16u, 5u);
    command(0x8000000Au, 0u, 0u, 64u);
    memory.iop_write32(kReceiver + 20u, kPacket);
    memory.iop_write32(kReceiver + 28u, kClient);
    memory.iop_write32(kReceiver + 32u, fno);
    memory.iop_write32(kReceiver + 36u, send_size);
    memory.iop_write32(kReceiver + 40u, kReply);
    memory.iop_write32(kReceiver + 44u, reply_size);
    memory.iop_write32(kReceiver + 48u, 1u); // r10 observed NOWAIT mode
    memory.iop_write32(kReceiver + 52u, 0x001E3000u);
    return services.receive(memory, kReceiver, 64u, kCmdData);
  };
  const auto open_path = [&](const char* name) {
    memory.iop_write32(kFileArgs, 1u); // FIO_O_RDONLY
    for (std::uint32_t i = 0; i < 256u; ++i)
      memory.iop_write8(kFileArgs + 4u + i, 0u);
    for (std::uint32_t i = 0; name[i] && i < 255u; ++i)
      memory.iop_write8(kFileArgs + 4u + i,
                        static_cast<std::uint8_t>(name[i]));
  };
  open_path("host:NFS\\TEST.BIN");
  memory.write32(kReply + 4u, 0xA5A5A5A5u);
  require(call(0u, 272u) && memory.read32(kReply) == 3u &&
              memory.read32(kReply + 4u) == 0u &&
              services.last_rpc_call().client == kClient &&
              services.last_rpc_call().server == 0x001E3000u &&
              services.last_rpc_call().sema == 14u &&
              services.last_rpc_call().mode == 1u &&
              services.last_rpc_call().recv_size == 8u &&
              services.last_rpc_call().reject_flags == 0u,
          "real host file opened via observed eight-byte FILEIO RPC");
  require(services.take_pending_signal_sema() == 14u,
          "real host FILEIO open signals semaphore 14");
  memory.iop_write32(kFileArgs, 3u);
  memory.iop_write32(kFileArgs + 4u, kData);
  memory.iop_write32(kFileArgs + 8u, 8u);
  memory.iop_write32(kFileArgs + 12u, kCompletion);
  memory.write32(kCompletion, 0xFFFFFFFFu);
  require(call(2u, 16u) && memory.read32(kReply) == 3u &&
              memory.read8(kData) == 'A' && memory.read8(kData + 2u) == 'C' &&
              memory.read32(kCompletion) == 0u &&
              services.fileio_total_read() == 3u,
          "real FILEIO RPC read transfers file bytes and reports true EOF length");
  services.take_pending_signal_sema();
  require(call(2u, 16u) && memory.read32(kReply) == 0u,
          "real FILEIO RPC reports EOF without inventing data");
  services.take_pending_signal_sema();
  memory.iop_write32(kFileArgs, 3u);
  memory.iop_write32(kFileArgs + 4u, 0u);
  memory.iop_write32(kFileArgs + 8u, 0u);
  require(call(4u, 12u) && memory.read32(kReply) == 0u,
          "real FILEIO RPC seek returns actual file offset");
  services.take_pending_signal_sema();
  memory.iop_write32(kFileArgs, 3u);
  require(call(1u, 4u) && memory.read32(kReply) == 0u,
          "real FILEIO RPC closes the opened descriptor");
  services.take_pending_signal_sema();
  open_path("host:NFS\\MISSING.BIN");
  require(call(0u, 272u) &&
              static_cast<std::int32_t>(memory.read32(kReply)) == -2,
          "missing file returns real ENOENT through FILEIO RPC");
  services.take_pending_signal_sema();
  const auto count = services.fileio_request_count();
  memory.write32(kReply, 0xC0FFEEu);
  require(!call(0u, 272u, 6u) &&
              services.fileio_request_count() == count &&
              memory.read32(kReply) == 0xC0FFEEu,
          "malformed six-byte FILEIO reply cannot execute or modify memory");
  require(!call(99u, 4u) &&
              services.last_rpc_call().reject_flags != 0u,
          "unknown FILEIO RPC operation is rejected without fabricated result");
}

int main() {
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  const auto root = fs::temp_directory_path() /
      ("astrarecomp-fileio-" + std::to_string(stamp));
  fs::create_directories(root / "NFS");
  fs::create_directories(root / "disc");
  const auto cleanup = [&] { std::error_code ec; fs::remove_all(root, ec); };
  {
    std::ofstream out(root / "NFS" / "TEST.BIN", std::ios::binary);
    out.write("ABC", 3);
  }
  {
    std::ofstream out(root / "disc" / "LEVEL.DAT", std::ios::binary);
    out.write("disc", 4);
  }
  ps2vita::HostReadOnlyFileBackend backend(root);
  require(backend.valid(), "existing file root is valid");
  require(!ps2vita::HostReadOnlyFileBackend(root / "missing").valid(),
          "missing root rejected");
  require(backend.open("host:NFS\\TEST.BIN", 0x201u) == -30,
          "write/create flags rejected");
  require(backend.open("host:NFS\\TEST.BIN", 0x1000u) == -22,
          "unknown flags rejected");
  require(backend.open("host:NFS\\MISSING.BIN", 1u) == -2,
          "missing file is ENOENT");
  require(backend.open("host:NFS\\..\\disc\\LEVEL.DAT", 1u) == -13,
          "parent traversal rejected");
  require(backend.open("host:NFS\\.\\TEST.BIN", 1u) == -13,
          "dot traversal rejected");
  require(backend.open("host:OTHER\\TEST.BIN", 1u) == -2,
          "unsupported device rejected");
  require(backend.open("cdrom0:\\LEVEL.DAT;2", 1u) == -13,
          "unsupported ISO revision rejected");
  const int fd = backend.open("HoSt:nFs\\TEST.BIN", 1u);
  require(fd > 0, "read-only NFS file opened");
  std::array<char, 8> buf{};
  require(backend.read(fd, buf.data(), 2u) == 2 &&
          buf[0] == 'A' && buf[1] == 'B', "actual file bytes returned");
  require(backend.seek(fd, 0, 0u) == 0, "seek to start");
  require(backend.read(fd, buf.data(), 8u) == 3 &&
          buf[2] == 'C', "bounded read to actual EOF");
  require(backend.read(fd, buf.data(), 8u) == 0, "EOF returns zero");
  require(backend.seek(fd, -1, 2u) == 2, "seek from end");
  require(backend.seek(fd, 0, 3u) == -22, "invalid whence rejected");
  require(backend.close(fd) == 0, "close");
  require(backend.read(fd, buf.data(), 1u) == -9, "closed descriptor rejected");
  require(backend.read(-1, buf.data(), 1u) == -9, "invalid descriptor rejected");
  const int cap_fd = backend.open("host:NFS\\TEST.BIN", 1u);
  require(cap_fd > 0 && backend.read(cap_fd, buf.data(), 1024u * 1024u + 1u) == -22,
          "oversized backend read rejected before buffer access");
  require(backend.close(cap_fd) == 0, "close after oversized request");
  const int disc = backend.open("cdrom0:\\LEVEL.DAT;1", 1u);
  require(disc > 0 && backend.read(disc, buf.data(), 4u) == 4 &&
          buf[0] == 'd' && buf[3] == 'c', "ISO revision and disc root");
  require(backend.close(disc) == 0, "close disc");

  // Verify containment against an existing, readable file outside the
  // authorized root; a missing target would not exercise the escape check.
  const auto outside = fs::path(root.string() + "-outside");
  fs::create_directories(outside);
  {
    std::ofstream out(outside / "SECRET.BIN", std::ios::binary);
    out.write("secret", 6);
  }
  std::error_code ec;
  fs::create_directory_symlink(outside, root / "NFS" / "ESCAPE", ec);
  if (!ec)
    require(backend.open("host:NFS\\ESCAPE\\SECRET.BIN", 1u) == -13,
            "existing external file blocked through symlink escape");
  exercise_real_fileio_rpc(backend);
  cleanup();
  fs::remove_all(outside, ec);
  std::puts("host FILEIO backend: all synthetic-file checks passed");
  return 0;
}
