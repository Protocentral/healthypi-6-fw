/*
 * Copyright (c) 2024-2026 Protocentral Electronics
 * SPDX-License-Identifier: MIT
 *
 * USB composite device (L-transport) -- the headless host surface.
 *
 * One USBD device (usb_device_next stack) exposing two CDC ACM functions:
 *   CDC 0 (cdc_acm_uart0) -- one-way .HP6 sample stream (device -> host).
 *   CDC 1 (cdc_acm_uart1) -- MCUmgr SMP control (bidirectional), owned by the
 *                            stock Zephyr uart_mcumgr transport (bound via the
 *                            `zephyr,uart-mcumgr` chosen node in the board DTS).
 * Runs at Full Speed on OTG_HS (internal FS PHY). VID/PID are the Zephyr
 * development pair until a registered pair is provisioned.
 *
 * This module owns only CDC 0 (the stream pipe). The stream service
 * plugs `hpi_usb_stream_write` in as its sink; group-64 control lives in
 * control/mcumgr_hpi.
 */

#ifndef HPI_TRANSPORT_USBD_H
#define HPI_TRANSPORT_USBD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Initialise + enable the composite USB device. Call once at boot after the
 * bus/FS are up. Safe if no host is attached (enumerates when VBUS appears). */
void hpi_usb_start(void);

/* True once a host has opened CDC 0 (DTR asserted). The stream producer should
 * gate on this so the ring buffer stays empty until a client is listening. */
bool hpi_usb_host_connected(void);

/* True while a USB host is attached, from enumeration state (bus reset /
 * SET_CONFIGURATION up, SUSPEND down). Distinct from hpi_usb_host_connected(),
 * which needs host software to raise DTR on CDC 0. Neither sees a dumb
 * charger: this SoC has no VBUS detect and there is no VBUS sense line. For
 * "is input power present", prefer the BQ24074 PGOOD pin the power service
 * reads -- that one does see a charger. */
bool hpi_usb_attached(void);

/* Enqueue bytes onto the CDC 0 stream pipe (TX, device -> host). All-or-nothing:
 * returns false (drops the whole frame) if no host is connected or the ring has
 * insufficient space -- never writes a partial frame onto the wire. */
bool hpi_usb_stream_write(const uint8_t *buf, size_t len);

#if defined(CONFIG_USBD_MSC_CLASS)
/* USB MSC Transfer Mode. Arming re-enumerates the device WITH a mass-storage
 * function exposing the SD card; disarming removes it. The caller (transport/
 * msc) is responsible for unmounting the FS before arming and remounting after
 * disarming -- the device must never share the FAT volume with the host. */
int  hpi_usb_msc_set(bool armed);
bool hpi_usb_msc_armed(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* HPI_TRANSPORT_USBD_H */
