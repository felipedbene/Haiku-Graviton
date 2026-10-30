# LIMITATIONS — what does not work, or does not match Bun, and why

Each entry: what, why, observable effect in Claude Code, and what a real fix would need.
Entries come from measurement (diff tests against stock Bun 1.4.4, runs on DeBeOS);
nothing here is a guess.

## Bun API compat (Node 24)

### `Bun.YAML.stringify`: `+0`
- **What:** Bun prints `+0` for a zero that JSC holds as a double (`let h=0.5; h-h`,
  `[1.5, 0]`, `new Float64Array([0])`); the port prints `0`.
- **Why:** JSC's int/double representation is not observable from V8. It isn't stable
  even inside Bun: the same array printed `- +0` in one mode and `0` in the other.
- **Effect:** frontmatter written by Claude Code could contain `0` where Bun would write
  `+0`. Both parse to the number 0. Frontmatter values are user text, so in practice this
  doesn't arise.
- **Fix would need:** nothing portable. JSC's internal number tagging can't be read from JS.

### `Bun.YAML.stringify`: engine error wording
- **What:** when the *engine* throws while reading a value (a revoked Proxy, a boxed
  Number whose `valueOf` returns a BigInt), the message is V8's, not JSC's. Errors thrown
  by user code (getters, `valueOf`) pass through unchanged.
- **Effect:** none on Claude Code's own data (plain frontmatter objects).
- **Fix would need:** a table mapping V8 to JSC messages per error site. Not attempted.

### `Bun.YAML.stringify`: maximum nesting depth
- **What:** the line-for-line recursive port overflows at ~1,950 levels on Node 24's
  default stack; Bun reaches ~23,400. Between the two, Bun returns output and the port
  throws `RangeError("Maximum call stack size exceeded.")` (JSC's wording, matched).
- **Effect:** none expected; frontmatter is shallow.
- **Fix would need:** an iterative rewrite of the walk (a deliberate departure from the
  line-for-line port), or a larger `--stack-size`.

### `Bun.zstdDecompressSync` / `zstdDecompress`: error message
- **What:** corrupt input throws an `Error` in both runtimes, but the text differs
  (Bun: `Decompression failed: InvalidZstdData`; Node: zlib's message).
- **Effect:** the bundle wraps it (`embedded asset is missing or corrupt`), so it isn't
  visible.
- **Fix would need:** a map from zlib zstd error codes to Bun's names.

### `Bun.TOML.parse`: dates and times are strings, not Temporal objects
- **What:** Bun 1.4.4 returns `Temporal.Instant`/`PlainDateTime`/`PlainDate`/`PlainTime`.
  Node 24 has no `Temporal` by default, so the port returns the string Bun's object
  stringifies to (Temporal canonical form, ported: offsets folded to UTC, leap second
  clamped, fraction ≤ 9 digits). `String(x)` is identical; `typeof` is not.
- **Effect:** the bundle's only TOML input is another agent's `config.toml` import, which
  it validates with zod; that schema has no date fields. Not observable in Claude Code
  as far as the call site shows.
- **Fix would need:** Node 24's V8 13.6 has Temporal behind `--harmony-temporal` ("in
  progress / experimental", measured: `typeof Temporal` = `object` with the flag). The
  port already exposes `parseWith(input, onDateTime)` to construct real Temporal objects.
  That would make an experimental engine flag part of the runtime. Owner's call.

### `Bun.TOML.parse`: Blob input, error-object extras, depth
- **Blob:** Bun reads a `Blob` argument synchronously; Node can't, so the port throws a
  `TypeError` saying to pass `await blob.text()`. The bundle passes file text.
- **Error objects:** type, message and `code` match (`SyntaxError "TOML Parse error: …"`,
  `ERR_INVALID_ARG_TYPE`, `ERR_OUT_OF_RANGE`). JSC adds `line`/`column`/`sourceURL` to
  error objects and V8 doesn't; `code` is an own non-enumerable property here and not own in Bun.
- **Depth:** Bun overflows its native stack on very deep nesting (inline tables > ~10k,
  arrays > ~20k, headers > ~50k); the port uses an explicit stack (toml.rs's own
  non-recursive structure) and parses those. Ten named vectors cover this, and the checker
  fails if any of them starts matching.
