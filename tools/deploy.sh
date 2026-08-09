#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
#
# Copy the tree to a test machine and build it there.
#
#	tools/deploy.sh usbip@ftest2
#
# Uses tar over ssh rather than rsync, because a freshly installed
# FreeBSD does not have rsync and installing one just to copy a few
# hundred kilobytes is not worth it.
#
# The copy replaces the destination wholesale, so nothing stale survives
# and every build starts from the sources that are actually here.  Built
# objects are never copied: one built on this machine is for the wrong
# system.  The exclusions live with the script, because keeping them in
# a temporary file once turned a deploy into a silent no-op.

set -e

TARGET=${1:?usage: $0 user@host}
TOP=$(cd "$(dirname "$0")/.." && pwd)
DEST=${DEST:-usbip-freebsd}

echo "== copying to $TARGET:$DEST =="
tar czf - -C "$TOP" \
    --exclude=.git \
    --exclude='*.o' \
    --exclude='*.ko' \
    --exclude='*.kld' \
    --exclude='.depend*' \
    --exclude=export_syms \
    --exclude=machine \
    --exclude=x86 \
    --exclude=i386 \
    --exclude='opt_*.h' \
    --exclude=bus_if.h \
    --exclude=device_if.h \
    --exclude=usb_if.h \
    --exclude=usr.sbin/usbip/usbip \
    --exclude=usr.sbin/usbipd/usbipd \
    --exclude=tests/proto_test \
    --exclude=tests/bulk_test \
    --exclude=tests/iso_probe \
    --exclude=__pycache__ \
    . |
ssh "$TARGET" "rm -rf $DEST.new && mkdir -p $DEST.new &&
    tar xzf - -C $DEST.new &&
    rm -rf $DEST && mv $DEST.new $DEST"

echo "== building =="
ssh "$TARGET" "set -e
cd $DEST
if [ -d \$HOME/src/sys ]; then
	SYSDIR=\$HOME/src/sys
elif [ -d /usr/src/sys ]; then
	SYSDIR=/usr/src/sys
else
	SYSDIR=
fi
if [ -n \"\$SYSDIR\" ]; then
	make -C sys/modules/vhci SYSDIR=\$SYSDIR 2>&1 | grep -E ': error|: warning' || true
else
	echo '   (no kernel source tree; skipped the module)'
fi
make -C usr.sbin/usbip 2>&1 | grep -E ': error|: warning' || true
make -C usr.sbin/usbipd 2>&1 | grep -E ': error|: warning' || true
make -C tests bulk_test iso_probe 2>&1 | grep -E ': error|: warning' || true
echo '-- built --'
for f in sys/modules/vhci/vhci.ko usr.sbin/usbip/usbip usr.sbin/usbipd/usbipd tests/bulk_test tests/iso_probe; do
	[ -f \$f ] && echo \"    \$f\"
done
"
