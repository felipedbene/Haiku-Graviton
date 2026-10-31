#!/bin/bash
# stage-recipe.sh <haikuports-tree>: install the claude_code recipe as
# dev-util/claude_code in a HaikuPorts checkout, with this port's tooling as
# additional-files/claude-code-port.tar (evidence/ and recorded test output excluded).
set -euo pipefail
here=$(cd "$(dirname "$0")/.." && pwd)
hp=$1
d="$hp/dev-util/claude_code"
mkdir -p "$d/additional-files"
cp "$here"/recipe/claude_code-*.recipe "$d/"
# The upstream package's own LICENSE.md, verbatim, as a recipe-local license.
mkdir -p "$d/licenses" && cp "$here/recipe/licenses/"* "$d/licenses/"
COPYFILE_DISABLE=1 tar --no-xattrs -cf "$d/additional-files/claude-code-port.tar" -C "$here" \
	PATCHES.md LIMITATIONS.md AUDIT.md tools compat native recipe 2>/dev/null
echo "stage-recipe: $d ($(tar -tf "$d/additional-files/claude-code-port.tar" | wc -l | tr -d ' ') files)"
