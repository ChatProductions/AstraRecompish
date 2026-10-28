#include "ps2vita/emulator.hpp"

#include <algorithm>

namespace ps2vita {
namespace {

// Synthetic EE syscall table in KSEG0 RAM, below the bare-ELF load area.
// Games can discover the registered handlers by scanning kernel memory.
constexpr std::uint32_t kBareElfSyscallTable = 0x80010000u;
constexpr std::uint32_t kBareElfSyscallSlots = 0x130u;
// Guest-visible return trampoline for EE threads whose entry functions return.
constexpr std::uint32_t kThreadExitTrampoline = 0x80010800u;
constexpr std::uint32_t kThreadQuantum = 256u;

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
  sif_system_regs_ = {};
  deci2_last_debug_text_ = {};
  deci2_debug_print_count_ = 0u;
  deci2_last_debug_truncated_ = false;
  kernel_semaphores_ = {};
  kernel_threads_ = {};
  current_thread_index_ = 0u;
  thread_quantum_remaining_ = kThreadQuantum;
  intc_handlers_ = {};
  dmac_handlers_ = {};
  guest_syscall_returns_ = {};
  guest_syscall_depth_ = 0u;
  ready_ = image_.ok;
  if (ready_) {
    cpu_.reset(image_.entry);
    cpu_.set_exception_mode(false);
    initialize_kernel_threads();
  }
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
  sif_system_regs_ = {};
  deci2_last_debug_text_ = {};
  deci2_debug_print_count_ = 0u;
  deci2_last_debug_truncated_ = false;
  kernel_semaphores_ = {};
  kernel_threads_ = {};
  current_thread_index_ = 0u;
  thread_quantum_remaining_ = kThreadQuantum;
  intc_handlers_ = {};
  dmac_handlers_ = {};
  guest_syscall_returns_ = {};
  guest_syscall_depth_ = 0u;
  gif_.reset();
  vif1_.reset();
  set_bios_reset_state(cpu_.state());
  cpu_.set_exception_mode(true);
  return true;
}

void Emulator::initialize_kernel_threads() {
  kernel_threads_ = {};
  current_thread_index_ = 0u;
  thread_quantum_remaining_ = kThreadQuantum;
  // MIPS: addiu $v1, $zero, 0x24; syscall; break.
  // A normal returning thread receives the same ExitDeleteThread service.
  memory_.write32(kThreadExitTrampoline, 0x24030024u);
  memory_.write32(kThreadExitTrampoline + 4u, 0x0000000Cu);
  memory_.write32(kThreadExitTrampoline + 8u, 0x0000000Du);
  kernel_threads_[0].status = 1u;
  kernel_threads_[0].priority = 0u;
  kernel_threads_[0].entry = image_.entry;
  kernel_threads_[0].context = cpu_.save_thread_context();
}

bool Emulator::has_ready_thread() const {
  for (const auto& thread : kernel_threads_)
    if (thread.status == 2u) return true;
  return false;
}

bool Emulator::switch_to_ready_thread() {
  if (guest_syscall_depth_ != 0u) return false;
  // Prefer lower numeric priority, then stable round-robin among peers.
  std::size_t choice = kernel_threads_.size();
  std::uint32_t best = 128u;
  for (std::size_t offset = 1u; offset <= kernel_threads_.size(); ++offset) {
    const auto i = (current_thread_index_ + offset) % kernel_threads_.size();
    const auto& candidate = kernel_threads_[i];
    if (candidate.status != 2u || candidate.priority >= best) continue;
    best = candidate.priority;
    choice = i;
  }
  if (choice == kernel_threads_.size()) return false;
  auto& current = kernel_threads_[current_thread_index_];
  current.context = cpu_.save_thread_context();
  if (current.status == 1u) current.status = 2u;
  current_thread_index_ = choice;
  auto& next = kernel_threads_[choice];
  next.status = 1u;
  cpu_.load_thread_context(next.context);
  thread_quantum_remaining_ = kThreadQuantum;
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
    else if (image_.ok && has_ready_thread())
      budget = std::min(budget, thread_quantum_remaining_);
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
    // Slice boundaries and syscall retirements are safe EE scheduling points.
    // Never switch inside a guest-installed kernel syscall callback.
    const auto rotate_if_needed = [&]() {
      if (!image_.ok || guest_syscall_depth_ != 0u ||
          !has_ready_thread()) return;
      if (executed >= thread_quantum_remaining_) {
        if (!switch_to_ready_thread()) thread_quantum_remaining_ = kThreadQuantum;
      } else {
        thread_quantum_remaining_ -= executed;
      }
    };
    if (result == StopReason::Syscall && image_.ok &&
        bare_elf_startup_syscalls_ && handle_bare_elf_startup_syscall()) {
      // The syscall was already counted by Cpu::step; continue with the
      // remaining guest-instruction budget, without counting it twice.
      rotate_if_needed();
      continue;
    }
    if (result != StopReason::StepLimit) return result;
    if (executed == 0u) return StopReason::Halted;
    rotate_if_needed();
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
  const auto handler = number < kBareElfSyscallSlots
      ? memory_.read32(kBareElfSyscallTable + number * 4u) : 0u;
  if (handler != 0u) {
    if (guest_syscall_depth_ == guest_syscall_returns_.size()) return false;
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
  case 0x79u: // sceSifSetReg(id, value): kernel software or SBUS hardware.
  case 0x7Au: { // sceSifGetReg(id): return the actual current register.
    const bool system_reg = (arg0 & 0x80000000u) != 0u;
    std::uint32_t value = 0u;
    std::uint32_t mmio = 0u;
    std::size_t slot = 0u;
    if (system_reg) {
      slot = arg0 & 0x7FFFFFFFu;
      if (slot >= sif_system_regs_.size()) return false;
      value = sif_system_regs_[slot];
    } else {
      if (arg0 < 1u || arg0 > 4u) return false;
      mmio = Memory::kHwBase + 0xF200u + (arg0 - 1u) * 16u;
      value = memory_.read32(mmio);
    }
    if (number == 0x7Au) return cpu_.retire_trapped_syscall(value);
    if (!cpu_.retire_trapped_syscall(0u)) return false;
    if (system_reg) sif_system_regs_[slot] = arg1;
    else memory_.write32(mmio, arg1);
    return true;
  }
  case 0x78u: { // sceSifSetDChain(): enable EE SIF0 IOP-to-EE chain DMA.
    // PS2SDK specifies channel 5 CHCR=0x184 (TIE|CHAIN|STR).
    // Retire first: if the syscall is in a delay slot, no MMIO side effect.
    if (!cpu_.retire_trapped_syscall(0u, false)) return false;
    memory_.write32(Memory::kHwBase + 0xC000u, 0x184u);
    return true;
  }
  case 0x7Cu: { // Deci2Call(0x10, u32*): EE debug-console text.
    // PCSX2's EE kernel reads a pointer to a NUL-terminated debug string
    // from *a1 for this specific subcall. Do not accept other DECI2 ops.
    if (arg0 != 0x10u || (arg1 & 3u) != 0u) return false;
    const auto is_guest_ram = [](std::uint32_t addr, std::uint32_t size) {
      const auto phys = (addr & 0xC0000000u) == 0x80000000u
          ? addr & 0x1FFFFFFFu : addr;
      return phys != 0u && phys < Memory::kRamSize &&
             size <= Memory::kRamSize - phys;
    };
    if (!is_guest_ram(arg1, 4u) || !memory_.valid(arg1, 4u)) return false;
    const auto text_ptr = memory_.read32(arg1);
    if (!is_guest_ram(text_ptr, 1u)) return false;
    std::array<char, 1024> text{};
    constexpr char hex[] = "0123456789ABCDEF";
    constexpr std::size_t kMaxTextBytes = 192u;
    std::size_t used = 0u;
    bool terminated = false;
    for (std::size_t i = 0u; i < kMaxTextBytes; ++i) {
      if (std::uint64_t{text_ptr} + i > UINT32_MAX ||
          !is_guest_ram(text_ptr + static_cast<std::uint32_t>(i), 1u) ||
          !memory_.valid(text_ptr + static_cast<std::uint32_t>(i))) return false;
      const auto ch = memory_.read8(text_ptr + static_cast<std::uint32_t>(i));
      if (ch == 0u) { terminated = true; break; }
      if (ch >= 0x20u && ch <= 0x7Eu && ch != '\\') {
        text[used++] = static_cast<char>(ch);
      } else if (ch == '\n' || ch == '\r' || ch == '\t' || ch == '\\') {
        text[used++] = '\\';
        text[used++] = ch == '\n' ? 'n' : ch == '\r' ? 'r' :
                       ch == '\t' ? 't' : '\\';
      } else {
        text[used++] = '\\';
        text[used++] = 'x';
        text[used++] = hex[ch >> 4];
        text[used++] = hex[ch & 0xFu];
      }
    }
    // The debug-print syscall does not change the live $v0 return register.
    if (!cpu_.retire_trapped_syscall(0u, false)) return false;
    deci2_last_debug_text_ = text;
    deci2_last_debug_truncated_ = !terminated;
    ++deci2_debug_print_count_;
    return true;
  }
  case 0x74u: { // SetSyscall(number, guest_handler); returns void.
    if (arg0 >= kBareElfSyscallSlots ||
        (arg1 != 0u && ((arg1 & 3u) != 0u ||
         !memory_.instruction_reads_side_effect_free(arg1, 4u))))
      return false;
    if (!cpu_.retire_trapped_syscall(0u, false)) return false;
    // The game scans KSEG0 for the two registered function pointers,
    // 0xA4 bytes apart (0x83 - 0x5A syscall slots).
    memory_.write32(kBareElfSyscallTable + arg0 * 4u, arg1);
    return true;
  }
  case 0x10u: // AddIntcHandler(cause, handler, position, arg)
  case 0x12u: { // AddDmacHandler(channel, handler, position, arg)
    const bool intc = number == 0x10u;
    auto& handlers = intc ? intc_handlers_ : dmac_handlers_;
    const auto limit = intc ? 15u : 10u;
    // SDK position 0 prepends; -1 appends. Other chain positions would
    // require a guest-visible interrupt dispatch ordering implementation.
    const bool valid_position = arg2 == 0u || arg2 == UINT32_MAX;
    if (arg0 >= limit || arg1 == 0u || (arg1 & 3u) != 0u ||
        !memory_.instruction_reads_side_effect_free(arg1, 4u) ||
        !valid_position)
      return cpu_.retire_trapped_syscall(UINT64_MAX);
    for (std::size_t i = 0; i < handlers.size(); ++i) {
      if (handlers[i].address != 0u) continue;
      if (!cpu_.retire_trapped_syscall(i + 1u)) return false;
      handlers[i] = {arg1, arg0, static_cast<std::uint32_t>(state.gpr[28]),
                     static_cast<std::uint32_t>(state.gpr[7]), arg2};
      return true;
    }
    return cpu_.retire_trapped_syscall(UINT64_MAX);
  }
  case 0x11u: // RemoveIntcHandler(cause, handler_id)
  case 0x13u: { // RemoveDmacHandler(channel, handler_id)
    const bool intc = number == 0x11u;
    auto& handlers = intc ? intc_handlers_ : dmac_handlers_;
    if (arg0 >= (intc ? 15u : 10u) || arg1 == 0u ||
        arg1 > handlers.size() || handlers[arg1 - 1u].address == 0u ||
        handlers[arg1 - 1u].cause != arg0)
      return cpu_.retire_trapped_syscall(UINT64_MAX);
    if (!cpu_.retire_trapped_syscall(0u)) return false;
    handlers[arg1 - 1u] = {};
    return true;
  }
  case 0x14u: // _EnableIntc(cause)
  case 0x15u: { // _DisableIntc(cause)
    if (arg0 >= 15u) return cpu_.retire_trapped_syscall(UINT64_MAX);
    constexpr std::uint32_t kIntcMask = 0x1000F010u;
    const auto bit = 1u << arg0;
    const bool was_enabled = (memory_.read32(kIntcMask) & bit) != 0u;
    const bool should_enable = number == 0x14u;
    if (!cpu_.retire_trapped_syscall(was_enabled ? 1u : 0u)) return false;
    // EE INTC_MASK is write-one-to-toggle, not a plain assignment.
    if (was_enabled != should_enable) memory_.write32(kIntcMask, bit);
    return true;
  }
  case 0x16u: // _EnableDmac(channel)
  case 0x17u: { // _DisableDmac(channel)
    if (arg0 >= 10u) return cpu_.retire_trapped_syscall(UINT64_MAX);
    constexpr std::uint32_t kDmacStat = 0x1000E010u;
    const auto bit = 1u << (arg0 + 16u);
    const bool was_enabled = (memory_.read32(kDmacStat) & bit) != 0u;
    const bool should_enable = number == 0x16u;
    if (!cpu_.retire_trapped_syscall(was_enabled ? 1u : 0u)) return false;
    // DMAC_STAT high-half bits also toggle on write.
    if (was_enabled != should_enable) memory_.write32(kDmacStat, bit);
    return true;
  }
  case 0x20u: { // CreateThread(ee_thread_t*): nine 32-bit words.
    constexpr std::uint32_t kDescriptorSize = 36u;
    if ((arg0 & 3u) != 0u || !memory_.valid(arg0, kDescriptorSize))
      return false;
    const auto entry = memory_.read32(arg0 + 4u);
    const auto stack = memory_.read32(arg0 + 8u);
    const auto stack_size = memory_.read32(arg0 + 12u);
    const auto gp = memory_.read32(arg0 + 16u);
    const auto priority = memory_.read32(arg0 + 20u);
    const auto attr = memory_.read32(arg0 + 28u);
    const auto option = memory_.read32(arg0 + 32u);
    const std::uint64_t top = static_cast<std::uint64_t>(stack) + stack_size;
    if (entry == 0u || (entry & 3u) != 0u ||
        !memory_.instruction_reads_side_effect_free(entry, 4u) ||
        stack == 0u || (stack & 15u) != 0u ||
        stack_size < 0x100u || (stack_size & 15u) != 0u ||
        top > UINT32_MAX || !memory_.valid(stack, stack_size) ||
        priority >= 128u)
      return cpu_.retire_trapped_syscall(UINT64_MAX);
    // EE kernel reserves slot 0 for the boot thread. The observed NFS:MW
    // thread descriptor has priority 0, so retain that valid initial value.
    for (std::size_t i = 1u; i < kernel_threads_.size(); ++i) {
      if (kernel_threads_[i].status != 0u) continue;
      if (!cpu_.retire_trapped_syscall(i + 1u)) return false;
      auto& t = kernel_threads_[i];
      t = {};
      t.status = 16u; // DORMANT until StartThread.
      t.entry = entry;
      t.stack = stack;
      t.stack_size = stack_size;
      t.gp = gp;
      t.initial_priority = priority;
      t.priority = static_cast<std::uint16_t>(priority);
      t.attr = attr;
      t.option = option;
      return true;
    }
    return cpu_.retire_trapped_syscall(UINT64_MAX);
  }
  case 0x21u: { // DeleteThread(id): only dormant threads can be deleted.
    if (arg0 < 2u || arg0 > kernel_threads_.size() ||
        kernel_threads_[arg0 - 1u].status != 16u)
      return cpu_.retire_trapped_syscall(UINT64_MAX);
    if (!cpu_.retire_trapped_syscall(0u)) return false;
    kernel_threads_[arg0 - 1u] = {};
    return true;
  }
  case 0x22u: { // StartThread(id, arg): ready EE context, not a fake success.
    if (arg0 < 2u || arg0 > kernel_threads_.size() ||
        kernel_threads_[arg0 - 1u].status != 16u)
      return cpu_.retire_trapped_syscall(UINT64_MAX);
    auto& t = kernel_threads_[arg0 - 1u];
    auto ctx = cpu_.save_thread_context();
    ctx.branch_pending = false;
    ctx.pending_target = 0u;
    ctx.state.gpr = {};
    ctx.state.gpr_hi = {};
    ctx.state.pc = t.entry;
    ctx.state.gpr[4] = arg1;
    ctx.state.gpr[28] = t.gp;
    ctx.state.gpr[29] = static_cast<std::uint32_t>(
        (std::uint64_t{t.stack} + t.stack_size - 16u) & ~15ull);
    ctx.state.gpr[31] = kThreadExitTrampoline;
    if (!cpu_.retire_trapped_syscall(0u)) return false;
    t.context = ctx;
    t.status = 2u; // READY. The scheduler dispatches at a safe boundary.
    t.wait_type = 0u;
    t.wait_id = 0u;
    return true;
  }
  case 0x23u: // ExitThread()
  case 0x24u: { // ExitDeleteThread()
    if (guest_syscall_depth_ != 0u) return false;
    if (!cpu_.retire_trapped_syscall(0u, false)) return false;
    const auto exited = current_thread_index_;
    kernel_threads_[exited].status = 16u;
    kernel_threads_[exited].wait_type = 0u;
    if (!switch_to_ready_thread()) {
      // No runnable guest thread remains; do not keep executing a dead one.
      cpu_.request_halt();
      return true;
    }
    if (number == 0x24u) kernel_threads_[exited] = {};
    return true;
  }
  case 0x25u: { // TerminateThread(id) -> DORMANT.
    if (arg0 < 2u || arg0 > kernel_threads_.size() ||
        arg0 == current_thread_index_ + 1u ||
        kernel_threads_[arg0 - 1u].status == 0u)
      return cpu_.retire_trapped_syscall(UINT64_MAX);
    if (!cpu_.retire_trapped_syscall(0u)) return false;
    auto& t = kernel_threads_[arg0 - 1u];
    t.status = 16u;
    t.wait_type = 0u;
    t.wait_id = 0u;
    return true;
  }
  case 0x29u: { // ChangeThreadPriority(id, priority), id=0 is self.
    const auto index = arg0 == 0u ? current_thread_index_ : arg0 - 1u;
    if (arg0 > kernel_threads_.size() || arg1 >= 128u ||
        kernel_threads_[index].status == 0u)
      return cpu_.retire_trapped_syscall(UINT64_MAX);
    const auto previous = kernel_threads_[index].priority;
    if (!cpu_.retire_trapped_syscall(previous)) return false;
    kernel_threads_[index].priority = static_cast<std::uint16_t>(arg1);
    return true;
  }
  case 0x2Fu: // GetThreadId()
    return cpu_.retire_trapped_syscall(current_thread_index_ + 1u);
  case 0x30u: { // ReferThreadStatus(id, ee_thread_status_t*).
    const auto index = arg0 == 0u ? current_thread_index_ : arg0 - 1u;
    if (arg0 > kernel_threads_.size() ||
        kernel_threads_[index].status == 0u ||
        (arg1 & 3u) != 0u || !memory_.valid(arg1, 48u))
      return cpu_.retire_trapped_syscall(UINT64_MAX);
    const auto& t = kernel_threads_[index];
    if (!cpu_.retire_trapped_syscall(0u)) return false;
    const std::array<std::uint32_t, 12> fields{
        t.status, t.entry, t.stack, t.stack_size, t.gp,
        t.initial_priority, t.priority, t.attr, t.option,
        t.wait_type, t.wait_id, t.wakeups};
    for (std::size_t i = 0u; i < fields.size(); ++i)
      memory_.write32(arg1 + static_cast<std::uint32_t>(4u * i), fields[i]);
    return true;
  }
  case 0x32u: { // SleepThread(), waits unless a wakeup is pending.
    auto& t = kernel_threads_[current_thread_index_];
    if (t.wakeups != 0u) {
      if (!cpu_.retire_trapped_syscall(0u)) return false;
      --t.wakeups;
      return true;
    }
    if (!has_ready_thread() || guest_syscall_depth_ != 0u) return false;
    if (!cpu_.retire_trapped_syscall(0u)) return false;
    t.status = 4u;
    t.wait_type = 1u;
    t.wait_id = 0u;
    return switch_to_ready_thread();
  }
  case 0x33u: { // WakeupThread(id).
    if (arg0 == 0u || arg0 > kernel_threads_.size() ||
        kernel_threads_[arg0 - 1u].status == 0u ||
        kernel_threads_[arg0 - 1u].status == 16u)
      return cpu_.retire_trapped_syscall(UINT64_MAX);
    auto& t = kernel_threads_[arg0 - 1u];
    if (!cpu_.retire_trapped_syscall(0u)) return false;
    if (t.status == 4u && t.wait_type == 1u) {
      t.status = 2u;
      t.wait_type = 0u;
    } else if (t.wakeups != UINT32_MAX) {
      ++t.wakeups;
    }
    return true;
  }
  case 0x35u: { // CancelWakeupThread(id), returns old wakeup count.
    if (arg0 == 0u || arg0 > kernel_threads_.size() ||
        kernel_threads_[arg0 - 1u].status == 0u)
      return cpu_.retire_trapped_syscall(UINT64_MAX);
    auto& t = kernel_threads_[arg0 - 1u];
    if (!cpu_.retire_trapped_syscall(t.wakeups)) return false;
    t.wakeups = 0u;
    return true;
  }
  case 0x2Du: { // ReleaseWaitThread(id), only blocked EE threads.
    if (arg0 == 0u || arg0 > kernel_threads_.size() ||
        kernel_threads_[arg0 - 1u].status != 4u)
      return cpu_.retire_trapped_syscall(UINT64_MAX);
    if (!cpu_.retire_trapped_syscall(0u)) return false;
    auto& t = kernel_threads_[arg0 - 1u];
    t.status = 2u;
    t.wait_type = 0u;
    t.wait_id = 0u;
    // A semaphore wait ended without acquisition.
    t.context.state.gpr[2] = UINT64_MAX;
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
  case 0x3Eu: { // EndOfHeap(): return the upper bound configured by SetupHeap.
    // The observed NFS:MW stop at 0x00495504 executes this wrapper.
    // Do not synthesize a boundary if SetupHeap has not succeeded.
    if (startup_heap_end_ == 0u || startup_heap_end_ <= startup_heap_base_)
      return false;
    return cpu_.retire_trapped_syscall(startup_heap_end_);
  }
  case 0x7Fu: // GetMemorySize(): amount of installed EE RDRAM in bytes.
    // The NFSMW ELF calls this through its wrapper at 0x00495984. Report
    // the actual guest RAM capacity so the game's heap/TLB checks agree.
    return cpu_.retire_trapped_syscall(Memory::kRamSize);
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
      for (auto& t : kernel_threads_) {
        if (t.status != 4u || t.wait_type != 2u || t.wait_id != arg0)
          continue;
        t.status = 2u;
        t.wait_type = 0u;
        t.wait_id = 0u;
        t.context.state.gpr[2] = UINT64_MAX;
      }
      return true;
    }
    if (number == 0x42u) {
      // A signal transfers the token directly to one waiting thread.
      for (auto& t : kernel_threads_) {
        if (t.status != 4u || t.wait_type != 2u || t.wait_id != arg0)
          continue;
        if (!cpu_.retire_trapped_syscall(0u)) return false;
        t.status = 2u;
        t.wait_type = 0u;
        t.wait_id = 0u;
        return true;
      }
      if (sema.count >= sema.maximum)
        return cpu_.retire_trapped_syscall(UINT64_MAX);
      if (!cpu_.retire_trapped_syscall(0u)) return false;
      ++sema.count;
      return true;
    }
    if (sema.count == 0) {
      if (number == 0x45u)
        return cpu_.retire_trapped_syscall(UINT64_MAX);
      if (!has_ready_thread() || guest_syscall_depth_ != 0u) return false;
      if (!cpu_.retire_trapped_syscall(0u)) return false;
      auto& t = kernel_threads_[current_thread_index_];
      t.status = 4u;
      t.wait_type = 2u;
      t.wait_id = arg0;
      return switch_to_ready_thread();
    }
    if (!cpu_.retire_trapped_syscall(0u)) return false;
    --sema.count;
    return true;
  }
  case 0x4Au: // SetOsdConfigParam(const ConfigParam*)
  case 0x4Bu: { // GetOsdConfigParam(ConfigParam*): PS2SDK's packed 32-bit word.
    // The observed NFSMW startup passes a pointer near the top of EE RAM.
    // Validate the entire write and reject MMIO/ROM/null destinations.
    // Accept physical EE RAM and its KSEG0/KSEG1 direct aliases.
    const bool kseg_alias = (arg0 & 0xC0000000u) == 0x80000000u;
    const auto physical = kseg_alias ? (arg0 & 0x1FFFFFFFu) : arg0;
    if (arg0 == 0u || (arg0 & 3u) != 0u ||
        physical > Memory::kRamSize - 4u ||
        !memory_.valid(arg0, 4u)) return false;
    // Set and Get are both void. The game updates OSD version bits,
    // verifies the result, and restores the original packed four-byte word.
    const auto incoming = number == 0x4Au ? memory_.read32(arg0) : 0u;
    if (!cpu_.retire_trapped_syscall(0u, false)) return false;
    if (number == 0x4Au) osd_config_ = incoming;
    else memory_.write32(arg0, osd_config_);
    return true; // Preserve $v0 and leave adjacent guest stack words alone.
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
  sif_system_regs_ = {};
  deci2_last_debug_text_ = {};
  deci2_debug_print_count_ = 0u;
  deci2_last_debug_truncated_ = false;
  kernel_semaphores_ = {};
  kernel_threads_ = {};
  current_thread_index_ = 0u;
  thread_quantum_remaining_ = kThreadQuantum;
  intc_handlers_ = {};
  dmac_handlers_ = {};
  // Reset does not zero all EE RAM; explicitly remove old kernel hooks.
  for (std::uint32_t i = 0; i < kBareElfSyscallSlots; ++i)
    memory_.write32(kBareElfSyscallTable + i * 4u, 0u);
  guest_syscall_returns_ = {};
  guest_syscall_depth_ = 0u;
  gif_.reset();
  vif1_.reset();
  if (image_.ok) {
    cpu_.reset(image_.entry);
    cpu_.set_exception_mode(false);
    initialize_kernel_threads();
  }
  else if (memory_.has_bios()) {
    cpu_.reset(0xBFC00000u);
    iop_.reset();
    set_bios_reset_state(cpu_.state());
    cpu_.set_exception_mode(true);
  }
}

} // namespace ps2vita
