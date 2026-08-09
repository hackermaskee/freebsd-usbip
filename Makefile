# Top-level driver Makefile (GNU make / bmake).
#
# "make" builds the userland tool and tests (any OS).
# "make kmod" builds the vhci(4) kernel module (FreeBSD only).

all:
	$(MAKE) -C usr.sbin/usbip
	$(MAKE) -C tests
	@# usbipd needs libusb, which is base on FreeBSD but a package
	@# elsewhere; skip it rather than fail a plain "make".
	@if [ -e /usr/include/libusb.h ] || \
	    [ -e /usr/local/include/libusb.h ]; then \
		$(MAKE) -C usr.sbin/usbipd; \
	else \
		echo "skipping usbipd: no libusb.h"; \
	fi

check: all
	$(MAKE) -C tests check
	$(MAKE) -C usr.sbin/usbip check
	@if [ -d $${FREEBSD_SRC:-$$HOME/work/freebsd-src}/sys ]; then \
		tools/syntax-check.sh; \
	else \
		echo "skipping kernel type-check: no FreeBSD source tree"; \
	fi

kmod:
	$(MAKE) -C sys/modules/vhci

# Install everything a FreeBSD system needs: the module where the
# loader looks, the tools and their manuals under PREFIX, and an rc
# script with PREFIX substituted in.  The port drives this.
PREFIX?=	/usr/local
KMODDIR?=	/boot/modules

install:
	$(MAKE) -C usr.sbin/usbip install BINDIR=$(PREFIX)/sbin \
	    MANDIR=$(PREFIX)/share/man/man8
	$(MAKE) -C usr.sbin/usbipd install BINDIR=$(PREFIX)/sbin \
	    MANDIR=$(PREFIX)/share/man/man8
	install -d $(DESTDIR)$(KMODDIR) $(DESTDIR)$(PREFIX)/share/man/man4
	install -m 555 sys/modules/vhci/vhci.ko $(DESTDIR)$(KMODDIR)/
	install -m 444 sys/dev/vhci/vhci.4 \
	    $(DESTDIR)$(PREFIX)/share/man/man4/
	@# The port installs the rc script itself, through USE_RC_SUBR,
	@# which also substitutes %%PREFIX%% and adds it to the package
	@# list.  Installing it here as well would put it there twice.
	@if [ -z "$(NO_RC_SCRIPT)" ]; then \
		install -d $(DESTDIR)$(PREFIX)/etc/rc.d; \
		sed 's,%%PREFIX%%,$(PREFIX),g' rc.d/usbipd \
		    > $(DESTDIR)$(PREFIX)/etc/rc.d/usbipd; \
		chmod 555 $(DESTDIR)$(PREFIX)/etc/rc.d/usbipd; \
		echo "installed $(PREFIX)/etc/rc.d/usbipd"; \
	fi

clean:
	$(MAKE) -C usr.sbin/usbip clean
	$(MAKE) -C usr.sbin/usbipd clean
	$(MAKE) -C tests clean

.PHONY: all check kmod install clean
