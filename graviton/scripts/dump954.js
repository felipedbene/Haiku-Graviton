'use strict';
// dump954.js -- preload for hang954.py. Records the lifecycle events of
// every net.Socket in the process (cheaply: an array push per event), and
// from an unref'd timer dumps them with libuv's active handles, so a hung
// run shows which socket is still open and which of its events never came.
const net = require('net');
const t0 = process.hrtime.bigint();
const ms = () => Number((process.hrtime.bigint() - t0) / 1000000n);
const events = [];
let nextId = 1;
const kId = Symbol('id954');
const watched = new Set(['connect', 'end', 'finish', 'close', 'error',
	'timeout', 'ready']);

const emit = net.Socket.prototype.emit;
net.Socket.prototype.emit = function(name, ...args) {
	if (watched.has(name)) {
		if (this[kId] === undefined)
			this[kId] = nextId++;
		events.push([ms(), this[kId], name,
			name === 'error' ? String(args[0] && args[0].code) : '']);
	}
	return emit.call(this, name, ...args);
};

const serverEmit = net.Server.prototype.emit;
net.Server.prototype.emit = function(name, ...args) {
	if (name === 'connection') {
		const s = args[0];
		if (s[kId] === undefined)
			s[kId] = nextId++;
		events.push([ms(), s[kId], 'accepted', '']);
	} else if (name === 'close' || name === 'listening') {
		events.push([ms(), 0, 'server-' + name, '']);
	}
	return serverEmit.call(this, name, ...args);
};

function describe(h) {
	const o = { type: h.constructor && h.constructor.name };
	if (h instanceof net.Socket) {
		o.id = h[kId];
		o.local = h.localAddress + ':' + h.localPort;
		o.remote = h.remoteAddress + ':' + h.remotePort;
		o.destroyed = h.destroyed;
		o.connecting = h.connecting;
		o.readyState = h.readyState;
		o.readable = h.readable;
		o.writable = h.writable;
		o.readableEnded = h.readableEnded;
		o.writableEnded = h.writableEnded;
		o.writableFinished = h.writableFinished;
		o.writableLength = h.writableLength;
		o.bytesRead = h.bytesRead;
		o.bytesWritten = h.bytesWritten;
		o.allowHalfOpen = h.allowHalfOpen;
		o.reading = h._handle ? h._handle.reading : undefined;
		o.hasHandle = !!h._handle;
	} else if (h instanceof net.Server) {
		try { o.address = h.address(); } catch (e) { o.address = String(e); }
		o.listening = h.listening;
		o.connections = h._connections;
	}
	return o;
}

// A fresh poll() of the socket, from a child that shares the open file
// description (fd 3): what a new registration sees, without reading.
const kProbe = [
	'import select, socket, struct',
	'p = select.poll(); p.register(3, select.POLLIN | select.POLLOUT)',
	'r = p.poll(0)',
	's = socket.socket(fileno=3)',
	'e = s.getsockopt(socket.SOL_SOCKET, socket.SO_ERROR)',
	'try:',
	'    n = len(s.recv(65536, socket.MSG_PEEK | socket.MSG_DONTWAIT))',
	'except BlockingIOError:',
	'    n = "EAGAIN"',
	'except OSError as x:',
	'    n = "errno %d" % x.errno',
	's.detach()',
	'print("poll=%s so_error=%d peek=%s" % ([(f, hex(v)) for f, v in r], e, n))',
].join('\n');

function probe(fd) {
	const r = require('child_process').spawnSync('python3', ['-I', '-c', kProbe],
		{ stdio: ['ignore', 'pipe', 'pipe', fd], timeout: 10000 });
	return (String(r.stdout) + String(r.stderr)).trim().replace(/\n/g, ' | ')
		+ (r.error ? ' spawn-error ' + r.error : '');
}

const at = Number(process.env.DUMP954_AT_MS || 12000);
const timer = setTimeout(() => {
	const out = [];
	out.push('DUMP954 at ' + ms() + 'ms pid ' + process.pid);
	out.push('DUMP954 resources ' +
		JSON.stringify(process.getActiveResourcesInfo()));
	for (const h of process._getActiveHandles()) {
		out.push('DUMP954 handle ' + JSON.stringify(describe(h)));
		if (h instanceof net.Socket && h._handle && h._handle.fd >= 0)
			out.push('DUMP954 probe id ' + h[kId] + ' ' + probe(h._handle.fd));
	}
	for (const e of events)
		out.push('DUMP954 event ' + JSON.stringify(e));
	process._rawDebug(out.join('\n'));
}, at);
timer.unref();
