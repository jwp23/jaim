#!/bin/sh
#
# Regression test for ja-8lx: the CWD allow rule in jaim emitted an
# unscoped (allow file* (subpath CWD)) with no require-not clauses.
# Because Seatbelt unions allow rules, when CWD overlapped a masked
# subtree the masks were nullified for that subtree.  The worst case
# was `cd ~ && jaim` (the README's default invocation pattern) which
# silently exposed .ssh, .aws, .gnupg, shell histories, browser data,
# and every other masked path under $HOME.
#
# Fix: the CWD allow rule now carries the same require-not clauses as
# the home rule, and jaim refuses to start when CWD is itself inside a
# masked path (because require-not on the CWD subpath would nullify
# the rule entirely — better to fail loudly with a clear error than
# to let chdir fail with a confusing message).
#
# This test verifies:
#   1. `cd ~ && jaim cat ~/.ssh/<key>` returns EPERM — masks apply
#      to the CWD rule.
#   2. `cd ~/.ssh && jaim` refuses to start with a clear error.
#   3. `cd <sub> && jaim` refuses to start when <sub> is a descendant
#      of a masked path.
#   4. `-D/--nocwd` still disables CWD access entirely (no refusal,
#      no allow rule) — regression check.
#   5. `cd ~ && jaim` in casual mode can still read unmasked files
#      under $HOME (regression check — require-not should only
#      exclude masked paths, not block everything).
#   6. Running jaim from a non-home, non-masked directory still works
#      normally (regression check).
#
# Run manually from the jaim source root:
#
#     sh tests/cwd-mask-bypass-test.sh
#
# The autotools / Linux-style test harness in this directory is
# inherited from jai and is not wired into the jaim (macOS) GNUmakefile
# build, so this script is intentionally self-contained, mirroring
# tests/ssh-mask-bypass-test.sh, tests/kube-mask-test.sh, and
# tests/casual-home-readonly-test.sh.

set -u

# Resolve jaim to an absolute path so `cd`-ing during tests does not
# break the invocation.
if [ -n "${JAIM:-}" ]; then
  case $JAIM in
    /*) ;;
    *) JAIM="$PWD/$JAIM" ;;
  esac
else
  JAIM="$PWD/jaim"
fi
[ -x "$JAIM" ] || { echo "SKIP: no jaim binary at $JAIM (run make first)"; exit 77; }

# IMPORTANT: ~/.jaim/.defaults is generated once and then cached on
# disk.  Regenerate it so the test exercises the current binary.
if [ -f "$HOME/.jaim/.defaults" ]; then
  rm -f "$HOME/.jaim/.defaults"
  "$JAIM" --version >/dev/null 2>&1 || true
fi

SENTINEL="jaim-cwdmask-test-$$"

# Synthetic private key sentinel under ~/.ssh so the test does not
# touch any real key on the host.  The file is deleted on exit if
# its contents still match the sentinel.
SYNTH_KEY="$HOME/.ssh/${SENTINEL}-key"
SYNTH_KEY_CREATED=0
SYNTH_SUB_DIR="$HOME/.ssh/${SENTINEL}-sub"
SYNTH_SUB_CREATED=0
HOME_PROBE_FILE="$HOME/.${SENTINEL}-probe"
HOME_PROBE_CREATED=0
SSH_CREATED=0

cleanup() {
  if [ "$SYNTH_KEY_CREATED" -eq 1 ] && [ -f "$SYNTH_KEY" ]; then
    got=$(cat "$SYNTH_KEY" 2>/dev/null || true)
    if [ "$got" = "$SENTINEL" ]; then
      rm -f "$SYNTH_KEY"
    fi
  fi
  if [ "$SYNTH_SUB_CREATED" -eq 1 ] && [ -d "$SYNTH_SUB_DIR" ]; then
    rmdir "$SYNTH_SUB_DIR" 2>/dev/null || true
  fi
  if [ "$HOME_PROBE_CREATED" -eq 1 ] && [ -f "$HOME_PROBE_FILE" ]; then
    got=$(cat "$HOME_PROBE_FILE" 2>/dev/null || true)
    if [ "$got" = "$SENTINEL" ]; then
      rm -f "$HOME_PROBE_FILE"
    fi
  fi
  if [ "$SSH_CREATED" -eq 1 ] && [ -d "$HOME/.ssh" ]; then
    rmdir "$HOME/.ssh" 2>/dev/null || true
  fi
}
trap cleanup EXIT HUP INT TERM

# Ensure ~/.ssh exists for the "descendant of mask" test.  If the
# user already has a real ~/.ssh we leave it alone; otherwise we
# create a throwaway directory and track it for cleanup.
if [ ! -d "$HOME/.ssh" ]; then
  mkdir -m 700 "$HOME/.ssh" || {
    echo "FAIL: could not create $HOME/.ssh for test"
    exit 1
  }
  SSH_CREATED=1
fi

printf '%s' "$SENTINEL" > "$SYNTH_KEY" || {
  echo "FAIL: could not create $SYNTH_KEY"
  exit 1
}
chmod 600 "$SYNTH_KEY"
SYNTH_KEY_CREATED=1

mkdir "$SYNTH_SUB_DIR" || {
  echo "FAIL: could not create $SYNTH_SUB_DIR"
  exit 1
}
SYNTH_SUB_CREATED=1

# Seed an unmasked $HOME file for the regression check that casual
# mode from $HOME can still read unmasked files.  The dot prefix
# keeps it out of the way of a casual `ls ~`.
printf '%s' "$SENTINEL" > "$HOME_PROBE_FILE" || {
  echo "FAIL: could not create $HOME_PROBE_FILE"
  exit 1
}
HOME_PROBE_CREATED=1

fail=0
pass=0

# expect_denied_eperm <label> <cd-dir> <cmd...> — cd into <cd-dir>,
# run jaim with <cmd>, require it to fail via EPERM.
expect_denied_eperm() {
  label=$1; shift
  dir=$1; shift
  out=$(cd "$dir" && "$JAIM" "$@" 2>&1 </dev/null) && rc=0 || rc=$?
  if [ "$rc" -eq 0 ]; then
    echo "  FAIL [$label]: command succeeded but should have been denied"
    echo "    cmd: cd $dir && jaim $*"
    echo "    out: $out"
    fail=$((fail + 1))
    return 1
  fi
  case $out in
    *"Operation not permitted"*)
      echo "  ok   [$label]: denied (EPERM)"
      pass=$((pass + 1))
      ;;
    *)
      echo "  FAIL [$label]: denied but not via EPERM"
      echo "    cmd: cd $dir && jaim $*"
      echo "    out: $out"
      fail=$((fail + 1))
      return 1
      ;;
  esac
}

# expect_refused <label> <cd-dir> <cmd...> — cd into <cd-dir>, run
# jaim with <cmd>, require jaim to refuse to start with a
# "refusing to grant CWD access" error.
expect_refused() {
  label=$1; shift
  dir=$1; shift
  out=$(cd "$dir" && "$JAIM" "$@" 2>&1 </dev/null) && rc=0 || rc=$?
  if [ "$rc" -eq 0 ]; then
    echo "  FAIL [$label]: jaim started but should have refused"
    echo "    cmd: cd $dir && jaim $*"
    echo "    out: $out"
    fail=$((fail + 1))
    return 1
  fi
  case $out in
    *"refusing to grant CWD access"*)
      echo "  ok   [$label]: jaim refused to start"
      pass=$((pass + 1))
      ;;
    *)
      echo "  FAIL [$label]: failed but not with refusal message"
      echo "    cmd: cd $dir && jaim $*"
      echo "    out: $out"
      fail=$((fail + 1))
      return 1
      ;;
  esac
}

# expect_ok_stdout <label> <cd-dir> <expected-stdout> <cmd...> — cd
# into <cd-dir>, run jaim, require success with matching stdout.
expect_ok_stdout() {
  label=$1; shift
  dir=$1; shift
  want=$1; shift
  out=$(cd "$dir" && "$JAIM" "$@" 2>&1 </dev/null) && rc=0 || rc=$?
  if [ "$rc" -ne 0 ]; then
    echo "  FAIL [$label]: command failed (rc=$rc)"
    echo "    cmd: cd $dir && jaim $*"
    echo "    out: $out"
    fail=$((fail + 1))
    return 1
  fi
  if [ "$out" != "$want" ]; then
    echo "  FAIL [$label]: stdout mismatch"
    echo "    cmd: cd $dir && jaim $*"
    echo "    want: [$want]"
    echo "    got:  [$out]"
    fail=$((fail + 1))
    return 1
  fi
  echo "  ok   [$label]: succeeded"
  pass=$((pass + 1))
}

echo "==> Masks apply to CWD rule when CWD overlaps home (ja-8lx):"

# 1. `cd ~ && jaim cat ~/.ssh/<synth-key>` — masks must apply.
expect_denied_eperm "cd ~ && cat .ssh/key" \
  "$HOME" /bin/cat "$SYNTH_KEY"

echo "==> jaim refuses to start when CWD is inside a masked path:"

# 2. `cd ~/.ssh && jaim` — refused.
expect_refused "cd ~/.ssh && jaim" \
  "$HOME/.ssh" /bin/true

# 3. `cd ~/.ssh/<sub> && jaim` — refused.
expect_refused "cd ~/.ssh/<sub> && jaim" \
  "$SYNTH_SUB_DIR" /bin/true

echo "==> Regression: -D/--nocwd still disables CWD access:"

# 4. `-D` means no CWD allow rule is emitted, and the refusal check
#    does not apply either (there is no CWD grant to refuse).  jaim
#    should start and the sentinel key should still be denied
#    (because ~/.ssh is masked via the home mask-exclusion rule).
expect_denied_eperm "-D from ~/.ssh still denies key" \
  "$HOME/.ssh" -D /bin/cat "$SYNTH_KEY"

echo "==> Regression: casual mode from \$HOME can still read unmasked files:"

# 5. Reading the unmasked probe file from CWD = $HOME must work.
expect_ok_stdout "cd ~ && cat unmasked probe" \
  "$HOME" "$SENTINEL" /bin/cat "$HOME_PROBE_FILE"

echo "==> Regression: jaim from a non-masked directory still works:"

# 6. Running from the jaim source dir (outside $HOME masks) must
#    work and must still deny the synthetic key via the home rule.
expect_denied_eperm "cd /tmp && cat ~/.ssh/key" \
  "/tmp" /bin/cat "$SYNTH_KEY"

if [ "$fail" -ne 0 ]; then
  echo "FAIL: cwd-mask-bypass-test.sh ($fail failures, $pass passed)"
  exit 1
fi

echo "PASS: cwd-mask-bypass-test.sh ($pass passed)"
