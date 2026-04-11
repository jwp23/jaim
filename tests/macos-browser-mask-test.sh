#!/bin/sh
#
# Regression test for ja-2s0: default_conf.cc previously only masked
# Linux XDG browser paths (.mozilla, .config/google-chrome, etc.).
# On macOS, browsers store cookies, saved logins, and payment methods
# under ~/Library/... — none of which were covered.  This test
# verifies that the macOS-native masks added to default_conf.cc
# actually take effect under casual mode (the default).
#
# Run manually from the jaim source root:
#
#     sh tests/macos-browser-mask-test.sh
#
# The autotools / Linux-style test harness in this directory is
# inherited from jai and is not wired into the jaim (macOS)
# GNUmakefile build, so this script is intentionally self-contained,
# mirroring tests/ssh-mask-bypass-test.sh.
#
# The test probes each masked directory with `ls -d` from inside a
# casual-mode jaim and asserts EPERM.  It does NOT create synthetic
# files inside the target directories — several of them (~/Library/
# Safari, ~/Library/Cookies, ~/Library/Keychains) are TCC-protected,
# so the host shell cannot write there regardless of jaim.  Missing
# directories are skipped (but still counted) so the test remains
# meaningful on hosts that do not have every browser installed.

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
  # Run jaim once with a trivial arg to regenerate .defaults.
  "$JAIM" --version >/dev/null 2>&1 || true
fi

fail=0
pass=0
skip=0

# `check <label> <relative-to-HOME dir>` — if DIR exists on the host,
# run it through jaim and expect Operation not permitted.
check() {
  label=$1
  rel=$2
  abs="$HOME/$rel"
  if [ ! -e "$abs" ]; then
    echo "  skip [$label]: $rel does not exist on this host"
    skip=$((skip + 1))
    return 0
  fi
  out=$("$JAIM" /bin/ls -d "$abs" 2>&1 </dev/null) && rc=0 || rc=$?
  if [ "$rc" -eq 0 ]; then
    echo "  FAIL [$label]: $abs accessible inside casual-mode sandbox"
    fail=$((fail + 1))
    return 1
  fi
  case $out in
    *"Operation not permitted"*)
      echo "  ok   [$label]: $rel denied (EPERM)"
      pass=$((pass + 1))
      ;;
    *)
      echo "  FAIL [$label]: $abs denied but not via EPERM: $out"
      fail=$((fail + 1))
      ;;
  esac
}

echo "==> Masked macOS browser paths:"
check safari   "Library/Safari"
check cookies  "Library/Cookies"
check keychain "Library/Keychains"
check chrome   "Library/Application Support/Google/Chrome"
check firefox  "Library/Application Support/Firefox"
check brave    "Library/Application Support/BraveSoftware"
check edge     "Library/Application Support/com.microsoft.edgemac"
check vivaldi  "Library/Application Support/Vivaldi"
check arc      "Library/Application Support/Arc"

# Sanity: confirm the sandbox is casual (not strict) by reading a
# file under $HOME that is NOT masked.  If this fails, every check
# above could have been denied because the whole home was off limits
# — which would make the test meaningless.
sanity_file="$HOME/.jaim-macos-browser-mask-test-sanity-$$"
cleanup_sanity() { rm -f "$sanity_file" 2>/dev/null || true; }
trap cleanup_sanity EXIT HUP INT TERM
printf 'sanity' > "$sanity_file"
out=$("$JAIM" /bin/cat "$sanity_file" 2>&1 </dev/null) && rc=0 || rc=$?
if [ "$rc" -ne 0 ] || [ "$out" != "sanity" ]; then
  echo "FAIL: casual-mode sanity read failed — test results are not meaningful"
  echo "  jaim output: $out"
  exit 1
fi
echo "  ok   [sanity]: unmasked $HOME path still readable in casual mode"

# Require at least one real denial check to pass so we cannot
# "succeed" on a host that skipped everything.
if [ "$pass" -eq 0 ]; then
  echo "FAIL: no masked directories exist on this host — cannot verify masks"
  exit 1
fi

if [ "$fail" -ne 0 ]; then
  echo "FAIL: macos-browser-mask-test.sh ($fail failures, $pass passed, $skip skipped)"
  exit 1
fi

echo "PASS: macos-browser-mask-test.sh ($pass passed, $skip skipped)"
