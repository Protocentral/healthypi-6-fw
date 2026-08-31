// ProtoCentral Electronics (info@protocentral.com)
// SPDX-License-Identifier: Apache-2.0

#include "afe4400.h"

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(AFE4400_DECODER, CONFIG_SENSOR_LOG_LEVEL);

/* Decoder that converts AFE4400 24-bit big-endian register returns into signed 22-bit samples.
 * The driver provides raw buffers where each sample is MSB-first 3 bytes. This decoder accepts a
 * buffer that is a multiple of 3 bytes (N samples) and emits N int32_t values (4 bytes each)
 * with sign-extension from 22-bit two's complement (bit21 is sign).
 */
static int afe4400_decoder_get_frame_count(const uint8_t *buffer, enum sensor_channel channel,
                                           size_t channel_idx, uint16_t *frame_count)
{
    ARG_UNUSED(channel);
    ARG_UNUSED(channel_idx);

    /* infer number of samples from the known driver sample buffer size */
    size_t raw_bytes = sizeof(((struct afe4400_data *)0)->sample_buf);
    uint16_t samples = raw_bytes / 3;
    if (samples == 0) {
        *frame_count = 0;
        return -EINVAL;
    }

    /* single decoded frame containing all samples parsed from the buffer */
    *frame_count = 1;
    return 0;
}

static int afe4400_decoder_get_size_info(enum sensor_channel channel, size_t *base_size, size_t *frame_size)
{
    ARG_UNUSED(channel);
    size_t raw_bytes = sizeof(((struct afe4400_data *)0)->sample_buf);
    size_t samples = raw_bytes / 3;

    *base_size = 0;
    /* each decoded sample is an int32_t */
    *frame_size = samples * sizeof(int32_t);
    return 0;
}

static int afe4400_decoder_decode(const uint8_t *buffer, enum sensor_channel channel,
                                  size_t channel_idx, uint32_t *fit, uint16_t max_count, void *data_out)
{
    ARG_UNUSED(channel);
    ARG_UNUSED(channel_idx);
    ARG_UNUSED(fit);

    if (!data_out || !buffer) {
        return -EINVAL;
    }

    size_t raw_bytes = sizeof(((struct afe4400_data *)0)->sample_buf);
    size_t samples = raw_bytes / 3;
    int32_t *out = data_out;

    for (size_t i = 0; i < samples; ++i) {
        const uint8_t *p = &buffer[i * 3];
        uint32_t raw24 = ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | (uint32_t)p[2];
        /* ADC is 22-bit, lower 22 bits are valid; sign bit is bit21 */
        uint32_t v22 = raw24 & 0x3FFFFFu;
        int32_t sval;
        if (v22 & (1u << 21)) {
            /* negative: sign extend */
            sval = (int32_t)(v22 | ~0x3FFFFFu);
        } else {
            sval = (int32_t)v22;
        }
        out[i] = sval;
    }

    return 0;
}

SENSOR_DECODER_API_DT_DEFINE() = {
    .get_frame_count = afe4400_decoder_get_frame_count,
    .get_size_info = afe4400_decoder_get_size_info,
    .decode = afe4400_decoder_decode,
};

int afe4400_get_decoder(const struct device *dev, const struct sensor_decoder_api **decoder)
{
    ARG_UNUSED(dev);
    *decoder = &SENSOR_DECODER_NAME();
    return 0;
}
