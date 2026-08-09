#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
#
# End-to-end test: load vhci(4), start the emulated USB/IP server on
# localhost, attach to it, and see whether the device enumerates.
#
#	sudo tools/attach-test.sh
#
# Everything runs on this machine, so no hardware and no remote server
# are needed.  Leaves the module unloaded and the server stopped.

TOP=$(cd "$(dirname "$0")/.." && pwd)
KO=$TOP/sys/modules/vhci/vhci.ko
USBIP=$TOP/usr.sbin/usbip/usbip
SERVER=$TOP/tests/fake_usbipd.py
PORT=${PORT:-3240}
LOG=/tmp/fake_usbipd.log

if [ "$(id -u)" != 0 ]; then
	echo "$0: must be run as root" >&2
	exit 1
fi
for f in "$KO" "$USBIP" "$SERVER"; do
	[ -e "$f" ] || { echo "$0: missing $f" >&2; exit 1; }
done

srvpid=""
cleanup() {
	[ -n "$srvpid" ] && kill "$srvpid" 2>/dev/null
	kldstat | grep -q vhci && { sleep 1; kldunload vhci 2>/dev/null; }
	return 0
}
trap cleanup EXIT INT TERM

mark=$(dmesg | wc -l)
new_dmesg() {
	dmesg | tail -n "+$((mark + 1))" | sed 's/^/    /'
	mark=$(dmesg | wc -l)
}

echo "== start emulated server on port $PORT =="
python3 "$SERVER" -p "$PORT" -v > "$LOG" 2>&1 &
srvpid=$!
sleep 1
kill -0 "$srvpid" 2>/dev/null || { echo "server died:"; cat "$LOG"; exit 1; }

echo "== load vhci =="
kldload "$KO" || exit 1
sleep 3
new_dmesg

echo "== list from the server =="
"$USBIP" list -r 127.0.0.1 -t "$PORT" | sed 's/^/    /'

echo "== attach =="
"$USBIP" attach -r 127.0.0.1 -t "$PORT" -b 1-1 | sed 's/^/    /'
echo "  attach exit=$?"

echo "== settle =="
sleep 4
new_dmesg

echo "== ports =="
"$USBIP" port | sed 's/^/    /'

echo "== usbconfig =="
usbconfig list 2>&1 | sed 's/^/    /'

echo "== enumerated device =="
usbconfig -d ugen0.2 dump_device_desc 2>&1 | sed 's/^/    /'

echo "== bulk loopback =="
if [ -x "$TOP/tests/bulk_test" ]; then
	"$TOP/tests/bulk_test" 2>&1 | sed 's/^/    /'
	echo "  bulk exit=$?"
else
	echo "    (tests/bulk_test not built; make -C tests bulk_test)"
fi

echo "== isochronous =="
if [ -x "$TOP/tests/iso_probe" ]; then
	"$TOP/tests/iso_probe" 1209 0001 2>&1 | sed 's/^/    /'
	echo "  iso exit=$?"
else
	echo "    (tests/iso_probe not built; make -C tests iso_probe)"
fi

echo "== detach =="
"$USBIP" detach -p 0 | sed 's/^/    /'
sleep 2
new_dmesg

echo "== server log =="
sed 's/^/    /' "$LOG"

echo "== done =="
