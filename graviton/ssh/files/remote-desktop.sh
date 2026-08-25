#!/bin/sh
# remote-desktop.sh -- bring up a Haiku desktop on a machine with no framebuffer.
#
# Run by launch_daemon; see /boot/system/settings/launch/remote_desktop.
#
# Nitro instances expose no framebuffer, so app_server cannot create a normal
# Desktop and the ordinary user session never comes up. app_server does however
# contain a complete remote display backend, reached by setting TARGET_SCREEN on
# an application: that makes it join a Desktop keyed on (uid, targetScreen) whose
# HWInterface is a RemoteHWInterface listening on a TCP port. This script starts
# the pieces of a session against that Desktop.
#
# input_server is started here because on a machine with no framebuffer nothing
# else ever starts it. app_server only launches it from Desktop::_Init() when its
# HWInterface produces no usable event stream, and RemoteHWInterface always
# produces one (the client sends the input events), so that branch is never
# taken. Without input_server there is no current keymap, and Terminal's
# TermWindow::_SetupMenu() calls BKeymap::GetModifiedCharacters() without
# checking SetToCurrent()'s status -- so keep starting it.
#
# What this ordering does NOT fix, contrary to what this comment used to claim,
# is the "Deskbar is in `ps` for the whole boot and never draws" failure. That
# had nothing to do with the keymap: Deskbar uses no keymap API at all, and the
# wedge was reproduced on a boot where input_server was up first and completely
# healthy. The cause was app_server's remote send buffer having no reader before
# a client connects; it is fixed in RemoteHWInterface/StreamingRingBuffer. See
# graviton/docs/remote-desktop-send-buffer-wedge.md. Do not re-derive an
# ordering rule from the sleep below.
#
# Access is over an SSH tunnel only. RemoteHWInterface binds 127.0.0.1 (we
# changed it from INADDR_ANY, deliberately), because the remote protocol has no
# authentication: SSH is the authentication.

# TARGET_SCREEN is set for the whole user session by
# ~/config/settings/boot/UserSetupEnvironment, which is what makes
# launch_daemon's own Tracker and Deskbar services able to reach the remote
# Desktop. It is re-exported here only so this script also works when run by
# hand from a shell that has not sourced SetupEnvironment.
PORT=${HAIKU_REMOTE_DESKTOP_PORT:-10900}
export TARGET_SCREEN=$PORT

STAMP=/boot/system/var/remote_desktop.stamp

# launch_daemon restarts a failed job with no back-off, so guard against a hot
# loop the same way sshd_boot.sh does.
now=`date +%s 2>/dev/null || echo 0`
last=`cat "$STAMP" 2>/dev/null || echo 0`
if [ "$now" -gt 0 ] && [ "$last" -gt 0 ] && [ `expr $now - $last` -lt 10 ]; then
	sleep 10
fi
[ "$now" -gt 0 ] && echo "$now" > "$STAMP" 2>/dev/null

# Idempotent: each piece is started only if it is not already there, so a re-run
# after a client reconnects does not pile up duplicates. Tracker and Deskbar are
# single-launch anyway, but input_server is not and a second one is harmful.
running() {
	ps 2>/dev/null | grep -v grep | grep -q "$1"
}

start_once() {
	name=$1
	path=$2
	if [ ! -x "$path" ]; then
		echo "remote_desktop: $name missing at $path" 1>&2
		return
	fi
	if running "$path"; then
		echo "remote_desktop: $name already running" 1>&2
		return
	fi
	"$path" &
	echo "remote_desktop: started $name" 1>&2
}

# A solid-colour desktop, set before Tracker reads it.
#
# This is the one free performance decision available: bitmaps cross the remote
# protocol *raw and uncompressed* through a 16 KB ring buffer, so a wallpaper is
# the single most expensive thing that can be on screen -- far more so than
# anything the CPU does. A flat colour costs one fill. A user who wants a
# wallpaper can set one; the image should not ship one.
BGSETTINGS=/boot/home/config/settings/Backgrounds
if [ ! -e "$BGSETTINGS" ]; then
	mkdir -p /boot/home/config/settings 2>/dev/null
	# An empty settings file means "no image", which leaves the workspace colour
	# showing. Written only if absent, so a user's choice is never overwritten.
	: > "$BGSETTINGS" 2>/dev/null
fi

# input_server, and *only* input_server.
#
# Tracker and Deskbar are deliberately not started here any more. launch_daemon
# already owns them as services in the `desktop` target of data/launch/user, and
# with TARGET_SCREEN now in the session environment its copies work. Starting a
# second pair from here was actively harmful: `running` would see launch_daemon's
# instances, report "already running", and never start the ones that would have
# worked -- so the guard that was meant to prevent duplicates instead made a
# broken session permanent. See docs/desktop-by-default.md.
#
# input_server is not a launch_daemon job. On a framebuffer machine
# Desktop::_LaunchInputServer() (Desktop.cpp) starts it, but only when the
# HWInterface event stream is unusable -- and RemoteHWInterface's stream is
# always usable, so on this image that call never happens and this line is the
# only thing that starts it.
#
# Note this is the *system* launch_daemon context, and Tracker/Deskbar are in the
# *user* one. There is therefore no ordering relationship between them and no way
# to express one: `requires` is per-daemon, and naming a job the daemon does not
# know makes it silently delete the job that named it. Nothing here should be
# read as sequencing the session.
start_once input_server /boot/system/servers/input_server
# Let it finish registering before anything asks it for a keymap. This is
# politeness, not a barrier -- it guarantees nothing.
sleep 2

# Stay alive so launch_daemon sees a running job rather than an instant exit, and
# so input_server is restarted if it ever dies -- which would otherwise take the
# next app that builds a menu down with it.
while true; do
	sleep 30
	running /boot/system/servers/input_server || start_once input_server /boot/system/servers/input_server
done
