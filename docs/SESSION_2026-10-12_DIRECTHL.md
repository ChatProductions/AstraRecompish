# Later-boot VIF DIRECTHL boundary - October 12, 2026

Continue the actual PS2 boot sequence without substituting graphics or adding
guessed delays. Audio remains deferred. Firmware and derived diagnostic assets
remain local/ignored; they are not regression fixtures or GitHub assets.

## First failure, located before changing the parser

The fresh diagnostic runs ordinary whole-system execution with the normal
rasterizer and no GIF recording. Its outer slices are 2048 modeled EE cycles
until exactly 1.4B, then one cycle, with a 1.7B cap. The first VIF rejection is
at **1,585,418,617 cycles**, EE PC `002675F8`, IOP PC `0000AE94`:

- Command `51000004`: **DIRECTHL (0x51), four qwords**.
- VIF submission 100,839, command offset 116, submission size 192 bytes.
- STCYCL `0404`; 318 uploaded microinstructions and 124 unpacked vectors.
- MSCAL 1, MSCNT 6; VU1 remains normally stopped at PC `0308`, 998 pairs,
  with no recorded unsupported upper/lower instruction.
- Animation-thread state is already iteration 207, current/requested scene 2,
  substate 0. This later rejection is not the cause of its earlier normal exit.

The stopped diagnostic saved EE RAM and VU data/micro memory. Memory's DMA
source spans reconstruct a 192-byte candidate with complete nonoverlapping
coverage; the reconstructed failed code matches the sticky VIF diagnostic.
The command maps to scratchpad `7000008C..8F`, followed by a 64-byte GIF payload
whose source span starts at EE RAM `00700000`.

This is **post-completion source-snapshot evidence**, not an immutable capture
of the entire historical VIF submission. Other transfers may write source RAM
after the VIF copy but before the diagnostic observes it. Only the stopped
Memory copy clears its TLB for raw source inspection; the live execution is
not altered. No firmware-derived packet is added to the public test suite.

## Bounded parser change

VIF opcode `0x51` now shares `0x50`'s existing streamed DIRECT payload handler.
Both use the 16-bit immediate qword count, where zero means 65,536 qwords.
Completed earlier PATH1 packets retain the existing functional queue order;
payload bytes are not reinterpreted as VIF commands, and only a fully received
DIRECT payload is queued for GIF.

This is **payload/command support**, not full DIRECTHL hardware semantics.
GIF path arbitration, priority and backpressure remain unmodeled. No delays,
new cycles, scheduler quanta or guessed XGKICK latency are introduced. Masking
the command's IRQ bit follows the existing decoder; this does not implement
VIF interrupts. Command-header fragmentation remains unsupported, separately
from the already supported streaming of DIRECT payload bytes.

An independent comparison with the pinned primary reference confirms the
shared length encoding and also its distinct DIRECTHL transfer kind:
[PCSX2 v2.8.2 Vif_Codes.cpp](https://github.com/PCSX2/pcsx2/blob/v2.8.2/pcsx2/Vif_Codes.cpp#L132-L184).
No reference implementation code is copied.

## Owned regression and build gates

The new strict-warning `vif_directhl_tests` target passes **2,992 checks**.
It covers DIRECT/DIRECTHL and their masked IRQ-bit forms, streamed payloads
with 1..32-byte quanta (including split GIF headers), command-looking payload
bytes, following STCYCL/UNPACK commands, independent census buckets, mixed FIFO
and completed PATH1 ordering, QWC zero/maximum bounds, pending reset, unchanged
split-VIF-header rejection, incomplete GIF buffering, later unsupported VIF
commands and normal DMA-to-GIF draining on a zero-budget ready slice.

The same fixture linked against the frozen pre-change core fails **92 expected
checks** specific to the unsupported `0x51`/`0xD1` route; the old `0x50`/`0xD0`
cases pass. Release and full UBSan each pass **59/59 CTest targets**. The new
fixture also passes against the fully ASan+UBSan-instrumented core. VitaSDK
cross-build passes. Earlier subsecond Windows/WSL timestamp-skew warnings are
not treated as a timing measurement or fixed through global clock changes.

## Fresh post-change boot outcome

The fresh diagnostic finishes at **1,700,000,000 modeled EE cycles**, stop
reason `step limit`, with **zero VIF packet rejections** and **nine DIRECTHL
commands accepted**. There is no sticky unsupported VIF code/packet. Final EE
PC is `00081FD0`, IOP PC `0000AE98`, IOP cycle count 212,500,000. It records
103,645 VIF submissions, 103,652 DIRECT commands, 106,466 GIF submissions,
318 uploaded microinstructions, 124 unpacked vectors, 696,010 triangles and
3,955 sprites. MSCAL/MSCNT counts remain 1/6; VU1 is still the normal stopped
998-pair program at `0308`, with no unsupported instruction.

At 1.6B the old nine VIF rejections are already absent. Selected-display and
draw-target quarter previews are saved at 1.55B, 1.6B, 1.65B, 1.7B and
termination. The 1.6B previews are visually inspected and black; selected
1.6B/1.7B PPMs have identical SHA-256
`64fb49a2d2e06cc138056ba6fab5a33d564b3ac1aacd2a41d6d14ddaff31f4c2`.
Thus the missing opcode is demonstrably fixed, **not** the complete later
rendering or menu. The sprite plateau warrants inspecting actual GIF payloads
and buffering next, rather than inventing more VU work or transport delays.

Local diagnostic: `build-release/vif-first-reject-directhl-2026-10-12/`.
Its frozen executable SHA-256 is
`a17a4a3d6a8493d056cd4523c98da95bcce0c13af3209c59aacb88668132cb55`.
It does not record the complete GIF stream or report final pending GIF bytes;
neither whole-stream validity nor a later replay oracle is claimed. Host
duration is diagnostic only, not physical-Vita or Vita3K FPS.

The GIF rejected-packet counter alone is not a strict validity oracle: the
current GIF parser can buffer an incomplete declared payload and still return
success without incrementing that counter. Validation must also inspect pending
data, output and the relevant VIF/guest state. No GIF rejection-policy change
is folded into this small opcode fix.

## Separate package, installed app unchanged

VPK SHA-256:
`d106e4d1e9821b863d1da4f47f2e6c287d722e15ae949eac8c66d9831a417776`.
Its packaged `eboot.bin` agrees with the generated SELF, SHA-256
`e434021b56cfead7a6bf7f12a9ca5f84a0c1f77c0bf4da333a5894cca6f96c4d`.
Separate Desktop copy: `AstraRecomp-PS2-Vita-2026-10-12-directhl.vpk`.
This is a new file, not an overwrite/reinstallation of the user's Vita3K app
or BIOS data. Complete intro/menu correctness and physical-Vita FPS remain
unverified.
