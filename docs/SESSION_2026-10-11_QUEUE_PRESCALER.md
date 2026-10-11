# Empty graphics service and IOP timer arithmetic — October 11, 2026

Continue the real PS2 intro and reduce runtime work without changing guest
timing. Audio remains deferred. No physical-Vita FPS or complete intro is claimed.

## Four completed graphics queues, one empty guard

`Emulator::service_graphics` now returns early only if Memory's completed GIF
and VIF DMA deques, VIF's completed DIRECT deque, and VU1's completed PATH1
deque are all empty. Inline presence predicates check the deques, not packet
byte counts, so an actually queued empty VIF END-chain payload is still drained.

The existing drain loops/order remain intact. A zero-budget ready `run_slice`
still services previously completed output. Partial DIRECT data, armed DMA and
an active but incomplete XGKICK are not completed queues; inspecting readiness
does not advance them. No cycles, deadlines, IOP quantum or PATH arbitration
behavior are changed. This optimizes a coarse transport model, not a replacement
for cycle-accurate GIF arbitration.

`graphics_service_queue_tests` passes **297 checks**: four independently isolated
producer routes, repeated empty service, poison output on failed pops,
zero-budget drains, mixed submission order, earlier output surviving a rejected
VIF tail, empty END-chain output, normal GIF QWC0 fail-closed behavior, fragmented
DIRECT and live XGKICK header/payload visibility. Test-only VIF injection uses
an actually mutable Emulator; no mutable production queue API was added.

A strict full-core ASan+UBSan build passes the same 297 checks with all core
translation units instrumented. An ignored local Memory-only guard mutant fails
six assertions covering DIRECT-only/PATH1-only and later fragment completion.
Thus a guard that forgets either independent output route is actually detected.

The generated Vita ARM empty branch performs four deque comparisons and returns
without packet-vector initialization, stack frame or out-of-line pop calls.
The full service symbol grows from `0x98` to `0xD6` bytes; this is an empty-path
work reduction, not smaller total code or a measured throughput claim.

## Timer 5: preserve division results without calling a divider

Ordinary `Cpu::step` invokes `Memory::advance(1)`, so device-clock work is hotter
than the outer eight-cycle BIOS/IOP service boundary alone suggests. Timer 5's
existing divisors are `{1,8,16,256}`. Replace only the prescaler division and
remainder with shifts `{0,3,4,8}` and a corresponding mask. Keep the uint64
accumulator and count expansion; do not narrow arithmetic or alter any clock.

Target-reset modulo uses an unrelated, guest-programmed divisor and remains
unchanged, as do target/overflow interrupt ordering, gate handling, mode writes,
acknowledgements, remainders and event horizons.

`iop_timer_prescaler_tests` uses an independently division-based reference for
the existing timer model and passes **876 checks**. Coverage includes all four
selectors, exact carry boundaries, irregular/zero advances, byte divisor changes
with an old remainder, gated nonzero remainders, halfword/word mode writes,
unread active flags, target/reset/repeat/toggle/overflow ordering, clear and a
maximum uint32 advance. This validates unchanged model behavior, not real PS2
timer accuracy. A strict isolated Memory/SPU2 ASan+UBSan build also passes.

The actual Vita `Memory::advance` object formerly had two `__aeabi_uldivmod`
call relocations. It now has only the preserved target modulo call; the
prescaler call is gone. Symbol size grows `0x197E → 0x1990` (+18 bytes).

## Whole-system before/after oracle

Freeze the pre-guard library, executable and matching headers at main
`190839a91a790c1b4a8d1bcbe52c77a95e016b02`. Library SHA-256:
`f513464afcca840ad2d3d6a3e14f38b4c1eca2d3cc2d4936eaea23e70a86f818`.
The separately frozen queue-only library has SHA-256
`ebe5c2fbb22b822fd9b0f91960df076c56bc96597f5d153323aefa215c52c4a0`.
No class layouts change in this checkpoint.

Fresh pre-guard, queue-only and queue-plus-prescaler boots at **300,000,000
modeled EE cycles**, 2048-cycle outer quantum, reproduce all five checked digests:

| Digest | All three variants |
| --- | --- |
| CPU | 1B93EE2CA2585546 |
| EE RAM | 74942581933E8CDF |
| IOP RAM | E3DDB2552BA15A99 |
| VU | 6AE85C0A5A4737A7 |
| Graphics | 1D6CF63271F225EC |

Each ends at EE PC 00219ACC and IOP PC 0000AE98, with 3,611,510 existing fast-path
credits, 998 VU pairs, 4,416 GIF and 4,364 VIF submissions, and the same final
PAL mode registers. Digests cover public architecture, RAM and logical graphics
storage/counters, not every private pending pipeline/device state.

The single run-slice timings are 147.622397515, 158.207956821 and 174.639985293
seconds respectively. Vita3K was open, and the longer capture/builds/sanitizer
gates overlapped these runs. **This is an equivalence experiment, not a quiet
performance comparison:** the times establish neither speedup nor regression.
Modeled cycles include existing fast-path/interrupt credits and are not retired
instruction rate, real PS2 elapsed time or Vita FPS.

## Regression gates and separate package

Release and full UBSan suites each pass **58/58 CTest targets**, including the
final strengthened timer fixture. VitaSDK cross-build passes. Subsecond
Windows/WSL dependency timestamp warnings occurred in an earlier host build;
no global clock/security setting was changed.

VPK SHA-256:
`b833a2ce08e0df1aa703f0e5dec9c59011b8c03908459ddc0d2edae1afcc2702`.
The ZIP `eboot.bin` agrees with the generated SELF, SHA-256
`f9550292b9260628515ace590ab2b71be509de0142a09a8d2a0fab91236cd3e2`.
Separate Desktop copy: `AstraRecomp-PS2-Vita-2026-10-11-queue-prescaler.vpk`.
The installed Vita3K app and user BIOS are left unchanged.

## Longer intro experiment: validated first-billion prefix

The ignored runner observes guest RAM camera/scene state and saves actual
selected-display images every 50M modeled cycles. It uses ordinary whole-system
execution, no event injection, guest skipping or synthesized graphics. This
longer run was launched with the frozen **queue-only** core before the prescaler
patch; the latter is checked separately above, not falsely attributed to this
capture.

The reset-starting GIF trace is fully finished and detached at **1B cycles** so
it stays within the production reader's 64 MiB/100,000-record bounds. Later
live snapshots continue in the same Emulator instance, but are not part of that
trace and cannot be replayed from its prefix.

The first-1B container SHA-256 is exactly the previous capture's
`d3cc5393cde2e965ec741d7580dc0aececcabec3c15f97d0bad7f09e6b9bd992`.
It contains 68,083 submissions and 48,379,520 payload bytes. Production replay
fully validates it with zero GIF rejection/pending data and unchanged anchors:
preview `516D80164CD3BFB1`, full logical VRAM `23AC840ECC6669C2`, selected
640x256 quarter region `EBDD4780B54674AB` (DISPFB2=1450).

Plain-RAM observations establish that the intro is advancing rather than
remaining in one graphics queue. Camera Z is at 00288EB8, phase at 00346EC0,
guest application counter at 002C8700, current/requested scene at 002C8704/08,
and scene substate at 002C8710. The counter is a guest iteration variable,
**not** an authoritative completed-frame or wall-clock measurement.

At 800M: camera Z 20.5561543, phase 1, iteration 94. At 850M: Z 20.9861927,
phase 2, iteration 102, phase-control flag 002C8794 becomes 1. At 1B: Z
27.4089413, phase 2, iteration 127. At 1.25B: Z 59.298748, iteration 170.
The camera crosses the guest cloud routine's threshold 56 naturally. Inspection
of the actual 1.25B quarter capture shows faded title lettering, cloud detail
and longer colored-light trails, not a verified complete animation.

The VU1 count remains normal 998-pair E-bit termination. Do not inflate it or
call it the cause of missing later geometry: the later cloud DIRECT work is
guest EE-produced, and the prior PATH1 warning was also reference-reproduced.
Firmware and derived captures remain ignored/local, never published to GitHub.
