#!/bin/bash
exec >> /opt/haiku/libtool-fix.log 2>&1
set -x
KEY=/home/ubuntu/.ssh/haiku-ed25519
O="-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=20 -i $KEY"
scp -P 2227 $O /tmp/ltfix.py baron@127.0.0.1:/boot/home/ltfix.py
R=$(ssh -n -p 2227 $O baron@127.0.0.1 'ls /boot/home/haikuports/input-source-packages/develop/sources/libtool-*/libtool-*.recipe 2>/dev/null | head -1')
echo "recipe=$R"
ssh -n -p 2227 $O baron@127.0.0.1 "python3 /boot/home/ltfix.py '$R'"
# clock is fixed now, but pin the mtime anyway: haikuporter reverts a recipe that
# is not newer than its source package.
ssh -n -p 2227 $O baron@127.0.0.1 "H=\$(ls /boot/home/haikuports/input-source-packages/libtool_source_rigged-*.hpkg | head -1); touch -r \"\$H\" -d '+1 day' '$R'; ls -la '$R'"
ssh -n -p 2227 $O baron@127.0.0.1 'haikuporter -y libtool 2>&1' | tail -14
ssh -n -p 2227 $O baron@127.0.0.1 'ls /boot/home/haikuports/packages/ | grep -i libtool || echo NO_LIBTOOL'
echo "=== DONE $(date) ==="
touch /opt/haiku/libtool-fix.done
