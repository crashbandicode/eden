# Eden on Switch 2 titles (branch `nx2`)

Local experiment: get Eden (Switch 1 emulator) to load a Switch 2 title's loose ExeFS and push
boot as far as it goes. Nothing here is upstreamed or pushed. Graphics (Ampere / NVN2) is out of
scope for this pass.

## Setup

- Source + build live in `C:\dev\eden` (a worktree of `eden-nx2/`, branch `nx2`). The original
  working folder path is ~180 chars, and `cl.exe` can't open its PDB from a build folder there
  (C1041), so the build moved to a short path.
- Toolchain: VS 2026 (MSVC 14.51) via `vcvars64.bat`, Ninja from VS, CMake 4.4.
- Configure:
  `cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_INTERMEDIATE_DIR_STRATEGY=SHORT -DENABLE_QT=OFF -DYUZU_CMD=ON -DENABLE_WEB_SERVICE=OFF -DYUZU_ROOM=OFF -DYUZU_TESTS=OFF -DENABLE_WERROR=OFF -DENABLE_UPDATE_CHECKER=OFF`
- Build: `cmake --build build --target yuzu-cmd` -> `build/bin/eden-cli.exe`.
- Run (portable mode, `build/bin/user/` holds config/logs; no keys or firmware installed):
  `eden-cli.exe -g "<title>\exefs\main"`
- Debug helpers:
  - `EDEN_NX2_SVC_TRACE=1` logs every SVC with arguments and results.
  - `EDEN_NX2_IPC_TRACE=1` logs every dispatched CMIF command and non-zero result words.
  - `EDEN_NX2_AUTO_STUB=1` answers every unknown command with an empty success (the
    `use_auto_stub` setting gets reset by eden-cli's config rewrite). Used for all runs below.
  - `eden-cli --filter "<log filter>"` sets the log filter for one run (e.g. `*:Info Loader:Debug`
    to get module load addresses).
  - `tools/nx2/nso_syms.py <nso> addr|got|find ...` maps module offsets to exported symbols, GOT
    slots to imports, or searches symbol names. Eden's backtrace shows "unknown" modules for some
    titles; with the `loaded module X @ addr` lines this turns frames into SDK names.
  - `tools/nx2/a64_coverage.py <a64.inc> <nso>...` lists encodings in an NSO's .text that
    dynarmic's decoder table doesn't match.
  - `tools/nx2/npdm_info.py <main.npdm>` dumps the META/ACI0 header, services and kernel caps.
  - `tools/nx2/nso_dis.py <llvm bin> <nso> <text_off> <count> <workdir>` disassembles part of an
    NSO's .text (clang + llvm-objdump from VS 2022's LLVM).
  - Under cdb, stop only on second-chance AVs (`sxd -c2 "..." av`): dynarmic fastmem uses
    first-chance access violations.

## Titles

All titles share the same `main.npdm` layout: 64-bit, 39-bit address space, ThreadInfo cores
0..5 (core mask 0x3f), priorities 28..59, KernelVersion cap 9.1, SVCs 0x01-0x36 plus 0x80,
Switch 2 ACID layout.

All four are **Switch 2 builds**: none is a plain Switch 1 sanity check. Instruction counts that
stock dynarmic did not decode (`a64_coverage.py` against the unpatched `a64.inc`):

| Title (ExeFS) | Program id | RETAA/B | LDAPR | LSE atomics | CAS | SWP | other |
|---|---|---|---|---|---|---|---|
| first `exefs` | 0400C3F00006E000 | 114,767 | 37,560 | 24,718 | 6,640 | 2,470 | CASP 6, XPAC 7, PAC 2, STG 1 |
| second `exefs` and `updates/exefs` | 0400A3A01EE4C000 | 12 (rtld only) | - | - | - | - | main/sdk/subsdk0 don't decompress |
| third `exefs` (update `updates/65536` ~same) | 04005B902517C000 | 134,287 | 37,052 | 160,741 | 19,987 | 1,623 | XPAC 3, PAC 2, STG 1 |
| fourth `exefs` (update `updates/65536` ~same) | 0400380020056000 | 134,911 | 100,520 | 32,110 | 8,667 | 3,733 | PACGA 58, PAC 7, CASP 6, XPAC 3, STG 1 |

With the patched decoder, games 1/3/4 only leave ~60-110 rare SIMD encodings in `sdk` undecoded
(e.g. `5f119f9c`, `4ea05c42`, `0f413044`, FP16/v8.2 forms).

## Blockers hit, in order

| # | Where it stopped | Cause | Fix (commit) |
|---|---|---|---|
| 1 | CMake try-compile | C1041, PDB path ~290 chars | build in `C:\dev\eden` |
| 2 | Compile | MSVC 14.51: `reinterpret_cast<u64>(ULONG_PTR)`, C4505 on amalgamated zstd.c, C4800 int->bool | Fix build with MSVC 14.51 |
| 3 | Decoder | dynarmic had no LSE atomics, CAS/CASP, LDAPR, RETAA/RETAB, BRA*/BLRA*, LDRA*, PAC*/AUT*/XPAC*, PACGA, MTE ops. Title 1 has ~115k RETAB, ~37k LDAPR, ~25k LSE, ~6.6k CAS, ~2.5k SWP | dynarmic: decode ARMv8.1-8.5 instructions (atomics are exclusive load/op/store with retry; PAC is identity; MTE tags are 0) |
| 4 | Loader | `main` identified as a bare NSO, so main.npdm/rtld/sdk were skipped, then a crash writing into an uninitialised process | loader: load a loose ExeFS when given its main |
| 5 | NPDM parse (ErrorBadFileAccessControl) | Switch 2 ACID has a 0x300-byte signature/key area (Switch 1: 0x200); FAC/FAH offsets were also read without the block base | npdm: parse the Switch 2 ACID layout |
| 6 | eden-cli crash on start/exit | `*appstate` never set, SDL3 callbacks returned SUCCESS (quit), InputSubsystem was a local of SDL_AppInit, mouse timer before mouse driver | eden-cli: fix SDL3 main-callback lifetime bugs |
| 7 | `nnosInitialize` abort | `svcGetInfo` 29/30 unknown (region between alias and ASLR) | kernel: stub ids 29/30 (empty region after alias) |
| 8 | `VammManager::InitializeIfEnabled` abort | `svcGetInfo` 33 unknown; it's a page granule (module ranges are checked against `value-1`; VAMM maps one granule first) | return 0x1000 |
| 9 | `nn::am::InitializeApplicationInterfaces` | `appletOE` cmd 10 (Switch 2 OpenApplicationProxy) | alias to cmd 0 |
| 10 | same | `IApplicationProxy` cmds 60 and 40: new getters | return a generic `nx2:` stub interface (`nx2_stub_interface.h`); calls on it are auto-stubbed and logged |
| 11 | `nn::oe::SetupGpuErrorHandler` -> CreateThread | thread on virtual core 5; Eden emulates 4 cores | fold virtual cores 4/5 onto 0/1 in CreateThread/SetThreadCoreMask |
| 12 | (title 3) `nn::oe` init | `IApplicationFunctions` 320 has no handler | empty-success stub |
| 13 | (titles 3/4) `nn::fs::QueryMountRomCacheSize` -> `GetFileSystemProxyServiceObject` abort 2002-1002 | fsp-srv `SetCurrentProcess` returned ResultTargetNotFound: the loose-ExeFS loader never registered the process with the fs controller, and never set its RomFS | loader: give loose ExeFS boots a RomFS (sibling `romfs/`) and an fs registration |
| 14 | (title 4) `IApplicationFunctions` 321 | unknown | empty-success stub |
| 15 | (title 4) `nn::oe::GetCurrentGraphicsScale` UnexpectedDefault | `IApplicationProxy` 60 is a graphics-scale interface; its cmd 10 must return 10/20/30 | `IGraphicsScaleController` returning 10 |
| 16 | (title 4) `nn::gs::sdk::BootGraphicsSystem` -> `nn::os::GetRemoteMemoryUsage` abort | `svcGetInfo` 31/32 (remote memory for gsv/nvsrv) | return 0 for both (logged) |
| 17 | (title 4) `nn::gsv::BootWithRemoteMemory` abort | it sends TIPC on a session handle from `ISelfController` 2000, which had no handler | `ISelfController` 2000 returns a raw session to `INx2GraphicsSystemSession` (`nx2_gsv.h`), which accepts every command |
| 18 | (title 3) `nn::oe::GetPseudoDeviceId` abort | needs control.nacp, which loose ExeFS lacks | seed from program id |

Also changed: `svcGetInfo` 29/30 now report the upper half of the alias region instead of an
empty region (guess: VAMM address space). It made no visible difference for title 1.

Run settings used (in `build/bin/user/config/sdl2-config.ini`): `memory_layout_mode=4`
(12 GB). With the default 4 GB layout title 1 picks a 1.6 GB heap; with 12 GB it picks 6.5 GB.
Both stop at the same point. No GPU/nvdrv call is reached yet in any title.

## Where it stops now

All runs: `EDEN_NX2_AUTO_STUB=1`, 12 GB memory layout, loose ExeFS with the sibling `romfs/`.

**Title 4** (`fourth switch game\exefs`, 0400380020056000) gets furthest. Path:
rtld -> nn::init -> nnosInitialize -> VAMM -> nn::oe (applet proxy, window, focus, graphics
scale) -> fsp-srv + RomFS reads -> `nnMain` -> `nn::gs::sdk::BootGraphicsSystem`:
- `nn::os::GetRemoteMemoryUsage` (svcGetInfo 31/32) -> stubbed 0/0.
- `nn::gsv::BootWithRemoteMemory`: gets a session from `ISelfController` 2000 and sends TIPC
  4 (region, process handle, pid), 11, 101, 1501, 1502. Placeholder `nx2:gsv` accepts all.
- `nn::nvsrv::BootWithRemoteMemory`: `gsv` 1402 returns the nvsrv session (handle at word 3,
  result at word 4); nvsrv command 13 follows. Placeholder `nx2:nvsrv` accepts all.
- `nn::syncpt::sdk::InitializeEx`: `gsv` 201, then 202 returns the syncpt session (same reply
  layout). Placeholder `nx2:syncpt`. See the end of this section for the latest stop.

**Title 3** (`third switch game\exefs`, 04005B902517C000): same path, plus `GetPseudoDeviceId`
(fixed), and reaches `nn::nvsrv::BootWithRemoteMemory` like title 4.

**Title 1** (`first switch game\exefs`, 0400C3F00006E000, `Drive.nss`): stops earlier, inside
`nnMain` (`nnMain+0x40 .. +0xb74`): it builds an SDK allocator object at address 0 (writes to
0x0..0xF8, sdk vtable) and calls through its null function pointer (`main+0xF04E8`). The chain at
`main+0xCF25A8` sets a fixed 0x1BD600000 (7.46 GB) budget. No failing SVC or IPC precedes it, so
some Switch 2 memory API returns a null base without an error.

**Title 2** (`second switch game`, base and `updates`): main/sdk/subsdk0 segments fail LZ4 and
SHA-256 checks; can't be run from these files.

## What a picture needs

Everything from `gsv` on is placeholder IPC: commands succeed with empty replies. The graphics
stack is now being booted against servers that do nothing. A title screen needs:
1. A real `gsv`/`nvsrv`/`syncpt` model: what each TIPC/CMIF command takes and returns (handles,
   shared memory, events, syncpoint ids). This replaces Switch 1's `nvdrv` ioctl interface.
2. NVN2 command buffers decoded and executed: Ampere-class 3D/compute/copy engines, which Eden's
   `video_core` (Maxwell/Pascal) doesn't implement, plus a display/present path (vi/hos-binder or
   its Switch 2 replacement).
3. Shader translation for the Ampere SASS the game ships.
That's the bulk of a Switch 2 emulator and well beyond boot fixes.

## Prioritized next steps

1. **Keep walking title 4's graphics boot** with the IPC trace: each `nx2:` stub call in the log
   is a command whose reply layout is needed. Use `nso_syms.py` + `nso_dis.py` on the SDK caller
   to read the expected reply words (as done for 1402/202).
2. **Title 1 null allocator**: break at `main+0xCF25A8` and find which SDK memory call returns a
   null base; likely tied to the guessed svcGetInfo 29/30/33 meanings.
3. **Pin down svcGetInfo 29/30/31/32/33** from the SDK code that consumes them.
4. **New app-layer commands**: `IApplicationProxy` 40, `ISelfController` 2400, `IGraphicsScale`
   1050, `IApplicationFunctions` 320/321 are auto-stubbed; check their real outputs.
5. **Six cores** instead of folding 4/5 onto 0/1.
6. **Rare SIMD encodings** still undecoded (~60-110 per title in sdk).
7. Upstream-worthy pieces independent of Switch 2: MSVC 14.51 build fixes, eden-cli SDL3
   lifetime fixes, ExeFS loader fixes (directory detection, RomFS, fs registration), FAC/FAH
   offset fix, GetPseudoDeviceId without NACP.
