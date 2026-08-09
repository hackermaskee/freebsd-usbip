/* SPDX-License-Identifier: BSD-2-Clause */
/*-
 * Copyright (c) 2026 furuta@furuta.bsdclub.org
 *
 * Local USB devices, as USB/IP describes them.
 *
 * The protocol's device record was designed around Linux's sysfs, so
 * the fields have to be filled from what libusb can tell us here.  The
 * bus id is the one part a user types, so it is the name FreeBSD
 * already uses for the device - ugen0.2, as printed by usbconfig(8) -
 * rather than Linux's port path.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libusb.h>

#include "usbipd.h"

/*
 * FreeBSD names a USB device after its controller unit and its address
 * on that bus, which is what libusb reports too.
 */
void
usbipd_busid(libusb_device *dev, char *buf, size_t len)
{

	snprintf(buf, len, "ugen%u.%u", libusb_get_bus_number(dev),
	    libusb_get_device_address(dev));
}

static uint32_t
usbipd_speed(libusb_device *dev)
{

	switch (libusb_get_device_speed(dev)) {
	case LIBUSB_SPEED_LOW:
		return (USBIP_SPEED_LOW);
	case LIBUSB_SPEED_FULL:
		return (USBIP_SPEED_FULL);
	case LIBUSB_SPEED_HIGH:
		return (USBIP_SPEED_HIGH);
	case LIBUSB_SPEED_SUPER:
		return (USBIP_SPEED_SUPER);
	default:
		return (USBIP_SPEED_UNKNOWN);
	}
}

/*
 * Fill in the protocol's device record.  Interfaces are reported from
 * the first configuration, which is what a client sees before it has
 * chosen one.
 */
int
usbipd_describe(libusb_device *dev, struct usbip_usb_device *udev,
    struct usbip_usb_interface *intfs, uint8_t *nintfs)
{
	struct libusb_device_descriptor dd;
	struct libusb_config_descriptor *cfg = NULL;
	uint8_t i, n = 0;

	memset(udev, 0, sizeof(*udev));

	if (libusb_get_device_descriptor(dev, &dd) != 0)
		return (-1);

	usbipd_busid(dev, udev->busid, sizeof(udev->busid));
	/*
	 * The path is informational and Linux-shaped; give something
	 * truthful rather than inventing a sysfs layout we do not have.
	 */
	snprintf(udev->path, sizeof(udev->path), "/dev/%s", udev->busid);

	udev->busnum = libusb_get_bus_number(dev);
	udev->devnum = libusb_get_device_address(dev);
	udev->speed = usbipd_speed(dev);
	udev->idVendor = dd.idVendor;
	udev->idProduct = dd.idProduct;
	udev->bcdDevice = dd.bcdDevice;
	udev->bDeviceClass = dd.bDeviceClass;
	udev->bDeviceSubClass = dd.bDeviceSubClass;
	udev->bDeviceProtocol = dd.bDeviceProtocol;
	udev->bNumConfigurations = dd.bNumConfigurations;

	if (libusb_get_config_descriptor(dev, 0, &cfg) == 0 && cfg != NULL) {
		udev->bConfigurationValue = cfg->bConfigurationValue;
		n = cfg->bNumInterfaces;
		if (n > USBIP_MAX_INTERFACES)
			n = USBIP_MAX_INTERFACES;
		for (i = 0; i < n; i++) {
			const struct libusb_interface_descriptor *alt =
			    &cfg->interface[i].altsetting[0];

			intfs[i].bInterfaceClass = alt->bInterfaceClass;
			intfs[i].bInterfaceSubClass = alt->bInterfaceSubClass;
			intfs[i].bInterfaceProtocol = alt->bInterfaceProtocol;
			intfs[i].padding = 0;
		}
		libusb_free_config_descriptor(cfg);
	}
	udev->bNumInterfaces = n;
	*nintfs = n;

	return (0);
}

/*
 * Find a device by the bus id a client asked for.  Returns a reference
 * that the caller unrefs, or NULL.
 */
libusb_device *
usbipd_find(libusb_context *ctx, const char *busid)
{
	libusb_device **list, *found = NULL;
	char name[USBIP_BUSID_SIZE];
	ssize_t n, i;

	n = libusb_get_device_list(ctx, &list);
	if (n < 0)
		return (NULL);

	for (i = 0; i < n; i++) {
		usbipd_busid(list[i], name, sizeof(name));
		if (strcmp(name, busid) == 0) {
			found = libusb_ref_device(list[i]);
			break;
		}
	}

	libusb_free_device_list(list, 1);
	return (found);
}
