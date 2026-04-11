#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Joseph Presley
#
# check-gpl3.sh - verify GPL-3.0-or-later compliance for changed files.
#
# Usage:
#     sh scripts/check-gpl3.sh           # check working tree vs HEAD
#     sh scripts/check-gpl3.sh --staged  # check staged changes (pre-commit)
#
# Rules:
#   1. New .cc/.h/.sh files must contain either
#      'SPDX-License-Identifier' or 'GNU General Public License'.
#   2. Modified .cc/.h files must contain at least one
#      'Modified YYYY by' line for the current year.
#
# Tests and other shell files that already exist without headers are
# grandfathered: rule (1) only triggers for files added in the diff,
# and rule (2) only applies to .cc/.h files.
#
# Exit status:
#   0  all checks passed (or nothing to check)
#   1  at least one violation
#   2  usage / environment error

set -u

mode=worktree
for arg in "$@"; do
  case $arg in
    --staged)
      mode=staged
      ;;
    -h|--help)
      sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'
      exit 0
      ;;
    *)
      echo "check-gpl3.sh: unknown argument: $arg" >&2
      echo "usage: sh $0 [--staged]" >&2
      exit 2
      ;;
  esac
done

if ! git rev-parse --git-dir >/dev/null 2>&1; then
  echo "check-gpl3.sh: not a git repository" >&2
  exit 2
fi

year=$(date +%Y)
violations=0

fail() {
  printf '  FAIL: %s\n' "$1"
  violations=$((violations + 1))
}

# Returns 0 if this file was subject to a rule (and passed or
# failed), nonzero if it was skipped because no rule applies.
check_new_file() {
  file=$1
  case $file in
    *.cc|*.h|*.sh)
      if ! grep -qE 'SPDX-License-Identifier|GNU General Public License' "$file"; then
        fail "$file: new file missing SPDX-License-Identifier or GPL notice"
      fi
      return 0
      ;;
  esac
  return 1
}

check_modified_file() {
  file=$1
  case $file in
    *.cc|*.h)
      if ! grep -qE "Modified[[:space:]]+${year}[[:space:]]+by" "$file"; then
        fail "$file: modified but has no 'Modified ${year} by <name>' notice"
      fi
      return 0
      ;;
  esac
  return 1
}

if [ "$mode" = staged ]; then
  added=$(git diff --cached --name-only --diff-filter=A)
  modified=$(git diff --cached --name-only --diff-filter=M)
else
  # Worktree mode: tracked modifications come from 'git diff HEAD',
  # and new-but-untracked files come from 'git ls-files --others'.
  # Combining both means the manual check sees files you haven't
  # staged yet, which is the whole point of running it before 'git
  # add'.  Exclude files ignored via .gitignore.
  tracked_added=$(git diff --name-only --diff-filter=A HEAD)
  untracked=$(git ls-files --others --exclude-standard)
  added=$(printf '%s\n%s\n' "$tracked_added" "$untracked" | sed '/^$/d')
  modified=$(git diff --name-only --diff-filter=M HEAD)
fi

checked=0

# Newline-safe iteration: use 'while read' with IFS cleared so
# filenames with spaces survive.  Empty input from git produces zero
# iterations.
if [ -n "$added" ]; then
  while IFS= read -r f; do
    [ -z "$f" ] && continue
    [ -f "$f" ] || continue
    if check_new_file "$f"; then
      checked=$((checked + 1))
    fi
  done <<END_OF_LIST
$added
END_OF_LIST
fi

if [ -n "$modified" ]; then
  while IFS= read -r f; do
    [ -z "$f" ] && continue
    [ -f "$f" ] || continue
    if check_modified_file "$f"; then
      checked=$((checked + 1))
    fi
  done <<END_OF_LIST
$modified
END_OF_LIST
fi

if [ "$violations" -gt 0 ]; then
  echo
  echo "check-gpl3.sh: ${violations} violation(s) in ${checked} file(s) checked"
  echo "  See CLAUDE.md > License compliance for the full rules."
  echo "  To bypass (only with a real reason): git commit --no-verify"
  exit 1
fi

echo "check-gpl3.sh: ${checked} file(s) checked, no violations"
exit 0
