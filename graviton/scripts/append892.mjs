// append892.mjs -- two concurrent fs.appendFile() calls of one 161-byte line,
// <rounds> times (issue 892). A correct O_APPEND leaves two lines every time.
import { appendFile, readFile, rm } from 'node:fs/promises';
const rounds = Number(process.argv[2] ?? 200);
const dir = process.argv[3] ?? '/tmp';
const line = 'x'.repeat(160) + '\n';
let lost = 0;
for (let r = 0; r < rounds; r++) {
  const f = `${dir}/append892-${process.pid}-${r}`;
  await Promise.all([appendFile(f, line), appendFile(f, line)]);
  const n = (await readFile(f, 'utf8')).split('\n').length - 1;
  if (n !== 2) lost++;
  await rm(f);
}
console.log(`${lost === 0 ? 'PASS' : 'FAIL'} node appendFile x2: rounds ${rounds}, rounds with a lost append ${lost}`);
