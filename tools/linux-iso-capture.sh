#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
#
# Capture what the canonical USB/IP implementation puts on the wire for
# isochronous transfers.
#
# The protocol documentation describes every other PDU field by field,
# but not the isochronous packet descriptor.  Rather than guess, run
# Linux against itself over the loopback interface - its own usbipd
# serving a UAC gadget on usbip-vudc, its own vhci-hcd attaching to it -
# and watch.  Nothing here reads the GPL implementation's source; it
# only observes a running system.
#
# No USB hardware is touched.
#
#	sudo tools/linux-iso-capture.sh
#	sudo tools/linux-iso-capture.sh stop
#
# Leaves the capture in /tmp/usbip-iso.pcap.

set -e

TOP=$(cd "$(dirname "$0")/.." && pwd)
PROBE=$TOP/tests/iso_probe
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

teardown() {
	usbip detach -p 00 2>/dev/null || true
	pkill tcpdump 2>/dev/null || true
	pkill usbipd 2>/dev/null || true
	sleep 1
	modprobe -r vhci-hcd 2>/dev/null || true
	modprobe -r g_audio 2>/dev/null || true
	modprobe -r usbip-vudc 2>/dev/null || true
	modprobe -r usbip-core 2>/dev/null || true
}

if [ "$1" = stop ]; then
	teardown
	echo "stopped"
	exit 0
fi

echo "== building the probe =="
if [ ! -x "$PROBE" ]; then
	cc -O2 -Wall -o "$PROBE" "$TOP/tests/iso_probe.c" -lusb-1.0
fi

echo "== gadget with isochronous endpoints =="
modprobe usbip-vudc num=1
modprobe g_audio
udc=/sys/class/udc/usbip-vudc.0
printf '    %s: function=%s\n' "$(basename $udc)" "$(cat $udc/function)"
if [ "$(cat $udc/function)" != gaudio ] && \
   [ "$(cat $udc/function)" != "g_audio" ]; then
	echo "    (function is '$(cat $udc/function)'; continuing anyway)"
fi

echo "== server =="
pkill usbipd 2>/dev/null || true
sleep 1
usbipd -e -D
sleep 1
usbip list -d 2>&1 | sed 's/^/    /'

echo "== capture =="
rm -f "$PCAP"
tcpdump -i lo -s 0 -w "$PCAP" "tcp port $PORT" > /dev/null 2>&1 &
sleep 1

echo "== client =="
modprobe vhci-hcd
usbip attach -r 127.0.0.1 -b usbip-vudc.0
sleep 3

vid=$(lsusb | sed -n 's/.*ID \([0-9a-f]*\):\([0-9a-f]*\) Linux.*Audio.*/\1 \2/p' | head -1)
[ -z "$vid" ] && vid="1d6b 0101"
echo "    probing $vid"

echo "== isochronous traffic =="
"$PROBE" $vid 2>&1 | sed 's/^/    /' || true

sleep 2
pkill tcpdump 2>/dev/null || true
sleep 1

echo "== what was captured =="
ls -l "$PCAP" | sed 's/^/    /'
tcpdump -r "$PCAP" 2>/dev/null | wc -l | sed 's/^/    packets: /'

teardown
echo
echo "Capture is in $PCAP."
