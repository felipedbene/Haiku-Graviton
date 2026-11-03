#!/bin/bash
# working-soak.sh <label> <minutes> <claude command...>: a working soak. Repeatedly types a
# small tool-using task into the interactive TUI (model via Amazon Bedrock, instance-profile
# credentials), waits for its output file, and records per-task latency plus the process's
# RSS. A task that does not complete within 150 s is reported as failed. Linux and Haiku.
set -u
label=$1 MIN=$2; shift 2; CMD="$*"
# DISABLE_AUTOUPDATER: the real binary otherwise updates itself mid-run, so the two sides
# would no longer be the same version.
export DISABLE_AUTOUPDATER=1 CLAUDE_CODE_USE_BEDROCK=1 AWS_REGION=us-west-2 ANTHROPIC_MODEL=${ANTHROPIC_MODEL:-us.anthropic.claude-haiku-4-5-20251001-v1:0}
H=/tmp/ws-home-$label; rm -rf $H; mkdir -p $H/.claude
W=$(cd /tmp && pwd -P)/ws-$label; rm -rf $W; mkdir -p $W
echo '{"prefersReducedMotion":true,"theme":"dark","permissions":{"allow":["Bash(echo:*)","Bash(date:*)","Read","Write"]}}' > $H/.claude/settings.json
echo "{\"hasCompletedOnboarding\":true,\"theme\":\"dark\",\"projects\":{\"$W\":{\"hasTrustDialogAccepted\":true}}}" > $H/.claude.json
S=ws-$label; tmux -L $S kill-server 2>/dev/null; trap 'tmux -L $S kill-server 2>/dev/null' EXIT
tmux -L $S new-session -d -s $S -x 120 -y 40 "cd $W && env DISABLE_AUTOUPDATER=1 CLAUDE_CODE_USE_BEDROCK=1 AWS_REGION=$AWS_REGION ANTHROPIC_MODEL=$ANTHROPIC_MODEL HOME=$H TERM=xterm-256color $CMD"
# Wait for the TUI to be ready for input (its "❯" prompt line), not a fixed delay: on a cold
# first start the fixed 15 s was too short and task 1's Enter was swallowed (010361cf).
for i in $(seq 1 90); do tmux -L $S capture-pane -t $S -p | grep -q '^❯' && break; sleep 1; done
sleep 2
pid_of() {	# the claude process: newest node/claude descendant of the tmux pane
	if [ "$(uname -s)" = Haiku ]; then ps | awk '/bin\/node --zero-fill-buffers \/boot\/system\/lib\/clau/ { print $(NF-3) }' | sort -n | tail -1
	else pgrep -o -x claude; fi; }	# Claude Code sets process.title to "claude" (Linux shows it; Haiku keeps the node args)
rss() {
	if [ "$(uname -s)" = Haiku ]; then listarea "$1" | awk 'NF >= 10 && $(NF-5) ~ /^[0-9a-f]+$/ { s += strtonum("0x" $(NF-5)) } END { printf "%d", s / 1024 }'
	else awk '/VmRSS/ { print $2 }' /proc/$1/status; fi; }
p=$(pid_of); echo "[$label] pid/team $p, rss $(rss $p)KB"
end=$(( $(date +%s) + MIN * 60 )); k=0 ok=0 fail=0
while [ "$(date +%s)" -lt "$end" ]; do
	k=$((k + 1)); t0=$(date +%s)
	# The task pins the exact command form. The allowlist below covers Bash(echo:*) and Write,
	# and nothing else: a model that folds the two steps into one Bash call
	# (`echo ... > f && cat f`) is not covered, waits at a permission prompt and fails the
	# task on its tool choice, not on the port (measured on 010361cf, run 1: 0/12). Spelling
	# the form out keeps a pass or fail about the port; the allowlist is unchanged.
	tmux -L $S send-keys -t $S -l "Use the Bash tool to run exactly this command, with no redirection and nothing else: echo soak-$k-ok   Then use the Write tool, not Bash, to write that command's output to the file soak-$k.txt. Then reply with only the word done-$k"; sleep 1; tmux -L $S send-keys -t $S Enter
	# If the task text is still sitting in the input box, the Enter was not taken: send it
	# once more. (Checked against the pane, so a task that was submitted is never re-sent.)
	sleep 3; tmux -L $S capture-pane -t $S -p | grep -q "^❯ Use the Bash tool to run exactly this command.*soak-$k-ok" && tmux -L $S send-keys -t $S Enter
	r="FAIL"; for i in $(seq 1 150); do [ -s $W/soak-$k.txt ] && { r="ok $(( $(date +%s) - t0 ))s"; break; }; sleep 1; done
	[ "$r" = FAIL ] && fail=$((fail + 1)) || ok=$((ok + 1))
	p=$(pid_of); echo "[$label] task $k: $r rss=$(rss $p)KB (pid $p)"
	[ "$r" = FAIL ] && tmux -L $S capture-pane -t $S -p | grep -v '^ *$' | tail -6 | sed "s/^/[$label]   /"
	sleep 10
done
echo "[$label] working soak: $ok ok / $fail failed over ${MIN}min; final rss $(rss $p)KB"
[ $fail -eq 0 ]
