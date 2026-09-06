# Power, Battery & Charging — 2.8" ESP32-S3 ILI9341V module (ES3C28P / ES3N28P)

Permanent, cited reference for the power tree, Li-ion charger, battery-voltage ADC, USB power path, and voltage rails of the display board used by `hughes_bridge`. Written for firmware work: it answers what firmware *can* and *cannot* control.

## Sources mined
- `5-Schematic/2.8inch_ESP32-S3_Display_Schematic.pdf` (English block labels) and its Chinese twin `5-Schematic/2.8inch_ESP32-S3_Display_Module_Hardware_Schematic.pdf` — **identical circuit**; blocks "Battery charge and discharge management circuit", "Battery level detection circuit", "5V to 3.3V voltage regulator circuit", "Audio 5V to 3.3V voltage regulator circuit", "Type-C Interface circuit", "ESP32-S3 Main control circuit".
- `4-DataSheet/TP4054.PDF` (charger IC — pin table p.1, features p.1, typical-app circuit + abs-max p.2).
- `4-DataSheet/ME6217_LDO.pdf` (LDO — pin assignment p.2, electrical p.4, CE/shutdown operation p.5).
- IO-allocation CSV (`.../scratchpad/board_csv/io_alloc_sheet1.csv`), row GPIO9 (line 14).
- `1-Demo/Arduino/Demo/Example_13_Get_Battery_Voltage/GetBatteryVoltage/GetBatteryVoltage.ino` (ADC unit/channel/atten + ×2 divider math).
- `1-Demo/Arduino/Demo/Example_14_Backlight_PWM/Backlight_pwm_test/Backlight_pwm_test.ino` (backlight GPIO — for the pin-conflict note).

---

## TL;DR (firmware answers)

| Question | Answer |
|---|---|
| **(a) Charger IC / can charging be disabled in firmware?** | **TP4054** (marking `C668215`), SOT23-5. **No — charging is hardwired/automatic. There is no CE pin, and the only shutdown input (PROG) is tied to a fixed resistor, not a GPIO.** |
| **(b) Charge-status signal to a GPIO?** | **No.** TP4054 `CHRG` (open-drain) is tied to GND through a 10 kΩ pull-down (R11) and goes nowhere else — no GPIO, no LED. Charge state is **not readable by firmware**. |
| **(c) Battery ADC** | `BAT+` → **200 kΩ / 200 kΩ divider (÷2)** → net `BAT_ADC` → **GPIO9 = ADC1, channel `ADC1_CH8`**, attenuation **`ADC_ATTEN_DB_12`**, 12-bit. Multiply reading ×2 for battery mV. |
| **(d) USB / power path** | **Discrete auto-switchover (no power-path IC).** USB `VBUS`→Schottky `D8`→`+5` rail; battery `BAT+`→P-FET `Q3`→`+5` rail (P-FET ON only when USB absent). Load runs from the OR'd `+5` rail. **Board keeps running when USB is removed** (battery takes over) — inherent UPS. |
| **(e) Rails / LDOs** | Two **ME6217C33M5G** 3.3 V LDOs: `U3` (VCC3V3, digital) and `U4` (AU_VCC3V3, audio). Both CE tied to VIN → **always on, not firmware-switchable**. |

---

## Power tree

```
USB-C VBUS ──►[D8 B5819W Schottky]──┐
                                    ├──► +5 (system rail) ──►[U3 ME6217C33M5G]──► VCC3V3 (digital/ESP32/LCD)
Li-ion BAT+ ──►[Q3 SL2305 P-FET]────┘                       └►[U4 ME6217C33M5G]──► AU_VCC3V3 (audio codec/amp)
   ▲   (gate = VBUS, R13 100k pulldown; ON only when VBUS = 0)
   │
   └──[TP4054 charger, VCC = VBUS]  (charges BAT+ from USB only)
```
Source: `5-Schematic/2.8inch_ESP32-S3_Display_Schematic.pdf` (Type-C, Battery charge/discharge, 5V→3.3V, Audio 5V→3.3V blocks).

**Rail voltages**

| Net | Nominal | Source | Notes |
|---|---|---|---|
| `VBUS` | ~5.0 V | USB-C connector (pins A4/B9, A9/B4) | CC1/CC2 each have 5.1 kΩ Rd (R7/R8) → USB-C **sink** (device draws power). Schematic Type-C block. |
| `+5` | ~4.7 V on USB / 3.0–4.2 V on battery | `VBUS`−V_D8 (Schottky ≈0.3 V) **or** `BAT+` via Q3 | Misleading name: it is the OR'd system rail, **not regulated 5 V**. |
| `BAT+` | 3.0–4.2 V | single Li-ion cell (JP1 connector) | 4.2 V float set by TP4054. |
| `VCC3V3` | 3.30 V ±1% | U3 ME6217C33M5G from `+5` | Main digital rail (ESP32-S3, LCD logic, touch, SD). |
| `AU_VCC3V3` | 3.30 V ±1% | U4 ME6217C33M5G from `+5` | Separate audio rail (ES8311 codec). |
| `VDD_SPI` | 3.3 V | ESP32-S3 internal flash regulator | Feeds W25Q128 flash; not part of board power tree. |

---

## (a) Battery charger — TP4054

- **IC:** `U2` = **TP4054** (Tech Public "微型线性电池管理芯片" / micro linear Li-ion charger), board marking `C668215`, SOT23-5L. `4-DataSheet/TP4054.PDF p.1`.
- **Type:** complete linear Li-ion charger, trickle / CC / CV, **4.2 V ±1% preset float**, thermal regulation, reverse-battery protection. `4-DataSheet/TP4054.PDF p.1 (features)`.
- **Pinout (datasheet p.1 pin table):** 1 `CHRG` (open-drain charge-status out), 2 `GND`, 3 `BAT` (charge-current output), 4 `VCC` (supply in), 5 `PROG` (charge-current program **+ charge-current monitor + shutdown**). **There is no dedicated CE/enable pin.**

**Board wiring** (`5-Schematic/...Schematic.pdf`, "Battery charge and discharge management circuit"):
| TP4054 pin | Net on board | Meaning |
|---|---|---|
| 4 `VCC` | **`VBUS`** | Charger powered from **USB only** (C5 10 µF / C6 100 nF decouple). |
| 3 `BAT` | **`BAT+`** | To the cell (C3 10 µF / C4 100 nF). |
| 5 `PROG` | **R12 3.3 kΩ → GND** | Sets charge current; fixed resistor, **not a GPIO**. |
| 1 `CHRG` | **R11 10 kΩ → GND only** | Status output defeated (see (b)). |
| 2 `GND` | GND | |

**Charge current (derived):** TP4054 programs I_CHG with the PROG resistor; datasheet typical-app shows **R_PROG = 2 kΩ → 500 mA** (`TP4054.PDF p.2`), i.e. I_CHG(mA) ≈ 1000 / R_PROG(kΩ). Board R12 = 3.3 kΩ → **I_CHG ≈ 300 mA (derived)**. Datasheet abs-max BAT current = 500 mA (`p.2`).

### Can firmware disable charging? **NO.**
- No CE pin exists on the TP4054.
- The only charge-inhibit input is `PROG` (pin 5 doubles as a shutdown/关闭端 per `TP4054.PDF p.1`), but on this board PROG is hardwired to R12→GND — it is **not routed to any ESP32 GPIO**.
- Charging therefore starts automatically whenever `VCC (=VBUS) > BAT+` and stops on the CV/termination logic. **Firmware has zero control over the charger.** If charge control is required, it must be done upstream of USB power (e.g. cut VBUS externally).

## (b) Charge-status signal — NOT available to firmware
- `CHRG` (pin 1) is an **open-drain** charge-status output (pulls low while charging, high-Z otherwise). `4-DataSheet/TP4054.PDF p.1`.
- On this board `CHRG` connects **only** to R11 (10 kΩ) to GND and to nothing else — verified by rendering the schematic block; `CHRG` occurs exactly once in the schematic net text (a lone pin label). There is **no pull-up, no LED, and no GPIO** on this node.
- Consequence: the node sits near GND regardless of charge state; **there is no charge/standby signal the ESP32 can read.** Charge state can only be *inferred* by firmware from the battery-voltage trend and USB presence (there is also no direct VBUS-present GPIO — see (d)).

## (c) Battery-voltage ADC
Divider (`5-Schematic/...`, "Battery level detection circuit"):
```
BAT+ ──[R14 200k]──●── BAT_ADC ──[R15 200k]── GND      (C16 100nF filter on the node)
```
- **Ratio = 200k / (200k+200k) = 1/2 (÷2).** Firmware multiplies the measured node voltage ×2 to recover battery mV — `Example_13 .../GetBatteryVoltage.ino:90` (`int v1 = vol * 2; // 考虑分压`).
- **GPIO:** net `BAT_ADC` → **GPIO9**. CSV row GPIO9 (line 14): "电池电量ADC值读取引脚" = *battery-level ADC read pin*. GPIO9 is not broken out ("是否引出 = N").
- **ADC unit/channel/atten** (`Example_13 .../GetBatteryVoltage.ino:25-28`):
  - `ADC_UNIT_1`, `ADC_CHANNEL_8` (= **ADC1_CH8**, which on ESP32-S3 is GPIO9 — consistent with the CSV `ADC1_CH8` alt-function list, line 14).
  - `ADC_ATTEN_DB_12` (≈ 0–3.1 V input range; note ESP-IDF renamed the old `DB_11`), `ADC_BITWIDTH_12`.
  - Uses `esp_adc/adc_oneshot` + curve-fitting calibration (`adc_cali_create_scheme_curve_fitting`) → `adc_cali_raw_to_voltage()` gives calibrated mV, then ×2.
- **Range sanity (derived):** at 4.2 V battery the node is 2.10 V (within the ~3.1 V window at 12 dB). Demo maps 2.5 V→4.2 V (post-×2, i.e. node 1.25–2.10 V) to 0–100 % (`GetBatteryVoltage.ino:98-108`).

## (d) USB / 5 V power path — discrete UPS, no power-path IC
Two discrete OR-ing elements merge onto the `+5` rail (`5-Schematic/...`, Type-C + Battery blocks):
- **USB branch:** `VBUS` → **D8 (B5819W Schottky, anode=VBUS, cathode=+5)** → `+5`. ~0.3 V drop.
- **Battery branch:** `BAT+` → **Q3 (SL2305 P-channel MOSFET)** → `+5`. Q3 gate = `VBUS`, pulled to GND by **R13 100 kΩ**.
  - USB present → gate ≈ 5 V → P-FET **OFF** → battery isolated from `+5`; load runs from USB via D8; TP4054 charges the cell.
  - USB removed → gate pulled to GND → P-FET **ON** → `BAT+` feeds `+5`; load runs from battery.
- **This is an automatic power-path / load-switch built from discrete parts — there is no dedicated power-path IC.** The load (both LDOs) always runs from the OR'd `+5` node, never directly wired to only the battery.
- **Does the board keep running if USB is removed? YES** — Q3 seamlessly connects the battery, so it behaves as a UPS. (Brief note (derived): at very low battery the `+5` node ≈ V_BAT; the ME6217 needs V_IN ≥ 3.3 V + ~0.1 V dropout, so the 3.3 V rails may brown out below ~3.4 V cell voltage.)
- **No VBUS-present GPIO:** USB/charge presence is not brought to any ESP32 pin (JP1 is the 2-pin battery connector; VBUS is used only for power/charging). Firmware cannot directly sense "on USB vs on battery."

## (e) LDOs & regulators
| Ref | Part | In → Out | CE | Rating |
|---|---|---|---|---|
| `U3` | **ME6217C33M5G** | `+5` → **VCC3V3** | **tied to VIN (always on)** | 3.3 V ±1%, 800 mA max, dropout ~100 mV @300 mA, VIN 2–6.5 V. `ME6217_LDO.pdf p.2,4` |
| `U4` | **ME6217C33M5G** | `+5` → **AU_VCC3V3** | **tied to VIN (always on)** | same part; separate audio rail. |

- ME6217 CE pin (SOT23-5 pin 3) is an active-high ON/OFF (`ME6217_LDO.pdf p.2 pin table`, `p.5 shutdown operation`). On this board **both CE pins are tied to VIN**, so the 3.3 V rails are permanently enabled and **cannot be gated by firmware**. Datasheet warns CE must not float — hardwiring to VIN satisfies that.
- `U6 SC8002B` (audio power amp) and `U5 ES8311` (codec) are **loads**, not rails; the SC8002B has a `SHUTDOWN` pin driven by `AUDIO_EN` — audio-domain, out of scope here.

---

## Firmware notes / gotchas
1. **Battery gauging is the only battery telemetry available.** No CHRG, no STDBY, no VBUS-present line reaches a GPIO. Estimate charge/discharge state purely from the GPIO9 ADC trend.
2. **GPIO9 is dedicated to the battery ADC and is not broken out** (CSV line 14, "是否引出=N"). Don't repurpose it. Use ADC1 oneshot + curve-fit cal, atten 12 dB, ×2.
3. **Charger is fully autonomous** — do not expect any register/pin to start/stop charging.
4. **`+5` is not a stable 5 V** — treat it as ~3.0–4.7 V. Anything needing true 5 V does not exist on-board; only VBUS (present only when USB attached) is ~5 V.

## Cross-source conflicts
1. **Example_13 header comment vs schematic/CSV (pin lies):** `GetBatteryVoltage.ino:4-5` header comment claims `BAT_VOLT_ADC = 34` and `BL = 27`. Both are **wrong** — schematic/CSV give battery ADC = **GPIO9** (`ADC1_CH8`) and backlight = **GPIO45** (`Example_14 Backlight_pwm_test.ino:23`; CSV IO45 line). The Example_13 *code itself* uses `ADC1_CH8` (=GPIO9), contradicting its own comment. Comment is copy-paste junk; trust the schematic/CSV/`#define`.
