#!/bin/bash
# gworker.sh <sshport> <port>...  -- build ports sequentially in one guest.
# Differs from worker.sh in two ways that both cost real debugging time earlier:
#  - the FULL haikuporter log is kept (worker.sh piped through `tail -18`, which
#    threw away the ./bootstrap "git: command not found" lines that explained the
#    libtool failure), and
#  - success is asserted by the hpkg existing, not by an exit code.
PORT=$1; shift
KEY=/home/ubuntu/.ssh/haiku-ed25519
S="-n -p $PORT -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=20 -o ServerAliveInterval=30 -i $KEY"
SCP="-P $PORT -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=20 -i $KEY"
LOG=/opt/haiku/gworker-$PORT.log
exec >> $LOG 2>&1
echo "=== GWORKER $PORT START $(date -u) : $* ==="
for p in "$@"; do
  echo "########## $p start $(date -u)"
  ssh $S baron@127.0.0.1 "haikuporter -y $p > /boot/home/g-$p.log 2>&1; echo RC=\$?"
  scp $SCP "baron@127.0.0.1:/boot/home/g-$p.log" /opt/haiku/g-$p-$PORT.log 2>/dev/null
  echo "---- hpkg check ----"
  ssh $S baron@127.0.0.1 "ls /boot/home/haikuports/packages/ | grep -E \"^$p-[0-9]\" || echo NO_PKG_$p"
  echo "---- last 20 log lines ----"
  tail -20 /opt/haiku/g-$p-$PORT.log
  # harvest and make durable after EVERY port: a finished diffutils was lost once
  # Harvest via a staging dir and DO NOT bring back haiku*.hpkg. Those are the
  # chroot's build *inputs*, not outputs: haikuporter activates its own packagefs
  # from /boot/home/haikuports/packages/haiku.hpkg, so sweeping them into the
  # guest seed directory made a stale copy immortal. One dated 20 Aug carried the
  # pre-fix system_time() (CNTPCT_EL0 plus an overflowing multiply) and reinstalled
  # itself into every new guest for days, giving every chroot an 18h-skewed clock
  # that silently reverted recipe edits. Rebuilding the image never fixed it,
  # because the loop re-imported the artifact afterwards.
  _stage=$(mktemp -d)
  scp $SCP "baron@127.0.0.1:/boot/home/haikuports/packages/*.hpkg" "$_stage/" >/dev/null 2>&1
  rm -f "$_stage"/haiku.hpkg "$_stage"/haiku_*.hpkg "$_stage"/haiku-*.hpkg
  cp -p "$_stage"/*.hpkg /opt/haiku/hpkg-out/arm64/ 2>/dev/null
  rm -rf "$_stage"
  aws s3 sync /opt/haiku/hpkg-out/arm64/ s3://haiku-graviton-668984504585-us-west-2/hpkg/arm64/ \
      --exclude "*" --include "*.hpkg" >/dev/null 2>&1
  echo "########## $p done $(date -u)"
done
echo "=== GWORKER $PORT DONE $(date -u) ==="
touch /opt/haiku/gworker-$PORT.done
