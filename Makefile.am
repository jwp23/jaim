SUBDIRS = tests

bin_PROGRAMS = jai

jai_SOURCES = complete.cc cred.cc default_conf.cc fs.cc jai.cc	\
options.cc argtype.h config.h cred.h defer.h err.h fs.h jai.h	\
move_only_function.h options.h

man1_MANS = jai.1

jai.1: jai.1.md
	-pandoc -s -w man $(srcdir)/jai.1.md -o jai.1

sysusersdir = $(prefix)/lib/sysusers.d
sysusers_DATA = jai.conf

bashcompdir = $(datadir)/bash-completion/completions
bashcomp_DATA = bash-completion/jai

EXTRA_DIST = jai.1 jai.1.md jai.conf.in bash-completion/jai logo.svg

CLEANFILES = *~ jai.suid
DISTCLEANFILES = jai.conf

maintainer-clean-local:
	rm -rf .cache .deps
	rm -f compile_commands.json jai-*.tar.gz

MAINTAINERCLEANFILES = \
	jai.1 \
	$(srcdir)/aclocal.m4 \
	$(srcdir)/configure \
	$(srcdir)/Makefile.in \
	$(srcdir)/config.h.in \
	$(srcdir)/depcomp \
	$(srcdir)/install-sh \
	$(srcdir)/missing

JAI_USER = @UNTRUSTED_USER@

install-exec-hook:
	-chown root $(DESTDIR)$(bindir)/jai
	-chmod 04511 $(DESTDIR)$(bindir)/jai

install-data-hook:
	@if test -z "$(DESTDIR)" && \
		! getent passwd $(JAI_USER) >/dev/null; then \
	    printf "\nTo to create the jai user, please run\n\n"; \
	    if test /usr = "$(prefix)" -o /usr/local = "$(prefix)"; then \
	      printf "    systemd-sysusers\n\n"; \
	    else \
	      printf "    systemd-sysusers %s\n\n" "$(sysusersdir)/jai.conf"; \
	    fi; \
	fi

uninstall-hook:
	@if test -z "$(DESTDIR)" && \
		getent passwd $(JAI_USER) >/dev/null; then \
	    printf "\nTo to remove the jai user, please run\n\n    %s\n\n" \
		"userdel $(JAI_USER)"; \
	fi
