#!/usr/bin/env node
//
// selftest.js -- drive HaikuRemoteDesktop.js's drawing handlers over real wire
// bytes and measure what they draw.  Run with `node selftest.js`; it prints
// SELFTEST=PASS or SELFTEST=FAIL and exits 0 or 1.
//
// WHY THIS EXISTS (#494).  The JavaScript client decoded font rotation, shear
// and false_bold_width and then threw all three away, so a rotated label
// rendered horizontal.  So did the Python instrument and so did the
// out-of-tree client, which meant all three AGREED with each other while
// disagreeing with app_server, and any comparison between them passed.  That
// family of bug -- the check that cannot fail -- has been the most productive
// source of real defects in this tree (#475, #479, #485, #488), and it is not
// fixed by writing the transform code: it is fixed by having an assertion that
// goes red when the transform code is removed.  So this file exists, and the
// mutation arms at the bottom delete the fix and require these checks to fail.
//
// WHAT IS MEASURED, AND WHAT IS NOT.  Node has no canvas, so ModelCanvas below
// is a deliberately crude one: a monospace model in which every character is a
// rectangle, transformed through the real canvas matrix the client sets and
// scan-filled into a byte buffer.  What that supports is GEOMETRY -- where ink
// landed, which way a run ran, how a box grew.  What it does NOT support is
// typography: glyph shapes, kerning, real advances, antialiasing, or any claim
// that a browser would produce these pixels.  The client is exercised for
// real (real wire bytes, the real RemoteState handlers, the real canvas API
// calls); only the rasteriser is a model.

'use strict';

const fs = require('fs');
const path = require('path');
const vm = require('vm');

// --- the model canvas -----------------------------------------------------
// A 2x2 + translation matrix, applied as canvas applies it:
//   x' = a*x + c*y + e,  y' = b*x + d*y + f
function Matrix(a, b, c, d, e, f) {
	this.a = a; this.b = b; this.c = c; this.d = d; this.e = e; this.f = f;
}

Matrix.identity = function() { return new Matrix(1, 0, 0, 1, 0, 0); };

Matrix.prototype.clone = function() {
	return new Matrix(this.a, this.b, this.c, this.d, this.e, this.f);
};

Matrix.prototype.map = function(x, y) {
	return [this.a * x + this.c * y + this.e,
		this.b * x + this.d * y + this.f];
};

// this * m, in canvas's sense: m is applied in the coordinate system this
// already establishes, which is what context.transform() does.
Matrix.prototype.multiply = function(m) {
	return new Matrix(
		this.a * m.a + this.c * m.b,
		this.b * m.a + this.d * m.b,
		this.a * m.c + this.c * m.d,
		this.b * m.c + this.d * m.d,
		this.a * m.e + this.c * m.f + this.e,
		this.b * m.e + this.d * m.f + this.f);
};

const CHAR_WIDTH = 10;          // the model's monospace advance
const CHAR_ASCENT = 12;
const CHAR_DESCENT = 3;

function ModelCanvas(width, height) {
	this.width = width;
	this.height = height;
	this.ink = new Uint8Array(width * height);
	this.ctm = Matrix.identity();
	this.stack = [];
	this.lineWidth = 1;
	this.lineCap = 'butt';
	this.lineJoin = 'miter';
	this.miterLimit = 10;
	this.globalAlpha = 1;
	this.globalCompositeOperation = 'source-over';
	this.fillStyle = '#000';
	this.strokeStyle = '#000';
	this.font = '';
	// Every character box that was drawn, in device space: used to measure the
	// direction a run actually ran in.
	this.charBoxes = [];
	this.calls = [];
}

ModelCanvas.prototype.save = function() {
	this.stack.push([this.ctm.clone(), this.lineWidth, this.lineJoin,
		this.lineCap, this.strokeStyle, this.fillStyle]);
};

ModelCanvas.prototype.restore = function() {
	const s = this.stack.pop();
	if (s === undefined)
		throw new Error('restore() with an empty stack');
	this.ctm = s[0];
	this.lineWidth = s[1];
	this.lineJoin = s[2];
	this.lineCap = s[3];
	this.strokeStyle = s[4];
	this.fillStyle = s[5];
};

ModelCanvas.prototype.resetTransform = function() {
	this.ctm = Matrix.identity();
};

ModelCanvas.prototype.setTransform = function(a, b, c, d, e, f) {
	this.ctm = new Matrix(a, b, c, d, e, f);
};

ModelCanvas.prototype.transform = function(a, b, c, d, e, f) {
	this.ctm = this.ctm.multiply(new Matrix(a, b, c, d, e, f));
	this.calls.push(['transform', a, b, c, d, e, f]);
};

ModelCanvas.prototype.translate = function(x, y) {
	this.ctm = this.ctm.multiply(new Matrix(1, 0, 0, 1, x, y));
	this.calls.push(['translate', x, y]);
};

ModelCanvas.prototype.scale = function(x, y) {
	this.ctm = this.ctm.multiply(new Matrix(x, 0, 0, y, 0, 0));
};

ModelCanvas.prototype.measureText = function(text) {
	return { width: text.length * CHAR_WIDTH };
};

ModelCanvas.prototype.beginPath = function() {};
ModelCanvas.prototype.closePath = function() {};
ModelCanvas.prototype.clip = function() {};
ModelCanvas.prototype.rect = function() {};
ModelCanvas.prototype.moveTo = function() {};
ModelCanvas.prototype.lineTo = function() {};
ModelCanvas.prototype.stroke = function() {};
ModelCanvas.prototype.fill = function() {};
ModelCanvas.prototype.fillRect = function() {};
ModelCanvas.prototype.createPattern = function() { return null; };

// Scan-fill a convex quad given as four device-space points.
ModelCanvas.prototype._fillQuad = function(pts) {
	let minY = Infinity, maxY = -Infinity;
	for (const p of pts) {
		if (p[1] < minY) minY = p[1];
		if (p[1] > maxY) maxY = p[1];
	}
	const y0 = Math.max(0, Math.ceil(minY));
	const y1 = Math.min(this.height - 1, Math.floor(maxY));
	for (let y = y0; y <= y1; y++) {
		const sy = y + 0.5;
		let lo = Infinity, hi = -Infinity;
		for (let i = 0; i < 4; i++) {
			const p = pts[i], q = pts[(i + 1) % 4];
			if ((p[1] <= sy && q[1] > sy) || (q[1] <= sy && p[1] > sy)) {
				const t = (sy - p[1]) / (q[1] - p[1]);
				const x = p[0] + t * (q[0] - p[0]);
				if (x < lo) lo = x;
				if (x > hi) hi = x;
			}
		}
		if (lo > hi)
			continue;
		const x0 = Math.max(0, Math.ceil(lo));
		const x1 = Math.min(this.width - 1, Math.floor(hi));
		for (let x = x0; x <= x1; x++)
			this.ink[y * this.width + x] = 1;
	}
};

ModelCanvas.prototype._text = function(text, x, y, grow) {
	for (let i = 0; i < text.length; i++) {
		const left = x + i * CHAR_WIDTH - grow;
		const right = x + (i + 1) * CHAR_WIDTH + grow;
		const top = y - CHAR_ASCENT - grow;
		const bottom = y + CHAR_DESCENT + grow;
		const pts = [this.ctm.map(left, top), this.ctm.map(right, top),
			this.ctm.map(right, bottom), this.ctm.map(left, bottom)];
		this._fillQuad(pts);
		let cx = 0, cy = 0;
		for (const p of pts) { cx += p[0] / 4; cy += p[1] / 4; }
		this.charBoxes.push({ index: i, centre: [cx, cy], points: pts });
	}
};

ModelCanvas.prototype.fillText = function(text, x, y) {
	this.calls.push(['fillText', text, x, y]);
	this._text(text, x, y, 0);
};

ModelCanvas.prototype.strokeText = function(text, x, y) {
	this.calls.push(['strokeText', text, x, y, this.lineWidth]);
	// A centred stroke grows the shape by half the line width on each side.
	this._text(text, x, y, this.lineWidth / 2);
};

// -- measurements on the result -------------------------------------------
ModelCanvas.prototype.inkBox = function() {
	let x0 = Infinity, y0 = Infinity, x1 = -Infinity, y1 = -Infinity;
	for (let y = 0; y < this.height; y++) {
		for (let x = 0; x < this.width; x++) {
			if (!this.ink[y * this.width + x])
				continue;
			if (x < x0) x0 = x;
			if (x > x1) x1 = x;
			if (y < y0) y0 = y;
			if (y > y1) y1 = y;
		}
	}
	return x1 < x0 ? null : [x0, y0, x1, y1];
};

ModelCanvas.prototype.inkPixels = function() {
	let n = 0;
	for (let i = 0; i < this.ink.length; i++)
		n += this.ink[i];
	return n;
};

// The direction the run actually ran, in BFont rotation degrees:
// counter-clockwise positive, y up, so the sign of dy is inverted from device
// space.  Read off the character boxes that were drawn, NOT off the matrix
// that was set -- a measurement of the output, not a restatement of intent.
// null for fewer than two boxes, because one box has no direction and
// answering 0 for it would be the same false negative in a new place.
ModelCanvas.prototype.runAxisDegrees = function() {
	if (this.charBoxes.length < 2)
		return null;
	const first = this.charBoxes[0].centre;
	const last = this.charBoxes[this.charBoxes.length - 1].centre;
	const dx = last[0] - first[0];
	const dy = last[1] - first[1];
	if (Math.abs(dx) < 1e-9 && Math.abs(dy) < 1e-9)
		return null;
	return Math.atan2(-dy, dx) * 180 / Math.PI;
};

function angleDifference(a, b) {
	return Math.abs(((a - b + 180) % 360 + 360) % 360 - 180);
}

// --- the wire -------------------------------------------------------------
// Frames are spelled the way RemoteMessage spells them
// (RemoteMessage.h:239-263): uint16 code, uint32 total size including the
// 6-byte header, then the payload.
function Writer() {
	this.bytes = [];
}

Writer.prototype.u8 = function(v) { this.bytes.push(v & 0xff); return this; };
Writer.prototype.u16 = function(v) {
	this.bytes.push(v & 0xff, (v >> 8) & 0xff);
	return this;
};
Writer.prototype.u32 = function(v) {
	this.bytes.push(v & 0xff, (v >>> 8) & 0xff, (v >>> 16) & 0xff,
		(v >>> 24) & 0xff);
	return this;
};
Writer.prototype.f32 = function(v) {
	const b = new Uint8Array(4);
	new DataView(b.buffer).setFloat32(0, v, true);
	this.bytes.push(b[0], b[1], b[2], b[3]);
	return this;
};
Writer.prototype.f64 = function(v) {
	// The view transform's members are doubles on the wire (BAffineTransform
	// stores them as double; RemoteTransform.readFrom reads readFloat64), so a
	// float32 write would desynchronise the reader by 24 bytes.
	const b = new Uint8Array(8);
	new DataView(b.buffer).setFloat64(0, v, true);
	for (let i = 0; i < 8; i++)
		this.bytes.push(b[i]);
	return this;
};
Writer.prototype.str = function(s) {
	this.u32(s.length);
	for (let i = 0; i < s.length; i++)
		this.bytes.push(s.charCodeAt(i) & 0xff);
	return this;
};
Writer.prototype.done = function() { return new Uint8Array(this.bytes); };

// RemoteMessage.cpp:147-161 (AddFont), packed, 25 bytes after the token:
// uint8 direction, uint8 encoding, uint32 flags, uint8 spacing, float shear,
// float rotation, float falseBoldWidth, float size, uint16 face,
// uint32 familyAndStyle.
function fontPayload(opts) {
	const o = opts || {};
	return new Writer()
		.u8(0).u8(0).u32(0).u8(0)
		.f32(o.shear === undefined ? 90 : o.shear)
		.f32(o.rotation || 0)
		.f32(o.falseBold || 0)
		.f32(o.size === undefined ? 16 : o.size)
		.u16(o.face || 0).u32(0)
		.done();
}

// --- loading the client ---------------------------------------------------
function loadClient() {
	const file = path.join(__dirname, 'HaikuRemoteDesktop.js');
	const source = fs.readFileSync(file, 'utf8');
	// RemotePattern builds a scratch canvas at load time
	// (HaikuRemoteDesktop.js:919), so document has to exist far enough for
	// that. Nothing below is exercised by the text path.
	const stubElement = {
		width: 8, height: 8,
		getContext() {
			return {
				createImageData() {
					return { data: new Uint8Array(8 * 8 * 4), width: 8,
						height: 8 };
				},
				putImageData() {},
				createPattern() { return null; }
			};
		}
	};
	const sandbox = {
		console: { log() {}, warn() {}, error() {} },
		document: { createElement() { return stubElement; } },
		window: undefined,
		localStorage: {},
		Uint8Array, Uint32Array, Int32Array, DataView, ArrayBuffer, Math,
		JSON, Array, Object, String, Number, Boolean, Date, Error, isNaN,
		parseInt, parseFloat, setTimeout, clearTimeout,
		TextDecoder, TextEncoder
	};
	sandbox.globalThis = sandbox;
	vm.createContext(sandbox);
	// `function` declarations land on the sandbox object by themselves; `const`
	// ones do not, they live in the script's lexical scope. So an epilogue
	// re-exports every top-level constant by name, scraped from the source
	// rather than listed here -- a hand-written list would silently go stale
	// and leave a constant reading `undefined`, which in a wire test means
	// sending opcode 0 and asserting on the silence that follows. (That is
	// exactly how this file failed on its first run.)
	const names = [];
	const re = /^const\s+([A-Za-z_$][\w$]*)\s*=/gm;
	let m;
	while ((m = re.exec(source)) !== null)
		names.push(m[1]);
	if (names.length < 50)
		throw new Error('only ' + names.length + ' top-level constants found; '
			+ 'the scrape is broken, not the client');
	const epilogue = '\n;globalThis.__constants = {'
		+ names.map(n => JSON.stringify(n) + ':' + n).join(',') + '};\n';
	// The file is all declarations plus init(), which is never called here.
	vm.runInContext(source + epilogue, sandbox, { filename: file });
	Object.assign(sandbox, sandbox.__constants);
	return sandbox;
}

const client = loadClient();

function FakeSession(canvas) {
	this.context = canvas;
	this.removeClipping = function() {};
	this.applyClipping = function() {};
}

function FakeReply() {
	this.frames = [];
	const self = this;
	this.dataView = {
		values: [],
		writeInt32(v) { this.values.push(v); },
		writeFloat32(v) { this.values.push(v); },
		position: 0
	};
	this.start = function(code) {
		self.current = { code, values: [] };
		self.dataView.values = self.current.values;
	};
	this.flush = function() { self.frames.push(self.current); };
}

// Drive one wire message straight at RemoteState, the way
// RemoteDesktopSession.messageReceived does after it has eaten the token
// (HaikuRemoteDesktop.js:2262-2269).
function deliver(state, canvas, reply, code, payload) {
	const frame = new Uint8Array(6 + 4 + payload.length);
	const view = new DataView(frame.buffer);
	view.setUint16(0, code, true);
	view.setUint32(2, frame.length, true);
	view.setInt32(6, 1, true);              // token
	frame.set(payload, 10);
	const message = new client.RemoteMessage(null);
	message.attach(frame, 0);
	message.dataView.readInt32();           // the token, as the session does
	state.messageReceived(message, reply);
}

function drawRun(opts) {
	const o = opts || {};
	const canvas = new ModelCanvas(o.width || 300, o.height || 300);
	const session = new FakeSession(canvas);
	const state = new client.RemoteState(session, 1);
	const reply = new FakeReply();
	state.invalidated = true;
	deliver(state, canvas, reply, client.RP_SET_FONT, fontPayload(o));
	if (o.view) {
		// RP_SET_TRANSFORM: bool isIdentity, then the six BAffineTransform
		// members as DOUBLES, agg order sx shy shx sy tx ty
		// (RemoteMessage.cpp:313-327).
		const v = o.view;
		deliver(state, canvas, reply, client.RP_SET_TRANSFORM,
			new Writer().u8(0).f64(v.sx === undefined ? 1 : v.sx)
				.f64(v.shy || 0).f64(v.shx || 0)
				.f64(v.sy === undefined ? 1 : v.sy)
				.f64(v.tx || 0).f64(v.ty || 0).done());
	}
	const text = o.text === undefined ? 'Rotated' : o.text;
	const origin = o.origin || [150, 150];
	if (o.offsets) {
		const w = new Writer().str(text);
		for (const p of o.offsets)
			w.f32(p[0]).f32(p[1]);
		deliver(state, canvas, reply, client.RP_DRAW_STRING_WITH_OFFSETS,
			w.done());
	} else {
		deliver(state, canvas, reply, client.RP_DRAW_STRING,
			new Writer().f32(origin[0]).f32(origin[1]).str(text).done());
	}
	return { canvas, reply, state };
}

// --- the checks -----------------------------------------------------------
const checks = [];
const failures = [];

function check(label, ok, detail) {
	checks.push(label);
	if (ok) {
		console.log('  ok    ' + label);
	} else {
		failures.push(label);
		console.log('  FAIL  ' + label + (detail ? '  ' + detail : ''));
	}
}

// A run that inked nothing has no box. That has to come out as a failed check
// and not as a TypeError two checks later: one of the mutation arms below moves
// the text clean off the canvas, and a harness that crashes there reports no
// verdict at all, which reads the same as a verdict nobody wrote.
const NO_BOX = [NaN, NaN, NaN, NaN];

function boxOf(r) { return r.canvas.inkBox() || NO_BOX; }

function realBox(box) { return box !== NO_BOX && box.every(v => !isNaN(v)); }

console.log('  -- the client loads and draws --');
check('the client source loads and exports its drawing classes',
	typeof client.RemoteState === 'function'
	&& typeof client.RemoteFont === 'function'
	&& typeof client.RP_DRAW_STRING === 'number');

const flat = drawRun({});
const flatBox = boxOf(flat);
check('an upright run draws ink',
	realBox(flatBox) && flat.canvas.inkPixels() > 0, JSON.stringify(flatBox));
check('an upright run reports a pen to the right of its origin',
	flat.reply.frames.length === 1
	&& flat.reply.frames[0].code === client.RP_DRAW_STRING_RESULT
	&& Math.abs(flat.reply.frames[0].values[1] - (150 + 7 * CHAR_WIDTH)) < 0.01
	&& Math.abs(flat.reply.frames[0].values[2] - 150) < 0.01,
	JSON.stringify(flat.reply.frames));

console.log('  -- the matrix, against hand-computed goldens --');
// Worked out from AGGTextRenderer.cpp:72-85 with a pencil, in y-down space:
//   a = -rotation in radians;  t = tan(90 - shear in radians)
//   M = R * S = ((cos a, cos a * t - sin a), (sin a, sin a * t + cos a))
// and canvas takes (a, b, c, d) = (m00, m10, m01, m11).  Deriving these from
// textTransform() would make the check a tautology, which is the failure mode
// #423 shipped; they are pinned here instead.
const goldens = [
	['upright', { rotation: 0, shear: 90 }, null],
	['rotation 30', { rotation: 30, shear: 90 },
		{ a: 0.8660254, b: -0.5, c: 0.5, d: 0.8660254 }],
	['rotation 90', { rotation: 90, shear: 90 },
		{ a: 0, b: -1, c: 1, d: 0 }],
	['shear 45', { rotation: 0, shear: 45 },
		{ a: 1, b: 0, c: 1, d: 1 }],
	['both', { rotation: 30, shear: 45 },
		{ a: 0.8660254, b: -0.5, c: 1.3660254, d: 0.3660254 }]
];
for (const [label, spec, want] of goldens) {
	const font = new client.RemoteFont();
	font.rotation = spec.rotation;
	font.shear = spec.shear;
	const got = font.textTransform();
	if (want === null) {
		check(label + ' needs no transform at all', got === null,
			JSON.stringify(got));
		continue;
	}
	check(label + ' composes to the hand-computed matrix',
		got !== null
		&& ['a', 'b', 'c', 'd'].every(k => Math.abs(got[k] - want[k]) < 1e-6),
		JSON.stringify(got) + ' want ' + JSON.stringify(want));
}
check('a default RemoteFont is upright, i.e. shear 90 and not 0',
	new client.RemoteFont().shear === 90
	&& new client.RemoteFont().textTransform() === null,
	String(new client.RemoteFont().shear));
check('shear and rotation compose in the server order (R*S)',
	Math.abs(new (function() {
		const f = new client.RemoteFont();
		f.rotation = 30; f.shear = 45;
		this.c = f.textTransform().c;
	})().c - 1.3660254) < 1e-6);

console.log('  -- the discriminator: rotated is not horizontal --');
const quarter = drawRun({ rotation: 90 });
const turnBox = boxOf(quarter);
const flatW = flatBox[2] - flatBox[0], flatH = flatBox[3] - flatBox[1];
const turnW = turnBox[2] - turnBox[0], turnH = turnBox[3] - turnBox[1];
check('both runs inked somewhere on the canvas at all',
	realBox(flatBox) && realBox(turnBox),
	JSON.stringify(flatBox) + ' / ' + JSON.stringify(turnBox));
check("an upright run's ink box is wider than it is tall",
	flatW > 2 * flatH, flatW + 'x' + flatH);
check("a quarter-turned run's ink box is TALLER than it is wide",
	turnH > 2 * turnW, turnW + 'x' + turnH);
check('turning the run swaps the box extents',
	Math.abs(turnH - flatW) <= 2 && Math.abs(turnW - flatH) <= 2,
	'flat ' + flatW + 'x' + flatH + ' turned ' + turnW + 'x' + turnH);
check('the turned box is not the upright one',
	JSON.stringify(turnBox) !== JSON.stringify(flatBox),
	JSON.stringify(turnBox));
// ...and the reason a box check is needed rather than a count: a count is the
// same either way, which is how the defect survived.
check('ink pixel counts CANNOT tell a turned run from an upright one',
	Math.abs(quarter.canvas.inkPixels() - flat.canvas.inkPixels())
		< 0.1 * flat.canvas.inkPixels(),
	quarter.canvas.inkPixels() + ' vs ' + flat.canvas.inkPixels());
check('character counts CANNOT tell them apart either',
	quarter.canvas.charBoxes.length === flat.canvas.charBoxes.length,
	quarter.canvas.charBoxes.length + ' vs ' + flat.canvas.charBoxes.length);

for (const [rot, want] of [[0, 0], [30, 30], [90, 90], [-90, -90], [180, 180]]) {
	const r = drawRun({ rotation: rot });
	const axis = r.canvas.runAxisDegrees();
	check('ink drawn at rotation=' + rot + ' runs at ' + want + ' degrees',
		axis !== null && angleDifference(axis, want) <= 2, String(axis));
}
check('a one-character run reports NO direction rather than a made-up 0',
	drawRun({ text: 'X' }).canvas.runAxisDegrees() === null,
	String(drawRun({ text: 'X' }).canvas.runAxisDegrees()));

console.log('  -- the pen follows the turned baseline --');
const turnPen = quarter.reply.frames[0].values;
check('RP_DRAW_STRING_RESULT for a quarter-turned run goes straight UP',
	Math.abs(turnPen[1] - 150) < 0.01
	&& Math.abs(turnPen[2] - (150 - 7 * CHAR_WIDTH)) < 0.01,
	JSON.stringify(turnPen));

console.log('  -- shear --');
const leanBack = drawRun({ shear: 45 });
const leanFwd = drawRun({ shear: 135 });
check('shear widens the ink box without turning the run',
	boxOf(leanBack)[2] - boxOf(leanBack)[0] > flatW + 4
	&& angleDifference(leanBack.canvas.runAxisDegrees(), 0) <= 2,
	JSON.stringify(boxOf(leanBack)));
check('shear<90 leans the ink LEFT of where upright text started',
	boxOf(leanBack)[0] < flatBox[0] - 4,
	boxOf(leanBack)[0] + ' vs ' + flatBox[0]);
check('shear>90 leans the ink RIGHT of where upright text ended',
	boxOf(leanFwd)[2] > flatBox[2] + 4,
	boxOf(leanFwd)[2] + ' vs ' + flatBox[2]);
check('the two shears lean opposite ways, so the sign is not ignored',
	boxOf(leanBack)[0] < boxOf(leanFwd)[0]
	&& boxOf(leanBack)[2] < boxOf(leanFwd)[2],
	JSON.stringify(boxOf(leanBack)) + ' vs ' + JSON.stringify(boxOf(leanFwd)));
check('shear does NOT change the reported pen (StringWidth is untransformed)',
	Math.abs(leanBack.reply.frames[0].values[1] - (150 + 7 * CHAR_WIDTH)) < 0.01,
	JSON.stringify(leanBack.reply.frames[0].values));

console.log('  -- false bold --');
// agg conv_contour offsets the outline OUTWARD by falseBoldWidth
// (AGGTextRenderer.cpp:84 + agg_vcgen_contour.h:54 + agg_math_stroke.h:136),
// so 2.0 in must mean 2 px per side out.  Pinned from the specification.
const bold2 = drawRun({ falseBold: 2 });
const bb = boxOf(bold2);
check('false_bold_width grows the ink by itself on every side',
	Math.abs((flatBox[0] - bb[0]) - 2) <= 1
	&& Math.abs((flatBox[1] - bb[1]) - 2) <= 1
	&& Math.abs((bb[2] - flatBox[2]) - 2) <= 1
	&& Math.abs((bb[3] - flatBox[3]) - 2) <= 1,
	JSON.stringify(bb) + ' vs ' + JSON.stringify(flatBox));
check('false_bold_width is drawn as a stroke of twice the width',
	bold2.canvas.calls.some(c => c[0] === 'strokeText' && c[4] === 4),
	JSON.stringify(bold2.canvas.calls.filter(c => c[0] === 'strokeText')));
check('false_bold_width puts down strictly more ink',
	bold2.canvas.inkPixels() > flat.canvas.inkPixels(),
	bold2.canvas.inkPixels() + ' vs ' + flat.canvas.inkPixels());
check('false_bold_width does NOT move the pen',
	Math.abs(bold2.reply.frames[0].values[1] - (150 + 7 * CHAR_WIDTH)) < 0.01,
	JSON.stringify(bold2.reply.frames[0].values));
check('an upright run with no false bold strokes nothing at all',
	!flat.canvas.calls.some(c => c[0] === 'strokeText'),
	JSON.stringify(flat.canvas.calls));
const boldTurned = drawRun({ rotation: 90, falseBold: 2 });
check('false bold and rotation compose: still turned, still fattened',
	angleDifference(boldTurned.canvas.runAxisDegrees(), 90) <= 2
	&& boldTurned.canvas.inkPixels() > quarter.canvas.inkPixels(),
	boldTurned.canvas.runAxisDegrees() + ' '
		+ boldTurned.canvas.inkPixels() + ' vs '
		+ quarter.canvas.inkPixels());

console.log('  -- RP_STRING_WIDTH is deliberately untransformed --');
// StringWidth is the sum of the UNTRANSFORMED advances: GlyphLayoutEngine has
// no notion of the embedded transform, which lives one level up in
// AGGTextRenderer. So a rotated or sheared or false-bolded font must answer the
// same width as an upright one. Getting this "consistent" with the rotation
// would be a new bug wearing the old one's clothes, so it has its own check.
function stringWidth(opts) {
	const canvas = new ModelCanvas(300, 300);
	const state = new client.RemoteState(new FakeSession(canvas), 1);
	const reply = new FakeReply();
	state.invalidated = true;
	deliver(state, canvas, reply, client.RP_SET_FONT, fontPayload(opts));
	deliver(state, canvas, reply, client.RP_STRING_WIDTH,
		new Writer().str('Rotated').done());
	const frame = reply.frames[reply.frames.length - 1];
	return frame && frame.code === client.RP_STRING_WIDTH_RESULT
		? frame.values[1] : null;
}
const plainWidth = stringWidth({});
check('RP_STRING_WIDTH is answered at all', plainWidth !== null,
	String(plainWidth));
check('RP_STRING_WIDTH is the untransformed advance sum',
	Math.abs(plainWidth - 7 * CHAR_WIDTH) < 0.01, String(plainWidth));
for (const [label, spec] of [['rotation 90', { rotation: 90 }],
	['rotation 30', { rotation: 30 }], ['shear 45', { shear: 45 }],
	['false bold 2', { falseBold: 2 }]]) {
	check('RP_STRING_WIDTH is unchanged by ' + label,
		Math.abs(stringWidth(spec) - plainWidth) < 0.01,
		stringWidth(spec) + ' vs ' + plainWidth);
}

console.log('  -- WITH_OFFSETS --');
// The WITH_OFFSETS overload does not translate by a baseline
// (AGGTextRenderer.cpp:415-416), so the server's own origins go through the
// embedded transform about the view origin.  20 degrees, not 90, only so the
// result stays on the canvas.
const offFlat = drawRun({ offsets: [[80, 150], [100, 150], [120, 150]],
	text: 'abc' });
const offTurned = drawRun({ rotation: 20, text: 'abc',
	offsets: [[80, 150], [100, 150], [120, 150]] });
check('WITH_OFFSETS runs flat when the font is upright',
	offFlat.canvas.charBoxes.length === 3
	&& angleDifference(offFlat.canvas.runAxisDegrees(), 0) <= 2,
	String(offFlat.canvas.runAxisDegrees()));
check('WITH_OFFSETS puts the SERVER\'s own origins through the font transform',
	offTurned.canvas.charBoxes.length === 3
	&& angleDifference(offTurned.canvas.runAxisDegrees(), 20) <= 3,
	String(offTurned.canvas.runAxisDegrees()));
check('a turned WITH_OFFSETS run does not land in the upright one\'s box',
	JSON.stringify(boxOf(offTurned)) !== JSON.stringify(boxOf(offFlat)),
	JSON.stringify(boxOf(offTurned)));

console.log('  -- MUTATION: delete the fix, require these to go red --');
// The arm that makes all of the above evidence rather than decoration.  Each
// mutation reverts one part of the #494 fix in the LOADED client and re-runs
// the checks that are supposed to notice.  A mutation nothing notices is an
// assertion that cannot fail, which is the bug, not a test of it.
function withMutation(name, mutate, probe) {
	const saved = {
		textTransform: client.RemoteFont.prototype.textTransform,
		applyTextTransform: client.RemoteFont.prototype.applyTextTransform,
		applyFalseBold: client.RemoteFont.prototype.applyFalseBold
	};
	try {
		mutate();
		return probe();
	} finally {
		client.RemoteFont.prototype.textTransform = saved.textTransform;
		client.RemoteFont.prototype.applyTextTransform
			= saved.applyTextTransform;
		client.RemoteFont.prototype.applyFalseBold = saved.applyFalseBold;
	}
}

// 1. The pre-#494 client exactly: decode the three fields, apply none of them.
const discarded = withMutation('discard the transform', () => {
	client.RemoteFont.prototype.textTransform = function() { return null; };
	client.RemoteFont.prototype.applyTextTransform = function() {
		return null;
	};
}, () => {
	const r = drawRun({ rotation: 90 });
	return { box: boxOf(r), axis: r.canvas.runAxisDegrees(),
		ink: r.canvas.inkPixels(), chars: r.canvas.charBoxes.length };
});
check('MUTATION: with the transform discarded a rotated run still inks',
	discarded.ink > 0 && discarded.chars === 7,
	JSON.stringify(discarded));
check('MUTATION: ...and it comes out in the UPRIGHT box -- the #494 defect, '
	+ 'reproduced',
	JSON.stringify(discarded.box) === JSON.stringify(flatBox),
	JSON.stringify(discarded.box) + ' vs ' + JSON.stringify(flatBox));
check('MUTATION: ...so the direction check goes RED',
	angleDifference(discarded.axis, 90) > 2, String(discarded.axis));
check('MUTATION: ...while an ink-pixel check stays GREEN, which is why it was '
	+ 'never going to catch this',
	Math.abs(discarded.ink - flat.canvas.inkPixels())
		< 0.1 * flat.canvas.inkPixels(),
	discarded.ink + ' vs ' + flat.canvas.inkPixels());

// 2. Rotate the wrong way: the sign of the angle is a real thing to get wrong
//    and a screenshot makes it look plausible.
const unsigned = withMutation('do not negate the rotation', () => {
	client.RemoteFont.prototype.textTransform = function() {
		const rot = this.rotation || 0;
		const shear = this.shear === undefined ? 90 : this.shear;
		if (Math.abs(rot) < 1e-6 && Math.abs(shear - 90) < 1e-6)
			return null;
		const t = Math.tan((90 - shear) * Math.PI / 180);
		const a = rot * Math.PI / 180;     // NOT negated
		const cos = Math.cos(a), sin = Math.sin(a);
		return { a: cos, b: sin, c: cos * t - sin, d: sin * t + cos };
	};
}, () => drawRun({ rotation: 90 }).canvas.runAxisDegrees());
check('MUTATION: rotating the wrong way goes RED',
	angleDifference(unsigned, 90) > 2, String(unsigned));

// 3. Drop false bold.
const unbolded = withMutation('drop false bold', () => {
	client.RemoteFont.prototype.applyFalseBold = function() { return 0; };
}, () => boxOf(drawRun({ falseBold: 2 })));
check('MUTATION: dropping false_bold_width goes RED',
	JSON.stringify(unbolded) === JSON.stringify(flatBox),
	JSON.stringify(unbolded));

// 4. Shear only, ignored.
const unsheared = withMutation('ignore shear', () => {
	const real = client.RemoteFont.prototype.textTransform;
	client.RemoteFont.prototype.textTransform = function() {
		const saved = this.shear;
		this.shear = 90;
		try { return real.call(this); } finally { this.shear = saved; }
	};
}, () => boxOf(drawRun({ shear: 45 })));
check('MUTATION: ignoring shear goes RED',
	JSON.stringify(unsheared) === JSON.stringify(flatBox),
	JSON.stringify(unsheared));

check('the real behaviour is back after the mutation arms',
	JSON.stringify(boxOf(drawRun({ rotation: 90 }))) === JSON.stringify(turnBox)
	&& JSON.stringify(boxOf(drawRun({}))) === JSON.stringify(flatBox));

console.log('  -- the view transform: RP_SET_TRANSFORM, opcode 49 (#501) --');
// A SEPARATE mechanism from the font's rotation/shear: it scales/turns the whole
// view the text lands in, and in this client it reaches text through the canvas
// CTM applyContext sets (HaikuRemoteDesktop.js:1229-1233), offset-conjugated
// exactly as Painter::SetTransform (Painter.cpp:372-383).  So a scaled view
// enlarges the glyph RASTER via fillText under the CTM, not merely the spacing
// -- the difference between a filled letter and the dot lattice #27 fixed in the
// C++ client.  A 600x600 canvas with the origin at (150,150) keeps a 2x-scaled
// run (origin -> (300,300)) on the canvas whichever way it also turns.
const BIG = { width: 600, height: 600 };
const vFlat = drawRun(Object.assign({}, BIG));
const vFlatBox = boxOf(vFlat);
const vfW = vFlatBox[2] - vFlatBox[0], vfH = vFlatBox[3] - vFlatBox[1];
const vScaled = drawRun(Object.assign({ view: { sx: 2, sy: 2 } }, BIG));
const vScaledBox = boxOf(vScaled);
const vsW = vScaledBox[2] - vScaledBox[0], vsH = vScaledBox[3] - vScaledBox[1];
check('a view-scaled run is drawn, not discarded',
	realBox(vScaledBox) && vScaled.canvas.inkPixels() > 0,
	JSON.stringify(vScaledBox));
check("a view-scaled run's ink box is about twice the identity box on each axis",
	Math.abs(vsW - 2 * vfW) <= 2 && Math.abs(vsH - 2 * vfH) <= 2,
	vfW + 'x' + vfH + ' -> ' + vsW + 'x' + vsH);
// THE DISCRIMINATOR, against an INDEPENDENT oracle: the area scale factor of an
// affine is |determinant| = sx*sy = 4, worked out here from the matrix
// definition and NOT from the client's transform code.  A dot lattice -- spacing
// scaled, each glyph raster left alone -- would ink ~1x, which is exactly why a
// count-vs-zero or "did text draw?" check passes on the bug.
const det = 2 * 2;
check('a view-scaled run inks about |det| = 4x the identity run (independent '
	+ 'area-scale oracle; a lattice would ink ~1x)',
	Math.abs(vScaled.canvas.inkPixels() / vFlat.canvas.inkPixels() - det)
		< 0.15 * det,
	vScaled.canvas.inkPixels() + ' vs ' + vFlat.canvas.inkPixels() + ' x' + det);
check('character counts CANNOT tell a view-scaled run from the identity one',
	vScaled.canvas.charBoxes.length === vFlat.canvas.charBoxes.length,
	vScaled.canvas.charBoxes.length + ' vs ' + vFlat.canvas.charBoxes.length);

// Translation-only view: the fast path all real traffic takes.  Area is
// preserved (det 1), so the ink count is unchanged and the box only shifts.
const vShift = drawRun(Object.assign({ view: { tx: 40, ty: 25 } }, BIG));
const shBox = boxOf(vShift);
check('a translation-only view leaves the ink count unchanged',
	vShift.canvas.inkPixels() === vFlat.canvas.inkPixels(),
	vShift.canvas.inkPixels() + ' vs ' + vFlat.canvas.inkPixels());
check('a translation-only view shifts the box by the translation, unscaled',
	Math.abs((shBox[0] - vFlatBox[0]) - 40) <= 1
	&& Math.abs((shBox[1] - vFlatBox[1]) - 25) <= 1
	&& (shBox[2] - shBox[0]) === vfW,
	JSON.stringify(shBox) + ' vs ' + JSON.stringify(vFlatBox) + '+(40,25)');

// Composition with the FONT transform: font-rotated 90 AND view-scaled 2x comes
// out both turned and enlarged.
const vBoth = drawRun(Object.assign({ view: { sx: 2, sy: 2 }, rotation: 90 },
	BIG));
const bBox = boxOf(vBoth);
check('a view-scaled, font-rotated run is BOTH turned and enlarged',
	realBox(bBox) && (bBox[3] - bBox[1]) > (bBox[2] - bBox[0])
	&& vBoth.canvas.inkPixels() > 2.5 * vFlat.canvas.inkPixels(),
	JSON.stringify(bBox) + ' ink ' + vBoth.canvas.inkPixels());

// MUTATION: the pre-#501 client -- decode RP_SET_TRANSFORM, apply none of it.
// applyContext gates the view transform on !transform.isIdentity()
// (HaikuRemoteDesktop.js:1229), so forcing isIdentity true is exactly
// "decoded and discarded".
const savedIsIdentity = client.RemoteTransform.prototype.isIdentity;
let mutBox, mutInk;
try {
	client.RemoteTransform.prototype.isIdentity = function() { return true; };
	const m = drawRun(Object.assign({ view: { sx: 2, sy: 2 } }, BIG));
	mutBox = boxOf(m);
	mutInk = m.canvas.inkPixels();
} finally {
	client.RemoteTransform.prototype.isIdentity = savedIsIdentity;
}
check('MUTATION: with the view transform discarded a scaled run still inks',
	mutInk > 0, String(mutInk));
check('MUTATION: ...and it comes out in the IDENTITY box, so a count-vs-zero '
	+ 'check stays GREEN -- the #501 defect reproduced',
	JSON.stringify(mutBox) === JSON.stringify(vFlatBox),
	JSON.stringify(mutBox) + ' vs ' + JSON.stringify(vFlatBox));
check('MUTATION: ...so the |det| area-scale discriminator goes RED',
	!(Math.abs(mutInk / vFlat.canvas.inkPixels() - det) < 0.15 * det),
	mutInk + ' vs ' + vFlat.canvas.inkPixels());
check('the real view-transform behaviour is back after the mutation arm',
	JSON.stringify(boxOf(drawRun(Object.assign({ view: { sx: 2, sy: 2 } },
		BIG)))) === JSON.stringify(vScaledBox));

console.log('');
console.log('SELFTEST_CHECKS=' + checks.length);
if (failures.length) {
	console.log('SELFTEST=FAIL  (' + failures.length + '/' + checks.length
		+ ' failed: ' + failures.join(', ') + ')');
	process.exit(1);
}
console.log('SELFTEST=PASS');
process.exit(0);
