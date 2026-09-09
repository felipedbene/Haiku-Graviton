#!/bin/bash
# build-openssh-arm64.sh -- cross-build OpenSSH for Haiku/arm64 and wrap it in
# an hpkg, on the c7g.metal builder.
#
# Why this exists: HaikuPorts publishes NO arm64 package repository at all
# (https://eu.hpkg.haiku-os.org/haikuports/master/ lists only riscv64, x86_64
# and x86_gcc2), so there is no prebuilt openssh hpkg to drop in. Everything
# below is a cross-build against the Haiku arm64 tree that the builder already
# has.
#
# It is deliberately independent of jam: it only *reads* the already-built
# haiku/haiku_devel package staging directories. It does not run jam, so it is
# safe to run while someone else is building an image (jam and this script must
# not both write generated.arm64, but this script never writes there).
#
# Usage:  ./build-openssh-arm64.sh
# Output: $OUT/openssh-$OSSH_VERSION-1-arm64.hpkg
#         $OUT/image/{sshd_config,ssh_config,launch-sshd,services,authorized_keys}
set -euo pipefail

HAIKU_TOP=${HAIKU_TOP:-/opt/haiku/haiku}
GEN=${GEN:-$HAIKU_TOP/generated.arm64}
OUT=${OUT:-/opt/haiku/ssh}
SYSROOT=${SYSROOT:-/opt/haiku/sysroot-arm64}
OSSH_VERSION=${OSSH_VERSION:-10.4p1}
ZLIB_VERSION=${ZLIB_VERSION:-1.3.1}
PKG_REVISION=1
SRCDIR="$(cd "$(dirname "$0")" && pwd)"

CT=$GEN/cross-tools-arm64
PB=$GEN/objects/haiku/arm64/packaging/packages_build/minimum
DEVPKG=$PB/hpkg_-haiku_devel.hpkg/contents
RTPKG=$PB/hpkg_-haiku.hpkg/contents
PACKAGE_TOOL=$(find "$GEN/objects/linux" -type f -name package -perm -u+x | head -1)

export PATH="$CT/bin:$PATH"
HOST=aarch64-unknown-haiku

echo "== sanity checks =="
[ -x "$CT/bin/$HOST-gcc" ] || { echo "no arm64 cross gcc at $CT/bin" >&2; exit 1; }
[ -n "$PACKAGE_TOOL" ] || { echo "host 'package' tool not built" >&2; exit 1; }
[ -d "$DEVPKG/develop/headers/posix" ] || {
	echo "haiku_devel staging dir missing: $DEVPKG" >&2
	echo "(a jam image build recreates it; wait for jam to finish and retry)" >&2
	exit 1
}

# ---------------------------------------------------------------------------
# 1. A stable sysroot.
#
# The cross-tools' own sysroot ships empty: Haiku's jam passes header and
# library paths explicitly, so nothing populates it. gcc *is* configured to look
# in <sysroot>/boot/system/develop/{headers,lib} (it just prunes the paths when
# they do not exist), so filling those two directories makes the toolchain work
# for ordinary autoconf packages.
#
# The files are COPIED, not symlinked into generated.arm64: jam recreates the
# package staging directories during an image build, and symlinks into them
# vanish mid-compile -- which shows up as bizarre configure results such as
# "checking for sys/mman.h... no".
# ---------------------------------------------------------------------------
echo "== staging sysroot at $SYSROOT =="
SR=$SYSROOT/boot/system
sudo mkdir -p "$SR/develop/lib"
sudo rsync -aL --delete "$DEVPKG/develop/headers/" "$SR/develop/headers/"
sudo rsync -a --delete --exclude '*.so' "$DEVPKG/develop/lib/" "$SR/develop/lib/"
# develop/lib's .so entries are symlinks to ../../lib, which only resolve on an
# installed system; take the real shared libraries from the haiku package.
for f in "$DEVPKG"/develop/lib/*.so; do
	b=$(basename "$f")
	[ -e "$RTPKG/lib/$b" ] && sudo cp -f "$RTPKG/lib/$b" "$SR/develop/lib/$b"
done
sudo rm -rf "$CT/sysroot"
sudo mkdir -p "$CT/sysroot/boot/system/develop"
sudo ln -sfn "$SR/develop/headers" "$CT/sysroot/boot/system/develop/headers"
sudo ln -sfn "$SR/develop/lib" "$CT/sysroot/boot/system/develop/lib"

mkdir -p "$OUT"
cd "$OUT"

# ---------------------------------------------------------------------------
# 2. zlib, static.
#
# OpenSSH 10 has no --without-zlib. A zlib hpkg does exist for arm64 (the
# bootstrap one), but linking it statically keeps sshd's only dependencies
# libroot/libnetwork/libbsd, all of which are in the base haiku package -- so
# sshd cannot be broken by a package dependency problem.
# ---------------------------------------------------------------------------
echo "== zlib $ZLIB_VERSION (static) =="
[ -f "zlib-$ZLIB_VERSION.tar.gz" ] || curl -sSLf -o "zlib-$ZLIB_VERSION.tar.gz" \
	"https://github.com/madler/zlib/releases/download/v$ZLIB_VERSION/zlib-$ZLIB_VERSION.tar.gz"
rm -rf "zlib-$ZLIB_VERSION"
tar xf "zlib-$ZLIB_VERSION.tar.gz"
(
	cd "zlib-$ZLIB_VERSION"
	CHOST=$HOST CC=$HOST-gcc AR=$HOST-ar RANLIB=$HOST-ranlib \
		./configure --static --prefix="$OUT/deps" >/dev/null
	make -j"$(nproc)" libz.a >/dev/null
	mkdir -p "$OUT/deps/lib" "$OUT/deps/include"
	cp libz.a "$OUT/deps/lib/"
	cp zlib.h zconf.h "$OUT/deps/include/"
)

# ---------------------------------------------------------------------------
# 3. OpenSSH.
#
# --without-openssl: no OpenSSL is packaged for Haiku/arm64 either, and porting
# it is a much bigger job. The cost is that only Ed25519 keys and the internal
# cipher/KEX set are available -- notably NO RSA and NO ECDSA, for host keys or
# for user keys. See docs/gap-analysis.md.
# ---------------------------------------------------------------------------
echo "== openssh $OSSH_VERSION =="
[ -f "openssh-$OSSH_VERSION.tar.gz" ] || curl -sSLf -o "openssh-$OSSH_VERSION.tar.gz" \
	"https://cdn.openbsd.org/pub/OpenBSD/OpenSSH/portable/openssh-$OSSH_VERSION.tar.gz"
[ -f "openssh-$OSSH_VERSION.patchset" ] || curl -sSLf -o "openssh-$OSSH_VERSION.patchset" \
	"https://raw.githubusercontent.com/haikuports/haikuports/master/net-misc/openssh/patches/openssh-$OSSH_VERSION.patchset"
rm -rf "openssh-$OSSH_VERSION"
tar xf "openssh-$OSSH_VERSION.tar.gz"
cd "openssh-$OSSH_VERSION"
# HaikuPorts' own patchset: Haiku paths (~/config/settings/ssh instead of
# ~/.ssh), a gcc2 build fix, and rename-instead-of-hardlink fixes.
patch -p1 -s < "../openssh-$OSSH_VERSION.patchset"

DEFPATH="/boot/home/config/non-packaged/bin:/boot/home/config/bin"
DEFPATH+=":/boot/system/non-packaged/bin:/boot/system/bin"
DEFPATH+=":/boot/system/apps:/boot/system/preferences"

./configure --host=$HOST --build="$(gcc -dumpmachine)" \
	--prefix=/boot/system \
	--bindir=/boot/system/bin --sbindir=/boot/system/bin \
	--sysconfdir=/boot/system/settings/ssh \
	--libexecdir=/boot/system/lib/openssh \
	--mandir=/boot/system/documentation/man \
	--with-privsep-path=/boot/system/var/empty \
	--with-privsep-user=sshd \
	--with-pid-dir=/boot/system/var \
	--with-default-path="$DEFPATH" \
	--with-zlib="$OUT/deps" \
	--without-openssl --disable-utmpx \
	LIBS="-lnetwork -lbsd"

# Canary: if the sysroot is incomplete, configure silently decides half the
# system headers are missing and the build fails much later in confusing ways.
grep -q '^#define HAVE_SYS_MMAN_H' config.h || {
	echo "sys/mman.h was not found -- the sysroot is incomplete (was jam" >&2
	echo "rebuilding the package staging dirs while this ran?)" >&2
	exit 1
}
make -j"$(nproc)"

rm -rf "$OUT/pkgroot"
make install-nokeys DESTDIR="$OUT/pkgroot"
cd "$OUT"

# ---------------------------------------------------------------------------
# 4. Wrap it in an hpkg.
#
# Package contents are rooted at /boot/system. settings/ssh is dropped from the
# package: /boot/system/settings is a shine-through (real BFS) directory, and we
# install our own sshd_config there through the image instead.
# ---------------------------------------------------------------------------
echo "== hpkg =="
STAGE=$OUT/pkgstage
rm -rf "$STAGE"
mkdir -p "$STAGE"
cp -a "$OUT/pkgroot/boot/system/." "$STAGE/"
rm -rf "$STAGE/settings"
install -m 755 "$SRCDIR/files/sshd_boot.sh" "$STAGE/bin/sshd_boot.sh"
mkdir -p "$STAGE/data/licenses"
cp "openssh-$OSSH_VERSION/LICENCE" "$STAGE/data/licenses/OpenSSH"
mkdir -p "$STAGE/var/empty"

cat > "$STAGE/.PackageInfo" <<EOF
name			openssh
version			$OSSH_VERSION-$PKG_REVISION
architecture		arm64
summary			"Secure Shell client and server (remote login program)"
description		"OpenSSH cross-built for Haiku/arm64 for the AWS Graviton port.
Configured --without-openssl because no OpenSSL package exists for arm64, so
only Ed25519 keys and OpenSSH's internal ciphers are supported. Statically
linked against zlib; the only shared library dependencies are libroot,
libnetwork and libbsd, all part of the base haiku package."
packager		"haiku-graviton port <felipe@localhost>"
# vendor "DeBeOS": this DeBeOS-built package is baked straight into
# system/packages, so it must report the same vendor as the DeBeOS repo -- else
# a later `pkgman install`/refresh that supersedes it would force an "allow
# vendor change" (the gcc/cc1 breakage of issue #39). Set at build time (no
# metadata repack needed for a package we build ourselves).
vendor			"DeBeOS"
licenses {
	"OpenSSH"
}
copyrights {
	"2005-2025 Tatu Ylonen et al."
}
provides {
	openssh = $OSSH_VERSION
	cmd:scp = $OSSH_VERSION
	cmd:sftp = $OSSH_VERSION
	cmd:ssh = $OSSH_VERSION
	cmd:ssh_add = $OSSH_VERSION
	cmd:ssh_agent = $OSSH_VERSION
	cmd:ssh_keygen = $OSSH_VERSION
	cmd:ssh_keyscan = $OSSH_VERSION
	cmd:sshd = $OSSH_VERSION
}
requires {
	haiku
}
urls {
	"https://www.openssh.com/"
}
EOF

HPKG=$OUT/openssh-$OSSH_VERSION-$PKG_REVISION-arm64.hpkg
rm -f "$HPKG"
# The host 'package' tool links libroot_build.so from the build's linux objects,
# which is not on the default loader search path -- point LD_LIBRARY_PATH at it.
export LD_LIBRARY_PATH="$GEN/objects/linux/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
(cd "$STAGE" && "$PACKAGE_TOOL" create -q "$HPKG")
"$PACKAGE_TOOL" list -i "$HPKG" | head -12
ls -l "$HPKG"

# ---------------------------------------------------------------------------
# 5. Image-level files consumed by UserBuildConfig.
# ---------------------------------------------------------------------------
echo "== image files =="
mkdir -p "$OUT/image"
cp "$SRCDIR/files/sshd_config"     "$OUT/image/sshd_config"
cp "$SRCDIR/files/launch-sshd"     "$OUT/image/launch-sshd"
cp "$SRCDIR/files/services"        "$OUT/image/services"
cp "$SRCDIR/files/authorized_keys" "$OUT/image/authorized_keys"
# The rest of what UserBuildConfig injects (cloud_init_lite launch job, the
# browser remote-desktop session, and its TARGET_SCREEN setup) must be staged
# here too, or the injecting image build fails on the missing source files.
cp "$SRCDIR/files/launch-cloud-init"     "$OUT/image/launch-cloud-init"
cp "$SRCDIR/files/launch-remote-desktop" "$OUT/image/launch-remote-desktop"
cp "$SRCDIR/files/remote-desktop.sh"     "$OUT/image/remote-desktop.sh"
cp "$SRCDIR/files/UserSetupEnvironment"  "$OUT/image/UserSetupEnvironment"
cp "$OUT/pkgroot/boot/system/settings/ssh/ssh_config" "$OUT/image/ssh_config"
ls -l "$OUT/image"

echo
echo "OK. Now install the jam hook and build the image:"
echo "  cp $SRCDIR/UserBuildConfig $GEN/UserBuildConfig"
echo "  cd $GEN && HAIKU_REVISION=hrev59996 jam -q -j\$(nproc) @minimum-mmc"
