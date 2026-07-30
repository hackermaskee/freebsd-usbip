# Portable Makefile (GNU make / bmake).

CC?=	cc
CFLAGS+=	-Wall -Wextra -O2 -g
CFLAGS+=	-I../sys/dev/vhci -I../usr.sbin/usbip/libusbip

LIBOBJ=	../usr.sbin/usbip/libusbip/usbip_net.o

all: proto_test

check: proto_test
	./proto_test

proto_test: proto_test.o $(LIBOBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o proto_test proto_test.o $(LIBOBJ)

proto_test.o: proto_test.c ../usr.sbin/usbip/libusbip/usbip_net.h \
		../sys/dev/vhci/usbip_proto.h
	$(CC) $(CFLAGS) -c -o proto_test.o proto_test.c

$(LIBOBJ):
	$(MAKE) -C ../usr.sbin/usbip libusbip/usbip_net.o

clean:
	rm -f proto_test proto_test.o

.PHONY: all check clean
