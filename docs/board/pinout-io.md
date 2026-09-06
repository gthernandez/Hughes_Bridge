# Pinout & IO Allocation — ES3C28P / ES3N28P (2.8" IPS ESP32-S3, ILI9341V)

Definitive GPIO map for this board. Every pin's on-board net, function, header-breakout status, and notes — verified against the schematic, the vendor IO-allocation sheet, and the demo `#define`s. **Never trust a demo header-comment pin; see the "Wrong demo comments" section at the bottom.**

**Sources mined**
- IO-allocation sheet (authoritative, Chinese): `5-Schematic/ESP32-S3_Chip_IO_Resource_Allocation.xlsx` — converted to CSV, cited as "IO CSV row N".
- `5-Schematic/2.8inch_ESP32-S3_Display_Module_Hardware_Schematic.pdf` (board schematic, cited "HW-Sch").
- `5-Schematic/2.8inch_ESP32-S3_Display_Schematic.pdf` (LCD/FPC schematic).
- Vendor demo `#define`s: `1-Demo/Arduino/Demo/Example_01_Simple_test/Simple_test/spi_dev.h`, `Example_13_Get_Battery_Voltage`, `Example_16_music/music/ESP_Panel_Board_Custom.h`, `Example_17_echo`, plus `1-Demo/Arduino/Replaced files/User_Setup.h`.

The board is an **ESP32-S3R8** module: octal (OPI) PSRAM in-package + external QSPI flash, so a large block of GPIOs is captured by flash/PSRAM and can never be used. USB, LCD (4-wire SPI), capacitive touch (I2C), audio (I2S + I2C codec/amp), microSD (SDIO), a WS2812 RGB LED, and a Li-ion charger/fuel-gauge are all wired on-board.

## Master GPIO table

`pin#` = ESP32-S3 module physical pin (from the IO sheet). `Hdr` = broken out to a user header (Y/N). Active levels and directions are from the IO sheet unless marked (derived).

| GPIO | pin# | Function / net | On-board device | Hdr | Notes |
|------|------|----------------|-----------------|-----|-------|
| CHIP_PU | 4 | **RESET** (chip enable) | Also drives LCD reset | N | Active-low. Resets ESP32 **and** the LCD together. IO CSV row 3. This is why LCD `RST` is set to `-1` in code (no dedicated GPIO). |
| GPIO0 | 5 | **BOOT** strapping / button | BOOT push-button | N | Strapping pin. Not usable as normal IO. IO CSV row 4. |
| GPIO1 | 6 | Audio power-amp **enable** | SC8002B amp EN | N | Active-low enable per IO sheet (row 5). Amp = `SC8002B` (HW-Sch). Not broken out. |
| GPIO2 | 7 | General-purpose IO | — | **Y** | Free user IO. IO CSV row 6; header net `IO2` (HW-Sch). |
| GPIO3 | 8 | General-purpose IO | — | **Y** | Free user IO, **but also an ESP32-S3 strapping pin** (JTAG select) — avoid holding at boot. IO CSV row 7; header net `IO3` (HW-Sch). |
| GPIO4 | 9 | I2S **MCLK** (master clock) | Audio codec/amp | N | `#define I2S_MCK 4` (Example_16 custom.h:19). IO CSV row 8. |
| GPIO5 | 10 | I2S **BCLK/SCLK** (bit clock) | Audio | N | `#define I2S_BCK 5` (Example_16:20). IO CSV row 9. |
| GPIO6 | 11 | I2S audio-data **OUT** (per IO sheet) | Audio | N | IO sheet calls this the "send/output" data pin (row 10). **Demo names disagree — see conflict #3.** |
| GPIO7 | 12 | I2S **WS/LRCK** (word select) | Audio | N | `#define I2S_WS 7` (Example_16:22). IO CSV row 11. |
| GPIO8 | 13 | I2S audio-data **IN** (per IO sheet) | Audio | N | IO sheet calls this the "receive/input" data pin (row 12). **Demo names disagree — see conflict #3.** |
| GPIO9 | 14 | **Battery-voltage ADC** (ADC1_CH8) | Li-ion divider → `BAT_ADC` | N | Reads pack voltage through a resistor divider. Code uses `ADC_CHANNEL_8` (= GPIO9). IO CSV row 13; `BAT_ADC` net (HW-Sch). Divider ≈ ÷2 (200K/200K, HW-Sch) (derived). |
| GPIO10 | 15 | **LCD CS** (chip select, active-low) | ILI9341V | N | `#define LCD_CS 10` (Example_01 spi_dev.h:24). IO CSV row 14. |
| GPIO11 | 16 | **LCD SPI MOSI** (write data) | ILI9341V | N | `#define SPI_MOSI 11` (spi_dev.h:30); `TFT_MOSI 11` (User_Setup.h:213). IO CSV row 15. |
| GPIO12 | 17 | **LCD SPI SCLK** | ILI9341V | N | `#define SPI_SCLK 12` (spi_dev.h:31); `TFT_SCLK 12`. IO CSV row 17. |
| GPIO13 | 18 | **LCD SPI MISO** (read data) | ILI9341V | N | `#define SPI_MISO 13` (spi_dev.h:29); `TFT_MISO 13`. IO CSV row 18. |
| GPIO14 | 19 | General-purpose IO | — | **Y** | Free user IO. IO CSV row 19; header net `IO14` (HW-Sch). |
| GPIO15 | 21 | **I2C SCL** (touch + audio codec) | FT6336 touch / codec | **Y** (shared) | Only free as GPIO if touch AND audio are unused; otherwise it is the I2C clock. `#define I2C_SCL/TOUCH_FT6336_SCL 15`. IO CSV rows 20–22; net `IIC_SCL` (HW-Sch). |
| GPIO16 | 22 | **I2C SDA** (touch + audio codec) | FT6336 touch / codec | **Y** (shared) | Same sharing rule as GPIO15. `#define I2C_SDA/TOUCH_FT6336_SDA 16`. IO CSV rows 23–25; net `IIC_SDA` (HW-Sch). |
| GPIO17 | 23 | **Touch INT** (active-low interrupt) | FT6336 | N | `#define TOUCH_FT6336_INT 17`. IO CSV row 26. |
| GPIO18 | 24 | **Touch RST** (active-low reset) | FT6336 | N | `#define TOUCH_FT6336_RST 18`. IO CSV row 27. |
| GPIO19 | 25 | **USB D−** | USB-C | N | Native USB. IO CSV row 28. |
| GPIO20 | 26 | **USB D+** | USB-C | N | Native USB. IO CSV row 29. |
| GPIO21 | 27 | General-purpose IO | — | **Y** | Free user IO. IO CSV row 30; header net `IO21` (HW-Sch). |
| GPIO26 | 28 | SPICS1 — **in-package PSRAM CS** | OPI PSRAM | N | Reserved. IO CSV row 31. |
| GPIO27 | 30 | Flash D3 / PSRAM D3 | Flash + PSRAM | N | Reserved. IO CSV row 32. |
| GPIO28 | 31 | Flash D2 / PSRAM D2 | Flash + PSRAM | N | Reserved. IO CSV row 34. |
| GPIO29 | 32 | External QSPI **flash CS** | Flash | N | Reserved. IO CSV row 36. |
| GPIO30 | 33 | Flash CLK / PSRAM CLK | Flash + PSRAM | N | Reserved. IO CSV row 37. |
| GPIO31 | 34 | Flash D1 / PSRAM D1 | Flash + PSRAM | N | Reserved. IO CSV row 39. |
| GPIO32 | 35 | Flash D0 / PSRAM D0 | Flash + PSRAM | N | Reserved. IO CSV row 41. |
| GPIO33 | 38 | PSRAM D4 | OPI PSRAM | N | Reserved. IO CSV row 45. |
| GPIO34 | 39 | PSRAM D5 | OPI PSRAM | N | Reserved. IO CSV row 46. **Not the battery ADC** (see conflict #2). |
| GPIO35 | 40 | PSRAM D6 | OPI PSRAM | N | Reserved. IO CSV row 47. |
| GPIO36 | 41 | PSRAM D7 | OPI PSRAM | N | Reserved. IO CSV row 48. |
| GPIO37 | 42 | PSRAM DQS/DM | OPI PSRAM | N | Reserved. IO CSV row 49. |
| GPIO38 | 43 | **microSD CLK** (SDIO) | microSD | N | `#define SD_SCK 38`. IO CSV row 50. |
| GPIO39 | 44 | **microSD DATA0** | microSD | N | `#define SD_D0 39`. IO CSV row 51. |
| GPIO40 | 45 | **microSD CMD** | microSD | N | `#define SD_CMD 40`. IO CSV row 52. |
| GPIO41 | 47 | **microSD DATA1** | microSD | N | `#define SD_D1 41`. IO CSV row 53. |
| GPIO42 | 48 | **WS2812 RGB LED** (single-wire) | on-board RGB LED | N | `#define LED_PIN 42`. IO CSV row 54; net `RGB`/`WS2812` (HW-Sch). |
| GPIO43 | 49 | **U0TXD** (UART0 TX) | USB-UART header | **Y** | Free IO when UART unused. 499R series resistor on `TXD0` (HW-Sch). IO CSV row 55. |
| GPIO44 | 50 | **U0RXD** (UART0 RX) | USB-UART header | **Y** | Free IO when UART unused. 100R series on `RXD0` (HW-Sch). IO CSV row 56. |
| GPIO45 | 51 | **LCD backlight** (active-high) | LED driver | N | HIGH = backlight on. `#define LCD_BL 45` / `TFT_BL 45` (User_Setup.h:132). **Also an ESP32-S3 strapping pin (VDD_SPI).** IO CSV row 57. |
| GPIO46 | 52 | **LCD DC / RS** (data-high, cmd-low) | ILI9341V | N | `#define LCD_DC 46` / `TFT_DC 46`. **Also an ESP32-S3 strapping pin.** IO CSV row 58; net `LCD_RS` (HW-Sch). |
| GPIO47 | 36 | **microSD DATA3** | microSD | N | `#define SD_D3 47`. IO CSV row 44. |
| GPIO48 | 37 | **microSD DATA2** | microSD | N | `#define SD_D2 48`. IO CSV row 43. |

## Header / broken-out pins (the only user-available GPIOs)

Per the IO sheet's "是否引出" (broken-out) column, exactly eight GPIOs reach a header, confirmed by the `IO2/IO3/IO14/IO21/IIC_SCL/IIC_SDA/TXD0/RXD0` connector nets in the HW schematic:

| GPIO | Truly free? | Caveat |
|------|-------------|--------|
| GPIO2  | Yes | Clean GPIO / ADC1_CH1 / TOUCH2. |
| GPIO3  | Yes | GPIO / ADC1_CH2, **strapping pin** — don't hold at reset. |
| GPIO14 | Yes | Clean GPIO / ADC2_CH3. |
| GPIO21 | Yes | Clean GPIO. |
| GPIO15 | Conditional | Shared I2C SCL — free only if touch **and** audio are unused. |
| GPIO16 | Conditional | Shared I2C SDA — same condition as GPIO15. |
| GPIO43 | Conditional | U0TXD — free only if UART0 console unused. |
| GPIO44 | Conditional | U0RXD — same condition as GPIO43. |

## Strapping / boot / reset pins

- **CHIP_PU (RESET)** — active-low chip enable, also resets the LCD (IO CSV row 3).
- **GPIO0** — boot-mode strapping + BOOT button (IO CSV row 4).
- **GPIO3** — JTAG-source strapping; broken out, so avoid external pulls at boot (derived, ESP32-S3 strapping list).
- **GPIO45** — VDD_SPI voltage strapping; here repurposed as LCD backlight (IO CSV row 57).
- **GPIO46** — boot/ROM-message strapping; here repurposed as LCD DC (IO CSV row 58).

## Reserved buses (never repurpose)

- **In-package OPI PSRAM + external QSPI flash:** GPIO26–GPIO37 (IO CSV rows 31–49). Touching any of these bricks memory access.
- **Native USB:** GPIO19 (D−), GPIO20 (D+).
- **LCD 4-wire SPI:** CS=10, MOSI=11, SCLK=12, MISO=13, DC=46, BL=45, RST=via CHIP_PU.
- **Touch (I2C):** SCL=15, SDA=16, INT=17, RST=18.
- **Audio:** amp-EN=1, MCLK=4, BCLK=5, WS=7, data pins 6 & 8 (direction disputed — conflict #3), codec I2C shares 15/16.
- **microSD (SDIO 4-bit):** CLK=38, CMD=40, D0=39, D1=41, D2=48, D3=47.
- **WS2812 RGB LED:** GPIO42.

## Wrong demo comments (the reason this doc exists)

Every item below is a demo **comment** that contradicts the schematic / IO sheet / the demo's own `#define`s. Trust the table above, not these.

1. **Example_01 `Simple_test.ino:11`** header comment lists `DC/RS=2, RESET=15, LED=21`. **Wrong.** The same sketch's own `spi_dev.h` defines `LCD_DC 46`, `LCD_BL 45`, `LCD_RST -1`. Correct: DC=46, backlight/LED=45, reset via CHIP_PU. (CS=10, MOSI=11, SCK=12, MISO=13 in the comment happen to be right.)
2. **Example_13 `GetBatteryVoltage.ino:5`** header comment lists `CS=15, DC/RS=2, RESET=ESP32-EN, MOSI=13, SCK=14, MISO=12, BL=27, BAT_VOLT_ADC=34`. **Almost entirely wrong.** Correct per IO sheet/schematic: CS=10, DC=46, MOSI=11, SCK=12, MISO=13, BL=45, and **battery ADC = GPIO9 (ADC1_CH8)** — the sketch itself uses `ADC_CHANNEL_8`. GPIO34 is in-package PSRAM (IO CSV row 46) and cannot be an ADC input at all.
3. **I2S data-line direction:** the demos define `I2S_DOUT 8` (Example_16 custom.h:21) and `I2S_DINT 6` (Example_17 custom.h:21), i.e. DOUT→GPIO8, DIN→GPIO6. The IO sheet says the **opposite**: GPIO6 = "audio data output" and GPIO8 = "audio data input" (IO CSV rows 10, 12). The two pins are unambiguous (4=MCLK,5=BCLK,7=WS); only the 6/8 labels conflict. If wiring a codec by hand, verify DOUT/DIN on the amp/codec footprint against the schematic before trusting either name.

Additional guidance embedded in the demos that is *correct*: `Example_01 Simple_test.ino:6` notes the touch "SDA/SCK pins are defined by the system and can't be modified" (they are the shared I2C on GPIO15/16), and that setting a display control pin to `-1` ties it to 3.3V / board reset (this is how RST works here).
