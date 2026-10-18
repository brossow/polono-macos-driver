#!/bin/bash
# Build the test cases in tests/cases/<model> from a known-good filter.
#
# Usage: tests/make-cases.sh path/to/rastertotspl [model ...]
#        (or: make test-vectors MODELS="PL70e")
#
# Models default to every tests/<model>.cases table. Each table line is
#   name | width_mm | height_mm | pages | copies | options
# and the model's PPD is ppd/<model>-open-source.ppd.
#
# Each case: input.pdf (test label), input.ras.gz (the CUPS raster macOS made
# from it, the filter's real input), args (copies, then the options string),
# expected.tspl (the filter's output) and expected.txt (decoded, readable).
# Nothing is sent to a printer.
set -euo pipefail
cd "$(dirname "$0")/.."
FILTER=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
shift
models=("$@")
if [ ${#models[@]} -eq 0 ]; then
  for t in tests/*.cases; do models+=("$(basename "$t" .cases)"); done
fi

for model in "${models[@]}"; do
  PPD_FILE=$PWD/ppd/$model-open-source.ppd
  OUT=tests/cases/$model
  rm -rf "$OUT"; mkdir -p "$OUT"
  while IFS='|' read -r name w h pages copies opts; do
    name=$(echo "$name" | xargs)
    [ -z "$name" ] && continue
    dir=$OUT/$name
    mkdir -p "$dir"
    python3 tests/make_test_pdf.py "$(echo "$w" | xargs)" "$(echo "$h" | xargs)" "$dir/input.pdf" "$pages"
    cf_args=(-p "$PPD_FILE" -m application/vnd.cups-raster -n "$copies")
    for o in $opts; do cf_args+=(-o "$o"); done
    cupsfilter "${cf_args[@]}" "$dir/input.pdf" > "$dir/input.ras" 2>/dev/null
    printf '%s\n%s\n' "$copies" "$opts" > "$dir/args"
    PPD=$PPD_FILE "$FILTER" 1 user label "$copies" "$opts" "$dir/input.ras" > "$dir/expected.tspl" 2>/dev/null
    python3 tests/decode_tspl.py "$dir/expected.tspl" > "$dir/expected.txt"
    gzip -n -9 -f "$dir/input.ras"
  done < "tests/$model.cases"
  echo "$model: $(ls "$OUT" | wc -l | xargs) cases written to $OUT"
done
