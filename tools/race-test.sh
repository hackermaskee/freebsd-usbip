#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
#
# Unload vhci(4) the instant it is loaded, which used to panic the
# machine: the USB stack brings the root hub up on its own thread and
# tears down regardless of whether that finished, leaving the hub
# driver pointing at the module's freed memory.
#
# The driver now refuses to detach until the bus has settled, so the
# expected result is a failed unload followed by a clean one.
#
#	sudo tools/race-test.sh

KO=$(dirname "$0")/../sys/modules/vhci/vhci.ko

if [ "$(id -u)" != 0 ]; then
	echo "$0: must be run as root" >&2
	exit 1
fi

fail=0

echo "== load, then unload immediately =="
kldload "$KO" || { echo "load failed"; exit 1; }
if kldunload vhci 2>&1; then
	echo "  FAIL: unload succeeded during enumeration"
	fail=1
else
	echo "  ok: unload refused while the bus was coming up"
fi

echo "== wait for the bus to settle, then unload =="
sleep 4
if kldunload vhci 2>&1; then
	echo "  ok: unload succeeded once settled"
else
	echo "  FAIL: unload still refused after settling"
	fail=1
fi

if kldstat | grep -q vhci; then
	echo "  FAIL: vhci still loaded"
	kldunload vhci 2>/dev/null
	fail=1
fi

echo "== dmesg =="
dmesg | tail -8 | sed 's/^/    /'

[ $fail -eq 0 ] && echo "== race test passed ==" || echo "== race test FAILED =="
exit $fail
