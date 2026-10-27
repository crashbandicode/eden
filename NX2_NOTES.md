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
  - `tools/nx2/a64_coverage.py <a64.inc> <nso>...` lists encodings in an NSO's .text that
    dynarmic's decoder table doesn't match.
  - `tools/nx2/npdm_info.py <main.npdm>` dumps the META/ACI0 header, services and kernel caps.
  - `tools/nx2/nso_dis.py <llvm bin> <nso> <text_off> <count> <workdir>` disassembles part of an
    NSO's .text (clang + llvm-objdump from VS 2022's LLVM).
  - Under cdb, stop only on second-chance AVs (`sxd -c2 "..." av`): dynarmic fastmem uses
    first-chance access violations.

## Titles

All three titles with a `main.npdm` share the same layout: 64-bit, 39-bit address space,
ThreadInfo cores 0..5 (core mask 0x3f), priorities 28..59, KernelVersion cap 9.1, SVCs 0x01-0x36
plus 0x80, Switch 2 ACID layout. The fourth title's ExeFS has no `main.npdm`.

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

Also changed: `svcGetInfo` 29/30 now report the upper half of the alias region instead of an
empty region (guess: VAMM address space). It made no visible difference for title 1.

Run settings used (in `build/bin/user/config/sdl2-config.ini`): `memory_layout_mode=4`
(12 GB). With the default 4 GB layout title 1 picks a 1.6 GB heap; with 12 GB it picks 6.5 GB.
Both stop at the same point. No GPU/nvdrv call is reached yet in any title.

## Where it stops now

**Title 1** (`first switch game`, program id 0400C3F00006E000, main module `Drive.nss`):
rtld -> nn::init -> nnosInitialize -> VAMM -> nn::oe (applet proxy, window, focus) all complete;
the game's own static initializers run. It then builds an SDK allocator object at address 0
(writes to 0x0..0xF8; vtable 0x...14d8 inside sdk) and calls through its null function pointer
(`main+0xF04E8 blr x8`, PC=0). The chain starts at `main+0xCF25A8`, which fills a settings struct
with a fixed 0x1BD600000 (7.46 GB) budget and 0xB000000/0xC000000 sizes. Whatever should supply
the object's memory returns 0 without any failing SVC or IPC call in the trace. The next step is to
find that source, probably an SDK memory API whose Switch 2 behaviour differs (VAMM / address
region / `nn::mem`).

**Title 2** (`second switch game`): can't be tested with these files. Its rtld is intact, but the
`main`, `sdk` and `subsdk0` segments fail LZ4 decompression or their SHA-256 check (checked with
`a64_coverage.py`'s decoder and Eden's `nso.cpp:124`). The dump/extraction needs redoing.

**Title 3** (`third switch game`, 04005B902517C000): same path as title 1 through nn::oe, then
`IApplicationFunctions` 321 (unknown) and an abort with result 2002-1002 (fs, path not found) from
game code. Most likely the game's first file/RomFS mount; the title's `romfs/` folder isn't wired up
for loose-ExeFS boots yet.

**Title 4** (`fourth switch game/base/exefs`): ExeFS has `main` but no `main.npdm`; not run.

## Prioritized next steps

1. **Title 1 null allocator**: break at `main+0xCF25A8` (cdb or the SVC trace plus a dynarmic
   breakpoint) and find which SDK call should hand back the allocator's memory. Likely a
   VAMM/address-region or `nn::mem` API that relies on the ids 29/30/33 semantics guessed here.
2. **Pin down the new svcGetInfo ids (29, 30, 33)** from the SDK code that consumes them; they
   decide memory layout for every later step.
3. **RomFS/save for loose ExeFS** (title 3's 2002-1002): mount `../romfs` next to `exefs`, then
   trace the fs IPC that fails.
4. **New IPC commands**: `IApplicationProxy` 40/60 (now stub interfaces; log what is called on
   them), `IApplicationFunctions` 321, `appletOE` 10 args. Each needs its real reply layout.
5. **Six cores**: the folding hack works for boot but changes scheduling; a 6-core kernel/CPU
   manager would be more faithful.
6. **Rare SIMD encodings** still undecoded (~60 in title 1, e.g. `5f119f9c`, `4ea05c42`,
   `0f413044`): FP16 by-element / v8.2 SIMD forms. They'll trap when reached.
7. **GPU**: nvdrv/NVN2 (Ampere) is untouched; every title will stop at its first nvdrv ioctl once
   the steps above are done. Start by logging nvdrv ioctls and stubbing what blocks boot.
8. Upstream-worthy pieces independent of Switch 2: MSVC 14.51 build fixes, eden-cli SDL3
   lifetime fixes, the ExeFS loader fix, FAC/FAH offset fix.
