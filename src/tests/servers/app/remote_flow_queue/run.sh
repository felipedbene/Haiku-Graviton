#!/bin/sh
# Build and run the RemoteFlowQueue unit test.
#
# The policy is compiled off-target with a plain host compiler: it takes no
# locks and touches no Haiku API beyond a handful of integer typedefs, which is
# deliberate -- flow control that can only be exercised by booting an image is
# flow control nobody tests. The same script runs natively on the target.
set -e

here=$(cd "$(dirname "$0")" && pwd)
remote=$here/../../../../servers/app/drawing/interface/remote
out=${OUT:-$here/build}
mkdir -p "$out"

${CXX:-g++} -std=c++11 -Wall -O1 -g \
	-DREMOTE_FLOW_QUEUE_TEST \
	-I"$here" -I"$remote" \
	"$here/RemoteFlowQueueTest.cpp" "$remote/RemoteFlowQueue.cpp" \
	-o "$out/remote_flow_queue_test"

"$out/remote_flow_queue_test"
