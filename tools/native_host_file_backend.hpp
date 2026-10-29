#pragma once

// Host-only, explicitly configured, read-only FILEIO backend. No private
// title assets are compiled into the project or read during CI.
#include "ps2vita/native_iop_services.hpp"

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <string>
#include <system_error>

namespace ps2vita {
class HostReadOnlyFileBackend final : public NativeFileBackend {
public:
  explicit HostReadOnlyFileBackend(const std::filesystem::path& root) {
    std::error_code ec;
    root_ = std::filesystem::canonical(root, ec);
    valid_ = !ec && std::filesystem::is_directory(root_, ec) && !ec;
  }

  ~HostReadOnlyFileBackend() override {
    for (auto& file : files_) {
      if (file) std::fclose(file);
      file = nullptr;
    }
  }

  bool valid() const { return valid_; }

  int open(const char* ps2_path, std::uint32_t flags) override {
    if (!valid_ || !ps2_path) return -22; // EINVAL
    if ((flags & 3u) > 1u || (flags & (0x100u | 0x200u | 0x400u | 0x800u)))
      return -30; // EROFS
    if ((flags & ~1u) != 0u) return -22; // Unknown flags are not guessed.
    std::string raw;
    for (std::size_t i = 0; i < 256u && ps2_path[i]; ++i)
      raw.push_back(ps2_path[i]);
    if (raw.empty() || raw.size() == 256u) return -22;

    std::string device;
    std::string relative;
    if (prefix(raw, "host:NFS/") || prefix(raw, "host:NFS\\"))
      device = "NFS", relative = raw.substr(9u);
    else if (prefix(raw, "cdrom0:"))
      device = "disc", relative = raw.substr(7u);
    else if (prefix(raw, "cdrom:"))
      device = "disc", relative = raw.substr(6u);
    else return -2; // No implicit host filesystem or disc mount.

    for (auto& ch : relative) {
      if (ch == '\\') ch = '/';
      if (static_cast<unsigned char>(ch) < 0x20u || ch == ':')
        return -13;
    }
    // cdrom0:\\NAME;1 and host:NFS\\NAME are absolute within their
    // explicitly selected device root, not absolute host paths.
    while (!relative.empty() && relative.front() == '/')
      relative.erase(relative.begin());
    if (relative.size() > 2u &&
        relative.compare(relative.size() - 2u, 2u, ";1") == 0)
      relative.resize(relative.size() - 2u);
    if (relative.empty() || relative.find(';') != std::string::npos)
      return -13;
    std::filesystem::path subpath;
    std::size_t at = 0u;
    while (at < relative.size()) {
      const auto slash = relative.find('/', at);
      const auto end = slash == std::string::npos ? relative.size() : slash;
      const auto component = relative.substr(at, end - at);
      if (component.empty() || component == "." || component == "..")
        return -13; // No traversal or ambiguous path normalization.
      subpath /= component;
      at = end + 1u;
    }

    std::error_code ec;
    const auto device_root = std::filesystem::canonical(root_ / device, ec);
    if (ec || !inside(root_, device_root)) return -2;
    const auto target = std::filesystem::canonical(device_root / subpath, ec);
    if (ec) return -2;
    if (!inside(device_root, target)) return -13; // Symlink escape.
    if (!std::filesystem::is_regular_file(target, ec) || ec) return -2;

    std::size_t slot = files_.size();
    for (std::size_t i = 0; i < files_.size(); ++i)
      if (!files_[i]) { slot = i; break; }
    if (slot == files_.size()) return -24; // EMFILE
    errno = 0;
    std::FILE* stream = std::fopen(target.string().c_str(), "rb");
    if (!stream) return errno == EACCES ? -13 : errno == ENOENT ? -2 : -5;
    files_[slot] = stream;
    return static_cast<int>(slot + 1u);
  }

  int close(int handle) override {
    const auto index = handle - 1;
    if (index < 0 || static_cast<std::size_t>(index) >= files_.size() ||
        !files_[index]) return -9;
    auto* stream = files_[index];
    files_[index] = nullptr;
    return std::fclose(stream) == 0 ? 0 : -5;
  }

  int read(int handle, void* buffer, std::uint32_t size) override {
    const auto index = handle - 1;
    if (index < 0 || static_cast<std::size_t>(index) >= files_.size() ||
        !files_[index]) return -9;
    if (size > 1024u * 1024u || (size && !buffer)) return -22;
    if (!size) return 0;
    const auto count = std::fread(buffer, 1u, size, files_[index]);
    if (std::ferror(files_[index])) return -5;
    return static_cast<int>(count); // Core caps FILEIO reads at 1 MiB.
  }

  int seek(int handle, std::int32_t offset, std::uint32_t whence) override {
    const auto index = handle - 1;
    if (index < 0 || static_cast<std::size_t>(index) >= files_.size() ||
        !files_[index]) return -9;
    if (whence > 2u) return -22;
    if (std::fseek(files_[index], static_cast<long>(offset),
                   static_cast<int>(whence)) != 0) return -22;
    const auto position = std::ftell(files_[index]);
    if (position < 0) return -5;
    if (static_cast<std::uint64_t>(position) >
        static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max()))
      return -75; // EOVERFLOW; no truncated file position.
    return static_cast<int>(position);
  }

private:
  static bool prefix(const std::string& value, const char* expected) {
    for (std::size_t i = 0; expected[i]; ++i) {
      if (i >= value.size()) return false;
      auto ch = value[i];
      auto ex = expected[i];
      if (ch >= 'a' && ch <= 'z') ch = static_cast<char>(ch - 'a' + 'A');
      if (ex >= 'a' && ex <= 'z') ex = static_cast<char>(ex - 'a' + 'A');
      if (ch != ex) return false;
    }
    return true;
  }

  static bool inside(const std::filesystem::path& root,
                     const std::filesystem::path& candidate) {
    auto left = root.begin();
    auto right = candidate.begin();
    for (; left != root.end(); ++left, ++right)
      if (right == candidate.end() || *left != *right) return false;
    return right != candidate.end(); // A file must be below its device root.
  }

  std::filesystem::path root_;
  bool valid_ = false;
  std::array<std::FILE*, 16> files_{};
};
} // namespace ps2vita
