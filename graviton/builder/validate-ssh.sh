#!/bin/bash
K=/home/ubuntu/.ssh/haiku-ed25519
O="-i $K -p 2222 -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=10 -o LogLevel=ERROR"
kill_qemu() {
	for p in $(pgrep -f "[q]emu-system-aarch64"); do sudo kill "$p" 2>/dev/null; done
	for i in $(seq 1 30); do pgrep -f "[q]emu-system-aarch64" >/dev/null || return 0; sleep 1; done
	for p in $(pgrep -f "[q]emu-system-aarch64"); do sudo kill -9 "$p" 2>/dev/null; done; sleep 2
}
boot() { sudo env LOG="$1" PCAP="$2" /opt/haiku/boot-ssh-test.sh; }
wait_ssh() {
	local t0=$SECONDS i
	for i in $(seq 1 48); do
		sleep 5
		if ssh $O -o BatchMode=yes baron@127.0.0.1 true 2>/dev/null; then
			echo "[$1] sshd answered $((SECONDS-t0))s after qemu start"; return 0
		fi
	done
	echo "[$1] TIMED OUT"; return 1
}
kill_qemu
timeout 3 bash -c 'exec 3<>/dev/tcp/127.0.0.1/2222' 2>/dev/null \
	&& echo "WARNING: 2222 still open" || echo "pre-flight: port 2222 closed"
cp /opt/haiku/haiku/generated.arm64/haiku-mmc.image /opt/haiku/ssh-test.img
echo "image under test: $(md5sum /opt/haiku/ssh-test.img)"

echo; echo "########## COLD BOOT 1 -- fresh image, no host keys, NO framebuffer ##########"
nohup bash -c 'boot() { sudo env LOG=/opt/haiku/logs/b1.log PCAP=/opt/haiku/logs/b1.pcap /opt/haiku/boot-ssh-test.sh; }; boot' >/opt/haiku/logs/q1.log 2>&1 &
wait_ssh boot1 || exit 1
echo "--- ssh -v handshake (pubkey auth proof) ---"
ssh -v $O baron@127.0.0.1 true 2>&1 | grep -E "Server host key|Authentication succeeded|Remote protocol|Offering public key|kex: algorithm|cipher"
echo "--- interactive command output ---"
ssh $O baron@127.0.0.1 'uname -a; id; netstat -ltn; ssh-keygen -lf /boot/system/settings/ssh/ssh_host_ed25519_key.pub; grep -a sshd /boot/system/var/log/syslog | tail -5' 2>&1
echo "--- headless proof: app_server failure count on the serial console ---"
tr -d '\000' < /opt/haiku/logs/b1.log | grep -ac "Failed to initialize virtual screen configuration"
echo "--- restart-on-death ---"
OLD=$(ssh $O baron@127.0.0.1 'cat /boot/system/var/sshd.pid' 2>/dev/null)
ssh $O baron@127.0.0.1 "kill -9 $OLD" 2>&1
sleep 15
NEW=$(ssh $O baron@127.0.0.1 'cat /boot/system/var/sshd.pid' 2>/dev/null)
echo "sshd pid before kill: $OLD   after: $NEW"
[ -n "$NEW" ] && [ "$NEW" != "$OLD" ] && echo "restart-on-death: PASS" || echo "restart-on-death: CHECK"

echo; echo "########## COLD BOOT 2 -- same disk, second consecutive boot ##########"
kill_qemu
nohup bash -c 'sudo env LOG=/opt/haiku/logs/b2.log PCAP=/opt/haiku/logs/b2.pcap /opt/haiku/boot-ssh-test.sh' >/opt/haiku/logs/q2.log 2>&1 &
wait_ssh boot2 || exit 1
ssh $O baron@127.0.0.1 'uname -a; ssh-keygen -lf /boot/system/settings/ssh/ssh_host_ed25519_key.pub; netstat -ltn; echo "--- boot 2 syslog sshd lines ---"; grep -a sshd /boot/system/var/log/syslog | tail -4' 2>&1
echo "--- host key regenerated on boot 2? (0 = no, correct) ---"
tr -d '\000' < /opt/haiku/logs/b2.log | grep -ac "generating ed25519 host key"
echo "--- serial console still alive on boot 2 (line count) ---"
tr -d '\000' < /opt/haiku/logs/b2.log | wc -l
echo DONE
