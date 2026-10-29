# NFSMW 2005 PS2 → Vita: AstraRecomp research checkpoint 0.3

This branch is an experimental *native recompiler* starting point, not a
playable PS2 game. It does not modify the existing NFSMW Vita port, its car
files, or any saves. Private title-specific test VPKs are distributed through
VitaTunnel-Relay; `ux0:data/ps2vita/boot.elf` is supplied independently and
is **never** included inside these VPKs.

## VitaTunnel r7: real, read-only FILEIO RPC operations

The r6 Vita fault report reached SIFRPC bind for server `0x80000001`
(FILEIO) after one accepted IOP restart, ten SIFCMD packets, and two CDVD
initializations. Title AOT telemetry confirmed 1,258,988 native function
calls and 3,805,341 translated guest instructions by that stop. This is
partial title execution, not evidence of a fully recompiled game.

r7 adds a **file-backed** PS2SDK FILEIO server to the native IOP service
model. Its RPC open(0), close(1), read(2), and lseek(4) calls use an explicit
platform backend, tracked descriptors, bounded EE destination buffers,
the original PS2 FILEIO argument layouts, and real error/EOF results.
Write(3) returns a read-only filesystem error. Unsupported FILEIO functions
still stop instead of being silently acknowledged.

The Vita backend confines paths to two **external, user-controlled** roots:

- `host:NFS\\...` maps to `ux0:data/ps2vita/NFS/...`.
- `cdrom0:\\...` maps to `ux0:data/ps2vita/disc/...` (if the user has
  independently placed their own PS2 disc files there).

It rejects traversal, unknown devices, and file creation/overwrite. Missing
files return real errors. This change does not mount an ISO, decode the PS2
filesystem, execute actual IOP IRX modules, or bundle proprietary files.
Logs show FILEIO binds, requests, last operation/return code, read totals,
and last requested PS2 path. The existing `boot.elf` and
`IOPRP280.IMG` are never modified or repackaged.

## VitaTunnel r6: full ROMDIR validation, restart gating, AOT counters

The r5 physical-Vita logs verified that the existing image was found at
`ux0:data/ps2vita/NFS/IOPRP280.IMG` (269,889 bytes). The parser listed 18
modules, including SIFCMD and CDVD, but stopped with
`ROMDIR aligned entry exceeds file bounds` before the native reboot.

r6 first reads the terminated directory, then validates every module payload.
Non-final modules still require aligned bounds, but the last module may have
up to 15 bytes of padding omitted *after its real data*. The diagnostics now
record every entry's name/offset/size and the failed entry and bounds; this
will distinguish a genuine malformed image from an unpadded final module.
The optional IOPBTCONF entry is reported, but reboot eligibility is based on
a complete, valid ROMDIR containing SIFCMD and CDVD service modules.
Both progress and fault logs now record actual title AOT function calls and
guest instructions, separately from trace-only AOT counters.

This is still a bounded HLE restart, not execution of proprietary IOP
modules. Hardware behavior beyond the r5 observed reset is unverified.

## VitaTunnel r5: external IOPRP image validation and bounded HLE reboot

An updated title-specific VPK (`NFMRA0001`) now incorporates the native IOP
SIFCMD/RPC/CDVD initialization model, includes a checked ROMDIR parser for the
separately supplied `IOPRP280.IMG`, and writes more detailed startup/fault
diagnostics. The package has only `eboot.bin` and `sce_sys/param.sfo`.

On TRIANGLE (load external ELF), the Vita checks these IOP image locations:

1. `ux0:data/ps2vita/NFS/IOPRP280.IMG` (preferred)
2. `ux0:data/ps2vita/IOPRP280.IMG`
3. `ux0:data/ps2vita/nfs/IOPRP280.IMG`
4. `ux0:data/nfsmw/NFS/IOPRP280.IMG`
5. `ux0:data/nfsmw/IOPRP280.IMG`

The first readable file is parsed as an IOPRP ROMDIR container. The result,
resolved Vita path, module counts, SIFCMD/CDVD module names and the IOP reset
command appear in the ordinary VitaTunnel-uploaded text logs. An absent or
malformed image does not cause a false reboot success.

With a valid ROMDIR and the required SIFCMD/CDVD service modules, a request for the observed IOPRP reboot
starts a **bounded native IOP service restart** after a guest-cycle delay.
This does not execute the proprietary IOP IRX modules. It reinstalls only the
services currently implemented in native C++, with any unknown SIF/RPC call
remaining an explicit stop. This is an on-device *diagnostic checkpoint*, not
an assertion that the game or all its IOP modules are supported.

The user's already-installed `ux0:data/ps2vita/boot.elf` and
`IOPRP280.IMG` remain external and are neither moved nor altered. Install
through VitaTunnel (Square to sync/download/install, Triangle in the installed
test app to load `boot.elf`, Cross to run). Reopen VitaTunnel to send the
`startup.txt`, `progress.txt`, and `fault.txt` diagnostics.

## October 29, 2026: host replay replaces the VPK-per-syscall cycle

### Native IOP service milestone (later October 29)

The host replay now supports an explicitly enabled, stateful **native IOP
service replacement** (`--native-iop`). This is **not** IOP CPU emulation and
is not enabled in the currently published VitaTunnel VPK. It models the IOP
SIFMAN/SIFCMD receive buffer, validates EE SIF1 DMA command packets, completes
the SIFCMD and RPC initialization handshakes, manages transfer IDs, and
implements the first bounded CDVDFSV RPC server (bind plus `sceCdInit` RPC
function 0). Unsupported RPC servers and commands remain visible stops. The
EE `UNCACHED_SEG` and `UCAB_SEG` RAM aliases, critical to the SIFRPC packet
tables, are also mapped explicitly. These transitions have synthetic unit
tests as well as a replay with the private title ELF.

That replay now proceeds through **five** actual EE-to-IOP command packets:

1. `SIF_CMD_INIT_CMD`, SIFCMD handshake.
2. `SIF_CMD_INIT_CMD`, RPC handshake, including SREG RPCINIT notification.
3. RPC bind for `0x80000592`, CDVD initialization server.
4. RPC call 0 to that server, returning the driver-initialized result (no
   game disc/filesystem is mounted, and no sector data is invented).
5. `SIF_CMD_RESET_CMD`, an IOP reboot request.

The new **real dependency** is decoded from the fifth packet:

```
reason=syscall
pc=004958E4
cycles=6858760
iop_hle_phase=4
iop_cmd_packets=5
cdvd_init_calls=1
iop_reboots_requested=1
iop_reboot_mode=0
iop_reboot_arg=rom0:UDNL host:NFS\IOPRP280.IMG
```

`host:NFS\IOPRP280.IMG` is a game-specific IOP reboot/module image. It was
**not present in the supplied `ps2vita (9).zip`**, which only contains logs,
benchmark output and the existing external `boot.elf`. The HLE decoder
intentionally stops at that reboot instead of setting `BOOTEND` and pretending
the image was loaded. The next substantial piece is a native asset/module
service that can obtain the IOP image from separately supplied **game data**,
parse its contained modules and replace their required services. The existing
`ux0:data/ps2vita/boot.elf` stays external; it must not be repackaged.

Host validation with the same privately held ELF:

```sh
cmake -S . -B build-host -DCMAKE_BUILD_TYPE=Release
cmake --build build-host --target nfsmw_startup_replay \
  native_iop_services_tests bare_elf_startup_syscalls_tests -j4
build-host/native_iop_services_tests
build-host/bare_elf_startup_syscalls_tests
build-host/nfsmw_startup_replay /absolute/path/to/boot.elf 16000000 --native-iop
```

The replay's cycle count is **startup progress, not game performance**. The
host-only replay still uses Astra's diagnostic interpreter; full native AOT
translation and Vita rendering are not proven. **No new VitaTunnel release**
should be advertised until the reboot/module dependency has a real service
implementation and an on-hardware test is warranted.

### Earlier baseline

Vita r4 stopped at `0x00495904`, syscall `0x78` (`sceSifSetDChain`), after
6,855,355 guest cycles. Its preceding debug call printed `main() argc = 0`.
The new host-only `nfsmw_startup_replay` tool reproduces **exactly** that
cycle count, instruction address, syscall and debug string using the user's
private ELF without a Vita transfer or any private binary in this repository.
This is a debugging interpreter on the development computer, **not** a
proposed PS2 emulator to install on Vita.

The EE-side SIF0 CHCR initialization (`0x184`) and the real/software SIF
register accesses (`sceSifGetReg`/`sceSifSetReg`) now have narrowly scoped
implementations with host regression tests. With these calls handled, the
same guest reaches `0x00496D28`–`0x00496D38`: a loop reading SIF register 4
(`SMFLAG`) until bit `0x00020000` (`SIF_STAT_CMDINIT`) becomes set.
The replay detects this loop at approximately 7.2 million cycles with
`sif_smflag=0` and **`iop_cycles=0`**.

This is a real architectural dependency: the bare-ELF path currently does
not boot or execute the IOP firmware/modules, so no IOP-side SIFCMD handshake
can occur. Simply fabricating the ready bit would conceal the problem and
lead to more arbitrary startup failures. There is therefore **no new Vita
test VPK** for these host-only changes. A genuine IOP service replacement
(SIFCMD/RPC and required module/asset services), or an explicitly supported
IOP execution environment, must be designed and validated first.

To reproduce the stall on a host C++17 toolchain, with your own private ELF:

```sh
cmake -S . -B build-host -DCMAKE_BUILD_TYPE=Release
cmake --build build-host --target nfsmw_startup_replay -j4
build-host/nfsmw_startup_replay /absolute/path/to/boot.elf 15000000
```

The expected final diagnostic is `reason=blocked-sifcmd-no-iop`, not
successful game initialization. The game ELF and any generated AOT are never
placed in source control by this workflow.

## Proven on the supplied USA PS2 ELF

Original filename: `SLUS_213.51` (not included in source distributions).

- MIPS R5900 ELF32, one load segment, entry `0x00436128`.
- AstraRT AOT now supports `PADDUW` (unsigned saturated addition of each
  32-bit lane of a 128-bit general-purpose register), `MTHI`, `MTLO`,
  `MTHI1`, `MTLO1`, the R5900-only `MTSAH`, COP1 `MTC1`, `CTC1`,
  `ADDA.S` and `SYNC`.
- Generated host C++ translates the first **76** NFSMW startup instructions,
  up to PC `0x0043626c`, including its first completed branch.
- With a maximum of eight guest instructions per AOT entry, the package
  resumes across Astra's eight-cycle IOP scheduling quantum. Branches and
  their delay slots remain inseparable during code generation.
- Host differential test matches the interpreter's GPR low/high halves,
  HI/LO accumulator pairs, FPR/FCR arrays, FPU accumulator, SA register,
  PC, cycles and COP0 Count
  after these instructions.
- Crucially, a second integration test enables the AOT package through the
  real `Emulator::run_slice()` path. It successfully runs the 76 instructions
  in scheduler-sized pieces with the same guest state as the interpreter.
- The optional generated title package verifies the original guest instruction
  words at each native entry, falling back to the interpreter if the ELF
  differs. A host test deliberately modifies one instruction and confirms
  that no native instructions execute.
- 25 AstraIR unit tests passed at the original checkpoint. Later VitaTunnel
  hardware testing and the host startup replay are described above.

These results **do not** establish that the game can boot, load assets, or render.

## Reproduce with your own game

From the repository root (CMake and a C++17 compiler required):

```sh
cmake -S . -B build-nfsmw -DCMAKE_BUILD_TYPE=Release \
  -DASTRART_GAME_ELF=/absolute/path/to/SLUS_213.51
cmake --build build-nfsmw --target nfsmw_entry_probe -j4
ctest --test-dir build-nfsmw -R '^nfsmw_entry_probe$' --output-on-failure
```

Or generate inspectable C++ without a host build:

```sh
python3 tools/generate_astrart.py \
  --output /tmp/nfsmw_startup_aot.cpp \
  --package-name private-nfsmw-entry \
  --guard-guest-code --max-block-instructions 8 \
  /absolute/path/to/SLUS_213.51
```

Keep `SLUS_213.51` and any generated source translated from it private.
Neither is included in the release package.

## Public GitHub Actions build

The [Vita VPK workflow](https://github.com/ChatProductions/AstraRecompish/actions/workflows/vita-vpk.yml)
builds the general AstraRecomp frontend and uploads the resulting VPK as a
downloadable Actions artifact. It runs on pushes to `main` and this research
branch, or manually via **Run workflow**. It does **not** include the private
NFSMW executable or any game-specific AOT instructions, and therefore does
not constitute an NFSMW game build.

## Opt-in Vita experiment (maintainer build environment)

A local Vita build can include the private title package without committing
the proprietary ELF or the generated C++ to this repository:

```sh
cmake -S . -B build-vita-nfsmw -DCMAKE_TOOLCHAIN_FILE="$VITASDK/share/vita.toolchain.cmake" \
  -DASTRART_PRIVATE_VITA_ELF=/absolute/path/to/SLUS_213.51
cmake --build build-vita-nfsmw -j4
```

Copy the **same** personally obtained ELF to
`ux0:data/ps2vita/boot.elf` before trying the custom build. In the Vita
frontend, use the existing **TRIANGLE** external-ELF load action. The
opt-in package is armed after that load; firmware/BIOS boot and ordinary
builds remain interpreter-only. If the ELF's loaded code differs, individual
AOT entries reject it and fall back to interpretation.

This is a development instrumentation path, **not a playable Vita port**.
The user does not need VitaSDK to install a test VPK through VitaTunnel.

## Work required to turn this into a game

1. Continue through the next branches and indirect call targets of the real
   startup control flow, with differential tests at every stage.
2. Add a title-specific ELF + required disc-file loader in AstraRT. Compare
   the same input against PCSX2 before writing game-specific HLE shortcuts.
3. Measure the new opt-in execution mode on real Vita or Vita3K, and fix any
   device-specific timing or recompiler semantics before expanding coverage.
4. Investigate required IOP, VIF/VU1, GIF/GS, SPU2, CD/DVD and scheduling
   services, as well as Vita memory pressure. These are not solved by AOT.
5. Only once ELF startup and game services work, attempt frame output and
   performance analysis on Vita hardware.

AstraRecomp is MIT licensed. No game data, PS2 BIOS or proprietary material
from its executable belongs in a public source repository.
