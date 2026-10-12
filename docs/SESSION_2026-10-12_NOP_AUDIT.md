# NOP performance candidate rejected; independent retirement coverage

The real-boot DIRECTHL fix is saved separately in
[its checkpoint](SESSION_2026-10-12_DIRECTHL.md). This investigation changes
**no production CPU, interrupt, device or scheduler code**. Audio remains deferred.

## Candidate and decision

An ignored prototype adds an exact-zero early return inside `Cpu::execute`.
Word zero is `SLL zero,zero,0`, whose destination write is discarded. The
ordinary `Cpu::step` still owns fetch validation, halt/interrupt handling,
zero-register normalization, Count/cycle increments, one-cycle device advance
and branch-delay retirement. Nonzero SLL words must not take that shortcut.

VitaSDK ARM O3 inspection shows the candidate's zero path returning before the
usual stack frame/GPR reads. `execute` shrinks from 0x39CC to 0x38D8 bytes;
`step` and `run` sizes are unchanged. Nonzero words pay an extra compare and
branch; whole-function register allocation/layout also changes. Host x86 code
does not have the same prologue behavior as ARM, so neither size nor a host
timing is a physical-Vita speed measurement.

Seven alternating baseline/candidate pairs run each of three owned eight-word
loops through ordinary `Cpu::run`: 87.5% NOP, no NOP and 37.5% NOP. Each sample
warms 100,003 cycles, resets, then times 3,000,003 cycles. All **42** runs match
their seeded CPU/program/selected-device digests and odd-quantum continuation
digests. This is not a full whole-Emulator state oracle.

Median paired candidate/baseline duration ratios are 1.00475 for NOP, 1.00284
for nonzero and 0.97851 for the blend. The candidate wins only 3/7, 3/7 and 4/7
pairs respectively; the blend includes a large baseline outlier. The real BIOS
probe competed for host resources, so these uncontrolled results establish
**no repeatable benefit**. The prototype is **not merged** and no emulator/Vita
FPS improvement is claimed. Raw owned results and ARM objects remain in the
ignored `.tools/nop-20261012/` diagnostic directory.

## Retained correctness tests

The new strict-warning `cpu_nop_tests` fixture uses explicitly constructed
expected architectural state, not `run` versus `step` sharing the same decoder.
It covers all seeded CPU fields and diagnostics, dirty low/high GPR zero halves,
COP0 Count wrap, taken-branch NOP delay slots with an enabled interrupt deferred
until the target, nonzero SLL/sign extension/discarded destinations, halt before
fetch, alignment/unmapped fetch faults, prior fault retention, all four Timer 5
prescalers at exact target boundaries and owned GIF DMA discovery/retirement.

The strict fixture passes **77 checks** on the unchanged production core.
Integrated Release and full UBSan each pass **60/60 CTest targets**. The fixture
also passes against the fully ASan+UBSan-instrumented core. No production code
changes follow the separately cross-built DIRECTHL package, so that package's
runtime/SELF remains the same; this additional checkpoint is test coverage and
an explicitly rejected performance experiment.

## Other unimplemented prospect

Validating only the first four instruction bytes before a possible zero-fill
loop's full 28-byte fingerprint would avoid a second TLB translation only at
page boundaries. The current validator translates once for same-page spans;
the observed EE idle loop never crosses a page. True SQ candidates would pay
an additional validation. No patch or benchmark is warranted without measured
boundary hotspots. Existing side-effect-free fetch guards and event horizons
remain intact; no direct-RAM masking or idle-cycle skipping is introduced.
