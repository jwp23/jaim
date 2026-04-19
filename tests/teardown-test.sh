#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Joseph Presley
#
# Regression test for ja-8bh: jaim -u cleanup flag.
#
# The cleanup flag has two jobs:
#   1. Remove per-jail private home directories (<name>.home/) under
#      $JAIM_CONFIG_DIR.  Bare mode creates these and leaves them
#      across invocations by design, so users need a way to reset.
#   2. Remove stray per-invocation private tmp directories
#      (jaim.XXXXXXXX under $TMPDIR) left behind when a prior jaim
#      invocation was killed before exec()'s atexit_fn ran.
#
# With -j NAME, only that jail's home is removed.  Without -j, every
# <name>.home/ the user owns under $JAIM_CONFIG_DIR is swept.
#
# Run manually from the jaim source root:
#
#     sh tests/teardown-test.sh
#
# The autotools / Linux-style test harness in this directory is
# inherited from jai and is not wired into the jaim (macOS) GNUmakefile
# build, so this script is intentionally self-contained, mirroring
# tests/casual-home-readonly-test.sh and friends.

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

# Isolate every touched path under a dedicated $JAIM_CONFIG_DIR so the
# test cannot harm an existing ~/.jaim/ tree and so concurrent jaim
# invocations from the user's real session are not disturbed.
TEST_CFG=$(mktemp -d -t jaim-teardown-cfg-XXXXXX)
TEST_TMP=$(mktemp -d -t jaim-teardown-tmp-XXXXXX)
export JAIM_CONFIG_DIR="$TEST_CFG"
export TMPDIR="$TEST_TMP"

cleanup() {
  rm -rf "$TEST_CFG" "$TEST_TMP" 2>/dev/null || true
}
trap cleanup EXIT HUP INT TERM

# Regenerate .defaults so the sandbox exercises the current binary.
# Without this, a stale .defaults from an earlier build would be
# reused, and the test wouldn't actually test the current -u code.
rm -f "$TEST_CFG/.defaults"

fail=0
pass=0

assert_missing() {
  label=$1; path=$2
  if [ -e "$path" ]; then
    echo "  FAIL [$label]: expected missing, still present: $path"
    fail=$((fail + 1))
  else
    echo "  ok   [$label]: missing"
    pass=$((pass + 1))
  fi
}

assert_present() {
  label=$1; path=$2
  if [ -e "$path" ]; then
    echo "  ok   [$label]: present"
    pass=$((pass + 1))
  else
    echo "  FAIL [$label]: expected present, missing: $path"
    fail=$((fail + 1))
  fi
}

echo "==> -u before --init is a silent no-op (nothing to clean)"

# No $JAIM_CONFIG_DIR yet: -u should exit 0 and print nothing.  The
# atexit cleanup in exec() is not involved here because exec() never
# runs on the -u path.
rm -rf "$TEST_CFG"
out=$("$JAIM" -u 2>&1) && rc=0 || rc=$?
if [ "$rc" -eq 0 ] && [ -z "$out" ]; then
  echo "  ok   [pre-init no-op]: silent success"
  pass=$((pass + 1))
else
  echo "  FAIL [pre-init no-op]: rc=$rc out=[$out]"
  fail=$((fail + 1))
fi
# Restore config dir for the rest of the test.
mkdir -p "$TEST_CFG"

echo "==> targeted -u -j NAME removes only that jail's home"

# Seed three jails.  Non-empty contents prove that remove_all
# recurses, not just rmdir — a file under the home would keep a
# naïve rmdir cleanup from succeeding.
mkdir -p "$TEST_CFG/jail-a.home" \
         "$TEST_CFG/jail-b.home/nested" \
         "$TEST_CFG/jail-c.home"
printf seed > "$TEST_CFG/jail-a.home/file-a"
printf seed > "$TEST_CFG/jail-b.home/nested/file-b"

out=$("$JAIM" -u -j jail-a 2>&1) && rc=0 || rc=$?
if [ "$rc" -ne 0 ]; then
  echo "  FAIL [targeted]: rc=$rc out=[$out]"
  fail=$((fail + 1))
else
  echo "  ok   [targeted]: rc=0"
  pass=$((pass + 1))
fi
assert_missing "jail-a removed" "$TEST_CFG/jail-a.home"
assert_present "jail-b untouched" "$TEST_CFG/jail-b.home"
assert_present "jail-c untouched" "$TEST_CFG/jail-c.home"

echo "==> bare -u (no -j) removes every *.home under \$JAIM_CONFIG_DIR"

out=$("$JAIM" -u 2>&1) && rc=0 || rc=$?
if [ "$rc" -ne 0 ]; then
  echo "  FAIL [sweep]: rc=$rc out=[$out]"
  fail=$((fail + 1))
else
  echo "  ok   [sweep]: rc=0"
  pass=$((pass + 1))
fi
assert_missing "jail-b removed" "$TEST_CFG/jail-b.home"
assert_missing "jail-c removed" "$TEST_CFG/jail-c.home"

echo "==> -u does not touch non-*.home entries in \$JAIM_CONFIG_DIR"

# Everything under $JAIM_CONFIG_DIR that isn't a <name>.home directory
# must be left alone — config files, .jail files, arbitrary user
# data.  This is what separates teardown from `rm -rf`.
KEEPFILE="$TEST_CFG/default.jail"
printf 'mode bare\n' > "$KEEPFILE"
mkdir "$TEST_CFG/to-sweep.home"
out=$("$JAIM" -u 2>&1) && rc=0 || rc=$?
assert_missing "to-sweep.home removed" "$TEST_CFG/to-sweep.home"
assert_present "default.jail untouched" "$KEEPFILE"

echo "==> end-to-end: bare mode creates a home, -u sweeps it"

# Exercise the production path: bare mode creates <name>.home/ via
# ensure_udir during exec(), and -u must be able to tear that down.
"$JAIM" --init >/dev/null 2>&1 || true
out=$("$JAIM" -m bare -j e2e /usr/bin/true 2>&1) && rc=0 || rc=$?
if [ "$rc" -ne 0 ]; then
  echo "  FAIL [e2e run]: bare-mode invocation failed: rc=$rc out=[$out]"
  fail=$((fail + 1))
else
  assert_present "e2e.home created" "$TEST_CFG/e2e.home"
fi
"$JAIM" -u -j e2e >/dev/null 2>&1
assert_missing "e2e.home swept by -u" "$TEST_CFG/e2e.home"

echo "==> -u sweeps leaked jaim.XXXXXXXX dirs in \$TMPDIR"

# Simulate a signal-killed jaim that leaked its private tmp: plant a
# jaim.* directory owned by the user and verify -u removes it.  Also
# plant a non-jaim sibling to verify the sweep doesn't touch unrelated
# tmp files.
LEAK="$TEST_TMP/jaim.DEADBEEF"
KEEP="$TEST_TMP/other.XYZ"
mkdir "$LEAK" "$KEEP"
printf leak > "$LEAK/leftover"
printf keep > "$KEEP/payload"

"$JAIM" -u >/dev/null 2>&1
assert_missing "leaked jaim tmp removed" "$LEAK"
assert_present "non-jaim sibling untouched" "$KEEP"
rm -rf "$KEEP"

if [ "$fail" -ne 0 ]; then
  echo "FAIL: teardown-test.sh ($fail failures, $pass passed)"
  exit 1
fi

echo "PASS: teardown-test.sh ($pass passed)"
