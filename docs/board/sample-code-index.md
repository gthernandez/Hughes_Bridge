# Vendor Sample-Code Index (Arduino Demo Example_01..30)

Catalogue of the board vendor's 30 Arduino examples for the ES3C28P / ES3N28P (ESP32-S3, ILI9341V). For each example: what it demonstrates, the entry `.ino`, the key pins/peripherals/APIs it exercises, and which subsystem doc it is the working reference for. Third-party `Install libraries/` example trees and all `build/` dirs are excluded.

## Sources mined
- `1-Demo/Arduino/Demo/Example_01..30/**/*.ino` (header comments + `setup()`), file-by-file
- `1-Demo/Arduino/Demo/Example_14_Backlight_PWM/Backlight_pwm_test/touch.h` (capacitive-touch pin map)
- `1-Demo/Arduino/Demo/Example_16_music/music/ESP_Panel_Board_Custom.h` (audio/I2S, I2C, SD pin map)
- `1-Demo/Arduino/Replaced files/User_Setup.h` (authoritative TFT_eSPI display pin map)
- `1-Demo/Arduino/Replaced files/` (TFT_eSPI_ESP32_S3.c, lv_conf.h, ILI9341_Init.h)
- `1-Demo/ESP-IDF/`, `1-Demo/MicroPython/` (directory listings, for coverage note)

> **WARNING — do not trust the `.ino` header pin tables.** Almost every header carries a copy-pasted `//pin usage as follow` block listing **ESP32-WROOM-32E / ESP32-classic** pins (CS 15, DC 2, RESET "ESP32-EN"/15, MOSI 13, SCK 14, MISO 12, BL 21/27, BAT 34, resistive-touch pins). These are **wrong for this ESP32-S3 board.** The authoritative pins come from the `Replaced files/User_Setup.h` (display) and `ESP_Panel_Board_Custom.h` / `touch.h` (audio, SD, touch), listed in "Verified board pins" below. See `conflicts[]` in this pack's notes.

---

## Verified board pins (authoritative; use these, not the header tables)

| Function | Pin(s) | Source |
|---|---|---|
| TFT MOSI / SCLK / MISO | 11 / 12 / 13 | `Replaced files/User_Setup.h:213,214,212` |
| TFT CS / DC / RST / BL | 10 / 46 / -1 / 45 | `Replaced files/User_Setup.h:215,216,218,132` |
| TFT driver / SPI clock | `ILI9341_DRIVER`, 40 MHz (read 20 MHz) | `Replaced files/User_Setup.h:45,363,368` |
| Capacitive touch (FT6336) SCL/SDA/INT/RST | 15 / 16 / 17 / 18 | `Example_14.../touch.h:4-7` |
| Audio (ES8311) I2S MCK/BCK/DOUT/WS | 4 / 5 / 8 / 7 | `Example_16.../ESP_Panel_Board_Custom.h:19-22` |
| Audio/touch shared I2C SCL/SDA | 15 / 16 @ 400 kHz | `Example_16.../ESP_Panel_Board_Custom.h:24-26` |
| SD (SDMMC) CLK/CMD/D0/D1/D2/D3 | 38 / 40 / 39 / 41 / 48 / 47 | `Example_16.../ESP_Panel_Board_Custom.h:11-16` |
| RGB LED (WS2812 NeoPixel) DIN | 42 | `Example_06.../RGB_LED.ino:11` |
| BOOT / user key | 0 | `Example_09.../Key_test.ino:8` |
| Battery sense ADC | ADC1 channel 8 (= GPIO9 on S3) | `Example_13.../GetBatteryVoltage.ino:24-25` |

---

## Example catalogue

| # | Folder | Entry `.ino` | Demonstrates | Key pins / peripherals / APIs | Reference for |
|---|---|---|---|---|---|
| 01 | `Example_01_Simple_test` | `Simple_test/Simple_test.ino` | Bare-metal screen clear/fill; **no display library** (custom `spi_dev.h`) for ILI9341 | Raw `SPIClass(SPI_PORT)`, `beginTransaction`, ILI9341 cmds `0x2A/0x2B/0x2C/0x36`; 240x320 | display |
| 02 | `Example_02_colligate_test` | `colligate_test/colligate_test.ino` | Broad tour of the graphics library (text, lines, shapes, timing) | `TFT_eSPI` `drawCentreString`, `drawFastVLine/HLine`, `fillRect`, `micros()` | display |
| 03 | `Example_03_display_graphics` | `display_graphics/display_graphics.ino` | Draw picture + rotated string primitives | `TFT_eSPI` `fillScreen`, `drawString`, `drawFloat`, `drawNumber`, `setRotation` | display |
| 04 | `Example_04_display_scroll` | `display_scroll/display_scroll.ino` | Text scrolling + custom GB16 bitmap font rendering | `TFT_eSPI` `setAddrWindow`, `drawPixel`, `pgm_read_byte`, `font.h` | display |
| 05 | `Example_05_show_SD_jpg_picture` | `Show_SD_Jpg/Show_SD_Jpg.ino` | Decode & render JPEG from SD card | `SD_MMC`, `TJpg_Decoder` (`TJpgDec`, `pushImage`); SD pins 38/40/39/41/48/47 | sd (+ display) |
| 06 | `Example_06_RGB_LED` | `RGB_LED/RGB_LED.ino` | Drive onboard WS2812 RGBW NeoPixel | `Adafruit_NeoPixel(LED_COUNT,42,NEO_GRBW+NEO_KHZ800)`, `colorWipe` | led |
| 07 | `Example_07_Flash_DMA_jpg` | `Flash_Jpg_DMA_test/Flash_Jpg_DMA_test.ino` | JPEG from flash array rendered via SPI DMA double-buffering | `TFT_eSPI` `initDMA`, `pushImageDMA`, `TJpg_Decoder`, `image.h` | display |
| 08 | `Example_08_LVGL_Demos` | `LVGL_Demos/LVGL_Demos.ino` | LVGL widget/benchmark demos with capacitive touch input | `lvgl` v8.3, `lv_demos.h`, `TFT_eSPI` flush, `touch.h` (`touch_touched`, `touch_last_x/y`) | display + touch |
| 09 | `Example_09_key` | `Key_test/Key_test.ino` | Debounced BOOT-button cycles NeoPixel color | `pinMode(0,INPUT_PULLUP)`, `digitalRead`, `millis()` debounce, `Adafruit_NeoPixel` | core/peripherals (+ led) |
| 10 | `Example_10_uart` | `Uart_test/Uart_test.ino` | UART TX/RX echo to serial + on-screen | `Serial.begin(115200)`, `Serial.readString()`, `Serial.print(x,BIN/HEX...)` | core/peripherals |
| 11 | `Example_11_RTC_test` | `RTC_test/RTC_test.ino` | Software RTC clock display | `ESP32Time` (`setTime`, `getHour/Minute/...`), `setFreeFont` | core/peripherals |
| 12 | `Example_12_timer_test` | `Timer_test/Timer_test.ino` | Hardware timer ISR driving LED | `hw_timer_t`, `timerBegin/AttachInterrupt/AlarmWrite`, `IRAM_ATTR`, NeoPixel 42 | core/peripherals (+ led) |
| 13 | `Example_13_Get_Battery_Voltage` | `GetBatteryVoltage/GetBatteryVoltage.ino` | Read + display battery voltage via ADC oneshot + calibration | `adc_oneshot_*`, `adc_cali_*`, `ADC_UNIT_1`, `ADC_CHANNEL_8` (GPIO9), `ADC_ATTEN_DB_12` | power |
| 14 | `Example_14_Backlight_PWM` | `Backlight_pwm_test/Backlight_pwm_test.ino` | PWM backlight dimming via touch slider | `ledcSetup/ledcAttachPin(45)`, `touch.h` (`touch_init`), 2 kHz/8-bit | display (backlight) + touch |
| 15 | `Example_15_RGB_LED_TOUCH` | `RGB_LED_TOUCH/RGB_LED_TOUCH.ino` | On-screen touch buttons control NeoPixel channels | `TFT_eSPI_Button`, `touch.h`, `Adafruit_NeoPixel(...,NEO_GRB)` pin 42 | led + touch |
| 16 | `Example_16_music` | `music/music.ino` | Play audio file from SD through ES8311 codec (I2S) | `es8311.h`, `Audio.h`, I2C init (SDA16/SCL15), I2S MCK4/BCK5/DOUT8/WS7, `ESP_Panel_Board_Custom.h` | audio (+ sd) |
| 17 | `Example_17_echo` | `echo/echo.ino` | Mic-to-speaker loopback (record + playback) via ES8311 | `es8311.h`, `i2s_chan_handle_t` tx+rx, `I2S_MCLK_MULTIPLE_384` | audio |
| 18 | `Example_18_WiFi_scan` | `Wifi_scan_test/Wifi_scan_test.ino` | Scan and list Wi-Fi APs on screen | `WiFi.mode(WIFI_STA)`, `WiFi.scanNetworks()`, `SSID/RSSI/encryptionType` | peripherals (wifi) |
| 19 | `Example_19_WiFi_AP` | `Wifi_AP_test/Wifi_AP_test.ino` | Start SoftAP, show IP/MAC/client count | `WiFi.softAP`, `softAPIP`, `softAPgetStationNum` | peripherals (wifi) |
| 20 | `Example_20_WiFi_SmartConfig` | `Wifi_SmartConfig_test/Wifi_SmartConfig_test.ino` | ESP-Touch SmartConfig provisioning + persist creds | `WiFi.beginSmartConfig`, `Preferences` NVS, key pin 0 | peripherals (wifi) |
| 21 | `Example_21_WiFi_STA` | `Wifi_STA_test/Wifi_STA_test.ino` | Connect to an AP as station | `WiFi.begin(ssid,pw)`, `WiFi.status()`, `WiFi.setSleep(false)` | peripherals (wifi) |
| 22 | `Example_22_WiFi_STA_TCP_Client` | `wifi_STA_TCP_Client_test/wifi_STA_TCP_Client_test.ino` | TCP client to a server IP/port | `WiFiClient`, `client.connect/print/read` | peripherals (net) |
| 23 | `Example_23_WiFi_STA_TCP_Server` | `wifi_STA_TCP_Server_test/wifi_STA_TCP_Server_test.ino` | TCP server accepting a client | `WiFiServer(port)`, `server.available()` | peripherals (net) |
| 24 | `Example_24_WiFi_STA_UDP` | `wifi_STA_UDP_test/wifi_STA_UDP_test.ino` | Async UDP broadcast/receive | `AsyncUDP` (`udp.listen`, `onPacket`, `broadcast`) | peripherals (net) |
| 25 | `Example_25_BLE_scan` | `ble_scan_test/ble_scan_test.ino` | Scan BLE advertisers, list name/RSSI | `BLEDevice/BLEScan/BLEAdvertisedDevice` callbacks | peripherals (BLE) |
| 26 | `Example_26_BLE_server` | `Blue_Server_test/Blue_Server_test.ino` | BLE GATT server with notify characteristic | `BLEServer/BLEService/BLECharacteristic/BLE2902`, custom UUIDs | peripherals (BLE) |
| 27 | `Example_27_Desktop_Display` | `DesktopDisplay/DesktopDisplay.ino` | Astronaut weather-clock: Wi-Fi + HTTP weather + JPEG UI | `HTTPClient`, `ArduinoJson`, `TimeLib`, `TJpg_Decoder`, `EEPROM`, `LCD_BL_PIN 45`; needs 3MB-No-OTA partition | display (integration) |
| 28 | `Example_28_display_phonecall` | `display_phonecall/display_phonecall.ino` | Touch phone-call UI mockup | `TFT_eSPI` widgets + `touch.h` | display + touch |
| 29 | `Example_29_touch_pen` | `touch_pen/touch_pen.ino` | Finger/pen drawing canvas | `touch.h` (FT6336), `TFT_eSPI` line drawing | touch |
| 30 | `Example_30_ai_chat` | `ai_chat/ai_chat.ino` | Full AI voice assistant: record→WebSocket→TTS playback | ES8311 audio, `ArduinoWebsockets`, `ArduinoJson`, `WiFi`, SD, NeoPixel, `mbedtls`, key pin 0 | audio + wifi + integration |

---

## `1-Demo/Arduino/Replaced files/` — board-specific library overrides

These four files **must be copied over the stock library files** before the examples compile correctly; they are what makes the generic libraries match this ES3C28P/ES3N28P board. This is why the header pin tables can be ignored — the real pins live here.

| File | Replaces | Why it matters |
|---|---|---|
| `User_Setup.h` | `TFT_eSPI/User_Setup.h` | Selects `ILI9341_DRIVER` and sets the **authoritative** display pins (CS 10, DC 46, RST -1, MOSI 11, SCK 12, MISO 13, BL 45), 40 MHz SPI, loaded fonts. `User_Setup.h:45,132,212-218,363` |
| `TFT_eSPI_ESP32_S3.c` | `TFT_eSPI/Processors/TFT_eSPI_ESP32_S3.c` | ESP32-S3-specific low-level SPI/DMA driver implementation. `TFT_eSPI_ESP32_S3.c:1-30` |
| `ILI9341_Init.h` | `TFT_eSPI/TFT_Drivers/ILI9341_Init.h` | ILI9341 power/gamma init command sequence (`0xCF/0xED/0xE8/0xCB...`) tuned for this panel. `ILI9341_Init.h:8-30` |
| `lv_conf.h` | `lvgl/lv_conf.h` | LVGL v8.3.6 config: `LV_COLOR_DEPTH 16`, `LV_COLOR_16_SWAP 0`; required by Example_08. `lv_conf.h:3,26,29` |

---

## Other demo trees (coverage note)

- **`1-Demo/ESP-IDF/`** — one full ESP-IDF project `2.8inch_ESP32-S3_LVGL/` (CMakeLists, `components/`, `main/`, `sdkconfig`) plus setup PDFs. Single LVGL porting demo, not a 30-way catalogue; the real pin authority is still the Arduino `Replaced files`. `1-Demo/ESP-IDF/` listing.
- **`1-Demo/MicroPython/`** — `firmware/`, `libraries/`, `Font/`, `BMP/`, and six demo scripts in `demos/`: `Simple_test.py`, `graphical_test.py`, `font_test.py`, `BMP_test.py`, `Read_ID_GRAM.py`, `Touch_Pen.py`. Covers display, fonts, BMP, register read, and touch — no audio/Wi-Fi/BLE examples. See `README(Important).txt`. `1-Demo/MicroPython/demos/` listing.

---

## Recommended per-subsystem reference examples

- **display**: 01 (raw), 02/03/04 (TFT_eSPI API), 07 (DMA), 08 (LVGL)
- **touch**: 29 (drawing), 08 (LVGL input), 14/15/28 (touch UI)
- **audio**: 16 (playback), 17 (loopback), 30 (full duplex + net)
- **sd**: 05 (JPEG from SD)
- **led**: 06 (basic), 12 (timer-driven), 15 (touch-driven)
- **power**: 13 (battery ADC)
- **core/peripherals**: 09 (key), 10 (uart), 11 (RTC), 12 (timer); Wi-Fi 18-24; BLE 25-26
- **integration**: 27 (weather clock), 30 (AI chat)
