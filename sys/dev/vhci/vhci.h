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
#include <sys/condvar.h>
#include <sys/queue.h>
#include <sys/_task.h>

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

/*
 * Size of the per-port socket staging buffers.  Transfer payloads are
 * moved through them in chunks, so this bounds memory use rather than
 * transfer size.  Keep it at or below the socket buffer high-water mark
 * so a MSG_WAITALL receive of a full chunk cannot deadlock.
 */
#define	VHCI_STAGE_SIZE		(64 * 1024)

struct vhci_softc;

/*
 * Per-transfer USB/IP state.  Allocated out of the transfer's own
 * memory in xfer_setup, so submitting never has to allocate.  All
 * fields are protected by the USB bus lock.
 */
enum vhci_urb_state {
	VHCI_URB_IDLE = 0,	/* not in use */
	VHCI_URB_TX,		/* queued for, or being, transmitted */
	VHCI_URB_INFLIGHT,	/* sent, awaiting USBIP_RET_SUBMIT */
};

struct vhci_urb {
	TAILQ_ENTRY(vhci_urb) entry;
	struct usb_xfer	*xfer;
	uint32_t	seqnum;		/* of the CMD_SUBMIT */
	uint8_t		state;		/* enum vhci_urb_state */
	uint8_t		queued;		/* on port->tx_queue */
	/*
	 * Set while a thread is using this transfer with the bus lock
	 * dropped.  Completion is deferred to that thread, because the
	 * transfer's memory - which holds this struct - may be freed as
	 * soon as usbd_transfer_done() runs.
	 */
	uint8_t		busy;
	uint8_t		aborted;	/* deferred completion pending */
};

TAILQ_HEAD(vhci_urb_list, vhci_urb);

/*
 * A cancelled submission.  Kept separate from struct vhci_urb because
 * the transfer being cancelled is completed immediately and its memory,
 * which holds the vhci_urb, may be gone before the unlink is even sent.
 *
 * The record also outlives the unlink itself: a server that had already
 * finished the transfer answers with a USBIP_RET_SUBMIT for the
 * original sequence number, and because the reply header carries no
 * usable direction we would not otherwise know whether a payload
 * follows it.
 */
#define	VHCI_UNLINK_MAX		32

struct vhci_unlink {
	uint32_t	seqnum;		/* of the CMD_UNLINK */
	uint32_t	target;		/* of the CMD_SUBMIT being cancelled */
	uint8_t		valid;
	uint8_t		pending;	/* CMD_UNLINK not sent yet */
	uint8_t		is_in;		/* a late reply carries a payload */
};

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

	/*
	 * Transport threads and their queues.  Everything here is
	 * protected by the USB bus lock; the threads drop it around
	 * socket I/O and re-validate afterwards.
	 */
	struct cv	tx_cv;		/* work for the TX thread */
	struct cv	exit_cv;	/* thread has finished */
	struct vhci_urb_list tx_queue;	/* waiting to be transmitted */
	struct vhci_urb_list inflight;	/* awaiting a reply */
	struct vhci_unlink unlink[VHCI_UNLINK_MAX];
	uint8_t		unlink_pending;	/* how many need transmitting */
	uint32_t	next_seqnum;
	uint8_t		*tx_buf;	/* TX thread only */
	uint8_t		*rx_buf;	/* RX thread only */
	uint8_t		tx_running;
	uint8_t		rx_running;
	uint8_t		disconnecting;

	/*
	 * Set when a transport thread gives up on the connection.  The
	 * threads cannot tear their own port down - releasing it waits
	 * for them to exit - so they hand that to a task instead.
	 */
	uint8_t		dead;
	struct task	death_task;
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

/* vhci_hcd.c */
extern const struct usb_pipe_methods vhci_pipe_methods;

int		vhci_session_start(struct vhci_port *);
void		vhci_session_stop(struct vhci_port *);
void		vhci_session_died(struct vhci_port *);

#endif /* !_VHCI_H_ */
