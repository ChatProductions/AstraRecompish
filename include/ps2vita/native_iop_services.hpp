#pragma once

#include <cstddef>
#include <cstdint>
#include <array>

namespace ps2vita {

class Memory;

// Opt-in HLE counterpart for a bare-ELF EE title. This models the IOP-side
// SIFCMD receive state and the RPC readiness handshake; it never runs an IOP
// CPU and does not claim to provide arbitrary RPC/file/audio servers.
class NativeIopServices {
public:
  static constexpr std::uint32_t kReceiveAddress = 0x001F0000u;
  enum class Phase : std::uint8_t {
    Offline, AwaitingInit, CmdConnected, RpcReady, RebootRequired
  };

  void reset();
  void boot(Memory& memory);
  // A descriptor was copied into IOP RAM using EE SIF1 DMA semantics.
  // Returns false when a recognized command is malformed or unsupported.
  bool receive(Memory& memory, std::uint32_t destination,
               std::uint32_t transfer_size, std::uint32_t ee_cmd_data);

  Phase phase() const { return phase_; }
  std::uint32_t ee_receive_address() const { return ee_receive_address_; }
  std::uint32_t command_count() const { return command_count_; }
  std::uint32_t unsupported_cid() const { return unsupported_cid_; }
  std::uint32_t unsupported_rpc_sid() const { return unsupported_rpc_sid_; }
  std::uint32_t cdvd_init_calls() const { return cdvd_init_calls_; }
  std::uint32_t reboot_count() const { return reboot_count_; }
  std::uint32_t reboot_mode() const { return reboot_mode_; }
  const char* reboot_arg() const { return reboot_arg_.data(); }
  // Returns the EE semaphore to signal after an IOP RPC_END notification.
  // 0 means an asynchronous request or no pending response.
  std::uint32_t take_pending_signal_sema();

private:
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
};

} // namespace ps2vita
