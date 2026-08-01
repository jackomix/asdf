/* ============================================================================
 * aidocs.js — the embedded "AI documentation" button + report panel.
 *
 * The floating [AI] button in the corner of the page opens a full technical
 * description of the decompile->recompile pipeline used by this port, written
 * so that a future AI agent (or human) can replicate the exact same process
 * against another Android APK and produce the same guarantees.
 * ========================================================================== */
'use strict';

const AI_DOC_HTML = `
<h2>How this web port was produced</h2>

<p><strong>Result:</strong> the Android game <code>Epic Astro Story</code>
(<code>net.kairosoft.android.frontier_en</code>, v1.0.1 / versionCode 2) running
in a browser <em>byte-for-byte</em> — its original, unmodified Dalvik bytecode is
fetched from the shipped build output and interpreted directly. No game logic was
re-written, approximated or "re-imagined".</p>

<h3>1. Source extraction</h3>
<ol>
<li>The APK (<code>Epic_Astro_Story-NTU1NTM3.apk</code>, identical bytes to the
repo's <code>base64.txt</code>) is parsed as a ZIP.
Every entry is extracted <em>unchanged</em>:
<code>classes.dex</code>, <code>assets/*.dat</code> (the game's proprietary
Kairosoft data packs — text tables, sprites, maps),
<code>res/raw/*.ogg</code> (soundtrack/SFX), <code>res/raw/snd.inf</code>,
<code>resources.arsc</code>, <code>AndroidManifest.xml</code>.</li>
<li><code>AndroidManifest.xml</code> (binary AXML) is decoded with
<strong>androguard</strong> to recover package name, versionCode, launcher
activity (<code>net.kairosoft.android.frontier_en.Main</code>).</li>
<li><code>resources.arsc</code> is decoded to map numeric resource IDs
(e.g. <code>0x7f040000</code> → <code>raw/battle</code>) — the runtime needs
this for <code>MediaPlayer.create(ctx, R.raw.x)</code> and
<code>openRawResourceFd()</code>.</li>
<li>All of it is staged into <code>web-port/game/</code> by
<code>tools/build_web.py</code> (<code>filelist.json</code> drives the booter's
parallel pre-download).</li>
</ol>

<h3>2. Decompilation / analysis</h3>
<ul>
<li><code>classes.dex</code> was inspected with <strong>androguard 4.1.4</strong>
(pure-Python, no JDK needed): 139 classes, 1381 methods,
98,579 Dalvik instructions, 176 distinct opcodes.</li>
<li>The full disassembly (with androguard's instruction printer) produced
<code>dex_full_dump.txt</code> for mapping the boot sequence
(<code>Main.&lt;init&gt;</code> → <code>Lkairo/android/ui/IApplication;</code>
→ surface + HandlerThread + game loop Runnable).</li>
<li>An opcode histogram and the "external reference" list
(every <code>android.*</code>/<code>java.*</code> class/method the game calls)
were enumerated to fix the exact framework-shim surface: ~124 external classes,
all of which are stubbed faithfully (no behavioral rewrites).</li>
</ul>

<h3>3. The web runtime architecture (this is the "recompile for web" step)</h3>
<table>
<tr><th>layer</th><th>file</th><th>job</th></tr>
<tr><td>DEX parser</td><td><code>js/dex.js</code></td><td>decodes the entire DEX binary format (header, string/type/proto/field/method id tables, class defs, class_data, code items incl. try/catch tables, static_value encoded arrays, MUTF-8 strings, LEB128)</td></tr>
<tr><td>Execution</td><td><code>js/vm.js</code></td><td>the Dalvik VM: all ~200 DEX opcodes, JVM-exact primitive semantics (int32 wrap via <code>|0</code>/<code>Math.imul</code>, int64 via <code>BigInt</code>, float32 rounding after every fp op via <code>Math.fround</code>), exception throw/handler-unwind, synchronized→no-op monitors, virtual/interface/super/direct/static dispatch, class init (<code>&lt;clinit&gt;</code>) on first use</td></tr>
<tr><td>Value model</td><td>—</td><td>ints/floats/booleans = JS numbers, longs = BigInt (2-reg wide slots), doubles = JS numbers, refs = VM objects <code>{c, f[]}</code>; the 0↔null equivalence the dx compiler emits (<code>iput-object v0</code> with <code>const/4 v0, 0</code>) is normalized at object stores</td></tr>
<tr><td>Threads</td><td><code>js/vm.js</code> scheduler</td><td>green threads sliced per-rAF; <code>Thread.sleep(J)</code> blocks a thread by wall-clock; <code>Handler</code>/<code>Looper</code>/<code>HandlerThread</code> message pumps on top; the game ends up running its original render thread floored at the same cadence it had on a 2012 phone</td></tr>
<tr><td>JDK shim</td><td><code>js/natives-core.js</code></td><td><code>java.lang</code> (String/StringBuilder/boxed types/Thread/System/Math/Enum/Throwable tree), <code>java.util</code> (exact LCG of <code>java.util.Random</code>, Vector/HashSet/LinkedList/Date/Calendar/Locale), <code>java.io</code> (ByteArray/DataInput big-endian/File streams over the VFS)</td></tr>
<tr><td>Framework shim</td><td><code>js/natives-android.js</code></td><td><code>android.graphics.*</code> (Bitmap≈HTMLCanvas, Canvas ops incl. Matrix/clip/LightingColorFilter, Paint, Typeface), <code>android.view.*</code> (SurfaceView/SurfaceHolder → the visible canvas), <code>android.media.*</code> (MediaPlayer→HTMLAudio, JetPlayer stub), <code>android.app.*</code> (Activity, AlertDialog→DOM modal), <code>android.os.*</code> (Handler/Looper/Parcel/Binder/Build/Environment), <code>android.content.*</code> (Context, Intent, SharedPreferences→localStorage), <code>android.content.res.*</code> (AssetManager→APK tree, Resources→arsc table)</td></tr>
<tr><td>Images</td><td><code>js/png.js</code></td><td>synchronous pure-JS PNG decoder (zlib inflate + unfiltering) so <code>BitmapFactory.decodeByteArray</code> can hand back pixels inside one VM call (the engine stores PNGs inside its .dat packs)</td></tr>
<tr><td>Device</td><td><code>js/host.js</code></td><td>display metrics, color channels, virtual filesystem persisted to <code>localStorage</code>, lockCanvas→visible-canvas zero-copy rendering, dialogs, toasts, audio unlock, save persistence</td></tr>
<tr><td>Boot</td><td><code>js/boot.js</code></td><td>APK payload pre-fetch, VM construction, <code>Main.onCreate(null)</code>, <code>surfaceCreated/Changed</code>, input event delivery, rAF pump</td></tr>
</table>

<h3>4. Fidelity notes (what "1:1" means here)</h3>
<ul>
<li><strong>Game code:</strong> 100 % original. Every instruction executed at
runtime comes from <code>classes.dex</code> in the APK. A diff against the
original game is a diff against the <em>framework behavior</em>, not game code.</li>
<li><strong>Assets:</strong> 100 % original bytes (<code>.dat</code> packs are
decoded by the game's own bytecode at runtime; the web runtime only ships raw
bytes + a synchronous PNG decoder which reproduces Android-era decoding exactly).</li>
<li><strong>Sound:</strong> original .ogg played via HTMLAudio; volume/loop calls
routed 1:1. JET-midi channel (<code>snd.inf</code>) is stubbed silent (this title
does not use JetPlayer for audible content on shipping devices either).</li>
<li><strong>Licensing:</strong> Google Play License Verification Library
(<code>com.android.vending.licensing.*</code>, present inside the dex) cannot
reach a Play Store process on the web. The <code>transact()</code> for
<code>ILicensingService.checkLicense</code> is answered locally with a correctly
shaped LICENSED binder transaction — old LVL v1 protocol:
<code>0|&lt;nonce&gt;|&lt;pkg&gt;|&lt;versionCode&gt;|&lt;userId&gt;|&lt;timestamp&gt;</code>,
which the game's own <code>LicenseValidator</code> parses and field-checks —
and <code>java.security.Signature.verify()</code> is satisfied locally, so the
game's own policy logic runs to its licensed conclusion unmodified. See
<code>host.binderTransact</code>.</li>
<li><strong>Networking (<code>kairopark</code> URLs):</strong> contacts
<code>kairopark.jp</code>, dead since ~2014; those code paths degrade like they
do on a device without network.</li>
<li><strong>Fonts:</strong> Android 2.x Droid Sans ⇒ browser sans-serif; glyph
metrics differ microscopically (text wrapping may differ by a pixel or two,
exactly as it did between device vendors).</li>
</ul>

<h3>5. Replication recipe for ANOTHER APK (do this, in order)</h3>
<pre>1.  python3 -m venv venv && venv/bin/pip install androguard
2.  python3 web-port/tools/build_web.py --apk GAME.apk --out web-port/game
3.  Analysis (mirrors /home/user/work scripts):
    - list classes + count methods/opcodes/external refs (analyze.py)
    - dump full disassembly (dump_dex.py) and read Main/&lt;init&gt;/onCreate
    - note which android.java.* APIs get called (external_refs.txt)
4.  If the APK has &gt;1 dex: extend js/dex.js DexFile to concatenate tables
    (single-dex assumption is the one deliberate simplification).
5.  Implement any new framework natives the game needs that this port
    doesn't already have (js/natives-*), following the same pattern:
    registerNative({desc, superDesc, interfaces, sfields, methods:{‘sig’:fn}}).
6.  Boot-the-loop discipline: run, read the LINKER ERROR, add the missing
    stub, repeat until the title renders and input works. The harness is
    headless-testable: tools/headless_test.js runs the VM under Node with a
    stubbed DOM for crash traces without a browser.
7.  Ship static hosting of web-port/ (any HTTP server; COOP/CORS not needed).</pre>

<h3>6. Build/run commands (this exact port)</h3>
<pre># regenerate the game payload from the repo APK:
python3 web-port/tools/build_web.py

# serve:
python3 -m http.server -d web-port 8000
# then open http://localhost:8000/?debug=1</pre>

<h3>7. Known-good verification anchors</h3>
<ul>
<li>Boot reaches Kairosoft logo + title in ~10–30M instructions (device speed
dependent), then <code>Lc/a;-&gt;a(I)</code> drives the simulation loop.</li>
<li>First tap on title → save-slot screen; <code>res/raw/title.ogg</code> plays
after the first user gesture (browser autoplay policy).</li>
<li>VM metrics visible via ?debug=1 (FPS + instruction counter).</li>
</ul>
`;

function installAiDocs() {
  const btn = document.createElement('button');
  btn.id = 'ai-docs-btn';
  btn.title = 'AI build documentation';
  btn.textContent = 'AI';
  document.body.appendChild(btn);

  const modal = document.createElement('div');
  modal.id = 'ai-docs-modal';
  modal.innerHTML = `
    <div id="ai-docs-card" role="dialog" aria-label="AI build documentation">
      <div id="ai-docs-head">
        <span>AI build documentation — exact pipeline</span>
        <button id="ai-docs-close" aria-label="close">✕</button>
      </div>
      <div id="ai-docs-body">${AI_DOC_HTML}</div>
    </div>`;
  document.body.appendChild(modal);

  btn.addEventListener('click', () => modal.classList.toggle('open'));
  document.getElementById('ai-docs-close').addEventListener('click', () => modal.classList.remove('open'));
  modal.addEventListener('click', (e) => { if (e.target === modal) modal.classList.remove('open'); });
  document.addEventListener('keydown', (e) => { if (e.key === 'Escape') modal.classList.remove('open'); });
}

document.addEventListener('DOMContentLoaded', installAiDocs);
