# SPDX-License-Identifier: BSD-2-Clause
#
# FreeBSD port for USB/IP.
#
# This is the port skeleton.  It has no distfile yet because the
# project has no public release: fill in MASTER_SITES, or switch to
# USE_GITHUB with the account and project, and run "make makesum".
# Everything else is in place.
#
# To try it against a local tree meanwhile:
#
#	make -C port DISTDIR=/tmp NO_CHECKSUM=yes \
#	    WRKSRC=/path/to/usbip-freebsd install

PORTNAME=	usbip
DISTVERSION=	0.1.0
CATEGORIES=	sysutils

MAINTAINER=	furuta@furuta.bsdclub.org
COMMENT=	USB/IP client and server for FreeBSD
WWW=		https://github.com/example/usbip-freebsd

LICENSE=	BSD2CLAUSE
LICENSE_FILE=	${WRKSRC}/LICENSE

# kmod arranges for vhci.ko to be built against the running kernel and
# installed where the loader looks; uidfix lets the build run without
# being root until the install step.
USES=		kmod uidfix
USE_RC_SUBR=	usbipd

# The tree's own Makefiles honour PREFIX, DESTDIR and KMODDIR, so the
# port does not have to know where anything goes.
MAKE_ARGS=	PREFIX=${PREFIX} KMODDIR=${KMODDIR}

PLIST_FILES=	sbin/usbip \
		sbin/usbipd \
		share/man/man4/vhci.4.gz \
		share/man/man8/usbip.8.gz \
		share/man/man8/usbipd.8.gz \
		/boot/modules/vhci.ko

# The kernel module needs kernel sources.  Say so plainly rather than
# failing halfway through a build.
.include <bsd.port.pre.mk>

.if !exists(${SRC_BASE}/sys/Makefile)
IGNORE=	requires kernel sources in ${SRC_BASE}/sys
.endif

do-build:
	${MAKE} -C ${WRKSRC} kmod SYSDIR=${SRC_BASE}/sys
	${MAKE} -C ${WRKSRC}/usr.sbin/usbip
	${MAKE} -C ${WRKSRC}/usr.sbin/usbipd

do-install:
	${MAKE} -C ${WRKSRC} install DESTDIR=${STAGEDIR} \
	    PREFIX=${PREFIX} KMODDIR=${KMODDIR} NO_RC_SCRIPT=1

.include <bsd.port.post.mk>
