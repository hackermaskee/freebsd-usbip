/* SPDX-License-Identifier: BSD-2-Clause */
/*-
 * Copyright (c) 2026 furuta@furuta.bsdclub.org
 *
 * usbipd(8): the transfer phase.
 *
 * A client's USBIP_CMD_SUBMIT becomes a libusb transfer against the
 * real device; when that finishes, the result goes back as a
 * USBIP_RET_SUBMIT.  Transfers are asynchronous because a client
 * pipelines them, and because a blocking submit on an interrupt
 * endpoint that never fires would stall everything else.
 *
 * Two threads: this one reads the socket and submits, while a second
 * runs libusb's event loop and answers from the completion callbacks.
 * A mutex serialises writes to the socket, which is the only state
 * they share besides the in-flight list.
 */

#include <sys/queue.h>

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <libusb.h>

#include "usbipd.h"

struct usbipd_session;

struct usbipd_urb {
	TAILQ_ENTRY(usbipd_urb)	entry;
	struct usbipd_session	*s;
	struct libusb_transfer	*xfer;
	uint32_t		seqnum;
	uint32_t		datalen;	/* requested, without setup */
	unsigned char		*buf;		/* setup + data, or data */
	int			is_in;
	int			is_control;
	/*
	 * Set once we have told the client we cancelled this transfer.
	 * Its completion must then stay silent: a client that has been
	 * told a transfer was unlinked has forgotten the sequence
	 * number, and answering it anyway makes it drop the session.
	 */
	int			unlinked;

	/*
	 * Isochronous only.  The offsets are the client's, into its own
	 * buffer, and mean nothing here - but they have to come back
	 * unchanged, because they are how the client knows where to put
	 * each packet.
	 */
	int			npackets;
	uint32_t		*offsets;
	uint32_t		*iso_lengths;
};

struct usbipd_session {
	struct usbipd		*d;
	int			fd;
	libusb_device_handle	*dh;

	pthread_mutex_t		lock;		/* in-flight list */
	pthread_mutex_t		wlock;		/* socket writes */
	TAILQ_HEAD(, usbipd_urb) inflight;
	int			ninflight;

	int			done;		/* tell the event loop to stop */
	uint8_t			claimed[USBIP_MAX_INTERFACES];
	uint8_t			nclaimed;

	/*
	 * Endpoint transfer types, indexed by direction and number.
	 * USB/IP says which endpoint but not what kind it is, and libusb
	 * needs to know.  Rebuilt when the client changes configuration
	 * or alternate setting.
	 */
	uint8_t			ep_type[2][16];
	uint8_t			ep_known;
};

#define	EP_DIR_INDEX(addr)	(((addr) & 0x80) ? 1 : 0)
#define	EP_NUM_INDEX(addr)	((addr) & 0x0F)

/* libusb's outcome, as the negative Linux errno the protocol carries. */
static int32_t
usbipd_status(const struct libusb_transfer *xfer)
{

	switch (xfer->status) {
	case LIBUSB_TRANSFER_COMPLETED:
		return (USBIP_E_OK);
	case LIBUSB_TRANSFER_STALL:
		return (USBIP_E_EPIPE);
	case LIBUSB_TRANSFER_TIMED_OUT:
		return (USBIP_E_ETIMEDOUT);
	case LIBUSB_TRANSFER_CANCELLED:
		return (USBIP_E_ECONNRESET);
	case LIBUSB_TRANSFER_NO_DEVICE:
		return (USBIP_E_ENODEV);
	case LIBUSB_TRANSFER_OVERFLOW:
		return (USBIP_E_EOVERFLOW);
	case LIBUSB_TRANSFER_ERROR:
	default:
		return (USBIP_E_EPROTO);
	}
}

static void
put_header(uint8_t *hdr, uint32_t command, uint32_t seqnum)
{

	memset(hdr, 0, 48);
	hdr[0] = command >> 24; hdr[1] = command >> 16;
	hdr[2] = command >> 8;  hdr[3] = command;
	hdr[4] = seqnum >> 24;  hdr[5] = seqnum >> 16;
	hdr[6] = seqnum >> 8;   hdr[7] = seqnum;
	/* devid, direction and ep are zero in a reply, by convention. */
}

static void
put_be32(uint8_t *p, uint32_t v)
{

	p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
}

static uint32_t
get_be32(const uint8_t *p)
{

	return (((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	    ((uint32_t)p[2] << 8) | p[3]);
}

/*
 * Learn the transfer type of every endpoint in the active
 * configuration.  A submission names an endpoint but not its kind, and
 * libusb needs the kind to know which call to make.
 */
static void
learn_endpoints(struct usbipd_session *s)
{
	struct libusb_config_descriptor *cfg;
	libusb_device *dev = libusb_get_device(s->dh);
	int i, j, k;

	memset(s->ep_type, 0xFF, sizeof(s->ep_type));
	s->ep_known = 0;

	if (libusb_get_active_config_descriptor(dev, &cfg) != 0)
		return;

	for (i = 0; i < cfg->bNumInterfaces; i++) {
		const struct libusb_interface *iface = &cfg->interface[i];

		for (j = 0; j < iface->num_altsetting; j++) {
			const struct libusb_interface_descriptor *alt =
			    &iface->altsetting[j];

			for (k = 0; k < alt->bNumEndpoints; k++) {
				const struct libusb_endpoint_descriptor *ep =
				    &alt->endpoint[k];
				uint8_t a = ep->bEndpointAddress;

				s->ep_type[EP_DIR_INDEX(a)][EP_NUM_INDEX(a)] =
				    ep->bmAttributes &
				    LIBUSB_TRANSFER_TYPE_MASK;
			}
		}
	}
	libusb_free_config_descriptor(cfg);
	s->ep_known = 1;
}

static int
endpoint_type(struct usbipd_session *s, uint8_t addr)
{
	uint8_t t;

	if (EP_NUM_INDEX(addr) == 0)
		return (LIBUSB_TRANSFER_TYPE_CONTROL);
	if (!s->ep_known)
		learn_endpoints(s);
	t = s->ep_type[EP_DIR_INDEX(addr)][EP_NUM_INDEX(addr)];
	if (t == 0xFF) {
		/* The client may have just changed the configuration. */
		learn_endpoints(s);
		t = s->ep_type[EP_DIR_INDEX(addr)][EP_NUM_INDEX(addr)];
	}
	return (t == 0xFF ? -1 : (int)t);
}

/* Serialised so two completions cannot interleave on the socket. */
static int
send_locked(struct usbipd_session *s, const void *buf, size_t len)
{
	int error;

	pthread_mutex_lock(&s->wlock);
	error = usbip_net_send_all_quiet(s->fd, buf, len);
	pthread_mutex_unlock(&s->wlock);
	return (error);
}

static void
urb_free(struct usbipd_urb *u)
{

	if (u->xfer != NULL)
		libusb_free_transfer(u->xfer);
	free(u->offsets);
	free(u->iso_lengths);
	free(u->buf);
	free(u);
}

/*
 * Reply to an isochronous transfer: the bytes actually moved, packed
 * together, then a descriptor per packet.  Only the transferred bytes
 * go on the wire - the gaps a short packet leaves are not sent - and
 * the descriptors are what let the client put them back where they
 * belong.
 */
static void
reply_iso(struct usbipd_urb *u, struct libusb_transfer *xfer)
{
	struct usbipd_session *s = u->s;
	unsigned char *msg, *p;
	size_t total = 0, msglen;
	uint32_t errors = 0;
	int i;

	for (i = 0; i < xfer->num_iso_packets; i++)
		total += xfer->iso_packet_desc[i].actual_length;

	msglen = 48 + total + (size_t)xfer->num_iso_packets * 16;
	msg = malloc(msglen);
	if (msg == NULL)
		return;

	p = msg + 48;
	for (i = 0; i < xfer->num_iso_packets; i++) {
		struct libusb_iso_packet_descriptor *d =
		    &xfer->iso_packet_desc[i];

		if (u->is_in && d->actual_length > 0) {
			memcpy(p, libusb_get_iso_packet_buffer(xfer, i),
			    d->actual_length);
		}
		if (u->is_in)
			p += d->actual_length;
		if (d->status != LIBUSB_TRANSFER_COMPLETED)
			errors++;
	}
	if (!u->is_in) {
		/* Nothing comes back for an OUT transfer. */
		msglen = 48 + (size_t)xfer->num_iso_packets * 16;
		p = msg + 48;
		total = 0;
	}

	for (i = 0; i < xfer->num_iso_packets; i++) {
		struct libusb_iso_packet_descriptor *d =
		    &xfer->iso_packet_desc[i];

		put_be32(p + 0, u->offsets[i]);
		put_be32(p + 4, d->length);
		put_be32(p + 8, d->actual_length);
		put_be32(p + 12, d->status == LIBUSB_TRANSFER_COMPLETED ?
		    0 : (uint32_t)USBIP_E_EPROTO);
		p += 16;
	}

	put_header(msg, USBIP_RET_SUBMIT, u->seqnum);
	put_be32(msg + 20, (uint32_t)usbipd_status(xfer));
	put_be32(msg + 24, (uint32_t)total);
	put_be32(msg + 28, 0);				/* start_frame */
	put_be32(msg + 32, (uint32_t)xfer->num_iso_packets);
	put_be32(msg + 36, errors);

	pthread_mutex_lock(&s->wlock);
	(void)usbip_net_send_all_quiet(s->fd, msg, msglen);
	pthread_mutex_unlock(&s->wlock);

	free(msg);
}

static void LIBUSB_CALL
on_complete(struct libusb_transfer *xfer)
{
	struct usbipd_urb *u = xfer->user_data;
	struct usbipd_session *s = u->s;
	uint8_t hdr[48];
	const unsigned char *data;
	int32_t status;
	uint32_t actual;

	if (u->unlinked)
		goto retire;

	if (u->npackets > 0) {
		reply_iso(u, xfer);
		goto retire;
	}

	status = usbipd_status(xfer);
	actual = (uint32_t)xfer->actual_length;

	/*
	 * A control transfer's buffer starts with the 8-byte setup
	 * packet, which is not part of the payload and does not count
	 * towards the length libusb reports.
	 */
	data = u->is_control ? libusb_control_transfer_get_data(xfer) :
	    xfer->buffer;

	if (actual > u->datalen)
		actual = u->datalen;

	put_header(hdr, USBIP_RET_SUBMIT, u->seqnum);
	put_be32(hdr + 20, (uint32_t)status);
	put_be32(hdr + 24, actual);
	put_be32(hdr + 28, 0);				/* start_frame */
	put_be32(hdr + 32, (uint32_t)USBIP_NUMBER_OF_PACKETS_NON_ISO);
	put_be32(hdr + 36, 0);				/* error_count */

	pthread_mutex_lock(&s->wlock);
	if (usbip_net_send_all_quiet(s->fd, hdr, sizeof(hdr)) == 0 &&
	    u->is_in && actual > 0)
		(void)usbip_net_send_all_quiet(s->fd, data, actual);
	pthread_mutex_unlock(&s->wlock);

retire:
	pthread_mutex_lock(&s->lock);
	TAILQ_REMOVE(&s->inflight, u, entry);
	s->ninflight--;
	pthread_mutex_unlock(&s->lock);

	urb_free(u);
}

static void *
event_thread(void *arg)
{
	struct usbipd_session *s = arg;

	while (!s->done) {
		struct timeval tv = { 0, 200000 };	/* 200ms */

		if (libusb_handle_events_timeout_completed(s->d->ctx, &tv,
		    NULL) != 0)
			break;
	}
	return (NULL);
}

/*
 * Answer a submission we cannot carry out at all, so the client sees a
 * failure rather than waiting for a reply that will never come.
 */
static int
reject(struct usbipd_session *s, uint32_t seqnum, int32_t status)
{
	uint8_t hdr[48];

	put_header(hdr, USBIP_RET_SUBMIT, seqnum);
	put_be32(hdr + 20, (uint32_t)status);
	put_be32(hdr + 32, (uint32_t)USBIP_NUMBER_OF_PACKETS_NON_ISO);
	return (send_locked(s, hdr, sizeof(hdr)));
}

/*
 * SET_CONFIGURATION and SET_INTERFACE cannot simply be forwarded.
 *
 * Passed through as raw control transfers they would reach the device,
 * but libusb would not know the device had changed underneath it, and
 * on FreeBSD the alternate setting is what makes ugen(4) allocate an
 * isochronous endpoint at all.  So they are carried out through
 * libusb's own calls and answered here.
 *
 * Returns 1 if the request was handled, 0 to submit it normally.
 */
static int
intercept_control(struct usbipd_session *s, uint32_t seqnum,
    const uint8_t *setup)
{
	uint8_t bmRequestType = setup[0], bRequest = setup[1];
	uint16_t wValue = setup[2] | ((uint16_t)setup[3] << 8);
	uint16_t wIndex = setup[4] | ((uint16_t)setup[5] << 8);
	uint8_t reply[48];
	int error;

	if (bmRequestType == 0x00 && bRequest == 0x09) {
		error = libusb_set_configuration(s->dh, (int)wValue);
		usbipd_log(s->d, "set configuration %u: %s", wValue,
		    error == 0 ? "ok" : libusb_strerror(error));
	} else if (bmRequestType == 0x01 && bRequest == 0x0B) {
		error = libusb_set_interface_alt_setting(s->dh, (int)wIndex,
		    (int)wValue);
		usbipd_log(s->d, "interface %u to alternate setting %u: %s",
		    wIndex, wValue, error == 0 ? "ok" : libusb_strerror(error));
	} else {
		return (0);
	}

	s->ep_known = 0;

	put_header(reply, USBIP_RET_SUBMIT, seqnum);
	put_be32(reply + 20, error == 0 ? 0 : (uint32_t)USBIP_E_EPIPE);
	put_be32(reply + 32, (uint32_t)USBIP_NUMBER_OF_PACKETS_NON_ISO);
	(void)send_locked(s, reply, sizeof(reply));
	return (1);
}

static int
handle_submit(struct usbipd_session *s, const uint8_t *hdr)
{
	struct usbipd_urb *u;
	uint32_t seqnum, datalen, interval;
	uint8_t epaddr;
	int type, is_in, timeout, npackets, i;

	seqnum = get_be32(hdr + 4);
	is_in = get_be32(hdr + 12) == USBIP_DIR_IN;
	epaddr = (uint8_t)(get_be32(hdr + 16) & 0x0F) | (is_in ? 0x80 : 0);
	datalen = get_be32(hdr + 24);
	interval = get_be32(hdr + 36);

	if (datalen > USBIPD_MAX_XFER_LEN) {
		usbipd_log(s->d, "seq %u asks for %u bytes; refusing",
		    seqnum, datalen);
		return (reject(s, seqnum, USBIP_E_EOVERFLOW));
	}
	npackets = (int32_t)get_be32(hdr + 32);
	if (npackets < 0)
		npackets = 0;
	if (npackets > USBIPD_MAX_ISO_PACKETS) {
		usbipd_log(s->d, "seq %u asks for %d packets; refusing",
		    seqnum, npackets);
		return (reject(s, seqnum, USBIP_E_EOVERFLOW));
	}

	/*
	 * A configuration or interface change has to be applied through
	 * libusb, not sent as bytes; see intercept_control().  Endpoint
	 * zero with no payload, so nothing else has been read yet.
	 */
	if (EP_NUM_INDEX(epaddr) == 0 && datalen == 0 && npackets == 0 &&
	    intercept_control(s, seqnum, hdr + 40))
		return (0);

	u = calloc(1, sizeof(*u));
	if (u == NULL)
		return (reject(s, seqnum, USBIP_E_ENODEV));
	u->s = s;
	u->seqnum = seqnum;
	u->datalen = datalen;
	u->is_in = is_in;
	u->npackets = npackets;

	type = endpoint_type(s, epaddr);
	u->is_control = (type == LIBUSB_TRANSFER_TYPE_CONTROL);

	u->buf = calloc(1, (u->is_control ? 8 : 0) + datalen + 1);
	if (u->buf == NULL) {
		free(u);
		return (reject(s, seqnum, USBIP_E_ENODEV));
	}
	if (u->is_control)
		memcpy(u->buf, hdr + 40, 8);

	/* An OUT transfer's payload follows the header. */
	if (!is_in && datalen > 0) {
		unsigned char *p = u->buf + (u->is_control ? 8 : 0);

		if (usbip_net_recv_exact_quiet(s->fd, p, datalen) != 0) {
			urb_free(u);
			return (-1);
		}
	}

	/*
	 * The packet descriptors follow the payload.  They have to be
	 * read whether or not we can carry the transfer, or the stream
	 * loses its framing.
	 */
	if (npackets > 0) {
		unsigned char *raw = malloc((size_t)npackets * 16);

		u->offsets = calloc((size_t)npackets, sizeof(*u->offsets));
		if (raw == NULL || u->offsets == NULL) {
			free(raw);
			urb_free(u);
			return (-1);
		}
		if (usbip_net_recv_exact_quiet(s->fd, raw,
		    (size_t)npackets * 16) != 0) {
			free(raw);
			urb_free(u);
			return (-1);
		}
		for (i = 0; i < npackets; i++)
			u->offsets[i] = get_be32(raw + i * 16);
		u->iso_lengths = calloc((size_t)npackets,
		    sizeof(*u->iso_lengths));
		if (u->iso_lengths == NULL) {
			free(raw);
			urb_free(u);
			return (-1);
		}
		for (i = 0; i < npackets; i++)
			u->iso_lengths[i] = get_be32(raw + i * 16 + 4);
		free(raw);
	}

	u->xfer = libusb_alloc_transfer(npackets);
	if (u->xfer == NULL) {
		urb_free(u);
		return (reject(s, seqnum, USBIP_E_ENODEV));
	}

	/*
	 * Zero means "no timeout" to libusb.  A control transfer that
	 * hangs would otherwise never be answered, so give it a bound;
	 * the other types are the client's business to time out, and it
	 * does, with CMD_UNLINK.
	 */
	timeout = u->is_control ? 5000 : 0;

	if (npackets > 0) {
		if (type != LIBUSB_TRANSFER_TYPE_ISOCHRONOUS) {
			usbipd_log(s->d, "seq %u: endpoint %#04x is not "
			    "isochronous", seqnum, epaddr);
			urb_free(u);
			return (reject(s, seqnum, USBIP_E_EPIPE));
		}
		libusb_fill_iso_transfer(u->xfer, s->dh, epaddr, u->buf,
		    (int)datalen, npackets, on_complete, u, 0);
		for (i = 0; i < npackets; i++)
			u->xfer->iso_packet_desc[i].length = u->iso_lengths[i];
		goto submit;
	}

	switch (type) {
	case LIBUSB_TRANSFER_TYPE_CONTROL:
		libusb_fill_control_transfer(u->xfer, s->dh, u->buf,
		    on_complete, u, timeout);
		break;
	case LIBUSB_TRANSFER_TYPE_BULK:
		libusb_fill_bulk_transfer(u->xfer, s->dh, epaddr, u->buf,
		    (int)datalen, on_complete, u, timeout);
		break;
	case LIBUSB_TRANSFER_TYPE_INTERRUPT:
		libusb_fill_interrupt_transfer(u->xfer, s->dh, epaddr, u->buf,
		    (int)datalen, on_complete, u, timeout);
		break;
	default:
		usbipd_log(s->d, "seq %u: endpoint %#04x is not one we can "
		    "carry", seqnum, epaddr);
		urb_free(u);
		return (reject(s, seqnum, USBIP_E_EPIPE));
	}
	(void)interval;		/* libusb takes the period from the device */

submit:
	pthread_mutex_lock(&s->lock);
	if (s->ninflight >= USBIPD_MAX_INFLIGHT) {
		pthread_mutex_unlock(&s->lock);
		usbipd_log(s->d, "too many transfers outstanding; refusing "
		    "seq %u", seqnum);
		urb_free(u);
		return (reject(s, seqnum, USBIP_E_EPROTO));
	}
	TAILQ_INSERT_TAIL(&s->inflight, u, entry);
	s->ninflight++;
	pthread_mutex_unlock(&s->lock);

	if (libusb_submit_transfer(u->xfer) != 0) {
		pthread_mutex_lock(&s->lock);
		TAILQ_REMOVE(&s->inflight, u, entry);
		s->ninflight--;
		pthread_mutex_unlock(&s->lock);
		urb_free(u);
		return (reject(s, seqnum, USBIP_E_EPIPE));
	}

	/*
	 * A configuration or interface change invalidates what we know
	 * about the endpoints.
	 */
	if (u->is_control && (u->buf[1] == 0x09 || u->buf[1] == 0x0B))
		s->ep_known = 0;

	return (0);
}

static int
handle_unlink(struct usbipd_session *s, const uint8_t *hdr)
{
	struct usbipd_urb *u;
	uint32_t seqnum, target;
	uint8_t reply[48];
	int found = 0;

	seqnum = get_be32(hdr + 4);
	target = get_be32(hdr + 20);

	pthread_mutex_lock(&s->lock);
	TAILQ_FOREACH(u, &s->inflight, entry) {
		if (u->seqnum == target) {
			/*
			 * Marked before cancelling, so the completion
			 * that cancelling causes finds it already set.
			 */
			u->unlinked = 1;
			libusb_cancel_transfer(u->xfer);
			found = 1;
			break;
		}
	}
	pthread_mutex_unlock(&s->lock);

	/*
	 * -ECONNRESET if we found it, 0 if it had already finished,
	 * which is how the client tells a cancelled transfer from one
	 * that beat the cancellation.
	 */
	put_header(reply, USBIP_RET_UNLINK, seqnum);
	put_be32(reply + 20, found ? (uint32_t)USBIP_E_ECONNRESET : 0);
	return (send_locked(s, reply, sizeof(reply)));
}

static void
claim_interfaces(struct usbipd_session *s)
{
	struct libusb_config_descriptor *cfg;
	libusb_device *dev = libusb_get_device(s->dh);
	int i;

	if (libusb_get_active_config_descriptor(dev, &cfg) != 0)
		return;

	for (i = 0; i < cfg->bNumInterfaces &&
	    s->nclaimed < USBIP_MAX_INTERFACES; i++) {
		uint8_t num = cfg->interface[i].altsetting[0].bInterfaceNumber;

		if (libusb_claim_interface(s->dh, num) == 0)
			s->claimed[s->nclaimed++] = num;
		else
			usbipd_log(s->d, "could not claim interface %u", num);
	}
	libusb_free_config_descriptor(cfg);
}

static void
release_interfaces(struct usbipd_session *s)
{
	uint8_t i;

	for (i = 0; i < s->nclaimed; i++)
		(void)libusb_release_interface(s->dh, s->claimed[i]);
	s->nclaimed = 0;
}

int
usbipd_serve_urbs(struct usbipd *d, int fd, libusb_device_handle *dh)
{
	struct usbipd_session s;
	struct usbipd_urb *urb;
	pthread_t events;
	uint8_t hdr[48];
	uint32_t command;
	int error = 0, waited, empty = 1;

	memset(&s, 0, sizeof(s));
	s.d = d;
	s.fd = fd;
	s.dh = dh;
	TAILQ_INIT(&s.inflight);
	pthread_mutex_init(&s.lock, NULL);
	pthread_mutex_init(&s.wlock, NULL);

	claim_interfaces(&s);
	learn_endpoints(&s);

	if (pthread_create(&events, NULL, event_thread, &s) != 0) {
		release_interfaces(&s);
		return (-1);
	}

	for (;;) {
		if (usbip_net_recv_exact_quiet(fd, hdr, sizeof(hdr)) != 0)
			break;

		command = get_be32(hdr);
		if (command == USBIP_CMD_SUBMIT)
			error = handle_submit(&s, hdr);
		else if (command == USBIP_CMD_UNLINK)
			error = handle_unlink(&s, hdr);
		else {
			usbipd_log(d, "unexpected command %#x", command);
			break;
		}
		if (error != 0)
			break;
	}

	/*
	 * The client is gone.  Cancel everything outstanding and let the
	 * callbacks run, so libusb is never left owning memory we freed.
	 * The bound stops a device that has stopped answering from
	 * keeping the daemon here for ever.
	 */
	pthread_mutex_lock(&s.lock);
	TAILQ_FOREACH(urb, &s.inflight, entry) {
		/* Nobody is listening any more. */
		urb->unlinked = 1;
		libusb_cancel_transfer(urb->xfer);
	}
	pthread_mutex_unlock(&s.lock);

	for (waited = 0; waited < 5000; waited += 20) {
		pthread_mutex_lock(&s.lock);
		empty = TAILQ_EMPTY(&s.inflight);
		pthread_mutex_unlock(&s.lock);
		if (empty)
			break;
		usleep(20000);
	}
	if (!empty)
		usbipd_log(d, "gave up waiting for %d transfers to finish",
		    s.ninflight);

	s.done = 1;
	pthread_join(events, NULL);

	release_interfaces(&s);
	pthread_mutex_destroy(&s.lock);
	pthread_mutex_destroy(&s.wlock);

	return (0);
}
