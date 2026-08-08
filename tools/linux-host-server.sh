#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
#
# Export a real USB device from a real Linux usbipd, and capture the
# traffic.  This is the only way left to see what a server's
# isochronous reply looks like: usbip-vudc never completes an
# isochronous URB, so a software gadget cannot answer the question.
#
# THIS TAKES THE DEVICE AWAY from this machine while it is bound.  Its
# drivers are unbound and it disappears from the host until "stop"
# below, so do not do this to something in use.
#
# Be ready to unplug it afterwards.  A device that stops responding
# while exported - which a real one may well do - has to be power
# cycled before the host sees it again.
#
# The client must be ANOTHER MACHINE.  Do not attach this host to the
# device it is itself exporting: the client and server drivers end up
# in the same kernel waiting on each other through a loopback socket,
# and it hangs hard enough to need the power switch.  This script
# refuses to start if vhci-hcd is loaded, and never attaches anything
# itself.
#
#	sudo tools/linux-host-server.sh start 3-1
#	  ... drive it from the FreeBSD machine ...
#	sudo tools/linux-host-server.sh stop 3-1
#
# Leaves the capture in /tmp/usbip-iso.pcap.

set -e

PCAP=${PCAP:-/tmp/usbip-iso.pcap}
PORT=3240
ACTION=${1:-}
BUSID=${2:-}

if [ "$(id -u)" != 0 ]; then
	echo "$0: must be run as root" >&2
	exit 1
fi
if [ "$(uname -s)" != Linux ]; then
	echo "$0: this is the Linux side" >&2
	exit 1
fi
if [ -z "$BUSID" ]; then
	echo "usage: $0 start|stop <busid>" >&2
	echo "run 'usbip list -l' to see the bus ids" >&2
	exit 1
fi

case "$ACTION" in
stop)
	pkill tcpdump 2>/dev/null || true
	sleep 1
	if [ -f "$PCAP" ]; then
		echo "capture: $PCAP"
		tcpdump -r "$PCAP" 2>/dev/null | wc -l |
		    sed 's/^/    packets: /'
	fi
	pkill usbipd 2>/dev/null || true
	sleep 1
	echo "== giving the device back =="
	usbip unbind -b "$BUSID" 2>&1 | sed 's/^/    /' || true
	sleep 2

	#
	# unbind fails if the device stopped responding while it was
	# exported, and it is exactly then that this matters: the busid
	# stays in usbip-host's match list, so the driver grabs the
	# device again every time it re-enumerates and it never comes
	# back.  Replugging does not help, because the port is the same.
	#
	# Note the missing newline: the kernel compares whatever follows
	# "del " verbatim, so an echo would leave a stray \n and the
	# entry would not match.
	#
	match=/sys/bus/usb/drivers/usbip-host/match_busid
	if [ -e "$match" ] && grep -qx "$BUSID" "$match" 2>/dev/null; then
		echo "== releasing the busid usbip-host still claims =="
		printf 'del %s' "$BUSID" > "$match" 2>/dev/null ||
		    echo "    could not remove $BUSID; unloading instead"
	fi
	# Belt and braces: with the driver gone, nothing can claim it.
	modprobe -r usbip-host 2>/dev/null || true
	sleep 1
	echo "== the device should be back on this machine =="
	lsusb -s "$(echo "$BUSID" | cut -d- -f1):" 2>/dev/null | sed 's/^/    /' ||
	    lsusb | sed 's/^/    /'
	cat <<'EOF'

If it is not listed, unplug it and plug it back in, power cycling it if
it has its own supply.  A device that stopped responding while exported
needs that; the host cannot bring it back on its own.
EOF
	echo "stopped"
	exit 0
	;;
start)
	;;
*)
	echo "usage: $0 start|stop <busid>" >&2
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

echo "== the device about to be taken over =="
usbip list -l 2>/dev/null | grep -A1 "busid $BUSID" | sed 's/^/    /' ||
    { echo "    no such busid: $BUSID" >&2; exit 1; }

echo "== what is using it now =="
for d in /sys/bus/usb/devices/"$BUSID":*; do
	[ -e "$d/driver" ] || continue
	printf '    %s -> %s\n' "$(basename "$d")" \
	    "$(basename "$(readlink -f "$d/driver")")"
done

echo "== exporting =="
modprobe usbip-host
pkill usbipd 2>/dev/null || true
sleep 1
usbipd -D
sleep 1
usbip bind -b "$BUSID" 2>&1 | sed 's/^/    /'
sleep 1
usbip list -r 127.0.0.1 2>&1 | sed 's/^/    /'

echo "== capture =="
rm -f "$PCAP"
tcpdump -i any -s 0 -w "$PCAP" "tcp port $PORT" > /dev/null 2>&1 &
sleep 1
pgrep -l tcpdump | sed 's/^/    /'

addr=$(hostname -I 2>/dev/null | awk '{print $1}')
cat <<EOF

Exported and capturing.  Nothing on this machine will attach to it.
From the FreeBSD machine:

    usbip attach -r $addr -b $BUSID
    tests/iso_probe <vendor> <product>

When finished, and to give the device back:

    sudo $0 stop $BUSID
EOF
