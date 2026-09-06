# hughes_bridge — documentation index

Start-here map of every doc in this project, so a fresh session (human or AI) can
orient fast. **On a cold start, read in this order:**

1. [`../LEDGER.md`](../LEDGER.md) — **live cross-session state**: confirmed hardware
   facts (MAC/IP), gotchas, open items, and the dated session changelog. Read this first.
2. [`../README.md`](../README.md) — what the product is + build/flash quickstart.
3. [`ARCHITECTURE.md`](ARCHITECTURE.md) — the "why": BLE protocol, packet decode, the
   three-service system design, and the roadmap.
4. [`board/`](board/) — distilled, **cited** hardware reference for the display board
   (pinout + every subsystem). Start at its own [`board/README.md`](board/README.md).
5. [`status-spec.md`](status-spec.md) — the `/status` JSON shape + 60 s rolling aggregates.

---

## `docs/` contents

| Path | Covers | Read when |
|---|---|---|
| [ARCHITECTURE.md](ARCHITECTURE.md) | Problem/system design, BLE hold-and-subscribe model, Gen-1 protocol + 40-byte packet decode, command channel (`fff5`/`RESEt`), billing-math split, roadmap. | Before touching firmware or the collector. |
| [status-spec.md](status-spec.md) | `/status` field-by-field spec incl. `watts_avg`/`peak` + `combined_watts_avg` 60 s aggregates. | Building/consuming the HTTP API (e.g. `hughes-collect`). |
| [board/](board/) | Hardware truth for the ES3C28P/ES3N28P 2.8" ESP32-S3 board — see below. | Any pin/peripheral question, or new hardware bring-up. |

## `docs/board/` — board reference (machine-mined, cited)

Distilled from the vendor documentation pack (datasheets, schematics, IO-allocation
sheet, 30 vendor examples) by a subagent swarm; **every fact is traced to a primary
source**. This folder exists because the vendor demo header comments list the *wrong*
pins — these docs replace those comments with schematic-verified truth.

| Doc | Covers |
|---|---|
| [board/README.md](board/README.md) | **Index + the single consolidated authoritative pinout** (46 GPIOs) + conflicts + reconciliation against this firmware. **The pin source of truth.** |
| [board/pinout-io.md](board/pinout-io.md) | Full GPIO map, the 8 header pins (only 4 truly free), strapping/reserved buses. |
| [board/esp32-s3-core.md](board/esp32-s3-core.md) | ESP32-S3R8 chip: OPI PSRAM + QSPI flash, native USB, strapping/boot, ADC units/atten. |
| [board/power-battery-charging.md](board/power-battery-charging.md) | TP4054 charger (no firmware control, no CHRG line), battery ADC on GPIO9 (÷2), discrete USB↔battery UPS path, ME6217 LDOs. |
| [board/display-ili9341v.md](board/display-ili9341v.md) | ILI9341V pins, SPI host/clock, IPS inversion (0x21) required, MADCTL/rotation, LEDC backlight, full init. |
| [board/touch-ft6336.md](board/touch-ft6336.md) | FT6336G cap-touch: I2C 0x38 shared bus, INT/RST, register map, power modes, rotation mapping. |
| [board/audio-es8311.md](board/audio-es8311.md) | ES8311 codec (I2C 0x18, I2S 16 kHz), SC8002B/FM8002E amp (EN=GPIO1 active-LOW), MEMS mic, init regs. |
| [board/sd-storage.md](board/sd-storage.md) | microSD on native SDMMC/SDIO 4-bit, own bus (no LCD contention), pins, no card-detect. |
| [board/status-led-ws2812.md](board/status-led-ws2812.md) | WS2812B on GPIO42, GRB 24-bit, count=1, timing. |
| [board/peripherals-misc.md](board/peripherals-misc.md) | Internal RTC, hw timer, BOOT button (GPIO0), UART0 (43/44), WiFi/BLE, chip antenna. |
| [board/sample-code-index.md](board/sample-code-index.md) | Catalogue of all 30 vendor Arduino examples + the `Replaced files` overrides → which example demos which subsystem. |

> Open hardware question the mining surfaced: **I2S audio data direction (GPIO6 vs GPIO8)** —
> schematic/CSV say DOUT=6/DIN=8, hardware-tested demo code says DOUT=8. Verify on the board
> before trusting either for audio work. See [board/README.md](board/README.md#conflicts--open-questions).

## Project docs outside `docs/`

| Path | Covers |
|---|---|
| [../LEDGER.md](../LEDGER.md) | Live session state, confirmed hardware facts, gotchas, open items. **The durable memory.** |
| [../README.md](../README.md) | Product overview, quickstart, `/status` shape, status LED, layout. |
| [../case/README.md](../case/README.md) | 3D-printed enclosure (OpenSCAD): shell + bezel, battery bay, print/assembly notes. |
