/* SPDX-License-Identifier: BSD-2-Clause */
/*-
 * Copyright (c) 2026 furuta@furuta.bsdclub.org
 *
 * Submit isochronous transfers to a USB device and report what comes
 * back.
 *
 * This builds and runs on both Linux and FreeBSD, which is the point.
 * On Linux it is used to make the canonical usbip client emit
 * isochronous traffic so the wire format can be captured; the protocol
 * documentation does not describe the isochronous packet descriptor, so
 * observing it is the only way to settle it without reading the GPL
 * implementation.  On FreeBSD it is the test for our own driver.
 *
 *	cc -o iso_probe iso_probe.c -lusb        # FreeBSD
 *	cc -o iso_probe iso_probe.c -lusb-1.0    # Linux
 *
 *	./iso_probe [vendor product]
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#ifdef __FreeBSD__
#include <libusb.h>
#else
#include <libusb-1.0/libusb.h>
#endif

#define	DEFAULT_VENDOR	0x1d6b
#define	DEFAULT_PRODUCT	0x0101

/* Enough transfers in flight to keep a stream going for a moment. */
#define	NTRANSFERS	4
#define	NPACKETS	8
#ifndef NROUNDS
#define	NROUNDS		4	/* -DNROUNDS=n to stream for longer */
#endif

static int completed;
static int inflight;
static int rounds;
static int failures;

struct target {
	int		interface;
	int		altsetting;
	unsigned char	endpoint;
	int		maxp;
	int		is_in;
};

/*
 * Find an alternate setting with an isochronous endpoint.  Audio and
 * video devices put their streaming endpoints on a non-zero alternate
 * setting and leave setting 0 empty, which is why this cannot just look
 * at the active configuration's first interface.
 *
 * An IN endpoint is preferred.  Only a reply to an IN transfer carries
 * data and packet descriptors back, and those descriptors are the whole
 * reason for running this against a real device.
 */
static int
find_iso(libusb_device *dev, struct target *t)
{
	struct libusb_config_descriptor *cfg;
	struct target found;
	int have = 0;
	int error, i, j, k;

	error = libusb_get_active_config_descriptor(dev, &cfg);
	if (error != 0) {
		fprintf(stderr, "config descriptor: %s\n",
		    libusb_strerror(error));
		return (-1);
	}

	for (i = 0; i < cfg->bNumInterfaces; i++) {
		const struct libusb_interface *iface = &cfg->interface[i];

		for (j = 0; j < iface->num_altsetting; j++) {
			const struct libusb_interface_descriptor *alt =
			    &iface->altsetting[j];

			for (k = 0; k < alt->bNumEndpoints; k++) {
				const struct libusb_endpoint_descriptor *ep =
				    &alt->endpoint[k];

				if ((ep->bmAttributes &
				    LIBUSB_TRANSFER_TYPE_MASK) !=
				    LIBUSB_TRANSFER_TYPE_ISOCHRONOUS)
					continue;
				if (ep->wMaxPacketSize == 0)
					continue;

				found.interface = alt->bInterfaceNumber;
				found.altsetting = alt->bAlternateSetting;
				found.endpoint = ep->bEndpointAddress;
				/* Low 11 bits; the rest is the multiplier. */
				found.maxp = ep->wMaxPacketSize & 0x7FF;
				found.is_in = (ep->bEndpointAddress &
				    LIBUSB_ENDPOINT_IN) != 0;

				if (!have || (found.is_in && !t->is_in)) {
					*t = found;
					have = 1;
				}
				if (t->is_in)
					goto done;
			}
		}
	}

done:
	libusb_free_config_descriptor(cfg);
	if (!have) {
		fprintf(stderr, "no isochronous endpoint on this device\n");
		return (-1);
	}
	return (0);
}

static void LIBUSB_CALL
on_done(struct libusb_transfer *xfer)
{
	int shortp = 0, errp = 0, misplaced = 0, i;

	completed++;

	for (i = 0; i < xfer->num_iso_packets; i++) {
		struct libusb_iso_packet_descriptor *p =
		    &xfer->iso_packet_desc[i];
		unsigned char *data;

		if (p->status != LIBUSB_TRANSFER_COMPLETED) {
			errp++;
			continue;
		}
		if (p->actual_length < p->length) {
			shortp++;
			continue;
		}

		/*
		 * tests/fake_usbipd.py fills every byte of packet i with
		 * i, so this catches a packet delivered to the wrong
		 * offset - which is the easiest thing to get wrong, since
		 * the descriptors that say where each one goes arrive
		 * after the data they describe.
		 */
		data = libusb_get_iso_packet_buffer_simple(xfer, i);
		if (data != NULL && (data[0] != (unsigned char)i ||
		    data[p->actual_length - 1] != (unsigned char)i))
			misplaced++;
	}

	printf("  transfer %2d: status %d, %d packets, %d short, %d failed, "
	    "%d misplaced\n", completed, xfer->status, xfer->num_iso_packets,
	    shortp, errp, misplaced);

	if (misplaced != 0 && completed == 1) {
		/* Show what actually arrived, to tell a wrong offset
		 * from data that never got written at all. */
		printf("    first byte of each packet, expected 0,1,2...:");
		for (i = 0; i < xfer->num_iso_packets; i++) {
			unsigned char *d =
			    libusb_get_iso_packet_buffer_simple(xfer, i);
			printf(" %d", d != NULL ? d[0] : -1);
		}
		printf("\n    raw head of the buffer:");
		for (i = 0; i < 8 && i < xfer->length; i++)
			printf(" %02x", xfer->buffer[i]);
		printf("\n");
	}
	if (misplaced != 0)
		failures++;

	if (xfer->status != LIBUSB_TRANSFER_COMPLETED &&
	    xfer->status != LIBUSB_TRANSFER_CANCELLED)
		failures++;

	inflight--;

	/* Keep the stream going for a while, then let it drain. */
	if (rounds < NROUNDS && libusb_submit_transfer(xfer) == 0) {
		rounds++;
		inflight++;
		return;
	}
	libusb_free_transfer(xfer);
}

int
main(int argc, char **argv)
{
	libusb_device_handle *dh;
	struct libusb_transfer *xfers[NTRANSFERS];
	struct target t;
	unsigned char *bufs[NTRANSFERS];
	int vendor = DEFAULT_VENDOR, product = DEFAULT_PRODUCT;
	int error, i, live, buflen, idle = 0;

	if (argc == 3) {
		vendor = (int)strtol(argv[1], NULL, 16);
		product = (int)strtol(argv[2], NULL, 16);
	} else if (argc != 1) {
		fprintf(stderr, "usage: %s [vendor product]\n", argv[0]);
		return (1);
	}

	/*
	 * Line buffer the log.  This test is expected to be killed
	 * partway through, and a block buffered stdout loses whatever
	 * had not been flushed - which once made an interrupted run look
	 * like it had stopped somewhere it had not.
	 */
	setvbuf(stdout, NULL, _IOLBF, 0);

	error = libusb_init(NULL);
	if (error != 0) {
		fprintf(stderr, "libusb_init: %s\n", libusb_strerror(error));
		return (1);
	}

	dh = libusb_open_device_with_vid_pid(NULL, vendor, product);
	if (dh == NULL) {
		fprintf(stderr, "no %04x:%04x found\n", vendor, product);
		libusb_exit(NULL);
		return (1);
	}

	if (find_iso(libusb_get_device(dh), &t) != 0) {
		libusb_close(dh);
		libusb_exit(NULL);
		return (1);
	}
	printf("isochronous endpoint %#04x %s, interface %d alt %d, "
	    "max packet %d\n", t.endpoint, t.is_in ? "IN" : "OUT",
	    t.interface, t.altsetting, t.maxp);

#ifndef __FreeBSD__
	libusb_set_auto_detach_kernel_driver(dh, 1);
#endif
	error = libusb_claim_interface(dh, t.interface);
	if (error != 0) {
		fprintf(stderr, "claim interface %d: %s\n", t.interface,
		    libusb_strerror(error));
		libusb_close(dh);
		libusb_exit(NULL);
		return (1);
	}
	error = libusb_set_interface_alt_setting(dh, t.interface,
	    t.altsetting);
	if (error != 0) {
		fprintf(stderr, "alt setting %d: %s\n", t.altsetting,
		    libusb_strerror(error));
		libusb_release_interface(dh, t.interface);
		libusb_close(dh);
		libusb_exit(NULL);
		return (1);
	}

	buflen = t.maxp * NPACKETS;
	live = 0;
	for (i = 0; i < NTRANSFERS; i++) {
		bufs[i] = calloc(1, buflen);
		xfers[i] = libusb_alloc_transfer(NPACKETS);
		if (bufs[i] == NULL || xfers[i] == NULL) {
			fprintf(stderr, "out of memory\n");
			return (1);
		}
		libusb_fill_iso_transfer(xfers[i], dh, t.endpoint, bufs[i],
		    buflen, NPACKETS, on_done, NULL, 2000);
		libusb_set_iso_packet_lengths(xfers[i], t.maxp);

		error = libusb_submit_transfer(xfers[i]);
		if (error != 0) {
			fprintf(stderr, "submit %d: %s\n", i,
			    libusb_strerror(error));
			failures++;
		} else {
			live++;
			inflight++;
		}
	}
	printf("submitted %d isochronous transfers of %d packets\n", live,
	    NPACKETS);

	/*
	 * Wait on what is actually outstanding rather than on a
	 * predicted total.  An earlier version counted up to a target
	 * that grew as transfers were resubmitted, so a resubmission
	 * that failed left it waiting for completions that could never
	 * arrive.  The idle limit means this can never hang either way.
	 */
	while (inflight > 0 && idle < 5) {
		struct timeval tv = { 1, 0 };
		int before = completed;

		if (libusb_handle_events_timeout(NULL, &tv) != 0)
			break;
		idle = (completed == before) ? idle + 1 : 0;
	}

	libusb_release_interface(dh, t.interface);
	libusb_close(dh);
	libusb_exit(NULL);

	printf("iso_probe: %d transfers completed, %d failed\n", completed,
	    failures);
	return (failures != 0 || completed == 0);
}
