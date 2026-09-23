#!/bin/sh
# Runs ON a DeBeOS/Haiku arm64 box. Installs the golang hpkg and runs the
# native acceptance tests: go version, go env, go build hello (+ run), and a
# small multi-package module build (+ run). Prints a clear PASS/FAIL banner.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
VER=$(sed -n '1s/^go//p' "$HERE/root/develop/lib/go/VERSION")
HPKG="$HERE/golang-${VER}-1-arm64.hpkg"

echo "############## INSTALL ##############"
INSTALLED=no
if pkgman install -y "$HPKG" 2>&1; then
	INSTALLED=pkgman
else
	echo "pkgman install failed; falling back to /system/packages drop"
	cp "$HPKG" /boot/system/packages/ && INSTALLED=copy || true
fi
echo "install-method=$INSTALLED"

# Resolve a working go: prefer the installed one on PATH, else the extracted GOROOT.
GO=""
if command -v go >/dev/null 2>&1; then
	GO="$(command -v go)"
elif [ -x /boot/system/bin/go ]; then
	GO=/boot/system/bin/go
fi
if [ -z "$GO" ] || ! "$GO" version >/dev/null 2>&1; then
	echo "installed go not resolvable yet; using extracted GOROOT directly"
	export GOROOT="$HERE/root/develop/lib/go"
	GO="$GOROOT/bin/go"
fi
echo "using go: $GO"

# --- #504 watchdog -------------------------------------------------------------
# Bound each native `go` build with a wall-clock cap and, on a stall, capture the
# forensic trail issue #504 asked for -- ps of go/compile/link, whether `go env`
# still answers (wedged-go vs wedged-shell/agent), and a late-appearing go.mod --
# instead of hanging to the SSM 600 s kill (rc=137, no output). A first
# module-mode `go` stalled once ~14 min post-launch and never reproduced (4/4
# later), so this is a diagnostic net, not a fix. It is non-behavioural for a
# healthy build: `timeout` only fires on a true stall, and where `timeout` is
# absent the command runs unbounded exactly as before. Same guarded-`timeout` +
# ps-capture pattern the build fleet already uses (scripts/build-fleet).
GO_TMO="${GO_TMO:-120}"   # generous: healthy native builds here run 8-10 s

run_bounded() {   # run_bounded <label> <cmd...>
	_label="$1"; shift
	if command -v timeout >/dev/null 2>&1; then
		timeout "$GO_TMO" "$@"; _rc=$?
	else
		"$@"; _rc=$?
	fi
	if [ "$_rc" = 124 ]; then
		echo "!! #504 WATCHDOG: '$_label' exceeded ${GO_TMO}s -- capturing diagnostics"
		echo "-- ps (go/compile/link) --"
		ps 2>/dev/null | grep -iE 'go|compile|link' | grep -vE 'grep' || true
		echo "-- does 'go env' still answer? (wedged-go vs wedged-shell/agent) --"
		if command -v timeout >/dev/null 2>&1; then
			timeout 15 "$GO" env GOOS GOARCH GOPROXY GOSUMDB GOTOOLCHAIN 2>&1 \
				|| echo "(go env did NOT answer -> go itself is wedged)"
		else
			"$GO" env GOOS GOARCH GOPROXY GOSUMDB GOTOOLCHAIN 2>&1 || true
		fi
		[ -f go.mod ] && { echo "-- go.mod present (appeared during the stall) --"; cat go.mod; }
	fi
	return $_rc
}

echo "############## go version ##############"
"$GO" version

echo "############## go env (key vars) ##############"
"$GO" env GOROOT GOOS GOARCH GOVERSION CGO_ENABLED GOPATH GOCACHE

echo "############## go build hello ##############"
cd "$HERE/tests/hello"
rm -f hello
HELLO_RC=0; run_bounded "go build hello" "$GO" build -o hello ./hello.go || HELLO_RC=$?
if [ "$HELLO_RC" = 0 ]; then
	echo "-- file --"; file ./hello
	echo "-- run --"; ./hello; HELLO_RC=$?
fi
echo "hello-rc=$HELLO_RC"

echo "############## go build multi-package module ##############"
cd "$HERE/tests/greetmod"
rm -f greetmod
GREET_RC=0; run_bounded "go build greetmod" "$GO" build -o greetmod . || GREET_RC=$?
if [ "$GREET_RC" = 0 ]; then
	echo "-- file --"; file ./greetmod
	echo "-- run --"; ./greetmod; GREET_RC=$?
fi
echo "greet-rc=$GREET_RC"

echo "############## RESULT ##############"
if [ "$HELLO_RC" = 0 ] && [ "$GREET_RC" = 0 ]; then
	echo "NATIVE-GO-378: PASS"
else
	echo "NATIVE-GO-378: FAIL"
fi
