# Game Dev Story — Unity/IL2CPP port (KairoVM)

This runs the **shipped ARM64 machine code** of Game Dev Story 2.6.9
(`net.kairosoft.android.gamedev3en`, Unity IL2CPP) — the real
`lib/arm64-v8a/libil2cpp.so` from the APK. Nothing here is a remake, a clone
or a reimplementation of the game: the game logic, the forms, the asset
decoders, the layout maths and the draw-call generation all come out of
Kairosoft's own binary. This tree only supplies the platform the binary
expects to find underneath it.

Nothing from the APK is committed. `tools/extract_apk.sh` unpacks a
user-supplied APK into `out/` (git-ignored) before anything else runs.

## What the host has to provide

The APK ships two native libraries. `libil2cpp.so` is the game; `libunity.so`
is the engine. On a development machine that is not ARM64 there is no way to
run either directly, so `kairovm/` boots `libil2cpp.so` under Unicorn and
stands in for everything it links against:

| piece | file | stands in for |
|---|---|---|
| ELF loader + relocations | `kairovm/elfimage.py`, `boot.py` | Android's linker |
| libc / libm / pthread / futex | `kairovm/bionic.py` | bionic |
| JNI (`JNIEnv`, Java classes the game calls) | `kairovm/androidjni.py` | Android runtime |
| Unity internal calls, objects, assets | `kairovm/unity.py` | `libunity.so` |
| serialized asset files | `tools/unityfs.py` | Unity's asset loader |
| player loop (Awake/Start/Update/OnGUI) | `kairovm/game.py` | Unity's player loop |
| software rasteriser for the engine's draw calls | `kairovm/raster.py` | GLES |

`kairovm/machine.py` is the CPU and scheduler: guest threads are green
threads driven by the emulator, with futex, timed waits and a virtual clock
(the engine's loader threads sleep in wall-clock terms, and an emulated frame
is far slower than a real one, so the VM runs its clock forward to the next
deadline instead of stalling).

## Running it

```sh
tools/extract_apk.sh ../apks/Game+Dev+Story_2.6.9.apk
python3 tools/render.py --frames 30 --dump --ascii   # frames -> out/frame.png
python3 tools/profile.py --frames 4                  # where the engine is
python3 tools/threads.py --frames 12 --pump          # loader-thread states
python3 tools/disasm.py BootForm::Load               # static ARM64 listing
```

Requires `unicorn`, `capstone`, `keystone-engine`, `pyelftools`.

## Where this is going: the handheld build

The end goal is an R36S-class AArch64 handheld (PortMaster / ArkOS). That
target does **not** need the emulator — it is already ARM64, so the shipped
`libil2cpp.so` can execute natively and only the platform underneath it has
to be shimmed. The same surface this VM implements in Python becomes a small
C loader on the device:

* bionic → glibc shims (FORTIFY `_chk` wrappers, `__sF`, `__system_property_get`,
  and the bionic/glibc `sigaction`/`sigset_t` ABI difference, which is a real
  stack-smashing hazard on arm64),
* a JNI shim with a generated stub table for the Java classes the game calls,
* EGL/GLES + SDL2 for window, input and audio.

[`NextOs-Ports/terraria-nextos`](https://github.com/NextOs-Ports/terraria-nextos)
is prior art for exactly that shape of loader (Terraria Android, Unity 2021
IL2CPP, validated on an R36S-class ArkOS device at 640x480) and is worth
reading before writing the device side: `src/bionic_shims.c`,
`src/jni_shim.c` + `src/jni_idx_stubs.gen.c`, `src/egl_shim.c`,
`src/imports.gen.c`, plus its BYO-APK extractor and PortMaster packaging.
Its one structural difference is that it also loads the APK's `libunity.so`,
so the engine runs natively and no Unity host is needed; that is the cheaper
path on-device, and the icall inventory in `kairovm/unity.py` is then only
needed for development here.
