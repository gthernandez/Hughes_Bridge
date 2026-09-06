# Audio Subsystem — ES8311 Codec + FM8002E/SC8002B Amp + Analog MEMS Mic

Permanent board reference for the ES3C28P/ES3N28P (ESP32-S3 2.8" IPS) audio path: I2C-controlled ES8311 codec, I2S data path, the class-D power amplifier and its enable pin, and the analog MEMS microphone. Written for hughes_bridge firmware.

## Sources mined
- `5-Schematic/2.8inch_ESP32-S3_Display_Module_Hardware_Schematic.pdf` (audio + amp + mic + module-pin sections) — **authority rank 1**
- `5-Schematic/2.8inch_ESP32-S3_Display_Schematic.pdf` (same nets, block labels in English) — **rank 1**
- IO-allocation CSV (`.../scratchpad/board_csv/io_alloc_sheet1.csv`, from `5-Schematic/ESP32-S3_Chip_IO_Resource_Allocation.xlsx`) — **rank 1**
- `4-DataSheet/ES8311_DS.pdf` (I2C address, clocking, registers) — **rank 2**
- `4-DataSheet/FM8002E.pdf` (amplifier; Chinese, text partly garbled) — **rank 2**
- `4-DataSheet/MEMS_MIC_LMA2718B381-OA7.PDF` — **rank 2**
- `1-Demo/Arduino/Demo/Example_16_music/` (`music.ino`, `es8311.cpp/.h`, `es8311_reg.h`, `ESP_Panel_Board_Custom.h`) — **rank 4**
- `1-Demo/Arduino/Demo/Example_17_echo/echo.ino` (native ESP-IDF I2S config) — **rank 4**

---

## 1. Pin map (audio-relevant GPIOs)

| Signal | ESP32-S3 GPIO | Chip pin | Direction | Confirmed by |
|--------|---------------|----------|-----------|--------------|
| Amp enable (`AUDIO_EN`) | **GPIO1** | 6 | out, **active-LOW** | CSV row (GPIO1); schematic net `AUDIO_EN`→FM8002E pin1 SHUTDOWN; demo `AP_ENABLE 1` |
| I2S **MCLK** | **GPIO4** | 9 | out (ESP master) | CSV; schematic net `I2S_MCK`; `echo.ino:77` `.mclk=GPIO_NUM_4` |
| I2S **BCLK/SCLK** | **GPIO5** | 10 | out (ESP master) | CSV; schematic net `I2S_SCK`; `echo.ino:78` `.bclk=GPIO_NUM_5` |
| I2S **WS/LRCK** | **GPIO7** | 12 | out (ESP master) | CSV; schematic net `I2S_LRC`; `echo.ino:79` `.ws=GPIO_NUM_7` |
| I2S data — **GPIO6/GPIO8** | 6 / 8 | 11 / 13 | **see §2 conflict** | disputed |
| I2C **SCL** (`AU_SCL`) | **GPIO15** | 20 | out (ESP master) | CSV; schematic; `ESP_Panel_Board_Custom.h:24` `I2C_SCL 15` |
| I2C **SDA** (`AU_SDA`) | **GPIO16** | 22 | bidir | CSV; schematic; `ESP_Panel_Board_Custom.h:25` `I2C_SDA 16` |

Notes:
- **MCLK=4, BCLK=5, WS=7 are agreed by all three source tiers** (schematic net names, IO-CSV, demo code). Treat as authoritative.
- The I2C bus (GPIO15/16, `I2C_NUM_0`, 400 kHz) is **shared** between the ES8311 codec and the FT6336G touch controller. CSV rows GPIO15/GPIO16: "when using touch OR audio, these can only be used as the I2C clock/data pins." Cite: CSV rows GPIO15 (pin 20/21) and GPIO16 (pin 22).
- Amp-enable prior-project finding **(GPIO1, active-LOW) is CONFIRMED** — see §4.

---

## 2. ✅ RESOLVED — I2S DOUT vs DIN (GPIO6 / GPIO8)

> **CONFIRMED ON HARDWARE (intercom firmware, 2026-08): `DOUT=GPIO8`, `DIN=GPIO6`** — i.e. the
> **vendor-demo mapping wins**, NOT the rank-1 schematic/CSV net labels. The vendor
> `Example_17_echo` (native `i2s_std`, STEREO, MCLK 384×) plays audibly with `dout=8/din=6`;
> the reversed mapping is silent. This is the case the doc flagged below where the schematic
> net-label *direction convention* is exactly what was in question, so the rank-1 "sources" were
> effectively ambiguous. **Use DOUT=8 / DIN=6.** (Also note: the ES8311 is a SLAVE — playback
> only works with STEREO framing + MCLK 384× from the native driver; the Arduino `ESP_I2S`
> wrapper's MONO/256× is silent. See §3.2.)

The electrically-fixed constraint (either way): ESP **DOUT → codec DSDIN** (playback) and ESP **DIN ← codec ASDOUT** (mic). Only the GPIO6/GPIO8 assignment is in dispute.

| Source (tier) | ESP DOUT (playback→codec) | ESP DIN (mic←codec) |
|---------------|---------------------------|---------------------|
| IO-CSV (rank 1) — GPIO6="I2S output pin that **sends** audio data", GPIO8="I2S input pin that **receives** audio data" | **GPIO6** | **GPIO8** |
| Schematic net names (rank 1) — `I2S_DO`=GPIO6, `I2S_DI`=GPIO8 | **GPIO6** | **GPIO8** |
| Demo code (rank 4) — `music.ino:87` `setPinout(BCK,WS,DOUT=8,MCK)`; `echo.ino:80-81` `.dout=GPIO_NUM_8, .din=GPIO_NUM_6` | **GPIO8** | **GPIO6** |

- The two **rank-1 sources (schematic + IO-CSV) agree: DOUT=GPIO6, DIN=GPIO8.** The vendor demo code uses the **opposite** and, since the vendor demos are presumably hardware-tested, the code mapping (DOUT=GPIO8, DIN=GPIO6) may be what actually works on silicon.
- **Recommendation for hughes_bridge:** this is the one audio pin you must verify empirically on the board. If playback works with `dout=GPIO8` (as the demo ships), trust the demo. Do **not** assume the schematic net label resolves it — the label direction convention is exactly what is in question. `header comments` were not the source here; this is `#define` config vs schematic net name.
- `ESP_Panel_Board_Custom.h:20-22` declares only `I2S_MCK 4 / I2S_BCK 5 / I2S_DOUT 8 / I2S_WS 7` (no DIN); the music demo is playback-only. The echo demo adds `.din=GPIO_NUM_6`.

---

## 3. ES8311 codec

### 3.1 I2C control
- **7-bit address `0x18`** (CE pin low) or `0x19` (CE high). Datasheet: "The chip address must be `0011 00x`, where x equals CE." (`4-DataSheet/ES8311_DS.pdf` p.9, §I2C interface). Board ties CE low → **0x18** (schematic annotation "IIC 0x18", module schematic p.1; demo `es8311.h:20` `ES8311_ADDRRES_0 0x18u`).
- Transfer rate up to 400 kbps (datasheet p.9); demo bus runs 400 kHz (`ESP_Panel_Board_Custom.h:27` `I2C_SPEED 400000`).
- Codec runs in **I2S slave mode**; ESP32-S3 I2S is master (`es8311.cpp:289` "ES8311 in Slave mode and I2S format"; `es8311.cpp:293` clears reg00 bit6 for slave serial port).
- Chip-ID registers (for a probe/sanity read): `0xFD` default `0x83`, `0xFE` default `0x11` (`ES8311_DS.pdf` p.29, "REGISTER 0xFD/0xFE — CHIP").

### 3.2 Clocking / MCLK-to-fs ratio
- Vendor config: **fs = 16000 Hz**, **MCLK multiple = 384** → **MCLK = 6.144 MHz**, 16-bit, stereo, Philips/I2S standard.
  - `es8311.h:25-27` `EXAMPLE_SAMPLE_RATE 16000`, `EXAMPLE_MCLK_MULTIPLE 384`, `EXAMPLE_MCLK_FREQ_HZ = 16000*384`.
  - `echo.ino:16` `EXAMPLE_MCLK_MULTIPLE I2S_MCLK_MULTIPLE_384`; `echo.ino:75` 16-bit stereo Philips slot; `echo.ino:89` `mclk_multiple = 384`.
- MCLK is taken from the **dedicated MCLK pin** (not derived from BCLK): `es8311.cpp:452-453` `.mclk_from_mclk_pin = true`, `.mclk_frequency = EXAMPLE_MCLK_FREQ_HZ`.
- ES8311 supports standard 256Fs/384Fs/512Fs etc. (`ES8311_DS.pdf` p.~8, "standard audio clocks (64F,128Fs,256Fs,384Fs,512Fs)"). Header note: "If not using 24-bit data width, 256 should be enough" (`es8311.h:26`) — i.e. 384× is the vendor's chosen headroom, 256× is also valid for 16-bit.
- Clock dividers are looked up from a `coeff_div[]` table keyed on (mclk, rate) and written to regs `0x02`–`0x08` (`es8311.cpp:41-142`, `es8311.cpp:173-227`). For {6.144 MHz, 16 kHz} the demo actually programs via `EXAMPLE_SAMPLE_RATE * EXAMPLE_MCLK_MULTIPLE` = 6.144 MHz (`es8311.cpp:459`).

### 3.3 Essential init register sequence
From `es8311_init()` (`es8311.cpp:318-349`) + `es8311_codec_init()` (`es8311.cpp:445-463`). Register name map in `es8311_reg.h`.

| Reg | Value | Meaning | Cite |
|-----|-------|---------|------|
| `0x00` | `0x1F` → (20 ms) → `0x00` → `0x80` | reset digital/CSM/clock-mgr, then power-on | `es8311.cpp:330-333` |
| `0x01` | `0x3F` (+bit7 only if MCLK from SCK) | enable all codec clocks; here MCLK from MCLK pin so bit7=0 | `es8311.cpp:232,246` |
| `0x02`–`0x08` | from `coeff_div[]` | pre-div/mult, ADC/DAC OSR, BCLK & LRCK dividers | `es8311.cpp:187-224` |
| `0x00` | clear bit6 (`&=0xBF`) | slave serial port (I2S) | `es8311.cpp:292-293` |
| `0x09` / `0x0A` | `0x0C` for 16-bit | SDP-in / SDP-out resolution (`(3<<2)` = 16-bit) | `es8311.cpp:263-264,296-300` |
| `0x0D` | `0x01` | power up analog | `es8311.cpp:341` |
| `0x0E` | `0x02` | enable analog PGA + ADC modulator | `es8311.cpp:342` |
| `0x12` | `0x00` | power up DAC | `es8311.cpp:343` |
| `0x13` | `0x10` | enable output to HP/line drive | `es8311.cpp:344` |
| `0x1C` | `0x6A` | ADC EQ bypass, cancel DC offset | `es8311.cpp:345` |
| `0x37` | `0x08` | DAC EQ bypass | `es8311.cpp:346` |
| `0x32` | `(vol*256/100)-1` | DAC volume; demo sets vol=85 | `es8311.cpp:356-375,460`; `es8311.h:28` |

Playback vs mic:
- **DAC/playback** path is fully powered in the standard init above.
- **Mic/ADC** path: `es8311_microphone_config()` writes `0x17=0xC8` (ADC gain) and `0x14=0x1A` (**enable analog MIC** + max PGA gain; OR bit6 for a PDM digital mic) — `es8311.cpp:305-316`. **In the music demo this call is commented out** (`es8311.cpp:461`); the echo demo drives capture through the I2S RX channel (`echo.ino:137` `i2s_channel_read`). For hughes_bridge mic use, call `es8311_microphone_config(dev, false)` (analog mic) explicitly.

---

## 4. Power amplifier — FM8002E (board silkscreen: SC8002B)

- **Part naming:** the datasheet supplied is **FM8002E** (Shenzhen Fine Made, "S&C IC1301"), but the schematic reference on this board reads **`SC8002B`** (`sch_module.txt` amp block, `2.8inch_ESP32-S3_Display_Module_Hardware_Schematic.pdf`). These are pin-compatible SOP-8 mono BTL audio amps; treat FM8002E datasheet as the electrical reference. *(derived: equivalence inferred from identical SOP-8 pinout + "S&C" vendor mark)*
- **Enable / SHUTDOWN pin (pin 1):** driven by net `AUDIO_EN` = **ESP32-S3 GPIO1**. **Active-LOW: drive GPIO1 LOW to run the amp, HIGH to shut it down.**
  - IO-CSV row GPIO1 (chip pin 6): "音频功放IC使能引脚，低电平使能" = *audio power-amplifier IC enable pin, LOW-level enable* — **rank 1**.
  - Both demos drive it LOW at startup before/for playback: `music.ino:76-77` and `echo.ino:160-161` `pinMode(AP_ENABLE, OUTPUT); digitalWrite(AP_ENABLE, LOW);` (`AP_ENABLE` = `1`, `ESP_Panel_Board_Custom.h:10`).
  - ⇒ As wired, the FM8002E "SHUTDOWN" input is held LOW for normal operation. (The FM8002E datasheet text is garbled in extraction and does not cleanly state the pin polarity; the board-level polarity above is taken from the rank-1 CSV + demo behavior, which agree.) **Prior finding GPIO1 active-LOW: CONFIRMED.**
- **Topology:** BTL bridge output. FM8002E pin5 `VO1`→ speaker `SP-`, pin8 `VO2`→ speaker `SP+`; differential inputs pin3 `+IN` / pin4 `-IN` fed from ES8311 DAC `OUTP`/`OUTN` (schematic amp block: `1 SHUTDOWN | VO2 8 SP+`, `2 BYPASS | GND 7 SP-`, `3 +IN | VDD 6`, `4 -IN | VO1 5`; ES8311 `OUTP`/`OUTN` at codec pins 16/17).
- **Gain:** AV = 2·(Rf/Ri) (`FM8002E.pdf` p.2). Board uses Rf=20K, Ri=20K, Ci=0.39 µF (schematic values R23/R24 20K, C42/C45 0.39 µF) ⇒ **gain ≈ 2 V/V (~6 dB)**. *(derived from schematic component values)*
- **BYPASS pin (pin2):** 1 µF to AP_GND (C43, schematic) — required per datasheet ("Bypass ... 1.0µF", `FM8002E.pdf` p.2).
- **Ratings (from FM8002E.pdf p.3, garbled but legible fields):** VDD abs-max ~6 V; ~2 W into 4 Ω / ~1.5 W into 8 Ω at 5 V; shutdown current ~4.2 µA; switching ~2.5 MHz (class-D). Speaker connects via a 2-pin header (`SP+`/`SP-`). The amp is powered from **+5 V** (schematic `+5`), separate `AP_GND`.

---

## 5. MEMS microphone — LMA2718B381-OA7 (analog, LinkMems)

- **Analog** electret-replacement MEMS with integrated pre-amp ASIC — single-ended analog output, **not PDM/digital** (`MEMS_MIC_LMA2718B381-OA7.PDF` p.3, "integrated with specialized Pre-amplification ASIC"). This is why the ES8311 is configured for the **analog** MIC input (`0x14=0x1A`), not DMIC.
- **Pinout (4-pad):** 1 `OUT` (signal), 2 `GND`, 3 `VDD` (power), 4 `GND` (`MEMS_MIC_...PDF` p.5 mechanical table).
- **Board wiring:** mic `OUT` → net `MIC_OUT` → ES8311 differential mic input `MIC1P`/`MIC1N` (codec pins 1/19) via series R20 and R19 (0 Ω); mic `VDD` from net `MIC_VDD`; DC-block/filter caps around the codec inputs (schematic "MIC control circuit" block; ES8311 pins `MIC1P/DMIC_SDA`, `MIC1N`).
- **Key specs (`MEMS_MIC_...PDF` p.3, VDD=2.0 V):** omnidirectional; sensitivity −38 dB typ (−39/−37) @94 dB SPL 1 kHz; SNR ~60 dB(A); operating VDD 1.6–3.6 V; current ~120 µA; AOP 125 dBSPL @10% THD; output impedance ≤300 Ω.

---

## 6. Firmware quick-reference (for hughes_bridge)

```c
// I2C control (shared with FT6336 touch)
#define AUDIO_I2C_PORT   I2C_NUM_0
#define AUDIO_I2C_SCL    15
#define AUDIO_I2C_SDA    16
#define AUDIO_I2C_HZ     400000
#define ES8311_ADDR_7BIT 0x18      // CE low

// I2S (ESP32-S3 = master, ES8311 = slave)
#define I2S_MCLK 4                 // agreed by all sources
#define I2S_BCLK 5                 // agreed by all sources
#define I2S_WS   7                 // agreed by all sources
// DOUT/DIN: **CONFIRMED ON HW** (intercom firmware, 2026-08) = the vendor-demo values.
#define I2S_DOUT 8                 // ESP -> codec DAC (speaker)  -- CONFIRMED
#define I2S_DIN  6                 // codec ADC (mic) -> ESP      -- CONFIRMED

// Clocking: fs=16k, MCLK=384*fs=6.144MHz, 16-bit STEREO Philips  -- CONFIRMED ON HW.
// IMPORTANT: this exact combo (native driver/i2s_std, STEREO, MCLK 384x) is what makes sound.
// The Arduino ESP_I2S wrapper hardcodes MCLK 256x and, with MONO framing, is SILENT on this
// board (the slave ES8311 gets framing it can't clock the DAC from; reg 0x02 divider reads
// 0x00 under 256x vs 0x48 under 384x). Use the native i2s_std driver + STEREO for playback.

// Power amp enable — ACTIVE LOW
#define AUDIO_AMP_EN 1             // GPIO1
// gpio_set_level(AUDIO_AMP_EN, 0);  // 0 = amp ON, 1 = amp shutdown
```
