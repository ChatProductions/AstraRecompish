#include "native_host_file_backend.hpp"

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
  cleanup();
  fs::remove_all(outside, ec);
  std::puts("host FILEIO backend: all synthetic-file checks passed");
  return 0;
}
