#include "ps2vita/emulator.hpp"

#include <algorithm>

namespace ps2vita {
namespace {

void set_bios_reset_state(CpuState& state) {
  state.cop0[12] = 0x70400004u; // CU0/1/2 + BEV + reset-time ERL.
  state.cop0[16] = 0x00000440u; // R5900 Config reset value.
  state.cop0[15] = 0x00002E20u; // R5900 processor revision.
  state.fcr[0] = 0x00002E30u;   // EE FPU revision.
  state.fcr[31] = 0x01000001u;  // EE FPU control/status reset value.
}

} // namespace

Emulator::Emulator()
    : memory_(), cpu_(memory_), iop_(memory_), gif_(gs_), vif1_(memory_) {}

ElfLoadResult Emulator::load_elf(const void* data, std::size_t size) {
  memory_.clear();
  gif_.reset();
  vif1_.reset();
  image_ = load_elf32(data, size, memory_);
  // Native title code is never inherited by the next ELF accidentally.
  cpu_.set_startup_aot_package(nullptr);
  ee_cycles_until_iop_ = 8u;
  bare_elf_startup_syscalls_ = false;
  startup_stack_top_ = startup_heap_base_ = startup_heap_end_ = 0u;
  kernel_semaphores_ = {};
  guest_syscall_handlers_ = {};
  guest_syscall_returns_ = {};
  guest_syscall_depth_ = 0u;
  ready_ = image_.ok;
  if (ready_) { cpu_.reset(image_.entry); cpu_.set_exception_mode(false); }
  return image_;
}

bool Emulator::load_bios(const void* data, std::size_t size) {
  return memory_.load_bios(data, size);
}

bool Emulator::boot_bios() {
  if (!memory_.has_bios()) return false;
  cpu_.set_startup_aot_package(nullptr);
  memory_.clear();
  image_ = {};
  ready_ = true;
  cpu_.reset(0xBFC00000u);
  iop_.reset();
  ee_cycles_until_iop_ = 8u;
  bare_elf_startup_syscalls_ = false;
  startup_stack_top_ = startup_heap_base_ = startup_heap_end_ = 0u;
  kernel_semaphores_ = {};
  guest_syscall_handlers_ = {};
  guest_syscall_returns_ = {};
  guest_syscall_depth_ = 0u;
  gif_.reset();
  vif1_.reset();
  set_bios_reset_state(cpu_.state());
  cpu_.set_exception_mode(true);
  return true;
}

StopReason Emulator::run_slice(std::uint32_t instructions) {
  if (!ready_) return StopReason::Halted;
  // Drain already completed transfers before executing another EE instruction.
  // This is still the functional packet model, not cycle-interleaved VU/GIF.
  service_graphics();
  std::uint32_t remaining = instructions;
  while (remaining != 0u) {
    auto budget = std::min(remaining, memory_.cycles_until_next_event());
    if (!image_.ok) budget = std::min(budget, ee_cycles_until_iop_);
    // A guest syscall handler returns through an EE instruction sequence.
    // Observe each instruction while one runs, so $ra is restored before
    // the original syscall wrapper executes its next instruction.
    if (guest_syscall_depth_ != 0u) budget = std::min(budget, 1u);
    const auto cycles_before = cpu_.state().cycles;
    const auto result = cpu_.run(budget);
    const auto executed = static_cast<std::uint32_t>(
        cpu_.state().cycles - cycles_before);
    remaining -= executed;
    if (!image_.ok) {
      ee_cycles_until_iop_ -= executed;
      if (ee_cycles_until_iop_ == 0u) {
        iop_.step();
        ee_cycles_until_iop_ = 8u;
      }
    }
    // DMA can retire inside a requested host slice. Consume its packet now,
    // before EE polling code observes completion and reads VU memory. A fixed
    // host quantum alone can delay this visibility by several instructions.
    service_graphics();
    if (guest_syscall_depth_ != 0u &&
        cpu_.state().pc ==
            guest_syscall_returns_[guest_syscall_depth_ - 1u].continuation) {
      const auto frame = guest_syscall_returns_[--guest_syscall_depth_];
      cpu_.state().gpr[31] = frame.original_ra;
    }
    if (result == StopReason::Syscall && image_.ok &&
        bare_elf_startup_syscalls_ && handle_bare_elf_startup_syscall()) {
      // The syscall was already counted by Cpu::step; continue with the
      // remaining guest-instruction budget, without counting it twice.
      continue;
    }
    if (result != StopReason::StepLimit) return result;
    if (executed == 0u) return StopReason::Halted;
  }
  return StopReason::StepLimit;
}

bool Emulator::handle_bare_elf_startup_syscall() {
  auto& state = cpu_.state();
  const auto number = static_cast<std::uint32_t>(state.gpr[3]); // $v1
  const auto arg0 = static_cast<std::uint32_t>(state.gpr[4]);
  const auto arg1 = static_cast<std::uint32_t>(state.gpr[5]);
  const auto arg2 = static_cast<std::uint32_t>(state.gpr[6]);
  // User-installed SetSyscall handlers take precedence over built-in EE
  // services, as they do when the BIOS dispatch table has been patched.
  if (number < guest_syscall_handlers_.size() &&
      guest_syscall_handlers_[number] != 0u) {
    if (guest_syscall_depth_ == guest_syscall_returns_.size()) return false;
    const auto handler = guest_syscall_handlers_[number];
    if (!memory_.instruction_reads_side_effect_free(handler, 4u)) return false;
    const auto continuation = state.pc + 4u;
    const auto saved_ra = state.gpr[31];
    if (!cpu_.retire_trapped_syscall(0u, false)) return false;
    guest_syscall_returns_[guest_syscall_depth_++] = {continuation, saved_ra};
    state.gpr[31] = continuation;
    state.pc = handler;
    return true;
  }
  switch (number) {
  case 0x74u: { // SetSyscall(number, guest_handler); returns void.
    if (arg0 >= guest_syscall_handlers_.size() ||
        (arg1 != 0u && ((arg1 & 3u) != 0u ||
         !memory_.instruction_reads_side_effect_free(arg1, 4u))))
      return false;
    if (!cpu_.retire_trapped_syscall(0u, false)) return false;
    guest_syscall_handlers_[arg0] = arg1;
    return true;
  }
  case 0x3Cu: { // SetupThread(gp, stack_base, stack_size, args, root_func)
    // In the common explicit-stack form, the kernel returns stack+size.
    // Refuse sentinel/invalid ranges rather than invent a kernel layout.
    const std::uint64_t top = std::uint64_t{arg1} + arg2;
    if (arg1 == 0xFFFFFFFFu || arg2 == 0u || top > Memory::kRamSize ||
        (top & 15u) != 0u || !memory_.valid(arg1, arg2)) return false;
    if (!cpu_.retire_trapped_syscall(top)) return false;
    startup_stack_top_ = static_cast<std::uint32_t>(top);
    return true;
  }
  case 0x3Du: { // SetupHeap(heap_base, heap_size)
    // SDKs use -1 to request the remainder of the main thread's stack
    // boundary. Do not treat an arbitrary huge value as a valid allocation.
    const std::uint64_t end = arg1 == 0xFFFFFFFFu
        ? startup_stack_top_ : std::uint64_t{arg0} + arg1;
    if (end == 0u || end > Memory::kRamSize || arg0 >= end ||
        !memory_.valid(arg0, static_cast<std::size_t>(end - arg0))) return false;
    if (!cpu_.retire_trapped_syscall(end)) return false;
    startup_heap_base_ = arg0;
    startup_heap_end_ = static_cast<std::uint32_t>(end);
    return true;
  }
  case 0x40u: { // CreateSema(ee_sema_t *): six little-endian 32-bit fields.
    // Input layout: count, max_count, init_count, wait_threads, attr, option.
    constexpr std::size_t kDescriptorBytes = 24u;
    if ((arg0 & 3u) != 0u || !memory_.valid(arg0, kDescriptorBytes))
      return false;
    const auto maximum = static_cast<std::int32_t>(memory_.read32(arg0 + 4u));
    const auto initial = static_cast<std::int32_t>(memory_.read32(arg0 + 8u));
    if (maximum <= 0 || initial < 0 || initial > maximum) return false;
    for (std::size_t i = 0; i < kernel_semaphores_.size(); ++i) {
      if (kernel_semaphores_[i].in_use) continue;
      if (!cpu_.retire_trapped_syscall(i + 1u)) return false;
      kernel_semaphores_[i] = {true, initial, maximum};
      return true;
    }
    // Allocation exhausted; do not manufacture duplicate IDs.
    return cpu_.retire_trapped_syscall(UINT64_MAX);
  }
  case 0x41u: // DeleteSema(id)
  case 0x42u: // SignalSema(id)
  case 0x44u: // WaitSema(id)
  case 0x45u: { // PollSema(id)
    if (arg0 == 0u || arg0 > kernel_semaphores_.size() ||
        !kernel_semaphores_[arg0 - 1u].in_use)
      return cpu_.retire_trapped_syscall(UINT64_MAX);
    auto& sema = kernel_semaphores_[arg0 - 1u];
    if (number == 0x41u) {
      if (!cpu_.retire_trapped_syscall(0u)) return false;
      sema = {};
      return true;
    }
    if (number == 0x42u) {
      if (sema.count >= sema.maximum)
        return cpu_.retire_trapped_syscall(UINT64_MAX);
      if (!cpu_.retire_trapped_syscall(0u)) return false;
      ++sema.count;
      return true;
    }
    if (sema.count == 0) {
      // PollSema is nonblocking; WaitSema is not. Until EE thread switching
      // exists, trapping is safer than pretending an empty wait succeeded.
      return number == 0x45u
          ? cpu_.retire_trapped_syscall(UINT64_MAX) : false;
    }
    if (!cpu_.retire_trapped_syscall(0u)) return false;
    --sema.count;
    return true;
  }
  case 0x64u: // FlushCache(mode)
    // EE reads and writes in this runtime are already coherent: there is no
    // separately modeled dirty D-cache or stale I-cache to synchronize.
    // This is a semantic no-op, not a blanket skip for unknown syscalls.
    return cpu_.retire_trapped_syscall(0u, false);
  default:
    return false; // Unknown kernel calls must remain visible in fault.txt.
  }
}

void Emulator::service_graphics() {
  // Empty pops do not advance partial transfers. Only completed queues matter,
  // including PATH1 output that has not been moved into VIF's DIRECT queue.
  if (!memory_.has_completed_graphics_packets() && !vif1_.has_gif_packet()) return;
  std::vector<std::uint8_t> packet;
  while (memory_.pop_gif_packet(packet))
    gif_.submit(packet.data(), packet.size());
  while (memory_.pop_vif1_packet(packet))
    vif1_.submit(packet.data(), packet.size());
  while (vif1_.pop_gif_packet(packet))
    gif_.submit(packet.data(), packet.size());
}

void Emulator::reset() {
  ee_cycles_until_iop_ = 8u;
  startup_stack_top_ = startup_heap_base_ = startup_heap_end_ = 0u;
  kernel_semaphores_ = {};
  guest_syscall_handlers_ = {};
  guest_syscall_returns_ = {};
  guest_syscall_depth_ = 0u;
  gif_.reset();
  vif1_.reset();
  if (image_.ok) { cpu_.reset(image_.entry); cpu_.set_exception_mode(false); }
  else if (memory_.has_bios()) {
    cpu_.reset(0xBFC00000u);
    iop_.reset();
    set_bios_reset_state(cpu_.state());
    cpu_.set_exception_mode(true);
  }
}

} // namespace ps2vita
