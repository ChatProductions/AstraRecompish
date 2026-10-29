#pragma once

#include <cstddef>
#include <cstdint>
#include <array>

namespace ps2vita {

class Memory;

// Platform-owned file access. The core knows PS2 FILEIO protocol semantics,
// not Vita paths or host stdio. Operations return an actual descriptor,
// byte count, position, or a negative PS2-style errno. No implicit writes.
class NativeFileBackend {
public:
  virtual ~NativeFileBackend() = default;
  virtual int open(const char* ps2_path, std::uint32_t flags) = 0;
  virtual int close(int handle) = 0;
  virtual int read(int handle, void* buffer, std::uint32_t size) = 0;
  virtual int seek(int handle, std::int32_t offset, std::uint32_t whence) = 0;
};

// Opt-in HLE counterpart for a bare-ELF EE title. This models the IOP-side
// SIFCMD receive state and the RPC readiness handshake; it never runs an IOP
// CPU and does not claim to provide arbitrary RPC/file/audio servers.
class NativeIopServices {
public:
  static constexpr std::uint32_t kReceiveAddress = 0x001F0000u;
  enum class Phase : std::uint8_t {
    Offline, AwaitingInit, CmdConnected, RpcReady, RebootRequired,
    RebootPending
  };

  // Last SIF_RPC_CALL packet metadata, including rejected calls. No guest bytes.
  struct RpcCallTrace {
    std::uint32_t packet_size = 0;
    std::uint32_t client = 0;
    std::uint32_t bound_fileio_client = 0;
    std::uint32_t server = 0;
    std::uint32_t function = 0xFFFFFFFFu;
    std::uint32_t send_size = 0;
    std::uint32_t recvbuf = 0;
    std::uint32_t recv_size = 0;
    std::uint32_t mode = 0;
    std::uint32_t original_packet = 0;
    std::uint32_t sema = 0xFFFFFFFFu;
    std::uint32_t fileio_candidate = 0;
    // Metadata only: the preceding SIF1 DMA descriptor, not payload bytes.
    // exact=1 means the transfer size equals the RPC send size. This is
    // not proof that an unknown retail FILEIO ABI is supported.
    std::uint32_t fileio_dma_source = 0;
    std::uint32_t fileio_dma_size = 0;
    std::uint32_t fileio_dma_exact = 0;
    // Read-only structural observations of an *unverified* 1048-byte OPEN
    // envelope. These are offsets/lengths, never guest path bytes, hashes,
    // flags, or proof of a supported ABI. 0xFFFFFFFF means no candidate.
    std::uint32_t fileio_open_pathlike_count = 0;
    std::uint32_t fileio_open_first_pathlike_offset = 0xFFFFFFFFu;
    std::uint32_t fileio_open_first_pathlike_length = 0;
    // Rejection bitmask: 1=client, 2=server, 4=recv size, 8=mode,
    // 16=original packet, 32=semaphore, 64=client pointer,
    // 128=FILEIO operation/args, 256=short packet, 512=wrong phase.
    std::uint32_t reject_flags = 0;
  };
  const RpcCallTrace& last_rpc_call() const { return last_rpc_call_; }

  void reset();
  // Replacing a backend must close handles on their *owning* backend before
  // the pointer changes. A previous FILEIO bind cannot remain usable after
  // detaching or replacing its filesystem service.
  void set_file_backend(NativeFileBackend* backend) {
    if (file_backend_ == backend) return;
    close_all_files();
    file_backend_ = backend;
    bound_fileio_client_ = 0u;
    pending_signal_sema_ = 0u;
    fileio_dma_source_ = fileio_dma_size_ = 0u;
    fileio_dma_pending_ = false;
  }
  void boot(Memory& memory);
  // Four-byte FILEIO response. Only set a nonzero value when the IOP
  // module's response and EE comparison protocol are independently verified.
  // An EE libkernl tag alone does not establish the IOP's reply.
  // Zero rejects 0xFF rather than inventing a firmware response.
  void set_fileio_compatibility_version(std::uint32_t version) {
    fileio_compatibility_version_ = version;
  }
  std::uint32_t fileio_compatibility_version() const {
    return fileio_compatibility_version_;
  }
  void permit_reboot_with_verified_image(bool permitted) {
    verified_ioprp_ = permitted;
  }
  // An IOP image was accepted by the ROMDIR parser and the IOP startup delay
  // has elapsed. Starts only the currently implemented native HLE services.
  bool complete_reboot(Memory& memory);
  // A descriptor was copied into IOP RAM using EE SIF1 DMA semantics.
  // Returns false when a recognized command is malformed or unsupported.
  bool receive(Memory& memory, std::uint32_t destination,
               std::uint32_t transfer_size, std::uint32_t ee_cmd_data,
               std::uint32_t ee_source = 0u);

  Phase phase() const { return phase_; }
  std::uint32_t ee_receive_address() const { return ee_receive_address_; }
  std::uint32_t command_count() const { return command_count_; }
  std::uint32_t unsupported_cid() const { return unsupported_cid_; }
  std::uint32_t unsupported_rpc_sid() const { return unsupported_rpc_sid_; }
  std::uint32_t cdvd_init_calls() const { return cdvd_init_calls_; }
  std::uint32_t fileio_bind_count() const { return fileio_bind_count_; }
  std::uint32_t fileio_request_count() const { return fileio_request_count_; }
  std::uint32_t fileio_last_function() const { return fileio_last_function_; }
  std::int32_t fileio_last_result() const { return fileio_last_result_; }
  std::uint32_t fileio_total_read() const { return fileio_total_read_; }
  const char* fileio_last_path() const { return fileio_last_path_.data(); }
  std::uint32_t reboot_count() const { return reboot_count_; }
  std::uint32_t reboot_completed() const { return reboot_completed_; }
  std::uint32_t reboot_mode() const { return reboot_mode_; }
  const char* reboot_arg() const { return reboot_arg_.data(); }
  // Returns the EE semaphore to signal after an IOP RPC_END notification.
  // 0 means an asynchronous request or no pending response.
  std::uint32_t take_pending_signal_sema();

private:
  bool execute_fileio(Memory& memory, std::uint32_t function,
                      std::uint32_t send_size, std::uint32_t recvbuf,
                      std::uint32_t recv_size);
  struct OpenFile {
    int backend_handle = -1;
    bool in_use = false;
  };
  void close_all_files();
  std::array<OpenFile, 16> open_files_{};
  NativeFileBackend* file_backend_ = nullptr;
  std::uint32_t bound_fileio_client_ = 0;
  std::uint32_t fileio_bind_count_ = 0;
  std::uint32_t fileio_request_count_ = 0;
  std::uint32_t fileio_last_function_ = 0xFFFFFFFFu;
  std::int32_t fileio_last_result_ = 0;
  std::uint32_t fileio_compatibility_version_ = 0u;
  std::uint32_t fileio_total_read_ = 0;
  std::array<char, 257> fileio_last_path_{};
  RpcCallTrace last_rpc_call_{};
  std::uint32_t fileio_dma_source_ = 0;
  std::uint32_t fileio_dma_size_ = 0;
  bool fileio_dma_pending_ = false;
  Phase phase_ = Phase::Offline;
  std::uint32_t ee_receive_address_ = 0;
  std::uint32_t command_count_ = 0;
  std::uint32_t unsupported_cid_ = 0;
  std::uint32_t unsupported_rpc_sid_ = 0;
  std::uint32_t cdvd_init_calls_ = 0;
  std::uint32_t pending_signal_sema_ = 0;
  std::uint32_t bound_cdvd_client_ = 0;
  std::uint32_t reboot_count_ = 0;
  std::uint32_t reboot_mode_ = 0;
  std::array<char, 81> reboot_arg_{};
  bool verified_ioprp_ = false;
  std::uint32_t reboot_completed_ = 0;
};

} // namespace ps2vita
