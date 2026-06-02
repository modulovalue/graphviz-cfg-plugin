#!/usr/bin/env bash
# Render every fixture with the `cfg` engine to a PDF, into test/render/ which
# mirrors test/fixtures/ and test/golden/ one-to-one. Useful for eyeballing what
# each fixture actually looks like. These are faithful renders (no injected
# caption) — the filename is the identity.
#
#   test/render.sh                 # all categories -> test/render/<cat>/<name>.pdf
#   test/render.sh subgraphs ports # only the named categories
#   test/render.sh --overview      # also build the combined captioned overview.pdf
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
FIX="$HERE/fixtures"
OUT="$HERE/render"
DOT="${DOT:-dot}"

overview=0
args=()
for a in "$@"; do
  [ "$a" = "--overview" ] && { overview=1; continue; }
  args+=("$a")
done
cats=("${args[@]}")
[ ${#cats[@]} -eq 0 ] && cats=(layout edges subgraphs ports integration realworld)

n=0
for cat in "${cats[@]}"; do
  for f in "$FIX/$cat"/*.dot; do
    [ -e "$f" ] || continue
    g="$OUT/$cat/$(basename "${f%.dot}").pdf"
    mkdir -p "$(dirname "$g")"
    "$DOT" -Kcfg -Tpdf "$f" -o "$g" 2>/dev/null
    n=$((n+1))
  done
done
echo "rendered $n PDFs under $OUT (mirrors fixtures/ and golden/)"

if [ "$overview" = 1 ] && command -v pdfunite >/dev/null; then
  TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT; i=0
  for cat in "${cats[@]}"; do
    for f in "$FIX/$cat"/*.dot; do
      [ -e "$f" ] || continue
      rel="$cat / $(basename "${f%.dot}")"
      sed "1 s|{|{ labelloc=\"t\"; fontsize=13; fontname=\"Courier-Bold\"; label=\"$rel\\\\n\"; |" "$f" \
        | "$DOT" -Kcfg -Tpdf -o "$(printf '%s/%03d.pdf' "$TMP" "$i")" 2>/dev/null && i=$((i+1)) || true
    done
  done
  pdfunite "$TMP"/*.pdf "$HERE/fixtures-overview.pdf"
  echo "wrote $HERE/fixtures-overview.pdf ($i captioned pages)"
fi
