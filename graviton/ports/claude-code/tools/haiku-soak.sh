#!/bin/bash
# haiku-soak.sh [minutes] [interval-s] [prompt-every-n-samples]: run Claude Code's TUI in tmux on Haiku
# and sample the node team's resident memory (sum of listarea's alloc. = area ram_size)
# and thread count every interval, then print min/max/first/last and the growth.
# Runs on the Haiku target (via tools/ssh-run). Exits 1 if the process dies.
set -u
MIN=${1:-30} IV=${2:-30} EVERY=${3:-0}
# EVERY>0: a working soak. Every EVERY samples a small tool-using task is typed into the
# session (model via Amazon Bedrock with instance-profile credentials when
# CLAUDE_CODE_USE_BEDROCK=1 is exported), and its completion marker is checked.
S=soak; H=/tmp/soak-home; rm -rf $H; mkdir -p $H/.claude
W=$(cd /tmp && pwd -P)/soak-work; rm -rf $W; mkdir -p $W
echo '{"prefersReducedMotion":true,"theme":"dark","permissions":{"allow":["Bash(echo:*)","Bash(date:*)","Read","Write"]}}' > $H/.claude/settings.json
echo "{\"hasCompletedOnboarding\":true,\"theme\":\"dark\",\"projects\":{\"$W\":{\"hasTrustDialogAccepted\":true}}}" > $H/.claude.json
tmux -L $S kill-server 2>/dev/null
trap 'tmux -L $S kill-server 2>/dev/null' EXIT
before=$(ps | awk '/bin\/node --zero-fill-buffers \/boot\/system\/lib\/clau/ { print $(NF-3) }' | sort -n | tail -1)
tmux -L $S new-session -d -s $S -x 120 -y 40 "cd $W && env ${CLAUDE_CODE_USE_BEDROCK:+CLAUDE_CODE_USE_BEDROCK=1 AWS_REGION=$AWS_REGION ANTHROPIC_MODEL=$ANTHROPIC_MODEL} HOME=$H TERM=xterm-256color claude"
sleep 15
# Haiku ps: the team's args (truncated to 63 chars), then Id, #Threads, Gid, Uid. The
# newest matching team that did not exist before the launch is ours.
team=$(ps | awk -v b="${before:-0}" '/bin\/node --zero-fill-buffers \/boot\/system\/lib\/clau/ && $(NF-3) > b { print $(NF-3) }' | sort -n | tail -1)
echo "node team: ${team:-NOT FOUND}"; [ -n "$team" ] || { tmux -L $S capture-pane -t $S -p | head -20; exit 1; }
rss() { listarea "$team" | awk 'NF >= 10 && $(NF-5) ~ /^[0-9a-f]+$/ { s += strtonum("0x" $(NF-5)) } END { printf "%d", s / 1024 }'; }
threads() { ps | awk -v t="$team" '$(NF-3) == t { print $(NF-2); exit }'; }
n=$(( MIN * 60 / IV )); echo "sampling $n x ${IV}s"
first="" min="" max="" last="" tasks=0 ok=0
for i in $(seq 0 $n); do
	r=$(rss); t=$(threads)
	[ -n "$r" ] && [ "$r" -gt 0 ] || { echo "sample $i: process gone"; tmux -L $S capture-pane -t $S -p | tail -15; exit 1; }
	echo "t=$((i * IV))s rss=${r}KB threads=${t:-?}"
	if [ "$EVERY" -gt 0 ] && [ $i -gt 0 ] && [ $((i % EVERY)) = 0 ]; then
		k=$((i / EVERY)); tmux -L $S send-keys -t $S -l "Run: echo soak-$k-\$(date +%s) and write its output to soak-$k.txt, then reply with only the word done-$k"; sleep 1; tmux -L $S send-keys -t $S Enter
		tasks=$((tasks + 1))
	fi
	if [ "$EVERY" -gt 0 ] && [ $i -gt 0 ] && [ $(((i - 2) % EVERY)) = 0 ] && [ $i -ge 2 ]; then
		k=$(((i - 2) / EVERY)); [ $k -ge 1 ] && { tmux -L $S capture-pane -t $S -p | grep -q "done-$k" && [ -s $W/soak-$k.txt ] && ok=$((ok + 1)) && echo "  task $k: ok ($(cat $W/soak-$k.txt))" || echo "  task $k: NOT COMPLETED within $((2 * IV))s"; }
	fi
	first=${first:-$r}; last=$r
	[ -z "$min" ] || [ "$r" -lt "$min" ] && min=$r
	[ -z "$max" ] || [ "$r" -gt "$max" ] && max=$r
	[ $i -lt $n ] && sleep "$IV"
done
[ "$EVERY" -gt 0 ] && echo "soak tasks: $ok/$tasks completed"
echo "soak: first=${first}KB last=${last}KB min=${min}KB max=${max}KB growth=$(( last - first ))KB over ${MIN}min"
echo "--- final screen"; tmux -L $S capture-pane -t $S -p | grep -v '^ *$' | head -15
