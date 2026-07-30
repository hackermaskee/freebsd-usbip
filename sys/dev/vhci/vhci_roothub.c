/* SPDX-License-Identifier: BSD-2-Clause */
/*-
 * Copyright (c) 2026 furuta@furuta.bsdclub.org
 *
 * vhci(4): software root hub emulation.
 *
 * The USB stack talks to a root hub with control requests routed
 * through usb_bus_methods.roothub_exec instead of real transfers.  We
 * answer them from the per-port state in the softc, which the USB/IP
 * session code updates when a remote device is imported or released.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/condvar.h>

#include <dev/usb/usb.h>
#include <dev/usb/usbdi.h>
#include <dev/usb/usb_core.h>
#include <dev/usb/usb_busdma.h>
#include <dev/usb/usb_process.h>
#include <dev/usb/usb_util.h>
#include <dev/usb/usb_device.h>
#include <dev/usb/usb_hub.h>
#include <dev/usb/usb_controller.h>
#include <dev/usb/usb_bus.h>

#include "vhci.h"

/*
 * A USB 2.0 high-speed hub.  The stack derives the root hub's own
 * speed from usb_bus.usbrev (USB_REV_2_0), so bcdUSB and the hub
 * protocol must agree with that.
 */
static const struct usb_device_descriptor vhci_devd = {
	.bLength = sizeof(struct usb_device_descriptor),
	.bDescriptorType = UDESC_DEVICE,
	.bcdUSB = {0x00, 0x02},
	.bDeviceClass = UDCLASS_HUB,
	.bDeviceSubClass = UDSUBCLASS_HUB,
	.bDeviceProtocol = UDPROTO_HSHUBSTT,
	.bMaxPacketSize = 64,
	.bcdDevice = {0x00, 0x01},
	.iManufacturer = 1,
	.iProduct = 2,
	.bNumConfigurations = 1,
};

/*
 * Required by the specification even though we cannot actually operate
 * at another speed.
 */
static const struct usb_device_qualifier vhci_odevd = {
	.bLength = sizeof(struct usb_device_qualifier),
	.bDescriptorType = UDESC_DEVICE_QUALIFIER,
	.bcdUSB = {0x00, 0x02},
	.bDeviceClass = UDCLASS_HUB,
	.bDeviceSubClass = UDSUBCLASS_HUB,
	.bDeviceProtocol = UDPROTO_FSHUB,
	.bMaxPacketSize0 = 0,
	.bNumConfigurations = 0,
};

struct vhci_config_desc {
	struct usb_config_descriptor confd;
	struct usb_interface_descriptor ifcd;
	struct usb_endpoint_descriptor endpd;
} __packed;

/*
 * The interrupt endpoint is decorative: uhub_attach() skips transfer
 * setup for a root hub and we signal port changes with
 * uhub_root_intr() instead.  It is here so the hub driver sees a
 * well-formed hub.
 */
static const struct vhci_config_desc vhci_confd = {
	.confd = {
		.bLength = sizeof(struct usb_config_descriptor),
		.bDescriptorType = UDESC_CONFIG,
		.wTotalLength[0] = sizeof(vhci_confd),
		.bNumInterface = 1,
		.bConfigurationValue = 1,
		.iConfiguration = 0,
		.bmAttributes = UC_SELF_POWERED,
		.bMaxPower = 0,
	},
	.ifcd = {
		.bLength = sizeof(struct usb_interface_descriptor),
		.bDescriptorType = UDESC_INTERFACE,
		.bNumEndpoints = 1,
		.bInterfaceClass = UICLASS_HUB,
		.bInterfaceSubClass = UISUBCLASS_HUB,
		.bInterfaceProtocol = 0,
	},
	.endpd = {
		.bLength = sizeof(struct usb_endpoint_descriptor),
		.bDescriptorType = UDESC_ENDPOINT,
		.bEndpointAddress = UE_DIR_IN | VHCI_INTR_ENDPT,
		.bmAttributes = UE_INTERRUPT,
		.wMaxPacketSize[0] = 8,
		.bInterval = 255,
	},
};

/*
 * bDescLength and bNbrPorts are filled in at request time.  Ports are
 * never power-switched and are all reported removable.
 */
static const struct usb_hub_descriptor vhci_hubd = {
	.bDescriptorType = UDESC_HUB,
	.bPwrOn2PwrGood = 0,
	.bHubContrCurrent = 0,
};

#define	STRING_VENDOR	"F\0r\0e\0e\0B\0S\0D"
#define	STRING_PRODUCT	"U\0S\0B\0/\0I\0P\0 \0r\0o\0o\0t\0 \0H\0U\0B"

USB_MAKE_STRING_DESC(STRING_VENDOR, vhci_vendor);
USB_MAKE_STRING_DESC(STRING_PRODUCT, vhci_product);

/*
 * Tell the USB stack that a port's state changed, so that it explores
 * the hub and picks up the connect or disconnect.  Callable from any
 * sleepable context; the bus lock must NOT already be held by a
 * different lock order.
 */
void
vhci_root_intr(struct vhci_softc *sc, uint8_t port_index)
{
	uint8_t bit;

	USB_BUS_LOCK_ASSERT(&sc->sc_bus, MA_OWNED);

	/* Bit 0 is the hub itself, bit N is port N. */
	bit = port_index + 1;
	sc->sc_hub_idata[bit / 8] |= 1 << (bit % 8);

	uhub_root_intr(&sc->sc_bus, sc->sc_hub_idata,
	    sizeof(sc->sc_hub_idata));
}

static void
vhci_get_port_status(struct vhci_softc *sc, struct vhci_port *port)
{
	uint16_t status, change;

	status = 0;
	if (port->connected)
		status |= UPS_CURRENT_CONNECT_STATUS;
	if (port->enabled)
		status |= UPS_PORT_ENABLED;
	if (port->powered)
		status |= UPS_PORT_POWER;
	if (port->suspended)
		status |= UPS_SUSPEND;
	/*
	 * Neither speed bit means full speed.  The value is fixed when
	 * the device is imported, because the stack reads it right
	 * after port reset completes.
	 */
	status |= port->speed_bits;

	change = 0;
	if (port->change_connect)
		change |= UPS_C_CONNECT_STATUS;
	if (port->change_enable)
		change |= UPS_C_PORT_ENABLED;
	if (port->change_suspend)
		change |= UPS_C_SUSPEND;
	if (port->change_reset)
		change |= UPS_C_PORT_RESET;

	USETW(sc->sc_hub_temp.ps.wPortStatus, status);
	USETW(sc->sc_hub_temp.ps.wPortChange, change);
}

/*
 * There is no reset primitive in the USB/IP protocol, so a port reset
 * only re-establishes local state.  We complete it synchronously: the
 * stack polls GET_STATUS and stops as soon as it sees C_PORT_RESET.
 */
static void
vhci_port_reset(struct vhci_softc *sc, struct vhci_port *port)
{

	(void)sc;
	if (!port->connected)
		return;
	port->enabled = 1;
	port->change_reset = 1;
}

usb_error_t
vhci_roothub_exec(struct usb_device *udev, struct usb_device_request *req,
    const void **pptr, uint16_t *plength)
{
	struct vhci_softc *sc = VHCI_BUS2SC(udev->bus);
	struct vhci_port *port;
	const void *ptr;
	const char *str_ptr;
	uint16_t len, value, index;
	usb_error_t err;

	USB_BUS_LOCK_ASSERT(&sc->sc_bus, MA_OWNED);

	ptr = (const void *)&sc->sc_hub_temp;
	len = 0;
	err = 0;

	value = UGETW(req->wValue);
	index = UGETW(req->wIndex);

#define	C(x, y)	((x) | ((y) << 8))
	switch (C(req->bRequest, req->bmRequestType)) {
	case C(UR_CLEAR_FEATURE, UT_WRITE_DEVICE):
	case C(UR_CLEAR_FEATURE, UT_WRITE_INTERFACE):
	case C(UR_CLEAR_FEATURE, UT_WRITE_ENDPOINT):
		/* DEVICE_REMOTE_WAKEUP and ENDPOINT_HALT are no-ops. */
		break;

	case C(UR_GET_CONFIG, UT_READ_DEVICE):
		len = 1;
		sc->sc_hub_temp.temp[0] = sc->sc_conf;
		break;

	case C(UR_GET_DESCRIPTOR, UT_READ_DEVICE):
		switch (value >> 8) {
		case UDESC_DEVICE:
			if ((value & 0xff) != 0) {
				err = USB_ERR_IOERROR;
				goto done;
			}
			len = sizeof(vhci_devd);
			ptr = (const void *)&vhci_devd;
			break;
		case UDESC_DEVICE_QUALIFIER:
			if ((value & 0xff) != 0) {
				err = USB_ERR_IOERROR;
				goto done;
			}
			len = sizeof(vhci_odevd);
			ptr = (const void *)&vhci_odevd;
			break;
		case UDESC_CONFIG:
			if ((value & 0xff) != 0) {
				err = USB_ERR_IOERROR;
				goto done;
			}
			/*
			 * usbd_req_get_descriptor_ptr() uses this
			 * pointer directly, so it must be static.
			 */
			len = sizeof(vhci_confd);
			ptr = (const void *)&vhci_confd;
			break;
		case UDESC_STRING:
			switch (value & 0xff) {
			case 0:		/* language table */
				len = sizeof(usb_string_lang_en);
				ptr = (const void *)&usb_string_lang_en;
				break;
			case 1:		/* vendor */
				len = sizeof(vhci_vendor);
				ptr = (const void *)&vhci_vendor;
				break;
			case 2:		/* product */
				len = sizeof(vhci_product);
				ptr = (const void *)&vhci_product;
				break;
			default:
				str_ptr = "";
				len = usb_make_str_desc(sc->sc_hub_temp.temp,
				    sizeof(sc->sc_hub_temp.temp), str_ptr);
				break;
			}
			break;
		default:
			err = USB_ERR_IOERROR;
			goto done;
		}
		break;

	case C(UR_GET_INTERFACE, UT_READ_INTERFACE):
		len = 1;
		sc->sc_hub_temp.temp[0] = 0;
		break;

	case C(UR_GET_STATUS, UT_READ_DEVICE):
		/*
		 * Must report self-powered, or the hub driver refuses
		 * to attach a bus-powered hub below a bus-powered one.
		 */
		len = 2;
		USETW(sc->sc_hub_temp.stat.wStatus, UDS_SELF_POWERED);
		break;

	case C(UR_GET_STATUS, UT_READ_INTERFACE):
	case C(UR_GET_STATUS, UT_READ_ENDPOINT):
		len = 2;
		USETW(sc->sc_hub_temp.stat.wStatus, 0);
		break;

	case C(UR_SET_ADDRESS, UT_WRITE_DEVICE):
		if (value >= VHCI_MAX_DEVICES) {
			err = USB_ERR_IOERROR;
			goto done;
		}
		sc->sc_rt_addr = value;
		break;

	case C(UR_SET_CONFIG, UT_WRITE_DEVICE):
		if (value != 0 && value != 1) {
			err = USB_ERR_IOERROR;
			goto done;
		}
		sc->sc_conf = value;
		break;

	case C(UR_SET_DESCRIPTOR, UT_WRITE_DEVICE):
	case C(UR_SET_INTERFACE, UT_WRITE_INTERFACE):
	case C(UR_SYNCH_FRAME, UT_WRITE_ENDPOINT):
		break;

	case C(UR_SET_FEATURE, UT_WRITE_DEVICE):
	case C(UR_SET_FEATURE, UT_WRITE_INTERFACE):
	case C(UR_SET_FEATURE, UT_WRITE_ENDPOINT):
		err = USB_ERR_IOERROR;
		goto done;

	/* Hub class requests. */

	case C(UR_CLEAR_FEATURE, UT_WRITE_CLASS_DEVICE):
	case C(UR_SET_FEATURE, UT_WRITE_CLASS_DEVICE):
		/* C_HUB_LOCAL_POWER and C_HUB_OVER_CURRENT are no-ops. */
		break;

	case C(UR_GET_DESCRIPTOR, UT_READ_CLASS_DEVICE):
		if ((value & 0xff) != 0) {
			err = USB_ERR_IOERROR;
			goto done;
		}
		sc->sc_hub_temp.hubd = vhci_hubd;
		sc->sc_hub_temp.hubd.bNbrPorts = VHCI_NPORTS;
		USETW(sc->sc_hub_temp.hubd.wHubCharacteristics,
		    UHD_PWR_NO_SWITCH | UHD_OC_INDIVIDUAL);
		/* All ports removable: DeviceRemovable stays zero. */
		memset(sc->sc_hub_temp.hubd.DeviceRemovable, 0,
		    sizeof(sc->sc_hub_temp.hubd.DeviceRemovable));
		sc->sc_hub_temp.hubd.bDescLength =
		    8 + ((VHCI_NPORTS + 7) / 8);
		len = sc->sc_hub_temp.hubd.bDescLength;
		break;

	case C(UR_GET_STATUS, UT_READ_CLASS_DEVICE):
		len = 16;
		memset(sc->sc_hub_temp.temp, 0, len);
		break;

	case C(UR_GET_STATUS, UT_READ_CLASS_OTHER):
		if (index < 1 || index > VHCI_NPORTS) {
			err = USB_ERR_IOERROR;
			goto done;
		}
		vhci_get_port_status(sc, &sc->sc_port[index - 1]);
		len = sizeof(sc->sc_hub_temp.ps);
		break;

	case C(UR_SET_FEATURE, UT_WRITE_CLASS_OTHER):
		if (index < 1 || index > VHCI_NPORTS) {
			err = USB_ERR_IOERROR;
			goto done;
		}
		port = &sc->sc_port[index - 1];
		switch (value) {
		case UHF_PORT_ENABLE:
			port->enabled = 1;
			break;
		case UHF_PORT_SUSPEND:
			port->suspended = 1;
			break;
		case UHF_PORT_RESET:
			vhci_port_reset(sc, port);
			break;
		case UHF_PORT_POWER:
			port->powered = 1;
			break;
		case UHF_PORT_TEST:
		case UHF_PORT_INDICATOR:
			break;
		default:
			err = USB_ERR_IOERROR;
			goto done;
		}
		break;

	case C(UR_CLEAR_FEATURE, UT_WRITE_CLASS_OTHER):
		if (index < 1 || index > VHCI_NPORTS) {
			err = USB_ERR_IOERROR;
			goto done;
		}
		port = &sc->sc_port[index - 1];
		switch (value) {
		case UHF_PORT_ENABLE:
			port->enabled = 0;
			break;
		case UHF_PORT_SUSPEND:
			port->suspended = 0;
			break;
		case UHF_PORT_POWER:
			port->powered = 0;
			break;
		case UHF_C_PORT_CONNECTION:
			port->change_connect = 0;
			break;
		case UHF_C_PORT_ENABLE:
			port->change_enable = 0;
			break;
		case UHF_C_PORT_SUSPEND:
			port->change_suspend = 0;
			break;
		case UHF_C_PORT_RESET:
			port->change_reset = 0;
			break;
		case UHF_C_PORT_OVER_CURRENT:
		case UHF_PORT_TEST:
		case UHF_PORT_INDICATOR:
			break;
		default:
			err = USB_ERR_IOERROR;
			goto done;
		}
		break;

	case C(UR_CLEAR_TT_BUFFER, UT_WRITE_CLASS_OTHER):
	case C(UR_RESET_TT, UT_WRITE_CLASS_OTHER):
	case C(UR_STOP_TT, UT_WRITE_CLASS_OTHER):
		break;

	case C(UR_GET_TT_STATE, UT_READ_CLASS_OTHER):
		len = 2;
		USETW(sc->sc_hub_temp.wValue, 0);
		break;

	case C(UR_SET_DESCRIPTOR, UT_WRITE_CLASS_DEVICE):
	default:
		err = USB_ERR_IOERROR;
		goto done;
	}
#undef C

done:
	*plength = len;
	*pptr = ptr;
	return (err);
}
