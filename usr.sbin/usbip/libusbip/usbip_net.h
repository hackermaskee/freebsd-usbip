/* SPDX-License-Identifier: BSD-2-Clause */
/*-
 * Copyright (c) 2026 furuta@furuta.bsdclub.org
 *
 * libusbip: USB/IP handshake-phase protocol library, used by both
 * usbip(8), which imports devices, and usbipd(8), which exports them.
 */

#ifndef _USBIP_NET_H_
#define	_USBIP_NET_H_

#include <stdint.h>
#include <stddef.h>

#include "usbip_proto.h"

/* Parse limits for data received from the network. */
#define	USBIP_MAX_DEVLIST_DEVICES	256
#define	USBIP_MAX_INTERFACES		32

struct usbip_devlist_entry {
	struct usbip_usb_device		udev;	/* host byte order */
	struct usbip_usb_interface	*intfs;	/* [udev.bNumInterfaces] */
};

/*
 * All functions return 0 on success and -1 on error unless noted.
 * On error a message has been printed to stderr.
 */

/* Connect to a remote usbipd.  Returns a socket fd, or -1. */
int	usbip_net_connect(const char *host, const char *service);

/*
 * Exact-length raw I/O; handle short reads/writes and EINTR.  The
 * _quiet forms leave errno set and say nothing, for callers that do
 * their own logging; a peer that closed reports ECONNRESET.
 */
int	usbip_net_recv_exact(int fd, void *buf, size_t len);
int	usbip_net_send_all(int fd, const void *buf, size_t len);
int	usbip_net_recv_exact_quiet(int fd, void *buf, size_t len);
int	usbip_net_send_all_quiet(int fd, const void *buf, size_t len);

/* Handshake header exchange. */
int	usbip_net_send_op_common(int fd, uint16_t code);
int	usbip_net_recv_op_common(int fd, uint16_t expected_code,
	    uint32_t *statusp);

/*
 * OP_REQ_DEVLIST: fetch the exportable-device list.  On success
 * *entriesp is a malloc'd array of *countp entries; free it with
 * usbip_devlist_free().  The connection is consumed (close the fd).
 */
int	usbip_devlist_fetch(int fd, struct usbip_devlist_entry **entriesp,
	    uint32_t *countp);
void	usbip_devlist_free(struct usbip_devlist_entry *entries,
	    uint32_t count);

/*
 * OP_REQ_IMPORT: import busid.  On success fills *udevp (host byte
 * order) and the fd is ready to switch to the URB phase.
 */
int	usbip_import_device(int fd, const char *busid,
	    struct usbip_usb_device *udevp);

/*
 * Server side.
 *
 * These do not print to stderr: a daemon decides for itself how to log,
 * and a misbehaving client should not be able to fill anyone's terminal.
 */

/*
 * Read a request header.  Returns the request code, or -1 if the peer
 * spoke a version we do not, or the connection failed.  A version
 * mismatch is answered before returning so the client sees why.
 */
int	usbip_net_recv_op_request(int fd, uint16_t *codep);

/* Reply header.  Use a non-zero status to refuse the request. */
int	usbip_net_send_op_reply(int fd, uint16_t code, uint32_t status);

/* In-place conversion of the numeric fields, either direction. */
void	usbip_usb_device_ntoh(struct usbip_usb_device *udev);
void	usbip_usb_device_hton(struct usbip_usb_device *udev);

const char *usbip_speed_string(uint32_t speed);

#endif /* !_USBIP_NET_H_ */
