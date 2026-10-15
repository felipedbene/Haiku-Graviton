#!/bin/bash
# Build the headless render path: WebContent + WebDriver (screenshot is built
# into WebDriver; there is no standalone headless-browser exe). -j32 suits a
# c7g.8xlarge. Run detached (nohup) so an SSH/agent death does not kill it.
export PATH=/boot/home/config/non-packaged/bin:/boot/system/bin:$PATH

# ---- single-instance cargo guard (DeBeOS #638) -----------------------------
# Ladybird's build runs cargo (ninja's depth-1 "cargo" job pool serialises it
# *within* one ninja, but a job pool does not bind across processes). This is a
# long detached build, so one that looks stalled gets re-launched -- which puts
# a second cargo on the same ${CMAKE_BINARY_DIR}/cargo/build target dir. That is
# silently unsafe here: std's File::lock returns ErrorKind::Unsupported on Haiku
# and cargo treats an unsupported lock as ACQUIRED without warning, so neither
# side blocks and both interleave writes. The torn rlib then stays cached and
# rustc keeps ICEing on it. Keyed on $CARGO_HOME, so this also excludes the
# other cargo drivers on the box, which share the registry. Drop once rust_bin
# ships >= nightly-2026-10-13 (upstream rust-lang/rust#164031).
# See graviton/docs/rust-file-locking.md.
LOCK_DIR="${CARGO_HOME:-$HOME/.cargo}/.debeos-cargo.lock"
mkdir -p "${CARGO_HOME:-$HOME/.cargo}"
if ! mkdir "$LOCK_DIR" 2>/dev/null; then
	owner=$(cat "$LOCK_DIR/pid" 2>/dev/null || true)
	if [ -n "$owner" ] && ! kill -0 "$owner" 2>/dev/null; then
		echo "== stale cargo lock from pid $owner -- taking it over =="
		rm -rf "$LOCK_DIR"
		mkdir "$LOCK_DIR"
	else
		echo "== ABORT: cargo already running here (pid ${owner:-unknown}); concurrent cargo corrupts the shared target dir on DeBeOS (#638) =="
		exit 1
	fi
fi
printf '%s\n' "$$" > "$LOCK_DIR/pid"
trap 'rm -rf "$LOCK_DIR"' EXIT

cd /boot/home/lb/build-wave2
ninja WebContent WebDriver -j32
echo "BUILD_RC=$? SENTINEL_DONE"
