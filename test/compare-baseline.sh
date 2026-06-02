#!/usr/bin/env bash
# Prove the flat (non-cluster, non-port) layout is byte-for-byte unchanged
# against the pre-change baseline (git HEAD).
#
# Clusters and ports are new capabilities with no baseline to compare, so this
# only checks the categories that existed before: layout, edges, realworld.
# It builds+installs the HEAD version of the plugin, snapshots those fixtures,
# restores the working tree, and diffs.
#
# Requires a clean-ish tree (it stashes cfg_*.c/.h). Always restores on exit.
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
FIX="$HERE/fixtures"
DOT="${DOT:-dot}"
WORK="$(mktemp -d)"
SRC=(cfg_layout.c cfg_core.c cfg_core.h)

snap() { # $1 = output dir
  mkdir -p "$1"
  for cat in layout edges realworld; do
    for f in "$FIX/$cat"/*.dot; do
      [ -e "$f" ] || continue
      "$DOT" -Kcfg -Tdot "$f" 2>/dev/null > "$1/${cat}__$(basename "$f").txt"
    done
  done
}

cd "$ROOT"
stashed=0
restore() {
  [ "$stashed" = 1 ] && git stash pop -q 2>/dev/null
  make -s >/dev/null 2>&1 && make -s install >/dev/null 2>&1
}
trap restore EXIT

echo ">> building CURRENT working tree"
make -s >/dev/null && make -s install >/dev/null
snap "$WORK/new"

echo ">> stashing cfg sources -> building baseline (git HEAD)"
if git stash push -q -- "${SRC[@]}" 2>/dev/null; then stashed=1; fi
make -s >/dev/null && make -s install >/dev/null
snap "$WORK/orig"

echo ">> restoring working tree"
[ "$stashed" = 1 ] && { git stash pop -q; stashed=0; }
make -s >/dev/null && make -s install >/dev/null
trap - EXIT

echo "============================================"
if diff -rq "$WORK/orig" "$WORK/new" >/tmp/cfg_baseline_diff.txt 2>&1; then
  echo "IDENTICAL — flat layout/edges output unchanged vs baseline (git HEAD)"
  rm -rf "$WORK"
else
  echo "DIFFERENCES vs baseline:"
  cat /tmp/cfg_baseline_diff.txt
  echo "(snapshots kept in $WORK)"
  exit 1
fi
