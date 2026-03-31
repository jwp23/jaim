#!/bin/sh

. ./common.sh

setup_test script
init_config

real_user_write_file "$CONFIG_DIR/from-config.sh" 'export SCRIPT_ORDER=config'
real_user_write_file "$WORKDIR/from-cli.sh" 'export SCRIPT_ORDER=cli'

cat >"$CONFIG_DIR/order.conf" <<'EOF'
conf .defaults
script from-config.sh
command source "${JAI_SCRIPT:-/dev/null}"; /usr/bin/env
EOF

capture_in_dir "$WORKDIR" run_jai --script from-cli.sh -C order
assert_status 0
assert_output_line "SCRIPT_ORDER=config"
