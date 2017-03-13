RELEASE=4.4

PACKAGE=spiceterm
VERSION=2.0
PACKAGERELEASE=2

ARCH:=$(shell dpkg-architecture -qDEB_BUILD_ARCH)
GITVERSION:=$(shell cat .git/refs/heads/master)

DEB=${PACKAGE}_${VERSION}-${PACKAGERELEASE}_${ARCH}.deb

PROGRAMS=spiceterm

HEADERS=translations.h event_loop.h glyphs.h spiceterm.h keysyms.h
SOURCES=screen.c event_loop.c input.c spiceterm.c auth-pve.c

PKGS := glib-2.0 spice-protocol spice-server libsasl2
CFLAGS += `pkg-config --cflags $(PKGS)`
LIBS += `pkg-config --libs $(PKGS)`

#export G_MESSAGES_DEBUG=all 
#export SPICE_DEBUG=1

all: ${PROGRAMS}

spiceterm: ${SOURCES} ${HEADERS} spiceterm.c 
	gcc -Werror -Wall -Wl,-z,relro -Wtype-limits ${SOURCES} -g -O2 $(CFLAGS) -o $@ -lutil $(LIBS)

genfont: genfont.c
	gcc -g -O2 -o $@ genfont.c -Wall -D_GNU_SOURCE -lz

keysyms.h: genkeysym.pl
	./genkeysym.pl >$@

.PHONY: glyphs
glyphs: genfont
	./genfont > glyphs.h

spiceterm.1: spiceterm.pod
	rm -f $@
	pod2man -n $< -s 1 -r ${VERSION} <$< >$@

.PHONY: install
install: spiceterm spiceterm.1
	mkdir -p ${DESTDIR}/usr/share/doc/${PACKAGE}
	install -m 0644 copyright ${DESTDIR}/usr/share/doc/${PACKAGE}
	mkdir -p ${DESTDIR}/usr/share/man/man1
	install -m 0644 spiceterm.1 ${DESTDIR}/usr/share/man/man1
	mkdir -p ${DESTDIR}/usr/bin
	install -s -m 0755 spiceterm ${DESTDIR}/usr/bin

.PHONY: deb
deb: ${DEB}
${DEB}:
	make clean
	rsync -a . --exclude build build
	echo "git clone git://git.proxmox.com/git/spiceterm.git\\ngit checkout ${GITVERSION}" > build/debian/SOURCE
	cd build; dpkg-buildpackage -rfakeroot -b -us -uc
	lintian ${DEB}

.PHONY: dinstall
dinstall: ${DEB}
	dpkg -i ${DEB}


.PHONY: upload
upload: ${DEB}
	tar cf - ${DEB} | ssh repoman@repo.proxmox.com -- upload --product pve --dist stretch --arch ${ARCH}

.PHONY: test
test: spiceterm
	./spiceterm --noauth --keymap de & remote-viewer spice://localhost?tls-port=5900
	#G_MESSAGES_DEBUG=all SPICE_DEBUG=1 SPICE_TICKET=test ./spiceterm & G_MESSAGES_DEBUG=all SPICE_DEBUG=1 remote-viewer --debug 'spice://localhost?tls-port=5900' --spice-ca-file /etc/pve/pve-root-ca.pem --spice-secure-channels=all

.PHONY: distclean
distclean: clean

.PHONY: clean
clean:
	rm -rf *~ ${PROGRAMS} build *.deb *.changes genfont
