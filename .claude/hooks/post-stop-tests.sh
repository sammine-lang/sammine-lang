#!/bin/bash
# Post-Stop hook: runs build + tests if source files changed
# If tests fail, Claude continues and fixes them (exit 2)

input=$(cat)

# Ensure we're in the project root
cd "$CLAUDE_PROJECT_DIR" || exit 0

# Guard: skip if no source files changed (avoids running on pure Q&A)
changed=$(git diff --name-only 2>/dev/null)
if ! echo "$changed" | grep -qE '\.(cpp|h)$'; then
  echo "No source changes, skipping tests." >&2
  exit 0
fi

# Guard: skip if we already tested this exact set of changes
diff_hash=$(git diff 2>/dev/null | md5)
cache_file="$CLAUDE_PROJECT_DIR/.claude/.last_test_hash"
if [ -f "$cache_file" ] && [ "$(cat "$cache_file")" = "$diff_hash" ]; then
  echo "Already tested this diff, skipping." >&2
  exit 0
fi

# Always cache the hash so we don't re-run on the same diff
echo "$diff_hash" > "$cache_file"

# Run build + tests
output=$(cmake --build build -j --target unit-tests e2e-tests 2>&1)
rc=$?

# Check for real failures (not just unresolved tests)
# lit returns non-zero for UNRESOLVED tests too, so check output for actual FAILs
if echo "$output" | grep -qE 'FAILED|FAIL:'; then
  tail_output=$(echo "$output" | tail -40)
  # Clear cache so we re-test after fixes
  rm -f "$cache_file"
  cat <<EOF
{
  "decision": "block",
  "reason": "Build/tests FAILED after source changes. Fix the issues before stopping:\n${tail_output}"
}
EOF
  exit 2
fi

echo "All tests passed." >&2
exit 0
