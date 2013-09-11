
PROGRAMS=spiceterm

HEADERS=translations.h event_loop.h glyphs.h spiceterm.h
SOURCES=screen.c event_loop.c spiceterm.c

all: ${PROGRAMS}

spiceterm: ${SOURCES} ${HEADERS} spiceterm.c 
	gcc -Werror -Wall -Wtype-limits ${SOURCES} -o $@ -lutil $(shell pkg-config --cflags gdk-3.0) $(shell pkg-config --cflags --libs gthread-2.0,spice-protocol,spice-server)

.PHONY: test
test: spiceterm
	./spiceterm & remote-viewer spice://localhost:5912

.PHONY: distclean
distclean: clean

.PHONY: clean
clean:
	rm -rf *~ ${PROGRAMS}