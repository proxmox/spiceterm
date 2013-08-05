
PROGRAMS=test_display_no_ssl
HEADERS=test_display_base.h basic_event_loop.h ring.h
SOURCES=test_display_base.c test_display_no_ssl.c basic_event_loop.c

all: ${PROGRAMS}

test_display_no_ssl: ${SOURCES} ${HEADERS}
	gcc ${SOURCES} -o $@ $(shell pkg-config --cflags --libs spice-protocol,spice-server)

.PHONY: test
test: test_display_no_ssl
	./test_display_no_ssl & remote-viewer spice://localhost:5912
