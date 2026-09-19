# Go toolchain (native haiku/arm64). GOROOT is also auto-detected from the
# bin/go symlink target, so `go` works even without this file.
export GOROOT=/boot/system/develop/lib/go
case ":$PATH:" in *":$GOROOT/bin:"*) ;; *) export PATH="$PATH:$GOROOT/bin";; esac
[ -n "$GOPATH" ] || export GOPATH="$HOME/go"
case ":$PATH:" in *":$GOPATH/bin:"*) ;; *) export PATH="$PATH:$GOPATH/bin";; esac
