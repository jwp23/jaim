
all: jai

CXX ?= c++
CXXFLAGS ?= -std=gnu++23 -Wall -Werror -ggdb
CPPFLAGS += $(shell pkg-config --cflags mount)
LDLIBS += $(shell pkg-config --libs mount)

OBJS = fs.o jai.o

all: jai

jai: $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $(OBJS) $(LDLIBS)

.c.o:
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -o $@ $<

$(OBJS): jai.h

clean:
	rm -f jai *~ *.o

.PHONY: all clean
