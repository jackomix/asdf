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

- Session branch `arena/019fc860-asdf`.  (Note: the Unity/IL2CPP GDS APKs were
  committed on `arena/019fbc18-asdf` under `APKs/`; that branch's `APKs/`
  directory was NOT on `main`.  `git fetch origin '+refs/heads/*:refs/remotes/origin/*'`
  pulls it; `git show origin/arena/019fbc18-asdf:APKs/Game+Dev+Story_2.6.9.apk`
  recovers the 53 MB APK.)
- Latest commits on `arena/019fc860-asdf`: loader loads all three real native
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
- Deploy: `curl -sL -o gds_deploy.sh https://github.com/jackomix/asdf/raw/arena/019fc860-asdf/GDS_Unity/tools/gds_deploy.sh && chmod +x gds_deploy.sh && ./gds_deploy.sh ark@192.168.18.20`
- Read `gamedevstory/gamedevstory/loader.log` (fresh each run now).
- Key files: `loader/loader_glibc_main.c` (boot), `loader/egl_shim.c` (EGL/GL),
  `loader/jni_shim.c` (JNI shim — the dialog is built through here),
  `loader/fs_redirect.c` (open/fopen redirect + cmdline inject),
  `loader/bionic_bridge.c` (pthread/signal/statfs), `loader/glibc_shims.c`.
- Reference: `NextOs-Ports/terraria-nextos` runs a full Unity game on this exact
  device; its `src/main.c` + `src/bionic_shims.c` + `src/pthread_fake.c` + `src/jni_shim.c`
  solve these exact problems.

## LATEST STATUS (build 0.25.0-glibc) - 1-CPU shim v3: + opendir/readdir /sys/cpu

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
curl -sL -o gds_deploy.sh https://github.com/jackomix/asdf/raw/arena/019fc860-asdf/GDS_Unity/tools/gds_deploy.sh && chmod +x gds_deploy.sh && ./gds_deploy.sh ark@192.168.18.20
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
    curl -sL -o gds_deploy.sh https://github.com/jackomix/asdf/raw/arena/019fc860-asdf/GDS_Unity/tools/gds_deploy.sh && chmod +x gds_deploy.sh && ./gds_deploy.sh ark@192.168.18.20
Expect `[loader] build: 0.7.0-glibc` in the log (stale-zip guard aborts otherwise).
