/* SPDX-License-Identifier: BSD-2-Clause */
/*-
 * Copyright (c) 2026 furuta@furuta.bsdclub.org
 *
 * Drive transfers through a USB device attached over USB/IP.
 *
 * Endpoints are discovered from the device's own descriptors rather
 * than assumed, so the same test runs against tests/fake_usbipd.py and
 * against Linux's g_zero gadget, which number their endpoints
 * differently.
 *
 * The device must loop its bulk OUT endpoint back to its bulk IN
 * endpoint: that is what fake_usbipd.py emulates, and what g_zero does
 * in its "loop input to output" configuration (loopdefault=1).
 *
 * Build with libusb, which is in the FreeBSD base system:
 *	cc -o bulk_test bulk_test.c -lusb
 *
 *	./bulk_test              # the emulated device
 *	./bulk_test 0525 a4a0    # Linux's g_zero gadget
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <inttypes.h>
#include <string.h>

#include <libusb.h>

#define	TEST_VENDOR	0x1209
#define	TEST_PRODUCT	0x0001
#define	TIMEOUT_MS	5000

static int failures;

static unsigned char ep_bulk_in, ep_bulk_out, ep_intr_in;
static int intf_number = -1;
static int bulk_maxp;

#define	CHECK(cond, ...) do {						\
	if (!(cond)) {							\
		fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);	\
		fprintf(stderr, __VA_ARGS__);				\
		fprintf(stderr, "\n");					\
		failures++;						\
	}								\
} while (0)

/*
 * Pick the first interface that offers both a bulk IN and a bulk OUT
 * endpoint, and note an interrupt IN endpoint if it has one.
 */
static int
find_endpoints(libusb_device_handle *dh)
{
	struct libusb_config_descriptor *cfg;
	libusb_device *dev = libusb_get_device(dh);
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
			unsigned char in = 0, out = 0, intr = 0;

			for (k = 0; k < alt->bNumEndpoints; k++) {
				const struct libusb_endpoint_descriptor *ep =
				    &alt->endpoint[k];
				int type = ep->bmAttributes &
				    LIBUSB_TRANSFER_TYPE_MASK;
				int is_in = (ep->bEndpointAddress &
				    LIBUSB_ENDPOINT_IN) != 0;

				if (type == LIBUSB_TRANSFER_TYPE_BULK) {
					if (is_in && in == 0) {
						in = ep->bEndpointAddress;
					} else if (!is_in && out == 0) {
						out = ep->bEndpointAddress;
						bulk_maxp = ep->wMaxPacketSize;
					}
				} else if (type ==
				    LIBUSB_TRANSFER_TYPE_INTERRUPT &&
				    is_in && intr == 0) {
					intr = ep->bEndpointAddress;
				}
			}
			if (in != 0 && out != 0) {
				ep_bulk_in = in;
				ep_bulk_out = out;
				ep_intr_in = intr;
				intf_number = alt->bInterfaceNumber;
				libusb_free_config_descriptor(cfg);
				return (0);
			}
		}
	}

	libusb_free_config_descriptor(cfg);
	fprintf(stderr, "no interface with both bulk IN and bulk OUT\n");
	return (-1);
}

/* Write len bytes of a repeatable pattern, read them back, compare. */
static void
loopback(libusb_device_handle *dh, int len)
{
	unsigned char *out, *in;
	int transferred, error, i;
	int zlp_sent = 0;

	out = malloc(len);
	in = malloc(len);
	if (out == NULL || in == NULL) {
		fprintf(stderr, "out of memory\n");
		exit(1);
	}
	for (i = 0; i < len; i++)
		out[i] = (unsigned char)(i * 7 + 13);
	memset(in, 0, len);

	transferred = 0;
	error = libusb_bulk_transfer(dh, ep_bulk_out, out, len, &transferred,
	    TIMEOUT_MS);
	CHECK(error == 0, "%d byte write: %s", len, libusb_strerror(error));
	CHECK(transferred == len, "%d byte write moved %d", len, transferred);
	if (error != 0)
		goto out;

	/*
	 * A write whose length is an exact multiple of the maximum
	 * packet size gives the device no short packet to mark the end
	 * of the transfer, so it keeps waiting for more.  Terminate it
	 * with a zero length packet, as any USB host must.  The emulated
	 * device does not need this - it takes the length from the
	 * USB/IP header - but a real one does.
	 */
	if (bulk_maxp > 0 && len % bulk_maxp == 0) {
		transferred = 0;
		error = libusb_bulk_transfer(dh, ep_bulk_out, out, 0,
		    &transferred, TIMEOUT_MS);
		CHECK(error == 0, "%d byte terminating ZLP: %s", len,
		    libusb_strerror(error));
		if (error != 0)
			goto out;
		zlp_sent = 1;
	}

	transferred = 0;
	error = libusb_bulk_transfer(dh, ep_bulk_in, in, len, &transferred,
	    TIMEOUT_MS);
	CHECK(error == 0, "%d byte read: %s", len, libusb_strerror(error));
	CHECK(transferred == len, "%d byte read moved %d", len, transferred);
	if (error != 0)
		goto out;

	CHECK(memcmp(out, in, len) == 0, "%d byte round trip differs", len);

	/*
	 * A loopback device echoes the terminating zero length packet
	 * too, so consume it.  Leaving it queued would make the next
	 * read return zero bytes and put every later transfer out of
	 * step with its reply.
	 */
	if (zlp_sent) {
		transferred = -1;
		error = libusb_bulk_transfer(dh, ep_bulk_in, in, len,
		    &transferred, TIMEOUT_MS);
		CHECK(error == 0, "%d byte echoed ZLP: %s", len,
		    libusb_strerror(error));
		CHECK(transferred == 0, "%d byte echoed ZLP moved %d", len,
		    transferred);
	}

	printf("  %7d bytes: ok\n", len);
out:
	free(out);
	free(in);
}

/*
 * The emulated device's interrupt endpoint returns an incrementing
 * counter, so consecutive reads must come back in order.  That catches
 * a transfer engine that mismatches replies to requests as well as one
 * that fails outright.
 */
static void
interrupt_reads(libusb_device_handle *dh, int count)
{
	unsigned char buf[8];
	uint64_t got, expect = 0;
	int transferred, error, i;

	for (i = 0; i < count; i++) {
		memset(buf, 0, sizeof(buf));
		transferred = 0;
		error = libusb_interrupt_transfer(dh, ep_intr_in, buf,
		    sizeof(buf), &transferred, TIMEOUT_MS);
		CHECK(error == 0, "interrupt read %d: %s", i,
		    libusb_strerror(error));
		if (error != 0)
			return;
		CHECK(transferred == (int)sizeof(buf),
		    "interrupt read %d moved %d", i, transferred);

		memcpy(&got, buf, sizeof(got));
		if (i == 0)
			expect = got;
		CHECK(got == expect, "interrupt read %d out of order: "
		    "got %ju, expected %ju", i, (uintmax_t)got,
		    (uintmax_t)expect);
		expect = got + 1;
	}
	printf("  %d interrupt reads, in order: ok\n", count);
}

int
main(int argc, char **argv)
{
	libusb_device_handle *dh;
	int vendor = TEST_VENDOR, product = TEST_PRODUCT;
	int error;

	/* Point the test at another device, e.g. Linux's g_zero gadget. */
	if (argc == 3) {
		vendor = (int)strtol(argv[1], NULL, 16);
		product = (int)strtol(argv[2], NULL, 16);
	} else if (argc != 1) {
		fprintf(stderr, "usage: %s [vendor product]\n", argv[0]);
		return (1);
	}

	error = libusb_init(NULL);
	if (error != 0) {
		fprintf(stderr, "libusb_init: %s\n", libusb_strerror(error));
		return (1);
	}

	dh = libusb_open_device_with_vid_pid(NULL, vendor, product);
	if (dh == NULL) {
		fprintf(stderr,
		    "no %04x:%04x found; is the device attached?\n",
		    vendor, product);
		libusb_exit(NULL);
		return (1);
	}

	if (find_endpoints(dh) != 0) {
		libusb_close(dh);
		libusb_exit(NULL);
		return (1);
	}
	printf("interface %d: bulk in %#04x, bulk out %#04x", intf_number,
	    ep_bulk_in, ep_bulk_out);
	if (ep_intr_in != 0)
		printf(", interrupt in %#04x", ep_intr_in);
	printf("\n");

	error = libusb_claim_interface(dh, intf_number);
	if (error != 0) {
		fprintf(stderr, "claim interface %d: %s\n", intf_number,
		    libusb_strerror(error));
		libusb_close(dh);
		libusb_exit(NULL);
		return (1);
	}

	printf("bulk loopback through USB/IP:\n");
	/*
	 * Sizes chosen around the 512 byte max packet and the driver's
	 * 64 KiB staging buffer, so both the single-chunk and the
	 * multi-chunk paths get used, including an exact multiple of
	 * the packet size.
	 */
	loopback(dh, 1);
	loopback(dh, 64);
	loopback(dh, 512);
	loopback(dh, 513);
	loopback(dh, 4096);
	loopback(dh, 65536);
	loopback(dh, 100000);

	if (ep_intr_in != 0)
		interrupt_reads(dh, 32);
	else
		printf("  (no interrupt endpoint on this device)\n");

	libusb_release_interface(dh, intf_number);
	libusb_close(dh);
	libusb_exit(NULL);

	if (failures == 0) {
		printf("bulk_test: all transfers passed\n");
		return (0);
	}
	fprintf(stderr, "bulk_test: %d failure(s)\n", failures);
	return (1);
}
