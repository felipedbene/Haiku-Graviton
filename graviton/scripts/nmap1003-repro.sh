#!/bin/sh
# nmap1003-repro.sh <same-subnet-peer> <routed-peer> -- issue 1003 reproduction.
#
# Run on a Haiku box with nmap installed (and, for comparison, on Linux with the
# same arguments). Before the libdnet route backend, every scan that needs the
# routing table -- anything but an unprivileged connect scan -Pn -sT -- aborts with
#   route_dst_generic: Failed to obtain system routes: getsysroutes_dnet: route_open() failed
# and --iflist prints "ROUTES: NONE FOUND(!)".
near="${1:?usage: $0 <same-subnet-peer> <routed-peer>}"
far="${2:?usage: $0 <same-subnet-peer> <routed-peer>}"
run() { echo "== nmap $*"; nmap "$@" 2>&1; echo "rc=$?"; }
run --iflist
run -Pn -sT -p 22,23,8080 "$far"
run -Pn -sT -p 22,23 "$near"
run -sn "$far" "$near"
run -Pn -p 22,23 "$near"
run -Pn -p 22,23,8080 "$far"
run -Pn -p 22 127.0.0.1
