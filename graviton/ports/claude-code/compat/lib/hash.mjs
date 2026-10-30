// Bun.hash and the members of it the bundle calls: wyhash (the default), crc32,
// xxHash64. Bit-exact against stock Bun 1.4.4 (compat/test/hash.vectors.json).
//
// Bun.hash is Zig's std.hash.Wyhash (final v4.2) one-shot `hash(seed, input)`,
// ported line for line below. Strings hash as their UTF-8 bytes; binary inputs as
// their bytes; the seed is reduced to u64. Results are BigInt, like Bun's.
import { crc32 as zlibCrc32 } from "node:zlib";

const M64 = (1n << 64n) - 1n;
const S0 = 0xa0761d6478bd642fn, S1 = 0xe7037ed1a0b428dbn,
	S2 = 0x8ebc6af09c88c6e3n, S3 = 0x589965cc75374cc3n;
const SECRET = [S0, S1, S2, S3];
const enc = new TextEncoder();

export function toBytes(data) {
	if (typeof data === "string") return enc.encode(data);
	if (data instanceof ArrayBuffer || data instanceof SharedArrayBuffer) return new Uint8Array(data);
	if (ArrayBuffer.isView(data)) return new Uint8Array(data.buffer, data.byteOffset, data.byteLength);
	throw new TypeError("Expected a string, ArrayBuffer or TypedArray");
}

function toSeed(seed) {
	if (seed === undefined) return 0n;
	if (typeof seed === "bigint") return BigInt.asUintN(64, seed);
	if (typeof seed === "number") return BigInt.asUintN(64, BigInt(Math.trunc(seed)));
	throw new TypeError("Expected seed to be a number or bigint");
}

// Little-endian unsigned read of `n` (4 or 8) bytes.
function read(b, at, n) {
	let v = 0n;
	for (let i = n - 1; i >= 0; i--) v = (v << 8n) | BigInt(b[at + i]);
	return v;
}

function mum(a, b) {
	const x = a * b;
	return [x & M64, (x >> 64n) & M64];
}

function mix(a, b) {
	const [lo, hi] = mum(a, b);
	return lo ^ hi;
}

export function wyhash(data, seed) {
	const input = toBytes(data), len = input.length, sd = toSeed(seed);
	const s0 = sd ^ mix(sd ^ S0, S1);
	const state = [s0, s0, s0];
	let a, b;
	if (len <= 16) {
		if (len >= 4) {
			const end = len - 4, quarter = (len >> 3) << 2;
			a = (read(input, 0, 4) << 32n) | read(input, quarter, 4);
			b = (read(input, end, 4) << 32n) | read(input, end - quarter, 4);
		} else if (len > 0) {
			a = (BigInt(input[0]) << 16n) | (BigInt(input[len >> 1]) << 8n) | BigInt(input[len - 1]);
			b = 0n;
		} else {
			a = 0n;
			b = 0n;
		}
	} else {
		let i = 0;
		if (len >= 48) {
			while (i + 48 < len) {
				for (let r = 0; r < 3; r++) {
					const x = read(input, i + 16 * r, 8), y = read(input, i + 16 * r + 8, 8);
					state[r] = mix(x ^ SECRET[r + 1], y ^ state[r]);
				}
				i += 48;
			}
			state[0] ^= state[1] ^ state[2];
		}
		// final1(input, i): fold the tail in 16-byte steps, then take the last 16 bytes.
		let j = i;
		while (j + 16 < len) {
			state[0] = mix(read(input, j, 8) ^ S1, read(input, j + 8, 8) ^ state[0]);
			j += 16;
		}
		a = read(input, len - 16, 8);
		b = read(input, len - 8, 8);
	}
	// final2
	a ^= S1;
	b ^= state[0];
	[a, b] = mum(a, b);
	return mix(a ^ S0 ^ BigInt(len), b ^ S1);
}

export function crc32(data, seed) {
	return zlibCrc32(toBytes(data), seed === undefined ? 0 : Number(seed) >>> 0);
}

// XXH64, reference algorithm (Collet), 64-bit lanes in BigInt.
const P1 = 11400714785074694791n, P2 = 14029467366897019727n, P3 = 1609587929392839161n,
	P4 = 9650029242287828579n, P5 = 2870177450012600261n;
const rotl = (x, r) => ((x << BigInt(r)) | (x >> BigInt(64 - r))) & M64;
const round64 = (acc, lane) => (rotl((acc + lane * P2) & M64, 31) * P1) & M64;
const merge = (acc, v) => (((acc ^ round64(0n, v)) * P1) & M64) + P4 & M64;

export function xxHash64(data, seed) {
	const p = toBytes(data), len = p.length, sd = toSeed(seed);
	let i = 0, h;
	if (len >= 32) {
		let v1 = (sd + P1 + P2) & M64, v2 = (sd + P2) & M64, v3 = sd, v4 = (sd - P1) & M64;
		while (i + 32 <= len) {
			v1 = round64(v1, read(p, i, 8));
			v2 = round64(v2, read(p, i + 8, 8));
			v3 = round64(v3, read(p, i + 16, 8));
			v4 = round64(v4, read(p, i + 24, 8));
			i += 32;
		}
		h = (rotl(v1, 1) + rotl(v2, 7) + rotl(v3, 12) + rotl(v4, 18)) & M64;
		h = merge(h, v1); h = merge(h, v2); h = merge(h, v3); h = merge(h, v4);
	} else {
		h = (sd + P5) & M64;
	}
	h = (h + BigInt(len)) & M64;
	while (i + 8 <= len) {
		h ^= round64(0n, read(p, i, 8));
		h = (rotl(h, 27) * P1 + P4) & M64;
		i += 8;
	}
	if (i + 4 <= len) {
		h ^= (read(p, i, 4) * P1) & M64;
		h = (rotl(h, 23) * P2 + P3) & M64;
		i += 4;
	}
	while (i < len) {
		h ^= (BigInt(p[i]) * P5) & M64;
		h = (rotl(h, 11) * P1) & M64;
		i++;
	}
	h ^= h >> 33n; h = (h * P2) & M64;
	h ^= h >> 29n; h = (h * P3) & M64;
	h ^= h >> 32n;
	return h;
}

// Bun.hash(data, seed) is wyhash; the named algorithms hang off it. Only those
// the bundle calls are present -- the rest of Bun's list is absent, not faked.
export function makeHash() {
	const hash = (data, seed) => wyhash(data, seed);
	hash.wyhash = wyhash;
	hash.crc32 = crc32;
	hash.xxHash64 = xxHash64;
	return hash;
}
