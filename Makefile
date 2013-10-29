
PROGRAMS=spiceterm

HEADERS=translations.h event_loop.h glyphs.h spiceterm.h keysyms.h
SOURCES=screen.c event_loop.c input.c spiceterm.c auth-pve.c

#export G_MESSAGES_DEBUG=all 
#export SPICE_DEBUG=1

all: ${PROGRAMS}

spiceterm: ${SOURCES} ${HEADERS} spiceterm.c 
	gcc -Werror -Wall -Wtype-limits ${SOURCES} -g -O2 -o $@ -lutil $(shell pkg-config --cflags gdk-3.0) $(shell pkg-config --cflags --libs gthread-2.0,spice-protocol,spice-server,gdk-3.0)

keysyms.h: genkeysym.pl
	./genkeysym.pl >$@

.PHONY: test
test: spiceterm
	./spiceterm --noauth --keymap de & remote-viewer spice://localhost?tls-port=5900
	#G_MESSAGES_DEBUG=all SPICE_DEBUG=1 SPICE_TICKET=test ./spiceterm & G_MESSAGES_DEBUG=all SPICE_DEBUG=1 remote-viewer --debug 'spice://localhost?tls-port=5900' --spice-ca-file /etc/pve/pve-root-ca.pem --spice-secure-channels=all

.PHONY: distclean
distclean: clean

.PHONY: clean
clean:
	rm -rf *~ ${PROGRAMS}