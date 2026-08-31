/*
 * Copyright (c) 2025-2026 ProtoCentral Electronics
 * SPDX-License-Identifier: MIT
 */

/* AFE4400 register addresses and bitfields (from TI datasheet) */
#ifndef ZEPHYR_DRIVERS_SENSOR_AFE4400_REGS_H_
#define ZEPHYR_DRIVERS_SENSOR_AFE4400_REGS_H_

/* Register addresses (0x00 - 0x30) */
#define AFE4400_REG_CONTROL0       0x00

/* Timer / LED timing registers (0x01..0x1D) */
#define AFE4400_REG_LED2STC        0x01
#define AFE4400_REG_LED2ENDC       0x02
#define AFE4400_REG_LED2LEDSTC     0x03
#define AFE4400_REG_LED2LEDENDC    0x04
#define AFE4400_REG_ALED2STC       0x05
#define AFE4400_REG_ALED2ENDC      0x06
#define AFE4400_REG_LED1STC        0x07
#define AFE4400_REG_LED1ENDC       0x08
#define AFE4400_REG_LED1LEDSTC     0x09
#define AFE4400_REG_LED1LEDENDC    0x0A
#define AFE4400_REG_ALED1STC       0x0B
#define AFE4400_REG_ALED1ENDC      0x0C
#define AFE4400_REG_LED2CONVST     0x0D
#define AFE4400_REG_LED2CONVEND    0x0E
#define AFE4400_REG_ALED2CONVST    0x0F
#define AFE4400_REG_ALED2CONVEND   0x10
#define AFE4400_REG_LED1CONVST     0x11
#define AFE4400_REG_LED1CONVEND    0x12
#define AFE4400_REG_ALED1CONVST    0x13
#define AFE4400_REG_ALED1CONVEND   0x14
#define AFE4400_REG_ADCRSTSTCT0    0x15
#define AFE4400_REG_ADCRSTENDCT0   0x16
#define AFE4400_REG_ADCRSTSTCT1    0x17
#define AFE4400_REG_ADCRSTENDCT1   0x18
#define AFE4400_REG_ADCRSTSTCT2    0x19
#define AFE4400_REG_ADCRSTENDCT2   0x1A
#define AFE4400_REG_ADCRSTSTCT3    0x1B
#define AFE4400_REG_ADCRSTENDCT3   0x1C
#define AFE4400_REG_PRPCOUNT       0x1D
#define AFE4400_REG_CONTROL1       0x1E
#define AFE4400_REG_SPARE1         0x1F

/* Receiver / TIA / LED control (0x20 .. 0x29) */
#define AFE4400_REG_TIAGAIN        0x20
#define AFE4400_REG_TIA_AMB_GAIN   0x21
#define AFE4400_REG_LEDCNTRL       0x22
#define AFE4400_REG_CONTROL2       0x23
#define AFE4400_REG_SPARE2         0x24
#define AFE4400_REG_SPARE3         0x25
#define AFE4400_REG_SPARE4         0x26
#define AFE4400_REG_RESERVED1      0x27
#define AFE4400_REG_RESERVED2      0x28
#define AFE4400_REG_ALARM          0x29

/* ADC result registers (read-only) */
#define AFE4400_REG_LED2VAL        0x2A
#define AFE4400_REG_ALED2VAL       0x2B
#define AFE4400_REG_LED1VAL        0x2C
#define AFE4400_REG_ALED1VAL       0x2D
#define AFE4400_REG_LED2_ALED2     0x2E
#define AFE4400_REG_LED1_ALED1     0x2F

#define AFE4400_REG_DIAG           0x30

/* CONTROL0 bits (write-only, Address 0x00) */
#define AFE4400_CONTROL0_SW_RST        (1u << 3)
#define AFE4400_CONTROL0_DIAG_EN       (1u << 2)
#define AFE4400_CONTROL0_TIM_CNT_RST   (1u << 1)
#define AFE4400_CONTROL0_SPI_READ      (1u << 0)

/* CONTROL1 bits (Address 0x1E) */
#define AFE4400_CONTROL1_CLKALMPIN_SHIFT 9
#define AFE4400_CONTROL1_CLKALMPIN_MASK  (0x7u << AFE4400_CONTROL1_CLKALMPIN_SHIFT)
#define AFE4400_CONTROL1_TIMEREN         (1u << 8)

/* TIAGAIN (0x20) - transimpedance amplifier gain */

/* TIA_AMB_GAIN (0x21) fields */
#define AFE4400_TIA_AMB_AMBDAC_SHIFT 16
#define AFE4400_TIA_AMB_AMBDAC_MASK  (0xFu << AFE4400_TIA_AMB_AMBDAC_SHIFT)
#define AFE4400_TIA_AMB_STAGE2EN     (1u << 14)
#define AFE4400_TIA_AMB_STG2GAIN_SHIFT 8
#define AFE4400_TIA_AMB_STG2GAIN_MASK  (0x7u << AFE4400_TIA_AMB_STG2GAIN_SHIFT)
#define AFE4400_TIA_AMB_CF_LED_SHIFT 3
#define AFE4400_TIA_AMB_CF_LED_MASK  (0x1Fu << AFE4400_TIA_AMB_CF_LED_SHIFT)
#define AFE4400_TIA_AMB_RF_LED_SHIFT 0
#define AFE4400_TIA_AMB_RF_LED_MASK  (0x7u << AFE4400_TIA_AMB_RF_LED_SHIFT)

/* LEDCNTRL (0x22) */
#define AFE4400_LEDCNTRL_LEDCUROFF   (1u << 17)
#define AFE4400_LEDCNTRL_LED1_SHIFT  8
#define AFE4400_LEDCNTRL_LED1_MASK   (0xFFu << AFE4400_LEDCNTRL_LED1_SHIFT)
#define AFE4400_LEDCNTRL_LED2_SHIFT  0
#define AFE4400_LEDCNTRL_LED2_MASK   (0xFFu << AFE4400_LEDCNTRL_LED2_SHIFT)

/* CONTROL2 (0x23) bits */
#define AFE4400_CONTROL2_TXBRGMOD       (1u << 11)
#define AFE4400_CONTROL2_DIGOUT_TRISTATE (1u << 10)
#define AFE4400_CONTROL2_XTALDIS        (1u << 9)
/* Bits 7:3 reserved */
#define AFE4400_CONTROL2_PDN_TX         (1u << 2)
#define AFE4400_CONTROL2_PDN_RX         (1u << 1)
#define AFE4400_CONTROL2_PDN_AFE        (1u << 0)

/* ALARM register bits (0x29) */
#define AFE4400_ALARM_ALMPINCLKEN       (1u << 7)

/* DIAG register (0x30) flags */
#define AFE4400_DIAG_PD_ALM             (1u << 12)
#define AFE4400_DIAG_LED_ALM            (1u << 11)
#define AFE4400_DIAG_LED1OPEN           (1u << 10)
#define AFE4400_DIAG_LED2OPEN           (1u << 9)
#define AFE4400_DIAG_LEDSC              (1u << 8)
#define AFE4400_DIAG_OUTPSHGND          (1u << 7)
#define AFE4400_DIAG_OUTNSHGND          (1u << 6)
#define AFE4400_DIAG_PDOC               (1u << 5)
#define AFE4400_DIAG_PDSC               (1u << 4)
#define AFE4400_DIAG_INNSCGND           (1u << 3)
#define AFE4400_DIAG_INPSCGND           (1u << 2)
#define AFE4400_DIAG_INNSCLED           (1u << 1)
#define AFE4400_DIAG_INPSCLED           (1u << 0)

#endif /* ZEPHYR_DRIVERS_SENSOR_AFE4400_REGS_H_ */
