#!/bin/bash
exec >> /opt/haiku/opt-boottest.log 2>&1
set -x
mkdir -p /opt/haiku/logs
cp /opt/haiku/haiku-opt/generated.arm64/haiku-mmc.image /opt/haiku/opt-test.img
md5sum /opt/haiku/opt-test.img
echo "=== other qemu still running (MUST stay) ==="
pgrep -af '[q]emu-system' | sed 's/-object.*//' | cut -c1-120
echo "=== launching my guest on :2223 ==="
setsid sudo nohup /opt/haiku/opt-boot.sh >/opt/haiku/logs/opt-qemu.log 2>&1 </dev/null &
K=/home/ubuntu/.ssh/haiku-ed25519
O="-i $K -p 2223 -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=10 -o LogLevel=ERROR -o BatchMode=yes"
T0=$SECONDS
OK=0
for i in $(seq 1 48); do
  sleep 5
  if ssh $O baron@127.0.0.1 true 2>/dev/null; then
    echo "SSHD_UP after $((SECONDS-T0))s"; OK=1; break
  fi
done
if [ $OK = 1 ]; then
  echo "=== in-guest verification ==="
  ssh $O baron@127.0.0.1 'uname -a; id; echo "--- cpuinfo ---"; sysinfo -cpu 2>/dev/null | head -20; echo "--- uptime ---"; uptime; echo "--- syslog tail ---"; tail -20 /boot/system/var/log/syslog' 2>&1
  echo "=== stress: fork/atomic-heavy load (exercises LSE paths) ==="
  ssh $O baron@127.0.0.1 'for i in $(seq 1 200); do /bin/true & done; wait; echo FORKLOOP_OK; ls -lR /boot/system/lib > /dev/null && echo FSWALK_OK' 2>&1
  echo "BOOT_TEST=PASS"
else
  echo "BOOT_TEST=FAIL (no sshd in 240s)"
fi
echo "=== serial log: first 60 lines ==="
tr -d '\000' < /opt/haiku/logs/opt-boot.log | head -60
echo "=== serial log: last 40 lines ==="
tr -d '\000' < /opt/haiku/logs/opt-boot.log | tail -40
echo "=== KDEBUG / panic check ==="
tr -d '\000' < /opt/haiku/logs/opt-boot.log | grep -ciE 'kdebug|PANIC|Unhandled exception|data abort|undefined instruction' 
tr -d '\000' < /opt/haiku/logs/opt-boot.log | grep -iE 'kdebug|PANIC|Unhandled|abort|undefined instruction' | head -10
echo "=== shutting down MY guest only (pid via monitor) ==="
echo quit | sudo socat - unix-connect:/tmp/qmon-opt 2>/dev/null || sudo pkill -f 'qmon-opt'
sleep 3
pgrep -af '[q]emu-system' | sed 's/-object.*//' | cut -c1-120
echo "BOOTTEST_DONE"
touch /opt/haiku/opt-boot.done
