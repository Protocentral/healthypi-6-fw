/*
 * Copyright (c) 2024-2026 Protocentral Electronics
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/storage/disk_access.h>
#include <zephyr/fs/fs.h>
#include <ff.h>

#include "fs_mount.h"

LOG_MODULE_REGISTER(hpi_fs, CONFIG_HPI_APP_LOG_LEVEL);

#define DISK_DRIVE_NAME "SD"
#define DISK_MOUNT_PT   "/" DISK_DRIVE_NAME ":"

static FATFS fat_fs;
static struct fs_mount_t mp = {
    .type = FS_FATFS,
    .fs_data = &fat_fs,
    .mnt_point = DISK_MOUNT_PT,
};

/* Consumers (UI, recorder) wait on this; posted only after a clean mount. */
static K_SEM_DEFINE(fs_ready_sem, 0, 1);
static volatile bool fs_ready;

int platform_fs_mount_init(void)
{
    int rc;

    /* Bring up the SD block device first. A missing/failed card is
     * non-fatal -- we log and leave the FS "not ready". */
    rc = disk_access_init(DISK_DRIVE_NAME);
    if (rc != 0) {
        LOG_WRN("SD disk init failed (%d) -- continuing without SD", rc);
        return rc;
    }

    rc = fs_mount(&mp);
    if (rc != 0) {
        LOG_WRN("SD mount failed (%d) -- continuing without SD", rc);
        return rc;
    }

    struct fs_statvfs s;
    if (fs_statvfs(DISK_MOUNT_PT, &s) == 0) {
        uint32_t total_mb = (uint32_t)((uint64_t)s.f_frsize * s.f_blocks >> 20);
        uint32_t free_mb  = (uint32_t)((uint64_t)s.f_frsize * s.f_bfree  >> 20);
        LOG_INF("SD mounted at %s: %u MB total, %u MB free",
                DISK_MOUNT_PT, total_mb, free_mb);
    } else {
        LOG_INF("SD mounted at %s", DISK_MOUNT_PT);
    }

    fs_ready = true;
    k_sem_give(&fs_ready_sem);
    return 0;
}

bool platform_fs_wait_ready(uint32_t timeout_ms)
{
    if (fs_ready) {
        return true;
    }
    return k_sem_take(&fs_ready_sem, K_MSEC(timeout_ms)) == 0;
}

bool platform_fs_is_ready(void)
{
    return fs_ready;
}

int platform_fs_unmount(void)
{
    if (!fs_ready) {
        return 0;   /* already unmounted / never mounted */
    }
    int rc = fs_unmount(&mp);
    if (rc != 0 && rc != -EINVAL) {   /* -EINVAL = not mounted */
        LOG_ERR("SD unmount failed (%d)", rc);
        return rc;
    }
    fs_ready = false;
    /* Re-arm the one-shot ready sem so a later remount can post it again. */
    k_sem_reset(&fs_ready_sem);
    LOG_INF("SD unmounted (Transfer Mode)");
    return 0;
}

int platform_fs_remount(void)
{
    if (fs_ready) {
        return 0;
    }
    int rc = fs_mount(&mp);
    if (rc != 0) {
        LOG_WRN("SD remount failed (%d)", rc);
        return rc;
    }
    fs_ready = true;
    k_sem_give(&fs_ready_sem);
    LOG_INF("SD remounted at %s", DISK_MOUNT_PT);
    return 0;
}
