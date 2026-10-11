#include "ps2vita/memory.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <limits>

namespace {

constexpr std::uint32_t kCount = 0x1F8014A0u;
constexpr std::uint32_t kMode = 0x1F8014A4u;
constexpr std::uint32_t kTarget = 0x1F8014A8u;
constexpr std::uint32_t kInterrupt = 1u << 16;
constexpr std::array<std::uint32_t, 4> kDivisors{{1u, 8u, 16u, 256u}};
unsigned checks = 0, failures = 0;

void check(bool condition, const char* description, unsigned selector) {
  ++checks;
  if (!condition) {
    ++failures;
    std::fprintf(stderr, "FAIL: selector %u: %s\n", selector, description);
  }
}

// Arithmetic oracle for the existing Timer 5 model. Deliberately use integer
// division/remainder, not the production power-of-two shift/mask optimization.
// Video/DMA behavior and real-hardware timer accuracy are outside this fixture.
struct ReferenceTimer {
  std::uint32_t count = 0, target = 0, mode = 0;
  std::uint32_t ee_remainder = 0, timer_remainder = 0;
  bool target_future = false, interrupt = false;

  void advance(std::uint32_t cycles) {
    const std::uint64_t ee_total = std::uint64_t{ee_remainder} + cycles;
    const std::uint64_t iop_cycles = ee_total / 8u;
    ee_remainder = static_cast<std::uint32_t>(ee_total % 8u);
    if (iop_cycles == 0u) return;
    auto next_mode = mode;
    if ((next_mode & 0x40u) != 0u && (next_mode & 0x80u) == 0u)
      next_mode |= 0x400u;
    if ((next_mode & 1u) != 0u) return;
    const auto divisor = kDivisors[(next_mode >> 13u) & 3u];
    const std::uint64_t accumulated = std::uint64_t{timer_remainder} + iop_cycles;
    const auto ticks = accumulated / divisor;
    timer_remainder = static_cast<std::uint32_t>(accumulated % divisor);
    if (ticks == 0u) return;
    const std::uint64_t sum = std::uint64_t{count} + ticks;
    const bool reached_target = !target_future && count < target && sum >= target;
    const bool overflow = sum > std::numeric_limits<std::uint32_t>::max();
    auto next_count = static_cast<std::uint32_t>(sum);
    const auto fire = [&](std::uint32_t event_flag) {
      if ((next_mode & 0x400u) == 0u) return;
      if ((next_mode & 0x40u) != 0u || (next_mode & event_flag) == 0u)
        interrupt = true;
      if ((next_mode & 0x80u) != 0u)
        next_mode ^= 0x400u;
      else if ((next_mode & 0x40u) != 0u)
        next_mode &= ~0x400u;
    };
    if (reached_target) {
      if ((next_mode & 0x10u) != 0u) fire(0x800u);
      next_mode |= 0x800u;
      if ((next_mode & 8u) != 0u && target != 0u)
        next_count = static_cast<std::uint32_t>(sum % target);
      else
        target_future = true;
    }
    if (overflow) {
      if ((next_mode & 0x20u) != 0u) fire(0x1000u);
      next_mode |= 0x1000u;
      target_future = false;
    }
    count = next_count;
    mode = next_mode;
  }

  std::uint32_t normalized_mode(std::uint32_t value) const {
    const bool active_event = (mode & 0x1800u) != 0u && (mode & 0x30u) != 0u;
    return (value & 0x63FFu) | (active_event ? mode & 0x1C00u : 0x400u);
  }

  void write_mode(std::uint32_t value, bool halfword = false) {
    const auto next = normalized_mode(value);
    mode = halfword ? (mode & 0xFFFF0000u) | next : next;
    count = 0u;
    timer_remainder = 0u;
    target_future = false;
  }

  std::uint32_t read_mode() {
    const auto observed = mode;
    mode = (mode & ~0x1800u) | 0x400u;
    return observed;
  }
};

struct Fixture {
  ps2vita::Memory memory;
  ReferenceTimer reference;
  unsigned selector;

  explicit Fixture(unsigned clock_selector) : selector(clock_selector) {
    memory.iop_write32(0x1F801074u, kInterrupt);
    memory.iop_write32(0x1F801078u, 1u);
  }

  void mode(std::uint32_t value, bool halfword = false) {
    if (halfword) memory.iop_write16(kMode, static_cast<std::uint16_t>(value));
    else memory.iop_write32(kMode, value);
    reference.write_mode(value, halfword);
  }

  void mode_byte(unsigned byte, std::uint8_t value) {
    memory.iop_write8(kMode + byte, value);
    const auto shift = byte * 8u;
    reference.mode = (reference.mode & ~(0xFFu << shift)) |
                     (std::uint32_t{value} << shift);
  }

  void count(std::uint32_t value) {
    memory.iop_write32(kCount, value);
    reference.count = value;
    reference.target_future = value > reference.target;
  }

  void target(std::uint32_t value) {
    memory.iop_write32(kTarget, value);
    reference.target = value;
    reference.target_future = value <= reference.count;
  }

  void advance(std::uint32_t cycles) {
    memory.advance(cycles);
    reference.advance(cycles);
  }

  void acknowledge() {
    memory.iop_write32(0x1F801070u, ~kInterrupt);
    reference.interrupt = false;
  }

  void observe(const char* label) {
    check(memory.iop_read32(kCount) == reference.count, label, selector);
    check(memory.iop_read32(kTarget) == reference.target,
          "target register remains unchanged", selector);
    check(((memory.iop_read32(0x1F801070u) & kInterrupt) != 0u) ==
              reference.interrupt, "Timer 5 interrupt status matches", selector);
    check(memory.iop_interrupt_pending() == reference.interrupt,
          "masked IOP interrupt line matches", selector);
    const auto expected_mode = reference.read_mode();
    check(memory.iop_read32(kMode) == expected_mode,
          "mode flags and interrupt latch match before read acknowledgement", selector);
  }
};

void test_clock_carry(unsigned selector) {
  Fixture fixture(selector);
  const auto period = kDivisors[selector] * 8u;
  fixture.mode(selector << 13u);
  fixture.advance(0u);
  fixture.observe("zero advance preserves timer count");
  fixture.advance(period - 1u);
  check(fixture.memory.iop_read32(kCount) == 0u,
        "prescaled tick remains pending one EE cycle before deadline", selector);
  fixture.observe("prescaler carries the pre-deadline fraction");
  fixture.advance(1u);
  check(fixture.memory.iop_read32(kCount) == 1u,
        "all selectors tick on their exact existing deadline", selector);
  fixture.observe("carried fraction emits exactly one tick");
  constexpr std::array<std::uint32_t, 15> quanta{{
      0u, 1u, 6u, 1u, 3u, 0u, 13u, 255u, 7u, 1027u, 2u, 31u, 8u, 0u, 4097u}};
  for (const auto quantum : quanta) {
    fixture.advance(quantum);
    fixture.observe("irregular quanta preserve divided-clock carry");
  }
}

void test_target_and_overflow(unsigned selector) {
  Fixture fixture(selector);
  const auto period = kDivisors[selector] * 8u;
  fixture.target(3u);
  fixture.mode((selector << 13u) | 0x58u); // Target, reset-on-target, pulsed repeat.
  fixture.advance(period * 3u - 1u);
  fixture.observe("target remains quiet before its final prescaled cycle");
  fixture.advance(1u);
  check(fixture.memory.iop_read32(kCount) == 0u,
        "reset-on-target retains exact count zero", selector);
  check((fixture.memory.iop_read32(0x1F801070u) & kInterrupt) != 0u,
        "target raises interrupt at its prescaled deadline", selector);
  fixture.observe("target and repeat flags match");
  fixture.acknowledge();
  fixture.advance(period * 7u);
  fixture.observe("batched repeat retains independent target modulo");

  fixture.mode((selector << 13u) | 0x30u); // One-shot target and overflow.
  fixture.target(5u);
  fixture.count(10u);
  fixture.acknowledge();
  fixture.advance(period);
  fixture.observe("target behind count does not fire early");
  fixture.count(0xFFFFFFFEu);
  fixture.advance(period * 2u - 1u);
  fixture.observe("overflow remains pending before its exact tick");
  fixture.advance(1u);
  fixture.observe("overflow wraps and releases the future-target marker");
  fixture.acknowledge();
  fixture.advance(period * 5u);
  fixture.observe("target becomes reachable again following count wrap");

  fixture.mode((selector << 13u) | 0xB8u); // Target/reset/overflow/toggle.
  fixture.target(0xFFFFFFFFu);
  fixture.count(0xFFFFFFFEu);
  fixture.acknowledge();
  fixture.advance(period * 3u);
  fixture.observe("simultaneous target reset and overflow preserve toggle ordering");

  fixture.mode((selector << 13u) | 0x10u);
  fixture.target(1u);
  fixture.advance(period);
  // Do not read MODE before rewriting it: an unread active event retains its
  // flags/latch across the mode write even though count and prescaler reset.
  fixture.mode((selector << 13u) | 0x30u, true);
  fixture.observe("halfword mode write retains an unread active event");
  fixture.advance(period);
  fixture.mode(selector << 13u);
  fixture.observe("word mode write retains an unread active event");
}

void test_writes_and_reset(unsigned selector) {
  Fixture fixture(selector);
  // A byte write can change the divisor without resetting the old remainder.
  // Start with remainder 37, then consume it under each selected clock divisor.
  fixture.mode(3u << 13u);
  fixture.advance(37u * 8u + 3u);
  fixture.mode_byte(1u, static_cast<std::uint8_t>(selector << 5u));
  fixture.advance(5u);
  fixture.observe("byte clock change preserves and consumes the earlier remainder");
  fixture.mode_byte(0u, 1u);
  fixture.advance(1003u);
  fixture.observe("gate preserves an already accumulated timer remainder");
  fixture.mode_byte(0u, 0u);
  fixture.advance(kDivisors[selector] * 8u + 5u);
  fixture.observe("ungating consumes the retained nonzero prescaler remainder");
  fixture.advance(3u);
  fixture.mode((selector << 13u) | 0x10u, true);
  fixture.target(2u);
  fixture.advance(0u);
  fixture.observe("halfword mode write resets count and timer prescaler only");
  fixture.advance(kDivisors[selector] * 16u - 3u);
  fixture.observe("EE fractional phase survives halfword mode write");

  fixture.mode_byte(2u, 0xA5u);
  fixture.mode_byte(3u, 0x5Au);
  fixture.mode(selector << 13u, true);
  fixture.observe("halfword mode preserves unrelated upper bytes");
  fixture.mode(selector << 13u);
  fixture.observe("word mode write clears unrelated upper bytes");
  fixture.mode_byte(0u, 1u); // Gate counting while EE phase continues.
  fixture.advance(1003u);
  fixture.observe("gated clock does not consume prescaler state");
  fixture.mode_byte(0u, 0u);
  fixture.advance(13u);
  fixture.observe("ungated clock resumes at retained EE phase");

  fixture.memory.clear();
  fixture.reference = {};
  fixture.memory.iop_write32(0x1F801074u, kInterrupt);
  fixture.memory.iop_write32(0x1F801078u, 1u);
  fixture.observe("Memory clear resets timer and both fractional phases");
  fixture.advance(7u);
  fixture.observe("reset starts one cycle before the first IOP edge");
  fixture.advance(1u);
  fixture.observe("reset restores divisor-one counting");
}

void test_maximum_advance(unsigned selector) {
  Fixture fixture(selector);
  fixture.mode(selector << 13u);
  fixture.target(0u); // No target divisor or target event while spanning wraps.
  fixture.count(0xF0000000u);
  fixture.advance(7u);
  fixture.advance(std::numeric_limits<std::uint32_t>::max());
  fixture.observe("maximum uint32 advance retains full-width phase/count arithmetic");
  fixture.advance(2u);
  fixture.observe("maximum advance retains its fractional EE carry");
  fixture.advance(kDivisors[selector] * 8u - 1u);
  fixture.observe("maximum advance retains its timer prescaler carry");
}

} // namespace

int main() {
  for (unsigned selector = 0; selector < kDivisors.size(); ++selector) {
    test_clock_carry(selector);
    test_target_and_overflow(selector);
    test_writes_and_reset(selector);
    test_maximum_advance(selector);
  }
  std::printf("IOP Timer 5 prescaler: %u checks, %u failures\n", checks, failures);
  return failures == 0u ? 0 : 1;
}
