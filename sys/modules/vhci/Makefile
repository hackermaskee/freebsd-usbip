# vhci(4) kernel module.  FreeBSD only; build on the target machine.
#
#	make            build vhci.ko
#	make load       kldload it
#	make unload     kldunload it

.PATH:	${.CURDIR}/../../dev/vhci

KMOD=	vhci
MAN=	vhci.4
SRCS=	vhci.c vhci_roothub.c vhci_hcd.c
SRCS+=	bus_if.h device_if.h usb_if.h
SRCS+=	opt_bus.h opt_usb.h

CFLAGS+=	-I${.CURDIR}/../../dev/vhci

.include <bsd.kmod.mk>
