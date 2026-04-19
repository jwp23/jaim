#!/bin/sh
#
# Regression test for ja-x9q: casual mode granted `file*` (full
# read/write/unlink) on the home directory, contradicting the README
# claim that casual mode is read-only on home.  A sandboxed agent
# running `rm -rf ~` in casual mode could destroy the user's home
# directory (minus masked subtrees).
#
# Fix: casual mode now grants `file-read*` on the home directory,
# matching bare mode and the README.  CWD and the private $TMPDIR
# remain writable so normal work (editing files, compilation, etc.)
# still functions; users who need writable state outside CWD should
# grant it with --dir.
#
# This test verifies:
#   1. Creating a file under $HOME via jaim fails with EPERM.
#   2. Writing to an existing file under $HOME via jaim fails with EPERM.
#   3. Deleting a file under $HOME via jaim fails with EPERM.
#   4. Reads of existing unmasked $HOME files still succeed (regression
#      check — we want read-only, not no-access).
#   5. CWD remains writable (regression check).
#   6. The shared system /tmp is denied (ja-4fk) but the sandbox's
#      private $TMPDIR is writable.
#
# Run manually from the jaim source root:
#
#     sh tests/casual-home-readonly-test.sh
#
# The autotools / Linux-style test harness in this directory is
# inherited from jai and is not wired into the jaim (macOS) GNUmakefile
# build, so this script is intentionally self-contained, mirroring
# tests/ssh-mask-bypass-test.sh and friends.

set -u

# Resolve jaim to an absolute path so `cd`-ing during the CWD test
# does not break the invocation.
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

# Paths under $HOME that we own end-to-end.  Each write test gets
# its own file so ordering does not matter and so a late failure
# does not mask an earlier one.
READ_SEED="$HOME/.jaim-casual-readonly-read-$$"
OVERWRITE_SEED="$HOME/.jaim-casual-readonly-overwrite-$$"
UNLINK_SEED="$HOME/.jaim-casual-readonly-unlink-$$"
NEW_FILE="$HOME/.jaim-casual-readonly-new-$$"
CWD_DIR=$(mktemp -d -t jaim-casual-readonly-XXXXXX)

cleanup() {
  rm -f "$READ_SEED" "$OVERWRITE_SEED" "$UNLINK_SEED" "$NEW_FILE" 2>/dev/null || true
  rm -rf "$CWD_DIR" 2>/dev/null || true
}
trap cleanup EXIT HUP INT TERM

# Seed the three files on the host (not via jaim).  We never touch
# any pre-existing user file — the PID-suffixed names guarantee
# uniqueness.
printf 'seed-content' > "$READ_SEED"
printf 'seed-content' > "$OVERWRITE_SEED"
printf 'seed-content' > "$UNLINK_SEED"

fail=0
pass=0

# expect_denied_eperm <label> <command> [args...] — run command through
# jaim and require it to fail with Operation not permitted.
expect_denied_eperm() {
  label=$1; shift
  out=$("$JAIM" "$@" 2>&1 </dev/null) && rc=0 || rc=$?
  if [ "$rc" -eq 0 ]; then
    echo "  FAIL [$label]: command succeeded but should have been denied"
    echo "    cmd: $*"
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
      echo "    cmd: $*"
      echo "    out: $out"
      fail=$((fail + 1))
      return 1
      ;;
  esac
}

# expect_ok <label> <command> [args...] — run command through jaim and
# require it to succeed (regression check for reads / CWD / /tmp).
expect_ok() {
  label=$1; shift
  out=$("$JAIM" "$@" 2>&1 </dev/null) && rc=0 || rc=$?
  if [ "$rc" -ne 0 ]; then
    echo "  FAIL [$label]: command failed (rc=$rc)"
    echo "    cmd: $*"
    echo "    out: $out"
    fail=$((fail + 1))
    return 1
  fi
  echo "  ok   [$label]: succeeded"
  pass=$((pass + 1))
}

echo "==> Reads under \$HOME must still succeed (casual is read-only, not no-access):"

# 1. Reads of existing unmasked files under $HOME must still work.
#    Done first so that the following destructive tests cannot
#    spuriously make this pass by having already wiped the seed.
expect_ok "read \$HOME file" \
  /bin/cat "$READ_SEED"

echo "==> Writes under \$HOME must be denied (casual mode is read-only on home):"

# 2. Creating a new file under $HOME via jaim must fail.
expect_denied_eperm "create \$HOME file" \
  /usr/bin/touch "$NEW_FILE"

# 3. Truncating / overwriting an existing $HOME file via jaim must fail.
expect_denied_eperm "overwrite \$HOME file" \
  /bin/sh -c 'printf overwritten > "$1"' sh "$OVERWRITE_SEED"

# Sanity: the seed we asked jaim to overwrite still contains the
# original contents.  If the overwrite test above reported EPERM but
# the file changed anyway, the sandbox is broken in a different way.
got=$(cat "$OVERWRITE_SEED" 2>/dev/null || true)
if [ "$got" != "seed-content" ]; then
  echo "  FAIL [overwrite content unchanged]: got [$got] expected [seed-content]"
  fail=$((fail + 1))
else
  echo "  ok   [overwrite content unchanged]: file still has seed content"
  pass=$((pass + 1))
fi

# 4. Deleting an existing $HOME file via jaim must fail.
expect_denied_eperm "unlink \$HOME file" \
  /bin/rm -f "$UNLINK_SEED"

# Sanity: the file we asked jaim to unlink is still present.
if [ ! -f "$UNLINK_SEED" ]; then
  echo "  FAIL [unlink file still present]: file was removed despite EPERM"
  fail=$((fail + 1))
else
  echo "  ok   [unlink file still present]: seed file still on disk"
  pass=$((pass + 1))
fi

echo "==> Regression: CWD remains writable:"

# 5. CWD is still writable — jaim grants `file*` on CWD regardless
#    of home-directory mode.  Run jaim from a directory outside $HOME
#    so this test is real isolation from the home rule.
(
  cd "$CWD_DIR" || exit 99
  out=$("$JAIM" /bin/sh -c 'printf cwd-write > cwd-file && /bin/cat cwd-file' 2>&1 </dev/null) && rc=0 || rc=$?
  if [ "$rc" -ne 0 ] || [ "$out" != "cwd-write" ]; then
    echo "  FAIL [cwd writable]: rc=$rc out=[$out]"
    exit 1
  fi
  echo "  ok   [cwd writable]: CWD write+read worked"
  exit 0
)
cwd_rc=$?
if [ "$cwd_rc" -ne 0 ]; then
  fail=$((fail + 1))
else
  pass=$((pass + 1))
fi

echo "==> ja-4fk: shared /tmp is denied, private \$TMPDIR is writable:"

# 6a. /tmp and /private/tmp are the shared system temp directories.
#     ja-4fk replaced the blanket allow on them with an allow on a
#     per-invocation private directory, so writes to /tmp must fail
#     and writes to $TMPDIR must succeed.
TMPNAME="jaim-casual-readonly-tmp-$$"
expect_denied_eperm "write /tmp denied" \
  /bin/sh -c 'printf tmp-write > "/tmp/'"$TMPNAME"'"'
expect_denied_eperm "write /private/tmp denied" \
  /bin/sh -c 'printf tmp-write > "/private/tmp/'"$TMPNAME"'"'

# 6b. Private $TMPDIR (set by jaim to the per-invocation dir under
#     the real TMPDIR) must be writable — otherwise programs that
#     rely on $TMPDIR for scratch space break inside the sandbox.
expect_ok "write \$TMPDIR" \
  /bin/sh -c 'printf tmp-write > "$TMPDIR/'"$TMPNAME"'" && /bin/cat "$TMPDIR/'"$TMPNAME"'"'

if [ "$fail" -ne 0 ]; then
  echo "FAIL: casual-home-readonly-test.sh ($fail failures, $pass passed)"
  exit 1
fi

echo "PASS: casual-home-readonly-test.sh ($pass passed)"
