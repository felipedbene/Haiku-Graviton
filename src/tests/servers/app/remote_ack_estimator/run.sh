#!/bin/sh
# Build and run the RemoteAckEstimator unit test.
#
# The estimator is compiled off-target with a plain host compiler: it takes no
# locks, allocates nothing and touches no Haiku API beyond a handful of integer
# typedefs, which is deliberate -- the two properties it exists for (an RTT
# with the client's held time removed, and a backpressure signal an idle screen
# cannot raise) are both invisible on a healthy LAN client, so testing them by
# booting an image tests nothing. The same script runs natively on the target.
set -e

here=$(cd "$(dirname "$0")" && pwd)
remote=$here/../../../../servers/app/drawing/interface/remote
out=${OUT:-$here/build}
mkdir -p "$out"

${CXX:-g++} -std=c++11 -Wall -O1 -g \
	-DREMOTE_ACK_ESTIMATOR_TEST \
	-I"$here" -I"$remote" \
	"$here/RemoteAckEstimatorTest.cpp" "$remote/RemoteAckEstimator.cpp" \
	-o "$out/remote_ack_estimator_test"

"$out/remote_ack_estimator_test"
