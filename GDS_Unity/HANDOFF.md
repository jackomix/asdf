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
- **The loader's `.so`-loading path is now verified end-to-end** — with a small
  synthetic shared object, since the 33 MB game asset is absent:
  `python3 -m ziglang cc -target aarch64-linux-gnu -shared -fPIC -ffreestanding
  -nostdlib -fno-sanitize=undefined loader/so_probe.c -o /tmp/so_probe.so`, then
  `python3 tools/run_aarch64.py loader/loader2 /tmp/so_probe.so`.  The loader
  now maps PT_LOAD, applies RELATIVE, resolves **GLOB_DAT symbols defined in the
  module itself** (`kairo_marker` -> `bias+st_value`, not host dlsym), resolves
  JUMP_SLOT imports through `.rela.plt` (`DT_JMPREL`) against the host symbol
  table (`strlen` -> `kv_strlen`), and runs DT_INIT_ARRAY.  Verified output:
  `[so_probe] ctor ran; strlen("probe")=5 marker=1234abcd marker_addr=200030808`,
  exit 0.
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

- The loader cannot yet load the REAL `libil2cpp.so` in the bench — because
  **the 33 MB `.so` asset is gitignored and not present in this sandbox
  instance** (`out/apk/lib/arm64-v8a/libil2cpp.so` is missing).  When present,
  run:
  `python3 tools/run_aarch64.py loader/loader2 /abs/path/libil2cpp.so`
  The loader's `.so` machinery itself is verified via `loader/so_probe.c`
  (see DONE/WORKING); the only unverified step is the real binary, which will
  also surface TLS/IRELATIVE relocation types the loader currently skips.
- **Asset mismatch to be aware of:** the only APK in this sandbox is
  `Epic_Astro_Story-NTU1NTM3.apk`, and it is the OLD Java/Dalvik engine — it
  contains `classes.dex` + `assets/*.dat` + ogg, **no native `libil2cpp.so` /
  `libunity.so`** (that APK belongs to `EAS_OldEngine/`, a JS/Dalvik emulator,
  not the Unity loader).  A Unity/IL2CPP Kairosoft title (e.g. Game Dev Story)
  is required to exercise the loader against a real `.so`.  That is a
  user-supplied APK; it is not on disk here.
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

0. The loader's `.so` machinery is now verified with a synthetic `.so`
   (`loader/so_probe.c`, see DONE/WORKING).  To repeat:
   `python3 -m ziglang cc -target aarch64-linux-gnu -shared -fPIC -ffreestanding
   -nostdlib -fno-sanitize=undefined loader/so_probe.c -o /tmp/so_probe.so`
   `python3 tools/run_aarch64.py loader/loader2 /tmp/so_probe.so`
   Expect the `[so_probe] ctor ran ...` line and `[guest exit 0]`.
1. Restore the real asset: run `tools/extract_apk.sh <Unity-Kairosoft.apk>` (a
   Unity/IL2CPP title such as Game Dev Story — NOT the old-engine Epic Astro
   Story APK present here, which has no native libs) so
   `out/apk/lib/arm64-v8a/libil2cpp.so` exists. Then:
   `python3 tools/run_aarch64.py loader/loader2 $(pwd)/out/apk/lib/arm64-v8a/libil2cpp.so`
   Expect: it loads, runs init_array, prints `[loader] ... init_array ran`,
   exits 0. First run will likely surface TLS/IRELATIVE reloc types the loader
   currently skips — add them to the reloc loop.
2. Load `libunity.so` too and resolve its imports; call its entry. This is the
   bulk of the remaining work and will surface the bionic/glibc ABI gaps.
3. Add the shim surface modeled on terraria-nextos:
   - `bionic_shims.c`: FORTIFY `_chk` wrappers, `__sF`, `__system_property_get`,
     and the **bionic/glibc `sigaction`/`sigset_t` ABI difference** (bionic arm64
     sigset is 8 bytes; glibc 128 — must convert, or it overruns caller stack).
     This is a real hazard.
   - `jni_shim.c` + generated stubs: from the JNI call inventory the VM produced.
   - `egl_shim.c` / SDL2: window, input, audio.
4. Build for the real device: the same `loader.c` compiled for `aarch64-linux-gnu`
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

- Branch `arena/019fbc18-asdf` on `github.com/jackomix/asdf`.
- Last push: `1b3c5bc` "GDS_Unity/loader: native aarch64 ELF loader + Unicorn
  test bench". Subsequent local edits to the harness/freestdlib are NOT yet
  pushed (see note 7 about module availability).
- `out/` and any `*.so`/APK binaries are gitignored by design.
