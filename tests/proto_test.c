/* SPDX-License-Identifier: BSD-2-Clause */
/*-
 * Copyright (c) 2026 furuta@furuta.bsdclub.org
 *
 * Golden tests for the USB/IP wire format and the libusbip parsers.
 * Pure userland; feeds canned byte streams to the parsers via pipes.
 */

#include <sys/socket.h>

#include <arpa/inet.h>

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "usbip_net.h"

static int failures;

#define	CHECK(cond) do {						\
	if (!(cond)) {							\
		fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,	\
		    #cond);						\
		failures++;						\
	}								\
} while (0)

/* Build a wire-format usbip_usb_device into buf (312 bytes). */
static void
put_wire_udev(uint8_t *buf, const char *busid, uint32_t busnum,
    uint32_t devnum, uint32_t speed, uint16_t vid, uint16_t pid,
    uint8_t nifaces)
{
	uint32_t be32;
	uint16_t be16;

	memset(buf, 0, 312);
	snprintf((char *)buf, 256, "/sys/devices/fake/%s", busid);
	strncpy((char *)buf + 256, busid, 31);
	be32 = htonl(busnum); memcpy(buf + 288, &be32, 4);
	be32 = htonl(devnum); memcpy(buf + 292, &be32, 4);
	be32 = htonl(speed);  memcpy(buf + 296, &be32, 4);
	be16 = htons(vid);    memcpy(buf + 300, &be16, 2);
	be16 = htons(pid);    memcpy(buf + 302, &be16, 2);
	be16 = htons(0x0100); memcpy(buf + 304, &be16, 2);
	buf[306] = 0x00;	/* bDeviceClass */
	buf[307] = 0x00;	/* bDeviceSubClass */
	buf[308] = 0x00;	/* bDeviceProtocol */
	buf[309] = 1;		/* bConfigurationValue */
	buf[310] = 1;		/* bNumConfigurations */
	buf[311] = nifaces;	/* bNumInterfaces */
}

static void
test_struct_layout(void)
{
	struct usbip_header hdr;

	/* Offsets of the command-specific part must land at byte 20. */
	CHECK((size_t)((char *)&hdr.u - (char *)&hdr) == 20);
	CHECK(sizeof(hdr.u.cmd_submit) == 28);
	CHECK(sizeof(hdr.u.ret_submit) == 28);
	CHECK(sizeof(hdr.u.cmd_unlink) == 28);
	CHECK(sizeof(hdr.u.ret_unlink) == 28);
	CHECK(USBIP_DEVID(3, 7) == 0x00030007);
}

static void
test_op_common_roundtrip(void)
{
	struct usbip_op_common op;
	uint8_t expect[8] = { 0x01, 0x11, 0x80, 0x05, 0x00, 0x00, 0x00, 0x00 };
	int fds[2];
	uint32_t status;

	/* Encode: what usbip_net_send_op_common(OP_REQ_DEVLIST) emits. */
	CHECK(pipe(fds) == 0);
	CHECK(usbip_net_send_op_common(fds[1], OP_REQ_DEVLIST) == 0);
	CHECK(usbip_net_recv_exact(fds[0], &op, sizeof(op)) == 0);
	CHECK(memcmp(&op, expect, sizeof(expect)) == 0);

	/* Decode: a well-formed OP_REP_DEVLIST header. */
	op.version = htons(0x0111);
	op.code = htons(OP_REP_DEVLIST);
	op.status = htonl(0);
	CHECK(write(fds[1], &op, sizeof(op)) == sizeof(op));
	CHECK(usbip_net_recv_op_common(fds[0], OP_REP_DEVLIST, &status) == 0);
	CHECK(status == 0);

	/* Decode: server-side failure must be reported. */
	op.status = htonl(USBIP_ST_NA);
	CHECK(write(fds[1], &op, sizeof(op)) == sizeof(op));
	CHECK(usbip_net_recv_op_common(fds[0], OP_REP_DEVLIST, &status) == -1);
	CHECK(status == USBIP_ST_NA);

	close(fds[0]);
	close(fds[1]);
}

static void
test_devlist_parse(void)
{
	struct usbip_devlist_entry *entries;
	uint8_t blob[8 + 4 + 312 + 2 * 4];
	uint8_t reqbuf[8];
	uint32_t be32, count;
	int sv[2];

	/* OP_REP_DEVLIST with one high-speed device with 2 interfaces. */
	blob[0] = 0x01; blob[1] = 0x11;			/* version */
	blob[2] = 0x00; blob[3] = 0x05;			/* OP_REP_DEVLIST */
	memset(blob + 4, 0, 4);				/* status OK */
	be32 = htonl(1); memcpy(blob + 8, &be32, 4);	/* n devices */
	put_wire_udev(blob + 12, "1-2", 1, 5, USBIP_SPEED_HIGH,
	    0x1234, 0x5678, 2);
	/* if0: mass storage 08/06/50, if1: vendor ff/00/00 */
	blob[324] = 0x08; blob[325] = 0x06; blob[326] = 0x50; blob[327] = 0;
	blob[328] = 0xff; blob[329] = 0x00; blob[330] = 0x00; blob[331] = 0;

	/* Preload the canned reply; the socket buffers it. */
	CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
	CHECK(write(sv[1], blob, sizeof(blob)) == (ssize_t)sizeof(blob));
	CHECK(usbip_devlist_fetch(sv[0], &entries, &count) == 0);
	/* The 8-byte OP_REQ_DEVLIST must have been sent on the socket. */
	CHECK(read(sv[1], reqbuf, sizeof(reqbuf)) == 8);
	CHECK(reqbuf[2] == 0x80 && reqbuf[3] == 0x05);
	close(sv[0]);
	close(sv[1]);

	CHECK(count == 1);
	CHECK(strcmp(entries[0].udev.busid, "1-2") == 0);
	CHECK(entries[0].udev.busnum == 1);
	CHECK(entries[0].udev.devnum == 5);
	CHECK(entries[0].udev.speed == USBIP_SPEED_HIGH);
	CHECK(entries[0].udev.idVendor == 0x1234);
	CHECK(entries[0].udev.idProduct == 0x5678);
	CHECK(entries[0].udev.bNumInterfaces == 2);
	CHECK(entries[0].intfs != NULL);
	CHECK(entries[0].intfs[0].bInterfaceClass == 0x08);
	CHECK(entries[0].intfs[1].bInterfaceClass == 0xff);
	usbip_devlist_free(entries, count);
}

static void
test_import_parse(void)
{
	struct usbip_usb_device udev;
	uint8_t blob[8 + 312];
	uint8_t reqbuf[8 + 32];
	int sv[2];

	blob[0] = 0x01; blob[1] = 0x11;
	blob[2] = 0x00; blob[3] = 0x03;			/* OP_REP_IMPORT */
	memset(blob + 4, 0, 4);
	put_wire_udev(blob + 8, "2-1.4", 2, 9, USBIP_SPEED_FULL,
	    0x046d, 0xc077, 1);

	CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
	CHECK(write(sv[1], blob, sizeof(blob)) == (ssize_t)sizeof(blob));
	CHECK(usbip_import_device(sv[0], "2-1.4", &udev) == 0);
	/* Request on the wire: op_common + 32-byte busid. */
	CHECK(read(sv[1], reqbuf, sizeof(reqbuf)) ==
	    (ssize_t)sizeof(reqbuf));
	CHECK(reqbuf[2] == 0x80 && reqbuf[3] == 0x03);
	CHECK(strcmp((char *)reqbuf + 8, "2-1.4") == 0);
	CHECK(reqbuf[8 + 31] == 0);
	close(sv[0]);
	close(sv[1]);

	CHECK(strcmp(udev.busid, "2-1.4") == 0);
	CHECK(udev.devnum == 9);
	CHECK(udev.idVendor == 0x046d);
	CHECK(USBIP_DEVID(udev.busnum, udev.devnum) == 0x00020009);
}

int
main(void)
{

	test_struct_layout();
	test_op_common_roundtrip();
	test_devlist_parse();
	test_import_parse();

	if (failures == 0) {
		printf("proto_test: all tests passed\n");
		return (0);
	}
	fprintf(stderr, "proto_test: %d failure(s)\n", failures);
	return (1);
}
