#!/bin/bash
# haiku-soak.sh [minutes] [interval-s]: run Claude Code's interactive TUI in tmux on Haiku
# and sample the node team's resident memory (sum of listarea's alloc. = area ram_size)
# and thread count every interval, then print min/max/first/last and the growth.
# Runs on the Haiku target (via tools/ssh-run). Exits 1 if the process dies.
set -u
MIN=${1:-30} IV=${2:-30}
S=soak; H=/tmp/soak-home; rm -rf $H; mkdir -p $H/.claude
echo '{"prefersReducedMotion":true}' > $H/.claude/settings.json
tmux -L $S kill-server 2>/dev/null
tmux -L $S new-session -d -s $S -x 120 -y 40 "cd /tmp && HOME=$H TERM=xterm-256color claude"
sleep 15
# Haiku ps: the team's args, then Id, #Threads, Gid, Uid.
team=$(ps | awk '/[c]laude-code\/claude.mjs/ { print $(NF-3); exit }')
echo "node team: ${team:-NOT FOUND}"; [ -n "$team" ] || { tmux -L $S capture-pane -t $S -p | head -20; exit 1; }
rss() { listarea "$team" | awk 'NF >= 10 && $(NF-5) ~ /^[0-9a-f]+$/ { s += strtonum("0x" $(NF-5)) } END { printf "%d", s / 1024 }'; }
threads() { ps | awk -v t="$team" '$(NF-3) == t { print $(NF-2); exit }'; }
n=$(( MIN * 60 / IV )); echo "sampling $n x ${IV}s"
first="" min="" max="" last=""
for i in $(seq 0 $n); do
	r=$(rss); t=$(threads)
	[ -n "$r" ] && [ "$r" -gt 0 ] || { echo "sample $i: process gone"; tmux -L $S capture-pane -t $S -p | tail -15; exit 1; }
	echo "t=$((i * IV))s rss=${r}KB threads=${t:-?}"
	first=${first:-$r}; last=$r
	[ -z "$min" ] || [ "$r" -lt "$min" ] && min=$r
	[ -z "$max" ] || [ "$r" -gt "$max" ] && max=$r
	[ $i -lt $n ] && sleep "$IV"
done
echo "soak: first=${first}KB last=${last}KB min=${min}KB max=${max}KB growth=$(( last - first ))KB over ${MIN}min"
echo "--- final screen"; tmux -L $S capture-pane -t $S -p | grep -v '^ *$' | head -15
tmux -L $S kill-server
