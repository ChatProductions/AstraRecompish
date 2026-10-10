#pragma once

#include <cstdint>
#include <limits>

namespace ps2vita {

struct ExecutionBudget {
  std::uint64_t microseconds = 0;
  std::uint32_t quantum = 0; // Positive work units passed unchanged to callback.
  std::uint32_t max_quanta = 0;
};

enum class ExecutionBudgetStop {
  InvalidBudget,
  CallbackStopped,
  Elapsed,
  ClockStalled,
  ClockBackwards,
  QuantumLimit
};

struct ExecutionBudgetResult {
  ExecutionBudgetStop stop = ExecutionBudgetStop::InvalidBudget;
  std::uint32_t quanta = 0;
  // Last nondecreasing clock sample minus the initial sample. On a backwards
  // clock this retains the last valid elapsed value, rather than underflowing.
  std::uint64_t elapsed_microseconds = 0;
};

// A SOFT wall-time budget: one synchronous callback cannot be interrupted and
// may overrun the budget. No guest execution/device state is owned or advanced
// here. Clock returns uint64 microseconds; callback(quantum) returns whether
// another quantum is allowed. A stopped callback always wins over clock/limit
// statuses, letting the caller preserve its own terminal reason.
template <typename Clock, typename Run>
ExecutionBudgetResult run_execution_budget(const ExecutionBudget& budget,
                                           Clock&& clock, Run&& run) {
  ExecutionBudgetResult result;
  if (budget.microseconds == 0 || budget.quantum == 0 || budget.max_quanta == 0)
    return result; // Invalid arguments call neither the clock nor the callback.

  const auto start = static_cast<std::uint64_t>(clock());
  auto previous = start;
  while (true) {
    const bool keep_running = run(budget.quantum);
    ++result.quanta;
    const auto now = static_cast<std::uint64_t>(clock());
    if (now >= previous) result.elapsed_microseconds = now - start;
    if (!keep_running) {
      result.stop = ExecutionBudgetStop::CallbackStopped;
      return result;
    }
    if (now < previous) {
      result.stop = ExecutionBudgetStop::ClockBackwards;
      return result;
    }
    if (now == previous) {
      result.stop = ExecutionBudgetStop::ClockStalled;
      return result;
    }
    // Subtract only monotonic values; never form start + budget, which can
    // overflow near UINT64_MAX. A wrapping clock is treated as backwards.
    if (result.elapsed_microseconds >= budget.microseconds) {
      result.stop = ExecutionBudgetStop::Elapsed;
      return result;
    }
    if (result.quanta == budget.max_quanta) {
      result.stop = ExecutionBudgetStop::QuantumLimit;
      return result;
    }
    previous = now;
  }
}

// Diagnostic thresholds must not wrap into an immediate-write loop. Callers
// should also remember the last reported value to avoid repeating at MAX.
constexpr std::uint64_t next_execution_progress(std::uint64_t completed,
                                                std::uint64_t interval) {
  constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
  return interval > maximum - completed ? maximum : completed + interval;
}

constexpr bool execution_progress_due(std::uint64_t completed,
                                      std::uint64_t next,
                                      std::uint64_t last_reported) {
  return completed >= next && completed != last_reported;
}

} // namespace ps2vita
