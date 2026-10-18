#!/bin/bash
# Check a filter build against every case in tests/cases/<model>, byte for
# byte, using that model's PPD (ppd/<model>-open-source.ppd).
#
# Usage: tests/run.sh path/to/rastertotspl     (or: make test)
set -uo pipefail
cd "$(dirname "$0")/.."
FILTER=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
total_fail=0
for mdir in tests/cases/*/; do
  model=$(basename "$mdir")
  pass=0; fail=0
  for dir in "$mdir"*/; do
    name=$(basename "$dir")
    copies=$(sed -n 1p "$dir/args"); opts=$(sed -n 2p "$dir/args")
    gunzip -c "$dir/input.ras.gz" > "$tmp/in.ras"
    PPD=$PWD/ppd/$model-open-source.ppd "$FILTER" 1 user label "$copies" "$opts" "$tmp/in.ras" \
      > "$tmp/out.tspl" 2>/dev/null
    if cmp -s "$tmp/out.tspl" "$dir/expected.tspl"; then
      pass=$((pass + 1))
    else
      fail=$((fail + 1)); echo "FAIL $model/$name"
      python3 tests/decode_tspl.py "$tmp/out.tspl" | diff "$dir/expected.txt" - | head -20
    fi
  done
  echo "$model: $pass passed, $fail failed"
  total_fail=$((total_fail + fail))
done
[ "$total_fail" -eq 0 ]
