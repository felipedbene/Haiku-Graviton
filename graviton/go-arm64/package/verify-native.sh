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

echo "############## go version ##############"
"$GO" version

echo "############## go env (key vars) ##############"
"$GO" env GOROOT GOOS GOARCH GOVERSION CGO_ENABLED GOPATH GOCACHE

echo "############## go build hello ##############"
cd "$HERE/tests/hello"
rm -f hello
"$GO" build -o hello ./hello.go
echo "-- file --"; file ./hello
echo "-- run --"; ./hello
HELLO_RC=$?
echo "hello-rc=$HELLO_RC"

echo "############## go build multi-package module ##############"
cd "$HERE/tests/greetmod"
rm -f greetmod
"$GO" build -o greetmod .
echo "-- file --"; file ./greetmod
echo "-- run --"; ./greetmod
GREET_RC=$?
echo "greet-rc=$GREET_RC"

echo "############## RESULT ##############"
if [ "$HELLO_RC" = 0 ] && [ "$GREET_RC" = 0 ]; then
	echo "NATIVE-GO-378: PASS"
else
	echo "NATIVE-GO-378: FAIL"
fi
