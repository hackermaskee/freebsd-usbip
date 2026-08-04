#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
#
# Point the driver at a server that answers with malformed PDUs.
#
# Everything vhci(4) parses after the hand-off comes off a TCP socket,
# so it is untrusted input being read in kernel context.  Each mode here
# should end with the session dropped and the port released, and in no
# case with a hang or a panic.
#
#	sudo tools/hostile-test.sh

TOP=$(cd "$(dirname "$0")/.." && pwd)
KO=$TOP/sys/modules/vhci/vhci.ko
USBIP=$TOP/usr.sbin/usbip/usbip
SERVER=$TOP/tests/hostile_usbipd.py
PORT=${PORT:-3240}

if [ "$(id -u)" != 0 ]; then
	echo "$0: must be run as root" >&2
	exit 1
fi

fail=0
srvpid=""

cleanup() {
	[ -n "$srvpid" ] && kill -9 "$srvpid" 2>/dev/null
	if kldstat | grep -q vhci; then
		sleep 1
		kldunload vhci 2>/dev/null
	fi
	return 0
}
trap cleanup EXIT INT TERM

kldload "$KO" || exit 1
sleep 3

for mode in $(python3 "$SERVER" --list | awk '{print $1}'); do
	echo "== $mode =="

	python3 "$SERVER" -p "$PORT" "$mode" > /tmp/hostile.log 2>&1 &
	srvpid=$!
	sleep 1

	if ! "$USBIP" attach -r 127.0.0.1 -t "$PORT" -b 1-1 > /dev/null 2>&1; then
		echo "  attach refused (fine: nothing was imported)"
	fi

	# Enumeration runs and hits the malformed reply.  Long enough for
	# the stack to give up and for the port to be released.
	sleep 8

	expect=$(python3 "$SERVER" --expect "$mode")

	if ! "$USBIP" port > /tmp/hostile_port.log 2>&1; then
		echo "  FAIL: usbip port failed, driver may be wedged"
		fail=1
	elif grep -q "no imported devices" /tmp/hostile_port.log; then
		if [ "$expect" = drop ]; then
			echo "  ok: session dropped, port released"
		else
			echo "  FAIL: session dropped, but this input is legal"
			fail=1
		fi
	else
		if [ "$expect" = survive ]; then
			echo "  ok: session survived, as it should"
		else
			echo "  FAIL: session should have been dropped:"
			sed 's/^/      /' /tmp/hostile_port.log
			fail=1
		fi
		"$USBIP" detach -p 0 >/dev/null 2>&1
	fi

	kill -9 "$srvpid" 2>/dev/null; srvpid=""
	sleep 1
done

echo
echo "== the machine is still here =="
dmesg | tail -6 | sed 's/^/    /'

echo
echo "== unload =="
sleep 1
if kldunload vhci; then
	echo "  ok: unloaded cleanly"
else
	echo "  FAIL: unload failed"
	fail=1
fi

echo
[ $fail -eq 0 ] && echo "== hostile test passed ==" || echo "== hostile test FAILED =="
exit $fail
