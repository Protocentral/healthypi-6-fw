/*
 * Copyright (c) 2026 Protocentral
 *
 * SPDX-License-Identifier: MIT
 *
 * HealthyBridge host link over UART -- full duplex.
 *
 * Framed binary link (healthybridge_esp32_codec.h) over a UART with hardware
 * RTS/CTS. The flow control is the point: when the ESP32-C6 stalls -- typically
 * a WiFi burst on its single core -- it de-asserts RTS and the STM32 halts the
 * transmitter at the byte boundary in hardware. Nothing is lost on the wire.
 * The stall surfaces here as a longer uart_tx(), and if it outlasts the
 * deadline, as one counted dropped frame.
 *
 * Receive is a DMA double-buffer feeding a streaming decoder from a work item;
 * the peer may speak unsolicited, so control replies and status arrive without
 * polling.
 */

#define DT_DRV_COMPAT protocentral_healthybridge_esp32_uart

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/linker/section_tags.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/ring_buffer.h>
#include <errno.h>
#include <string.h>

#include "healthybridge_esp32_codec.h"
#include "healthybridge_esp32_link.h"
#include "healthybridge_esp32_uart_driver.h"

LOG_MODULE_REGISTER(healthybridge_uart_drv, LOG_LEVEL_INF);

BUILD_ASSERT(IS_ENABLED(CONFIG_UART_ASYNC_API),
	     "HealthyBridge UART needs CONFIG_UART_ASYNC_API (DMA transmit). "
	     "At 2 Mbaud an interrupt-per-byte transmitter costs ~200k IRQ/s.");

#define HB_UART_FRAME_MAX (HPI_HB_HEADER_SIZE + HPI_HB_MAX_PAYLOAD_SIZE + HPI_HB_CRC_SIZE)

/*
 * The transmit buffer is a DMA source and must live in non-cached RAM: the
 * stm32 drivers perform no cache maintenance on DMA transfers, so a cached
 * buffer transfers whatever happens to be in physical RAM (correctly framed
 * garbage). One buffer is enough because tx_lock serialises encode-and-send
 * and the caller does not return until the transfer completes.
 */
static uint8_t hb_uart_tx_buf[HB_UART_FRAME_MAX] __nocache __aligned(32);

/*
 * Receive: two DMA landing buffers handed to the driver alternately, plus a
 * staging ring the ISR copies into so the decoder runs in a work item rather
 * than in interrupt context (the same rule the M4 IPC path follows).
 *
 * Sizing: ESP->M7 traffic is control replies and status, tens of bytes per
 * second, so this is generously oversized. The chunk size only sets how often
 * UART_RX_RDY fires; the inactivity timeout below is what actually delivers a
 * short frame promptly.
 */
#define HB_UART_RX_CHUNK   256
#define HB_UART_RX_RING    1024
#define HB_UART_RX_IDLE_US 2000   /* deliver a partial frame after 2 ms of silence */

static uint8_t hb_uart_rx_bufs[2][HB_UART_RX_CHUNK] __nocache __aligned(32);
static uint8_t hb_uart_rx_ring_store[HB_UART_RX_RING];

/* Pending control command. The reply arrives asynchronously on the RX work
 * item, so the requester waits on a semaphore instead of holding any lock. */
struct hb_uart_pending {
	struct k_sem done;
	uint8_t cmd_id;       /* command awaiting a reply, 0 = none */
	uint8_t status;
	uint16_t data_len;
	uint8_t data[64];
};

struct healthybridge_uart_config {
	const struct device *uart_dev;
	struct gpio_dt_spec reset_gpio;
	struct gpio_dt_spec ready_gpio;
	uint32_t baud;
};

struct healthybridge_uart_data {
	const struct device *dev;
	bool initialized;
	/* Reset line released and the boot wait elapsed. Commands sent while
	 * this is false cannot be answered, so they are refused rather than
	 * left to burn HB_UART_CMD_TIMEOUT_MS against a part in shutdown. */
	bool powered;
	struct k_mutex tx_lock;
	struct k_sem tx_done;
	uint8_t *tx_buf;
	uint16_t seq;
	int32_t tx_timeout_us;
	volatile int tx_result;
	struct healthybridge_uart_stats stats;

	/* receive */
	struct ring_buf rx_ring;
	struct k_work rx_work;
	struct hpi_hb_parser parser;
	uint8_t rx_next;                 /* which of hb_uart_rx_bufs to hand back */

	/* control request/response */
	struct k_mutex cmd_lock;         /* one outstanding command at a time */
	struct hb_uart_pending pending;

	/* last unsolicited status frame from the co-processor, with the uptime
	 * it arrived at -- a cache with no age is indistinguishable from a lie */
	struct hpi_hb_wifi_status_resp wifi;
	bool wifi_valid;
	int64_t wifi_at;
};

/* ---- transmit ---------------------------------------------------------- */

static int hb_uart_transmit(const struct healthybridge_uart_config *cfg,
			    struct healthybridge_uart_data *data, size_t len);

static void hb_uart_cb(const struct device *uart, struct uart_event *evt, void *user)
{
	struct healthybridge_uart_data *data = user;

	switch (evt->type) {
	case UART_TX_DONE:
		data->tx_result = 0;
		k_sem_give(&data->tx_done);
		break;
	case UART_TX_ABORTED:
		/*
		 * The transmitter did not drain before the deadline. With
		 * hardware flow control the overwhelmingly likely cause is the
		 * ESP32 holding RTS de-asserted, i.e. back-pressure -- not an
		 * error. Report it so the caller drops this frame rather than
		 * queueing behind a stalled link.
		 */
		data->tx_result = -ETIMEDOUT;
		k_sem_give(&data->tx_done);
		break;

	case UART_RX_RDY: {
		/*
		 * ISR context. Copy out and defer decoding: the CRC pass plus
		 * dispatch must not run here, and the callback must never
		 * block -- same rule as the M4 IPC path.
		 */
		const uint8_t *src = evt->data.rx.buf + evt->data.rx.offset;
		uint32_t n = ring_buf_put(&data->rx_ring, src, evt->data.rx.len);

		data->stats.rx_bytes += n;
		if (n < evt->data.rx.len) {
			data->stats.rx_ovf += evt->data.rx.len - n;
		}
		k_work_submit(&data->rx_work);
		break;
	}
	case UART_RX_BUF_REQUEST:
		/* Hand back the buffer that is not currently being filled. */
		data->rx_next ^= 1U;
		(void)uart_rx_buf_rsp(uart, hb_uart_rx_bufs[data->rx_next],
				      HB_UART_RX_CHUNK);
		break;

	case UART_RX_STOPPED:
		/* Framing/parity/overrun. Counted, then the DISABLED event that
		 * follows restarts reception -- a stopped receiver that is never
		 * re-enabled is indistinguishable from a dead wire. */
		data->stats.rx_stopped++;
		break;

	case UART_RX_DISABLED:
		data->rx_next = 0;
		(void)uart_rx_enable(uart, hb_uart_rx_bufs[0], HB_UART_RX_CHUNK,
				     HB_UART_RX_IDLE_US);
		break;

	default:
		break;
	}
}

/* ---- receive ----------------------------------------------------------- */

static void hb_uart_on_frame(uint8_t type, uint8_t flags, uint16_t seq,
			     const uint8_t *payload, uint16_t len, void *user)
{
	struct healthybridge_uart_data *data = user;

	ARG_UNUSED(flags);
	ARG_UNUSED(seq);

	switch (type) {
	case HPI_HB_MSG_TYPE_CONTROL_RESP: {
		const struct hpi_hb_control_resp *r = (const void *)payload;

		if (len < sizeof(*r)) {
			return;
		}
		/*
		 * Only wake a requester that is actually waiting for THIS
		 * command. A late reply to a command that already timed out
		 * must not be handed to the next caller as its answer.
		 */
		if (data->pending.cmd_id == 0 || data->pending.cmd_id != r->cmd_id) {
			return;
		}
		uint16_t dl = r->data_len;

		if (dl > len - sizeof(*r)) {
			dl = len - sizeof(*r);   /* trust the frame, not the field */
		}
		if (dl > sizeof(data->pending.data)) {
			dl = sizeof(data->pending.data);
		}
		data->pending.status = r->status;
		data->pending.data_len = dl;
		if (dl > 0) {
			memcpy(data->pending.data, r->data, dl);
		}
		k_sem_give(&data->pending.done);
		break;
	}
	case HPI_HB_MSG_TYPE_STATUS_RESP:
		/* Unsolicited status; cached here and served from cache by
		 * wifi_status(). */
		if (len >= sizeof(struct hpi_hb_wifi_status_resp)) {
			memcpy(&data->wifi, payload, sizeof(data->wifi));
			data->wifi.ssid[sizeof(data->wifi.ssid) - 1] = '\0';
			data->wifi_at = k_uptime_get();
			data->wifi_valid = true;
		}
		break;
	default:
		break;
	}
}

static void hb_uart_rx_work(struct k_work *work)
{
	struct healthybridge_uart_data *data =
		CONTAINER_OF(work, struct healthybridge_uart_data, rx_work);
	uint8_t chunk[64];
	uint32_t n;

	while ((n = ring_buf_get(&data->rx_ring, chunk, sizeof(chunk))) > 0) {
		hpi_hb_parser_feed(&data->parser, chunk, n, hb_uart_on_frame, data);
	}

	data->stats.rx_frames = data->parser.frames;
	data->stats.rx_crc_err = data->parser.crc_errors;
	data->stats.rx_resync = data->parser.resyncs;
}

/* Encode into the DMA buffer and transmit. Returns 0, or a negative errno. */
static int hb_uart_send_frame(const struct device *dev, uint8_t type,
			      const uint8_t *payload, uint16_t payload_len)
{
	const struct healthybridge_uart_config *cfg = dev->config;
	struct healthybridge_uart_data *data = dev->data;
	int len;
	int ret;

	if (!data->initialized) {
		return -ENODEV;
	}
	/*
	 * Co-processor in reset. CTS is pulled DOWN on this board, so the
	 * transmitter would happily clock every frame into a dead line at
	 * 2 Mbaud -- burning DMA, the USART clock and PA0 toggling, and counting
	 * each one as a success. Refuse instead, and distinctly from -ENODEV so
	 * a caller can tell "powered off" from "no such link".
	 */
	if (!data->powered) {
		return -ENETDOWN;
	}

	k_mutex_lock(&data->tx_lock, K_FOREVER);

	len = hpi_hb_encode(data->tx_buf, HB_UART_FRAME_MAX, type, 0, data->seq, payload, payload_len);
	if (len < 0) {
		data->stats.tx_error++;
		k_mutex_unlock(&data->tx_lock);
		return len;
	}
	data->seq++;

	ret = hb_uart_transmit(cfg, data, (size_t)len);

	k_mutex_unlock(&data->tx_lock);
	return ret;
}

/* Common tail: hand tx_buf[0..len) to the UART and wait for completion.
 * Caller holds tx_lock. */
static int hb_uart_transmit(const struct healthybridge_uart_config *cfg,
			    struct healthybridge_uart_data *data, size_t len)
{
	/*
	 * Deadline for THIS transfer: its own wire time x4, floored at 20 ms.
	 * Per-transfer, not max-frame, so a stalled link drops early instead of
	 * blocking the connectivity thread. x4 tolerates a co-processor stall of
	 * a few frame periods; the 20 ms floor keeps short control frames from
	 * timing out on scheduling jitter alone.
	 */
	int32_t deadline = (int32_t)MAX((uint32_t)((len * 10U * 4U * 1000000ULL) /
						   (cfg->baud ? cfg->baud : 921600U)),
					20000U);

	k_sem_reset(&data->tx_done);
	data->tx_result = -EIO;

	int ret = uart_tx(cfg->uart_dev, data->tx_buf, len, deadline);

	if (ret < 0) {
		data->stats.tx_error++;
		LOG_ERR("uart_tx rejected %zu B: %d", len, ret);
		return ret;
	}

	/*
	 * uart_tx() always completes with an event, so this wait is bounded by
	 * the driver's own timeout. The extra margin only covers scheduling.
	 */
	if (k_sem_take(&data->tx_done, K_USEC((uint64_t)deadline + 50000U)) != 0) {
		uart_tx_abort(cfg->uart_dev);
		data->stats.tx_timeout++;
		return -ETIMEDOUT;
	}

	if (data->tx_result != 0) {
		data->stats.tx_timeout++;
		return data->tx_result;
	}

	data->stats.frames++;
	data->stats.bytes += (uint32_t)len;
	return 0;
}

/* Encode a batched-sample frame straight into the DMA buffer -- no temporary
 * payload allocation. */
static int hb_uart_send_batch(const struct device *dev, uint8_t type,
			      uint32_t timestamp_ms, uint16_t rate_hz,
			      const void *samples, uint16_t count, size_t sample_sz)
{
	const struct healthybridge_uart_config *cfg = dev->config;
	struct healthybridge_uart_data *data = dev->data;
	int len;
	int ret;

	if (!data->initialized) {
		return -ENODEV;
	}
	if (samples == NULL || count == 0) {
		return -EINVAL;
	}

	k_mutex_lock(&data->tx_lock, K_FOREVER);

	len = hpi_hb_encode_batch(data->tx_buf, HB_UART_FRAME_MAX, type, data->seq, timestamp_ms, rate_hz,
				  samples, count, sample_sz);
	if (len < 0) {
		data->stats.tx_error++;
		k_mutex_unlock(&data->tx_lock);
		return len;
	}
	data->seq++;

	ret = hb_uart_transmit(cfg, data, (size_t)len);

	k_mutex_unlock(&data->tx_lock);
	return ret;
}

/* ---- link API ---------------------------------------------------------- */

static int hb_uart_api_send_ecg_multi(const struct device *dev, uint32_t timestamp_ms,
				      const struct hpi_ecg_sample_multi *samples,
				      uint16_t count)
{
	return hb_uart_send_batch(dev, HPI_HB_MSG_TYPE_ECG_DATA, timestamp_ms,
				  HPI_HB_ECG_RATE_HZ, samples, count,
				  sizeof(struct hpi_ecg_sample_multi));
}

static int hb_uart_api_send_ppg(const struct device *dev, uint32_t timestamp_ms,
				const struct hpi_ppg_sample *samples, uint16_t count)
{
	return hb_uart_send_batch(dev, HPI_HB_MSG_TYPE_PPG_DATA, timestamp_ms,
				  HPI_HB_PPG_RATE_HZ, samples, count,
				  sizeof(struct hpi_ppg_sample));
}

static int hb_uart_api_send_vitals(const struct device *dev,
				   const struct hpi_hb_vitals_payload *vitals)
{
	if (vitals == NULL) {
		return -EINVAL;
	}
	return hb_uart_send_frame(dev, HPI_HB_MSG_TYPE_VITALS,
				  (const uint8_t *)vitals, sizeof(*vitals));
}

static int hb_uart_api_send_hrv(const struct device *dev,
				const struct hpi_hb_hrv_payload *hrv)
{
	if (hrv == NULL) {
		return -EINVAL;
	}
	return hb_uart_send_frame(dev, HPI_HB_MSG_TYPE_HRV,
				  (const uint8_t *)hrv, sizeof(*hrv));
}

/* ---- control commands (request/response) -------------------------------- */

/*
 * Reply deadline. The co-processor answers from its own task, so this is
 * bounded by its scheduling latency under WiFi load, not by wire time. The
 * transmit lock is released as soon as the command is on the wire and the wait
 * happens on a semaphore, so a slow or absent reply costs the caller time and
 * the data path nothing.
 */
#define HB_UART_CMD_TIMEOUT_MS 300

static int hb_uart_command(const struct device *dev, uint8_t cmd_id,
			   const void *params, uint16_t param_len,
			   uint8_t *out, uint16_t out_cap, uint16_t *out_len)
{
	struct healthybridge_uart_data *data = dev->data;
	uint8_t buf[sizeof(struct hpi_hb_control_cmd) + 96];
	struct hpi_hb_control_cmd *c = (struct hpi_hb_control_cmd *)buf;
	int ret;

	if (!data->initialized) {
		return -ENODEV;
	}
	/*
	 * The co-processor is in reset. Every command would time out after
	 * HB_UART_CMD_TIMEOUT_MS, and a caller polling on a timer would spend
	 * that on every tick forever. Say so immediately instead.
	 */
	if (!data->powered) {
		return -EHOSTDOWN;
	}
	if (param_len > sizeof(buf) - sizeof(*c)) {
		return -EINVAL;
	}

	k_mutex_lock(&data->cmd_lock, K_FOREVER);

	c->cmd_id = cmd_id;
	c->reserved = 0;
	c->param_len = param_len;
	if (param_len > 0 && params != NULL) {
		memcpy(c->params, params, param_len);
	}

	k_sem_reset(&data->pending.done);
	data->pending.cmd_id = cmd_id;      /* arm before sending, not after */
	data->pending.data_len = 0;
	data->pending.status = 0;

	ret = hb_uart_send_frame(dev, HPI_HB_MSG_TYPE_CONTROL_CMD, buf,
				 (uint16_t)(sizeof(*c) + param_len));
	if (ret == 0) {
		if (k_sem_take(&data->pending.done, K_MSEC(HB_UART_CMD_TIMEOUT_MS)) != 0) {
			ret = -ETIMEDOUT;
		} else if (data->pending.status != HPI_HB_RESP_OK) {
			ret = -EIO;
		} else if (data->pending.data_len == 0) {
			/* Acked, but no payload: the link and the command are
			 * fine, the reply body is not implemented on the ESP
			 * side. */
			ret = -ENODATA;
		} else if (out != NULL) {
			uint16_t n = MIN(data->pending.data_len, out_cap);

			memcpy(out, data->pending.data, n);
			if (out_len != NULL) {
				*out_len = n;
			}
		}
	}

	data->pending.cmd_id = 0;
	k_mutex_unlock(&data->cmd_lock);
	return ret;
}

/*
 * Power the co-processor via its EN/reset line. The C6 has no separate reset
 * pin -- this is EN/CHIP_PU -- so asserting it is a shutdown (microamps), not a
 * held reset.
 *
 * Releasing blocks for CONFIG_HEALTHYBRIDGE_ESP32_BOOT_DELAY_MS so the caller
 * may send a command as soon as this returns. Powering down also invalidates
 * the cached WiFi status: serving a pre-shutdown state would report a radio
 * that is not merely disconnected but unpowered.
 */
static int hb_uart_api_link_power(const struct device *dev, bool on)
{
	const struct healthybridge_uart_config *cfg = dev->config;
	struct healthybridge_uart_data *data = dev->data;
	int ret;

	if (cfg->reset_gpio.port == NULL) {
		/* No line to drive: the part is whatever the board leaves it. */
		return -ENOSYS;
	}
	if (data->powered == on) {
		return 0;
	}

	/* gpio_dt_spec is logical: 1 = asserted = reset held = powered down. */
	ret = gpio_pin_set_dt(&cfg->reset_gpio, on ? 0 : 1);
	if (ret < 0) {
		LOG_ERR("reset GPIO set failed: %d", ret);
		return ret;
	}

	if (on) {
		k_msleep(CONFIG_HEALTHYBRIDGE_ESP32_BOOT_DELAY_MS);
		data->powered = true;
		LOG_INF("ESP32 powered up (boot wait %d ms)",
			CONFIG_HEALTHYBRIDGE_ESP32_BOOT_DELAY_MS);
	} else {
		data->powered = false;
		data->wifi_valid = false;
		hpi_hb_parser_reset(&data->parser);
		LOG_INF("ESP32 powered down (held in reset)");
	}
	return 0;
}

/* ---- BLE control ------------------------------------------------------- */

static int hb_uart_api_ble_adv(const struct device *dev, bool on)
{
	int rc = hb_uart_command(dev, on ? HPI_HB_CMD_BLE_ADV_START : HPI_HB_CMD_BLE_ADV_STOP,
				 NULL, 0, NULL, 0, NULL);

	/* A bare ack is success for a command that returns no data. */
	return (rc == -ENODATA) ? 0 : rc;
}

static int hb_uart_api_ble_set_name(const struct device *dev, const char *name)
{
	size_t len;

	if (name == NULL) {
		return -EINVAL;
	}
	len = strlen(name);
	/* The co-processor copies into a 32-byte buffer and truncates at 31
	 * (control.c BLE_SET_NAME). Reject rather than send a name that will
	 * silently come back different. */
	if (len == 0 || len > 31) {
		return -EINVAL;
	}

	int rc = hb_uart_command(dev, HPI_HB_CMD_BLE_SET_NAME, name, (uint16_t)len,
				 NULL, 0, NULL);

	return (rc == -ENODATA) ? 0 : rc;
}

/*
 * How long an unsolicited status frame may be served from cache. Must stay
 * longer than the peer's push interval (common path stays round-trip-free) and
 * shorter than the service's 2 s poll (a peer that stops pushing is re-queried
 * rather than believed indefinitely).
 */
#define HB_UART_WIFI_CACHE_MS 1500

static int hb_uart_api_wifi_status(const struct device *dev,
				   struct hpi_hb_wifi_status_resp *out)
{
	struct healthybridge_uart_data *data = dev->data;

	if (data->wifi_valid &&
	    (k_uptime_get() - data->wifi_at) < HB_UART_WIFI_CACHE_MS) {
		if (out != NULL) {
			*out = data->wifi;
		}
		return 0;
	}
	return hb_uart_command(dev, HPI_HB_CMD_GET_STATUS, NULL, 0,
			       (uint8_t *)out, sizeof(*out), NULL);
}

static int hb_uart_api_wifi_enable(const struct device *dev, bool on)
{
	int rc = hb_uart_command(dev, on ? HPI_HB_CMD_WIFI_ENABLE : HPI_HB_CMD_WIFI_DISABLE,
				 NULL, 0, NULL, 0, NULL);

	/* A bare ack is success for a command that returns no data. */
	return (rc == -ENODATA) ? 0 : rc;
}

static int hb_uart_api_wifi_disconnect(const struct device *dev)
{
	int rc = hb_uart_command(dev, HPI_HB_CMD_WIFI_DISABLE, NULL, 0, NULL, 0, NULL);

	return (rc == -ENODATA) ? 0 : rc;
}

static int hb_uart_api_wifi_softap(const struct device *dev)
{
	int rc = hb_uart_command(dev, HPI_HB_CMD_WIFI_SOFTAP, NULL, 0, NULL, 0, NULL);

	return (rc == -ENODATA) ? 0 : rc;
}

static int hb_uart_api_wifi_connect(const struct device *dev, const char *ssid,
				    const char *password)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(ssid);
	ARG_UNUSED(password);
	/*
	 * Deliberately not supported: credentials are provisioned through the
	 * ESP32's SoftAP captive portal, not pushed over the link. There is no
	 * HB_CMD to carry them, and inventing one would hit the far end's
	 * `default:` and be silently ignored.
	 */
	return -ENOTSUP;
}

static int hb_uart_api_rx_stats(const struct device *dev,
				struct healthybridge_rx_stats *out)
{
	struct healthybridge_uart_data *data = dev->data;

	if (out == NULL) {
		return -EINVAL;
	}
	out->bytes = data->stats.rx_bytes;
	out->frames = data->parser.frames;
	out->crc_err = data->parser.crc_errors;
	out->resync = data->parser.resyncs;
	out->ovf = data->stats.rx_ovf;
	out->stopped = data->stats.rx_stopped;
	return 0;
}

static const struct healthybridge_link_api hb_uart_api = {
	.send_ecg_multi = hb_uart_api_send_ecg_multi,
	.send_ppg = hb_uart_api_send_ppg,
	.send_vitals = hb_uart_api_send_vitals,
	.send_hrv = hb_uart_api_send_hrv,
	.link_power = hb_uart_api_link_power,
	.ble_adv = hb_uart_api_ble_adv,
	.ble_set_name = hb_uart_api_ble_set_name,
	.wifi_status = hb_uart_api_wifi_status,
	.wifi_enable = hb_uart_api_wifi_enable,
	.wifi_connect = hb_uart_api_wifi_connect,
	.wifi_disconnect = hb_uart_api_wifi_disconnect,
	.wifi_softap = hb_uart_api_wifi_softap,
	.get_rx_stats = hb_uart_api_rx_stats,
	.name = "uart",
};

/* ---- diagnostics ------------------------------------------------------- */

void healthybridge_uart_get_stats(const struct device *dev,
				  struct healthybridge_uart_stats *out, bool clear)
{
	struct healthybridge_uart_data *data = dev->data;

	k_mutex_lock(&data->tx_lock, K_FOREVER);
	if (out != NULL) {
		*out = data->stats;
	}
	if (clear) {
		memset(&data->stats, 0, sizeof(data->stats));
	}
	k_mutex_unlock(&data->tx_lock);
}

/* ---- init -------------------------------------------------------------- */

static int hb_uart_init(const struct device *dev)
{
	const struct healthybridge_uart_config *cfg = dev->config;
	struct healthybridge_uart_data *data = dev->data;
	int ret;

	if (!device_is_ready(cfg->uart_dev)) {
		LOG_ERR("parent UART %s not ready", cfg->uart_dev->name);
		return -ENODEV;
	}

	data->dev = dev;
	k_mutex_init(&data->tx_lock);
	k_mutex_init(&data->cmd_lock);
	k_sem_init(&data->tx_done, 0, 1);
	k_sem_init(&data->pending.done, 0, 1);
	k_work_init(&data->rx_work, hb_uart_rx_work);
	ring_buf_init(&data->rx_ring, sizeof(hb_uart_rx_ring_store), hb_uart_rx_ring_store);
	hpi_hb_parser_reset(&data->parser);
	data->tx_buf = hb_uart_tx_buf;
	data->seq = 0;
	data->rx_next = 0;

	/* Per-transfer, computed in hb_uart_transmit(); this is only for the
	 * banner, using the ECG frame that dominates the link. */
	uint32_t ecg_us = (uint32_t)((530ULL * 10U * 4U * 1000000ULL) /
				     (cfg->baud ? cfg->baud : 921600U));

	data->tx_timeout_us = (int32_t)MAX(ecg_us, 20000U);

	ret = uart_callback_set(cfg->uart_dev, hb_uart_cb, data);
	if (ret < 0) {
		LOG_ERR("uart_callback_set failed: %d (async API available?)", ret);
		return ret;
	}

	if (cfg->reset_gpio.port != NULL) {
		if (!device_is_ready(cfg->reset_gpio.port)) {
			LOG_ERR("reset GPIO port not ready");
			return -ENODEV;
		}
		/*
		 * Assert (GPIO_OUTPUT_ACTIVE on an active-low line drives it
		 * low), holding the co-processor's EN/CHIP_PU down. A bare
		 * configure-and-deassert is too brief to pull EN low through
		 * its decoupling cap, so the part would never reboot in step
		 * with the M7 -- hence the explicit 20 ms below when releasing.
		 */
		ret = gpio_pin_configure_dt(&cfg->reset_gpio, GPIO_OUTPUT_ACTIVE);
		if (ret < 0) {
			LOG_ERR("failed to configure reset GPIO: %d", ret);
			return ret;
		}
		data->powered = false;

		if (IS_ENABLED(CONFIG_HEALTHYBRIDGE_ESP32_HOLD_RESET_AT_BOOT)) {
			/*
			 * Leave it asserted. The C6 stays in shutdown until
			 * something calls link_power(true) -- see the Kconfig
			 * help for why booting it here browns out a USB-powered
			 * unit.
			 */
			LOG_INF("ESP32 held in reset (radios off until enabled)");
		} else {
			k_msleep(20);
			(void)hb_uart_api_link_power(dev, true);
		}
	}

	if (cfg->ready_gpio.port != NULL && device_is_ready(cfg->ready_gpio.port)) {
		(void)gpio_pin_configure_dt(&cfg->ready_gpio, GPIO_INPUT);
	}

	/*
	 * Enable receive LAST, after the ESP32 reset pulse above. Starting the
	 * receiver first would land the co-processor's boot-time line noise in
	 * the decoder, and -- more importantly -- an undrained receiver holds
	 * nRTS de-asserted, stalling the far end. With reception running, the
	 * FIFO drains and RTS asserts.
	 */
	ret = uart_rx_enable(cfg->uart_dev, hb_uart_rx_bufs[0], HB_UART_RX_CHUNK,
			     HB_UART_RX_IDLE_US);
	if (ret < 0) {
		LOG_ERR("uart_rx_enable failed: %d", ret);
		return ret;
	}

	data->initialized = true;

	LOG_INF("HealthyBridge UART up: %s @ %u baud, RTS/CTS, ECG-frame tx deadline %d us, rx enabled",
		cfg->uart_dev->name, cfg->baud, data->tx_timeout_us);

	return 0;
}

#define HEALTHYBRIDGE_UART_DEVICE_DEFINE(inst)                                        \
	static struct healthybridge_uart_data healthybridge_uart_data_##inst;         \
										      \
	static const struct healthybridge_uart_config healthybridge_uart_config_##inst = { \
		.uart_dev = DEVICE_DT_GET(DT_PARENT(DT_DRV_INST(inst))),              \
		.reset_gpio = GPIO_DT_SPEC_INST_GET_OR(inst, reset_gpios, {0}),       \
		.ready_gpio = GPIO_DT_SPEC_INST_GET_OR(inst, ready_gpios, {0}),       \
		.baud = DT_PROP(DT_PARENT(DT_DRV_INST(inst)), current_speed),         \
	};                                                                            \
										      \
	BUILD_ASSERT(DT_PROP(DT_PARENT(DT_DRV_INST(inst)), hw_flow_control),          \
		     "HealthyBridge UART requires hw-flow-control on the parent "     \
		     "UART -- without RTS/CTS this transport loses bytes exactly "    \
		     "the way the SPI link it replaces did.");                        \
										      \
	DEVICE_DT_INST_DEFINE(inst,                                                   \
			      hb_uart_init,                                           \
			      NULL,                                                   \
			      &healthybridge_uart_data_##inst,                        \
			      &healthybridge_uart_config_##inst,                      \
			      POST_KERNEL,                                            \
			      CONFIG_HEALTHYBRIDGE_ESP32_INIT_PRIORITY,               \
			      &hb_uart_api);

DT_INST_FOREACH_STATUS_OKAY(HEALTHYBRIDGE_UART_DEVICE_DEFINE)
