# Game Dev Story (R36S port) — handoff notes

Game: `net.kairosoft.android.gamedev3en` 2.6.9, Unity 2022.3.62f2, IL2CPP arm64.
Target: R36S (ArkOS, RK3326, Mali-G31, 640×480, KMSDRM), custom ELF loader
(`GDS_Unity/loader_ref`, builds `loader2`, ships in `gamedevstory.zip`).

## 0.95.6-multishow (crackle device-VERIFIED gone; chord now 1s; splash re-presented at milestones)
Device-verified on 0.95.5: **crackle GONE** (user) with the continuous-phase
resampler line live in the log (`[audio] resample 24000->44100
continuous-phase (16.16 carry, step=35666)`); **chord hold works**
(`SELECT+START held 2004ms -> graceful exit`, clean lifecycle shutdown,
exit 0). Follow-ups shipped here:

- **Chord hold default 2000->1000ms** (user request), both the evdev watcher
  and the SDL-side fallback; `GDS_QUITCHORD_MS` still overrides; the +4s
  wedged-loop `_exit` escape hatch is unchanged.
- **Splash ran but was never visible (user 0.95.5: "didn't see it at all,
  the game just opened up to the loading screen")** even though the log
  proved `splash_early_show` executed (npot tex upload, swap). The black
  phase the user describes (ES pop-up gone -> black -> game loading screen)
  is provably OUR era (window + module load + Unity boot = seconds), so a
  splash there is feasible; the draw or the flip was eaten. 0.95.6
  converges with evidence + redundancy, no blind fix:
  - **readback witness** (one line, first present): center pixel after the
    draw — navy/white = pixels landed (flip is the suspect), 000000 = GL
    no-op'd (draw is the suspect);
  - **multi-present**: same proven SDL_GL_SwapWindow route re-fires at
    `modules loaded` / `initJni OK` / `nativeResume OK` milestones
    (gds_splash_reshow from main.c) until `g_first_unity_swap` (set in both
    eglSwapBuffers present paths), so a dropped first KMSDRM flip is
    covered by later ones — all strictly before the loading screen's first
    frame, so the game's own screen is still never covered.

## 0.95.5-earlysplash (splash in the boot gap; real chord hold; music crackle SOLVED)
Device-verified 0.95.5 must still confirm on hardware; the three items and
their mechanisms:

- **Music "tiny clicks/pops like distortion" — SOLVED BY MECHANISM + SIM:**
  the SDL mixer callback resampled 24k→44.1k with a FRESH phase every 46.4ms
  chunk (`pos=0`) and discarded the tail source frames it had pulled.
  Simulation on the user's own `echo_prod.pcm` device capture: **exactly 2
  source frames (83µs of music) skipped every callback = a discontinuity at
  21.6Hz**, 4× larger than the music's own sample movement (boundary jump
  p95 2547 vs inner p95 668; new code: boundary p95 1009 ≈ inner). Volume-
  independent, echo-unrelated — exactly the reported symptom. Fix: per-player
  continuous-phase resampler (`rs_frac` 16.16 phase + 2-frame `rs_carry`
  across callbacks, rounded step; resync on underrun/track-change where the
  fade already masks the seam). One-time log: `[audio] resample 24000->44100
  continuous-phase`.
- **Splash moved INTO the boot gap (Android order):** 0.95.4's present-gate
  could only start at Unity's first frame, so it covered ~2s of the game's
  OWN loading screen ("I don't want to replace the loading screen") while
  the A-press→loading black gap stayed black. 0.95.5 draws the harvested
  BMP right after window/GL identity setup in `egl_shim_create_window`,
  presenting via the proven route (`SDL_GL_SwapWindow`; raw swap never
  reaches the panel) and leaves the frame on the KMSDRM panel through
  module-load/Unity-boot until the first real swap replaces it; the
  present-gate is suppressed afterwards (fallback only if early fails).
  Fixed-function ES1 draw because the SDL share root really is an
  "OpenGL ES-CM 1.1" context (identity print proves it; shaders can't run
  there); NPOT checked, POT-canvas + glTexSubImage2D fallback, size-agnostic
  for other Kairosoft splashes.
- **Quit chord is now a REAL hold:** 0.95.4 requested graceful exit at the
  2nd poll (~100ms; only the _exit fallback waited 2s) → user: "quits as
  soon as I hold them". Now nothing happens until SELECT+START are held
  continuously for `GDS_QUITCHORD_MS` (default 2000; both the evdev watcher
  and the SDL-side fallback); early release resets and logs one line;
  holding 4s MORE after the graceful request still `_exit(0)`s a wedged loop.

## 0.95.4-splash (real APK splash; quit-chord named; adaptive audio cap)
User-verified on 0.95.3: **echo gone, OSK trailing blank gone, naming
round-trips clean** (DONE "Sunny Studios" / wire "Sunny Studios").

- **Start+Select force-quit — codes CAUGHT BY THE USER'S PRESSES.** The
  0.95.3 raw transition logger recorded: SELECT=**0x2c0**, START=**0x2c1**
  on the 'GO-Super Gamepad' node (both down/up once, nothing surfaced on
  the SDL side, matching their dialogue). Now baked into a per-device
  table in input.c (`g_known_pads`): the watcher adopts the pair
  automatically when the stock node doesn't advertise standard
  BTN_SELECT/START; `GDS_QUITCHORD_KEYS` still overrides. Other handhelds
  just extend the table (per-game/per-device design kept for the port
  series).
- **Splash, per the standing directive (harvest, don't invent):**
  `tools/harvest_splash.py` pulls the real `res/iF.png` (1024×2048
  portrait KAIROSOFT screen) out of the APK at package time and recomposes
  it for 640×480 (navy fill + centered wordmark band, auto-bboxed) →
  `ports/gamedevstory/gamedevstory/splash.bmp`. The loader presents it for
  the first **GDS_SPLASH_MS (default 2200; 0 disables)** after the first
  real swap via a *present-gate* (drawn over the backbuffer at swap time
  with cursor-overlay state discipline) — NOT the 0.93.0 SDL-software blit
  that nil-displayed the window and got hotfixed.
- **Music clicks/pops (user report on 0.95.3):** the flat 85ms queue was
  starve-prone under normal frame jitter. The cap is now *risk-windowed*:
  **8192B during the first 1.2s of every music run** (the double-start
  echo window only ever opens at track start — mechanism stays impossible
  there), then **24576B (~5.5 callbacks)** for steady-state margin. If
  clicks persist, listen whether they cluster at track starts.
- Repack note: zip now also carries `gamedevstory/splash.bmp`.

## 0.95.3-backpress (echo killed by mechanism, not detection; OSK CR; key hunt)
**Echo fix done right this time (user called out the matcher as inelegant —
they were right).** The 0.95.2 content-fingerprint never fired on-device
anyway: FMOD's re-mixed restart audio is 99.9% identical, NOT bit-exact, so
the exact-hash pattern could never match, and a fuzzy matcher carried
false-positive risk. Replaced with plain **AudioTrack-style backpressure**:
the fmod pump may now queue at most **8192B (~85ms)** of mixed audio ahead
of the speaker (was ~40960B/232ms). The game double-starts the title BGM on
real Android too (see 0.95.2 analysis) — inaudible there only because
Android's write call blocks the mixer thread at ~1-2 buffers. With a small
queue the restart seam is as short as a phone's, for EVERY game, no
sniffing. The 0.95.2 mechanism analysis remains correct and stays below;
only the fix changed. Removed: `echo_restart_check` entirely.

- **"Extra space" in naming — SOLVED BY NAME:** the 0.95.2 boot log's
  rawtail hex shows `53 74 75 64 69 6f 73 0d` = `"Studios" + CR (0x0d)`. It
  was never a space: the bitmap font draws the control byte as an empty
  cell ("blank, then cursor"), and printing it raw made the log line
  overwrite itself — the real source of every "interleave garble" all
  along. Trim now strips `<0x20` from the prefill; all OSK/DONE/wire log
  prints escape control bytes (`gds_vis`). The game re-saves whatever DONE
  returns, so one clean naming removes the CR from the save permanently.
- **SELECT/START hunt, final chapter:** 0.95.2 boot dump proves the
  GO-Super Gamepad node does NOT expose 0x13a/0x13b (keys: `0x130 0x131
  0x133 0x134 0x136-0x139 0x220-0x223 0x2c0-0x2c4`), odroidgo3-keys has
  `0x72 0x73`, and the SDL layer never surfaces SELECT/START either
  (first-press roll: only A, D-pad, R1 arrived). New: a raw evdev
  **transition logger** (`[input] evkey … 0xNNN DOWN`) watches every
  advertised code on every node — one press of the physical buttons names
  the codes, then `GDS_QUITCHORD_KEYS` binds them (no rebuild).
- **Loudness/clipping report — measured, not guessed:** user PCM captures
  show the music itself peaks at **-7dBFS (14655/32767), RMS -22dBFS, zero
  clipped samples**; our chain adds 0.64 gain (now 0.56) and a soft-limiter
  whose knee (28000) is never reached — digital clipping in our chain is
  mathematically excluded for the music. Trimmed default music volume
  0.8 → 0.70 and added `GDS_MUSIC_VOL` (0.05..1.5) so the user can A/B on
  device; music START/STOP lines now carry `peak=N/32767` as a witness.
  If it still distorts at these levels it's the codec/speaker, not us.

## 0.95.2-echofix (music intro echo SOLVED from user PCM captures + fixed)
**The echo mystery is closed with hard data.** The user uploaded the 0.95.0
captures (`echo_prod.pcm` = what FMOD's mixer pushed, `echo_play.pcm` = what
the speaker played; 24000Hz stereo s16, first ~5.4s of the title music).
Offline cross-correlation:

- **prod == play BYTE-EXACT over the whole 5.07s overlap** (max|diff| = 0):
  our ring repeats nothing. The echo was already inside FMOD's output.
- **The repeat:** play[0:0.20s] reappears at **0.3628s** (ncc 0.999, same
  amplitude, ≤-43dB sample deltas = mixer float noise) — i.e. the speaker
  heard `stream[0:0.3628s]` and then *the same stream again from offset 0*.
- **Mechanism (fits the 0.95.0 log exactly):** the game DOUBLE-STARTS the
  title BGM (`music START` at producer block #15096 → ~16 content blocks →
  one silent block → `music START` again). Real Android does the same thing
  invisibly (its mixer backlog is ~1 block, so the restart cuts after a few
  ms). Our first run had stacked **33800B ≈ 0.35s of unread backlog**, all
  of which played before the restart's audio — that backlog is the echo.
- The user's clue "the first tiny piece changes length per boot" is just
  however much of run #1 piled up before the game's second start (race).

**Fix (`echo_restart_check` in opensles_audio.c):** content-verified ring
realignment. Every pushed quantum (~21ms) of the first 128 after ≥2s of
music silence gets a 4-byte-word hash + its absolute ring position; when
the opening 8-quantum pattern reappears at quantum `j` (8 exact consecutive
hash matches — cannot false-positive on ordinary playback), we:
1. **drop** the unread run-1 backlog (advance the read cursor to the
   restart's first byte, `qw[j-1]`), and
2. **skip** the restart stream forward by however much the listener already
   heard since session start (phase-aligned continuation).
Result by construction (verified against the real captures): what plays is
`stream[heard:...]` — the music continues seamlessly, **zero repeated
audio**. CAS on the ring tail with 4 retries; graceful no-op if the
listener already passed the seam. One log line per fired fix.

**Lesson for the next Kairosoft ports:** a game restarting a track at boot
is NORMAL and inaudible when the producer can't run far ahead. Whenever a
port buffers more than ~100ms at stream start, implement this same
pattern-verified seam realignment (or cap the startup backlog harder) —
do NOT wait for push-pattern evidence per game, the code is generic
(opensles_audio.c, session detector + realign).

Also in this build: nothing else changed vs 0.95.1-evidence (OSK rawtail
hex evidence line, evdev key dumps + GDS_QUITCHORD_KEYS, log-diet round 2
are all still in).

## 0.95.1-evidence (unambiguous OSK/key evidence; log diet round 2)
Shipped in response to the 0.95.0 device logs (Aug 7): three open items all
get *yes/no evidence* in the next boot log instead of more guesswork.

- **OSK trailing blank:** the 0.95.0 trim was ASCII-space-only; user still
  saw a blank before the caret and the garbled log (two threads racing
  stderr, e.g. `maxlen=144)=...`) couldn't prove which byte it is. Now:
  `gds_osk_open` prints ONE flockfile'd line with `rawlen=` + `rawtail=[hex]`
  + post-trim text; the jni-side arrival print got the same treatment. The
  trim also strips Unicode blanks now (NBSP `C2 A0`, U+3000 `E3 80 80`,
  figure/narrow-NBSP/zero-width `E2 80 87/AF/8B`) — the OSK bitmap font
  renders all of these as a gap, matching the user's report. If the byte is
  something else entirely, the hex tail names it.
- **Start+Select force-quit:** 0.95.0 scan showed NO evdev node advertises
  BTN_SELECT/BTN_START (all `sel=0 start=0`), and the SDL-side chord never
  fired either. New diagnostics: (a) each evdev node dumps EVERY set EV_KEY
  code once per boot, (b) a bounded "first physical press arrived as SDL X"
  roll names each pad slot as it's first pressed (no GDS_PADLOG needed),
  and (c) the chord pair is configurable: `GDS_QUITCHORD_KEYS="0x129,0x12b"`
  (hex/dec) in gds_env.cfg — no rebuild needed once the dump names the
  real codes.
- **Log diet round 2:** fixed the two 0.95.0 misses — the NRE[2..40] detail
  + sp-slot dumps were keyed on the `cxa_throw`/`cxa_throw(late)` probe tag
  (never in the quiet list), and `step-over` probes are one-shot/fresh so
  the "first 2 hits" rule never engaged (now a global cap of 6/boot).
  Also verbose-gated: EGL symbol table + per-config dumps, and ALL logcat
  below INFO (`[?/Unity] GL_EXTENSIONS` wall etc.).
- **Music echo:** PCM captures from the Aug 7 run are ON THE DEVICE at
  `/roms/ports/gamedevstory/echo_{prod,play}.pcm` — pull with
  `scp ark@<dev>:/roms/ports/gamedevstory/echo_prod.pcm .` (and `_play.pcm`,
  password `ark`), then cross-correlate offline: prod==play ⇒ repeat is
  upstream (FMOD/game), prod!=play ⇒ consumer-side (our ring). Analysis
  pending on those files.

## 0.95.0-quietlog (user-verified name fix on device; log diet; echo capture)
**Device-verified in 0.94.0:** the wire-packed name arrives whole
(`DONE "Sunny Stud"` → parser `[0]='Sunny Stud'`). Name bug CLOSED.

- **Log diet (user request: "can the log not print so much").** Quiet is now
  the default; `GDS_VERBOSE=1` in gds_env.cfg restores everything. Gated:
  EGL TR trace (MakeCurrent flap / config-attrib walls), trap-arming lines
  (~110/boot), periodic fmod block/mixer-state pumps, kjoy query + 1200-frame
  hit-summaries, JNI string-conversion probe, per-frame render counter.
  Repeat-fire traps (Storage.Open / GetFolder×5 / NRE stubs / storm.raise /
  Substring / step-over / GetNumRecords) print their first 2 hits only.
  Kept always: boot chain, music START/STOP transitions, OSK open/result
  lines, echo/preroll events, first NREs of a NEW kind, exits/errors.
- **Music intro echo — capture harness.** Producer (fmodProcess push) and
  consumer (SDL ring read) both dump the first 512KB after music starts to
  `<gamedir>/echo_prod.pcm` + `echo_play.pcm` (24000Hz stereo s16, RAM-captured,
  flushed off the audio thread when full). Compare offline: prod==play ⇒
  the repeat is upstream (FMOD/game), prod!=play ⇒ consumer-side. Ship the
  files for analysis. New clue from the user: the pre-repeat piece LENGTH
  VARIES per boot ⇒ timing-dependent (race), consistent with consumer-side.
- **OSK trailing space:** user-verified it's a REAL character before the
  caret (they backspace it out), and the dex shows the plugin copies
  `text_` verbatim — the space comes from the game data itself. Fix:
  `gds_osk_open` right-trims spaces from the PREFILL only (never typed
  text), with a "(trailing space(s) trimmed)" marker in the open log.
  Also: the `_` caret is BACK (the "no cursor" wish was the mouse pointer).
- **Start+Select force-quit:** evdev now logs every probed node once
  (name + sel/start capability bits) AND, when no node exposes both codes
  (the 0.94.0 silent failure), the watcher falls back to the SDL pad state
  pad_poll already publishes — same chord semantics, still able to _exit
  on a 2s hold. Diagnostics will show what the GO-Super Gamepad exposes.
- **Benign storm, confirmed:** the ~40 post-naming NREs = background GCM
  reg-id lookup returning null (dex: preference `_registration_id`, returns
  null when unset — real phones without Play Services behave identically).
  Extended kind-33 dump now also prints literal cell `[1ec9240]` (the name
  of the invoke that returns null; suspect `getGCMRegistrationId`).

## 0.94.0-wirepack (name fix, proven from the original dex — no guessing)
**Symptom fixed:** typed name lost its first and last character
(`qwsaderdf` → game stored `wsaderd`).

**Root cause (fully decoded 2026-08-07):** the game's managed parser
(libil2cpp `0x1810a7c` split by `,` + `0x18102b0` per-field unescape) strips
exactly the first and last character of every field — it expects the reply
PRE-WRAPPED. The wrapping is done by the Java plugin, read straight from
`classes.dex` (extracted from `APKs/Game Dev Story_2.6.9.apk`,
`kairo/android/plugin/util/StringUtil`):
- `escape(s)` = `"` + *s with every `,` `&` `@` `\` prefixed by a backslash* + `"`
  (special set = static field `ESCAPES = ",&@\\"`).
- `getString(String[])` = fields joined by `,`. (2D joins rows by `&`,
  3D blocks by `@`.)
- `unescape(s)` = strips first+last char; on any ESCAPES char takes the
  following char literally. **Byte-for-byte the same algorithm as the
  managed `0x18102b0`** — the format is a cross-side contract.
- `Utility.getInputPanelResult()` returns `StringUtil.getString(fepPanel.result_)`
  (or `getString(new String[]{null, ""})` = `"",""` when the panel is gone).

**Fix:** `mk_kairo_packed1()` in jni.c packs the OSK result like the real
`escape()`. NULL (cancel) stays NULL. The dispatch-ring forensics from 0.86
stay; the string-conversion probes stay armed via `GDS_TRAP_AT`.

**Retraction (user-confirmed 2026-08-07):** "game self-Terminates ~15s after
naming" was a misread — the user quit normally through the in-game menu.
`IApplication.Terminate` + `Unity requested render-loop stop` + exit 0 IS the
signature of a normal quit, not a crash. The ~40-NRE storm before it is the
background GCM lookup: `Utility.getGCMRegistrationId()` reads preference
`_registration_id` and returns null when absent — real phones without Play
Services behave the same and the managed side catches+retries. Cosmetic
noise; if wanted later, serve a synthetic stable id from that preference key.

**Also in 0.94.0:**
- OSK: removed the appended `_` caret from the drawn text — the user reads
  it as a stray trailing space ("extra space at the end by default").
- Audio: `g_audio_quit` flag gates the pump-thread open-retry; teardown
  (`engine_obj_Destroy`, `gds_audio_stop`) sets it so audio no longer
  resurrects during shutdown ("late audio open succeeded" while exiting).

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

## Music intro echo — still heard on device (0.93.2): NOT an exact-prefix repeat
The 0.93.2 fingerprint probe logged `no repeat in first 2.7s -- repeat must be
earlier than 8 quanta or consumer-side`, yet the user still hears the intro
restart. Next experiment: dump the first ~400 KB of the fmod stream session
to `echo_session.pcm` in the game dir (24000Hz stereo s16), ship it, and
cross-correlate offline — catches restarts whose mixing offset is not
21ms-aligned (the strict fingerprint can't see those).
### 0.92 preroll gate (partial mitigation, kept)
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

## RESOLVED (device-confirmed 0.92): crash after company-name DONE
User verdict: "no more crash! i can play the game!" (2026-08-06 0.92 run).

## RESOLVED (0.93): idle/background SIGSEGV (stack smash in input poll)
Device evidence (0.92 log): `[input] kjoy hit-summary` printed TRUNCATED
mid-line, then `signal 11 pc=__strcpy_chk' strb wzr,[NULL+0]` from
gds_input_poll ~60s into gameplay.  Cause: the hit-summary used
`snprintf(line+p, sizeof line - p, ...)` with p unclamped; snprintf returns
the would-be length, so past 512 bytes p ran ahead of the actual write end
and the next call wrote out-of-bounds with a wrapped (huge) size into a
512-byte stack buffer -> canary trip.  Fixed: clamp p per call, buffer 1024
(a full 8-family x 24-slot summary is ~700B).

## OPEN: company name loses its first char(s) ("wertyuio" from "Qwertyuiop")
getInputPanelResult logs the text correctly, but FepPanel result [0] shows
chars dropped (0.91: "Sunny Studios"->"unny Studios"; 0.93.1: "Qwertyuiop"->
"wertyuio").  0.93.2 full-pipeline reversal (all disasm-pinned):
  FepPanel::Update -> GetFepPanelResult(0x17f4c08) ->
    KairoPlugin.fire(0x174f29c): makes state object {+0x10 delegate, +0x20
      exception, +0x28 done, +0x30 result}, queues it on [plugin+0x40],
      Thread.Sleep(1)-spins on +0x28 (param w2=1 = block).
    KairoPlugin.pump(0x1756b80): per frame, dequeues state[0], invokes
      delegate (+0x10), stores its return at state+0x30, sets done.
    GetFepPanelResult wakes, throws state+0x20 if set, casts state+0x30 to
      String[] (klass cell 0x1ebf2f8), returns it.
  => The DELEGATE does the JNI call AND the String->String[] parse; IT is
     the mangling site.  Delegate ctor-wire: GetFep caches it at compat
     statics+0xc0, target = manager singleton (klass cell 0x1ecff90
     statics[0]), MethodInfo = cell 0x1ecff98 -- runtime-only value, NOT
     resolvable from the file (0xcb0bbc resolver walks registration tables).
  Device data fits "text parsed as PREFIX+body+SUFFIX": Substring(1, L-2)
  explains 10->"wertyuio"(8); real kairo Android likely returns a packed
  form (status char + text + terminator) that the delegate unpacks, and our
  bare-text reply gets clipped on both sides.  NOTE: no Substring(int,int)
  caller and no '\n' literal exists in the manager region 0x174b000-
  0x1756c00, so the parse lives elsewhere (delegate target's class region).
0.93.2 ships the pinning probe: kind-32 at 0x17f4d08 dumps x20 (delegate)
slots [+0x08..+0x40] once -- [del+0x10] = parse fn VA (ctor 0x174d2f0
stores [MethodInfo+8] there).  One run -> disasm that VA -> exact algorithm
-> exact answer over any guessed packing.  strprobe re-arms at fetch too
(cap spent on boot strings in 0.93, conversion after fetch now captured).

## Music intro echo -- 0.92 preroll gate did NOT fix it on device
Ring is append-only/monotonic: it cannot repeat PCM, so the ~1s repeat must
be the source (FMOD/game) re-emitting.  0.93: echo-probe fingerprints every
pushed 512-frame quantum for the first 128 after a >=2s silence and reports
if the opening 8-quantum pattern recurs (+ offset in ms).  Plus preroll
arm/open/flush transition logs (cap 10).  Await next run's verdict.

## Boot splash -- REVERTED in 0.93.1 (0.93.0's blit broke boot)
0.93.0 software-blitted a BMP onto the KMSDRM GL window: SDL_GetWindowSurface
returned NULL and the touch left SDL state where the NEXT eglGetCurrentDisplay
returned nil -> Unity "no configuration matching minimum spec" -> abort.
Rule learned: NEVER touch the GL window with SDL software APIs.
User wants the real launch screen anyway: the KAIROSOFT logo Android shows
at app start (windowBackground/windowSplashScreen theme drawable in the
APK's res/, NOT in Unity data).  Deferred with user's blessing; plan:
harvest the drawable from the APK (build script or user drops the APK) and
present it in a SEPARATE temporary NON-GL SDL window during module load,
destroyed before the GL window is created.  Reference ports draw nothing
(their Unity splashes are native libunity) -- do not copy them here.
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

## 0.93.2: audio retry (silent-session fix) + force-quit chord
- DEVICE REGRESSION (0.93.1 20:18 run): `SDL_OpenAudioDevice` failed 4x
  `ALSA: ... Device or resource busy` from attempt 1 -> whole session
  silent (old code gave up after 4x250ms FOREVER).  Fix: fast burst kept,
  then the (now always-running) pump thread retries every 2s with no cap;
  success line tags the attempt number.  On a late open all players' rings
  are flushed first (no 24s stale-audio blast; preroll rearms naturally).
  Each early failure also logs a /proc diag: live loader2 process count +
  every ALSA playback substream status -> names the busy-holder next time.
- FORCE-QUIT (user request): SELECT+START now also watched by a dedicated
  thread reading evdev DIRECTLY (EVIOCGKEY current-state ioctl, node found
  by capability bits BTN_SELECT+BTN_START; no SDL threading issues, immune
  to a wedged render loop).  Chord -> graceful flag as before; chord held
  >=2s (GDS_QUITCHORD_MS) -> `_exit(0)` on the spot.  GDS_QUITCHORD=0
  disables the watcher.  Debounce = 2 consecutive 50ms reads.

## OPEN: post-naming NRE storm -> game self-Terminate
Reproducible 2x (0.92 and 0.93.1): after naming, ~2s storm of
System.NullReferenceException, raise site `bl 0xcb0de4 @ il2cpp+0x16e3724`,
then `IApplication.Terminate` (caller il2cpp+0xe90394), render-loop stop at
frame ~580, exit 0.  Storm fn pinned (0.93.2 disasm): body 0x16e3468..0x16e37e0,
guard `cbz x19 @0x16e3658` -> raise when `0xf04490(...)` returns NULL.  The
success path queries with two interned literal cells 0x1ec9248/0x1ec9228
(double-indirect: cell -> slot -> Il2CppString; values are runtime
singletons, unreadable offline) on an owner object x20 = [x21], then a
vtable call at klass+0x138.  Catch at 0x16e372c swallows it (w22=1, returns
null) -> caller retries each frame -> ~50 NREs -> Main gives up.
0.93.2 ships kind-33 probe at the raise call 0x16e3724: dumps BOTH literal
strings + owner class name.  One run names exactly what lookup fails.
Hypothesis to test when named: a UI object whose creation depends on the
broken tab/layout path (same root as small-tabs?) or a shop/name entry.

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
