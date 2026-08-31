/*
 * Copyright (c) 2024-2026 Protocentral Electronics
 * SPDX-License-Identifier: MIT
 */

#ifndef HPI_CORE_BUS_SELFTEST_H
#define HPI_CORE_BUS_SELFTEST_H

#ifdef __cplusplus
extern "C" {
#endif

/* Run the on-device sample-bus self-test (logs a PASS/FAIL line). */
void hpi_bus_selftest_run(void);

#ifdef __cplusplus
}
#endif

#endif /* HPI_CORE_BUS_SELFTEST_H */
