#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Joseph Presley
#
# Regression test for ja-oa7: file descriptors above stderr inherited
# from the caller must be closed in the child before execve().  The
# macOS sandbox only enforces access control on path-based syscalls,
# so an FD the caller already has open is a bypass — the sandboxed
# program can read or write whatever the descriptor points at,
# regardless of the profile.  jaim's child-side scrub closes
# everything from fd 3 up, keeping only stdin/stdout/stderr.
#
# This test verifies:
#   1. A descriptor left open on fd 3 in the caller is closed inside
#      the sandbox (strongest path-denying mode, where the target
#      path itself is denied — so both the path and the FD must be
#      blocked).
#   2. Higher-numbered descriptors (fd 7 here) are also closed — the
#      scrub must cover the range, not just fd 3.
#   3. The scrub applies in every mode (casual and bare, not just
#      the path-denying mode) — the leak is a sandbox bypass
#      regardless of mode.
#   4. stdin, stdout, and stderr stay open — the sandboxed program
#      can still speak to its controlling tty.
#
# Since ja-txx, strict mode requires root (sudo).  Bare mode denies
# the real $HOME path identically (both rely on the default-deny
# rule at the top of the Seatbelt profile), so non-root runs exercise
# bare in place of strict — the FD-scrub invariant is mode-
# independent.  Root runs still exercise strict, which is the
# original intent.
#
# Run manually from the jaim source root:
#
#     sh tests/fd-scrub-test.sh
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

# Strict mode requires root (ja-txx).  When non-root, fall back to
# bare mode, which has the same $HOME path-denial semantics for the
# purposes of this test — both modes leave the real home off the
# sandbox's allow list, so opening SECRET by path fails either way,
# and the FD-leak probe still proves scrubbing works.  When root,
# test the original strict mode.
if [ "$(id -u)" = 0 ]; then
  DENY_MODE=strict
else
  DENY_MODE=bare
fi

# Regenerate .defaults so the test exercises the current binary.
if [ -f "$HOME/.jaim/.defaults" ]; then
  rm -f "$HOME/.jaim/.defaults"
  "$JAIM" --version >/dev/null 2>&1 || true
fi

# A secret file in $HOME: strict mode denies path-based access to it
# via the default-deny rule, so if the FD leaks into the sandbox,
# the sandboxed program can still read it through the descriptor.
# That is the bypass we want to close.
SECRET="$HOME/.jaim-fd-scrub-secret-$$"
SECRET_CONTENT="leaked-via-fd-$$"
printf '%s' "$SECRET_CONTENT" > "$SECRET"

cleanup() {
  rm -f "$SECRET" 2>/dev/null || true
  # Close any lingering descriptors from failed test invocations.
  exec 3<&- 7<&- 2>/dev/null || true
}
trap cleanup EXIT HUP INT TERM

fail=0
pass=0

# Emit a perl probe that tries to dup the named fd and read from it.
# Prints "LEAKED: <content>" on success, "OK <fd> closed: <err>" on
# failure.  Using perl (not bash) because bash silently swallows
# EBADF errors on some redirections; perl reports $! directly.
probe() {
  fd=$1
  # shellcheck disable=SC2016
  printf 'if(open F, "<&=%s"){print "LEAKED: ", scalar(<F>), "\\n"} else {print "OK fd%s closed: $!\\n"}' \
    "$fd" "$fd"
}

expect_closed() {
  label=$1; fd=$2; shift 2
  out=$("$@" </dev/null 2>&1) && rc=0 || rc=$?
  case $out in
    *"OK fd${fd} closed"*)
      echo "  ok   [$label]: fd $fd closed (rc=$rc)"
      pass=$((pass + 1))
      ;;
    *LEAKED*)
      echo "  FAIL [$label]: fd $fd leaked into sandbox"
      echo "    out: $out"
      fail=$((fail + 1))
      ;;
    *)
      echo "  FAIL [$label]: unexpected output"
      echo "    out: $out"
      fail=$((fail + 1))
      ;;
  esac
}

echo "==> host baseline: fd 3 open in the caller leaks without a sandbox"
# Sanity: if the parent shell's fd 3 is not actually open, the rest
# of the test is vacuous.  This proves the test is meaningful.
exec 3< "$SECRET"
out=$(perl -e "$(probe 3)" 2>&1)
case $out in
  "LEAKED: $SECRET_CONTENT")
    echo "  ok   [baseline leak]: unsandboxed perl reads fd 3"
    pass=$((pass + 1))
    ;;
  *)
    echo "  FAIL [baseline leak]: expected LEAKED, got [$out]"
    fail=$((fail + 1))
    ;;
esac
exec 3<&-

echo "==> $DENY_MODE mode: fd 3 must be scrubbed before execve"
exec 3< "$SECRET"
expect_closed "$DENY_MODE fd 3" 3 \
  "$JAIM" -m "$DENY_MODE" /usr/bin/perl -e "$(probe 3)"
exec 3<&-

echo "==> casual mode: fd 3 must also be scrubbed (leak is mode-independent)"
exec 3< "$SECRET"
expect_closed "casual fd 3" 3 \
  "$JAIM" -m casual /usr/bin/perl -e "$(probe 3)"
exec 3<&-

echo "==> bare mode: fd 3 must also be scrubbed"
exec 3< "$SECRET"
expect_closed "bare fd 3" 3 \
  "$JAIM" -m bare /usr/bin/perl -e "$(probe 3)"
exec 3<&-

echo "==> higher-numbered fds are scrubbed (not just fd 3)"
exec 7< "$SECRET"
expect_closed "$DENY_MODE fd 7" 7 \
  "$JAIM" -m "$DENY_MODE" /usr/bin/perl -e "$(probe 7)"
exec 7<&-

echo "==> stdio (fds 0/1/2) stays open — regression check"
# If the scrub is too aggressive and also closes stdio, /bin/echo
# inside the sandbox would have nowhere to write and this would
# either fail or produce no output.
out=$("$JAIM" -m "$DENY_MODE" /bin/echo "stdio-alive-$$" </dev/null 2>&1) && rc=0 || rc=$?
if [ "$rc" -eq 0 ] && [ "$out" = "stdio-alive-$$" ]; then
  echo "  ok   [stdio alive]: echo over stdout works post-scrub"
  pass=$((pass + 1))
else
  echo "  FAIL [stdio alive]: rc=$rc out=[$out]"
  fail=$((fail + 1))
fi

if [ "$fail" -ne 0 ]; then
  echo "FAIL: fd-scrub-test.sh ($fail failures, $pass passed)"
  exit 1
fi

echo "PASS: fd-scrub-test.sh ($pass passed)"
