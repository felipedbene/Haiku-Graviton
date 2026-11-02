// h2coalesce954.mjs -- issue 954: does a Node http2 server whose stream is
// echoing (req.pipe(res)) finish closing when the client's last DATA frame,
// its GOAWAY and the stream's RST_STREAM reach the server in ONE read, as they
// did in every hung run of parallel/test-http2-many-writes-and-destroy?
//
// A raw TCP client speaks HTTP/2 to an in-process http2 server built exactly
// like the test's. It sends the request body, waits for the first echoed DATA
// (where the test destroys its request), calls server.close() as the test
// does, and then sends the rest:
//   coalesced: last DATA + GOAWAY + RST_STREAM(NO_ERROR) + GOAWAY in one write
//   separate:  last DATA alone, 200 ms later the rest
// and then ends the TCP connection. Pass = the server's 'close' event fires
// (its session, and with it the accepted socket, closed) within 5 s.
//
// usage: node h2coalesce954.mjs <coalesced|separate> [rounds]
import http2 from 'node:http2';
import net from 'node:net';

const mode = process.argv[2];
const rounds = Number(process.argv[3] || 20);
if (mode !== 'coalesced' && mode !== 'separate') {
	console.error('usage: h2coalesce954.mjs <coalesced|separate> [rounds]');
	process.exit(2);
}

function frame(type, flags, stream, payload = Buffer.alloc(0)) {
	const header = Buffer.alloc(9);
	header.writeUIntBE(payload.length, 0, 3);
	header[3] = type;
	header[4] = flags;
	header.writeUInt32BE(stream, 5);
	return Buffer.concat([header, payload]);
}
const DATA = 0, HEADERS = 1, RST_STREAM = 3, SETTINGS = 4, GOAWAY = 7;
const goaway = () => frame(GOAWAY, 0, 0, Buffer.alloc(8));
	// last stream id 0, NO_ERROR, as the test's client sends

function round(n) {
	return new Promise((resolve) => {
		const server = http2.createServer((req, res) => req.pipe(res));
		let serverClosed = false;
		server.on('close', () => { serverClosed = true; });
		server.listen(0, () => {
			const client = net.connect(server.address().port, '127.0.0.1');
			let gotSettings = false, gotData = false, finished = false;
			client.on('error', () => {});
			client.on('data', (chunk) => {
				// A crude frame walk is enough: the server sends whole frames,
				// and only their types are of interest.
				for (let i = 0; i + 9 <= chunk.length;) {
					const length = chunk.readUIntBE(i, 3), type = chunk[i + 3],
						flags = chunk[i + 4];
					if (type === SETTINGS && (flags & 1) === 0 && !gotSettings) {
						gotSettings = true;
						client.write(frame(SETTINGS, 1, 0));
					}
					if (type === DATA && !gotData) {
						gotData = true;
						finish();
					}
					i += 9 + length;
				}
			});
			client.write(Buffer.from('PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n'));
			client.write(frame(SETTINGS, 0, 0));
			const block = Buffer.concat([Buffer.from([0x83, 0x86, 0x84, 0x41,
				0x09]), Buffer.from('localhost')]);
			client.write(frame(HEADERS, 0x4, 1, block));
			// the body before the last chunk, as the test's 4000 6-byte
			// writes reach the wire: in frames of up to 16384 bytes
			client.write(frame(DATA, 0, 1, Buffer.alloc(16384)));
			client.write(frame(DATA, 0, 1, Buffer.alloc(2144)));

			function finish() {
				if (finished)
					return;
				finished = true;
				server.close();
				const last = frame(DATA, 0, 1, Buffer.alloc(5604));
				const rest = Buffer.concat([goaway(),
					frame(RST_STREAM, 0, 1, Buffer.alloc(4)), goaway()]);
				if (mode === 'coalesced') {
					client.write(Buffer.concat([last, rest]));
					client.end();
				} else {
					client.write(last);
					setTimeout(() => { client.write(rest); client.end(); }, 200);
				}
				const start = Date.now();
				const timer = setInterval(() => {
					if (serverClosed || Date.now() - start > 5000) {
						clearInterval(timer);
						console.log('round %d %s: %s after %d ms', n, mode,
							serverClosed ? 'PASS server closed'
								: 'FAIL server still open', Date.now() - start);
						client.destroy();
						// a server that never closes is left behind on purpose:
						// it is what is being measured, and the process exits
						// below anyway
						resolve(serverClosed);
					}
				}, 10);
			}
		});
	});
}

let failed = 0;
for (let n = 1; n <= rounds; n++) {
	if (!await round(n))
		failed++;
}
console.log('h2coalesce954 %s: %d/%d rounds failed', mode, failed, rounds);
process.exit(failed);
