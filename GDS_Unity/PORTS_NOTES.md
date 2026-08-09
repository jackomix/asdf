# Game Dev Story (R36S port) — handoff notes

Game: `net.kairosoft.android.gamedev3en` 2.6.9, Unity 2022.3.62f2, IL2CPP arm64.
Target: R36S (ArkOS, RK3326, Mali-G31, 640×480, KMSDRM), custom ELF loader
(`GDS_Unity/loader_ref`, builds `loader2`, ships in `gamedevstory.zip`).

## 0.95.16-osk9 (swallow gate CORRECTED + probeA phys-vs-game timestamps)

- **0.95.15's release gate was NOT actually what ran on the device.**  A
  leftover `osk_swallow--` from the old fixed-18 code inside the zeroing
  block decremented the gate away regardless of release, so silence
  after close was ~1 frame in practice -- and the user's dead-A did NOT
  budge.  Two readings: (a) the swallow was probably never the cause
  (symptom persisted at 1 frame), pushing suspicion toward a game-side
  post-FEP cooldown (scene fade / tutorial typewriter text); (b) the
  experiment was not clean, so 0.95.16 makes it clean: pure release
  gate (silence until pad fully idle), capped at 8 frames as insurance
  against a noisy axis.
- **probeA: phys-vs-game timestamp pair per A press**, windowed to the
  user's scenario (15s after boot, 15s after each OSK close, ~2 log
  lines per press, quiet otherwise):
  `[probeA] PHYSICAL A down at t=...ms` (pad tail, SDL+evdev merged
  view) and `[probeA] GAME consumed A at t=...ms (+Nms)` (first
  Canvas.GetJoystickButton(slot0) read observing it).  Verdict rules:
  +N ~1-2 frames but action lags ~0.5s -> GAME-side cooldown (nothing
  left on our side to fix); consumption absent for ~0.5s -> game not
  polling during a fade/tutorial lockout, still game-side; any real
  PHYSICAL->table lag -> ours.  The gate episode line
  `[input] osk closed: game input resumed after N frame(s)` proves the
  swallow is no longer a fixed blackout.
- Ask alongside: EXACTLY when the dead press is felt (immediately after
  Company Name Done? office intro dialogue? main menu?) and the retail
  A/B: same APK on an Android phone, same moment -- if retail feels the
  same, case closed with gold-standard evidence.
- **Deploy v3 (md5-verified upload)**: the user's device kept running
  0.95.13-osk6 through two "still dead-A" reports -- banner in their
  port_launch.log proved it (loader2 still Friday's build).  The last
  deploy attempt showed zip activity at the ports dir then nothing
  changed: consistent with the truncating-upload gremlin RECURRING and
  v2's integrity test refusing (as designed) without self-healing.  v3:
  local md5+bytes are compared against the device RIGHT AFTER scp
  (`!! transfer corrupted: remote RB/RMD5 != local LB/LMD5` evidence
  printed per attempt), retried up to 5x, abort leaves the live folder
  untouched; staged version must now EXACTLY equal the expected build,
  and a successful run states `uploaded + verified (md5 matches)`.
  Mock-verified: one-corrupt-then-heal installs cleanly (saves carried
  over); always-corrupt aborts with live folder + save intact.

## 0.95.15-osk8 (SELECT fully inert + dead-A root-caused: OUR 18-frame swallow)

- **SELECT removed from the OSK entirely** (user decision 2026-08-09:
  "no curated list ... just remove the select back button entirely").
  Binding, SEL pill, `vk_cancel`, `g_orig` snapshot and the 0.95.14
  probe knob are all gone, in both styles.  The game's cancel path has
  no trigger left on the OSK; `g_negative` still parses and appears in
  the open-line log only.  START remains Done.  Asserts 80 -> 74
  (SELECT-inert now asserted with and without a negative label, both
  styles; START commits the edited text).
- **"Can't press A for ~0.5s after the game starts": NOT the game --
  ours.**  `gds_input_poll` fed a FIXED-18-frame TOTAL input blackout
  to the game after every OSK close (Terraria `g_vkbd_swallow`), ~0.3-
  0.6s dead at device frame rate.  Its sole legitimate purpose: keep
  the confirming press from phantoming into the game as an edge.
  Replaced with a RELEASE GATE: silence until the pad is fully idle
  (buttons + stick/shoulder axes), then resume instantly -- ~0ms dead
  beyond the user's own release.  Each close logs
  `[input] osk closed: game input resumed after N frame(s)` so the
  device can confirm the hold tracks release, not a fixed timer.  NOTE:
  if any A-deadness turns up somewhere NOT immediately after an OSK
  close, that's a separate beast -- report it with a log.

## 0.95.14-osk7 (cancel probe knob; cancel-branch disasm says PER-PROMPT)

- **Is the raw Android cancel (null) ever safe?  The BINARY says: it is
  per-prompt DATA, not a hardwired fatal.**  Disasm of the FepPanel
  negative branch (il2cpp file VA == offset 0x17f4e48, reached by
  `cbz x0` in FepPanel::Update 0x17f4974): static-init guarded loads of
  an object from a static slot, `cmp` gate, then at 0x17f4f74
  `cbz x19 -> skip; ldr args; b #0xf003f0` -- a guarded tail dispatch
  into a RUNTIME-REGISTERED handler.  The boot Company-Name handler is
  the fatal error-dialog one (device-proven 0.95.12); what OTHER prompts
  register cannot be known from the binary without chasing every
  registration store site through generated code -- deliberately NOT
  chased (rabbit hole).  The cheap ground truth is the device.
- **Probe knob shipped: `GDS_OSK_CANCEL=null` in gds_env.cfg** restores
  the raw Android null on SELECT-cancel (default stays the 0.95.13
  safe "back out unchanged").  Probe logs `[osk] PROBE CANCEL ...` so
  port_launch.log self-describes the experiment.  PROTOCOL for the
  device: (1) never probe at the boot prompt (known fatal -- if the
  session ends there, that's expected); (2) boot past Company Name with
  Done, rename something mid-game (e.g. a game title), press SELECT;
  (3) outcome decides: error dialog + session end => the fatal handler
  is (near-)universal => 0.95.13 uniform behavior is FINAL, document
  and done; game continues with old name => that prompt is cancel-safe
  => implement a curated title->cancel-mode table (true null cancel for
  proven-safe prompts, unchanged-text everywhere else).
- Host asserts 76 -> 80: probe on returns g_ok==0 (raw null), probe off
  restores the safe path.

## 0.95.13-osk6 (cancel can no longer kill the game + even blink + echo-dump retired)

- **Cancel redefined: NULL is game-fatal, so cancel = "back out
  unchanged".**  The 0.95.12 device run answered the open question the
  hard way: the user hit SELECT at the boot "Company Name" prompt again
  -> same `CANCEL` -> `getInputPanelResult -> (null)` -> game shows its
  own "An error has occurred." -> render-loop stop at frame 127 ->
  clean exit.  The 0.95.12 verdict "not a crash" was TRUE but did
  nothing for the player: on the device the game still ends.  Since
  the FepPanel negative path was never observed to be safe ANYWHERE,
  `vk_cancel` no longer returns NULL at all: it restores the open-time
  prefill snapshot (`g_orig`, captured post-trim) and finishes on the
  normal OK path.  Effects: SELECT at the boot prompt keeps
  "Sunny Studios" and the game CONTINUES; SELECT at a mid-game rename
  keeps the old name and UNDOES any mid-prompt edits; the error+
  shutdown path is unreachable from the OSK (classic keyboard gets the
  same protection).  This also retires the "does mid-game cancel error
  too?" question BY DESIGN: cancel never returns null, so it cannot
  error anywhere, and no device test is needed to prove it (host
  asserts: cancel restores prefill, undoes edits, reports OK).
- **Blink evened**: caret visible 500ms / hidden 500ms (was 600/400 --
  user measured the asymmetry: "invisible for shorter than it is
  visible"; correct, hidden was 40% of the cycle).
- **Echo-capture harness retired behind `GDS_ECHO_DUMP=1`**: it was the
  intro-echo evidence tool, solved since 0.95.5, yet still wrote ~1MB
  of PCM (`echo_{prod,play}.pcm`) into the game folder at EVERY launch.
  Delete the two files on the device (skippable); knob survives for
  future Kairosoft ports' audio diffs.
- Tests: 76 -> 78 asserts green; cancel now proven to restore the
  prefill AND undo mid-prompt edits (new style + classic).
- **Deploy script v2: ATOMIC install + save preservation** (device
  incident, 2026-08-09).  The user redeployed 0.95.13 and the launcher
  found `/roms/ports/gamedevstory` containing ONLY `data/`.  Root
  cause: v1's on-device flow was `rm -rf gamedevstory` THEN `unzip`,
  with the uploaded zip never re-validated on the device.  When the
  upload landed unusable (truncated transfer / tight card), unzip died
  after the archive's first entries (`gamedevstory/data/` is first in
  the zip) and the live folder was already gone -> launcher refused to
  boot ("Put the contents of the gamedevstory/ inner folder here.").
  v2: integrity-test the uploaded zip ON THE DEVICE, require >=200MB
  free (zip + staged + live coexist briefly), extract into
  `$PORTS_DIR/.gds_install`, verify staged loader2+libil2cpp exist and
  the version banner parses, THEN carry over player state and swap
  live<->staged (old tree kept as `gamedevstory.old` until the end).
  Every failure aborts with the LIVE folder untouched and prints
  `df -h` so the next report names the cause.
- **SAVES now survive redeploys.**  Loader home = `gamedevstory/home`
  (main.c `gds_home`): RecordStore save slots + shared-preferences.bin
  live there, and v1 silently wiped them at EVERY deploy -- unnoticed
  only because playtime was still at the boot Company-Name prompt.  v2
  copies `home/` and `gds_env.cfg` into the staged tree BEFORE the
  swap (replacing the old /tmp keep-file dance).
- v2 verified end-to-end locally against a mocked ssh/scp device:
  happy path (swap + saves/knobs carried over + zero leftovers) and a
  replay of the incident (truncated upload -> abort, live folder and
  saves untouched).  WHY the device upload was truncated is still
  open (prime suspect: free space on /roms after roms pile up);
  v2's bail prints `df` output so the next failure report settles it.

## 0.95.12-osk5 (SELECT "crash" SOLVED-BY-LOG + caret blink + title CR trim)

- **The "SELECT crashes the game" verdict: NOT A CRASH, and NOT the
  quit chord -- the GAME's own cancel policy.**  The user's
  port_launch.log shows the complete clean sequence: `[osk] CANCEL` ->
  `isInputPanelFinish -> 1` -> `getInputPanelResult -> (null) "canceled"`
  (exactly the Android cancel contract) -> `[jni] Kairo.showDialog
  type=0 title="Error" message="An error has occurred."` -> `Unity
  requested render-loop stop` -> nativePause -> `exited with code 0`.
  Zero faults, zero backtrace.  Canceling the boot Company-Name entry
  makes the GAME raise its own error dialog and shut itself down --
  the same thing a phone does when you press Android BACK on that
  required prompt.  Our plumbing did everything right.
- **Device log FALSIFIED a 0.95.9 dex claim:** Company-Name prompt
  passes `neg="Back"` (open line logs it), so the SEL pill + armed
  SELECT-cancel at boot were CORRECT... and the game's answer to that
  Back is error+quit.  NOTE: also `pos="OK\r"` and `title="Company
  Name\r"` -- labels carry the same trailing-CR junk the text does.
  OPEN QUESTION for device: does SELECT-cancel at a MID-GAME rename
  (e.g. "Game name") also error+quit, or return benignly?  If it
  errors too, the SEL pill is never useful and should be retired.
  (ANSWERED BY DESIGN in 0.95.13: cancel never returns NULL anymore,
  so it cannot error anywhere -- it backs out with the original text.)
- "have the cursor blink" -> caret blinks 600ms on / 400ms off via a
  shared clock (new `gds_mono_ms()` input.c export); blink snaps solid
  on open/set_text/type/backspace/caret-walk so it never hides
  mid-action (classic OSK left solid).
- Title trailing control bytes trimmed at open ("Company Name?" was
  junk from the \x0d); text trim already existed.
- Housekeeping: host harness files moved from /tmp (wiped every ~20min)
  into tools/ (test_osk_logic.c, render_osk.c, render_osk.py) with
  repo-relative includes.  76 asserts green.

## 0.95.11-osk4 (caret repeat speed + badge AA; SELECT crash PENDING LOG)

- "holding L1/R1 repeats too slow (1.5-2x please)" -> caret repeat
  initial 14f->8f, repeat 8f->4f (~2x at device frame rate).  Dpad
  repeat untouched (nav speed was praised).
- "button icons look kind of pixelated" -> badges were integer-scanline
  discs/pills (hard stair-stepped edges at 640x480).  Now coverage-AA:
  solid middle runs + fractional edge pixels blended via RRA, soft
  single-pixel shape tips; badge circle radius 8->8.5 to keep apparent
  size.  Pixel-true preview zoom shows smooth perimeters.
- **"the start button works, but going back doesn't work, it crashes the
  game" -- OPEN, need /roms/ports/port_launch.log from THAT run (before
  the next launch overwrites it).**  Three candidates, log discriminates
  instantly: (1) held SELECT+START ~2s tripped the user-approved quit
  chord (log: "SELECT+START held ... graceful exit") = looks like a
  crash, is not; (2) OSK SELECT-cancel path (log: "[osk] CANCEL" then a
  fault) -- never device-exercised before 0.95.10 because SELECT never
  arrived; (3) game-side SELECT (kairo slot 10 / Unity 360/109) live for
  the first time ever (in-game Back was always NPB_B -> KC_ESCAPE, which
  still works).  Loader has a SIGSEGV/SIGBUS/SIGILL/SIGTRAP handler with
  backtrace to stderr, so a native fault self-describes.  Code review of
  the cancel path shows it returns plain JNI null = exactly the Android
  cancel contract; no suspected mechanism found on host yet.

## 0.95.10-osk3 (OSK v3 + START/SELECT input fix, user feedback round)

User device-tested 0.95.9 ("this is actually really good") with another
feedback list.  Every point -> change:

- **"Start and select don't actually do anything when I press them"** --
  INPUT BUG, not an OSK wiring bug: `gds_osk_pad_tick` already mapped
  NPB_START->commit / NPB_BACK->cancel, but the bytes never arrived.
  Root cause was already proven by our own 0.94.0/0.95.0 device runs
  ("the SDL-side chord never fired: whatever physical SELECT reports as,
  it is not SDL BACK"; the quit chord only works because the evdev
  watcher thread polls codes 0x2c0/0x2c1 straight off the 'GO-Super
  Gamepad' node via EVIOCGKEY).  Something between evdev and SDL's
  button fill eats those two keys (generic mapping says back:b8/start:b9;
  device evidence says no).  FIX = watcher shadow: `evdev_chord_down()`
  now also publishes g_ev_sel/g_ev_sta/g_ev_live each 50ms poll, and
  `pad_poll()` OR-merges them into g_npb[NPB_BACK/START] -- AFTER its
  npb-chord block, so the chord path (user-approved, DO NOT TOUCH) keeps
  reading exactly the SDL-only bytes it always read; timing/behavior
  unchanged (2s watcher path fires first as before).  First-press roll /
  GDS_PADLOG stay SDL-view diagnostics on purpose.  Side effect: kairo
  slots 360/361 + Unity 108/109 (game-side SELECT/START) now also see
  real presses for the first time -- GDS mobile ignores them, harmless.
- **SELECT cancel gating**: with the key suddenly real, vk_cancel is now
  gated on the prompt's negative_ label (new style).  Unadvertised
  cancel (e.g. company-name boot prompt, negative_="" dex-verified)
  could re-prompt-loop the game, so SELECT is inert there -- matches the
  UI, which only shows SEL when offered.  Classic OSK keeps old
  semantics (SELECT always cancels); classic gains START=Done.
- "no caps visual indicator, maybe the caps button can turn white" ->
  caps lock = Shift key face WHITE + dark text + dark badge; one-shot =
  lit gray (0x6a->0x7a).  Layer semantics now PHYSICAL: one-shot shift
  applies the full layer, **caps lock uppercases letters only** (a real
  Caps Lock never touches the number row).
- "selected colours look odd, outline wraps the sides of the shadow but
  not underneath" -> selection is a 2px FOCUS RING + 10% white face lift
  (no more inverted white face: white is now caps-only).  v2's ring rect
  covered to y+h+2.5 = a hidden 0.5px sliver under the shadow -- exactly
  the "legs on the sides, no bar underneath" they saw; v3 covers
  y+h+4 = full 2px bottom bar.  Ring goes DARK on the caps-white key so
  selection never washes out (PS5 lesson).
- "highlight transition isn't seamless, both keys light then old
  dehighlights (esp. Done)" -> CANNOT reproduce or explain in code (one
  g_sel, one draw loop, one highlight); likely a 1-frame tearing/ghost
  line between the letters rows and the fn row, made loud by the white
  inverted face + white ring jumping together.  Mitigation shipped:
  ring-only selection means a ghosted frame now shows a thin 2px ring on
  the old key instead of a big white face.  Honest status: tell us if it
  still bothers on device; next lever would be dpad repeat rate.
- "symbols page is weird, barely anything there, why shift-pairs there?
  put shift symbols on the letters page's numbers + bottom punct" ->
  DONE, user's exact suggestion: symbols page is FLAT now -- all 32
  printable ASCII punct, 4x8 grid of 62px keys exactly filling the grid
  span (pairs grouped: () -_ =+ `~ [] {} \| ;: '" ,< .> /?, bang-row
  !@#$%^&*).  Shift pairs moved to the LETTERS page digit row + bottom
  punct row with the other-layer glyph small top-left (physical
  keyboards don't print a small 'A' on 'a', so letters show nothing).
  Shift/X still cycle globally on both pages (uniform state, no inert
  keys); "#+=" flips to "ABC" on the symbols page as before.
- "ABXY badges are white-with-black but start/bumper pills are
  black-with-white" -> pills unified: light face + dark text, mid-gray
  hairline edge.  Badges invert (dark disc) ONLY on the caps-white key.
- "B symbol sits on top of Del" -> badge tucked to (x+11,y+11) r=8, "Del"
  label nudged down+right.  Still a ~1-2px kiss on a 48px key; user
  already called this a possible necessary evil.
- "select thing on the bottom looks ugly, it's the only thing there" ->
  bottom band deleted; SEL pill + negative label now sit in the title
  band just left of n/max, only when the prompt offers cancel.
- "keyboard not centred vertically, a bit too high" -> whole panel +8px
  (screen margins now 42/42; all row/fn Y constants shifted in the
  generator, single source of truth).
- "title slightly higher than the counter's line" -> title + n/max now
  share one optical centre (cell-centre math at both scales).
- "max-length shake not noticeable; cut the counter to RED then fade
  back" -> exactly that: cut to red instantly, fade to gray across the
  shake; shake 14->22 frames, 5px->8px amplitude.  Red is the ONE
  sanctioned colour exception to the grayscale rule -- user requested.
  Also: fresh prompt no longer inherits a mid-decay flash (g_shake=0 in
  gds_osk_open).
- "darken the background a tad more" -> scrim 0.68 -> 0.74.
- Retracted by user, no action: single-quote rationale; L1/R1 pill
  placement ("I'm kind of used to it now" -- kept as-is).

Evidence on host: 74-assert logic suite green (shift layer truth table
incl. caps-letters-only + one-shot full layer, flat symbols no-pairs,
10<->8 page-flip mapping + col clamp + fn<->fn, caret ops, START/SELECT
gating incl. classic, maxlen).  Pixel-true render harness regenerated
preview_osk_{letters,shift,symbols,caps}.png from the REAL C draw path
(caught the stale-shake bug in scene 3's counter!).  Device: watch for
`[input] force-quit watcher on ...` (shadow source live) and exercise
START=Done at the company-name prompt; SELECT there should do NOTHING
(no pill, inert) while a prompt WITH a negative label shows SEL<->cancel
in the title band (open line logs pos=/neg=).

## 0.95.9-osk2 (OSK v2: monochrome + console conventions, user feedback round)

User feedback on 0.95.8 + "do a shit ton of research" request.  Research done
(Xbox 2024 gamepad layout screenshots/articles, Steam Deck Big Picture
keyboard screenshots, Switch OSK, PS5 OSK reddit complaints), then every user
point was implemented; research only filtered HOW (not WHAT).  Key findings:
Xbox's new layout is vertically-aligned grid w/ button-glyph badges on key
corners (X/Y on keys, LT/RB on <> caret keys) and Microsoft's X=backspace/
Y=space bindings; Deck shows shift PAIRS on symbol keys (small glyph above),
dark translucent panel; PS5's redesign was PANNED for low contrast + removing
button color/icons -> selection must stay high-contrast even when mono.

Every user point -> change:
- "no colour at all, monochromatic, aesthetically agnostic" -> full grayscale
  palette (charcoal gradient panel #2e2e34->#141418, gray keys, white text);
  selection = INVERTED (white face + dark text, PS5-contrast lesson).
- "fn row doesn't align; space bar 3.5u" -> function row snapped to the grid:
  Shift 2u, #+= 2u, Space 3u, Del 1u, Done 2u (sums exactly to 10 cols).
- "keys spaced vertically not horizontally" -> gap 3px->8px horizontal.
- "bottom right dead space, put quotes/periods there" -> letters page rows
  now 10-wide: `asdfghjkl'` and `zxcvbnm,.-`.
- "space too wide" -> space advance 18.5 (2.3x!) -> natural 8.0 (tool bug).
- "everything is caps" -> labels title-case (Shift/#+=/Space/Del/Done),
  title shown as the game sends it, "Caps" label only when locked.
- "bottom legend is crappy, draw the button glyph on the key's corner" ->
  legend deleted; badges drawn with rects: light circle + dark letter
  (X on Shift, Y on Space, B on Del), dark pills (START on Done,
  L1/R1 gripping the text-box edges); badge inverts on selected keys.
- "maybe L and R move the cursor left and right" -> REAL caret: L1/R1 walk
  it (edge+repeat), insert/backspace at caret, auto-scroll keeps caret
  visible; page flip now lives ONLY on the #+= key.
- "gradient is just 4 strips" -> per-1px-row lerped gradient (396 rects).
- "background a little more darkened" -> scrim alpha 0.55 -> 0.68.
- "text a bit too bold" -> atlas font DejaVuSans-Bold -> DejaVuSans (Book).
- "counter should shake when typing at max" -> g_shake=14 frames of decaying
  alternating x-offset on the n/max counter (fails insert, exact ask).
- "shift does nothing on symbols page" -> symbols page = physical-kb shift
  pairs (1!, 2@, ... -_ =+ [{ ]} ;: '" ,< .> /? \| and lone `~); small
  sibling glyph drawn above (Deck style); typing shifted emits hi glyph,
  one-shot/lock semantics shared with letters.
- "instead of symbols text, Nintendo uses glyph" -> page key label "#+="
  (Nintendo Switch's convention) / "ABC" when on symbols page.
- "SEL cancel does nothing here" -> INVESTIGATED in classes.dex:
  Utility$3$1.onCancel stores text + ok=false; FepPanel.startInputPanel
  takes positive_/negative_ button label strings; getInputPanelResult
  returns StringUtil.getString(result_) or null.  So prompts are
  INDIVIDUALLY cancellable; company-name at boot passes negative_="" -> on
  Android there was no cancel button either; the old SEL hint was a lie.
  Now: loader passes negative_ through (gds_osk_set_negative), OSK draws a
  "SEL <label>" pill centered in the bottom band ONLY when offered, and the
  open log prints pos="/neg=" so future prompts can be checked.

Host evidence: 60-assert logic suite green (caret ops, pairs, shake flag,
page-flip-only-via-key, negative API, latch, nav grid); pixel-true render
harness (real C draw -> preview_osk_letters/symbols/caps.png) caught the
letters-page pair clutter (pair display now symbols-page-only).  NOT yet
device-verified -- watch `[osk] font atlas live` and the open line's
pos=/neg= fields in port_launch.log.

Deferred from earlier notes: ~0.5s dead-A after DONE (likely game-side),
big tab glyphs (not in this build), dump_hooks.py automation.

## 0.95.8-osk (full OSK overhaul; single-source-of-truth generator tool)

User brief (2026-08): the OSK was a verbatim Terraria/Prizefighters port and
"just kind of crappy" — pixel font, midnight/gold colors, staggered QWERTY
with no number row, X shift that latched AND typed a space at the same time,
backspace re-derived caps from the letter you deleted onto, no '#' glyph at
all ("Game #1" untypable), panel bottom-anchored.  PC/console editions of
GDS show a dark gradient input WINDOW with gamepad glyphs — hunted for it in
this APK: does NOT exist.  The mobile build has only `FepPanel` -> OS-level
EditText IME (`kairo/android/plugin` framework, shared by every Kairosoft
game); the "dark window" memory is the system IME chrome.  No leanback/
gamepad features in the manifest at all.  So the look had to be built, not
enabled.  User picks (ask_user): charcoal + blue accent style, clean rounded
sans font.

**Generator tool `tools/make_osk_font.py` = single source of truth.**  One
run rasterizes DejaVu Sans Bold @24px into a 320x320 RGBA glyph atlas
(`ports/.../osk_font.rgba`, shipped in the zip), and emits
`loader_ref/osk_font_data.h` (metrics + per-glyph advances) and
`loader_ref/osk_layout.h` (palette, panel/box/positions, key tables for
letters 36 + symbols 40 + func row 5) — the C draw and the mock can never
drift apart.  DejaVu license file ships in `licenses/` (permissive; bitmap
derived work, renamed/notices kept).

New keyboard (default): charcoal 4-band gradient panel near screen centre
over a 0.55 scrim, skeuomorphic bevel keys (shadow/face/lit top/dark bottom),
blue accent for selection/caret/DONE.  Real number row, aligned 10-wide grid
(user hated dpad-ing across staggered rows).  Letters/symbols pages toggled
by **L1/R1** or the SYM key; '#' and full 0x20..0x7E included.  Shift is now
a cycle: X or SHIFT key goes off -> one-shot -> CAPS LOCK -> off; a typed
letter spends one-shot; space does not; **backspace never touches shift**.
Open arms one-shot when the text is empty/ends in space (sentence case like
a phone), and opens on the symbols page when the prefill ends in a symbol.
Classic Terraria keyboard stays verbatim behind `GDS_OSK=classic`, and is
also the automatic fallback when `osk_font.rgba` or the GL text path is
missing (one log line).

GL seam (egl_shim.c, after overlay_rect): `gds_egl_overlay_rect_a` (alpha
rects/scrim), `gds_egl_overlay_atlas` (one-time text program aPos/aUV +
uTex/uColor, coverage from tex alpha; lazy compile on the render thread like
the splash), `gds_egl_overlay_quads` (batched y-down pixel quads).  Extra
syms (glGenTextures/glTexImage2D/glActiveTexture/glBlendFunc[Separate]) come
from the same RTLD_NOLOAD libGLESv2 pattern as splash_gl_load.  Every
binding Unity could observe is saved/restored — including the *blend
factors*, which plain begin/end did NOT save (glBlendFunc is global state):
rect_a/quads snapshot the 4 factors + restore via glBlendFuncSeparate.

Evidence before shipping: host logic test (`#include osk.c` + stubbed GL,
39 asserts) — latch swallow, one-shot/caps-lock cycle, backspace/shift
independence, L1/R1 pages, '#' typable, maxlen clamp, DONE/START commit,
SELECT cancel, aligned-grid dpad nav (right 1->2, down 2->w->s->x->SHIFT);
all pass.  Pixel-true render harness (`render_osk.c` records the real C
draw's rects/quads, `render_osk.py` rasterizes with the real atlas ->
`preview_osk_letters.png` / `preview_osk_symbols.png`) — caught the footer
hint overflowing the panel (745px into a 580px panel), fixed by the tool's
double-space hint string + centered 0.48 scale.  NOT yet device-verified;
mock-level confidence is high but text rendering is new GL (texture +
blending in the overlay path), so watch `[osk] font atlas live` in the log
on first open.

Deferred (user-mentioned, not in 0.95.8): ~0.5s dead-A right after DONE
(probably game-side tutorial fade-in — said it may "actually just be the
game"); big tab glyphs (other editions' UI, none in this build).

## 0.95.7-bindfix (splash VISIBLE + one-line crash regression fixed)
0.95.6 device run, in the user's words: "it displayed the kairosoft logo and
then crashed".  Both halves confirmed and handled:

- **SPLASH WORKS:** draw witness `px=fff202ff` = the KAIROSOFT wordmark
  yellow at dead center — the ES1 fixed-func path draws correctly and the
  multi-present put it on the panel across the boot gap (presents #1
  window-ready … #4 nativeResume OK all logged).
- **CRASH = my one-line regression, diagnosed by the log itself:** 0.95.6's
  present function added `SDL_GL_MakeCurrent(NULL)` after every present.
  The window-ready call site runs BEFORE `gds_capture_real_egl()`, whose
  real-EGL discovery reads the CURRENT SDL binding (the 0.68 precondition,
  even commented in-tree).  With the binding released: `real dpy=(nil)` →
  `no real EGLDisplay` → `real_cfg=(nil)` → SIGTRAP/abort when Unity asks
  for its EGL config.  Fix: `leave_current` parameter — window-ready keeps
  the share root bound (exact 0.95.5 semantics, which booted fine); the
  later milestone re-presents release it so the render thread's shrswap can
  never hit EGL_BAD_ACCESS.  PLUS defensive hardening:
  `gds_capture_real_egl` now rebinds the share root itself at entry, so its
  precondition never depends on callers again.

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
