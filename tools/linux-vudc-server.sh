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
ls /sys/class/udc/ | sed 's/^/    /'

echo "== starting usbipd =="
pkill usbipd 2>/dev/null || true
sleep 1
usbipd -D
sleep 1

echo "== exportable devices =="
usbip list -l 2>&1 | sed 's/^/    /'

cat <<EOF

The gadget presents itself as 0525:a4a0.  From the FreeBSD machine:

    usbip list -r $(hostname -I 2>/dev/null | awk '{print $1}')
    usbip attach -r $(hostname -I 2>/dev/null | awk '{print $1}') -b usbip-vudc.0
    tests/bulk_test 0525 a4a0

Tear down with: sudo $0 stop
EOF
