#!/bin/sh
# partition-grow.sh -- first-boot root-partition grow, deferred off the boot
# critical path.
#
# Run by launch_daemon; see /boot/system/settings/launch/partition_grow. It waits
# for the box's control plane to come up and then runs partition_grow, which
# grows the root GPT partition entry to fill a larger-than-baked EBS volume.
#
# WHY DEFER (issue #254): partition_grow does raw GPT surgery on the whole-disk
# device (open O_RDWR, pwrite the relocated backup + primary headers, fsync, and
# close). Writing and closing the raw boot device makes the kernel
# disk_device_manager rescan the boot device's partition table (see
# nvme_disk.cpp, B_NVME_RESCAN_CAPACITY, and KDiskDeviceManager). When that
# rescan lands in the first-boot service storm -- which is exactly when a
# larger-than-baked root makes partition_grow do real work rather than no-op --
# it races the late-boot, network-gated launch jobs (sshd/EICE and the SSM
# agent). On a big root the observed result was that neither ever came up: the
# box booted healthy but had no control channel. A default 20 GiB root makes
# partition_grow a clean idempotent no-op, so it never disturbed the storm and
# the box was reachable in ~1 min.
#
# WHY DEFERRING IS SAFE FOR THE GROW: partition_grow only rewrites the *partition*
# entry. The filesystem grow is owned by the BFS mount-time engine and happens on
# the NEXT mount (next boot). Convergence is two boots by construction no matter
# when in this boot the partition grow runs, so running it a little later --
# after the control plane is established -- changes nothing except that the
# disk-device-manager rescan no longer lands in the boot storm. partition_grow is
# idempotent, so a reboot before it runs simply re-runs it on the next boot, and
# its backup-first write ordering keeps every crash window recoverable.
#
# This runs in its own launch_daemon "job" and so delays nothing else -- the same
# posture as cloud_init_lite, which waits for DHCP in its own job.

GROW=/boot/system/bin/partition_grow

# Settle delay, not a correctness barrier. Let the network-gated control plane
# (sshd + the SSM agent) win the first-boot race before the disruptive GPT
# rewrite + device rescan. An undisturbed default-root box reaches SSM Online in
# ~1 min and sshd listens within seconds, so wait comfortably past that; once
# those services are up and listening, a brief disk-device-manager lock hold no
# longer keeps them from starting. The grow is consumed on the next boot
# regardless of when in this boot it runs, so a generous delay costs nothing.
SETTLE=150
sleep "$SETTLE" 2>/dev/null

echo "partition-grow: settle delay elapsed, running partition_grow" 1>&2
exec "$GROW"
