#!/bin/sh

. ./common.sh

setup_test script-optional
init_config

cat >"$CONFIG_DIR/optional.conf" <<'EOF'
mode bare
command /usr/bin/env
EOF

capture_in_dir "$WORKDIR" run_jai --script? missing.sh -C optional
assert_status 0

mkdir "$WORKDIR/locked"
chmod 000 "$WORKDIR/locked"

capture_in_dir "$WORKDIR" run_jai --script? locked/file.sh -C optional
chmod 755 "$WORKDIR/locked"
assert_status 0
assert_not_contains "$CAPTURE_STDERR" "Permission denied"
