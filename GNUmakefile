# GNUmakefile — standalone build for macOS (no autotools required)

CXX ?= c++
CXXFLAGS ?= -O2
CXXFLAGS += -std=gnu++23 -Wall -Wextra
PREFIX ?= /usr/local

SRCS = complete.cc cred.cc default_conf.cc fs.cc jaim.cc options.cc
OBJS = $(SRCS:.cc=.o)
TARGET = jaim

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $^

%.o: %.cc
	$(CXX) $(CXXFLAGS) -c -o $@ $<

# Header dependencies
complete.o: jaim.h fs.h err.h defer.h options.h cred.h config.h argtype.h move_only_function.h
cred.o: cred.h err.h config.h
default_conf.o: jaim.h fs.h err.h defer.h options.h cred.h config.h argtype.h move_only_function.h
fs.o: fs.h defer.h err.h config.h argtype.h move_only_function.h
jaim.o: jaim.h fs.h err.h defer.h options.h cred.h config.h argtype.h move_only_function.h
options.o: options.h err.h config.h

install: $(TARGET)
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 $(TARGET) $(DESTDIR)$(PREFIX)/bin/

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/$(TARGET)

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all install uninstall clean
