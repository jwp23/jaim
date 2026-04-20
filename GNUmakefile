# GNUmakefile — standalone build for macOS (no autotools required)

CXX ?= c++
CXXFLAGS ?= -O2
CXXFLAGS += -std=gnu++23 -Wall -Wextra
PREFIX ?= $(HOME)/.local

SRCS = acl.cc complete.cc cred.cc default_conf.cc fs.cc jaim.cc options.cc setup_user.cc
OBJS = $(SRCS:.cc=.o)
TARGET = jaim

# jaim-overlay: copy-on-write overlay filesystem used by casual mode.
# Built only when macFUSE 5.2+ is installed (libfuse C API headers
# under /usr/local/include/fuse).  When absent, `make` builds jaim
# alone; jaim-overlay is a hard requirement for casual-mode writable
# home — the casual-mode integration surfaces a clear error if the
# helper is missing rather than silently degrading.
FUSE_PREFIX ?= /usr/local
HAVE_FUSE := $(wildcard $(FUSE_PREFIX)/include/fuse/fuse.h)
ifneq (,$(HAVE_FUSE))
OVERLAY_TARGET = jaim-overlay
OVERLAY_CXXFLAGS = -I$(FUSE_PREFIX)/include/fuse -D_FILE_OFFSET_BITS=64 \
                   -D_DARWIN_USE_64_BIT_INODE
OVERLAY_LDFLAGS = -L$(FUSE_PREFIX)/lib -lfuse -pthread
endif

all: $(TARGET) $(OVERLAY_TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $^

%.o: %.cc
	$(CXX) $(CXXFLAGS) -c -o $@ $<

# overlay.cc is compiled with FUSE flags only and never linked into
# jaim itself — keeping the macFUSE dependency confined to the
# helper binary.
overlay.o: overlay.cc
	$(CXX) $(CXXFLAGS) $(OVERLAY_CXXFLAGS) -c -o $@ $<

jaim-overlay: overlay.o
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $^ $(OVERLAY_LDFLAGS)

# Header dependencies
acl.o: acl.h defer.h err.h argtype.h move_only_function.h
complete.o: jaim.h fs.h err.h defer.h options.h cred.h config.h argtype.h move_only_function.h
cred.o: cred.h err.h config.h
default_conf.o: jaim.h fs.h err.h defer.h options.h cred.h config.h argtype.h move_only_function.h
fs.o: fs.h defer.h err.h config.h argtype.h move_only_function.h
jaim.o: jaim.h fs.h err.h defer.h options.h cred.h config.h argtype.h move_only_function.h acl.h
options.o: options.h err.h config.h
setup_user.o: cred.h err.h config.h

install: $(TARGET) $(OVERLAY_TARGET)
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 $(TARGET) $(DESTDIR)$(PREFIX)/bin/
ifneq (,$(HAVE_FUSE))
	install -m 755 jaim-overlay $(DESTDIR)$(PREFIX)/bin/
endif

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/$(TARGET)
	rm -f $(DESTDIR)$(PREFIX)/bin/jaim-overlay

clean:
	rm -f $(OBJS) overlay.o $(TARGET) jaim-overlay

.PHONY: all install uninstall clean
