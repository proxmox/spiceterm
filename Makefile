include /usr/share/dpkg/pkg-info.mk
include /usr/share/dpkg/architecture.mk

PACKAGE=spiceterm

GITVERSION:=$(shell cat .git/refs/heads/master)
BUILDDIR ?= $(PACKAGE)-$(DEB_VERSION_UPSTREAM)

export VERSION=$(DEB_VERSION_UPSTREAM)

DEB=$(PACKAGE)_$(DEB_VERSION_UPSTREAM_REVISION)_$(DEB_BUILD_ARCH).deb
DBG_DEB=$(PACKAGE)-dbgsym_$(DEB_VERSION_UPSTREAM_REVISION)_$(DEB_BUILD_ARCH).deb
DSC=$(PACKAGE)_$(DEB_VERSION_UPSTREAM_REVISION).dsc

$(BUILDDIR): src/ debian/
	rm -rf $(BUILDDIR)
	rsync -a src/ debian $(BUILDDIR)
	echo "git clone git://git.proxmox.com/git/spiceterm.git\\ngit checkout $(GITVERSION)" > $(BUILDDIR)/debian/SOURCE

.PHONY: dsc
dsc: $(DSC)
$(DSC): $(BUILDDIR)
	cd $(BUILDDIR); dpkg-buildpackage -S -us -uc -d
	lintian $(DSC)

.PHONY: deb
deb: $(DEB)
$(DEB): $(BUILDDIR)
	cd $(BUILDDIR); dpkg-buildpackage -b -us -uc
	lintian $(DEB)

.PHONY: dinstall
dinstall: $(DEB)
	dpkg -i $(DEB)

.PHONY: upload
upload: $(DEB)
	tar cf - $(DEB) $(DBG_DEB) | ssh repoman@repo.proxmox.com -- upload --product pve --dist bullseye --arch $(ARCH)

.PHONY: distclean clean
distclean: clean
clean:
	rm -rf *~ $(PACKAGE)-*/ *.deb *.changes genfont *.buildinfo *.dsc *.tar.gz
