# Host functional tests for the artnet component.
#
# The component is plain C over BSD sockets, so it compiles and runs on Linux
# against the thin shims in shims/. The tests exchange real UDP datagrams on
# 127.0.0.1, which exercises the socket path, the parser and the packet
# builder rather than mocking them.
#
#   make        build and run
#   make clean

COMPONENT := ../../components/artnet
CFLAGS    := -std=c11 -D_GNU_SOURCE -Wall -Wextra -O2 -Ishims -I$(COMPONENT)/include
SRCS      := $(COMPONENT)/src/artnet.c test_artnet.c

.PHONY: all run clean
all: run

test_artnet: $(SRCS) $(wildcard shims/*.h shims/*/*.h)
	$(CC) $(CFLAGS) $(SRCS) -o $@

run: test_artnet
	./test_artnet

clean:
	rm -f test_artnet
