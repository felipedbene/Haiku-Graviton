# Runs every reproduction in this directory with the given node, then lists the
# host's localhost entries and the TCP listeners left (none of these leave any).
# Usage: (from compat/test/platform-repro) sh run.sh <node> <scratch-dir>
N=$1; D=$2
$N --version; uname -a
for r in v6only reuseport localhost half-close; do echo "== $r"; timeout 30 $N $r.mjs; echo "rc=$?"; done
echo "== exec-dir"; timeout 30 $N exec-dir.mjs /tmp; echo "rc=$?"
echo "== shebang"; rm -rf $D; timeout 30 $N shebang.mjs $D; echo "rc=$?"; rm -rf $D
echo "== same, from bash"; mkdir -p $D && printf '#!/no/such/interp\necho should-not-run\n' > $D/b.sh && chmod 755 $D/b.sh && bash -c "$D/b.sh; echo bash-saw-rc=\$?"; rm -rf $D
echo "== hosts file"; for h in /etc/hosts /boot/system/settings/network/hosts; do [ -f $h ] && { echo "-- $h"; grep -v '^#' $h | grep -E 'localhost|::1'; }; done
echo "== TCP listeners"; if [ "$(uname)" = Haiku ]; then netstat -n -t -l; else ss -ltn; fi
