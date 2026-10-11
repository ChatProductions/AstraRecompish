#include "ps2vita/emulator.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

using Bytes = std::vector<std::uint8_t>;
unsigned checks = 0u, failures = 0u;
constexpr std::uint32_t kGifDma = 0x1000A000u;
constexpr std::uint32_t kVifDma = 0x10009000u;
constexpr std::uint32_t kSource = 0x2000u;
constexpr std::uint32_t kVuPacket = 0x40u;
constexpr std::uint64_t kTag = 0x1000000000008001ull;

void check(bool condition, const char* property) {
  ++checks;
  if (!condition) {
    ++failures;
    if (failures <= 32u) std::fprintf(stderr, "FAIL: %s\n", property);
  }
}

void append(Bytes& bytes, std::uint64_t value, unsigned width) {
  for (unsigned byte = 0u; byte < width; ++byte)
    bytes.push_back(static_cast<std::uint8_t>(value >> (byte * 8u)));
}

Bytes gif_packet(std::uint32_t marker) {
  // A complete one-loop PACKED A+D RGBAQ write, with no drawing required.
  Bytes result;
  append(result, kTag, 8u);
  append(result, 0xEu, 8u);
  append(result, marker, 8u);
  append(result, 1u, 8u);
  return result;
}

Bytes direct_packet(const Bytes& payload, bool dma_padding = false) {
  Bytes result;
  append(result, 0x50000000u | (payload.size() / 16u), 4u);
  result.insert(result.end(), payload.begin(), payload.end());
  if (dma_padding) while ((result.size() & 15u) != 0u) result.push_back(0u);
  return result;
}

void store(ps2vita::Memory& memory, std::uint32_t address, const Bytes& bytes) {
  for (std::size_t byte = 0u; byte < bytes.size(); ++byte)
    memory.write8(address + static_cast<std::uint32_t>(byte), bytes[byte]);
}

void arm_dma(ps2vita::Memory& memory, std::uint32_t channel,
             std::uint32_t source, std::uint32_t qwc, bool chain = false) {
  memory.write32(0x1000E000u, 1u);
  memory.write32(channel + 0x10u, source);
  memory.write32(channel + 0x20u, qwc);
  memory.write32(channel + 0x30u, source);
  memory.write32(channel, chain ? 0x105u : 0x101u);
}

void queue_dma(ps2vita::Memory& memory, std::uint32_t channel,
               std::uint32_t source, const Bytes& bytes) {
  store(memory, source, bytes);
  const auto qwc = static_cast<std::uint32_t>(bytes.size() / 16u);
  check(!bytes.empty() && (bytes.size() & 15u) == 0u,
        "owned normal DMA input consists of complete nonempty qwords");
  arm_dma(memory, channel, source, qwc);
  // These are existing transport-model deadlines, not PS2 hardware timings.
  memory.advance(1u); // Discover the armed transfer without completing it.
  memory.advance(qwc * 8u);
}

void prepare_vu(ps2vita::Memory& memory, ps2vita::Vu1& vu,
                const Bytes& packet, bool end = true) {
  vu.reset();
  for (unsigned pair = 0u; pair < 8u; ++pair) {
    memory.vu1_store_micro_word(static_cast<std::uint16_t>(pair * 8u),
                               pair == 0u ? 0x800016FCu : 0x8000033Cu);
    memory.vu1_store_micro_word(static_cast<std::uint16_t>(pair * 8u + 4u),
                               pair == 0u && end ? 0x400002FFu : 0x2FFu);
  }
  store(memory, ps2vita::Memory::kVu1DataBase + kVuPacket, packet);
  vu.state().vi[2] = kVuPacket / 16u;
  vu.start(0u);
}

void queue_vu(ps2vita::Memory& memory, ps2vita::Vu1& vu, const Bytes& packet) {
  prepare_vu(memory, vu, packet);
  vu.run(2u);
  check(vu.has_path1_packet() && !vu.path1_active() && !vu.running() &&
            vu.pairs_executed() == 2u && vu.cycles_executed() == 4u &&
            vu.state().pc == 16u && vu.xgkick_stall_cycles() == 2u &&
            vu.path1_tags_queued() == 1u && vu.path1_tags_rejected() == 0u,
        "owned E-bit VU program completes one PATH1 packet at its existing deadline");
}

Bytes owned_elf() {
  // Minimal owned MIPS ELF: a self-branch and its NOP delay slot.
  Bytes result(0x108u, 0u);
  const auto put = [&](std::size_t offset, std::uint32_t value, unsigned width) {
    for (unsigned byte = 0u; byte < width; ++byte)
      result[offset + byte] = static_cast<std::uint8_t>(value >> (byte * 8u));
  };
  put(0u, 0x464C457Fu, 4u); put(4u, 0x010101u, 3u);
  put(16u, 2u, 2u); put(18u, 8u, 2u); put(20u, 1u, 4u);
  put(24u, 0x1000u, 4u); put(28u, 52u, 4u);
  put(40u, 52u, 2u); put(42u, 32u, 2u); put(44u, 1u, 2u);
  put(52u, 1u, 4u); put(56u, 0x100u, 4u);
  put(60u, 0x1000u, 4u); put(64u, 0x1000u, 4u);
  put(68u, 8u, 4u); put(72u, 8u, 4u);
  put(76u, 5u, 4u); put(80u, 16u, 4u);
  put(0x100u, 0x1000FFFFu, 4u);
  return result;
}

void ready(ps2vita::Emulator& emulator) {
  const auto elf = owned_elf();
  check(emulator.load_elf(elf.data(), elf.size()).ok && emulator.ready(),
        "minimal owned ELF readies the zero-budget execution route");
}

ps2vita::Vif1& test_vif(ps2vita::Emulator& emulator) {
  // Test-only injection into an actually mutable Emulator. Its production
  // accessor remains const; no mutable queue API is added just for this test.
  return const_cast<ps2vita::Vif1&>(emulator.vif1());
}

struct Observed {
  std::vector<Bytes> packets;
  std::vector<std::uint64_t> ordinals;
  void attach(ps2vita::Emulator& emulator) {
    emulator.set_gif_submission_observer(
        [this](std::uint64_t ordinal, const std::uint8_t* data, std::size_t size) {
          ordinals.push_back(ordinal);
          packets.emplace_back();
          if (size != 0u) packets.back().assign(data, data + size);
        });
  }
};

struct QuietState {
  std::array<std::uint64_t, 17> values{};
  ps2vita::Vu1State vu{};
};

QuietState quiet_state(const ps2vita::Emulator& emulator) {
  const auto& vu = emulator.vif1().vu1();
  return {{{emulator.cpu().state().cycles, emulator.cpu().state().pc,
      emulator.iop().state().cycles, emulator.iop().state().pc,
      emulator.memory().cycles_until_next_event(),
      emulator.memory().read32(0x10000000u),
      emulator.memory().read32(kGifDma), emulator.memory().read32(kVifDma),
      vu.cycles_executed(), vu.pairs_executed(), vu.xgkick_stall_cycles(),
      vu.path1_tags_queued(), vu.path1_tags_rejected(), vu.last_kick_tag(),
      emulator.vif1().pending_direct_bytes(), vu.path1_active() ? 1u : 0u,
      vu.running() ? 1u : 0u}},
      vu.state()};
}

bool same_quiet(const QuietState& a, const QuietState& b) {
  return a.values == b.values && a.vu.vf == b.vu.vf && a.vu.vi == b.vu.vi &&
      a.vu.acc == b.vu.acc && a.vu.i == b.vu.i && a.vu.q == b.vu.q &&
      a.vu.mac == b.vu.mac && a.vu.pc == b.vu.pc;
}

void repeat_empty_service(ps2vita::Emulator& emulator, Observed& observed) {
  const auto before = quiet_state(emulator);
  const auto gif_submissions = emulator.gif().packets_submitted();
  const auto vif_submissions = emulator.vif1().packets_submitted();
  const auto vif_rejections = emulator.vif1().packets_rejected();
  const auto observer_count = observed.packets.size();
  for (unsigned repetition = 0u; repetition < 8u; ++repetition) {
    emulator.service_graphics();
    check(emulator.run_slice(0u) == ps2vita::StopReason::StepLimit,
          "ready zero-budget slices retain StepLimit without executing work");
    check(same_quiet(before, quiet_state(emulator)) &&
              emulator.gif().packets_submitted() == gif_submissions &&
              emulator.vif1().packets_submitted() == vif_submissions &&
              emulator.vif1().packets_rejected() == vif_rejections &&
              observed.packets.size() == observer_count,
          "repeated empty service preserves clocks, VU state, deadlines and submissions");
  }
}

void empty_components() {
  ps2vita::Memory memory;
  ps2vita::Vif1 vif(memory);
  ps2vita::Vu1 vu(memory);
  const Bytes poison{0xAAu, 0xBBu, 0xCCu};
  Bytes packet = poison;
  for (unsigned repetition = 0u; repetition < 8u; ++repetition) {
    check(!memory.has_completed_graphics_packets() && !vif.has_gif_packet() &&
              !vif.vu1().has_path1_packet() && !vu.has_path1_packet(),
          "all four completed source deques report empty after reset");
    check(!memory.pop_gif_packet(packet) && packet == poison &&
              !memory.pop_vif1_packet(packet) && packet == poison &&
              !vif.pop_gif_packet(packet) && packet == poison &&
              !vu.pop_path1_packet(packet) && packet == poison,
          "false pop routes leave the caller's packet unchanged");
  }
  ps2vita::Emulator emulator;
  Observed observed;
  observed.attach(emulator);
  check(emulator.run_slice(0u) == ps2vita::StopReason::Halted,
        "an unready zero-budget Emulator still returns Halted");
  ready(emulator);
  repeat_empty_service(emulator, observed);
}

void memory_completed_queues() {
  for (const auto channel : {kGifDma, kVifDma}) {
    ps2vita::Memory memory;
    const auto payload = channel == kGifDma ? gif_packet(0x80112233u) :
        direct_packet(gif_packet(0x80445566u), true);
    store(memory, kSource, payload);
    const auto qwc = static_cast<std::uint32_t>(payload.size() / 16u);
    arm_dma(memory, channel, kSource, qwc);
    check(!memory.has_completed_graphics_packets() &&
              memory.cycles_until_next_event() == 1u,
          "armed DMA is not a completed queue and exposes discovery horizon");
    memory.advance(1u);
    memory.advance(qwc * 8u - 1u);
    check(!memory.has_completed_graphics_packets() &&
              (memory.read32(channel) & 0x100u) != 0u,
          "normal DMA remains incomplete until its modeled QWC deadline");
    memory.advance(1u);
    check(memory.has_completed_graphics_packets(),
          "each Memory deque independently makes the combined predicate true");
    Bytes packet{0xFFu};
    const auto popped = channel == kGifDma ? memory.pop_gif_packet(packet) :
        memory.pop_vif1_packet(packet);
    check(popped && packet == payload && !memory.has_completed_graphics_packets(),
          "completed normal DMA is byte-exact and clears readiness when popped");
    const auto prior = packet;
    check(!memory.pop_gif_packet(packet) && !memory.pop_vif1_packet(packet) &&
              packet == prior,
          "consumed Memory packet cannot be popped twice");
  }
}

void vif_and_vu_completed_queues() {
  ps2vita::Memory memory;
  ps2vita::Vif1 vif(memory);
  const auto path1 = gif_packet(0x80123456u);
  queue_vu(memory, vif.vu1(), path1);
  check(!memory.has_completed_graphics_packets() && vif.has_gif_packet() &&
            vif.vu1().has_path1_packet() && vif.packets_submitted() == 0u,
        "VIF predicate sees PATH1-only output without any VIF DIRECT submission");
  Bytes packet;
  check(vif.pop_gif_packet(packet) && packet == path1 && !vif.has_gif_packet() &&
            !vif.vu1().has_path1_packet(),
        "VIF fallback drains its contained VU queue exactly once");
  const auto direct = gif_packet(0x80654321u);
  const auto input = direct_packet(direct);
  check(vif.submit(input.data(), input.size()) && vif.has_gif_packet() &&
            !vif.vu1().has_path1_packet() && !memory.has_completed_graphics_packets(),
        "completed DIRECT independently makes only the VIF queue ready");
  check(vif.pop_gif_packet(packet) && packet == direct && !vif.has_gif_packet(),
        "VIF DIRECT pop preserves raw bytes and clears readiness");
  const auto prior = packet;
  check(!vif.pop_gif_packet(packet) && packet == prior,
        "consumed VIF packet cannot be popped twice");
}

void zero_budget_each_source() {
  for (unsigned source = 0u; source < 4u; ++source) {
    ps2vita::Emulator emulator;
    ready(emulator);
    Observed observed;
    observed.attach(emulator);
    const auto payload = gif_packet(0x80010203u + source);
    if (source == 0u) queue_dma(emulator.memory(), kGifDma, kSource, payload);
    if (source == 1u)
      queue_dma(emulator.memory(), kVifDma, kSource, direct_packet(payload, true));
    if (source == 2u) {
      const auto input = direct_packet(payload);
      check(test_vif(emulator).submit(input.data(), input.size()),
            "test-only injection queues a complete DIRECT packet");
    }
    if (source == 3u) queue_vu(emulator.memory(), test_vif(emulator).vu1(), payload);
    check(emulator.memory().has_completed_graphics_packets() == (source < 2u) &&
              emulator.vif1().has_gif_packet() == (source >= 2u) &&
              emulator.vif1().vu1().has_path1_packet() == (source == 3u),
          "zero-budget fixture isolates each of the four completed source queues");
    check(emulator.run_slice(0u) == ps2vita::StopReason::StepLimit &&
              observed.packets == std::vector<Bytes>{payload} &&
              observed.ordinals == std::vector<std::uint64_t>{1u} &&
              emulator.gif().packets_submitted() == 1u &&
              emulator.gif().packets_rejected() == 0u &&
              emulator.gif().pending_bytes() == 0u,
          "zero-budget slice drains each completed source exactly once and byte-exactly");
    check(emulator.cpu().state().cycles == 0u && emulator.cpu().state().pc == 0x1000u &&
              emulator.iop().state().cycles == 0u &&
              !emulator.memory().has_completed_graphics_packets() &&
              !emulator.vif1().has_gif_packet() &&
              !emulator.vif1().vu1().has_path1_packet(),
          "zero-budget drain executes no EE/IOP work and empties completed queues");
    check(emulator.vif1().packets_submitted() == (source == 1u || source == 2u ? 1u : 0u),
          "zero-budget drain submits to VIF only on the relevant input routes");
    repeat_empty_service(emulator, observed);
  }
}

void mixed_original_order() {
  for (const bool reject_tail : {false, true}) {
    ps2vita::Emulator emulator;
    ready(emulator);
    auto& memory = emulator.memory();
    auto& vif = test_vif(emulator);
    Observed observed;
    observed.attach(emulator);
    const auto a = gif_packet(0x80111111u), b = gif_packet(0x80222222u);
    const auto c = gif_packet(0x80333333u), d = gif_packet(0x80444444u);
    const auto first_direct = direct_packet(a);
    check(vif.submit(first_direct.data(), first_direct.size()),
          "mixed fixture first queues DIRECT independently");
    queue_vu(memory, vif.vu1(), b);
    auto later_direct = direct_packet(c);
    if (reject_tail) append(later_direct, 0x7F000000u, 4u);
    while ((later_direct.size() & 15u) != 0u) later_direct.push_back(0u);
    queue_dma(memory, kVifDma, kSource, later_direct);
    queue_dma(memory, kGifDma, kSource + 0x100u, d);
    check(memory.has_completed_graphics_packets() && vif.has_gif_packet() &&
              vif.vu1().has_path1_packet(),
          "mixed fixture leaves all four completed deques populated before service");
    check(emulator.run_slice(0u) == ps2vita::StopReason::StepLimit &&
              observed.packets == std::vector<Bytes>{d, a, b, c} &&
              observed.ordinals == std::vector<std::uint64_t>{1u, 2u, 3u, 4u},
          "drain order remains Memory GIF, prior DIRECT, prior PATH1, DMA DIRECT");
    check(emulator.gif().packets_submitted() == 4u &&
              emulator.gif().packets_rejected() == 0u &&
              vif.packets_submitted() == 2u &&
              vif.packets_rejected() == (reject_tail ? 1u : 0u) &&
              vif.first_unsupported_code() == (reject_tail ? 0x7F000000u : 0u) &&
              !memory.has_completed_graphics_packets() && !vif.has_gif_packet(),
          "earlier complete output survives a later VIF rejection without duplication");
    repeat_empty_service(emulator, observed);
  }
}

void empty_vif_and_zero_gif_dma() {
  ps2vita::Memory memory;
  // A QWC0 END chain is an actually empty completed vector in the current
  // VIF transport model. DIRECT QWC0 and empty GIFtags are not empty vectors.
  memory.write32(kSource, 0x70000000u);
  arm_dma(memory, kVifDma, kSource, 0u, true);
  memory.advance(1u);
  memory.advance(7u);
  check(!memory.has_completed_graphics_packets(),
        "empty VIF chain still waits for its existing eight-cycle deadline");
  memory.advance(1u);
  check(memory.has_completed_graphics_packets(),
        "queued empty vector remains ready regardless of payload byte count");
  Bytes packet{0xFFu};
  check(memory.pop_vif1_packet(packet) && packet.empty() &&
            !memory.has_completed_graphics_packets(),
        "empty completed VIF vector drains exactly once");
  ps2vita::Emulator emulator;
  ready(emulator);
  Observed observed;
  observed.attach(emulator);
  emulator.enable_vif_command_census(true);
  emulator.memory().write32(kSource, 0x70000000u);
  arm_dma(emulator.memory(), kVifDma, kSource, 0u, true);
  emulator.memory().advance(1u);
  emulator.memory().advance(8u);
  check(emulator.memory().has_completed_graphics_packets() &&
            emulator.run_slice(0u) == ps2vita::StopReason::StepLimit &&
            emulator.vif1().packets_submitted() == 1u &&
            emulator.vif1().census_empty_packets() == 1u &&
            observed.packets.empty() && emulator.gif().packets_submitted() == 0u &&
            !emulator.memory().has_completed_graphics_packets(),
        "zero-budget empty DMA is submitted once to VIF, not silently skipped");
  repeat_empty_service(emulator, observed);
  memory.clear();
  arm_dma(memory, kGifDma, kSource, 0u);
  memory.advance(1u);
  memory.advance(64u);
  check(!memory.has_completed_graphics_packets() && !memory.pop_gif_packet(packet) &&
            (memory.read32(kGifDma) & 0x100u) != 0u &&
            (memory.read32(0x1000E010u) & 4u) == 0u,
        "GIF normal QWC0 remains fail-closed instead of fabricating completion");
}

void partial_direct_stays_partial() {
  ps2vita::Emulator emulator;
  ready(emulator);
  auto& vif = test_vif(emulator);
  Observed observed;
  observed.attach(emulator);
  const auto payload = gif_packet(0x80ABCDEFu);
  const auto input = direct_packet(payload);
  check(vif.submit(input.data(), 11u) && vif.pending_direct_bytes() == 25u &&
            !vif.has_gif_packet() && !vif.vu1().has_path1_packet() &&
            !emulator.memory().has_completed_graphics_packets(),
        "seven DIRECT payload bytes are pending, not a completed output queue");
  repeat_empty_service(emulator, observed);
  check(vif.submit(input.data() + 11u, input.size() - 11u) &&
            vif.pending_direct_bytes() == 0u && vif.has_gif_packet(),
        "only explicit later input completes the fragmented DIRECT payload");
  emulator.service_graphics();
  check(observed.packets == std::vector<Bytes>{payload} &&
            emulator.gif().packets_submitted() == 1u &&
            vif.packets_submitted() == 2u && vif.packets_rejected() == 0u,
        "fragmented DIRECT is byte-exact after empty service and submits only once");
  repeat_empty_service(emulator, observed);
}

void unfinished_xgkick_stays_unfinished() {
  ps2vita::Emulator emulator;
  ready(emulator);
  auto& memory = emulator.memory();
  auto& vif = test_vif(emulator);
  auto& vu = vif.vu1();
  Observed observed;
  observed.attach(emulator);
  const auto original = gif_packet(0x8013579Bu);
  const auto late = gif_packet(0x802468ACu);
  prepare_vu(memory, vu, original, false);
  vu.run(1u);
  check(vu.running() && vu.path1_active() && vu.cycles_executed() == 1u &&
            vu.pairs_executed() == 1u && vu.state().pc == 8u &&
            vu.last_kick_tag() == 0u && !vu.has_path1_packet() &&
            !vif.has_gif_packet() && !memory.has_completed_graphics_packets(),
        "issued XGKICK has not consumed its header or completed any packet");
  repeat_empty_service(emulator, observed);
  vu.run(2u);
  check(vu.cycles_executed() == 3u && vu.pairs_executed() == 3u &&
            vu.state().pc == 24u && vu.last_kick_tag() == kTag &&
            vu.path1_active() && !vu.has_path1_packet() && !vif.has_gif_packet(),
        "explicit VU execution consumes only the header before the payload deadline");
  // A later invalid header must not replace the already consumed header;
  // unread payload remains live and is changed before actual VU advancement.
  memory.write64(ps2vita::Memory::kVu1DataBase + kVuPacket,
                 0x1000000000008400ull);
  store(memory, ps2vita::Memory::kVu1DataBase + kVuPacket + 16u,
        Bytes(late.begin() + 16u, late.end()));
  repeat_empty_service(emulator, observed);
  vu.run(2u);
  check(vu.running() && !vu.path1_active() && vu.has_path1_packet() &&
            vif.has_gif_packet() && vu.cycles_executed() == 5u &&
            vu.pairs_executed() == 5u && vu.state().pc == 40u &&
            vu.xgkick_stall_cycles() == 0u && vu.path1_tags_rejected() == 0u,
        "only explicit VU work completes active XGKICK, without rereading its header");
  check(emulator.run_slice(0u) == ps2vita::StopReason::StepLimit &&
            observed.packets == std::vector<Bytes>{late} &&
            vu.cycles_executed() == 5u && vu.pairs_executed() == 5u &&
            !vu.has_path1_packet() && !vif.has_gif_packet(),
        "completed PATH1 drain samples late payload and does not advance the running VU");
  repeat_empty_service(emulator, observed);
}

} // namespace

int main() {
  empty_components();
  memory_completed_queues();
  vif_and_vu_completed_queues();
  zero_budget_each_source();
  mixed_original_order();
  empty_vif_and_zero_gif_dma();
  partial_direct_stays_partial();
  unfinished_xgkick_stays_unfinished();
  std::printf("graphics_service_queue_tests: %u checks, %u failures\n", checks, failures);
  return failures == 0u ? 0 : 1;
}
