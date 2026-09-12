# HealthyLink expansion port

The physical and electrical specification of the HealthyLink expansion slots:
the Hirose DF9-31 dual-connector interface, its pin assignment, module
mechanics, and how a plugged-in module is identified before it is powered.

The firmware side (the provider registry, the interface arbiter and the fault
supervisor) is summarised in [`ARCHITECTURE.md`](ARCHITECTURE.md). The two
modules that exist today are covered at the end: the
[NPU compute module](#12-npu-compute-module) and the [EEG module](#13-eeg-module).

---

## 1. Overview

### 1.1 Design

The host carries **two module slots**. Each slot is a pair of **Hirose DF9-31**
board-to-board connectors (four connectors in total), giving a module 62 signal
pins. Any module fits either slot: a module is identified from its ID EEPROM,
powered through its slot's own load switch, and driven over buses shared by both
slots.

### 1.2 Key features

| Feature | Specification |
|---|---|
| Connector | Hirose DF9-31P-1V (31-pin, 1.0 mm pitch), 2 per slot |
| Slots | 2, usable simultaneously |
| SPI | SPI4 (primary) and SPI6 (secondary), shared by both slots |
| UART | USART2 with hardware flow control |
| CAN | FDCAN1 (CAN-FD) |
| USB host | USB OTG FS (optional, off by default) |
| I²C | I2C3, shared; one ID EEPROM per slot |
| Module power | Per-slot load switch with fault input |
| Module identification | ID EEPROM on an always-on rail; no detect GPIO |
| Analog | 8 ADC inputs on the connector (6 have host pins on v5, §10) |
| GPIO | 4 general purpose on the connector (3 have host pins on v5, §10) |
| StackLink | 5 module-to-module signals |

---

## 2. Connector specifications

### 2.1 Hirose DF9-31 series

| Parameter | Value |
|---|---|
| Part number (receptacle, host) | DF9-31P-1V(32) |
| Part number (plug, module) | DF9-31S-1V(32) |
| Pin count | 31 |
| Pitch | 1.0 mm |
| Mated height | 5.0 mm (standard) |
| Current rating | 0.5 A per pin |
| Voltage rating | 50 V AC/DC |
| Operating temperature | −55 °C to +85 °C |

### 2.2 Host board configuration

Four DF9-31P receptacles, two per slot:

```
    ┌──────────────────────────────────────────────────────────────────────┐
    │                        HealthyPi 6 host board                         │
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

### 2.3 Connector wiring scheme

Per module:

- **CONN1 (Primary):** core signals — SPI4, I²C, USART, module control, ID
- **CONN2 (Secondary):** extended signals — SPI6, FDCAN, USB, ADC, GPIO, StackLink

Between the two slots:

| Signal type | Wiring | Notes |
|---|---|---|
| Module supply | Per slot | Gated by the slot's own load switch (§11) |
| Ground (GND, AGND) | Parallel | Common ground plane |
| SPI4 (SCK, MOSI, MISO) | Parallel | Shared bus |
| SPI6 (SCK, MOSI, MISO) | Parallel | Shared bus |
| SPI6_CS_A | Slot A only | Individual chip select |
| SPI6_CS_B | Slot B only | Individual chip select |
| I²C (SCL, SDA) | Parallel | Shared bus; ID EEPROM at 0x50 (A), 0x51 (B) |
| USART | Parallel | Directly connected |
| FDCAN | Parallel | Shared bus (use addressing) |
| USB host | Slot A only | Single USB host port |
| ADC channels | Parallel | Module-specific usage |
| GPIO | Parallel | Directly connected |
| StackLink | Parallel | Inter-module bus |
| Module control | Parallel | Except MOD_RESET, which is per slot |

---

## 3. Pin assignment

### 3.1 Two connectors per module

| Connector | Purpose | Key signals |
|---|---|---|
| **Primary (CONN1)** | Core communication | SPI4, I²C, USART, module control, module ID |
| **Secondary (CONN2)** | Extended I/O | SPI6, FDCAN, USB, ADC, GPIO, StackLink |

1. **Critical signals on Primary:** a module must work with CONN1 alone for basic
   SPI/I²C communication.
2. **Optional features on Secondary:** ADC, USB host, FDCAN and StackLink are
   module-specific.
3. **Power on both:** balanced current across the two connectors.
4. **Ground at the edges:** mechanical stability.

The tables below define the connector. Which host MCU pin drives each signal on
HealthyPi 6 v5 is in [§10](#10-healthypi-6-v5-host-pin-mapping).

### 3.2 Connector 1 (Primary) — pins 1–31

| Pin | Signal | Direction (host) | Description |
|---|---|---|---|
| **Power** ||||
| 1 | GND | — | Ground (edge) |
| 2 | GND | — | Ground |
| 3 | VCC_3V3 | PWR | 3.3 V supply |
| 4 | VCC_3V3 | PWR | 3.3 V supply |
| 5 | VCC_5V | PWR | 5 V supply |
| **SPI4 (primary bus)** ||||
| 6 | SPI4_SCK | OUT | SPI4 clock |
| 7 | SPI4_MOSI | OUT | SPI4 master out |
| 8 | SPI4_MISO | IN | SPI4 master in |
| 9 | SPI4_CS_A | OUT | Chip select A |
| 10 | SPI4_CS_B | OUT | Chip select B |
| **I²C** ||||
| 11 | I2C_SCL | OUT | I²C clock |
| 12 | I2C_SDA | I/O | I²C data |
| **USART2 (hardware flow control)** ||||
| 13 | USART_TX | OUT | UART transmit |
| 14 | USART_RX | IN | UART receive |
| 15 | USART_RTS | OUT | Request to send |
| 16 | USART_CTS | IN | Clear to send |
| **Module control** ||||
| 17 | MOD_RESET_N | OUT | Module reset (active low) |
| 18 | MOD_ENABLE | OUT | Module enable / PWDN |
| 19 | MOD_IRQ_N | IN | Module interrupt / DRDY |
| 20 | MOD_READY | IN | Module ready / START |
| **Module identification** ||||
| 21 | MOD_ID_0 | IN | Module ID bit 0 (LSB) |
| 22 | MOD_ID_1 | IN | Module ID bit 1 |
| 23 | MOD_ID_2 | IN | Module ID bit 2 |
| 24 | MOD_ID_3 | IN | Module ID bit 3 (MSB) |
| **Power (edge)** ||||
| 25 | GND | — | Ground |
| 26 | GND | — | Ground |
| 27 | VCC_3V3 | PWR | 3.3 V supply |
| 28 | GND | — | Ground (edge) |
| 29 | AGND | — | Analog ground |
| 30 | AGND | — | Analog ground |
| 31 | VCC_5V | PWR | 5 V supply |

### 3.3 Connector 2 (Secondary) — pins 32–62

| Pin | Signal | Direction (host) | Description |
|---|---|---|---|
| **SPI6 (secondary bus)** ||||
| 32 | SPI6_SCK | OUT | SPI6 clock |
| 33 | SPI6_MOSI | OUT | SPI6 master out |
| 34 | SPI6_MISO | IN | SPI6 master in |
| 35 | SPI6_CS_A | OUT | Chip select slot A |
| 36 | SPI6_CS_B | OUT | Chip select slot B |
| **FDCAN1 (CAN-FD)** ||||
| 37 | FDCAN_TX | OUT | CAN-FD transmit |
| 38 | FDCAN_RX | IN | CAN-FD receive |
| **USB host (optional)** ||||
| 39 | USB_FS_DM | I/O | USB full-speed D− |
| 40 | USB_FS_DP | I/O | USB full-speed D+ |
| 41 | GND | — | Ground (USB reference) |
| **ADC** ||||
| 42–49 | ADC_CH0 – ADC_CH7 | IN | Analog inputs |
| 50 | AGND | — | Analog ground |
| **GPIO** ||||
| 51–54 | GPIO_0 – GPIO_3 | I/O | General purpose |
| **StackLink (inter-module bus)** ||||
| 55 | SL_CLK | — | StackLink clock |
| 56 | SL_MOSI | — | StackLink data, downstream |
| 57 | SL_MISO | — | StackLink data, upstream |
| 58 | SL_SYNC | — | StackLink sync pulse |
| 59 | SL_IRQ_N | — | StackLink interrupt |
| **Power (edge)** ||||
| 60 | VCC_3V3 | PWR | 3.3 V supply |
| 61 | GND | — | Ground |
| 62 | GND | — | Ground (edge) |

### 3.4 Mounting pads

| Pad | Description |
|---|---|
| MP1 | CONN1 mounting pad (left) |
| MP2 | CONN1 mounting pad (right) |
| MP3 | CONN2 mounting pad (left) |
| MP4 | CONN2 mounting pad (right) |

### 3.5 Pin count summary (per module)

| Function | Pins 1–31 | Pins 32–62 | Total |
|---|---|---|---|
| VCC_3V3 | 3 | 1 | 4 |
| VCC_5V | 2 | 0 | 2 |
| GND | 5 | 3 | 8 |
| AGND | 2 | 1 | 3 |
| SPI4 | 5 | 0 | 5 |
| SPI6 | 0 | 5 | 5 |
| I²C | 2 | 0 | 2 |
| USART | 4 | 0 | 4 |
| FDCAN | 0 | 2 | 2 |
| USB | 0 | 2 | 2 |
| ADC | 0 | 8 | 8 |
| Module control | 4 | 0 | 4 |
| Module ID | 4 | 0 | 4 |
| GPIO | 0 | 4 | 4 |
| StackLink | 0 | 5 | 5 |
| **Signal pins** | **31** | **31** | **62** |
| **Mounting pads** | 2 | 2 | 4 |
| **Total pads** | **33** | **33** | **66** |

---

## 4. Module design

### 4.1 Module PCB

| Parameter | Value |
|---|---|
| Connectors | 2× DF9-31S-1V plugs (CONN1 + CONN2) |
| Connector spacing | 18 mm centre-to-centre (asymmetric) |
| PCB thickness | 1.6 mm |
| Layers | 4 (recommended) |
| Finish | HASL or ENIG |
| Dimensions | 50 × 55 mm |

### 4.2 Orientation keying

DF9 connectors are symmetric and can be inserted either way round. Placing the
two connectors **asymmetrically** prevents a reversed module from mating:

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

Additional keying:

1. **Asymmetric mounting holes:** M2.5 holes at different distances from the corners
2. **Silkscreen arrow:** mark "THIS SIDE UP" on the module
3. **Chamfered corner:** remove one corner for visual orientation

### 4.3 Module ID EEPROM

Every module carries one 256-byte I²C EEPROM (24AA02 / AT24CS02 class) holding
its identity: magic `HLNK`, module ID, hardware revision, capabilities, name,
serial, and a CRC. Its A0 pin is strapped by the **host**, per slot, so the same
module answers at the address of the slot it is in:

| Slot | A0 strap | Address |
|---|---|---|
| A | GND | 0x50 |
| B | 3V3 | 0x51 |

The EEPROM must be an address-strappable part (A0 brought out), and it is
powered from the host's always-on 3V3 rail, not the switched module supply.
That is what lets the host identify a module *before* deciding whether to
power it, and it is also what makes the EEPROM reachable from a running
HealthyPi whatever state the slot is in.

Registered module IDs:

| ID | Module | Claims |
|---|---|---|
| `0x0001` | EEG-8CH (ADS1299) | SPI4 |
| `0x0002` | EMG-4CH | SPI4 |
| `0x0003` | TRIGGER-IO | GPIO |
| `0x0004` | CAN-INTERFACE | FDCAN |
| `0x0005` | HealthyLink Compute (STM32N657) | SPI4 |
| `0x0006` | HIGH-RES-ADC | SPI6 |
| `0x0007` | STIM-OUTPUT | — |
| `0x0008` | SYNC-MASTER | GPIO |
| `0x0009` | GSR-RESPIRATION | SPI6 |
| `0x000A` | GPIO breakout | nothing |

`0x000B`–`0x00FF` are reserved for ProtoCentral; `0x0100`–`0xFFFE` are free for
community modules. The GPIO breakout claims nothing on purpose: it brings every
interface out to headers and drives none of them, and the interface bits are
exclusive across the two slots — claiming one would lock a real module out of
the other slot for nothing.

#### 4.3.1 Programming a module's EEPROM

Through the HealthyPi itself, over the CDC 1 control port. The host is wired to
both slot EEPROMs, so the module stays in its slot and no external programmer
is involved:

```bash
healthypi hl eeprom dump --slot a                    # what is on it now
healthypi hl eeprom program --slot a \
    --module-id GPIO --name "GPIO breakout" --serial 1
healthypi module list                                # slot A should be active
```

`program` builds the 256-byte image, writes it in chunks (group-64 `0x0054`),
reads it back and compares, then re-detects the slot so the new identity takes
effect. It refuses to overwrite an EEPROM that already holds a valid image
unless you pass `--force`. `healthypi hl eeprom erase --slot a --yes` puts a
mis-programmed module back to factory-blank `0xFF`.

A module that is **not** in a slot still needs an external programmer (FT232H,
Raspberry Pi I²C, CH341A, Bus Pirate): `healthypi hl eeprom generate` writes the
same image to a file.

**Auto-provisioning is off** (`CONFIG_HEALTHYLINK_AUTO_PROVISION=n`). The
firmware can stamp a blank EEPROM by itself, but the identity comes from one
board-wide devicetree node rather than from the module in front of it — so the
first boot after plugging in an unprogrammed module would silently label it as
whatever that node names, in either slot, and the result reads back as a
perfectly valid something-else. Enable it only on a production line that
programs one module type per station.

### 4.4 Example module block diagram

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

- **CONN1:** SPI4 for ADS1299 data, I²C for the ID EEPROM, module control
- **CONN2:** GPIO, StackLink (optional); ADC and USART unused on this module

---

## 5. Signal routing guidelines

### 5.1 SPI buses

- Route SCK, MOSI and MISO as parallel traces
- Length-match to within 5 mm
- 50 Ω impedance above 25 MHz
- Decouple close to the connector

### 5.2 Analog signals

- Separate AGND from digital GND on the module
- Star-ground at the host connector
- Guard ADC traces with ground
- Keep analog traces away from SPI

### 5.3 Power distribution

```
Host VCC_3V3 ──┬── 100nF ──┬── Slot A (pins 3,4)
               │           │
               │           └── Slot B (pins 3,4)
               │
               └── 10µF bulk
```

---

## 6. Connector rationale

Why a dual-DF9 board-to-board interface:

- **Low module cost:** a standard 4-layer PCB, no gold-finger edge card
- **Fits biosignal modules:** EEG, ECG and EMG front ends on SPI fit the 62-pin
  budget with headroom
- **Prototyping-friendly:** any PCB fab can build a module
- **Signal integrity:** short traces with controlled impedance

---

## 7. Bill of materials

### 7.1 Host side (per slot)

| Part | Qty | Part number | Description |
|---|---|---|---|
| J1, J2 | 2 | DF9-31P-1V(32) | Receptacle, SMD (Primary + Secondary) |
| C1–C4 | 4 | 100 nF 0402 | Decoupling (2 per connector) |
| C5 | 1 | 10 µF 0805 | Bulk capacitor |

**Both slots:** 4× DF9-31P receptacles, 8× 100 nF, 2× 10 µF.

### 7.2 Module side

| Part | Qty | Part number | Description |
|---|---|---|---|
| J1, J2 | 2 | DF9-31S-1V(32) | Plug, SMD (Primary + Secondary) |
| U1 | 1 | 24AA02 / AT24CS02 | ID EEPROM (address-strappable) |
| C1–C4 | 4 | 100 nF 0402 | Decoupling (2 per connector) |

---

## 8. Mechanical layout

### 8.1 Dual module configuration

Two modules mount side by side on the 120 × 70 mm host board:

```
                              120 mm (Host Board)
◄──────────────────────────────────────────────────────────────────────────────────►

┌──────────────────────────────────────────────────────────────────────────────────┐  ▲
│                             HealthyPi 6 host board                                │  │
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
│       5mm + 50mm + 10mm + 50mm + 5mm = 120mm                                     │
└──────────────────────────────────────────────────────────────────────────────────┘
```

### 8.2 Module dimensions

| Parameter | Value | Notes |
|---|---|---|
| **Host board** | 120 × 70 mm | HealthyPi 6 main board |
| **Module width** | 50 mm | Fits 2 modules + gap in 120 mm |
| **Module length** | 55 mm | Leaves 15 mm routing on the host |
| PCB thickness | 1.6 mm | Standard 4-layer |
| Mated height | 5.0 mm | DF9-31 standard stack height |
| Connector spacing | 18 mm | Centre-to-centre (asymmetric) |
| Edge clearance | 5 mm | Each side of the host board |
| Inter-module gap | 10 mm | Between module A and B |
| **Total module area** | 110 × 55 mm | Both modules + gap |

### 8.3 Host board layout

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

### 8.4 Mounting holes

Host board (120 × 70 mm):

| Hole | Position (X, Y) | Size | Purpose |
|---|---|---|---|
| H1 | (5, 5) mm | M3 | Corner mount |
| H2 | (115, 5) mm | M3 | Corner mount |
| H3 | (5, 65) mm | M3 | Corner mount |
| H4 | (115, 65) mm | M3 | Corner mount |

Modules are held by the friction-fit DF9 connectors (rated 30+ mating cycles).

### 8.5 Connector placement (asymmetric for keying)

**Slot A (left module):**

| Connector | Centre X | Centre Y | Offset from module edge |
|---|---|---|---|
| CONN1-A (Primary) | 17.5 mm | 45 mm | 12.5 mm from left |
| CONN2-A (Secondary) | 35.5 mm | 45 mm | 14.5 mm from right |

**Slot B (right module):**

| Connector | Centre X | Centre Y | Offset from module edge |
|---|---|---|---|
| CONN1-B (Primary) | 67.5 mm | 45 mm | 7.5 mm from slot left |
| CONN2-B (Secondary) | 85.5 mm | 45 mm | 24.5 mm from slot right |

A module rotated 180° does not line up with the host receptacles: CONN1 sits
12.5 mm from the left edge, 14.5 mm if reversed.

### 8.6 Module PCB template

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

Module mounting holes (asymmetric, for additional keying):

| Hole | Position (X, Y) | Size | Notes |
|---|---|---|---|
| H1 | (5, 5) mm | M2.5 | Front-left |
| H2 | (45, 5) mm | M2.5 | Front-right |
| H3 | (5, 50) mm | M2.5 | Rear-left |
| H4 | (42, 50) mm | M2.5 | Rear-right (offset 3 mm) |

### 8.7 Clearance requirements

| Zone | Minimum | Notes |
|---|---|---|
| Component height (top) | 8 mm | Above the host board surface |
| Component height (bottom) | 3 mm | Below the module PCB (mated space) |
| Inter-module gap | 10 mm | Edge to edge between modules |
| Connector-to-connector gap | 2.5 mm | Between CONN1 and CONN2 edges |
| Edge clearance | 3 mm | Module edge to components |
| DF9 keep-out | 2 mm | Around each connector footprint |
| Electrode header zone | 10 × 40 mm | Far edge of the module |

### 8.8 More than two modules

More than two modules need a **HealthyLink Hub** board, which sits in the slots
and multiplexes chip selects to further modules (e.g. with a 74LVC1G3157):

```
    HealthyPi 6
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

---

## 9. KiCad library

The library files are not distributed in this repository; this is what is
needed to recreate them.

**Symbol `HEALTHYLINK_DF9_DUAL`** — one per module slot:

- Connector 1 (Primary): pins 1–31
- Connector 2 (Secondary): pins 32–62
- Mounting pads: MP1–MP4

**Footprint `HEALTHYLINK_DF9_MODULE_SLOT`** (host board):

- Module outline (50 × 55 mm) on silkscreen
- Two DF9-31P receptacle pad arrays (CONN1, CONN2)
- Asymmetric connector placement for keying (12.5 mm / 14.5 mm)
- Four mounting holes (H1–H4), H4 offset for keying
- Pin 1 markers and orientation arrow
- Courtyard with 1 mm clearance

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

## 10. HealthyPi 6 v5 host pin mapping

The STM32H757 pins behind each connector signal on the v5 board, as the
firmware drives them (`boards/protocentral/healthypi6_v5/healthypi6_v5.dtsi`).

### 10.1 Buses

| Interface | Signal | STM32 pin | Notes |
|---|---|---|---|
| **SPI4** | SCK / MOSI / MISO | PE2 / PE6 / PE5 | AF5 |
| | CS_A / CS_B | PE4 / PE3 | Plain GPIO (soft NSS) |
| **SPI6** | SCK / MOSI / MISO | PG13 / PG14 / PG12 | AF5; no chip select assigned on v5 — a consumer brings its own |
| **I2C3** | SCL / SDA | PH7 / PH8 | AF4 |
| **USART2** | TX / RX / RTS / CTS | PD5 / PD6 / PD4 / PD3 | AF7 |
| **FDCAN1** | TX / RX | PH13 / PH14 | AF9 |
| **USB OTG FS** | D− / D+ | PA11 / PA12 | Disabled by default: PA11 is the ESP32-C6 reset line |

### 10.2 Per slot

| Function | Slot A | Slot B |
|---|---|---|
| Module power enable (`EN_MOD_x`, active high) | PI1 | PH15 |
| Load-switch fault (`MOD_x_FLT`, active low) | PH11 | PI4 |
| Module reset (`MOD_RESET_x`) | PI2 | PI5 |
| ID EEPROM (I2C3) | 0x50 | 0x51 |

### 10.3 Shared control, GPIO and analog

| Function | STM32 pin | Notes |
|---|---|---|
| PWDN / MOD_ENABLE | PI6 | Shared by both slots |
| DRDY / MOD_IRQ_N | PH6 | Shared by both slots, active low |
| GPIO_0 – GPIO_2 | PI12, PI13, PI14 | No host pin is assigned to GPIO_3 on v5 |
| Analog inputs | PA1, PA2, PA3, PC0, PC1, PC4 | ADC1; not read by the firmware yet |
| MOD_ID_0 – MOD_ID_3 | — | Not used on v5: identification is by the ID EEPROM |
| StackLink | — | Module-to-module only; not wired to the MCU |

---

## 11. Module detection

### Summary

- **Detection is by ID EEPROM only.** There is no module-present or detect GPIO.
- Each slot's EEPROM answers at its own address on the shared I2C3 bus (slot A
  0x50, slot B 0x51), so the address says *which slot* and the contents say
  *which module*.
- The EEPROMs are on an **always-on rail**, so a module is identified before its
  slot is powered.
- Each slot has a **load switch** (`EN_MOD_x`) and a **fault** input
  (`MOD_x_FLT`) for a safe, sequenced power-up.

Why an always-on EEPROM:

1. **Identify before power:** read module ID, hardware revision and
   capabilities with the load switch off, then power only modules the firmware
   recognises.
2. **Clean bus:** the EEPROM shares the always-on domain with the I²C pull-ups,
   so no powered-down device loads the bus.
3. **Hot-plug without a detect pin:** insertion and removal show up as the
   EEPROM address appearing or disappearing, without energising the module.

### Enumeration sequence (identify-then-power)

For each slot:

```
1. probe the ID EEPROM (I2C ACK at the slot address)   # slot unpowered
     - NAK -> slot empty; next slot
2. read + validate the header (magic, CRC)             # still unpowered
     - invalid -> slot error; leave EN_MOD off
3. assert EN_MOD_x; settle
4. if MOD_x_FLT asserted:
     de-assert EN_MOD_x; slot error
5. match a provider for the module ID; claim its interfaces; start it
     - no provider, or probe/claim/start fails -> de-assert EN_MOD_x
```

**A slot stays powered only while its module is running.** Switching a slot on
again (group-64 `0x0052`, or the HealthyLink screen) re-runs the sequence, so it
is also the retry and the rescan.

### Hardware notes

- Put the I2C3 pull-ups on the always-on domain, alongside the EEPROM supply.
- Reference the A0 strap to the always-on rail (A0 → AON-GND / AON-3V3).
- Route A0 + A1 rather than A0 alone to go beyond two slots (up to 4 with two
  lines, 8 with three).
- On v4, slot A's EEPROM is on the switched rail and slot B has no EEPROM. Its
  slot A node carries `eeprom-switched-rail`, so that slot is powered from init
  and probed with power applied.

### Devicetree and driver

- Binding: `dts/bindings/misc/protocentral,healthylink-slot.yaml` —
  `power-gpios`, `fault-gpios`, `eeprom` (phandle), `eeprom-switched-rail`,
  `slot-label`.
- Nodes: `healthylink_slot_a` / `healthylink_slot_b`, with `healthylink_eeprom_a`
  (0x50) and `healthylink_eeprom_b` (0x51) on I2C3.
- Driver: `drivers/misc/healthylink/healthylink_core.c`, one device per slot node
  that has an `eeprom`. `healthylink_detect()` runs the sequence above for one
  slot; `healthylink_slot_power()` switches its load switch and checks the fault
  line on every power-up. A slot without an `eeprom` has no device and is
  reported as not detectable.
- Providers (`app_m7/src/healthylink/`) are told which slot their module is in.
  The arbiter runs one module per shared interface, so two SPI4 modules cannot
  be active at once.

---

## 12. NPU compute module

The HealthyLink Compute module: an STM32N657 (Cortex-M55 with a Neural-ART NPU)
that runs inference next to the signal source.

The module's firmware lives in its own repository,
[`Protocentral/healthylink-compute-fw`](https://github.com/Protocentral/healthylink-compute-fw),
because it bundles STMicroelectronics' NPU runtime under a proprietary licence
that cannot be redistributed in an MIT repository. This section covers the
**host** side only.

### 12.1 Status

| | |
|---|---|
| Detection, power and ACTIVE, in either slot | ✅ validated on v5 |
| HLink v2 handshake (alive signature, GET_INFO, STATUS), in either slot | ✅ validated on v5 |
| Data plane (model, tensor and stream commands) | not driven by the host yet |

Nothing in this repository classifies a beat today.

### 12.2 Host-side files

| File | Role |
|---|---|
| `app_m7/src/healthylink/mod_npu.c` | The provider: runs the HLink v2 handshake off the boot path and caches the result |
| `app_m7/src/healthylink/hlink_proto.{c,h}` | HLink v2 framing, CRC and command codes (host half) |
| `app_m7/src/healthylink/mod_npu.h` | `hpi_npu_link_get()`, the handshake snapshot read by the UI and the group-64 self-test |
| `boards/protocentral/healthypi6_v5/healthylink-compute.overlay` | The module's SPI4 node (added to the default build by `scripts/build.sh m7`) |

The slot EEPROM reports magic `HLNK` and module ID **`0x0005`**.

### 12.3 Host wiring

| Signal | Slot A | Slot B | Notes |
|---|---|---|---|
| SPI4 SCK / MISO / MOSI | PE2 / PE5 / PE6 | same | Shared bus |
| SPI4 CS | PE4 (CS_A) | PE4 (CS_A) | The module is wired to CS_A. Both chip selects reach both slots, so a module selects on the pin its own board uses. `mod_npu.c` drives CS as a plain GPIO — routing it through `spi_config.cs.gpio` faults on soft-NSS SPI4 — holds the other line inactive, and tries CS_B once if CS_A draws nothing. |
| IRQ from module | PI12 | PI12 | Active low; an optimisation only — every wait falls back to a timeout |
| MOD_RESET | PI2 | PI5 | Not driven; the module boots when its slot is powered |
| PWDN | PI6 | PI6 | Shared with the EEG module |

The slot is powered only after the module is identified, so the module is
cold-booting when the provider starts; it first answers about 1.4 s after
power-up. The handshake waits `CONFIG_HPI_NPU_BOOT_WAIT_MS` (1500 ms) before its
first frame and looks for the alive signature up to three times.

The handshake is controlled by `CONFIG_HPI_NPU_COMMS_CHECK`: on in the dev
flavor, off in prod. Detection, power and ACTIVE work without it.

**Do not touch SPI6 while SPI4 is in use** — it wedges the next SPI4 transceive.

### 12.4 The wire contract (HLink v2)

The host half is `app_m7/src/healthylink/hlink_proto.{c,h}`; the module half and
the specification (`docs/HLINK_PROTOCOL.md`) are in the compute repository. The
two are separate code, so **a constant changed on one side fails on the wire,
not at build time.**

- **Fixed 256-byte frames.** Every CS transaction clocks exactly 256 bytes both
  ways: the STM32 SPI slave completes on the programmed count, not on NSS
  deassert, so a short transaction hangs it. GET_INFO reports the module's
  frame size so a mismatch is diagnosed rather than suffered.
- **Frame:** `sof 0xA5 | cmd | flags | seq | len u16 | rsvd u16 | payload |
  crc16`, little-endian. CRC16-CCITT (init 0xFFFF) over everything after the
  SOF.
- **Replies are one transaction late.** The module parses a command after its
  frame has been clocked and stages the reply for the next one. The host waits
  for the IRQ (or its timeout), then clocks a NOP frame and finds the reply by
  SOF scan and `seq` echo — the slave can emit leading underrun bytes, so the
  reply may start at any offset.
- **Alive signature:** an idle module clocks out `'H' 'L' 'N' 'K'`, protocol
  major, minor, and the module ID (big-endian). The host matches the magic and
  ID and reads the version; a v1 module is reported, never driven.
- **Commands the host sends:** `0x00` NOP, `0x02` GET_INFO, `0x03` STATUS.
  PING (`0x01`), RESET (`0x04`) and the data plane (`0x10`–`0x13` models,
  `0x20`–`0x22` tensors, `0x30`–`0x32` streaming) are defined but not yet used.

### 12.5 UART

SPI is the only transport. `app_m7/src/healthylink/npu_uart_host.c` is a parked
host half with no module counterpart; `CONFIG_HPI_NPU_UART` defaults to `n`.

---

## 13. EEG module

An 8-channel front end on the TI ADS1299 (24-bit, designed for EEG). What
follows is a minimal electrode setup for first light, and how to tell a real
EEG signal from noise.

EEG capture has not yet been validated end to end on v5 hardware.

### 2-channel referential montage

The default configuration uses two channels with a shared reference (SRB1):

| Electrode | Position | ADS1299 input | Purpose |
|---|---|---|---|
| **Channel 1 (+)** | Fp1 (forehead, left) | IN1P | Left frontal signal |
| **Channel 2 (+)** | Fp2 (forehead, right) | IN2P | Right frontal signal |
| **Reference** | A1 (left earlobe) | SRB1 | Shared reference for all channels |
| **Ground / DRL** | Fpz (forehead centre) | BIASOUT | Driven right leg / bias |

Four electrodes capture bilateral frontal activity: eye blinks, concentration
and frontal asymmetry.

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

10-20 system positions:

- **Fp1:** 10% of nasion-to-inion distance, left of midline
- **Fpz:** 10% of nasion-to-inion distance, on midline
- **A1:** left earlobe or mastoid
- **A2:** right earlobe or mastoid (alternative ground)

Why this works:

1. **Referential montage with SRB1:** every channel shares one reference, so
   fewer electrodes and comparable channels
2. **Bilateral frontal (Fp1/Fp2):** eye blinks, frontal asymmetry,
   concentration-related activity
3. **Earlobe reference:** low-impedance contact, little muscle artifact
4. **DRL/bias at Fpz:** actively cancels 50/60 Hz common-mode interference

### Electrode preparation

You need four EEG cup electrodes (Ag/AgCl recommended) or disposable adhesive
electrodes, conductive gel (Ten20, Elefix or similar), skin prep gel or alcohol
wipes, and medical tape for cup electrodes.

1. **Clean the skin** at each site with alcohol or prep gel.
2. **Lightly abrade** (optional) to reduce skin impedance.
3. **Apply gel** to the cup or adhesive electrode.
4. **Attach** firmly; tape if needed.
5. **Check impedance:** below 10 kΩ per electrode, ideally below 5 kΩ.

### Verification protocol

1. **Eyes open, 30 s.** Relaxed, looking at a fixed point. Expect low-amplitude
   mixed activity, beta (13–30 Hz) dominant, 5–20 µV.
2. **Eyes closed, 60 s.** Expect **alpha** (8–12 Hz) within 5–10 s, 20–100 µV —
   the classic alpha-block test.
3. **Blinks.** Expect large deflections, 100–500 µV. Blink artifacts are
   unmistakable, which confirms the signal path.
4. **Eyes open again, 30 s.** Alpha should **attenuate** within 1–2 s. This is
   what shows you are seeing brain activity, not noise.

| Brain state | Dominant frequency | Typical amplitude | Notes |
|---|---|---|---|
| Eyes open, alert | Beta (13–30 Hz) | 5–20 µV | Low amplitude, fast activity |
| Eyes closed, relaxed | Alpha (8–12 Hz) | 20–100 µV | Posterior dominant rhythm |
| Drowsy | Theta (4–8 Hz) | 20–50 µV | Slowing of background |
| Eye blink artifact | DC shift | 100–500 µV | Sharp, stereotyped waveform |
| Muscle artifact | > 30 Hz | Variable | High frequency, irregular |

### Firmware

The EEG provider (`app_m7/src/healthylink/mod_eeg.c`) and the ADS1299 driver are
in the default M7 build (`scripts/build.sh m7`). When a slot's EEPROM reports an
EEG module, the provider brings up the ADS1299 node for that slot (`ads1299` for
slot A, `ads1299_b` for slot B — they differ only in their reset line) and
publishes 8-channel frames to the sample bus. From there they reach the CDC0
`.HP6` stream and the SD recording like any onboard signal. The EEG path applies
no digital filtering on the device.

A working front end logs, on the console:

```
<inf> ads1299: ADS1299 detected, ID: 0x3E
<inf> ads1299: ADS1299: CH1+CH2 active (gain=24), CH3-8 powered down
<inf> ads1299: ADS1299: SRB1 enabled as reference
```

### Troubleshooting

| Symptom | Likely cause | Fix |
|---|---|---|
| **50/60 Hz noise dominant** | Poor ground electrode contact | Improve the DRL electrode, add gel |
| **Flat line** | No signal path | Check electrodes; check the ADS1299 lines above appeared |
| **Saturated / railed** | DC offset too high | Re-apply electrodes with fresh gel |
| **High-frequency noise** | EMI pickup | Move away from supplies, monitors, phones |
| **No alpha with eyes closed** | Subject not relaxed, poor contact | Coach relaxation, recheck Fp1 |
| **Constant muscle artifact** | Forehead or jaw tension | Relax facial muscles |
| **Intermittent dropouts** | Loose electrode | Secure with more tape |

Common noise sources: fluorescent lights, monitors, phones and Wi-Fi routers,
switching power adapters, air-conditioning units.

### More channels

**4-channel frontal:**

| Channel | Positive | Negative | Region |
|---|---|---|---|
| CH1 | Fp1 | A1 | Left frontal |
| CH2 | Fp2 | A2 | Right frontal |
| CH3 | F3 | A1 | Left frontal-central |
| CH4 | F4 | A2 | Right frontal-central |

**8-channel standard:**

| Channel | Positive | Negative | Region |
|---|---|---|---|
| CH1 | Fp1 | A1 | Left prefrontal |
| CH2 | Fp2 | A2 | Right prefrontal |
| CH3 | F3 | A1 | Left frontal |
| CH4 | F4 | A2 | Right frontal |
| CH5 | C3 | A1 | Left central |
| CH6 | C4 | A2 | Right central |
| CH7 | O1 | A1 | Left occipital |
| CH8 | O2 | A2 | Right occipital |

### Safety

- **HealthyPi 6 is not a medical device.** Whenever electrodes are on a person,
  run it from its battery or a properly isolated supply — never from a
  mains-derived USB supply. See the
  [README's important notice](../README.md#important-notice).
- Remove electrodes if the skin becomes irritated.
- Do not use during thunderstorms or near high-voltage equipment.
