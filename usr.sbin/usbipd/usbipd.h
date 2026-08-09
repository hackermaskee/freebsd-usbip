/* SPDX-License-Identifier: BSD-2-Clause */
/*-
 * Copyright (c) 2026 furuta@furuta.bsdclub.org
 *
 * usbipd(8): export local USB devices over USB/IP.
 */

#ifndef _USBIPD_H_
#define	_USBIPD_H_

#include <stdint.h>
#include <stdbool.h>

#include <libusb.h>

#include "usbip_net.h"
#include "usbip_proto.h"

/*
 * A device must be named on the command line to be exported.  Handing
 * out whatever happens to be plugged in would be a poor default: the
 * client takes the device over, so exporting a keyboard by accident
 * costs you the keyboard.
 */
#define	USBIPD_MAX_EXPORTS	32

/* Bounds on anything a client can ask for. */
#define	USBIPD_MAX_XFER_LEN	(1024 * 1024)
#define	USBIPD_MAX_ISO_PACKETS	1024
/* How many transfers a client may have outstanding at once. */
#define	USBIPD_MAX_INFLIGHT	64

struct usbipd {
	libusb_context	*ctx;
	const char	*exports[USBIPD_MAX_EXPORTS];
	int		nexports;
	int		verbose;
	bool		exportable_is_open;	/* -a: export everything */
};

/* device.c */
void	usbipd_busid(libusb_device *dev, char *buf, size_t len);
int	usbipd_describe(libusb_device *dev, struct usbip_usb_device *udev,
	    struct usbip_usb_interface *intfs, uint8_t *nintfs);
libusb_device *usbipd_find(libusb_context *ctx, const char *busid);

/* urb.c: serve the transfer phase until the client goes away. */
int	usbipd_serve_urbs(struct usbipd *d, int fd,
	    libusb_device_handle *dh);

/* usbipd.c */
void	usbipd_log(const struct usbipd *d, const char *fmt, ...)
	    __printflike(2, 3);
bool	usbipd_is_exported(const struct usbipd *d, const char *busid);

#endif /* !_USBIPD_H_ */
