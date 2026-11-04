#!/bin/sh
# keymaker1043-check.sh <openssh hpkg> -- issue 1043 check on a fresh headless
# canonical box (drive it over SSM: installing the package removes
# openssh_debeos and its sshd launch path).
#
# Installs the package (answering the openssh_debeos conflict with solution 1),
# then reports the ML-DSA host key files, their fingerprints, and the hostkey
# list of `sshd -T` under the box's config and under the package's default
# config, and whether every listed key exists. Then re-runs the keymaker twice
# and checks nothing changes. Before the fix the box ends with two different
# ML-DSA keys: the image's ssh_host_mldsa44-ed25519_key and the underscore
# one `ssh-keygen -A` adds.
# With --state-only, only the report and the idempotence check run (no install).
hpkg="${1:?usage: $0 <openssh hpkg> | --state-only}"
d=/boot/system/settings/ssh
k=/boot/system/boot/post-install/sshd_keymaker.sh
pkgdef=/boot/system/data/openssh/sshd_config.default

state() {
	echo "-- files"; ls -la "$d"/ssh_host_*mldsa* 2>&1
	echo "-- distinct ML-DSA keys"
	for f in "$d"/ssh_host_*mldsa*_key.pub; do ssh-keygen -l -f "$f"; done 2>&1 \
		| awk '{print $2}' | sort -u | wc -l
	for cfg in "$d/sshd_config" "$pkgdef"; do
		echo "-- sshd -T hostkeys ($cfg)"
		/boot/system/bin/sshd -T -f "$cfg" 2>&1 | grep -i '^hostkey ' | while read -r _ f; do
			[ -f "$f" ] && echo "present $f" || echo "MISSING $f"
		done
	done
}

catattr SYS:PACKAGE_FILE /boot/system/bin/sshd
if [ "$hpkg" != --state-only ]; then
	echo "== before install"; ls -la "$d"/ssh_host_* 2>&1
	printf '1\ny\n' | pkgman install "$hpkg" 2>&1 | tail -6
	grep -i -E "sshd_keymaker|post-installation" /var/log/syslog | tail -3
fi
pkgman search -i -a -D openssh 2>&1 | grep -i ssh
echo "== after install"; state
ls -la "$d"/ssh_host_* | md5sum > /tmp/k1043.before
for f in "$d"/ssh_host_*; do cat "$f"; done | md5sum >> /tmp/k1043.before
for n in 1 2; do
	"$k"; echo "== re-run $n: keymaker rc=$?"
done
ls -la "$d"/ssh_host_* | md5sum > /tmp/k1043.after
for f in "$d"/ssh_host_*; do cat "$f"; done | md5sum >> /tmp/k1043.after
[ "$(cat /tmp/k1043.before)" = "$(cat /tmp/k1043.after)" ] && echo "re-runs idempotent: yes" || echo "re-runs idempotent: NO"
state
