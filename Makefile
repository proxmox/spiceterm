
PROGRAMS=test_display_no_ssl spiceterm

HEADERS=test_display_base.h basic_event_loop.h glyphs.h spiceterm.h
SOURCES=test_display_base.c basic_event_loop.c

all: ${PROGRAMS}

test_display_no_ssl: ${SOURCES} ${HEADERS} test_display_no_ssl.c 
	gcc ${SOURCES} test_display_no_ssl.c -o $@ $(shell pkg-config --cflags --libs  gthread-2.0,spice-protocol,spice-server)

spiceterm: ${SOURCES} ${HEADERS} spiceterm.c 
	gcc ${SOURCES} spiceterm.c -o $@ -lutil $(shell pkg-config --cflags gdk-3.0) $(shell pkg-config --cflags --libs gthread-2.0,spice-protocol,spice-server)

.PHONY: test1
test1: test_display_no_ssl
	./test_display_no_ssl & remote-viewer spice://localhost:5912

.PHONY: test2
test2: spiceterm
	./spiceterm & remote-viewer spice://localhost:5912

.PHONY: distclean
distclean: clean

.PHONY: clean
clean:
	rm -rf *~ ${PROGRAMS}