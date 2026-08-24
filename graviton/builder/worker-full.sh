#!/bin/bash
# worker-full.sh <sshport> <port> [port...]
# Like worker.sh, but keeps the ENTIRE haikuporter log per port instead of
# `tail -18`.  The libtool blocker hid for hours behind that tail: the lines that
# explained it ("./bootstrap: git: command not found") were thrown away.  Full log
# is written in the guest, then scp'd to the metal so it can be grepped offline.
PORT=$1; shift
KEY=/home/ubuntu/.ssh/haiku-ed25519
S="-n -p $PORT -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=20 -o ServerAliveInterval=30 -i $KEY"
SCP="-P $PORT -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=20 -i $KEY"
LOG=/opt/haiku/worker-$PORT.log
rm -f /opt/haiku/worker-$PORT.done
exec >> $LOG 2>&1
echo "=== WORKER-FULL $PORT START $(date) : $* ==="
ssh $S baron@127.0.0.1 'mkdir -p /boot/system/settings/fonts'
for p in "$@"; do
  tag=$(echo "$p" | tr -c 'A-Za-z0-9._-' '_' | sed 's/_*$//')
  echo "########## $p  $(date)"
  ssh $S baron@127.0.0.1 "haikuporter -y $HP_FLAGS $p > /boot/home/hp-$tag.log 2>&1; echo HAIKUPORTER_RC=\$?"
  scp $SCP "baron@127.0.0.1:/boot/home/hp-$tag.log" /opt/haiku/logs/hp-$PORT-$tag.log
  echo "--- last 40 of full log (full copy at /opt/haiku/logs/hp-$PORT-$tag.log) ---"
  tail -40 /opt/haiku/logs/hp-$PORT-$tag.log
  echo "--- packages in guest ---"
  ssh $S baron@127.0.0.1 "ls /boot/home/haikuports/packages/"
  # HARVEST-EXCLUDE-haiku-hpkg: haiku*.hpkg are build *inputs* (the chroot's own
  # packagefs), not outputs. Harvesting them made a stale haiku.hpkg -- whose
  # libroot.so still had the pre-fix system_time() reading CNTPCT_EL0 -- copy
  # itself back into the guest seed after every port, so it survived every image
  # rebuild and kept the ~18 h chroot clock step alive. Stage, drop them, then move.
  _harvest=$(mktemp -d)
  scp $SCP "baron@127.0.0.1:/boot/home/haikuports/packages/*.hpkg" "$_harvest"/ >/dev/null 2>&1
  rm -f "$_harvest"/haiku*.hpkg
  if ls "$_harvest"/*.hpkg >/dev/null 2>&1; then mv -f "$_harvest"/*.hpkg /opt/haiku/hpkg-out/arm64/; fi
  rm -rf "$_harvest"
  aws s3 sync /opt/haiku/hpkg-out/arm64/ s3://haiku-graviton-668984504585-us-west-2/hpkg/arm64/ \
      --exclude "*" --include "*.hpkg" >/dev/null 2>&1
  echo "########## $p done $(date)"
done
echo "=== WORKER-FULL $PORT DONE $(date) ==="
touch /opt/haiku/worker-$PORT.done
