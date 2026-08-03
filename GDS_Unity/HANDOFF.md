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
- **The loader now loads the REAL Game Dev Story `libil2cpp.so` end-to-end.**
  With the extracted APK at `out/apk/lib/arm64-v8a/libil2cpp.so`:
  `python3 tools/run_aarch64.py loader/loader2 $(pwd)/out/apk/lib/arm64-v8a/libil2cpp.so`
  reads the entire 33 MB file, maps PT_LOAD, resolves ALL ~261 imported libc/
  libm/pthread/locale symbols (0 unresolved), applies ~30k relocations, **runs
  all 18 real init_array ctors of the IL2CPP runtime, and exits 0**:
  `[loader] ... mapped @0x200000000 span=0x221f000 relas=30285`
  `[loader] ... init_array ran (18 ctors)`
  `[loader] OK: ... loaded and initialised` / `[guest exit 0]`.
  A synthetic `loader/so_probe.c` still exercises the reloc path in isolation
  (RELATIVE, self GLOB_DAT, JUMP_SLOT, DT_INIT_ARRAY) without the big asset.
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

- The real `libil2cpp.so` **loads and runs init_array** now (see
  DONE/WORKING).  What is NOT yet done: loading `libunity.so` (the engine) and
  resolving ITS imports, calling `UnityPluginLoad` / `UnityMain`, and providing
  the JNI/EGL/SDL surface.  `libunity.so` is in the same extracted APK
  (`out/apk/lib/arm64-v8a/libunity.so`, 16 MB) and is the natural next target;
  it will surface bionic/glibc ABI gaps (notably the arm64 `sigaction`/`sigset_t`
  size difference) and the JNI-call inventory.
- The real IL2CPP boot passed because `freestdlib.c` + `host_syms.c` now carry a
  broad freestanding libc: allocator (incl. realloc/memalign), mem/str, printf,
  time/syscalls, pthread/mutex/cond/sem stubs, math stubs, locale/wchar stubs,
  and bionic extras (`__errno`, `environ`, `__sF`, `__system_property_get`,
  `__cxa_atexit`/`__cxa_finalize`, `__stack_chk_fail`, `uname`,
  `__android_log_print`).  These are the symbols `host_dlsym`'s table maps to;
  add any newly-unresolved symbol there (it prints `unresolved <name>`).
- Stage 1 runs **only libil2cpp.so's init_array** (18 ctors).  It does NOT call
  into the game logic, and the IL2CPP runtime initialised by init_array is not
  yet driven (no `il2cpp_runtime_invoke`/threads/Unity host).  The next step is
  `libunity.so`.
- No GPU / JNI / SDL yet. `loader.c` only does the ELF load. Next milestones
  (stage 2+): load BOTH `libil2cpp.so` and `libunity.so`, resolve
  `libunity.so`'s imports against glibc, call Unity's `UnityPluginLoad` /
  `UnityMain`, and provide EGL/SDL + JNI shims (model on
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

0. **(DONE)** Real `libil2cpp.so` loads, init_array runs, exit 0.  The real
   GDS APK is present at `APKs/Game+Dev+Story_2.6.9.apk` (53 MB) and extracted
   to `GDS_Unity/out/apk/` (gitignored).  Rebuild + rerun:
   `python3 -m ziglang cc ... loader/loader.c loader/freestdlib.c loader/host_syms.c -o loader/loader2`
   `python3 tools/run_aarch64.py loader/loader2 $(pwd)/out/apk/lib/arm64-v8a/libil2cpp.so`
   Expect `init_array ran (18 ctors)` and `[guest exit 0]`.
1. **Load `libunity.so` too** and resolve its imports; call its entry
   (`UnityPluginLoad`/`UnityMain`).  It's at
   `out/apk/lib/arm64-v8a/libunity.so` (16 MB).  The loader's `real_main` and
   `load_object` currently take a single path; extend to load both, resolve
   libunity's imports against the same host table, and drive the Unity player
   loop.  This is the bulk of the remaining work and will surface the
   bionic/glibc ABI gaps (notably arm64 `sigaction`/`sigset_t`: bionic 8 bytes
   vs glibc 128).
2. Add the shim surface modeled on terraria-nextos:
   - `bionic_shims.c`: FORTIFY `_chk` wrappers, `__sF`, `__system_property_get`,
     and the **bionic/glibc `sigaction`/`sigset_t` ABI difference** (bionic arm64
     sigset is 8 bytes; glibc 128 — must convert, or it overruns caller stack).
     This is a real hazard.
   - `jni_shim.c` + generated stubs: from the JNI call inventory the VM produced.
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
python3 -m ziglang cc -target aarch64-linux-gnu -ffreestanding -nostdlib \
    -fno-pic -fno-pie -O0 -fno-sanitize=undefined \
    loader/loader.c loader/freestdlib.c loader/host_syms.c -o loader/loader2
# freestanding smoke test (no asset needed):
python3 tools/run_aarch64.py loader/tiny
# full loader (needs extracted .so):
python3 tools/run_aarch64.py loader/loader2 /abs/path/to/libil2cpp.so
```

## COMMIT STATE

- Session branch `arena/019fc860-asdf`.  (Note: the Unity/IL2CPP GDS APKs were
  committed on `arena/019fbc18-asdf` under `APKs/`; that branch's `APKs/`
  directory was NOT on `main`.  `git fetch origin '+refs/heads/*:refs/remotes/origin/*'`
  pulls it; `git show origin/arena/019fbc18-asdf:APKs/Game+Dev+Story_2.6.9.apk`
  recovers the 53 MB APK.)
- Latest commit on `arena/019fc860-asdf`: loader loads real libil2cpp.so, runs
  init_array, exits 0 (see commit message).
- `out/`, `*.so`, and APK binaries are gitignored by design; only source is
  committed.  The extracted libs live in `GDS_Unity/out/apk/` (not committed).
