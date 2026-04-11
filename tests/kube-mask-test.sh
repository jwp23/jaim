#!/bin/sh
#
# Regression test for ja-61n: default_conf.cc previously had
# `mask .config/kube`, but kubectl's default kubeconfig location is
# ~/.kube/config on both Linux and macOS.  The XDG-style path did not
# exist for any normal user, so casual-mode sandboxes could read the
# kubeconfig — which typically contains cluster API server URLs,
# long-lived service-account bearer tokens, client certs and keys,
# and cluster CA certs.  Anyone with the kubeconfig had full cluster
# access (subject to RBAC).
#
# Fix: mask .kube (whole directory).  This test verifies that the
# real ~/.kube/config path is denied (EPERM) in casual mode.
#
# Run manually from the jaim source root:
#
#     sh tests/kube-mask-test.sh
#
# The autotools / Linux-style test harness in this directory is
# inherited from jai and is not wired into the jaim (macOS) GNUmakefile
# build, so this script is intentionally self-contained, mirroring
# tests/credential-mask-test.sh, tests/ssh-mask-bypass-test.sh, and
# tests/macos-browser-mask-test.sh.
#
# The test never reads, writes, or deletes existing host credentials:
# if ~/.kube/config (or any probed file) already exists, we just run
# `ls -d` against it from inside the sandbox and assert EPERM.  If
# the path does not exist, we create a sentinel file containing a
# unique marker, probe it, then delete it — parent dirs we created
# are rmdir'd in reverse order so we only remove what we added.  A
# strict sentinel check ensures we never delete a file we did not
# create.

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

SENTINEL="jaim-kubemask-test-$$-sentinel"
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

  to_create=""
  cur=$dir
  while [ "$cur" != "." ] && [ "$cur" != "/" ]; do
    if [ ! -d "$HOME/$cur" ]; then
      to_create="$cur $to_create"
    fi
    cur=$(dirname "$cur")
  done
  for c in $to_create; do
    mkdir "$HOME/$c" || return 1
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

echo "==> Kubernetes config paths:"
# kubectl's real default — the whole reason this bug existed.
check kubeconfig         ".kube/config"
# kubectl writes discovery + http cache here; leaks cluster topology
# and sometimes bearer tokens alongside the kubeconfig.
check kube-cache         ".kube/cache"
# Per-user config files dropped in by tools like k9s, kubectx, etc.
# A blanket ~/.kube mask should cover any unusual filename.
check kube-unusual       ".kube/some-unusual-name"

# Sanity: confirm the sandbox is casual (not strict) by reading a
# file under $HOME that is NOT masked.  If this fails, every check
# above could have been denied because the whole home was off limits
# — which would make the test meaningless.
sanity_file="$HOME/.jaim-kubemask-test-sanity-$$"
printf 'sanity' > "$sanity_file"
trap 'cleanup; rm -f "'"$sanity_file"'"' EXIT HUP INT TERM

out=$("$JAIM" /bin/cat "$sanity_file" 2>&1 </dev/null) && rc=0 || rc=$?
if [ "$rc" -ne 0 ] || [ "$out" != "sanity" ]; then
  echo "FAIL: casual-mode sanity read failed — test results are not meaningful"
  echo "  jaim output: $out"
  exit 1
fi
echo "  ok   [sanity]: unmasked $HOME path still readable in casual mode"

if [ "$fail" -ne 0 ]; then
  echo "FAIL: kube-mask-test.sh ($fail failures, $pass passed)"
  echo "  ($existing existing host files probed, $synth sentinel files created)"
  exit 1
fi

echo "PASS: kube-mask-test.sh ($pass passed)"
echo "  ($existing existing host files probed, $synth sentinel files created)"
