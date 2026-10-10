# Session 2026-10-10: selected display-buffer diagnostic on Vita

The bounded texture-word milestone is saved on `main` as `dc172c9`. This
follow-up addresses a frontend mismatch: the Vita GS view used `Gs::pixels()`,
which shows the most recently bound drawing FRAME, not the buffer selected by
privileged PMODE/DISPFB registers. Saved BIOS checkpoints can select another
buffer, so the monitor and locally extracted startup-region video need not
show the same memory.

## Narrow display-preview contract

`GsLinearDisplayPreview` is a shared, owned-testable helper. PMODE EN1/EN2 must
enable exactly one circuit. The helper decodes that circuit's DISPFB and samples
a fixed 640x448 logical-linear region from DBX/DBY, using its FBW stride. The
output is 160x112, with one sample every four native pixels. RGB is made opaque
for the host monitor; guest alpha is not treated as host transparency.

DISPFB field packing is checked against the [ps2dev gsKit register macros](https://github.com/ps2dev/gsKit/blob/master/ee/gs/include/gsInit.h).
The existing 8 KiB framebuffer-base interpretation agrees with the pinned
[PCSX2 v2.8.2 register definitions](https://github.com/PCSX2/pcsx2/blob/v2.8.2/pcsx2/GS/GSRegs.h).
The diagnostic deliberately wraps physical reads at 4 MiB and retains
narrow-stride aliases. It does not reject a region merely because it extends
past a logical row or the true display dimensions.

This is **not GS scanout**. DISPLAY extents/magnification, interlace/fields,
native page/block swizzling, display timing and PMODE alpha/background mixing
are not implemented by this helper. A 640x448 probe may include adjacent
framebuffer storage below a 640x256 image; the monitor explicitly labels it
as a region, not a complete displayed frame. No guest frame-completion marker
or FPS measurement is added.

Disabled circuits, simultaneous circuits, zero FBW and formats other than
PSMCT32/24 produce a freshly black image and an explicit status. There is no
automatic fallback to a stale selected buffer or the active draw target.

## Vita controls and lifetime

R still toggles GS diagnostic / EE monitor. Within the GS view, L switches
between the selected DISPFB region and the old active draw-target diagnostic.
The latter is explicitly labelled as not the display buffer. Selecting views
does not modify guest registers, advance cycles, rebind the drawing target or
submit graphics commands.

The preview object is reused in function-static storage, not allocated each
frame and not placed as a 72 KiB object on the Vita thread stack. The compiled
ARM object places its `display_preview` symbol in static data with size
`0x1181C` (71,708 bytes including selection metadata). Screen copying is
synchronous. Firmware-derived video/capture assets remain ignored and local.

## Owned regression evidence

The new CTest fixture passes 172 checks in both Release and UBSan:

- Every PMODE enable combination, unrelated/reserved bits, both circuits,
  all 32 PSM field values and zero widths.
- All 17,920 samples of each of eight independently calculated byte-pattern
  regions, including maximum FBP/FBW/DBX/DBY, narrow-stride aliases and 4 MiB
  wrapping. Each sample makes exactly one fresh, physically aligned word read.
- Opaque host RGB, live replacements and full-image black clearing without
  reads for every unsupported/disabled status.
- Real owned GIF IMAGE uploads into two buffers, with a third buffer remaining
  the active sprite draw target. Actual Memory PMODE/DISPFB changes switch the
  selected image without any new draw. Later IMAGE writes appear immediately.
- Repeated preview copies preserve the full 4 MiB VRAM word hash, draw target
  and packet state. Clearing privileged registers removes the prior image.

Release and UBSan each pass **45/45 CTest targets**. The Vita SDK produces the
VPK. No physical-Vita or Vita3K rendering run was performed for this change;
the controls are cross-built and their shared sampling path is host-tested.

## Resume point

The 800M graphics stream and normal 998-pair VU1 endpoint are unchanged; this
does not establish a completed startup animation or playable retail games.
Audio remains deferred. Next work should distinguish selected-buffer content
from real display-circuit scanout and continue profiling measured runtime work,
keeping owned/reference regression oracles and the accurate path intact.
