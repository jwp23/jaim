AUTOMAKE_OPTIONS = subdir-objects

TESTS = \
		fs_acl_test \
		options_test \
		basic.sh \
		casual-overlay.sh \
		config-dir-hidden.sh \
		config-precedence.sh \
		home-cwd-guard.sh \
		jail-files.sh \
		job-control.sh \
		job-control-same-pgrp-stop.sh \
		job-control-sigtstp-fallback.sh \
		job-control-sigtstp-preserve.sh \
		modes.sh \
		mask.sh \
		process-state.sh \
		script-optional.sh \
		script.sh \
		strict-home-grant.sh \
		sudo-env.sh \
		storage.sh \
		storage-from-conf.sh \
		teardown.sh

check_PROGRAMS = jai_test_probe jai_test_pty_driver fs_acl_test options_test

jai_test_probe_SOURCES = jai-test-probe.cc
jai_test_pty_driver_SOURCES = jai-test-pty-driver.cc
fs_acl_test_SOURCES = fs_acl_test.cc ../fs.cc ../fs.h ../defer.h ../err.h
options_test_SOURCES = options_test.cc ../options.cc ../options.h

EXTRA_DIST = $(TESTS) common.sh.in

CLEANFILES = *~

maintainer-clean-local:
	rm -rf .cache .deps

MAINTAINERCLEANFILES = $(srcdir)/Makefile.in
