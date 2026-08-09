# Portable Makefile (GNU make / bmake).  FreeBSD only in practice: it
# needs libusb and exports devices through ugen(4).

PROG=	usbipd
CC?=	cc
CFLAGS+=	-Wall -Wextra -O2 -g
CFLAGS+=	-I../../sys/dev/vhci -I../usbip/libusbip
LDFLAGS+=	-lusb -lpthread -lutil

LIBOBJ=	../usbip/libusbip/usbip_net.o
OBJS=	usbipd.o device.o urb.o $(LIBOBJ)

all: $(PROG)

$(PROG): $(OBJS)
	$(CC) $(CFLAGS) -o $(PROG) $(OBJS) $(LDFLAGS)

usbipd.o: usbipd.c usbipd.h ../usbip/libusbip/usbip_net.h
	$(CC) $(CFLAGS) -c -o usbipd.o usbipd.c

device.o: device.c usbipd.h
	$(CC) $(CFLAGS) -c -o device.o device.c

urb.o: urb.c usbipd.h
	$(CC) $(CFLAGS) -c -o urb.o urb.c

$(LIBOBJ):
	$(MAKE) -C ../usbip libusbip/usbip_net.o

BINDIR?=	/usr/local/sbin
MANDIR?=	/usr/local/share/man/man8

install: $(PROG)
	install -d $(DESTDIR)$(BINDIR) $(DESTDIR)$(MANDIR)
	install -m 555 $(PROG) $(DESTDIR)$(BINDIR)/
	install -m 444 usbipd.8 $(DESTDIR)$(MANDIR)/

clean:
	rm -f $(PROG) usbipd.o device.o urb.o

.PHONY: all install clean
