# Session 2026-10-11: remove unused texture-tap clearing

Continued performance work from `main` checkpoint `7d12649`. The retained
change removes unnecessary temporary initialization, not filtering, texture
reads, diagnostics, guest timing or resolution. Audio remains deferred. There
is no further BIOS execution, verified complete startup or measured Vita FPS.

## Retained contract

The Vita sampler allocates a four-entry, 80-byte `Tap` array. Its original
value initialization produces an unconditional `memset(..., 80)` even for
nearest filtering with no pixel observer. `read_tap` assigns every member:
mapped U/V, wrapped address, raw texel and expanded color. Width-zero and
unsupported formats return before creating the array.

Remove the local member defaults and array value initialization. Nearest
assigns and reads only tap 0. Linear assigns all four before interpolation or
trace construction. `GsTextureTrace` retains its zero defaults, so unused
bilinear-only trace fields stay zero. No new persistent pointer or texel cache
is introduced; live framebuffer feedback and all source-read ordering remain.

The final Vita GIF object is byte-identical to the isolated tap-init-only
candidate, SHA-256 `49efc2b6543ff51b2dbe58016b4da5096c64d03cfbe14897617822d709cbfdfb`.
Its full object text shrinks 14,185 to 14,113 bytes, and `sample_texture` shrinks
from `0xCB0` to `0xC68` bytes. The unconditional 80-byte clear disappears.
The function still reserves 612 stack bytes; conditional trace initialization
and the existing NEON-vectorized channel interpolation remain. Do not claim
that all sampler stack traffic or clears have disappeared.

## Rejected packed-interpolation candidate

A separate candidate splits RGBA into two 16-bit-spaced byte lanes (RB/GA),
then computes unsigned weighted sums with weights `16-f` and `f`. Each lane's
numerator is at most 4,080, so no interlane carry or uint32 overflow occurs.
Masking after the shift removes the high lane's fractional residue. Three
calls preserve horizontal-then-vertical flooring; TEXA expansion still precedes
filtering and texture-function saturation still follows it.

The candidate passes exhaustive per-byte pairs/fractions under mixed-channel
patterns and saturated-neighbor isolation, plus extrema and two-stage rounding
cases. Those experimental suites pass strict ASan/UBSan, forced byte fallback
and frozen pre-turn libraries on scales 4/2/1 (625/577/565 checks).

However, actual ARM output replaces the existing vectorized channel loop with
scalar multiply/add/mask work. `sample_texture` grows from tap-only `0xC68` to
`0xCF6` bytes, and the complete object grows to 14,257 bytes. A smaller-looking
source expression is not evidence of better Vita execution. With no measured
ARM benefit and inconclusive host timings, the packed helper/integration and
its unused production API are removed. Experimental binaries and sanitizer
artifacts remain ignored; they are not the shipped path.

## Isolated host comparison

Five rounds rotate baseline, tap-only and packed candidates. Each invocation
validates/preloads the full trace, warms once, then times one fresh-instance
submit loop; setup, preview/VRAM hashing and output remain outside the interval.
All builds, tests and other project timing work finish first. Vita3K is no
longer running when these trials begin; it was not closed by this work. The
script refuses to run if the user's Vita3K trial restarts. Host frequency/load
are uncontrolled.

| Quarter-grid version | Submit seconds, chronological | Median |
| --- | --- | --- |
| Frozen `7d12649` | 1.011576694, 1.022760917, 0.914636814, 1.087280270, 0.942630445 | 1.011576694 |
| Retained tap-only | 1.087840200, 0.972117503, 0.981642598, 1.000552098, 0.873498700 | 0.981642598 |
| Rejected packed + tap-only | 0.824372714, 0.924247200, 0.900755501, 0.880459800, 1.211205900 | 0.900755501 |

Tap-only has a nominal 2.96% submit-time reduction. Packed has a nominal 8.24%
reduction versus tap-only, but all ranges overlap and the last packed trial
regresses substantially. These do not establish a new reliable speedup; no
headline FPS gain is added. Retaining the bounded clear removal rests on the
generated instruction removal and correctness gates, not a device claim.
These are graphics-submit measurements, not EE/IOP/VU rates or completed frames.

All 15 trials retain FRAME `00000000000A00F0`, preview `FB810EF9FC0E36C9`, full
VRAM word hash `9ABC363A5342F462`, 59,964 submissions, 42,686,400 payload bytes,
2,157 sprites, 384,780 triangles, 2,808 lines, 59,951 PACKED/13 IMAGE tags,
368,640 IMAGE/local bytes and zero rejected/pending data. Input SHA-256 remains
`39e00de6b227debe926af59bcfd3fd354cd122c0e313357b3e3ff9dfd5def9ad`.

Logs/summary: `build-release/sampler-candidates-cost-2026-10-11/`.
Ignored probe: `.tools/measure_sampler_candidates.ps1`.
Frozen binaries: `build-release/*-before-sampler-lanes`, `*-tap-init-only` and
`*-packed-sampler-lanes`. ARM objects: `build-vita/gif-before-sampler-lanes.obj`,
`gif-tap-init-only.obj` and `gif-packed-sampler-lanes.obj`.

## Resume point

Keep the original vectorized filtering as the accurate path. Added 36 nearest
draw fixtures: three formats times ST triangle/UV triangle/UV sprite times
AEM-zero/AEM-disabled-zero/nonzero-TA0/high-bit-TA1 samples. Owned contrast
texels distinguish the first tap from its neighbors. Extended nearest traces
must retain zero in every unused bilinear-only field/array. The final suites
pass 872/824/812 checks on scales 4/2/1, under strict ASan/UBSan, forced byte
fallback and frozen pre-turn libraries: nine independent successful runs in
ignored `build-nearest-tap-oracle-L7IlWU/`.

The final quarter benchmark is also byte-identical to the measured retained
candidate (SHA-256 `90753f3b2a0d342f59fc790199a866a592b00fd4a4f682a186254ff08c9fdca1`).
The final Vita package is built and its `eboot.bin` matches the generated SELF.
WSL/Windows timestamp skew produces sub-second make warnings during packaging;
these are recorded rather than called a warning-free build. The Desktop
package installed for the user's earlier Vita3K trial is not replaced while
they test it.

Final Release and UBSan each pass **53/53 CTest targets**. Post-selection
full-trace checks preserve each grid's own preview/full-VRAM hashes:
quarter `FB810EF9FC0E36C9` / `9ABC363A5342F462`, half `A7844706F03EBBD5` /
`4E2F3D91A7198922`, native `EB315B0A3A414B7C` / `C0F2910304BAA56B`.
Their counts/FRAME/rejection state also match the prior checkpoint. These
post-selection invocations overlap validation work and are correctness checks
only; their timings are not used in the quiet comparison above.

The rebuilt VPK SHA-256 is
`68e586794a07b8ebccbaca85a5a02873bbd43ac1b18d36508b1c2a9b6bef95b4`.
A separate Desktop copy, `AstraRecomp-PS2-Vita-2026-10-11-sampler-init.vpk`, is
prepared for a later trial, without changing the installed app or BIOS.

Next, profile whole EE/IOP/device execution separately, or isolate conditional
diagnostic stack cost before another sampler rewrite. Source simplification
alone is not a performance result. The existing checkpoint video remains the
visual record because this change intentionally preserves the image; firmware
captures stay local. Full scanout, VU timing/PATH1 correctness and Vita FPS are
still open.
