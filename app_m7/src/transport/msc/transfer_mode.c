/*
 * Copyright (c) 2024-2026 Protocentral Electronics
 * SPDX-License-Identifier: MIT
 *
 * USB MSC Transfer Mode. Defines the mass-storage LUN over the SD
 * disk and orchestrates arm/disarm: FS unmount <-> re-enumerate with MSC.
 */

#include "transfer_mode.h"
#include "transport/usb_composite/usbd.h"
#include "platform/fs_mount.h"
#include "services/recording_service.h"

#include <zephyr/logging/log.h>
#include <zephyr/usb/usbd.h>
#include <zephyr/usb/class/usbd_msc.h>
#include <zephyr/storage/disk_access.h>

LOG_MODULE_REGISTER(hpi_msc, CONFIG_HPI_APP_LOG_LEVEL);

/* One LUN over the SDMMC disk (disk-name = "SD" in the v4 board DTS). The first
 * macro arg is the disk_access name token; T10 vendor/product/revision are the
 * SCSI INQUIRY strings the host shows. */
USBD_DEFINE_MSC_LUN(SD, "SD", "Protocen", "HealthyPi6 SD", "1.0");

#define MSC_DISK_NAME "SD"

/* True only when a real SD medium is present and reports non-zero capacity.
 * Arming MSC over an empty slot would tear down USB/FS and expose a broken
 * "no medium" LUN to the host, so we refuse up front. */
static bool sd_medium_present(void)
{
	/* A mounted FS implies the card is present and good. */
	if (platform_fs_is_ready()) {
		return true;
	}

	/* FS not mounted (no card at boot, or hot-inserted). Probe the block
	 * device directly -- safe here because nothing is mounted on it. */
	if (disk_access_init(MSC_DISK_NAME) != 0) {
		return false;
	}

	uint32_t sector_count = 0;
	if (disk_access_ioctl(MSC_DISK_NAME, DISK_IOCTL_GET_SECTOR_COUNT,
			      &sector_count) != 0) {
		return false;
	}
	return sector_count > 0;
}

int hpi_transfer_arm(void)
{
	if (hpi_usb_msc_armed()) {
		return 0;
	}

	/* No SD card -> nothing to expose. Fail before touching USB/FS so the
	 * host keeps its CDC ports and gets a clean NO_MEDIA error. */
	if (!sd_medium_present()) {
		LOG_WRN("transfer arm refused: no SD card present");
		return -ENODEV;
	}

	/* Stop any active recording before releasing the FS to the host. */
	if (hpi_recording_active()) {
		LOG_INF("transfer arm: stopping active recording first");
		(void)hpi_recording_stop();
	}

	int rc = platform_fs_unmount();
	if (rc) {
		LOG_ERR("transfer arm: FS unmount failed (%d)", rc);
		return rc;
	}

	rc = hpi_usb_msc_set(true);
	if (rc) {
		LOG_ERR("transfer arm: MSC enable failed (%d); remounting FS", rc);
		(void)platform_fs_remount();
		return rc;
	}

	LOG_INF("Transfer Mode ARMED: SD exposed to host over USB MSC");
	return 0;
}

int hpi_transfer_disarm(void)
{
	if (!hpi_usb_msc_armed()) {
		return 0;
	}

	int rc = hpi_usb_msc_set(false);
	if (rc) {
		LOG_ERR("transfer disarm: MSC disable failed (%d)", rc);
		/* still attempt to remount so the device regains the FS */
	}

	int r2 = platform_fs_remount();
	if (r2) {
		LOG_ERR("transfer disarm: FS remount failed (%d)", r2);
	}
	/* TODO(step5): resume the recording service. */

	LOG_INF("Transfer Mode disarmed: SD back under device control");
	return rc ? rc : r2;
}

int hpi_transfer_set(bool on)
{
	return on ? hpi_transfer_arm() : hpi_transfer_disarm();
}

bool hpi_transfer_armed(void)
{
	return hpi_usb_msc_armed();
}
