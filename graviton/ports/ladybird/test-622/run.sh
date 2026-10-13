#!/bin/sh
# Host test for UI/Haiku/DamageRefinement.h (#622): exact rects and pixels on asymmetric fixtures, plus a
# pixel-level model of the window/app_server present protocol (400 random scroll/edit/expose sequences, whose
# oracle is "the screen equals the last frame"). Pass any argument to also run the mutation arms, each of which
# must leave wrong pixels. Needs a C++20 compiler; uses ASan/UBSan when clang provides them.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
# The header is a new file in the patch: every line of its hunk is an addition.
awk '/^\+\+\+ b\/UI\/Haiku\/DamageRefinement.h/{f=1;next} /^diff --git/{f=0} f && /^\+/{print substr($0,2)}' \
    "$HERE/../ladybird-622.patch" > "$T/DamageRefinement.h"
CXX=${CXX:-clang++}
SAN="-fsanitize=address,undefined -fno-sanitize-recover=all"
$CXX -std=c++20 -O1 -g $SAN -Wall -Wextra -I"$T" -o "$T/t" "$HERE/test_damage.cpp" 2>/dev/null \
    || $CXX -std=c++20 -O1 -g -D_GLIBCXX_ASSERTIONS -Wall -Wextra -I"$T" -o "$T/t" "$HERE/test_damage.cpp"
"$T/t" "$@"
