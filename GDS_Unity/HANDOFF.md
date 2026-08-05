# HANDOFF — Game Dev Story on the R36S (KairoVM / native loader)

> Continuity doc for the next session/agent, written like a compacted chat
> summary: current state, how we got here, what is true, what is broken, and
> the exact next steps. Read top to bottom.

## TL;DR

Goal: **Game Dev Story 2.6.9 running natively on an R36S-class AArch64 Linux
handheld** (ArkOS/PortMaster). The APK ships `libil2cpp.so` (the game, Unity
IL2CPP) and `libunity.so` (Unity's engine). On the R36S those run natively — we
only have to provide the platform *underneath* them (bionic->glibc shims, JNI,
EGL/GL, input/audio), a small, known surface.

Two approaches were started:
1. `kairovm/` — a Unicorn-based emulator that boots `libil2cpp.so` on x86 (a
   dev/debug reference; slow, not the device target).
2. `loader/` — a freestanding AArch64 C loader that maps the real Android `.so`
   files and runs them (the actual R36S deliverable). **This is the one to
   pursue.**

The web target was explicitly **deprioritised**: a browser would need a WASM ARM
emulator (the slow, hard half) and does not reuse the native win.

## THE TWO REFERENCE PORTS — READ BEFORE ANY DEEP WORK

Both are by the SAME author (NextOs-Ports), run Unity IL2CPP games on THIS R36S,
and between them cover every hard problem in this project.  When stuck, mine
their source for the exact mechanism instead of reinventing it.

1. **`NextOs-Ports/terraria-nextos`** (Terraria 1.4.5.6.4, Unity 2021.3 IL2CPP)
   - `src/main.c` — the complete loader boot: so_load/relocate/resolve, import
     + GOT patching (`set_import`/`patch_got`), asset-redirect `my_open`, TLS
     guard pad, crash handler, the whole Unity player-loop drive.
   - `src/bionic_shims.c` — bionic/glibc ABI: FORTIFY `_chk`, sigset_t 8B->128B,
     `my_sigaction`, `__system_property_get`.
   - `src/pthread_fake.c` / `pthread_bridge.c` — the "slot" trick (ptr to a real
     glibc object in the bionic object's first word) for mutex/cond/rwlock/sem;
     `pthread_sigmask` bionic/glibc size fix.
   - `src/jni_shim.c`, `src/egl_shim.c` — JNI + SDL-backed EGL shims.

2. **`NextOs-Ports/horizonchase-nextos`** (Horizon Chase, Unity, newer/more
   battle-tested loader).  This one was the KEY to our current blocker:
   - `src/main.c` `TER_FUTEXPOLL` + `my_syscall` — intercepts raw
     `syscall(SYS_futex, FUTEX_WAIT)` (which BYPASSES pthread/sem shims) and
     injects a short poll timeout so lost-wakeup job-system workers wake and
     re-check.  Also intercepts `SYS_sched_getaffinity` to force 1 CPU (jobs
     inline), and `SYS_rt_sigprocmask` for the GC's stop-the-world.
   - `TER_JOBWORKERS0` — calls `JobsUtility.set_JobWorkerCount(0)` (and
     ActiveThreadCount/WarpThreadCount) via `il2cpp_runtime_invoke` so jobs run
     inline.  `TER_JOBINLINE` reports 1 CPU via sched_getaffinity + /proc/cpuinfo
     + /sys/cpu.
   - `TER_FAKEACK` / `TER_NOSUSPEND` — fake-ACKs the GC's thread-suspend signal
     and swallows SIGPWR/SIGXCPU so the stop-the-world GC doesn't deadlock on
     threads it can't reach.
   - the futex-logging (`TER_FUTEXLOG`) and thread-comm helpers are invaluable
     diagnostics.

General rule: **if the game futex-waits with a NULL timeout, it is a RAW
`syscall(SYS_futex)` that pthread/cond/sem shims can't reach — the fix is the
`kv_syscall`/`my_syscall` interception (0.26.0 implements this).**  If a thread
spins `state=R` on `read()`, it's waiting on a pipe/socket for an Android
lifecycle event the loader must pump.  These two ports already solved both.

## What is DONE / WORKING

- Cross-toolchain: `python3 -m ziglang cc -target aarch64-linux-gnu` builds
  freestanding aarch64 binaries in-sandbox (Debian 12; apt's gcc-aarch64 /
  qemu-user are blocked by the sandbox's Debian mirrors, but the `ziglang` pip
  package works).
- Unicorn test bench works (`tools/run_aarch64.py`): runs a freestanding aarch64
  ELF headless (no GPU/device) so the loader can be iterated here.
- Proven end-to-end: `loader/tiny` (freestanding aarch64 calling write/exit and
  an init_array ctor) compiles, loads under the bench, prints its message, exits
  0.
- **The loader now loads the REAL Game Dev Story engine + game together.**
  With the extracted APK at `out/apk/lib/arm64-v8a/`:
  `python3 tools/run_aarch64.py loader/loader2 $(pwd)/out/apk/lib/arm64-v8a/libil2cpp.so $(pwd)/out/apk/lib/arm64-v8a/libunity.so`
  loads BOTH, resolves ALL imports (0 unresolved), runs libil2cpp's 18 ctors
  AND libunity's 0x1aa (426) ctors, and exits 0:
  `[loader] libil2cpp.so init_array ran (18 ctors)`
  `[loader] libunity.so init_array ran (1aa ctors)`
  `[loader] OK: 2 module(s) loaded and initialised` / `[guest exit 0]`.
  Each .so alone also loads/init/exits 0.  A synthetic `loader/so_probe.c`
  exercises the reloc path in isolation (RELATIVE, self GLOB_DAT, JUMP_SLOT,
  DT_INIT_ARRAY) without the big assets.
- **`JNI_OnLoad` now runs to completion under a built-in JNI shim.**
  `loader/jni_shim.c` provides a full JNINativeInterface + JavaVMFunctionTable
  (JNIEnv + JavaVM).  The loader can drive the whole Android native boot chain:
  pass libil2cpp.so + libunity.so + libmain.so and it loads all three, runs all
  their init_arrays, and calls `libmain.so`'s `JNI_OnLoad` (the real entry the
  Android VM calls, which dlopen/dlsym's the other libs through our shim).  It
  returns `JNI_VERSION_1_6` (0x10006) and the guest exits 0.  FindClass inventory
  it issues (the classes a real Android runtime must supply):
  `com/unity3d/player/UnityPlayer`, `com/google/androidgamesdk/ChoreographerCallback`,
  `com/google/androidgamesdk/SwappyDisplayManager`, `com/unity3d/player/GoogleARCoreApi`,
  `com/unity3d/player/Camera2Wrapper`, `com/unity3d/player/HFPStatus`,
  `com/unity3d/player/AudioVolumeHandler`, `com/unity3d/player/UnityCoreAssetPacksStatusCallbacks`,
  `com/unity3d/player/OrientationLockListener`, `com/unity3d/player/SoftInputProvider`.
  Build with `loader/build.sh` (compiles jni_shim.c; `-Wno-*` relax pedantic
  function-pointer checks that don't matter for the ARM64 JNI ABI).
- The loader's ELF machinery is written (`loader/loader.c`): maps PT_LOAD
  segments, applies RELATIVE + GLOB_DAT/JUMP_SLOT/ABS relocs, runs DT_INIT_ARRAY,
  resolves the `.so`'s libc/libm/bionic imports via a host symbol table.  It
  carries its own minimal libc (`freestdlib.c`) and a `host_syms.c` symbol
  provider, so it needs no host libc at load time.
- The loader now takes the `.so` path from the command line (`_start` reads
  argc/argv from x0/x1 per the aarch64 Linux entry ABI) and exits 0 on
  stage-1 success instead of re-looping.
- The Unicorn-based Unity emulator (`kairovm/`) got GDS past its loading screen
  (virtual-clock thread wake + UI-queue pumping). Useful as a reference and as a
  JNI-call inventory, but slow and not the device path.

## What is BROKEN / NOT YET DONE

- Both `libil2cpp.so` and `libunity.so` **load and run their init_arrays** now
  (see DONE/WORKING).  What is NOT yet done: **driving the Unity player loop** —
  calling Unity's entry (`UnityPluginLoad` / `UnityMain` / JNI_OnLoad), resolving
  the JNI classes the game calls, and providing a real EGL/SDL window + input +
  audio.  This will surface bionic/glibc ABI gaps (notably the arm64
  `sigaction`/`sigset_t` size difference) and the JNI-call inventory.
- The real IL2CPP + Unity boot passed because `freestdlib.c` + `host_syms.c`
  carry a broad freestanding libc: allocator (incl. realloc/memalign), mem/str,
  printf, time/syscalls, pthread/mutex/cond/sem stubs, math stubs, locale/wchar
  stubs, bionic extras (`__errno`, `environ`, `__sF`, `__system_property_get`,
  `__cxa_atexit`/`__cxa_finalize`, `__stack_chk_fail`, `uname`,
  `__android_log_print`), plus the libunity surface: **NDK native** (`ALooper*`,
  `ANativeWindow*`, `ASensor*`), **EGL** (`egl*`), **zlib** (`inflate*`), and
  **POSIX sockets/net**.  These are the symbols `host_dlsym`'s table maps to;
  add any newly-unresolved symbol there (it prints `unresolved <name>`).
- The loader runs each .so's **init_array** and **libunity's `JNI_OnLoad`** (which
  now returns JNI_VERSION_1_6).  It does NOT yet run Unity's **main loop**:
  `UnityMain` is invoked from the Java Activity's native methods (not from
  JNI_OnLoad), and it needs a real EGL surface + AssetManager + the APK's
  assets.  Those need a display/GPU and are not fully exercisable headless under
  the Unicorn bench (no GPU); that is the R36S-device stage.
- **JNI shim is minimal-by-design.**  `jni_shim.c` implements GetVersion,
  FindClass, GetMethodID/GetStaticMethodID, GetFieldID/GetStaticFieldID,
  NewStringUTF/GetStringUTFChars, RegisterNatives, exception funcs, NewObject,
  Call*/Get* as no-op stubs returning 0.  Everything else points at a non-null
  stub so calls don't fault.  The engine's JavaVM expects **GetEnv at offset
  0x20** in the JavaVM function table (it does `vm->functions[0x20](vm,&env,0)`
  to obtain the JNIEnv) - the shim is laid out to match that.
- No GPU / EGL / SDL window yet.  Next milestones (stage 2+): run Unity's main
  loop + player loop, and provide EGL/SDL + JNI shims (model on
  `NextOs-Ports/terraria-nextos` src/bionic_shims.c, jni_shim.c, egl_shim.c).
- `kairovm/` is slow/incomplete as a game and is intentionally not the target.

## Known GOTCHAS (cost real time — read before debugging)

1. Unicorn `UC_HOOK_INTR` callback MUST take 3 args: `(uc, intno, user_data)`. A
   2-arg lambda makes the hook silently never fire and the guest spins forever
   with no syscalls. (This was the "printf hangs / no output" bug. Fixed in
   run_aarch64.py.)
2. Unicorn syscall numbers are in `x8`; read `x0..x5` for args. `UC_HOOK_CODE`
   callback is `(uc, addr, size, data)`; `UC_HOOK_MEM_INVALID` is
   `(uc, access, addr, size, value, user)`.
3. Python `struct` ELF Phdr parsing: do NOT use a packed format string for the
   56-byte Phdr — field alignment is unreliable. Read each field by explicit byte
   offset: `p_type@0(I)`, `p_flags@4(I)`, `p_offset@8(Q)`, `p_vaddr@16(Q)`,
   `p_filesz@32(Q)`, `p_memsz@40(Q)`.
4. Zig freestanding build needs `-fno-sanitize=undefined` with `-O0`, otherwise
   UBSan refs (`__ubsan_handle_shift_out_of_bounds`) are emitted and the link
   fails.
5. The loader is freestanding/nostdlib with `_start` calling `real_main` in a
   `for(;;)`; `real_main` is marked `__attribute__((noreturn))`. Do not add a
   plain `return` or the optimizer turns the loop into a `b self`.
6. `.so` assets are gitignored (`out/`). They come from
   `tools/extract_apk.sh <apk>` (user-supplied APK, never committed). The APK is
   `net.kairosoft.android.gamedev3en` 2.6.9; Unity 2022.3.62f2 IL2CPP;
   `libunity.so` needs only `libmain, libandroid, liblog, libz, libEGL,
   libmediandk, libm, libdl, libc`.
7. Python modules (`unicorn`, `capstone`, `ziglang`) are NOT always present
   across sandbox instances — reinstall with
   `pip install --break-system-packages unicorn capstone ziglang` if imports
   fail. Earlier "module not found" runs were this, not code bugs.
8. `run_aarch64.py` resolves relative `.so` paths against the repo root, but if
   the asset is absent it can't run. Use an absolute path to the real `.so`.

9. **aarch64 Linux entry ABI is x0=argc, x1=argv** — `_start` must `mov` them
   out into plain C locals BEFORE anything else.  Do NOT use
   `register long x asm("x0")`: at -O0 the compiler spills such locals to the
   stack and reads garbage (that was the "loader always loads the default path"
   bug).  Use `__asm__ volatile("mov %0, x0" : "=r"(v))`.

10. **The bench must load a `-fno-pie` ET_EXEC at its OWN link base (minv)**,
    not relocate it to a made-up base.  PC-relative refs survive an offset, but
    the binary stores *absolute* link-time pointers in its data (e.g. the
    `host_syms.c` `tab[]` string table); an offset base makes those point at
    unmapped memory (`UC_ERR_READ_UNMAPPED` on a 1-byte read).  Fixed by mapping
    at `minv` in `run_aarch64.py`.

11. **ELF dynamic tags:** `DT_INIT_ARRAY=25`, `DT_FINI_ARRAY=26`,
    `DT_INIT_ARRAYSZ=27`, `DT_FINI_ARRAYSZ=28`.  It is very easy to swap
    INIT_ARRAYSZ/FINI_ARRAY (26/27) — if you do, `init_array` never runs
    ("init_array ran (0 ctors)").  Also parse `DT_JMPREL`/`DT_PLTRELSZ`
    (`.rela.plt`, tags 23/2) or JUMP_SLOT imports stay unresolved.

12. **GLOB_DAT/JUMP_SLOT relocs may reference symbols defined in the same .so**
    (e.g. `kairo_marker`).  Resolve against the module's own dynamic symtab
    first (`bias + st_value` when `st_shndx != SHN_UNDEF`), only then fall back
    to the host table.  Otherwise you get spurious "unresolved" warnings and a
    zeroed GOT slot.

13. `freestdlib.c` printf has no float/`%s`-with-width, and (until fixed) printed
    `%zu`/`%#zx` literally as `zu`/`#zx`.  It now strips `z/l/h/#/+` modifiers.

14. **`read_all()` used to read only the first 1 MB of the .so** — fine for the
    5 KB synthetic test, but it silently truncated the 33 MB `libil2cpp.so`, so
    the dynamic section (PT_DYNAMIC is at file offset ~30 MB) came out as
    garbage: relocations/init_array were skipped and the load "succeeded" with
    `relas=0`.  Fixed: it now grows the buffer to read the whole file.  If you
    ever see `relas=0` + no init_array on a real .so, suspect the file read.

15. **Init_array ctors run with the host's real-named libc symbols resolved**
    via `host_dlsym`'s table.  Any symbol the .so imports that isn't in the
    table prints `unresolved <name>` and its GOT slot is zeroed — a ctor that
    then calls it will crash with `UC_ERR_FETCH_UNMAPPED` at address 0.  Add the
    symbol to the table + `kv_libc.h` + a freestanding implementation.

16. **The loader binary is built `-fno-pie`, so its data holds absolute
    link-time addresses.**  Two consequences: (a) the bench must map it at its
    link base `minv` (gotcha 10); (b) `host_dlsym`'s `tab[]` entries that are
    `void*` *variables* (`__sF`, `environ`) must use `&__sF`/`&environ` in the
    table (the imported symbol is the *address* of the variable), or the
    initializer isn't a compile-time constant and the link fails.

17. **aarch64 reads thread-local storage through `tpidr_el0`.**  libunity.so's
    init_array does `mrs x19, tpidr_el0; ldr x10, [x19, #0x28]`, so if the bench
    leaves tpidr_el0=0 it faults on the first TLS access
    (`UC_ERR_READ_UNMAPPED @0x28`).  run_aarch64.py maps a zeroed TLS block and
    sets `UC_ARM64_REG_TPIDR_EL0` to it.  On the real device the kernel sets
    this per-thread.

18. **`read_all()` must not leak a doubling-growth buffer.**  It originally grew
    1MB→…→64MB by allocating a fresh buffer each time and leaking the old one
    into the bump allocator (64MB HEAP).  That was fine for one 33 MB .so, but
    loading a second 16 MB .so exhausted the heap and malloc(0) made the strtab
    read address 0.  Now it sizes the file first via `lseek(fd,0,SEEK_END)`,
    allocates exactly once, and HEAP_SIZE is 256 MB.  Requires the bench to
    implement lseek (syscall 62) — see the `sys_lseek` handler in run_aarch64.py.

## PROJECT LAYOUT

```
GDS_Unity/
  loader/                <-- THE R36S DELIVERABLE
    kv_elf.h             ELF64 types + relocation constants
    loader.c             maps .so, relocs, runs init_array, resolves imports
    freestdlib.c         minimal libc the loader carries (mmap/read/write/printf/mem*)
    host_syms.c          dlsym-style symbol table the .so's imports resolve to
    so_probe.c           synthetic .so that exercises the loader's .so path
                         (RELATIVE, self GLOB_DAT, JUMP_SLOT, DT_INIT_ARRAY)
    tiny.c / mini.c / rawtest.c   freestanding aarch64 test programs
    hello.c              (early) hello-world for toolchain smoke test
  tools/
    run_aarch64.py       Unicorn bench: runs a freestanding aarch64 ELF headless
    extract_apk.sh       unpack a user APK into out/ (gitignored)
    pngascii.py          dump a rendered PNG as ASCII (frame inspection)
    dbg_mini.py / dbg_raw.py   one-off harness debug scripts
  kairovm/               Unicorn-based x86 emulator of the game (REFERENCE only)
    game.py, unity.py, androidjni.py, bionic.py, machine.py, elfimage.py, ...
  README.md              overview + the terraria-nextos reference rationale
```

## NEXT STEPS (in order)

0. **(DONE)** Real `libil2cpp.so` + `libunity.so` both load, run init_array,
   exit 0.  **Also (DONE)** libunity's `JNI_OnLoad` runs to completion under
   `loader/jni_shim.c` and returns JNI_VERSION_1_6, guest exits 0.  The real GDS
   APK is at `APKs/Game+Dev+Story_2.6.9.apk` (53 MB), extracted to
   `GDS_Unity/out/apk/` (gitignored).  Rebuild + rerun:
   `bash loader/build.sh`
   `python3 tools/run_aarch64.py loader/loader2 $(pwd)/out/apk/lib/arm64-v8a/libil2cpp.so $(pwd)/out/apk/lib/arm64-v8a/libunity.so`
   Expect `init_array ran (18 ctors)` + `(1aa ctors)`, the FindClass inventory,
   `JNI_OnLoad returned 0x10006`, and `[guest exit 0]`.
1. **Drive Unity's main/player loop.**  `UnityMain` is invoked from the Java
   Activity's native methods (not JNI_OnLoad), and needs a real EGL surface +
   AssetManager + APK assets.  On the bench (no GPU) this can't fully run; the
   device-facing step is to call UnityMain / the player-loop entry and provide
   EGL/SDL + AssetManager.  This is the bulk of the remaining work and will
   surface the bionic/glibc ABI gaps (notably arm64 `sigaction`/`sigset_t`:
   bionic 8 bytes vs glibc 128).
2. Add the shim surface modeled on terraria-nextos:
   - `bionic_shims.c`: FORTIFY `_chk` wrappers, `__sF`, `__system_property_get`,
     and the **bionic/glibc `sigaction`/`sigset_t` ABI difference** (bionic arm64
     sigset is 8 bytes; glibc 128 — must convert, or it overruns caller stack).
     This is a real hazard.
   - `jni_shim.c` (built) + expand the stubs from the JNI call inventory
     (`jni_shim.c` already logs FindClass/GetMethodID — extend as new classes
     are requested).
   - `egl_shim.c` / SDL2: window, input, audio.
3. Build for the real device: the same `loader.c` compiled for `aarch64-linux-gnu`
   (glibc) instead of freestanding becomes the R36S binary. Test on-device for
   GPU/input; keep the Unicorn bench for non-GPU logic.

## WHY "ONE WRAPPER RUNS ALL KAIRSOFT GAMES" HOLDS

All Kairosoft titles of this era share the `kairo.unity.*` framework and the same
Unity version family; their `libunity.so` is identical and their `libil2cpp.so`
(the game) + a handful of `KairoPlugin` JNI calls differ. So the first port is
the expensive one; a second game is "write the new `KairoPlugin` JNI stubs it
happens to call, drop its APK in." The loader is the wrapper around the engine.
Honest caveat: not literally zero work per game — each title pokes slightly
different Java/plugin APIs and has its own asset quirks — but an order of
magnitude less than the first.

## ENVIRONMENT / HOW TO RUN

```
pip install --break-system-packages unicorn capstone ziglang   # if missing
cd GDS_Unity
bash loader/build.sh          # builds loader/loader2 (loader + freestdlib + host_syms + jni_shim)
# freestanding smoke test (no asset needed):
python3 tools/run_aarch64.py loader/tiny
# full loader: engine + game + JNI_OnLoad (needs extracted .so in out/apk):
python3 tools/run_aarch64.py loader/loader2 \
    $(pwd)/out/apk/lib/arm64-v8a/libil2cpp.so \
    $(pwd)/out/apk/lib/arm64-v8a/libunity.so
```

## DEVICE TEST (R36S) - READ THIS FIRST if handing off to a live test

The loader is a **standalone AArch64 ET_EXEC with no dynamic dependencies** -
it runs natively on the R36S (no glibc/pkg install needed).  It's packaged as a
**proper PortMaster port** so it shows up in EmulationStation under Ports.

Build the PortMaster package:

```
cd GDS_Unity
bash loader/build.sh                # builds loader/loader2
bash tools/extract_apk.sh ../APKs/"Game Dev Story_2.6.9.apk"   # -> out/apk
bash tools/make_port.sh             # builds GameDevStory_PortMaster.zip
```

Install on the R36S:
1. Copy `GameDevStory_PortMaster.zip` to the R36S SD card.
2. Unzip its contents into `/roms/ports/` (creates `/roms/ports/gamedevstory/`).
3. In EmulationStation: Ports → Game Dev Story (or rescan ROMs first).

The launcher (`Game Dev Story.sh`) locates the game folder and runs `loader2`
inside it.  `loader2` **self-logs to `gamedevstory/gamedevstory/loader.log`**
(open()d directly, so it is written regardless of how the shell captures
output).  **Send back that `loader.log`** - it shows: which libs loaded,
init_array ctor counts, JNI_OnLoad result, il2cpp_init result, and any memory
fault / abort.  The first line is
`[loader] === Game Dev Story native loader ===`, so you can tell at a glance
whether loader2 even started.

Port structure (matches PortMaster-New conventions):
```
gamedevstory/
  Game Dev Story.sh     launcher
  port.json             PM metadata (arch: aarch64)
  README.md
  gameinfo.xml          ES metadata
  screenshot.png / cover.png
  gamedevstory/         game files: loader2 + libil2cpp/libunity/libmain.so + data/
```

Known bench-only limitation (should NOT happen on the device): under Unicorn the
IL2CPP GC's deep runtime-internal calls can fault on the artificial stack/heap
model; on the real R36S with a real kernel/glibc/pthread/filesystem these run
natively and correctly.

## COMMIT STATE

- Session branch `arena/019fd2ec-asdf`.  (Note: the Unity/IL2CPP GDS APKs were
  committed on `arena/019fbc18-asdf` under `APKs/`; that branch's `APKs/`
  directory was NOT on `main`.  `git fetch origin '+refs/heads/*:refs/remotes/origin/*'`
  pulls it; `git show origin/arena/019fbc18-asdf:APKs/Game+Dev+Story_2.6.9.apk`
  recovers the 53 MB APK.)
- Latest commits on `arena/019fd2ec-asdf`: loader loads all three real native
  libs (libil2cpp.so + libunity.so + libmain.so), runs each init_array, drives
  the Android `JNI_OnLoad` chain (libmain's JNI_OnLoad -> JNI_VERSION_1_6),
  exits 0.  See commit messages for details.
- `out/`, `*.so`, and APK binaries are gitignored by design; only source is
  committed.  The extracted libs live in `GDS_Unity/out/apk/` (not committed).
  The Unity/IL2CPP GDS APKs are committed on `arena/019fbc18-asdf` under `APKs/`
  (not on main) - recover with
  `git fetch origin '+refs/heads/*:refs/remotes/origin/*'` then
  `git show origin/arena/019fbc18-asdf:APKs/Game+Dev+Story_2.6.9.apk`.

## CURRENT BLOCKER — CONSULT THIS FIRST

**The game boots into the player loop but never renders.**  We're handing this to
another model for fresh insight, so here is the complete, current picture.

### What WORKS (verified on-device, build 0.15.0-glibc)
- All 3 libs (libil2cpp.so + libunity.so + libmain.so) load, relocate, run
  init_array (24 + 426 + ctors).
- All 3 JNI_OnLoads fire, return JNI_VERSION_1_6.
- A real **SDL2 KMSDRM window + GLES2 context** is created with the **real Mali
  GPU driver**:
  ```
  [egl] SDL video driver = KMSDRM
  [egl] GL_VENDOR=ARM
  [egl] GL_RENDERER=Mali-G31
  [egl] GL_VERSION=OpenGL ES-CM 1.1 ...
  [egl] window 640x480 context ready (ES2)
  ```
  (The 4 `Can't window GBM/EGL surfaces` lines are the first 4 format attempts
  failing; the 5th format `{0,0,0}` SUCCEEDS — window + context exist.)
- initJni OK, nativeRecreateGfxState OK, then `nativeRender loop...` begins and
  runs a long sequence of JNI calls (hundreds of FindClass/GetMethodID for
  Android classes: Context, PackageManager, AssetManager, SharedPreferences,
  Looper/Handler, PlayAssetDelivery, ...) — the game IS executing its boot.

### What's WRONG (the blocker)
- The log ends right after Unity queries `android/app/AlertDialog$Builder` +
  `GetMethodID(setTitle)` + `GetMethodID(setMessage)`.  Unity is building an
  **AlertDialog** and then **hangs forever inside the first nativeRender call**.
- **Zero EGL calls** from Unity: the `[egl] CALLS ...` counters (GetDisplay/
  Initialize/ChooseConfig/CreateContext/MakeCurrent/SwapBuffers) NEVER appear.
  So Unity builds the dialog BEFORE it ever calls our EGL shim / GL.
- **nativeRender frame liveness never prints** (not even `frame 1 alive`), i.e.
  the FIRST `render()` call doesn't return — it blocks inside.
- Screen stays black.  It never gets to present.

### What we've already tried (each did NOT change the log — it stays byte-identical)
- **0.13 statfs shim** — made `statfs`/`statfs64` report 1 TiB free.  libunity
  contains "Not enough storage space to install required resources." (vaddr
  0xbdbe5).  No change.
- **0.14 fs-redirect** — intercepted open/fopen/stat/lstat/access to rewrite
  `assets/bin/Data/...` paths onto the local `data/` dir.  No change (so Unity's
  data reads go through the JNI AssetManager shim, which already works).
- **0.15 cmdline inject** — injected `-force-gfx-direct -force-gles20` into
  `/proc/self/cmdline` (Terraria's trick to force single-threaded rendering).
  No change.
- **0.16 JNI dialog trace** — added logging to Call* so the next log shows
  exactly what Unity does after setMessage (setPositiveButton/show/findLibrary)
  and returns the builder for AlertDialog.Builder chained setters.  Deploy
  pending.

### KEY HYPOTHESIS (unconfirmed)
The JNI calls right before the dialog include: `getPackageInfo`, `getPackageName`,
`findLibrary`, `PlayAssetDeliveryUnityWrapper.init`, `getAssetPackState`,
`getObbDirs`.  These smell like an **app-install / Play Asset Delivery / asset-pack
verification** check (not the storage-space check).  Unity verifies the game is a
"properly installed" Android app; finding none, it builds an AlertDialog and waits
on a **Java button callback** (setPositiveButton/OnClickListener) that our JNI
shim never invokes → deadlock.  The fix is likely: make the JNI shim return
"installed/verified OK" so no dialog is built, OR auto-fire the dialog's positive
button when it's shown.

The 0.16 trace (once deployed) will show the exact dialog-method sequence and
which check triggers it.

### The boot sequence our loader drives (loader_glibc_main.c kv_unity_boot)
egl_shim_create_window(); egl_shim_ensure_current(); → initJni(env,thiz,ctx) →
nativeRecreateGfxState(env,thiz,0,surf) x2 → nativeSendSurfaceChangedEvent →
nativeResume → nativeFocusChanged(1) → **loop nativeRender(env,thiz)**.  The
dialog build happens inside that first nativeRender.

### How to reproduce / iterate
- Deploy: `curl -sL -o gds_deploy.sh https://github.com/jackomix/asdf/raw/arena/019fd2ec-asdf/GDS_Unity/tools/gds_deploy.sh && chmod +x gds_deploy.sh && ./gds_deploy.sh ark@192.168.18.20`
- Read `gamedevstory/gamedevstory/loader.log` (fresh each run now).
- Key files: `loader/loader_glibc_main.c` (boot), `loader/egl_shim.c` (EGL/GL),
  `loader/jni_shim.c` (JNI shim — the dialog is built through here),
  `loader/fs_redirect.c` (open/fopen redirect + cmdline inject),
  `loader/bionic_bridge.c` (pthread/signal/statfs), `loader/glibc_shims.c`.
- Reference ports (both by NextOs-Ports, both run Unity on THIS R36S — see the
  "THE TWO REFERENCE PORTS" section near the top):
  `NextOs-Ports/terraria-nextos` (so_load/import-patch/asset-redirect/TLS/bionic)
  and `NextOs-Ports/horizonchase-nextos` (raw-futex `syscall` intercept, GC
  stop-the-world, job-inline).  Mine them before reinventing any mechanism.

## LATEST STATUS (build 0.36.0-glibc) - fix ptrace waitpid __WALL

0.34 deployed, 3 watchdog dumps captured. Findings:

1. **Deadlock persists across dumps #0..#2** (-12s, -24s, -36s). Process not progressing.
2. Main thread `tid=14158`:
   - dumps #0/#1: `state=S` syscall 98 op=`0x189` (`FUTEX_WAIT_BITSET | PRIVATE | CLOCK_REALTIME`) `a4=0x7fca297b58` (FINITE timeout). Our `kv_syscall` 2ms poll-reinject IS active (timeout pointer non-null).
   - dump #2: `state=R` (RUNNING) but `syscall=98 0x7d1a2c0 0x189` — still parked-on/futex-val but actively scheduled. So poll returns, Unity loops back to futex_wait, infinite loop.
3. 14 worker threads all parked `op=0x189` infinite (`a4=0x0`) — Unity ThreadPool threads waiting for jobs that never come (main thread holds the work queue). `kv_set_job_workers_zero` never ran (still waiting at f>=30 but renders never complete).
4. 2 workers on `ppoll` (syscall 73); `tid=14162` on syscall 115 (`clock_nanosleep`); `tid=14199` syscall 63 — at `sigaltstack`.
5. **`/proc/<tid>/wchan` and `/proc/<tid>/stack` printed ZERO lines** in all 3 dumps — `ark` user has no read perm on those (yama ptrace_scope=1 or similar). Useless for diagnosis.

### 0.35 changes

1. Watchdog now ALSO does `ptrace(PTRACE_ATTACH, tid)` on every thread, then `PTRACE_GETREGSET` (NT_PRSTATUS) → `user_regs_struct`. Prints user-space `pc`, `lr`, `sp`, `x0..x3`, `x19..x22` per thread.

   `PTRACE_ATTACH` from same-process sibling thread should work even under yama=1 (self-attach). `pc` will be inside libunity.so where the `pthread_cond_wait`/`sem_wait`/`futex` syscall was issued. We can disasm that page to identify the calling function name (via `objdump -T` symbol offsets) and proceed with a targeted GOT/call intercept.

2. Bumped version → 0.35.0-glibc.

### Expected 0.35 output

Each watchdog dump (~12s after render loop starts) will print per-thread:
```
[watchdog] tid=14158 user: pc=0x20234XXXX lr=0x20243YYYY sp=0x7fca297b58
[watchdog] tid=14158        x0=... x1=... x2=... x3=...
```
`pc` will fall in libunity (`0x20222f000..0x203269000`) or libil2cpp (`0x200000000..0x20221f000`). Cross-reference with `aarch64-elf-objdump -T libunity.so` dynamic symbol table + `objdump -d` disasm to find the function the main thread is parked inside (most likely `pthread_cond_wait` or `std::condition_variable::wait` inside Unity's `JobSystem::Exec` waiting for the worker queue signal).

Once identified, next version intercepts that function in `bionic_bridge.c` route table OR patches the GOT slot in libunity.

### 0.35 result - ptrace attach silent (no pc lines)

Watchdog dumps #0..#2 ran the syscall/kstack block but the ptrace block printed ZERO lines — no `user: pc=...`, no `PTRACE_ATTACH failed`, nothing. The for-loop went through every tid, all threads had `tid_l > 0`, so we DID call ptrace. Means `ptrace(PTRACE_ATTACH)` returned non-zero (silently falls through the if). Or the printf is buffered and the program is struggling to flush mid-loop. But `0.34` watchdog flushed `=== end dump #N ===` fine each round, so flush isn't the issue.

No-yama on R36S (verified `cat /proc/sys/kernel/yama/ptrace_scope` returns ENOENT). So permissions aren't the problem.

**Real cause**: `waitpid(tid, &status, 0)` blocks forever waiting for SIGSTOP. For ptrace-attach to a SIBLING thread (same process, not a child pid), glibc `waitpid(2)` with default flags refuses to wait for clone/non-fork children — returns ECHILD, ptrace attach never observes the stop, the `waitpid` either returns immediately (ECHILD), OR the conditional never enters the GETREGSET block.

Fix per Linux man-page `ptrace(2)`: "**Use the `__WALL` flag** with `waitpid(2)` for ptrace-attached threads" (since they are CLONE_CHILDREN from kernel perspective, not fork children).

### 0.36 result

Every thread printed `PTRACE_ATTACH failed`. Self-attach forbidden on this darkOSre R36S kernel build (no yama, but hardened — `prctl(PR_SET_PTRACER)` not pursue-able either).

**BUT(tolua nobody)**: the watchdog already had ALL the PC info it needed. `/proc/<tid>/syscall` format is `syscall# arg0 arg1 arg2 arg3 arg4 arg5 sp pc` — the LAST two fields are user-space SP and PC (return IP after `svc`). We were printing them raw the whole time but treating them as opaque context bytes.

For the main thread `tid=17377`: `pc=0x7f9f94ef6c` (the last hex value). That's pointer into dynamic librange. Now we need to resolve it.

### 0.37 - parse PC from /proc/<tid>/syscall + /proc/self/maps

Replaced ptrace approach with:

1. `maps_refresh()` reads `/proc/self/maps` once per watchdog dump (~64KB buffer).
2. Per-thread: parse the 9 fields of `/proc/<tid>/syscall`, take `fields[7]=sp`, `fields[8]=pc`.
3. `maps_resolve(pc)`: walks snapshot, finds the line `lo-hi perm offset dev inode pathname` that contains `pc` (range check `lo<=pc<hi`), returns `pathname` + `pc - lo`.
4. Printf `[watchdog] tid=X user-pc=0x... sp=0x... in <path/to/lib.so> +0xN`.

For our libil2cpp at base `0x200000000` and libunity at `0x20222f000`, we expect output like:

```
[watchdog] tid=17377 user-pc=0x7f9f94ef6c sp=0x... in /usr/lib/libpthread-2.33.so +0x1e6c
```

That's libc's `pthread_cond_wait` futex-wait loop — telling us libunity is parked in `pthread_cond_wait` (or `sem_wait`), waiting for the job-queue signal that never comes.

### Next (planned for 0.38)

Once 0.37 confirms the wait site is libc `pthread_cond_wait` / `sem_wait` (probably matching the `0x189` futex we already see but inside a libc symbol), `bionic_bridge.c` route table gets a GOT intercept of `pthread_cond_wait`/`pthread_cond_timedwait` returning 0 immediately (or `sem_wait` returning 0), so Unity's own wait returns "completed" and the player loop advances. Also `kv_set_job_workers_zero` still hasn't run (first frame never finishes), so once the wait is unblocked the jobfix runs naturally.

0.33 fix worked: `nativeRender` now enters (jobfix no longer pre-probe kills it).  Log shows the player loop STARTED:
```
[unity] nativeRender loop...
[fs] injected /proc/cpuinfo (1 CPU) * 4
[jni] FindClass(android/os/Process)
[jni] GetMethodID(setThreadPriority, (II)V)
[kv_syscall] SYS_futex tid=12025 op=0 a4=0x7f5e7ce6d0
... (many getSystemService / AudioFocus / ArCore / PlayAssetDelivery / SharedPreferences / AlertDialog.Builder / Handler / ...)
[jni] GetMethodID(setMessage, (...))
[watchdog] === thread dump (blocked syscalls) ===
[watchdog] tid=11980 state=S  <- MAIN thread, blocked!
[watchdog] tid=11980 syscall: 98 0x1cb24240 0x189 0x0 0x7fdbff9bd8 ...
```

**Main thread blocks on futex** (syscall 98, op `0x189` = `FUTEX_WAIT_BITSET | FUTEX_PRIVATE_FLAG | FUTEX_CLOCK_REALTIME`, `a4=0x0` infinite timeout). Our `kv_syscall` rewrites infinite futex waits to 2ms poll, so the wait returns regularly — but Unity immediately re-waits and never makes progress into the next frame.

14 extra worker threads spawned (Unity threadpool — all waiting `0x189`). `tid=12024 state=R` on syscall 63 (`sigaltstack`?); two workers on syscall 73 (`ppoll`). So we have:
- 1 blocked main + 14 blocked workers = entire process frozen IN the first `nativeRender` call.
- `tid=12025 op=0 a4=0x7f5e7ce6d0` prints from our `kv_syscall` log; that's one of the polling futexes, suggests our poll-inject flows.

The log CUT OFF at `[jni] GetMethodID(setMessage, ...)` — that's just where Unity happened to be when the watchdog fired (after 12s). Not the actual block site.

### What 0.34 does

1. Less verbose SDL window-format probing — only prints which format won, not the SDL "Can't window GBM/EGL surfaces on window creation" warning (which is SDL's own KMSDRM log, not ours to suppress).
2. Watchdog now loops 5 dumps at 12s intervals (so we see if the deadlock PERSISTS or just stalls briefly). Each dump includes per-thread `/proc/<tid>/wchan` (kernel wait site name e.g. `futex_wait_queue_me`) and `/proc/<tid>/stack` (kernel call chain) — that reveals which kernel primitive holds the wait, narrowing the lock site.
3. Bumped version → 0.34.0-glibc.

### Next

Deploy 0.34, capture `loader.log` with all 5 watchdog dumps. With per-thread `wchan` + kernel `stack` we'll know exactly which futex (REALTIME CLOCK vs MONOTONIC, private vs shared) the thread is parked on, and can either:
- hook the underlying libc primitive (pthread_cond_wait uses CLOCK_REALTIME under futex 0x189? check),
- or dump user-space PC via `ptrace(PTRACE_ATTACH, tid)` to disasm where in libunity/libil2cpp the wait call originates.

0.32 deployed the `ucontext_t` struct-access crash handler. **It worked perfectly**:

```
[loader] === CRASH sig=11 ===
[loader]   FAR=0x135
[loader]   pc=0x200cfccd4 sp=0x7ff0028f70 fp(x29)=0x7ff0029080 lr(x30)=0x200d11c24
 x0 =0x0 ...
[loader]   pc in [0x200000000..0x20221f000)   (pc-offset=0xcfccd4)
[loader] backtrace (x29 chain):
[loader]   #0 lr=0x101ac64 (fp=0x7ff0029080)
[loader]   #1 lr=0x1019fe0 (fp=0x7ff00290d0)
[loader]   #2 lr=0x1019af8 (fp=0x7ff0029190)
[loader]   #3 lr=0x7f806e225c (fp=0x7ff00291b0)
```

Decoding with `aarch64-elf-objdump -d` on libil2cpp.so + loader2 (installed via brew):

- `pc=0xcfccd4` → libil2cpp: `ldrb w8, [x0, #309]` where `x0 = x19 = 0`. Crash = NULL `MonoClass*` + offset `0x135` (309 = field flag byte). Label is `mono_class_get_checked+0x17b3c`.
- `lr=0xd11c24` → caller `mono_class_get_checked+0x2c9a0` (small wrapper at `0xd11c14` calls the buggy fn). At `0xd11b4c` there's an explicit `mov x19, xzr` when `tbnz` bit 4 not set — that NULL x19 then propagates into `[x19+309]`.
- `lr=0x101ac64` → loader2 `kv_unity_boot` line 584 — immediately AFTER `bl kv_set_job_workers_zero` at `0x101ac60`. Backtrace:
  - `#0 lr=0x101ac64` = `kv_unity_boot` (return from `kv_set_job_workers_zero`)
  - `#1 lr=0x1019fe0` = `real_main` line 754
  - `#2 lr=0x1019af8` = `main` line 661
  - `#3 lr=0x7f806e225c` = libc `_start`

**Root cause**: We call `kv_set_job_workers_zero()` BEFORE the first `render()` call (frame `f=0` in the player loop).  But `il2cpp_init` runs inside the FIRST `nativeRender` call — so calling `il2cpp_class_from_name("Unity.Jobs.LowLevel.Unsafe", "JobsUtility")` pre-init triggers `mono_class_get_checked` on a domain with no assemblies loaded yet → returns NULL → propagates through the wrapper's `mov x19, xzr` → next op `[x19+309]` faults.

`global-metadata.dat` IS present (`data/Managed/Metadata/global-metadata.dat`, verified) — metadata isn't missing; we're just probing IL2CPP domain state too early.

### Reference port confirmation

`/Users/jacko/Documents/astro/ref/terraria-nextos/src/main.c:2853`: ref ports call their equivalent `ter_jobworkers0()` from `ter_before_present()` — a hook on `eglSwapBuffers`, fired AFTER the first render+swap completes (IL2CPP runtime fully init'd). NOT pre-render.

### Fix in 0.33 (`loader/loader_glibc_main.c`)

Moved the `kv_set_job_workers_zero()` call to AFTER `render(env, thizp)` inside the player loop, gated to `f >= 30 && f < 240` (mirrors ref's 240-frame retry window + lets a few first frames settle). Same `kv_jobworkers_done` state guard.

### Toolchain note

Installed `aarch64-elf-binutils` (brew) for `objdump -d/-T`, `addr2line`, `nm`. libil2cpp.so is stripped but `objdump -T` gives dynamic symbol labels (the `mono_class_get_checked@@Base+0x17b3c` style offsets). loader2 has debug_info, so `addr2line` resolves source lines directly.

### Next

Deploy 0.33, read `loader.log`. Expect either:
- Successful render loop (frames printing) → `kv_set_job_workers_zero` succeeds around frame 30+ → `[jobfix] set_JobWorkerCount(0) invoked (exc=...)`.
- A NEW crash with real PC + backtrace deeper in Unity's first-frame pipeline (no longer the pre-render probing bug).

0.31 deployed the `sigaction`/`sigaltstack` filter.  **First time ever, the crash handler fired**:

```
[loader] === CRASH sig=11 ===
[loader]   si_addr=0x135 FAR=0x0
[loader]   pc=0x20000000 sp=0x7fd1dfef80 fp(x29)=0x200d11c24 lr(x30)=0x200cfccd4
[loader] backtrace (x29 chain):
[loader]   #0 lr=0x361800483944d668 (fp=0x200d11c24)
```

Decoding:
- `sig=11` = SIGSEGV - that's a real fault.
- `si_addr=0x135` / `FAR=0x0` - classic **NULL pointer + 0x135 field offset** pattern.  Some pointer is NULL, code did `ldr [ptr, #0x135]`.
- `lr=0x200cfccd4` lies inside libil2cpp's mapped range `0x200000000..0x20221f000` (offset `0xcfccd4`) - so the call site is in libil2cpp.
- `fp=0x200d11c24` is also libil2cpp.
- BUT `pc=0x20000000` is **only 8 hex digits** while our mapped libs all start at `0x20xxxxxxx` (9 hex digits) - 0x20000000 is unmapped, and the backtrace lr=`0x361800483944d668` is obviously garbage.

Diagnosis: my hand-computed byte offsets for mcontext fields were WRONG for this glibc build.  I read `pc@ucontext+0x1C0`, `x30@ucontext+0x1B8` etc; the actual aarch64 glibc `ucontext_t` layout depends on `stack_t` size inside `ucontext_t` (which can vary by build), so my byte offsets produced garbage.  The reference ports (terraria-nextos, horizonchase-nextos) just use the **struct field access**: `uc->uc_mcontext.pc`, `uc->uc_mcontext.regs[30]`, etc - that's the correct approach and compiler-checked.

Fix in `loader/glibc_shims.c`:
- `kv_sighandler` now does `ucontext_t *uc = (ucontext_t *)ucontext; unsigned long pc = uc->uc_mcontext.pc;` and `.regs[30]` for lr, `.sp` for sp, `.fault_address` for FAR, `.regs[29]` for fp.
- Dumped `x0..x28` in a 3-per-line grid so we can see arg values at crash site.
- Bumped crash handler buf from 640 → 4096 bytes (the maps-parse block + backtrace + x0..x28 fits).
- Removed the (silently failing) byte-offset k_si_addr / si_addr cross-check.

### Expected 0.32 result

A real, sane `pc=0x2XXXXXXX` value (9 hex digits) that maps cleanly to libil2cpp/libunity/loader/SDL/Mali's mapped range, with the `[loader]   pc in [<range>) <path>  (pc-offset=0xN)` line now printed (it was eaten by either the bad-char-in-path sprintf path or buf-truncation at 0x31).  That pc-offset, plus the x0..x28 dump + the [] backtrace, will pinpoint the exact crash site + function.

### What we already know about the crash from 0.31

Even with garbage pc: the **faulting NULL+0x135 access** likely means a Unity-internal structure pointer is NULL when libil2cpp's `nativeRender` enters its first real work.  The lr=0x200cfccd4 (inside libil2cpp, offset ~0xcfccd4) is the call site - that single value DID get read correctly (regs[30] is the lower-address +), at minimum we can disassemble around that offset with `aarch64-linux-gnu-objdump -d libil2cpp.so` once we have a real toolchain.  But the lr value tells us we're in libil2cpp code that called something which dereferenced NULL+0x135.

### Prior: 0.31 - filter sigaction/sigaltstack

0.30 silent exit again (same byte-identical log).  nm on libunity.so:

1. `nm -D libunity.so` confirmed libunity imports `exit`, `raise`, `__stack_chk_fail`, `_ctype_`, `sigaction`, `sigaltstack` (NO `abort`/`_exit`/`tgkill` directly).
2. **Unity installs its OWN sigaction + sigaltstack during its init/first-render path**, OVERWRITING ours. Our re-arm after `egl_shim_create_window` worked - briefly. But Unity re-installs again INSIDE `nativeRender` (we can see this in the crash point: boot passes nativeRecreateGfxState, gets through initJni, reaches nativeRender frame 0, dies silently = SIGSEGV went to Unity's own handler with default action).
3. Same diagnose as terraria-nextos bionic_shims.c:117 (comment CUP_NOSIGH): **"não deixa o engine instalar handler de sinais de crash -> nosso handler pega o fault ORIGINAL"**.  Both refs filter Unity's `sigaction` calls to no-op pretense for crash signals {4,5,6,7,8,11}.

Fix in `loader/bionic_bridge.c`:

- `kv_sigaction` now intercepts calls for crash-signals (`SIGILL=4, SIGTRAP=5, SIGABRT=6, SIGBUS=7, SIGFPE=8, SIGSEGV=11, SIGSYS`) — returns 0 (pretend-success) WITHOUT calling the real `sigaction`.  Our `on_crash` stays as the actual handler.
- `kv_sigaltstack_noop` added: Unity's `sigaltstack` calls return 0 pretense; we keep OUR 256KB alt stack installed (so the handler push never overflow-faults again).
- `kv_sigaltstack_noop` routed via `kv_bionic_route` table entry `{sigaltstack, kv_sigaltstack_noop}`.

If the crash was Unity SIGSEGV → its own overwriting handler → silent exit 139 pipeline, 0.31's filter cuts that pipeline: now SIGSEGV goes to OUR handler → expect `[loader] === CRASH sig=11 ===` block with PC + backtrace on next deploy.

If still silent: the crash takes a path not via `sigaction` (e.g., kernel-level - oops or seccomp) and our route didn't bind (the GOT entries for `sigaction`/`sigaltstack` weren't actually re-routed). Next debug step = verify `kv_sigaction` got called at least once by adding a counter print on first entry.

### Prior: 0.30 - mine reference ports

0.29's deploy showed **exit 139 with NO `[loader] === CRASH ===` block** despite our handler being installed.  Same silent death as 0.28.  We then cloned the two reference ports locally:

```
/Users/jacko/Documents/astro/ref/terraria-nextos/      # NextOs-Ports/terraria-nextos
/Users/jacko/Documents/astro/ref/horizonchase-nextos/ # NextOs-Ports/horizonchase-nextos
```

and mined them end-to-end.  **Decisive learnings** (all absent from our code, all cited in the HANDOFF's "REFERENCE PORTS" section near the top but never actually implemented here):

1. **256KB alt stack**, not 32KB.  Both refs: `static char altstk[256 * 1024]`.  Our 32KB was too small to host a handler that opens `/proc/self/maps` and walks frame chains - when the kernel tried to push the signal frame + run the handler, the alt stack overflowed, the kernel re-faulted, and it ran default SIGSEGV (silent exit 139).  (_terraria-nextos/src/main.c:4343, horizonchase-nextos/src/main.c:5994_)

2. **Catch SIGABRT, SIGTRAP, SIGSYS in addition to SEGV/BUS/ILL/FPE.**  Both refs `sigaction(SIGABRT/SIGTRAP/SIGSYS, &sa, 0)`.  Without these, BRK instructions, seccomp traps and Unity's `abort()` go to default action - silent. (_terrania/main.c:4347-4349, horizonchase/main.c:5998-6000_)

3. **Re-arm `on_crash` AFTER `egl_shim_create_window`.**  Both refs do this (terrania/main.c:4997-5006, horizonchase/main.c:6713-6720) with an explicit comment: **"SDL_Init(VIDEO) of kmsdrm and/or the Mali blob reinstall SIGSEGV default - our dump never runs."**  This was the smoking gun: our `kv_install_crash_handler` ran in `main()` BEFORE `egl_shim_create_window()`, so by the time `nativeRender` was called the Mali driver had overwritten our `on_crash` with its default.  Added `kv_install_crash_handler()` re-call in `kv_unity_boot` right between the egl-shim window setup and the first native call.

4. **Override `abort`/`raise`/`tgkill`/`exit`/`_exit`** GOT entries with logging wrappers - `set_import("abort", my_abort)` etc (_terrania/main.c:4374-4378, horizonchase/main.c:6026-6030_).  Without this, Unity detecting an internal error calls libc `abort()` directly, the libc kill path runs BEFORE our SIGABRT handler sees the signal, and the process exits silently (139 exit, no log line).
   Implementation: `kv_engine_abort` writes `[loader] === ENGINE ABORT caller=%p ===`, then triggers `*(volatile int*)0=0` so the crash handler dumps a real PC.  `kv_engine_raise`/`tgkill` forward via `raise()` (caught by our handler).  `kv_engine_exit` logs and exits with the requested code so the silent `_exit(139)` becomes **`[loader] === ENGINE exit(139) caller=0x...`** in the log.

5. **`_ctype_` must be a real bionic char-class TABLE POINTER, not an empty stub.**  Both refs: `static unsigned char g_ctype_table[257]; const unsigned char *g_ctype_ptr = g_ctype_table;`  and the import resolves to the pointer-to-table, not to a function.  (_terrania/main.c:3790-3826_)
   
   Our old `_ctype_(void){}` stub in `host_syms.c:341` and `glibc_shims.c` was the LIKELY ROOT CRASH CAUSE: libunity reads `_ctype_` as `ldr [got] -> ptr; ldr [ptr] -> table`.  When the GOT bound to a function's entry point (code), the second `ldr` returned unmapped bytes - in asset/string processing inside `nativeRender`, the engine eventually **fed that garbage into a load and SIGSEGV'd**. Terraranextos documents the exact symptom: "crash libunity+0xe449d4 no asset loading" - the crash site of the missing-`_ctype_` bug.
   Fix in `glibc_shims.c`: declare `unsigned char g_kv_ctype_table[257]; const unsigned char * const _ctype_ = g_kv_ctype_table;` plus `_tolower_tab_`/`_toupper_tab_` siblings, and `kv_ctype_init()` (called from `main` before any ctor runs) fills them with proper bionic bits (`_U=1 _L=2 _N=4 _S=8 _P=0x10 _C=0x20 _X=0x40 _B=0x80`).
   Also removed the conflicting stub `_ctype_()` in `host_syms.c`.

6. **`__stack_chk_fail` returns instead of aborting.**  Both refs override `__stack_chk_fail` to return (terrania/main.c:3765) because Unity's tagged-canary `operator-new` can mis-fire on certain `nativeRecreateGfxState` paths; killing the boot.  Returning lets the caller carry on.  Our `freestdlib.c:__stack_chk_fail` writes a line then `abort()`s - which previously took the abort path the ref ports avoid.  New `kv_stack_chk_fail` in glibc_shims.c writes a `[loader] __stack_chk_fail #%d caller=%p (continuing)` line and returns.  Routed via `kv_bionic_route` so the .so's own `__stack_chk_fail` import binds here in the glibc build.

### Expected deploy result of 0.30

If the `_ctype_` hypothesis is right, the `nativeRender loop...` will advance past the cmdline-inject line and we'll see either EGL calls from Unity or the next crash - but WITH a real `[loader] === CRASH ===` block now (because the handler is correctly re-armed post-SDL + alt-stack is 256KB + abort/raise/_exit routes log).

If the silent-exit 139 returns: the route table didn't bind.  Verify via log for `[loader] === ENGINE ABORT ... ===` or `[loader] === ENGINE exit(N) ... ===` - those tell us which path the engine took to terminate.

### Prior: 0.29 - sigaltstack + crash backtrace

0.28 segfaulted inside first `nativeRender`, exited 139 with **no crash line in log**.  Diagnosed as `SA_ONSTACK` without `sigaltstack()` - kernel had no alt stack to push signal frame.  0.29 added sigaltstack + the rich crash handler.  But the silent exit persisted - alt stack was 32KB (too small - see 0.30 fix above) AND either Unity's `abort` (not SIGSEGV) was the actual termination path AND SDL/Mali had re-installed the default SIGSEGV handler over ours AND `_ctype_` empty-stub crashed before any real fault dump landed.


---

## LATEST STATUS (build 0.28.0-glibc) - Deadlock bypassed, now hitting Segfault (exit 139)

0.28.0 successfully bypassed the `AlertDialog` deadlock! The two fixes were:
1. **`findLibrary` returns `NULL`**: In `jni_shim.c`, returning `NULL` instead of the game directory tells Unity's `ClassLoader` that the native library is already loaded. Previously, returning a path caused Unity to attempt a `dlopen`, which failed and triggered the generic "failed to load" `AlertDialog`.
2. **`TER_JOBWORKERS0` implemented**: Inside `loader_glibc_main.c`, we now use IL2CPP reflection to lazily invoke `JobsUtility.set_JobWorkerCount(0)` (and related setters) during the first few frames of `nativeRender`. This forces Unity to run all jobs inline on the main thread, bypassing the worker thread futex deadlock entirely.

**Current Blocker (Segfault):**
With the deadlock gone, Unity proceeds into the `nativeRender` loop. The log shows:
```
[unity] nativeRecreateGfxState OK
[unity] nativeRender loop...
[fs] injected cmdline: -force-gfx-direct -force-gles20
```
Immediately after this, the loader crashes with a **Segmentation fault (code 139)**.

The crash happens *after* `nativeRecreateGfxState` finishes and *during* the `nativeRender` loop execution. Because Unity is now actually trying to render (or at least initialize its internal rendering pipeline on the main thread), it is likely dereferencing a bad pointer related to our fake EGL/GL context, the `thiz`/`ctx`/`surf` JNI objects we pass to `nativeRender`, or a missing JNI method.

### Prior: 0.27.0 - dlsym interception

0.26.0's fix (intercepting `syscall`) successfully reached the main thread, but the job worker pool STILL blocked on `syscall(SYS_futex)` with a NULL timeout! The watchdog thread dump revealed that all blocked worker threads shared the exact same PC (`0x7f8616ef6c`) as the main thread. This meant they were calling the raw GLIBC `pthread_cond_wait` directly, executing its internal `svc 0` instruction.

Why did they bypass our `kv_pthread_cond_wait` and `kv_syscall` shims? Unity's C++ Engine core (main thread) resolves functions via the ELF PLT, which we intercepted correctly. However, Unity's IL2CPP Job System resolves platform primitives dynamically at runtime using `dlsym(handle, "pthread_cond_wait")` or `dlsym(handle, "syscall")`. 
Because we were not intercepting `dlsym` in `loader_glibc_main.c`, IL2CPP received pointers to the real, unshimmed GLIBC functions and used them directly.

Fix (0.27.0): Added `kv_dlsym` to `loader_glibc_main.c`. `dlsym` calls are now intercepted, and `kv_dlsym` feeds the requested symbol back through our `kv_bionic_route` and `kv_egl_route` tables before falling back to glibc's `dlsym`. Now, when IL2CPP requests `"pthread_cond_wait"` or `"syscall"`, it receives our shimmed polling functions.

### Prior: 0.26.0 - syscall shim: raw futex poll + sched_getaffinity

0.25's dump: workers STILL futex-wait with NULL timeout (0x0); the opendir shim
NEVER fired (so Unity doesn't count cores that way).  Studying horizonchase-nextos
(same author, Unity) revealed the missing piece: Unity's JOB SYSTEM calls raw
`syscall(SYS_futex, FUTEX_WAIT, ...)` DIRECTLY, bypassing pthread_cond/sem.  That
is exactly why workers futex-wait with timeout=NULL - my pthread/cond/sem polling
shims never reach them.  Horizonchase's TER_FUTEXPOLL intercepts syscall() and
injects a short timeout into any FUTEX_WAIT without one.

Fix (0.26.0) in bionic_bridge.c: kv_syscall intercepts:
- SYS_futex (98): any FUTEX_WAIT(0)/WAIT_BITSET(9) with timeout=NULL gets a 2ms
  poll timeout injected -> worker wakes, re-checks its queue, can make progress.
- SYS_sched_getaffinity (123): forces 1-CPU mask at the syscall level too.
Both libil2cpp.so and libunity.so import syscall (verified via nm), so this
reaches the raw futex path the worker pool uses.

### Prior: 0.25.0 - 1-CPU shim v3: + opendir/readdir /sys/cpu

Three external models reviewed 0.24's dump.  Decisive findings (cross-checked
against nm imports):
- libil2cpp.so AND libunity.so both import **opendir + readdir** (verified).  This
  is the missing CPU-count path: Unity scans /sys/devices/system/cpu/cpu*/ as a
  DIRECTORY to count cores.  My kv_open prefix interception never sees opendir.
- libs do NOT import openat (Claude's theory doesn't hold) or get_nprocs.
- Main thread tid 1211 now has a non-null timeout (polling works); workers
  (1218-1255) still futex-wait with NULL timeout = infinite waits not in poll shim.
- 1-CPU shims (sched_getaffinity/sysconf/cpuinfo/sys files) all fire but workers
  still get created -> the directory-scan path is the remaining leak.

Fix (0.25.0): add opendir/readdir/closedir interception to fs_redirect.c.
opendir("/sys/devices/system/cpu*") returns a fake DIR containing only "cpu0";
readdir/closedir on it are handled.  Combined with all prior shims, Unity should
now see 1 CPU from EVERY source -> 0 job workers -> jobs run inline.

### Prior: 0.24.0 - 1-CPU shim v2: + /proc/cpuinfo + /sys/cpu

0.23's dump: main thread tid 9866 now shows `syscall: running` (SPINNING - my
polling shim works), but the worker pool STILL exists (~10 threads, 9888-9892,
9920-9924) futex-waiting with NULL timeout (infinite pthread_cond_wait NOT going
through my poll shim - separate wait sites).  So Unity still sized a worker pool.

The sched_getaffinity/sysconf 1-CPU shims compiled (verified in binary) but did
NOT stop worker creation - Unity reads core count from /proc/cpuinfo or
/sys/devices/system/cpu/ directly too.  Terraria intercepts ALL THREE.

0.24 adds to fs_redirect.c's kv_open/kv_fopen: when Unity opens /proc/cpuinfo or
/sys/devices/system/cpu/{present,possible,online}, return a synthetic file
reporting 1 CPU.  Combined with the sched_getaffinity/sysconf shims, Unity should
now see 1 core -> 0 job workers -> jobs run inline -> main thread stops spinning
on a worker it can never wake.

### Prior: 0.23.0 - job-system deadlock: force 1 CPU (jobs inline)

0.22's thread dump PROVED the polling shim was working: main thread tid 8644 was
in futex WAIT_BITSET(0x189, CLOCK_REALTIME) WITH a timeout pointer (0x7fe57ab968) -
i.e. it IS in my polling pthread_cond_timedwait, waking every 2ms.  But it re-checks
and finds its predicate still false, so it sleeps again.  Polling the waiters does
NOT create the work that produces the result.

Root cause (confirmed): the main thread waits for a JOB to complete; the job is
dispatched to a worker pool, but the workers never run it (their Java/looper driver
doesn't exist under our loader).  This is Terraria's documented job-system break.

Fix (0.23.0): report a SINGLE CPU so Unity creates 0 job workers and runs jobs
INLINE on the main thread.  Added to bionic_bridge.c:
- sched_getaffinity -> mask with only CPU 0 (1 core)
- sched_setaffinity -> no-op
- sysconf(_SC_NPROCESSORS_ONLN=0x62/_SC_NPROCESSORS_CONF=0x61) -> 1
Terraria's TER_JOBWORKERS0 / my_sched_getaffinity approach.  libunity imports
sched_getaffinity/sysconf (verified via nm).  Jobs should now run inline -> main
thread stops futex-waiting on a worker it can never wake -> boot proceeds.

### Prior: 0.22.0 - ROOT CAUSE FOUND: job-system deadlock, fixed with polling

0.21's working thread dump gave us the answer:
- main thread tid 7330: state=S, syscall 98 = futex(0x2f346440, FUTEX_WAIT, ...)
- worker pool tids 7338-7344, 7370-7375: all state=S, futex FUTEX_WAIT on distinct
  addresses (0x2f2050d0..0x2f2114a8) - each worker blocked on its own cond var.
- tid 7376 state=R (spinning), 7377-7379 futex-wait on stack addresses.

This is the CLASSIC Unity job-system deadlock (Terraria documents this exact bug):
the main thread + a pool of workers all futex-wait on condition variables that are
never signaled, because on Android the Java Activity/looper that drives the job
system doesn't exist under our loader.  Every thread sleeps forever -> black screen.

Fix (0.22.0) in bionic_bridge.c: make pthread_cond_wait and sem_wait POLL instead
of blocking forever - timed wait with a short slice (2ms main / 5ms worker), return
as a spurious wakeup on timeout so the caller re-checks its predicate in its while()
loop and makes progress.  This is Terraria's CUP_CONDPOLL approach.  pthread_cond_
timedwait also capped to poll.

Next on-device: the job system should advance (main thread wakes, re-checks, runs
jobs inline) instead of sleeping forever.  Expect the log to proceed past the
AlertDialog registration and reach real egl calls / frames.

### Prior: 0.21.0 - watchdog fixed (struct dirent layout was wrong)

0.20's watchdog fired but found ZERO threads - that was a BUG in the diagnostic,
not a healthy sign: my struct dirent didn't match glibc's aarch64 layout (missing
d_off/d_reclen), so readdir returned entries with d_name at the wrong offset and
everything was skipped as "."/"..".  Fixed the struct; also now read each thread's
/proc/self/task/<tid>/stat state (R running/S sleeping/D) so we can tell a CPU
spin (R, no syscall) from a futex/cond block (S).

The boot still hangs inside the first nativeRender (byte-identical log) after
GetMethodID(setMessage) - the dialog methods are resolved but never called, i.e.
Unity is blocked in native code below JNI.  0.21's dump will finally show whether
the main thread is spinning or futex-waiting, and what worker threads exist.

### Prior: 0.20.0 - watchdog thread dump (native-block diagnosis)

CRITICAL INSIGHT from 0.19.0's log: it is **byte-identical to 0.17/0.18** despite
the varargs fix + runOnUiThread dispatch + dialog auto-fire all being real and in
the binary.  And decisively: after `GetMethodID(setMessage)` there is **NO
CallObjectMethod(setTitle)/setMessage and NO ALERTDIALOG line** - Unity resolves
the dialog method IDs but NEVER CALLS them.  It is blocked in **native code below
JNI** (a pthread/cond/futex wait) before the dialog build.  That is why every JNI
shim leaves the log frozen at the same byte.

0.20 adds a watchdog thread: 12s after the nativeRender loop starts it dumps each
thread's /proc/self/task/<tid>/syscall + wchan, showing the EXACT syscall each
thread is blocked in (futex/cond_wait/etc).  Next deploy will reveal the real
block point (likely a worker-thread or job-system wait, per Terraria's documented
GfxDeviceWorker/job-system deadlock) instead of guessing.

### Prior: 0.19.0 - FIXED: CallObjectMethod dropped all varargs

A second model's code review caught REAL, critical bugs we had all missed:

**BUG 1 (CRITICAL): kv_CallObj always passed ap=NULL.**  The variadic
CallObjectMethod (kv_CallObj, the path Unity uses for nearly every call) did
`return kv_CallObjectMethodV(env,o,m,0)` - so EVERY method argument was dropped:
- AssetManager.open(path) got no path -> boot.config was never read.
- setMessage/setTitle capture read ((void**)ap)[0] on NULL -> 0.18's diagnostic
  printed empty strings (dead code).
Fix: kv_CallObj now does va_start/va_arg and forwards (void*)&ap; kv_CallObjectMethodV
collects up to 8 args via va_arg.

**BUG 2: runOnUiThread / Handler.post dropped the Runnable.**  Unity posts work
to the Android UI thread (which doesn't exist) -> blocks forever.  Fix:
kv_CallVoid dispatches runOnUiThread's Runnable inline; kv_CallBool dispatches
post/postDelayed's Runnable and returns true.

**BUG 3: AlertDialog can't be shown -> deadlock.**  Fix: capture the
setPositiveButton listener and auto-fire listener.onClick(dialog, -1) when
show() is called, so Unity's boot doesn't block on a modal that has no Java UI.

**BUG 4: getPackageCodePath/getFilesDir returned ".".**  Now return the real
game dir (set via kv_set_game_dir(kv_abspath(argv0,".")) from real_main).

**BUG 5: KV_MAX_JSTR was 256 -> string aliasing during a long boot.**  Bumped to 4096.

The bionic TLS pad is CONFIRMED working (tp+0x28 lands in the pad).  These are
high-confidence fixes that address why Unity boots with an empty boot.config and
falls into its error dialog.  Next deploy should show boot.config being read and
the boot progressing past the dialog.

### Prior: 0.18.0 - capture the AlertDialog message text

0.17's playCoreApiMissing=true did NOT change the log - byte-identical, still the
AlertDialog after findLibrary.  So this is NOT the Play Asset Delivery dialog; it
is Unity's GENERIC error dialog (shown via runOnUiThread on an uncaught error
during startup), which appears right after findLibrary.  The dialog text is the
decisive diagnostic: it tells us exactly what Unity thinks failed.

0.18 adds capture of the dialog text: in kv_CallObjectMethodV, when Unity calls
AlertDialog.Builder.setMessage/setTitle, resolve arg[0] (a jstring) and print:
  [jni] ALERTDIALOG setMessage: "<text>"
Next deploy will reveal the actual error message, pinpointing the root cause.

### Prior: 0.17.0 - Play Asset Delivery bypass (playCoreApiMissing=true)

0.16's JNI trace confirmed the trigger sequence:
`GetMethodID(playCoreApiMissing, ()Z)` then `findLibrary` CallObjectMethod, then
immediately `FindClass(android/content/DialogInterface$OnClickListener)` +
`AlertDialog$Builder` -> setTitle/setMessage -> hang.

Diagnosis (from a second model, high confidence): `playCoreApiMissing()` returning
FALSE (the shim's default) tells Unity the Play Core API is PRESENT, so Unity takes
the **Play Asset Delivery** path (init PlayAssetDeliveryUnityWrapper, query
getAssetPackState/getObbDirs).  With no real Play Core behind it, Unity falls
through to an AlertDialog and blocks forever on a button the shim never fires.

Fix (0.17.0) in jni_shim.c:
- kv_CallBool: `playCoreApiMissing -> return 1` (Play Core is MISSING) so Unity
  uses the filesystem fallback, where our already-extracted data/ lives.
- kv_CallObjectMethodV: return real jstrings for getPackageName
  (net.kairosoft.android.gamedev3en), getPackageCodePath ("."), findLibrary (".")
  instead of the raw 0x6000 fake-object pointer (which GetStringUTFChars would
  treat as a string handle and dereference badly).

Next on-device: with Play Core reported missing, Unity should skip the PAD dialog
and proceed to the render loop (real egl calls + frames).

### Prior: 0.16.0 - trace the dialog flow

0.13/0.14/0.15 (statfs, fs-redirect, cmdline-inject) each left the log byte-identical:
Unity builds an AlertDialog inside the first nativeRender and never reaches GL (zero
EGL calls).  See the CURRENT BLOCKER section at top for the full picture and the
unconfirmed app-install/PlayAssetDelivery hypothesis.  0.16 adds JNI Call* tracing so
the next log shows exactly what Unity does after setMessage and what check triggers the
dialog, enabling a surgical fix instead of guessing.

### Prior: 0.15.0 - inject -force-gfx-direct (single-threaded rendering)

0.14.0 fs-redirect didn't change the log (identical, still stuck at AlertDialog,
zero EGL calls).  So Unity's data reads are going through the JNI AssetManager
shim (already working), not open().  The REAL problem is likely the
GfxDeviceWorker: Unity renders on a separate worker thread that the Android
Java Activity drives.  Our loader only calls nativeRender on the main thread,
so the worker never runs -> main blocks waiting for it -> zero EGL calls ->
boot hang before the render loop.

Terraria fixes this by injecting -force-gfx-direct + -force-gles20 into
/proc/self/cmdline, forcing Unity to render on the main thread.  Added to
fs_redirect.c: kv_open/kv_fopen intercept ".../cmdline" and return a synthetic
file with those args.

Next on-device: expect the boot to reach real egl calls (MakeCurrent/SwapBuffers)
and nativeRender frame liveness.

### Prior: 0.14.0 - fs-redirect (Android data paths -> local data/dir)

0.13.0 statfs shim did NOT stop the AlertDialog.  The 0.12 diagnostics revealed the
real cause: Unity NEVER calls our EGL shim (no eglInitialize/MakeCurrent/SwapBuffers)
and nativeRender's first call never returns - it's stuck building the dialog before
reaching GL.

Root cause (via Terraria's my_open/asset_redirect): libunity.so imports open/fopen/
stat/lstat/access (confirmed by nm) and reads its data files (unity_app_guid,
globalgamemanagers, level0, global-metadata.dat, sharedassets*) via DIRECT syscalls
using Android paths (assets/bin/Data/...).  Our extracted data is at data/, so those
opens fail -> Unity thinks unity_app_guid is empty -> tries to re-extract from the
missing APK -> storage dialog -> boot hang.

Fix (0.14.0): new loader/fs_redirect.c intercepts open/open64/fopen/stat/lstat/access
and rewrites Android data paths onto <asset_dir>/<rel> (same as terraria).  Wired
into resolve() and kv_fs_set_data_dir() called in real_main.

Next on-device: with data files findable, Unity's extraction check should see the
guid/resources are present, skip the dialog, and proceed to the render loop.

### Prior: 0.13.0 - statfs shim (Not enough storage dialog) + stale-kill fix

Confirmed the user's modal theory: GDS's libunity.so contains the string
"Not enough storage space to install required resources." (vaddr 0xbdbe5),
and the log ends right as Unity builds an AlertDialog$Builder/setTitle/setMessage
- Unity's storage check fails, it pops the dialog, and blocks the boot (black
screen; dialog never renders since our JNI shim has no real Java UI).

Root cause found via RE: libunity.so imports `statfs@LIBC`. In the glibc build
it resolves to real glibc statfs via dlsym, which returns -1 on an Android path
Unity probes -> Unity concludes "not enough storage" -> dialog.

Fix (0.13.0):
- loader/bionic_bridge.c: kv_statfs() always reports abundant free space
  (1 TiB, f_bavail*f_bsize huge) and routes "statfs"/"statfs64" imports to it.
- launcher: fixed the stale-kill self-kill bug.  The old version killed stale
  loader2 BEFORE launching, and the fresh loader reused the freed PID and got
  killed (exit 137).  Now it launches first (captures $LOADER_PID), then kills
  only OTHER loader2 instances, so the fresh loader is never killed.

Also added earlier: EGL per-call counters + nativeRender frame liveness (0.12.0)
to see if Unity reaches eglSwapBuffers/present.

On-device next: with statfs returning free space, Unity's storage check should
pass, no dialog, boot should continue into the actual game render.

### Prior: 0.10.0 - point SDL at the Mali EGL driver

0.9.0 confirmed the game boots fully into the player loop and SDL_Init(VIDEO)
works (KMSDRM).  Only remaining blocker: SDL_CreateWindow still couldn't load
EGL/GL -> no context -> no frames.

Device library inspection found the root cause: on this ArkOS device
`libEGL.so -> libMali.so` (Mali G31 gbm driver) but `libEGL.so.1 ->
libEGL.so.1.1.0` (a standalone non-Mali EGL).  SDL's KMSDRM backend dlopens
`libEGL.so.1` by default -> wrong/stub EGL -> "Can't load EGL/GL library."

Fix (0.10.0):
- launcher sets `SDL_VIDEO_EGL_DRIVER=libEGL.so` and `SDL_VIDEO_GL_DRIVER=libGLESv2.so`
  so SDL loads the Mali driver, and
- kv_egl_dlopen dlopens `libmali.so`/`libEGL.so` (Mali) RTLD_GLOBAL (was loading
  the standalone libEGL.so.1).

Expected next log: `[egl] SDL video driver = KMSDRM`, `[egl] GL_VENDOR=ARM ...`
`GL_RENDERER=... Mali-G31 ...`, `window ... context ready (ES2)`, then real frames.

### Prior: 0.9.0 - game boots player loop; GL context missing -> fix rendering

0.8.0's bionic bridge + TLS guard pad WORKED on-device:
- libil2cpp init_array ran (24 ctors) — no more "malloc(): invalid size"
- libunity init_array ran (426 ctors)
- all 3 JNI_OnLoads fired, initJni OK
- **`nativeRecreateGfxState OK`** (the old crash site!), then `nativeRender loop...`
- the game booted deep into Unity (hundreds of JNI FindClass/GetMethodID calls
  building the scene).  So it runs logic now, but was NOT rendering:
  `[egl] SDL video driver = (null)` + `Can't load EGL/GL library` on every
  SDL_CreateWindow -> no GL context.

Three fixes for rendering (0.9.0):
1. **SDL_Init(SDL_INIT_VIDEO) before SDL_CreateWindow** in egl_shim.c.  The first
   version resolved SDL_Init but never CALLED it, so SDL had no video driver and
   couldn't load the EGL/GL library at window creation.
2. **LD_LIBRARY_PATH in the launcher** (`Game Dev Story.sh`) — same firmware lib
   dirs Terraria's launcher exports, so dlopen finds libSDL2/libEGL/libGLESv2/
   libz on ArkOS.
3. Resolved the 4 previously-unresolved libunity symbols: `inflate`/`inflateInit2_`/
   `inflateEnd` (now that kv_egl_dlopen dlopens libz RTLD_GLOBAL) and
   `__FD_ISSET_chk`/`__FD_CLR_chk` (added to glibc_shims.c).

Next on-device test: expect `[egl] SDL video driver = kmsdrm` (or similar),
`[egl] GL_VENDOR/RENDERER/VERSION` (Mali live), `window ... context ready (ES2)`,
and then the game should actually present frames.

### Prior: 0.8.0 - bionic bridge (pthread/signal ABI) + TLS guard pad

After 0.7.0 reached graphics init, it died with `malloc(): invalid size
(unsorted)` during libil2cpp's init_array.  Verified the relocation phase is
clean (host x86 probe on the real libil2cpp.so: 197253 relas + 538 jmprels,
malloc OK after) - so the corruption is a **bionic/glibc ABI mismatch** in the
sync/signal layer that a ctor triggers.

Two fixes added (both modeled on terraria-nextos):
- `loader/bionic_bridge.c` - routes the .so's pthread/semaphore/signal imports
  to bionic->glibc shims.  The critical one: **sigset_t is 8 bytes in arm64
  bionic but 128 in glibc**, so libil2cpp calling glibc's
  sigemptyset/sigaction/pthread_sigmask on an 8-byte buffer wrote 128 bytes ->
  heap corruption.  Bridge translates 8B<->128B sigsets, and pthread mutex/cond/
  rwlock/sem use the "slot" trick (store a pointer to a real glibc object in the
  bionic object's first word) so glibc never overruns bionic-sized objects.
- **bionic TLS guard pad** - libil2cpp reads bionic thread-info slots off
  tpidr_el0 (stack guard @+0x28, stack lo/hi @+0x30/+0x38).  We can't msr
  tpidr_el0 (broke glibc -> pc=0).  Instead a 256-byte TLS variable
  (kv_bionic_pad, verified at TLS offset 0, size 0x100) lands right after the
  glibc TCB at tpidr_el0+0x28, and kv_setup_tls pre-fills the bionic slots
  inside it - same trick as terraria's g_bionic_guard_pad.
- Also resolved the 2 previously-unresolved symbols (pthread_atfork,
  android_set_abort_message) with stubs.

Verified in-binary: kv_sigemptyset/kv_sigaction/kv_pthread_sigmask/
kv_pthread_mutex_lock/kv_sem_wait/kv_bionic_route/android_set_abort_message/
kv_pthread_atfork all exported; TLS pad at offset 0 size 256.  Needs on-device
deploy to confirm init_array now survives.

### Prior: 0.7.0 - egl_shim (SDL-backed GLES2) added

The freestanding loader (0.5.0) crashed at Unity's `nativeRecreateGfxState`
because it stubbed EGL/GL and never created a real GL context.  After studying
terraria-nextos (the proven loader for a full Unity game on this exact R36S),
we resolved the "copy terraria?" debate **in favour of copying its approach**:
a **glibc+SDL2 loader** that dlopens the real Mali GPU drivers and creates a
real SDL2 window + GLES2 context, then routes the engine's `egl*`/`ANativeWindow*`
calls to that real context.

What terraria does that the old loader did not (this is the whole fix):
1. **glibc-linked PIE, `-rdynamic`**, linking only glibc+libdl (SDL2 is reached
   at runtime via dlopen/dlsym — so the zig build needs no SDL2 headers).
2. **dlopen real GPU drivers RTLD_GLOBAL** (`libEGL`/`libGLESv2`/`libSDL2`) so
   Unity's `eglGetProcAddress` returns real Mali function pointers.
3. **`egl_shim_create_window()` + `egl_shim_ensure_current()` BEFORE
   `nativeRecreateGfxState`** — creates the SDL window + share-root GLES2
   context and makes it current on the game thread.
4. Routes libunity's **21 `egl*` + 6 `ANativeWindow*` + `ALooper*` + `ASensor*`
   imports** (verified with `nm -D` on the committed libunity.so) to the shim,
   not to dlsym.  Unity resolves GL entry points via `eglGetProcAddress`, which
   returns `SDL_GL_GetProcAddress` pointers into the real driver.

Implemented in the glibc build (`loader/loader2_glibc`):
- `loader/egl_shim.c` / `egl_shim.h` — SDL-backed EGL/GLES2 shim (share-root
  context model; each fake EGL context owns a real SDL GL context; MakeCurrent
  binds it, SwapBuffers presents it), ANativeWindow shims reporting the real
  screen size, and ALooper/ASensor no-op stubs.  SDL2 resolved via dlopen/dlsym.
- `loader_glibc_main.c` — `resolve()` consults `kv_egl_route()` first; `kv_unity_boot()`
  calls `egl_shim_create_window()` + `egl_shim_ensure_current()` before graphics init.
- `gds_deploy.sh` version check now keeps the `-glibc` suffix (was stripping it,
  so it always aborted on a correct zip).

Next on-device test (needs deploy; bench has no GPU/SDL so this can't run headless):
```
curl -sL -o gds_deploy.sh https://github.com/jackomix/asdf/raw/arena/019fd2ec-asdf/GDS_Unity/tools/gds_deploy.sh && chmod +x gds_deploy.sh && ./gds_deploy.sh ark@192.168.18.20
```
Expect `[loader] build: 0.7.0-glibc`, then `[egl] GL_VENDOR/RENDERER/VERSION`
proving the Mali driver is behind the context, then graphics init to proceed.

> **GOTCHA (fixed in 0.7.0):** the glibc build must NOT call `msr tpidr_el0` to
> set up a fake bionic TLS block.  That hack belonged to the freestanding
> loader.  Now that we link glibc, clobbering tpidr_el0 destroys glibc's own
> TLS (errno/stdio/locale/stack-protector), so the next glibc call jumps to
> `pc=0x0` before the banner even prints.  We keep glibc's TLS.  If libunity's
> init_array later needs the bionic thread-info slots (thread id @+0x08, stack
> guard @+0x28, stack lo/hi @+0x30/+0x38), replicate terraria-nextos's TLS
> guard-pad layout instead of overwriting the register.

### Deploy (works, version-checked)
    curl -sL -o gds_deploy.sh https://github.com/jackomix/asdf/raw/arena/019fd2ec-asdf/GDS_Unity/tools/gds_deploy.sh && chmod +x gds_deploy.sh && ./gds_deploy.sh ark@192.168.18.20
Expect `[loader] build: 0.37.0-glibc` in the log (stale-zip guard aborts otherwise).

## 0.37 — maps_resolve fixed, deadlock root cause analysed (NOT YET RESOLVED)

### What was wrong with maps_resolve
Pre-rewrite parser counted space chars between fields to find the pathname.
`/proc/self/maps` lines pad the gap between inode and pathname with MANY spaces
(e.g. `... 0 0           /usr/bin/cat`), so the `sp==5` cutoff landed inside
the padding.  The trailing `while (*q==' ') q++` then advanced `q` past the
pathname into the next whitespace, and the very last line in maps (`[stack]`)
had no trailing newline so its `lo/hi` matched anything via garbage parse.
Result: every thread resolved to `[stack] +0x8ef6c`.

### Fix
Rewrote `maps_resolve()` in `loader_glibc_main.c` (≈line 74) as a 5-token
tokenizer: split each line on single spaces, NUL-terminate each token, then
skip residual whitespace to reach the pathname.  Trees robust against any
kernel padding.  Audit confirmed all Now resolved correctly:

    [wd-resolve] hit lo=7fa0070000 hi=7fa020c000 path=/usr/lib/aarch64-linux-gnu/libc.so.6 off=8ef6c

### Worker thread park sites
ALL worker threads (incl. main) sit at `libc.so.6 + 0x8ef6c` which disassembles to:

    8ef40: bti c
    ...
    8ef68: svc #0x0            ;; raw futex syscall
    8ef6c: ret                  ;; return trampoline after svc

So workers are INSIDE libc's raw `__syscall` return trampoline.  This is the
primitive used by libc's `sem_wait` slow path (jumps via `8edbc → 8ed40 → _dl_mcount+0x1c0`)
and by libc's `syscall()` wrapper body (start `0xebb00`, PC `0xebb28 = +0x28`).

The libc offset `0x8ef6c` lies between `sem_wait@@GLIBC_2.34 @0x8ee60` (size 0x60) and
`sem_trywait@@GLIBC_2.34 @0x8eec0`.  It is the raw `__syscall` PLT-like stub used by
libc's `sem_wait` slow path internally.

### GOT audit confirms routing IS applied
Added a post-load GOT audit in `loader_glibc_main.c` (`Stage 1.5`) which reads
every GLOB_DAT/JUMP_SLOT reloc for libil2cpp + libunity + libmain and prints
the GOT slot value for the key symbols:

    [audit] ./libil2cpp.so GOT[sysconf]            = 0x1023da0   (kv_sysconf)
    [audit] ./libil2cpp.so GOT[pthread_create]     = 0x1023994   (kv_pthread_create)
    [audit] ./libil2cpp.so GOT[sem_post]          = 0x1023478   (kv_sem_post)
    [audit] ./libil2cpp.so GOT[sem_wait]          = 0x1023550   (kv_sem_wait)
    [audit] ./libil2cpp.so GOT[syscall]           = 0x1023f6c   (kv_syscall)
    [audit] ./libunity.so  GOT[pthread_create]     = 0x1023994   (kv_pthread_create)
    [audit] ./libunity.so  GOT[syscall]           = 0x1023f6c   (kv_syscall)
    [audit] ./libunity.so  GOT[sysconf]           = 0x1023da0   (kv_sysconf)
    [audit] ./libunity.so  GOT[sched_getaffinity] = 0x1023d10   (kv_sched_getaffinity)
    [audit] ./libunity.so  GOT[sem_wait]          = 0x1023550   (kv_sem_wait)

ALL six key symbols in BOTH .so's are routed to `kv_*` shims, NOT to libc.
So route table is correct and GOT is patched.

### Mystery: shims ARE routed but never fire
- `[kv_pthread_create] n=1` logged ONLY ONCE.  Worker count = ~14 spawn.
- `[kv_sysconf] routed` ZERO hits.  (Unity never calls sysconf via GOT.)
- `[kv_sem_wait] routed` ZERO hits.  (Workers' park is NOT sem_wait via GOT.)
- `[kv_syscall] routed` printed ONCE (`n=178` = our own gettid).  Then `[kv_syscall] SYS_futex tid=... op=0 a4=...` ONLY fires for 2 different tids (main and one worker).  Other workers never go through kv_syscall.

This means: GOT IS patched correctly, BUT the workloads that actually run
inside the spawned worker threads do NOT call those imported symbols via
the patched GOT slot.  The workers park in libc `__syscall` directly.

### Most plausible explanation
- libil2cpp calls `pthread_create` ONCE via GOT → routed to `kv_pthread_create` → spawns ONE thread (start=`0x202737174` libil2cpp).
- That single thread's start function internally spawns the remaining ~13 worker threads via libc- internal `pthread_create` (NOT through any patched GOT) OR via raw `clone` syscall (but `kv_syscall` clone trace showed NOTHING — no SYS_clone / SYS_clone3).
- Workers then call `sem_wait` / `pthread_cond_wait` via libc-INTERNAL code paths (e.g. static asm wrappers inside libil2cpp that bypass its own GOT) — those calls land at libc `__syscall` trampoline directly without ever touching `kv_sem_wait`.

This is why all our route-table work doesn't help with the deadlock: the
sleeping workers never call our wrapped `sem_wait`.  They sit in libc's
internal `__syscall` after a `futex(FUTEX_WAIT_BITSET, op=0x189, a4=0)`,
which means **infinite wait with no timeout injected** — our `kv_syscall`
2ms timeout hack only catches the FIRST futex on the main thread, not the
worker threads (which never traverse the kv_syscall GOT slot).

### Open question to investigate tomorrow
- WHERE do the worker threads come from if not the patched `pthread_create`
  and not `clone`/`clone3` (already traced, both empty)?
  Candidate runs to investigate:
    * `pthread_create` from libc internals (sigevent/timer thread pool)
    * libil2cpp static asm trampoline that calls libc `__pthread_create@@GLIBC_2.34`
      via its OWN absolute address (resolved at libil2cpp build time as if it
      linked libpthread statically).  Verify by dumping `pthread_create` calls
      in libil2cpp disasm — does it BLR through GOT slot or through ABS branch?
    * `bionic.bridge` static-link pickup of `pthread_create` (Unity shipped
      with embedded libpthread code that bypasses dynamic PLT entirely).  Look
      for `pthread_create` symbol DEFINED in libil2cpp/libunity (not UND).

- Confirm which libc fn maps to worker PC `0x8ef6c`.  Most likely:
  `sem_wait@@GLIBC_2.34` slow path → libc-internal `__syscall` stub.
  But workers could just as well be in `pthread_cond_wait` slow path (also
  calls same `__syscall` trampoline).  Both end at PC `0x8ef6c`.  Distinguish
  by stack walk if possible (or by arg inspection — futex addr in
  `0x7fbfd8...` range = worker stack region for `sem_t` vs `pthread_cond_t`).

### Next moves (resume here)
1. Read libunity.so disasm of `pthread_create` call site — does it use the PLT
   (BLR via GOT slot) or a direct branch to an internal symbol?  If the latter,
   the IL2CPP worker-pool code likely has its own statically-linked thread
   spawn that never touches the GOT — we need a different intercept strategy
   (e.g. GOT-patch the `sem_wait` import to `sem_timedwait` redirect isn't
   enough; we'd need to patch libc's `sem_wait` itself in libc.so.6 text via
   mprotect PROT_WRITE + first-instruction-redirect).
2. Check if libil2cpp defines `pthread_create`/`sem_wait`/`syscall` as
   NON-UND symbols (statically linked copy).  `readelf -s libil2cpp.so`
   full (not just dynamic).
3. Alternative path: instead of intercepting libc internals, just SET
   `JobsUtility.JobWorkerCount=0` from a SEPARATE thread spawned before
   nativeRender is called.  Spawn a "fixer" thread that bootstraps after
   il2cpp_init (wait on a flag set by first nativeRender entry) then runs
   `il2cpp_class_from_name` + `il2cpp_runtime_invoke(set_JobWorkerCount,0)`.
   Ref: terraria-nextos/src/main.c:431 `ter_jobworkers0()` — called from
   `ter_before_present()` (an `eglSwapBuffers` hook), but our equivalent
   can be the "fixer" thread instead, since the main thread deadlocks
   BEFORE the first `eglSwapBuffers` returns.
4. Clean up the debug printf spam once root cause found — keep GOT audit,
   drop kv_pthread_create tid trace, drop kv_syscall clone/openat traces,
   drop maps_resolve log line (keep the function, just remove `[wd-resolve] hit`).

### Files touched this session
- `loader/loader_glibc_main.c`:
    - Rewrote `maps_resolve()` (stream-based 5-token parser).
    - Added Stage 1.5 GOT audit printing slot values for 6 key syms.
    - Removed `kv_set_job_workers_zero` early-trigger; moved to gated-after-render
      (currently cannot fire because main never returns from first render).
- `loader/bionic_bridge.c`:
    - Added `[kv_syscall] routed!` once-print, `SYS_clone`/`SYS_clone3`/`SYS_openat` traces.
    - `[kv_pthread_create]` now logs tid, up to 30 hits.
    - `[kv_sem_wait] routed!` once-print (never fires; route confirmed via audit).
    - `[kv_sysconf] routed!` once-print (never fires; route confirmed via audit).
- `loader/fs_redirect.c`:
    - Added `[fs] open(...)` trace for cpu-related paths (cpuinfo, /sys/.../cpu, etc.)

### Stable fact: the GOT route table IS fully applied — shims exist but are
### NOT the path the deadlocked threads take.  Tomorrow: figure out which path.

## 0.38 — fixer thread approach (PARTIALLY DEPLOYED, WIFI DROPPED)

### Hypothesis from 0.37 audit
The 14 parked worker threads are likely **libmali GPU worker threads**, NOT
Unity job workers.  libmali.so (`/usr/lib/aarch64-linux-gnu/libmali-bifrost-g31-rxp0-gbm.so`)
imports `pthread_create`/`sem_wait`/`pthread_cond_wait`, but its GOT is resolved
by glibc's dynamic linker to libc directly — our loader only patches GOTs of
libil2cpp/libunity/libmain.  So libmali hardware threads all sit on libc's
raw `__syscall @0x8ef6c` (parked in `sem_wait` waiting for work that never
comes because no GL draw was issued).  These threads are parked but **NOT the
cause of main thread blocking**.

The main thread actually deadlocks INSIDE first `nativeRender` waiting on
a Unity job-system futex that never gets woken (Android looper absent).
Our `kv_syscall` injects a 2ms poll timeout — but Unity's predicate stays
false so main re-waits forever.

### Approach:  fixer thread
Instead of trying to fix the workers, run `JobsUtility.set_JobWorkerCount(0)`
from a SEPARATE thread spawned BEFORE the render loop:

1. Spawn `pthread_create(kv_pthread_t *fixer, fn=kv_set_job_workers_zero)`.
2. fixer calls `il2cpp_init()` from a sibling thread — this synchronises
   against Unity's in-flight init on main (it's idempotent + guarded).
   Without this gate, calling `il2cpp_class_from_name` too early races
   with the in-flight init and crashes at `mono_class_get_checked +0x17b3c`
   (`pc=0x200cfccd4`, FAR=0x135, x0=0) — same crash as 0.32.
3. After il2cpp_init returns, scan assemblies for JobsUtility + invoke
   the three setters (`set_JobWorkerCount`, `SetJobQueueMaximumActiveThreadCount`,
   `SetJobQueueMaximumWarpThreadCount`).
4. Result: Unity job worker count set to 0 → Unity runs jobs inline on
   whichever thread dispatches them → no workers needed → no deadlock.

### State of 0.38.1 code (committed)
- `kv_set_job_workers_zero(void *unused)` now a thread fn — poll il2cpp_init
  then scan assemblies.
- Spawn point inside `kv_unity_boot` right after `kv_start_watchdog()` +
  BEFORE the render loop.  `pthread_detach` after create.
- Removed inline `kv_set_job_workers_zero` call from render loop.

### Deploy status (PARTIAL - WIFI DROPPED IN MIDDLE)
0.38.0 (initial impl) deploys: SIGSEGV @ `0xcfccd4` (NULL mono class race).
  Cause: domain non-NULL before assemblies loaded; il2cpp_class_from_name
  too early. Same exact crash as 0.32 (`x0=0 x1=0 FAR=0x135`).
0.38.1 (with il2cpp_init gate) built locally (`strings loader2_glibc`
  shows `0.38.1-glibc`) — BUT the deploy script pulled a STALE github-raw
  cached zip (showed `0.38.0`) because the git push happened too fast for
  GitHub's CDN cache to invalidate.  The version-bump commit `876d9ea`
  hadn't propagated when `gds_deploy.sh` curled `raw.githubusercontent`.
  Need to wait ≥30s after push before curl, OR upload zip directly.

Then WiFi dropped (long-running loader never returned, watchdog swap
caused heavy wifi traffic).  User needs to reboot R36S to recover WiFi.

### NEXT MOVE (resume here)
1. Wait for user to reboot R36S to recover WiFi.
2. After R36S back, re-deploy ensuring 0.38.1 zip is the one that uploads.
   The committed binary in `/gamedevstory.zip` IS 0.38.1 — just git/CDN lag.
3. If 0.38.1 STILL crashes at `pc=0xcfccd4` (mono_class_get_checked), it
   means `il2cpp_init` from the sibling thread returned before
   `il2cpp_class_from_name` is callable.  Need stricter gate:
     a. Look into the il2cpp STARTUP SEQUENCE: maybe `il2cpp_init` returns
        before assemblies are loaded?  Probably needs
        `il2cpp_init("Domain")` domain-complete event gate.
     b. Or use `il2cpp_thread_attach(NULL)` BEFORE looking up class —
        required by IL2CPP for class-lookup to actually have a thread
        context (otherwise internal mono_class_get_checked sees NULL
        thread-context and dereferences garbage).  terraria-nextos'
        `ter_jobworkers0` calls il2cpp_thread_attach first; that may be THE
        gate, not il2cpp_init.
   Try adding `il2cpp_thread_attach(il2cpp_domain_get())` BEFORE
   `cls_from_name` — this likely fixes the race.
4. If 0.38.1 actually deploys + runs WITHOUT crash but Unity still deadlocks
   (workers already spawned), need to ALSO add code to TEAR DOWN existing
   workers: `JobsUtility.JobWorkerCount` setter requires the new value to
   be applied — Unity probably shuts down workers asynchronously.  May need
   to also signal `JobWorker.Restart` or wait.

### Code notes
- `kv_set_job_workers_zero(void *)` in `loader_glibc_main.c` ~line 598.
- Spawn in `kv_unity_boot` ~line 700-13 (after kv_start_watchdog, before
  render loop).  Uses `kv_pthread_t fixer; pthread_create(&fixer, 0,
  (void *(*)(void *))kv_set_job_workers_zero, 0); pthread_detach(fixer);`.
- `il2cpp_init` exported by libil2cpp at 0xce3d34 (per readelf).
- The 0.38.0 crash log saved at `/tmp/loader_038a.log`; 0.38.1 NOT YET
  captured (WiFi died before fresh deploy).

### Stable fact update
- libmali.so spawns N worker threads via libc `pthread_create` (NOT routed
  through kv_pthread_create because libmali's GOT is patched by glibc
  dynamic linker, not our manual loader).  These threads park at
  `libc __syscall @0x8ef6c` waiting on libmali's own sem_wait (also libc-
  resolved).  They're harmless parkers.
- Unity's job workers (separate from libmali's) are spawned via libil2cpp's
  `pthread_create@LIBC` GOT slot, which IS routed to `kv_pthread_create`.
  But only ONE kv_pthread_create log appears, suggesting the code path that
  spawns the worker pool runs in the spawned thread itself (recursion)
  via libc's pthread_create (NOT via libil2cpp's GOT).  Needs disasm of
  `start=0x202737174` libil2cpp offset 0x2737174 to confirm.

## LATEST STATUS (build 0.42.0-glibc) - ROOT CAUSE: sysconf uses BIONIC constants (96/97)

The 0.40 log was the breakthrough: crash at mono_class_get_checked NULL on MAIN=1
right after kv_pthread_create spawns the JobWorker, with `[kv_sysconf] name=39`.

Studying horizonchase's my_sysconf revealed the ROOT CAUSE we'd been missing for
every 1-CPU attempt: **Unity calls sysconf() with BIONIC constants, not glibc's.**
Horizonchase documents (src/main.c my_sysconf):
- _SC_PAGE_SIZE/PAGESIZE (bionic) = 39/40
- **_SC_NPROCESSORS_CONF/ONLN (bionic) = 96/97**  <- THE CPU count
- _SC_PHYS_PAGES (bionic) = 98, _SC_AVPHYS_PAGES = 99

Our kv_sysconf was checking glibc's 83/84 and 0x61/0x62 - ALL WRONG.  So when
Unity called sysconf(96)/sysconf(97) (its CPU-count query), our shim passed
through to real glibc -> returned the real count (4) -> Unity spawned (4-1)=3
Job.Workers -> the worker bootstrap (libunity+0x508174) ran il2cpp class lookups
with no thread context -> crashed at mono_class_get_checked NULL on main.

Fix (0.42.0) in kv_sysconf: report 1 CPU for bionic 96/97 (plus keep 83/84/0x61/
0x62 as belt-and-suspenders).  Also removed the provably-wrong process-wide
sched_setaffinity pin (a host test proved it does NOT change hardware_concurrency
on glibc - that reads /sys/.../online, not affinity).  kv_pthread_create is
pass-through (0.41) so no worker wrapper crash.

If the fix works: Unity sees 1 CPU -> 0 job workers -> no worker spawn -> no
mono_class_get_checked crash -> jobs run inline -> boot proceeds to first frame.

### Prior: 0.40.0 - fire jobfix from the MAIN-THREAD futex wait

The 0.39.x line hit a wall: main thread deadlocks on a RAW futex (Unity job
system) via kv_syscall, and the jobfix was only hooked in kv_pthread_cond_wait
which main NEVER reaches.  So set_JobWorkerCount(0) never fired, Unity kept
dispatching jobs to workers that never run them, and main futex-waited forever.

Terraria fires its ter_jobworkers0 from the eglSwapBuffers hook (re-entrant into
the render path).  Our main never reaches eglSwapBuffers (it deadlocks first), so
the equivalent hook point is the MAIN-THREAD futex WAIT.  Fix (0.40.0):
- bionic_bridge.c kv_syscall: when the MAIN thread (tid==getpid) issues a
  FUTEX_WAIT/WAIT_BITSET and il2cpp_domain_get() is non-NULL, fire
  kv_set_job_workers_zero(0) ONCE (jf_tried guard, and kv_jobworkers_is_done()
  guard).  Setting JobWorkerCount=0 makes Unity run jobs INLINE, so the job
  completes, main's futex predicate becomes true, and main advances past the
  first nativeRender.  Matches terraria's eglSwapBuffers-hook re-entrancy.
- loader_glibc_main.c: expose kv_jobworkers_is_done() accessor for the static
  kv_jobworkers_done flag.
- JobWorkerCount=0 + the existing kv_syscall 2ms futex poll + kv_pthread_cond_wait
  poll together should finally break the job deadlock.

### Prior: 0.39.10 - Unity JobWorker spawn + il2cpp thread-attach (BLOCKED)

## 0.39.6 → 0.39.10 — Unity JobWorker spawn + il2cpp thread-attach (BLOCKED)

### Findings
- **Worker spawn site IDENTIFIED**: libunity `0x5c3490` ("JobWorker bootstrap"). Calls `pthread_create` at `0x5c3528` with `start = libunity+0x508174` (= 0x508174 in the stripped .text). Disasm of `0x508174` shows the worker bootstrap loads `arg` into x19, reads a global job-control struct via `.got+0x148`, then loops `0xf` times (CPU spin loop), then calls `bl 0x39a0f0` (= libunity init wrapper).
- **Crash signature** (0.39.7, with REAL spawn + kv_worker_wrapper logging): pc=`libil2cpp+0xcfccd4` (inside `mono_class_get_checked+0x17b3c`), lr=`libil2cpp+0xd11c24` (= `mono_class_get_checked+0x2ca70` wrapper). x0=NULL (MonoClass*). Crash tid = MAIN thread (kv_engine_abort/main=1 confirms).
- **Worker actually DOES start** — `[kv_worker] wrapper entered tid=6710 dispatching orig_start` prints before crash. Crash is on main, not worker. Means main calls `mono_class_get_checked` directly from inside `pthread_create@plt→kv_pthread_create`? NO — main only enters il2cpp class lookup somewhere AFTER pthread_create returns.
- **Vacuous main wait loop**: After pthread_create returns, main runs `pthread_mutex_lock; ldr w8,[x20+32]; cbz...,loop` then `kv_pthread_cond_wait` returns 0 spurious (dom_get still NULL → jobfix skipped, no print) → loop back. This does NOT call libil2cpp. So the crash must come from INSIDE `pthread_create` itself, somewhere in the post-spawn path in libil2cpp.
- **Wait — re-test with crash handler tid + main=1**: confirmed tid=6702 (main) AND main=1. So MAIN entered libil2cpp class lookup somewhere. Likely MAIN thread is the one actually RUNNING the worker bootstrap (single-threaded execution after pthread_create returned without spawning worker)? No — wrapper dispatched orig_start which crashes.
- **Definitive mapping (0.39.7 LOG)**:
  ```
  [kv_pthread_create] n=1 tid=8646 start=0x202737174 arg=0x7f5020f630
  [kv_pthread_create] Unity-internal worker spawn (start=... mod=libunity offset=0x508174) — wrapping with thread_attach
  [kv_worker] wrapper entered tid=8654 orig_start=0x202737174 arg=0x7f5020f630
  [kv_sysconf] routed! name=39 tid=8646
  [kv_worker] dispatching orig_start
  [loader] === CRASH sig=11 tid=8646 pthread_self=0x7f9d8a1440 main=1 ===
  ```
  - tid=8646 = MAIN (also logged in the kv_pthread_create line; both prints agree on tid).
  - tid=8654 = WORKER (kv_worker_wrapper ran on tid 8654).
  - Crash tid=8646 = MAIN. The crash is NOT in the worker. Main reaches `mono_class_get_checked+0x17b3c` (pc=0xcfccd4) without our `[jobfix]` traces printing — meaning the cond_wait spurious-return path is NOT what main is in when it crashes. Main is executing SOMETHING ELSE (presumably the JobWorker bootstrap aftermath) that immediately calls an il2cpp class lookup with NULL MonoClass*.
- **Hypothesis**: pthread_create substitutes a "registered" hook BEFORE running worker start. On glibc we drop this hook (Bionic registers a thread via `__libc_pthread_create_hook` or similar automatically). The hook wraps the worker start_routine with `il2cpp_thread_attach(domain)`. Without it, worker start runs without thread context, but we ALSO see the crash on main — so worker MAY signal main via global MonoClass* slot while its OWN frame goes through NULL lookup. Hard to say without diving deeper into 0x508174's full body.
- **0.39.8**: Tried `il2cpp_thread_attach(domain)` inside `kv_worker_wrapper` BEFORE calling orig_start. dom_get returned `0x7f91fb0fc0` (non-NULL — Unity's il2cpp_init has presumably completed by the time we hit pthread_create). Result: `mono_thread_attach(=il2cpp_thread_attach same impl) => Threads explicit registering is not previously enabled` → Unity engine ABORT (caller=libil2cpp 0xd46248).
- **0.39.9**: Same with `mono_thread_attach(0)` attempting fallback — `mono_thread_attach` redirects to identical body at 0xcd2380, same assertion. Aborts.
- **0.39.10**: Gave up on worker attach attempt; replaced with documentation. Worker is spawned WITHOUT attach — crashes at same mono_class_get_checked. JobFix from `kv_pthread_cond_wait` jobfix path is STILL blocked because we never see `[jobfix]` print (presumably main never enters cond_wait, OR main crashes before cond_wait).

### Next session paths
1. **Inspect libil2cpp's .init_array**: Unity il2cpp_init runs `mono_thread_init`/`mono_threads_install_registered_threads` to enable "Threads explicit registering". Without that init completing on worker thread context, attach fails. Try invoking every `.init_array` function explicitly before spawn.
2. **Dump worker bootstrap body 0x508174..0x509000 fully** — locate where MonoClass* lookup call is made and what class name string is passed. Likely class="Unity.Collections.JobWorker" or similar. The crash caller is from main, not worker, so this disasm only useful for understanding what MAIN was meant to wait for.
3. **Disable Worker spawn entirely (return 0xdeadbeef sentinel)** AND simultaneously BLOCK main from looping on cond_wait until dom_get is non-NULL AND `il2cpp_is_vm_thread()` returns true on main (signals registration enabled). Then call the jobfix. See if main eventually runs il2cpp_init internally without worker intervention. (Need `il2cpp_is_vm_thread` export check — `nm -D libil2cpp.so | grep is_vm_thread` shows `il2cpp_is_vm_thread@0xce47f8` jumps to 0xcd31a8).
4. **Bypass Unity's JobWorker bootstrap altogether**: NEVER let libunity enter `0x5c3490` (JobWorker spawn). This means intercepting before the call site or mprotect+patch the libunity call instruction to a NOP. Hard (we don't own libunity code).

### Build/deploy state
- HEAD of arena/019fd2ec-asdf: `0.39.10-glibc` (pushed but HANDOFF.md updates pending).
- See HANDOFF.md sections above (0.39.5) for the kv_pthread_cond_wait jobfix gate — still in code, only triggers when dom_get() non-NULL on MAIN.
- Crash handler updated to log tid + pthread_self + `kv_is_main_thread()` so the crashing thread is unambiguous (commit `0105bd9`).

## ADDENDUM (0.42.1, after 0.42 on-device)

0.42 deployed: SAME crash at mono_class_get_checked(NULL) on MAIN, right after
kv_pthread_create spawns the JobWorker (start=libunity+0x508174), pc=libil2cpp+
0xcfccd4 (ldrb [x0,#0x135] x0=NULL), lr=libil2cpp+0xd11c24, caller libunity+0x5c3550.

EXTENSIVE research + RE (not a new theory) established:
1. Horizon Chase uses Unity **2022.3.33f1** (SAME 2022.3 family as GDS 2022.3.62f2)
   and its loader works on this exact R36S.  Its boot is structurally IDENTICAL to
   ours: JNI_OnLoad -> initJni -> nativeRecreateGfxState x2 -> surfaceChanged ->
   resume -> focus -> nativeRender loop.
2. libunity reads "job-worker-count" and "Creating JobQueue using job-worker-count
   value %d" - a config value that sizes the worker pool.  If 0, no JobQueue
   workers are created, no JobWorker spawn, no mono_class_get_checked crash.
3. Horizonchase's fallback when job dispatch is broken is ter_inline_task: it
   FAKES the per-object job completion on main (sets node->next=1 + increments the
   global completion counter at g_unity_base+0xc10360) - but that offset is HC-specific.

Changes shipped in 0.42.1 (best-evidence, NOT device-proven):
- data/boot.config: added job-worker-count=0 + background-job-worker-count=0.
  If libunity reads these from boot.config (same config system as the working
  gfx-disable-mt-rendering=1), Unity creates 0 workers -> no JobWorker spawn ->
  no crash.
- loader/host_syms.c: fixed freestanding build (define `_ctype_` table so the
  Unicorn bench compiles) - enables headless iteration of the boot logic.

NEXT DEVICE TEST: deploy 0.42.1, read loader.log.  If the crash is GONE but the
game still doesn't render, we've confirmed job-worker-count=0 works and the next
blocker is elsewhere.  If the SAME mono_class_get_checked crash persists, then
libunity does NOT read job-worker-count from boot.config, and the fix must be a
targeted patch to libunity.so's JobWorker-spawn (libunity+0x5c3490) - either NOP
the pthread_create at 0x5c3528, or force the worker-count global to 0 via a direct
memory write (the reference ports' approach).

---

## 0.43.x — Link review + crash-site dissection (addendum, build 0.42.1-glibc era)

### Review of the 12 links the user provided (all examined; 4 cloned)
Genuinely useful (HIGH): **oceanhorn-nextos** and **hitmango-nextos** — both are the SAME
author and BOTH run a Unity 2022.3-family IL2CPP engine natively on THIS exact R36S/ArkOS
Mali-G31:
- **oceanhorn = Unity 2022.3.61f1** (straddles GDS's 62f2). README states it is PLAYABLE on
  "R36S / ArkOS (RK3326, Mali-G31), KMSDRM, ES3.0, 640×480". This is hard proof-by-example
  that the bionic→glibc so-loader + full Unity job system CAN work on this exact hardware.
- **hitmango = Unity 2022.3.67f2** (above GDS). Same architecture.
Both loaders already live in `/home/user/*-nextos` (same machine) — GDS's loader is derived
from them. Both default to NORMAL worker spawns (they do NOT force 1 CPU by default) and yet
do NOT crash — so spawning a Job.Worker is NOT inherently broken on this device. Their
`TER_JOBWORKERS0` (call `JobsUtility.set_JobWorkerCount(0)` via il2cpp_runtime_invoke) and
`TER_JOBINLINE`/`CUP_1CORE` (report 1 CPU) are OPT-IN fallbacks, fired from the swap-hook on
the FIRST PRESENT (`my_eglSwapBuffers` → `ter_shot_hook` → `ter_nuke_methods()+ter_jobworkers0()`).

Medium (worth a look if we need it): **Cpp2IL**, **cpp2il.com**, **Il2CppDumperLinux** —
decompile libil2cpp.so + global-metadata.dat → C#; would let us dump the EXACT managed class
being constructed (to see WHY it resolves NULL) and the exact `JobsUtility.set_JobWorkerCount`
signature. **frida-il2cpp-bridge** — runtime il2cpp hooking; concept useful but needs a JVM/Frida,
not applicable to our bare native loader.

Low / not applicable: kotamon-dev-cheese, MeowNet-recroom-Dump, operator-modding-toolkit,
kgc-private-server, SonolusReverse (all game-specific modding); theescapists (Box64 x86_64 — a
different paradigm; no x86_64 build of GDS exists, so N/A).

### Crash-site dissection (vaddr→file+0x4000 confirmed for both libs)
- libil2cpp+0xcfcccc is the real faulting fn: `ldrb w8,[x0,#0x135]` (reads a flag bit 1 at
  offset 0x135) with **x0=NULL**. lr=libil2cpp+0xd11c24 (a "checked class" helper that calls
  0xcfcccc). So a NULL *object/class* pointer reaches a managed-object/class accessor → the
  job path is constructing a managed object from a NULL class.
- libunity caller path: 0x5c3490 (JobWorker spawn) → `bl 0xeee5e0`(pthread_create) at 0x5c3528
  → spin on [x20+0x20] → `bl 0x5c35d0` → 0x5c395c jump-table dispatch → managed object
  construction → NULL class. NOTE: 0x5c3490 calls pthread_create in BOTH branches (w22==0 and
  w22!=0 converge at 0x5c3524), so forcing the count arg to 0 does NOT skip the spawn — only
  preventing 0x5c3490 from being CALLED does.

### NEW EVIDENCE (invalides the "report 1 CPU" approach for GDS)
The on-device log right before the crash shows Unity reading /proc/cpuinfo and
/sys/devices/system/cpu/{present,possible} MULTIPLE times (we inject 1 CPU in all of them),
plus sysconf(_SC_PAGESIZE=39). **A JobWorker STILL spawns.** So the GDS worker-spawn count is
NOT (cores−1) derived from cpuinfo/sysconf — the "report 1 core" trick that works for
Terraria/oceanhorn/hitmango does NOT transfer to GDS. That is WHY builds 0.23→0.42 all crashed
identically. The count is likely a player-setting (`job-worker-count` string @ libunity vaddr
0xb0dee) or a fixed value. boot.config `job-worker-count=0` did NOT stop it in 0.42.1 — but we
have NOT yet proven whether libunity even READS data/boot.config on device (bench showed the
loader's own read of `data/boot.config` resolves relative to cwd; on device cwd is
/roms/ports/gamedevstory so `data/boot.config` should be found — but it is a FOLDER named
`data`, i.e. the fs_redirect target is `data/boot.config` = the real file. UNCONFIRMED that
libunity parses it).

### Headless bench status
The Unicorn bench (run_aarch64.py) loads the real 3 .so, reaches initJni, then crashes in
nativeRecreateGfxState at libunity+0x5ccb38 = `strb 1,[thiz[0x148]]` with thiz[0x148]=NULL.
Pre-filling thiz[0x148] in loader.c did NOT survive — Unity re-zeroes that field in the
GPU-less path, so the bench diverges from the device at nativeRecreateGfxState and CANNOT be
pushed to repro the mono_class_get_checked crash. Bench = partial repro only (confirmed).

### Next experiments (pick one, device)
1. **Prove whether libunity reads/applies data/boot.config on device** (add a log to
   kv_fopen/kv_open when "boot.config" is opened; verify the file is parsed). If NOT read, fix
   the path so job-worker-count=0 actually reaches the config table — this is the cheapest test
   of the boot.config theory.
2. **Cpp2IL dump** of libil2cpp.so + global-metadata.dat to identify the NULL class being
   constructed in the job path (what assembly/class), to understand root cause.
3. **Match oceanhorn's boot exactly** (it works on this device): verify we call every lifecycle
   method it calls (nativeRestartActivityIndicator, nativeSendSurfaceChangedEvent order), and
   adopt its swap-hook `ter_nuke_methods()`/`set_JobWorkerCount(0)` timing if it can be fired
   BEFORE the first worker spawn (il2cpp_init completes early inside the first nativeRender).

---

## 0.43.1 — HEADLESS REPRO + metadata root-cause (addendum)

### BREAKTHROUGH: the exact device crash is now reproducible headlessly
Setting `il2cpp_set_data_dir` to the real port data tree in loader.c's boot (bench
build) made the Unicorn bench reach the SAME fault as the device: pc=libil2cpp+0xcfccd4,
FAR=0x135, x0=NULL, x20=0x202007000 — i.e. mono_class_get_checked(NULL).  The bench can
now iterate on the fix without the handheld.  run_aarch64.py gained a full-reg + FP-chain
backtrace dump and a hook that catches mono_class_get_checked entry.

### The crash mechanism (bench-verified)
- libil2cpp+0xcce854 (a cached-getter: allocates 0x38, caches @libil2cpp data 0x2007000+0x928)
  reads the class as `x8=[g_root+0x10]` where `g_root=[libil2cpp+0x1ef5000+0x418]`, then calls
  0xd11b0c which tail-calls 0xd11c14 -> 0xcfcccc (`ldrb w8,[x0,#0x135]`) with **x0=NULL**.
- So the NULL is a MonoClass* from a global that was never registered/initialized.

### ROOT-CAUSE HYPOTHESIS (strong, but UNCONFIRMED on device)
`global-metadata.dat` is **NEVER opened** during the entire bench boot (no openat for it),
and explicit `il2cpp_init()` returns 0 (failure) and hangs.  Without metadata, every class
resolves NULL -> mono_class_get_checked(NULL).  Two concrete bugs found:
  1. The player-loop path never called `il2cpp_set_data_dir` (fixed in 0.43.0).
  2. `fs_redirect` mapped bare `global-metadata.dat` to `<data_dir>/global-metadata.dat`
     (WRONG — the file lives at `<data_dir>/Managed/Metadata/global-metadata.dat`) —
     fixed in 0.43.1.
Caveat: the device reaches nativeRender's JobWorker spawn (deeper than the bench's
initJni-phase crash), which suggests the device's il2cpp is MORE initialized than the
bench's.  So on-device the metadata may already load, and the NULL class could instead be a
job-spawn-specific timing issue.  The 0.43.1 domain diagnostics
(`[il2cpp] domain_get before nativeRender / at nativeRender entry`) will disambiguate:
- domain != NULL  -> il2cpp IS up; metadata loads; the NULL class is a job-spawn issue.
- domain == NULL  -> il2cpp/init not complete; metadata/init is the root cause (0.43.1 targets this).

### What 0.43.1-glibc deploys (commit c564e57)
- kv_il_prepare_dirs(): il2cpp_set_data_dir("data")/set_config_dir/set_temp_dir/
  set_commandline_arguments BEFORE initJni (so Unity's internal il2cpp_init finds metadata).
- fs_redirect: bare `global-metadata.dat` -> `data/Managed/Metadata/global-metadata.dat`.
- nativeRestartActivityIndicator lifecycle call (oceanhorn parity).
- Domain diagnostics before/at nativeRender entry.

### If 0.43.1 does NOT fix the device
Next lever = the JobWorker-spawn NULL class (libunity+0x5c3490 -> 0x5c35d0 managed-object
construction).  The worker count is NOT CPU-count-driven in GDS (1-CPU injection everywhere
still spawned a worker), and 0x5c3490 calls pthread_create in BOTH branches (can't skip by
zeroing the count).  Options: (a) set_JobWorkerCount(0) via il2cpp as early as possible
(post-il2cpp-init, before the first job), or (b) identify the specific NULL class via Cpp2IL
dump and patch, or (c) match oceanhorn's exact boot/JNI surface.  The bench repro now makes
(a)/(b)/(c) testable headlessly.

---

## 0.43.3 — REGRESSION FOUND in 0.43.2 and reverted (on-device)

### The 0.43.2 regression (important diagnostic)
0.43.2's `kv_il_prepare_dirs()` called `il2cpp_set_data_dir/set_config_dir/set_temp_dir/
set_commandline_arguments/domain_get` BEFORE Unity's own il2cpp_init.  On-device the
crash MOVED to right after `[il2cpp] set_config_dir("data/etc")`, i.e. DURING those
pre-init calls, BEFORE initJni — same site (pc=libil2cpp+0xcfccd4, mono_class_get_checked
NULL).  Lesson: **any il2cpp_* call before Unity's il2cpp_init crashes in
mono_class_get_checked(NULL)** because the runtime's classes aren't registered yet.  This
also CONFIRMS the crash mechanism: it is the same NULL-class fault triggered whenever il2cpp
is touched before its classes are registered.

### 0.43.3 (pushed, commit 901ae7e)
- Removed `kv_il_prepare_dirs()` entirely (restores 0.42.1 boot timing so Unity's OWN
  il2cpp_init runs inside initJni/first nativeRender).
- KEPT the fs_redirect `global-metadata.dat -> <data_dir>/Managed/Metadata/global-metadata.dat`
  path fix (the real mechanism — applied when il2cpp opens the metadata during its own init).
- Added `[fs] open/fopen(global-metadata.dat '...')` diagnostics in kv_open/kv_fopen.
- Dropped the unsafe domain_get diagnostics (also pre-init risk).

### What the 0.43.3 log should tell us
Look for `[fs] open(global-metadata.dat '...')` (or fopen) lines during initJni/first
nativeRender:
- If it appears AND resolves to the correct data/Managed/Metadata path, metadata loads and
  classes should resolve -> the NULL-class crash should be GONE (metadata was the cause).
- If it appears but maps to a wrong/not-found path, fs_redirect still isn't serving it.
- If it NEVER appears, il2cpp isn't reaching its metadata open -> Unity's il2cpp_init isn't
  running/loading metadata -> different init problem.

---

## 0.43.4 — PROVEN: il2cpp never opens global-metadata.dat (on-device, 0.43.3)

### Hard evidence from 0.43.3 log
The 0.43.3 log (boot restored to 0.42.1 timing: initJni OK -> nativeRecreateGfxState OK ->
nativeRender loop -> JobWorker spawn -> crash at libil2cpp+0xcfccd4, NULL class) shows
**NO `[fs] open(global-metadata.dat ...)` line anywhere**.  So il2cpp's metadata open does
NOT go through our routed kv_open/kv_fopen.  Two possible causes:
  (a) il2cpp_init isn't running at all (metadata never requested), or
  (b) il2cpp reads the metadata via a path that bypasses our kv_open (raw syscall / a
      mechanism we don't route).

### Root cause framing (this is the REAL wall, not a metadata-path typo)
The NULL class in mono_class_get_checked is the symptom of il2cpp not having its classes
registered = metadata not loaded = il2cpp_init not completing in our loader.  The working
2022.3 reference (hitmango-nextos) triggers il2cpp init via the game's OWN boot path:
  hgo_load_modules() -> libmain JNI_OnLoad -> **NativeLoader.load(libdir)** -> run_unity().
Its loader has a **handle-aware dlopen bridge** so NativeLoader.load's dlopen of
libunity/libil2cpp returns the already-mapped modules, and libunity's init_array/JNI_OnLoad
(which is what registers il2cpp classes) runs in the Android-canonical order.  Our loader
skips NativeLoader.load entirely and drives initJni->nativeRender directly -> il2cpp_init
never completes.

### 0.43.4 (pushed, commit dfbbda1) — set_data_dir + routing audit
- Calls ONLY il2cpp_set_data_dir("data") before initJni (plain string store; the 0.43.2
  crash was from ALSO calling set_config/set_temp/set_cli/domain_get pre-init).  So Unity's
  own il2cpp_init should open data/Managed/Metadata/global-metadata.dat.
- Added open/fopen/mmap/stat/read to the GOT route audit -> next log shows whether il2cpp's
  file IO is actually routed to kv_* shims (the key unknown).
- Kept the [fs] open/fopen(global-metadata.dat ...) diagnostics.
- Added kv_dlopen (returns already-loaded game modules) as the groundwork for the
  reference-aligned NativeLoader.load boot step; NOT yet wired in.

### Next log interpretation
- `[il2cpp] set_data_dir("data")` + `[fs] open(global-metadata.dat 'data/Managed/...')`
  resolving correctly -> metadata loads, crash should be GONE.
- audit shows GOT[open]=kv ptr but no metadata open -> il2cpp_init not reaching the open.
- audit shows GOT[open]=libc (NOT routed) -> il2cpp's open bypasses our shims -> need raw
  syscall interception or the dlopen-bridge NativeLoader.load boot path.

### The fresh-start the user asked for (prepared, not yet wired)
Match hitmango's boot: after loading the 3 modules, call the game's `NativeLoader.load(libdir)`
(registered by libmain) with kv_dlopen returning the already-mapped modules, so libunity's
init_array/JNI_OnLoad runs in the Android-canonical order that registers il2cpp classes.
kv_dlopen is now in place; the call site + avoiding double-JNI_OnLoad is the next step.

---

## 0.50.0-rewrite — ROOT CAUSE FOUND (bench-verified): wrong il2cpp data_dir

### The actual bug (headless-proven in the Unicorn bench)
By hooking il2cpp_init in the bench and watching openat, we finally saw what il2cpp
actually opens:
  data_dir="data"       -> opens `<data_dir>/Metadata/global-metadata.dat`
                           = data/Metadata/global-metadata.dat  -> NOT FOUND
  data_dir="data/Managed" -> opens data/Managed/Metadata/global-metadata.dat
                           -> fd 103, 5477380 bytes (FOUND!)

**il2cpp's metadata path template is `<data_dir>/Metadata/global-metadata.dat`** (NO
"Managed/" prefix).  Our loader set data_dir="data", so il2cpp looked for
data/Metadata/global-metadata.dat and failed -> no metadata -> every class NULL ->
mono_class_get_checked(NULL) crash.  This is THE root cause, independent of whether
il2cpp_init is called by us or Unity: the path was simply wrong.

### The rewrite (0.50.0-rewrite)
1. **il2cpp_set_data_dir("data/Managed")** before initJni AND again before the explicit
   il2cpp_init.  (data_dir="data/Managed" makes il2cpp open the correct metadata path.)
2. **Explicit il2cpp_init("IL2CPP Root Domain")** on the main thread after initJni - the
   engine (libunity) was never calling it internally, so the runtime never initialised.
   il2cpp_init returns (does NOT hang); the bench's return-0 is a bench artifact (its mmap
   maps anonymous memory, not the file fd - on the device glibc mmap maps the real file).
3. Load order: libunity loaded first (init deferred), then libil2cpp, then libunity init,
   then libmain - with deferred re-resolution of libunity's il2cpp imports (oceanhorn order).
4. NativeLoader.load(libdir) boot step (hitmango reference) via a kv_dlopen bridge that
   returns already-mapped modules.
5. Kept fs_redirect global-metadata.dat -> Managed/Metadata/ fix + metadata-open logs.

### Caveat (honest)
data_dir="data/Managed" is bench-CONFIRMED to make il2cpp open the metadata.  The explicit
il2cpp_init + load-order + NativeLoader.load are reference-aligned but not headless-verifiable
(the bench crashes at nativeRecreateGfxState, GPU-less, and mmaps files as anonymous).  This
is the strongest, most evidence-based attempt: if il2cpp_init succeeds on device with the
correct data_dir, classes resolve and the job-spawn NULL-class crash is gone.

---

## 0.50.1-glibc — game now BOOTS past the crash (0.50.0 was the milestone)

### 0.50.0-rewrite result (on-device, log confirmed)
The `data_dir="data/Managed"` fix + explicit il2cpp_init WORKED:
- `[fs] open(global-metadata.dat 'data/Managed/Metadata/global-metadata.dat')` (metadata found)
- `[il2cpp] explicit il2cpp_init -> 1` (SUCCESS - first time)
- `[jobfix] assembly count=39` (all assemblies loaded, classes resolve)
- `[unity] nativeRecreateGfxState OK` -> `nativeRender loop...`
- **The mono_class_get_checked(NULL) crash is GONE** - it got past the job-worker spawn!

### The new blocker (a hang, not a crash)
After nativeRender, the process hangs (no exit 139, watchdog dumps repeat).  Main thread is
R (running), job workers futex-wait at glibc+0x8ef6c.  This is the classic job-system
deadlock the reference ports solve via 0 workers (jobs run inline):
- Reference's `set_JobWorkerCount(0)` does NOT exist in GDS's JobsUtility (metadata-verified:
  methods are only GetWorkStealingRange, ScheduleParallelFor, CreateJobReflectionData x2,
  InvokePanicFunction, ScheduleParallelFor_Injected).
- Reporting 1 CPU did not stop GDS's worker spawn (the worker start libunity+0x508174 was
  still created; 0x5c3490 always spawns one worker regardless of count).

### 0.50.1-glibc changes (pushed)
1. **Worker-TLS trampoline**: kv_pthread_create now wraps EVERY created thread (incl. the
   JobWorker at libunity+0x508174) with a trampoline that sets up that thread's bionic TLS
   slots (thread-id, stack-guard @tp+0x28, stack lo/hi @+0x30/+0x38) via kv_setup_worker_tls
   (silent copy of the main-thread kv_setup_tls).  The worker start reads these; without them
   workers get garbage TLS (documented difference vs oceanhorn's thr_trampoline).  No
   il2cpp_thread_attach (that asserts).
2. **jobfix no-op**: kv_set_job_workers_zero() now returns immediately (marks done) because
   GDS has no set_JobWorkerCount - the old body's 39-assembly re-entrant il2cpp reflection
   scan on the main thread during nativeRender was pure overhead + a re-entrant deadlock risk.

### Honest confidence
0.50.0 proved the metadata path + explicit il2cpp_init is THE fix that gets past the crash.
0.50.1 addresses the worker-TLS gap and removes the re-entrant jobfix scan.  It may or may
not fully resolve the post-boot job hang; if the hang persists, the next lever is forcing the
job-worker-count global to 0 (its source is behind a pointer table, hard to trace statically)
or patching the JobQueue to run jobs inline.

---

## 0.50.3-glibc — REFERENCE PORT: real cond/sem semantics + bionic log/sysprop

### SIGUSR1 confirmed the exact hang site
Main's PC dump showed backtrace `#1 lr=0x2005c3550` = libunity+0x5c3550, which is
the wait loop INSIDE the JobWorker-spawn function (0x5c3490) after pthread_create:
main spawns a worker and spins on [x20+0x20] (ready flag) while the worker futex-waits.

### Root cause (reference comparison)
Our main-thread `kv_pthread_cond_wait` returned 0 IMMEDIATELY **without releasing the
mutex**.  So main held the lock while spinning at 0x5c3550, the JobWorker could never
acquire it to set the ready flag, and the worker starved forever -> permanent hang.
The reference (hitmango b_cond_wait) does a REAL `pthread_cond_timedwait` with a short
ceiling for ALL threads, which releases the mutex during the wait so the worker proceeds.

### What 0.50.3 ports (matching hitmango/oceanhorn's foundational layer)
1. **kv_pthread_cond_wait** -> real 50ms `pthread_cond_timedwait` (hitmango b_cond_wait).
   Releases the mutex during wait; timeout = harmless spurious wakeup the while() re-checks.
2. **kv_pthread_cond_timedwait** -> honors the caller deadline (no main-thread short-circuit).
3. **kv_sem_wait** -> real blocking `sem_wait` retried on EINTR (hitmango b_sem_wait),
   instead of the old EAGAIN-on-timeout poll.
4. **Removed** the "jobfix" calls (set_JobWorkerCount(0)) from kv_syscall/cond paths —
   a no-op for GDS (no such setter) that did heavy re-entrant il2cpp reflection.
5. **Real bionic log + system-property shims** (ported from hitmango bionic.c): __android_log_*
   mirror to stderr; __system_property_* return a plausible Pixel-6 table (ro.opengles.version
   =196608, ro.build.version.sdk=31, etc) so Unity picks correct quality tiers instead of low-end
   fallbacks.

### Why not the rest of the reference yet
audio/input/gamepad/keyboard/render-scale are game-specific and not the blocker; porting them
before the game renders is premature. dl_iterate_phdr needs phdr storage in our Module and risks
the working boot — deferred.  The verified blocker (worker handshake) is addressed by the
cond/sem port.

---

## 0.60.0-ref — FULL REFERENCE PORT (hitmango-nextos architecture, wholesale)

### What this is
This is NOT another patch to the divergent 0.5x loader.  It is a complete,
wholesale port of the WORKING reference loader architecture (hitmango-nextos,
Unity 2022.3.67f2 — closest to GDS's 2022.3.62f2) adapted for GDS.  Source lives
in GDS_Unity/loader_ref/ (buildable via loader_ref/build.sh, zig glibc toolchain).

### Files ported verbatim (renamed hgo->gds, game-specific bits adapted)
- nx_elf.c/h   - the multi-module ARM64 ELF loader (mmap segments, DT_HASH/GNU_HASH
                  symbol walking, binary-search import resolution, TLS reloc skip).
- bionic.c     - the complete libc/liblog/libdl/libz surface (bionic ABI diffs:
                  __android_log_*, __system_property_* Pixel table, FORTIFY __*_chk,
                  __sF array, sigaction/sigset bridge, sysconf bionic constants,
                  handle-aware dlopen/dlsym + dl_iterate_phdr + dladdr).
- pthread_bridge.c - the bionic-slot->glibc-object bridge: mutex (honors bionic
                  type bits), cond (real 50ms-ceiling timedwait that RELEASES the
                  mutex - the fix that unblocked the JobWorker handshake), rwlock,
                  sem (handle table, real blocking wait), attr (bionic layout),
                  pthread_create honoring attr (stack size/detach).  THE reference
                  sync layer, complete.
- android.c     - libandroid/mediandk surface: ANativeWindow (width/height from fb),
                  ALooper (idle poll), ASensor (none), ATrace, AChoreographer.
- jni.c         - the complete generic Unity JNI surface: UnityPlayer + NativeLoader
                  + ReflectionHelper + SharedPreferences + AssetManager + Choreographer
                  + System.loadLibrary/Runtime.loadLibrary (-> nx_run_init, THE piece
                  our old loader was missing that made findLibrary-load fail).
- jni_slots.h   - JNI slot-number table (ABI check).
- main.c        - reference boot: setup paths -> JNI init -> EGL init (creates SDL
                  window+GLES2) -> build_imports (bionic+pthread+android+egl) ->
                  load modules (libmain->libunity->libil2cpp, relocate) -> libmain
                  init + JNI_OnLoad -> NativeLoader.load -> run_unity() lifecycle
                  (initJni -> nativeRecreateGfxState x2 -> sendSurfaceChanged ->
                  focus/resume -> nativeRender loop -> focus loss/pause).
- gds_egl.c     - reference EGL table interface over our dlopen-based egl_shim.c
                  (creates the real SDL window + GLES2 context; dlsym the Mali EGL).
- gds_fs.c      - GDS data-path redirect: maps assets/bin/Data, bin/Data, assets/,
                  and bare global-metadata.dat -> flat <gamedir>/data/ tree.
- input.c       - minimal touch-game input (exit chord via dlopen SDL; no-op
                  keyboard callbacks).  GDS is a Kairosoft touch game, not gamepad.
- audio.c       - no-op (GDS libunity imports only fmodf, a libm math fn, NOT FMOD).
- zlib_stub.c   - resolve libz symbols via dlsym so the loader doesn't link libz.

### GDS adaptations (only the game-specific bits)
- Package name -> net.kairosoft.android.gamedev3en
- LIBS list: libmain, libunity, libil2cpp (no Firebase)
- .so files at <gamedir>/ directly (not <gamedir>/lib/), data at <gamedir>/data/
- il2cpp_set_data_dir(<gamedir>/data/Managed) before libil2cpp init_array runs
  (the bench-proven metadata-path fix, applied in j_System_loadLibrary).

### Why this is different
Every prior 0.5x build was the OLD loader with targeted patches.  This replaces
the loader ENTIRELY with the reference's proven architecture - including the
System.loadLibrary binding that the old loader never had (which is what Unity
needs to actually load il2cpp after findLibrary returns a path).  Version marker
is "0.60.0-ref" (gds_deploy.sh regex already accepts -[a-z0-9]+ suffixes).

### Honest status
Builds cleanly as a glibc-linked aarch64 binary (not freestanding, so the Unicorn
bench can't run it headlessly - it must be tested on-device).  Deploy 0.60.0-ref
and the log should show the reference boot sequence [gds] initJni... ->
nativeRecreateGfxState -> nativeRender loop, now with System.loadLibrary binding
so il2cpp actually loads.  If it reaches nativeRender, the game data/rendering is
the next surface (input is minimal; GDS is touch).

## 0.60.1-ref — EVIDENCE-BASED ROOT CAUSE ANALYSIS & UNIFICATION

### Why previous builds failed on device (EVIDENCE FROM LOGS & DISASSEMBLY)

A rigorous comparison of `loader.log` (`0.50.4-glibc`, the older patched codebase in `loader/`) and `port_launch.log` (`0.60.0-ref`, the wholesale reference port in `loader_ref/`) revealed why previous attempts oscillated between hangs and silent/opaque crashes:

1. **Why `0.50.4-glibc` (`loader/`) hung in `nativeRender loop...`**:
   - `loader.log` showed thread `8487` spinning in `state=R` on syscall 63 (`read` on fd 0x16 = 22), while the main thread (`8435`) and worker threads were blocked in syscall 98 (`SYS_futex`).
   - Root cause: `ALooper_pollOnce` in the older stub was returning `0`. In the Android NDK, returning `0` means an event is ready on fd 0 and data should be read from the pipe/eventfd. This caused Unity's event loop to enter an infinite loop trying to read from fd 22, starving the frame pipeline.

2. **Why `0.60.0-ref` (`loader_ref/`) crashed with `Trace/breakpoint trap ./loader2` (`exit code 133 = SIGTRAP = signal 5`) without any EGL output or crash handler log**:
   - In `0.60.0-ref`, `my_sysconf()` checked glibc constants (`0x61, 0x62`) but missed Bionic constants (`96=_SC_NPROCESSORS_CONF`, `97=_SC_NPROCESSORS_ONLN`). When Unity queried `sysconf(96)`/`sysconf(97)`, it received the real CPU count (4) and spawned 3 JobWorker threads.
   - `b_create` in `loader_ref/pthread_bridge.c` did not set up Bionic TLS (`tp+0x28` stack guard canary, `tp+0x30` lo, `tp+0x38` hi) on newly spawned threads. When those threads executed inside Unity/IL2CPP, uninitialized TLS caused an assertion or stack canary failure, raising `SIGTRAP` (`BRK`).
   - `install_fault_handler()` in `loader_ref/main.c` only caught `SIGSEGV, SIGBUS, SIGILL`. It did NOT catch `SIGTRAP` (5), `SIGABRT` (6), `SIGFPE` (8), or `SIGSYS` (31). As a result, the Linux kernel terminated `./loader2` immediately with exit code 133 without invoking `on_fault()`.
   - `loader_ref/main.c` did not open `loader.log`, and `stdout` was block-buffered when redirected by `/roms/ports/Game Dev Story.sh`. All `printf(...)` logs from EGL/SDL initialization (`[egl] SDL_Init(VIDEO)`, GL vendor/renderer) were buffered in `stdout` and discarded when the kernel killed `loader2`.
   - Furthermore, `loader_ref/bionic.c` was missing imports for `environ`, `_Exit`, `perror`, and `_ZTH15gDeferredAction`, causing `3 relocations unresolved`.

### All Fixes Shipped in `0.60.1-ref`

1. **Unbuffered Log File Redirection (`loader_ref/main.c`)**:
   - Added `gds_log_open(argv[0])`, which opens `loader.log` (next to `argv[0]`) and `/tmp/gamedevstory_loader.log` (O_WRONLY | O_CREAT | O_TRUNC), duplicates `stdout` (1) and `stderr` (2) to the log file descriptor, and sets both to `_IONBF`. No log lines are ever lost to stream buffering.
   - Baked version marker `#define GDS_BUILD_VERSION "0.60.1-ref"` into startup banners on both stdout and stderr.

2. **Complete Signal Handling on Alternate Stack (`loader_ref/main.c` & `loader_ref/bionic.c`)**:
   - `gds_install_fault_handler()` now configures a 256KB alternate signal stack (`sigaltstack`) and registers `on_fault` for `SIGSEGV, SIGBUS, SIGILL, SIGTRAP, SIGABRT, SIGFPE, SIGSYS`.
   - Re-arms `gds_install_fault_handler()` in `run_unity()` immediately after `egl_shim_create_window()` and `egl_shim_ensure_current()` so Mali/SDL drivers cannot overwrite our crash handlers.
   - `my_sigaction` and `my_sigaltstack` in `loader_ref/bionic.c` intercept and filter Unity's attempts to overwrite crash signal handlers or alt stacks.
   - Enhanced `on_fault()` to print a complete register dump (`x0..x30`, `pc`, `sp`, `lr`), relative module offsets (`pc is libunity.so+0x...`), and a full stack backtrace (`x29` chain).

3. **1-CPU Enforcement (`loader_ref/bionic.c`)**:
   - `my_sysconf` returns `1` for all CPU-count queries (`96, 97, 98, 0x61, 0x62, 83, 84`).
   - `my_sched_getaffinity` reports a 1-CPU affinity mask (`*(unsigned long *)mask = 1UL;`).
   - `my_syscall` intercepts `SYS_sched_getaffinity` (123) to return 1 CPU, and intercepts `SYS_futex` (98) to inject 2ms poll timeouts on infinite `FUTEX_WAIT`/`FUTEX_WAIT_BITSET` calls with NULL timeout.
   - Result: Unity sees 1 CPU -> creates 0 JobWorker threads -> executes Job System tasks INLINE on the main thread without futex deadlocks.

4. **Bionic TLS Setup on All Threads (`loader_ref/main.c` & `loader_ref/pthread_bridge.c`)**:
   - Implemented `gds_setup_tls()` using `g_bionic_guard_pad[256]` to initialize `tp+0x28` (stack guard), `tp+0x30` (stack lo), and `tp+0x38` (stack hi).
   - In `pthread_bridge.c`, every thread spawned by `b_create` runs `gds_worker_tramp`, calling `gds_setup_tls()` before its original start routine. Every thread has valid Bionic TLS.

5. **0 Relocations Unresolved & Safe Stack Canary/Abort Overrides (`loader_ref/bionic.c`)**:
   - Added `environ`, `_Exit`, `my__exit`, `perror`, and `_ZTH15gDeferredAction` to `tab[]`.
   - `my___stack_chk_fail` logs a warning and returns cleanly without terminating the process.
   - Overrode `abort`, `raise`, `tgkill`, `exit`, `_exit` to log caller addresses and messages before terminating or raising signals.

6. **Unified Codebase & Packaging (`loader_ref/build.sh`, `loader/build_glibc.sh`, `tools/make_port.sh`, `tools/gds_deploy.sh`)**:
   - `loader_ref/build.sh` compiles `loader_ref/loader2` (`0.60.1-ref`) and copies it to `loader/loader2_glibc` and `ports/gamedevstory/gamedevstory/loader2`.
   - `loader/build_glibc.sh` delegates directly to `loader_ref/build.sh`.
   - `tools/make_port.sh` packages `0.60.1-ref` into both `GameDevStory_PortMaster.zip` and `gamedevstory.zip`.
   - `tools/gds_deploy.sh` verifies `GDS_EXPECT_VER="0.60.1-ref"`.

### How to test / deploy

```bash
curl -sL -o gds_deploy.sh https://github.com/jackomix/asdf/raw/arena/019fd2ec-asdf/GDS_Unity/tools/gds_deploy.sh && chmod +x gds_deploy.sh && ./gds_deploy.sh ark@192.168.18.20
```
Expect `[gds] build: 0.60.1-ref`, `0 relocations unresolved`, EGL/SDL window initialization (`[egl] window ... context ready (ES2)`), and smooth execution of `nativeRender loop` with jobs running inline on 1 CPU.

