
PROGRAMS=spiceterm

HEADERS=translations.h event_loop.h glyphs.h spiceterm.h
SOURCES=screen.c event_loop.c input.c spiceterm.c auth-pve.c

#export G_MESSAGES_DEBUG=all 
#export SPICE_DEBUG=1

all: ${PROGRAMS}

spiceterm: ${SOURCES} ${HEADERS} spiceterm.c 
	gcc -Werror -Wall -Wtype-limits ${SOURCES} -g -O2 -o $@ -lutil $(shell pkg-config --cflags gdk-3.0) $(shell pkg-config --cflags --libs gthread-2.0,spice-protocol,spice-server)

.PHONY: test
test: spiceterm
	#./spiceterm & remote-viewer spice://localhost:5912
	G_MESSAGES_DEBUG=all SPICE_DEBUG=1 ./spiceterm & G_MESSAGES_DEBUG=all SPICE_DEBUG=1 remote-viewer --debug 'spice://localhost?tls-port=5912' --spice-ca-file /etc/pve/pve-root-ca.pem --spice-secure-channels=all

.PHONY: distclean
distclean: clean

.PHONY: clean
clean:
	rm -rf *~ ${PROGRAMS}