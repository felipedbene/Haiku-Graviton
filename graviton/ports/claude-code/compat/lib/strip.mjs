// Bun.stripANSI -- port of Bun bun-v1.4.4 src/jsc/bindings/stripANSI.cpp.
// findEscapeCharacter also stops at a standalone C1 ST (0x9c), which consumeANSI
// does not consume, so it is copied literally -- the same as never stopping there.
import { isEscapeCharacter, consumeANSI } from "./ansi.mjs";

export function stripANSI(input) {
	const s = String(input);
	const end = s.length;
	let out = null, start = 0;
	for (let i = 0; i < end; i++) {
		if (!isEscapeCharacter(s.charCodeAt(i))) continue;
		const next = consumeANSI(s, i, end);
		if (next === i) continue;		// not a sequence: kept as is
		out = (out ?? "") + s.slice(start, i);
		start = next;
		i = next - 1;
	}
	return out === null ? s : out + s.slice(start);
}
