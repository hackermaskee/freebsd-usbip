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
| `tests/` | Protocol golden tests (`proto_test`) and a fake USB/IP server (`fake_usbipd.py`) for regression testing without hardware. |

## Status

- **M0 done**: protocol library + `usbip list` / handshake, tests.
  Pure userland; builds on both FreeBSD and Linux (the latter for
  development convenience and interop testing against Linux usbip).
- **M1 done**: vhci skeleton — pseudo-device on nexus, usbus child,
  software root hub, `/dev/vhci` with socket hand-off.
- **M2 written, untested**: control, bulk and interrupt transfers over
  the TCP session, with timeout and unlink handling. Nothing in the
  kernel has been run yet: it type-checks against FreeBSD 14 headers
  (`tools/syntax-check.sh`) but has never been built or loaded.
- M3: run it. Interrupt transfer timing, interop against Linux, error
  paths.
- M4: isochronous, polish, man pages.
- Later: server side (export FreeBSD devices), implemented in userland
  via ugen(4)/libusb.

## Build

```sh
make        # userland tool + tests (FreeBSD or Linux)
make check  # run protocol golden tests
make kmod   # vhci(4) kernel module (FreeBSD 14.x only)
```

## Quick interop test against a Linux server

On the Linux side:

```sh
sudo modprobe usbip-host
sudo usbipd -D
usbip list -l
sudo usbip bind -b <busid>
```

Then from this tree:

```sh
usr.sbin/usbip/usbip list -r <linux-host>
```

## License

BSD-2-Clause. See `LICENSE`.
