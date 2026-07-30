/* SPDX-License-Identifier: BSD-2-Clause */
/*-
 * Copyright (c) 2026 furuta@furuta.bsdclub.org
 *
 * usbip(8) - USB/IP client management tool for FreeBSD.
 *
 * Subcommands:
 *   list -r <host>            list devices exportable from a remote host
 *   attach -r <host> -b <id>  attach a remote device (needs vhci(4))
 *   detach -p <port>          detach an imported device
 *   port                      list imported devices
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "libusbip/usbip_net.h"

static const char *prog = "usbip";

static void
usage(void)
{

	fprintf(stderr,
	    "usage: %s list -r <host> [-t <port>]\n"
	    "       %s attach -r <host> -b <busid> [-t <port>]\n"
	    "       %s detach -p <port>\n"
	    "       %s port\n",
	    prog, prog, prog, prog);
	exit(1);
}

static void
print_device(const struct usbip_devlist_entry *e)
{
	const struct usbip_usb_device *d = &e->udev;
	uint32_t i;

	printf("  %-14s %04x:%04x  %s\n", d->busid, d->idVendor,
	    d->idProduct, usbip_speed_string(d->speed));
	printf("%17s class %02x/%02x/%02x  conf %u  ifaces %u\n", "",
	    d->bDeviceClass, d->bDeviceSubClass, d->bDeviceProtocol,
	    d->bConfigurationValue, d->bNumInterfaces);
	for (i = 0; i < d->bNumInterfaces && e->intfs != NULL; i++) {
		printf("%17s if%u  class %02x/%02x/%02x\n", "", i,
		    e->intfs[i].bInterfaceClass,
		    e->intfs[i].bInterfaceSubClass,
		    e->intfs[i].bInterfaceProtocol);
	}
}

static int
cmd_list(int argc, char **argv)
{
	struct usbip_devlist_entry *entries;
	const char *host = NULL, *service = NULL;
	uint32_t count, i;
	int ch, fd, error;

	while ((ch = getopt(argc, argv, "r:t:")) != -1) {
		switch (ch) {
		case 'r':
			host = optarg;
			break;
		case 't':
			service = optarg;
			break;
		default:
			usage();
		}
	}
	if (host == NULL)
		usage();

	fd = usbip_net_connect(host, service);
	if (fd < 0)
		return (1);
	error = usbip_devlist_fetch(fd, &entries, &count);
	close(fd);
	if (error != 0)
		return (1);

	printf("Exportable USB devices on %s:\n", host);
	if (count == 0)
		printf("  (none)\n");
	for (i = 0; i < count; i++)
		print_device(&entries[i]);
	usbip_devlist_free(entries, count);
	return (0);
}

/*
 * Hand the imported connection over to the vhci(4) kernel driver.
 * Implemented in M1; keep the handshake testable everywhere until then.
 */
static int
vhci_attach_port(int fd, const struct usbip_usb_device *udev)
{

	(void)fd;
	(void)udev;
	fprintf(stderr,
	    "%s: handshake OK, but vhci(4) hand-off is not implemented yet\n",
	    prog);
	return (-1);
}

static int
cmd_attach(int argc, char **argv)
{
	struct usbip_usb_device udev;
	const char *host = NULL, *service = NULL, *busid = NULL;
	int ch, fd;

	while ((ch = getopt(argc, argv, "r:b:t:")) != -1) {
		switch (ch) {
		case 'r':
			host = optarg;
			break;
		case 'b':
			busid = optarg;
			break;
		case 't':
			service = optarg;
			break;
		default:
			usage();
		}
	}
	if (host == NULL || busid == NULL)
		usage();

	fd = usbip_net_connect(host, service);
	if (fd < 0)
		return (1);
	if (usbip_import_device(fd, busid, &udev) != 0) {
		close(fd);
		return (1);
	}

	printf("imported %s: %04x:%04x (%s), devid %#x\n", udev.busid,
	    udev.idVendor, udev.idProduct, usbip_speed_string(udev.speed),
	    USBIP_DEVID(udev.busnum, udev.devnum));

	if (vhci_attach_port(fd, &udev) != 0) {
		close(fd);
		return (1);
	}
	return (0);
}

static int
cmd_detach(int argc, char **argv)
{

	(void)argc;
	(void)argv;
	fprintf(stderr, "%s: detach: not implemented yet (needs vhci(4))\n",
	    prog);
	return (1);
}

static int
cmd_port(int argc, char **argv)
{

	(void)argc;
	(void)argv;
	fprintf(stderr, "%s: port: not implemented yet (needs vhci(4))\n",
	    prog);
	return (1);
}

int
main(int argc, char **argv)
{
	const char *cmd;

	if (argc < 2)
		usage();
	cmd = argv[1];
	argc--;
	argv++;
	optind = 1;

	if (strcmp(cmd, "list") == 0)
		return (cmd_list(argc, argv));
	if (strcmp(cmd, "attach") == 0)
		return (cmd_attach(argc, argv));
	if (strcmp(cmd, "detach") == 0)
		return (cmd_detach(argc, argv));
	if (strcmp(cmd, "port") == 0)
		return (cmd_port(argc, argv));
	usage();
	return (1);
}
