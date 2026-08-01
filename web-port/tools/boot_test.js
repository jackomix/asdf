/* ============================================================================
 * tools/boot_test.js — E2E smoke test of the REAL browser scripts. Loads the
 * exact <script> set from index.html via vm.runInThisContext (same global
 * lexical semantics as <script> tags), fires DOMContentLoaded, and lets
 * boot.js do its full work: fetch payload, build VM, boot the activity,
 * rAF-pump the game loop. Optional scripted taps via TOUCH (canvas coords).
 *
 *   node tools/boot_test.js [maxRafFrames]
 *   env:
 *     FRAMEDUMP=dir   dump PNG of #screen every DUMPEVERY rAF frames
 *     DUMPEVERY=N     (default 100)
 *     TOUCH="raf:x,y;raf:x,y"   tap at rAF-frame indices (canvas coords)
 *     SAVEDUMP=1      print persistence summary at exit
 * ========================================================================== */
'use strict';

const fs = require('fs');
const path = require('path');
const nodevm = require('vm');
const ROOT = path.join(__dirname, '..');
const { installDomStubs } = require(path.join(__dirname, 'dom_stubs.js'));

const MAX_RAF = parseInt(process.argv[2] || '1500', 10);
const DUMP_DIR = process.env.FRAMEDUMP || null;
const DUMP_EVERY = parseInt(process.env.DUMPEVERY || '100', 10);

async function main() {
  console.log('== boot.js E2E (real browser scripts under Node) ==');
  const dom = installDomStubs(ROOT);
  global.PNG = require(path.join(ROOT, 'js', 'png.js'));

  /* count rAF callbacks the page schedules */
  let rafCount = 0;
  const origRaf = global.requestAnimationFrame;
  global.requestAnimationFrame = (cb) => origRaf((t) => { rafCount++; cb(t); });

  /* events */
  const touchScript = (process.env.TOUCH || '').split(';').filter(Boolean).map((s) => {
    const parts = s.split(':'); const xy = parts[1].split(',');
    return { at: parseInt(parts[0], 10), x: Number(xy[0]), y: Number(xy[1]), done: false };
  });
  const pendingUps = [];
  const stage = () => document.getElementById('stage');
  const mouse = (type, x, y) => dom.fire(stage(), type, { preventDefault: () => { }, clientX: x, clientY: y, changedTouches: [{ clientX: x, clientY: y }] });

  /* load the page scripts in index.html order, exactly like <script src> tags */
  const scripts = ['js/dex.js', 'js/png.js', 'js/vm.js', 'js/natives-core.js', 'js/natives-android.js', 'js/host.js', 'js/boot.js', 'js/aidocs.js'];
  for (const sp of scripts) {
    const code = fs.readFileSync(path.join(ROOT, sp), 'utf8');
    nodevm.runInThisContext(code, { filename: sp });
  }
  console.log('scripts loaded:', scripts.join(', '));

  /* boot the page */
  dom.fire('document', 'DOMContentLoaded');

  /* wall-clock pump: let the event loop run; watch progress */
  const t0 = Date.now();
  let lastRaf = 0;
  let dumped = 0;
  while (rafCount < MAX_RAF) {
    await new Promise((r) => setTimeout(r, 25));
    if (rafCount !== lastRaf) {
      lastRaf = rafCount;
      for (const t of touchScript) {
        if (!t.done && rafCount >= t.at) {
          t.done = true;
          console.log(`[touch] DOWN @${t.x},${t.y} raf=${rafCount}`);
          mouse('mousedown', t.x, t.y);
          pendingUps.push({ upAt: rafCount + 3, x: t.x, y: t.y });
        }
      }
      for (const u of pendingUps) {
        if (!u.done && rafCount >= u.upAt) {
          u.done = true;
          console.log(`[touch] UP @${u.x},${u.y} raf=${rafCount}`);
          mouse('mouseup', u.x, u.y);
        }
      }
      if (DUMP_DIR && Math.floor(rafCount / DUMP_EVERY) > dumped) {
        fs.mkdirSync(DUMP_DIR, { recursive: true });
        const out = path.join(DUMP_DIR, 'frame_' + String(rafCount).padStart(5, '0') + '.png');
        try { dom.dumpPng(document.getElementById('screen'), out); dumped++; } catch (e) { console.log('[dump err]', e.message); }
      }
      const eas = global.window.__eas;
      if (eas && rafCount % 300 === 0) {
        console.log(`[stat] raf=${rafCount} insns=${(eas.stats.insns / 1e6).toFixed(1)}M presents=${eas.host.frameCount} wall=${((Date.now() - t0) / 1000).toFixed(0)}s`);
      }
    }
    if (Date.now() - t0 > parseInt(process.env.BOOT_TIMEOUT_MS || '240000', 10)) { console.log('TIMEOUT'); break; }
  }

  const eas = global.window.__eas;
  if (!eas) {
    const status = document.getElementById('load-status').textContent;
    const log = document.getElementById('log-box').textContent;
    console.log('BOOT FAILED. load-status=' + JSON.stringify(status));
    console.log('log-box tail:\n' + log.split('\n').slice(-25).join('\n'));
    process.exit(2);
  }
  console.log(`== done == raf=${rafCount} insns=${(eas.stats.insns / 1e6).toFixed(1)}M presents=${eas.host.frameCount} wall=${((Date.now() - t0) / 1000).toFixed(1)}s dumps=${dumped}`);
  if (process.env.SAVEDUMP === '1') {
    try { eas.host.fsPersist(); } catch (e) { }
    console.log('localStorage keys: ' + [...localStorage._m.keys()].join(', '));
    console.log('vfs files: ' + [...eas.host.vfs.keys()].join(', '));
  }
  console.log('BOOT E2E OK');
}

main().catch((e) => { console.log('FATAL', e && e.stack || e); process.exit(1); });
