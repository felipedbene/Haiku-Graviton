#!/bin/bash
# fetch-deps.sh <destdir>: download every tools/deps.lock tarball and refuse it
# unless its sha512 matches the pinned npm integrity. Unpacks to <destdir>/<name>.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd); dest=$1; mkdir -p "$dest"
grep -v '^#' "$here/deps.lock" | while read -r name ver url integ; do
	[ -n "$name" ] || continue
	t="$dest/$name-$ver.tgz"
	[ -f "$t" ] || curl -fsSL "$url" -o "$t"
	got="sha512-$(openssl dgst -sha512 -binary "$t" | base64 | tr -d '\n')"
	[ "$got" = "$integ" ] || { echo "fetch-deps: $name $ver integrity mismatch: $got" >&2; exit 1; }
	rm -rf "$dest/$name" "$dest/package" && tar xzf "$t" -C "$dest" && mv "$dest/package" "$dest/$name"
	echo "fetch-deps: $name $ver OK ($integ)"
done
