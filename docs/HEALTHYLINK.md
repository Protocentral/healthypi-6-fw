# HealthyLink expansion port

The physical and electrical specification of the HealthyLink expansion slots —
the Hirose DF9-31 dual-connector interface, its pin assignment, module mechanics,
and how a plugged-in module is identified before it is powered.

The firmware side (the provider registry, the interface arbiter and the fault
supervisor) is summarised in [`ARCHITECTURE.md`](ARCHITECTURE.md). For the two
modules that exist today are covered here: the [NPU compute module](#12-npu-compute-module)
and the [EEG module](#13-eeg-module).

---

## 1. Overview

### 1.1 Design Philosophy

This specification defines the HealthyLink expansion interface using **four Hirose DF9-31** board-to-board connectors (2 connectors per module slot × 2 slots = 4 total), providing 62 pins per module:
- **Low module cost**: standard 4-layer PCB, no gold-finger edge card
- **Good signal integrity**: short traces, clean routing
- **Simple manufacturing**: any PCB fab, no specialized edge plating

### 1.2 Key Features

| Feature | Specification |
|---------|---------------|
| Connector type | Hirose DF9-31P-1V (31-pin, 1.0mm pitch) |
| Connectors per module | 2 (Primary + Secondary) |
| Connectors per host | 4 (2 modules × 2 connectors each) |
| Modules supported | 2 simultaneous |
| SPI buses | 2 (SPI4, SPI6) with individual CS per slot |
| UART | USART2 with HW flow control |
| CAN interface | FDCAN1 (CAN-FD) |
| USB Host | USB OTG FS (optional) |
| I2C bus | Shared I2C for EEPROM detection |
| ADC channels | 8 analog inputs |
| GPIO pins | 4 general purpose |
| StackLink | 5 inter-module signals |
| Module control | RESET, ENABLE, IRQ, READY + 4-bit ID |

## 2. Connector Specifications

### 2.1 Hirose DF9-31 Series

| Parameter | Value |
|-----------|-------|
| Part number (receptacle) | DF9-31P-1V(32) |
| Part number (plug) | DF9-31S-1V(32) |
| Pin count | 31 |
| Pitch | 1.0mm |
| Mated height | 5.0mm (standard) |
| Current rating | 0.5A per pin |
| Voltage rating | 50V AC/DC |
| Operating temp | -55°C to +85°C |

### 2.2 Host Board Configuration

Four DF9-31P receptacles (2 per module slot, 2 slots total):

```
    ┌──────────────────────────────────────────────────────────────────────┐
    │                        HealthyPi 6 v3 Host Board                      │
    │                                                                       │
    │   Module Slot A (2 connectors)          Module Slot B (2 connectors) │
    │   ┌─────────────┐ ┌─────────────┐      ┌─────────────┐ ┌─────────────┐│
    │   │   CONN1-A   │ │   CONN2-A   │      │   CONN1-B   │ │   CONN2-B   ││
    │   │  (Primary)  │ │ (Secondary) │      │  (Primary)  │ │ (Secondary) ││
    │   │  DF9-31P    │ │  DF9-31P    │      │  DF9-31P    │ │  DF9-31P    ││
    │   └─────────────┘ └─────────────┘      └─────────────┘ └─────────────┘│
    │         ▲               ▲                    ▲               ▲        │
    │         └───────┬───────┘                    └───────┬───────┘        │
    │                 │                                    │                │
    │            Module A                             Module B              │
    │         (62 pins total)                      (62 pins total)          │
    │                                                                       │
    └──────────────────────────────────────────────────────────────────────┘
```

### 2.3 Connector Wiring Scheme

**Per-module wiring (2 connectors):**
- CONN1 (Primary): Core signals - SPI4, I2C, USART, Control, ID
- CONN2 (Secondary): Extended signals - SPI6, FDCAN, USB, ADC, GPIO, StackLink

**Inter-slot wiring (parallel for shared buses):**

| Signal Type | Wiring | Notes |
|-------------|--------|-------|
| Power (VCC_3V3, VCC_5V) | Parallel | Both slots powered together |
| Ground (GND, AGND) | Parallel | Common ground plane |
| SPI4 (SCK, MOSI, MISO) | Parallel | Shared bus |
| SPI4_CS_A | Slot A only | Individual chip select |
| SPI4_CS_B | Slot B only | Individual chip select |
| SPI6 (SCK, MOSI, MISO) | Parallel | Shared bus |
| SPI6_CS_A | Slot A only | Individual chip select |
| SPI6_CS_B | Slot B only | Individual chip select |
| I2C (SCL, SDA) | Parallel | Shared bus (0x50=A, 0x51=B) |
| USART | Parallel | Directly connected |
| FDCAN | Parallel | Shared bus (use addressing) |
| USB Host | Slot A only | Single USB host port |
| ADC channels | Parallel | Module-specific usage |
| GPIO | Parallel | Directly connected |
| StackLink | Parallel | Inter-module bus |
| Module control | Parallel | Directly connected |

---

## 3. Pin Assignment

### 3.1 Design Rationale: Two Connectors Per Module

Each module uses **two DF9-31 connectors** (62 pins total) split logically:

| Connector | Purpose | Key Signals |
|-----------|---------|-------------|
| **Primary (CONN1)** | Core communication | SPI4, I2C, USART, Module Control, Module ID |
| **Secondary (CONN2)** | Extended I/O | SPI6, FDCAN, USB, ADC, GPIO, StackLink |

**Split Rationale:**
1. **Critical signals on Primary**: The module must function with just CONN1 for basic SPI/I2C communication
2. **Optional features on Secondary**: ADC, USB Host, FDCAN, StackLink are module-specific
3. **Power distribution**: Both connectors carry power for balanced current flow
4. **Mechanical stability**: GND pins at connector edges for stable mounting

### 3.2 Connector 1 (Primary) - Pins 1-31

| Pin | Signal | STM32 Pin | Direction | Description |
|-----|--------|-----------|-----------|-------------|
| **Power** |||||
| 1 | GND | - | - | Ground (edge) |
| 2 | GND | - | - | Ground |
| 3 | VCC_3V3 | - | PWR | 3.3V supply |
| 4 | VCC_3V3 | - | PWR | 3.3V supply |
| 5 | VCC_5V | - | PWR | 5V supply |
| **SPI4 Bus (Primary)** |||||
| 6 | SPI4_SCK | PE2 | OUT | SPI4 clock (AF5) |
| 7 | SPI4_MOSI | PE6 | OUT | SPI4 master out (AF5) |
| 8 | SPI4_MISO | PE5 | IN | SPI4 master in (AF5) |
| 9 | SPI4_CS_A | PE4 | OUT | Chip select Slot A (GPIO) |
| 10 | SPI4_CS_B | PE3 | OUT | Chip select Slot B (GPIO) |
| **I2C Bus** |||||
| 11 | I2C_SCL | PH7 | OUT | I2C3 clock (AF4) |
| 12 | I2C_SDA | PH8 | I/O | I2C3 data (AF4) |
| **USART2 (with HW Flow Control)** |||||
| 13 | USART_TX | PD5 | OUT | UART transmit (AF7) |
| 14 | USART_RX | PD6 | IN | UART receive (AF7) |
| 15 | USART_RTS | PD4 | OUT | Request to send (AF7) |
| 16 | USART_CTS | PD3 | IN | Clear to send (AF7) |
| **Module Control** |||||
| 17 | MOD_RESET_N | PI3 | OUT | Module reset (active low) |
| 18 | MOD_ENABLE | PI6 | OUT | Module enable / PWDN |
| 19 | MOD_IRQ_N | PH6 | IN | Module interrupt / DRDY (EXTI) |
| 20 | MOD_READY | PI2 | IN | Module ready / START |
| **Module Identification** |||||
| 21 | MOD_ID_0 | PI7 | IN | Module ID bit 0 (LSB) |
| 22 | MOD_ID_1 | PI8 | IN | Module ID bit 1 |
| 23 | MOD_ID_2 | PI9 | IN | Module ID bit 2 |
| 24 | MOD_ID_3 | PI10 | IN | Module ID bit 3 (MSB) |
| **Power (Edge)** |||||
| 25 | GND | - | - | Ground |
| 26 | GND | - | - | Ground |
| 27 | VCC_3V3 | - | PWR | 3.3V supply |
| 28 | GND | - | - | Ground (edge) |
| 29 | AGND | - | - | Analog ground |
| 30 | AGND | - | - | Analog ground |
| 31 | VCC_5V | - | PWR | 5V supply |

### 3.3 Connector 2 (Secondary) - Pins 32-62

| Pin | Signal | STM32 Pin | Direction | Description |
|-----|--------|-----------|-----------|-------------|
| **SPI6 Bus (Secondary)** |||||
| 32 | SPI6_SCK | PG13 | OUT | SPI6 clock (AF5) |
| 33 | SPI6_MOSI | PG14 | OUT | SPI6 master out (AF5) |
| 34 | SPI6_MISO | PG12 | IN | SPI6 master in (AF5) |
| 35 | SPI6_CS_A | PA15 | OUT | Chip select Slot A (GPIO) |
| 36 | SPI6_CS_B | PG11 | OUT | Chip select Slot B (GPIO) |
| **FDCAN1 (CAN-FD)** |||||
| 37 | FDCAN_TX | PH13 | OUT | CAN-FD transmit (AF9) |
| 38 | FDCAN_RX | PH14 | IN | CAN-FD receive (AF9) |
| **USB Host (Optional)** |||||
| 39 | USB_FS_DM | PA11 | I/O | USB Full-Speed D- (AF10) |
| 40 | USB_FS_DP | PA12 | I/O | USB Full-Speed D+ (AF10) |
| 41 | GND | - | - | Ground (USB reference) |
| **ADC Channels** |||||
| 42 | ADC_CH0 | PA1 | IN | ADC1_INP1 |
| 43 | ADC_CH1 | PA2 | IN | ADC1_INP2 |
| 44 | ADC_CH2 | PA3 | IN | ADC1_INP3 |
| 45 | ADC_CH3 | PA4 | IN | ADC1_INP4 |
| 46 | ADC_CH4 | PA5 | IN | ADC1_INP5 |
| 47 | ADC_CH5 | PA6 | IN | ADC1_INP6 |
| 48 | ADC_CH6 | PA7 | IN | ADC1_INP7 |
| 49 | ADC_CH7 | PC4 | IN | ADC1_INP8 |
| 50 | AGND | - | - | Analog ground |
| **GPIO** |||||
| 51 | GPIO_0 | PI12 | I/O | General purpose (EXTI) |
| 52 | GPIO_1 | PI13 | I/O | General purpose (EXTI) |
| 53 | GPIO_2 | PI14 | I/O | General purpose (EXTI) |
| 54 | GPIO_3 | PI15 | I/O | General purpose (EXTI) |
| **StackLink Inter-Module Bus** |||||
| 55 | SL_CLK | PI0 | OUT | StackLink clock |
| 56 | SL_MOSI | PI1 | OUT | StackLink data out |
| 57 | SL_MISO | PI11 | IN | StackLink data in |
| 58 | SL_SYNC | PI4 | OUT | StackLink sync pulse |
| 59 | SL_IRQ_N | PI5 | IN | StackLink interrupt (EXTI) |
| **Power (Edge)** |||||
| 60 | VCC_3V3 | - | PWR | 3.3V supply |
| 61 | GND | - | - | Ground |
| 62 | GND | - | - | Ground (edge) |

### 3.4 Mounting Pads

| Pad | Description |
|-----|-------------|
| MP1 | CONN1 mounting pad (left) |
| MP2 | CONN1 mounting pad (right) |
| MP3 | CONN2 mounting pad (left) |
| MP4 | CONN2 mounting pad (right) |

### 3.5 Pin Count Summary (Per Module)

| Function | Pins 1-31 | Pins 32-62 | Total |
|----------|-----------|------------|-------|
| VCC_3V3 | 3 | 1 | 4 |
| VCC_5V | 2 | 0 | 2 |
| GND | 5 | 3 | 8 |
| AGND | 2 | 1 | 3 |
| SPI4 | 5 | 0 | 5 |
| SPI6 | 0 | 5 | 5 |
| I2C | 2 | 0 | 2 |
| USART | 4 | 0 | 4 |
| FDCAN | 0 | 2 | 2 |
| USB | 0 | 2 | 2 |
| ADC | 0 | 8 | 8 |
| Module Control | 4 | 0 | 4 |
| Module ID | 4 | 0 | 4 |
| GPIO | 0 | 4 | 4 |
| StackLink | 0 | 5 | 5 |
| **Signal Pins** | **31** | **31** | **62** |
| **Mounting Pads** | 2 | 2 | 4 |
| **Total Pads** | **33** | **33** | **66** |

---

## 4. Module Design

### 4.1 Module PCB

| Parameter | Value |
|-----------|-------|
| Connectors | 2× DF9-31S-1V plugs (CONN1 + CONN2) |
| Connector spacing | 18mm center-to-center (asymmetric) |
| PCB thickness | 1.6mm standard |
| Layers | 4 (recommended) |
| Finish | HASL or ENIG (standard) |
| Dimensions | 50mm × 55mm |

### 4.2 Orientation Keying

DF9 connectors are symmetric and can be inserted in either orientation. To prevent wrong insertion, use **asymmetric connector placement**:

```
                        50 mm
    ◄───────────────────────────────────────────────────►

    ┌───────────────────────────────────────────────────┐
    │                                                   │
    │   12mm    ┌─────────┐  18mm   ┌─────────┐  8mm   │
    │  ◄────►   │  CONN1  │ ◄────► │  CONN2  │ ◄───►  │
    │           │ Primary │        │Secondary│         │
    │           └─────────┘        └─────────┘         │
    │                                                   │
    └───────────────────────────────────────────────────┘

    CONN1 center: 12mm + 15.5mm = 17.5mm from left edge
    CONN2 center: 17.5mm + 18mm = 35.5mm from left edge

    Asymmetric: 17.5mm left vs 14.5mm right (CONN2 to edge)

    If module is reversed (180°):
    - Host CONN1-A would align with Module CONN2 position → NO MATE
    - Different offset prevents insertion
```

**Additional Keying Methods:**
1. **Asymmetric mounting holes**: Use M2.5 holes at different distances from corners
2. **Silkscreen arrow**: Mark "THIS SIDE UP" on module
3. **Chamfered corner**: Remove one corner for visual orientation

### 4.3 Module EEPROM

Each module includes an I2C EEPROM for identification:

| Slot | EEPROM Address | Part |
|------|----------------|------|
| Slot A | 0x50 | 24AA02 |
| Slot B | 0x51 | 24AA02 (A0=1) |

EEPROM format matches the HealthyLink specification (magic "HLNK", module ID, capabilities).

### 4.4 Example Module Block Diagram

```
┌───────────────────────────────────────────────────────────────────┐
│                         EEG Module                                 │
│                                                                    │
│  ┌──────────────┐     ┌──────────────┐     ┌──────────┐          │
│  │  Electrode   │     │   ADS1299    │     │  24AA02  │          │
│  │   Header     │────►│  8-ch AFE    │     │  EEPROM  │          │
│  │  (10-pin)    │     │              │     └────┬─────┘          │
│  └──────────────┘     └──────┬───────┘          │                │
│                              │                   │                │
│                         SPI4 │              I2C  │                │
│                              │                   │                │
│  ┌───────────────────────────┴───────────────────┴────────────┐  │
│  │                                                             │  │
│  │   ┌─────────────────┐           ┌─────────────────┐        │  │
│  │   │   DF9-31S       │           │   DF9-31S       │        │  │
│  │   │   CONN1         │           │   CONN2         │        │  │
│  │   │   (Primary)     │           │   (Secondary)   │        │  │
│  │   │   SPI4, I2C,    │           │   ADC, GPIO,    │        │  │
│  │   │   USART, Ctrl   │           │   StackLink     │        │  │
│  │   └─────────────────┘           └─────────────────┘        │  │
│  │                                                             │  │
│  └─────────────────────────────────────────────────────────────┘  │
└───────────────────────────────────────────────────────────────────┘
```

**Connector Usage:**
- **CONN1 (Primary)**: SPI4 for ADS1299 data, I2C for EEPROM, USART (unused), Module Control
- **CONN2 (Secondary)**: ADC (unused on this module), GPIO (DRDY mapped), StackLink (optional)

---

## 5. Signal Routing Guidelines

### 5.1 SPI Bus

- Route SCK, MOSI, MISO as parallel traces
- Length match to within 5mm
- 50Ω impedance (if >25 MHz)
- Place decoupling capacitors near connector

### 5.2 Analog Signals

- Separate AGND from digital GND at module
- Star-ground at host connector
- Guard ADC traces with ground
- Keep analog traces away from SPI

### 5.3 Power Distribution

```
Host VCC_3V3 ──┬── 100nF ──┬── Slot A (pins 3,4)
               │           │
               │           └── Slot B (pins 3,4)
               │
               └── 10µF bulk
```

---

## 6. Connector Rationale

Why a dual-DF9 board-to-board interface:

- **Cost-sensitive modules**: standard PCBs with no gold-finger requirement keep
  module cost low
- **Standard biosignal modules**: EEG, ECG, EMG with SPI AFEs fit the 62-pin
  budget with headroom
- **Prototyping-friendly**: any PCB fab can build a module
- **Signal integrity**: short traces with controlled impedance

---

## 7. Bill of Materials

### 7.1 Host Side (per module slot)

| Part | Qty | Part Number | Description |
|------|-----|-------------|-------------|
| J1, J2 | 2 | DF9-31P-1V(32) | Receptacle, SMD (Primary + Secondary) |
| C1-C4 | 4 | 100nF 0402 | Decoupling (2 per connector) |
| C5 | 1 | 10µF 0805 | Bulk capacitor |

**Total for 2 slots:** 4× DF9-31P receptacles, 8× 100nF, 2× 10µF

### 7.2 Module Side

| Part | Qty | Part Number | Description |
|------|-----|-------------|-------------|
| J1, J2 | 2 | DF9-31S-1V(32) | Plug, SMD (Primary + Secondary) |
| U1 | 1 | 24AA02 | I2C EEPROM |
| C1-C4 | 4 | 100nF 0402 | Decoupling (2 per connector) |

---

## 8. Mechanical Layout

### 8.1 Dual Module Configuration

Two HealthyLink modules mount side-by-side on the 120×70mm host board:

```
                              120 mm (Host Board)
◄──────────────────────────────────────────────────────────────────────────────────►

┌──────────────────────────────────────────────────────────────────────────────────┐  ▲
│                           HealthyPi 6 v3 Host Board                               │  │
│                                                                                   │  │
│  5mm  Module Slot A (62 pins)        10mm       Module Slot B (62 pins)     5mm  │  │
│ edge  ┌───────────┐  ┌───────────┐   gap   ┌───────────┐  ┌───────────┐   edge  │  │
│       │  CONN1-A  │  │  CONN2-A  │         │  CONN1-B  │  │  CONN2-B  │         │  │
│       │  Primary  │  │ Secondary │         │  Primary  │  │ Secondary │         │  │
│       │ (DF9-31P) │  │ (DF9-31P) │         │ (DF9-31P) │  │ (DF9-31P) │         │  │  70mm
│       └─────┬─────┘  └─────┬─────┘         └─────┬─────┘  └─────┬─────┘         │  │
│             │   5mm        │                     │   5mm        │                │  │
│       ┌─────┴──────────────┴─────┐         ┌─────┴──────────────┴─────┐         │  │
│       │      HealthyLink         │         │      HealthyLink         │         │  │
│       │      Module A            │         │      Module B            │         │  │
│       │      50 × 55 mm          │         │      50 × 55 mm          │         │  │
│       │  ○                    ○  │         │  ○                    ○  │         │  │
│       └──────────────────────────┘         └──────────────────────────┘         │  │
│       │◄──────── 50mm ─────────►│         │◄──────── 50mm ─────────►│         │  ▼
│                                                                                   │
│       5mm + 50mm + 10mm + 50mm + 5mm = 120mm ✓                                   │
└──────────────────────────────────────────────────────────────────────────────────┘
```

### 8.2 Module Dimensions

| Parameter | Value | Notes |
|-----------|-------|-------|
| **Host board** | 120 × 70 mm | HealthyPi 6 v3 main board |
| **Module width** | 50 mm | Fits 2 modules + gap in 120mm |
| **Module length** | 55 mm | Leaves 15mm routing on host |
| PCB thickness | 1.6 mm | Standard 4-layer |
| Mated height | 5.0 mm | DF9-31 standard stack height |
| Connector spacing | 18 mm | Center-to-center (asymmetric) |
| Edge clearance | 5 mm | Each side of host board |
| Inter-module gap | 10 mm | Between Module A and B |
| **Total module area** | 110 × 55 mm | Both modules + gap |

### 8.3 Host Board Layout

```
                                   120 mm
    ◄──────────────────────────────────────────────────────────────────────►

    ┌──────────────────────────────────────────────────────────────────────┐  ▲
    │  ○ M3                                                        M3 ○    │  │
    │                                                                      │  │
    │   5mm   Slot A (Module 1)         10mm      Slot B (Module 2)  5mm  │  │
    │  edge   ┌───────┐ ┌───────┐       gap      ┌───────┐ ┌───────┐ edge │  │
    │         │CONN1-A│ │CONN2-A│                │CONN1-B│ │CONN2-B│      │  │
    │         │Primary│ │Second.│                │Primary│ │Second.│      │  │  70mm
    │         └───────┘ └───────┘                └───────┘ └───────┘      │  │
    │            ▲          ▲                       ▲          ▲          │  │
    │         17.5mm     35.5mm                  67.5mm     85.5mm        │  │
    │         (from left edge - asymmetric placement)                     │  │
    │                                                                      │  │
    │        Routing Area                        Routing Area             │  │
    │  ○ M3                                                        M3 ○    │  │
    └──────────────────────────────────────────────────────────────────────┘  ▼

    Connector X positions (asymmetric for keying):
    CONN1-A: 5 + 12.5 = 17.5mm    CONN1-B: 60 + 7.5 = 67.5mm
    CONN2-A: 17.5 + 18 = 35.5mm   CONN2-B: 67.5 + 18 = 85.5mm
```

### 8.4 Mounting Holes

**Host Board (120 × 70 mm):**

| Hole | Position (X, Y) | Size | Purpose |
|------|-----------------|------|---------|
| H1 | (5, 5) mm | M3 | Corner mount |
| H2 | (115, 5) mm | M3 | Corner mount |
| H3 | (5, 65) mm | M3 | Corner mount |
| H4 | (115, 65) mm | M3 | Corner mount |

Module mounting uses friction-fit DF9 connectors (30+ mating cycles rated).

### 8.5 Connector Placement Details (Asymmetric for Keying)

The connectors are placed **asymmetrically** to prevent wrong-orientation insertion:

**Slot A (Left Module - 2 connectors):**
| Connector | Center X | Center Y | Offset from Module Edge |
|-----------|----------|----------|-------------------------|
| CONN1-A (Primary) | 17.5 mm | 45 mm | 12.5mm from left |
| CONN2-A (Secondary) | 35.5 mm | 45 mm | 14.5mm from right |

**Slot B (Right Module - 2 connectors):**
| Connector | Center X | Center Y | Offset from Module Edge |
|-----------|----------|----------|-------------------------|
| CONN1-B (Primary) | 67.5 mm | 45 mm | 7.5mm from slot left |
| CONN2-B (Secondary) | 85.5 mm | 45 mm | 24.5mm from slot right |

**Keying Verification:**
- CONN1 offset from left edge: 12.5mm (Module) vs 14.5mm if reversed → NO MATE
- If module is inserted 180° rotated, connectors don't align with host receptacles

### 8.6 Module PCB Template

```
                              50 mm
    ◄─────────────────────────────────────────────────────────►

    ┌─────────────────────────────────────────────────────────┐  ▲
    │  ○ M2.5                                        M2.5 ○   │  │
    │                                                         │  │
    │  12.5mm  ┌───────────────┐  18mm  ┌───────────────┐     │  │
    │  ◄────► │   DF9-31S     │ ◄────► │   DF9-31S     │     │  │
    │          │   CONN1       │        │   CONN2       │     │  │
    │          │  (Primary)    │        │ (Secondary)   │     │  │  55 mm
    │          │   bottom      │        │   bottom      │     │  │
    │          └───────────────┘        └───────────────┘     │  │
    │                                                  14.5mm │  │
    │            ┌──────────────────────┐             ◄────► │  │
    │            │       AFE IC         │                     │  │
    │            │    (ADS1299 etc.)    │                     │  │
    │            └──────────────────────┘                     │  │
    │                                                         │  │
    │  ○ M2.5       [Electrode Header]               M2.5 ○   │  │
    └─────────────────────────────────────────────────────────┘  ▼

    CONN1 center: 12.5 + 15.5 = 17.5mm from left
    CONN2 center: 17.5 + 18 = 35.5mm from left (14.5mm from right)

    ASYMMETRIC: 12.5mm left gap ≠ 14.5mm right gap → KEYED
```

**Module Mounting Holes (Asymmetric for additional keying):**

| Hole | Position (X, Y) | Size | Notes |
|------|-----------------|------|-------|
| H1 | (5, 5) mm | M2.5 | Front-left |
| H2 | (45, 5) mm | M2.5 | Front-right |
| H3 | (5, 50) mm | M2.5 | Rear-left |
| H4 | (42, 50) mm | M2.5 | Rear-right (offset 3mm) |

The asymmetric H4 hole (42mm vs 45mm) provides additional mechanical keying.

### 8.7 Clearance Requirements

| Zone | Minimum | Notes |
|------|---------|-------|
| Component height (top) | 8 mm | Above host board surface |
| Component height (bottom) | 3 mm | Below module PCB (mated space) |
| Inter-module gap | 10 mm | Edge-to-edge between modules |
| Connector-to-connector gap | 2.5 mm | Between CONN1 and CONN2 edges |
| Edge clearance | 3 mm | From module edge to components |
| DF9 keep-out | 2 mm | Around each connector footprint |
| Electrode header zone | 10 × 40 mm | Far edge of module |

### 8.8 Stacking Considerations

For applications requiring more than 2 modules, use a **HealthyLink Hub** board:

```
    HealthyPi 6 v3
         │
    DF9 Dual Interface
         │
         ▼
┌─────────────────────────────────────────┐
│          HealthyLink Hub                │
│   ┌─────┐  ┌─────┐  ┌─────┐  ┌─────┐  │
│   │Mod 1│  │Mod 2│  │Mod 3│  │Mod 4│  │
│   └─────┘  └─────┘  └─────┘  └─────┘  │
└─────────────────────────────────────────┘
```

The Hub contains SPI multiplexing logic (e.g., 74LVC1G3157) to extend chip selects.

---

## 9. KiCad Library

> The library files themselves are not distributed in this repository; what
> follows is the symbol and footprint definition needed to recreate them.

### 9.1 Symbol

The KiCad symbol is defined as:
```
hardware/symbols/healthylink_df9_connector.kicad_sym
```

**Symbol:** `HEALTHYLINK_DF9_DUAL`

**Pin Numbering:**
- Connector 1 (Primary): Pins 1 through 31
- Connector 2 (Secondary): Pins 32 through 62
- Mounting Pads: MP1, MP2, MP3, MP4

**Usage:** Place one symbol per module slot. The CS_A and CS_B variants route to Slot A and Slot B respectively.

### 9.2 Footprint (Host Board)

The KiCad footprint for the host board module slot is available at:
```
hardware/footprints/healthylink_df9_module_slot.kicad_mod
```

**Footprint:** `HEALTHYLINK_DF9_MODULE_SLOT`

**Features:**
- Module outline (50 × 55mm) on silkscreen
- Two DF9-31P receptacle pad arrays (CONN1, CONN2)
- Asymmetric connector placement for keying (12.5mm / 14.5mm)
- Four mounting holes (H1-H4) with H4 offset for additional keying
- Pin 1 markers and orientation arrow
- Courtyard with 1mm clearance

**Footprint Layout:**
```
    ┌─────────────────────────────────────────────────────────┐
    │  ○H1                                              H2○   │
    │  →PIN1                                                  │
    │     ┌─────────────────────┐  ┌─────────────────────┐   │
    │     │   CONN1 (Primary)   │  │  CONN2 (Secondary)  │   │
    │     │   Pads 1.1 - 1.31   │  │   Pads 2.1 - 2.31   │   │
    │     └─────────────────────┘  └─────────────────────┘   │
    │     ●pin1                    ●pin1                      │
    │  - - - - - - - - - - - - - - - - - - - - - - - - - - - │
    │                                                         │
    │                    MODULE AREA                          │
    │                    50 x 55 mm                           │
    │                                                         │
    │  ○H3                                           *H4○     │
    └─────────────────────────────────────────────────────────┘
                                              *Asymmetric (42mm vs 45mm)
```

---

## 10. STM32H757 pin mapping summary

### A.1 Communication Interfaces

| Interface | Signal | STM32 Pin | Alternate Function |
|-----------|--------|-----------|-------------------|
| **SPI4** | SCK | PE2 | AF5 |
| | MOSI | PE6 | AF5 |
| | MISO | PE5 | AF5 |
| | CS_A | PE4 | GPIO |
| | CS_B | PE3 | GPIO |
| **SPI6** | SCK | PG13 | AF5 |
| | MOSI | PG14 | AF5 |
| | MISO | PG12 | AF5 |
| | CS_A | PA15 | GPIO |
| | CS_B | PG11 | GPIO |
| **I2C3** | SCL | PH7 | AF4 |
| | SDA | PH8 | AF4 |
| **USART2** | TX | PD5 | AF7 |
| | RX | PD6 | AF7 |
| | RTS | PD4 | AF7 |
| | CTS | PD3 | AF7 |
| **FDCAN1** | TX | PH13 | AF9 |
| | RX | PH14 | AF9 |
| **USB_FS** | DM | PA11 | AF10 |
| | DP | PA12 | AF10 |

### A.2 Control and GPIO

| Function | Signal | STM32 Pin | Notes |
|----------|--------|-----------|-------|
| **Module Control** | RESET_N | PI3 | Active low |
| | ENABLE | PI6 | PWDN |
| | IRQ_N | PH6 | EXTI, active low |
| | READY | PI2 | START |
| **Module ID** | ID_0 | PI7 | LSB |
| | ID_1 | PI8 | |
| | ID_2 | PI9 | |
| | ID_3 | PI10 | MSB |
| **StackLink** | CLK | PI0 | 20 MHz clock |
| | MOSI | PI1 | Data out |
| | MISO | PI11 | Data in |
| | SYNC | PI4 | Sync pulse |
| | IRQ_N | PI5 | Interrupt |
| **GPIO** | GPIO_0 | PI12 | EXTI capable |
| | GPIO_1 | PI13 | EXTI capable |
| | GPIO_2 | PI14 | EXTI capable |
| | GPIO_3 | PI15 | EXTI capable |

### A.3 ADC Channels

| Signal | STM32 Pin | ADC Channel |
|--------|-----------|-------------|
| ADC_CH0 | PA1 | ADC1_INP1 |
| ADC_CH1 | PA2 | ADC1_INP2 |
| ADC_CH2 | PA3 | ADC1_INP3 |
| ADC_CH3 | PA4 | ADC1_INP4 |
| ADC_CH4 | PA5 | ADC1_INP5 |
| ADC_CH5 | PA6 | ADC1_INP6 |
| ADC_CH6 | PA7 | ADC1_INP7 |
| ADC_CH7 | PC4 | ADC1_INP8 |

---

---

## 11. Module detection

### Summary

- **Detection is EEPROM-based only — there is no MOD_PRESENT / detect GPIO.**
- Each slot has an **ID EEPROM** (24AAxx) on a **shared I²C bus** (I²C3),
  strapped to a **distinct address per slot** (slot A = `0x50`, slot B = `0x51`,
  …) so the address identifies *which slot*, and the EEPROM contents identify
  *which module*.
- Each slot's EEPROM is on an **always-on rail** (not the switched module
  supply), enabling **identify-before-power**.
- Each slot has a **load switch enable** (`EN_MOD_x`) and a **fault** input
  (`MOD_x_FLT`) for safe, sequenced power-up.

### Per-slot resources

| Function | Signal | Owner | v4 pins | Notes |
|---|---|---|---|---|
| Module power enable | `EN_MOD_x` | MCU GPIO → baseboard load switch | A=PI1, B=PH15 | active-high |
| Load-switch fault | `MOD_x_FLT` | baseboard → MCU GPIO | A=PH11, B=PI4 | active-low, pulled up |
| ID EEPROM | I²C3 @ slot address | shared I²C3 (PH7/PH8) | A=0x50 (B=0x51 on v5) | 24AAxx, **always-on rail** |
| EEPROM addr strap | `A0` (per slot) | baseboard strap via connector | — (v5) | GND→0x50, VCC→0x51 |
| Functional bus | SPI/etc. | per module type | per module | e.g. EEG ADS1299 on SPI4 |

**No detect GPIO** — `PI5` (v3 MOD_PRESENT) and `PI0` (v3 StackLink) are freed.

### Why always-on EEPROM (not switched module power)

1. **Identify before power** — read module ID / HW rev / FW-compat with the load
   switch off, then power only recognized, compatible modules.
2. **Clean bus** — EEPROM shares the always-on domain with the I²C pull-ups, so
   there's no power-gated-device-on-an-always-on-bus back-power hazard.
3. **Pin-free hot-plug** — insertion/removal is observable by polling the
   EEPROM addresses, with no detect pin and without energizing module power.

### Connector pin additions for v5

Addressed-EEPROM mode costs **zero new MCU GPIOs**. Connector/baseboard adds:

| Pin | Purpose |
|---|---|
| `EEPROM_VCC_AON` | always-on rail for the module ID EEPROM (off existing 3V3 AON; ~1 µA) |
| `A0` (per slot) | EEPROM address strap, set per slot on the baseboard (GND/VCC) |

Route `A0`+`A1` instead of just `A0` to scale past 2 slots (≤4 with two lines,
≤8 with three). The module's ID EEPROM must be an **address-strappable 24AAxx
variant** (A0 pin available), not a fixed-0x50 part, and routes its EEPROM
`VCC` to `EEPROM_VCC_AON`.

### Enumeration sequence (identify-then-power)

For each slot (distinct EEPROM address on the shared bus):

```
1. probe EEPROM (I2C ACK at slot address)      # always-on; no power applied
     - NAK  -> slot empty; continue
2. read + validate header (magic/version/id)   # identity, still unpowered
3. if NOT recognized/compatible:
     leave EN_MOD off; report unsupported; continue
4. assert EN_MOD_x; settle (power-up)          # now apply module power
5. if MOD_x_FLT asserted:
     de-assert EN_MOD_x; mark FAULTED; continue
6. configure pinmux for module type; dispatch to module driver
```

The address answers "which slot", the EEPROM contents answer "which module",
the load switch + fault give safe power-up — all with no detect GPIO.

### Hardware watch-outs

- Put I²C3 **pull-ups on the always-on domain** (same as `EEPROM_VCC_AON`).
- With the EEPROM on the always-on rail the shared-bus back-power hazard does
  not arise. (It would if the EEPROM were on the switched rail and another
  slot kept the bus active — that's the v4 caveat below.)
- The address strap references the always-on rail (A0 → AON-GND / AON-VCC).

### v4 vs v5

| | v4 (current) | v5 (this spec) |
|---|---|---|
| EEPROM rail | switched (module power) | **always-on** |
| Slot addressing | single EEPROM @0x50 (slot A only) | per-slot distinct addr (0x50/0x51…) |
| Slots wired | A (EN/FLT/EEPROM), B (EN/FLT only) | A + B fully (EN/FLT/addressed EEPROM) |
| Detection order | power-then-probe (EEPROM switched) | identify-then-power |

Because the v4 EEPROM is switched, the driver also supports a **power-then-retry
fallback** (probe → if NAK, enable EN_MOD, settle, re-probe) so the same code
works on v4 and v5.

### Devicetree / driver mapping

- Slot binding: `dts/bindings/misc/protocentral,healthylink-slot.yaml`
  (`power-gpios`, `fault-gpios`, `eeprom` phandle, `slot-label`; **no detect-gpios**).
- Slot nodes: `healthylink_slot_a` / `healthylink_slot_b` in the v4 board dtsi.
- v5 DT change: add slot B's EEPROM node `@0x51` on I²C3 (deferred-init or raw
  I²C via the slot `eeprom` phandle so the `at24` driver doesn't probe it at
  boot), point `healthylink_slot_b.eeprom` at it.
- Driver: `drivers/misc/healthylink/healthylink_core.c` — `healthylink_detect()`
  implements EEPROM-only detection with the identify-then-power sequence and the
  per-slot power/fault + power-then-retry fallback. Multi-slot enumeration
  (iterating all `healthylink-slot` nodes) lands with the v5 slot-B EEPROM.

---

## 12. NPU compute module

The STM32N657 compute module: a Cortex-M55 with a Neural-ART NPU, used to run
inference next to the signal source rather than on the host.

> ### The module's firmware is in a separate repository
>
> **[`Protocentral/healthylink-compute-fw`](https://github.com/Protocentral/healthylink-compute-fw)**
>
> It moved out of this repository on **2026-08-31**, taking
> `app_healthylink_compute/`, `boards/protocentral/healthylink_compute/` and
> `scripts/npu/` with it. The reason is licensing, not tidiness: the module's
> firmware vendors STMicroelectronics' ATON NPU runtime under **ST SLA0104**, a
> proprietary agreement whose clause 5 forbids redistribution "in any manner
> that would subject the SOFTWARE PACKAGE to any Open Source Terms" — and the
> agreement names MIT explicitly. This repository declares MIT. The two could not
> both be true in one tree.
>
> What went with it, and where to find it now:
>
> | Was here | Now |
> |---|---|
> | `app_healthylink_compute/` | `app/` in the new repository |
> | `boards/protocentral/healthylink_compute/` | same path there |
> | `scripts/npu/` | `scripts/` there; the `scripts/build.sh npu` and `scripts/flash.sh npu` dispatchers are gone |
> | the module build/flash/self-install detail that used to fill this section | [`docs/MODULE_REFERENCE.md`](https://github.com/Protocentral/healthylink-compute-fw/blob/main/docs/MODULE_REFERENCE.md) there |
> | — | [`docs/SPI_PROTOCOL.md`](https://github.com/Protocentral/healthylink-compute-fw/blob/main/docs/SPI_PROTOCOL.md) — **new**, the wire contract, written from both implementations |
>
> What remains below is the **host** side: what the M7 does with a compute
> module, which is all that is implemented in this repository.

### 12.1 Status

The link is alive; the inference is not.

| | |
|---|---|
| Detection → match → claim → ACTIVE | ✅ validated on v5 |
| SPI4 link, `HLNK` proof-of-life echo | ✅ validated on v5 (2026-06-23) |
| `GET_INFO` reply alignment | 🟡 N657 slave timing still being tuned |
| `LOAD_INPUT` → `RUN_INFERENCE` → IRQ → `READ_OUTPUT` | implemented both ends, never demonstrated end to end |
| The inference itself | 🔴 **a stub on the module** — `RUN_INFERENCE` returns five zero bytes |

Nothing in this repository should be read as classifying a beat today.

### 12.2 What the M7 side consists of

| File | Role |
|---|---|
| `app_m7/src/healthylink/mod_npu.c` | the HealthyLink provider: detects, claims SPI4, runs the comms check, drives the inference sequence |
| `drivers/misc/healthylink/healthylink_compute.c` | the `protocentral,healthylink-compute` driver |
| `include/healthylink/healthylink_compute_arrhythmia.h` | `healthylink_compute_arrhythmia_available()` / `_init()` / `healthylink_compute_classify_arrhythmia()` |
| `boards/protocentral/healthypi6_v5/healthylink-compute.overlay` | the SPI4 child node, IRQ, reset and PWDN lines (added to the default build by `scripts/build.sh m7`) |

Module identification is the ordinary HealthyLink mechanism of section 11: the
slot EEPROM reports magic `HLNK` and module ID **`0x0005`**, and the compute
node is `zephyr,deferred-init` so it is only brought up when that ID matches.

### 12.3 Host wiring

From the v5 overlay:

| Signal | H7 pin | Note |
|---|---|---|
| SPI4 SCK / MISO / MOSI | PE2 / PE5 / PE6 | |
| SPI4 CS | **PE4** (slot A; PE3 for slot B) | SPI4 is `st,soft-nss` with **no `cs-gpios`** — `mod_npu.c` drives PE4 as a plain GPIO. Routing it through `spi_config.cs.gpio` faults. |
| IRQ from module | PI12, active-low, pull-up | slot A |
| MOD_RESET | PI2 (slot A) / PI5 (slot B) | |
| PWDN | PI6 | shared with the EEG module's line; only one module occupies a slot |

**Do not touch SPI6 while SPI4 is in use** — it wedges the next SPI4 transceive.

The comms check is **off by default** (`CONFIG_HPI_NPU_COMMS_CHECK=n`): a
faulting transceive resets the M7 into a boot loop. Detection, claim and ACTIVE
all work without it. The SPI4 loopback diagnostic
(`CONFIG_HPI_SPI4_LOOPBACK_TEST`) is kept for hardware debugging.

### 12.4 The wire contract

Two sets of constants, in two repositories, that must agree — `NPU_SPI_FRAME_SIZE`
and the command codes in `app_m7/src/healthylink/mod_npu.c` here, and
`app/src/spi_slave_handler.h` there. **A change to one side does not fail to
build; it fails on the wire, and it looks like bad hardware.**

The essentials, so this section stands alone:

- **Fixed 256-byte frames.** Every CS transaction clocks exactly
  `NPU_SPI_FRAME_SIZE` = 256 bytes both ways. Not a maximum — the length. The
  STM32 SPIv2 slave completes on EOT (TSIZE), not on NSS deassert, so a
  short-clocked transaction hangs the slave.
- **Replies are one transaction late.** The module is a software slave: it parses
  the command after the frame has been clocked and stages the reply for the
  *next* one. The host waits `NPU_TURNAROUND_MS` = 50 ms in between; reading
  sooner returns all-zero MISO.
- **The alive signature is the proof-of-life.** After any command that produces
  no response, the module re-arms its TX buffer with
  `'H' 'L' 'N' 'K' 0x01 0x00 0x00 0x05` — magic, protocol major/minor, module ID.
- **The slave can emit leading underrun bytes**, so the host **scans** the
  returned frame for the command marker rather than assuming offset 0.
- Commands: `0x00` NOP, `0x01` STATUS, `0x02` RESET, `0x10` LOAD_INPUT,
  `0x20` RUN_INFERENCE, `0x30` READ_OUTPUT, `0xF0` GET_INFO. Status bits: 0
  READY, 1 BUSY, 2 ERROR, 3 OUTPUT_READY. Input tensor 187 B, output 5 B.

Full detail, including the inference sequence diagram and the known quirks:
[`docs/SPI_PROTOCOL.md`](https://github.com/Protocentral/healthylink-compute-fw/blob/main/docs/SPI_PROTOCOL.md)
in the compute repository.

### 12.5 There is no UART transport

`app_m7/src/healthylink/npu_uart_host.c` exists here, and earlier revisions of
this document described a module-side `CONFIG_HL_NPU_UART_SERVER` to pair with
it. **That Kconfig and that code do not exist in the module firmware** — checked
against the sources during the split, not assumed. SPI is the only implemented
transport. It could not have worked as documented in any case: USART2 is the
module's console, so the link would need a different UART. `CONFIG_HPI_NPU_UART`
defaults `n` and the host half is parked.

---

## 13. EEG module

An 8-channel ADS1299 front end for the expansion port. What follows is the
minimal electrode setup for first light, and how to tell a real EEG signal from
noise.

This guide describes how to perform initial EEG testing with the HealthyLink ADS1299 module using a minimal electrode setup.

### Overview

The HealthyLink EEG module uses the Texas Instruments ADS1299 - an 8-channel, 24-bit analog front-end specifically designed for EEG applications. For initial body testing, a minimal 3-electrode single-channel configuration is recommended.

### 2-Channel Referential Montage Setup

The default firmware configuration uses a **2-channel referential montage** with a shared reference (SRB1):

| Electrode | Position | ADS1299 Input | Purpose |
|-----------|----------|---------------|---------|
| **Channel 1 (+)** | Fp1 (forehead, left) | IN1P | Left frontal signal |
| **Channel 2 (+)** | Fp2 (forehead, right) | IN2P | Right frontal signal |
| **Reference** | A1 (left earlobe) | SRB1 | Shared reference for all channels |
| **Ground/DRL** | Fpz (forehead center) | BIASOUT | Driven right leg / bias |

This 4-electrode setup captures bilateral frontal activity for detecting eye blinks, concentration, and frontal asymmetry.

### Electrode Placement Diagram

```
            Fpz (Ground/DRL)
               ●
        Fp1 ●     ● Fp2
       (CH1)       (CH2)

            FRONT OF HEAD

        A1 ●           ● A2
      (SRB1 Ref)    (optional)

           LEFT EAR    RIGHT EAR
```

#### International 10-20 System Reference

- **Fp1**: 10% of nasion-to-inion distance, left of midline
- **Fpz**: 10% of nasion-to-inion distance, on midline
- **A1**: Left earlobe or mastoid process
- **A2**: Right earlobe or mastoid process (alternative ground location)

### Why This Configuration Works

1. **Referential montage with SRB1**: All channels share a single reference electrode (A1 earlobe), reducing electrode count while enabling channel comparison
2. **Bilateral frontal (Fp1/Fp2)**: Captures eye blinks, frontal asymmetry, and concentration-related activity
3. **Earlobe reference (SRB1)**: Low impedance contact with minimal muscle artifact, electrically stable
4. **DRL/BIAS electrode (Fpz)**: Actively cancels common-mode interference (50/60 Hz) via driven right leg circuit
5. **4 electrodes total**: Balanced between setup simplicity and meaningful bilateral recording

### Electrode Preparation

#### Materials Needed

- 3 EEG cup electrodes (Ag/AgCl recommended) or disposable adhesive electrodes
- Conductive electrode gel (Ten20, Elefix, or similar)
- Skin preparation gel or alcohol wipes
- Medical tape (if using cup electrodes)

#### Preparation Steps

1. **Clean the skin**: Use alcohol wipes or skin prep gel at each electrode site
2. **Lightly abrade** (optional): Use NuPrep or similar to reduce skin impedance
3. **Apply gel**: Fill electrode cup or apply to adhesive electrode
4. **Attach electrode**: Press firmly and secure with tape if needed
5. **Check impedance**: Aim for <10 kΩ per electrode (ideally <5 kΩ)

### Quick Verification Test Protocol

Once electrodes are connected and the system is running:

#### 1. Eyes Open Baseline (30 seconds)
- Subject sits relaxed, eyes open, looking at a fixed point
- **Expected**: Low-amplitude mixed frequencies, beta (13-30 Hz) dominant
- **Amplitude**: 5-20 µV typical

#### 2. Eyes Closed Relaxation (60 seconds)
- Subject closes eyes and relaxes
- **Expected**: Increased **alpha rhythm** (8-12 Hz) appearing within 5-10 seconds
- **Amplitude**: 20-100 µV typical
- This is the classic "alpha block" test - alpha appears with eyes closed

#### 3. Eye Blink Test
- Subject blinks eyes several times
- **Expected**: Large amplitude deflections (artifact)
- **Amplitude**: 100-500 µV
- **Purpose**: Confirms the signal path is working (blink artifacts are unmistakable)

#### 4. Eyes Open Again (30 seconds)
- Subject opens eyes
- **Expected**: Alpha rhythm should **attenuate** (disappear) within 1-2 seconds
- This confirms you're seeing real brain activity, not noise

### Expected Signal Characteristics

| Brain State | Dominant Frequency | Typical Amplitude | Notes |
|-------------|-------------------|-------------------|-------|
| Eyes open, alert | Beta (13-30 Hz) | 5-20 µV | Low amplitude, fast activity |
| Eyes closed, relaxed | Alpha (8-12 Hz) | 20-100 µV | Posterior dominant rhythm |
| Drowsy | Theta (4-8 Hz) | 20-50 µV | Slowing of background |
| Eye blink artifact | DC shift | 100-500 µV | Sharp, stereotyped waveform |
| Muscle artifact | >30 Hz | Variable | High frequency, irregular |

### Hardware Setup

#### Build and Flash

```bash
# Activate Zephyr environment
source ~/zephyrproject/.venv/bin/activate

# Build M7 with EEG overlay
./make_m7_eeg.sh

# Build M4 (signal processing)
scripts/build.sh m4

# Flash both cores
scripts/flash.sh
```

#### Verify SPI4 Communication

The ADS1299 connects via SPI4. Check the serial console for initialization messages:

```
[00:00:01.234,000] <inf> ads1299: ADS1299 initialized successfully
[00:00:01.235,000] <inf> ads1299: Device ID: 0x3E (ADS1299)
[00:00:01.236,000] <inf> ads1299: CH1+CH2 active (gain=24), CH3-8 powered down
```

### Troubleshooting

| Symptom | Likely Cause | Solution |
|---------|--------------|----------|
| **50/60 Hz noise dominant** | Poor ground electrode contact | Improve DRL electrode connection, add more gel |
| **Flat line (no signal)** | No signal path | Check electrode connections, verify SPI4 working |
| **Saturated/railed signal** | DC offset too high | Re-apply electrodes with fresh gel, check for dried gel |
| **High-frequency noise** | EMI pickup | Move away from power supplies, monitors, phones |
| **No alpha with eyes closed** | Subject not relaxed, poor electrode contact | Coach relaxation, recheck Fp1 electrode |
| **Constant muscle artifact** | Tension in forehead/jaw | Have subject relax facial muscles |
| **Intermittent dropouts** | Loose electrode connection | Secure electrodes with additional tape |

#### Common Noise Sources to Avoid

- Fluorescent lights (high-frequency noise)
- Computer monitors (especially CRTs)
- Mobile phones and WiFi routers
- Power adapters and switching supplies
- Air conditioning units

### Expanding to Multi-Channel Recording

Once single-channel recording is verified, expand to more channels:

#### 4-Channel Frontal Montage

| Channel | Positive | Negative | Brain Region |
|---------|----------|----------|--------------|
| CH1 | Fp1 | A1 | Left frontal |
| CH2 | Fp2 | A2 | Right frontal |
| CH3 | F3 | A1 | Left frontal-central |
| CH4 | F4 | A2 | Right frontal-central |

#### 8-Channel Standard Montage

| Channel | Positive | Negative | Brain Region |
|---------|----------|----------|--------------|
| CH1 | Fp1 | A1 | Left prefrontal |
| CH2 | Fp2 | A2 | Right prefrontal |
| CH3 | F3 | A1 | Left frontal |
| CH4 | F4 | A2 | Right frontal |
| CH5 | C3 | A1 | Left central |
| CH6 | C4 | A2 | Right central |
| CH7 | O1 | A1 | Left occipital |
| CH8 | O2 | A2 | Right occipital |

### Safety Considerations

- **Isolation**: The HealthyPi 6 provides electrical isolation from mains power
- **Current limits**: ADS1299 bias current is <10 nA (safe for body contact)
- **Do not use** during thunderstorms or near high-voltage equipment
- **Skin irritation**: Remove electrodes if skin irritation occurs
- **Not for clinical diagnosis**: This is a development/research platform

### Digital Filtering

The M7 firmware applies a 3-stage digital filter chain to the EEG signal:

1. **High-pass filter**: 0.5 Hz (removes DC drift and slow artifacts)
2. **Low-pass filter**: 40 Hz Butterworth (removes high-frequency noise and EMG artifacts)
3. **Notch filter**: 50 Hz, Q=5 (removes powerline interference; configurable for 60 Hz)

Filter chain: HP → LP → Notch

These filters are implemented with Q15 fixed-point coefficients in the EEG
module provider, `app_m7/src/healthylink/mod_eeg.c`.

### Data Streaming

EEG data can be viewed via:

1. **USB CDC**: Connect to serial port, data streams in OpenView protocol format
2. **WiFi TCP**: Via ESP32-C6 HealthyBridge (when enabled)
3. **LVGL Display**: Real-time waveform display on the built-in screen
