# Board Misc Peripherals — RTC, Hardware Timer, BOOT Button, UART, WiFi/BLE

Purpose: cited reference for the remaining on-board bits of the 2.8" IPS ESP32-S3 (ES3C28P / ES3N28P) module and the example-proven APIs for each, for hughes_bridge firmware.

Sources mined:
- IO-allocation CSV (converted from `5-Schematic/ESP32-S3_Chip_IO_Resource_Allocation.xlsx`)
- `5-Schematic/2.8inch_ESP32-S3_Display_Module_Hardware_Schematic.pdf` (text-extracted)
- `1-Demo/Arduino/Demo/Example_09_key/Key_test/Key_test.ino`
- `1-Demo/Arduino/Demo/Example_10_uart/Uart_test/Uart_test.ino`
- `1-Demo/Arduino/Demo/Example_11_RTC_test/RTC_test/RTC_test.ino`
- `1-Demo/Arduino/Demo/Example_12_timer_test/Timer_test/Timer_test.ino`
- `1-Demo/Arduino/Demo/Example_18_WiFi_scan`, `Example_19_WiFi_AP`, `Example_21_WiFi_STA`, `Example_25_BLE_scan`, `Example_26_BLE_server`

> WARNING — junk header comments: every example's `//pin usage as follow:` block is copy-pasted and WRONG for this board. Example_11 even claims `ESP32-WROOM-32E` with `RESET=ESP32-EN, CS=15, DC=2, SCK=14` (`Example_11_RTC_test/RTC_test/RTC_test.ino:4-5`); Example_10/18/25/26 claim `CS=10 DC=2 RESET=15 LED=21` (`Example_10_uart/.../Uart_test.ino:4-5`). None of these describe the RTC/UART/WiFi peripherals below. Trust only the CSV, schematic, and actual `#define`s.

## SoC / module identity
Bare **ESP32-S3R8** chip (not a WROOM/MINI module) with on-board 40 MHz main crystal (`C62`) and an **SMD chip antenna** on the `LNA_IN` net — `5-Schematic/...Module_Hardware_Schematic.pdf` shows `ESP32-S3R8`, `SMD_ANT`, `LNA_IN1`, and `40MHz C62`. "R8" = 8 MB octal PSRAM variant (derived from part suffix).

## RTC (Example_11) — INTERNAL, no external RTC chip
There is **no external RTC IC** on this board. The example drives the ESP32-S3's **internal RTC** via the `ESP32Time` library (`Example_11_RTC_test/RTC_test/RTC_test.ino:19-21`, `esp32_rtc.setTime(...)` at :36). No RTC part appears anywhere in the schematic or IO-allocation CSV; only the SoC's own `VDD3P3_RTC` / `XTAL_32K_P/N` pins exist, and **no 32.768 kHz crystal is populated** (schematic lists only the 40 MHz main crystal) — so the RTC is not battery-backed and loses time on power loss.

| API | Purpose |
|---|---|
| `ESP32Time esp32_rtc;` | instantiate (`:21`) |
| `esp32_rtc.setTime(sec,min,hour,day,month,year)` | seed clock (`:36`) |
| `getHour(true)/getMinute/getSecond` | read time, 24 h (`:41`) |
| `getYear/getMonth/getDay/getDayofWeek` | read date; **month is 0-based**, code prints `getMonth()+1` (`:44`) |

Requires the `ESP32Time` third-party library (`:19` comment "installation required").

## Hardware timer (Example_12)
Uses the ESP32-S3 general-purpose hardware timer via the Arduino-ESP32 **v3.x** timer API (single-arg `timerBegin(freq)` form), ISR in IRAM (`Example_12_timer_test/Timer_test/Timer_test.ino`):

| Call | Line | Meaning |
|---|---|---|
| `timer = timerBegin(1000000)` | :65 | 1 MHz timer tick (1 µs) |
| `timerAttachInterrupt(timer, &onTimer)` | :68 | attach ISR |
| `timerAlarm(timer, 1000000, true, 0)` | :72 | fire every 1 000 000 ticks (=1 s), auto-reload |
| `void IRAM_ATTR onTimer()` | :48 | ISR body |

Note: the ISR here calls a blocking NeoPixel `colorWipe()` with `delay()` (`:50`, `:83-88`) — bad practice; do real work in `loop()`, not in the ISR, for hughes_bridge.

## BOOT / KEY button (Example_09)
- **GPIO0** is the BOOT button. IO-allocation CSV row 4 (GPIO0): "BOOT按键" (BOOT key), default `RTC_GPIO0, GPIO0`, and marked **not brought out** ("没引出，不能做普通IO口使用" — not exposed, cannot be used as a general IO). Schematic labels the two on-board keys `KEY1 KEY2` (BOOT on GPIO0; the other is RESET/CHIP_PU).
- Wiring: active-low, no external pull needed — example uses `pinMode(BUTTON_PIN, INPUT_PULLUP)` and treats `LOW` as pressed (`Example_09_key/Key_test/Key_test.ino:8`, `:32`, `:53`). Software debounce ~2 ms (`:18`).
- Caveat: GPIO0 also selects download mode at boot (held low = enters bootloader). Usable as a runtime button once booted, but it is not a spare GPIO — it is dedicated to this button.

## UART (Example_10)
- Example uses the Arduino `Serial` object at **115200 baud** (`Example_10_uart/.../Uart_test.ino:26`), `Serial.available()/readString()` echo loop (`:53-56`). No pins are set → default port.
- Physical **UART0** is on **GPIO43 = U0TXD** and **GPIO44 = U0RXD** — IO-allocation CSV rows 55-56 (GPIO43 "ESP32-S3串口0发送数据引脚" / GPIO44 "串口0接受数据引脚"), both **broken out** ("不使用串口通信时可做普通IO口使用" — usable as general IO when UART not in use). Schematic confirms `U0TXD 49` / `U0RXD 50` with series resistors `R39 100R` and `R44 499R` on the TX line.
- These GPIO43/44 header pins are the only exposed hardware UART. (Whether `Serial` maps to USB-CDC or to UART0 depends on the `USB CDC On Boot` build flag — the pins above are UART0 regardless.)

## WiFi / BLE (Example_18–26)
Standard Arduino-ESP32 stacks; nothing board-specific in code beyond the shared antenna.

| Feature | Example | API entry points |
|---|---|---|
| WiFi scan | 18 | `#include "WiFi.h"`, `WiFi.mode(WIFI_STA)`, `WiFi.scanNetworks()` (`Example_18_WiFi_scan/.../Wifi_scan_test.ino:18,29,40`) |
| WiFi SoftAP | 19 | `WiFi.softAP(ssid,password)`, `softAPIP()`, `softAPgetStationNum()` (`Example_19_WiFi_AP/.../Wifi_AP_test.ino:35,41,50`) |
| WiFi STA / TCP / UDP / SmartConfig | 21–24 | station connect + sockets |
| BLE scan | 25 | `BLEDevice`/`BLEScan`, 5 s scan (`Example_25_BLE_scan/.../ble_scan_test.ino:16-19,23`) |
| BLE server | 26 | `BLEServer`, custom 128-bit service `DFCD0001-...` (`Example_26_BLE_server/.../Blue_Server_test.ino:17-22`) |

**Antenna:** on-board **SMD chip antenna** on `LNA_IN` (schematic `SMD_ANT`, `R21 0R`/`R22 0R` matching-network zero-ohm links). No U.FL/IPEX external-antenna connector is present. Radio is the ESP32-S3's integrated 2.4 GHz WiFi (b/g/n) + Bluetooth 5 LE (derived from ESP32-S3 SoC; no separate radio part on the board).
