#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
failures=0
total=0
for test_script in "$ROOT"/tests/*.sh; do
    case "$(basename "$test_script")" in run_all.sh) continue ;; esac
    total=$((total + 1))
    echo "===== $(basename "$test_script") ====="
    if TERM=${TERM:-xterm} sh "$test_script"; then
        :
    else
        failures=$((failures + 1))
    fi
done
echo "Termux regression summary: $((total - failures))/$total passed"
[ "$failures" -eq 0 ]
