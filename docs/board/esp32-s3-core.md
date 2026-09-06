# ESP32-S3 Module Core — ES3C28P / ES3N28P (LCDWIKI 2.8" IPS)

MCU essentials for the board that runs hughes_bridge: exact silicon, memory, native USB, strapping/boot wiring, ADC, and reset — every fact cited to the hardware pack.

## Sources mined

- `5-Schematic/2.8inch_ESP32-S3_Display_Module_Hardware_Schematic.pdf` (main-control circuit, U7)
- `5-Schematic/2.8inch_ESP32-S3_Display_Schematic.pdf` (labelled sub-circuit blocks)
- IO-allocation CSV (converted from `5-Schematic/ESP32-S3_Chip_IO_Resource_Allocation.xlsx`)
- `4-DataSheet/esp32-s3_datasheet_en.pdf` (part numbering, strapping, boot, USB, ADC)
- `4-DataSheet/esp32-s3_hardware_design_guidelines_en.pdf` (CHIP_PU RC-reset guidance)
- `2-Specification/ES3C28P_ES3N28P_Product_Specification_V1.0.pdf` (§3.1 module summary)
- Local corroboration: `platformio.ini`, `LEDGER.md` (this repo — not a pack source; used only to flag agreement)

## Silicon and memory

The main controller is a **bare ESP32-S3R8 chip** (schematic designator **U7**, labelled `ESP32-S3R8`), *not* a WROOM/MINI module — the schematic exposes chip-level balls (CHIP_PU, GPIO0…GPIO48, the SPI0/1 flash+PSRAM pins). Ref: `5-Schematic/2.8inch_ESP32-S3_Display_Module_Hardware_Schematic.pdf` (U7 block, "ESP32-S3R8" x2, main-control circuit).

| Attribute | Value | Citation |
|---|---|---|
| CPU | Xtensa LX7 dual-core, 32-bit, up to 240 MHz | `2-Specification/ES3C28P_ES3N28P_Product_Specification_V1.0.pdf` §3.1 p.5 |
| On-chip ROM / SRAM | 384 KB ROM + 512 KB SRAM + 16 KB RTC SRAM | `2-Specification/...Product_Specification_V1.0.pdf` §3.1 p.5 |
| PSRAM | **8 MB Octal (OPI) PSRAM, in-package** | Prod Spec §3.1 p.5 ("8M OPI PSRAM"); chip = ESP32-S3R8 → `4-DataSheet/esp32-s3_datasheet_en.pdf` Ordering table p.10 (`ESP32-S3R8 … 8 MB (Octal SPI)`) |
| Flash | **16 MB QSPI flash, off-package (external)** | Prod Spec §3.1 p.5 ("16M QSPI Flash"); ESP32-S3R8 ships `--` in-package flash, so flash is external → datasheet Ordering table p.10 |
| VDD_SPI (flash/PSRAM rail) | 3.3 V | `4-DataSheet/esp32-s3_datasheet_en.pdf` Ordering table p.10 (`ESP32-S3R8 … 3.3 V`) |
| Radio | Wi-Fi 2.4 GHz 802.11 b/g/n + BLE 5.0 | Prod Spec §3.1 p.5 |
| Operating voltage | 3.0–3.6 V, −40…85 °C | Prod Spec §3.1 p.5 |

**"N16R8" clarified (derived):** the repo/LEDGER shorthand *N16R8* = 16 MB flash + 8 MB PSRAM. It resolves to **ESP32-S3R8 die + external 16 MB QSPI flash** — the same flash/PSRAM footprint Espressif markets as `…-N16R8`, but here on a bare chip, not a WROOM module.

### PlatformIO memory settings (the load-bearing bit)

- PSRAM is **OPI/octal** → the correct Arduino core memory layout is **`board_build.arduino.memory_type = qio_opi`** (quad flash + octal PSRAM). This matches the platformio.ini phase-2 note at `platformio.ini:33`, and follows directly from "8 MB Octal PSRAM + external QSPI flash" above. (derived from datasheet Ordering table p.10)
- Enabling PSRAM also needs `-DBOARD_HAS_PSRAM` (`platformio.ini:34`).
- hughes_bridge currently runs **PSRAM OFF** by choice (`platformio.ini:30-33`): a wrong OPI-PSRAM setting bootloops a board with no UART recovery path. Only turn it on deliberately for the display/LVGL phase.
- Flash size is pinned to 16 MB (`platformio.ini:27-28`); board env is generic `esp32-s3-devkitc-1` (`platformio.ini:21`).

## Native USB (no UART bridge)

There is **no USB-UART bridge chip** on this board. USB D−/D+ land directly on the ESP32-S3's on-chip USB pins:

| Signal | GPIO | Chip pin | Citation |
|---|---|---|---|
| USB_D− | GPIO19 | 25 | IO-alloc CSV row 25 (GPIO19, "USB…D−"); schematic `USB- 25` |
| USB_D+ | GPIO20 | 26 | IO-alloc CSV row 26 (GPIO20, "USB…D+"); schematic `USB+ 26` |

GPIO19/20 are the chip's integrated-PHY USB pins, multiplexed between the **USB Serial/JTAG controller** and **USB 2.0 OTG full-speed** interface via IO MUX (`4-DataSheet/esp32-s3_datasheet_en.pdf` p.54 "Pin Assignment"; p.53 §4.2.1.7). By default D+/D− bind to the USB Serial/JTAG controller (datasheet p. ~34, line "USB_D+/- … by default, connected to the USB Serial/JTAG Controller").

Practical consequences for firmware:
- Serial console = **USB-CDC over the S3's native USB**. Build with **`-DARDUINO_USB_CDC_ON_BOOT=1`** or the monitor is silent (`platformio.ini:37`).
- Enumerates as USB **VID:PID `303A:1001`** (Espressif; LEDGER confirmed on real hardware — not a pack source).
- Because there's no auto-reset UART chip, the **first flash may need manual BOOT+RESET** button entry into download mode (LEDGER; see strapping below).

## Strapping / boot pins and their board wiring

ESP32-S3 samples 4 strapping pins at reset (`4-DataSheet/esp32-s3_datasheet_en.pdf` §3 p.29). Board wiring of each (from IO-alloc CSV + schematic):

| Strap | Datasheet default | Controls | Board net / use | Citation |
|---|---|---|---|---|
| **GPIO0** | Weak pull-up (=1) | Boot mode (with GPIO46) | **BOOT button** ("download key circuit"), `IO0` | datasheet Table 3-1/3-3 p.29-30; IO-alloc CSV row 5 (GPIO0 "BOOT button", not broken out); schematic "Download key circuit"/`IO0`, `KEY1` |
| **GPIO46** | Weak pull-down (=0) | Boot mode secondary + ROM-print | **LCD_RS / TFT D/C** (high=data, low=command) | datasheet §3 p.29-30; IO-alloc CSV row 52 (IO46 = LCD command/data select); schematic `GPIO46 → LCD_RS 52`; `platformio.ini:82` `TFT_DC=46` |
| **GPIO45** | Weak pull-down (=0) | VDD_SPI voltage select | **LCD backlight** (active-HIGH) | datasheet Table 3-4 p.30; IO-alloc CSV row 51 (IO45 = backlight, high = on); `platformio.ini:84` `TFT_BL=45` |
| **GPIO3** | Floating | JTAG signal source | **Broken out**, general-purpose IO | datasheet §3 p.29; IO-alloc CSV row 8 (GPIO3 "可做普通IO口使用" = usable general IO) |

Boot-mode truth table (`4-DataSheet/esp32-s3_datasheet_en.pdf` Table 3-3 p.30):

| Boot mode | GPIO0 | GPIO46 |
|---|---|---|
| **SPI Boot** (normal run) | 1 (default) | any |
| **Joint Download Boot** | 0 | 0 |

Download boot supports USB-Serial-JTAG, USB-OTG, and UART0 download (datasheet p.30). To force download on a first/bricked flash: hold **BOOT (GPIO0) low**, pulse **RESET (CHIP_PU) low**, release RESET, release BOOT.

**Board interaction notes (derived):**
- **GPIO45 must be low at reset** to keep VDD_SPI at its default 3.3 V (matching ESP32-S3R8). The backlight is active-HIGH, so at reset it is naturally low (weak pull-down) — the display simply comes up dark until firmware drives it. No conflict.
- **GPIO46 is the TFT D/C line.** It is weak-pull-down (=0) at reset; boot mode is decided by GPIO0 (=1 → SPI boot) regardless, so reusing it for D/C is safe. It also gates ROM-message printing, a non-issue at runtime.
- UART0 (GPIO43 TXD / GPIO44 RXD) is broken out (IO-alloc CSV rows 49-50) but is **not** the programming path — native USB is. No auto-DTR/RTS reset exists.

## Reset (CHIP_PU / EN)

- **CHIP_PU (chip pin 4)** is the active-low reset for both the ESP32-S3 and the LCD. IO-alloc CSV row 4: "复位ESP32和液晶屏，低电平复位" (resets ESP32 **and** the LCD panel; low = reset).
- A physical **RESET button** ("Key reset circuit") pulls CHIP_PU low. Schematic: `CHIP_PU` net + "Key reset circuit"; `RESET`, `R10 10K`, `C2 105`.
- Board follows Espressif's recommended **RC power-on/reset delay of R = 10 kΩ, C = 1 µF** on CHIP_PU (`4-DataSheet/esp32-s3_hardware_design_guidelines_en.pdf` §1.3 p.~9; schematic shows 10 K + `105`=1 µF at CHIP_PU). CHIP_PU must never float.

## ADC (units, channels, attenuation) — battery-relevant

ESP32-S3 has **two 12-bit SAR ADCs, up to 20 channels total** (`4-DataSheet/esp32-s3_datasheet_en.pdf` feature list p.4, "Two 12-bit SAR ADCs, up to 20 channels"). ADC1 = channels on GPIO1–10, ADC2 = channels on GPIO11–20 (per IO-alloc CSV `ADC1_CHx` / `ADC2_CHx` annotations).

**Battery ADC on this board = GPIO9 = ADC1_CH8.** IO-alloc CSV row 14 (GPIO9): "电池电量ADC值读取引脚" (battery-level ADC read pin), annotated `ADC1_CH8`. Schematic net `BAT_ADC` → GPIO9 (chip pin 14), fed from BAT+ through a resistor divider (schematic shows `200K` in the "Battery level" block). Using **ADC1** matters: ADC2 is shared with Wi-Fi/USB and is unreliable while the radio is active — GPIO9 sits on ADC1, so battery reads survive Wi-Fi. (derived from unit assignment above)

**Attenuation → effective input range** (`4-DataSheet/esp32-s3_datasheet_en.pdf` Table 5-6 ADC Calibration Results p.65):

| Attenuation | Effective measurement range | Typical total error |
|---|---|---|
| ATTEN0 (0 dB) | 0 – 850 mV | ±5 mV |
| ATTEN1 (2.5 dB) | 0 – 1100 mV | ±5 mV |
| ATTEN2 (6 dB) | 0 – 1600 mV | ±6 mV |
| ATTEN3 (11 dB) | 0 – 2900 mV | ±10 mV |

Other ADC characteristics (`4-DataSheet/esp32-s3_datasheet_en.pdf` Table 5-5 p.65): DNL ±4 LSB, INL ±8 LSB, sampling rate up to 100 kSPS, measured with an external 100 nF cap on the ADC pin. For a full-scale reading of a battery near 4.24 V (Prod Spec §3.5 p.6 lists 4.24 V charge / 3.7 V nominal), the ~2:1 `200K` divider brings it under the ATTEN3 ~2900 mV window. (derived)

## RTC / low-power

- **RTC SRAM: 16 KB**, retained in Deep-sleep (Prod Spec §3.1 p.5; `4-DataSheet/esp32-s3_datasheet_en.pdf` p.5 "RTC memory remains powered on in Deep-sleep mode").
- Four power modes (Active / Modem-sleep / Light-sleep / Deep-sleep); Deep-sleep as low as 7 µA (`4-DataSheet/esp32-s3_datasheet_en.pdf` p.5, lines 176-177). Two ULP coprocessors present but unused by this firmware.
- Many board GPIOs are RTC-capable (`RTC_GPIOx` in IO-alloc CSV) — relevant only if wake-on-pin is ever needed.
- **Brownout detector:** the ESP32-S3 has an on-chip brownout detector, but it is documented in the *Technical Reference Manual*, not the datasheet or this board's schematic; no board-specific brownout threshold circuit appears in `5-Schematic/*`. hughes_bridge relies on the chip default. (Not found as a board-specific fact in the mined pack — treat as chip-generic.)

## Pin-function quick map (core-relevant, from IO-alloc CSV)

| GPIO | Chip pin | Board function | Broken out? |
|---|---|---|---|
| CHIP_PU | 4 | Reset (ESP32 + LCD), active-low | N |
| GPIO0 | 5 | BOOT button (strap) | N |
| GPIO3 | 8 | General IO (JTAG strap, floating) | **Y** |
| GPIO9 | 14 | **Battery ADC (ADC1_CH8)** | N |
| GPIO19 | 25 | USB D− | N |
| GPIO20 | 26 | USB D+ | N |
| GPIO26–37 | — | In-package OPI PSRAM + external QSPI flash bus | N |
| GPIO43 / 44 | 49 / 50 | UART0 TXD / RXD | **Y** |
| GPIO45 | 51 | LCD backlight (strap = VDD_SPI) | N |
| GPIO46 | 52 | LCD D/C (strap) | N |

(All rows: IO-allocation CSV, rows keyed by chip pin number.)

## Conflicts & caveats

- **No hard source conflicts** were found for the core-MCU facts. The only nuance is naming: the repo says "N16R8" while the schematic labels the chip `ESP32-S3R8` — these are consistent (R8 die + 16 MB external flash), documented above as *derived*, not a conflict.
- Reminder per project policy: demo header comments elsewhere in the pack list wrong pins — none were used here. Every pin above traces to the IO-allocation CSV, the schematic, or the ESP32-S3 datasheet.
