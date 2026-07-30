/* SPDX-License-Identifier: BSD-2-Clause */
/*-
 * Copyright (c) 2026 furuta@furuta.bsdclub.org
 *
 * USB/IP wire-protocol definitions, shared between the vhci(4) kernel
 * driver and the usbip(8) userland tool.
 *
 * This header was written from scratch based on the protocol description
 * in the Linux kernel documentation (Documentation/usb/usbip_protocol.rst)
 * and on-the-wire observation.  It contains no Linux kernel code.
 *
 * All multi-byte fields are big-endian on the wire.  The structures below
 * are declared in wire order; byte-order conversion is the caller's job.
 */

#ifndef _USBIP_PROTO_H_
#define	_USBIP_PROTO_H_

#ifdef _KERNEL
#include <sys/types.h>
#else
#include <stdint.h>
#endif

#ifndef __packed
#define	__packed	__attribute__((__packed__))
#endif

/*
 * Protocol version spoken by current Linux tools and usbipd-win.
 * Sent in the op_common header of every userspace-level request/reply.
 */
#define	USBIP_PROTO_VERSION	0x0111
#define	USBIP_PORT		3240
#define	USBIP_PORT_STRING	"3240"

/*
 * Userspace-level (handshake) operation codes.  The high bit of the
 * upper byte distinguishes request (0x80) from reply (0x00).
 */
#define	OP_REQ_DEVLIST	0x8005
#define	OP_REP_DEVLIST	0x0005
#define	OP_REQ_IMPORT	0x8003
#define	OP_REP_IMPORT	0x0003

/* op_common status values. */
#define	USBIP_ST_OK	0x00
#define	USBIP_ST_NA	0x01	/* generic "not available" / error */

#define	USBIP_BUSID_SIZE	32
#define	USBIP_DEV_PATH_SIZE	256

/* Prepended to every handshake request and reply.  8 bytes. */
struct usbip_op_common {
	uint16_t	version;
	uint16_t	code;
	uint32_t	status;
} __packed;

/*
 * Device description used in OP_REP_DEVLIST and OP_REP_IMPORT.
 * 312 bytes.  Descriptor-derived fields keep their USB spec names.
 */
struct usbip_usb_device {
	char		path[USBIP_DEV_PATH_SIZE];
	char		busid[USBIP_BUSID_SIZE];
	uint32_t	busnum;
	uint32_t	devnum;
	uint32_t	speed;		/* USBIP_SPEED_* */
	uint16_t	idVendor;
	uint16_t	idProduct;
	uint16_t	bcdDevice;
	uint8_t		bDeviceClass;
	uint8_t		bDeviceSubClass;
	uint8_t		bDeviceProtocol;
	uint8_t		bConfigurationValue;
	uint8_t		bNumConfigurations;
	uint8_t		bNumInterfaces;
} __packed;

/* Follows usbip_usb_device in OP_REP_DEVLIST, one per interface.  4 bytes. */
struct usbip_usb_interface {
	uint8_t		bInterfaceClass;
	uint8_t		bInterfaceSubClass;
	uint8_t		bInterfaceProtocol;
	uint8_t		padding;
} __packed;

/*
 * Device speed values (Linux enum usb_device_speed encoding — part of
 * the wire protocol).
 */
#define	USBIP_SPEED_UNKNOWN	0
#define	USBIP_SPEED_LOW		1
#define	USBIP_SPEED_FULL	2
#define	USBIP_SPEED_HIGH	3
#define	USBIP_SPEED_WIRELESS	4
#define	USBIP_SPEED_SUPER	5
#define	USBIP_SPEED_SUPER_PLUS	6

/*
 * URB-level commands, exchanged on the same TCP connection after a
 * successful OP_REQ_IMPORT.  Every PDU starts with a 48-byte header:
 * usbip_header_basic (20 bytes) + a 28-byte command-specific part
 * (padded to 28 where shorter).
 */
#define	USBIP_CMD_SUBMIT	0x0001
#define	USBIP_CMD_UNLINK	0x0002
#define	USBIP_RET_SUBMIT	0x0003
#define	USBIP_RET_UNLINK	0x0004

#define	USBIP_DIR_OUT		0
#define	USBIP_DIR_IN		1

/* devid identifies the exported device: busnum << 16 | devnum. */
#define	USBIP_DEVID(busnum, devnum) \
	((((uint32_t)(busnum)) << 16) | ((uint32_t)(devnum)))

struct usbip_header_basic {
	uint32_t	command;
	uint32_t	seqnum;		/* nonzero, allocated by client */
	uint32_t	devid;
	uint32_t	direction;	/* USBIP_DIR_* */
	uint32_t	ep;		/* endpoint number, 0-15 */
} __packed;

struct usbip_cmd_submit {
	uint32_t	transfer_flags;		/* USBIP_URB_* */
	int32_t		transfer_buffer_length;
	int32_t		start_frame;		/* ISO only, else 0 */
	int32_t		number_of_packets;	/* ISO only, else 0 */
	int32_t		interval;
	uint8_t		setup[8];		/* control only, else zero */
} __packed;

struct usbip_ret_submit {
	int32_t		status;		/* 0 or negative Linux errno */
	int32_t		actual_length;
	int32_t		start_frame;
	int32_t		number_of_packets;
	int32_t		error_count;
	uint8_t		padding[8];
} __packed;

struct usbip_cmd_unlink {
	uint32_t	seqnum;		/* seqnum of the SUBMIT to cancel */
	uint8_t		padding[24];
} __packed;

struct usbip_ret_unlink {
	int32_t		status;		/* -ECONNRESET(-104) if unlinked */
	uint8_t		padding[24];
} __packed;

struct usbip_header {
	struct usbip_header_basic base;
	union {
		struct usbip_cmd_submit	cmd_submit;
		struct usbip_ret_submit	ret_submit;
		struct usbip_cmd_unlink	cmd_unlink;
		struct usbip_ret_unlink	ret_unlink;
	} u;
} __packed;

/*
 * ISO transfers carry an array of these after the data payload
 * (RET_SUBMIT: after the IN data; CMD_SUBMIT: after the OUT data).
 */
struct usbip_iso_packet_descriptor {
	uint32_t	offset;
	uint32_t	length;
	uint32_t	actual_length;
	int32_t		status;
} __packed;

/*
 * URB transfer_flags bits (Linux URB_* encoding — part of the wire
 * protocol).  Only the bits meaningful across the wire are defined.
 */
#define	USBIP_URB_SHORT_NOT_OK	0x0001	/* short IN transfer is an error */
#define	USBIP_URB_ISO_ASAP	0x0002
#define	USBIP_URB_ZERO_PACKET	0x0040	/* end OUT with a short packet */
#define	USBIP_URB_NO_INTERRUPT	0x0080

/*
 * URB status values: negative Linux errno numbers cross the wire in
 * ret_submit.status / ret_unlink.status.  A non-Linux implementation
 * must translate; never feed local errno values to these fields.
 */
#define	USBIP_E_OK		0
#define	USBIP_E_ENOENT		(-2)	/* URB unlinked (usb_kill_urb) */
#define	USBIP_E_EXDEV		(-18)	/* ISO: partial completion */
#define	USBIP_E_ENODEV		(-19)	/* device gone */
#define	USBIP_E_EPIPE		(-32)	/* endpoint stalled */
#define	USBIP_E_EPROTO		(-71)	/* low-level bus error */
#define	USBIP_E_EOVERFLOW	(-75)	/* babble */
#define	USBIP_E_EILSEQ		(-84)	/* CRC/bitstuff error */
#define	USBIP_E_ECONNRESET	(-104)	/* URB unlinked (async) */
#define	USBIP_E_ESHUTDOWN	(-108)	/* host controller gone */
#define	USBIP_E_ETIMEDOUT	(-110)	/* transfer timed out */
#define	USBIP_E_EINPROGRESS	(-115)	/* URB still pending */
#define	USBIP_E_EREMOTEIO	(-121)	/* short read w/ SHORT_NOT_OK */

/* Wire-format invariants. */
_Static_assert(sizeof(struct usbip_op_common) == 8, "op_common size");
_Static_assert(sizeof(struct usbip_usb_device) == 312, "usb_device size");
_Static_assert(sizeof(struct usbip_usb_interface) == 4, "usb_interface size");
_Static_assert(sizeof(struct usbip_header_basic) == 20, "header_basic size");
_Static_assert(sizeof(struct usbip_header) == 48, "usbip_header size");
_Static_assert(sizeof(struct usbip_iso_packet_descriptor) == 16,
    "iso_packet_descriptor size");

#endif /* !_USBIP_PROTO_H_ */
