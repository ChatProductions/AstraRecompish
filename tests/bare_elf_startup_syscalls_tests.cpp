#include "ps2vita/emulator.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

void require(bool value, const char* description) {
  if (!value) {
    std::fprintf(stderr, "FAIL: %s\n", description);
    std::exit(EXIT_FAILURE);
  }
}

void put16(std::vector<std::uint8_t>& image, std::size_t offset,
           std::uint16_t value) {
  image[offset] = static_cast<std::uint8_t>(value);
  image[offset + 1u] = static_cast<std::uint8_t>(value >> 8);
}

void put32(std::vector<std::uint8_t>& image, std::size_t offset,
           std::uint32_t value) {
  put16(image, offset, static_cast<std::uint16_t>(value));
  put16(image, offset + 2u, static_cast<std::uint16_t>(value >> 16));
}

std::vector<std::uint8_t> syscall_elf() {
  std::vector<std::uint8_t> image(0x110u, 0);
  image[0] = 0x7F; image[1] = 'E'; image[2] = 'L'; image[3] = 'F';
  image[4] = 1; image[5] = 1; image[6] = 1;
  put16(image, 16, 2);  // ET_EXEC
  put16(image, 18, 8);  // EM_MIPS
  put32(image, 20, 1);
  put32(image, 24, 0x1000u);
  put32(image, 28, 52);
  put16(image, 40, 52);
  put16(image, 42, 32);
  put16(image, 44, 1);
  put32(image, 52, 1);          // PT_LOAD
  put32(image, 56, 0x100u);   // Offset
  put32(image, 60, 0x1000u);  // Virtual address
  put32(image, 68, 0x10u);    // File size
  put32(image, 72, 0x20u);    // Memory size
  put32(image, 76, 5);
  put32(image, 80, 16);
  put32(image, 0x100u, 0x0000000Cu); // SetupThread (from $v1)
  put32(image, 0x104u, 0x0000000Cu); // SetupHeap (from $v1)
  put32(image, 0x108u, 0x0000000Cu); // FlushCache (from $v1)
  put32(image, 0x10Cu, 0x0000000Du); // BREAK
  return image;
}

void set_stack_args(ps2vita::Emulator& emulator) {
  auto& gpr = emulator.cpu().state().gpr;
  gpr[3] = 0x3Cu;           // $v1: SetupThread
  gpr[4] = 0x005B3BF0u;    // $a0: gp
  gpr[5] = 0x01FF8000u;    // $a1: stack base
  gpr[6] = 0x00008000u;    // $a2: stack size
  gpr[7] = 0x005B9C80u;    // $a3: arg pointer
  gpr[8] = 0x00436340u;    // $t0: ExitThread
}

} // namespace

int main() {
  const auto elf = syscall_elf();
  ps2vita::Emulator emulator;
  require(emulator.load_elf(elf.data(), elf.size()).ok, "synthetic ELF loads");
  set_stack_args(emulator);
  require(emulator.run_slice(1u) == ps2vita::StopReason::Syscall &&
              emulator.cpu().state().pc == 0x1000u,
          "startup HLE is off by default; trapped PC remains at syscall");

  emulator.reset();
  set_stack_args(emulator);
  emulator.enable_bare_elf_startup_syscalls(true);
  require(emulator.run_slice(1u) == ps2vita::StopReason::StepLimit,
          "SetupThread is handled when explicitly enabled");
  require(emulator.cpu().state().gpr[2] == 0x02000000u &&
              emulator.cpu().state().pc == 0x1004u &&
              emulator.cpu().state().cycles == 1u,
          "SetupThread returns RAM top and retires syscall exactly once");

  auto& gpr = emulator.cpu().state().gpr;
  gpr[3] = 0x3Du;           // SetupHeap
  gpr[4] = 0x00400000u;    // Heap base
  gpr[5] = 0x00100000u;    // Fixed heap size
  require(emulator.run_slice(1u) == ps2vita::StopReason::StepLimit &&
              emulator.cpu().state().pc == 0x1008u &&
              emulator.cpu().state().cycles == 2u &&
              emulator.cpu().state().gpr[2] == 0x00500000u,
          "SetupHeap exposes the validated heap end and continues");
  gpr[3] = 0x64u;
  require(emulator.run_slice(1u) == ps2vita::StopReason::StepLimit &&
              emulator.cpu().state().pc == 0x100Cu &&
              emulator.cpu().state().gpr[2] == 0x00500000u &&
              emulator.cpu().state().cycles == 3u,
          "FlushCache is coherent and preserves the void syscall's $v0");
  require(emulator.run_slice(1u) == ps2vita::StopReason::Break,
          "unknown execution stop reason is not swallowed by HLE");

  emulator.reset();
  set_stack_args(emulator);
  require(emulator.run_slice(1u) == ps2vita::StopReason::StepLimit,
          "reset retains explicit HLE setting for the loaded ELF");
  emulator.cpu().state().gpr[3] = 0x3Du;
  emulator.cpu().state().gpr[4] = 0x00400000u;
  emulator.cpu().state().gpr[5] = 0xFFFFFFFFu;
  require(emulator.run_slice(1u) == ps2vita::StopReason::StepLimit &&
              emulator.cpu().state().gpr[2] == 0x02000000u,
          "SetupHeap -1 size is bounded by previously initialized stack");

  require(emulator.load_elf(elf.data(), elf.size()).ok,
          "loading a fresh ELF resets startup kernel state");
  require(emulator.run_slice(1u) == ps2vita::StopReason::Syscall,
          "loading a fresh ELF disables startup HLE until explicitly armed");

  emulator.reset();
  emulator.enable_bare_elf_startup_syscalls(true);
  set_stack_args(emulator);
  emulator.cpu().state().gpr[6] = 0x10000u;
  require(emulator.run_slice(1u) == ps2vita::StopReason::Syscall &&
              emulator.cpu().state().pc == 0x1000u,
          "invalid overflow stack remains a visible syscall stop");

  emulator.reset();
  set_stack_args(emulator);
  emulator.cpu().state().gpr[3] = 0x70u; // Unknown kernel service.
  require(emulator.run_slice(1u) == ps2vita::StopReason::Syscall &&
              emulator.cpu().state().pc == 0x1000u,
          "other kernel services are not silently skipped");


  // Bare-ELF kernel semaphores: use the PS2 ee_sema_t field layout.
  emulator.reset();
  emulator.enable_bare_elf_startup_syscalls(true);
  constexpr std::uint32_t descriptor = 0x1800u;
  emulator.memory().write32(descriptor, 0u);       // count
  emulator.memory().write32(descriptor + 4u, 2u);  // max_count
  emulator.memory().write32(descriptor + 8u, 1u);  // init_count
  emulator.memory().write32(descriptor + 12u, 0u); // wait_threads
  emulator.memory().write32(descriptor + 16u, 0u); // attr
  emulator.memory().write32(descriptor + 20u, 0u); // option
  auto& kernel_state = emulator.cpu().state();
  auto invoke_kernel = [&](std::uint32_t number, std::uint32_t argument) {
    kernel_state.pc = 0x1000u; // The fixture's first instruction is SYSCALL.
    kernel_state.gpr[3] = number;
    kernel_state.gpr[4] = argument;
    return emulator.run_slice(1u);
  };
  require(invoke_kernel(0x40u, descriptor) == ps2vita::StopReason::StepLimit &&
              kernel_state.pc == 0x1004u && kernel_state.gpr[2] != 0u,
          "CreateSema reads descriptor and returns a live object ID");
  const auto sema_id = static_cast<std::uint32_t>(kernel_state.gpr[2]);
  require(invoke_kernel(0x45u, sema_id) == ps2vita::StopReason::StepLimit &&
              kernel_state.gpr[2] == 0u,
          "PollSema consumes the initial semaphore count");
  require(invoke_kernel(0x45u, sema_id) == ps2vita::StopReason::StepLimit &&
              kernel_state.gpr[2] == UINT64_MAX,
          "PollSema on empty semaphore fails without blocking");
  const auto before_wait = kernel_state.cycles;
  require(invoke_kernel(0x44u, sema_id) == ps2vita::StopReason::Syscall &&
              kernel_state.pc == 0x1000u &&
              kernel_state.cycles == before_wait + 1u,
          "unsatisfied WaitSema stays a visible stop without a scheduler");
  require(invoke_kernel(0x42u, sema_id) == ps2vita::StopReason::StepLimit &&
              kernel_state.gpr[2] == 0u,
          "SignalSema increases the available count");
  require(invoke_kernel(0x44u, sema_id) == ps2vita::StopReason::StepLimit &&
              kernel_state.gpr[2] == 0u,
          "WaitSema consumes a signaled semaphore");
  require(invoke_kernel(0x41u, sema_id) == ps2vita::StopReason::StepLimit &&
              kernel_state.gpr[2] == 0u,
          "DeleteSema releases its allocated object");
  require(invoke_kernel(0x42u, sema_id) == ps2vita::StopReason::StepLimit &&
              kernel_state.gpr[2] == UINT64_MAX,
          "deleted semaphore IDs cannot be signaled");

  emulator.reset();
  emulator.enable_bare_elf_startup_syscalls(true);
  require(invoke_kernel(0x45u, sema_id) == ps2vita::StopReason::StepLimit &&
              kernel_state.gpr[2] == UINT64_MAX,
          "emulator reset discards all kernel semaphore IDs");
  require(invoke_kernel(0x40u, ps2vita::Memory::kRamSize - 8u) ==
              ps2vita::StopReason::Syscall &&
              kernel_state.pc == 0x1000u,
          "invalid pointer to CreateSema is not blindly dereferenced");


  // The game registers its own FindAddress (0x83) implementation using
  // SetSyscall (0x74). Exercise real guest code dispatch and the return path.
  emulator.reset();
  emulator.enable_bare_elf_startup_syscalls(true);
  emulator.memory().write32(0x1100u, 0x2402002Au); // addiu v0, zero, 42
  emulator.memory().write32(0x1104u, 0x03E00008u); // jr ra
  emulator.memory().write32(0x1108u, 0x00000000u); // nop (delay slot)
  kernel_state.gpr[2] = 0x12345u;
  kernel_state.gpr[5] = 0x1100u;
  require(invoke_kernel(0x74u, 0x83u) == ps2vita::StopReason::StepLimit &&
              kernel_state.pc == 0x1004u &&
              kernel_state.gpr[2] == 0x12345u,
          "SetSyscall registers guest code without clobbering a void return");
  kernel_state.gpr[31] = 0x2220u;
  require(invoke_kernel(0x83u, 0u) == ps2vita::StopReason::StepLimit &&
              kernel_state.pc == 0x1100u &&
              kernel_state.gpr[31] == 0x1004u,
          "registered syscall enters the guest handler");
  require(emulator.run_slice(3u) == ps2vita::StopReason::StepLimit &&
              kernel_state.pc == 0x1004u &&
              kernel_state.gpr[2] == 42u &&
              kernel_state.gpr[31] == 0x2220u,
          "guest handler returns to syscall continuation and restores RA");
  kernel_state.gpr[5] = 0u;
  require(invoke_kernel(0x74u, 0x83u) == ps2vita::StopReason::StepLimit,
          "SetSyscall with null handler unregisters the override");
  require(invoke_kernel(0x83u, 0u) == ps2vita::StopReason::Syscall &&
              kernel_state.pc == 0x1000u,
          "unregistered service remains an explicit syscall stop");

  emulator.reset();
  emulator.enable_bare_elf_startup_syscalls(true);
  kernel_state.gpr[5] = 0x1100u;
  require(invoke_kernel(0x83u, 0u) == ps2vita::StopReason::Syscall,
          "reset also removes guest syscall table registrations");
  require(invoke_kernel(0x74u, 0x130u) == ps2vita::StopReason::Syscall &&
              kernel_state.pc == 0x1000u,
          "out-of-range SetSyscall requests are not silently accepted");

  std::puts("Bare-ELF startup syscall tests passed.");
  return EXIT_SUCCESS;
}
