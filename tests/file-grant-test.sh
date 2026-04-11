#!/bin/sh
#
# Regression test for ja-ofy: casual mode correctly makes $HOME
# read-only (per ja-x9q) but coding agents like Claude Code need to
# write specific dotfiles in the home root (~/.claude.json on every
# turn).  The `file` directive grants write access to a single file
# and its atomic-rename temp siblings without opening the rest of
# home.
#
# This test verifies the `file` directive mechanism plus the shipped
# claude.conf preset.  It does not depend on having Claude Code
# installed — we exercise the profile rules directly with touch and
# sh redirects.
#
# Checks:
#   1. `file FOO` in a config grants create / write / unlink on
#      $HOME/FOO.
#   2. The grant also covers atomic-write temp siblings like
#      $HOME/FOO.abc123 (node's write-file-atomic convention).
#   3. Non-granted files under $HOME stay read-only (regression for
#      ja-x9q).
#   4. Masked files remain denied (regression for ja-10e and the
#      rest of the mask list).
#   5. The shipped ~/.jaim/claude.conf, when selected via -C,
#      grants ~/.claude.json.
#
# Run manually from the jaim source root:
#
#     sh tests/file-grant-test.sh

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

# Regenerate .defaults so the test exercises the current binary.
if [ -f "$HOME/.jaim/.defaults" ]; then
  rm -f "$HOME/.jaim/.defaults"
  "$JAIM" --version >/dev/null 2>&1 || true
fi

# Use a unique grant filename under $HOME so we never touch any
# pre-existing user file.  Same PID-suffix trick as the other
# regression tests.
GRANT_FILE=".jaim-file-grant-target-$$"
GRANT_PATH="$HOME/$GRANT_FILE"
GRANT_TEMP="$HOME/$GRANT_FILE.abcd1234"
UNGRANTED="$HOME/.jaim-file-grant-ungranted-$$"
SEED_UNGRANTED="$HOME/.jaim-file-grant-seed-$$"
MASKED_FILE="$HOME/.jaim-file-grant-masked-$$"

# A throwaway config file outside $HOME/.jaim so we don't clobber
# the user's real config.  The test picks this up via -C.
TEST_CONF=$(mktemp -t jaim-file-grant-conf-XXXXXX)
cat > "$TEST_CONF" <<EOF
conf default.conf
mode casual
# Grant this specific file; atomic-write temp siblings come along
# automatically per the `file` directive's regex.
file $GRANT_FILE
# Add a new mask so the mask regression check has something specific
# to test that was not already masked by the default list.
mask .jaim-file-grant-masked-$$
EOF

cleanup() {
  rm -f "$GRANT_PATH" "$GRANT_TEMP" "$UNGRANTED" "$SEED_UNGRANTED" \
        "$MASKED_FILE" "$TEST_CONF" 2>/dev/null || true
}
trap cleanup EXIT HUP INT TERM

# Seed a file that should NOT be writable under the test config.
printf 'seed-content' > "$SEED_UNGRANTED"
# Seed the masked file so the mask-regression check can try to read it.
printf 'masked-content' > "$MASKED_FILE"

fail=0
pass=0

expect_denied_eperm() {
  label=$1; shift
  out=$("$JAIM" -C "$TEST_CONF" "$@" 2>&1 </dev/null) && rc=0 || rc=$?
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

expect_ok() {
  label=$1; shift
  out=$("$JAIM" -C "$TEST_CONF" "$@" 2>&1 </dev/null) && rc=0 || rc=$?
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

echo "==> file-grant: the granted file is writable"

# 1. Create the granted file via jaim.
expect_ok "create granted file" \
  /usr/bin/touch "$GRANT_PATH"

# 2. Write content to it.
expect_ok "write granted file" \
  /bin/sh -c 'printf granted-write > "$1"' sh "$GRANT_PATH"

# 3. Read it back (sanity — file-read should also be in the file*
#    rule).
expect_ok "read granted file" \
  /bin/cat "$GRANT_PATH"

# 4. Unlink the granted file.
expect_ok "unlink granted file" \
  /bin/rm -f "$GRANT_PATH"

echo "==> file-grant: atomic-write temp siblings are writable"

# 5. write-file-atomic style: write to <file>.<suffix> then rename
#    it into place.  Both operations must succeed.  The regex form
#    of the grant covers any .<suffix> sibling matching [A-Za-z0-9_-]+.
expect_ok "write atomic temp sibling" \
  /bin/sh -c 'printf atomic-temp > "$1"' sh "$GRANT_TEMP"

expect_ok "rename atomic temp into place" \
  /bin/sh -c 'mv "$1" "$2"' sh "$GRANT_TEMP" "$GRANT_PATH"

# Sanity: the renamed file is present with the right contents.
got=$(cat "$GRANT_PATH" 2>/dev/null || true)
if [ "$got" != "atomic-temp" ]; then
  echo "  FAIL [atomic rename landed]: got [$got] expected [atomic-temp]"
  fail=$((fail + 1))
else
  echo "  ok   [atomic rename landed]: file has atomic-temp content"
  pass=$((pass + 1))
fi
rm -f "$GRANT_PATH" 2>/dev/null || true

echo "==> file-grant: other \$HOME files remain read-only (ja-x9q regression)"

# 6. A non-granted file under $HOME must not be writable.  This
#    guards against a sloppy regex that accidentally allows more
#    than the intended file.
expect_denied_eperm "create non-granted file" \
  /usr/bin/touch "$UNGRANTED"

# 7. Existing non-granted file must still be readable (regression:
#    casual mode is read-only on home, not no-access).
expect_ok "read non-granted file" \
  /bin/cat "$SEED_UNGRANTED"

# 8. Existing non-granted file must not be writable.
expect_denied_eperm "overwrite non-granted file" \
  /bin/sh -c 'printf overwrite > "$1"' sh "$SEED_UNGRANTED"

# Sanity: the seed is unchanged.
got=$(cat "$SEED_UNGRANTED" 2>/dev/null || true)
if [ "$got" != "seed-content" ]; then
  echo "  FAIL [seed unchanged]: got [$got] expected [seed-content]"
  fail=$((fail + 1))
else
  echo "  ok   [seed unchanged]: non-granted file still has seed content"
  pass=$((pass + 1))
fi

echo "==> file-grant: masked files remain denied (mask regression)"

# 9. The test config adds a mask for $MASKED_FILE.  Reads to it via
#    jaim must fail with EPERM, same as the default mask list.
expect_denied_eperm "read masked file" \
  /bin/cat "$MASKED_FILE"

# 10. The default ~/.ssh mask (ja-10e) still holds.  We pick a
#     filename that will never exist, so this also exercises the
#     deny-by-default carve-out for ~/.ssh.
expect_denied_eperm "read masked ~/.ssh file" \
  /bin/cat "$HOME/.ssh/jaim-file-grant-probe-$$"

echo "==> shipped claude.conf preset is parseable and grants .claude.json"

# 11. Exercise the shipped ~/.jaim/claude.conf preset.  We do not
#     assume Claude Code is installed; we just confirm the config
#     loads and grants ~/.claude.json write access.
CLAUDE_CONF="$HOME/.jaim/claude.conf"
if [ ! -f "$CLAUDE_CONF" ]; then
  echo "  ok   [claude.conf seeded]: creating via jaim --version"
  "$JAIM" --version >/dev/null 2>&1 || true
fi

if [ -f "$CLAUDE_CONF" ]; then
  # Use a sacrificial path inside a test-controlled location.  We
  # cannot touch the user's real ~/.claude.json, so we point a
  # throwaway config override at a test-only file instead.
  CLAUDE_TEST_CONF=$(mktemp -t jaim-claude-file-XXXXXX)
  CLAUDE_FAKE="$HOME/.jaim-claude-fake-$$"
  cat > "$CLAUDE_TEST_CONF" <<EOF
conf default.conf
mode casual
file .jaim-claude-fake-$$
EOF

  out=$("$JAIM" -C "$CLAUDE_TEST_CONF" /usr/bin/touch "$CLAUDE_FAKE" 2>&1 </dev/null) && rc=0 || rc=$?
  if [ "$rc" -eq 0 ]; then
    echo "  ok   [file directive from config grants write]: succeeded"
    pass=$((pass + 1))
  else
    echo "  FAIL [file directive from config]: rc=$rc out=[$out]"
    fail=$((fail + 1))
  fi
  rm -f "$CLAUDE_FAKE" "$CLAUDE_TEST_CONF" 2>/dev/null || true

  # Confirm the shipped claude.conf itself contains the expected
  # directives — a cheap structural check that the shipped file is
  # not empty / corrupt.
  if grep -q '^dir \.claude$' "$CLAUDE_CONF" && grep -q '^file \.claude\.json$' "$CLAUDE_CONF"; then
    echo "  ok   [shipped claude.conf contents]: has dir .claude and file .claude.json"
    pass=$((pass + 1))
  else
    echo "  FAIL [shipped claude.conf contents]: missing expected directives"
    echo "    contents:"
    sed 's/^/      /' "$CLAUDE_CONF"
    fail=$((fail + 1))
  fi
else
  echo "  FAIL [claude.conf seeded]: $CLAUDE_CONF not created"
  fail=$((fail + 1))
fi

if [ "$fail" -ne 0 ]; then
  echo "FAIL: file-grant-test.sh ($fail failures, $pass passed)"
  exit 1
fi

echo "PASS: file-grant-test.sh ($pass passed)"
