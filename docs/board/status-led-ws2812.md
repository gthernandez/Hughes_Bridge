# Status LED (WS2812B) — ES3C28P / ES3N28P board

Reference for the single onboard addressable RGB LED: exact part, data pin, colour order, logic/timing, and count. For hughes_bridge firmware engineers.

**Sources mined**
- `5-Schematic/2.8inch_ESP32-S3_Display_Schematic.pdf` (RGB LED circuit + module pinout)
- `5-Schematic/2.8inch_ESP32-S3_Display_Module_Hardware_Schematic.pdf`
- IO-allocation CSV (`board_csv/io_alloc_sheet1.csv`, converted from `5-Schematic/ESP32-S3_Chip_IO_Resource_Allocation.xlsx`)
- `4-DataSheet/RGB+LED(IC)_WS2812B-V5-W.PDF`
- `1-Demo/Arduino/Demo/Example_06_RGB_LED/RGB_LED/RGB_LED.ino`
- `1-Demo/Arduino/Demo/Example_15_RGB_LED_TOUCH/RGB_LED_TOUCH/RGB_LED_TOUCH.ino`

## Summary

| Property | Value | Source |
|---|---|---|
| Part (schematic) | `XL-5050RGBC-WS2812B` (WS2812B in 5050 package) | `5-Schematic/2.8inch_ESP32-S3_Display_Schematic.pdf` (schem. text line 623) |
| IC family / datasheet | WS2812B-V5/W (World Semi) | `4-DataSheet/RGB+LED(IC)_WS2812B-V5-W.PDF p.1` |
| Data GPIO | **GPIO42** | see "Pin confirmation" below |
| Count | **1** (onboard, designator LED2) | `5-Schematic/2.8inch_ESP32-S3_Display_Schematic.pdf` (line 609) |
| Colour order | **GRB**, 24-bit | `4-DataSheet/RGB+LED(IC)_WS2812B-V5-W.PDF p.4`; `Example_15_RGB_LED_TOUCH/.../RGB_LED_TOUCH.ino:36` |
| Channels | 3 (RGB only — **not** RGBW) | datasheet p.4 (24-bit GRB frame) |
| Supply (LED VDD) | **+5V** rail | `5-Schematic/2.8inch_ESP32-S3_Display_Schematic.pdf` (`+5` net feeds VDD, line ~605) |
| Data bitstream | 800 Kbps (NEO_KHZ800) | datasheet p.1; `Example_15_.../RGB_LED_TOUCH.ino:36` |
| Reset / latch time | ≥ 280 µs | datasheet p.3 |

## Pin confirmation — GPIO42 (verified, not from a comment)

The project finding **GPIO42 is CONFIRMED** by two independent authoritative sources:

1. **IO-allocation CSV row 54 (IO42)**: module pin 48 → `MTMS, GPIO42`, function
   "单线RGB三色LED灯控制引脚" = *single-wire RGB tri-colour LED control pin*; "not
   brought out, cannot be used as a general IO." (`board_csv/io_alloc_sheet1.csv` row 54)
2. **Schematic**: ESP32-S3 module pin **48** carries net `RGB_LED`
   (`5-Schematic/2.8inch_ESP32-S3_Display_Schematic.pdf`, line 783 "48 RGB_LED"),
   and that `RGB_LED` net drives the **DIN** pin of the WS2812B (LED2)
   (same PDF, RGB LED circuit block, lines ~605–623). Module pin 48 = chip IO42 per
   the CSV, so `RGB_LED` net = GPIO42.

The two demo sketches also `#define LED_PIN 42`
(`Example_06_RGB_LED/RGB_LED/RGB_LED.ino:11`, `Example_15_RGB_LED_TOUCH/.../RGB_LED_TOUCH.ino:26`),
which agrees — but per project policy the schematic/CSV are the authority, and they agree.

> Note: GPIO42 has no external breakout and is dedicated to this LED — do not repurpose it.

## Wiring

WS2812B (LED2) is a 4-pin 5050 device: **VDD, DIN, DOUT, VSS(GND)**
(`4-DataSheet/RGB+LED(IC)_WS2812B-V5-W.PDF p.2`; schematic block shows `VDD DIN DOU GND`).

- **VDD → +5V** rail (schematic `+5`).
- **DIN ← GPIO42** (net `RGB_LED`).
- **DOUT → unconnected** (single LED; no daisy-chain).
- **VSS → GND**.

Because VDD is **5V** but the ESP32-S3 drives DIN at **3.3V**: the datasheet input-high
threshold is **V_IH = 0.63·VDD** (`4-DataSheet/RGB+LED(IC)_WS2812B-V5-W.PDF p.2`),
i.e. ≈ 3.15 V at VDD=5V. A 3.3V GPIO clears this only by ~0.15V (derived) — marginal but
the standard/working configuration for these boards. No level shifter is fitted on this board.

## Colour order — GRB

The WS2812B frame is **24 bits, GRB order**: `G7..G0 R7..R0 B7..B0`, MSB first
(`4-DataSheet/RGB+LED(IC)_WS2812B-V5-W.PDF p.4`, explicitly labelled "GRB").
In firmware this is `NEO_GRB` (Adafruit_NeoPixel) — matched by
`Example_15_RGB_LED_TOUCH/.../RGB_LED_TOUCH.ino:36`
(`Adafruit_NeoPixel strip(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800)`).

The device is **3-channel RGB** — there is **no white channel** (24-bit frame, not 32-bit).

## Timing (datasheet p.3, WS2812B-V5/W, VDD=5V)

| Symbol | Meaning | Spec |
|---|---|---|
| T0H | "0" code, high time | 220–380 ns |
| T1H | "1" code, high time | 580 ns – 1 µs |
| T0L | "0" code, low time | 580 ns – 1 µs |
| T1L | "1" code, low time | 580 ns – 1 µs |
| RES | reset / latch (low) | ≥ 280 µs |

Data rate 800 Kbps. Refresh/scan rate 2 kHz (`4-DataSheet/RGB+LED(IC)_WS2812B-V5-W.PDF p.1`).
Note the **280 µs** reset — this V5 silicon needs a longer latch gap than the legacy
50 µs figure; short inter-frame gaps can cause flicker (derived from datasheet spec).

## Absolute / operating ratings (datasheet p.2)

- Operating VDD: **+3.7 to +5.3 V**.
- Absolute-max VDD input: −0.3 V to VDD+0.7 V; storage −40 to +85 °C; operating −40 to +65 °C.
- Per-channel drive current ≈ 12 mA DC at 5V (datasheet p.3).

## Firmware notes for hughes_bridge

- Single LED → set count = **1**. Both vendor demos hard-code `#define LED_COUNT 60`
  (`Example_06_.../RGB_LED.ino:14`, `Example_15_.../RGB_LED_TOUCH.ino:28`); that 60 is the
  unmodified Adafruit strip-example default (copy-paste), **not** the board's LED count.
  The board physically has one WS2812B (schematic designator LED2, DOUT unconnected).
- Use `NEO_GRB + NEO_KHZ800`.
- Colours in `strip.Color(r,g,b)` are passed R,G,B by the API and reordered to GRB internally.
- ESP32-S3 has an RMT peripheral; the RMT/led_strip driver on GPIO42 is the robust way to
  meet the timing above without bit-banging.

## Conflicts found

1. **RGBW vs RGB (channel count).** `Example_06_RGB_LED/RGB_LED/RGB_LED.ino:20` declares
   `Adafruit_NeoPixel strip(..., NEO_GRBW + NEO_KHZ800)` and its header comment (line 1)
   says "WHITE channel for RGBW pixels only." This is WRONG for this board — the fitted part
   `XL-5050RGBC-WS2812B` / WS2812B-V5/W is **3-channel GRB, 24-bit, no white channel**
   (`4-DataSheet/RGB+LED(IC)_WS2812B-V5-W.PDF p.4`). Example_06 is the unmodified Adafruit
   RGBW demo copied in verbatim. `Example_15_.../RGB_LED_TOUCH.ino:36` correctly uses
   `NEO_GRB`. **Authority (datasheet + Example_15) → use NEO_GRB.**
2. **LED count 60 vs 1** (see firmware notes) — demos' `LED_COUNT 60` contradicts the
   single-LED schematic. Schematic wins: count = 1.
