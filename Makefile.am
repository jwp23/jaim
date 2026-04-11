SUBDIRS = tests

bin_PROGRAMS = jaim

jaim_SOURCES = complete.cc cred.cc default_conf.cc fs.cc jaim.cc	\
options.cc argtype.h config.h cred.h defer.h err.h fs.h jaim.h	\
move_only_function.h options.h

man1_MANS = jaim.1

jaim.1: jaim.1.md
	-pandoc -s -w man $(srcdir)/jaim.1.md -o jaim.1

bashcompdir = $(datadir)/bash-completion/completions
bashcomp_DATA = bash-completion/jaim

EXTRA_DIST = jaim.1 jaim.1.md bash-completion/jaim logo.svg

CLEANFILES = *~ jaim.suid

maintainer-clean-local:
	rm -rf .cache .deps
	rm -f compile_commands.json jaim-*.tar.gz

MAINTAINERCLEANFILES = \
	jaim.1 \
	$(srcdir)/aclocal.m4 \
	$(srcdir)/configure \
	$(srcdir)/Makefile.in \
	$(srcdir)/config.h.in \
	$(srcdir)/depcomp \
	$(srcdir)/install-sh \
	$(srcdir)/missing
