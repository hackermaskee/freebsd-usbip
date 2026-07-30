#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 furuta@furuta.bsdclub.org
#
# fake_usbipd: a minimal USB/IP server that answers OP_REQ_DEVLIST and
# OP_REQ_IMPORT with a canned device.  Protocol regression fixture for
# the usbip(8) client (and later the vhci(4) URB phase).

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

FAKE = {
    "path": b"/sys/devices/fake/usb1/1-1",
    "busid": b"1-1",
    "busnum": 1,
    "devnum": 2,
    "speed": 3,  # high
    "idVendor": 0x1234,
    "idProduct": 0x5678,
    "bcdDevice": 0x0100,
    "dev_class": (0x00, 0x00, 0x00),
    "bConfigurationValue": 1,
    "bNumConfigurations": 1,
    # (class, subclass, protocol) per interface
    "interfaces": [(0x08, 0x06, 0x50)],
}


def pack_udev_be(d):
    buf = struct.pack("256s32s", d["path"], d["busid"])
    buf += struct.pack(">III", d["busnum"], d["devnum"], d["speed"])
    buf += struct.pack(">HHH", d["idVendor"], d["idProduct"], d["bcdDevice"])
    c, s, p = d["dev_class"]
    buf += struct.pack("BBBBBB", c, s, p, d["bConfigurationValue"],
                       d["bNumConfigurations"], len(d["interfaces"]))
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


def handle(conn, verbose):
    version, code, status = struct.unpack(">HHI", recv_exact(conn, 8))
    if verbose:
        print(f"request: version={version:#06x} code={code:#06x} "
              f"status={status}", file=sys.stderr)
    if version != USBIP_VERSION:
        conn.sendall(op_common(code & 0x7FFF, ST_NA))
        return

    if code == OP_REQ_DEVLIST:
        reply = op_common(OP_REP_DEVLIST)
        reply += struct.pack(">I", 1)
        reply += pack_udev_be(FAKE)
        for c, s, p in FAKE["interfaces"]:
            reply += struct.pack("BBBB", c, s, p, 0)
        conn.sendall(reply)
    elif code == OP_REQ_IMPORT:
        busid = recv_exact(conn, 32).rstrip(b"\x00")
        if busid == FAKE["busid"]:
            conn.sendall(op_common(OP_REP_IMPORT) + pack_udev_be(FAKE))
            if verbose:
                print(f"imported {busid.decode()}; holding connection "
                      "open for URB phase (no URB support yet)",
                      file=sys.stderr)
            # URB phase not implemented yet: just drain until close so
            # the client sees a live connection.
            try:
                while conn.recv(65536):
                    pass
            except ConnectionError:
                pass
        else:
            conn.sendall(op_common(OP_REP_IMPORT, ST_NA))
    else:
        conn.sendall(op_common(code & 0x7FFF, ST_NA))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-p", "--port", type=int, default=3240)
    ap.add_argument("-1", "--oneshot", action="store_true",
                    help="exit after one connection (for scripted tests)")
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
    print(f"fake_usbipd: listening on port {args.port}", file=sys.stderr)

    while True:
        conn, peer = srv.accept()
        if args.verbose:
            print(f"connection from {peer}", file=sys.stderr)
        try:
            handle(conn, args.verbose)
        except ConnectionError as e:
            print(f"fake_usbipd: {e}", file=sys.stderr)
        finally:
            conn.close()
        if args.oneshot:
            break


if __name__ == "__main__":
    main()
