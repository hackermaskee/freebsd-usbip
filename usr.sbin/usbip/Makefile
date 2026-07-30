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

clean:
	rm -f $(PROG) $(OBJS)

.PHONY: all clean
