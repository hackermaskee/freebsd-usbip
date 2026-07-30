/* SPDX-License-Identifier: BSD-2-Clause */
/*-
 * Copyright (c) 2026 furuta@furuta.bsdclub.org
 *
 * libusbip: USB/IP handshake-phase client library.
 */

#include <sys/types.h>
#include <sys/socket.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "usbip_net.h"

int
usbip_net_connect(const char *host, const char *service)
{
	struct addrinfo hints, *res, *ai;
	int fd, error, on;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;

	if (service == NULL)
		service = USBIP_PORT_STRING;

	error = getaddrinfo(host, service, &hints, &res);
	if (error != 0) {
		fprintf(stderr, "usbip: %s: %s\n", host, gai_strerror(error));
		return (-1);
	}

	fd = -1;
	for (ai = res; ai != NULL; ai = ai->ai_next) {
		fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
		if (fd < 0)
			continue;
		if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0)
			break;
		close(fd);
		fd = -1;
	}
	freeaddrinfo(res);

	if (fd < 0) {
		fprintf(stderr, "usbip: cannot connect to %s port %s: %s\n",
		    host, service, strerror(errno));
		return (-1);
	}

	on = 1;
	(void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
	(void)setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof(on));

	return (fd);
}

int
usbip_net_recv_exact(int fd, void *buf, size_t len)
{
	char *p = buf;
	ssize_t n;

	while (len > 0) {
		n = read(fd, p, len);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			fprintf(stderr, "usbip: recv: %s\n", strerror(errno));
			return (-1);
		}
		if (n == 0) {
			fprintf(stderr, "usbip: connection closed by peer\n");
			return (-1);
		}
		p += n;
		len -= (size_t)n;
	}
	return (0);
}

int
usbip_net_send_all(int fd, const void *buf, size_t len)
{
	const char *p = buf;
	ssize_t n;

	while (len > 0) {
		n = write(fd, p, len);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			fprintf(stderr, "usbip: send: %s\n", strerror(errno));
			return (-1);
		}
		p += n;
		len -= (size_t)n;
	}
	return (0);
}

int
usbip_net_send_op_common(int fd, uint16_t code)
{
	struct usbip_op_common op;

	memset(&op, 0, sizeof(op));
	op.version = htons(USBIP_PROTO_VERSION);
	op.code = htons(code);
	op.status = htonl(USBIP_ST_OK);

	return (usbip_net_send_all(fd, &op, sizeof(op)));
}

int
usbip_net_recv_op_common(int fd, uint16_t expected_code, uint32_t *statusp)
{
	struct usbip_op_common op;
	uint16_t version, code;
	uint32_t status;

	if (usbip_net_recv_exact(fd, &op, sizeof(op)) != 0)
		return (-1);

	version = ntohs(op.version);
	code = ntohs(op.code);
	status = ntohl(op.status);
	if (statusp != NULL)
		*statusp = status;

	if (version != USBIP_PROTO_VERSION) {
		fprintf(stderr,
		    "usbip: protocol version mismatch: peer 0x%04x, ours 0x%04x\n",
		    version, USBIP_PROTO_VERSION);
		return (-1);
	}
	if (code != expected_code) {
		fprintf(stderr,
		    "usbip: unexpected reply code 0x%04x (expected 0x%04x)\n",
		    code, expected_code);
		return (-1);
	}
	if (status != USBIP_ST_OK) {
		fprintf(stderr, "usbip: request failed on server, status %u\n",
		    status);
		return (-1);
	}
	return (0);
}

void
usbip_usb_device_ntoh(struct usbip_usb_device *udev)
{

	udev->busnum = ntohl(udev->busnum);
	udev->devnum = ntohl(udev->devnum);
	udev->speed = ntohl(udev->speed);
	udev->idVendor = ntohs(udev->idVendor);
	udev->idProduct = ntohs(udev->idProduct);
	udev->bcdDevice = ntohs(udev->bcdDevice);
	/* Force NUL termination on strings received from the network. */
	udev->path[USBIP_DEV_PATH_SIZE - 1] = '\0';
	udev->busid[USBIP_BUSID_SIZE - 1] = '\0';
}

int
usbip_devlist_fetch(int fd, struct usbip_devlist_entry **entriesp,
    uint32_t *countp)
{
	struct usbip_devlist_entry *entries;
	uint32_t count, i, j;

	*entriesp = NULL;
	*countp = 0;

	if (usbip_net_send_op_common(fd, OP_REQ_DEVLIST) != 0)
		return (-1);
	if (usbip_net_recv_op_common(fd, OP_REP_DEVLIST, NULL) != 0)
		return (-1);
	if (usbip_net_recv_exact(fd, &count, sizeof(count)) != 0)
		return (-1);
	count = ntohl(count);
	if (count > USBIP_MAX_DEVLIST_DEVICES) {
		fprintf(stderr, "usbip: absurd device count %u from server\n",
		    count);
		return (-1);
	}
	if (count == 0)
		return (0);

	entries = calloc(count, sizeof(*entries));
	if (entries == NULL) {
		fprintf(stderr, "usbip: out of memory\n");
		return (-1);
	}

	for (i = 0; i < count; i++) {
		struct usbip_devlist_entry *e = &entries[i];

		if (usbip_net_recv_exact(fd, &e->udev,
		    sizeof(e->udev)) != 0)
			goto fail;
		usbip_usb_device_ntoh(&e->udev);

		if (e->udev.bNumInterfaces > USBIP_MAX_INTERFACES) {
			fprintf(stderr,
			    "usbip: absurd interface count %u from server\n",
			    e->udev.bNumInterfaces);
			goto fail;
		}
		if (e->udev.bNumInterfaces == 0)
			continue;
		e->intfs = calloc(e->udev.bNumInterfaces,
		    sizeof(*e->intfs));
		if (e->intfs == NULL) {
			fprintf(stderr, "usbip: out of memory\n");
			goto fail;
		}
		for (j = 0; j < e->udev.bNumInterfaces; j++) {
			if (usbip_net_recv_exact(fd, &e->intfs[j],
			    sizeof(e->intfs[j])) != 0)
				goto fail;
		}
	}

	*entriesp = entries;
	*countp = count;
	return (0);

fail:
	usbip_devlist_free(entries, count);
	return (-1);
}

void
usbip_devlist_free(struct usbip_devlist_entry *entries, uint32_t count)
{
	uint32_t i;

	if (entries == NULL)
		return;
	for (i = 0; i < count; i++)
		free(entries[i].intfs);
	free(entries);
}

int
usbip_import_device(int fd, const char *busid,
    struct usbip_usb_device *udevp)
{
	char busid_buf[USBIP_BUSID_SIZE];

	if (strlen(busid) >= sizeof(busid_buf)) {
		fprintf(stderr, "usbip: busid too long: %s\n", busid);
		return (-1);
	}
	memset(busid_buf, 0, sizeof(busid_buf));
	strncpy(busid_buf, busid, sizeof(busid_buf) - 1);

	if (usbip_net_send_op_common(fd, OP_REQ_IMPORT) != 0)
		return (-1);
	if (usbip_net_send_all(fd, busid_buf, sizeof(busid_buf)) != 0)
		return (-1);
	if (usbip_net_recv_op_common(fd, OP_REP_IMPORT, NULL) != 0)
		return (-1);
	if (usbip_net_recv_exact(fd, udevp, sizeof(*udevp)) != 0)
		return (-1);
	usbip_usb_device_ntoh(udevp);

	if (strncmp(udevp->busid, busid, USBIP_BUSID_SIZE) != 0) {
		fprintf(stderr,
		    "usbip: server imported wrong device: asked %s, got %s\n",
		    busid, udevp->busid);
		return (-1);
	}
	return (0);
}

const char *
usbip_speed_string(uint32_t speed)
{

	switch (speed) {
	case USBIP_SPEED_LOW:
		return ("Low Speed (1.5 Mbit/s)");
	case USBIP_SPEED_FULL:
		return ("Full Speed (12 Mbit/s)");
	case USBIP_SPEED_HIGH:
		return ("High Speed (480 Mbit/s)");
	case USBIP_SPEED_WIRELESS:
		return ("Wireless");
	case USBIP_SPEED_SUPER:
		return ("Super Speed (5 Gbit/s)");
	case USBIP_SPEED_SUPER_PLUS:
		return ("Super Speed+ (10 Gbit/s)");
	default:
		return ("Unknown Speed");
	}
}
