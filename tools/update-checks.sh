#!/bin/bash
# Update CHECK lines in all e2e tests using SammineCheck --update.
# Usage: ./tools/update-checks.sh [path/to/specific/test.mn]

set -euo pipefail

SAMMINE="./build/bin/sammine"
CHECK="./build/bin/SammineCheck"
TMPDIR=$(mktemp -d)
trap "rm -rf $TMPDIR" EXIT

update_test() {
  local test_file="$1"
  local full="$(cd "$(dirname "$test_file")" && pwd)/$(basename "$test_file")"
  local base="$(basename "$test_file" .mn)"
  local dir="$(dirname "$full")"

  # Extract the RUN line
  local run_line
  run_line=$(grep -m1 '# RUN:' "$test_file" | sed 's/^# RUN: //')
  if [ -z "$run_line" ]; then
    echo "SKIP (no RUN): $test_file"
    return
  fi

  # Check if it has a %check — if not, nothing to update
  if ! echo "$run_line" | grep -q '%check'; then
    echo "SKIP (no %%check): $test_file"
    return
  fi

  # Split on | %check — left side is the command, right side is the check
  local cmd
  cmd=$(echo "$run_line" | sed 's/|[^|]*%check.*//')

  # Strip leading ! (expected failure)
  cmd=$(echo "$cmd" | sed 's/^!//')

  # Resolve substitutions
  cmd=$(echo "$cmd" \
    | sed "s|%sammine|$SAMMINE|g" \
    | sed "s|%full|$full|g" \
    | sed "s|%base|$base|g" \
    | sed "s|%dir|$dir|g" \
    | sed "s|%T|$TMPDIR|g" \
    | sed "s|%O|-O $TMPDIR|g" \
    | sed "s|%I|-I $TMPDIR|g" \
    | sed "s|%s|$full|g" \
  )

  # Run the command and pipe to SammineCheck --update
  local output
  if output=$(eval "$cmd" 2>&1); then
    echo "$output" | "$CHECK" --update "$full" 2>/dev/null && echo "OK: $test_file" || echo "FAIL: $test_file"
  else
    echo "$output" | "$CHECK" --update "$full" 2>/dev/null && echo "OK: $test_file" || echo "FAIL: $test_file"
  fi
}

if [ $# -gt 0 ]; then
  # Update specific files
  for f in "$@"; do
    update_test "$f"
  done
else
  # Update all .mn test files
  find e2e-tests -name '*.mn' -not -path '*/Inputs/*' | sort | while read -r f; do
    update_test "$f"
  done
fi
