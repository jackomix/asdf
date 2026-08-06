# Game Dev Story (R36S port) — handoff notes

Game: `net.kairosoft.android.gamedev3en` 2.6.9, Unity 2022.3.62f2, IL2CPP arm64.
Target: R36S (ArkOS, RK3326, Mali-G31, 640×480, KMSDRM), custom ELF loader
(`GDS_Unity/loader_ref`, builds `loader2`, ships in `gamedevstory.zip`).

## Works
- Display + ES3 context, landscape-forced, gamepad (no cursor; native pad).
- Audio: FMOD AudioTrack path reverse-engineered. Mix clock 24000Hz stereo s16;
  pump = JNI thread calling `fmodProcess(ByteBuffer)` directly (capacity 8192B).
  FMOD writes exactly ONE 512-frame quantum per call (readData zero-pads rest);
  we trim the push to the written span (`wr=` in block log). SDL @44100 with the
  existing per-player resampler keeps pitch correct. Gain stage: master 1.0
  (was 0.30); soft-knee limiter @28000 means worst legal source (32767×0.64)
  = 20971 < knee → provably clip-free. `GDS_GAIN` 0.05..3.0 knob.
- Save system (RecordStore across `home/Android/data/...`); deploy preserves
  `gds_env.cfg` between redeploys (knobs now sticky).

## Known cosmetic: music intro echo
At title-music start, first ~1s plays, then repeats once. Mechanism: ring is
empty when music starts; first 512-frame quantum (21 ms) is consumed with
immediate underruns until ~4 quanta buffer. Fix sketch (not shipped):
preroll gate in `sdl_audio_callback` for the fmod player — when a player
transitions empty→content, withhold mixing until `ring_readable >= 2 *
src_frames_needed` (≈ 42 ms); consumer already memsets silence so the gate
costs a one-time 40 ms delay, no artifact. Alternatively raise the pump's
producer floor at song start (push 4 quanta before unpausing the device).

## OPEN: crash after company-name DONE
`System.IndexOutOfRangeException` from `FepPanel::Update` @0x17f4aac:
`GetFepPanelResult` delivered `String[1] {text}` but the consumer requires
Length ≥ 2 (`[0]` = button marker, non-null→positive listener; `[1]` = text).
Device-witnessed: black+tabs transition appears (that is NOT listener-driven),
then dialog+quit. Matches real-device flow (transition then tutorial).

Evidence trail:
- `[trap] FepPanel.result` (kind 31 @0x17f4a40): `len=1, [0]=text` both runs.
- raise site confirmed: `raiseAOREchk.stub caller=0x17f4ab0`.
- FepPanel::Update disasm: Len check `b.ls` needs ≥2; then label.text=[1];
  `[0]==null`→negative listener `[x19,#0x28]` else positive `[x19,#0x20]`,
  vtable `[x8,#0x178]` w2=1 w3=0.
- b__91_0 (GetFepPanelResult closure @0x18040ac): builds 2-elem/{t,t} (flag≠0
  & [S+0x208]==1), empty 2-elem (flag≠0 else), 5-elem (flag==0 & statics[0]==2),
  else TAIL branch (0x1804378) invoking a delegate at compat-statics[0x10]
  (klass cell 0x1ebf338). Delivered len=1 ⇒ tail branch ran.
- WRONG ATTRIBUTION (withdrawn): 0.90 poked the UIMethod @0x1809498 (F3,
  `new string[1]` @0x18095a8) — device run showed len=1 unchanged. F3 is the
  show/initial-text path. Pokes removed in 0.91.

NEXT STEP: 0.91.0 kind-31 dumps the delegate itself (klass, target,
method_ptr) at the moment of failure. Read method_ptr offline → the real
builder of the 1-elem array → make IT produce {marker,text} (or make the
delegate's store duplicate to len 2).

## Tabs still small — ruled out
Screen.dpi 160/240/320 (`GDS_DPI`) — no change. `GDS_TABLET=0/1` — no change.
Next: kairo.unity.ui scale path in il2cpp (not density-driven); possibly form
layout fixed to window dims. Uninvestigated.

## il2cpp stub family (disasm-verified, earlier labels were wrong)
- 0xcb0ddc = raise(exception obj) — EH rethrow helper (NOT a bounds stub!)
- 0xcb0de4 = raise NullReference
- 0xcb0dec = bounds-check-fail stub (AORE canonical message)
- 0xcb0df4 = AORE with explicit message (String.CharAt sites)
- 0xcb0cbc = DirectoryNotFound helper (benign first-boot rs-dir miss)
- 0xcb1180 = InvalidCast
- 0xcb0bbc class-init · 0xcb0cc0 type-init · 0xcb0cc4 cast-check · 0xcb0c30 array_new(klass,len)

## Device operation
- Live log: `/roms/ports/port_launch.log` (loader.log is stale).
- Knobs file: `/roms/ports/gamedevstory/gds_env.cfg` (see `[gds] env cfg:` /
  `[gds] knobs:` lines at boot — self-verifying).
- Redeploy: `curl -sL -o gds_deploy.sh https://github.com/jackomix/asdf/raw/arena/019fd2ed-asdf/GDS_Unity/tools/gds_deploy.sh && chmod +x gds_deploy.sh && ./gds_deploy.sh ark@192.168.18.20` — installs only; launch from ES Ports menu.
- Enable traps: uncomment `#GDS_TRAP_AT=1` in cfg. Disable to slim logs.
- `GDS_LOGCAT=0` mutes mirrored game logcat. Loader auto-dedupes repeated
  Unity logcat spam since 0.91.

## Sandbox logistics
- venv wipes every ~15 min: `python3 -m venv /home/user/venv && /home/user/venv/bin/pip -q install ziglang==0.13.0 capstone pyelftools`.
- kit symlinks: `/home/user/kit/libil2cpp.so` → `GDS_Unity/ports/gamedevstory/gamedevstory/libil2cpp.so` (+ libunity, libmain).
- Tools: `/tmp/disbin.py` (capstone disasm of any ELF VA w/ string annot),
  `/tmp/findcall.py` (bl/b callers of a VA), `/tmp/scanimm.py` (imm-offset
  str/ldr scans).
- Git: local history sometimes rewinds to base — `git log --oneline -3` before
  push; if rewound: `git fetch origin arena/019fd2ed-asdf && git reset FETCH_HEAD`.
- Ship ritual: bump banner in main.c + `GDS_EXPECT_VER_BAKED` in
  tools/gds_deploy.sh → `PATH="/home/user/venv/bin:$PATH" bash build.sh` in
  loader_ref → cp loader2 → zip -u → verify version string in zip → commit → push.
