#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
#
# Stand up a real Linux USB/IP server, on a Linux machine, with no USB
# hardware involved.  This is the interoperability reference: our own
# test server shares our reading of the protocol, so agreeing with it
# proves nothing.  The Linux implementation is the one that matters.
#
# usbip-vudc provides a virtual UDC, and the g_zero gadget binds to it
# to present a device with bulk endpoints.  The real usbipd then exports
# that device.
#
#	sudo tools/linux-vudc-server.sh          # start
#	sudo tools/linux-vudc-server.sh stop     # tear down
#
# Then, from the FreeBSD machine:
#
#	usbip list -r <this host>
#	usbip attach -r <this host> -b usbip-vudc.0

set -e

if [ "$(id -u)" != 0 ]; then
	echo "$0: must be run as root" >&2
	exit 1
fi
if [ "$(uname -s)" != Linux ]; then
	echo "$0: this is the Linux side; run it on the server" >&2
	exit 1
fi

if [ "$1" = stop ]; then
	pkill usbipd 2>/dev/null || true
	sleep 1
	modprobe -r g_zero 2>/dev/null || true
	modprobe -r usbip-vudc 2>/dev/null || true
	modprobe -r usbip-core 2>/dev/null || true
	echo "stopped"
	exit 0
fi

echo "== loading modules =="
modprobe usbip-vudc num=1
# loopdefault makes the gadget's default configuration loop bulk OUT
# back to bulk IN, which is what tests/bulk_test.c expects.
modprobe g_zero loopdefault=1

echo "== virtual UDC =="
for udc in /sys/class/udc/*/; do
	name=$(basename "$udc")
	printf '    %s: function=%s state=%s\n' "$name" \
	    "$(cat "$udc/function" 2>/dev/null)" \
	    "$(cat "$udc/state" 2>/dev/null)"
done
if [ "$(cat /sys/class/udc/usbip-vudc.0/function 2>/dev/null)" != zero ]; then
	echo "    !! g_zero did not bind to usbip-vudc.0" >&2
	exit 1
fi

# Device mode is what serves a gadget bound to usbip-vudc; the default
# host mode only exports real USB devices attached to this machine.
echo "== starting usbipd in device mode =="
pkill usbipd 2>/dev/null || true
sleep 1
usbipd -e -D
sleep 1
pgrep -l usbipd | sed 's/^/    /' || { echo "    usbipd failed to start" >&2; exit 1; }

echo "== exportable gadgets =="
usbip list -d 2>&1 | sed 's/^/    /'

addr=$(hostname -I 2>/dev/null | awk '{print $1}')
cat <<EOF

Nothing here touches any real USB device on this machine.

The gadget presents itself as 0525:a4a0.  From the FreeBSD machine:

    usbip list -r $addr
    usbip attach -r $addr -b usbip-vudc.0
    tests/bulk_test 0525 a4a0

Tear down with: sudo $0 stop
EOF
