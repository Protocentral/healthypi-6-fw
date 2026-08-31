/*
 * Copyright (c) 2026 Protocentral Electronics
 * SPDX-License-Identifier: MIT
 *
 * DFU service -- see dfu_service.h for the design and why recovery exists.
 */

#include "dfu_service.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/reboot.h>
#include <errno.h>

LOG_MODULE_REGISTER(hpi_dfu, CONFIG_HPI_APP_LOG_LEVEL);

#if IS_ENABLED(CONFIG_HPI_RECOVERY_MODE)

/* Recovery mode without a place to put the flag would "succeed" and then
 * reboot into the normal application, so fail at compile time. Both symbols
 * come from prj.signed.conf; the DT side from healthypi6_v5_bootmode.dtsi. */
#if !IS_ENABLED(CONFIG_RETENTION_BOOT_MODE)
#error "CONFIG_HPI_RECOVERY_MODE needs CONFIG_RETENTION_BOOT_MODE and a \
'zephyr,boot-mode' chosen node -- both live in the signed flavor \
(app_m7/prj.signed.conf + healthypi6_v5_bootmode.dtsi)."
#endif

#include <zephyr/retention/bootmode.h>

bool hpi_dfu_recovery_available(void)
{
	return true;
}

int hpi_dfu_recovery_arm(void)
{
	int rc = bootmode_set(BOOT_MODE_TYPE_BOOTLOADER);

	if (rc != 0) {
		LOG_ERR("recovery: could not arm the boot-mode flag (%d)", rc);
		return -EIO;
	}

	LOG_WRN("recovery ARMED -- the next reset stops in MCUboot serial "
		"recovery (single USB CDC port), not the application");
	return 0;
}

int hpi_dfu_recovery_disarm(void)
{
	int rc = bootmode_clear();

	if (rc != 0) {
		LOG_ERR("recovery: could not clear the boot-mode flag (%d)", rc);
		return -EIO;
	}
	LOG_INF("recovery disarmed");
	return 0;
}

bool hpi_dfu_recovery_armed(void)
{
	/* bootmode_check() returns 1 on a match, 0 on no match, -errno when the
	 * area is unreadable or never written (e.g. garbage backup SRAM on first
	 * power-on). Report an error as "not armed" -- the safe reading. */
	return bootmode_check(BOOT_MODE_TYPE_BOOTLOADER) == 1;
}

#else /* !CONFIG_HPI_RECOVERY_MODE -- dev flavor, no bootloader */

bool hpi_dfu_recovery_available(void)
{
	return false;
}

int hpi_dfu_recovery_arm(void)
{
	return -ENOTSUP;
}

int hpi_dfu_recovery_disarm(void)
{
	return -ENOTSUP;
}

bool hpi_dfu_recovery_armed(void)
{
	return false;
}

#endif /* CONFIG_HPI_RECOVERY_MODE */

void hpi_dfu_reboot(void)
{
	LOG_WRN("rebooting on request");
	/* Give the log backend a moment to drain -- an SMP reply and the last
	 * console lines are both in flight when this is called from a command
	 * handler's work item. */
	k_sleep(K_MSEC(100));
	sys_reboot(SYS_REBOOT_WARM);
}
