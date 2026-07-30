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
- **ISO packet descriptor layout** is not spelled out in the spec text;
  we assume offset/length/actual_length/status, 4 bytes each. Verify
  before implementing M4.
- Whether the server tolerates a `setup[8]` that is non-zero on
  non-control endpoints (we always zero it).
