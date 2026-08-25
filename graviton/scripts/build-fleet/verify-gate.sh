#!/bin/bash
# verify-gate.sh -- prove the gate artifact carries the arm64 swap fix.
#
# A zero exit code from jam is not evidence that the shipped binary is correct.
# So: extract libroot.so OUT of the freshly built haiku.hpkg and disassemble
# __swap_float / __swap_double. The bug is an operand-order inversion --
# `fmov s0, w0` first destroys the incoming float with the garbage in w0, so the
# function returns the loop's byte offset. The fixed form reads the argument out
# of s0 FIRST (`fmov w0, s0`), byte-reverses, then writes it back to s0.
set -u
FLEET=/opt/haiku/fleet
GEN=/opt/haiku/haiku/generated.arm64
PKGTOOL=$GEN/objects/linux/arm64/release/tools/package/package
OBJDUMP=$GEN/cross-tools-arm64/bin/aarch64-unknown-haiku-objdump
WORK=$(mktemp -d)
cd "$WORK" || exit 1

echo "=== VERIFY GATE $(date -u +%FT%TZ) ==="
echo "--- tools ---"
ls -la "$PKGTOOL" 2>&1 | tail -1
ls -la "$OBJDUMP" 2>&1 | tail -1

echo "--- extracting libroot.so from the fresh haiku.hpkg ---"
"$PKGTOOL" list "$FLEET/gate-out/haiku.hpkg" 2>/dev/null | grep -i 'libroot.so' | head -5
"$PKGTOOL" extract -C "$WORK" "$FLEET/gate-out/haiku.hpkg" lib/libroot.so 2>&1 | tail -3
find "$WORK" -name 'libroot.so*' -exec ls -la {} \; 2>/dev/null

LR=$(find "$WORK" -name 'libroot.so' -type f | head -1)
if [ -z "$LR" ]; then
	echo "FAIL: could not extract libroot.so"
	exit 2
fi

echo "--- disassembly of __swap_float ---"
"$OBJDUMP" -d --disassemble='__swap_float' "$LR" 2>/dev/null | sed -n '1,25p'
echo "--- disassembly of __swap_double ---"
"$OBJDUMP" -d --disassemble='__swap_double' "$LR" 2>/dev/null | sed -n '1,25p'

echo "--- VERDICT ---"
# In AArch64 objdump syntax the fixed first instruction reads s0 into w0:
#   fmov w0, s0        (fixed)      vs      fmov s0, w0      (buggy)
FLOAT=$("$OBJDUMP" -d --disassemble='__swap_float' "$LR" 2>/dev/null | grep -oE 'fmov[[:space:]]+[ws]0,[[:space:]]*[ws]0' | head -1)
DOUBLE=$("$OBJDUMP" -d --disassemble='__swap_double' "$LR" 2>/dev/null | grep -oE 'fmov[[:space:]]+[xd]0,[[:space:]]*[xd]0' | head -1)
echo "first insn of __swap_float : ${FLOAT:-<none>}"
echo "first insn of __swap_double: ${DOUBLE:-<none>}"
case "$FLOAT" in
	*"w0, s0"*) echo "PASS: __swap_float reads the argument out of s0 first (FIXED)" ;;
	*"s0, w0"*) echo "FAIL: __swap_float still clobbers s0 with w0 (BUGGY)" ;;
	*)          echo "INCONCLUSIVE: could not read __swap_float's first fmov" ;;
esac
case "$DOUBLE" in
	*"x0, d0"*) echo "PASS: __swap_double reads the argument out of d0 first (FIXED)" ;;
	*"d0, x0"*) echo "FAIL: __swap_double still clobbers d0 with x0 (BUGGY)" ;;
	*)          echo "INCONCLUSIVE: could not read __swap_double's first fmov" ;;
esac

echo "--- translator inventory in the fresh haiku_datatranslators.hpkg ---"
"$PKGTOOL" list "$FLEET/gate-out/haiku_datatranslators.hpkg" 2>/dev/null \
	| grep -iE 'translator' | sed 's/^[[:space:]]*//' | head -30
echo "--- translator COUNT ---"
"$PKGTOOL" list "$FLEET/gate-out/haiku_datatranslators.hpkg" 2>/dev/null \
	| grep -ciE 'translator$|Translator[[:space:]]*$|Translator'
cd /; rm -rf "$WORK"
