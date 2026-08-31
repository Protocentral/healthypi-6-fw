/*
 * Copyright (c) 2024-2026 Protocentral Electronics
 * SPDX-License-Identifier: MIT
 *
 * Bridge Zephyr MCUmgr os/datetime_set to the STM32 RTC driver (L5 control).
 *
 * The stock MCUmgr datetime_write handler leaves rtc_time.tm_wday = -1; the
 * STM32 RTC driver rejects tm_wday = -1 with -EINVAL, making os/datetime set
 * unusable on this platform out of the box. We intercept
 * MGMT_EVT_OP_OS_MGMT_DATETIME_SET, compute tm_wday from the date via Zeller's
 * congruence, and let the handler continue. Zero-cost when no set is happening.
 */

#include <zcbor_common.h>   /* must precede os_mgmt_callbacks.h */

#include <zephyr/drivers/rtc.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/mgmt/mcumgr/mgmt/callbacks.h>
#include <zephyr/mgmt/mcumgr/grp/os_mgmt/os_mgmt_callbacks.h>

LOG_MODULE_DECLARE(hpi_mgmt, LOG_LEVEL_INF);

/* Zeller's congruence -> 0=Sunday .. 6=Saturday. year full (e.g. 2026),
 * month 1..12, day 1..31. */
static int compute_weekday(int year, int month, int day)
{
	if (month < 3) {
		month += 12;
		year -= 1;
	}
	int K = year % 100;
	int J = year / 100;
	int h = (day + (13 * (month + 1)) / 5 + K + K / 4 + J / 4 + 5 * J) % 7;
	/* Zeller 0=Sat..6=Fri -> tm_wday 0=Sun..6=Sat */
	return (h + 6) % 7;
}

static enum mgmt_cb_return hpi_datetime_set_cb(uint32_t event,
					       enum mgmt_cb_return prev_status,
					       int32_t *rc, uint16_t *group,
					       bool *abort_more, void *data,
					       size_t data_size)
{
	ARG_UNUSED(prev_status);
	ARG_UNUSED(rc);
	ARG_UNUSED(group);
	ARG_UNUSED(abort_more);

	if (event != MGMT_EVT_OP_OS_MGMT_DATETIME_SET || data == NULL ||
	    data_size != sizeof(struct rtc_time)) {
		return MGMT_CB_OK;
	}

	struct rtc_time *t = (struct rtc_time *)data;

	/* tm_year = years since 1900; tm_mon = 0..11. */
	int calendar_year = t->tm_year + 1900;
	int calendar_month = t->tm_mon + 1;
	t->tm_wday = compute_weekday(calendar_year, calendar_month, t->tm_mday);

	LOG_DBG("datetime_set hook: %04d-%02d-%02d -> tm_wday=%d",
		calendar_year, calendar_month, t->tm_mday, t->tm_wday);
	return MGMT_CB_OK;
}

static struct mgmt_callback hpi_datetime_cb = {
	.callback = hpi_datetime_set_cb,
	.event_id = MGMT_EVT_OP_OS_MGMT_DATETIME_SET,
};

static int hpi_datetime_hook_init(void)
{
	mgmt_callback_register(&hpi_datetime_cb);
	LOG_INF("datetime_set hook: tm_wday auto-compute for STM32 RTC");
	return 0;
}

SYS_INIT(hpi_datetime_hook_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
