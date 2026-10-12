#include "ps2vita/emulator.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

// Owned functional-transport oracles. These do not model DIRECTHL priority,
// GIF arbitration/backpressure, IRQ-bit interrupt delivery or hardware timing.
namespace {
using Bytes = std::vector<std::uint8_t>;
unsigned checks = 0u, failures = 0u;
constexpr std::array<unsigned, 4> kCommands{{0x50u, 0x51u, 0xD0u, 0xD1u}};
constexpr std::uint64_t kTag = 0x1000000000008001ull;

void check(bool condition, const std::string& name) {
  ++checks;
  if (!condition) {
    ++failures;
    if (failures <= 32u) std::fprintf(stderr, "FAIL: %s\n", name.c_str());
  }
}

std::string context(unsigned command, const char* name) {
  char prefix[24];
  std::snprintf(prefix, sizeof(prefix), "opcode %02X: ", command);
  return std::string(prefix) + name;
}

void append(Bytes& bytes, std::uint64_t value, unsigned width = 4u) {
  for (unsigned byte = 0u; byte < width; ++byte)
    bytes.push_back(static_cast<std::uint8_t>(value >> (byte * 8u)));
}

Bytes gif_packet(std::uint32_t marker) {
  Bytes bytes;
  append(bytes, kTag, 8u); append(bytes, 0xEu, 8u);
  append(bytes, marker, 8u); append(bytes, 1u, 8u);
  return bytes;
}

Bytes transfer(unsigned command, const Bytes& bytes) {
  Bytes stream;
  append(stream, (command << 24u) | static_cast<std::uint32_t>(bytes.size() / 16u));
  stream.insert(stream.end(), bytes.begin(), bytes.end());
  return stream;
}

void store(ps2vita::Memory& memory, std::uint32_t address, const Bytes& bytes) {
  for (std::size_t byte = 0u; byte < bytes.size(); ++byte)
    memory.write8(address + static_cast<std::uint32_t>(byte), bytes[byte]);
}

void check_census(const ps2vita::Vif1& vif, unsigned command, unsigned count = 1u) {
  const auto opcode = command & 0x7Fu;
  check(vif.command_counts()[opcode] == count &&
            vif.command_counts()[opcode == 0x50u ? 0x51u : 0x50u] == 0u,
        context(command, "DIRECT and DIRECTHL census buckets stay distinct after IRQ masking"));
}

void fragmented_payloads() {
  // The VIF command word is intact. Payload chunks split every byte of the
  // two-qword GIFtag/register packet, including its 16-byte GIF header.
  for (const auto command : kCommands) {
    for (std::size_t quantum = 1u; quantum <= 32u; ++quantum) {
      ps2vita::Memory memory;
      ps2vita::Vif1 vif(memory);
      vif.enable_command_census(true);
      const auto payload = gif_packet(0xD1000004u); // Command-looking payload is opaque.
      Bytes start;
      append(start, 0x01000101u);
      // NUM bits are not a byte count for these immediate-QWC commands.
      append(start, (command << 24u) | 0x00AB0002u);
      const auto begun = vif.submit(start.data(), start.size());
      check(begun && vif.pending_direct_bytes() == 32u && !vif.has_gif_packet(),
            context(command, "intact command starts exactly two qwords without early output"));
      if (!begun) continue; // A broken decoder must fail, not reinterpret arbitrary payload.
      std::size_t cursor = 0u, submits = 1u;
      while (cursor < payload.size()) {
        const auto bytes = std::min(quantum, payload.size() - cursor);
        Bytes input(payload.begin() + static_cast<std::ptrdiff_t>(cursor),
                    payload.begin() + static_cast<std::ptrdiff_t>(cursor + bytes));
        const bool last = cursor + bytes == payload.size();
        if (last) {
          append(input, 0x01000404u); // Following command begins after the exact payload.
          append(input, 0x6C010005u); // Following UNPACK has one owned V4-32 qword.
          for (const auto word : {0x12345678u, 0x89ABCDEFu, 0x01020304u, 0xFFEEDDCCu})
            append(input, word);
        }
        check(vif.submit(input.data(), input.size()),
              context(command, "every bounded payload fragment and its suffix is accepted"));
        ++submits;
        cursor += bytes;
        check(vif.pending_direct_bytes() == payload.size() - cursor &&
                  vif.has_gif_packet() == last && vif.cycle() == (last ? 0x0404u : 0x0101u),
              context(command, "same quanta complete only the payload and never consume the suffix early"));
      }
      Bytes packet;
      check(vif.pop_gif_packet(packet) && packet == payload && !vif.has_gif_packet(),
            context(command, "fragmented transfer emits exactly the original byte sequence once"));
      const auto saved = packet;
      check(!vif.pop_gif_packet(packet) && packet == saved,
            context(command, "second pop leaves the output unchanged"));
      const std::array<std::uint32_t, 4> expected{{0x12345678u, 0x89ABCDEFu, 0x01020304u, 0xFFEEDDCCu}};
      bool exact_unpack = true;
      for (unsigned lane = 0u; lane < expected.size(); ++lane)
        exact_unpack = exact_unpack && memory.vu1_data_word(static_cast<std::uint16_t>(80u + lane * 4u)) == expected[lane];
      check(exact_unpack && vif.vectors_unpacked() == 1u &&
                vif.command_counts()[0x01u] == 2u && vif.command_counts()[0x6Cu] == 1u &&
                vif.command_counts()[0x7Fu] == 0u && vif.packets_submitted() == submits &&
                vif.packets_rejected() == 0u && vif.first_unsupported_packet() == 0u &&
                vif.vu1().pairs_executed() == 0u,
            context(command, "following VIF commands execute, with no payload reinterpretation or VU execution"));
      check_census(vif, command);
    }
  }
}

void command_header_and_reset_boundaries() {
  for (const auto command : kCommands) {
    for (std::size_t size = 1u; size < 4u; ++size) {
      ps2vita::Memory memory;
      ps2vita::Vif1 vif(memory);
      Bytes header;
      append(header, (command << 24u) | 2u);
      check(!vif.submit(header.data(), size) && vif.packets_rejected() == 1u &&
                vif.pending_direct_bytes() == 0u && !vif.has_gif_packet(),
            context(command, "split VIF command words remain rejected, not newly buffered"));
    }
    ps2vita::Memory memory;
    ps2vita::Vif1 vif(memory);
    vif.enable_command_census(true);
    const auto payload = gif_packet(0x51000002u);
    const auto stream = transfer(command, payload);
    const auto begun = vif.submit(stream.data(), 11u);
    check(begun && vif.pending_direct_bytes() == 25u && !vif.has_gif_packet(),
          context(command, "partial transfer has exactly 25 outstanding bytes"));
    vif.reset();
    check(vif.pending_direct_bytes() == 0u && !vif.has_gif_packet() &&
              vif.packets_submitted() == 0u && vif.packets_rejected() == 0u &&
              vif.command_counts()[command & 0x7Fu] == 0u,
          context(command, "reset cancels pending payload and clears command census"));
    check(vif.submit(stream.data(), stream.size()),
          context(command, "complete fresh transfer is accepted after partial reset"));
    Bytes packet;
    check(vif.pop_gif_packet(packet) && packet == payload && !vif.has_gif_packet(),
          context(command, "reset cannot leak or splice the abandoned payload"));
    check_census(vif, command);
  }
}

void mixed_direct_census() {
  for (const bool irq : {false, true}) {
    for (const bool hl_first : {false, true}) {
      ps2vita::Memory memory;
      ps2vita::Vif1 vif(memory);
      vif.enable_command_census(true);
      const auto first = gif_packet(0xD0000002u), second = gif_packet(0x51000002u);
      const unsigned a = (hl_first ? 0x51u : 0x50u) | (irq ? 0x80u : 0u);
      const unsigned b = (hl_first ? 0x50u : 0x51u) | (irq ? 0x80u : 0u);
      auto stream = transfer(a, first);
      const auto tail = transfer(b, second);
      stream.insert(stream.end(), tail.begin(), tail.end());
      append(stream, 0x01001212u);
      const auto accepted = vif.submit(stream.data(), stream.size());
      check(accepted && vif.cycle() == 0x1212u && vif.command_counts()[0x50u] == 1u &&
                vif.command_counts()[0x51u] == 1u && vif.packets_rejected() == 0u,
            "mixed DIRECT/HL and masked IRQ forms remain separately counted in stream order");
      if (!accepted) continue;
      Bytes packet;
      check(vif.pop_gif_packet(packet) && packet == first &&
                vif.pop_gif_packet(packet) && packet == second && !vif.has_gif_packet(),
            "mixed DIRECT/HL outputs remain FIFO without interpreting their command-looking payloads");
    }
  }
}

void queue_path1(ps2vita::Memory& memory, ps2vita::Vu1& vu, const Bytes& packet) {
  vu.reset();
  memory.vu1_store_micro_word(0u, 0x800016FCu); // XGKICK VI2.
  memory.vu1_store_micro_word(4u, 0x400002FFu); // E + upper NOP.
  memory.vu1_store_micro_word(8u, 0x8000033Cu);
  memory.vu1_store_micro_word(12u, 0x2FFu);
  store(memory, ps2vita::Memory::kVu1DataBase + 64u, packet);
  vu.state().vi[2] = 4u;
  vu.start(0u);
  vu.run(2u);
  check(vu.has_path1_packet() && vu.pairs_executed() == 2u && vu.cycles_executed() == 4u &&
            !vu.running() && !vu.path1_active() && vu.path1_tags_rejected() == 0u,
        "owned two-pair VU producer completes PATH1 without new transfer timing");
}

void path1_forward_order() {
  for (const auto command : kCommands) {
    ps2vita::Memory memory;
    ps2vita::Vif1 vif(memory);
    const auto a = gif_packet(0x80111111u), b = gif_packet(0x80222222u);
    const auto c = gif_packet(0x80333333u), d = gif_packet(0x80444444u);
    const auto earlier = transfer(0x50u, a);
    check(vif.submit(earlier.data(), earlier.size()), "earlier DIRECT queues before later PATH1");
    queue_path1(memory, vif.vu1(), b);
    Bytes header;
    append(header, (command << 24u) | 2u);
    const auto begun = vif.submit(header.data(), header.size());
    check(begun && vif.pending_direct_bytes() == 32u && !vif.vu1().has_path1_packet(),
          context(command, "header preserves functional PATH1 order before an unfinished forward payload"));
    if (!begun) continue;
    Bytes packet;
    check(vif.pop_gif_packet(packet) && packet == a &&
              vif.pop_gif_packet(packet) && packet == b && !vif.has_gif_packet(),
          context(command, "prior DIRECT then completed PATH1 drain before current payload completion"));
    check(vif.submit(c.data(), 7u) && !vif.has_gif_packet() &&
              vif.submit(c.data() + 7u, c.size() - 7u) && vif.has_gif_packet(),
          context(command, "current forward payload remains independently fragmentable"));
    queue_path1(memory, vif.vu1(), d);
    const auto before_cycles = vif.vu1().cycles_executed();
    check(vif.pop_gif_packet(packet) && packet == c &&
              vif.pop_gif_packet(packet) && packet == d && !vif.has_gif_packet() &&
              vif.vu1().cycles_executed() == before_cycles,
          context(command, "current forward packet precedes later PATH1 without pop-driven VU advancement"));
  }
}

void qwc_boundaries() {
  for (const auto command : kCommands) {
    for (const unsigned immediate : {0u, 0xFFFFu}) {
      ps2vita::Memory memory;
      ps2vita::Vif1 vif(memory);
      vif.enable_command_census(true);
      const std::size_t size = static_cast<std::size_t>(immediate == 0u ? 65536u : 65535u) * 16u;
      Bytes payload(size);
      for (std::size_t byte = 0u; byte < size; ++byte)
        payload[byte] = static_cast<std::uint8_t>((byte * 29u) ^ 0x5Au);
      // Unsupported VIF command-looking bytes must still be pure payload.
      payload[0] = 0u; payload[1] = 0u; payload[2] = 0u; payload[3] = 0x7Fu;
      Bytes start;
      append(start, 0x01000101u);
      append(start, (command << 24u) | immediate);
      const auto begun = vif.submit(start.data(), start.size());
      check(begun && vif.pending_direct_bytes() == size && !vif.has_gif_packet(),
            context(command, "QWC0 means 65536 qwords; QWCffff is exactly one qword shorter"));
      if (!begun) continue;
      constexpr std::array<std::size_t, 5> chunks{{1u, 3u, 7u, 16381u, 65536u}};
      std::size_t cursor = 0u, part = 0u;
      while (cursor < size) {
        const auto bytes = std::min(chunks[part++ % chunks.size()], size - cursor);
        Bytes input(payload.begin() + static_cast<std::ptrdiff_t>(cursor),
                    payload.begin() + static_cast<std::ptrdiff_t>(cursor + bytes));
        const bool last = cursor + bytes == size;
        if (last) append(input, 0x01007F7Fu);
        check(vif.submit(input.data(), input.size()),
              context(command, "bounded large payload fragments do not execute embedded VIF-looking data"));
        cursor += bytes;
        check(vif.pending_direct_bytes() == size - cursor && vif.has_gif_packet() == last &&
                  vif.cycle() == (last ? 0x7F7Fu : 0x0101u),
              context(command, "large transfer and its following marker obey the exact immediate boundary"));
      }
      Bytes packet;
      check(vif.pop_gif_packet(packet) && packet == payload && !vif.has_gif_packet() &&
                vif.packets_rejected() == 0u && vif.command_counts()[0x7Fu] == 0u &&
                vif.command_counts()[0x01u] == 2u && vif.census_bytes() == size + 12u,
            context(command, "full bounded payload is byte-exact, single-delivery and opaque to the VIF decoder"));
      check_census(vif, command);
    }
  }
}

void malformed_forwarding_is_unchanged() {
  for (const auto command : kCommands) {
    ps2vita::Memory memory;
    ps2vita::Vif1 vif(memory);
    auto payload = gif_packet(0x7F000000u);
    // This tag requests 1024 PACKED qwords, but only one is supplied. The
    // existing Gif parser buffers it without drawing; it does not reject it.
    for (unsigned byte = 0u; byte < 8u; ++byte)
      payload[byte] = static_cast<std::uint8_t>(0x1000000000008400ull >> (byte * 8u));
    const auto stream = transfer(command, payload);
    const auto accepted = vif.submit(stream.data(), stream.size());
    check(accepted && vif.packets_rejected() == 0u,
          context(command, "malformed/incomplete GIF bytes are forwarded, not reinterpreted as VIF commands"));
    if (!accepted) continue;
    Bytes packet;
    check(vif.pop_gif_packet(packet) && packet == payload,
          context(command, "forwarding does not alter malformed GIF bytes or synthesize a replacement"));
    ps2vita::Gs gs;
    ps2vita::Gif gif(gs);
    check(gif.submit(packet.data(), packet.size()) && gif.pending_bytes() == payload.size() &&
              gif.packets_rejected() == 0u && gif.packed_tags() == 0u &&
              gif.points_emitted() == 0u && gif.lines_emitted() == 0u &&
              gif.triangles_emitted() == 0u && gif.sprites_emitted() == 0u &&
              gif.local_bytes_written() == 0u,
          context(command, "downstream incomplete-tag buffering stays unchanged and emits no partial draw"));

    vif.reset();
    auto rejected = transfer(command, gif_packet(0x80ABCDEFu));
    append(rejected, 0x7F000000u);
    check(!vif.submit(rejected.data(), rejected.size()) && vif.packets_rejected() == 1u &&
              vif.first_unsupported_code() == 0x7F000000u && vif.first_unsupported_offset() == 36u &&
              vif.first_unsupported_size() == 40u && vif.has_gif_packet(),
          context(command, "a later unsupported VIF command still rejects without losing completed output"));
    check(vif.pop_gif_packet(packet) && packet == gif_packet(0x80ABCDEFu) && !vif.has_gif_packet(),
          context(command, "supported payload survives a following unsupported VIF opcode exactly once"));
  }
}

Bytes owned_elf() {
  Bytes elf(0x108u, 0u);
  const auto put = [&](std::size_t offset, std::uint32_t value, unsigned width) {
    for (unsigned byte = 0u; byte < width; ++byte)
      elf[offset + byte] = static_cast<std::uint8_t>(value >> (byte * 8u));
  };
  put(0u, 0x464C457Fu, 4u); put(4u, 0x010101u, 3u);
  put(16u, 2u, 2u); put(18u, 8u, 2u); put(20u, 1u, 4u);
  put(24u, 0x1000u, 4u); put(28u, 52u, 4u);
  put(40u, 52u, 2u); put(42u, 32u, 2u); put(44u, 1u, 2u);
  put(52u, 1u, 4u); put(56u, 0x100u, 4u);
  put(60u, 0x1000u, 4u); put(64u, 0x1000u, 4u);
  put(68u, 8u, 4u); put(72u, 8u, 4u); put(76u, 5u, 4u); put(80u, 16u, 4u);
  put(0x100u, 0x1000FFFFu, 4u); // Owned self-branch, followed by NOP delay slot.
  return elf;
}

void zero_budget_dma() {
  for (const auto command : kCommands) {
    ps2vita::Emulator emulator;
    const auto elf = owned_elf();
    check(emulator.load_elf(elf.data(), elf.size()).ok, "owned ELF readies the zero-budget DMA route");
    emulator.enable_vif_command_census(true);
    const auto payload = gif_packet(0xD1000004u);
    auto stream = transfer(command, payload);
    append(stream, 0x01001234u);
    while ((stream.size() & 15u) != 0u) stream.push_back(0u);
    auto& memory = emulator.memory();
    store(memory, 0x2000u, stream);
    memory.write32(0x1000E000u, 1u);
    memory.write32(0x10009010u, 0x2000u);
    memory.write32(0x10009020u, 3u);
    memory.write32(0x10009000u, 0x101u);
    std::vector<Bytes> seen;
    emulator.set_gif_submission_observer(
        [&](std::uint64_t ordinal, const std::uint8_t* data, std::size_t size) {
          check(ordinal == seen.size() + 1u, "DMA-to-GIF observer has exact monotonic submission order");
          seen.emplace_back();
          if (size != 0u) seen.back().assign(data, data + size);
        });
    // Preserve the current one-cycle discovery and three-qword/24-cycle model.
    memory.advance(1u);
    memory.advance(23u);
    check(!memory.has_completed_graphics_packets() &&
              emulator.run_slice(0u) == ps2vita::StopReason::StepLimit && seen.empty() &&
              emulator.vif1().packets_submitted() == 0u,
          context(command, "zero-budget service cannot complete pending VIF DMA early"));
    memory.advance(1u);
    check(memory.has_completed_graphics_packets(), "normal VIF DMA completion queues its raw payload");
    check(emulator.run_slice(0u) == ps2vita::StopReason::StepLimit &&
              seen == std::vector<Bytes>{payload} && emulator.gif().packets_submitted() == 1u &&
              emulator.gif().packed_tags() == 1u && emulator.gif().pending_bytes() == 0u &&
              emulator.gif().packets_rejected() == 0u && emulator.vif1().packets_rejected() == 0u &&
              emulator.vif1().packets_submitted() == 1u && emulator.vif1().cycle() == 0x1234u &&
              emulator.cpu().state().cycles == 0u && emulator.cpu().state().pc == 0x1000u &&
              emulator.iop().state().cycles == 0u && emulator.vif1().vu1().pairs_executed() == 0u,
          context(command, "completed DMA reaches Gif once on a zero-budget slice without EE/IOP/VU execution"));
    check_census(emulator.vif1(), command);
    for (unsigned repeat = 0u; repeat < 4u; ++repeat) {
      check(emulator.run_slice(0u) == ps2vita::StopReason::StepLimit && seen.size() == 1u &&
                emulator.gif().packets_submitted() == 1u && !memory.has_completed_graphics_packets() &&
                !emulator.vif1().has_gif_packet(),
            context(command, "later empty zero-budget slices never replay the forwarded DMA"));
    }
  }
}
} // namespace

int main() {
  fragmented_payloads();
  command_header_and_reset_boundaries();
  mixed_direct_census();
  path1_forward_order();
  qwc_boundaries();
  malformed_forwarding_is_unchanged();
  zero_budget_dma();
  std::printf("vif_directhl_tests: %u checks, %u failures\n", checks, failures);
  return failures == 0u ? 0 : 1;
}
