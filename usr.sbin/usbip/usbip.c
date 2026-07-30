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

/*
 * The kernel side exists only on FreeBSD.  Elsewhere the tool still
 * builds and "list" still works, which is what the protocol regression
 * tests and interop checks against Linux need.  Define VHCI_SUPPORTED=1
 * by hand to compile-test the FreeBSD path on another system.
 */
#ifndef VHCI_SUPPORTED
#ifdef __FreeBSD__
#define	VHCI_SUPPORTED	1
#else
#define	VHCI_SUPPORTED	0
#endif
#endif

#if VHCI_SUPPORTED
#include <sys/ioctl.h>
#include <fcntl.h>
#include "vhci_ioctl.h"
#endif

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

#if VHCI_SUPPORTED

static int
vhci_open(void)
{
	int vfd;

	vfd = open(VHCI_DEVICE_PATH, O_RDWR);
	if (vfd < 0) {
		fprintf(stderr, "%s: %s: %s\n", prog, VHCI_DEVICE_PATH,
		    strerror(errno));
		if (errno == ENOENT)
			fprintf(stderr,
			    "%s: is the vhci(4) module loaded?\n", prog);
	}
	return (vfd);
}

/*
 * Hand the imported connection to vhci(4).  The kernel takes ownership
 * of the socket, so our descriptor is dead afterwards either way.
 */
static int
vhci_attach_port(int fd, const struct usbip_usb_device *udev,
    const char *host)
{
	struct vhci_ioc_attach ia;
	int vfd;

	vfd = vhci_open();
	if (vfd < 0)
		return (-1);

	memset(&ia, 0, sizeof(ia));
	ia.fd = fd;
	ia.devid = USBIP_DEVID(udev->busnum, udev->devnum);
	ia.speed = udev->speed;
	ia.port = -1;			/* any free port */
	strncpy(ia.busid, udev->busid, sizeof(ia.busid) - 1);
	strncpy(ia.host, host, sizeof(ia.host) - 1);

	if (ioctl(vfd, VHCI_IOC_ATTACH, &ia) != 0) {
		fprintf(stderr, "%s: attach: %s\n", prog, strerror(errno));
		close(vfd);
		return (-1);
	}
	close(vfd);

	printf("attached to port %d\n", ia.port);
	return (0);
}

#else /* !VHCI_SUPPORTED */

static int
vhci_attach_port(int fd, const struct usbip_usb_device *udev,
    const char *host)
{

	(void)fd;
	(void)udev;
	(void)host;
	fprintf(stderr,
	    "%s: handshake succeeded, but vhci(4) exists only on FreeBSD\n",
	    prog);
	return (-1);
}

#endif /* VHCI_SUPPORTED */

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

	if (vhci_attach_port(fd, &udev, host) != 0) {
		close(fd);
		return (1);
	}
	/* The kernel owns the socket now. */
	return (0);
}

#if VHCI_SUPPORTED

static int
cmd_detach(int argc, char **argv)
{
	struct vhci_ioc_detach id;
	int ch, vfd, port = -1;

	while ((ch = getopt(argc, argv, "p:")) != -1) {
		switch (ch) {
		case 'p':
			port = atoi(optarg);
			break;
		default:
			usage();
		}
	}
	if (port < 0)
		usage();

	vfd = vhci_open();
	if (vfd < 0)
		return (1);

	memset(&id, 0, sizeof(id));
	id.port = port;
	if (ioctl(vfd, VHCI_IOC_DETACH, &id) != 0) {
		fprintf(stderr, "%s: detach port %d: %s\n", prog, port,
		    strerror(errno));
		close(vfd);
		return (1);
	}
	close(vfd);

	printf("detached port %d\n", port);
	return (0);
}

static int
cmd_port(int argc, char **argv)
{
	struct vhci_ioc_port_info pi;
	int vfd, i, found = 0;

	(void)argc;
	(void)argv;

	vfd = vhci_open();
	if (vfd < 0)
		return (1);

	for (i = 0; i < VHCI_PORT_COUNT; i++) {
		memset(&pi, 0, sizeof(pi));
		pi.port = i;
		if (ioctl(vfd, VHCI_IOC_PORT_INFO, &pi) != 0) {
			fprintf(stderr, "%s: port %d: %s\n", prog, i,
			    strerror(errno));
			close(vfd);
			return (1);
		}
		if (!pi.occupied)
			continue;
		printf("port %d: %s from %s (%s), devid %#x\n", i, pi.busid,
		    pi.host, usbip_speed_string(pi.speed), pi.devid);
		found++;
	}
	close(vfd);

	if (found == 0)
		printf("no imported devices\n");
	return (0);
}

#else /* !VHCI_SUPPORTED */

static int
cmd_detach(int argc, char **argv)
{

	(void)argc;
	(void)argv;
	fprintf(stderr, "%s: detach: vhci(4) exists only on FreeBSD\n", prog);
	return (1);
}

static int
cmd_port(int argc, char **argv)
{

	(void)argc;
	(void)argv;
	fprintf(stderr, "%s: port: vhci(4) exists only on FreeBSD\n", prog);
	return (1);
}

#endif /* VHCI_SUPPORTED */

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
