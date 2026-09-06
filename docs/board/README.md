# Board Reference — ES3C28P / ES3N28P (2.8" IPS ESP32-S3, ILI9341V)

Distilled, cited engineering reference for the LCDWIKI/Hosyond 2.8" ESP32-S3 display module
(`ES3C28P` / `ES3N28P`, [lcdwiki](https://www.lcdwiki.com/2.8inch_ESP32-S3_Display)) that runs
`hughes_bridge`. Every fact here was mined from the vendor hardware pack by a ten-agent swarm and
traced to a primary source. This folder exists because **the vendor demo `.ino` header comments list
the wrong pins** (copy-pasted ESP32-WROOM-32E/classic maps); these docs replace those comments with
schematic-verified truth.

## How to use this folder

1. Need a pin? Use the **[Consolidated Authoritative Pinout](#consolidated-authoritative-pinout)** below — it is
   the single cross-checked source of truth. Anything marked `!!` is disputed; read the cited conflict.
2. Need subsystem depth (registers, init sequences, timing, APIs)? Open the linked subsystem doc.
3. **Never trust a demo `//pin usage` header comment.** They are wrong for this board. Trust the
   IO-allocation sheet, the schematic nets, and the actual `#define`s / `Replaced files/User_Setup.h`.

### Source-authority ranking (highest first)

1. **IO-allocation sheet** — `5-Schematic/ESP32-S3_Chip_IO_Resource_Allocation.xlsx` (Chinese, converted
   to CSV). The definitive GPIO→function map. Cited as "IO CSV".
2. **Schematic nets** — `5-Schematic/2.8inch_ESP32-S3_Display_*Schematic.pdf` (EN + CN twins, identical circuit).
3. **Device datasheets** — `4-DataSheet/*` (ILI9341V, FT6336G, ES8311, TP4054, WS2812B-V5/W, etc.) and
   `2-Specification/*` vendor init/spec text.
4. **Vendor demo `#define`s** — `1-Demo/Arduino/.../Replaced files/User_Setup.h`, `ESP_Panel_Board_Custom.h`,
   `touch.h`, `spi_dev.h`. Authoritative for pins; **but demo `.ino` header *comments* are junk (rank ∞, ignore).**

Where a lower tier disagrees with a higher tier, the higher tier wins **except** the audio I2S data
direction, where the two rank-1 sources and the (hardware-tested) demo code disagree — see
[Conflicts](#conflicts--open-questions).

---

## Summary table

| Subsystem doc | What it covers |
|---|---|
| [./pinout-io.md](./pinout-io.md) | Master 46-row GPIO map; the 8 header pins (only 4 truly free); strapping/reserved buses; the full "wrong demo comment" list. |
| [./esp32-s3-core.md](./esp32-s3-core.md) | Bare ESP32-S3R8 chip (not WROOM): 8 MB OPI PSRAM + 16 MB ext QSPI flash; native USB; strapping/boot/download; CHIP_PU reset; ADC units/atten. |
| [./power-battery-charging.md](./power-battery-charging.md) | TP4054 charger (no firmware control), battery ADC on GPIO9, discrete USB↔battery UPS power path, two ME6217 3.3 V LDOs. |
| [./display-ili9341v.md](./display-ili9341v.md) | ILI9341V pins, SPI host/clock, **IPS inversion (0x21) required**, MADCTL/rotation, RGB565, LEDC backlight, full power-on init. |
| [./touch-ft6336.md](./touch-ft6336.md) | FT6336G cap-touch: I2C 0x38 on shared bus, INT/RST wiring, register map, reset timing, power modes, coordinate/rotation mapping. |
| [./audio-es8311.md](./audio-es8311.md) | ES8311 codec (I2C 0x18, I2S slave 16 kHz/384×), FM8002E/SC8002B amp (EN=GPIO1 active-LOW), analog MEMS mic, init registers. |
| [./sd-storage.md](./sd-storage.md) | microSD on native **SDMMC/SDIO 4-bit**, own bus (no LCD contention), pins, on-board pull-ups, no card-detect GPIO. |
| [./status-led-ws2812.md](./status-led-ws2812.md) | Single WS2812B-V5/W on GPIO42; GRB 24-bit (no white); count=1; 5 V VDD w/ 3.3 V data; timing. |
| [./peripherals-misc.md](./peripherals-misc.md) | Internal RTC (not battery-backed), hw timer (core 3.x API), BOOT button GPIO0, UART0 43/44, WiFi/BLE, SMD chip antenna. |
| [./sample-code-index.md](./sample-code-index.md) | Catalogue of all 30 vendor Arduino examples + the 4 `Replaced files` overrides; which example references which subsystem. |

---

## Consolidated Authoritative Pinout

Single cross-checked GPIO map, built from `pinout-io.md` and reconciled against every subsystem doc.
`Hdr` = broken out to a user header. **`!!` = two docs disagree on this pin — see the cited conflict.**
Sources agree on all pin *numbers*; the only genuine cross-doc disagreement is the I2S data *direction*.

| GPIO | Function | On-board net | Hdr | Source |
|---|---|---|---|---|
| CHIP_PU (pin 4) | RESET (chip enable, active-low); also resets LCD | `CHIP_PU` | N | IO CSV r3; core, display (LCD `RST=-1` shares this) |
| GPIO0 | BOOT strapping / BOOT button (active-low) | `IO0`/`KEY1` | N | IO CSV r4; core, peripherals |
| GPIO1 | Audio power-amp **enable, active-LOW** (drive LOW = amp on) | `AUDIO_EN` → FM8002E/SC8002B pin1 | N | IO CSV r5; audio (`AP_ENABLE 1`) |
| GPIO2 | Free general-purpose IO (ADC1_CH1/TOUCH2) | `IO2` | **Y** | IO CSV r6 |
| GPIO3 | Free GPIO **but JTAG-select strapping** — no boot-time pull | `IO3` | **Y** | IO CSV r7; core |
| GPIO4 | I2S **MCLK** | `I2S_MCK` | N | IO CSV r8; audio (`I2S_MCK 4`) |
| GPIO5 | I2S **BCLK/SCLK** | `I2S_SCK` | N | IO CSV r9; audio (`I2S_BCK 5`) |
| **!!** GPIO6 | I2S audio data — **DOUT per schematic+CSV / DIN per demo code** | `I2S_DO` | N | IO CSV r10 = "output"; audio §2 & pinout conflict #3 (demo `.din=6`) |
| GPIO7 | I2S **WS/LRCK** | `I2S_LRC` | N | IO CSV r11; audio (`I2S_WS 7`) |
| **!!** GPIO8 | I2S audio data — **DIN per schematic+CSV / DOUT per demo code** | `I2S_DI` | N | IO CSV r12 = "input"; audio §2 & pinout conflict #3 (demo `DOUT=8`) |
| GPIO9 | **Battery-voltage ADC** (ADC1_CH8), ÷2 divider | `BAT_ADC` | N | IO CSV r13; power, core (`ADC_CHANNEL_8`) |
| GPIO10 | **LCD CS** (active-low) | ILI9341V CS | N | IO CSV r14; display (`TFT_CS 10`) |
| GPIO11 | **LCD SPI MOSI** | ILI9341V SDI | N | IO CSV r15; display (`TFT_MOSI 11`) |
| GPIO12 | **LCD SPI SCLK** | ILI9341V | N | IO CSV r17; display (`TFT_SCLK 12`) |
| GPIO13 | **LCD SPI MISO** | ILI9341V SDO | N | IO CSV r18; display (`TFT_MISO 13`) |
| GPIO14 | Free general-purpose IO (ADC2_CH3) | `IO14` | **Y** | IO CSV r19 |
| GPIO15 | **I2C SCL** (touch **+** audio codec, shared) | `IIC_SCL` | **Y** (shared) | IO CSV r20-22; touch, audio |
| GPIO16 | **I2C SDA** (touch **+** audio codec, shared) | `IIC_SDA` | **Y** (shared) | IO CSV r23-25; touch, audio |
| GPIO17 | **Touch INT** (active-low) | FT6336 INT | N | IO CSV r26; touch (`TOUCH_FT6336_INT 17`) |
| GPIO18 | **Touch RST** (active-low) | FT6336 RST | N | IO CSV r27; touch (`TOUCH_FT6336_RST 18`) |
| GPIO19 | **USB D−** | USB-C | N | IO CSV r28; core |
| GPIO20 | **USB D+** | USB-C | N | IO CSV r29; core |
| GPIO21 | Free general-purpose IO | `IO21` | **Y** | IO CSV r30 |
| GPIO26 | In-package OPI PSRAM CS (SPICS1) | PSRAM | N | IO CSV r31 |
| GPIO27 | Flash/PSRAM D3 | Flash+PSRAM | N | IO CSV r32 |
| GPIO28 | Flash/PSRAM D2 | Flash+PSRAM | N | IO CSV r34 |
| GPIO29 | External QSPI flash CS | Flash | N | IO CSV r36 |
| GPIO30 | Flash/PSRAM CLK | Flash+PSRAM | N | IO CSV r37 |
| GPIO31 | Flash/PSRAM D1 | Flash+PSRAM | N | IO CSV r39 |
| GPIO32 | Flash/PSRAM D0 | Flash+PSRAM | N | IO CSV r41 |
| GPIO33 | PSRAM D4 | OPI PSRAM | N | IO CSV r45 |
| GPIO34 | PSRAM D5 — **NOT the battery ADC** (see conflict) | OPI PSRAM | N | IO CSV r46; power/pinout |
| GPIO35 | PSRAM D6 | OPI PSRAM | N | IO CSV r47 |
| GPIO36 | PSRAM D7 | OPI PSRAM | N | IO CSV r48 |
| GPIO37 | PSRAM DQS/DM | OPI PSRAM | N | IO CSV r49 |
| GPIO38 | **microSD CLK** (SDIO) | `SD_CLK` | N | IO CSV r50; sd (`SD_SCK 38`) |
| GPIO39 | **microSD DATA0** | `SD_D0` | N | IO CSV r51; sd (`SD_D0 39`) |
| GPIO40 | **microSD CMD** | `SD_CMD` | N | IO CSV r52; sd (`SD_CMD 40`) |
| GPIO41 | **microSD DATA1** | `SD_D1` | N | IO CSV r53; sd (`SD_D1 41`) |
| GPIO42 | **WS2812 RGB LED** (single-wire DIN) | `RGB_LED` | N | IO CSV r54; led (`LED_PIN 42`) |
| GPIO43 | **UART0 TXD** (499R series) | `TXD0`/`U0TXD` | **Y** | IO CSV r55; peripherals |
| GPIO44 | **UART0 RXD** (100R series) | `RXD0`/`U0RXD` | **Y** | IO CSV r56; peripherals |
| GPIO45 | **LCD backlight, active-HIGH** (also VDD_SPI strapping) | backlight | N | IO CSV r57; display (`TFT_BL 45`) |
| GPIO46 | **LCD DC/RS** (H=data,L=cmd; also strapping) | `LCD_RS` | N | IO CSV r58; display (`TFT_DC 46`) |
| GPIO47 | **microSD DATA3** | `SD_D3` | N | IO CSV r44; sd (`SD_D3 47`) |
| GPIO48 | **microSD DATA2** | `SD_D2` | N | IO CSV r43; sd (`SD_D2 48`) |

**Only 8 GPIOs reach a header; only 4 are cleanly free:** GPIO2, GPIO14, GPIO21, and GPIO3 (but GPIO3 is a
strapping pin). GPIO15/16 (shared I2C) and GPIO43/44 (UART0) are free only if those buses are unused.
LCD panel **RST has no dedicated GPIO** — it is tied to CHIP_PU, so firmware uses `RST=-1`.

---

## Conflicts & open questions

Aggregated from all ten miners plus reconciliation notes.

### Hard conflicts (verify before trusting)

- **`!!` I2S data direction, GPIO6 vs GPIO8 — the one open hardware question.** Two rank-1 sources
  (IO CSV: GPIO6="sends/output", GPIO8="receives/input"; schematic nets `I2S_DO`=6, `I2S_DI`=8) say
  **DOUT=GPIO6 / DIN=GPIO8**. The vendor demo code (`music.ino:87` `DOUT=8`; `echo.ino:80-81`
  `dout=GPIO8, din=GPIO6`) says the **opposite**. Pin *numbers* are agreed; only the direction label is
  disputed. The demos are presumably hardware-tested, so **DOUT=8/DIN=6 may be what actually works** —
  this is the one audio pin to confirm empirically on the board. (pinout-io conflict #3, audio §2)

- **Display Function Control (0xB6) init bytes differ between two vendor files.** `ILI9341V_Init.txt` /
  `Simple_test.ino` use `0x0A,0xA2` (spec path, higher authority); TFT_eSPI `Replaced files/ILI9341_Init.h`
  uses `0x08,0x82`. Either boots the panel; use `0x0A,0xA2` to match spec. (display §6)

- **FT6336 power-mode count.** Datasheet prose names 3 modes (Active/Monitor/Hibernation); register
  `0xA5 ID_G_PMODE` enumerates 4, adding an undocumented `P_STANDBY` (0x02). Treat P_STANDBY as
  register-only. (touch §5)

### Copy-paste "junk comment" conflicts (all resolved — comment is always wrong)

- **Battery ADC:** `Example_13` comment says `BAT_VOLT_ADC=34` (and `BL=27`). **Wrong** — battery ADC is
  **GPIO9 = ADC1_CH8** (the sketch itself uses `ADC_CHANNEL_8`); backlight is GPIO45. GPIO34 is PSRAM and
  cannot be an ADC input. All docs agree on GPIO9. (power, pinout, core, sample-index)
- **LCD pins:** `Example_01/13` + most demos comment `DC=2, RESET=15, CS=15, MOSI=13, SCK=14, MISO=12,
  BL=21/27`. **Wrong** — real values DC=46, RST=-1, CS=10, MOSI=11, SCK=12, MISO=13, BL=45
  (`User_Setup.h`/`spi_dev.h`). (display, pinout, sample-index)
- **SD:** `Example_05` comment implies SPI SD with `SD_CS=7`. **Wrong** — SD is SDIO 4-bit (no CS);
  GPIO7 is an audio I2S pin. (sd)
- **Touch:** `Example_08_LVGL` comment lists resistive XPT2046 pins (`RTP_*`). **Wrong** — board is
  capacitive FT6336 on I2C 15/16 + INT 17 / RST 18. (sample-index)
- **Module identity:** Examples 07/11/22/23/24 name "ESP32-WROOM-32E" with `RESET=ESP32-EN`. **Wrong** —
  bare ESP32-S3R8; reset is CHIP_PU / `TFT_RST=-1`. (peripherals, core, sample-index)

### Non-blocking / derived notes

- **SPI overclock:** ILI9341V datasheet write-clock max ≈10 MHz, read ≈6.6 MHz; vendor runs 40 MHz
  (TFT_eSPI) and 80 MHz (bare demo) — normal for these modules, but above spec. (display §2)
- **IPS colour inversion is required.** Stock upstream TFT_eSPI does **not** invert; inversion comes from
  the replaced `ILI9341_Init.h` (`0x21`) or `-DTFT_INVERSION_ON`. Without it every colour is complemented. (display §3)
- **WS2812 is 3-channel GRB (no white channel); count = 1.** `Example_06` uses `NEO_GRBW` and both demos
  hard-code `LED_COUNT 60` — both are unmodified Adafruit defaults. Use `NEO_GRB`, count 1. (led)
- **Amp part naming:** datasheet is FM8002E; board silk reads SC8002B — treated as pin-compatible SOP-8
  BTL amps (equivalence derived, not stated in pack). (audio §4)
- **FT6336 Monitor current** printed "220 mA" in datasheet Table 3-2; context indicates ~220 µA (likely a
  unit typo). (touch §5)
- **No charge status / no USB-present GPIO.** TP4054 `CHRG` dead-ends into a 10k pull-down; VBUS-present
  reaches no pin. Firmware can only infer charge state from the GPIO9 battery-voltage trend. (power b/d)
- **Unreadable-source workarounds (no data lost):** several `5-Schematic/*.pdf` could not be image-rendered
  (no poppler); text was recovered via `pdftotext`/`pypdf`. `TP4054.PDF` and `WS2812B` PDFs were image-only
  scans, rasterized and read as images. Chinese CID-font cells came from the pre-converted IO-allocation CSV.

---

## Reconciliation with current firmware

Checked against [`../ARCHITECTURE.md`](../ARCHITECTURE.md) and `platformio.ini [display]`.

### CONFIRMED

- **Display pins** (CS 10 / DC 46 / RST -1 / SCLK 12 / MOSI 11 / MISO 13 / BL 45 active-HIGH) — the
  firmware's `[display]` flags and the ARCHITECTURE pin table both match the mined authority exactly.
- **`-DTFT_INVERSION_ON=1`** — CONFIRMED necessary. The display doc proves this IPS panel needs inversion
  (`0x21`), and the vendor `User_Setup.h` omits it. The firmware correctly adds it.
- **SD on its own SDMMC/SDIO 4-bit bus** (CLK 38 / CMD 40 / D0 39 / D1 41 / D2 48 / D3 47), no LCD
  contention — CONFIRMED by IO CSV + schematic + demo. Matches ARCHITECTURE's "SD on its OWN bus" claim.
- **NeoPixel on GPIO42** — CONFIRMED (schematic + IO CSV). Firmware drives it as the status LED.
- **Touch FT6336 SDA 16 / SCL 15 / INT 17 / RST 18** — CONFIRMED (a rare case where even the demo header
  comment is right). Matches ARCHITECTURE.
- **Native USB, no UART bridge; `-DARDUINO_USB_CDC_ON_BOOT=1` required; N16R8 = ESP32-S3R8 + 16 MB flash;
  OPI PSRAM → `memory_type=qio_opi`** — all CONFIRMED by the core doc.
- **`USE_HSPI_PORT` → SPI3_HOST at 40 MHz** — CONFIRMED by the display doc's host-mapping analysis.

### CONTRADICTS / firmware may have stale info

- **`platformio.ini` line 72 comment says "no colour inversion"** but line 77 correctly sets
  `-DTFT_INVERSION_ON=1`. The stale comment is **wrong** (contradicts both the working flag and the display
  doc, which shows inversion is required). The flag is right; fix the comment.
- **ARCHITECTURE labels the LCD bus "FSPI, ≤80 MHz."** That describes the *bare vendor register demo*
  (SPI2/FSPI, 80 MHz). The actual firmware build uses `-DUSE_HSPI_PORT=1`, which maps to **SPI3_HOST at
  40 MHz** — not FSPI, not 80 MHz. The "safe 40 MHz" figure in the same table is correct; the "FSPI/≤80 MHz"
  label is misleading for this firmware.

### Missing from firmware docs (present on board, not yet recorded)

- **Battery-voltage ADC = GPIO9 (ADC1_CH8), ÷2 divider, atten 12 dB, ×2 in code.** ARCHITECTURE mentions a
  "battery-voltage divider (out of scope)" but does not record the pin. When phase-2 telemetry wants it, use
  GPIO9/ADC1 (ADC1 survives Wi-Fi). Charging is autonomous (TP4054) and cannot be gated in firmware.
- **Audio (relevant to the `[audiotest]` env):** ES8311 codec I2C 0x18 on the **shared** GPIO15/16 bus;
  I2S MCLK 4 / BCLK 5 / WS 7; amp EN = **GPIO1 active-LOW**; **I2S data pins 6/8 direction is unresolved —
  verify on hardware** (see conflicts). The audio codec shares the touch I2C bus, so the audiotest env's
  goal of proving touch survives on that shared bus is well-founded.
