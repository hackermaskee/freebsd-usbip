#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
#
# Export a gadget with isochronous endpoints from a real Linux usbipd,
# and capture the traffic, so that the isochronous packet descriptor
# layout can be read off the wire.  The protocol documentation describes
# every other field but not that one.
#
# The client must be ANOTHER MACHINE.  Do not attach this host to its
# own exported device: usbip-vudc and vhci-hcd would both be running in
# this kernel, each waiting on the other through a loopback socket, and
# the machine hangs hard enough to need the power switch.  This script
# refuses to start if vhci-hcd is loaded, and never attaches anything
# itself.
#
# No USB hardware is touched; the device is a software gadget.
#
#	sudo tools/linux-iso-server.sh start
#	  ... drive it from the FreeBSD machine ...
#	sudo tools/linux-iso-server.sh stop
#
# Leaves the capture in /tmp/usbip-iso.pcap.

set -e

PCAP=${PCAP:-/tmp/usbip-iso.pcap}
PORT=3240

if [ "$(id -u)" != 0 ]; then
	echo "$0: must be run as root" >&2
	exit 1
fi
if [ "$(uname -s)" != Linux ]; then
	echo "$0: this is the Linux side" >&2
	exit 1
fi

#
# Deliberately does not unload the modules.  Removing g_audio while
# usbip-vudc still references it leaves both stuck at a refcount of -1
# and hangs the next modprobe, which needs a reboot to clear.  Nothing
# is gained by unloading them: leaving the gadget bound is harmless, and
# start below is happy to find it already there.
#
teardown() {
	pkill tcpdump 2>/dev/null || true
	pkill usbipd 2>/dev/null || true
	sleep 1
}

case "$1" in
stop)
	pkill tcpdump 2>/dev/null || true
	sleep 1
	if [ -f "$PCAP" ]; then
		echo "capture: $PCAP"
		ls -l "$PCAP" | sed 's/^/    /'
		tcpdump -r "$PCAP" 2>/dev/null | wc -l |
		    sed 's/^/    packets: /'
	fi
	teardown
	echo "stopped"
	exit 0
	;;
start)
	;;
*)
	echo "usage: $0 start|stop" >&2
	exit 1
	;;
esac

# The hazard this script exists to avoid.
if lsmod | grep -q '^vhci_hcd'; then
	echo "$0: vhci-hcd is loaded on this machine." >&2
	echo "$0: attaching a host to its own exported device deadlocks" >&2
	echo "$0: the kernel.  Unload it and drive this from another host." >&2
	exit 1
fi

echo "== gadget with isochronous endpoints =="
udc=/sys/class/udc/usbip-vudc.0
# Idempotent: a previous run leaves the gadget bound, and loading it
# again would fail with EBUSY.
if [ ! -d "$udc" ]; then
	modprobe usbip-vudc num=1
fi
if [ "$(cat $udc/function 2>/dev/null)" != g_audio ]; then
	modprobe g_audio
fi
printf '    %s: function=%s state=%s\n' "$(basename $udc)" \
    "$(cat $udc/function 2>/dev/null)" "$(cat $udc/state 2>/dev/null)"

echo "== endpoints the gadget offers =="
# The gadget is not plugged in anywhere yet, so this is the descriptor
# it will present rather than anything enumerated.
if [ -d /sys/kernel/config/usb_gadget ]; then
	find /sys/kernel/config/usb_gadget -maxdepth 2 -name UDC 2>/dev/null |
	    sed 's/^/    /'
fi

echo "== server =="
pkill usbipd 2>/dev/null || true
sleep 1
usbipd -e -D
sleep 1
usbip list -d 2>&1 | sed 's/^/    /'

echo "== capture =="
rm -f "$PCAP"
# Capture on every interface: the client is remote, and which address it
# arrives on is not worth guessing.
tcpdump -i any -s 0 -w "$PCAP" "tcp port $PORT" > /dev/null 2>&1 &
sleep 1
pgrep -l tcpdump | sed 's/^/    /'

addr=$(hostname -I 2>/dev/null | awk '{print $1}')
cat <<EOF

Server is up and the capture is running.  Nothing on this machine will
attach to it.  From the FreeBSD machine:

    kldload vhci.ko
    usbip attach -r $addr -b usbip-vudc.0
    tests/iso_probe

Then, back here:

    sudo $0 stop
EOF
