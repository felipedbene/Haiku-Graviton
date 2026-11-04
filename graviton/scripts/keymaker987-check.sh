#!/bin/sh
# keymaker987-check.sh -- issue 987 check, run on a headless Haiku box after
# `pkgman install openssh` (answering the openssh_debeos conflict with solution 1).
#
# Reports the installed keymaker's mode, the package daemon's verdict on it, the
# host keys present, and the keymaker's own exit status when re-run with the keys
# removed. 10.4p1-1 exits 1 (its trailing notify fails headless); 10.4p1-2 ships
# the script mode 0444, so the daemon's system(3) gets 126 and no keys are made.
d=/boot/system/settings/ssh
k=/boot/system/boot/post-install/sshd_keymaker.sh
pkgman search -i -a -D openssh 2>&1 | grep -i ssh
ls -la "$k"
grep -i -E "sshd_keymaker|post-installation" /var/log/syslog | tail -5
ls -la "$d"/ssh_host_*
echo "== re-run with host keys removed"
mkdir -p /tmp/k987 && mv "$d"/ssh_host_* /tmp/k987/
"$k"; echo "keymaker rc=$?"
ls -la "$d"/ssh_host_*
