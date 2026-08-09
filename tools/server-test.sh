#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
#
# Exercise usbipd(8) with more than one client at once.
#
# The point is that a client holding a device must not stop anyone else
# from using the daemon, while the device itself still goes to exactly
# one of them.  Both halves are easy to get wrong in opposite
# directions: serving one connection at a time is safe but useless, and
# serving them all without exclusion hands the same endpoints to two
# hosts.
#
# There is no USB hardware on a test VM, so the device being exported is
# one vhci(4) has imported from the emulated server.  That is a chain,
# but it is the only way to have a local device to export at all.
#
#	sudo tools/server-test.sh

TOP=$(cd "$(dirname "$0")/.." && pwd)
KO=$TOP/sys/modules/vhci/vhci.ko
USBIP=$TOP/usr.sbin/usbip/usbip
USBIPD=$TOP/usr.sbin/usbipd/usbipd
EMU=$TOP/tests/fake_usbipd.py
EMUPORT=3241
PORT=3240

if [ "$(id -u)" != 0 ]; then
	echo "$0: must be run as root" >&2
	exit 1
fi

fail=0
emupid=""
srvpid=""

ok()  { echo "  ok: $1"; }
bad() { echo "  FAIL: $1"; fail=1; }

cleanup() {
	[ -n "$srvpid" ] && kill "$srvpid" 2>/dev/null
	[ -n "$emupid" ] && kill -9 "$emupid" 2>/dev/null
	"$USBIP" detach -p 0 >/dev/null 2>&1
	if kldstat | grep -q vhci; then
		sleep 1
		kldunload vhci 2>/dev/null
	fi
	return 0
}
trap cleanup EXIT INT TERM

echo "== a local device to export =="
python3 "$EMU" -p "$EMUPORT" > /tmp/server_emu.log 2>&1 &
emupid=$!
sleep 1
kldload "$KO" 2>/dev/null
sleep 2
"$USBIP" attach -r 127.0.0.1 -t "$EMUPORT" -b 1-1 >/dev/null 2>&1 || {
	echo "could not set up the device to export" >&2
	exit 1
}
sleep 3
busid=$("$USBIPD" -l | awk '/1209:0001/ {print $1}')
[ -n "$busid" ] && ok "exporting $busid" || { bad "no device to export"; exit 1; }

echo
echo "== start the daemon =="
"$USBIPD" -v -t "$PORT" "$busid" > /tmp/usbipd_test.log 2>&1 &
srvpid=$!
sleep 2
kill -0 "$srvpid" 2>/dev/null && ok "running" || { bad "died"; exit 1; }

echo
echo "== several clients listing at once =="
pids=""
for i in 1 2 3 4 5; do
	"$USBIP" list -r 127.0.0.1 -t "$PORT" > /tmp/list.$i 2>&1 &
	pids="$pids $!"
done
# Wait for these specifically: a bare "wait" would also wait for the
# daemon, which is a background job of this same shell and never exits.
for p in $pids; do
	wait "$p" 2>/dev/null
done
n=0
for i in 1 2 3 4 5; do
	grep -q "$busid" /tmp/list.$i && n=$((n + 1))
done
[ "$n" -eq 5 ] && ok "all five got the list" || \
    bad "only $n of 5 got the list"
rm -f /tmp/list.*

echo
echo "== a client holding the device must not block the others =="
# Import and keep the connection open by leaving the fd to a sleeper.
"$USBIP" attach -r 127.0.0.1 -t "$PORT" -b "$busid" > /tmp/attach.log 2>&1
if grep -q "attached to port" /tmp/attach.log; then
	ok "first client imported it"
else
	bad "first client could not import"
	sed 's/^/      /' /tmp/attach.log
fi
sleep 1

if "$USBIP" list -r 127.0.0.1 -t "$PORT" 2>&1 | grep -q "$busid"; then
	ok "listing still works while it is held"
else
	bad "listing blocked behind the held device"
fi

echo
echo "== a second import of the same device must be refused =="
if "$USBIP" attach -r 127.0.0.1 -t "$PORT" -b "$busid" > /tmp/attach2.log 2>&1
then
	bad "the device was handed out twice"
	sed 's/^/      /' /tmp/attach2.log
else
	ok "refused: $(tail -1 /tmp/attach2.log)"
fi

echo
echo "== releasing it makes it available again =="
"$USBIP" detach -p 1 >/dev/null 2>&1 || "$USBIP" detach -p 0 >/dev/null 2>&1
sleep 2
if "$USBIP" attach -r 127.0.0.1 -t "$PORT" -b "$busid" > /tmp/attach3.log 2>&1
then
	ok "imported again after release"
else
	bad "still refused after release"
	sed 's/^/      /' /tmp/attach3.log
fi

echo
echo "== daemon log =="
sed 's/^/    /' /tmp/usbipd_test.log

echo
[ $fail -eq 0 ] && echo "== server test passed ==" || echo "== server test FAILED =="
exit $fail
