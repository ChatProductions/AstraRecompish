#pragma once

#include "ps2vita/cpu.hpp"
#include "ps2vita/elf_loader.hpp"
#include "ps2vita/gif.hpp"
#include "ps2vita/gs.hpp"
#include "ps2vita/iop_cpu.hpp"
#include "ps2vita/vif.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace ps2vita {

class Emulator {
public:
  Emulator();
  ElfLoadResult load_elf(const void* data, std::size_t size);
  bool load_bios(const void* data, std::size_t size);
  bool boot_bios();
  StopReason run_slice(std::uint32_t instructions);
  // Minimal PS2 kernel startup services for an explicitly loaded bare ELF.
  // Opt-in; never intercept BIOS exception handling or other syscalls.
  void enable_bare_elf_startup_syscalls(bool enabled) {
    bare_elf_startup_syscalls_ = enabled;
  }
  void service_graphics();
  void reset();

  Memory& memory() { return memory_; }
  const Memory& memory() const { return memory_; }
  Cpu& cpu() { return cpu_; }
  const Cpu& cpu() const { return cpu_; }
  IopCpu& iop() { return iop_; }
  const IopCpu& iop() const { return iop_; }
  Gs& gs() { return gs_; }
  const Gs& gs() const { return gs_; }
  const Gif& gif() const { return gif_; }
  void set_gif_submission_observer(Gif::SubmissionObserver observer) {
    gif_.set_submission_observer(std::move(observer));
  }
  void set_gif_triangle_observer(Gif::TriangleObserver observer) {
    gif_.set_triangle_observer(std::move(observer));
  }
  void enable_triangle_trace(bool enabled) { gif_.enable_triangle_trace(enabled); }
  void capture_sprite_framebuffer_at(std::uint64_t sequence) {
    gif_.capture_sprite_framebuffer_at(sequence);
  }
  void enable_vif_packet_capture(bool enabled) { vif1_.enable_packet_capture(enabled); }
  void enable_vif_command_census(bool enabled) { vif1_.enable_command_census(enabled); }
  const Vif1& vif1() const { return vif1_; }
  const ElfLoadResult& image() const { return image_; }
  bool ready() const { return ready_; }

private:
  bool handle_bare_elf_startup_syscall();
  void initialize_kernel_threads();
  bool has_ready_thread() const;
  bool switch_to_ready_thread();
  Memory memory_;
  Cpu cpu_;
  IopCpu iop_;
  Gs gs_;
  Gif gif_;
  Vif1 vif1_;
  ElfLoadResult image_{};
  std::uint32_t ee_cycles_until_iop_ = 8u;
  bool ready_ = false;
  bool bare_elf_startup_syscalls_ = false;
  std::uint32_t startup_stack_top_ = 0;
  std::uint32_t startup_heap_base_ = 0;
  std::uint32_t startup_heap_end_ = 0;
  // Bounded EE kernel subset for one active bare-ELF thread.
  // An unsatisfied WaitSema must remain a visible stop until scheduling exists.
  struct KernelSemaphore {
    bool in_use = false;
    std::int32_t count = 0;
    std::int32_t maximum = 0;
  };
  std::array<KernelSemaphore, 64> kernel_semaphores_{};
  // EE thread objects: 0=free, 1=run, 2=ready, 4=wait, 16=dormant.
  // Slots are bounded; slot 0 is the primary ELF thread (ID 1).
  struct KernelThread {
    std::uint8_t status = 0;
    std::uint8_t wait_type = 0; // 1=sleep, 2=semaphore
    std::uint16_t priority = 0;
    std::uint32_t wait_id = 0;
    std::uint32_t wakeups = 0;
    std::uint32_t entry = 0;
    std::uint32_t stack = 0;
    std::uint32_t stack_size = 0;
    std::uint32_t gp = 0;
    std::uint32_t initial_priority = 0;
    std::uint32_t attr = 0;
    std::uint32_t option = 0;
    Cpu::ThreadContext context{};
  };
  std::array<KernelThread, 32> kernel_threads_{};
  std::size_t current_thread_index_ = 0;
  std::uint32_t thread_quantum_remaining_ = 256u;
  // Kernel interrupt-handler registrations. Delivery needs the separately
  // modeled EE timer/interrupt scheduler; registration alone never fires.
  struct KernelInterruptHandler {
    std::uint32_t address = 0;
    std::uint32_t cause = 0;
    std::uint32_t gp = 0;
    std::uint32_t argument = 0;
    std::uint32_t position = 0;
  };
  std::array<KernelInterruptHandler, 32> intc_handlers_{};
  std::array<KernelInterruptHandler, 32> dmac_handlers_{};
  // SetSyscall entries live in guest-visible EE kernel RAM.
  // A bounded return stack preserves RA across guest callbacks.
  struct GuestSyscallReturn {
    std::uint32_t continuation = 0;
    std::uint64_t original_ra = 0;
  };
  std::array<GuestSyscallReturn, 4> guest_syscall_returns_{};
  std::size_t guest_syscall_depth_ = 0;
};

} // namespace ps2vita
