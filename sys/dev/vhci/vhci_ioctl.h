/* SPDX-License-Identifier: BSD-2-Clause */
/*-
 * Copyright (c) 2026 furuta@furuta.bsdclub.org
 *
 * vhci(4) /dev/vhci ioctl interface, shared with usbip(8).
 *
 * usbip(8) performs the USB/IP handshake (OP_REQ_IMPORT) in userland
 * and then hands the connected TCP socket to the kernel with
 * VHCI_IOC_ATTACH; from that point the driver owns the socket and
 * runs the URB phase (CMD_SUBMIT/RET_SUBMIT/...) from kernel threads.
 */

#ifndef _VHCI_IOCTL_H_
#define	_VHCI_IOCTL_H_

#if defined(_KERNEL) || defined(__FreeBSD__)
#include <sys/ioccom.h>
#else
#include <sys/ioctl.h>		/* only so the header can be compile-tested */
#endif

#include "usbip_proto.h"

#define	VHCI_DEVICE_PATH	"/dev/vhci"

/* Number of root-hub ports on the virtual controller. */
#define	VHCI_PORT_COUNT		8

struct vhci_ioc_attach {
	int		fd;		/* in: connected TCP socket */
	uint32_t	devid;		/* in: busnum << 16 | devnum */
	uint32_t	speed;		/* in: USBIP_SPEED_* */
	int		port;		/* in: -1 = any free; out: chosen */
	char		busid[USBIP_BUSID_SIZE];	/* in: informational */
	char		host[64];	/* in: informational, for "port" cmd */
};

struct vhci_ioc_detach {
	int		port;
};

struct vhci_ioc_port_info {
	int		port;		/* in */
	int		occupied;	/* out */
	uint32_t	devid;		/* out */
	uint32_t	speed;		/* out */
	char		busid[USBIP_BUSID_SIZE];	/* out */
	char		host[64];	/* out */
};

#define	VHCI_IOC_ATTACH		_IOWR('V', 100, struct vhci_ioc_attach)
#define	VHCI_IOC_DETACH		_IOW('V', 101, struct vhci_ioc_detach)
#define	VHCI_IOC_PORT_INFO	_IOWR('V', 102, struct vhci_ioc_port_info)

#endif /* !_VHCI_IOCTL_H_ */
