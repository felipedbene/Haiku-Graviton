'use strict';
// fs.watch on DeBeOS: events for a watched file and a watched directory, then
// start/stop cycles checked for thread, port, semaphore, fd and kernel-listener
// leaks. Run with: node fswatch-demo.js <scratch dir>
const fs = require('fs');
const path = require('path');
const { execFileSync } = require('child_process');
const { Worker, isMainThread, parentPort } = require('worker_threads');

const settle = (ms = 300) => new Promise((r) => setTimeout(r, ms));

if (!isMainThread) {
  // Each worker has its own libuv loop; it exits once it has cycled.
  const { dir, n } = require('worker_threads').workerData;
  for (let i = 0; i < n; i++) fs.watch(dir).close();
  parentPort.postMessage('done');
  return;
}

function counts() {
  const pid = String(process.pid);
  const ports = execFileSync('listport', [pid]).toString()
    .split('\n').filter((l) => /^\s*\d+\s/.test(l));
  const sems = execFileSync('listsem', [pid]).toString()
    .split('\n').filter((l) => /^\s*\d+\s/.test(l));
  // `ps` lists every team: name, Id, #Threads, Gid, Uid.
  const row = execFileSync('ps').toString().split('\n')
    .map((l) => l.trim().split(/\s+/))
    .find((f) => f.length >= 5 && f[f.length - 4] === pid);
  const nthreads = Number(row[row.length - 3]);
  let nfds = 0;
  for (let fd = 0; fd < 4096; fd++) {
    try { fs.fstatSync(fd); nfds++; } catch { /* not open */ }
  }
  return {
    threads: nthreads,
    ports: ports.length,
    fsPorts: ports.filter((l) => l.includes('libuv fs event')).length,
    sems: sems.length,
    fds: nfds,
  };
}

async function events(root) {
  const log = [];
  const file = path.join(root, 'f.txt');
  const dir = path.join(root, 'd');
  fs.writeFileSync(file, 'one\n');
  fs.mkdirSync(dir);

  const wf = fs.watch(file, (ev, name) => log.push(`file watcher: ${ev} ${name}`));
  const wd = fs.watch(dir, (ev, name) => log.push(`dir watcher:  ${ev} ${name}`));

  const step = async (what, fn) => {
    log.push(`-- ${what}`);
    fn();
    await settle();
  };
  await step('modify f.txt (append)', () => fs.appendFileSync(file, 'two\n'));
  await step('rename f.txt -> g.txt', () => fs.renameSync(file, path.join(root, 'g.txt')));
  await step('delete g.txt', () => fs.unlinkSync(path.join(root, 'g.txt')));
  await step('create d/a', () => fs.writeFileSync(path.join(dir, 'a'), ''));
  await step('modify d/a', () => fs.writeFileSync(path.join(dir, 'a'), 'x'));
  await step('rename d/a -> d/b', () => fs.renameSync(path.join(dir, 'a'), path.join(dir, 'b')));
  await step('delete d/b', () => fs.unlinkSync(path.join(dir, 'b')));
  await step('rename d -> e', () => fs.renameSync(dir, path.join(root, 'e')));
  await step('delete e', () => fs.rmdirSync(path.join(root, 'e')));
  wf.close();
  wd.close();
  return log;
}

async function main() {
  const root = fs.mkdtempSync(path.join(process.argv[2] || '/tmp', 'fswatch-'));
  console.log(`node ${process.version}, pid ${process.pid}, root ${root}`);
  console.log('counts before the first fs.watch:', JSON.stringify(counts()));

  for (const l of await events(root)) console.log(l);

  const scratch = path.join(root, 'cycle');
  fs.mkdirSync(scratch);
  fs.writeFileSync(path.join(scratch, 'f'), '');

  console.log('\ncounts after the first watchers closed:', JSON.stringify(counts()));
  const N = 5000;  // above the default per-team node-monitor limit (4096)
  for (let i = 0; i < N; i++) {
    fs.watch(scratch).close();
    fs.watch(path.join(scratch, 'f')).close();
  }
  await settle(100);
  console.log(`after ${N} x 2 start/stop cycles:`, JSON.stringify(counts()));

  // Watchers kept open at once, then all closed.
  const many = [];
  for (let i = 0; i < 1000; i++) many.push(fs.watch(scratch));
  console.log('with 1000 watchers open:   ', JSON.stringify(counts()));
  for (const w of many) w.close();
  await settle(100);
  console.log('after closing all 1000:    ', JSON.stringify(counts()));

  // Loops that close: every worker starts its own helper and must tear it down.
  for (let i = 0; i < 20; i++) {
    await new Promise((resolve, reject) => {
      const w = new Worker(__filename, { workerData: { dir: scratch, n: 100 } });
      w.on('error', reject);
      w.on('exit', resolve);
    });
  }
  await settle(200);
  console.log('after 20 workers x 100 cycles, all exited:', JSON.stringify(counts()));

  // A watcher still reports after all of that.
  const got = await new Promise((resolve) => {
    const w = fs.watch(scratch, (ev, name) => { w.close(); resolve(`${ev} ${name}`); });
    fs.writeFileSync(path.join(scratch, 'late'), '');
  });
  console.log('watch after the cycles still reports:', got);

  fs.rmSync(root, { recursive: true });
}

main().catch((e) => { console.error(e); process.exit(1); });
