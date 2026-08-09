# USB/IP for FreeBSD

A from-scratch, BSD-2-Clause implementation of
[USB/IP](https://docs.kernel.org/usb/usbip_protocol.html) for FreeBSD,
both halves of it: attach USB devices exported by a remote host as if
they were plugged in locally, and export local devices to remote hosts.
Interoperates with Linux at both ends.

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
| `usr.sbin/usbipd/` | `usbipd(8)`: exports local devices. Pure userland on top of libusb and ugen(4), so the server side needs no kernel support at all. |
| `tests/` | Protocol golden tests (`proto_test`), a USB/IP server that emulates a device in software (`fake_usbipd.py`) and one that lies (`hostile_usbipd.py`), and transfer exercisers (`bulk_test.c`, `iso_probe.c`). No hardware needed. |
| `rc.d/`, `port/` | An rc script for `usbipd(8)`, and a FreeBSD port skeleton that builds and stages the lot. |
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
- **M4 done**: isochronous transfers work in both directions and the
  wire format is confirmed against Linux at both ends. No server would
  send us an isochronous reply, so the question was turned around:
  `usbipd(8)` sends them to a Linux client, which places every packet
  where our descriptors say and delivers data that matches byte for
  byte. See `docs/protocol-notes.md`.
- **Server side done**: `usbipd(8)` exports local devices, verified
  against the real Linux `usbip` client - which enumerates the device
  and moves bulk, interrupt and isochronous data through it correctly.
  Clients are served concurrently; a device still goes to one of them
  at a time.

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

## Installing

```sh
make && make kmod SYSDIR=/usr/src/sys
sudo make install            # PREFIX and KMODDIR are honoured
sysrc usbipd_enable=YES usbipd_devices=ugen0.2
service usbipd start
```

`port/` is a FreeBSD port skeleton that does the same through the ports
framework. It needs a release to fetch: fill in `MASTER_SITES`, or
switch to `USE_GITHUB`, and run `make makesum`.

## Testing

Everything below runs without USB hardware.

```sh
sudo tools/smoke-test.sh    # does the driver load and the root hub appear?
sudo tools/race-test.sh     # is unloading during enumeration refused?
sudo tools/attach-test.sh   # attach an emulated device and move data
sudo tools/error-test.sh    # what happens when the server dies
sudo tools/hostile-test.sh  # what happens when the server lies
sudo tools/server-test.sh   # does usbipd(8) cope with several clients
```

To test against the canonical implementation rather than our own
server, on a **separate** Linux machine:

```sh
sudo tools/linux-vudc-server.sh          # bulk, via the g_zero gadget
sudo tools/linux-iso-server.sh start     # isochronous, via g_audio
```

Never attach a machine to a device it is itself exporting: the client
and server drivers end up in the same kernel waiting on each other
through a loopback socket, and it hangs.

then from FreeBSD:

```sh
usbip attach -r <linux-host> -b usbip-vudc.0
tests/bulk_test 0525 a4a0
```

## License

BSD-2-Clause. See `LICENSE`.
