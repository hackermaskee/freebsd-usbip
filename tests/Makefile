# Portable Makefile (GNU make / bmake).

CC?=	cc
CFLAGS+=	-Wall -Wextra -O2 -g
CFLAGS+=	-I../sys/dev/vhci -I../usr.sbin/usbip/libusbip

LIBOBJ=	../usr.sbin/usbip/libusbip/usbip_net.o

all: proto_test

check: proto_test
	./proto_test

# Needs a device attached through vhci(4), so it is not part of "check".
# Built only where libusb is present; on FreeBSD that is the base system.
bulk_test: bulk_test.c
	$(CC) $(CFLAGS) -I/usr/include -o bulk_test bulk_test.c -lusb

# Also builds on Linux, where it is used to make the canonical usbip
# client emit isochronous traffic for capture: cc ... -lusb-1.0
iso_probe: iso_probe.c
	$(CC) $(CFLAGS) -o iso_probe iso_probe.c -lusb

proto_test: proto_test.o $(LIBOBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o proto_test proto_test.o $(LIBOBJ)

proto_test.o: proto_test.c ../usr.sbin/usbip/libusbip/usbip_net.h \
		../sys/dev/vhci/usbip_proto.h
	$(CC) $(CFLAGS) -c -o proto_test.o proto_test.c

$(LIBOBJ):
	$(MAKE) -C ../usr.sbin/usbip libusbip/usbip_net.o

clean:
	rm -f proto_test proto_test.o bulk_test iso_probe

.PHONY: all check clean
