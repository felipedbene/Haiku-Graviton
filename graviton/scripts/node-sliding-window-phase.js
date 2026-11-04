'use strict';
// node-sliding-window-phase.js -- issue 997 probe.
//
// Replays the time-based block at the end of Node 24.22.x's
// test/parallel/test-perf-hooks-sliding-window-histogram.js and prints where
// the timer wakeup lands relative to the histogram's window origin.
//
// SlidingWindowHistogram's time generation is (uv_hrtime() - origin_) /
// chunkDuration, with origin_ taken in the constructor (src/histogram.cc,
// SlidingWindowHistogram::CurrentTimeGeneration). The test sleeps
// delay(200) -- exactly two chunk durations -- and then expects record() and
// the following snapshot() to fall in the same generation, so it fails when a
// chunk boundary lands between those two calls.
//
// One trial per process. The timer is armed from libuv's loop time, which is
// whole milliseconds, so where the wakeup falls against origin_ depends on
// where in its millisecond origin_ was taken -- and that is set by start-up.
// NOTE: this small module starts its block a few ms after process start and
// its wakeup clusters ~0.4 ms past the boundary, so it rarely fails; the real
// test starts ~35 ms in and spreads across the boundary. To measure the real
// test, use node-sliding-window-wake-phase, which instruments the test itself.
//
// usage: node node-sliding-window-phase.js [delayMs] [chunks]
//   defaults 200 1 (the 24.22.2 test); 250 2 is the upstream deflake
//   (nodejs/node 147ade5ff9).
// output: one line,
//   wake_ms=<timer wakeup - origin> pair_us=<record..snapshot> gen_wake=<n>
//   ctor_us=<constructor bracket> after=<count after the sleep>
//   count=<final snapshot count> PASS|FAIL
// PASS needs both of the test's assertions: after=0 and count=1.

const { createSlidingWindowHistogram } = require('perf_hooks');
const { setTimeout: delay } = require('timers/promises');

const delayMs = Number(process.argv[2] || 200);
const chunks = Number(process.argv[3] || 1);
const chunkMs = 100;

(async () => {
  // Bracket the constructor so origin_ is known to within the bracket.
  const before = process.hrtime.bigint();
  const histogram = createSlidingWindowHistogram({
    chunks,
    chunkDuration: chunkMs,
    highest: 100,
  });
  const after = process.hrtime.bigint();
  const origin = (before + after) / 2n;

  histogram.record(1);
  histogram.snapshot();

  await delay(delayMs);
  const wake = process.hrtime.bigint();
  const afterSleep = histogram.snapshot().count;

  const t0 = process.hrtime.bigint();
  histogram.record(2);
  const current = histogram.snapshot();
  const t1 = process.hrtime.bigint();

  const wakeMs = Number(wake - origin) / 1e6;
  const pairUs = Number(t1 - t0) / 1e3;
  const ok = afterSleep === 0 && current.count === 1;
  console.log(`wake_ms=${wakeMs.toFixed(3)} pair_us=${pairUs.toFixed(1)} ` +
              `gen_wake=${Math.floor(wakeMs / chunkMs)} ` +
              `ctor_us=${(Number(after - before) / 1e3).toFixed(1)} ` +
              `after=${afterSleep} count=${current.count} ${ok ? 'PASS' : 'FAIL'}`);
})();
