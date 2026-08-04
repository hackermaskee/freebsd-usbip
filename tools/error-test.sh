#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
#
# Exercise the paths that only run when something goes wrong: a server
# that dies underneath an attached device, and one that dies in the
# middle of a transfer.  These are the parts most likely to strand a
# device or panic the machine, and the least likely to be reached by
# accident.
#
#	sudo tools/error-test.sh

TOP=$(cd "$(dirname "$0")/.." && pwd)
KO=$TOP/sys/modules/vhci/vhci.ko
USBIP=$TOP/usr.sbin/usbip/usbip
SERVER=$TOP/tests/fake_usbipd.py
BULK=$TOP/tests/bulk_test
PORT=${PORT:-3240}
LOG=/tmp/fake_usbipd_err.log

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

ok()   { echo "  ok: $1"; }
bad()  { echo "  FAIL: $1"; fail=1; }

start_server() {
	python3 "$SERVER" -p "$PORT" > "$LOG" 2>&1 &
	srvpid=$!
	sleep 1
}

attach() {
	"$USBIP" attach -r 127.0.0.1 -t "$PORT" -b 1-1 > /dev/null 2>&1
}

port_is_free() {
	"$USBIP" port 2>/dev/null | grep -q "no imported devices"
}

device_present() {
	usbconfig list 2>/dev/null | grep -q ugen0.2
}

echo "== setup =="
start_server
kldload "$KO" || exit 1
sleep 3
attach || { echo "attach failed"; exit 1; }
sleep 3
device_present && ok "device enumerated" || bad "device did not enumerate"

echo
echo "== server killed while idle =="
kill -9 "$srvpid" 2>/dev/null; srvpid=""
sleep 4
port_is_free && ok "port released" || bad "port still shows the dead device"
device_present && bad "device still enumerated" || ok "device removed"
"$USBIP" detach -p 0 >/dev/null 2>&1 && \
    bad "detaching an already released port succeeded" || \
    ok "detaching an already released port is refused"

echo
echo "== port is reusable =="
start_server
attach && ok "attached again" || bad "could not reattach"
sleep 3
device_present && ok "device enumerated again" || bad "no device after reattach"

echo
echo "== server killed during transfers =="
if [ -x "$BULK" ]; then
	# The loop form keeps the device open across iterations, so the
	# kill lands with a transfer outstanding rather than between
	# runs, which is the case this test exists for.
	"$BULK" loop > /tmp/bulk_during_kill.log 2>&1 &
	bulkpid=$!
	sleep 2
	kill -9 "$srvpid" 2>/dev/null; srvpid=""
	wait $bulkpid 2>/dev/null
	rc=$?

	if [ $rc -eq 0 ]; then
		bad "bulk_test exited cleanly; it was never interrupted"
	elif grep -q '^FAIL.*byte' /tmp/bulk_during_kill.log; then
		ok "an outstanding transfer failed rather than hanging"
		grep -m1 '^FAIL.*byte' /tmp/bulk_during_kill.log |
		    sed 's/^/      /'
	else
		bad "bulk_test failed, but not inside a transfer"
		tail -2 /tmp/bulk_during_kill.log | sed 's/^/      /'
	fi

	sleep 4
	port_is_free && ok "port released after mid-transfer death" || \
	    bad "port stranded after mid-transfer death"
	device_present && bad "device still enumerated" || ok "device removed"
else
	echo "  (tests/bulk_test not built; skipping)"
fi

echo
echo "== unload =="
sleep 1
if kldunload vhci; then
	ok "unloaded cleanly"
else
	bad "unload failed"
fi

echo
[ $fail -eq 0 ] && echo "== error test passed ==" || echo "== error test FAILED =="
exit $fail
