# Epic Astro Story — byte-exact web port

The Android game **Epic Astro Story** (Kairosoft, `net.kairosoft.android.frontier_en`,
v1.0.1 / versionCode 2) running in a desktop/mobile browser **with zero game-code
changes**: the original `classes.dex` Dalvik bytecode from the APK is fetched and
interpreted directly by a purpose-built in-browser Dalvik VM. Nothing was
re-implemented, remade or approximated — every instruction executed at runtime
comes byte-for-byte from the original game, and the game's own engine code does
all loading, simulation, rendering, input, sound and saving.

Tap/click the small floating **`[AI]`** button on the page for the full technical
write-up of the decompile → recompile pipeline (aimed at future AI agents).

---

## Run it

Any static file server works. From the repo root:

```sh
python3 -m http.server -d web-port 8000
# then open http://localhost:8000/
```

Useful query parameters:

| param     | meaning                                                     |
|-----------|-------------------------------------------------------------|
| `?debug=1`| FPS + instruction counter HUD, VM console button            |
| `?w=&h=`  | display size (default `480x320`, the game's native HVGA)    |
| `?density=`| DisplayMetrics density (default `1.0`)                     |

**Controls:** touch/click = the game's own touch handling (menus, the map,
dialog advance). Keyboard: `Esc`/`Backspace` = Android **BACK**, arrows + Enter
= DPAD, `F1` = MENU. First click/tap unlocks audio (browser autoplay policy).

**Saves:** the game's own autosave/slot files persist in `localStorage`
(`eas.vfs`), and Kairosoft config in `eas.prefs.*`. Continue works across
Reloads.

## What you should see

1. Loading screen (APK payload pre-fetch, ~4 MB).
2. Kairosoft / Epic Astro Story logo splash, then the title screen
   (`Start`, `Hi Scores`, softkeys `Site`/`Finish`, attract-mode mascot flying).
3. `Start` → save-slot select → `Empty` → intro dialogue → the home-planet
   colony sim (touch to place buildings, open menus, etc.).

## Layout

```
index.html            page shell; loads js/* in order
css/style.css         stage fit, HUD, log panel, AI-docs modal
js/
  dex.js              DEX binary parser (tables, class data, code items, MUTF-8)
  vm.js               Dalvik interpreter: ~200 opcodes, exact primitive
                      semantics (i32 wrap, Math.imul, BigInt long, fround
                      float32), exception unwind tables, green-thread scheduler
  natives-core.js     java.lang/java.util/java.io shim (host-implemented)
  natives-android.js  android.app/os/view/graphics/media/webkit shim
  png.js              synchronous pure-JS PNG decoder (zlib + unfilter)
  host.js             the "device": display/canvas/surfaces, VFS→localStorage,
                      SharedPreferences→localStorage, MediaPlayer→<audio>,
                      binder/Parcel, local LVL licensing-service stub, dialogs
  boot.js             payload fetch, activity boot, input delivery, rAF pump
  aidocs.js           the [AI] floating documentation button
game/                 APK payload, extracted byte-identical by tools/build_web.py
  files/classes.dex         original bytecode (493,020 bytes)
  files/assets/*.dat        original Kairosoft data packs (sprites/maps/text)
  files/res/raw/*.ogg       original soundtrack + SFX (+ snd.inf playlist)
  app.json, resources.json, filelist.json   manifest/arsc/index
tools/
  build_web.py        APK → game/ extractor (androguard for manifest/arsc)
  headless_test.js    Node harness: boots the VM directly on a stubbed DOM;
                      scripted taps, virtual clock, PNG frame dumps, probes
  boot_test.js        Node E2E: runs the REAL index.html script set + boot.js
                      via runInThisContext (catches browser-only bugs)
  dom_stubs.js        shared Node DOM/canvas/audio/fetch/localStorage stubs
  swcanvas.js         software-rasterizing Canvas2D for the harnesses
  link_audit.js       static linker audit (every referenced method/field resolvable)
```

## Verification anchors (automated, headless)

* `node tools/headless_test.js 800 620` — boots the VM; reaches the title
  screen and presents one frame per pump step.
* `node tools/boot_test.js 900` — boots via the real browser scripts;
  `BOOT E2E OK`, ~1 present/rAF.
* `node tools/audio_loop_test.js` — boots the real scripts, taps once to
  unlock audio, waits for the title BGM MediaPlayer, simulates track end and
  asserts the game's own `onCompletion` restarts it
  (prints `AUDIO LOOP E2E OK`).
* With `TOUCH="500:240,218;800:240,200"` theses drive the title
  (`Start` → slot screen → new game); `FRAMEDUMP=/tmp/frames` records PNGs;
  `SAVEDUMP=1` shows persisted save files.

## Fidelity notes

* **Licensing (LVL):** the Play Store licensing service obviously does not
  exist in a browser. The binder transaction for
  `ILicensingService.checkLicense` is answered locally with a correctly shaped
  LICENSED reply (old LVL v1: `0|nonce|pkg|versionCode|userId|timestamp`) that
  the game's *own* `LicenseValidator`/`Policy` code validates and accepts.
* **Networking:** Kairosoft's online ranking/registration endpoints
  (kairopark.jp) died ~2014; those paths degrade exactly as they do on a phone
  with no coverage.
* **Fonts:** Droid Sans ⇒ browser sans-serif; engine-drawn bitmap text is
  pixel-exact (it comes from the original sprite packs).
* **Sound:** `.ogg` music/SFX through `HTMLAudioElement`, volumes routed 1:1;
  `snd.inf` selects them. The game never calls `MediaPlayer.setLooping` —
  its audio wrapper (`kairo/android/ui/a`) loops BGM by registering itself as
  `OnCompletionListener` and doing `seekTo(0)`+`start()` in `onCompletion()`
  (verified by dex disassembly), so the host maps HTMLAudio `ended` to a
  real `onCompletion` dispatch into the VM, and the game's own unmodified
  restart code runs. The vestigial JetPlayer channel is stubbed silent.
* **Rendering:** Android's default `Paint.filterBitmap=false` is honored, so
  scaled sprite draws sample nearest (a real-browser-only bug had them
  bilinear-smoothed — the software canvas used in tests ignores smoothing).
  The page CSS deliberately uses only `image-rendering: pixelated` on the
  game canvas: Chrome/Safari implement `crisp-edges` as smooth scaling,
  which would silently re-blur the upscale if listed after `pixelated`.

## Provenance / rights

Everything under `game/` is the original, unmodified content of the provided
APK (reference bytes identical to the repo's `base64.txt`), used here to drive
a compatibility runtime for the rightful owner of that copy. The runtime code
under `js/`, `css/`, `tools/`, `index.html` is new work for this port.
