#!/usr/bin/env node
/* =============================================================================
 * tools/smoke.js -- head-less boot of the recompiled game.
 *
 * Loads exactly what web/index.html loads (dex-meta, runtime, dex-classes,
 * boot) into a bare-bones DOM with a recording 2D canvas, boots the
 * translated activity through the real Android life-cycle and pumps the
 * cooperative scheduler on a virtual clock.  Missing runtime classes,
 * signature mismatches or VM exceptions fail the build here instead of
 * showing up as a blank page in the browser.
 *
 *   node tools/smoke.js [ticks]
 * ========================================================================== */
'use strict';

const { createHost } = require('./harness');

const TICKS = parseInt(process.argv[2] || '400', 10);
const host = createHost({});
const { sandbox, stats } = host;

try {
  host.load();
} catch (e) {
  console.error('!! load failed');
  console.error((e && e.stack) || e);
  process.exit(1);
}
console.log('loaded: ' + host.FILES.join(', '));

const origErr = console.error;
console.error = function (...a) {
  stats.errors.push(a.map((x) => (x && x.stack) || String(x)).join(' '));
  origErr.apply(console, a);
};

function jerr(rt, e) {
  return (e && e.$stack) ? (rt.jToString(e) + '\n' + e.$stack) : ((e && e.stack) || e);
}

(async () => {
  const $EAS = sandbox.$EAS;
  const $rt = sandbox.$rt;
  if (!$EAS) { console.log('FAIL: $EAS missing'); process.exit(1); }

  try {
    await $EAS.boot();
  } catch (e) {
    origErr('\n!! boot threw');
    origErr(jerr($rt, e));
    process.exit(1);
  }
  console.log('booted. runtime+translated classes: ' +
              Object.keys($rt.classes).length);

  /* Virtual clock: the game's main loop sleeps 50ms per frame, which would
   * make a meaningful simulation take minutes of wall time. */
  let vnow = Date.now();
  $rt.scheduler.clock = () => vnow;

  for (let i = 0; i < TICKS; i++) {
    vnow += 50;
    try {
      $rt.scheduler.tick();
    } catch (e) {
      origErr('\n!! scheduler tick ' + i + ' threw');
      origErr(jerr($rt, e));
      process.exit(1);
    }
    if (i % 8 === 0) await new Promise((r) => setImmediate(r));
  }

  const live = $rt.scheduler.threads.filter((t) => !t.done);
  console.log('frames simulated: ' + TICKS +
              '  live coroutines: ' + live.length +
              ' [' + live.map((t) => t.name).join(', ') + ']' +
              '  draw calls: ' + stats.drawOps);

  if (stats.errors.length) {
    console.log('\n' + stats.errors.length + ' error(s) logged during the run:');
    stats.errors.slice(0, 12).forEach((e) => console.log('  - ' + e.split('\n')[0]));
  }
  const ok = stats.drawOps > 0 && stats.errors.length === 0 && live.length > 0;
  console.log(ok ? '\nSMOKE: PASS' : '\nSMOKE: FAIL');
  process.exit(ok ? 0 : 1);
})();
