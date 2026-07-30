/* SPDX-License-Identifier: BSD-2-Clause */
/*-
 * Copyright (c) 2026 furuta@furuta.bsdclub.org
 *
 * vhci(4): USB/IP transfer engine.
 *
 * Each occupied root hub port owns a TCP session with two kernel
 * threads.  The TX thread turns USB transfers into USBIP_CMD_SUBMIT
 * messages; the RX thread matches USBIP_RET_SUBMIT replies back to
 * transfers by sequence number and completes them.
 *
 * Locking: the USB bus lock protects every field of struct vhci_urb and
 * the port queues, and must be held to call usbd_transfer_done().  It
 * may not be held across socket I/O, so the threads drop it around each
 * send and receive and re-validate the transfer afterwards.  The
 * per-port staging buffers are touched only by their owning thread, so
 * they need no lock.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/bus.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/condvar.h>
#include <sys/sx.h>
#include <sys/malloc.h>
#include <sys/endian.h>
#include <sys/kthread.h>
#include <sys/proc.h>
#include <sys/socket.h>
#include <sys/socketvar.h>
#include <sys/protosw.h>
#include <sys/uio.h>

#include <dev/usb/usb.h>
#include <dev/usb/usbdi.h>
#include <dev/usb/usb_core.h>
#include <dev/usb/usb_busdma.h>
#include <dev/usb/usb_process.h>
#include <dev/usb/usb_transfer.h>
#include <dev/usb/usb_device.h>
#include <dev/usb/usb_hub.h>
#include <dev/usb/usb_util.h>
#include <dev/usb/usb_controller.h>
#include <dev/usb/usb_bus.h>

#include "vhci.h"

static MALLOC_DEFINE(M_VHCI_BUF, "vhcibuf", "USB/IP transport buffers");

#define	VHCI_LOCK(sc)		USB_BUS_LOCK(&(sc)->sc_bus)
#define	VHCI_UNLOCK(sc)		USB_BUS_UNLOCK(&(sc)->sc_bus)
#define	VHCI_LOCK_ASSERT(sc)	USB_BUS_LOCK_ASSERT(&(sc)->sc_bus, MA_OWNED)

/*
 * Socket helpers.  Called with no locks held.
 */

static int
vhci_sock_send(struct socket *so, const void *buf, size_t len)
{
	struct uio uio;
	struct iovec iov;
	int error;

	while (len > 0) {
		iov.iov_base = __DECONST(void *, buf);
		iov.iov_len = len;
		memset(&uio, 0, sizeof(uio));
		uio.uio_iov = &iov;
		uio.uio_iovcnt = 1;
		uio.uio_rw = UIO_WRITE;
		uio.uio_segflg = UIO_SYSSPACE;
		uio.uio_td = curthread;
		uio.uio_resid = len;

		error = sosend(so, NULL, &uio, NULL, NULL, 0, curthread);
		if (error != 0)
			return (error);
		if (uio.uio_resid == len)
			return (EPIPE);		/* no progress */
		buf = (const char *)buf + (len - uio.uio_resid);
		len = uio.uio_resid;
	}
	return (0);
}

/*
 * Receive exactly len bytes.  Requests are capped at the staging size
 * so that MSG_WAITALL cannot ask for more than the socket buffer can
 * hold, which would return short.
 */
static int
vhci_sock_recv(struct socket *so, void *buf, size_t len)
{
	struct uio uio;
	struct iovec iov;
	size_t chunk;
	int error, flags;

	while (len > 0) {
		chunk = len;
		if (chunk > VHCI_STAGE_SIZE)
			chunk = VHCI_STAGE_SIZE;

		iov.iov_base = buf;
		iov.iov_len = chunk;
		memset(&uio, 0, sizeof(uio));
		uio.uio_iov = &iov;
		uio.uio_iovcnt = 1;
		uio.uio_rw = UIO_READ;
		uio.uio_segflg = UIO_SYSSPACE;
		uio.uio_td = curthread;
		uio.uio_resid = chunk;

		flags = MSG_WAITALL;
		error = soreceive(so, NULL, &uio, NULL, NULL, &flags);
		if (error != 0)
			return (error);
		if (uio.uio_resid != 0)
			return (EPIPE);		/* peer closed */
		buf = (char *)buf + chunk;
		len -= chunk;
	}
	return (0);
}

/* Discard len bytes from the stream, to stay in sync after a bad PDU. */
static int
vhci_sock_drain(struct vhci_port *port, size_t len)
{
	size_t chunk;
	int error;

	while (len > 0) {
		chunk = len;
		if (chunk > VHCI_STAGE_SIZE)
			chunk = VHCI_STAGE_SIZE;
		error = vhci_sock_recv(port->so, port->rx_buf, chunk);
		if (error != 0)
			return (error);
		len -= chunk;
	}
	return (0);
}

/*
 * Transfer geometry.  A USB/IP submission carries one contiguous data
 * payload.  For a control transfer frame 0 is the 8-byte SETUP packet
 * and the data, if any, is in frame 1; other transfer types put data in
 * every frame.
 */
static uint8_t
vhci_first_data_frame(const struct usb_xfer *xfer)
{

	return (xfer->flags_int.control_xfr ? 1 : 0);
}

static uint32_t
vhci_data_length(const struct usb_xfer *xfer)
{
	uint32_t len = 0;
	uint8_t i;

	for (i = vhci_first_data_frame(xfer); i < xfer->nframes; i++)
		len += xfer->frlengths[i];
	return (len);
}

static int
vhci_is_in(const struct usb_xfer *xfer)
{

	return ((xfer->endpointno & UE_DIR_IN) != 0);
}

/* Translate a USB/IP status, which is a negative Linux errno. */
static usb_error_t
vhci_status_to_usb_error(int32_t status)
{

	switch (status) {
	case USBIP_E_OK:
		return (USB_ERR_NORMAL_COMPLETION);
	case USBIP_E_EPIPE:
		return (USB_ERR_STALLED);
	case USBIP_E_ENOENT:
	case USBIP_E_ECONNRESET:
		return (USB_ERR_CANCELLED);
	case USBIP_E_ETIMEDOUT:
		return (USB_ERR_TIMEOUT);
	case USBIP_E_EREMOTEIO:
		return (USB_ERR_SHORT_XFER);
	case USBIP_E_ENODEV:
	case USBIP_E_ESHUTDOWN:
		return (USB_ERR_IOERROR);
	default:
		return (USB_ERR_IOERROR);
	}
}

/*
 * Find the root hub port a transfer belongs to by walking up to the
 * device that is plugged directly into our root hub.
 */
static struct vhci_port *
vhci_xfer_port(struct vhci_softc *sc, struct usb_device *udev)
{

	while (udev->parent_hub != NULL && udev->parent_hub->parent_hub != NULL)
		udev = udev->parent_hub;
	if (udev->parent_hub == NULL)
		return (NULL);		/* the root hub itself */
	if (udev->port_index >= VHCI_NPORTS)
		return (NULL);
	return (&sc->sc_port[udev->port_index]);
}

static uint32_t
vhci_next_seqnum(struct vhci_port *port)
{

	VHCI_LOCK_ASSERT(port->sc);

	/* Sequence number zero is reserved. */
	if (++port->next_seqnum == 0)
		port->next_seqnum = 1;
	return (port->next_seqnum);
}

/*
 * Fail a transfer that was never queued on a port.  Safe to call more
 * than once for the same transfer, which the USB stack requires.
 */
static void
vhci_device_done(struct usb_xfer *xfer, usb_error_t error)
{
	struct vhci_urb *urb = xfer->qh_start[0];

	USB_BUS_LOCK_ASSERT(xfer->xroot->bus, MA_OWNED);

	if (urb != NULL)
		urb->state = VHCI_URB_IDLE;
	usbd_transfer_done(xfer, error);
}

/*
 * Detach an urb from whichever port queue it is on and mark it done.
 * The port is passed explicitly because a transfer can outlive the
 * lookup path that found it.
 */
static void
vhci_urb_retire(struct vhci_port *port, struct vhci_urb *urb,
    usb_error_t error)
{

	VHCI_LOCK_ASSERT(port->sc);

	if (urb->queued) {
		TAILQ_REMOVE(&port->tx_queue, urb, entry);
		urb->queued = 0;
	} else if (urb->state == VHCI_URB_INFLIGHT) {
		TAILQ_REMOVE(&port->inflight, urb, entry);
	}
	urb->state = VHCI_URB_IDLE;

	/*
	 * A thread is still using this transfer with the lock dropped.
	 * It notices the abort flag and completes the transfer itself;
	 * completing it here would let the memory holding this struct
	 * be freed underneath that thread.
	 */
	if (urb->busy) {
		urb->aborted = 1;
		return;
	}
	usbd_transfer_done(urb->xfer, error);
}

/*
 * Record a submission that has already gone out but whose transfer we
 * are giving up on, and queue a USBIP_CMD_UNLINK for it.  The transfer
 * is completed by the caller, so nothing here refers to it afterwards.
 *
 * If there is no room for the record we cannot tell later whether a
 * reply for this sequence number carries a payload, so the only safe
 * course is to drop the session.
 */
static void
vhci_cancel_sent(struct vhci_port *port, uint32_t target, int is_in)
{
	struct vhci_unlink *u;
	int i;

	VHCI_LOCK_ASSERT(port->sc);

	for (i = 0; i < VHCI_UNLINK_MAX; i++) {
		u = &port->unlink[i];
		if (u->valid)
			continue;
		u->seqnum = vhci_next_seqnum(port);
		u->target = target;
		u->is_in = is_in ? 1 : 0;
		u->pending = 1;
		u->valid = 1;
		port->unlink_pending++;
		cv_signal(&port->tx_cv);
		return;
	}

	device_printf(port->sc->sc_dev,
	    "port %d: too many outstanding unlinks, dropping session\n",
	    port->index);
	port->disconnecting = 1;
	cv_signal(&port->tx_cv);
}

static struct vhci_unlink *
vhci_find_unlink(struct vhci_port *port, uint32_t target)
{
	int i;

	VHCI_LOCK_ASSERT(port->sc);

	for (i = 0; i < VHCI_UNLINK_MAX; i++) {
		if (port->unlink[i].valid &&
		    port->unlink[i].target == target)
			return (&port->unlink[i]);
	}
	return (NULL);
}

/* Drop the record for a completed unlink, identified by its own seqnum. */
static void
vhci_release_unlink(struct vhci_port *port, uint32_t seqnum)
{
	struct vhci_unlink *u;
	int i;

	VHCI_LOCK_ASSERT(port->sc);

	for (i = 0; i < VHCI_UNLINK_MAX; i++) {
		u = &port->unlink[i];
		if (!u->valid || u->seqnum != seqnum)
			continue;
		u->valid = 0;
		if (u->pending) {
			u->pending = 0;
			port->unlink_pending--;
		}
		return;
	}
}

static struct vhci_urb *
vhci_find_seqnum(struct vhci_port *port, uint32_t seqnum)
{
	struct vhci_urb *urb;

	VHCI_LOCK_ASSERT(port->sc);

	TAILQ_FOREACH(urb, &port->inflight, entry) {
		if (urb->seqnum == seqnum)
			return (urb);
	}
	return (NULL);
}

/*
 * Pipe methods.
 */

static void
vhci_pipe_open(struct usb_xfer *xfer)
{

	(void)xfer;
}

static void
vhci_pipe_close(struct usb_xfer *xfer)
{
	struct vhci_softc *sc = VHCI_BUS2SC(xfer->xroot->bus);
	struct vhci_urb *urb = xfer->qh_start[0];
	struct vhci_port *port;

	VHCI_LOCK_ASSERT(sc);

	port = vhci_xfer_port(sc, xfer->xroot->udev);
	if (port == NULL || urb == NULL || urb->state == VHCI_URB_IDLE) {
		usbd_transfer_done(xfer, USB_ERR_CANCELLED);
		return;
	}

	/*
	 * If the submission is already on the wire the server will
	 * answer it, so leave a record telling the receive path what to
	 * expect.  The TX thread does the same for a submission it is
	 * in the middle of sending.
	 */
	if (urb->state == VHCI_URB_INFLIGHT)
		vhci_cancel_sent(port, urb->seqnum, vhci_is_in(xfer));

	vhci_urb_retire(port, urb, USB_ERR_CANCELLED);
}

static void
vhci_pipe_enter(struct usb_xfer *xfer)
{

	(void)xfer;
}

static void
vhci_timeout(void *arg)
{
	struct usb_xfer *xfer = arg;
	struct vhci_softc *sc = VHCI_BUS2SC(xfer->xroot->bus);
	struct vhci_urb *urb = xfer->qh_start[0];
	struct vhci_port *port;

	VHCI_LOCK_ASSERT(sc);

	port = vhci_xfer_port(sc, xfer->xroot->udev);
	if (port == NULL || urb == NULL) {
		usbd_transfer_done(xfer, USB_ERR_TIMEOUT);
		return;
	}

	/*
	 * Ask the server to drop the request so it stops working on it.
	 * The transfer fails now either way; the unlink exchange, and
	 * any reply that was already on its way, are reconciled against
	 * the record vhci_cancel_sent() leaves behind.
	 */
	if (urb->state == VHCI_URB_INFLIGHT)
		vhci_cancel_sent(port, urb->seqnum, vhci_is_in(xfer));

	vhci_urb_retire(port, urb, USB_ERR_TIMEOUT);
}

static void
vhci_pipe_start(struct usb_xfer *xfer)
{
	struct vhci_softc *sc = VHCI_BUS2SC(xfer->xroot->bus);
	struct vhci_urb *urb = xfer->qh_start[0];
	struct vhci_port *port;

	VHCI_LOCK_ASSERT(sc);

	port = vhci_xfer_port(sc, xfer->xroot->udev);
	if (port == NULL || !port->connected || port->disconnecting) {
		vhci_device_done(xfer, USB_ERR_IOERROR);
		return;
	}
	if (urb == NULL) {
		vhci_device_done(xfer, USB_ERR_NOMEM);
		return;
	}

	/*
	 * USB/IP carries a control transfer as one message: setup, data
	 * and status together.  The stack only splits a control transfer
	 * across submissions when a client driver asks it to, which we
	 * cannot express on the wire.
	 */
	if (xfer->flags_int.control_xfr &&
	    (!xfer->flags_int.control_hdr || xfer->flags_int.control_act)) {
		device_printf(sc->sc_dev,
		    "split control transfers are not supported\n");
		vhci_device_done(xfer, USB_ERR_IOERROR);
		return;
	}
	if (vhci_data_length(xfer) > VHCI_MAX_XFER_LEN) {
		vhci_device_done(xfer, USB_ERR_BAD_BUFSIZE);
		return;
	}

	urb->xfer = xfer;
	urb->seqnum = vhci_next_seqnum(port);
	urb->aborted = 0;
	urb->state = VHCI_URB_TX;
	TAILQ_INSERT_TAIL(&port->tx_queue, urb, entry);
	urb->queued = 1;

	usbd_transfer_enqueue(&sc->sc_bus.intr_q, xfer);
	if (xfer->timeout != 0)
		usbd_transfer_timeout_ms(xfer, vhci_timeout, xfer->timeout);

	cv_signal(&port->tx_cv);
}

const struct usb_pipe_methods vhci_pipe_methods = {
	.open = vhci_pipe_open,
	.close = vhci_pipe_close,
	.enter = vhci_pipe_enter,
	.start = vhci_pipe_start,
};

/*
 * Transmit path.
 */

static void
vhci_put_header(uint8_t *buf, uint32_t command, uint32_t seqnum,
    uint32_t devid, uint32_t direction, uint32_t ep)
{

	be32enc(buf + 0, command);
	be32enc(buf + 4, seqnum);
	be32enc(buf + 8, devid);
	be32enc(buf + 12, direction);
	be32enc(buf + 16, ep);
}

/*
 * Send one CMD_SUBMIT.  Called with the bus lock held; drops it around
 * each socket write.  Returns a socket error, or 0.
 */
static int
vhci_tx_submit(struct vhci_port *port, struct vhci_urb *urb)
{
	struct vhci_softc *sc = port->sc;
	struct usb_xfer *xfer = urb->xfer;
	uint32_t flags, datalen;
	uint8_t *hdr = port->tx_buf;
	uint8_t i, first;
	int is_in, error;

	VHCI_LOCK_ASSERT(sc);

	is_in = vhci_is_in(xfer);
	datalen = vhci_data_length(xfer);
	first = vhci_first_data_frame(xfer);

	/*
	 * Deliberately not setting URB_SHORT_NOT_OK: the FreeBSD stack
	 * decides for itself whether a short IN transfer is an error,
	 * and asking the server to fail them causes spurious errors.
	 */
	flags = 0;
	if (!is_in && xfer->flags.force_short_xfer)
		flags |= USBIP_URB_ZERO_PACKET;

	memset(hdr, 0, 48);
	vhci_put_header(hdr, USBIP_CMD_SUBMIT, urb->seqnum, port->devid,
	    is_in ? USBIP_DIR_IN : USBIP_DIR_OUT,
	    UE_GET_ADDR(xfer->endpointno));
	be32enc(hdr + 20, flags);
	be32enc(hdr + 24, datalen);
	be32enc(hdr + 28, 0);				/* start_frame */
	be32enc(hdr + 32, (uint32_t)USBIP_NUMBER_OF_PACKETS_NON_ISO);
	be32enc(hdr + 36, xfer->interval);
	if (xfer->flags_int.control_xfr)
		usbd_copy_out(xfer->frbuffers + 0, 0, hdr + 40, 8);

	VHCI_UNLOCK(sc);
	error = vhci_sock_send(port->so, hdr, 48);
	VHCI_LOCK(sc);
	if (error != 0 || is_in || datalen == 0)
		return (error);

	/*
	 * Outgoing payload, one frame at a time, in staging-sized bites.
	 * We always send the whole announced length even if the transfer
	 * gets cancelled meanwhile: stopping short would desynchronise
	 * the stream.  Reading the buffer stays safe because completion
	 * is deferred while urb->busy is set.
	 */
	for (i = first; i < xfer->nframes; i++) {
		uint32_t off = 0, flen = xfer->frlengths[i];

		while (off < flen) {
			uint32_t n = flen - off;

			if (n > VHCI_STAGE_SIZE)
				n = VHCI_STAGE_SIZE;
			usbd_copy_out(xfer->frbuffers + i, off,
			    port->tx_buf, n);

			VHCI_UNLOCK(sc);
			error = vhci_sock_send(port->so, port->tx_buf, n);
			VHCI_LOCK(sc);
			if (error != 0)
				return (error);
			off += n;
		}
	}
	return (0);
}

static int
vhci_tx_unlink(struct vhci_port *port, const struct vhci_unlink *u)
{
	struct vhci_softc *sc = port->sc;
	uint8_t *hdr = port->tx_buf;
	int error;

	VHCI_LOCK_ASSERT(sc);

	memset(hdr, 0, 48);
	vhci_put_header(hdr, USBIP_CMD_UNLINK, u->seqnum, port->devid,
	    USBIP_DIR_OUT, 0);
	be32enc(hdr + 20, u->target);

	VHCI_UNLOCK(sc);
	error = vhci_sock_send(port->so, hdr, 48);
	VHCI_LOCK(sc);

	return (error);
}

static void
vhci_tx_thread(void *arg)
{
	struct vhci_port *port = arg;
	struct vhci_softc *sc = port->sc;
	struct vhci_urb *urb;
	int error;

	VHCI_LOCK(sc);
	for (;;) {
		while (!port->disconnecting && port->unlink_pending == 0 &&
		    TAILQ_EMPTY(&port->tx_queue))
			cv_wait(&port->tx_cv, &sc->sc_bus.bus_mtx);
		if (port->disconnecting)
			break;

		/* Cancellations first; they free the server's resources. */
		if (port->unlink_pending != 0) {
			struct vhci_unlink u;
			int i;

			for (i = 0; i < VHCI_UNLINK_MAX; i++) {
				if (port->unlink[i].valid &&
				    port->unlink[i].pending)
					break;
			}
			if (i == VHCI_UNLINK_MAX) {
				port->unlink_pending = 0;
				continue;
			}
			port->unlink[i].pending = 0;
			port->unlink_pending--;
			u = port->unlink[i];
			if (vhci_tx_unlink(port, &u) != 0)
				break;
			continue;
		}

		urb = TAILQ_FIRST(&port->tx_queue);
		TAILQ_REMOVE(&port->tx_queue, urb, entry);
		urb->queued = 0;
		urb->busy = 1;

		error = vhci_tx_submit(port, urb);
		urb->busy = 0;

		if (urb->aborted) {
			/*
			 * The transfer was cancelled while we were
			 * copying out of it.  We sent the submission
			 * anyway - stopping short would desynchronise the
			 * stream - so the server will answer it and the
			 * receive path needs to know that.
			 */
			urb->aborted = 0;
			if (error == 0)
				vhci_cancel_sent(port, urb->seqnum,
				    vhci_is_in(urb->xfer));
			usbd_transfer_done(urb->xfer, USB_ERR_CANCELLED);
			if (error != 0)
				break;
			continue;
		}
		if (error != 0) {
			vhci_urb_retire(port, urb, USB_ERR_IOERROR);
			break;
		}
		urb->state = VHCI_URB_INFLIGHT;
		TAILQ_INSERT_TAIL(&port->inflight, urb, entry);
	}

	port->tx_running = 0;
	cv_broadcast(&port->exit_cv);
	VHCI_UNLOCK(sc);
	kthread_exit();
}

/*
 * Receive path.
 */

/*
 * Account for a completed transfer: spread actual_length across the
 * data frames, reading the payload off the socket for an IN transfer.
 * Runs with the bus lock held and drops it around each read.
 */
static int
vhci_rx_complete(struct vhci_port *port, struct vhci_urb *urb,
    uint32_t actual_length)
{
	struct vhci_softc *sc = port->sc;
	struct usb_xfer *xfer = urb->xfer;
	uint32_t remaining = actual_length;
	uint8_t i, first;
	int is_in, error;

	VHCI_LOCK_ASSERT(sc);

	is_in = vhci_is_in(xfer);
	first = vhci_first_data_frame(xfer);
	/* The SETUP frame of a control transfer always completes. */
	xfer->aframes = first;

	for (i = first; i < xfer->nframes; i++) {
		uint32_t off, flen;

		flen = xfer->frlengths[i];
		if (flen > remaining)
			flen = remaining;

		for (off = 0; is_in && off < flen; ) {
			uint32_t n = flen - off;

			if (n > VHCI_STAGE_SIZE)
				n = VHCI_STAGE_SIZE;

			VHCI_UNLOCK(sc);
			error = vhci_sock_recv(port->so, port->rx_buf, n);
			VHCI_LOCK(sc);
			if (error != 0)
				return (error);

			/*
			 * The transfer may have been cancelled while the
			 * lock was dropped.  Keep draining the socket so
			 * the stream stays in sync, but stop writing to a
			 * buffer we no longer own.
			 */
			if (!urb->aborted)
				usbd_copy_in(xfer->frbuffers + i, off,
				    port->rx_buf, n);
			off += n;
		}

		if (!urb->aborted) {
			xfer->frlengths[i] = flen;
			xfer->aframes = i + 1;
		}
		remaining -= flen;
	}

	/* The server sent more than we asked for; discard the excess. */
	if (is_in && remaining > 0) {
		VHCI_UNLOCK(sc);
		error = vhci_sock_drain(port, remaining);
		VHCI_LOCK(sc);
		if (error != 0)
			return (error);
	}
	return (0);
}

static int
vhci_rx_ret_submit(struct vhci_port *port, const uint8_t *hdr)
{
	struct vhci_softc *sc = port->sc;
	struct vhci_urb *urb;
	uint32_t seqnum, actual_length;
	int32_t status;
	usb_error_t err;
	int error;

	seqnum = be32dec(hdr + 4);
	status = (int32_t)be32dec(hdr + 20);
	actual_length = be32dec(hdr + 24);

	if (actual_length > VHCI_MAX_XFER_LEN) {
		device_printf(sc->sc_dev,
		    "port %d: server announced an absurd length %u\n",
		    port->index, actual_length);
		return (EPROTO);
	}

	VHCI_LOCK(sc);
	urb = vhci_find_seqnum(port, seqnum);
	if (urb == NULL) {
		struct vhci_unlink *u = vhci_find_unlink(port, seqnum);

		/*
		 * No transfer is waiting for this sequence number.  That
		 * is expected when a reply lost the race with an unlink,
		 * and the unlink record is what tells us whether a
		 * payload follows, since the reply header's direction is
		 * always zero.  Anything else means we have lost track
		 * of the stream.
		 */
		if (u == NULL) {
			VHCI_UNLOCK(sc);
			device_printf(sc->sc_dev,
			    "port %d: reply for unknown sequence %u\n",
			    port->index, seqnum);
			return (EPROTO);
		}
		u->valid = 0;
		if (u->pending) {
			u->pending = 0;
			port->unlink_pending--;
		}
		if (!u->is_in)
			actual_length = 0;
		VHCI_UNLOCK(sc);
		return (vhci_sock_drain(port, actual_length));
	}

	/*
	 * Take the transfer off the in-flight list before dropping the
	 * lock, so nothing else can complete it, and mark it busy so a
	 * concurrent cancel defers to us.
	 */
	TAILQ_REMOVE(&port->inflight, urb, entry);
	urb->state = VHCI_URB_IDLE;
	urb->busy = 1;

	err = vhci_status_to_usb_error(status);
	error = vhci_rx_complete(port, urb, actual_length);

	urb->busy = 0;
	if (urb->aborted) {
		urb->aborted = 0;
		err = USB_ERR_CANCELLED;
	} else if (error != 0) {
		/* The payload never arrived in full. */
		err = USB_ERR_IOERROR;
	}
	usbd_transfer_done(urb->xfer, err);
	VHCI_UNLOCK(sc);

	return (error);
}

static void
vhci_rx_thread(void *arg)
{
	struct vhci_port *port = arg;
	struct vhci_softc *sc = port->sc;
	uint8_t hdr[48];
	uint32_t command;
	int error;

	for (;;) {
		error = vhci_sock_recv(port->so, hdr, sizeof(hdr));
		if (error != 0)
			break;

		command = be32dec(hdr);
		switch (command) {
		case USBIP_RET_SUBMIT:
			error = vhci_rx_ret_submit(port, hdr);
			break;
		case USBIP_RET_UNLINK:
			/*
			 * Carries no payload.  The transfer was failed
			 * when the unlink was queued; all that is left is
			 * to release the bookkeeping, unless a late reply
			 * already did so.
			 */
			VHCI_LOCK(sc);
			vhci_release_unlink(port, be32dec(hdr + 4));
			VHCI_UNLOCK(sc);
			error = 0;
			break;
		default:
			device_printf(sc->sc_dev,
			    "port %d: unexpected command %#x from server\n",
			    port->index, command);
			error = EPROTO;
			break;
		}
		if (error != 0)
			break;

		VHCI_LOCK(sc);
		error = port->disconnecting;
		VHCI_UNLOCK(sc);
		if (error != 0)
			break;
	}

	VHCI_LOCK(sc);
	port->rx_running = 0;
	/* Nothing more will ever arrive; wake the TX thread too. */
	port->disconnecting = 1;
	cv_signal(&port->tx_cv);
	cv_broadcast(&port->exit_cv);
	VHCI_UNLOCK(sc);
	kthread_exit();
}

/*
 * Session lifecycle.  Called from the ioctl path with sc_sx held and
 * the bus lock not held.
 */

int
vhci_session_start(struct vhci_port *port)
{
	struct vhci_softc *sc = port->sc;
	int error;

	port->tx_buf = malloc(VHCI_STAGE_SIZE, M_VHCI_BUF, M_WAITOK);
	port->rx_buf = malloc(VHCI_STAGE_SIZE, M_VHCI_BUF, M_WAITOK);

	cv_init(&port->tx_cv, "vhcitx");
	cv_init(&port->exit_cv, "vhciex");
	TAILQ_INIT(&port->tx_queue);
	TAILQ_INIT(&port->inflight);
	port->next_seqnum = 0;
	memset(port->unlink, 0, sizeof(port->unlink));
	port->unlink_pending = 0;
	port->disconnecting = 0;

	VHCI_LOCK(sc);
	port->tx_running = 1;
	port->rx_running = 1;
	VHCI_UNLOCK(sc);

	error = kthread_add(vhci_tx_thread, port, NULL, NULL, 0, 0,
	    "vhci%d tx", port->index);
	if (error != 0) {
		VHCI_LOCK(sc);
		port->tx_running = 0;
		port->rx_running = 0;
		VHCI_UNLOCK(sc);
		goto fail;
	}
	error = kthread_add(vhci_rx_thread, port, NULL, NULL, 0, 0,
	    "vhci%d rx", port->index);
	if (error != 0) {
		VHCI_LOCK(sc);
		port->rx_running = 0;
		port->disconnecting = 1;
		cv_signal(&port->tx_cv);
		while (port->tx_running)
			cv_wait(&port->exit_cv, &sc->sc_bus.bus_mtx);
		VHCI_UNLOCK(sc);
		goto fail;
	}
	return (0);

fail:
	cv_destroy(&port->tx_cv);
	cv_destroy(&port->exit_cv);
	free(port->tx_buf, M_VHCI_BUF);
	free(port->rx_buf, M_VHCI_BUF);
	port->tx_buf = NULL;
	port->rx_buf = NULL;
	return (error);
}

void
vhci_session_stop(struct vhci_port *port)
{
	struct vhci_softc *sc = port->sc;
	struct vhci_urb *urb;

	VHCI_LOCK(sc);
	port->disconnecting = 1;
	cv_signal(&port->tx_cv);
	VHCI_UNLOCK(sc);

	/* Dislodge a thread blocked in soreceive() or sosend(). */
	if (port->so != NULL)
		(void)soshutdown(port->so, SHUT_RDWR);

	VHCI_LOCK(sc);
	while (port->tx_running || port->rx_running)
		cv_wait(&port->exit_cv, &sc->sc_bus.bus_mtx);

	/* Nothing can complete these now. */
	while ((urb = TAILQ_FIRST(&port->tx_queue)) != NULL)
		vhci_urb_retire(port, urb, USB_ERR_CANCELLED);
	while ((urb = TAILQ_FIRST(&port->inflight)) != NULL)
		vhci_urb_retire(port, urb, USB_ERR_CANCELLED);
	VHCI_UNLOCK(sc);

	cv_destroy(&port->tx_cv);
	cv_destroy(&port->exit_cv);
	free(port->tx_buf, M_VHCI_BUF);
	free(port->rx_buf, M_VHCI_BUF);
	port->tx_buf = NULL;
	port->rx_buf = NULL;
}
