/*
 * Copyright (c) 2024 Protocentral
 *
 * SPDX-License-Identifier: MIT
 */

#include <app_version.h>

#include "ipc_module.h"
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/ipc/ipc_service.h>
#include <zephyr/logging/log.h>
#include <string.h>

LOG_MODULE_REGISTER(ipc_module_m4, LOG_LEVEL_DBG);

/* IPC endpoint and configuration */
static struct ipc_ept hpi_ipc_ept;
static const struct device *ipc_instance;
static bool ipc_ready = false;

/* Semaphore for IPC bound state */
K_SEM_DEFINE(ipc_bound_sem, 0, 1);



/* Message callbacks */
static struct {
	hpi_ipc_msg_callback_t callback;
	void *user_data;
} msg_callbacks[HPI_IPC_MSG_TYPE_MAX];

/* Statistics */
static struct hpi_ipc_stats ipc_stats = {0};

/* IPC endpoint callbacks */
static void hpi_ipc_ept_bound(void *priv)
{
	LOG_INF("IPC endpoint bound - M4 REMOTE ready");
	ipc_ready = true;
	k_sem_give(&ipc_bound_sem);
}

static void hpi_ipc_ept_unbound(void *priv)
{
	LOG_WRN("IPC endpoint unbound");
	ipc_ready = false;
}

static void hpi_ipc_ept_recv(const void *data, size_t len, void *priv)
{
	const struct hpi_ipc_msg *msg = (const struct hpi_ipc_msg *)data;
	
	if (len < sizeof(struct hpi_ipc_msg)) {
		LOG_ERR("IPC message too small: %d", len);
		ipc_stats.receive_errors++;
		return;
	}

	if (msg->type >= HPI_IPC_MSG_TYPE_MAX) {
		LOG_ERR("IPC invalid message type: %d", msg->type);
		ipc_stats.receive_errors++;
		return;
	}

	if (msg->length != (len - sizeof(struct hpi_ipc_msg))) {
		LOG_ERR("IPC message length mismatch: expected=%d, actual=%d", 
			msg->length, len - sizeof(struct hpi_ipc_msg));
		ipc_stats.receive_errors++;
		return;
	}

	ipc_stats.messages_received++;

	/* Call registered callback if available */
	if (msg_callbacks[msg->type].callback) {
		msg_callbacks[msg->type].callback(msg->type, msg->data, msg->length,
						  msg_callbacks[msg->type].user_data);
	}
}

static void hpi_ipc_ept_error(const char *message, void *priv)
{
	LOG_ERR("IPC endpoint error: %s", message);
	ipc_stats.send_errors++;
}

/* IPC endpoint configuration */
static struct ipc_ept_cfg hpi_ipc_ept_cfg = {
	.name = "hpi_ipc",
	.cb = {
		.bound    = hpi_ipc_ept_bound,
		.unbound  = hpi_ipc_ept_unbound,
		.received = hpi_ipc_ept_recv,
		.error    = hpi_ipc_ept_error,
	},
};

int hpi_ipc_init(void)
{
	int ret;

	LOG_INF("Initializing IPC service - M4 REMOTE");
	
	/* M7 takes ~6-8 seconds to boot (display + hardware init).
	 * Wait with generous timeout for M7 to complete IPC initialization. */
	LOG_INF("M4: Waiting for M7 to initialize IPC (M7 boots slower due to display)...");
	
	/* Give M7 time to open IPC instance and register endpoint */
	LOG_INF("M4: Waiting 7 seconds for M7 to reach IPC init...");
	k_sleep(K_SECONDS(7));
	LOG_INF("M4: Proceeding with IPC initialization...");
	
	/* Get IPC instance from device tree */
	ipc_instance = DEVICE_DT_GET(DT_NODELABEL(ipc0));
	if (!device_is_ready(ipc_instance)) {
		LOG_ERR("IPC instance not ready");
		return -ENODEV;
	}
	LOG_INF("M4: IPC instance ready");

	/* Open IPC instance */
	ret = ipc_service_open_instance(ipc_instance);
	if (ret < 0 && ret != -EALREADY) {
		LOG_ERR("Failed to open IPC instance: %d", ret);
		return ret;
	}
	LOG_INF("M4: IPC instance opened (ret=%d)", ret);

	/* Register endpoint - REMOTE registers and waits for HOST to bind */
	LOG_INF("M4: Registering endpoint as REMOTE (will wait for HOST)...");
	ret = ipc_service_register_endpoint(ipc_instance, &hpi_ipc_ept, &hpi_ipc_ept_cfg);
	if (ret < 0) {
		LOG_ERR("Failed to register IPC endpoint: %d", ret);
		return ret;
	}
	LOG_INF("M4: Endpoint registered, waiting for HOST to bind...");

	/* Wait for endpoint to be bound 
	 * M7 takes ~6-8 seconds to boot (display + hardware init), so give it 15s */
	LOG_INF("M4: Waiting up to 15s for M7 HOST to bind...");
	ret = k_sem_take(&ipc_bound_sem, K_MSEC(15000));
	if (ret != 0) {
		LOG_ERR("IPC endpoint binding timeout (15s)");
		return -ETIMEDOUT;
	}

	LOG_INF("IPC service initialized successfully - M4 REMOTE");

	/* Report our firmware version to the M7 first thing after binding. The M4
	 * image is host-updatable, and the M4 console (USART2) is not broken out
	 * on v5, so this IPC message is the only way the running version is
	 * observable (surfaced as m4fw in device_info / fw_versions). Non-fatal:
	 * a send failure must not fail the bind. */
	{
		struct hpi_ipc_version ver = {0};
		strncpy(ver.version, APP_VERSION_STRING, sizeof(ver.version) - 1);
		int vret = hpi_ipc_send(HPI_IPC_MSG_TYPE_VERSION, &ver, sizeof(ver));
		if (vret < 0) {
			LOG_WRN("M4: version report failed (%d) - continuing", vret);
		} else {
			LOG_INF("M4: reported version %s", ver.version);
		}
	}

	return 0;
}

int hpi_ipc_send(enum hpi_ipc_msg_type msg_type, const void *data, size_t len)
{
	struct hpi_ipc_msg *msg;
	size_t total_len;
	int ret;

	if (!ipc_ready) {
		LOG_ERR("IPC not ready");
		return -ENOTCONN;
	}

	if (msg_type >= HPI_IPC_MSG_TYPE_MAX) {
		LOG_ERR("Invalid message type: %d", msg_type);
		return -EINVAL;
	}

	if (len > HPI_IPC_MAX_DATA_SIZE) {
		LOG_ERR("Message data too large: %d", len);
		return -EMSGSIZE;
	}

	total_len = sizeof(struct hpi_ipc_msg) + len;
	msg = k_malloc(total_len);
	if (!msg) {
		LOG_ERR("Failed to allocate message buffer");
		ipc_stats.send_errors++;
		return -ENOMEM;
	}

	/* Prepare message */
	msg->type = msg_type;
	msg->reserved = 0;
	msg->length = len;
	if (data && len > 0) {
		memcpy(msg->data, data, len);
	}

	/* Send message */
	ret = ipc_service_send(&hpi_ipc_ept, msg, total_len);
	if (ret < 0) {
		LOG_ERR("Failed to send IPC message: %d", ret);
		ipc_stats.send_errors++;
	} else {
		ipc_stats.messages_sent++;
		ret = 0;  /* Success - normalize return value */
	}

	k_free(msg);
	return ret;
}

bool hpi_ipc_is_ready(void)
{
	return ipc_ready;
}

int hpi_ipc_register_callback(enum hpi_ipc_msg_type msg_type,
			      hpi_ipc_msg_callback_t callback,
			      void *user_data)
{
	if (msg_type >= HPI_IPC_MSG_TYPE_MAX) {
		return -EINVAL;
	}

	msg_callbacks[msg_type].callback = callback;
	msg_callbacks[msg_type].user_data = user_data;

	LOG_DBG("Registered callback for message type %d", msg_type);
	return 0;
}

void hpi_ipc_get_stats(struct hpi_ipc_stats *stats)
{
	if (stats) {
		*stats = ipc_stats;
	}
}


/* IPC message callback functions */
static void ipc_test_msg_callback(enum hpi_ipc_msg_type msg_type,
				  const void *data, size_t len, void *user_data)
{
	/* Logged only: replying here would add bidirectional traffic that can
	 * saturate the IPC buffers. */
	LOG_DBG("M4: Received IPC test message from M7, len=%d", len);
}

static void ipc_cmd_msg_callback(enum hpi_ipc_msg_type msg_type,
				 const void *data, size_t len, void *user_data)
{
	/* Logged only: see ipc_test_msg_callback(). */
	LOG_DBG("M4: Received IPC command message from M7, len=%d", len);
}

/* Called directly from main(); there is deliberately no IPC thread here. An
 * auto-started one caused duplicate initialization and timing races. */
int hpi_ipc_m4_init(void)
{
	int ret;

	/* Initialize IPC service - M4 (remote) should initialize before M7 (host) */
	LOG_INF("M4: Initializing IPC service...");
	ret = hpi_ipc_init();
	if (ret < 0) {
		LOG_ERR("M4: Failed to initialize IPC service: %d", ret);
		return ret;
	} else {
		LOG_INF("M4: IPC service initialized successfully");
	}
	
	/* Register message callbacks */
	ret = hpi_ipc_register_callback(HPI_IPC_MSG_TYPE_TEST, ipc_test_msg_callback, NULL);
	if (ret < 0) {
		LOG_ERR("M4: Failed to register test callback: %d", ret);
		return ret;
	}
	
	ret = hpi_ipc_register_callback(HPI_IPC_MSG_TYPE_CMD, ipc_cmd_msg_callback, NULL);
	if (ret < 0) {
		LOG_ERR("M4: Failed to register command callback: %d", ret);
		return ret;
	}
	
	LOG_INF("M4: IPC callbacks registered successfully");

	return 0;
}
