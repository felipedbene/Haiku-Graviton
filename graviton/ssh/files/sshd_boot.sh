#!/bin/sh
# sshd_boot.sh -- bring sshd up on a headless Haiku/arm64 (AWS Graviton) box.
#
# Run by launch_daemon; see /boot/system/settings/launch/sshd. This script
# generates host keys on first boot and then exec()s sshd, so the team that
# launch_daemon tracks *is* sshd -- if sshd dies, launch_daemon restarts the
# service (LaunchDaemon.cpp, B_TEAM_DELETED handler, job->IsService()).
#
# Deliberately does NOT depend on: app_server, the desktop, a logged-in user,
# a configured network interface, or any particular NIC. sshd binds the
# wildcard address, so it listens even with no link, and any interface that
# appears later (virtio_net, ena, ...) is served with no further action.

SSHDIR=/boot/system/settings/ssh
VARDIR=/boot/system/var
STAMP=$VARDIR/sshd_boot.stamp
KEYGEN=/boot/system/bin/ssh-keygen
SSHD=/boot/system/bin/sshd

# launch_daemon restarts services with no back-off ("TODO: take restart
# throttle into account"), so guard against a hot restart loop ourselves.
now=`date +%s 2>/dev/null || echo 0`
last=`cat "$STAMP" 2>/dev/null || echo 0`
if [ "$now" -gt 0 ] && [ "$last" -gt 0 ] && [ `expr $now - $last` -lt 5 ]; then
	sleep 5
fi
[ "$now" -gt 0 ] && echo "$now" > "$STAMP" 2>/dev/null

mkdir -p "$SSHDIR" "$VARDIR/empty" 2>/dev/null
chmod 755 "$SSHDIR" 2>/dev/null
chmod 755 "$VARDIR/empty" 2>/dev/null

# Host keys, generated once on first boot.
#
# NOTE: this build of OpenSSH is configured --without-openssl (no OpenSSL is
# packaged for Haiku/arm64), so only Ed25519 and the ML-DSA hybrid key types
# exist. RSA and ECDSA host keys cannot be generated or used.
if [ ! -f "$SSHDIR/ssh_host_ed25519_key" ]; then
	echo "sshd_boot: generating ed25519 host key" 1>&2
	"$KEYGEN" -q -t ed25519 -N "" -f "$SSHDIR/ssh_host_ed25519_key" </dev/null
fi
if [ ! -f "$SSHDIR/ssh_host_mldsa44-ed25519_key" ]; then
	"$KEYGEN" -q -t mldsa44-ed25519 -N "" \
		-f "$SSHDIR/ssh_host_mldsa44-ed25519_key" </dev/null 2>/dev/null || true
fi

# The image build creates files mode 0664; sshd's StrictModes would then reject
# authorized_keys. sshd_config sets "StrictModes no" as well, but fix the modes
# anyway so that turning StrictModes back on keeps working.
for home in /boot/home; do
	d=$home/config/settings/ssh
	if [ -d "$d" ]; then
		chmod 700 "$d" 2>/dev/null
		[ -f "$d/authorized_keys" ] && chmod 600 "$d/authorized_keys" 2>/dev/null
	fi
done

echo "sshd_boot: starting sshd" 1>&2
# -D: do not detach, so the team launch_daemon tracks is sshd itself and
# launch_daemon's restart-on-death applies. No -e: let sshd log through
# syslog(), which Haiku's syslog_daemon persists to /boot/system/var/log/syslog.
# On a box with no display that file is the only place "Server listening on
# 0.0.0.0 port 22" and any auth failure can be read after the fact.
exec "$SSHD" -D
