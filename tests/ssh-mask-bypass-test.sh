#!/bin/sh
#
# Regression test for ja-7n4: the ~/.ssh mask bypass where literal-name
# masks (mask .ssh/id_rsa, etc.) failed to cover private keys with the
# common id_<algo>_<label> naming convention or any non-standard name.
#
# This test creates synthetic files inside the real ~/.ssh directory
# with a uniquely-prefixed name so it can verify deny-by-default under
# ~/.ssh without touching the user's actual keys.  Run manually from
# the jaim source root:
#
#     sh tests/ssh-mask-bypass-test.sh
#
# The autotools / Linux-style test harness in this directory is
# inherited from jai and is not wired into the jaim (macOS) GNUmakefile
# build, so this script is intentionally self-contained.

set -eu

JAIM=${JAIM:-./jaim}
[ -x "$JAIM" ] || { echo "SKIP: no jaim binary at $JAIM (run make first)"; exit 77; }

[ -d "$HOME/.ssh" ] || mkdir -m 700 "$HOME/.ssh"

PREFIX=".jaim-ssh-mask-test-$$"
CLEANUP=""

cleanup() {
  for name in $CLEANUP; do
    rm -f "$HOME/.ssh/$name" 2>/dev/null || true
  done
}
trap cleanup EXIT HUP INT TERM

make_test_file() {
  name=$1
  body=$2
  printf '%s' "$body" > "$HOME/.ssh/$name"
  chmod 600 "$HOME/.ssh/$name"
  CLEANUP="$CLEANUP $name"
}

assert_denied() {
  name=$1
  out=$("$JAIM" /bin/cat "$HOME/.ssh/$name" 2>&1 </dev/null) && rc=0 || rc=$?
  if [ "$rc" -eq 0 ]; then
    echo "FAIL: ~/.ssh/$name was readable inside sandbox (exit $rc)"
    exit 1
  fi
  case $out in
    *"Operation not permitted"*) : ;;
    *)
      echo "FAIL: ~/.ssh/$name denied but not via EPERM: $out"
      exit 1
      ;;
  esac
}

assert_allowed() {
  name=$1
  expect=$2
  out=$("$JAIM" /bin/cat "$HOME/.ssh/$name" 2>&1 </dev/null) && rc=0 || rc=$?
  if [ "$rc" -ne 0 ]; then
    echo "FAIL: ~/.ssh/$name denied inside sandbox (exit $rc): $out"
    exit 1
  fi
  if [ "$out" != "$expect" ]; then
    echo "FAIL: ~/.ssh/$name body was [$out], expected [$expect]"
    exit 1
  fi
}

# Private-key-style files with varying names should all be denied.
make_test_file "${PREFIX}-id_ed25519"            "synth-priv-id_ed25519"
make_test_file "${PREFIX}-id_ed25519_work"       "synth-priv-id_ed25519_work"
make_test_file "${PREFIX}-google_compute_engine" "synth-priv-gce"
make_test_file "${PREFIX}-unusual_name"          "synth-priv-unusual"

assert_denied "${PREFIX}-id_ed25519"
assert_denied "${PREFIX}-id_ed25519_work"
assert_denied "${PREFIX}-google_compute_engine"
assert_denied "${PREFIX}-unusual_name"

# Public keys are supposed to be readable (git, ssh-add -L, etc.).
make_test_file "${PREFIX}-id_ed25519.pub" "ssh-ed25519 synth-pub test@host"
assert_allowed "${PREFIX}-id_ed25519.pub" "ssh-ed25519 synth-pub test@host"

echo "PASS: ssh-mask-bypass-test.sh"
