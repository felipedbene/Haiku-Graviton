#!/bin/bash
# working-soak.sh <label> <minutes> <claude command...>: a working soak. Repeatedly types a
# small tool-using task into the interactive TUI (model via Amazon Bedrock, instance-profile
# credentials), waits for its output file, and records per-task latency plus the process's
# RSS. A task that does not complete within 150 s is reported as failed. Linux and Haiku.
set -u
label=$1 MIN=$2; shift 2; CMD="$*"
export CLAUDE_CODE_USE_BEDROCK=1 AWS_REGION=us-west-2 ANTHROPIC_MODEL=${ANTHROPIC_MODEL:-us.anthropic.claude-haiku-4-5-20251001-v1:0}
H=/tmp/ws-home-$label; rm -rf $H; mkdir -p $H/.claude
W=$(cd /tmp && pwd -P)/ws-$label; rm -rf $W; mkdir -p $W
echo '{"prefersReducedMotion":true,"theme":"dark","permissions":{"allow":["Bash(echo:*)","Bash(date:*)","Read","Write"]}}' > $H/.claude/settings.json
echo "{\"hasCompletedOnboarding\":true,\"theme\":\"dark\",\"projects\":{\"$W\":{\"hasTrustDialogAccepted\":true}}}" > $H/.claude.json
S=ws-$label; tmux -L $S kill-server 2>/dev/null; trap 'tmux -L $S kill-server 2>/dev/null' EXIT
tmux -L $S new-session -d -s $S -x 120 -y 40 "cd $W && env CLAUDE_CODE_USE_BEDROCK=1 AWS_REGION=$AWS_REGION ANTHROPIC_MODEL=$ANTHROPIC_MODEL HOME=$H TERM=xterm-256color $CMD"
sleep 15
pid_of() {	# the claude process: newest node/claude descendant of the tmux pane
	if [ "$(uname -s)" = Haiku ]; then ps | awk '/bin\/node --zero-fill-buffers \/boot\/system\/lib\/clau/ { print $(NF-3) }' | sort -n | tail -1
	else pgrep -n -f "$(echo "$CMD" | awk '{print $NF}')"; fi; }
rss() {
	if [ "$(uname -s)" = Haiku ]; then listarea "$1" | awk 'NF >= 10 && $(NF-5) ~ /^[0-9a-f]+$/ { s += strtonum("0x" $(NF-5)) } END { printf "%d", s / 1024 }'
	else awk '/VmRSS/ { print $2 }' /proc/$1/status; fi; }
p=$(pid_of); echo "[$label] pid/team $p, rss $(rss $p)KB"
end=$(( $(date +%s) + MIN * 60 )); k=0 ok=0 fail=0
while [ "$(date +%s)" -lt "$end" ]; do
	k=$((k + 1)); t0=$(date +%s)
	tmux -L $S send-keys -t $S -l "Run: echo soak-$k-\$(date +%s) and write its output to soak-$k.txt, then reply with only the word done-$k"; sleep 1; tmux -L $S send-keys -t $S Enter
	r="FAIL"; for i in $(seq 1 150); do [ -s $W/soak-$k.txt ] && { r="ok $(( $(date +%s) - t0 ))s"; break; }; sleep 1; done
	[ "$r" = FAIL ] && fail=$((fail + 1)) || ok=$((ok + 1))
	echo "[$label] task $k: $r rss=$(rss $p)KB"
	[ "$r" = FAIL ] && tmux -L $S capture-pane -t $S -p | grep -v '^ *$' | tail -6 | sed "s/^/[$label]   /"
	sleep 10
done
echo "[$label] working soak: $ok ok / $fail failed over ${MIN}min; final rss $(rss $p)KB"
[ $fail -eq 0 ]
