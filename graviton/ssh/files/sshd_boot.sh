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
#
# The stamp is read defensively: it is a runtime-written file, so after an
# unclean power-off it can come back as the right number of NUL bytes rather
# than a number (see the host-key comment below). Strip it to digits and treat
# anything else as "no stamp", so a corrupt stamp costs nothing instead of
# making `expr` fail. The clock can also step, so a stamp from the future is
# discarded rather than trusted into a negative difference.
now=`date +%s 2>/dev/null | tr -dc 0-9`
last=`cat "$STAMP" 2>/dev/null | tr -dc 0-9`
[ -n "$now" ] || now=0
[ -n "$last" ] || last=0
if [ "$now" -gt 0 ] && [ "$last" -gt 0 ] && [ "$last" -le "$now" ] \
		&& [ `expr $now - $last` -lt 5 ]; then
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
#
# The test is "does the key parse", not "does the file exist". Existence is not
# enough on a box that can lose power without warning: BFS journals the inode
# but not the file's data, so a host key generated on one boot and not flushed
# before the power went away comes back as a file of exactly the right size,
# mode and mtime whose blocks are still zero. sshd then reports
#   Unable to load host key "...": invalid format
# and, because sshd_config names exactly one HostKey, exits with no host keys at
# all -- the machine answers ping and refuses port 22 forever, and no amount of
# restarting by launch_daemon helps, because the broken file still "exists".
# ssh-keygen -y costs one exec and turns that into a self-healing boot.
#
# An EC2 stop is always an unclean power-off for this guest (the ACPI
# power-button event Nitro sends has no inbound path here yet), and terminate
# and a genuine crash never give a clean shutdown at all, so this has to be
# tolerated rather than merely avoided.
keygen_if_bad() {
	type=$1
	file=$SSHDIR/$2
	if [ -f "$file" ] && "$KEYGEN" -y -f "$file" >/dev/null 2>&1; then
		return 0
	fi
	if [ -e "$file" ]; then
		echo "sshd_boot: host key $file is unreadable, regenerating" 1>&2
		rm -f "$file" "$file.pub"
	fi
	echo "sshd_boot: generating $type host key" 1>&2
	"$KEYGEN" -q -t "$type" -N "" -f "$file" </dev/null
}

keygen_if_bad ed25519 ssh_host_ed25519_key
keygen_if_bad mldsa44-ed25519 ssh_host_mldsa44-ed25519_key 2>/dev/null || true

# Get the keys onto the platter before anything can cut the power. Without this
# the first boot leaves them in the page cache, and a stop/start immediately
# afterwards comes back to the zero-filled files described above -- the
# regeneration above would recover it, but at the cost of a changed host key and
# a host-key-mismatch warning on every client that had already connected.
# /bin/sync is a coreutils binary; it may not be present in a smaller profile,
# so its absence must not be fatal.
sync 2>/dev/null || true

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
