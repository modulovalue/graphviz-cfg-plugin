#!/usr/bin/env bash
# Regenerate golden -Tdot snapshots from the currently installed `cfg` plugin.
# Run this only after reviewing an intentional layout change (test/run.sh shows
# the diffs). Goldens are tied to the installed graphviz version.
#
#   test/gen-golden.sh                 # all categories
#   test/gen-golden.sh subgraphs       # only the named categories
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
FIX="$HERE/fixtures"
GOLD="$HERE/golden"
DOT="${DOT:-dot}"

cats=("$@")
[ ${#cats[@]} -eq 0 ] && cats=(layout edges subgraphs ports integration realworld)

n=0
for cat in "${cats[@]}"; do
  for f in "$FIX/$cat"/*.dot; do
    [ -e "$f" ] || continue
    g="$GOLD/$cat/$(basename "${f%.dot}").dot"
    mkdir -p "$(dirname "$g")"
    "$DOT" -Kcfg -Tdot "$f" > "$g" 2>/dev/null
    n=$((n+1))
  done
done
echo "wrote $n golden snapshots under $GOLD"
