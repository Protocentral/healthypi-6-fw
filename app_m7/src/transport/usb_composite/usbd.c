/*
 * Copyright (c) 2024-2026 Protocentral Electronics
 * SPDX-License-Identifier: MIT
 *
 * USB composite device (usb_device_next): CDC 0 stream + CDC 1 MCUmgr SMP.
 * Exposes a minimal sink/lifecycle API (see usbd.h). CDC 1 is owned by the
 * stock uart_mcumgr transport via the board's chosen node, so this file only
 * manages the device lifecycle and the CDC 0 stream pipe.
 */

#include "usbd.h"
#include "platform/fs_mount.h"

#include <zephyr/device.h>
#include <zephyr/usb/usbd.h>
#include <zephyr/usb/bos.h>
#include <zephyr/sys/ring_buffer.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>

LOG_MODULE_REGISTER(hpi_usbd, CONFIG_HPI_APP_LOG_LEVEL);

/* VID/PID + USB string descriptors are configurable: see
 * CONFIG_HPI_USB_{VID,PID,MANUFACTURER,PRODUCT} in app_m7/Kconfig. The default
 * VID 0x2FE3 is the Zephyr development pair -- NOT shippable; replace with the
 * registered pid.codes PID (VID 0x1209) before customer shipment.
 * tools/ci/check_prod_surface.sh fails a prod build still on the 0x2FE3 VID. */

/* CDC 0 -- the sample-stream pipe (this module). CDC 1 is bound to the
 * uart_mcumgr transport in DT and is not referenced here. We address CDC 0 by
 * label because DEVICE_DT_GET_ONE would be ambiguous with two CDC nodes. */
static const struct device *const usb_cdc_dev =
	DEVICE_DT_GET(DT_NODELABEL(cdc_acm_uart0));

/* 16 KiB buys ~700 ms of producer headroom at full stream rate, absorbing host
 * read-gap bursts without dropping frames. */
#define RING_BUF_SIZE 16384
static uint8_t ring_buffer[RING_BUF_SIZE];
static struct ring_buf ringbuf_usb_cdc;
static bool rx_throttled;

/* Set/cleared on CDC ACM DTR changes; the producer gates on it. */
static volatile bool g_usb_host_connected;

static struct usbd_context *hpi_usbd_context;

#if defined(CONFIG_USBD_MSC_CLASS)
/* MSC (Transfer Mode). The next-stack MSC class instance is "msc_0"; cfg id 1
 * matches usbd_add_configuration above. Verify the class name in the boot log if
 * arm/disarm returns -ENOENT on this Zephyr revision. */
#define HPI_MSC_CLASS_NAME "msc_0"
#define HPI_USB_CFG_ID     1
/* DIAGNOSTIC: set to 1 to leave MSC enumerated from boot (no dynamic arm) and
 * unmount the FS so the host always sees the SD. Use this to confirm the MSC
 * class itself works; if the host mounts the drive with this =1 but not via the
 * Transfer button, the bug is in the dynamic re-enumeration, not MSC. */
#define HPI_MSC_ALWAYS_ON  0
static volatile bool g_msc_armed;
static bool          g_msc_disarm_ok;
#endif

/* Do not register the USB DFU class DFU-mode instance. */
static const char *const blocklist[] = { "dfu_dfu", NULL };

USBD_DEVICE_DEFINE(hpi_usbd_dev,
		   DEVICE_DT_GET(DT_NODELABEL(zephyr_udc0)),
		   CONFIG_HPI_USB_VID, CONFIG_HPI_USB_PID);

USBD_DESC_LANG_DEFINE(hpi_lang);
USBD_DESC_MANUFACTURER_DEFINE(hpi_mfr, CONFIG_HPI_USB_MANUFACTURER);
USBD_DESC_PRODUCT_DEFINE(hpi_product, CONFIG_HPI_USB_PRODUCT);
/* Serial number from the STM32 UID via HWINFO (matches the Zephyr reference
 * flow; hosts expect a SN string and some enumerate more reliably with one). */
IF_ENABLED(CONFIG_HWINFO, (USBD_DESC_SERIAL_NUMBER_DEFINE(hpi_sn)));

USBD_DESC_CONFIG_DEFINE(fs_cfg_desc, "FS Configuration");
USBD_DESC_CONFIG_DEFINE(hs_cfg_desc, "HS Configuration");

static const uint8_t attributes =
	(IS_ENABLED(CONFIG_SAMPLE_USBD_SELF_POWERED) ? USB_SCD_SELF_POWERED : 0) |
	(IS_ENABLED(CONFIG_SAMPLE_USBD_REMOTE_WAKEUP) ? USB_SCD_REMOTE_WAKEUP : 0);

/* bMaxPower in 2 mA units. The HS config is only used when the controller
 * actually negotiates High-Speed; on this board the OTG_HS runs the internal
 * FS PHY (caps == FS), so the FS config is the one that enumerates. */
USBD_CONFIGURATION_DEFINE(hpi_fs_config, attributes, 125, &fs_cfg_desc);
USBD_CONFIGURATION_DEFINE(hpi_hs_config, attributes, 125, &hs_cfg_desc);

static const struct usb_bos_capability_lpm bos_cap_lpm = {
	.bLength = sizeof(struct usb_bos_capability_lpm),
	.bDescriptorType = USB_DESC_DEVICE_CAPABILITY,
	.bDevCapabilityType = USB_BOS_CAPABILITY_EXTENSION,
	.bmAttributes = 0UL,
};
USBD_DESC_BOS_DEFINE(hpi_usbext, sizeof(bos_cap_lpm), &bos_cap_lpm);

bool hpi_usb_host_connected(void)
{
	return g_usb_host_connected;
}

bool hpi_usb_stream_write(const uint8_t *buf, size_t len)
{
	/* No host listening -> drop at the producer so the ring stays empty
	 * until a client connects. */
	if (!g_usb_host_connected) {
		return false;
	}
	/* All-or-nothing: a partial ring_buf_put would corrupt framed bytes on
	 * the wire. */
	if (ring_buf_space_get(&ringbuf_usb_cdc) < len) {
		return false;
	}
	(void)ring_buf_put(&ringbuf_usb_cdc, buf, len);
	uart_irq_tx_enable(usb_cdc_dev);
	return true;
}

static void fix_code_triple(struct usbd_context *uds_ctx,
			    const enum usbd_speed speed)
{
	if (IS_ENABLED(CONFIG_USBD_CDC_ACM_CLASS)) {
		/* Multi-interface class -> IAD triple. */
		usbd_device_set_code_triple(uds_ctx, speed,
					    USB_BCC_MISCELLANEOUS, 0x02, 0x01);
	} else {
		usbd_device_set_code_triple(uds_ctx, speed, 0, 0, 0);
	}
}

/* Tolerate -EALREADY: usbd_setup_device is re-run on each MSC transition (after
 * usbd_shutdown), and some definitions persist across shutdown. */
static inline bool setup_ok(int err)
{
	return err == 0 || err == -EALREADY;
}

/* Build the device definition: descriptors, the FS configuration, and ALL
 * classes (both CDC ACMs + the MSC LUN). MSC is removed afterwards by
 * apply_msc_state() unless armed. Safe to call again after usbd_shutdown(). */
static struct usbd_context *usbd_setup_device(usbd_msg_cb_t msg_cb)
{
	int err;

	err = usbd_add_descriptor(&hpi_usbd_dev, &hpi_lang);
	if (!setup_ok(err)) { LOG_ERR("language descriptor (%d)", err); return NULL; }
	err = usbd_add_descriptor(&hpi_usbd_dev, &hpi_mfr);
	if (!setup_ok(err)) { LOG_ERR("manufacturer descriptor (%d)", err); return NULL; }
	err = usbd_add_descriptor(&hpi_usbd_dev, &hpi_product);
	if (!setup_ok(err)) { LOG_ERR("product descriptor (%d)", err); return NULL; }

#if defined(CONFIG_HWINFO)
	err = usbd_add_descriptor(&hpi_usbd_dev, &hpi_sn);
	if (!setup_ok(err)) { LOG_ERR("serial-number descriptor (%d)", err); return NULL; }
#endif

	/* High-Speed configuration FIRST, but only when the controller actually
	 * runs HS. On this board the OTG_HS uses the internal FS PHY, so caps ==
	 * FS and this branch is skipped; it's kept for parity with the Zephyr
	 * reference flow (sample_usbd_init.c) and forward-compat with an external
	 * HS PHY. */
	if (USBD_SUPPORTS_HIGH_SPEED &&
	    usbd_caps_speed(&hpi_usbd_dev) == USBD_SPEED_HS) {
		err = usbd_add_configuration(&hpi_usbd_dev, USBD_SPEED_HS, &hpi_hs_config);
		if (!setup_ok(err)) { LOG_ERR("add HS configuration (%d)", err); return NULL; }

		err = usbd_register_all_classes(&hpi_usbd_dev, USBD_SPEED_HS, 1, blocklist);
		if (!setup_ok(err)) { LOG_ERR("register HS classes (%d)", err); return NULL; }

		fix_code_triple(&hpi_usbd_dev, USBD_SPEED_HS);
	}

	err = usbd_add_configuration(&hpi_usbd_dev, USBD_SPEED_FS, &hpi_fs_config);
	if (!setup_ok(err)) { LOG_ERR("add FS configuration (%d)", err); return NULL; }

	err = usbd_register_all_classes(&hpi_usbd_dev, USBD_SPEED_FS, 1, blocklist);
	if (!setup_ok(err)) { LOG_ERR("register classes (%d)", err); return NULL; }

	fix_code_triple(&hpi_usbd_dev, USBD_SPEED_FS);
	usbd_self_powered(&hpi_usbd_dev, attributes & USB_SCD_SELF_POWERED);

	if (msg_cb != NULL) {
		err = usbd_msg_register_cb(&hpi_usbd_dev, msg_cb);
		if (!setup_ok(err)) { LOG_ERR("register msg cb (%d)", err); return NULL; }
	}

	return &hpi_usbd_dev;
}

#if defined(CONFIG_USBD_MSC_CLASS)
/* Apply the desired MSC state to a freshly-set-up (pre-init) device. When NOT
 * armed, the MSC LUN registered by register_all_classes is removed so the device
 * enumerates as 2x CDC only. Must be called while the stack is uninitialized
 * ((un)register_class returns -EBUSY once usbd_init() has run). */
static int apply_msc_state(bool armed)
{
	if (armed) {
		return 0;   /* MSC already registered by register_all_classes */
	}
	int err = usbd_unregister_class(&hpi_usbd_dev, HPI_MSC_CLASS_NAME,
					USBD_SPEED_FS, HPI_USB_CFG_ID);
	if (err) {
		LOG_ERR("MSC unregister failed (%d=%s); expected class \"%s\"",
			err, err == -ENOENT ? "ENOENT/bad-name" : "err",
			HPI_MSC_CLASS_NAME);
	}
	return err;
}
#endif

/* "A USB host is attached", inferred from enumeration -- this board has no
 * VBUS detect (the STM32 UDC reports no VBUS capability and there is no sense
 * GPIO), so USBD_MSG_VBUS_READY/REMOVED never arrive. A bus reset or
 * SET_CONFIGURATION means a host is talking; SUSPEND means it stopped, which
 * is also what an unplugged cable looks like. A dumb charger enumerates
 * nothing and reads "not attached" here -- for input-present, use the BQ24074
 * PGOOD pin via the power service instead. */
static atomic_t g_usb_attached = ATOMIC_INIT(0);

bool hpi_usb_attached(void)
{
	return atomic_get(&g_usb_attached) != 0;
}

static void usbd_msg_cb(struct usbd_context *const ctx, const struct usbd_msg *msg)
{
	/* RESET fires on every bus reset and spams the log when a host probes;
	 * keep it at DBG, everything else at INF for enumeration visibility. */
	if (msg->type == USBD_MSG_RESET) {
		LOG_DBG("USBD: %s", usbd_msg_type_string(msg->type));
	} else {
		LOG_INF("USBD: %s", usbd_msg_type_string(msg->type));
	}

	switch (msg->type) {
	case USBD_MSG_RESET:
	case USBD_MSG_RESUME:
	case USBD_MSG_CONFIGURATION:
	case USBD_MSG_VBUS_READY:
		atomic_set(&g_usb_attached, 1);
		break;
	case USBD_MSG_SUSPEND:
	case USBD_MSG_VBUS_REMOVED:
		atomic_set(&g_usb_attached, 0);
		break;
	default:
		break;
	}

	if (usbd_can_detect_vbus(ctx)) {
		if (msg->type == USBD_MSG_VBUS_READY) {
			if (usbd_enable(ctx)) {
				LOG_ERR("usbd_enable failed");
			}
		}
		if (msg->type == USBD_MSG_VBUS_REMOVED) {
			if (usbd_disable(ctx)) {
				LOG_ERR("usbd_disable failed");
			}
		}
	}

	if (msg->type == USBD_MSG_CDC_ACM_CONTROL_LINE_STATE) {
		uint32_t dtr = 0U;

		uart_line_ctrl_get(msg->dev, UART_LINE_CTRL_DTR, &dtr);
		/* Only CDC 0 (the stream pipe) is tracked here; CDC 1's DTR is
		 * the mcumgr transport's concern. */
		if (msg->dev == usb_cdc_dev) {
			if (dtr) {
				g_usb_host_connected = true;
				ring_buf_reset(&ringbuf_usb_cdc);
				/* Re-arm RX defensively on the DTR-up edge. */
				uart_irq_rx_enable(msg->dev);
			} else {
				g_usb_host_connected = false;
			}
		}
	}
}

static void cdc0_interrupt_handler(const struct device *dev, void *user_data)
{
	ARG_UNUSED(user_data);

	while (uart_irq_update(dev) && uart_irq_is_pending(dev)) {
		/* CDC 0 is stream-TX-only; drain & discard any host RX so the
		 * controller does not stall, but do nothing with it. */
		if (!rx_throttled && uart_irq_rx_ready(dev)) {
			uint8_t scratch[64];
			int n = uart_fifo_read(dev, scratch, sizeof(scratch));
			ARG_UNUSED(n);
		}

		if (uart_irq_tx_ready(dev)) {
			uint8_t *claim_ptr;
			uint32_t claim_size;
			int send_len;

			/* Zero-copy claim: consume only what the USB FIFO takes,
			 * so we never drop bytes mid-frame. */
			claim_size = ring_buf_get_claim(&ringbuf_usb_cdc,
							&claim_ptr, 64);
			if (claim_size == 0) {
				uart_irq_tx_disable(dev);
				continue;
			}
			send_len = uart_fifo_fill(dev, claim_ptr, claim_size);
			if (send_len < 0) {
				send_len = 0;
			}
			ring_buf_get_finish(&ringbuf_usb_cdc, (uint32_t)send_len);
		}
	}
}

static int usbd_enable_device(void)
{
	int err;

	hpi_usbd_context = usbd_setup_device(usbd_msg_cb);
	if (hpi_usbd_context == NULL) {
		return -ENODEV;
	}

#if defined(CONFIG_USBD_MSC_CLASS)
	/* Boot MSC state: armed only in the diagnostic build, else disarmed (2x CDC).
	 * Done pre-init while (un)register_class is permitted. g_msc_disarm_ok feeds
	 * the FS safety-unmount in hpi_usb_start(). */
	{
		bool boot_armed = (HPI_MSC_ALWAYS_ON != 0);
		int merr = apply_msc_state(boot_armed);
		g_msc_armed = boot_armed;
		g_msc_disarm_ok = (!boot_armed && merr == 0);
		if (boot_armed) {
			LOG_WRN("MSC ALWAYS-ON diagnostic: enumerated at boot, FS unmounted");
		} else if (merr == 0) {
			LOG_INF("MSC available (disarmed); arm via Transfer Mode");
		}
	}
#endif

	err = usbd_init(hpi_usbd_context);
	if (err) {
		LOG_ERR("usbd_init failed (%d)", err);
		return err;
	}

	if (!usbd_can_detect_vbus(hpi_usbd_context)) {
		err = usbd_enable(hpi_usbd_context);
		if (err) {
			LOG_ERR("usbd_enable failed (%d)", err);
			return err;
		}
	}

	LOG_INF("USB composite up: CDC0=stream, CDC1=mcumgr (FS)");
	return 0;
}

void hpi_usb_start(void)
{
	if (!device_is_ready(usb_cdc_dev)) {
		LOG_ERR("CDC0 device not ready");
		return;
	}

	/* Wire CDC 0 callback + ring BEFORE enabling the device, so the class
	 * -enable work items find the callback set (avoids benign driver noise). */
	ring_buf_init(&ringbuf_usb_cdc, RING_BUF_SIZE, ring_buffer);
	uart_irq_callback_set(usb_cdc_dev, cdc0_interrupt_handler);

	if (usbd_enable_device() != 0) {
		LOG_ERR("USB device enable failed");
	}

#if defined(CONFIG_USBD_MSC_CLASS)
	/* Safety: if MSC could not be disarmed at setup it is enumerated now, so
	 * the host can access the SD block device -- the device FS must NOT be
	 * mounted at the same time. Force it unmounted. */
	if (!g_msc_disarm_ok) {
		(void)platform_fs_unmount();
	}
#endif
}

#if defined(CONFIG_USBD_MSC_CLASS)
bool hpi_usb_msc_armed(void)
{
	return g_msc_armed;
}

int hpi_usb_msc_set(bool armed)
{
	if (hpi_usbd_context == NULL) {
		return -ENODEV;
	}
	if (armed == g_msc_armed) {
		return 0;
	}

	/* (un)register_class is only allowed while the stack is NOT initialized
	 * (returns -EBUSY otherwise; usbd_disable alone is insufficient). And
	 * usbd_shutdown() drops every registered class, so we rebuild the whole
	 * definition (all classes via usbd_setup_device) and then remove MSC unless
	 * arming. Sequence: disable -> shutdown -> setup(all classes) ->
	 * apply_msc_state -> init -> enable. */
	(void)usbd_disable(hpi_usbd_context);

	int err = usbd_shutdown(hpi_usbd_context);
	if (err) {
		LOG_ERR("usbd_shutdown (MSC) failed (%d)", err);
		return err;
	}

	if (usbd_setup_device(usbd_msg_cb) == NULL) {
		LOG_ERR("USB re-setup failed during MSC change");
		return -EIO;
	}

	err = apply_msc_state(armed);
	if (err == 0) {
		g_msc_armed = armed;
	}

	int ei = usbd_init(hpi_usbd_context);
	if (ei) {
		LOG_ERR("usbd_init after MSC change failed (%d)", ei);
		return ei;
	}
	int ee = usbd_enable(hpi_usbd_context);
	if (ee) {
		LOG_ERR("usbd_enable after MSC change failed (%d)", ee);
		return ee;
	}
	LOG_INF("USB re-enumerated: MSC %s", g_msc_armed ? "ARMED" : "disarmed");
	return err;
}
#endif
