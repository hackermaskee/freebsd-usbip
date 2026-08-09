/* SPDX-License-Identifier: BSD-2-Clause */
/*-
 * Copyright (c) 2026 furuta@furuta.bsdclub.org
 *
 * usbipd(8): export local USB devices over USB/IP.
 *
 * Runs entirely in userland on top of libusb, so no kernel support is
 * needed on this side; the device is claimed through ugen(4) like any
 * other libusb program.
 *
 * Clients are served concurrently, on a thread each, so that a machine
 * holding a device does not stop others from listing what is on offer or
 * importing something else.  A device itself still goes to one client at
 * a time, which is what the protocol assumes.
 */

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/wait.h>

#include <netinet/in.h>
#include <netinet/tcp.h>

#include <err.h>
#include <errno.h>
#include <libutil.h>
#include <netdb.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "usbipd.h"

static volatile sig_atomic_t stop_requested;

static void
on_signal(int sig)
{

	(void)sig;
	stop_requested = 1;
}

void
usbipd_log(const struct usbipd *d, const char *fmt, ...)
{
	struct usbipd *dd = __DECONST(struct usbipd *, d);
	va_list ap;

	if (!d->verbose)
		return;
	/*
	 * Serialised so that two clients cannot interleave halves of
	 * each other's lines.
	 */
	pthread_mutex_lock(&dd->lock);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	fflush(stderr);
	pthread_mutex_unlock(&dd->lock);
}

/*
 * A device goes to one client at a time.  Anything else would let two
 * hosts drive the same endpoints, and neither would get sense back.
 */
bool
usbipd_acquire(struct usbipd *d, const char *busid)
{
	int i, free_slot = -1;

	pthread_mutex_lock(&d->lock);
	for (i = 0; i < USBIPD_MAX_SESSIONS; i++) {
		if (d->inuse[i][0] == '\0') {
			if (free_slot < 0)
				free_slot = i;
		} else if (strcmp(d->inuse[i], busid) == 0) {
			pthread_mutex_unlock(&d->lock);
			return (false);		/* someone already has it */
		}
	}
	if (free_slot < 0) {
		pthread_mutex_unlock(&d->lock);
		return (false);			/* no room for another */
	}
	strlcpy(d->inuse[free_slot], busid, sizeof(d->inuse[free_slot]));
	pthread_mutex_unlock(&d->lock);
	return (true);
}

void
usbipd_release(struct usbipd *d, const char *busid)
{
	int i;

	pthread_mutex_lock(&d->lock);
	for (i = 0; i < USBIPD_MAX_SESSIONS; i++) {
		if (strcmp(d->inuse[i], busid) == 0) {
			d->inuse[i][0] = '\0';
			break;
		}
	}
	pthread_mutex_unlock(&d->lock);
}

bool
usbipd_is_exported(const struct usbipd *d, const char *busid)
{
	int i;

	if (d->exportable_is_open)
		return (true);
	for (i = 0; i < d->nexports; i++) {
		if (strcmp(d->exports[i], busid) == 0)
			return (true);
	}
	return (false);
}

static void
usage(void)
{

	fprintf(stderr,
	    "usage: usbipd [-Dv] [-4|-6] [-t port] [-P pidfile] [-a] "
	    "busid ...\n"
	    "       usbipd -l\n"
	    "\n"
	    "  -l   list local devices and exit\n"
	    "  -a   export every device, rather than only those named\n"
	    "  -t   TCP port to listen on (default %s)\n"
	    "  -D   run in the background\n"
	    "  -P   write the process id here\n"
	    "  -v   report what happens\n",
	    USBIP_PORT_STRING);
	exit(1);
}

/*
 * List what could be exported.  The bus id in the first column is what
 * a client passes to "usbip attach -b".
 */
static int
list_local(struct usbipd *d)
{
	libusb_device **list;
	struct usbip_usb_device udev;
	struct usbip_usb_interface intfs[USBIP_MAX_INTERFACES];
	uint8_t nintfs, i;
	ssize_t n, k;

	n = libusb_get_device_list(d->ctx, &list);
	if (n < 0) {
		warnx("cannot list devices: %s", libusb_strerror((int)n));
		return (1);
	}

	printf("Local USB devices:\n");
	for (k = 0; k < n; k++) {
		if (usbipd_describe(list[k], &udev, intfs, &nintfs) != 0)
			continue;
		printf("  %-14s %04x:%04x  %s\n", udev.busid, udev.idVendor,
		    udev.idProduct, usbip_speed_string(udev.speed));
		for (i = 0; i < nintfs; i++) {
			printf("%17s if%u  class %02x/%02x/%02x\n", "", i,
			    intfs[i].bInterfaceClass,
			    intfs[i].bInterfaceSubClass,
			    intfs[i].bInterfaceProtocol);
		}
	}
	if (n == 0)
		printf("  (none)\n");

	libusb_free_device_list(list, 1);
	return (0);
}

/* OP_REQ_DEVLIST: every device this daemon is willing to hand out. */
static int
serve_devlist(struct usbipd *d, int fd)
{
	libusb_device **list;
	struct usbip_usb_device udev;
	struct usbip_usb_interface intfs[USBIP_MAX_INTERFACES];
	uint32_t count = 0, be;
	uint8_t nintfs;
	ssize_t n, k;
	int error = 0;

	n = libusb_get_device_list(d->ctx, &list);
	if (n < 0)
		return (usbip_net_send_op_reply(fd, OP_REP_DEVLIST,
		    USBIP_ST_NA));

	for (k = 0; k < n; k++) {
		char busid[USBIP_BUSID_SIZE];

		usbipd_busid(list[k], busid, sizeof(busid));
		if (usbipd_is_exported(d, busid))
			count++;
	}

	if (usbip_net_send_op_reply(fd, OP_REP_DEVLIST, USBIP_ST_OK) != 0)
		goto out;
	be = htonl(count);
	if (usbip_net_send_all_quiet(fd, &be, sizeof(be)) != 0)
		goto out;

	for (k = 0; k < n; k++) {
		char busid[USBIP_BUSID_SIZE];

		usbipd_busid(list[k], busid, sizeof(busid));
		if (!usbipd_is_exported(d, busid))
			continue;
		if (usbipd_describe(list[k], &udev, intfs, &nintfs) != 0)
			continue;

		usbip_usb_device_hton(&udev);
		if (usbip_net_send_all_quiet(fd, &udev, sizeof(udev)) != 0)
			goto out;
		if (nintfs != 0 && usbip_net_send_all_quiet(fd, intfs,
		    (size_t)nintfs * sizeof(intfs[0])) != 0)
			goto out;
	}
	error = 0;
out:
	libusb_free_device_list(list, 1);
	return (error);
}

/*
 * OP_REQ_IMPORT: hand a device over and then serve its transfers.  The
 * connection belongs to that device until the client goes away.
 */
static int
serve_import(struct usbipd *d, int fd)
{
	char busid[USBIP_BUSID_SIZE];
	struct usbip_usb_device udev;
	struct usbip_usb_interface intfs[USBIP_MAX_INTERFACES];
	libusb_device *dev;
	libusb_device_handle *dh;
	uint8_t nintfs;
	int error;

	if (usbip_net_recv_exact_quiet(fd, busid, sizeof(busid)) != 0)
		return (-1);
	busid[sizeof(busid) - 1] = '\0';

	if (!usbipd_is_exported(d, busid)) {
		usbipd_log(d, "refused %s: not exported", busid);
		return (usbip_net_send_op_reply(fd, OP_REP_IMPORT,
		    USBIP_ST_NA));
	}

	/*
	 * Claim it before opening.  Two clients importing the same
	 * device would each think they had it to themselves.
	 */
	if (!usbipd_acquire(d, busid)) {
		usbipd_log(d, "refused %s: already imported", busid);
		return (usbip_net_send_op_reply(fd, OP_REP_IMPORT,
		    USBIP_ST_NA));
	}

	dev = usbipd_find(d->ctx, busid);
	if (dev == NULL) {
		usbipd_log(d, "refused %s: no such device", busid);
		usbipd_release(d, busid);
		return (usbip_net_send_op_reply(fd, OP_REP_IMPORT,
		    USBIP_ST_NA));
	}

	if (usbipd_describe(dev, &udev, intfs, &nintfs) != 0) {
		libusb_unref_device(dev);
		usbipd_release(d, busid);
		return (usbip_net_send_op_reply(fd, OP_REP_IMPORT,
		    USBIP_ST_NA));
	}

	error = libusb_open(dev, &dh);
	libusb_unref_device(dev);
	if (error != 0) {
		usbipd_log(d, "refused %s: %s", busid, libusb_strerror(error));
		usbipd_release(d, busid);
		return (usbip_net_send_op_reply(fd, OP_REP_IMPORT,
		    USBIP_ST_NA));
	}

	if (usbip_net_send_op_reply(fd, OP_REP_IMPORT, USBIP_ST_OK) != 0)
		goto done;
	usbip_usb_device_hton(&udev);
	if (usbip_net_send_all_quiet(fd, &udev, sizeof(udev)) != 0)
		goto done;

	usbipd_log(d, "exported %s", busid);
	usbipd_serve_urbs(d, fd, dh);
	usbipd_log(d, "released %s", busid);

done:
	libusb_close(dh);
	usbipd_release(d, busid);
	return (0);
}

struct client {
	struct usbipd	*d;
	int		fd;
	char		peer[NI_MAXHOST];
};

static void
serve_client(struct usbipd *d, int fd)
{
	uint16_t code;
	int one = 1;

	(void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

	if (usbip_net_recv_op_request(fd, &code) != 0)
		return;

	switch (code) {
	case OP_REQ_DEVLIST:
		(void)serve_devlist(d, fd);
		break;
	case OP_REQ_IMPORT:
		(void)serve_import(d, fd);
		break;
	default:
		usbipd_log(d, "unexpected request %#06x", code);
		(void)usbip_net_send_op_reply(fd, code & 0x7FFF, USBIP_ST_NA);
		break;
	}
}

/*
 * One thread per connection.  A client that has imported a device holds
 * its connection open for as long as it uses the device, so serving
 * them one after another would mean nobody else could so much as ask
 * what is available.
 */
static void *
client_thread(void *arg)
{
	struct client *c = arg;
	struct usbipd *d = c->d;

	serve_client(d, c->fd);
	close(c->fd);
	usbipd_log(d, "%s disconnected", c->peer);

	pthread_mutex_lock(&d->lock);
	d->nclients--;
	pthread_mutex_unlock(&d->lock);

	free(c);
	return (NULL);
}

static int
listen_on(const char *service, int family)
{
	struct addrinfo hints, *res, *ai;
	int fd = -1, on = 1, error;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = family;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_PASSIVE;

	error = getaddrinfo(NULL, service, &hints, &res);
	if (error != 0)
		errx(1, "%s", gai_strerror(error));

	for (ai = res; ai != NULL; ai = ai->ai_next) {
		fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
		if (fd < 0)
			continue;
		(void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on,
		    sizeof(on));
		if (ai->ai_family == AF_INET6) {
			int off = 0;

			/* Accept IPv4 on the same socket where allowed. */
			(void)setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &off,
			    sizeof(off));
		}
		if (bind(fd, ai->ai_addr, ai->ai_addrlen) == 0 &&
		    listen(fd, 4) == 0)
			break;
		close(fd);
		fd = -1;
	}
	freeaddrinfo(res);

	if (fd < 0)
		err(1, "cannot listen on port %s", service);
	return (fd);
}

int
main(int argc, char **argv)
{
	struct usbipd d;
	struct sigaction sa_int;
	const char *service = USBIP_PORT_STRING;
	struct pidfh *pfh = NULL;
	const char *pidfile = NULL;
	int family = AF_UNSPEC;
	int ch, lfd, listing = 0, background = 0, error, i;

	memset(&d, 0, sizeof(d));

	while ((ch = getopt(argc, argv, "46aDlP:t:v")) != -1) {
		switch (ch) {
		case 'D':
			background = 1;
			break;
		case 'P':
			pidfile = optarg;
			break;
		case '4':
			family = AF_INET;
			break;
		case '6':
			family = AF_INET6;
			break;
		case 'a':
			d.exportable_is_open = true;
			break;
		case 'l':
			listing = 1;
			break;
		case 't':
			service = optarg;
			break;
		case 'v':
			d.verbose = 1;
			break;
		default:
			usage();
		}
	}
	argc -= optind;
	argv += optind;

	for (i = 0; i < argc; i++) {
		if (d.nexports >= USBIPD_MAX_EXPORTS)
			errx(1, "too many devices; the limit is %d",
			    USBIPD_MAX_EXPORTS);
		d.exports[d.nexports++] = argv[i];
	}
	if (!listing && d.nexports == 0 && !d.exportable_is_open) {
		warnx("name at least one device to export, or use -a");
		usage();
	}

	pthread_mutex_init(&d.lock, NULL);

	error = libusb_init(&d.ctx);
	if (error != 0)
		errx(1, "libusb: %s", libusb_strerror(error));

	if (listing) {
		error = list_local(&d);
		libusb_exit(d.ctx);
		return (error);
	}

	/* A client that vanishes mid-write must not take us with it. */
	signal(SIGPIPE, SIG_IGN);

	/*
	 * Deliberately not signal(3): on BSD it installs the handler with
	 * SA_RESTART, so accept() would resume instead of returning, and
	 * the daemon would not notice it had been asked to stop until the
	 * next client happened to connect.
	 */
	memset(&sa_int, 0, sizeof(sa_int));
	sa_int.sa_handler = on_signal;
	sigemptyset(&sa_int.sa_mask);
	sa_int.sa_flags = 0;
	sigaction(SIGINT, &sa_int, NULL);
	sigaction(SIGTERM, &sa_int, NULL);

	lfd = listen_on(service, family);

	/*
	 * Bind before detaching, so that a port already in use is
	 * reported to whoever started us rather than disappearing into
	 * the background.
	 */
	if (pidfile != NULL) {
		pid_t other;

		pfh = pidfile_open(pidfile, 0600, &other);
		if (pfh == NULL) {
			if (errno == EEXIST)
				errx(1, "already running as pid %jd",
				    (intmax_t)other);
			warn("cannot write %s", pidfile);
		}
	}
	if (background && daemon(0, 0) != 0) {
		pidfile_remove(pfh);
		err(1, "cannot detach");
	}
	if (pfh != NULL)
		pidfile_write(pfh);

	usbipd_log(&d, "listening on port %s", service);

	while (!stop_requested) {
		struct sockaddr_storage sa;
		socklen_t salen = sizeof(sa);
		char host[NI_MAXHOST];
		struct client *c;
		pthread_t tid;
		int fd;

		fd = accept(lfd, (struct sockaddr *)&sa, &salen);
		if (fd < 0) {
			if (errno == EINTR)
				continue;
			warn("accept");
			break;
		}
		if (getnameinfo((struct sockaddr *)&sa, salen, host,
		    sizeof(host), NULL, 0, NI_NUMERICHOST) != 0)
			strlcpy(host, "?", sizeof(host));

		c = calloc(1, sizeof(*c));
		if (c == NULL) {
			close(fd);
			continue;
		}
		c->d = &d;
		c->fd = fd;
		strlcpy(c->peer, host, sizeof(c->peer));

		pthread_mutex_lock(&d.lock);
		if (d.nclients >= USBIPD_MAX_CLIENTS) {
			pthread_mutex_unlock(&d.lock);
			usbipd_log(&d, "refusing %s: too many clients", host);
			close(fd);
			free(c);
			continue;
		}
		d.nclients++;
		pthread_mutex_unlock(&d.lock);

		usbipd_log(&d, "connection from %s", host);
		if (pthread_create(&tid, NULL, client_thread, c) != 0) {
			pthread_mutex_lock(&d.lock);
			d.nclients--;
			pthread_mutex_unlock(&d.lock);
			warn("cannot serve %s", host);
			close(fd);
			free(c);
			continue;
		}
		/* Nothing waits for it; it cleans up after itself. */
		pthread_detach(tid);
	}

	close(lfd);

	/*
	 * Let anyone still connected finish rather than pulling libusb
	 * out from under their transfers.  Bounded, because a client
	 * that never goes away should not stop us exiting either.
	 */
	for (i = 0; i < 100; i++) {
		int busy;

		pthread_mutex_lock(&d.lock);
		busy = d.nclients;
		pthread_mutex_unlock(&d.lock);
		if (busy == 0)
			break;
		if (i == 0)
			usbipd_log(&d, "waiting for %d client(s)", busy);
		usleep(100000);
	}

	libusb_exit(d.ctx);
	pthread_mutex_destroy(&d.lock);
	if (pfh != NULL)
		pidfile_remove(pfh);
	return (0);
}
