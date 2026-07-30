/* SPDX-License-Identifier: BSD-2-Clause */
/*-
 * Copyright (c) 2026 furuta@furuta.bsdclub.org
 *
 * vhci(4): virtual USB host controller for USB/IP.
 *
 * The controller has no hardware.  Its root hub is emulated in
 * software, and each occupied port forwards USB transfers to a remote
 * USB/IP server over a TCP socket handed to us by usbip(8).
 */

#ifndef _VHCI_H_
#define	_VHCI_H_

#include <sys/lock.h>
#include <sys/sx.h>

#include <dev/usb/usb.h>
#include <dev/usb/usbdi.h>
#include <dev/usb/usb_core.h>
#include <dev/usb/usb_bus.h>

#include "usbip_proto.h"
#include "vhci_ioctl.h"

#define	VHCI_NPORTS		VHCI_PORT_COUNT
#define	VHCI_MAX_DEVICES	(VHCI_NPORTS + 2)

/* Root hub interrupt-endpoint number; the descriptor is decorative. */
#define	VHCI_INTR_ENDPT		1

/*
 * Sanity limits on anything read from the network.  A hostile or buggy
 * server must not be able to make us allocate without bound.
 */
#define	VHCI_MAX_XFER_LEN	(1024 * 1024)
#define	VHCI_MAX_ISO_PACKETS	1024

struct vhci_softc;

/*
 * One root-hub port.  Fields are protected by the USB bus lock unless
 * noted; roothub_exec runs with that lock held.
 */
struct vhci_port {
	struct vhci_softc *sc;
	uint8_t		index;		/* 0-based; wIndex is index + 1 */

	/* Root hub port state. */
	uint8_t		connected;
	uint8_t		enabled;
	uint8_t		powered;
	uint8_t		suspended;
	uint16_t	speed_bits;	/* UPS_HIGH_SPEED, UPS_LOW_SPEED, 0 */

	/* Sticky change bits, cleared only by CLEAR_FEATURE(UHF_C_*). */
	uint8_t		change_connect;
	uint8_t		change_enable;
	uint8_t		change_suspend;
	uint8_t		change_reset;

	/* USB/IP session state, valid while connected. */
	struct socket	*so;
	uint32_t	devid;
	uint32_t	usbip_speed;	/* USBIP_SPEED_* as imported */
	char		busid[USBIP_BUSID_SIZE];
	char		host[64];
};

union vhci_hub_temp {
	uWord			wValue;
	struct usb_status	stat;
	struct usb_port_status	ps;
	struct usb_hub_descriptor hubd;
	uint8_t			temp[128];
};

struct vhci_softc {
	struct usb_bus	sc_bus;		/* must be first */
	struct usb_device *sc_devices[VHCI_MAX_DEVICES];

	device_t	sc_dev;
	struct cdev	*sc_cdev;

	/*
	 * Serializes port attach and detach.  Those need to sleep
	 * (socket teardown, thread rendezvous), which the bus mutex
	 * does not allow, so port ownership is decided under this lock
	 * and only the root hub state is touched under the bus lock.
	 */
	struct sx	sc_sx;

	struct vhci_port sc_port[VHCI_NPORTS];

	/* Root hub emulation scratch, used under the bus lock. */
	union vhci_hub_temp sc_hub_temp;
	uint8_t		sc_hub_idata[(VHCI_NPORTS + 1 + 7) / 8];
	uint8_t		sc_rt_addr;	/* root hub USB address */
	uint8_t		sc_conf;	/* root hub configuration value */
};

#define	VHCI_BUS2SC(bus) __containerof(bus, struct vhci_softc, sc_bus)

/* vhci_roothub.c */
usb_error_t	vhci_roothub_exec(struct usb_device *,
		    struct usb_device_request *, const void **, uint16_t *);
void		vhci_root_intr(struct vhci_softc *, uint8_t port_index);

#endif /* !_VHCI_H_ */
