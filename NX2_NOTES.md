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
| 43 | (title 1) `nn::syncpt::sdk::Initialize` abort | newer SDK opens syncpt with TIPC 0(u64 token) and aborts unless it is echoed | handshake echo |
| 44 | (title 1) NVN closes the GPU device after GetVARegions | newer SDK asks gpu 201/202(id) for the user/gsv partitions (two u64s each, stored as min/max) instead of using 203 | 201 = [0x8000000, 0xC000000000), 202 empty |
| 45 | NVN command buffer with no command memory (~12 s, titles 1 and 4) | `NvRmGpuTSGSubcontextCreate` failed (0x11): characteristics reported one VEID | flags bit 8/22 (TSG, TSG_SUBCONTEXTS) and 64 subcontexts at characteristics +0xb4 (12125bc) |
| 46 | NVN asserts in `GetSyncpointShimRoAddress` | Switch 2 reads syncpoints through a read-only GPU page (flag bit 26, nvhost-as ioctl 0xB) | shim at the top of the VA range; GPU reads of it return the syncpoint value (12125bc, from changes.patch) |
| 47 | window/image creation | gsv 111/112/114/423/429/430/435/1510 native memory and images; svc 0x80 is SetGpuMemoryAttribute | native object registry + layout calc (from changes.patch), real svc 0x80 (12125bc) |
| 48 | presents go nowhere | vi2 229 (word 5 image id, ~word 22/word 24 fence) | composite the block-linear image with native GOB tiling (f63ff90, from changes.patch) |
| 49 | crash in `DmaPusher::CallMethod` (null engine) | NVN uses subchannel 2 (inline-to-memory) without binding a class | bind KeplerMemory there by default; drop methods on unbound subchannels (9f30683) |
| 50 | GPU parses garbage, then stalls (~14-19 s) | NVN2 gates each command buffer with a SEMAPHORED acquire the CPU releases after writing it; Eden read past unmet acquires and ran stale commands (`EDEN_NX2_PB_CHECK` confirmed entries changed after submit) | suspend the channel on an unmet acquire and resume it from the same word (9f30683, approach from changes.patch) |
| 51 | desync after a called command buffer | NVN2 ends a GPFIFO entry inside a method's data, calls another buffer, and continues at the next address | set the unfinished method aside and resume it when the stream continues (9f30683) |
| 52 | semaphore words next to releases get clobbered | 4-byte releases (SEMAPHORED bit 24) were written as 16 bytes | 4-byte releases write 4 bytes (9f30683) |
| 53 | no draws: 3D macros skipped | NVN2 drives 3D/compute state and draws through Turing MME64 macros; compute dispatches through QMD 3.0 | changes.patch's MME interpreter (one macro context shared by the process's channels, also on the compute class), native compute and QMD scheduler (d7826ce) |
| 54 | draws hit Eden's Maxwell decoder ("Invalid insn") | shaders are SM86 SASS | changes.patch's Ampere-to-Maxwell lowering, widened until all 32 of title 4's programs lower; `EDEN_NX2_SHADER_DUMP` + `C:\dev\nx2work\lowertest` test it offline (d7826ce) |
| 55 | GPU desyncs again after ~15-30 s | the game ran a full ring of command memory ahead of Eden's GPU thread: the vi2 229 reply reports all buffers free, so nothing paced it | vi2 229 waits (bounded) for the frame's GPU fence (325cac7) |
| 56 | macro arguments lost | NVN interleaves several command buffers, so more than one split method can be pending | several parked methods keyed by resume address, dropped at the end of the submission (325cac7) |
| 57 | frames all black although draws run | gpu 214 creates GPU-only memory (six 300 MB regions at 0x81_0000_0000) that the graphics server backs itself; the render targets live there and were unmapped | back each region with application-pool pages mapped into the alias code region (325cac7). Title 4 now shows its loading screen and a menu screen |
| 58 | (title 4) hang after confirming difficulty: macro 0x142 "parameter FIFO underflow" | NVN2's conditional-render macro reads two query counters with MME_DMA_READ_FIFOED (3D methods 0x154/0x155 address, 0x158 count) and loads them after a load barrier | MME DMA reads into the load FIFO / data RAM (8cbe854) |
| 59 | equal-value gate semaphore missed (~24 s) | the CPU side ran ~96 command buffers ahead when the 250 ms present wait timed out during shader compiles | present waits up to 4 s for the frame fence (8cbe854) |
| 60 | 3D queue waits forever on a word the compute queue should release | NVN2 patches a third QMD release slot at word 52 for queue-to-queue syncs | fire release slots 24, 28 and 52 (8cbe854) |
| 61 | 3D queue waits for N compute completions, 4 missing | compute jobs whose shader Eden rejected never completed, so their +1 releases never fired | rejected QMDs still complete and release (a84ec80); IMAD -UR addend, LDSM lowered |
| 62 | queue waits for ≥ a value that keeps moving backwards | QMD release slot 0 (flag bit 19) is a max release into a word a 3D macro also advances | bit-19 QMD releases write max(old, payload) (a84ec80) |
| 63 | in-game draws refused (~20k per run), black screen behind the dialogue | strong LDG (bit 77), 2D TLD4, RED MAX, AST of RZ unsupported | lowered (a84ec80); 210/214 post-menu shaders lower |
| 64 | intro scene black (DLSS-style pass) | title 4 renders at 2560x1440 and upscales to 3840x2160 with a tensor-core (HMMA/MOVM/LDSM) compute network; emulating it takes the host driver minutes to compile and would cost seconds per frame | bilinear blit from its input (cbuf0+0x90) to its output (cbuf0+0xa0) when the program is rejected (22333bd); full emulation opt-in (EDEN_NX2_TENSOR) |
| 65 | fullscreen blit passes read constant bank 24 | console-provided blit parameter block, never bound by the game | bank 24 -> 17, synthesized from the render area by BlitConstantsPass in vertex and fragment shaders (22333bd, d75e57d) |
| 66 | synthesized blit sizes were 0 | the game never programs surface_clip, which feeds Eden's render area | fall back to colour target 0's size (d75e57d) |
| 67 | tonemap output black | the 3D colour-grading LUT is drawn once per scene by a pixel shader using CALL.REL.NOINC; capture treated it as a real call, never found the end and rejected the shader | NOINC calls count as forward branches in capture (4d58a74). **Intro cutscene renders correctly** |

Run settings used (in `build/bin/user/config/sdl2-config.ini`): `memory_layout_mode=4`
(12 GB). Debug knobs: `EDEN_NX2_AUTO_STUB`, `EDEN_NX2_IPC_TRACE`, `EDEN_NX2_SVC_TRACE`,
`EDEN_NX2_PULLER_TRACE`, `EDEN_NX2_METHOD_TRACE`, `EDEN_NX2_HIDE_WINDOW`,
`EDEN_NX2_FRAME_CAPTURE=<dir>` (every 120th presented frame as TGA), `EDEN_NX2_PB_CHECK`
(pushbuffer entries changed between submit and execution), `EDEN_NX2_QUERY_TRACE` (every GPU
report/semaphore write). `C:\dev\crashrun.sh` runs eden-cli under cdb and prints the crash stack;
`C:\dev\stackdump.sh <main> <seconds>` dumps host thread stacks of a running test.

## Where it stops now

All runs: `EDEN_NX2_AUTO_STUB=1`, 12 GB memory layout, loose ExeFS with the sibling `romfs/`.

**Title 4** (`fourth switch game\exefs`, 0400380020056000) **plays its intro cutscene correctly
rendered**: boot, loading screen, menu (difficulty select, keyboard input), confirm, then the intro
(plane cockpit, newspaper/map) lit, colour-graded and subtitled, matching console footage. The
DLSS-style upscale is a bilinear stand-in. In a played session the game went on to a later loading
screen that did not finish; open there: a compute shader with a 64-bit add (UIADD3 carry-out to
UP0), TMML (0xb69) in two graphics shaders, "reserved attribute 23" (a[0x5c]) reads. Scripted runs
(`C:\dev\run.sh`) move any title-4 save to `C:\dev\save_backups` first so they start a new game.
Scripted input: `EDEN_NX2_KEY_SCRIPT="50:4,50.5:-4,55:79,55.4:-79,57:4,57.5:-4"`.

**Title 3**: last checked before the graphics work; should now track title 4.

**Title 1** (`first switch game\exefs`, 0400C3F00006E000, `Drive.nss`): reached its main loop and
the empty-command-buffer stop that blocker 45 fixed for both titles; not retested since the
macro/shader/present work.

**Title 2**: main/sdk/subsdk0 fail LZ4 and SHA-256 checks; can't be run from these files.

## Prioritized next steps

1. **Title 4 past the intro**: lower the 64-bit add with carry (UIADD3/IADD3 carry-out and .X
   carry-in), TMML, and attribute 0x5c; then whatever the next area needs (dump + lowertest).
   Squid/paint-splat hatch on the menu. A faster tensor-core emulation (shared-memory HMMA) would
   replace the bilinear upscale stand-in.
2. **Title 1 / title 3** retests with the current tree; dump their shaders and widen the lowering.
3. Replace guesses with real protocol knowledge: syncpt page layout, the semaphore mirror page,
   gsv 214 (GPU-only memory) and vi2 present buffer availability (real buffer release instead of
   the fence wait), gsv 1510 formats, svcGetInfo 29-33.
4. **Six cores** instead of folding 4/5 onto 0/1; rare SIMD encodings still undecoded.
5. Upstream-worthy pieces independent of Switch 2: MSVC 14.51 build fixes, eden-cli SDL3
   lifetime fixes and `_setmaxstdio`, ExeFS loader fixes, FAC/FAH offset fix, GetPseudoDeviceId
   without NACP.
