#!/bin/sh
#
# Regression test for ja-9oc: a `setenv VAR=VALUE` line in a config
# file silently overrides any prior `unsetenv VAR` (or matching
# wildcard pattern), defeating the automatic credential strip.  jaim
# now emits a warning when this happens inside a config file.  This
# test verifies both the warning and the underlying (documented)
# override behavior: the variable does still pass through, but noisily.
#
# Run manually from the jaim source root:
#
#     sh tests/setenv-override-warn-test.sh
#
# Self-contained; does not use the autotools harness.

set -eu

JAIM=${JAIM:-./jaim}
[ -x "$JAIM" ] || { echo "SKIP: no jaim binary at $JAIM (run make first)"; exit 77; }

TMP=$(mktemp -d -t jaim-setenv-override.XXXXXX)
trap 'rm -rf "$TMP"' EXIT
export JAIM_CONFIG_DIR=$TMP

# Materialize .defaults so `conf .defaults` resolves.  Running jaim
# once with no work is the lightest way to do this.
"$JAIM" --version >/dev/null 2>&1 || true

fail=0
pass=0

# ---- case 1: config override of a wildcard unsetenv pattern ----
cat >"$TMP/wild.conf" <<'EOF'
conf .defaults
mode casual
setenv FAKE_API_KEY=${FAKE_API_KEY}
EOF

out=$(FAKE_API_KEY=sentinel-wild "$JAIM" -C "$TMP/wild.conf" /usr/bin/env 2>&1 </dev/null) || true

if printf '%s\n' "$out" | grep -q 'warning: setenv FAKE_API_KEY overrides unsetenv \*_API_KEY'; then
  echo "  ok   [wildcard]: warning emitted for *_API_KEY override"
  pass=$((pass + 1))
else
  echo "  FAIL [wildcard]: expected warning for *_API_KEY override, got:"
  printf '    %s\n' "$out" | head -20
  fail=$((fail + 1))
fi

# Verify the variable still leaked through — the warning is advisory,
# not a block.  If the underlying override behavior stops working this
# test will change, but for now it documents intentional semantics.
if printf '%s\n' "$out" | grep -Fqx 'FAKE_API_KEY=sentinel-wild'; then
  echo "  ok   [wildcard]: variable still overrides as documented"
  pass=$((pass + 1))
else
  echo "  FAIL [wildcard]: variable did not override as documented"
  fail=$((fail + 1))
fi

# ---- case 2: config override of an exact unsetenv name ----
cat >"$TMP/exact.conf" <<'EOF'
conf .defaults
mode casual
setenv DATABASE_URL=postgres://fake
EOF

out=$("$JAIM" -C "$TMP/exact.conf" /usr/bin/env 2>&1 </dev/null) || true

if printf '%s\n' "$out" | grep -q 'warning: setenv DATABASE_URL overrides unsetenv DATABASE_URL'; then
  echo "  ok   [exact]: warning emitted for DATABASE_URL override"
  pass=$((pass + 1))
else
  echo "  FAIL [exact]: expected warning for DATABASE_URL override, got:"
  printf '    %s\n' "$out" | head -20
  fail=$((fail + 1))
fi

# ---- case 3: config setenv for a non-filtered var — no warning ----
cat >"$TMP/clean.conf" <<'EOF'
conf .defaults
mode casual
setenv MY_CUSTOM_VAR=hello
EOF

out=$("$JAIM" -C "$TMP/clean.conf" /usr/bin/env 2>&1 </dev/null) || true

if printf '%s\n' "$out" | grep -q 'warning: setenv'; then
  echo "  FAIL [clean]: unexpected warning for non-filtered var, got:"
  printf '    %s\n' "$out" | head -20
  fail=$((fail + 1))
else
  echo "  ok   [clean]: no warning for non-filtered var"
  pass=$((pass + 1))
fi

# ---- case 4: CLI --setenv does NOT warn (explicit user intent) ----
out=$("$JAIM" -C "$TMP/clean.conf" --setenv DATABASE_URL=postgres://cli /usr/bin/env 2>&1 </dev/null) || true

if printf '%s\n' "$out" | grep -q 'warning: setenv'; then
  echo "  FAIL [cli]: unexpected warning for CLI --setenv, got:"
  printf '    %s\n' "$out" | head -20
  fail=$((fail + 1))
else
  echo "  ok   [cli]: no warning for command-line --setenv"
  pass=$((pass + 1))
fi

echo
echo "==> Summary: $pass passed, $fail failed"
[ "$fail" -eq 0 ] || exit 1
exit 0
