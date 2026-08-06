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

## Music intro echo — FIXED in 0.92 (preroll gate, verify on device)
Mechanism: ring empty when a song starts; first replenishing quantum was
consumed while still underrun → opening ~1s audibly restarted.
Fix shipped: `sdl_audio_callback` rearms a 2-quantum gate (4096B) on every
TOTAL starve of the fmod player; mixing is withheld (silence) until the ring
holds it, so the first audible frame of a song starts with ~43 ms buffered.
Auto-rearm covers track changes; a 12-callback (~0.55 s) flush timeout means a
lone <2-quantum blip can never be trapped behind the gate. Fields
`preroll_bytes/preroll_ticks` in AudioPlayer; cleared at every ring reset
(reset_meta / SetPlayState(STOPPED) / bq_Clear).

## Known cosmetic: black screen during boot (Unity splash absent)
On Android, the "Made with Unity" / Kairosoft-logo splash is drawn by the
UnityPlayer Java/splash pipeline; our loader drives `nativeRender` directly
and presents nothing until the game's first own frame, so the whole slow
module-load/il2cpp-warmup window is just black (preswap br shows the game's
own bg `#4b6791` only from frame ~1). Options if wanted later: (a) loader-side
SDL surface blit of a bundled BMP right after KMSDRM comes up, before egl
init — cheap; (b) investigate libunity's SplashScreen native path — likely a
rabbit hole (Java-side scheduling), recommend (a) or nothing.

## RESOLVED: crash after company-name DONE (fix pending device confirm)
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

RESOLUTION (0.92, `arm_fep_fix` consumer-side patch): the builder-attribution
chase was abandoned after 0.91's dump proved statics+0x10 is NOT a delegate
(unaligned ASCII garbage). Patched the READER instead, offsets byte-verified
against 2.6.9 libil2cpp.so:
- 0x17f4a48 `b.ls → b.lo` (0x54000329→0x54000323): Length==1 no longer AOREs.
- 0x17f4a54 `ldr x9,[x0,#0x28] → [x0,#0x20]` (0xf9401409→0xf9401009): label
  text = result[0] (the entered name) instead of result[1].
- result[0] non-null then selects the POSITIVE (OK) listener = real-device
  post-OK flow into the tutorial. Length==0 still AOREs (old semantics).
Expected device proof: `[trap] FepPanel.result len=1` still prints, NO
`FepPanel.aoresite`/`raiseAOREchk` follows, no "An error has occurred."
dialog; game proceeds past company naming.

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
  Unity logcat spam since 0.91; 0.92 additionally mutes the
  [V/Unity] "AndroidJNIHelper" warning+stack blocks (first + 1/256) and
  collapses identical `[egl]` trace repeats (first + 1/32).

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
