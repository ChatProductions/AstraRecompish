# Session 2026-10-11: exact framebuffer word accesses

Continued the request to improve FPS from `main` checkpoint `b1cf5ea`.
This milestone reduces measured graphics-submit work without changing raster
density or the image. Actual Vita FPS, complete boot animation and retail-game
playability remain unverified. Audio remains deferred.

## Measured bottleneck and bounded change

A fresh quarter-grid `-O3 -pg -fno-partial-inlining` profile of the saved 800M
GIF stream attributes 33.74% of sampled self time to `Gs::target_write`, 27.91%
to texture sampling and 13.50% to triangle work. Across one warmup and five
trials it counts 107,644,824 target writes. These instrumented samples identify
work, not uninstrumented speed or device FPS. The initial ordinary `-pg` build
mislabels the local target-write clone as `frame_dummy`; the second profile
disables partial inlining to expose its function name correctly.

The frozen Vita GS object confirms actual byte-wise work: each target read
assembles four individually wrapped byte loads, and each destination word
uses four byte stores. The new implementation uses alias-safe `memcpy` words
only on compiler-identified little-endian targets. Unknown byte-order builds
keep the explicit byte route.

All private target-read callers use FRAME pixel addresses or validated aligned
physical watches. FRAME bases are multiples of 8192 and native pixel offsets
are multiples of four. Masking an aligned address to 4 MiB yields at most
`0x3FFFFC`, so its entire word is contiguous and in bounds. A tile may wrap
between words or rows; no word crosses the allocation boundary. Stores have
the same alignment proof and retain the original row/column ordering.

The effective-zero-mask shortcut still applies only to replicated grids;
nonzero FBMSK, PSMCT24 and RGB_ONLY read and preserve each destination word
separately. Blending, diagnostics and same-draw feedback still read live VRAM.
There is no persistent texture/pointer cache, typed aliasing cast, skipped draw,
reduced filtering, altered guest timing or new Vita resolution. `clear()` keeps
its existing explicit zero-mask behavior.

The final Vita target-read function at `0x59C` contains one address mask and one
word `ldr`, rather than four byte loads. Masked destination writes contain a
word `ldr`/`str` pair; overwrites use word stores. These fixed-size copies do not
call a library `memcpy`. The complete GS object grows from 11,844 to 11,944 text
bytes; this is an instruction-path improvement, not a whole-object size saving.

## Rotated adjacent comparison

Current Release binaries and libraries were frozen before editing as
`build-release/*-before-target-words*`. The Vita object is
`build-vita/gs-before-target-words.obj`. Each invocation uses one warmup and one
fresh-instance sample, with the validated stream preloaded before timing.
Three rounds rotate quarter/half/native order and alternate old/new order.
No builds, tests, BIOS capture, video encoding or other project timings run
alongside the measured loops. Host frequency and external load are uncontrolled.

| Grid | Frozen baseline submit seconds | Word-access submit seconds | Baseline median | New median |
| --- | --- | --- | --- | --- |
| Default quarter | 2.486565200, 2.336134597, 2.185607991 | 1.683231700, 1.764890741, 1.636941167 | 2.336134597 | 1.683231700 |
| PC-only half | 6.635936083, 6.364020353, 6.423563488 | 5.500168294, 5.497065566, 5.573313467 | 6.423563488 | 5.500168294 |
| PC-only native | 20.509564285, 19.847064669, 20.145963318 | 18.335993805, 20.163137249, 18.718756712 | 20.145963318 | 18.718756712 |

The default-grid median gives **27.95% less host graphics-submit time**, or
**38.79% greater throughput**, in this pair. Half-grid time falls 14.38%, or
16.79% greater throughput. Native medians suggest 7.62% greater throughput,
but the ranges overlap and one adjacent pair regresses: that is not a robust
native-grid speedup claim. Absolute times differ from earlier sessions; do not
combine these uncontrolled batches into a cumulative performance percentage.

These intervals submit an existing captured GIF workload, not its preceding
800 million EE instructions. They exclude full EE/IOP/VU execution, presentation,
frame completion and physical Vita performance. They do not establish FPS.
The benchmark's scope label explicitly preserves that distinction.

Every warmup and sample keeps FRAME `00000000000A00F0`, 59,964 submissions,
42,686,400 payload bytes, 2,157 sprites, 384,780 triangles, 2,808 lines,
368,640 IMAGE/local bytes and zero rejected/pending GIF data. Per-grid hashes:

| Grid | Active-preview word hash | Complete 4 MiB logical-VRAM word hash |
| --- | --- | --- |
| Quarter | `FB810EF9FC0E36C9` | `9ABC363A5342F462` |
| Half | `A7844706F03EBBD5` | `4E2F3D91A7198922` |
| Native | `EB315B0A3A414B7C` | `C0F2910304BAA56B` |

Input `build-release/source-extent-check-800m.agf` remains 44,605,312 bytes,
SHA-256 `39e00de6b227debe926af59bcfd3fd354cd122c0e313357b3e3ff9dfd5def9ad`.
Ignored `.tools/measure_target_words.ps1` validates complete state per run and
saves raw logs and `summary.json` under
`build-release/target-words-cost-2026-10-10/`. The directory was named before
the local date rolled over. No firmware-derived inputs are committed.

## Independent gates

`gs_target_access_tests.cpp` is separately linked against default, half and
native libraries. Each configuration passes 43 checks using independent byte
assembly, uint64/modulo addressing and full-4-MiB byte comparisons. Cases cover
the final aligned word, FBP511 wrapping into byte zero, distinct per-word alpha,
zero/full/partial masks, PSMCT24, RGB_ONLY, narrow-stride rejection, wrapped
clear footprints, every preview pixel, observer before/after identity, live
blend reads with clamp/PABE and same-draw aliased texture callback feedback.

The feedback fixture initially assumed a closed diagonal and failed against
both the new and frozen old renderer. Its independent expectation was corrected
to the established top/left coverage rule: three accepted anchors, not six.
No production coverage change was made to accommodate the fixture.

All three fixtures also pass against their frozen pre-change libraries.
Each grid passes AddressSanitizer plus UBSan both normally and with
`__BYTE_ORDER__` undefined, forcing these helpers' byte fallback. This checks
the fallback implementation, not a complete big-endian project port.

Release and UBSan pass **53/53 CTest targets** each. The VitaSDK cross-build
produces the VPK. These are host correctness/build gates, not device validation.

## Recording and resume point

The image is intentionally unchanged, so a new held-checkpoint video would not
show this performance change. The actual 15-second
[half-grid recording](SESSION_2026-10-10_HALF_GRID.md#cost-and-progress-recording) remains the
visual record: ten captured 350M-800M states, not live FPS or a complete intro.
This session adds raw throughput logs instead of presenting encoded playback
rate as a performance result.

Reprofile the retained implementation before choosing another optimization.
Potential next work is reducing repeated target-row setup without caching
destination values, or profiling the complete EE/IOP/device runtime separately.
Keep per-grid hashes and the accurate live-feedback path as gates. The known
VU1 normal E-bit endpoint at 998 pairs/PC `0308`, PATH1 issues, native GS storage,
scanout and complete animation remain separate correctness work.
