# Touch Controller — FocalTech FT6336G

Reference for the capacitive touch controller on the 2.8" ES3C28P / ES3N28P board: I2C address & bus pins, INT/RST wiring, register map, coordinate/rotation mapping, and power modes.

**Sources mined**
- `4-DataSheet/D-FT6336G-DataSheet-V1.0.pdf` (FocalTech FT6336G datasheet V1.0, 2015)
- Register maps: `ft6336_reg_sheet1.csv` (working-mode page) and `ft6336_reg_sheet2.csv` (factory-mode TEST0 page) — converted from `4-DataSheet/FT6336G_Register.xlsx`
- IO-allocation CSV (`io_alloc_sheet1.csv`, from `5-Schematic`) — touch bus/INT/RST pin assignments
- Vendor driver: `1-Demo/Arduino/Install libraries/FT6336-arduino/FT6336.h` + `FT6336.cpp`
- Vendor demos: `1-Demo/Arduino/Demo/Example_15_RGB_LED_TOUCH/RGB_LED_TOUCH/touch.h`, `1-Demo/Arduino/Demo/Example_29_touch_pen/touch_pen/{touch.h,touch_pen.ino}`

---

## 1. Part & capabilities

| Item | Value | Source |
|---|---|---|
| Controller | FocalTech FT6336G, self-capacitive single-chip TP controller w/ built-in 16-bit MCU | `4-DataSheet/D-FT6336G-DataSheet-V1.0.pdf` p.1 (INTRODUCTION) |
| Simultaneous touch points | 1 point + gesture, or 2 points | datasheet p.1 (FEATURES); reg `TD_STATUS` max 0x02 → `ft6336_reg_sheet1.csv` row 8 |
| Report rate | up to 100 Hz (Active default 60 fps; Monitor default 25 fps) | datasheet p.1 (FEATURES), p.4 §2.3 |
| Channels | up to 31 sensor/driver channels | datasheet p.1 (FEATURES), p.1 §1.1 table |
| Interface | I2C slave only | datasheet p.4 §2.4 / p.5 §2.4.1 |
| Operating voltage | VDDA 2.8–3.6 V; independent IOVCC 1.8–3.6 V | datasheet p.1 (FEATURES), p.6 §3.1/§3.2 |

---

## 2. I2C address & bus

| Item | Value | Source |
|---|---|---|
| I2C 7-bit slave address | **0x38** (write byte 0x70, read byte 0x71) | `FT6336.h:8` `#define FT6336_ADDR (uint8_t)0x38`; datasheet p.6 Table 2-1 defines the frame's `A[6:0]` slave-address field but does not print the numeric value |
| Register addressing | 8-bit register pointer (single byte written before data) | `FT6336.cpp:97-132` — `highByte(reg)` writes are commented out; only `lowByte(reg)` is sent |
| SCL frequency | 10 kHz min – 400 kHz max | datasheet p.6 Table 2-2 |
| Mode | always I2C slave | datasheet p.5 §2.4.1 |
| Data setup / START-hold timing | data setup 250 ns min; (repeated) START hold 4.0 µs; bus-free 4.7 µs | datasheet p.6 Table 2-2 |

The board's touch I2C bus is **shared with the ES8311 audio codec** — GPIO15/GPIO16 serve both the capacitive TP and the audio codec I2C (IO-allocation CSV rows for GPIO15/GPIO16). When touch or audio is in use these pins are I2C-only.

### 2.1 Board pin map (verified against IO-allocation CSV, not comments)

| Signal | ESP32-S3 GPIO | Active level / note | Sources |
|---|---|---|---|
| SDA (I2C data) | **GPIO16** | shared with audio codec I2C data | IO-allocation CSV row GPIO16 ("电容触摸屏I2C总线数据信号引脚"); `Example_15…/touch.h:5`, `Example_29…/touch.h:5` `#define TOUCH_FT6336_SDA 16` |
| SCL (I2C clock) | **GPIO15** | shared with audio codec I2C clock | IO-allocation CSV row GPIO15 ("电容触摸屏I2C总线时钟信号引脚"); `Example_15…/touch.h:4` `#define TOUCH_FT6336_SCL 15` |
| INT (touch interrupt → host) | **GPIO17** | not broken out; **low** on touch event | IO-allocation CSV row GPIO17 ("电容触摸屏中断输入引脚，发生触摸事件时，输入低电平"); `Example_15…/touch.h:6` `#define TOUCH_FT6336_INT 17` |
| RST (reset from host) | **GPIO18** | not broken out; **low = reset** | IO-allocation CSV row GPIO18 ("电容触摸屏复位控制引脚，低电平复位"); `Example_15…/touch.h:7` `#define TOUCH_FT6336_RST 18` |

Pin numbers are **consistent** across schematic/IO-CSV, the demo `#define`s, and the header comment in `Example_29…/touch_pen.ino:7-8` (`CTP_INT 17, CTP_RST 18, CTP_SDA 16, CTP_SCL 15`) — a rare case where the pack's header comment is correct.

Note: the vendor `FT6336::read()` **polls** `TD_STATUS` over I2C and never attaches an ISR to INT (`FT6336.cpp:56-67`); the INT commented-out ISR is disabled (`FT6336.h:59`). The driver only drives INT high once in `reset()` (`FT6336.cpp:23`). For interrupt-driven use, configure GPIO17 as an input and read on the falling edge yourself.

---

## 3. Reset / power-on sequence

Vendor `reset()` sequence (`FT6336.cpp:19-49`): INT→1, RST→1, wait 20 ms; RST→0, wait 20 ms; RST→1, wait **500 ms**; then verify chip ID registers.

Datasheet constraints (p.9-10 §3.5, Table 3-5):

| Parameter | Meaning | Min | Max | Source |
|---|---|---|---|---|
| Tris | supply rise 0.1→0.9 VDD | — | 3 ms | datasheet p.10 Table 3-5 |
| Tpon | start reporting after power-on | 300 ms | — | datasheet p.10 Table 3-5 |
| Tprt | INT held low after power-on | 1 ms | — | datasheet p.10 Table 3-5 |
| Trsi | start reporting after reset | 300 ms | — | datasheet p.10 Table 3-5 |
| Trst | reset low-pulse width | 5 ms | — | datasheet p.10 Table 3-5 |

Datasheet advises INT and I2C lines be **low before power-on**, and RST pulled **low before power-on**; on power-down the supply must drop below 0.3 V with Trst ≥ 5 ms before re-power (datasheet p.9 §3.5). After reset the chip enters **Active** mode (datasheet p.5).

The vendor 20 ms low pulse and 500 ms settle both satisfy Trst (≥5 ms) and Trsi (≥300 ms) (derived).

### Chip-ID verification (what `reset()` checks)
| Register | Addr | Expected | Source |
|---|---|---|---|
| ID_G_FOCALTECH_ID (vendor id) | 0xA8 | 0x11 | `FT6336.cpp:30-34`; `ft6336_reg_sheet1.csv` row 55 |
| ID_G_CIPHER_MID | 0x9F | 0x26 | `FT6336.cpp:35-39`; `ft6336_reg_sheet1.csv` row 40 |
| ID_G_CIPHER_LOW | 0xA0 | 0x00 Ft6236G / **0x01 Ft6336G** / 0x02 Ft6336U / 0x03 Ft6426 | `FT6336.cpp:40-43`; `ft6336_reg_sheet1.csv` row 41 |
| ID_G_CIPHER_HIGH | 0xA3 | 0x64 | `FT6336.cpp:44-48`; `ft6336_reg_sheet1.csv` row 44 |

---

## 4. Register map — working-mode page (default)

Registers below are on the default working-mode page. Switch pages by writing register `0x00` (`Mode_Switch`): write `0x00` for working mode, `0x40` for factory/TEST mode (see §6). All source rows from `ft6336_reg_sheet1.csv` unless noted.

### 4.1 Touch-data registers (read these to get touches)

| Addr | Symbol | R/W | Meaning | Default | Source |
|---|---|---|---|---|---|
| 0x00 | Mode_Switch / DEVICE_MODE | RW | page/mode switch (write 0x40 → factory mode) | — | row 6; `FT6336.h:15` `FT6336_DEVIDE_MODE 0x00` |
| 0x01 | Reserved (GEST_ID) | RO | — | 0x00 | row 7 |
| 0x02 | **TD_STATUS** | RO | number of touch points (0–2) | 0x00 | row 8; `FT6336.h:16` |
| 0x03 | **P1_XH** | RO | pt1 X[11:8] in bits[3:0]; **event flag in bits[7:6]** | 0xFF | row 9; `FT6336.h:18` `FT6336_TOUCH_1 0x03` |
| 0x04 | P1_XL | RO | pt1 X[7:0] | 0xFF | row 10 |
| 0x05 | P1_YH | RO | pt1 Y[11:8] in bits[3:0]; **touch ID in bits[7:4]** | 0xFF | row 11 |
| 0x06 | P1_YL | RO | pt1 Y[7:0] | 0xFF | row 12 |
| 0x07 | P1_WEIGHT | RO | pt1 touch weight/pressure | 0xFF | row 13 |
| 0x08 | P1_MISC | RO | pt1 touch area (misc) | 0xFF | row 14 |
| 0x09 | **P2_XH** | RO | pt2 X hi + event flag (same layout as 0x03) | 0xFF | row 15; `FT6336.h:19` `FT6336_TOUCH_2 0x09` |
| 0x0A | P2_XL | RO | pt2 X[7:0] | 0xFF | row 16 |
| 0x0B | P2_YH | RO | pt2 Y hi + ID (same layout as 0x05) | 0xFF | row 17 |
| 0x0C | P2_YL | RO | pt2 Y[7:0] | 0xFF | row 18 |
| 0x0D | P2_WEIGHT | RO | pt2 weight | 0xFF | row 19 |
| 0x0E | P2_MISC | RO | pt2 area | 0xFF | row 20 |

Each touch point occupies 6 bytes; point N base = `0x03 + N*6` (`FT6336.cpp:63`).

**Bit layout of the XH/YH bytes** (datasheet register descriptions, `ft6336_reg_sheet1.csv` rows 9 & 11):
- `Pn_XH`: bits[7:6] = touch **event flag** (0=press-down, 1=lift-up, 2=contact, 3=no-event), bits[3:0] = X[11:8].
- `Pn_YH`: bits[7:4] = touch **ID** (finger index), bits[3:0] = Y[11:8].

Vendor decode (`FT6336.cpp:68-72`): `id = data[2]>>4`; `x = ((data[0]&0x0F)<<8) | data[1]`; `y = ((data[2]&0x0F)<<8) | data[3]`. The vendor driver does **not** mask/extract the event-flag bits and does not read WEIGHT/MISC (only 4 bytes per point are read).

### 4.2 Config / gesture / ID registers

| Addr | Symbol | R/W | Meaning | Default | Source |
|---|---|---|---|---|---|
| 0x80 | ID_G_THGROUP | RW | touch threshold (value×16) | 0xBB | row 22; `FT6336.h:29` |
| 0x81 | ID_G_PEAKTH | RW | peak threshold | 0x0F | row 23 |
| 0x85 | ID_G_THDIFF | RW | filter-range threshold (≈val/16) | 0xA0 | row 25 |
| 0x86 | ID_G_CTRL | RW | allow entering Monitor mode (0=disable,1=enable) | 0x01 | row 26 |
| 0x87 | ID_G_TIMEENTERMONITOR | RW | idle seconds before entering Monitor | 0x1E | row 29 |
| 0x88 | ID_G_PERIODACTIVE | RW | Active-mode scan period (report-rate ctrl), 0x04–0x14 | 0x08 | row 30; `FT6336.h:30` |
| 0x89 | ID_G_PERIODMONITOR | RW | Monitor-mode scan period, 0x04–0x14 | 0x08 | row 31 |
| 0x8B | ID_G_FREQ_HOPPING_EN | RW | charger plug in/out flag | 0x00 | row 33 |
| 0x9F | ID_G_CIPHER_MID | RO | chip code (mid byte) = 0x26 | 0x26 | row 40; `FT6336.h:23` |
| 0xA0 | ID_G_CIPHER_LOW | RO | chip code (low): 0x01=Ft6336G, 0x02=Ft6336U | — | row 41; `FT6336.h:24` |
| 0xA1 | ID_G_LIB_VERSION_H | RO | firmware lib version, high byte | 0x10 | row 42; `FT6336.h:25` |
| 0xA2 | ID_G_LIB_VERSION_L | RO | firmware lib version, low byte | 0x01 | row 43 |
| 0xA3 | ID_G_CIPHER_HIGH | RO | chip code (high byte) = 0x64 | 0x64 | row 44; `FT6336.h:26` |
| 0xA4 | ID_G_MODE | RW | INT report mode (0=pulse, 1=hold low while reporting) | 0x01 | row 45; `FT6336.h:27` |
| **0xA5** | **ID_G_PMODE** | RW | **power mode** (see §5) | 0x00 | row 48 |
| 0xA6 | ID_G_FIRMID | RO | firmware version | 0x00 | row 53 |
| 0xA8 | ID_G_FOCALTECH_ID | RO | vendor ID = 0x11 | 0x11 | row 55; `FT6336.h:28` |
| 0xAD | ID_G_IS_CALLING | RW | host "in-call" flag | 0x00 | row 58 |
| 0xAE | ID_G_FACTORY_MODE | RW | factory-mode kind (0=Normal,1=Test1,2=Test2) | 0x00 | row 59 |
| 0xB0 | ID_G_FACE_DEC_MODE | RW | proximity/face detect enable (0=off,1=on) | 0x00 | row 63 |
| 0xBC | ID_G_STATE | W | work mode (0=InFO,1=normal,3=factory,4=auto-calib; write 0xAA,0x55 to trigger FW upgrade) | 0x01 | row 66 |
| 0xD0 | ID_G_SPEC_GESTURE_ENABLE | RW | special-gesture enable (write 1 to enter gesture) | 0x00 | row 72 |
| 0xD3 | (gesture id) | RO | recognized **gesture ID** number | — | row 78 |
| 0xD1–0xD8 | gesture enable bitmaps | RW | per-gesture enable bits (direction/character recognition) | — | rows 76-83 |

Gesture direction bits at 0xD1 (`ft6336_reg_sheet1.csv` row 76): bit0=slide-left, bit1=slide-right, bit2=slide-up, bit3=slide-down, bit4=double-click; bit5=digit/char master-enable.

---

## 5. Power modes

**Conflict — two authoritative sources disagree on mode count** (both cited; report both):

- **Datasheet** (p.4 §2.3, p.1 FEATURES) describes **three** operating modes:
  - **Active** — actively scans; default 60 fps; host can raise/lower rate.
  - **Monitor** — reduced-speed scan (default 25 fps), simplified detection; serial port closed, no host data transfer; on touch it immediately re-enters Active.
  - **Hibernation** — power-down; responds only to RESET or Wakeup; minimal current.
- **Register `0xA5 ID_G_PMODE`** (`ft6336_reg_sheet1.csv` rows 48-52) enumerates **four** values:

| ID_G_PMODE value | Mode | Source |
|---|---|---|
| 0x00 | P_ACTIVE | `ft6336_reg_sheet1.csv` row 49 |
| 0x01 | P_MONITOR | row 50 |
| 0x02 | P_STANDBY | row 51 |
| 0x03 | P_HIBERNATE | row 52 |

The register adds a `P_STANDBY` (0x02) state not named in the datasheet prose. Treat 0x00/0x01/0x03 as the datasheet's Active/Monitor/Hibernation; `P_STANDBY` is register-only and undocumented in the prose.

Current draw (datasheet p.7 Table 3-2, VDDA=VDD3=2.8 V, 25 °C): Active (Iopr) ≈ 4.32 mA @75 Hz; Monitor (Imon) ≈ 220 µA @25 Hz (datasheet prints "mA" but context/units are µA — treat as µA, *derived*); Sleep (Islp) ≈ 55 µA.

Automatic Active↔Monitor transition is governed by `ID_G_CTRL` (0x86, enable) and `ID_G_TIMEENTERMONITOR` (0x87, idle seconds).

---

## 6. Register pages (mode switch)

Register `0x00` selects the register page (`ft6336_reg_sheet1.csv` rows 1-2, `ft6336_reg_sheet2.csv` rows 1-2):

| Write to 0x00 | Page | Contents | Source |
|---|---|---|---|
| 0x00 | Working mode | touch data + config (§4) | `ft6336_reg_sheet1.csv` row 2 |
| 0x40 | Factory / TEST0 mode | rawdata, CB, channel-order, AFE tuning, water-proof, Vsync/Hsync, chip type at 0x2F | `ft6336_reg_sheet2.csv` rows 2, 6 |

Factory-page highlights (`ft6336_reg_sheet2.csv`): `0x02 Work_Mode`, `0x0A TP_Channel_Num` (VA channels, default 0x28=40), `0x0B TP_Key_Num`, `0x18 Water_Proof_Level` (0–3), `0x2F Chip_Type` (0x00 Ft6236G / 0x01 Ft6336G / 0x02 Ft6336U / 0x03 Ft6436U, RO), rawdata via `0x34`/`0x35`, CB via `0x33`/`0x39`. These are for production test/calibration; normal firmware stays on the working-mode page.

---

## 7. Coordinate & rotation mapping (board specifics)

- **Native panel resolution used by the demos: 240 (X) × 320 (Y)** — portrait. `TOUCH_MAP_X1..X2 = 0..240`, `TOUCH_MAP_Y1..Y2 = 0..320` (`Example_15…/touch.h:8-11`, `Example_29…/touch.h:8-11`). This matches the ILI9341V 240×320 display.
- The FT6336 constructor is called with **(SDA, SCL, INT, RST, width=240, height=320)** — note argument order is SDA **then** SCL (`Example_29…/touch.h:16`, `FT6336.cpp:5`).
- Raw controller X/Y are 12-bit (0–4095 theoretical; panel maps to 0–239 / 0–319).
- **Rotation transform** applied in `FT6336::readPoint()` (`FT6336.cpp:73-94`), where `width`=240, `height`=320:

| ROTATION | Value | Transform (raw x,y → out) | Source |
|---|---|---|---|
| NORMAL | 0 | x=x, y=y | `FT6336.cpp:74-77`; `FT6336.h:13` |
| RIGHT | 1 | x'=y, y'=width−x | `FT6336.cpp:87-91`; `FT6336.h:12` |
| INVERTED | 2 | x'=width−x, y'=height−y | `FT6336.cpp:83-86`; `FT6336.h:11` |
| LEFT | 3 | x'=height−y, y'=x | `FT6336.cpp:78-82`; `FT6336.h:10` |

- The higher-level `touch_touched()` then `map()`s the point into the current LCD width/height, swapping the X/Y span limits for LEFT/RIGHT rotations (`Example_29…/touch.h:44-67`, min/max selection at lines 22-39). A commented-out `TOUCH_SWAP_XY` block (lines 49-55) is present but disabled.
- `setRotation()` only stores the value; it is applied at read time (`FT6336.cpp:52-54`).

---

## 8. Firmware integration checklist (derived)

1. `Wire.begin(16 /*SDA*/, 15 /*SCL*/)`; I2C ≤ 400 kHz; slave addr 0x38.
2. Drive RST (GPIO18) low ≥ 5 ms, release, wait ≥ 300 ms before first read (Trsi).
3. Verify IDs: 0xA8==0x11, 0x9F==0x26, 0xA3==0x64, 0xA0∈{0x00,0x01,0x02}.
4. Poll `TD_STATUS` (0x02); if 1–2, read 6 bytes/point from `0x03 + N*6`. Decode per §4.1.
5. Optional: use INT (GPIO17, active-low) as a data-ready falling-edge trigger instead of polling.
6. Apply rotation to match the ILI9341V orientation (§7).
