#include "ps2vita/execution_budget.hpp"

#include <cstdint>
#include <cstdio>
#include <limits>
#include <vector>

namespace {

unsigned checks = 0, failures = 0;

void check(bool condition, const char* name, const char* property) {
  ++checks;
  if (!condition) {
    ++failures;
    std::fprintf(stderr, "FAIL [%s]: %s\n", name, property);
  }
}

using Stop = ps2vita::ExecutionBudgetStop;
constexpr auto kMaximum = std::numeric_limits<std::uint64_t>::max();
constexpr auto kQuantumMaximum = std::numeric_limits<std::uint32_t>::max();

void run_case(const char* name, const ps2vita::ExecutionBudget& budget,
              const std::vector<std::uint64_t>& timestamps,
              const std::vector<bool>& allowed,
              Stop expected_stop, std::uint32_t expected_quanta,
              std::uint64_t expected_elapsed) {
  std::size_t clock_calls = 0, run_calls = 0;
  bool unexpected_clock = false, unexpected_run = false;
  bool unchanged_quantum = true;
  const auto original = budget;
  const auto result = ps2vita::run_execution_budget(budget, [&]() {
    const auto index = clock_calls++;
    if (index >= timestamps.size()) {
      unexpected_clock = true;
      return std::uint64_t{0};
    }
    return timestamps[index];
  }, [&](std::uint32_t quantum) {
    unchanged_quantum &= quantum == budget.quantum;
    const auto index = run_calls++;
    if (index >= allowed.size()) {
      unexpected_run = true;
      return false;
    }
    return static_cast<bool>(allowed[index]);
  });
  check(result.stop == expected_stop, name, "stop status is distinct and exact");
  check(result.quanta == expected_quanta && run_calls == expected_quanta,
        name, "callback count stops at the first applicable boundary");
  check(result.elapsed_microseconds == expected_elapsed,
        name, "elapsed subtraction retains only valid monotonic samples");
  check(!unexpected_clock && !unexpected_run && clock_calls == timestamps.size(),
        name, "no extra callback or clock reads after stopping");
  check(unchanged_quantum, name, "positive work quantum is forwarded unchanged");
  check(original.microseconds == budget.microseconds &&
            original.quantum == budget.quantum && original.max_quanta == budget.max_quanta,
        name, "budget configuration is not mutated");
}

void boundaries() {
  const ps2vita::ExecutionBudget normal{8000u, 2000u, 1024u};
  run_case("exact soft deadline", normal, {100, 3100, 6100, 8100},
      {true, true, true}, Stop::Elapsed, 3, 8000);
  run_case("last quantum crosses deadline", normal, {100, 2099, 8000, 8101},
      {true, true, true}, Stop::Elapsed, 3, 8001);
  run_case("one expensive quantum overruns", normal, {100, 10100},
      {true}, Stop::Elapsed, 1, 10000);
  run_case("terminal first quantum", normal, {100, 200},
      {false}, Stop::CallbackStopped, 1, 100);
  run_case("terminal later quantum", normal, {0, 1000, 2500},
      {true, false}, Stop::CallbackStopped, 2, 2500);
  run_case("terminal wins over elapsed and cap", {8000, 2000, 1}, {0, 9000},
      {false}, Stop::CallbackStopped, 1, 9000);
  run_case("terminal wins over stalled clock", normal, {100, 100},
      {false}, Stop::CallbackStopped, 1, 0);
  run_case("terminal wins over backwards clock", normal, {1000, 999},
      {false}, Stop::CallbackStopped, 1, 0);

  run_case("clock stalls immediately", normal, {1000, 1000},
      {true}, Stop::ClockStalled, 1, 0);
  run_case("clock stalls after progress", normal, {1000, 1500, 1500},
      {true, true}, Stop::ClockStalled, 2, 500);
  run_case("clock moves backwards immediately", normal, {1000, 999},
      {true}, Stop::ClockBackwards, 1, 0);
  run_case("clock moves backwards above starting value", normal, {1000, 1500, 1250},
      {true, true}, Stop::ClockBackwards, 2, 500);
  run_case("finite quantum safety cap", {100, 2000, 3}, {0, 1, 2, 3},
      {true, true, true}, Stop::QuantumLimit, 3, 3);
  run_case("elapsed wins over simultaneous cap", {2, 1, 1}, {10, 12},
      {true}, Stop::Elapsed, 1, 2);
  run_case("stall wins over simultaneous cap", {2, 1, 1}, {10, 10},
      {true}, Stop::ClockStalled, 1, 0);
  run_case("backwards wins over simultaneous cap", {2, 1, 1}, {10, 9},
      {true}, Stop::ClockBackwards, 1, 0);

  run_case("zero time invalid", {0, 2000, 1024}, {}, {}, Stop::InvalidBudget, 0, 0);
  run_case("zero quantum invalid", {8000, 0, 1024}, {}, {}, Stop::InvalidBudget, 0, 0);
  run_case("zero cap invalid", {8000, 2000, 0}, {}, {}, Stop::InvalidBudget, 0, 0);
  run_case("all zero invalid", {}, {}, {}, Stop::InvalidBudget, 0, 0);
}

void overflow_and_reuse() {
  run_case("exact deadline near uint64 maximum", {10, 1, 8},
      {kMaximum - 10, kMaximum - 7, kMaximum},
      {true, true}, Stop::Elapsed, 2, 10);
  run_case("unrepresentable absolute deadline then wrap", {30, 1, 8},
      {kMaximum - 20, kMaximum - 10, kMaximum - 1, kMaximum, 0},
      {true, true, true, true}, Stop::ClockBackwards, 4, 20);
  run_case("full uint64 elapsed range", {kMaximum, 1, 8},
      {0, kMaximum}, {true}, Stop::Elapsed, 1, kMaximum);
  run_case("huge budget cannot overflow deadline", {kMaximum, 1, 8},
      {1, kMaximum, 0}, {true, true}, Stop::ClockBackwards, 2, kMaximum - 1);
  run_case("maximal quantum and cap forward safely", {1, kQuantumMaximum, kQuantumMaximum},
      {0, 1}, {true}, Stop::Elapsed, 1, 1);

  // Lvalue functors retain their own state. The helper does not copy the
  // clock/callback or retain state between distinct host-frame invocations.
  struct Clock {
    unsigned calls = 0;
    std::uint64_t operator()() { return calls++ * 5u; }
  } clock;
  struct Run {
    unsigned calls = 0;
    std::uint32_t last_quantum = 0;
    bool operator()(std::uint32_t quantum) { ++calls; last_quantum = quantum; return true; }
  } run;
  const auto first = ps2vita::run_execution_budget({5, 123, 1024}, clock, run);
  const auto second = ps2vita::run_execution_budget({5, 123, 1024}, clock, run);
  check(first.stop == Stop::Elapsed && second.stop == Stop::Elapsed &&
            first.quanta == 1 && second.quanta == 1 &&
            first.elapsed_microseconds == 5 && second.elapsed_microseconds == 5,
        "fresh frame budget", "each invocation uses a new starting time");
  check(clock.calls == 4 && run.calls == 2 && run.last_quantum == 123,
        "borrowed functor state", "clock and callback state is not copied or retained");
}

void progress_thresholds() {
  check(ps2vita::next_execution_progress(0, 1000000) == 1000000,
        "progress initial", "first interval starts from zero");
  check(ps2vita::next_execution_progress(1000000, 1000000) == 2000000,
        "progress normal", "ordinary intervals add exactly");
  check(ps2vita::next_execution_progress(kMaximum - 1000000, 1000000) == kMaximum,
        "progress exact maximum", "an exactly representable sum is preserved");
  check(ps2vita::next_execution_progress(kMaximum - 999999, 1000000) == kMaximum,
        "progress saturation", "overflow saturates instead of wrapping into repeated writes");
  check(ps2vita::next_execution_progress(kMaximum, 1000000) == kMaximum,
        "progress at maximum", "saturation remains at maximum");
  check(ps2vita::next_execution_progress(10, 0) == 10 &&
            ps2vita::next_execution_progress(kMaximum, 0) == kMaximum,
        "progress zero interval", "zero interval never alters the marker");
  check(ps2vita::next_execution_progress(0, kMaximum) == kMaximum,
        "progress maximal interval", "maximal interval from zero is representable");
  check(!ps2vita::execution_progress_due(0, 1000000, 0) &&
            !ps2vita::execution_progress_due(999999, 1000000, 0),
        "progress below threshold", "zero and incomplete intervals do not report");
  check(ps2vita::execution_progress_due(1000000, 1000000, 0) &&
            ps2vita::execution_progress_due(1000001, 1000000, 0),
        "progress threshold reached", "equal or crossed intervals report once");
  check(ps2vita::execution_progress_due(kMaximum, kMaximum, kMaximum - 1),
        "progress reaches maximum", "saturated threshold can be reported once");
  check(!ps2vita::execution_progress_due(kMaximum, kMaximum, kMaximum) &&
            !ps2vita::execution_progress_due(1000000, 1000000, 1000000),
        "progress duplicate guard", "identical cycles never repeat even at maximum");
}

} // namespace

int main() {
  boundaries(); overflow_and_reuse(); progress_thresholds();
  std::printf("Execution budget tests: %u checks, %u failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
