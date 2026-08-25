#!/bin/bash
# fleet-worker.sh <sshport> <recipe>... -- build recipes sequentially in one guest.
#
# Descends from gworker.sh/cwork.sh, which are the proven path for this fork: the
# QEMU arm64 Haiku guests on the metal build natively with haikuporter, and the
# host harvests the resulting hpkgs. Three things are kept from them deliberately:
#   - the FULL haikuporter log is retained (an earlier tail -18 threw away the
#     "git: command not found" lines that explained a libtool failure),
#   - success is asserted by the hpkg EXISTING, never by an exit code, and
#   - harvest never brings back haiku*.hpkg: those are the chroot's build INPUTS,
#     and sweeping them into the seed directory once made a stale pre-fix
#     libroot immortal across every newly cloned guest.
#
# What is new here is a per-recipe TIMEOUT. The gnulib/autoconf conftest-hang
# class wedges a guest indefinitely -- an unkillable conftest takes the whole
# mount with it -- so a hung recipe must be bounded and the guest reclaimed
# rather than silently occupying a slave for the rest of the run.
set -u
PORT=${1:?usage: fleet-worker.sh <sshport> <recipe>...}
shift
FLEET=/opt/haiku/fleet
KEY=/home/ubuntu/.ssh/haiku-ed25519
OPTS="-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=20 -o ServerAliveInterval=30"
S="-n -p $PORT $OPTS -i $KEY"
SCP="-P $PORT $OPTS -i $KEY"
G=baron@127.0.0.1
GP=/boot/home/haikuports/packages
OUT=/opt/haiku/hpkg-out/arm64
# Shared package repository, resolved from the caller's own credentials so that
# no AWS account id is written down in a public tree. HAIKU_GRAVITON_BUCKET
# overrides it. Same convention as graviton/scripts/ssm-run, which explains why
# the lookup is validated rather than interpolated blind: an empty account id
# composes "haiku-graviton--us-west-2", a valid bucket name that is nobody's.
BUCKET="${HAIKU_GRAVITON_BUCKET:-}"
if [ -z "$BUCKET" ]; then
	ACCT=$(aws sts get-caller-identity --query Account --output text) || ACCT=""
	case "$ACCT" in ""|*[!0-9]*)
		echo "$(basename "$0"): cannot resolve the AWS account id; refresh credentials or set HAIKU_GRAVITON_BUCKET" >&2
		exit 1 ;;
	esac
	BUCKET="haiku-graviton-$ACCT-${AWS_REGION:-us-west-2}"
fi
S3=s3://$BUCKET/hpkg/arm64/
LOG=$FLEET/worker-$PORT.log
exec >> "$LOG" 2>&1

# Per-recipe wall clock. The hazard list is the autoconf/gnulib configure-hang
# class named in the brief; meson/cmake ports get the default; llvm12 is simply
# enormous and needs a much longer rope than anything else in the closure.
timeout_for() {
	case "$1" in
		fontconfig|openssh|lame|libogg|libvorbis|speex|wpa_supplicant) echo 5400 ;;   # 90m
		llvm12)                                                       echo 36000 ;;  # 10h
		ffmpeg6|mesa|harfbuzz|glib2|freetype)                          echo 21600 ;;  # 6h
		*)                                                            echo 14400 ;;  # 4h
	esac
}

echo "=== FLEET WORKER $PORT START $(date -u +%FT%TZ) : $* ==="

for p in "$@"; do
	TMO=$(timeout_for "$p")
	echo "########## $p start $(date -u +%FT%TZ) (timeout ${TMO}s)"
	touch "$FLEET/heartbeat"

	# -y: assume yes.  -G: use patch(1) rather than git, required for every port
	# whose sources are downloaded in these guests.
	#
	# --all-dependencies is load-bearing and its absence cost a whole dispatch
	# round. Without it haikuporter resolves build-requires ONLY against packages
	# that already exist in /boot/home/haikuports/packages and /boot/system/packages
	# -- it will not build a missing one from the recipe tree. The first run failed
	# in seconds with "build-requires devel:libmp3lame ... could not be resolved"
	# (ffmpeg6) and "devel:libopenjp2" (libicns), which read like broken recipes but
	# were just an unbuilt DAG. hpx has always passed this flag; gworker.sh omitted
	# it because it drove explicit hand-ordered batches instead.
	#
	# The timeout is applied on BOTH sides: `timeout` inside the guest bounds the
	# build itself, and a slightly longer local timeout bounds the ssh channel so
	# a wedged guest cannot hang the dispatcher too.
	timeout $((TMO + 300)) ssh $S $G \
		"if command -v timeout > /dev/null 2>&1; then \
		     timeout ${TMO} haikuporter -y -G --all-dependencies $p > /boot/home/f-$p.log 2>&1; \
		 else \
		     haikuporter -y -G --all-dependencies $p > /boot/home/f-$p.log 2>&1; \
		 fi; echo RC=\$?"
	SSHRC=$?
	if [ $SSHRC -eq 124 ]; then
		echo "!!! $p TIMED OUT after ${TMO}s -- reclaiming the guest"
		# Best effort: stop the build so the guest is usable for the next recipe.
		#
		# Haiku's `ps` prints "Team Id #Threads Gid Uid", and Team is the team's
		# whole command line -- spaces included. So for
		# `timeout 14400 haikuporter ...` field $2 is "14400", and for the
		# matching pipeline itself it is "-i": never a pid. The Id is always the
		# 4th field from the end, so $(NF-3) is right whether or not the command
		# line contains spaces. Verified on a live guest: a team whose name has
		# no spaces gives NF=5 with $2 == $(NF-3) == the Id.
		#
		# Two further guards, both needed:
		#   - drop the matcher's own team. Its command line contains both
		#     "haikuporter" and "grep", so without this the kill takes out the
		#     very shell running it and never reaches `echo killed`.
		#   - require a numeric result, so a short line can never yield a stray
		#     word that kill would reject (or worse, misread).
		timeout 60 ssh $S $G 'ps 2>/dev/null | grep -i haikuporter | head -5' || true
		timeout 60 ssh $S $G 'kill -9 $(ps 2>/dev/null | grep -iE "haikuporter|conftest" | grep -vE "grep|awk" | awk "NF>=5 { print \$(NF-3) }" | grep -E "^[0-9]+$") 2>/dev/null; echo killed' || true
	fi

	scp $SCP "$G:/boot/home/f-$p.log" "$FLEET/logs/f-$p-$PORT.log" 2>/dev/null

	# Verify by capability: did an hpkg actually appear? An exit code of 0 has
	# lied here before, and an empty grep result means "not found", not "no news".
	echo "---- hpkg check (ls, not exit code) ----"
	timeout 60 ssh $S $G "ls $GP/ | grep -E \"^${p}[-_][0-9]\" || echo NO_PKG_$p"

	echo "---- last 25 log lines ----"
	tail -25 "$FLEET/logs/f-$p-$PORT.log" 2>/dev/null

	# Harvest through a staging dir, dropping haiku*.hpkg (build inputs, see above).
	_stage=$(mktemp -d)
	scp $SCP "$G:$GP/*.hpkg" "$_stage/" >/dev/null 2>&1
	rm -f "$_stage"/haiku.hpkg "$_stage"/haiku_*.hpkg "$_stage"/haiku-*.hpkg
	cp -pn "$_stage"/*.hpkg "$OUT/" 2>/dev/null
	rm -rf "$_stage"
	aws s3 sync "$OUT/" "$S3" --exclude "*" --include "*.hpkg" >/dev/null 2>&1

	echo "########## $p done $(date -u +%FT%TZ)"
	touch "$FLEET/heartbeat"
done

echo "=== FLEET WORKER $PORT DONE $(date -u +%FT%TZ) ==="
touch "$FLEET/worker-$PORT.done"
