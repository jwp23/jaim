#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Joseph Presley
#
# Regression test for ja-fe2: --setup-setuid flag and do_main's
# privilege drop for a setuid-root jaim install.
#
# --setup-setuid installs the running jaim binary with mode 4555 so
# users can invoke `jaim -m strict` without sudo.  Actually exercising
# chmod/chown requires root and mutates the installed binary, so this
# script only covers the checks that run before any privileged work
# and therefore fire reliably unprivileged:
#
#   1. Non-root invocation of --setup-setuid refuses and points at
#      sudo.  The check runs after do_main's privilege drop, so a
#      setuid-installed jaim without sudo still gets rejected — only
#      real sudo (or a non-setuid install run under sudo) reaches
#      the chmod.
#   2. --help advertises --setup-setuid.
#   3. `-m strict` without root (setuid or sudo) points at both
#      `sudo` and `--setup-setuid` as remediation.
#
# The actual chmod + strict-mode-without-sudo end-to-end path is a
# manual root-run smoke test rather than part of this script, since
# mutating the on-disk jaim binary is not something a CI-style test
# should do on the developer's machine.
#
# Run manually from the jaim source root:
#
#     sh tests/setup-setuid-test.sh

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

# --setup-setuid's require_root check fires after do_main's drop:
# when root, the drop is a no-op and the check would attempt the
# actual chmod — which is not what this test is set up to exercise.
if [ "$(id -u)" = 0 ]; then
  echo "SKIP: setup-setuid-test.sh must be run as non-root"
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

echo "==> --setup-setuid without root refuses and points at sudo"
out=$("$JAIM" --setup-setuid 2>&1); status=$?
assert_ok "$status" 1 "setup-setuid exit"
assert_contains "$out" "requires root" "setup-setuid message"
assert_contains "$out" "sudo" "setup-setuid sudo hint"

echo "==> --help advertises --setup-setuid"
out=$("$JAIM" --help 2>&1); status=$?
assert_ok "$status" 0 "help exit"
assert_contains "$out" "--setup-setuid" "help lists --setup-setuid"

echo "==> -m strict without root names both sudo and --setup-setuid"
out=$("$JAIM" -m strict /bin/true 2>&1); status=$?
assert_ok "$status" 1 "strict exit"
assert_contains "$out" "strict mode requires root" "strict message"
assert_contains "$out" "sudo" "strict sudo hint"
assert_contains "$out" "--setup-setuid" "strict setup-setuid hint"

echo
if [ "$fail" -gt 0 ]; then
  echo "FAIL: setup-setuid-test.sh ($fail failed, $pass passed)"
  exit 1
fi
echo "PASS: setup-setuid-test.sh ($pass passed)"
