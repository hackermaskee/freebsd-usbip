#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
#
# Load vhci(4), look at what it did, and unload it again.  Run as root
# on the FreeBSD test machine:
#
#	sudo tools/smoke-test.sh
#
# This is the first thing to run after building the module.  It does not
# talk to any server; it only checks that the driver attaches, that the
# root hub appears, and that it unloads cleanly.

set -e

KO=$(dirname "$0")/../sys/modules/vhci/vhci.ko

if [ "$(id -u)" != 0 ]; then
	echo "$0: must be run as root" >&2
	exit 1
fi
if [ ! -f "$KO" ]; then
	echo "$0: $KO not built" >&2
	exit 1
fi

mark=$(dmesg | wc -l)
show_new_dmesg() {
	dmesg | tail -n "+$((mark + 1))" | sed 's/^/    /'
	mark=$(dmesg | wc -l)
}

echo "== loading =="
kldload "$KO"
show_new_dmesg

echo "== module =="
kldstat -v | grep -i vhci | sed 's/^/    /' || echo "    (no vhci in kldstat)"

echo "== device node =="
ls -l /dev/vhci 2>&1 | sed 's/^/    /'

echo "== newbus =="
devinfo 2>/dev/null | grep -A2 -i vhci | sed 's/^/    /' || \
    echo "    (devinfo not available)"

echo "== USB buses =="
usbconfig list 2>&1 | sed 's/^/    /'

echo "== unloading =="
kldunload vhci
show_new_dmesg

echo "== done: loaded and unloaded without a panic =="
