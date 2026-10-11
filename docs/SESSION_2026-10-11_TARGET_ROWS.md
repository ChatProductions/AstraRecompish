# Session 2026-10-11: target setup and bounded unmasked rows

Continued FPS work from `main` checkpoint `b07f3b2`, retaining exact logical
VRAM output rather than changing resolution, filtering or guest timing.
This is a host graphics-submit improvement, not measured Vita FPS, verified
complete startup animation or playable retail games. Audio remains deferred.

## Reprofile and exact contracts

A new quarter-grid `-O3 -pg -fno-partial-inlining` profile after the word-access
change attributes sampled self time to texture sampling (40.18%), triangle
work (22.28%), fragment writes (14.71%) and target writes (13.39%). It counts
107,644,824 target writes across one warmup and five trials. Instrumented
percentages identify remaining work; they are not an uninstrumented speedup.
The saved profile is `build-release/profile-after-words.txt`.

ARM inspection shows repeated loads of FRAME base, stride, vector pointer and
data pointer after individual word stores. The first candidate snapshots only
target setup for one `target_write` call and computes its initial row/stride
once. No callback or rebinding occurs inside that tile; diagnostic before-values
are collected before it and callbacks run after it. No destination value or
pointer is cached between calls. Masked words still read live before their own
store, in the same row/column order, including the explicit byte fallback.

The second candidate adds a contiguous row copy **only for known-little-endian
default scale 4 and an effective zero mask**. A four-word row is 16-byte-aligned:
FRAME base is a multiple of 8192, native pitch a multiple of 64 pixels and the
sample's X offset a multiple of four native pixels. Thus the wrapped row starts
at most at `0x3FFFF0`; its full 16 bytes cannot cross the 4 MiB boundary. Later
rows can wrap normally. The local repeated-color array is separate from VRAM,
and copying it with `memcpy` introduces no typed-aliasing cast.

Nonzero FBMSK, PSMCT24 and RGB_ONLY still preserve each destination word; no row
copy is used for those raster writes. `clear()` retains its existing explicit
zero-mask semantics even on PSMCT24. Unknown byte-order builds retain individual
byte accesses. Alpha/depth rejection, blending, accepted-write diagnostics and
live aliased texture feedback remain unchanged. No hardware-swizzling or timed
scanout claim is added.

The Vita quarter-grid target-write code now loads target setup once. Its row
copy at `0x6F4` uses `vdup.32` to prepare repeated color, stages the 16-byte array
on the stack, then loops with one wrapped-row mask, a four-register `ldmia` and
four word stores per row. There is no library `memcpy` call. This is **not** four
direct NEON stores into VRAM: the actual generated staging/store sequence matters.
Full GS object text is 11,944 bytes before, 11,760 setup-only and 11,376 final.
Device throughput for these instructions is still unmeasured.

## Candidate selection and timings

Frozen pre-turn binaries/libraries are `build-release/*-before-target-rows*`;
setup-only intermediates are `build-release/*-target-setup-only*`. Corresponding
Vita objects are `build-vita/gs-before-target-rows.obj` and
`build-vita/gs-target-setup-only.obj`.

The first experiment uses three rotated adjacent old/setup-only pairs on each
grid. Default median falls 1.607695191 to 1.481515816 seconds; half falls
5.368589147 to 5.173645683; native falls 18.579467071 to 18.302758963.
The respective nominal throughput gains are 8.52%, 3.77% and 1.51%, but all
ranges overlap and each grid has a regressing adjacent pair. These exploratory
results do not justify firm per-grid speedup claims. Raw logs and summary are
`build-release/target-setup-cost-2026-10-11/`.

After correctness gates, a final quiet comparison rotates all three default-grid
versions through five rounds. Every invocation uses one warmup and one timed
fresh-instance sample. Validated trace preloading, setup, hashing and output
remain outside the submit interval. No project builds, tests, BIOS capture,
video encoding or other timings run alongside the loops.

| Default-grid version | Submit times (seconds), chronological | Median |
| --- | --- | --- |
| Frozen `b07f3b2` | 1.624609879, 1.870526299, 1.638540376, 1.572144982, 1.575631181 | 1.624609879 |
| Setup-only | 1.482957032, 1.534346027, 1.624571161, 1.492496081, 1.568875038 | 1.534346027 |
| Final setup + rows | 1.372281402, 1.296218537, 1.432894298, 1.316997830, 1.483606113 | 1.372281402 |

The final pair gives **15.53% less host graphics-submit time**, or **18.39%
higher throughput**, versus the previous word-access checkpoint. Final versus
setup-only medians give 10.56% less time, although those ranges slightly overlap.
The final and pre-turn ranges do not overlap in this batch. Host frequency/load
are uncontrolled, and these numbers must not be added to earlier session gains.
They exclude EE/IOP/VU execution, presentation, guest frame completion and Vita FPS.

The row-specific change is compiled out of half/native builds: both benchmark
executables are byte-identical to their setup-only intermediates. SHA-256 is
`9abc094f9149fc7cb20101cebac49ae2ccd49c160529ea906faeee8e4cd0e04f` for half
and `e8832c23747ed99034b99ec255dd52eb2828b040e677bb3baf83b7d46c0a8060` for native.
Their earlier exploratory setup timings are not promoted into device claims.

Final 15-run logs/summary are in `build-release/target-row-cost-2026-10-11/`;
ignored `.tools/measure_target_rows.ps1` rotates versions and validates complete
state. The shared setup probe is `.tools/measure_target_words.ps1` with
`-Directory build-release/target-setup-cost-2026-10-11 -BeforeSuffix before-target-rows`.

## Unchanged output and stronger gates

All warmups/trials reproduce FRAME `00000000000A00F0`, 59,964 submissions,
42,686,400 payload bytes, 2,157 sprites, 384,780 triangles, 2,808 lines,
368,640 IMAGE/local bytes and zero rejection/pending data. The default-grid
preview hash is `FB810EF9FC0E36C9`; the full 4 MiB word hash is `9ABC363A5342F462`.
Half/native hashes retain the
[per-grid word-access baseline](SESSION_2026-10-11_TARGET_WORDS.md#rotated-adjacent-comparison).
The input remains SHA-256
`39e00de6b227debe926af59bcfd3fd354cd122c0e313357b3e3ff9dfd5def9ad`.

The target byte oracle grows from 43 to **204 checks per grid**. It adds base
0/511 × FBW 1/2/10/63 cases, independently derived row/end/wrap samples, distinct
destination alpha, reverse-order writes, unmasked/partial/full masks, PSMCT24,
RGB_ONLY and full VRAM/preview equality. Clear is tested under PSMCT24/full mask;
six unsupported bindings retain host-buffer fallback without memory writes.
Valid A → B → A buffer switches test fresh pointers and live destination reads.
Earlier observer, blend/clamp/PABE and same-draw feedback cases remain.

All three grids pass ASan plus UBSan normally, with byte-order detection
undefined, and against the frozen pre-turn libraries: nine independent runs,
each with 204 checks and zero failures. Direct builds use strict warnings and
`-Werror`, with no diagnostics; artifacts are in ignored
`build-target-oracle-204-lIsnWz/`. Byte fallback tests are not a complete big-endian port.
Release and UBSan each pass **53/53 CTest targets**; the VitaSDK VPK build passes.
The row-copy byte count explicitly uses `kRasterScale * sizeof(std::uint32_t)`
rather than the size of the array object. Rebuilding after that portability
clarification leaves the measured quarter-grid executable and Vita GS object
byte-identical; Release and UBSan again pass all 53 targets.

## Local Vita3K hand-off

The user requested a Vita3K trial of this checkpoint. The rebuilt VPK is copied
to the Desktop as `AstraRecomp-PS2-Vita-2026-10-11.vpk` (199,463 bytes; SHA-256
`f48f06c2598e34858b0946692a1de4fa88f6d8ee7e4bc73647d514293ce5a5c1`).
The installed `PS2VITA01` app and its external data were backed up and verified
under ignored `build-vita/` before using the existing Vita3K 4056 positional-VPK
launcher. Both installed files match their VPK entries, and the user's BIOS is
unchanged. Vita3K opens at its Welcome screen; guest boot and live performance
are not yet verified. Desktop-control initialization was unavailable, so no
setup dialog was clicked or bypassed. The package contains no BIOS.

## Recording and next step

The image is deliberately unchanged. The
[existing actual half-grid checkpoint video](SESSION_2026-10-10_HALF_GRID.md#cost-and-progress-recording)
remains the visual record; encoded playback FPS is not a performance measurement.
This milestone records timing logs rather than manufacturing new visual progress.
No firmware-derived capture is published to GitHub.

Next, profile full EE/IOP/device execution separately or inspect actual sampler
instructions before another source rewrite. Texture sampling now dominates the
instrumented graphics profile. Keep the accurate live path as the oracle; do not
trade away blend/filter/coverage or event timing to improve a benchmark. Full
startup scanout, VU timing/PATH1 correctness and actual Vita FPS remain open.
