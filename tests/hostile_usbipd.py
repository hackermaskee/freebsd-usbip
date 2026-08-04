#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 furuta@furuta.bsdclub.org
#
# hostile_usbipd: a USB/IP server that completes the handshake and then
# answers the first URB with something malformed.
#
# The client's kernel parses this stream, so every field in it is
# untrusted input reaching kernel memory.  The driver is supposed to
# reject each of these and drop the session, not read out of bounds,
# allocate without bound, or wedge.
#
#	tests/hostile_usbipd.py <mode>
#
# Modes are listed by --list.

import argparse
import os
import socket
import struct
import sys

USBIP_VERSION = 0x0111
OP_REQ_IMPORT = 0x8003
OP_REP_IMPORT = 0x0003
OP_REQ_DEVLIST = 0x8005
OP_REP_DEVLIST = 0x0005
ST_OK = 0
ST_NA = 1

CMD_SUBMIT = 1
RET_SUBMIT = 3
RET_UNLINK = 4

BUSID = b"1-1"

MODES = {
    "unknown-command":
        "a PDU whose command field is not one we know",
    "huge-length":
        "actual_length far larger than any transfer we would allow",
    "negative-length":
        "actual_length with the sign bit set",
    "overlong":
        "more data than the transfer asked for",
    "bad-seqnum":
        "a reply whose sequence number was never sent",
    "short-header":
        "half a header, then the connection closes",
    "garbage":
        "random bytes where a header should be",
    "unlink-storm":
        "RET_UNLINK replies that match nothing",
    "stall-forever":
        "a valid handshake and then silence",
}

# Whether the driver is expected to drop the session, or to shrug and
# keep going because the input was odd but not a protocol violation.
EXPECT_DROP = {
    "unknown-command": True,
    "huge-length": True,
    "negative-length": True,
    "overlong": True,
    "bad-seqnum": True,
    "short-header": True,
    "garbage": True,
    "unlink-storm": False,
    "stall-forever": False,
}


def pack_udev():
    buf = struct.pack("256s32s", b"/sys/devices/fake/usb1/1-1", BUSID)
    buf += struct.pack(">III", 1, 2, 3)          # busnum, devnum, high speed
    buf += struct.pack(">HHH", 0x1209, 0x0001, 0x0100)
    buf += struct.pack("BBBBBB", 0xFF, 0, 0, 1, 1, 1)
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


def ret_submit(seqnum, status, actual, npackets=-1):
    hdr = struct.pack(">IIIII", RET_SUBMIT, seqnum, 0, 0, 0)
    hdr += struct.pack(">iiiii", status, actual, 0, npackets, 0)
    hdr += b"\x00" * 8
    assert len(hdr) == 48
    return hdr


def attack(conn, mode, seqnum, buflen, log):
    log(f"answering seq={seqnum} with: {mode}")

    if mode == "unknown-command":
        hdr = struct.pack(">IIIII", 0x99999999, seqnum, 0, 0, 0)
        conn.sendall(hdr + b"\x00" * 28)

    elif mode == "huge-length":
        conn.sendall(ret_submit(seqnum, 0, 0x7FFFFFFF))

    elif mode == "negative-length":
        conn.sendall(ret_submit(seqnum, 0, -1))

    elif mode == "overlong":
        # Claim, and actually send, far more than was requested.
        extra = buflen + 8192
        conn.sendall(ret_submit(seqnum, 0, extra) + os.urandom(extra))

    elif mode == "bad-seqnum":
        conn.sendall(ret_submit(seqnum ^ 0x5A5A5A5A, 0, 0))

    elif mode == "short-header":
        # Half a header and then EOF, so the client is left mid-read.
        conn.sendall(ret_submit(seqnum, 0, 0)[:20])
        conn.shutdown(socket.SHUT_RDWR)
        return

    elif mode == "garbage":
        conn.sendall(os.urandom(48))

    elif mode == "unlink-storm":
        for i in range(64):
            hdr = struct.pack(">IIIII", RET_UNLINK, 0xDEAD0000 + i, 0, 0, 0)
            conn.sendall(hdr + struct.pack(">i", -104) + b"\x00" * 24)

    elif mode == "stall-forever":
        pass                # never answer at all

    else:
        raise SystemExit(f"unknown mode {mode}")


def serve(conn, mode, verbose):
    def log(msg):
        if verbose:
            print(f"hostile: {msg}", file=sys.stderr, flush=True)

    version, code, status = struct.unpack(">HHI", recv_exact(conn, 8))
    if version != USBIP_VERSION:
        conn.sendall(op_common(code & 0x7FFF, ST_NA))
        return

    if code == OP_REQ_DEVLIST:
        conn.sendall(op_common(OP_REP_DEVLIST) + struct.pack(">I", 1) +
                     pack_udev() + struct.pack("BBBB", 0xFF, 0, 0, 0))
        return
    if code != OP_REQ_IMPORT:
        conn.sendall(op_common(code & 0x7FFF, ST_NA))
        return

    busid = recv_exact(conn, 32).rstrip(b"\x00")
    if busid != BUSID:
        conn.sendall(op_common(OP_REP_IMPORT, ST_NA))
        return
    conn.sendall(op_common(OP_REP_IMPORT) + pack_udev())
    log("imported; waiting for the first URB")

    hdr = recv_exact(conn, 48)
    command, seqnum, _devid, direction, _ep = struct.unpack(">IIIII", hdr[:20])
    buflen = struct.unpack(">i", hdr[24:28])[0]
    if command == CMD_SUBMIT and direction == 0 and buflen > 0:
        recv_exact(conn, buflen)

    attack(conn, mode, seqnum, max(buflen, 0), log)

    # Stay open so the client is not merely seeing a closed socket.
    log("attack sent; holding the connection open")
    try:
        while conn.recv(65536):
            pass
    except (ConnectionError, OSError):
        pass


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("mode", nargs="?", help="malformed reply to send")
    ap.add_argument("-p", "--port", type=int, default=3240)
    ap.add_argument("-v", "--verbose", action="store_true")
    ap.add_argument("--list", action="store_true", help="list modes")
    ap.add_argument("--expect", metavar="MODE",
                    help="print drop or survive for MODE and exit")
    args = ap.parse_args()

    if args.expect:
        print("drop" if EXPECT_DROP[args.expect] else "survive")
        return
    if args.list or args.mode is None:
        for name, desc in MODES.items():
            print(f"{name:18s} {desc}")
        return
    if args.mode not in MODES:
        raise SystemExit(f"unknown mode {args.mode}; try --list")

    srv = socket.socket(socket.AF_INET6, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        srv.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_V6ONLY, 0)
    except OSError:
        pass
    srv.bind(("::", args.port))
    srv.listen(1)
    print(f"hostile_usbipd: {args.mode} on port {args.port}", file=sys.stderr,
          flush=True)

    while True:
        conn, _peer = srv.accept()
        conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        try:
            serve(conn, args.mode, args.verbose)
        except (ConnectionError, OSError) as e:
            print(f"hostile_usbipd: {e}", file=sys.stderr, flush=True)
        finally:
            conn.close()


if __name__ == "__main__":
    main()
