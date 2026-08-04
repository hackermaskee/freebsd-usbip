# USB/IP for FreeBSD

A from-scratch, BSD-2-Clause implementation of the
[USB/IP](https://docs.kernel.org/usb/usbip_protocol.html) client for
FreeBSD: attach USB devices exported by a remote host (Linux `usbipd`,
`usbipd-win`, ...) as if they were plugged into the local machine.

Both the kernel driver and the userland tool are new code written from
the protocol documentation; no GPL Linux implementation code is used or
consulted. (The Linux *protocol documentation* is the interoperability
reference.)

## Components

| Path | What |
|---|---|
| `sys/dev/vhci/` | `vhci(4)`: virtual USB host controller kernel driver. Plugs into the FreeBSD usb(4) stack, speaks the USB/IP URB phase over a TCP socket handed off from userland. |
| `sys/modules/vhci/` | kmod build glue (FreeBSD only, `bsd.kmod.mk`). |
| `usr.sbin/usbip/` | `usbip(8)`: `list` / `attach` / `detach` / `port`. Performs the OP_REQ_DEVLIST / OP_REQ_IMPORT handshake in userland, then passes the connected socket to `vhci(4)`. |
| `tests/` | Protocol golden tests (`proto_test`), a USB/IP server that emulates a device in software (`fake_usbipd.py`), and a transfer exerciser (`bulk_test.c`). No hardware or privileges needed. |
| `tools/` | Build and test helpers: `syntax-check.sh` type-checks the driver on a non-FreeBSD machine; `smoke-test.sh`, `race-test.sh` and `attach-test.sh` are the three things worth running on the target; `linux-vudc-server.sh` stands up a real Linux server for interop testing. |

## Status

Verified on FreeBSD 14.4-RELEASE (amd64), against **the real Linux
`usbipd`** as well as against our own test server: a remote device
enumerates and control, bulk and interrupt transfers all work.

- **M0 done**: protocol library, `usbip list`, golden tests.
- **M1 done**: root hub enumerates; `uhub0: 8 ports with 8 removable`.
- **M2 done**: control, bulk and interrupt transfers, verified end to
  end. Bulk round trips are byte-exact from 1 byte to 100 KB, spanning
  the max packet size and the driver's staging buffer.
- **M3 done**: interoperability against the real Linux `usbipd`
  confirmed, serving a `g_zero` gadget over `usbip-vudc`. Bulk round
  trips are byte-exact from 1 byte to 100 KB. A server that dies, with
  or without transfers outstanding, takes the device away cleanly and
  frees the port for reuse.
- M4: isochronous transfers. Not started, and worth knowing before it
  is: the protocol documentation does not spell out the isochronous
  packet descriptor layout, so unlike everything above it cannot be
  settled by reading the spec. It needs checking against a real
  implementation carrying real isochronous traffic.
- Later: server side (export FreeBSD devices), implemented in userland
  via ugen(4)/libusb.

## Build

```sh
make        # userland tool + tests (FreeBSD or Linux)
make check  # golden tests, plus a type-check of the driver if a
            # FreeBSD source tree is available (see FREEBSD_SRC)
make kmod   # vhci(4) kernel module (FreeBSD 14.x only)
```

The kernel module needs a FreeBSD source tree matching the running
kernel:

```sh
make -C sys/modules/vhci SYSDIR=/usr/src/sys
kldload sys/modules/vhci/vhci.ko
```

Development can happen on a non-FreeBSD machine: `tools/syntax-check.sh`
compiles the driver against real FreeBSD headers to catch wrong struct
members and signatures without a round trip to the target.

## Testing

Everything below runs without USB hardware.

```sh
sudo tools/smoke-test.sh    # does the driver load and the root hub appear?
sudo tools/race-test.sh     # is unloading during enumeration refused?
sudo tools/attach-test.sh   # attach an emulated device and move data
sudo tools/error-test.sh    # what happens when the server dies
sudo tools/hostile-test.sh  # what happens when the server lies
```

To test against the canonical implementation rather than our own
server, on a Linux machine:

```sh
sudo tools/linux-vudc-server.sh
```

then from FreeBSD:

```sh
usbip attach -r <linux-host> -b usbip-vudc.0
tests/bulk_test 0525 a4a0
```

## License

BSD-2-Clause. See `LICENSE`.
