#!/bin/sh
#
# Regression test for ja-1rj: default_conf.cc previously missed several
# common credential-bearing files and shell history files that hold
# plaintext secrets.  This test verifies that casual-mode jaim denies
# access (EPERM) to each such path.
#
# Run manually from the jaim source root:
#
#     sh tests/credential-mask-test.sh
#
# The autotools / Linux-style test harness in this directory is
# inherited from jai and is not wired into the jaim (macOS) GNUmakefile
# build, so this script is intentionally self-contained, mirroring
# tests/ssh-mask-bypass-test.sh and tests/macos-browser-mask-test.sh.
#
# The mask list in default_conf.cc uses fixed paths (e.g. mask .pgpass)
# — unlike .ssh, which denies a whole directory, most of these are
# per-file masks.  That means we cannot use a synthetic prefix the
# way the ssh test does; the sandbox only knows about the real path.
# Instead, for each target path we:
#
#   1. If the path already exists on the host: probe it via jaim.
#      We NEVER read, write, or delete existing host credentials —
#      the probe just runs `ls -d` against it from inside the sandbox
#      and asserts EPERM.
#
#   2. If the path does not exist: create a sentinel file containing
#      a unique marker, probe it, then delete it.  Parent dirs we
#      created along the way are rmdir'd in reverse order so we only
#      remove what we added.  A strict sentinel check ensures we
#      never delete a file we did not create.
#
# This guarantees the test never touches real credentials and leaves
# no trace on the host regardless of what was already present.

set -eu

JAIM=${JAIM:-./jaim}
[ -x "$JAIM" ] || { echo "SKIP: no jaim binary at $JAIM (run make first)"; exit 77; }

# IMPORTANT: ~/.jaim/.defaults is generated once and then cached on
# disk.  If the user built jaim with a previous mask set, the new
# masks will not take effect until .defaults is regenerated.  We do
# that here so the test exercises the current binary's defaults
# rather than a stale cache.
if [ -f "$HOME/.jaim/.defaults" ]; then
  rm -f "$HOME/.jaim/.defaults"
  "$JAIM" --version >/dev/null 2>&1 || true
fi

SENTINEL="jaim-credmask-test-$$-sentinel"
CLEANUP_FILES=""
CLEANUP_DIRS=""

cleanup() {
  for rel in $CLEANUP_FILES; do
    f="$HOME/$rel"
    # Only delete if the file still contains exactly our sentinel.
    # A mid-run crash could theoretically leave a file that a user
    # edited — this guards against overwriting unrelated data.
    if [ -f "$f" ]; then
      got=$(cat "$f" 2>/dev/null || true)
      if [ "$got" = "$SENTINEL" ]; then
        rm -f "$f"
      fi
    fi
  done
  # rmdir synthetic dirs in reverse order (innermost first).
  for rel in $CLEANUP_DIRS; do
    rmdir "$HOME/$rel" 2>/dev/null || true
  done
}
trap cleanup EXIT HUP INT TERM

fail=0
pass=0
existing=0
synth=0

# mkparents_tracked <rel> — create any parent dirs of $HOME/<rel>
# that do not already exist, recording them for cleanup with
# innermost first so rmdir unwinds correctly.
mkparents_tracked() {
  rel=$1
  dir=$(dirname "$rel")
  [ "$dir" = "." ] && return 0

  # Walk from the outermost missing dir down to the deepest so we
  # create parents before children.  Track newly-created dirs in
  # reverse (deepest first) for cleanup.
  to_create=""
  cur=$dir
  while [ "$cur" != "." ] && [ "$cur" != "/" ]; do
    if [ ! -d "$HOME/$cur" ]; then
      # Prepend — outermost missing ends up first in the list.
      to_create="$cur $to_create"
    fi
    cur=$(dirname "$cur")
  done
  for c in $to_create; do
    mkdir "$HOME/$c" || return 1
    # Record deepest first for cleanup.
    CLEANUP_DIRS="$c $CLEANUP_DIRS"
  done
}

# check <label> <rel-path> — ensure $HOME/<rel> is denied by jaim in
# casual mode.  Creates a sentinel file if the path does not exist.
check() {
  label=$1
  rel=$2
  abs="$HOME/$rel"

  created_synth=0
  if [ ! -e "$abs" ]; then
    mkparents_tracked "$rel" || {
      echo "  FAIL [$label]: could not create parent dirs for $rel"
      fail=$((fail + 1))
      return 1
    }
    printf '%s' "$SENTINEL" > "$abs" 2>/dev/null || {
      echo "  FAIL [$label]: could not create sentinel at $rel"
      fail=$((fail + 1))
      return 1
    }
    CLEANUP_FILES="$rel $CLEANUP_FILES"
    created_synth=1
    synth=$((synth + 1))
  else
    existing=$((existing + 1))
  fi

  # Probe with `ls -d` (does not need to read contents; just needs
  # stat+open-dir permission to be denied).  This matches the style
  # of macos-browser-mask-test.sh.
  out=$("$JAIM" /bin/ls -d "$abs" 2>&1 </dev/null) && rc=0 || rc=$?
  if [ "$rc" -eq 0 ]; then
    tag="existing"
    [ "$created_synth" -eq 1 ] && tag="synth"
    echo "  FAIL [$label] ($tag): $rel accessible inside casual-mode sandbox"
    fail=$((fail + 1))
    return 1
  fi
  case $out in
    *"Operation not permitted"*)
      tag="existing"
      [ "$created_synth" -eq 1 ] && tag="synth"
      echo "  ok   [$label] ($tag): $rel denied (EPERM)"
      pass=$((pass + 1))
      ;;
    *)
      echo "  FAIL [$label]: $rel denied but not via EPERM: $out"
      fail=$((fail + 1))
      return 1
      ;;
  esac
}

echo "==> Credential files:"
check pgpass       ".pgpass"
check mycnf        ".my.cnf"
check npmrc        ".npmrc"
check yarnrc       ".yarnrc"
check yarnrc-yml   ".yarnrc.yml"
check pypirc       ".pypirc"
check cargo-creds  ".cargo/credentials.toml"
check gem-creds    ".gem/credentials"
check hex-config   ".hex/hex.config"
check helm-repos   ".config/helm/repositories.yaml"
check rclone-conf  ".config/rclone/rclone.conf"

echo "==> Shell history files:"
check fish-history   ".local/share/fish/fish_history"
check nushell-txt    ".local/share/nushell/history.txt"
check nushell-sqlite ".local/share/nushell/history.sqlite3"
check xonsh-history  ".xonsh_history"

# Sanity: confirm the sandbox is casual (not strict) by reading a
# file under $HOME that is NOT masked.  If this fails, every check
# above could have been denied because the whole home was off limits
# — which would make the test meaningless.
sanity_file="$HOME/.jaim-credmask-test-sanity-$$"
printf 'sanity' > "$sanity_file"
CLEANUP_FILES="$(basename $sanity_file) $CLEANUP_FILES"
# Overwrite the sentinel-guarded cleanup: the sanity file is not a
# credential mask target, so delete it directly on exit regardless
# of contents.
trap 'cleanup; rm -f "'"$sanity_file"'"' EXIT HUP INT TERM

out=$("$JAIM" /bin/cat "$sanity_file" 2>&1 </dev/null) && rc=0 || rc=$?
if [ "$rc" -ne 0 ] || [ "$out" != "sanity" ]; then
  echo "FAIL: casual-mode sanity read failed — test results are not meaningful"
  echo "  jaim output: $out"
  exit 1
fi
echo "  ok   [sanity]: unmasked $HOME path still readable in casual mode"

if [ "$fail" -ne 0 ]; then
  echo "FAIL: credential-mask-test.sh ($fail failures, $pass passed)"
  echo "  ($existing existing host files probed, $synth sentinel files created)"
  exit 1
fi

echo "PASS: credential-mask-test.sh ($pass passed)"
echo "  ($existing existing host files probed, $synth sentinel files created)"
