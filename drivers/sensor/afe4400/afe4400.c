// ProtoCentral Electronics (info@protocentral.com)
// SPDX-License-Identifier: Apache-2.0

#define DT_DRV_COMPAT ti_afe4400

/* Minimal device glue for AFE4400 - hooks into async RTIO code */

#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "afe4400.h"
#include "afe4400_regs.h"

LOG_MODULE_REGISTER(afe4400, LOG_LEVEL_INF);

/*
 * ---- Pulse repetition frequency (PPG sample rate) ----
 *
 * The AFE4400's timing engine counts from its 4 MHz clock (one count = 250 ns)
 * and repeats the acquisition pattern every PRPCOUNT+1 counts. Each repetition
 * asserts DATA_READY once, so the PRF *is* the PPG sample rate. Derived from
 * CONFIG_AFE4400_PRF_HZ so there is exactly one place to set the rate.
 *
 * The whole acquisition pattern scales with the period: it is TI's standard
 * four-phase timing, one quarter per phase (see afe4400_init_registers()).
 */
#define AFE4400_TIMER_CLK_HZ   4000000U
#define AFE4400_PRPCOUNT       ((AFE4400_TIMER_CLK_HZ / CONFIG_AFE4400_PRF_HZ) - 1U)

/* PRPCOUNT is a 16-bit register field, which bounds the minimum PRF. The
 * maximum is the AFE's own (2000 Hz); Kconfig limits the range. */
BUILD_ASSERT(AFE4400_PRPCOUNT <= 65535U,
	     "CONFIG_AFE4400_PRF_HZ too low: PRPCOUNT exceeds the 16-bit field");

/* Helper to read a 24-bit register and return signed 22-bit sample */
static int afe4400_read_result_reg(const struct device *dev, uint8_t reg, int32_t *out)
{
    uint32_t raw = afe4400_reg_read(dev, reg);
    /* Note: afe4400_reg_read currently returns the 24-bit register value and
     * does not signal errors. If the API is extended to return status, update
     * this function to handle errors appropriately. */
    uint32_t v22 = raw & 0x3FFFFFu;
    int32_t sval;
    if (v22 & (1u << 21))
    {
        sval = (int32_t)(v22 | ~0x3FFFFFu);
    }
    else
    {
        sval = (int32_t)v22;
    }
    *out = sval;
    return 0;
}

static int afe4400_sample_fetch(const struct device *dev, enum sensor_channel chan)
{
    ARG_UNUSED(chan);
    struct afe4400_data *data = dev->data;
    const uint8_t regs[6] = {
        AFE4400_REG_LED2VAL, AFE4400_REG_ALED2VAL, AFE4400_REG_LED1VAL,
        AFE4400_REG_ALED1VAL, AFE4400_REG_LED2_ALED2, AFE4400_REG_LED1_ALED1};

    for (size_t i = 0; i < 6; ++i)
    {
        int32_t v;
        int rc = afe4400_read_result_reg(dev, regs[i], &v);
        if (rc < 0)
        {
            return rc;
        }
        data->last_values[i] = v;
    }
    data->last_sample_ts = k_uptime_get();
    return 0;
}

static int afe4400_channel_get(const struct device *dev, enum sensor_channel chan, struct sensor_value *val)
{
    ARG_UNUSED(dev);
    ARG_UNUSED(chan);
    ARG_UNUSED(val);

    return -ENOTSUP;
}

/* Hardware reset. The RESET pin is active LOW and the DT flags it
 * GPIO_ACTIVE_LOW, so gpio_pin_set_dt(1) = LOW = in reset,
 * gpio_pin_set_dt(0) = HIGH = operational. */
int afe4400_hardware_reset(const struct device *dev)
{
    const struct afe4400_config *cfg = dev->config;
    int rc;

    if (!device_is_ready(cfg->reset_pin.port))
    {
        LOG_WRN("Reset pin not available");
        return -ENODEV;
    }

    /* Configure reset pin as output, initially INACTIVE (HIGH = not in reset) */
    rc = gpio_pin_configure_dt(&cfg->reset_pin, GPIO_OUTPUT_INACTIVE);
    if (rc < 0)
    {
        LOG_ERR("Failed to configure reset pin: %d", rc);
        return rc;
    }
    LOG_DBG("RESET configured: INACTIVE (DT says LOW = not in reset)");

    /* Assert reset: set to ACTIVE state (LOW voltage) */
    gpio_pin_set_dt(&cfg->reset_pin, 1);
    LOG_DBG("RESET set ACTIVE (LOW = device in reset)");
    k_msleep(10); /* Hold reset for 10ms per datasheet recommendation */

    /* Deassert reset: set to INACTIVE state (HIGH voltage) */
    gpio_pin_set_dt(&cfg->reset_pin, 0);
    LOG_DBG("RESET set INACTIVE (HIGH = device operational)");
    k_msleep(50); /* Wait for device to come out of reset */

    LOG_DBG("Hardware reset completed");
    return 0;
}

/* Software reset via CONTROL0 register */
int afe4400_software_reset(const struct device *dev)
{
    int rc;

    /* Write software reset bit to CONTROL0 register */
    rc = afe4400_reg_write(dev, AFE4400_REG_CONTROL0, AFE4400_CONTROL0_SW_RST);
    if (rc < 0)
    {
        LOG_ERR("Failed to write software reset: %d", rc);
        return rc;
    }

    /* Wait for reset to complete - datasheet recommends 50ms */
    k_msleep(50);

    LOG_DBG("Software reset completed");
    return 0;
}

/* Power down control via CONTROL2 register */
int afe4400_powerdown(const struct device *dev, bool powerdown)
{
    const struct afe4400_config *cfg = dev->config;

    int rc;

    if (powerdown)
    {
        /* Then assert PWDN pin (drive low to power down) */
        if (device_is_ready(cfg->pdwn_pin.port))
        {
            rc = gpio_pin_set_dt(&cfg->pdwn_pin, 0);
            if (rc < 0)
            {
                LOG_ERR("Failed to assert PWDN pin: %d", rc);
                return rc;
            }
        }

        LOG_DBG("AFE4400 powered down (register + PWDN pin)");
    }
    else
    {
        /* First deassert PWDN pin (drive high to power up) */
        if (device_is_ready(cfg->pdwn_pin.port))
        {
            rc = gpio_pin_set_dt(&cfg->pdwn_pin, 1);
            if (rc < 0)
            {
                LOG_ERR("Failed to deassert PWDN pin: %d", rc);
                return rc;
            }
            /* Wait for power-up to settle before register access */
            k_msleep(1);
        }

        LOG_DBG("AFE4400 powered up (PWDN pin + register)");
    }

    /* Wait for power state to settle */
    k_msleep(10);
    return 0;
}

/* Configure LED current levels */
int afe4400_configure_led_current(const struct device *dev, uint8_t led1_current, uint8_t led2_current)
{
    uint32_t ledcntrl_val;
    int rc;

    /* Validate current values (0-255, where 255 = ~50mA) */
    if (led1_current > 255 || led2_current > 255)
    {
        LOG_ERR("LED current values must be 0-255");
        return -EINVAL;
    }

    /* Combine LED1 and LED2 current settings */
    ledcntrl_val = ((uint32_t)led1_current << AFE4400_LEDCNTRL_LED1_SHIFT) |
                   ((uint32_t)led2_current << AFE4400_LEDCNTRL_LED2_SHIFT);

    rc = afe4400_reg_write(dev, AFE4400_REG_LEDCNTRL, ledcntrl_val);
    if (rc < 0)
    {
        LOG_ERR("Failed to write LED control register: %d", rc);
        return rc;
    }

    LOG_DBG("LED currents configured: LED1=%d, LED2=%d", led1_current, led2_current);
    return 0;
}

/* Configure transimpedance amplifier gain */
int afe4400_configure_tia_gain(const struct device *dev)
{
    int rc;

    /* Configure TIAGAIN register for moderate gain setting
     * This is typically application-specific based on photodiode characteristics
     */
    rc = afe4400_reg_write(dev, AFE4400_REG_TIAGAIN, 0x000000); /* Start with lowest gain */
    if (rc < 0)
        return rc;

    /* Configure TIA_AMB_GAIN for ambient light cancellation and second stage gain
     * Bit configuration:
     * [19:16] AMBDAC - Ambient DAC setting (0-15)
     * [14] STAGE2EN - Enable second stage (1)
     * [10:8] STG2GAIN - Second stage gain (0=0dB, 1=3dB, 2=6dB, etc.)
     * [7:3] CF_LED - Feedback capacitor setting
     * [2:0] RF_LED - Feedback resistor setting (0=500k, 1=250k, 2=100k, etc.)
     */
    uint32_t tia_amb_val = 0;
    tia_amb_val |= (0x8 << AFE4400_TIA_AMB_AMBDAC_SHIFT);   /* Mid-range ambient DAC */
    tia_amb_val |= AFE4400_TIA_AMB_STAGE2EN;                /* Enable second stage */
    tia_amb_val |= (0x1 << AFE4400_TIA_AMB_STG2GAIN_SHIFT); /* 3dB second stage gain */
    tia_amb_val |= (0x5 << AFE4400_TIA_AMB_CF_LED_SHIFT);   /* Moderate feedback cap */
    tia_amb_val |= (0x2 << AFE4400_TIA_AMB_RF_LED_SHIFT);   /* 100k feedback resistor */

    rc = afe4400_reg_write(dev, AFE4400_REG_TIA_AMB_GAIN, tia_amb_val);
    if (rc < 0)
    {
        LOG_ERR("Failed to write TIA_AMB_GAIN register: %d", rc);
        return rc;
    }

    LOG_DBG("TIA gain configuration completed");
    return 0;
}

/* Read diagnostic register for fault detection */
int afe4400_read_diagnostics(const struct device *dev, uint32_t *diag_status)
{
    int rc;

    /* Enable diagnostics mode */
    rc = afe4400_reg_write(dev, AFE4400_REG_CONTROL0, AFE4400_CONTROL0_DIAG_EN);
    if (rc < 0)
    {
        LOG_ERR("Failed to enable diagnostics: %d", rc);
        return rc;
    }

    /* Wait for diagnostics to complete */
    k_msleep(100);

    /* Read diagnostic register */
    *diag_status = afe4400_reg_read(dev, AFE4400_REG_DIAG);

    /* Log diagnostic results */
    if (*diag_status & AFE4400_DIAG_LED1OPEN)
    {
        LOG_WRN("LED1 open circuit detected");
    }
    if (*diag_status & AFE4400_DIAG_LED2OPEN)
    {
        LOG_WRN("LED2 open circuit detected");
    }
    if (*diag_status & AFE4400_DIAG_LEDSC)
    {
        LOG_WRN("LED short circuit detected");
    }
    if (*diag_status & AFE4400_DIAG_PDOC)
    {
        LOG_WRN("Photodiode open circuit detected");
    }
    if (*diag_status & AFE4400_DIAG_PDSC)
    {
        LOG_WRN("Photodiode short circuit detected");
    }

    return 0;
}

/* Trigger callback functions */
static void afe4400_drdy_gpio_callback(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
    struct afe4400_data *data = CONTAINER_OF(cb, struct afe4400_data, drdy_cb);
    const struct afe4400_config *config = data->dev->config;
    int ret;

    /* CRITICAL DEBUG: Log interrupt firing rate */
    static uint32_t irq_count = 0;
    irq_count++;
    if (irq_count % 500 == 0) {  // Log every 500th interrupt (~1 Hz at 500 Hz sampling)
        LOG_DBG("AFE4400 DRDY interrupt count: %u (should be ~500 Hz)", irq_count);
    }

    /* Disable interrupt during processing */
    ret = gpio_pin_interrupt_configure_dt(&config->drdy_pin, GPIO_INT_DISABLE);
    if (ret) {
        LOG_ERR("Failed to disable interrupt");
    }

    /* Submit work to system workqueue */
    k_work_submit(&data->work);
}

static void afe4400_thread_cb(const struct device *dev)
{
    struct afe4400_data *data = dev->data;
    const struct afe4400_config *config = dev->config;
    int ret;

    /* Call registered trigger handler */
    if (data->drdy_handler != NULL) {
        data->drdy_handler(dev, data->drdy_trigger);
    } else {
        LOG_ERR("DRDY Handler not set");
    }

    /* Re-enable interrupt */
    ret = gpio_pin_interrupt_configure_dt(&config->drdy_pin, GPIO_INT_EDGE_TO_ACTIVE);
    if (ret) {
        LOG_ERR("Failed to re-enable interrupt");
    }
}

static void afe4400_work_cb(struct k_work *work)
{
    struct afe4400_data *data = CONTAINER_OF(work, struct afe4400_data, work);
    afe4400_thread_cb(data->dev);
}

static int afe4400_trigger_set(const struct device *dev, const struct sensor_trigger *trig, 
                               sensor_trigger_handler_t handler)
{
    const struct afe4400_config *config = dev->config;
    struct afe4400_data *data = dev->data;
    int ret;

    if (trig->type != SENSOR_TRIG_DATA_READY) {
        return -ENOTSUP;
    }

    /* Store trigger and handler */
    data->drdy_handler = handler;
    data->drdy_trigger = trig;

    if (handler == NULL) {
        /* Disable interrupt if handler is NULL */
        return gpio_pin_interrupt_configure_dt(&config->drdy_pin, GPIO_INT_DISABLE);
    }

    /* Initialize work item */
    k_work_init(&data->work, afe4400_work_cb);

    /* Configure DRDY pin for interrupt if not already done */
    if (!device_is_ready(config->drdy_pin.port)) {
        LOG_ERR("DRDY GPIO not ready");
        return -ENODEV;
    }

    /* Set up GPIO callback */
    gpio_init_callback(&data->drdy_cb, afe4400_drdy_gpio_callback, BIT(config->drdy_pin.pin));
    ret = gpio_add_callback(config->drdy_pin.port, &data->drdy_cb);
    if (ret < 0) {
        LOG_ERR("Failed to add GPIO callback: %d", ret);
        return ret;
    }

    /* Enable interrupt */
    ret = gpio_pin_interrupt_configure_dt(&config->drdy_pin, GPIO_INT_EDGE_TO_ACTIVE);
    if (ret < 0) {
        LOG_ERR("Failed to configure interrupt: %d", ret);
        return ret;
    }

    LOG_DBG("AFE4400 trigger configured successfully");
    return 0;
}

static const struct sensor_driver_api afe4400_driver_api = {
    .sample_fetch = afe4400_sample_fetch,
    .channel_get = afe4400_channel_get,
    .trigger_set = afe4400_trigger_set,
    .submit = afe4400_submit,
};

/* Complete AFE4400 register initialization sequence - matches reference implementation */
int afe4400_init_registers(const struct device *dev)
{
    int rc;

    rc = afe4400_reg_write(dev, AFE4400_REG_CONTROL0, 0x000000);
    if (rc < 0) return rc;

    rc = afe4400_reg_write(dev, AFE4400_REG_CONTROL0, 0x000008);
    if (rc < 0) return rc;

    /* Wait for software reset to complete before writing registers */
    k_msleep(50);

    /* Samples get a net >>8 scaling downstream (after sign extension); the
     * TIA/LED settings below assume it. */
    rc = afe4400_reg_write(dev, AFE4400_REG_TIAGAIN, 0x000000); /* CF = 5pF, RF = 500kΩ (HP5 default) */
    if (rc < 0) return rc;
    
    rc = afe4400_reg_write(dev, AFE4400_REG_TIA_AMB_GAIN, 0x000001); /* Stage 2 enabled (HP5 setting) */
    if (rc < 0) return rc;
    
    /* LEDCNTRL: LED1 (IR) current in bits 15:8, LED2 (red) in bits 7:0.
     * 0x14 = 3.9 mA each, as on HealthyPi 5.
     *
     * WAS 0x005014 (IR 15.6 mA, red 3.9 mA), "deliberately asymmetric" to get
     * R into range -- but that R was an artefact of the bugs fixed in 1.0.1
     * (the conversion timing below, red/IR swapped, a 2^21 offset in the
     * M4's DC). R is AC/DC per channel, so LED current cancels out of it: set
     * currents for headroom and signal, not for R. Bench, v5, finger in,
     * 2026-11-24: 15.6 mA clipped the IR channel at some placements (pulse
     * flattened, M4 reports saturated, everything blank); 3.9 mA sat at about
     * half scale with a 3-4 % pulse on both channels. */
    rc = afe4400_reg_write(dev, AFE4400_REG_LEDCNTRL, 0x001414); /* IR (LED1) and red (LED2) 3.9 mA */
    if (rc < 0) return rc;
    
    rc = afe4400_reg_write(dev, AFE4400_REG_CONTROL2, 0x000000); /* LED_RANGE=100mA, LED=50mA */
    if (rc < 0) return rc;
    
    rc = afe4400_reg_write(dev, AFE4400_REG_CONTROL1, 0x010707); /* Timers ON, NUMAV 7 = average 8 conversions */
    if (rc < 0) return rc;
    
    /* Pulse repetition period = PPG sample period. See AFE4400_PRPCOUNT. */
    rc = afe4400_reg_write(dev, AFE4400_REG_PRPCOUNT, AFE4400_PRPCOUNT);
    if (rc < 0) return rc;

    /* Timing engine: TI's standard four-phase pattern (the AFE4400 datasheet
     * example, and what HealthyPi 5 programs), scaled to the period. Q is a
     * quarter period. Each quarter samples one phase -- ALED2, LED1, ALED1,
     * LED2 -- with the LED on for its whole quarter, and CONVERTS the phase
     * sampled in the quarter before it, after an ADC reset at the boundary.
     *
     * Before 1.0.1 this was a compressed custom pattern (87.5 us LED pulses,
     * each phase converted DURING its own sampling window, ADC resets mid-
     * pattern). The AFE converts what was held from the previous phase, so the
     * result registers did not hold the phases their names say. Bench, v5,
     * finger in, same placement, timings alternated (2026-11-24): the custom
     * pattern gave red and IR pulsing in ANTI-phase (r = -0.98) -- impossible
     * for a blood-volume pulse -- with R jumping 2.5 / 10.4; this one gave
     * r = +1.00 and R 0.66-0.68 in both rounds. */
    {
        const uint32_t Q = (AFE4400_PRPCOUNT + 1U) / 4U;
        const struct {
            uint8_t reg;
            uint32_t val;
        } t[] = {
            /* LED2 (red): sample in Q3, convert in Q0 (next period) */
            { AFE4400_REG_LED2STC,      3U * Q },
            { AFE4400_REG_LED2ENDC,     4U * Q - 2U },
            { AFE4400_REG_LED2LEDSTC,   3U * Q },
            { AFE4400_REG_LED2LEDENDC,  4U * Q - 1U },
            { AFE4400_REG_LED2CONVST,   2U },
            { AFE4400_REG_LED2CONVEND,  Q - 1U },
            /* ALED2 (ambient after red): sample in Q0, convert in Q1 */
            { AFE4400_REG_ALED2STC,     0U },
            { AFE4400_REG_ALED2ENDC,    Q - 2U },
            { AFE4400_REG_ALED2CONVST,  Q + 2U },
            { AFE4400_REG_ALED2CONVEND, 2U * Q - 1U },
            /* LED1 (IR): sample in Q1, convert in Q2 */
            { AFE4400_REG_LED1STC,      Q },
            { AFE4400_REG_LED1ENDC,     2U * Q - 2U },
            { AFE4400_REG_LED1LEDSTC,   Q },
            { AFE4400_REG_LED1LEDENDC,  2U * Q - 1U },
            { AFE4400_REG_LED1CONVST,   2U * Q + 2U },
            { AFE4400_REG_LED1CONVEND,  3U * Q - 1U },
            /* ALED1 (ambient after IR): sample in Q2, convert in Q3 */
            { AFE4400_REG_ALED1STC,     2U * Q },
            { AFE4400_REG_ALED1ENDC,    3U * Q - 2U },
            { AFE4400_REG_ALED1CONVST,  3U * Q + 2U },
            { AFE4400_REG_ALED1CONVEND, 4U * Q - 1U },
            /* ADC reset at each quarter boundary */
            { AFE4400_REG_ADCRSTSTCT0,  0U },
            { AFE4400_REG_ADCRSTENDCT0, 0U },
            { AFE4400_REG_ADCRSTSTCT1,  Q },
            { AFE4400_REG_ADCRSTENDCT1, Q },
            { AFE4400_REG_ADCRSTSTCT2,  2U * Q },
            { AFE4400_REG_ADCRSTENDCT2, 2U * Q },
            { AFE4400_REG_ADCRSTSTCT3,  3U * Q },
            { AFE4400_REG_ADCRSTENDCT3, 3U * Q },
        };

        for (size_t i = 0; i < ARRAY_SIZE(t); i++) {
            rc = afe4400_reg_write(dev, t[i].reg, t[i].val);
            if (rc < 0) return rc;
        }
    }

    /* Enable SPI_READ for register readback */
    rc = afe4400_reg_write(dev, AFE4400_REG_CONTROL0, AFE4400_CONTROL0_SPI_READ);
    if (rc < 0) return rc;

    /* Verify critical registers were written correctly */
    uint32_t ledcntrl = afe4400_reg_read(dev, AFE4400_REG_LEDCNTRL);
    uint32_t control1 = afe4400_reg_read(dev, AFE4400_REG_CONTROL1);
    uint32_t prpcount = afe4400_reg_read(dev, AFE4400_REG_PRPCOUNT);
    LOG_DBG("AFE4400 init done: LEDCNTRL=0x%06X CONTROL1=0x%06X PRPCOUNT=%u",
            ledcntrl, control1, prpcount);
    LOG_INF("AFE4400 init OK");

    return 0;
}


static int afe4400_init(const struct device *dev)
{
    struct afe4400_data *data = dev->data;
    const struct afe4400_config *cfg = dev->config;
    int rc;
    uint32_t reg_val;

     LOG_DBG("Starting AFE4400 init");

    /* Check SPI bus is ready */
    if (!spi_is_ready_dt(&cfg->spi))
    {
        LOG_ERR("SPI bus is not ready");
        return -ENODEV;
    }
    
    LOG_DBG("SPI bus ready - SPI%d at %d Hz",
            cfg->spi.bus->name[3] - '0',  // Extract SPI number
            cfg->spi.config.frequency);

    /* Initialize data structure */
    data->dev = dev;

    /* Configure reset pin if available */
    if (device_is_ready(cfg->reset_pin.port))
    {
        rc = gpio_pin_configure_dt(&cfg->reset_pin, GPIO_OUTPUT);
        if (rc < 0)
        {
            LOG_ERR("Failed to configure reset pin: %d", rc);
            return rc;
        }
        gpio_pin_set_dt(&cfg->reset_pin, 0);
        LOG_DBG("Reset pin configured");
    }

    if (device_is_ready(cfg->pdwn_pin.port))
    {
        rc = gpio_pin_configure_dt(&cfg->pdwn_pin, GPIO_OUTPUT);
        if (rc < 0)
        {
            LOG_ERR("Failed to configure powerdown pin: %d", rc);
            return rc;
        }

        /* PWDN is active LOW (datasheet) and GPIO_ACTIVE_LOW in the DT:
         * gpio_pin_set_dt(1) = LOW = powered down,
         * gpio_pin_set_dt(0) = HIGH = powered up. */

        /* Power down first */
        rc = gpio_pin_set_dt(&cfg->pdwn_pin, 1);  /* Set to active (LOW) = powered down */
        if (rc < 0)
        {
            LOG_ERR("Failed to power down device: %d", rc);
            return rc;
        }
        LOG_DBG("PWDN set ACTIVE (LOW) - device powered down");
        k_sleep(K_MSEC(10));

        /* Power up */
        rc = gpio_pin_set_dt(&cfg->pdwn_pin, 0);  /* Set to inactive (HIGH) = powered up */
        if (rc < 0)
        {
            LOG_ERR("Failed to power up device: %d", rc);
            return rc;
        }
        LOG_DBG("PWDN set INACTIVE (HIGH) - device powered up");
        k_sleep(K_MSEC(100));  /* Wait for device to power up and stabilize */

        LOG_DBG("AFE4400 power-up sequence complete");
    }

    /* Configure DRDY pin for input */
    if (device_is_ready(cfg->drdy_pin.port))
    {
        rc = gpio_pin_configure_dt(&cfg->drdy_pin, GPIO_INPUT);
        if (rc < 0)
        {
            LOG_ERR("Failed to configure DRDY pin: %d", rc);
            return rc;
        }
        LOG_DBG("DRDY pin configured");
    }

    /* Perform hardware reset to ensure clean state */
    if (device_is_ready(cfg->reset_pin.port))
    {
        LOG_DBG("Performing hardware reset...");
        gpio_pin_set_dt(&cfg->reset_pin, 1);  /* Reset is active HIGH */
        k_msleep(10);
        gpio_pin_set_dt(&cfg->reset_pin, 0);
        k_msleep(50);  /* Wait for device to come out of reset */
        LOG_DBG("Hardware reset complete");
    }

    /* Initialize AFE4400 registers with recommended settings */
    rc = afe4400_init_registers(dev);
    if (rc)
    {
        LOG_ERR("AFE4400 register initialization failed: %d", rc);
        return rc;
    }

    // Read back CONTROL0 register to verify communication
    reg_val = afe4400_reg_read(dev, AFE4400_REG_CONTROL0);
    LOG_DBG("CONTROL0 register after init: 0x%06X", reg_val);

    /* Small delay after register initialization to allow device to settle */
    k_msleep(500);

    return 0;
}

/* Blocking register access functions */
int afe4400_reg_write_blocking(const struct device *dev, uint8_t reg, uint32_t val)
{
    const struct afe4400_config *config = dev->config;
    /* AFE4400 write: Address byte (no READ bit) + 3 data bytes (MSB first) */
    uint8_t cmd[] = {reg, (uint8_t)(val >> 16), (uint8_t)(val >> 8), (uint8_t)val};

    const struct spi_buf tx_buf = {.buf = cmd, .len = sizeof(cmd)};
    const struct spi_buf_set tx = {.buffers = &tx_buf, .count = 1};
    int ret;

    ret = spi_write_dt(&config->spi, &tx);
    if (ret)
    {
        LOG_ERR("spi_write FAIL %d for reg 0x%02X = 0x%06X\n", ret, reg, val);
        return ret;
    }
    return 0;
}

uint32_t afe4400_reg_read_blocking(const struct device *dev, uint8_t reg)
{
    const struct afe4400_config *config = dev->config;
    uint8_t spiTxCommand = reg;

    uint8_t buf[3];

    const struct spi_buf tx_buf[1] = {{.buf = &spiTxCommand, .len = 1}};
    const struct spi_buf_set tx = {.buffers = tx_buf, .count = 1};
    struct spi_buf rx_buf[2] = {{.buf = NULL, .len = 1}, {.buf = buf, .len = 3}}; /* 24 bit register + 1 dummy byte */
    const struct spi_buf_set rx = {.buffers = rx_buf, .count = 2};

    spi_transceive_dt(&config->spi, &tx, &rx);

    /* Data comes in buf[0..2] (byte 0 is during address transmission) */
    uint32_t val = ((uint32_t)buf[0] << 16) | ((uint32_t)buf[1] << 8) | (uint32_t)buf[2];
    return val;
}

int afe4400_reg_write(const struct device *dev, uint8_t reg, uint32_t val)
{
    /* Currently delegates to blocking implementation
     * Future enhancement: Check if RTIO context is available and use async path
     */
    return afe4400_reg_write_blocking(dev, reg, val);
}

uint32_t afe4400_reg_read(const struct device *dev, uint8_t reg)
{
    /* Currently delegates to blocking implementation
     * Future enhancement: Check if RTIO context is available and use async path
     */
    return afe4400_reg_read_blocking(dev, reg);
}

/* AFE4400 supports SPI Mode 3: CPOL=1 (clock idle high), CPHA=1 (sample on trailing edge)
 * Verified working on HealthyPi 5 and HealthyPi 6 v2 */
#define AFE4400_SPI_OPERATION (SPI_WORD_SET(8) | SPI_TRANSFER_MSB | SPI_MODE_CPHA | SPI_MODE_CPOL)

#define AFE4400_DEFINE(inst)                                           \
    static struct afe4400_data afe4400_data_##inst;                    \
    static const struct afe4400_config afe4400_config_##inst = {       \
        .spi = SPI_DT_SPEC_INST_GET(inst, AFE4400_SPI_OPERATION, 0),   \
        .drdy_pin = GPIO_DT_SPEC_INST_GET_OR(inst, drdy_gpios, {0}),   \
        .reset_pin = GPIO_DT_SPEC_INST_GET_OR(inst, reset_gpios, {0}), \
        .pdwn_pin = GPIO_DT_SPEC_INST_GET_OR(inst, pwdn_gpios, {0}),   \
    };                                                                 \
    SENSOR_DEVICE_DT_INST_DEFINE(inst,                                 \
                                 afe4400_init,                         \
                                 NULL,                                 \
                                 &afe4400_data_##inst,                 \
                                 &afe4400_config_##inst,               \
                                 POST_KERNEL,                          \
                                 CONFIG_SENSOR_INIT_PRIORITY,          \
                                 &afe4400_driver_api);

DT_INST_FOREACH_STATUS_OKAY(AFE4400_DEFINE)
