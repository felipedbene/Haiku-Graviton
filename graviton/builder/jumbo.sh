#!/bin/bash
exec > /opt/haiku/jumbo-test.txt 2>&1
KEY=/home/ubuntu/.ssh/haiku-ed25519
O="-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=25 -i $KEY"
H=10.42.0.76
echo "===== 1. who are we talking to"
timeout 40 ssh -n $O baron@$H 'uname -a' 2>&1 | tail -2
echo "===== 2. the negotiated MTU (this is the jumbo claim)"
timeout 40 ssh -n $O baron@$H 'ifconfig' 2>&1 | grep -aiE "^/dev|MTU|inet addr" | head -12
echo "===== 3. ENA bring-up line from the guest's syslog"
timeout 40 ssh -n $O baron@$H 'grep -aiE "ena|mtu|descriptor" /boot/system/var/log/syslog 2>/dev/null | tail -14' 2>&1 | tail -16
echo "===== 4. TX chaining: Haiku -> metal with an 8973-byte payload (9015-byte frame)"
timeout 60 ssh -n $O baron@$H 'ping -c 3 -s 8973 10.42.0.149' 2>&1 | tail -6
echo "===== 5. RX chaining: metal -> Haiku, DF set, 8973-byte payload"
ping -c 3 -M do -s 8973 $H 2>&1 | tail -5
echo "===== 6. control: standard-size ping still fine"
ping -c 2 -s 56 $H 2>&1 | tail -3
echo "===== DONE"
