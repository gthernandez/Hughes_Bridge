# SD Card / Storage — ES3C28P / ES3N28P (ESP32-S3 2.8" IPS, ILI9341V)

Permanent reference for the microSD interface on the board driven by hughes_bridge firmware: bus type, width, exact GPIO pins, and bus ownership.

## Sources mined
- **IO-allocation CSV** (converted from `5-Schematic` authority tier): `board_csv/io_alloc_sheet1.csv` — authoritative GPIO→function map.
- `5-Schematic/2.8inch_ESP32-S3_Display_Schematic.pdf` (EN) — MicroSD slot connector + net labels + pull-ups.
- `5-Schematic/2.8inch_ESP32-S3_Display_Module_Hardware_Schematic.pdf` (CN duplicate, identical netlist) — cross-check.
- `1-Demo/Arduino/Demo/Example_05_show_SD_jpg_picture/Show_SD_Jpg/Show_SD_Jpg.ino` — the only demo that actually mounts the card (SD_MMC).
- `1-Demo/Arduino/Demo/Example_07_Flash_DMA_jpg/Flash_Jpg_DMA_test/Flash_Jpg_DMA_test.ino` — decodes JPEG from Flash array, **does not touch SD** (included for completeness; header pins are for an unrelated ESP32-WROOM board).

## Verdict (project finding CONFIRMED)

The SD card is on a **native SDMMC / SDIO** peripheral in **4-bit** mode, on its **own dedicated bus** — it does **not** share the LCD SPI bus. Confirmed independently by the IO-allocation CSV, the schematic netlist, and the vendor demo `#define`s (all three agree).

| Signal | GPIO | ESP32-S3 pkg pin | microSD connector pin | Source |
|--------|------|------------------|-----------------------|--------|
| SD CLK   | **GPIO38** | 43 (GPIO38)       | pin 5  (CLK)     | CSV row `43,IO38` "SD卡SDIO总线时钟引脚"; INO:22 |
| SD CMD   | **GPIO40** | 45 (MTDO)         | pin 3  (CMD)     | CSV row `45,IO40` "SD卡SDIO总线命令引脚"; INO:23 |
| SD D0    | **GPIO39** | 44 (MTCK)         | pin 7  (DATA0)   | CSV row `44,IO39` "SD卡SDIO总线数据DATA0引脚"; INO:24 |
| SD D1    | **GPIO41** | 47 (MTDI)         | pin 8  (DATA1)   | CSV row `47,IO41` "SD卡SDIO总线数据DATA1引脚"; INO:25 |
| SD D2    | **GPIO48** | 36 (SPICLK_N)     | pin 1  (DATA2)   | CSV row `36,GPIO48` "SD卡SDIO总线数据DATA2引脚"; INO:26 |
| SD D3    | **GPIO47** | 37 (SPICLK_P)     | pin 2  (CD/DAT3) | CSV row `37,GPIO47` "SD卡SDIO总线数据DATA3引脚"; INO:27 |

Citations: `IO-allocation CSV rows 43–53 (GPIO38/40/39/41/48/47)`; `.../Show_SD_Jpg.ino:22-27`; schematic net labels `SD_CLK/SD_CMD/SD_D0/SD_D1/SD_D2/SD_D3` at `2.8inch_ESP32-S3_Display_Schematic.pdf p.1` (lines 534–545 of extracted text).

This matches the mnemonic in the task exactly: **CLK 38 / CMD 40 / D0-3 = 39/41/48/47.**

## Bus type & width — how we know

- **SDIO, not SPI.** The CSV describes every one of the six pins as an `SD卡SDIO总线...` ("SD card SDIO bus …") pin. The demo uses the Espressif `SD_MMC` (SDMMC host) driver, not `SD`/SPI: `#include "SD_MMC.h"` and `SD_MMC.begin()` (`Show_SD_Jpg.ino:15,75`). There is **no chip-select (CS) net** on the SD connector — a defining trait of SDIO vs SPI.
- **4-bit width.** All four data lines D0–D3 are routed and are passed to the driver:
  `SD_MMC.setPins(SD_CLK, SD_CMD, SD_D0, SD_D1, SD_D2, SD_D3)` (`Show_SD_Jpg.ino:68`). Arduino-ESP32 `SD_MMC.begin()` called with no `mode1bit` argument (`Show_SD_Jpg.ino:75`) defaults to **4-bit** mode. (derived, from Arduino-ESP32 `SD_MMC.begin(path="/sdcard", mode1bit=false)` default.)

## Own bus vs shared with LCD — how we know

Separate buses. The LCD (ILI9341V) is on its own SPI bus using different GPIOs — `LCD_CS=GPIO10, LCD_MOSI=GPIO11, LCD_SCK=GPIO12, LCD_MISO=GPIO13` (CSV rows 14–18, GPIO10–13, "液晶屏SPI…"). None of those overlap the SD pins (38/40/39/41/48/47). The demo also initializes SD **before** the TFT and they run concurrently (`Show_SD_Jpg.ino:74-83`, comment "Initialise SD before TFT"), which is only possible because they are independent peripherals.

## MicroSD connector (SD_CARD1, "Micro_sd card")

Push-push microSD slot `SD_CARD1`, schematic label "MicroSD card slot interface circuit" (`2.8inch_ESP32-S3_Display_Schematic.pdf p.1`, ext. line 486, 586). Pinout as wired:

| Conn pin | Pad name | Net |
|----------|----------|-----|
| 1 | DATA2   | SD_D2 (GPIO48) |
| 2 | CD/DAT3 | SD_D3 (GPIO47) |
| 3 | CMD     | SD_CMD (GPIO40) |
| 4 | VDD     | VCC3V3 |
| 5 | CLK     | SD_CLK (GPIO38) |
| 6 | VSS     | GND |
| 7 | DATA0   | SD_D0 (GPIO39) |
| 8 | DATA1   | SD_D1 (GPIO41) |
| 9 | NC      | — |

Citation: `2.8inch_ESP32-S3_Display_Schematic.pdf p.1` (extracted lines 477–486).

Notes for firmware:
- **No dedicated card-detect GPIO.** Pin 2 is the standard combined **CD/DAT3**; card detect is not broken out to a separate line, so presence must be inferred by attempting a mount. (derived, from connector pad naming.)
- **Pull-ups present.** The SD interface block carries four 10K pull-ups `R34/R35/R36/R37` (on CMD + data lines, standard SDIO) plus decoupling `C51 104` (`2.8inch_ESP32-S3_Display_Schematic.pdf p.1`, ext. lines 488–493). So external pull-ups are on-board; firmware need not rely solely on internal pulls.
- **VDD = 3.3V** (VCC3V3 rail via the ME6217C33 regulator). SDIO signalling is 3.3V.
- These six GPIOs are **not brought out** to any header (CSV "是否引出 = N" / "没引出" for all six); they are dedicated to the SD slot on this board.

## Strapping / conflict caution

GPIO39/40/41 (MTCK/MTDO/MTDI) and GPIO38 are JTAG pins repurposed for the SD bus — fine, but avoid enabling hardware JTAG while the SD peripheral is in use. (derived.)

## CONFLICT (copy-paste junk header comment — do NOT trust)

`Show_SD_Jpg.ino:9-10` header comment claims an SPI-style wiring:
`// CS 10  DC/RS 2  RESET 15  SDI/MOSI 11  SCK 12  SDO/MISO 13  LED 21  SD_CS 7`.
This is **wrong for the SD card**: it implies an SPI SD with a chip-select on **GPIO7**. Per the IO-allocation CSV, **GPIO7 is an audio I2S signal** (`CSV row 11, GPIO7` "音频I2S采样频率…"), and the actual code below the comment uses `SD_MMC` (SDIO 4-bit) on GPIO38/40/39/41/48/47 (`Show_SD_Jpg.ino:22-27,68`). The comment appears to be leftover from a generic Bodmer TFT_eSPI example. **Cite the `#define`s and schematic, never this comment.** (This is exactly the copy-paste-junk failure mode this doc set exists to guard against.)
