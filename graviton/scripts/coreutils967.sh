# coreutils matrix for #967 on mount point $1 (run as root): chmod, install -m, mkdir -m,
# chown/chgrp, cp -p, tar xp. Prints each rc and what stat() shows afterwards. Contents are
# compared by sha256 (Haiku has no cmp); tar xp runs only where tar exists.
M=$1; T=$M/cu967; same() { [ "$(sha256sum < "$1")" = "$(sha256sum < "$2")" ] && echo "  content ok" || echo "  content DIFFERS"; }
 rm -rf $T; mkdir $T || exit 1
st() { stat -c "  -> %n mode=%a uid=%u gid=%g size=%s" "$@"; }
echo x > $T/f; st $T/f
chmod 0600 $T/f; echo "chmod 0600 rc=$?"; st $T/f
chmod 0666 $T/f; echo "chmod 0666 rc=$?"; st $T/f
chmod u+x $T/f; echo "chmod u+x rc=$?"; st $T/f
echo "install-src" > /tmp/src967; chmod 0644 /tmp/src967
install -m 0644 /tmp/src967 $T/inst644; echo "install -m 0644 rc=$?"; st $T/inst644; same /tmp/src967 $T/inst644
install -m 0755 /tmp/src967 $T/inst755; echo "install -m 0755 rc=$?"; st $T/inst755
install -d -m 0700 $T/instdir; echo "install -d -m 0700 rc=$?"; st $T/instdir
mkdir -m 0700 $T/d700; echo "mkdir -m 0700 rc=$?"; st $T/d700
chown 1002 $T/f; echo "chown 1002 rc=$?"; st $T/f
chown 1002:1002 $T/f; echo "chown 1002:1002 rc=$?"; st $T/f
chgrp 1002 $T/f; echo "chgrp 1002 rc=$?"; st $T/f
chown 0:0 $T/f; echo "chown 0:0 rc=$?"; st $T/f
cp -p /tmp/src967 $T/cpp; echo "cp -p (0644 source) rc=$?"; st $T/cpp; same /tmp/src967 $T/cpp
command -v tar > /dev/null || { echo "tar: not installed, skipped"; exit 0; }
rm -rf /tmp/tar967 && mkdir -p /tmp/tar967/t && echo tarred > /tmp/tar967/t/a && chmod 0640 /tmp/tar967/t/a && chown 1002:1002 /tmp/tar967/t/a
(cd /tmp/tar967 && tar cf ../t967.tar t) && (cd $T && tar xpf /tmp/t967.tar); echo "tar xpf (0640, 1002:1002) rc=$?"; st $T/t/a; same /tmp/tar967/t/a $T/t/a
