# Session 2026-10-10: remove the monitor's per-refresh instruction ceiling

The preceding exact graphics-throughput milestone is on `main` as `21a93a9`.
Its paired host result is documented in the
[graphics session](SESSION_2026-10-10_GRAPHICS_THROUGHPUT.md).
This follow-up changes frontend pacing, not PS2 hardware timing or the renderer.
Physical Vita FPS remains unmeasured, and audio remains deferred.

## The frontend ceiling

The Vita monitor previously called `Emulator::run_slice(25000)` exactly once
before drawing its UI and waiting for VBlank in `Screen::present()`. Even if
execution became much faster, that loop could request only 25,000 guest
instructions per display refresh. At a hypothetical 60 refreshes/s, that is a
1.5-million-instruction/s request ceiling, not a measured interpreter rate.

The replacement gives each running UI iteration an **8,000-microsecond soft
compute budget**, using the existing wide process microsecond clock. It repeats
small **2,000-instruction quanta**, up to a finite **1,024-quantum safety cap**.
Controls are polled before computation; presentation and progress reporting
remain outside the inner quantum loop. There is no skipped guest draw, new
fixed delay, host-time-driven device advance or change to raster resolution.

All EE/IOP/DMA/VIF/VU/GIF work remains inside `Emulator::run_slice`. That method
still splits execution at guest device events and the BIOS IOP horizon. Changing
how many calls fit before presentation does not replace those guest horizons
with wall-clock timing.

## Bounded clock handling

The portable budget controller is independent of the emulator and Vita APIs.
Its injected clock and callback make the policy deterministically testable.
It stops on elapsed budget, a terminal callback, a stalled or backward clock,
or the finite quantum limit. Zero/invalid budgets perform no guest work.
Elapsed-time subtraction is checked for monotonicity rather than constructing
an overflowing `start + budget` deadline. Budget/clock statuses are not guest
CPU faults and do not set the interpreter's sticky halt request.

The time budget is deliberately **soft**: one `run_slice` can synchronously
service and rasterize a large packet before returning. The controller cannot
preempt that work, and UI drawing/report-file I/O is outside the compute
interval. This is not a hard 8 ms responsiveness or 60 FPS guarantee.

Progress scheduling is restarted when guest execution is reset/reloaded; its
next million-cycle threshold saturates instead of wrapping. Manual stepping
also uses the scheduler entry point so it does not bypass guest device service.

## Validation

The portable helper passes **163 checks** for exact/crossed deadlines, a long
single quantum, terminal-stop precedence, clock stalls/regressions/wrap,
invalid inputs, maximal integer arguments, reused functor state and saturated
progress thresholds including duplicate suppression. No real-time timing
threshold is used in these tests.

The scheduler suite passes **1,398 checks**. Owned ELF fixtures verify that a
terminal BREAK stops paced execution without an extra guest slice. Owned reset
ROM fixtures kick normal/source-chain VIF DMA and a separate PATH3 GIF DMA
from actual EE stores, run a live IOP delayed-load/branch/store loop, and draw
an independently expected colored sprite. Paced and fixed-total execution
agree at 257 EE instructions and again after a 17-instruction continuation.
The comparison covers EE/IOP architectural fields, all words in the touched
RAM region, DMA/interrupt registers, all 4,096 VU1 data words, every aligned
word of 4 MiB logical graphics memory, the whole active preview and packet
counts. Quantum sizes 1, 7 and 2,000 exercise device boundaries and both
elapsed-budget and finite-quantum-limit host frames.

Final Release and UBSan suites each pass **44/44 CTest targets**. The VitaSDK
cross-build produces `ps2vita.vpk`. A transient sub-second WSL/Windows timestamp
skew warning disappeared on the subsequent completed builds; neither gate is
a new Vita3K run or a physical-device performance measurement.

## Resume point

Measure the complete Vita runtime before making FPS claims. A host replay
submit rate, encoded checkpoint-video frame rate and UI refresh rate are three
different quantities; none establishes guest frame completion or retail-game
performance. The local checkpoint video remains ten captured states, not a
continuous verified startup animation.

Further exact renderer candidates are integer edge recurrences and per-row
sprite texture-coordinate work. Keep the existing sampler rounding/order and
live-VRAM feedback, and require unchanged per-grid hashes before keeping a
performance change. VU1 arithmetic/timing, native GS storage, display scanout
and complete intro correctness remain open.
