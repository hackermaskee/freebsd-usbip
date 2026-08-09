#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 furuta@furuta.bsdclub.org
#
# fake_usbipd: a USB/IP server that emulates a USB device in software.
#
# It answers the handshake and then serves the URB phase, emulating a
# vendor-specific high-speed device with a bulk IN and a bulk OUT
# endpoint that loop back to each other.  That is enough for a client to
# enumerate the device completely and move bulk data, with no hardware
# and no privileges anywhere.
#
#	tests/fake_usbipd.py -v
#	usbip attach -r <host> -b 1-1

import argparse
import socket
import struct
import sys

USBIP_VERSION = 0x0111

OP_REQ_DEVLIST = 0x8005
OP_REP_DEVLIST = 0x0005
OP_REQ_IMPORT = 0x8003
OP_REP_IMPORT = 0x0003
ST_OK = 0
ST_NA = 1

CMD_SUBMIT = 1
CMD_UNLINK = 2
RET_SUBMIT = 3
RET_UNLINK = 4

DIR_OUT = 0
DIR_IN = 1

# Negative Linux errno values, as they appear on the wire.
E_OK = 0
E_PIPE = -32          # stalled
E_CONNRESET = -104    # unlinked

SPEED_HIGH = 3

BUSID = b"1-1"
BUSNUM = 1
DEVNUM = 2
VENDOR = 0x1209       # pid.codes, "for testing only"
PRODUCT = 0x0001

EP_BULK_IN = 1
EP_BULK_OUT = 2
EP_INTR_IN = 3
EP_ISO_IN = 4
BULK_MAXP = 512
INTR_MAXP = 64
INTR_LEN = 8
ISO_MAXP = 192

# bInterval on the interrupt endpoint; at high speed the stack turns
# this into 2^(4-4) = 1 ms, which a Linux URB counts as 8 microframes.
INTR_BINTERVAL = 4
# On an isochronous endpoint bInterval is the exponent directly, so 1
# means every microframe.  Deliberately different from the interrupt
# endpoint's so the two are told apart in the log.
ISO_BINTERVAL = 1

NUM_INTERFACES = 2


def le16(v):
    return struct.pack("<H", v)


DEVICE_DESC = (
    bytes([18, 0x01])
    + le16(0x0200)          # bcdUSB 2.0
    + bytes([0xFF, 0x00, 0x00, 64])   # vendor class, bMaxPacketSize0
    + le16(VENDOR) + le16(PRODUCT) + le16(0x0100)
    + bytes([1, 2, 3, 1])   # iManufacturer, iProduct, iSerial, bNumConfigurations
)

# Interface 0 carries the bulk and interrupt endpoints.  Interface 1
# holds the isochronous one on alternate setting 1, leaving setting 0
# empty, which is how audio and video devices are arranged and what
# makes the host issue SET_INTERFACE before streaming.
_IFACE0 = (
    bytes([9, 0x04, 0, 0, 3, 0xFF, 0x00, 0x00, 0])
    + bytes([7, 0x05, 0x80 | EP_BULK_IN, 0x02]) + le16(BULK_MAXP) + bytes([0])
    + bytes([7, 0x05, EP_BULK_OUT, 0x02]) + le16(BULK_MAXP) + bytes([0])
    + bytes([7, 0x05, 0x80 | EP_INTR_IN, 0x03]) + le16(INTR_MAXP)
    + bytes([INTR_BINTERVAL])
)
_IFACE1_ALT0 = bytes([9, 0x04, 1, 0, 0, 0xFF, 0x00, 0x00, 0])
_IFACE1_ALT1 = (
    bytes([9, 0x04, 1, 1, 1, 0xFF, 0x00, 0x00, 0])
    + bytes([7, 0x05, 0x80 | EP_ISO_IN, 0x01]) + le16(ISO_MAXP)
    + bytes([ISO_BINTERVAL])
)

_CONFIG_BODY = _IFACE0 + _IFACE1_ALT0 + _IFACE1_ALT1
CONFIG_DESC = (
    bytes([9, 0x02]) + le16(9 + len(_CONFIG_BODY))
    + bytes([2, 1, 0, 0xC0, 0])       # 2 ifaces, cfg 1, self powered
    + _CONFIG_BODY
)


def string_desc(s):
    body = s.encode("utf-16-le")
    return bytes([len(body) + 2, 0x03]) + body


STRINGS = {
    0: bytes([4, 0x03]) + le16(0x0409),   # language table: en-US
    1: string_desc("FreeBSD USB/IP test"),
    2: string_desc("Emulated bulk loopback"),
    3: string_desc("0001"),
}


def pack_udev():
    path = b"/sys/devices/fake/usb1/1-1"
    buf = struct.pack("256s32s", path, BUSID)
    buf += struct.pack(">III", BUSNUM, DEVNUM, SPEED_HIGH)
    buf += struct.pack(">HHH", VENDOR, PRODUCT, 0x0100)
    buf += struct.pack("BBBBBB", 0xFF, 0x00, 0x00, 1, 1, NUM_INTERFACES)
    assert len(buf) == 312
    return buf


def op_common(code, status=ST_OK):
    return struct.pack(">HHI", USBIP_VERSION, code, status)


def recv_exact(conn, n):
    buf = b""
    while len(buf) < n:
        chunk = conn.recv(n - len(buf))
        if not chunk:
            raise ConnectionError("peer closed")
        buf += chunk
    return buf


class Device:
    """The emulated USB device's own state."""

    def __init__(self, log):
        self.log = log
        self.configuration = 0
        self.loopback = bytearray()
        self.intr_counter = 0
        self.iso_counter = 0

    def control(self, setup, out_data, length):
        """Return (status, in_data) for a control transfer."""
        bmRequestType, bRequest, wValue, wIndex, wLength = struct.unpack(
            "<BBHHH", setup)
        is_in = (bmRequestType & 0x80) != 0
        self.log(f"  control {bmRequestType:#04x} {bRequest:#04x} "
                 f"wValue={wValue:#06x} wIndex={wIndex} wLength={wLength}")

        # GET_DESCRIPTOR
        if is_in and bRequest == 0x06:
            dtype, dindex = wValue >> 8, wValue & 0xFF
            if dtype == 1:
                return E_OK, DEVICE_DESC[:wLength]
            if dtype == 2:
                return E_OK, CONFIG_DESC[:wLength]
            if dtype == 3:
                d = STRINGS.get(dindex)
                if d is None:
                    return E_PIPE, b""
                return E_OK, d[:wLength]
            # Device qualifier and anything else: stall, like a real
            # high-speed-only device does.
            return E_PIPE, b""

        if not is_in and bRequest == 0x05:      # SET_ADDRESS
            # Should never reach us: the client is expected to absorb it.
            self.log("  !! SET_ADDRESS arrived over the wire")
            return E_OK, b""

        if not is_in and bRequest == 0x09:      # SET_CONFIGURATION
            self.configuration = wValue
            return E_OK, b""

        if is_in and bRequest == 0x08:          # GET_CONFIGURATION
            return E_OK, bytes([self.configuration])

        if is_in and bRequest == 0x00:          # GET_STATUS
            return E_OK, bytes([1, 0])          # self powered

        if not is_in and bRequest == 0x01:      # CLEAR_FEATURE
            return E_OK, b""

        if not is_in and bRequest == 0x0B:      # SET_INTERFACE
            return E_OK, b""

        self.log("  unsupported request, stalling")
        return E_PIPE, b""

    def bulk_out(self, data):
        self.loopback.extend(data)
        return E_OK

    def bulk_in(self, length):
        take = min(length, len(self.loopback))
        data = bytes(self.loopback[:take])
        del self.loopback[:take]
        return E_OK, data

    def isochronous_in(self, packets):
        """
        Fill each requested packet with a recognisable pattern.

        Only the bytes actually transferred go on the wire, packed
        together; the descriptors say where each packet belongs in the
        client's buffer.  Every packet is filled here, so the two
        layouts happen to agree, but the client must not depend on it.
        """
        out = []
        for i, (offset, length) in enumerate(packets):
            # Every byte of packet i is i.  That way a client that
            # scatters a packet to the wrong offset sees the wrong
            # value, rather than data that merely looks plausible.
            body = bytes([i & 0xFF]) * length
            out.append((offset, length, body))
        self.iso_counter += 1
        return E_OK, out

    def interrupt_in(self, length):
        """A counter, so the client can tell one report from the next."""
        data = struct.pack("<Q", self.intr_counter)[:min(length, INTR_LEN)]
        self.intr_counter += 1
        return E_OK, data


class Session:
    def __init__(self, conn, verbose):
        self.conn = conn
        self.verbose = verbose
        self.dev = Device(self.log)

    def log(self, msg):
        if self.verbose:
            print(msg, file=sys.stderr, flush=True)

    def serve_urbs(self):
        while True:
            hdr = recv_exact(self.conn, 48)
            command, seqnum, devid, direction, ep = struct.unpack(
                ">IIIII", hdr[:20])

            if command == CMD_UNLINK:
                target = struct.unpack(">I", hdr[20:24])[0]
                self.log(f"CMD_UNLINK seq={seqnum} target={target}")
                # Nothing is ever queued, so the transfer has always
                # already completed by the time an unlink arrives.
                self.send_ret_unlink(seqnum, E_OK)
                continue

            if command != CMD_SUBMIT:
                raise ConnectionError(f"unexpected command {command:#x}")

            (flags, buflen, start_frame, npackets,
             interval) = struct.unpack(">IiiiI", hdr[20:40])
            setup = hdr[40:48]

            self.log(f"CMD_SUBMIT seq={seqnum} ep={ep} "
                     f"dir={'IN' if direction else 'OUT'} len={buflen} "
                     f"flags={flags:#x} npkt={npackets} "
                     f"interval={interval}")

            out_data = b""
            if direction == DIR_OUT and buflen > 0:
                out_data = recv_exact(self.conn, buflen)

            # Isochronous descriptors follow the payload, one per packet.
            packets = None
            if npackets > 0:
                raw = recv_exact(self.conn, npackets * 16)
                packets = []
                for i in range(npackets):
                    o, ln, _al, _st = struct.unpack(
                        ">IIIi", raw[i * 16:(i + 1) * 16])
                    packets.append((o, ln))
                self.log(f"  {npackets} packets, first offset={packets[0][0]} "
                         f"length={packets[0][1]}")

            if packets is not None:
                if direction == DIR_IN and ep == EP_ISO_IN:
                    status, iso = self.dev.isochronous_in(packets)
                else:
                    self.log(f"  no isochronous endpoint {ep}, stalling")
                    status, iso = E_PIPE, [(o, ln, b"") for o, ln in packets]
                self.send_ret_iso(seqnum, status, iso)
                continue

            if ep == 0:
                status, in_data = self.dev.control(setup, out_data, buflen)
            elif direction == DIR_IN and ep == EP_BULK_IN:
                status, in_data = self.dev.bulk_in(buflen)
            elif direction == DIR_IN and ep == EP_INTR_IN:
                status, in_data = self.dev.interrupt_in(buflen)
            elif direction == DIR_OUT and ep == EP_BULK_OUT:
                status = self.dev.bulk_out(out_data)
                in_data = b""
            else:
                self.log(f"  no such endpoint {ep}, stalling")
                status, in_data = E_PIPE, b""

            if direction == DIR_IN:
                actual = len(in_data)
            else:
                actual = 0 if status != E_OK else buflen

            self.log(f"  -> RET_SUBMIT status={status} actual={actual}")
            self.send_ret_submit(seqnum, status, actual,
                                 in_data if direction == DIR_IN else b"")

    def send_ret_iso(self, seqnum, status, iso):
        """
        Reply to an isochronous submission: the bytes actually
        transferred, packed together, then a descriptor per packet
        saying where each one belongs.
        """
        body = b"".join(body for (_o, _l, body) in iso)
        hdr = struct.pack(">IIIII", RET_SUBMIT, seqnum, 0, 0, 0)
        hdr += struct.pack(">iiiii", status, len(body), 0, len(iso), 0)
        hdr += b"\x00" * 8
        assert len(hdr) == 48

        desc = b""
        for (offset, length, body_i) in iso:
            desc += struct.pack(">IIIi", offset, length, len(body_i),
                                0 if status == E_OK else status)
        self.log(f"  -> RET_SUBMIT seq={seqnum} status={status} "
                 f"actual={len(body)} npkt={len(iso)}")
        self.conn.sendall(hdr + body + desc)

    def send_ret_submit(self, seqnum, status, actual, payload):
        hdr = struct.pack(">IIIII", RET_SUBMIT, seqnum, 0, 0, 0)
        hdr += struct.pack(">iiiii", status, actual, 0, -1, 0)
        hdr += b"\x00" * 8
        assert len(hdr) == 48
        self.conn.sendall(hdr + payload)

    def send_ret_unlink(self, seqnum, status):
        hdr = struct.pack(">IIIII", RET_UNLINK, seqnum, 0, 0, 0)
        hdr += struct.pack(">i", status) + b"\x00" * 24
        assert len(hdr) == 48
        self.conn.sendall(hdr)

    def handle(self):
        version, code, status = struct.unpack(">HHI", recv_exact(self.conn, 8))
        self.log(f"request version={version:#06x} code={code:#06x}")
        if version != USBIP_VERSION:
            self.conn.sendall(op_common(code & 0x7FFF, ST_NA))
            return

        if code == OP_REQ_DEVLIST:
            reply = op_common(OP_REP_DEVLIST) + struct.pack(">I", 1)
            reply += pack_udev()
            # One descriptor per interface.  The count has to match the
            # bNumInterfaces just sent, or the client waits for bytes
            # that never arrive.
            for _ in range(NUM_INTERFACES):
                reply += struct.pack("BBBB", 0xFF, 0x00, 0x00, 0)
            self.conn.sendall(reply)
        elif code == OP_REQ_IMPORT:
            busid = recv_exact(self.conn, 32).rstrip(b"\x00")
            if busid != BUSID:
                self.conn.sendall(op_common(OP_REP_IMPORT, ST_NA))
                return
            self.conn.sendall(op_common(OP_REP_IMPORT) + pack_udev())
            print(f"fake_usbipd: imported {busid.decode()}, serving URBs",
                  file=sys.stderr, flush=True)
            self.serve_urbs()
        else:
            self.conn.sendall(op_common(code & 0x7FFF, ST_NA))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-p", "--port", type=int, default=3240)
    ap.add_argument("-1", "--oneshot", action="store_true")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    srv = socket.socket(socket.AF_INET6, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        srv.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_V6ONLY, 0)
    except OSError:
        pass
    srv.bind(("::", args.port))
    srv.listen(1)
    print(f"fake_usbipd: listening on port {args.port}", file=sys.stderr,
          flush=True)

    while True:
        conn, peer = srv.accept()
        conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        print(f"fake_usbipd: connection from {peer}", file=sys.stderr,
              flush=True)
        try:
            Session(conn, args.verbose).handle()
        except (ConnectionError, OSError) as e:
            print(f"fake_usbipd: {e}", file=sys.stderr, flush=True)
        finally:
            conn.close()
        if args.oneshot:
            break


if __name__ == "__main__":
    main()
