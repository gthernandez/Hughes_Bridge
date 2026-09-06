# Display — ILI9341V (2.8" IPS, 240×320)

Permanent reference for the ILI9341V LCD controller on the ES3C28P / ES3N28P ESP32-S3 board: pins, SPI host, clock, colour inversion, MADCTL/rotation, pixel format, backlight, and the exact vendor power-on init sequence.

## Sources mined
- `4-DataSheet/ILI9341V_DataSheet.pdf` (controller datasheet, 249 pp.)
- `2-Specification/ILI9341V_Init.txt` (vendor bare-metal init sequence)
- IO-allocation CSV (converted from `5-Schematic`): `scratchpad/board_csv/io_alloc_sheet1.csv`
- `1-Demo/Arduino/Demo/Example_01_Simple_test` (`Simple_test.ino`, `spi_dev.h`) — bare register-level driver
- `1-Demo/Arduino/Demo/Example_14_Backlight_PWM/Backlight_pwm_test/Backlight_pwm_test.ino` — LEDC backlight
- `1-Demo/Arduino/Replaced files/User_Setup.h`, `ILI9341_Init.h`, `TFT_eSPI_ESP32_S3.c` (TFT_eSPI overrides)

---

## 1. Pin map (VERIFIED against schematic CSV + actual `#define`s)

All project "known facts" are **CONFIRMED**. Pins are proven by two independent authoritative sources: the schematic-derived IO-allocation CSV and the actual `#define`s in `spi_dev.h` / `User_Setup.h` (not header comments).

| Signal | GPIO | Active/Note | CSV citation | Code citation |
|---|---|---|---|---|
| LCD CS    | **GPIO10** | active LOW | CSV row 14 (GPIO10): "液晶屏片选引脚，低电平使能" | `Example_01…/spi_dev.h:24` (`LCD_CS 10`); `User_Setup.h:215` (`TFT_CS 10`) |
| LCD MOSI (SDI) | **GPIO11** | SPI write data | CSV row 15/16 (GPIO11): "液晶屏SPI总线写数据引脚" | `spi_dev.h:30` (`SPI_MOSI 11`); `User_Setup.h:213` (`TFT_MOSI 11`) |
| LCD SCLK  | **GPIO12** | SPI clock | CSV row 17 (GPIO12): "液晶屏SPI总线时钟引脚" | `spi_dev.h:31` (`SPI_SCLK 12`); `User_Setup.h:214` (`TFT_SCLK 12`) |
| LCD MISO (SDO) | **GPIO13** | SPI read data | CSV row 18 (GPIO13): "液晶屏SPI总线读数据引脚" | `spi_dev.h:29` (`SPI_MISO 13`); `User_Setup.h:212` (`TFT_MISO 13`) |
| LCD DC (RS) | **GPIO46** | HIGH=data, LOW=command | CSV row 58 (IO46): "液晶屏命令/数据选择控制引脚，高电平：数据；低电平：命令" | `spi_dev.h:26` (`LCD_DC 46`); `User_Setup.h:216` (`TFT_DC 46`) |
| LCD RST   | **not on a GPIO / tied to chip reset** — use `-1` | — | CSV row 3 (CHIP_PU): "复位ESP32和液晶屏" (LCD reset shares the ESP32 CHIP_PU/RESET line) | `spi_dev.h:25` (`LCD_RST -1`); `User_Setup.h:218` (`TFT_RST -1`) |
| Backlight (BL) | **GPIO45** | active HIGH (HIGH = backlight on) | CSV row 57 (IO45): "液晶屏背光控制引脚，高电平点亮背光" | `spi_dev.h:27` (`LCD_BL 45`); `User_Setup.h:132-133` (`TFT_BL 45`, `TFT_BACKLIGHT_ON HIGH`) |

**RST detail:** there is no dedicated GPIO for the panel reset. The panel RST is driven by the board-level ESP32 reset (CHIP_PU), so firmware sets `TFT_RST = -1` / `LCD_RST = -1` and never toggles it. In `Example_01` the software reset lines in `Lcd_Init()` are commented out (`Simple_test.ino:112-117`) and it relies on the controller's power-on reset instead.

> WRONG-COMMENT WARNING (do not trust these): `Example_01_Simple_test/Simple_test.ino:10-11` header comment lists `DC/RS 2`, `RESET 15`, `LED 21` — all **wrong** (copy-paste junk). The real values from the same file's `spi_dev.h` are DC=46, RST=-1, BL=45. `Example_14…/Backlight_pwm_test.ino:5-6` also mislabels the SPI columns but its `#define BACKLIGHT_PIN 45` (line 23) is correct.

---

## 2. SPI interface & host

- **Bus:** 4-line SPI (MOSI/MISO/SCLK/CS + DC). Mode 0 (`SPI_MODE0`, CPOL=0/CPHA=0) — `spi_dev.h:16`.
- **On ESP32-S3 the SPI peripheral pins are arbitrary** (GPIO matrix); the CSV notes these are FSPI-capable pads but any routing works.
- **Which host — differs by driver stack:**
  - Bare-metal `Example_01` uses **FSPI = SPI2** (`spi_dev.h:14` `#define SPI_PORT FSPI`), talking directly to `DR_REG_SPI2_BASE` registers.
  - TFT_eSPI is configured with `#define USE_HSPI_PORT` (`User_Setup.h:377`). On the ESP32-S3 branch of `TFT_eSPI_ESP32_S3.c:45-51`, `USE_HSPI_PORT` maps the DMA host to **`SPI3_HOST`** (the `#else` branch would be `SPI2_HOST`). So the TFT_eSPI build drives the LCD on **SPI3**, while the register demo uses **SPI2** — same physical pins either way via the IO mux.
- MOSI/MISO are re-routed through the GPIO matrix with `FSPID_OUT_IDX` / `FSPIQ_IN_IDX` (`TFT_eSPI_ESP32_S3.c:66,77`).
- SPI init in the bare demo: `spi.begin(SPI_SCLK, SPI_MISO, SPI_MOSI, -1)` — CS handled manually, not by the peripheral (`Simple_test.ino:107`).

### Max SPI clock — datasheet spec vs. vendor practice (CONFLICT)
- **Datasheet limit:** 4-line SPI write serial-clock cycle `twc` **min = 100 ns → ~10 MHz max**; read cycle `trc` min = 150 ns → ~6.6 MHz (`ILI9341V_DataSheet.pdf` p.246-247, §18.3.3/18.3.4). SCL H/L pulse widths min 40 ns (write), 60 ns (read).
- **Vendor code runs far above spec:** TFT_eSPI `SPI_FREQUENCY 40000000` (**40 MHz**) with `SPI_READ_FREQUENCY 20000000` (`User_Setup.h:364,369`); the bare register demo pushes `SPI_FREQUENCY 80000000` (**80 MHz**) (`spi_dev.h:15`).
- **Guidance (derived):** 40 MHz is the vendor's proven write clock for graphics — use it. It is ~4× the datasheet's 10 MHz write limit, which is normal for ILI9341 modules but means the safe/spec-guaranteed ceiling for a marginal panel is lower. Reads must be slower (20 MHz used; datasheet spec is ~6.6 MHz). Confirms the project's "~40 MHz" fact.

---

## 3. Colour inversion — IPS panel REQUIRES inversion ON (CONFIRMED)

The vendor init issues **command `0x21` (DINVON, Display Inversion ON)** near the top of every init variant. This is the IPS-panel inversion the project expects.

- Datasheet: `0x21` = "DINVON (Display Inversion ON) … Every bit is inverted from the frame memory to the display" (`ILI9341V_DataSheet.pdf` p.108, §8.2.16). Default after power-on/reset is inversion **OFF**, so it must be set explicitly.
- Vendor bare init: `LCD_WR_REG(0x21)` (`ILI9341V_Init.txt:93`); `Example_01` `LCD_Write_Reg(0x21)` (`Simple_test.ino:162`).
- TFT_eSPI: the **custom** `ILI9341_Init.h` in *Replaced files* adds `writecommand(0x21);` (`ILI9341_Init.h:53`) — the stock upstream ILI9341 init does **not** invert. `User_Setup.h` leaves both `TFT_INVERSION_ON`/`_OFF` commented (`User_Setup.h:116-117`), i.e. inversion is handled by the replaced init header, **not** by the `TFT_INVERSION_ON` macro.

**Firmware implication:** if you use plain upstream TFT_eSPI without the vendor's replaced `ILI9341_Init.h`, colours will be inverted — you must either drop in the replaced init header, add `#define TFT_INVERSION_ON`, or call `tft.invertDisplay(true)` after `begin()`.

---

## 4. Pixel format & MADCTL / rotation

### Pixel format (COLMOD 0x3A = 0x55 → RGB565, 16 bpp)
- Init writes `0x3A` param `0x55` (`ILI9341V_Init.txt:102-103`; `Simple_test.ino:171-172`; `ILI9341_Init.h:62-63`).
- `0x55` → DPI=`101` and DBI=`101` = **16 bits/pixel** for both RGB and MCU interface (`ILI9341V_DataSheet.pdf` p.136, §8.2.33 COLMOD table). This is RGB565.

### MADCTL (0x36) — colour order and rotation
- Panel is **BGR** colour order: init writes `0x36 = 0x08` (bit3 BGR=1) as the base orientation (`ILI9341V_Init.txt:95-96`; datasheet MADCTL bit map p.129, §8.2.29: MY/MX/MV/ML/BGR/MH).
- Rotation table from the vendor `LCD_direction()` (`ILI9341V_Init.txt:14-44`), all with BGR=1:

| dir | Orientation | W×H | `0x36` value | Bits |
|---|---|---|---|---|
| 0 | Portrait          | 240×320 | **0x08** | BGR |
| 1 | Landscape         | 320×240 | **0x68** | BGR·MX·MV |
| 2 | Portrait flipped  | 240×320 | **0xC8** | BGR·MX·MY |
| 3 | Landscape flipped | 320×240 | **0xA8** | BGR·MY·MV |

Native panel resolution is **240 (W) × 320 (H)** portrait (`LCD_W 240`, `LCD_H 320`, `ILI9341V_Init.txt:1-2`; datasheet title "240RGB×320"). The vendor demos run **landscape** — `Example_14` calls `my_lcd.setRotation(1)` (`Backlight_pwm_test.ino:45`), i.e. 320×240.

---

## 5. Backlight control (GPIO45, PWM via LEDC)

- Pin **GPIO45, active HIGH** (see §1). Simple on/off: `digitalWrite(45, HIGH)` (`Simple_test.ino:298-299`).
- **PWM dimming** uses the ESP32 LEDC peripheral (`Example_14`, `Backlight_pwm_test.ino`):
  - `freq = 2000` Hz, `resolution = 8` bits (0–255 duty), `channel = 0` (lines 35-37).
  - Attach + write use the **Arduino-ESP32 core 3.x LEDC API**: `ledcAttachChannel(45, 2000, 8, 0)` then `ledcWrite(45, duty)` (lines 59-60, 78) — note `ledcWrite` takes the **pin**, not the channel, in the new API.
- TFT_eSPI does **not** manage brightness: `User_Setup.h:126-133` notes the library only turns BL on at `begin()`; PWM must be done in the sketch.

---

## 6. Power-on init sequence (authoritative vendor transcription)

From `2-Specification/ILI9341V_Init.txt` (identical data bytes in `Example_01/Simple_test.ino:119-224`). Format: `REG` then its data bytes. Comments are the vendor's.

```
Hardware reset (RST tied to chip reset; software RST omitted — RST=-1)

CF  00 C1 30
ED  64 03 12 81
E8  85 00 78
CB  39 2C 00 34 02
F7  20
EA  00 00
C0  13                 ; Power control 1  (VRH)
C1  13                 ; Power control 2  (SAP/BT)
C5  22 35              ; VCOM control 1
C7  BD                 ; VCOM control 2
21                     ; *** DISPLAY INVERSION ON (IPS) ***
36  08                 ; MADCTL: BGR, portrait 240x320
B6  0A A2              ; Display Function Control   (see conflict below)
3A  55                 ; COLMOD: 16 bpp RGB565
F6  01 30              ; Interface Control (MCU)
B1  00 1B              ; Frame Rate Control
F2  00                 ; 3-Gamma disable
26  01                 ; Gamma curve select
E0  0F 35 31 0B 0E 06 49 A7 33 07 0F 03 0C 0A 00   ; +Gamma
E1  00 0A 0F 04 11 08 36 58 4D 07 10 0C 32 34 0F   ; -Gamma
11                     ; Sleep OUT
delay 120 ms
29                     ; Display ON
LCD_direction(USE_HORIZONTAL)   ; final MADCTL per rotation (see §4)
```

Key sleep/timing: **Sleep-out `0x11` then delay 120 ms before Display-ON `0x29`** (`ILI9341V_Init.txt:153-156`).

### Init-sequence conflict: Display Function Control (0xB6) params
- `ILI9341V_Init.txt:98-100` and `Example_01/Simple_test.ino:167-169`: `0xB6 → 0x0A, 0xA2`.
- TFT_eSPI *Replaced files* `ILI9341_Init.h:58-60`: `0xB6 → 0x08, 0x82`.
- Both are the vendor's own files; the bytes differ (scan direction / interval bits). Neither affects colour or basic operation materially; the raw-register + spec path (`0x0A, 0xA2`) is the higher-authority source (rank 3 spec vs rank 4 demo). Use `0x0A, 0xA2` if matching the spec, but either boots the panel.

---

## 7. Quick config summary for firmware

| Parameter | Value | Source |
|---|---|---|
| Controller | ILI9341V, 240×320, RGB565 262K | datasheet title |
| SPI mode | Mode 0 | `spi_dev.h:16` |
| SPI host (TFT_eSPI) | SPI3 (`USE_HSPI_PORT`) | `User_Setup.h:377`, `TFT_eSPI_ESP32_S3.c:47` |
| SPI host (bare) | SPI2 (FSPI) | `spi_dev.h:14` |
| Write clock | 40 MHz (vendor); 10 MHz datasheet spec | `User_Setup.h:364` / datasheet p.246 |
| Read clock | 20 MHz | `User_Setup.h:369` |
| CS / DC / SCLK / MOSI / MISO / RST | 10 / 46 / 12 / 11 / 13 / -1 | `spi_dev.h`, `User_Setup.h` |
| Backlight | GPIO45, active HIGH, LEDC 2 kHz/8-bit | CSV row 57, `Example_14` |
| Colour inversion | **ON** (cmd 0x21) | init files + datasheet p.108 |
| Colour order | BGR (MADCTL bit3) | init `0x36=0x08` |
| Landscape MADCTL | 0x68 (rot 1) / 0xA8 (rot 3) | `ILI9341V_Init.txt` |
