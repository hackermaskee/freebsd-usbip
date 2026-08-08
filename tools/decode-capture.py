#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 furuta@furuta.bsdclub.org
#
# Decode USB/IP PDUs out of a packet capture.
#
#	tools/decode-capture.py /tmp/usbip-iso.pcap
#
# Written because the isochronous packet descriptor is the one part of
# the protocol the documentation does not describe, so the only way to
# settle it is to look at what a real implementation sends.  It
# reassembles each direction of the TCP stream and walks the PDUs.

import struct
import sys

CMD_SUBMIT = 1
CMD_UNLINK = 2
RET_SUBMIT = 3
RET_UNLINK = 4

NAMES = {
    CMD_SUBMIT: "CMD_SUBMIT",
    CMD_UNLINK: "CMD_UNLINK",
    RET_SUBMIT: "RET_SUBMIT",
    RET_UNLINK: "RET_UNLINK",
}

LINKTYPE_ETHERNET = 1
LINKTYPE_LINUX_SLL = 113
LINKTYPE_LINUX_SLL2 = 276
LINKTYPE_RAW = 101


def read_pcap(path):
    """Yield raw link-layer frames."""
    with open(path, "rb") as f:
        hdr = f.read(24)
        magic = struct.unpack("<I", hdr[:4])[0]
        if magic in (0xA1B2C3D4, 0xA1B23C4D):
            endian = "<"
        elif magic in (0xD4C3B2A1, 0x4D3CB2A1):
            endian = ">"
        else:
            raise SystemExit(f"{path}: not a pcap file")
        linktype = struct.unpack(endian + "I", hdr[20:24])[0]
        while True:
            ph = f.read(16)
            if len(ph) < 16:
                return
            _, _, caplen, _ = struct.unpack(endian + "IIII", ph)
            yield linktype, f.read(caplen)


def strip_link(linktype, frame):
    if linktype == LINKTYPE_ETHERNET:
        if struct.unpack(">H", frame[12:14])[0] != 0x0800:
            return None
        return frame[14:]
    if linktype == LINKTYPE_LINUX_SLL:
        if struct.unpack(">H", frame[14:16])[0] != 0x0800:
            return None
        return frame[16:]
    if linktype == LINKTYPE_LINUX_SLL2:
        if struct.unpack(">H", frame[0:2])[0] != 0x0800:
            return None
        return frame[20:]
    if linktype == LINKTYPE_RAW:
        return frame
    return None


def tcp_streams(path):
    """Reassemble each direction into (key, bytes)."""
    chunks = {}
    for linktype, frame in read_pcap(path):
        ip = strip_link(linktype, frame)
        if not ip or len(ip) < 20 or (ip[0] >> 4) != 4:
            continue
        ihl = (ip[0] & 0xF) * 4
        if ip[9] != 6:                      # not TCP
            continue
        total = struct.unpack(">H", ip[2:4])[0]
        src, dst = ip[12:16], ip[16:20]
        tcp = ip[ihl:total]
        if len(tcp) < 20:
            continue
        sport, dport, seq = struct.unpack(">HHI", tcp[:8])
        off = (tcp[12] >> 4) * 4
        payload = tcp[off:]
        if not payload:
            continue
        key = (".".join(map(str, src)), sport, ".".join(map(str, dst)), dport)
        chunks.setdefault(key, {})[seq] = payload

    for key, by_seq in chunks.items():
        data = b""
        base = None
        for seq in sorted(by_seq):
            if base is None:
                base = seq
            data += by_seq[seq]
        yield key, data


def hexdump(b, indent="        "):
    out = []
    for i in range(0, len(b), 16):
        row = b[i:i + 16]
        hexs = " ".join(f"{c:02x}" for c in row)
        out.append(f"{indent}{i:04x}  {hexs}")
    return "\n".join(out)


def decode(data, label, directions=None):
    """
    Walk USB/IP PDUs, skipping the handshake.

    A reply's own direction field is zeroed by the server, so whether a
    payload follows it can only be known from the direction of the
    request that the sequence number belongs to.  Getting this wrong
    silently shifts everything after it, which is precisely the hazard
    the driver has to handle too.  `directions` carries what the
    client's stream said.
    """
    print(f"=== {label}: {len(data)} bytes ===")
    seen = {} if directions is None else directions
    off = 0

    # The handshake comes first and is not 48-byte framed.
    if len(data) >= 8 and struct.unpack(">H", data[:2])[0] == 0x0111:
        code = struct.unpack(">H", data[2:4])[0]
        print(f"  handshake code {code:#06x}")
        off = 8
        if code == 0x8003:              # OP_REQ_IMPORT
            off += 32
        elif code == 0x0003:            # OP_REP_IMPORT
            off += 312
        elif code == 0x0005:            # OP_REP_DEVLIST
            n = struct.unpack(">I", data[8:12])[0]
            off = 12
            for _ in range(n):
                nif = data[off + 311]
                off += 312 + nif * 4
        elif code == 0x8005:
            pass

    while off + 48 <= len(data):
        hdr = data[off:off + 48]
        command, seqnum, devid, direction, ep = struct.unpack(">IIIII", hdr[:20])
        if command not in NAMES:
            print(f"  [{off}] unrecognised command {command:#x}; stopping")
            print(hexdump(hdr))
            return
        off += 48

        if command in (CMD_UNLINK, RET_UNLINK):
            print(f"  {NAMES[command]} seq={seqnum}")
            continue

        if command == CMD_SUBMIT:
            flags, buflen, start, npkt, interval = struct.unpack(
                ">IiiiI", hdr[20:40])
            setup = hdr[40:48]
            print(f"  CMD_SUBMIT seq={seqnum} ep={ep} "
                  f"dir={'IN' if direction else 'OUT'} len={buflen} "
                  f"flags={flags:#x} start_frame={start} npkt={npkt} "
                  f"interval={interval} setup={setup.hex()}")
            seen[seqnum] = direction
            if direction == 0 and buflen > 0:
                off += buflen
        else:
            status, alen, start, npkt, errcnt = struct.unpack(
                ">iiiii", hdr[20:40])
            is_in = seen.get(seqnum)
            print(f"  RET_SUBMIT seq={seqnum} status={status} "
                  f"actual={alen} start_frame={start} npkt={npkt} "
                  f"error_count={errcnt} "
                  f"(request was {'IN' if is_in else 'OUT'})")
            if is_in and alen > 0:
                off += alen

        if npkt > 0:
            n = min(npkt, 64)
            desc = data[off:off + npkt * 16]
            print(f"    {npkt} isochronous packet descriptors, "
                  f"{len(desc)} bytes:")
            for i in range(min(n, len(desc) // 16)):
                d = desc[i * 16:(i + 1) * 16]
                o, ln, al, st = struct.unpack(">IIIi", d)
                print(f"      [{i}] offset={o} length={ln} "
                      f"actual_length={al} status={st}")
            print("    raw:")
            print(hexdump(desc[:64]))
            off += npkt * 16


def main():
    if len(sys.argv) != 2:
        raise SystemExit(f"usage: {sys.argv[0]} capture.pcap")
    streams = [(k, d) for k, d in tcp_streams(sys.argv[1])
               if 3240 in (k[1], k[3])]
    # The client's stream first, so the replies can be interpreted.
    streams.sort(key=lambda kd: 0 if kd[0][3] == 3240 else 1)

    directions = {}
    for key, data in streams:
        src, sport, dst, dport = key
        who = "client -> server" if dport == 3240 else "server -> client"
        decode(data, f"{who}  {src}:{sport} > {dst}:{dport}", directions)
        print()


if __name__ == "__main__":
    main()
