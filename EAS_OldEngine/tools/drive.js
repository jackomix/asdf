#!/usr/bin/env node
/* =============================================================================
 * tools/drive.js -- run the recompiled game head-less and interact with it
 * through the *game's own* hit-test tables, so a play session can be scripted
 * and verified without a browser or a pointing device.
 *
 * The engine keeps its live touch targets in kairo/android/a/c#d (a Vector of
 * kairo/android/a/i rectangles expressed in the game's logical coordinate
 * space).  Screen coordinates are recovered with the same transform the
 * engine applies to an incoming MotionEvent:
 *
 *     gx = (event.x - c.t) / (c.x / 100)
 *     gy = (event.y + c.y - c.u) / (c.x / 100)
 *
 * so the inverse used here is
 *
 *     screenX = i.c * scale + c.t
 *     screenY = i.d * scale + c.u - c.y
 *
 * usage:
 *   NODE_PATH=/tmp/w/node_modules node tools/drive.js [script...]
 *
 *   script steps, comma separated inside one argument:
 *     wait:<frames>          advance N engine frames
 *     targets                print the live touch targets
 *     tap:<idx>              tap the centre of live target #idx
 *     tap:<x>,<y>            tap absolute screen coordinates
 *     key:<code>             send an Android key code
 *     shot:<file.png>        write the current surface to a PNG
 *     text                   print every string the game font renderer drew
 *     input:<value>          answer the next AlertDialog: type <value> into
 *                            its EditText and press the positive button
 *     input:                 cancel the next AlertDialog
 *
 *   example:
 *     node tools/drive.js wait:40 text tap:240,443 wait:30 text targets
 * ========================================================================== */
'use strict';

const fs = require('fs');
const path = require('path');
const { createHost, ROOT } = require('./harness');

let napi = null;
for (const p of ['@napi-rs/canvas', '/tmp/w/node_modules/@napi-rs/canvas']) {
  try { napi = require(p); break; } catch (e) { /* next */ }
}
if (!napi) {
  console.error('tools/drive.js needs @napi-rs/canvas:  npm i @napi-rs/canvas');
  process.exit(2);
}

function canvasFactory(w, h) {
  const cv = napi.createCanvas(Math.max(1, w | 0), Math.max(1, h | 0));
  cv.style = {};
  cv.addEventListener = () => {};
  cv.removeEventListener = () => {};
  cv.setPointerCapture = () => {};
  cv.appendChild = () => {};
  cv.getBoundingClientRect = () =>
    ({ left: 0, top: 0, width: cv.width, height: cv.height });
  return cv;
}

const host = createHost({
  canvasFactory,
  extend(sb) { sb.Path2D = napi.Path2D; sb.ImageData = napi.ImageData; },
});
const { sandbox } = host;
host.load();

/* one step per argv entry; several steps may also be packed into a single
 * argument separated by ';' so a whole session fits in one shell string. */
const steps = [];
for (const a of process.argv.slice(2)) {
  for (const s of String(a).split(';')) if (s.trim()) steps.push(s.trim());
}

(async () => {
  const { $EAS, $rt, $host } = sandbox;
  await $EAS.boot();

  /* ---- record every string handed to the game's own text renderer ------
   * The label position is read back from the destination canvas transform so
   * it can be reported in surface pixels: the engine composites its off-screen
   * frame buffer at (0, c.u), the same offset the hit tester uses.           */
  const I = $rt.classes['kairo/android/c/i'];
  const C = $rt.classes['kairo/android/ui/c'];
  let frameText = new Map();
  function surfaceOffsetY() {
    const gv = $host.gameView;
    if (!gv || !gv.f_i) return 0;
    const vec = $rt.invoke(gv.f_i, 'a()Ljava/util/Vector;', []);
    if ($rt.invoke(vec, 'size()I', []) === 0) return 0;
    return $rt.invoke(vec, 'elementAt(I)Ljava/lang/Object;', [0]).f_u || 0;
  }
  for (const [name, ai] of [['a$99', 0], ['a$100', 0], ['a$140', 1], ['a$141', 1]]) {
    const orig = C.prototype[name];
    if (!orig) continue;
    C.prototype[name] = function (...a) {
      const s = a[ai];
      if (typeof s === 'string' && s.length) {
        const cv = this.f_a && this.f_a.ctx ? this.f_a : null;
        let x = a[ai + 1], y = a[ai + 2], onSurface = false;
        if (cv && cv.ctx && cv.ctx.getTransform) {
          const t = cv.ctx.getTransform();
          x = t.a * a[ai + 1] + t.e;
          y = t.d * a[ai + 2] + t.f;
          onSurface = cv.ctx.canvas === $host.surface;
        }
        if (!onSurface) y += surfaceOffsetY();
        frameText.set(Math.round(y) * 4096 + Math.round(x),
                      I.b$22(s) + '@' + Math.round(x) + ',' + Math.round(y));
      }
      return orig.apply(this, a);
    };
  }

  let vnow = Date.now();
  $rt.scheduler.clock = () => vnow;

  const advance = (n) => {
    for (let i = 0; i < n; i++) {
      vnow += 50;
      frameText = new Map();
      $rt.scheduler.tick();
    }
  };

  /** live touch targets, in surface coordinates */
  function targets() {
    const gv = $host.gameView;
    const out = [];
    if (!gv || !gv.f_i) return out;
    const vec = $rt.invoke(gv.f_i, 'a()Ljava/util/Vector;', []);
    const n = $rt.invoke(vec, 'size()I', []);
    for (let li = 0; li < n; li++) {
      const c = $rt.invoke(vec, 'elementAt(I)Ljava/lang/Object;', [li]);
      const scale = c.f_x / 100;
      const d = c.f_d;
      const m = $rt.invoke(d, 'size()I', []);
      for (let i = 0; i < m; i++) {
        const r = $rt.invoke(d, 'elementAt(I)Ljava/lang/Object;', [i]);
        out.push({
          owner: c.constructor.$name,
          id: r.f_b,
          x: Math.round(r.f_c * scale + c.f_t),
          y: Math.round(r.f_d * scale + c.f_u - c.f_y),
          w: Math.round(r.f_e * scale),
          h: Math.round(r.f_f * scale),
        });
      }
    }
    return out;
  }

  advance(1);

  for (const step of steps) {
    const [op, arg] = step.split(':');
    if (op === 'wait') {
      advance(parseInt(arg || '1', 10));
      await new Promise((r) => setImmediate(r));
    } else if (op === 'targets') {
      const t = targets();
      console.log('-- ' + t.length + ' live touch targets');
      t.forEach((q, i) => console.log('   #' + String(i).padEnd(3) +
        q.owner.padEnd(8) + 'id=' + String(q.id).padEnd(4) +
        'rect ' + q.x + ',' + q.y + ' ' + q.w + 'x' + q.h +
        '  centre ' + (q.x + (q.w >> 1)) + ',' + (q.y + (q.h >> 1))));
    } else if (op === 'text') {
      const l = [...frameText.entries()].sort((a, b) => a[0] - b[0])
        .map((e) => e[1]);
      console.log('-- screen text: ' + l.join(' | '));
    } else if (op === 'tap') {
      if (arg.indexOf(',') >= 0) {
        const [x, y] = arg.split(',').map(Number);
        console.log('-- tap ' + x + ',' + y);
        $EAS.tap(x, y);
      } else {
        const t = targets()[parseInt(arg, 10)];
        if (!t) { console.log('-- no target #' + arg); continue; }
        const x = t.x + (t.w >> 1), y = t.y + (t.h >> 1);
        console.log('-- tap target #' + arg + ' (' + t.owner + ' id=' + t.id +
                    ') at ' + x + ',' + y);
        $EAS.tap(x, y);
      }
      advance(2);
    } else if (op === 'key') {
      $EAS.key(parseInt(arg, 10));
      advance(2);
    } else if (op === 'input') {
      /* The game asks for the planet / company name through a real
       * android.app.AlertDialog holding an EditText.  Without a DOM the
       * runtime hands the dialog to $host.onDialog, so answer it here. */
      const value = arg === undefined ? '' : arg;
      $host.onDialog = (d, s) => {
        console.log('-- dialog "' + (s.title || '') + '"' +
                    (value ? ' <- "' + value + '"' : ' (cancel)'));
        if (!value) { $rt.invoke(d, 'cancel()V', []); return; }
        if (s.view) {
          $rt.invoke(s.view, 'setText(Ljava/lang/CharSequence;)V', [value]);
        }
        $rt.answerDialog(d, 0);
      };
    } else if (op === 'shot') {
      const p = path.resolve(arg);
      fs.mkdirSync(path.dirname(p), { recursive: true });
      fs.writeFileSync(p, $host.surface.toBuffer('image/png'));
      console.log('-- wrote ' + path.relative(ROOT, p));
    } else {
      console.log('-- unknown step ' + step);
    }
  }
  process.exit(0);
})().catch((e) => {
  const $rt = sandbox.$rt;
  console.error((e && e.$stack) ? ($rt.jToString(e) + '\n' + e.$stack) : e);
  process.exit(1);
});
