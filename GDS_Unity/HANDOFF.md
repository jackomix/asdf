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
it runs natively on the R36S (no glibc/pkg install needed).  Build the device
package with:

```
cd GDS_Unity
bash loader/build.sh                    # builds loader/loader2
rm -rf /tmp/gds_pkg && mkdir -p /tmp/gds_pkg
cp loader/loader2 loader/dev_run.sh /tmp/gds_pkg/
cp out/apk/lib/arm64-v8a/libil2cpp.so \
   out/apk/lib/arm64-v8a/libunity.so \
   out/apk/lib/arm64-v8a/libmain.so /tmp/gds_pkg/
cp -r out/apk/assets/bin/Data /tmp/gds_pkg/data
cd /tmp/gds_pkg && zip -r GameDevStory_R36S.zip .
```

Copy `GameDevStory_R36S.zip` (or the folder) to the R36S SD card, e.g.
`/roms/ports/gamedevstory/`, unzip, and run:

```
cd /roms/ports/gamedevstory
./dev_run.sh
```

`dev_run.sh` runs `./loader2` and tees everything to `loader.log`.  **Send back
`loader.log`** - it shows: which libs loaded, init_array ctor counts, JNI_OnLoad
result, il2cpp_init result, and any memory fault / abort.  The log is the
comprehensive diagnostic needed to iterate.

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
