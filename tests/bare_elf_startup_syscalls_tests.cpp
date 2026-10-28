#include "ps2vita/emulator.hpp"

#include <array>
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

  // Real NFSMW wrapper at 0x00495980 is `addiu $v1, $zero, 0x7f`,
  // followed by syscall at 0x00495984. GetMemorySize returns bytes.
  emulator.reset();
  set_stack_args(emulator);
  auto& memory_size_state = emulator.cpu().state();
  memory_size_state.gpr[2] = 0x1234u;
  memory_size_state.gpr[3] = 0x7Fu;
  const auto memory_size_cycles = memory_size_state.cycles;
  require(emulator.run_slice(1u) == ps2vita::StopReason::StepLimit &&
              memory_size_state.pc == 0x1004u &&
              memory_size_state.cycles == memory_size_cycles + 1u &&
              memory_size_state.gpr[2] == ps2vita::Memory::kRamSize &&
              memory_size_state.gpr[3] == 0x7Fu &&
              memory_size_state.gpr[4] == 0x005B3BF0u,
          "NFSMW GetMemorySize returns 32 MiB and retires exactly once");


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

  // Model a second registered syscall, then perform the exact class of
  // KSEG0 pointer search used by startup code. A host-only C++ registry
  // cannot satisfy this test: the pointers must live in guest EE RAM.
  emulator.memory().write32(0x1180u, 0x03E00008u); // jr ra
  emulator.memory().write32(0x1184u, 0u);          // delay slot
  kernel_state.gpr[5] = 0x1180u;
  require(invoke_kernel(0x74u, 0x5Au) == ps2vita::StopReason::StepLimit,
          "second guest syscall registration succeeds");
  const auto find_handler = [&](std::uint32_t target) {
    for (std::uint32_t addr = 0x80000000u; addr < 0x80080000u; addr += 4u) {
      if (emulator.memory().read32(addr) == target) return addr;
    }
    return std::uint32_t{0};
  };
  const auto location_83 = find_handler(0x1100u);
  const auto location_5a = find_handler(0x1180u);
  require(location_83 != 0u && location_5a != 0u &&
              location_83 - 0x83u * 4u == location_5a - 0x5Au * 4u &&
              location_83 - location_5a == 0xA4u,
          "both registered handlers are discoverable with syscall-table spacing");
  // Direct guest table patches must also affect dispatch.
  emulator.memory().write32(location_83, 0x1180u);
  kernel_state.gpr[31] = 0x4444u;
  require(invoke_kernel(0x83u, 0u) == ps2vita::StopReason::StepLimit &&
              kernel_state.pc == 0x1180u,
          "guest memory writes change the registered syscall target");
  require(emulator.run_slice(2u) == ps2vita::StopReason::StepLimit &&
              kernel_state.pc == 0x1004u &&
              kernel_state.gpr[31] == 0x4444u,
          "guest-patched syscall handler returns normally");
  emulator.memory().write32(location_83, 0x1100u);
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
  require(emulator.memory().read32(location_83) == 0u &&
              emulator.memory().read32(location_5a) == 0u,
          "reset also clears guest-visible syscall table pointers");
  require(invoke_kernel(0x74u, 0x130u) == ps2vita::StopReason::Syscall &&
              kernel_state.pc == 0x1000u,
          "out-of-range SetSyscall requests are not silently accepted");


  // EE INTC source 11 is Timer 2. Registering a handler is meaningful even
  // before we have Timer 2 event delivery; the returned ID must be stable.
  emulator.reset();
  emulator.enable_bare_elf_startup_syscalls(true);
  constexpr std::uint32_t timer_handler = 0x1100u;
  kernel_state.gpr[5] = timer_handler;
  kernel_state.gpr[6] = 0u; // AddIntcHandler position: insert at front.
  kernel_state.gpr[7] = 0x1200u;
  require(invoke_kernel(0x10u, 11u) == ps2vita::StopReason::StepLimit &&
              kernel_state.gpr[2] >= 1u && kernel_state.gpr[2] <= 32u,
          "AddIntcHandler registers Timer 2 callback and returns an ID");
  const auto intc_id = static_cast<std::uint32_t>(kernel_state.gpr[2]);
  require(invoke_kernel(0x14u, 11u) == ps2vita::StopReason::StepLimit &&
              kernel_state.gpr[2] == 0u &&
              (emulator.memory().read32(0x1000F010u) & (1u << 11u)) != 0u,
          "_EnableIntc toggles actual EE hardware mask and reports prior state");
  require(invoke_kernel(0x14u, 11u) == ps2vita::StopReason::StepLimit &&
              kernel_state.gpr[2] == 1u &&
              (emulator.memory().read32(0x1000F010u) & (1u << 11u)) != 0u,
          "_EnableIntc is idempotent on already enabled source");
  require(invoke_kernel(0x15u, 11u) == ps2vita::StopReason::StepLimit &&
              kernel_state.gpr[2] == 1u &&
              (emulator.memory().read32(0x1000F010u) & (1u << 11u)) == 0u,
          "_DisableIntc clears the physical mask without toggling twice");
  kernel_state.gpr[5] = intc_id;
  require(invoke_kernel(0x11u, 11u) == ps2vita::StopReason::StepLimit &&
              kernel_state.gpr[2] == 0u,
          "RemoveIntcHandler releases only the matching cause and ID");
  require(invoke_kernel(0x11u, 11u) == ps2vita::StopReason::StepLimit &&
              kernel_state.gpr[2] == UINT64_MAX,
          "removing the same INTC handler twice is rejected");
  kernel_state.gpr[5] = 0x1100u;
  kernel_state.gpr[6] = 0u;
  require(invoke_kernel(0x12u, 2u) == ps2vita::StopReason::StepLimit &&
              kernel_state.gpr[2] >= 1u && kernel_state.gpr[2] <= 32u,
          "AddDmacHandler allocates a bounded DMA callback");
  const auto dmac_id = static_cast<std::uint32_t>(kernel_state.gpr[2]);
  require(invoke_kernel(0x16u, 2u) == ps2vita::StopReason::StepLimit &&
              (emulator.memory().read32(0x1000E010u) & (1u << 18u)) != 0u,
          "_EnableDmac toggles the DMA hardware mask");
  require(invoke_kernel(0x17u, 2u) == ps2vita::StopReason::StepLimit &&
              (emulator.memory().read32(0x1000E010u) & (1u << 18u)) == 0u,
          "_DisableDmac clears the DMA hardware mask");
  kernel_state.gpr[5] = dmac_id;
  require(invoke_kernel(0x13u, 2u) == ps2vita::StopReason::StepLimit &&
              kernel_state.gpr[2] == 0u,
          "RemoveDmacHandler releases the DMA callback");
  kernel_state.gpr[5] = 0x1100u;
  require(invoke_kernel(0x10u, 15u) == ps2vita::StopReason::StepLimit &&
              kernel_state.gpr[2] == UINT64_MAX,
          "invalid INTC source cannot be registered");

  // Real EE thread contexts: match NFS:MW's observed descriptor shape
  // (function, stack, 0x400-byte size, gp, priority=0).
  emulator.reset();
  emulator.enable_bare_elf_startup_syscalls(true);
  constexpr std::uint32_t thread_desc = 0x1800u;
  constexpr std::uint32_t thread_entry = 0x1300u;
  constexpr std::uint32_t thread_stack = 0x3000u;
  emulator.memory().write32(thread_entry, 0x24080007u);       // addiu t0,zero,7
  emulator.memory().write32(thread_entry + 4u, 0x24030024u);  // v1=ExitDeleteThread
  emulator.memory().write32(thread_entry + 8u, 0x0000000Cu);  // syscall
  emulator.memory().write32(0x1400u, 0x08000500u);            // j 0x1400
  emulator.memory().write32(0x1404u, 0u);                    // nop
  const std::array<std::uint32_t, 9> thread_fields{
      0u, thread_entry, thread_stack, 0x400u, 0x2000u, 0u, 0u, 0u, 0u};
  for (std::size_t i = 0u; i < thread_fields.size(); ++i)
    emulator.memory().write32(thread_desc + 4u * i, thread_fields[i]);
  require(invoke_kernel(0x20u, thread_desc) == ps2vita::StopReason::StepLimit &&
              kernel_state.gpr[2] == 2u && kernel_state.pc == 0x1004u,
          "CreateThread validates the real descriptor and allocates ID 2");
  kernel_state.gpr[5] = 0xBEEFu;
  require(invoke_kernel(0x22u, 2u) == ps2vita::StopReason::StepLimit &&
              kernel_state.gpr[2] == 0u,
          "StartThread schedules the created EE entry with its argument");
  require(invoke_kernel(0x2Fu, 0u) == ps2vita::StopReason::StepLimit &&
              kernel_state.gpr[2] == 1u,
          "the initial game thread retains EE ID 1");
  // The bounded host quantum dispatches the ready guest thread.
  kernel_state.pc = 0x1400u;
  kernel_state.gpr[8] = 0x1234u;
  const auto clocks_before_switch = kernel_state.cycles;
  require(emulator.run_slice(254u) == ps2vita::StopReason::StepLimit &&
              kernel_state.pc == thread_entry &&
              kernel_state.gpr[4] == 0xBEEFu &&
              kernel_state.gpr[28] == 0x2000u &&
              kernel_state.gpr[29] == thread_stack + 0x3F0u &&
              kernel_state.cycles == clocks_before_switch + 254u,
          "scheduler saves main context and restores entry, argument and stack");
  require(emulator.run_slice(1u) == ps2vita::StopReason::StepLimit &&
              kernel_state.gpr[8] == 7u,
          "scheduled thread actually executes its own guest instructions");
  require(emulator.run_slice(2u) == ps2vita::StopReason::StepLimit &&
              kernel_state.pc == 0x1400u &&
              kernel_state.gpr[8] == 0x1234u &&
              kernel_state.cycles == clocks_before_switch + 257u,
          "ExitDeleteThread resumes main with intact GPRs and monotonic clock");
  require(invoke_kernel(0x21u, 2u) == ps2vita::StopReason::StepLimit &&
              kernel_state.gpr[2] == UINT64_MAX,
          "deleted guest EE thread cannot be deleted a second time");

  // NFSMW 2005 startup stopped at PC 0x004955D4 with $v1=0x4B.
  // GetOsdConfigParam returns a packed four-byte ConfigParam at $a0.
  emulator.reset();
  emulator.enable_bare_elf_startup_syscalls(true);
  constexpr std::uint32_t osd_output = ps2vita::Memory::kRamSize - 0x80u;
  emulator.memory().write32(osd_output, 0xDEADBEEFu);
  emulator.memory().write32(osd_output + 4u, 0xA5A5A5A5u);
  kernel_state.gpr[2] = 2u; // Void syscall must not overwrite a live $v0.
  const auto before_osd = kernel_state.cycles;
  require(invoke_kernel(0x4Bu, osd_output) == ps2vita::StopReason::StepLimit &&
              kernel_state.pc == 0x1004u &&
              kernel_state.cycles == before_osd + 1u &&
              kernel_state.gpr[2] == 2u &&
              emulator.memory().read32(osd_output) == 0x00014011u &&
              emulator.memory().read32(osd_output + 4u) == 0xA5A5A5A5u,
          "GetOsdConfigParam writes the 32-bit English OSD profile once");

  // Replicate the title's observed Get/Set/Get/Set OSD sequence at
  // 0x0049A41C..0x0049A454, including the version-bit change.
  const auto original_osd = emulator.memory().read32(osd_output);
  const auto updated_osd = (original_osd & 0xFFFF1FFFu) | 0x2000u;
  emulator.memory().write32(osd_output + 4u, updated_osd);
  require(invoke_kernel(0x4Au, osd_output + 4u) ==
              ps2vita::StopReason::StepLimit &&
              kernel_state.gpr[2] == 2u,
          "SetOsdConfigParam consumes one word and preserves void $v0");
  emulator.memory().write32(osd_output + 4u, 0xDEADBEEFu);
  require(invoke_kernel(0x4Bu, osd_output + 4u) ==
              ps2vita::StopReason::StepLimit &&
              emulator.memory().read32(osd_output + 4u) == updated_osd,
          "GetOsdConfigParam returns the state set by the previous syscall");
  require(invoke_kernel(0x4Au, osd_output) ==
              ps2vita::StopReason::StepLimit &&
              invoke_kernel(0x4Bu, osd_output + 4u) ==
              ps2vita::StopReason::StepLimit &&
              emulator.memory().read32(osd_output + 4u) == original_osd,
          "OSD settings can be restored by the game's final Set call");

  require(invoke_kernel(0x4Bu, osd_output + 1u) ==
              ps2vita::StopReason::Syscall && kernel_state.pc == 0x1000u,
          "misaligned GetOsdConfigParam pointer stays a visible stop");
  require(invoke_kernel(0x4Bu, ps2vita::Memory::kRamSize - 2u) ==
              ps2vita::StopReason::Syscall && kernel_state.pc == 0x1000u,
          "out-of-bounds GetOsdConfigParam pointer stays a visible stop");
  require(invoke_kernel(0x4Bu, ps2vita::Memory::kHwBase) ==
              ps2vita::StopReason::Syscall && kernel_state.pc == 0x1000u,
          "GetOsdConfigParam never writes to EE MMIO");
  require(invoke_kernel(0x4Bu, 0u) ==
              ps2vita::StopReason::Syscall && kernel_state.pc == 0x1000u,
          "GetOsdConfigParam rejects a null destination");
  require(invoke_kernel(0x4Au, 0u) ==
              ps2vita::StopReason::Syscall && kernel_state.pc == 0x1000u,
          "SetOsdConfigParam rejects a null source");
  require(invoke_kernel(0x4Bu, osd_output) ==
              ps2vita::StopReason::StepLimit &&
              emulator.memory().read32(osd_output) == original_osd,
          "an invalid Set does not mutate the persistent OSD profile");

  // The October 28 NFSMW hardware stop at PC 0x00495504 is EE
  // syscall 0x3E (EndOfHeap). It must reflect SetupHeap, not a dummy
  // success code or a guessed limit.
  emulator.reset();
  emulator.enable_bare_elf_startup_syscalls(true);
  require(invoke_kernel(0x3Eu, 0u) == ps2vita::StopReason::Syscall &&
              kernel_state.pc == 0x1000u,
          "EndOfHeap without SetupHeap remains a visible syscall stop");
  set_stack_args(emulator);
  require(invoke_kernel(0x3Cu, 0x005B3BF0u) ==
              ps2vita::StopReason::StepLimit,
          "EndOfHeap fixture sets up main thread first");
  kernel_state.gpr[5] = 0x00100000u;
  require(invoke_kernel(0x3Du, 0x00400000u) ==
              ps2vita::StopReason::StepLimit &&
              kernel_state.gpr[2] == 0x00500000u,
          "EndOfHeap fixture sets up a bounded 1 MiB heap");
  kernel_state.gpr[2] = 0xDEADu;
  const auto cycles_before_heap_query = kernel_state.cycles;
  require(invoke_kernel(0x3Eu, 0u) == ps2vita::StopReason::StepLimit &&
              kernel_state.gpr[2] == 0x00500000u &&
              kernel_state.pc == 0x1004u &&
              kernel_state.cycles == cycles_before_heap_query + 1u,
          "EndOfHeap returns actual configured bound and retires once");
  kernel_state.gpr[5] = 0x00200000u;
  require(invoke_kernel(0x3Du, 0x00400000u) ==
              ps2vita::StopReason::StepLimit &&
              invoke_kernel(0x3Eu, 0u) == ps2vita::StopReason::StepLimit &&
              kernel_state.gpr[2] == 0x00600000u,
          "EndOfHeap tracks a later successful SetupHeap call");
  emulator.reset();
  emulator.enable_bare_elf_startup_syscalls(true);
  require(invoke_kernel(0x3Eu, 0u) == ps2vita::StopReason::Syscall,
          "a guest reset clears the prior heap boundary");

  std::puts("Bare-ELF startup syscall tests passed.");
  return EXIT_SUCCESS;
}
