/* ============================================================================
 * tools/audio_loop_test.js — E2E proof that BGM loops. The game NEVER calls
 * MediaPlayer.setLooping (verified by dex disassembly: kairo/android/ui/a
 * plays a track, registers itself as OnCompletionListener, and its
 * onCompletion() does seekTo(0)+start() to loop). Host wiring must therefore
 * deliver the HTMLAudioElement 'ended' event into the VM as onCompletion().
 *
 * This test boots the REAL index.html script set under Node DOM stubs, taps
 * once (gesture unlocks WebAudio), waits for the title BGM MediaPlayer to be
 * playing, simulates the natural track end, then asserts the game itself
 * restarted playback (seekTo(0) + start()) with no errors.
 *
 *   node tools/audio_loop_test.js
 * ========================================================================== */
'use strict';

const fs = require('fs');
const path = require('path');
const nodevm = require('vm');
const ROOT = path.join(__dirname, '..');
const { installDomStubs } = require(path.join(__dirname, 'dom_stubs.js'));

async function main() {
  console.log('== audio loop E2E (real browser scripts under Node) ==');
  const dom = installDomStubs(ROOT);
  global.PNG = require(path.join(ROOT, 'js', 'png.js'));

  let rafCount = 0;
  const origRaf = global.requestAnimationFrame;
  global.requestAnimationFrame = (cb) => origRaf((t) => { rafCount++; cb(t); });

  const stage = () => document.getElementById('stage');
  const mouse = (type, x, y) => dom.fire(stage(), type, { preventDefault: () => { }, clientX: x, clientY: y, changedTouches: [{ clientX: x, clientY: y }] });

  const scripts = ['js/dex.js', 'js/png.js', 'js/vm.js', 'js/natives-core.js', 'js/natives-android.js', 'js/host.js', 'js/boot.js', 'js/aidocs.js'];
  for (const sp of scripts) nodevm.runInThisContext(fs.readFileSync(path.join(ROOT, sp), 'utf8'), { filename: sp });

  dom.fire('document', 'DOMContentLoaded');

  const t0 = Date.now();
  const waitRaf = async (n) => { const target = rafCount + n; while (rafCount < target && Date.now() - t0 < 200000) await new Promise((r) => setTimeout(r, 15)); };

  /* 1. boot to title screen */
  await waitRaf(800);
  const eas = global.window.__eas;
  if (!eas || !eas.booted) { console.log('FAIL: never booted'); process.exit(2); }
  console.log('booted, raf=' + rafCount);

  /* 2. gesture: unlock audio (title BGM start was queued behind autoplay) */
  mouse('mousedown', 240, 218);
  mouse('mouseup', 240, 218);
  await waitRaf(300);

  /* 3. find a playing MediaPlayer (the title BGM) */
  const host = eas.host;
  let bgm = null;
  for (let tries = 0; tries < 40 && !bgm; tries++) {
    for (const p of host._players) {
      if (p.playing && p.el && !p.el.paused) { bgm = p; break; }
    }
    if (!bgm) await waitRaf(60);
  }
  console.log('players: ' + host._players.size + ', audio elements: ' + global.__audioEls.length);
  for (const p of host._players) {
    console.log(`  player ${p.name}: playing=${p.playing} paused=${p.el ? p.el.paused : '-'} playCount=${p.el ? p.el.playCount : '-'} endedHandler=${p.el && p.el._listeners['ended'] ? 'yes' : 'no'}`);
  }
  if (!bgm) { console.log('FAIL: no BGM MediaPlayer ever started'); process.exit(3); }
  const el = bgm.el;
  if (!el._listeners['ended']) { console.log('FAIL: BGM audio element has no ended handler — loop chain broken at host'); process.exit(4); }

  /* 3b. direct VM probe: exception-handler lookup for ui/a.onCompletion's
   * deliberate throw (try [0xe,0x14) units -> handler 0x14 catching ISE) */
  const vm0 = eas.vm;
  try {
    const ISE1 = vm0.requireClass('Ljava/lang/IllegalStateException;');
    const ISE2 = vm0.requireClass('Ljava/lang/IllegalStateException;');
    console.log('[probe] requireClass identity: ' + (ISE1 === ISE2) + ', isAssignable(self): ' + vm0.isAssignable(ISE1, ISE2));
    const uia = vm0.requireClass('Lkairo/android/ui/a;');
    const m = uia.sigMap.get('onCompletion(Landroid/media/MediaPlayer;)V');
    vm0._methodCode(m);
    console.log('[probe] onCompletion tries: ' + JSON.stringify(m.code.tries.map((t) => ({ s: t.startAddr, e: t.endAddr, h: t.handlers.pairs.map((p) => p.typeDesc + '@' + p.addr), ca: t.handlers.catchAllAddr }))));
    console.log('[probe] _findHandler(onCompletion, 0x13, ISE) = 0x' + vm0._findHandler(m, 0x13, ISE1).toString(16) + ' (want 0x14)');
    console.log('[probe] _findHandler(onCompletion, 0x9,  ISE) = 0x' + vm0._findHandler(m, 0x9, ISE1).toString(16) + ' (seekTo, want -1)');
  } catch (pe) { console.log('[probe] threw: ' + (pe && pe.stack || pe)); }

  /* instrument: trace VM throws (the game's onCompletion uses a deliberate
   * IllegalStateException as retry control flow) + every media native call */
  const vm = eas.vm;
  const errLog = [];
  const origErr = vm.onError;
  vm.onError = (...a) => { errLog.push(a.map(String).join(' ')); origErr(...a); };
  vm.onThrow = (x) => {
    console.log('[throw]', x.c.desc, (x.vmMsg ? ': ' + x.vmMsg : ''));
    for (const t of (x.vmTrace || []).slice(0, 6)) console.log('    at ' + t);
  };
  for (const fn of ['mediaStart', 'mediaStop', 'mediaSeek', 'mediaRelease', 'mediaPause']) {
    const orig = host[fn].bind(host);
    host[fn] = (mp, ...a) => { console.log('[media] ' + fn + '(' + a.join(',') + ')'); return orig(mp, ...a); };
  }

  /* 4. simulate natural track end; game should seekTo(0)+start() itself */
  el.currentTime = 42;                 // prove seekTo(0) really runs
  const beforePlays = el.playCount;
  console.log(`simulating track end (playCount=${beforePlays}, currentTime=42)`);
  el.simulateEnded();
  await waitRaf(120);                  // let the rAF pump drain the UI queue
  console.log('--- diagnostics ---');
  console.log('mainThread.dead = ' + vm.mainThread.dead + ', exc = ' + (vm.mainThread.exc ? 'yes' : 'no'));
  console.log('vm.onError lines during ended: ' + JSON.stringify(errLog).slice(0, 800));
  console.log('--- log-box tail ---');
  console.log(document.getElementById('log-box').textContent.split('\n').slice(-8).join('\n'));

  const errBox = document.getElementById('log-box').textContent;
  const compErr = errBox.includes('onCompletion error');

  console.log(`after ended: playCount=${el.playCount} (want > ${beforePlays}), currentTime=${el.currentTime} (want 0), paused=${el.paused} (want false), playing=${bgm.playing} (want true), completionErrors=${compErr}`);
  const ok = el.playCount > beforePlays && el.currentTime === 0 && el.paused === false && bgm.playing === true && !compErr;
  console.log(ok ? 'AUDIO LOOP E2E OK — game loops BGM via its own onCompletion(seekTo+start)' : 'FAIL: BGM did not restart');
  process.exit(ok ? 0 : 5);
}

main().catch((e) => { console.log('FATAL', e && e.stack || e); process.exit(1); });
