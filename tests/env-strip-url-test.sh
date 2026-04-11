#!/bin/sh
#
# Regression test for ja-24f: default_conf.cc previously missed most
# connection-string URL env vars (DATABASE_URL, REDIS_URL, MONGODB_URI,
# etc.), so applications that stash credentials in those vars leaked
# them into casual-mode sandboxes.  This test verifies that each of
# the newly-enumerated vars is stripped from the sandbox environment.
#
# Run manually from the jaim source root:
#
#     sh tests/env-strip-url-test.sh
#
# Like tests/credential-mask-test.sh and tests/macos-browser-mask-test.sh,
# this test is self-contained and does not use the autotools harness
# inherited from jai (which is not wired into the macOS GNUmakefile).

set -eu

JAIM=${JAIM:-./jaim}
[ -x "$JAIM" ] || { echo "SKIP: no jaim binary at $JAIM (run make first)"; exit 77; }

# IMPORTANT: ~/.jaim/.defaults is generated once and then cached on
# disk.  If the user built jaim with a previous env strip list, the
# new entries will not take effect until .defaults is regenerated.
# Regenerate here so the test exercises the current binary's defaults.
if [ -f "$HOME/.jaim/.defaults" ]; then
  rm -f "$HOME/.jaim/.defaults"
  "$JAIM" --version >/dev/null 2>&1 || true
fi

SENTINEL="jaim-envstrip-test-$$-sentinel"

fail=0
pass=0

# check <var-name> — export $var=$SENTINEL, run `env` inside jaim, and
# verify the var is absent from the sandbox environment.  A pass means
# the unsetenv rule fired; a fail means the credential leaked through.
check() {
  var=$1
  # Use `env NAME=VAL` to scope the export to this one jaim invocation
  # so we don't pollute the surrounding shell.  `-0` would be nicer for
  # parsing but `env` without args prints `NAME=VALUE` per line, which
  # is fine for a fixed-suffix grep.
  out=$(env "$var=$SENTINEL" "$JAIM" /usr/bin/env 2>&1 </dev/null) || {
    echo "  FAIL [$var]: jaim invocation failed: $out"
    fail=$((fail + 1))
    return 1
  }
  # Match on "NAME=sentinel" to avoid substring collisions with other
  # vars that happen to contain NAME as a suffix.
  if printf '%s\n' "$out" | grep -Fqx "$var=$SENTINEL"; then
    echo "  FAIL [$var]: leaked into sandbox env"
    fail=$((fail + 1))
    return 1
  fi
  echo "  ok   [$var]: stripped"
  pass=$((pass + 1))
}

echo "==> Connection-string URL env vars:"
check AMQP_URL
check CELERY_BROKER_URL
check CLICKHOUSE_URL
check DATABASE_URI
check DATABASE_URL
check DSN
check ELASTICSEARCH_URL
check INFLUXDB_URL
check MONGODB_URI
check MONGO_URI
check MONGO_URL
check RABBITMQ_URL
check REDIS_URI
check REDIS_URL
check SENTRY_DSN

echo
echo "==> Summary: $pass passed, $fail failed"
[ "$fail" -eq 0 ] || exit 1
exit 0
