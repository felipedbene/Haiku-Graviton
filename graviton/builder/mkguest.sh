#!/bin/bash
# mkguest.sh <N> -- clone a builder guest from the current image and seed it.
# Guest N lives in /opt/haiku/runN and forwards ssh on port 222(N-1)+... see PORT.
set -x
N=$1
[ -z "$N" ] && exit 1
D=/opt/haiku/run$N
PORT=$((2220 + N))
IMG=/opt/haiku/run8/haiku/generated.arm64/haiku-mmc.image
KEY=/home/ubuntu/.ssh/haiku-ed25519
exec >> /opt/haiku/mkguest-$N.log 2>&1
echo "=== MKGUEST $N (port $PORT) START $(date) ==="
mkdir -p $D/logs
cp -f $IMG $D/builder-run$N.img
cat > $D/boot.sh <<BOOT
#!/bin/bash
exec qemu-system-aarch64 -M virt,gic-version=3,its=on,accel=kvm -cpu host \\
  -m 16384 -smp 12 -bios /usr/share/AAVMF/AAVMF_CODE.fd \\
  -display none -serial file:$D/logs/boot.log -monitor unix:/tmp/qmon-run$N,server,nowait \\
  -drive if=none,id=hd,file=$D/builder-run$N.img,format=raw \\
  -device qemu-xhci,id=xhci -device usb-storage,bus=xhci.0,drive=hd \\
  -netdev user,id=n0,hostfwd=tcp::$PORT-:22 \\
  -device virtio-net-pci,netdev=n0,vectors=0
BOOT
chmod +x $D/boot.sh
rm -f $D/logs/boot.log /tmp/qmon-run$N
setsid nohup $D/boot.sh >$D/logs/qemu.out 2>&1 < /dev/null &
sleep 100   # boot + first-boot package processing

SSHN="-n -p $PORT -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=20 -o ServerAliveInterval=30 -i $KEY"
SSHIN="-p $PORT -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=20 -i $KEY"
SCP="-P $PORT -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=20 -i $KEY"

for i in 1 2 3 4 5 6; do
  ssh $SSHN baron@127.0.0.1 true 2>/dev/null && break
  echo "waiting for sshd (try $i)"; sleep 25
done
ssh $SSHN baron@127.0.0.1 'uname -a; date -u'

# same seeding the main guest needed
ssh $SSHN baron@127.0.0.1 'mkdir -p /boot/system/settings/fonts /boot/home/config/settings'
scp $SCP /opt/haiku/hp.conf baron@127.0.0.1:/boot/home/config/settings/haikuports.conf
scp $SCP /opt/haiku/extract2.py baron@127.0.0.1:/boot/home/extract2.py
gzip -dc /opt/haiku/haikuports-recipes.tar.gz | ssh $SSHIN baron@127.0.0.1 'python3 /boot/home/extract2.py /boot/home/hp-extract 2>&1 | tail -2'
ssh $SSHN baron@127.0.0.1 'cp -r /boot/home/hp-extract/* /boot/home/haikuports/ 2>&1 | tail -1; ls -d /boot/home/haikuports/*/ | wc -l'
scp $SCP /opt/haiku/hpkg-out/arm64/*.hpkg baron@127.0.0.1:/boot/home/haikuports/packages/ 2>&1 | tail -1
scp $SCP /opt/haiku/hpkg-out/arm64/stage1/*.hpkg baron@127.0.0.1:/boot/home/haikuports/packages/ 2>&1 | tail -1
ssh $SSHN baron@127.0.0.1 'ls /boot/home/haikuports/packages/*.hpkg | wc -l'
echo "=== MKGUEST $N READY $(date) ==="
touch /opt/haiku/mkguest-$N.done
