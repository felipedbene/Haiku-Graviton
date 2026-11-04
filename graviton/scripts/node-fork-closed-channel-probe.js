'use strict';
// node-fork-closed-channel-probe.js -- issue 978 probe.
//
// The body of Node's test/parallel/test-child-process-fork-closed-channel-segfault.js
// (24.22.2, which already ignores EPIPE on the second send), made standalone and
// instrumented. When a run has not finished after WATCHDOG_MS it prints what the
// primary is still waiting on, then exits 3 so a hung run is counted instead of
// waiting out a harness timeout.
//
// usage: node node-fork-closed-channel-probe.js [watchdogMs]
// exit: 0 = the primary saw the worker's 'close' and finished
//       3 = watchdog fired (the issue-978 hang); state is printed first
//       4 = the loop drained without the child's 'close' (would be a mustCall failure)
//       other = a real failure of the test's own assertions

const assert = require('assert');
const cluster = require('cluster');
const net = require('net');

if (!cluster.isPrimary) {
  // Exit on first received handle to leave the queue non-empty in primary
  process.on('message', function() {
    process.exit(1);
  });
  return;
}

const WATCHDOG_MS = Number(process.argv[2] || 20000);
const t0 = process.hrtime.bigint();
const ms = () => (Number(process.hrtime.bigint() - t0) / 1e6).toFixed(1);
const events = [];
const note = (what) => events.push(`${ms()} ${what}`);

let worker;
const ignorable = new Set(['ECONNRESET', 'ECONNREFUSED', 'EMFILE']);

const server = net
  .createServer(function(s) {
    note('server: connection');
    setTimeout(function() {
      s.destroy();
    }, 100);
  })
  .listen(0, () => {
    note('server: listening');
    worker = cluster.fork();
    note(`fork: pid ${worker.process.pid}`);

    worker.on('error', function(err) {
      note(`worker error ${err.code}`);
      if (!ignorable.has(err.code)) throw err;
    });

    function send(tag, callback) {
      const s = net.connect(server.address().port, function() {
        note(`${tag}: connected, sending handle`);
        worker.send({}, s, (err) => {
          note(`${tag}: send callback ${err ? err.code : 'ok'}`);
          callback(err);
        });
      });
      s.on('error', function(err) {
        note(`${tag}: socket error ${err.code}`);
        if (!ignorable.has(err.code)) throw err;
      });
    }

    worker.process.on('exit', (code, sig) => note(`child 'exit' ${code} ${sig}`));
    worker.process.on('disconnect', () => note("child 'disconnect'"));
    worker.process.once('close', function() {
      note("child 'close'");
      assert.strictEqual(worker.process.channel, null);
      server.close(() => note('server: closed'));
    });

    worker.on('online', () => {
      note('worker online');
      send('send1', (err) => {
        assert.ifError(err);
        send('send2', function(err) {
          if (err && err.code !== 'ERR_IPC_CHANNEL_CLOSED' &&
                     err.code !== 'EPIPE' &&
                     !ignorable.has(err.code)) {
            throw err;
          }
        });
      });
    });
  });

setTimeout(() => {
  const cp = worker && worker.process;
  console.log(`HANG after ${ms()} ms`);
  for (const e of events) console.log(`  ev ${e}`);
  if (cp) {
    console.log(`  child pid=${cp.pid} exitCode=${cp.exitCode} ` +
                `signalCode=${cp.signalCode} connected=${cp.connected} ` +
                `channel=${cp.channel ? 'open' : cp.channel}`);
    // stdio of a cluster worker is inherited; only the IPC pipe can hold 'close'.
    // pendingHandle / _handleQueue: what the primary still has queued to send.
    const ch = cp.channel;
    if (ch) {
      console.log(`  channel: fd=${ch.fd} pendingHandle=${!!ch.pendingHandle} ` +
                  `writeQueueSize=${ch.writeQueueSize} ` +
                  `handleQueue=${cp._handleQueue ? cp._handleQueue.length : 'null'}`);
    }
  }
  console.log(`  server.listening=${server.listening}`);
  console.log(`  resources=${JSON.stringify(process.getActiveResourcesInfo())}`);
  process.exit(3);
}, WATCHDOG_MS).unref();

process.on('exit', (code) => {
  if (code === 0 && !events.some((e) => e.endsWith("child 'close'"))) {
    console.log("EXITED without the child's 'close'");
    for (const e of events) console.log(`  ev ${e}`);
    process.exitCode = 4;
  } else if (code === 0) {
    // How the second send ended: ok, EPIPE, ERR_IPC_CHANNEL_CLOSED, ...
    const s2 = events.find((e) => e.includes('send2: send callback'));
    console.log(`OK ${ms()} ms send2=${s2 ? s2.split(' ').pop() : 'none'}`);
  }
});
