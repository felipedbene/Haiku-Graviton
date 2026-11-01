#!/bin/bash
# compact-test.sh <label> <claude command...>: interactive TUI under tmux via Amazon Bedrock
# (instance-profile credentials): set a token, /compact, then ask for the token back.
# Prints timestamped screen snapshots; exits 0 only if compaction completes and the
# token is recalled. Used to compare the real binary, node+compat, and the Haiku package.
set -u
label=$1; shift; CMD="$*"
export CLAUDE_CODE_USE_BEDROCK=1 AWS_REGION=us-west-2 ANTHROPIC_MODEL=${ANTHROPIC_MODEL:-us.anthropic.claude-sonnet-5}
H=/tmp/compact-home-$label; rm -rf $H; mkdir -p $H/.claude
W=$(cd /tmp && pwd -P)/compact-$label; rm -rf $W; mkdir -p $W
echo '{"prefersReducedMotion":true,"theme":"dark"}' > $H/.claude/settings.json
echo "{\"hasCompletedOnboarding\":true,\"theme\":\"dark\",\"projects\":{\"$W\":{\"hasTrustDialogAccepted\":true}}}" > $H/.claude.json
S=ct-$label; tmux -L $S kill-server 2>/dev/null
tmux -L $S new-session -d -s $S -x 110 -y 34 "cd $W && env CLAUDE_CODE_USE_BEDROCK=1 AWS_REGION=$AWS_REGION ANTHROPIC_MODEL=$ANTHROPIC_MODEL HOME=$H TERM=xterm-256color $CMD 2>/tmp/compact-$label.err"
trap 'tmux -L $S kill-server 2>/dev/null' EXIT
cap() { tmux -L $S capture-pane -t $S -p | sed 's/[[:space:]]*$//' | grep -v '^$'; }
waitfor() { local i; for i in $(seq 1 $2); do cap | grep -q -- "$1" && return 0; sleep 1; done; return 1; }
type_() { tmux -L $S send-keys -t $S -l "$1"; sleep 1; tmux -L $S send-keys -t $S Enter; }
waitfor "❯" 60 || { echo "[$label] no prompt"; cap | tail -10; exit 1; }
type_ 'Remember this token for later: ZEBRA-42. Reply only OK.'
waitfor "● OK" 90 || { echo "[$label] no OK"; cap | tail -10; exit 1; }
type_ '/compact'
t0=$(date +%s); seen=0; done_=0
for i in $(seq 1 180); do
	c=$(cap)
	echo "$c" | grep -q "Compacting conversation" && { seen=1; [ $((i % 15)) = 0 ] && echo "[$label] t=$(( $(date +%s) - t0 ))s $(echo "$c" | grep -m1 'Compacting conversation')"; }
	if [ $seen = 1 ] && ! echo "$c" | grep -q "Compacting conversation"; then done_=1; break; fi
	sleep 1
done
echo "[$label] compaction: seen=$seen finished=$done_ after $(( $(date +%s) - t0 ))s"
cap | tail -8 | sed "s/^/[$label] /"
type_ 'What token did I ask you to remember? Answer with the token only.'
if waitfor "● .*ZEBRA-42" 120; then echo "[$label] recall: OK"; else echo "[$label] recall: FAILED"; fi
cap | tail -5 | sed "s/^/[$label] /"
echo "[$label] stderr: $(grep -v 'Cannot open directory /boot/system/non-packaged' /tmp/compact-$label.err | head -3)"
[ $done_ = 1 ] && cap | grep -q "● .*ZEBRA-42"
