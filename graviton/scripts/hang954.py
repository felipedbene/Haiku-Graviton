#!/usr/bin/env python3
# hang954.py -- run one Node test many times, C at a time, and on a run that
# outlives its deadline capture the hung process before killing it:
# its threads (bt918), the kernel's TCP table, and the libuv handles that
# the preload dumps from inside the process.
#
# After the in-process dump (dump954.js, loaded with -r) it runs bt918 (built
# from graviton/scripts/bt918.c, next to this file) on the hung team, which
# also interrupts its poll(), then waits 5 s: libuv re-enters poll() with a
# fresh registration of every fd it watches, so a run that is still hung
# after that is not waiting on a readiness event the kernel failed to send.
# With "debug", NODE_DEBUG=net,http2 and NODE_DEBUG_NATIVE=HTTP2SESSION,
# HTTP2STREAM are set. A run that passes leaves no log behind. On Linux
# (NODE954=<node binary>), ss stands in for netstat and bt918 is skipped.
#
# usage: hang954.py <runs> <concurrency> <deadline-s> <test.js> <outdir> [debug]

import os, subprocess, sys, time

runs, conc, deadline = int(sys.argv[1]), int(sys.argv[2]), float(sys.argv[3])
test, outdir = sys.argv[4], sys.argv[5]
debug = len(sys.argv) > 6 and sys.argv[6] == "debug"
here = os.path.dirname(os.path.abspath(__file__))
preload = os.path.join(here, "dump954.js")
bt = os.path.join(here, "bt918")
os.makedirs(outdir, exist_ok=True)
netstat = "netstat -n" if os.path.exists("/boot/system") else "ss -tanpi"

env = dict(os.environ)
if debug:
	env["NODE_DEBUG"] = "net,http2"
	env["NODE_DEBUG_NATIVE"] = "HTTP2SESSION,HTTP2STREAM"
env["DUMP954_AT_MS"] = str(int(deadline * 1000 * 0.6))

def sh(cmd):
	p = subprocess.run(cmd, shell=True, stdout=subprocess.PIPE,
		stderr=subprocess.STDOUT, timeout=120)
	return p.stdout.decode(errors="replace")

active = {}
started = done = hung = failed = 0
durations = []
t0 = time.time()
while done < runs:
	while started < runs and len(active) < conc:
		n = started
		f = open(os.path.join(outdir, "run%04d.log" % n), "wb")
		p = subprocess.Popen([os.environ.get("NODE954", "/boot/system/bin/node"), "-r", preload, test],
			stdin=subprocess.PIPE, stdout=f, stderr=subprocess.STDOUT, env=env,
			cwd=os.path.dirname(test))
		p.stdin.close()
		active[n] = (p, time.time(), f)
		started += 1
	time.sleep(0.05)
	for n, (p, s, f) in list(active.items()):
		rc = p.poll()
		if rc is not None:
			f.close()
			durations.append(time.time() - s)
			if rc != 0:
				failed += 1
				print("RUN %d rc=%d after %.1fs" % (n, rc, time.time() - s),
					flush=True)
			else:
				os.unlink(os.path.join(outdir, "run%04d.log" % n))
			del active[n]
			done += 1
		elif time.time() - s > deadline:
			hung += 1
			cap = os.path.join(outdir, "hang%04d.txt" % n)
			with open(cap, "w") as c:
				c.write("=== HANG run %d pid %d after %.1fs (deadline %.0fs)\n"
					% (n, p.pid, time.time() - s, deadline))
				c.write("=== netstat -n\n" + sh(netstat))
				c.write("=== ps -o\n" + sh("ps -T %d 2>&1 || ps" % p.pid))
				if os.path.exists(bt):
					c.write("=== bt918 %d\n" % p.pid + sh("%s %d 40" % (bt, p.pid)))
				c.write("=== netstat -n (again, after bt918)\n" + sh(netstat))
				# bt918 interrupted the main thread's poll(), so libuv has
				# re-entered it with a fresh registration of every fd it
				# watches. A socket it watches for reading whose EOF is there
				# would now be seen, and the test would finish.
				time.sleep(5)
				rc = p.poll()
				c.write("=== 5 s after bt918: %s\n" % ("exited by itself, rc %d"
					% rc if rc is not None else "still running"))
				c.write("=== netstat -n (5 s after bt918)\n" + sh(netstat))
			p.kill()
			p.wait()
			f.close()
			print("HANG run %d pid %d captured in %s" % (n, p.pid, cap),
				flush=True)
			del active[n]
			done += 1

durations.sort()
med = durations[len(durations) // 2] if durations else 0
print("SUMMARY runs=%d hung=%d failed(nonzero)=%d wall=%.0fs median=%.2fs "
	"max-completed=%.2fs" % (runs, hung, failed, time.time() - t0, med,
		durations[-1] if durations else 0), flush=True)
