#!/usr/bin/env bash
# Regression runner for the `cfg` layout plugin.
#
# For every fixture under test/fixtures it (1) smoke-tests that `dot -Kcfg`
# renders SVG without crashing, and (2) diffs the canonical `-Tdot` layout
# output (node positions, edge splines, cluster boxes/labels) against the
# committed golden snapshot. Layout is deterministic for a fixed graphviz
# version, so any geometry change shows up as a diff.
#
#   test/run.sh                 # run all categories
#   test/run.sh subgraphs ports # run only the named categories
#   DOT=/path/to/dot test/run.sh
#
# Regenerate goldens after an intentional change with test/gen-golden.sh.
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
FIX="$HERE/fixtures"
GOLD="$HERE/golden"
DOT="${DOT:-dot}"

cats=("$@")
[ ${#cats[@]} -eq 0 ] && cats=(layout edges subgraphs ports integration realworld)

pass=0; fail=0; rendered=0
declare -a failures

for cat in "${cats[@]}"; do
  for f in "$FIX/$cat"/*.dot; do
    [ -e "$f" ] || continue
    rel="$cat/$(basename "$f")"
    gold="$GOLD/$cat/$(basename "${f%.dot}").dot"

    if ! "$DOT" -Kcfg -Tsvg "$f" -o /dev/null 2>/dev/null; then
      echo "CRASH  $rel"; fail=$((fail+1)); failures+=("$rel (render failed)"); continue
    fi
    rendered=$((rendered+1))

    out="$("$DOT" -Kcfg -Tdot "$f" 2>/dev/null)"
    if [ ! -f "$gold" ]; then
      echo "NOGOLD $rel"; fail=$((fail+1)); failures+=("$rel (no golden)"); continue
    fi
    if [ "$out" = "$(cat "$gold")" ]; then
      pass=$((pass+1))
    else
      echo "DIFF   $rel"; fail=$((fail+1)); failures+=("$rel (geometry changed)")
    fi
  done
done

echo "--------------------------------------------"
echo "rendered ok: $rendered   golden match: $pass   failures: $fail"
if [ $fail -eq 0 ]; then
  echo "ALL PASS"
else
  echo "FAILURES:"
  printf '  - %s\n' "${failures[@]}"
  echo "(if a diff is intentional, review it then run test/gen-golden.sh)"
  exit 1
fi
