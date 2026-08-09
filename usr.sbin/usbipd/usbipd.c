/* SPDX-License-Identifier: BSD-2-Clause */
/*-
 * Copyright (c) 2026 furuta@furuta.bsdclub.org
 *
 * usbipd(8): export local USB devices over USB/IP.
 *
 * Runs entirely in userland on top of libusb, so no kernel support is
 * needed on this side; the device is claimed through ugen(4) like any
 * other libusb program.
 *
 * One client at a time per device, which is what the protocol assumes:
 * a device is imported by exactly one host.
 */

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/wait.h>

#include <netinet/in.h>
#include <netinet/tcp.h>

#include <err.h>
#include <errno.h>
#include <netdb.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "usbipd.h"

static volatile sig_atomic_t stop_requested;

static void
on_signal(int sig)
{

	(void)sig;
	stop_requested = 1;
}

void
usbipd_log(const struct usbipd *d, const char *fmt, ...)
{
	va_list ap;

	if (!d->verbose)
		return;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	fflush(stderr);
}

bool
usbipd_is_exported(const struct usbipd *d, const char *busid)
{
	int i;

	if (d->exportable_is_open)
		return (true);
	for (i = 0; i < d->nexports; i++) {
		if (strcmp(d->exports[i], busid) == 0)
			return (true);
	}
	return (false);
}

static void
usage(void)
{

	fprintf(stderr,
	    "usage: usbipd [-v] [-4|-6] [-t port] [-a] busid ...\n"
	    "       usbipd -l\n"
	    "\n"
	    "  -l   list local devices and exit\n"
	    "  -a   export every device, rather than only those named\n"
	    "  -t   TCP port to listen on (default %s)\n"
	    "  -v   report what happens\n",
	    USBIP_PORT_STRING);
	exit(1);
}

/*
 * List what could be exported.  The bus id in the first column is what
 * a client passes to "usbip attach -b".
 */
static int
list_local(struct usbipd *d)
{
	libusb_device **list;
	struct usbip_usb_device udev;
	struct usbip_usb_interface intfs[USBIP_MAX_INTERFACES];
	uint8_t nintfs, i;
	ssize_t n, k;

	n = libusb_get_device_list(d->ctx, &list);
	if (n < 0) {
		warnx("cannot list devices: %s", libusb_strerror((int)n));
		return (1);
	}

	printf("Local USB devices:\n");
	for (k = 0; k < n; k++) {
		if (usbipd_describe(list[k], &udev, intfs, &nintfs) != 0)
			continue;
		printf("  %-14s %04x:%04x  %s\n", udev.busid, udev.idVendor,
		    udev.idProduct, usbip_speed_string(udev.speed));
		for (i = 0; i < nintfs; i++) {
			printf("%17s if%u  class %02x/%02x/%02x\n", "", i,
			    intfs[i].bInterfaceClass,
			    intfs[i].bInterfaceSubClass,
			    intfs[i].bInterfaceProtocol);
		}
	}
	if (n == 0)
		printf("  (none)\n");

	libusb_free_device_list(list, 1);
	return (0);
}

/* OP_REQ_DEVLIST: every device this daemon is willing to hand out. */
static int
serve_devlist(struct usbipd *d, int fd)
{
	libusb_device **list;
	struct usbip_usb_device udev;
	struct usbip_usb_interface intfs[USBIP_MAX_INTERFACES];
	uint32_t count = 0, be;
	uint8_t nintfs;
	ssize_t n, k;
	int error = 0;

	n = libusb_get_device_list(d->ctx, &list);
	if (n < 0)
		return (usbip_net_send_op_reply(fd, OP_REP_DEVLIST,
		    USBIP_ST_NA));

	for (k = 0; k < n; k++) {
		char busid[USBIP_BUSID_SIZE];

		usbipd_busid(list[k], busid, sizeof(busid));
		if (usbipd_is_exported(d, busid))
			count++;
	}

	if (usbip_net_send_op_reply(fd, OP_REP_DEVLIST, USBIP_ST_OK) != 0)
		goto out;
	be = htonl(count);
	if (usbip_net_send_all_quiet(fd, &be, sizeof(be)) != 0)
		goto out;

	for (k = 0; k < n; k++) {
		char busid[USBIP_BUSID_SIZE];

		usbipd_busid(list[k], busid, sizeof(busid));
		if (!usbipd_is_exported(d, busid))
			continue;
		if (usbipd_describe(list[k], &udev, intfs, &nintfs) != 0)
			continue;

		usbip_usb_device_hton(&udev);
		if (usbip_net_send_all_quiet(fd, &udev, sizeof(udev)) != 0)
			goto out;
		if (nintfs != 0 && usbip_net_send_all_quiet(fd, intfs,
		    (size_t)nintfs * sizeof(intfs[0])) != 0)
			goto out;
	}
	error = 0;
out:
	libusb_free_device_list(list, 1);
	return (error);
}

/*
 * OP_REQ_IMPORT: hand a device over and then serve its transfers.  The
 * connection belongs to that device until the client goes away.
 */
static int
serve_import(struct usbipd *d, int fd)
{
	char busid[USBIP_BUSID_SIZE];
	struct usbip_usb_device udev;
	struct usbip_usb_interface intfs[USBIP_MAX_INTERFACES];
	libusb_device *dev;
	libusb_device_handle *dh;
	uint8_t nintfs;
	int error;

	if (usbip_net_recv_exact_quiet(fd, busid, sizeof(busid)) != 0)
		return (-1);
	busid[sizeof(busid) - 1] = '\0';

	if (!usbipd_is_exported(d, busid)) {
		usbipd_log(d, "refused %s: not exported", busid);
		return (usbip_net_send_op_reply(fd, OP_REP_IMPORT,
		    USBIP_ST_NA));
	}

	dev = usbipd_find(d->ctx, busid);
	if (dev == NULL) {
		usbipd_log(d, "refused %s: no such device", busid);
		return (usbip_net_send_op_reply(fd, OP_REP_IMPORT,
		    USBIP_ST_NA));
	}

	if (usbipd_describe(dev, &udev, intfs, &nintfs) != 0) {
		libusb_unref_device(dev);
		return (usbip_net_send_op_reply(fd, OP_REP_IMPORT,
		    USBIP_ST_NA));
	}

	error = libusb_open(dev, &dh);
	libusb_unref_device(dev);
	if (error != 0) {
		usbipd_log(d, "refused %s: %s", busid, libusb_strerror(error));
		return (usbip_net_send_op_reply(fd, OP_REP_IMPORT,
		    USBIP_ST_NA));
	}

	if (usbip_net_send_op_reply(fd, OP_REP_IMPORT, USBIP_ST_OK) != 0)
		goto done;
	usbip_usb_device_hton(&udev);
	if (usbip_net_send_all_quiet(fd, &udev, sizeof(udev)) != 0)
		goto done;

	usbipd_log(d, "exported %s", busid);
	usbipd_serve_urbs(d, fd, dh);
	usbipd_log(d, "released %s", busid);

done:
	libusb_close(dh);
	return (0);
}

static void
serve_client(struct usbipd *d, int fd)
{
	uint16_t code;
	int one = 1;

	(void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

	if (usbip_net_recv_op_request(fd, &code) != 0)
		return;

	switch (code) {
	case OP_REQ_DEVLIST:
		(void)serve_devlist(d, fd);
		break;
	case OP_REQ_IMPORT:
		(void)serve_import(d, fd);
		break;
	default:
		usbipd_log(d, "unexpected request %#06x", code);
		(void)usbip_net_send_op_reply(fd, code & 0x7FFF, USBIP_ST_NA);
		break;
	}
}

static int
listen_on(const char *service, int family)
{
	struct addrinfo hints, *res, *ai;
	int fd = -1, on = 1, error;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = family;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_PASSIVE;

	error = getaddrinfo(NULL, service, &hints, &res);
	if (error != 0)
		errx(1, "%s", gai_strerror(error));

	for (ai = res; ai != NULL; ai = ai->ai_next) {
		fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
		if (fd < 0)
			continue;
		(void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on,
		    sizeof(on));
		if (ai->ai_family == AF_INET6) {
			int off = 0;

			/* Accept IPv4 on the same socket where allowed. */
			(void)setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &off,
			    sizeof(off));
		}
		if (bind(fd, ai->ai_addr, ai->ai_addrlen) == 0 &&
		    listen(fd, 4) == 0)
			break;
		close(fd);
		fd = -1;
	}
	freeaddrinfo(res);

	if (fd < 0)
		err(1, "cannot listen on port %s", service);
	return (fd);
}

int
main(int argc, char **argv)
{
	struct usbipd d;
	const char *service = USBIP_PORT_STRING;
	int family = AF_UNSPEC;
	int ch, lfd, listing = 0, error, i;

	memset(&d, 0, sizeof(d));

	while ((ch = getopt(argc, argv, "46alt:v")) != -1) {
		switch (ch) {
		case '4':
			family = AF_INET;
			break;
		case '6':
			family = AF_INET6;
			break;
		case 'a':
			d.exportable_is_open = true;
			break;
		case 'l':
			listing = 1;
			break;
		case 't':
			service = optarg;
			break;
		case 'v':
			d.verbose = 1;
			break;
		default:
			usage();
		}
	}
	argc -= optind;
	argv += optind;

	for (i = 0; i < argc; i++) {
		if (d.nexports >= USBIPD_MAX_EXPORTS)
			errx(1, "too many devices; the limit is %d",
			    USBIPD_MAX_EXPORTS);
		d.exports[d.nexports++] = argv[i];
	}
	if (!listing && d.nexports == 0 && !d.exportable_is_open) {
		warnx("name at least one device to export, or use -a");
		usage();
	}

	error = libusb_init(&d.ctx);
	if (error != 0)
		errx(1, "libusb: %s", libusb_strerror(error));

	if (listing) {
		error = list_local(&d);
		libusb_exit(d.ctx);
		return (error);
	}

	/* A client that vanishes mid-write must not take us with it. */
	signal(SIGPIPE, SIG_IGN);
	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);

	lfd = listen_on(service, family);
	usbipd_log(&d, "listening on port %s", service);

	while (!stop_requested) {
		struct sockaddr_storage sa;
		socklen_t salen = sizeof(sa);
		char host[NI_MAXHOST];
		int fd;

		fd = accept(lfd, (struct sockaddr *)&sa, &salen);
		if (fd < 0) {
			if (errno == EINTR)
				continue;
			warn("accept");
			break;
		}
		if (getnameinfo((struct sockaddr *)&sa, salen, host,
		    sizeof(host), NULL, 0, NI_NUMERICHOST) != 0)
			strlcpy(host, "?", sizeof(host));
		usbipd_log(&d, "connection from %s", host);

		serve_client(&d, fd);
		close(fd);
	}

	close(lfd);
	libusb_exit(d.ctx);
	return (0);
}
