/* SPDX-License-Identifier: BSD-2-Clause */
/*-
 * Copyright (c) 2026 furuta@furuta.bsdclub.org
 *
 * Drive bulk transfers through an attached USB/IP device.
 *
 * Pairs with tests/fake_usbipd.py, whose emulated device loops its bulk
 * OUT endpoint back to its bulk IN endpoint.  Writing a pattern and
 * reading it back exercises the whole path: usbip(8) hands the socket
 * to vhci(4), the transfer engine turns the transfer into a
 * USBIP_CMD_SUBMIT, and the reply comes back through the receive
 * thread.
 *
 * Build with libusb, which is in the FreeBSD base system:
 *	cc -o bulk_test bulk_test.c -lusb
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libusb.h>

#define	TEST_VENDOR	0x1209
#define	TEST_PRODUCT	0x0001
#define	EP_BULK_IN	0x81
#define	EP_BULK_OUT	0x02
#define	TIMEOUT_MS	5000

static int failures;

#define	CHECK(cond, ...) do {						\
	if (!(cond)) {							\
		fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);	\
		fprintf(stderr, __VA_ARGS__);				\
		fprintf(stderr, "\n");					\
		failures++;						\
	}								\
} while (0)

/* Write len bytes of a repeatable pattern, read them back, compare. */
static void
loopback(libusb_device_handle *dh, int len)
{
	unsigned char *out, *in;
	int transferred, error, i;

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
	error = libusb_bulk_transfer(dh, EP_BULK_OUT, out, len, &transferred,
	    TIMEOUT_MS);
	CHECK(error == 0, "%d byte write: %s", len, libusb_strerror(error));
	CHECK(transferred == len, "%d byte write moved %d", len, transferred);
	if (error != 0)
		goto out;

	transferred = 0;
	error = libusb_bulk_transfer(dh, EP_BULK_IN, in, len, &transferred,
	    TIMEOUT_MS);
	CHECK(error == 0, "%d byte read: %s", len, libusb_strerror(error));
	CHECK(transferred == len, "%d byte read moved %d", len, transferred);
	if (error != 0)
		goto out;

	CHECK(memcmp(out, in, len) == 0, "%d byte round trip differs", len);
	printf("  %7d bytes: ok\n", len);
out:
	free(out);
	free(in);
}

int
main(void)
{
	libusb_device_handle *dh;
	int error;

	error = libusb_init(NULL);
	if (error != 0) {
		fprintf(stderr, "libusb_init: %s\n", libusb_strerror(error));
		return (1);
	}

	dh = libusb_open_device_with_vid_pid(NULL, TEST_VENDOR, TEST_PRODUCT);
	if (dh == NULL) {
		fprintf(stderr,
		    "no %04x:%04x found; is the device attached?\n",
		    TEST_VENDOR, TEST_PRODUCT);
		libusb_exit(NULL);
		return (1);
	}

	error = libusb_claim_interface(dh, 0);
	if (error != 0) {
		fprintf(stderr, "claim: %s\n", libusb_strerror(error));
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

	libusb_release_interface(dh, 0);
	libusb_close(dh);
	libusb_exit(NULL);

	if (failures == 0) {
		printf("bulk_test: all transfers passed\n");
		return (0);
	}
	fprintf(stderr, "bulk_test: %d failure(s)\n", failures);
	return (1);
}
