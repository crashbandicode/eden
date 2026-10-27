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
| 19 | (title 4) `nn::syncpt::sdk::InitializeEx` | gsv 202 must return the syncpt session; gsv 204 takes two transfer memories and must leave a header (version 1, count, offsets) in the read-only one and return two events; syncpt 1 returns a u32 < 0x400 | `INx2SyncptSession`, gsv 202/204 |
| 20 | (title 4) `nn::gpu::sdk::BootWithRemoteMemory` | gsv 497 returns the GPU session; gpu 11 (id range), 12(id) event, 21(id) start + wait on that event, 22 another session | `INx2GpuSession` |
| 21 | (title 4) `nn::vi2::sdk::Initialize` / `nn::vi::CreateLayer` | gsv 305 returns the vi2 session; vi2 1/3/5 return small u32s (5 = timestamp recorder count, 7 needed by LayerHistory), 11/31/32 return handles | `INx2Vi2Session` |
| 22 | all TIPC replies with data | Eden bug: `ResponseBuilder::Push(Result)` writes the 2-word CMIF result, so TIPC data lands one word late and past the copied size | `TipcReplyWords` |
| 23 | (title 4) NVN -> NvRm device open | nvsrv is the nvdrv INvDrvServices protocol; serve it with Eden's nvdrv, initialised without `Initialize` | nvdrv-backed `nx2:nvsrv` |
| 24 | NvRm chip check | Switch 2 NvRm asks for a 0xD0-byte characteristics block and accepts chips 0x17E/0x12B/0x12E, but NVN needs the Ampere one; s16 interface versions at 0x70..0x80 must be >= 6 etc. | report chip 0x17E, Ampere classes, versions 0x40; unknown ctrl-gpu ioctls zero-stubbed for such callers |
| 25 | NvRm address space | AllocAsEx(flags 5, no big pages, [0x8000000, 0xC000000000), split 0); NVN then reserves 4 KiB VA up to 512 GiB; Eden's split/limit handling made the reported regions unusable | small pages up to the end, absolute big-page limit |
| 26 | (title 4) NvRm channel setup | nvhost-gpu ioctls 0x1F/0x20, `/dev/nvhost-tsg-gpu`, GetEngineInfo, object class 0 | logged echo for unknown ioctls once an nx2 driver is seen; GR + async-copy engines; Ampere/class-0 objects on Maxwell slots (70f1651, 82dbd88) |
| 27 | GPU memory | svc 0x80 (make memory GPU-accessible), gpu sub-session 101 (register range) and 211 (job) | stubs (608f0d6) |
| 28 | `syncpt::sdk::InitializeEx` -> `InterProcessEvent::Signal` abort E401 | gsv 204's first handle is attached *writable* (client -> server request), the second readable; the shared page has u64 values (RO+0xc), armed bits (RW+0x18, RO+0x1c) and summary words | gsv 204 returns a writable event, lays out the page, a 500 us poller mirrors host1x values and acknowledges armed waits (88367ea) |
| 29 | `NvOsEventPoolWait` E401 | syncpt 22(slot) must return an event; 201(slot, ~syncpoint, value, mode) / 202(slot) request/cancel a signal | slot events via host1x guest actions (88367ea) |
| 30 | GPU never executed submissions | Switch 2 never maps through nvhost-as-gpu: memory registered with gpu sub 101 is used at GPU VA == CPU VA | `Nx2GpuMemoryRegistry` identity-maps every range into each GMMU; SMMU physical range 33 -> 34 bits for 10/12 GB layouts (88367ea) |
| 31 | Puller | binds Ampere classes C797/C7C0/C7B5 and class 0; NVN2 uploads Turing+ (MME64) macros, which Eden's Fermi macro JIT runs as garbage until it crashes | Ampere classes run on the Maxwell engines; macros skipped while `Tegra::g_nx2_ampere` (88367ea) |
| 32 | NVN device init waits on syncpoint 1 >= 1 | submits use `increment_value` (bit 8) and signal completion only with semaphore releases | Eden appends the syncpoint increments for such submits (88367ea) |
| 33 | `wsi::ounce::NativeWindow::Acquire` on a null window | gsv 1510 is `gsv::shim::CalculateRequirements`; size 0 made window init fail | block-linear size/alignment, 4 bytes/pixel (5fb37b5) |
| 34 | `NvOsEventPoolWait` E401 (second channel) | nvhost-gpu QueryEvent 7 | per-channel event (5fb37b5) |
| 35 | CPU spins in NVN for `[0x2106028010] >= 3` | queues release progress to page P+0x1000 and wait (GPU acquire + CPU poll) on the same offset in page P | puller mirrors releases into the first two-page gpu region (5fb37b5; guess, works) |
| 36 | font header check on a wild pointer | `pl:u` 2010 `GetBuiltInFontData` (bool at offset 0, then {flags, u32 offset, u32 size}) and 2000 (font shared memory + size) | plain TTF copies of Eden's shared fonts after the BFTTF data (5fb37b5) |
| 37 | null "SoundResource" heap (300 MB) | `nninitStartup` sizes the heap as total - used - 0x195000000; Eden capped total at the 8 GiB heap region, leaving 1.55 GiB | Switch 2 processes count the alias region in max memory (5fb37b5) |
| 38 | game waits for service `aud:u` | Switch 2 audio entry (`nn::audio::CreateUserClientSessionByHipc`); cmd 2 returns an IAudioRendererManager | `INx2AudioUserClient` backed by Eden's audren (2e6c39c) |
| 39 | every audio `RequestUpdate` fails | updates tagged `REv?` (lowercase v), renderer opened with `REV?` | compare revision numbers (2e6c39c) |
| 40 | main thread parked in `ExecuteVirtualMachineMapBufferEx` | gpu sub 211's job slot is request word 17, not word 2 | signal word 17's event (2e6c39c) |
| 41 | "too many files open" reading RomFS | eden-cli never raised the CRT stdio limit | `_setmaxstdio(8192)` (2e6c39c) |
| 42 | GPU memory model | gpu sub 101 = `CreateGpuAccessibleMemory` (reply word 3 = GpuMemoryHandle); 211 = `NormalMapGpuMemory4K` (words 3-4 first VA, 5-6 last, 7 handle, 8-9 offset, 17 slot), also into sparse regions at 0x81_xxxx_xxxx; 102/213 (`NormalUnmapGpuMemory4K`)/214 (`CreateSparseRegion`) complete through slot events (words 3/8/11) | real handles, `Nx2GpuMemoryRegistry::MapAt` with SMMU range reuse, slot signals |

Run settings used (in `build/bin/user/config/sdl2-config.ini`): `memory_layout_mode=4`
(12 GB). Debug knobs: `EDEN_NX2_AUTO_STUB`, `EDEN_NX2_IPC_TRACE`, `EDEN_NX2_SVC_TRACE`,
`EDEN_NX2_PULLER_TRACE`, `EDEN_NX2_METHOD_TRACE`, `EDEN_NX2_HIDE_WINDOW`.

## Where it stops now

All runs: `EDEN_NX2_AUTO_STUB=1`, 12 GB memory layout, loose ExeFS with the sibling `romfs/`.

**Title 4** (`fourth switch game\exefs`, 0400380020056000) **runs its main loop**: it stays up for
the whole 90 s test, polls input (`hid` 214, auto-stubbed) every 16 ms and drives the audio
renderer every frame. Boot order now: nn::init -> nn::oe -> fs/RomFS -> graphics system (gsv,
nvsrv, syncpt, gpu, vi2) -> NVN device, two GPU channels, window with 4K swapchain images ->
fonts (its own `.bfotf` plus system fonts via pl:u 2010) -> heaps (incl. 300 MB sound) -> aud:u
audio renderer -> Gyml sequence files -> main loop.
- GPU submissions are executed by Eden's puller/engines (Ampere classes on Maxwell engines),
  syncpoints and queue semaphores advance, but Turing+ macros are skipped and nothing is presented.
- Only six GPU submissions happen (all during init). At ~11.5-12.4 s the main thread hands NVN
  a command buffer (from a per-context table at game `main+0x320149c`, used at `main+0x32014e8`)
  that was initialised but never given command memory or a memory callback: the whole state at
  cmdbuf+0x2be8.. is zero, so NVN writes its first methods to address 0 and then calls a null
  function. Some earlier game-side setup of that command buffer (its command-memory pool or the
  frame-begin that adds memory) silently failed or never ran; that is the next thing to find.
  Another thread keeps polling input afterwards.

**Title 3**: last checked before the graphics work; should now track title 4.

**Title 1** (`first switch game\exefs`, 0400C3F00006E000, `Drive.nss`): stops earlier, inside
`nnMain` (`nnMain+0x40 .. +0xb74`): it builds an SDK allocator object at address 0 and calls
through its null function pointer (`main+0xF04E8`). Worth retrying now that Switch 2 processes
get the larger memory budget (it sets a fixed 0x1BD600000 budget at `main+0xCF25A8`).

**Title 2**: main/sdk/subsdk0 fail LZ4 and SHA-256 checks; can't be run from these files.

## What a picture needs

1. **Present path**: gsv 315/423/429/435/111 (window buffer create/register) and vi2 present must
   hand the swapchain image to Eden's display, so whatever the GPU wrote appears.
2. **Turing+ macro engine (MME64)**: NVN2 uses macros for much of its 3D state; Eden only has the
   Fermi-era macro ISA. Mesa's `nouveau/mme` (tu104) documents the encoding.
3. **Ampere shader frontend**: NVN2 ships Ampere SASS (128-bit Volta+ encoding); Eden's shader
   recompiler only decodes Maxwell. This is the largest piece.
4. **Ampere 3D-class method differences** versus Maxwell B197, texture/RT formats from gsv image
   preferences.

## Prioritized next steps

0. **Empty command buffer** at ~12 s (title 4): find where the game normally adds command memory
   to the per-context command buffers (`main+0x320149c` table) and which earlier call failed.
   Break on NVN's command-buffer init and on the game's frame-begin, compare with the stores to
   cmdbuf+0x2be8 (53 sites in sdk).
1. **Present**: decode gsv 315/423/429/435/111 and vi2 present/acquire, and blit the presented
   image through Eden's renderer. First visible milestone: the game's clear colour.
2. **MME64 interpreter** behind `g_nx2_ampere` (replace the skip in `Maxwell3D::CallMacroMethod`).
3. **Ampere SASS decoder** for the shader recompiler.
4. **Title 1 / title 3** retests with the current tree.
5. Confirm the guesses: syncpt page layout, semaphore mirror page, gsv 1510 pixel formats,
   aud:u command numbers, pl:u built-in font type numbering, svcGetInfo 29-33.
6. **Six cores** instead of folding 4/5 onto 0/1; rare SIMD encodings still undecoded.
7. Upstream-worthy pieces independent of Switch 2: MSVC 14.51 build fixes, eden-cli SDL3
   lifetime fixes and `_setmaxstdio`, ExeFS loader fixes, FAC/FAH offset fix, GetPseudoDeviceId
   without NACP.
