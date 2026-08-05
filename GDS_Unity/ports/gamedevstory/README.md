# Game Dev Story

Thanks to **Kairosoft** for creating Game Dev Story and making it available on
Android. This port runs the **actual shipped ARM64 machine code** of Game Dev
Story 2.6.9 natively on the R36S via a custom loader — nothing here is a
remake or reimplementation.

## How it works

The APK ships two native libraries:
- `libil2cpp.so` — the game (Unity IL2CPP)
- `libunity.so` — the Unity engine

Both are **ARM64**, the same architecture as the R36S, so they execute
natively. The loader (`loader2`) supplies the platform Android usually
provides underneath them: bionic→Linux shims, a JNI environment, and EGL
stubs. It then boots the engine + game and runs `il2cpp_init`.

## Controls

Not yet implemented — the game is a touch-first Android title. Input mapping
(joystick → touch/mouse via gptokeyb) is the current work-in-progress.

## Files

- `gamedevstory/loader2` — the native ARM64 loader (standalone, no deps)
- `gamedevstory/libil2cpp.so`, `libunity.so`, `libmain.so` — the game engine
- `gamedevstory/data/` — the game's `assets/bin/Data` (managed assemblies,
  metadata, resources)

## Build

```
cd GDS_Unity
bash loader/build.sh        # cross-compile loader2 (aarch64, freestanding)
```
