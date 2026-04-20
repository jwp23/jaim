#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Joseph Presley
#
# Regression test for ja-txx: strict-mode UID separation.
#
# Strict mode forks and setuids to the _jaim system user before
# sandbox_init, so the sandboxed child inherits an unprivileged UID
# that macOS TCC has no grants for.  Exercising the drop end-to-end
# requires root and a working _jaim user; this script only covers
# the prerequisite checks that run before any privileged work and
# therefore fire reliably when tests are run unprivileged:
#
#   1. Non-root invocation of `jaim -m strict` refuses and names sudo.
#   2. Casual and bare modes still run without root (regression guard
#      against accidentally tightening the check to every mode).
#   3. -u, --help, --version, --init, --setup-user, and --remove-user
#      still short-circuit before the strict-mode check (the check
#      lives in exec(), not at startup).
#
# Running the actual UID drop is gated behind `sudo jaim --setup-user`
# plus extended ACLs (ja-ekp) for CWD grants, so the full end-to-end
# path is a manual, root-run smoke test rather than part of this
# script.
#
# Run manually from the jaim source root:
#
#     sh tests/strict-uid-separation-test.sh

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

# Strict-mode's root check rejects non-root; running as root would
# instead try to perform the actual UID drop, which this test is
# deliberately not set up to exercise.
if [ "$(id -u)" = 0 ]; then
  echo "SKIP: strict-uid-separation-test.sh must be run as non-root"
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

assert_not_contains() {
  haystack=$1; needle=$2; label=$3
  case $haystack in
    *"$needle"*)
      echo "  FAIL [$label]: output unexpectedly contains \"$needle\""
      echo "    got: $haystack"
      fail=$((fail + 1))
      ;;
    *)
      echo "  ok   [$label]: does not contain \"$needle\""
      pass=$((pass + 1))
      ;;
  esac
}

echo "==> -m strict without root refuses and points at sudo"
out=$("$JAIM" -m strict /bin/true 2>&1); status=$?
assert_ok "$status" 1 "strict exit"
assert_contains "$out" "strict mode requires root" "strict message"
assert_contains "$out" "sudo" "strict sudo hint"

echo "==> -m strict with no cmd also refuses (same error)"
out=$("$JAIM" -m strict </dev/null 2>&1); status=$?
assert_ok "$status" 1 "strict noargs exit"
assert_contains "$out" "strict mode requires root" "strict noargs message"

echo "==> -m casual still runs without root"
out=$("$JAIM" -m casual /bin/sh -c 'id -u' 2>&1); status=$?
assert_ok "$status" 0 "casual exit"
assert_ok "$out" "$(id -u)" "casual runs as invoking uid"

echo "==> -m bare still runs without root"
out=$("$JAIM" -m bare /bin/sh -c 'id -u' 2>&1); status=$?
assert_ok "$status" 0 "bare exit"
assert_ok "$out" "$(id -u)" "bare runs as invoking uid"

echo "==> -m strict -u short-circuits before the root check"
out=$("$JAIM" -m strict -u 2>&1); status=$?
assert_ok "$status" 0 "strict -u exit"
assert_not_contains "$out" "strict mode requires root" "strict -u no prereq error"

echo "==> -m strict --help short-circuits before the root check"
out=$("$JAIM" -m strict --help 2>&1); status=$?
assert_ok "$status" 0 "strict --help exit"
assert_not_contains "$out" "strict mode requires root" "strict --help no prereq error"
assert_contains "$out" "usage:" "strict --help prints usage"

echo "==> -m strict --version short-circuits before the root check"
out=$("$JAIM" -m strict --version 2>&1); status=$?
assert_ok "$status" 0 "strict --version exit"
assert_not_contains "$out" "strict mode requires root" "strict --version no prereq error"

echo "==> -m strict --setup-user reaches require_root (not strict prereq)"
out=$("$JAIM" -m strict --setup-user 2>&1); status=$?
assert_ok "$status" 1 "strict --setup-user exit"
# --setup-user itself requires root, so the message is the setup-user
# one, not the strict-mode one: --setup-user is an early-exit action.
assert_contains "$out" "requires root" "strict --setup-user message"
assert_not_contains "$out" "strict mode requires root" "strict --setup-user bypasses strict prereq"

echo
if [ "$fail" -gt 0 ]; then
  echo "FAIL: strict-uid-separation-test.sh ($fail failed, $pass passed)"
  exit 1
fi
echo "PASS: strict-uid-separation-test.sh ($pass passed)"
