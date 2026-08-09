# Protocol notes

Interoperability notes for our implementation of the USB/IP wire
protocol, as defined in
<https://docs.kernel.org/usb/usbip_protocol.html>. The definitions live
in `sys/dev/vhci/usbip_proto.h`.

## Confirmed against the specification

- Version 0x0111, big-endian throughout. Port 3240 is not stated in the
  spec but is what the Linux tools and usbipd-win use.
- `op_common` is 8 bytes: version, code, status.
- The device description is 312 bytes: `path[256]`, `busid[32]`,
  busnum, devnum, speed (4 each), idVendor, idProduct, bcdDevice
  (2 each), then 6 single bytes ending with `bNumInterfaces`. Offsets
  cross-checked in both OP_REP_DEVLIST (record starts at 12) and
  OP_REP_IMPORT (starts at 8).
- Interface description is 4 bytes: class, subclass, protocol, padding.
- The URB header is 48 bytes: 20-byte `usbip_header_basic` plus a
  28-byte command-specific part. Payload follows, then the ISO packet
  descriptors.
- `devid` is `busnum << 16 | devnum` from the client; the server sends 0.
  The server likewise zeroes `direction` and `ep` in replies, so match
  replies by `seqnum` only.
- CMD_UNLINK carries the seqnum to cancel; RET_UNLINK returns
  -ECONNRESET (-104) when the URB was actually unlinked, or 0 if the
  UNLINK lost the race with RET_SUBMIT.

## Consequences for the implementation

- **A reply's header direction is useless.** The server zeroes
  `direction` and `ep` in `USBIP_RET_SUBMIT`, so the only way to know
  whether `actual_length` bytes of payload follow is to remember the
  direction of the submission that the sequence number belongs to. This
  matters most for a submission we have already given up on: `vhci(4)`
  keeps a record of every cancelled sequence number, with its
  direction, until the reply or the `USBIP_RET_UNLINK` arrives. Without
  it a late reply to an OUT transfer would be mistaken for one with a
  payload and the stream would silently desynchronise.
- **A submission that has started must be sent in full.** Cancelling
  part-way through would leave the server waiting for the rest of the
  announced `transfer_buffer_length`.
- **`URB_SHORT_NOT_OK` is deliberately never set.** The FreeBSD stack
  decides for itself whether a short IN transfer is an error; asking the
  server to fail them turns normal short reads into `-EREMOTEIO`.

## Confirmed against the real Linux implementation

Verified by attaching to a Linux `usbipd` serving a `g_zero` gadget on
`usbip-vudc`, from FreeBSD 14.4. Enumeration, descriptors, strings and
bulk transfers from 1 byte to 100 KB all work. What that settles:

- The handshake, the 48-byte header layout, `devid`, and the
  direction/endpoint encoding are right, because the canonical
  implementation understands them.
- `number_of_packets` as 0xffffffff for non-ISO is accepted by Linux.
- Absorbing SET_ADDRESS locally is required, not optional. The gadget
  never saw one, which is what we want; forwarding it would have
  re-addressed the device out from under the server.

The one thing our own test server was too generous about: it takes the
transfer length from the USB/IP header, so it does not care about
**zero-length packet termination**. A real device does. Writing an
exact multiple of the maximum packet size leaves the device waiting for
more data until a short or zero-length packet ends the transfer, and a
loopback device then echoes that ZLP back. That is USB semantics rather
than anything USB/IP-specific, and it is carried correctly, but it is
worth knowing that passing against `fake_usbipd.py` does not prove a
transfer size is safe.

## Input the driver refuses

Everything after the hand-off is parsed in kernel context from a socket,
so it is untrusted. `tools/hostile-test.sh` drives each of these.
Rejecting them means dropping the session, which takes the device away
and frees the port.

- A command field that is not one of the four we know.
- `actual_length` beyond `VHCI_MAX_XFER_LEN`, or with the sign bit set.
- `actual_length` larger than the transfer asked for. There is nowhere
  to put the excess, and believing it would leave us out of step with
  the stream.
- A reply whose sequence number is neither in flight nor recorded as
  cancelled, since without that record we cannot know whether a payload
  follows it.
- A truncated header, or bytes that do not parse as one.

Two things are deliberately *not* treated as fatal, because they are
odd rather than wrong: a `USBIP_RET_UNLINK` matching nothing, which is
the normal outcome of a race we already resolved, and a server that
simply stops answering, which the transfer timeout handles.

## To verify on the wire (tcpdump against Linux, milestone M2)

- **`number_of_packets` for non-ISO transfers.** The spec says
  0xffffffff; implementations have been reported to send 0. We send the
  spec value and accept both on receive (`USBIP_IS_ISO()`).
- **`transfer_flags`.** These are raw Linux `URB_*` bits. Only the few
  that are meaningful across the wire are defined in the header; confirm
  which ones Linux actually sets for control/bulk/interrupt transfers,
  and that a server ignores the rest.
- **`status` values.** Negative Linux errno numbers cross the wire, so
  they need translating to and from `usb_error_t`. Confirm the mapping
  for stall (-EPIPE) and short reads (-EREMOTEIO with SHORT_NOT_OK).
- Whether the server tolerates a `setup[8]` that is non-zero on
  non-control endpoints (we always zero it).
- Whether a server minds `start_frame` being zero once
  `URB_ISO_ASAP` is set (we always send zero).

## The interval field

Settled, and it needed converting. A Linux URB counts the polling
period in frames at low and full speed but in **microframes** at high
speed and above, while the FreeBSD stack works in milliseconds
throughout - it folds the endpoint's exponential encoding away itself,
which the "125us -> 1ms" comment in `usbd_transfer_setup_sub()` spells
out. So a high-speed interrupt interval was being sent eight times too
short.

Isochronous was worse: the stack leaves the interval at zero and
comments that it is unused, and Linux rejects a URB whose interval is
zero outright. That alone would stop any isochronous transfer from ever
being scheduled, so the value has to come from the endpoint descriptor
instead. An isochronous endpoint encodes its period as
2^(bInterval-1), which is already in the units Linux wants at either
speed.

Confirmed on the wire against `tests/fake_usbipd.py`: an interrupt
endpoint with `bInterval` 4 at high speed now goes out as 8
microframes, and an isochronous endpoint with `bInterval` 1 as 1.

## Isochronous: settled

The packet descriptor layout is the one thing the documentation does
not describe. `tools/linux-iso-server.sh` exports a `g_audio` gadget
from a real Linux `usbipd`, and `tools/decode-capture.py` reads the
resulting capture.

**The client must be a different machine.** Attaching a host to a
device it is itself exporting puts `usbip-vudc` and `vhci-hcd` in the
same kernel, each waiting on the other through a loopback socket, and
hangs the machine hard enough to need the power switch. Found the hard
way.

Settled by observation:

- Our `CMD_SUBMIT` is **accepted**. It carries `number_of_packets`, the
  payload, then one 16-byte descriptor per packet of
  offset/length/actual_length/status, big-endian. A wrong layout would
  desynchronise the server's parser and corrupt everything after it;
  instead the following `RET_UNLINK` arrives correctly framed, so the
  framing is right.
- **`URB_ISO_ASAP` (0x0002) is required.** Without it the server takes
  `start_frame` literally, and zero is always in the past.

**The reply format is settled too**, by turning the question around.
No server would send us an isochronous reply, but `usbipd(8)` can send
one to a **Linux client** - and the same descriptor appears in both
directions, so a client that understands ours settles the format.

It does. Linux's `vhci-hcd` accepts our replies, places every packet at
the offset our descriptors give, and hands an application data that
matches byte for byte: forty-two consecutive transfers of eight packets
each, with the emulated device filling packet *i* with the byte *i* so
that a packet delivered to the wrong offset could not pass unnoticed.

Two rules came out of that exchange rather than out of the
documentation:

- **A reply must not follow an unlink.** Having answered a
  `USBIP_CMD_UNLINK` with `-ECONNRESET`, a server must stay silent
  about that transfer. Linux has already forgotten the sequence
  number, and answering anyway makes it log *cannot find a urb of
  seqnum N* and drop the session. This is the mirror of the record the
  client keeps for the same race.
- **`SET_CONFIGURATION` and `SET_INTERFACE` cannot be forwarded as
  bytes.** They have to be carried out through the local USB API, or
  the host library does not know the device changed underneath it - and
  on FreeBSD the alternate setting is what makes `ugen(4)` allocate an
  isochronous endpoint at all, so forwarding them means no isochronous
  transfer ever leaves the machine.
