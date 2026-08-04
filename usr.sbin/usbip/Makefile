# Portable Makefile: works with both GNU make (Linux dev box) and
# bmake (FreeBSD).  Kept intentionally explicit.

PROG=	usbip
CC?=	cc
CFLAGS+=	-Wall -Wextra -O2 -g
CFLAGS+=	-I../../sys/dev/vhci -Ilibusbip

OBJS=	usbip.o libusbip/usbip_net.o

all: $(PROG)

$(PROG): $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $(PROG) $(OBJS)

usbip.o: usbip.c libusbip/usbip_net.h ../../sys/dev/vhci/usbip_proto.h
	$(CC) $(CFLAGS) -c -o usbip.o usbip.c

libusbip/usbip_net.o: libusbip/usbip_net.c libusbip/usbip_net.h \
		../../sys/dev/vhci/usbip_proto.h
	$(CC) $(CFLAGS) -c -o libusbip/usbip_net.o libusbip/usbip_net.c

# The FreeBSD-only code path is skipped by the compiler elsewhere, so
# type-check it explicitly to keep it from rotting.
check:
	$(CC) $(CFLAGS) -DVHCI_SUPPORTED=1 -fsyntax-only usbip.c

BINDIR?=	/usr/local/sbin
MANDIR?=	/usr/local/share/man/man8

install: $(PROG)
	install -d $(DESTDIR)$(BINDIR) $(DESTDIR)$(MANDIR)
	install -m 555 $(PROG) $(DESTDIR)$(BINDIR)/
	install -m 444 usbip.8 $(DESTDIR)$(MANDIR)/

clean:
	rm -f $(PROG) $(OBJS)

.PHONY: all check install clean
