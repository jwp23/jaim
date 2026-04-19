#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Joseph Presley
#
# Regression test for ja-du9: --setup-user and --remove-user flags.
#
# These flags create and remove the _jaim system user (via dscl).
# Actually exercising dscl requires root and mutates host state, so
# this script only covers the checks that run before dscl is reached:
#
#   1. Non-root invocation refuses and points at sudo.
#   2. Supplying both flags at once is rejected.
#   3. --help advertises both flags.
#
# Run manually from the jaim source root:
#
#     sh tests/setup-user-test.sh
#
# The dscl-invoking code paths are covered by a manual, root-run smoke
# test rather than this script to avoid surprise side-effects on the
# developer's machine.

set -u

if [ -n "${JAIM:-}" ]; then
  case $JAIM in
    /*) ;;
    *) JAIM="$PWD/$JAIM" ;;
  esac
else
  JAIM="$PWD/jaim"
fi
[ -x "$JAIM" ] || { echo "SKIP: no jaim binary at $JAIM (run make first)"; exit 77; }

# The --setup-user and --remove-user paths check euid == 0 before
# doing any work; this test is only meaningful when run as non-root,
# since a root test would attempt to actually create the system user.
if [ "$(id -u)" = 0 ]; then
  echo "SKIP: setup-user-test.sh must be run as non-root"
  exit 77
fi

fail=0
pass=0

assert_ok() {
  if [ "$1" = "$2" ]; then
    echo "  ok   [$3]: $2"
    pass=$((pass + 1))
  else
    echo "  FAIL [$3]: got '$1', expected '$2'"
    fail=$((fail + 1))
  fi
}

assert_contains() {
  haystack=$1; needle=$2; label=$3
  case $haystack in
    *"$needle"*)
      echo "  ok   [$label]: contains \"$needle\""
      pass=$((pass + 1))
      ;;
    *)
      echo "  FAIL [$label]: output missing \"$needle\""
      echo "    got: $haystack"
      fail=$((fail + 1))
      ;;
  esac
}

echo "==> --setup-user without root refuses and points at sudo"
out=$("$JAIM" --setup-user 2>&1); status=$?
assert_ok "$status" 1 "setup-user exit"
assert_contains "$out" "requires root" "setup-user message"
assert_contains "$out" "sudo" "setup-user sudo hint"

echo "==> --remove-user without root refuses and points at sudo"
out=$("$JAIM" --remove-user 2>&1); status=$?
assert_ok "$status" 1 "remove-user exit"
assert_contains "$out" "requires root" "remove-user message"
assert_contains "$out" "sudo" "remove-user sudo hint"

echo "==> --setup-user + --remove-user together is rejected"
out=$("$JAIM" --setup-user --remove-user 2>&1); status=$?
assert_ok "$status" 1 "mutex exit"
assert_contains "$out" "mutually exclusive" "mutex message"

echo "==> --help advertises the new flags"
out=$("$JAIM" --help 2>&1); status=$?
assert_ok "$status" 0 "help exit"
assert_contains "$out" "--setup-user" "help lists --setup-user"
assert_contains "$out" "--remove-user" "help lists --remove-user"

echo
if [ "$fail" -gt 0 ]; then
  echo "FAIL: setup-user-test.sh ($fail failed, $pass passed)"
  exit 1
fi
echo "PASS: setup-user-test.sh ($pass passed)"
