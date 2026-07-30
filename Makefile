# Top-level driver Makefile (GNU make / bmake).
#
# "make" builds the userland tool and tests (any OS).
# "make kmod" builds the vhci(4) kernel module (FreeBSD only).

all:
	$(MAKE) -C usr.sbin/usbip
	$(MAKE) -C tests

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

clean:
	$(MAKE) -C usr.sbin/usbip clean
	$(MAKE) -C tests clean

.PHONY: all check kmod clean
