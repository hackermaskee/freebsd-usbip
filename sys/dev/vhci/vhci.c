/* SPDX-License-Identifier: BSD-2-Clause */
/*-
 * Copyright (c) 2026 furuta@furuta.bsdclub.org
 *
 * vhci(4): virtual USB host controller for USB/IP.
 *
 * The driver attaches to nexus as a pseudo-device, grows a usbus child
 * so the normal FreeBSD USB stack runs on top of it, and emulates a
 * root hub whose ports are filled in by usbip(8) over /dev/vhci.  Each
 * occupied port owns a TCP socket to a remote USB/IP server.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/bus.h>
#include <sys/conf.h>
#include <sys/malloc.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/condvar.h>
#include <sys/sx.h>
#include <sys/capsicum.h>
#include <sys/file.h>
#include <sys/filedesc.h>
#include <sys/socket.h>
#include <sys/socketvar.h>
#include <sys/protosw.h>
#include <sys/proc.h>
#include <sys/taskqueue.h>

#include <netinet/in.h>
#include <netinet/tcp.h>

#include <machine/bus.h>

#include <dev/usb/usb.h>
#include <dev/usb/usbdi.h>
#include <dev/usb/usb_core.h>
#include <dev/usb/usb_busdma.h>
#include <dev/usb/usb_process.h>
#include <dev/usb/usb_transfer.h>
#include <dev/usb/usb_device.h>
#include <dev/usb/usb_hub.h>
#include <dev/usb/usb_util.h>
#include <dev/usb/usb_controller.h>
#include <dev/usb/usb_bus.h>

#include "vhci.h"

static MALLOC_DEFINE(M_VHCI, "vhci", "USB/IP virtual host controller");

/*
 * Socket buffer sizes.  Large enough that a maximum-size bulk transfer
 * does not stall on the socket buffer high-water mark.
 */
#define	VHCI_SOCKBUF_SIZE	(1024 * 1024)

static void
vhci_ep_init(struct usb_device *udev, struct usb_endpoint_descriptor *edesc,
    struct usb_endpoint *ep)
{
	struct vhci_softc *sc = VHCI_BUS2SC(udev->bus);

	/* The root hub is served by roothub_exec, not by transfers. */
	if (udev->device_index == sc->sc_rt_addr)
		return;

	switch (edesc->bmAttributes & UE_XFERTYPE) {
	case UE_CONTROL:
	case UE_BULK:
	case UE_INTERRUPT:
	case UE_ISOCHRONOUS:
		ep->methods = &vhci_pipe_methods;
		break;
	default:
		/* Leaving methods NULL marks the endpoint unsupported. */
		break;
	}
}

static void
vhci_xfer_setup(struct usb_setup_params *parm)
{
	struct vhci_urb *urb;

	/*
	 * We never touch hardware, so these only have to be non-zero
	 * and large enough not to constrain the stack.
	 */
	parm->hc_max_packet_size = 0x500;
	parm->hc_max_packet_count = 3;
	parm->hc_max_frame_size = 3 * 0x500;

	usbd_transfer_setup_sub(parm);
	if (parm->err != 0)
		return;

	/*
	 * Carve the per-transfer USB/IP state out of the transfer's own
	 * allocation, so submitting a transfer never has to allocate.
	 */
	parm->size[0] += ((-parm->size[0]) & (USB_HOST_ALIGN - 1));
	if (parm->buf != NULL) {
		urb = USB_ADD_BYTES(parm->buf, parm->size[0]);
		memset(urb, 0, sizeof(*urb));
		urb->xfer = parm->curr_xfer;
		parm->curr_xfer->qh_start[0] = urb;
	}
	parm->size[0] += sizeof(*urb);
}

static void
vhci_xfer_unsetup(struct usb_xfer *xfer)
{

	(void)xfer;
}

/*
 * Absorb SET_ADDRESS instead of sending it.
 *
 * The remote device is already enumerated and addressed on the server's
 * own bus, and USB/IP identifies it by devid rather than by USB
 * address.  Putting a SET_ADDRESS on the wire would change the address
 * the server itself is talking to and break the session.  Returning
 * anything but USB_ERR_INVAL tells the stack we handled it; it assigns
 * the address locally, which is all that is needed.
 */
static usb_error_t
vhci_set_address(struct usb_device *udev, struct mtx *mtx, uint16_t addr)
{

	(void)mtx;
	(void)addr;

	/* The root hub is emulated, so let the normal path handle it. */
	if (udev->parent_hub == NULL)
		return (USB_ERR_INVAL);

	return (USB_ERR_NORMAL_COMPLETION);
}

/*
 * get_dma_delay, clear_stall, xfer_poll and the device-mode methods are
 * deliberately absent.  We have no DMA engine to drain, stall recovery
 * in host mode is a CLEAR_FEATURE sent over the wire by the stack, and
 * a socket cannot be polled from a panic context.
 */
static const struct usb_bus_methods vhci_bus_methods = {
	.roothub_exec = vhci_roothub_exec,
	.endpoint_init = vhci_ep_init,
	.xfer_setup = vhci_xfer_setup,
	.xfer_unsetup = vhci_xfer_unsetup,
	.set_address = vhci_set_address,
};

/*
 * Translate an imported device's USB/IP speed into the root hub port
 * status bits.  Neither bit set means full speed.
 */
static int
vhci_speed_bits(uint32_t usbip_speed, uint16_t *bitsp)
{

	switch (usbip_speed) {
	case USBIP_SPEED_LOW:
		*bitsp = UPS_LOW_SPEED;
		return (0);
	case USBIP_SPEED_FULL:
		*bitsp = 0;
		return (0);
	case USBIP_SPEED_HIGH:
		*bitsp = UPS_HIGH_SPEED;
		return (0);
	default:
		/* This root hub is USB 2.0; super speed needs an xHCI. */
		return (EINVAL);
	}
}

/*
 * Take ownership of a connected socket from userland.  After this the
 * caller's file descriptor is dead, so the daemon exiting cannot pull
 * the transport out from under us.
 */
static int
vhci_socket_take(struct thread *td, int fd, struct socket **sop)
{
	struct file *fp;
	struct socket *so;
	cap_rights_t rights;
	int error, one;

	error = fget(td, fd, cap_rights_init_one(&rights, CAP_SOCK_CLIENT),
	    &fp);
	if (error != 0)
		return (error);
	if (fp->f_type != DTYPE_SOCKET) {
		fdrop(fp, td);
		return (ENOTSOCK);
	}
	so = fp->f_data;
	if (so->so_type != SOCK_STREAM) {
		fdrop(fp, td);
		return (EINVAL);
	}

	fp->f_ops = &badfileops;
	fp->f_data = NULL;
	fdrop(fp, td);

	/* USB/IP is latency sensitive and strictly request/response. */
	one = 1;
	(void)so_setsockopt(so, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
	(void)soreserve(so, VHCI_SOCKBUF_SIZE, VHCI_SOCKBUF_SIZE);
	so->so_snd.sb_flags |= SB_AUTOSIZE;
	so->so_rcv.sb_flags |= SB_AUTOSIZE;

	*sop = so;
	return (0);
}

static int
vhci_ioctl_attach(struct vhci_softc *sc, struct vhci_ioc_attach *ia,
    struct thread *td)
{
	struct vhci_port *port;
	struct socket *so;
	uint16_t speed_bits;
	int error, i;

	if (vhci_speed_bits(ia->speed, &speed_bits) != 0)
		return (EINVAL);
	if (ia->port >= VHCI_NPORTS)
		return (EINVAL);

	error = vhci_socket_take(td, ia->fd, &so);
	if (error != 0)
		return (error);

	sx_xlock(&sc->sc_sx);

	port = NULL;
	if (ia->port >= 0) {
		if (!sc->sc_port[ia->port].connected)
			port = &sc->sc_port[ia->port];
	} else {
		for (i = 0; i < VHCI_NPORTS; i++) {
			if (!sc->sc_port[i].connected) {
				port = &sc->sc_port[i];
				break;
			}
		}
	}
	if (port == NULL) {
		sx_xunlock(&sc->sc_sx);
		soclose(so);
		return (EBUSY);
	}

	port->so = so;
	port->devid = ia->devid;
	port->usbip_speed = ia->speed;
	strlcpy(port->busid, ia->busid, sizeof(port->busid));
	strlcpy(port->host, ia->host, sizeof(port->host));

	error = vhci_session_start(port);
	if (error != 0) {
		port->so = NULL;
		sx_xunlock(&sc->sc_sx);
		soclose(so);
		return (error);
	}

	USB_BUS_LOCK(&sc->sc_bus);
	port->speed_bits = speed_bits;
	port->connected = 1;
	port->change_connect = 1;
	vhci_root_intr(sc, port->index);
	USB_BUS_UNLOCK(&sc->sc_bus);

	ia->port = port->index;
	sx_xunlock(&sc->sc_sx);

	device_printf(sc->sc_dev, "port %d: attached %s from %s\n",
	    port->index, port->busid, port->host);
	return (0);
}

/* Caller holds sc_sx.  Returns the socket to close, or NULL. */
static struct socket *
vhci_port_release(struct vhci_softc *sc, struct vhci_port *port)
{
	struct socket *so;

	sx_assert(&sc->sc_sx, SA_XLOCKED);

	/*
	 * Stop the transport before touching the port state, so that no
	 * thread can complete a transfer against a port we are tearing
	 * down.  This fails every transfer still outstanding.
	 */
	vhci_session_stop(port);

	so = port->so;
	port->so = NULL;
	port->dead = 0;
	port->devid = 0;
	port->usbip_speed = 0;
	port->busid[0] = '\0';
	port->host[0] = '\0';

	USB_BUS_LOCK(&sc->sc_bus);
	port->connected = 0;
	port->enabled = 0;
	port->suspended = 0;
	port->speed_bits = 0;
	port->change_connect = 1;
	vhci_root_intr(sc, port->index);
	USB_BUS_UNLOCK(&sc->sc_bus);

	return (so);
}

/*
 * Tear down a port whose transport threads gave up on the connection.
 * They cannot do this themselves, because releasing the port waits for
 * them to exit.
 */
static void
vhci_death_task(void *arg, int pending)
{
	struct vhci_port *port = arg;
	struct vhci_softc *sc = port->sc;
	struct socket *so;

	(void)pending;

	sx_xlock(&sc->sc_sx);
	/* An explicit detach may have got here first. */
	if (!port->connected || !port->dead) {
		sx_xunlock(&sc->sc_sx);
		return;
	}
	so = vhci_port_release(sc, port);
	sx_xunlock(&sc->sc_sx);

	if (so != NULL)
		soclose(so);

	device_printf(sc->sc_dev, "port %d: released\n", port->index);
}

static int
vhci_ioctl_detach(struct vhci_softc *sc, struct vhci_ioc_detach *id)
{
	struct vhci_port *port;
	struct socket *so;

	if (id->port < 0 || id->port >= VHCI_NPORTS)
		return (EINVAL);

	sx_xlock(&sc->sc_sx);
	port = &sc->sc_port[id->port];
	if (!port->connected) {
		sx_xunlock(&sc->sc_sx);
		return (ENXIO);
	}
	so = vhci_port_release(sc, port);
	sx_xunlock(&sc->sc_sx);

	/*
	 * Drain outside the lock: a death task racing us blocks on it,
	 * and will find the port already released once it runs.
	 */
	taskqueue_drain(taskqueue_thread, &port->death_task);

	if (so != NULL)
		soclose(so);

	device_printf(sc->sc_dev, "port %d: detached\n", id->port);
	return (0);
}

static int
vhci_ioctl_port_info(struct vhci_softc *sc, struct vhci_ioc_port_info *pi)
{
	struct vhci_port *port;

	if (pi->port < 0 || pi->port >= VHCI_NPORTS)
		return (EINVAL);

	sx_slock(&sc->sc_sx);
	port = &sc->sc_port[pi->port];
	pi->occupied = port->connected;
	pi->devid = port->devid;
	pi->speed = port->usbip_speed;
	strlcpy(pi->busid, port->busid, sizeof(pi->busid));
	strlcpy(pi->host, port->host, sizeof(pi->host));
	sx_sunlock(&sc->sc_sx);

	return (0);
}

static int
vhci_cdev_ioctl(struct cdev *dev, u_long cmd, caddr_t addr, int flags,
    struct thread *td)
{
	struct vhci_softc *sc = dev->si_drv1;

	(void)flags;
	if (sc == NULL)
		return (ENXIO);

	switch (cmd) {
	case VHCI_IOC_ATTACH:
		return (vhci_ioctl_attach(sc, (struct vhci_ioc_attach *)addr,
		    td));
	case VHCI_IOC_DETACH:
		return (vhci_ioctl_detach(sc, (struct vhci_ioc_detach *)addr));
	case VHCI_IOC_PORT_INFO:
		return (vhci_ioctl_port_info(sc,
		    (struct vhci_ioc_port_info *)addr));
	default:
		return (ENOTTY);
	}
}

static struct cdevsw vhci_cdevsw = {
	.d_version = D_VERSION,
	.d_ioctl = vhci_cdev_ioctl,
	.d_name = "vhci",
};

static void
vhci_identify(driver_t *driver, device_t parent)
{

	if (device_find_child(parent, driver->name, -1) != NULL)
		return;
	if (BUS_ADD_CHILD(parent, 0, driver->name, -1) == NULL)
		device_printf(parent, "vhci: add child failed\n");
}

static int
vhci_probe(device_t dev)
{

	device_set_desc(dev, "USB/IP virtual host controller");
	return (BUS_PROBE_NOWILDCARD);
}

static int
vhci_attach(device_t dev)
{
	struct vhci_softc *sc = device_get_softc(dev);
	struct make_dev_args args;
	devclass_t dc, pdc;
	int error, i;

	sc->sc_dev = dev;
	sx_init(&sc->sc_sx, "vhci");

	for (i = 0; i < VHCI_NPORTS; i++) {
		sc->sc_port[i].sc = sc;
		sc->sc_port[i].index = i;
		TASK_INIT(&sc->sc_port[i].death_task, 0, vhci_death_task,
		    &sc->sc_port[i]);
	}

	sc->sc_bus.parent = dev;
	sc->sc_bus.devices = sc->sc_devices;
	sc->sc_bus.devices_max = VHCI_MAX_DEVICES;
	sc->sc_bus.dma_bits = 32;
	sc->sc_bus.usbrev = USB_REV_2_0;
	sc->sc_bus.methods = &vhci_bus_methods;

	/* Initializes bus_mtx and friends; we have no DMA memory. */
	if (usb_bus_mem_alloc_all(&sc->sc_bus,
	    USB_GET_DMA_TAG(dev), NULL) != 0) {
		device_printf(dev, "could not allocate bus memory\n");
		error = ENOMEM;
		goto fail_sx;
	}

	/*
	 * The usbus driver is only registered against the devclasses of
	 * the in-tree controllers, so a child named "usbus" would find
	 * no driver under our own devclass.  Borrow ehci's, which also
	 * carries the usbus driver and matches our USB 2.0 root hub.
	 */
	dc = devclass_find("vhci");
	pdc = devclass_find("ehci");
	if (dc == NULL || pdc == NULL) {
		device_printf(dev, "usbus devclass not available\n");
		error = ENXIO;
		goto fail_busmem;
	}
	devclass_set_parent(dc, pdc);

	sc->sc_bus.bdev = device_add_child(dev, "usbus", -1);
	if (sc->sc_bus.bdev == NULL) {
		device_printf(dev, "could not add usbus\n");
		error = ENXIO;
		goto fail_busmem;
	}
	device_set_ivars(sc->sc_bus.bdev, &sc->sc_bus);

	make_dev_args_init(&args);
	args.mda_devsw = &vhci_cdevsw;
	args.mda_uid = UID_ROOT;
	args.mda_gid = GID_WHEEL;
	args.mda_mode = 0600;
	args.mda_si_drv1 = sc;
	error = make_dev_s(&args, &sc->sc_cdev, "vhci");
	if (error != 0) {
		device_printf(dev, "could not create /dev/vhci\n");
		goto fail_child;
	}

	error = device_probe_and_attach(sc->sc_bus.bdev);
	if (error != 0) {
		device_printf(dev, "usbus attach failed\n");
		goto fail_cdev;
	}

	return (0);

fail_cdev:
	destroy_dev(sc->sc_cdev);
	sc->sc_cdev = NULL;
fail_child:
	device_delete_children(dev);
fail_busmem:
	usb_bus_mem_free_all(&sc->sc_bus, NULL);
fail_sx:
	sx_destroy(&sc->sc_sx);
	return (error);
}

/*
 * True when every device below us has finished attaching.
 *
 * The USB stack brings the root hub up on its own explore thread, so
 * the bus is still coming up when device_probe_and_attach() returns.
 * If we tear down during that window, usb_bus_detach() frees the root
 * hub even though the hub driver refused to detach - it ignores the
 * return of bus_generic_detach() - and the hub driver is left pointing
 * at this module's memory.  Unloading then panics the machine.
 *
 * We cannot wait for the explore thread here: attach and detach both
 * run holding the newbus topology lock, which that thread needs in
 * order to make progress.  So report the state and let the caller
 * refuse instead.
 */
static bool
vhci_subtree_attached(device_t dev)
{
	device_t *children;
	int n, i;
	bool settled = true;

	if (device_get_children(dev, &children, &n) != 0)
		return (false);
	for (i = 0; i < n; i++) {
		if (!device_is_attached(children[i]) ||
		    !vhci_subtree_attached(children[i])) {
			settled = false;
			break;
		}
	}
	free(children, M_TEMP);
	return (settled);
}

/*
 * The root hub is the one child usbus always ends up with, so its
 * absence means enumeration has not reached it yet rather than that
 * there is nothing to wait for.  Treating an empty child list as
 * settled would miss exactly the window this guard exists for.
 */
static bool
vhci_bus_settled(struct vhci_softc *sc)
{
	device_t bdev = sc->sc_bus.bdev;
	device_t *children;
	int n;
	bool settled;

	if (bdev == NULL)
		return (true);
	if (!device_is_attached(bdev))
		return (false);
	if (device_get_children(bdev, &children, &n) != 0)
		return (false);
	free(children, M_TEMP);
	if (n == 0)
		return (false);

	settled = vhci_subtree_attached(bdev);
	return (settled);
}

static int
vhci_detach(device_t dev)
{
	struct vhci_softc *sc = device_get_softc(dev);
	struct socket *so[VHCI_NPORTS];
	int i;

	/*
	 * Refuse to unload while anything below us is still attaching;
	 * see vhci_subtree_attached().  Retrying a moment later works.
	 */
	if (!vhci_bus_settled(sc)) {
		device_printf(dev, "USB bus is still coming up, try again\n");
		return (EBUSY);
	}

	/*
	 * Release every port first so nothing is still using a socket
	 * once the USB stack goes away.
	 */
	sx_xlock(&sc->sc_sx);
	for (i = 0; i < VHCI_NPORTS; i++) {
		so[i] = NULL;
		if (sc->sc_port[i].connected)
			so[i] = vhci_port_release(sc, &sc->sc_port[i]);
	}
	sx_xunlock(&sc->sc_sx);

	/* No death task may still be holding a port once we free it. */
	for (i = 0; i < VHCI_NPORTS; i++)
		taskqueue_drain(taskqueue_thread, &sc->sc_port[i].death_task);

	for (i = 0; i < VHCI_NPORTS; i++) {
		if (so[i] != NULL)
			soclose(so[i]);
	}

	if (sc->sc_cdev != NULL) {
		destroy_dev(sc->sc_cdev);
		sc->sc_cdev = NULL;
	}

	device_delete_children(dev);
	usb_bus_mem_free_all(&sc->sc_bus, NULL);
	sx_destroy(&sc->sc_sx);

	return (0);
}

static device_method_t vhci_methods[] = {
	DEVMETHOD(device_identify, vhci_identify),
	DEVMETHOD(device_probe, vhci_probe),
	DEVMETHOD(device_attach, vhci_attach),
	DEVMETHOD(device_detach, vhci_detach),
	DEVMETHOD_END
};

static driver_t vhci_driver = {
	"vhci",
	vhci_methods,
	sizeof(struct vhci_softc),
};

DRIVER_MODULE(vhci, nexus, vhci_driver, 0, 0);
MODULE_DEPEND(vhci, usb, 1, 1, 1);
MODULE_VERSION(vhci, 1);
