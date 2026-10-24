#include "ps2vita/aot.hpp"
#include "ps2vita/emulator.hpp"
#include "ps2vita/execution_budget.hpp"
#include "ps2vita/gs_display_preview.hpp"
#include "screen.hpp"

#include <psp2/ctrl.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#if defined(PS2VITA_PRIVATE_GAME_AOT)
namespace ps2vita {
const AotPackage& generated_private_game_aot_package();
}
#endif

namespace {
constexpr const char* kBootPath = "ux0:data/ps2vita/boot.elf";
constexpr const char* kBiosPath = "ux0:data/ps2vita/bios.bin";
constexpr const char* kProbePath = "ux0:data/ps2vita/startup.txt";
constexpr const char* kFaultPath = "ux0:data/ps2vita/fault.txt";
constexpr const char* kProgressPath = "ux0:data/ps2vita/progress.txt";
constexpr const char* kBenchmarkPath = "ux0:data/ps2vita/benchmark.txt";
constexpr std::uint64_t kProgressInterval = 1000000u;
// Host pacing only, not PS2 timing. Leave room for UI/presentation, while
// replacing the fixed one-slice-per-VBlank throughput ceiling. The deadline
// is soft: synchronous graphics inside one quantum can exceed this budget.
constexpr ps2vita::ExecutionBudget kFrameExecution{8000u, 2000u, 1024u};

std::uint64_t benchmark_clock_microseconds() {
  return sceKernelGetProcessTimeWide();
}

void write_probe(const char* status) {
  sceIoMkdir("ux0:data/ps2vita", 0777);
  const int fd = sceIoOpen(kProbePath, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
  if (fd < 0) return;
  for (const char* cursor = status; *cursor; ++cursor) sceIoWrite(fd, cursor, 1);
  sceIoClose(fd);
}

void truncate_report(const char* path) {
  // Vita3K's host-backed filesystem has historically retained stale trailing
  // bytes after O_TRUNC. Removing the tiny diagnostic first makes each run's
  // report unambiguous on both Vita hardware and Vita3K.
  sceIoRemove(path);
  const int fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
  if (fd >= 0) {
    static constexpr char reset[] = "none\n";
    sceIoWrite(fd, reset, sizeof(reset) - 1);
    sceIoClose(fd);
  }
}

void write_fault(const ps2vita::Emulator& emulator, ps2vita::StopReason reason) {
  char report[768];
  const auto& cpu = emulator.cpu();
  const auto& memory = emulator.memory();
  const auto& state = cpu.state();
  const auto read_debug = [&](std::uint32_t address) {
    return memory.valid(address, 4) ? memory.read32(address) : 0xFFFFFFFFu;
  };
  std::snprintf(report, sizeof(report),
      "reason=%s\npc=%08X\nopcode=%08X\ncycles=%llu\n"
      "v0=%016llX\na0=%016llX\nsp=%016llX\nra=%016llX\n"
      "status=%08X\ncause=%08X\nepc=%08X\nbadvaddr=%08X\n"
      "epc_opcode=%08X\nvector180=%08X,%08X,%08X,%08X\n"
      "vector200=%08X,%08X,%08X,%08X\n",
      ps2vita::stop_reason_name(reason), state.pc, cpu.fault_instruction(),
      static_cast<unsigned long long>(state.cycles),
      static_cast<unsigned long long>(state.gpr[2]),
      static_cast<unsigned long long>(state.gpr[4]),
      static_cast<unsigned long long>(state.gpr[29]),
      static_cast<unsigned long long>(state.gpr[31]),
      state.cop0[12], state.cop0[13], state.cop0[14], state.cop0[8],
      read_debug(state.cop0[14]),
      read_debug(0x80000180u), read_debug(0x80000184u),
      read_debug(0x80000188u), read_debug(0x8000018Cu),
      read_debug(0x80000200u), read_debug(0x80000204u),
      read_debug(0x80000208u), read_debug(0x8000020Cu));
  const int fd = sceIoOpen(kFaultPath, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
  if (fd < 0) return;
  sceIoWrite(fd, report, std::strlen(report));
  sceIoClose(fd);
}

void write_progress(const ps2vita::Emulator& emulator) {
  char report[256];
  const auto& state = emulator.cpu().state();
  const auto opcode = emulator.memory().read32(state.pc);
  std::snprintf(report, sizeof(report),
      "pc=%08X\nopcode=%08X\ncycles=%llu\nfast_path_instructions=%llu\n"
      "aot_trace_entries=%llu\naot_trace_horizon_fallbacks=%llu\n"
      "v0=%016llX\na0=%016llX\nsp=%016llX\nra=%016llX\n"
      "status=%08X\ncause=%08X\nepc=%08X\nbadvaddr=%08X\n",
      state.pc, opcode, static_cast<unsigned long long>(state.cycles),
      static_cast<unsigned long long>(state.fast_path_instructions),
      static_cast<unsigned long long>(state.aot_trace_entries),
      static_cast<unsigned long long>(state.aot_trace_horizon_fallbacks),
      static_cast<unsigned long long>(state.gpr[2]),
      static_cast<unsigned long long>(state.gpr[4]),
      static_cast<unsigned long long>(state.gpr[29]),
      static_cast<unsigned long long>(state.gpr[31]), state.cop0[12],
      state.cop0[13], state.cop0[14], state.cop0[8]);
  const int fd = sceIoOpen(kProgressPath, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
  if (fd < 0) return;
  sceIoWrite(fd, report, std::strlen(report));
  sceIoClose(fd);
}

void write_benchmark(const ps2vita::AotBenchmarkResult& result) {
  char report[768];
  std::snprintf(report, sizeof(report),
      "schema=astrart-performance-v1\nmatched=%u\niterations=%u\nsamples=%u\n"
      "guest_instructions=%llu\ninterpreter_us=%llu\nnative_us=%llu\n"
      "speedup_x100=%u\ninterpreter_checksum=%016llX\n"
      "native_checksum=%016llX\ntrace_probe_us=%llu\n"
      "trace_probe_guest_instructions=%llu\ntrace_entries=%llu\n"
      "trace_horizon_fallbacks=%llu\ntrace_probe_checksum=%016llX\n"
      "timing_note=Vita3K validates correctness only; physical Vita decides performance\n",
      result.matched ? 1u : 0u, result.iterations, result.samples,
      static_cast<unsigned long long>(result.guest_instructions),
      static_cast<unsigned long long>(result.interpreter_microseconds),
      static_cast<unsigned long long>(result.aot_microseconds),
      result.speedup_x100,
      static_cast<unsigned long long>(result.interpreter_checksum),
      static_cast<unsigned long long>(result.aot_checksum),
      static_cast<unsigned long long>(result.trace_probe_microseconds),
      static_cast<unsigned long long>(result.trace_probe_guest_instructions),
      static_cast<unsigned long long>(result.trace_entries),
      static_cast<unsigned long long>(result.trace_horizon_fallbacks),
      static_cast<unsigned long long>(result.trace_probe_checksum));
  sceIoRemove(kBenchmarkPath);
  const int fd = sceIoOpen(kBenchmarkPath,
                           SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
  if (fd < 0) return;
  sceIoWrite(fd, report, std::strlen(report));
  sceIoClose(fd);
}

void put16(std::vector<std::uint8_t>& bytes, std::size_t at, std::uint16_t value) {
  bytes[at] = static_cast<std::uint8_t>(value);
  bytes[at + 1] = static_cast<std::uint8_t>(value >> 8);
}

void put32(std::vector<std::uint8_t>& bytes, std::size_t at, std::uint32_t value) {
  put16(bytes, at, static_cast<std::uint16_t>(value));
  put16(bytes, at + 2, static_cast<std::uint16_t>(value >> 16));
}

std::vector<std::uint8_t> builtin_smoke_elf() {
  std::vector<std::uint8_t> bytes(0x10C, 0);
  bytes[0] = 0x7F; bytes[1] = 'E'; bytes[2] = 'L'; bytes[3] = 'F';
  bytes[4] = 1; bytes[5] = 1; bytes[6] = 1;
  put16(bytes, 16, 2); put16(bytes, 18, 8); put32(bytes, 20, 1);
  put32(bytes, 24, 0x1000); put32(bytes, 28, 52);
  put16(bytes, 40, 52); put16(bytes, 42, 32); put16(bytes, 44, 1);
  put32(bytes, 52, 1); put32(bytes, 56, 0x100); put32(bytes, 60, 0x1000);
  put32(bytes, 64, 0x1000); put32(bytes, 68, 12); put32(bytes, 72, 16);
  put32(bytes, 76, 5); put32(bytes, 80, 16);
  put32(bytes, 0x100, 0x2402002A); // addiu v0,zero,42
  put32(bytes, 0x104, 0xAC022000); // sw v0,0x2000(zero)
  put32(bytes, 0x108, 0x0000000D); // break
  return bytes;
}

bool read_file(const char* path, std::vector<std::uint8_t>& bytes) {
  const int fd = sceIoOpen(path, SCE_O_RDONLY, 0);
  if (fd < 0) return false;
  const auto size = sceIoLseek(fd, 0, SCE_SEEK_END);
  sceIoLseek(fd, 0, SCE_SEEK_SET);
  if (size <= 0 || size > 32 * 1024 * 1024) { sceIoClose(fd); return false; }
  bytes.resize(static_cast<std::size_t>(size));
  std::size_t done = 0;
  while (done < bytes.size()) {
    const int got = sceIoRead(fd, bytes.data() + done, bytes.size() - done);
    if (got <= 0) { sceIoClose(fd); return false; }
    done += static_cast<std::size_t>(got);
  }
  sceIoClose(fd);
  return true;
}

void draw(Screen& screen, const ps2vita::Emulator& emu, const char* message,
          const ps2vita::AotProbeResult& aot_probe,
          const ps2vita::AotProbeResult& aot_chain_probe,
          const ps2vita::AotBenchmarkResult& benchmark, bool benchmark_ready,
          bool running,
          bool show_gs, bool show_draw_target, ps2vita::GsLinearDisplayPreview& display_preview) {
  constexpr std::uint32_t bg = 0xFF100C18;
  constexpr std::uint32_t panel = 0xFF21182E;
  constexpr std::uint32_t ink = 0xFFF4ECFF;
  constexpr std::uint32_t dim = 0xFFB7A8C8;
  constexpr std::uint32_t accent = 0xFFFF4E9A;
  if (show_gs) {
    screen.clear(0xFF000000);
    char heading[96];
    char detail[96];
    if (show_draw_target) {
      screen.blit_160x112(emu.gs().pixels());
      std::snprintf(heading, sizeof(heading), "GS DRAW TARGET / L: VIEW / R: RETURN");
      std::snprintf(detail, sizeof(detail), "FRAME %016llX / NOT THE DISPLAY BUFFER",
          static_cast<unsigned long long>(emu.gs().color_frame()));
    } else {
      const auto status = display_preview.update_source(emu.memory().read64(0x12000000u),
          emu.memory().read64(0x12000070u), emu.memory().read64(0x12000090u),
          emu.memory().read64(0x12000080u), emu.memory().read64(0x120000A0u),
          emu.memory().read64(0x12000010u),
          emu.memory().read64(0x12000020u),
          [&](std::uint32_t address) { return emu.gif().read_local32(address); });
      screen.blit_160x112(display_preview.pixels().data());
      const auto& selected = display_preview.selection();
      if (selected.circuit != 0u)
        std::snprintf(heading, sizeof(heading), "GS DISPFB%u SOURCE REGION / L: VIEW / R: RETURN",
            selected.circuit);
      else
        std::snprintf(heading, sizeof(heading), "GS DISPLAY PROBE / L: VIEW / R: RETURN");
      if (status == ps2vita::GsLinearDisplayStatus::Ready) {
        std::snprintf(detail, sizeof(detail), "BASE %06X STRIDE %u ORIGIN %u,%u / SOURCE %uX%u%s",
            selected.framebuffer.base_bytes(), selected.framebuffer.width_pixels(),
            selected.framebuffer.dbx, selected.framebuffer.dby,
            display_preview.source_width(), display_preview.source_height(),
            display_preview.source_width() > 640u || display_preview.source_height() > 448u ?
                " / CROP 640X448" : "");
      } else {
        const char* reason = status == ps2vita::GsLinearDisplayStatus::Disabled ?
            "PMODE DISABLED" : status == ps2vita::GsLinearDisplayStatus::MultipleCircuits ?
            "DUAL-CIRCUIT MIXING NOT IMPLEMENTED" :
            status == ps2vita::GsLinearDisplayStatus::ZeroWidth ? "ZERO BUFFER WIDTH" :
            status == ps2vita::GsLinearDisplayStatus::UnsupportedMode ? "INTERLACED MODE NOT SUPPORTED" :
            status == ps2vita::GsLinearDisplayStatus::EmptyExtent ? "EMPTY SOURCE EXTENT" :
            "UNSUPPORTED DISPLAY PIXEL FORMAT";
        std::snprintf(detail, sizeof(detail), "%s / L FOR DRAW-TARGET DIAGNOSTIC", reason);
      }
    }
    screen.rect(0, 0, 960, 40, 0xFF100C18);
    screen.text(18, 10, heading, ink, 2);
    screen.text(18, 504, detail, dim, 1);
    screen.text(18, 522, "DIAGNOSTIC ONLY / NOT SCANOUT, FIELDS OR NATIVE GS SWIZZLING", dim, 1);
    screen.present();
    return;
  }
  screen.clear(bg);
  screen.rect(0, 0, 960, 70, accent);
  screen.text(28, 19, "ASTRARECOMP / EE MONITOR", 0xFF180C13, 3);
  screen.rect(28, 96, 904, 274, panel);
  screen.text(52, 116, message, ink, 2);

  char line[96];
  const auto& cpu = emu.cpu().state();
  std::snprintf(line, sizeof(line), "STATE: %s", running ? "RUNNING" : ps2vita::stop_reason_name(emu.cpu().stop_reason()));
  screen.text(52, 164, line, running ? 0xFF6CFFB5 : accent, 2);
  std::snprintf(line, sizeof(line), "PC: %08X    CYCLES: %llu", cpu.pc,
                static_cast<unsigned long long>(cpu.cycles));
  screen.text(52, 196, line, ink, 2);
  std::snprintf(line, sizeof(line), "V0: %08X    A0: %08X    SP: %08X",
                static_cast<unsigned>(cpu.gpr[2]), static_cast<unsigned>(cpu.gpr[4]),
                static_cast<unsigned>(cpu.gpr[29]));
  screen.text(52, 228, line, dim, 2);
  std::snprintf(line, sizeof(line), "NATIVE FAST: %llu INSTRUCTIONS",
                static_cast<unsigned long long>(cpu.fast_path_instructions));
  screen.text(52, 260, line, dim, 2);
  std::snprintf(line, sizeof(line), "PS2R AOT T0: %s   INTERP:%u   NATIVE:%u",
                aot_probe.matched ? "PASS" : "FAIL",
                aot_probe.interpreter_value, aot_probe.aot_value);
  screen.text(52, 292, line, aot_probe.matched ? 0xFF6CFFB5 : accent, 2);
  std::snprintf(line, sizeof(line), "PS2R AOT T1 CALL/RET: %s   NATIVE:%u",
                aot_chain_probe.matched ? "PASS" : "FAIL",
                aot_chain_probe.aot_value);
  screen.text(52, 316, line,
              aot_chain_probe.matched ? 0xFF6CFFB5 : accent, 2);

  if (benchmark_ready) {
    std::snprintf(line, sizeof(line), "AOT PERF: %s  %u.%02uX  %llu GUEST OPS",
                  benchmark.matched ? "PASS" : "FAIL",
                  benchmark.speedup_x100 / 100u, benchmark.speedup_x100 % 100u,
                  static_cast<unsigned long long>(benchmark.guest_instructions));
    screen.text(52, 344, line, benchmark.matched ? 0xFF6CFFB5 : accent, 2);
  } else {
    screen.text(52, 344, "AOT PERF: NOT RUN / L TO START", dim, 2);
  }

  screen.text(38, 387, "CROSS  RUN / PAUSE", ink, 2);
  screen.text(38, 419, "SQUARE STEP    TRIANGLE RELOAD", ink, 2);
  screen.text(38, 451, "CIRCLE BIOS   R GS VIEW   START RESET", ink, 2);
  screen.text(38, 503, "L PERF TEST    SELECT EXIT", dim, 2);
  screen.present();
}
}

int main() {
  write_probe("main entered\n");
  truncate_report(kFaultPath);
  truncate_report(kProgressPath);
  Screen screen;
  if (!screen.init()) return -1;
  write_probe("screen ready\n");
  sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);

  ps2vita::Emulator emulator;
  const auto aot_probe = ps2vita::run_phase0_aot_probe();
  const auto aot_chain_probe = ps2vita::run_phase0_aot_chain_probe();
  // Keep the first UI frame independent of the expensive interpreter-vs-AOT
  // benchmark. Physical Vita startup must stay responsive even if profiling
  // stalls or is much slower than the host. L runs a smaller optional sample.
  ps2vita::AotBenchmarkResult benchmark{};
  bool benchmark_ready = false;
  std::vector<std::uint8_t> image;
  char message[96];
  std::snprintf(message, sizeof(message), "PHASE-0 AOT %s / I=%u A=%u",
                aot_probe.matched ? "PASS" : "FAIL",
                aot_probe.interpreter_value, aot_probe.aot_value);
  bool running = false;
  bool show_gs = false;
  bool show_draw_target = false;
  // Reused host-only storage, not 72 KiB on the Vita thread's stack. No
  // per-frame allocation or guest state changes; every update replaces it.
  static ps2vita::GsLinearDisplayPreview display_preview;
  std::uint32_t previous = 0;
  std::uint64_t next_progress_cycle = kProgressInterval;
  std::uint64_t last_progress_cycle = 0;
  const auto reset_progress = [&]() {
    next_progress_cycle = kProgressInterval;
    last_progress_cycle = 0;
    truncate_report(kProgressPath);
  };

  emulator.gs().clear(0xFF120A20);
  emulator.gs().triangle({8, 100, 200, 0xFFFF4E9A},
                         {80, 8, 100, 0xFF6CFFB5},
                         {152, 100, 200, 0xFFFFC857});
  emulator.gs().line({8, 104, 50, 0xFFFFFFFF}, {152, 104, 50, 0xFFFFFFFF});

  image = builtin_smoke_elf();
  const auto initial = emulator.load_elf(image.data(), image.size());
  if (!initial.ok)
    std::snprintf(message, sizeof(message), "BUILT-IN LOAD ERROR: %s", initial.error);

  std::vector<std::uint8_t> bios;
  if (read_file(kBiosPath, bios) && emulator.load_bios(bios.data(), bios.size()) &&
      emulator.boot_bios()) {
    running = true;
    std::snprintf(message, sizeof(message), "BIOS AUTOLOADED / RUNNING");
  }
  write_probe("monitor loop ready\n");
  bool first_frame_presented = false;

  while (true) {
    SceCtrlData pad{};
    sceCtrlPeekBufferPositive(0, &pad, 1);
    const std::uint32_t pressed = pad.buttons & ~previous;
    previous = pad.buttons;

    if (pressed & SCE_CTRL_SELECT) break;
    if (pressed & SCE_CTRL_RTRIGGER) show_gs = !show_gs;
    if (show_gs && (pressed & SCE_CTRL_LTRIGGER)) show_draw_target = !show_draw_target;
    if (!show_gs && !running && (pressed & SCE_CTRL_LTRIGGER)) {
      std::snprintf(message, sizeof(message), "AOT BENCHMARK RUNNING / PLEASE WAIT");
      draw(screen, emulator, message, aot_probe, aot_chain_probe,
           benchmark, benchmark_ready, running, show_gs, show_draw_target,
           display_preview);
      write_probe("optional AOT benchmark started\n");
      benchmark = ps2vita::run_phase0_aot_benchmark(
          benchmark_clock_microseconds, 256u, 3u);
      benchmark_ready = true;
      write_benchmark(benchmark);
      write_probe(benchmark.matched ? "optional AOT benchmark passed\n" :
                                      "optional AOT benchmark failed\n");
      std::snprintf(message, sizeof(message), "AOT BENCHMARK %s",
                    benchmark.matched ? "PASSED" : "FAILED");
    }
    if (pressed & SCE_CTRL_TRIANGLE) {
      running = false;
      image.clear();
      bool loaded_external = read_file(kBootPath, image);
      if (!loaded_external) {
        image = builtin_smoke_elf();
      }
      if (image.empty()) {
        std::snprintf(message, sizeof(message), "COULD NOT READ ANY ELF");
      } else {
        const auto result = emulator.load_elf(image.data(), image.size());
        reset_progress();
#if defined(PS2VITA_PRIVATE_GAME_AOT)
        if (result.ok && loaded_external) {
          const bool enabled = emulator.cpu().set_startup_aot_package(
              &ps2vita::generated_private_game_aot_package());
          write_probe(enabled ? "private title startup AOT armed\n" :
                                "private title AOT validation failed\n");
        }
#endif
        if (result.ok) std::snprintf(message, sizeof(message), "%s / ENTRY %08X",
            loaded_external ? "EXTERNAL ELF LOADED" : "BUILT-IN TEST LOADED", result.entry);
        else std::snprintf(message, sizeof(message), "LOAD ERROR: %s", result.error);
      }
    }
    if (pressed & SCE_CTRL_CIRCLE) {
      running = false;
      image.clear();
      if (!read_file(kBiosPath, image)) {
        std::snprintf(message, sizeof(message), "COULD NOT READ BIOS.BIN");
      } else if (!emulator.load_bios(image.data(), image.size())) {
        std::snprintf(message, sizeof(message), "BIOS MUST BE EXACTLY 4 MIB");
      } else {
        emulator.boot_bios();
        reset_progress();
        std::snprintf(message, sizeof(message), "BIOS MAPPED / RESET BFC00000");
      }
    }
    if ((pressed & SCE_CTRL_CROSS) && emulator.ready()) running = !running;
    if ((pressed & SCE_CTRL_START) && emulator.ready()) {
      emulator.reset();
      running = false;
      reset_progress();
    }
    if ((pressed & SCE_CTRL_SQUARE) && emulator.ready()) {
      // Manual stepping must use the same EE/IOP/DMA/graphics scheduler.
      const auto reason = emulator.run_slice(1u);
      running = false;
      if (reason != ps2vita::StopReason::StepLimit) {
        write_fault(emulator, reason);
        std::snprintf(message, sizeof(message), "STOPPED: %s", ps2vita::stop_reason_name(reason));
      }
    }
    if (running) {
      auto reason = ps2vita::StopReason::StepLimit;
      ps2vita::run_execution_budget(kFrameExecution,
          benchmark_clock_microseconds, [&](std::uint32_t quantum) {
            reason = emulator.run_slice(quantum);
            return reason == ps2vita::StopReason::StepLimit;
          });
      // Controls/presentation and diagnostic I/O remain once per UI frame.
      // Pacing limits/clock faults are NOT guest stop reasons.
      const auto cycles = emulator.cpu().state().cycles;
      if (ps2vita::execution_progress_due(cycles, next_progress_cycle,
                                          last_progress_cycle)) {
        write_progress(emulator);
        last_progress_cycle = cycles;
        next_progress_cycle = ps2vita::next_execution_progress(cycles, kProgressInterval);
      }
      if (reason != ps2vita::StopReason::StepLimit) {
        running = false;
        write_fault(emulator, reason);
        std::snprintf(message, sizeof(message), "STOPPED: %s", ps2vita::stop_reason_name(reason));
      }
    }
    draw(screen, emulator, message, aot_probe, aot_chain_probe,
         benchmark, benchmark_ready, running, show_gs, show_draw_target,
         display_preview);
    if (!first_frame_presented) {
      write_probe("first monitor frame presented\n");
      first_frame_presented = true;
    }
  }

  screen.shutdown();
  sceKernelExitProcess(0);
  return 0;
}
