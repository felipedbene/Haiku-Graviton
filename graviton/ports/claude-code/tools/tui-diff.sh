#!/bin/bash
# tui-diff.sh <claude-binary> <node> <tree> [scenario-filter]: end-to-end screen
# diff of the interactive TUI. Each scenario runs twice, first the real Bun
# binary (A), then node + compat (B, <tree>/claude.mjs), each in a fresh tmux
# server with the same geometry, the same fresh HOME path and cwd, the same
# keystrokes. Every scenario starts with a snap, so no key is sent before the
# app has drawn (keys sent earlier land in the shell, not the app). After every step the pane is captured with `capture-pane -e -p`
# (text plus SGR attributes) once it has settled, and A and B are compared byte
# for byte. Animation: HOME/.claude/settings.json sets prefersReducedMotion,
# Claude Code's reduce-motion setting; "settled" means the pane is
# non-blank and unchanged over 3 s (6 captures 0.5 s apart, max 60 s: node
# starts slower than the binary), and a step that never settles on A is reported, not
# compared, so motion can't pass as a match.
#
# Scenario step syntax (one string per step, run in order):
#   size:COLSxROWS   resize the window      key:NAME    tmux key (Down, Enter, ...)
#   text:STRING      literal UTF-8 input    snap        capture and compare
#   mask:SED         sed script applied to both captures before comparing
#                    (named in the output; used only for the OAuth URL block,
#                    whose PKCE challenge, state and link id are random)
# SCEN_ENV="VAR=VAL ..." in the environment of a scenario call is exported to
# both sides (TERM_PROGRAM=vscode selects the renderer's bidi-reordering path).
# Prints PASS/FAIL per snapshot and "tui-diff: pass=N fail=M unsettled=U".
#
# Cross-host mode: TUI_ONLY=A or TUI_ONLY=B runs just that side, keeps every capture
# in $OUT (default /tmp/tuidiff) with <name>.n holding the snap count, and compares
# nothing; tools/tui-compare.sh then diffs an A capture dir from one host against a
# B capture dir from another (the real binary on Linux vs the package on Haiku).
set -u
BIN=$1 NODE=$2 TREE=$3 FILTER=${4:-}
H=/tmp/tuidiff-home S=tuidiff OUT=${OUT:-/tmp/tuidiff}
rm -rf $OUT; mkdir -p $OUT
pass=0 fail=0 unsettled=0

settle() {	# settle <file>: capture until non-blank and unchanged for 3 s; 1 if never
	local prev="" cur n=0 same=0
	while [ $n -lt 120 ]; do
		sleep 0.5; n=$((n + 1))
		cur=$(tmux -L $S capture-pane -t $S -e -p)
		if [ "$cur" = "$prev" ] && [ -n "$(printf '%s' "$cur" | tr -d ' \n')" ]; then
			same=$((same + 1)); [ $same -ge 6 ] && { printf '%s\n' "$cur" > "$1"; return 0; }
		else same=0; fi
		prev=$cur
	done
	printf '%s\n' "$cur" > "$1"; return 1
}

run_side() {	# run_side A|B <name> <cwd> <cols> <rows> <steps...>
	local side=$1 name=$2 cwd=$3 cols=$4 rows=$5; shift 5
	rm -rf $H; mkdir -p $H/.claude "$cwd"; : > $OUT/$name.mask
	echo '{"prefersReducedMotion":true}' > $H/.claude/settings.json
	local cmd="$BIN"; [ $side = B ] && cmd="$NODE --zero-fill-buffers $TREE/claude.mjs"
	tmux -L $S kill-server 2>/dev/null
	tmux -L $S new-session -d -s $S -x $cols -y $rows "cd '$cwd' && env ${SCEN_ENV:-} HOME=$H TERM=xterm-256color LANG=C.UTF-8 $cmd 2>$OUT/$name.$side.err"
	local k=0 st
	for st in "$@"; do
		case $st in
		size:*) local g=${st#size:}; tmux -L $S resize-window -t $S -x ${g%x*} -y ${g#*x}; sleep 0.3 ;;
		key:*) tmux -L $S send-keys -t $S "${st#key:}"; sleep 0.3 ;;
		text:*) tmux -L $S send-keys -t $S -l "${st#text:}"; sleep 0.3 ;;
		mask:*) echo "${st#mask:}" > $OUT/$name.mask ;;
		snap) k=$((k + 1)); settle $OUT/$name.$k.$side || echo unsettled > $OUT/$name.$k.$side.unsettled
			[ -s $OUT/$name.mask ] && { cp $OUT/$name.$k.$side $OUT/$name.$k.$side.raw; sed -i -E -f $OUT/$name.mask $OUT/$name.$k.$side; } ;;
		esac
	done
	tmux -L $S kill-server 2>/dev/null
	echo $k
}

scenario() {	# scenario <name> <cwd> <cols> <rows> <steps...>
	local name=$1
	[ -n "$FILTER" ] && [[ $name != $FILTER* ]] && return
	if [ -n "${TUI_ONLY:-}" ]; then
		run_side "$TUI_ONLY" "$@" | tail -1 > $OUT/$name.n
		echo "captured $name ($(cat $OUT/$name.n) snaps, side $TUI_ONLY)"
		return
	fi
	local n; n=$(run_side A "$@" | tail -1); run_side B "$@" > /dev/null
	local k
	for k in $(seq 1 $n); do
		local a=$OUT/$name.$k.A b=$OUT/$name.$k.B
		if [ -f $a.unsettled ]; then unsettled=$((unsettled + 1)); echo "UNSETTLED $name#$k (real binary still changing: not compared)"; continue; fi
		local m=""; [ -f $a.raw ] && m=", masked: $(cat $OUT/$name.mask)"
		if cmp -s $a $b; then pass=$((pass + 1)); echo "PASS $name#$k ($(wc -c < $a) bytes$m)"
		else fail=$((fail + 1)); echo "FAIL $name#$k"; diff $a $b | head -${DIFFN:-8} | cut -c1-200 | cat -v; fi
	done
	# evidence that typed text reached the screen: the prompt line(s) of every snap, side A
	[[ $name == text-* ]] && for k in $(seq 1 $n); do sed 's/\x1b\[[0-9;:]*m//g' $OUT/$name.$k.A | grep -A2 'Paste' | sed "s/^/  [$name#$k A] /" | head -3; done
	[ -n "${SHOW:-}" ] && { echo "--- $name last screen (A, text only):"; sed 's/\x1b\[[0-9;:]*m//g' $OUT/$name.$n.A | grep -v '^ *$' | head -${SHOW}; }
}

# Theme picker at several sizes (the first screen of a fresh install).
for g in 100x30 80x24 40x20 200x50 120x40 60x16; do
	scenario "theme-$g" /tmp/tuidiff-cwd ${g%x*} ${g#*x} snap
done
# Arrow navigation through every theme, and back.
steps=(snap); for i in 1 2 3 4 5 6 7; do steps+=(key:Down snap); done; for i in 1 2 3; do steps+=(key:Up snap); done
scenario theme-arrows /tmp/tuidiff-cwd 100 30 "${steps[@]}"
# Resize while open.
scenario theme-resize /tmp/tuidiff-cwd 100 30 snap size:60x20 snap size:140x45 snap size:30x12 snap size:100x30 snap
# Arrow + resize together.
scenario theme-arrow-resize /tmp/tuidiff-cwd 90 28 snap key:Down key:Down snap size:50x18 snap key:Down snap
# Theme picker -> login method -> "Paste code here if prompted >", the one
# pre-login screen that echoes typed text: wide, emoji, ZWJ, combining text.
# Compared region: the "Paste code" prompt line and everything below it. Above
# it sits the OAuth URL, whose PKCE challenge, state and link id are random per
# run (and at small sizes the URL is all that is left above the prompt); the
# screens before it, compared unmasked, already cover the banner and menus.
URLMASK='1,/Paste/{/Paste/!d}'
to_prompt=(snap key:Enter snap key:Enter "mask:$URLMASK" snap)
scenario login-flow /tmp/tuidiff-cwd 100 30 "${to_prompt[@]}"
T1='日本語のテキスト 中文 한국어 ＡＢＣ'
T2='👍🏽 😀 👨‍👩‍👧 🏳️‍🌈 🇺🇸🇯🇵 1️⃣ ❤️'
T3='é è ñ ü ạ̄ x̅ a̐ क्ष กำ'
T4='wide at the edge 日本語日本語日本語日本語日本語日本語日本語日本語日本語日本語日本語'
T5='שלום עולם مرحبا بالعالم abc 123'
for g in 100x30 40x20; do
	scenario "text-$g" /tmp/tuidiff-cwd ${g%x*} ${g#*x} "${to_prompt[@]}" "text:$T1" snap "text: $T2" snap "text: $T3" snap \
		key:BSpace key:BSpace key:BSpace snap "text: $T4" snap
done
scenario text-resize /tmp/tuidiff-cwd 80 24 "${to_prompt[@]}" "text:$T1 $T2" snap size:33x20 snap size:120x30 snap
# The prompt masks input but shows its tail, so the RTL text is typed last.
T6='123 abc (x) שלום עולם مرحبا ١٢٣'
scenario text-rtl /tmp/tuidiff-cwd 100 30 "${to_prompt[@]}" "text:$T5" snap "text: $T6" snap
SCEN_ENV="TERM_PROGRAM=vscode" scenario text-rtl-vscode /tmp/tuidiff-cwd 100 30 "${to_prompt[@]}" "text:$T5" snap "text: $T6" snap "text: $T1" snap
SCEN_ENV="TERM_PROGRAM=vscode" scenario theme-vscode /tmp/tuidiff-cwd 100 30 snap key:Down snap
if [ -n "${TUI_ONLY:-}" ]; then echo "tui-diff: captured side $TUI_ONLY into $OUT"; exit 0; fi
echo "tui-diff: pass=$pass fail=$fail unsettled=$unsettled"
[ $fail -eq 0 ] && [ $unsettled -eq 0 ]
