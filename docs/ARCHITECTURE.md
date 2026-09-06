# hughes_bridge — architecture & protocol

Read this before touching the firmware or wiring up the collector. It's the
"why" behind every decision the code takes as given.

## The problem

The rig is on a **50A shore connection** (two 120V legs). The Victron/Cerbo
inverter system is fed off **one leg** and only backs the inside-the-house loads
— not the water heater, washing machine, or air conditioning. So the powerplant
dashboard, which reads the Cerbo, has only ever seen part of the picture.

The **Hughes Power Watchdog** is the surge protector inline on the 50A inlet. It
meters **both legs at the pedestal** — which is the whole rig, and the number a
park's meter actually bills. With an upcoming stay at **$0.28/kWh**, that total
is the one we need, and it's the half we weren't collecting.

## The system

```
  Hughes Watchdog (Gen 1, 50A)
        │  BLE notify stream (~1/s, both legs)
        ▼
  Hosyond ESP32-S3  ──► hughes_bridge  (this repo)
        │  WiFi / HTTP  GET /status  (per-leg V/A/W/kWh/Hz/err + raw hex)
        ▼
  raspbpi : hughes-collect  (separate service — NOT built yet)
        │  - polls /status
        │  - tiered SQLite history (copied from pepwave-panel's rollup)
        │  - stay baseline + manual reset  (the billing math lives here)
        ▼
  a local dashboard  (powerplant dashboard, on the local network)
        gains a "Shore Power (50A)" section + range-button graphs
```

Three services, one per data source — matching the fleet shape (cerbo /
easytouch / pepwave are already separate). A flaky BLE link or a downed ESP32
must never be able to wedge the live power dashboard, so ingestion is isolated in
`hughes-collect` and `a local dashboard` only reads it.

## Why this board

The board is a 2.8" **4-line SPI** display (ILI9341V, 240×320), not an
RGB-parallel panel like the aitrip. That matters for BLE: an RGB panel hammers the
bus and PSRAM continuously and tends to starve the radio; an SPI screen only
touches the bus on redraw. So this board is a *good* BLE host, and the screen is a
free bonus for phase 2 — the mission runs headless.

Hardware ref: [lcdwiki 2.8inch ESP32-S3 Display](https://www.lcdwiki.com/2.8inch_ESP32-S3_Display).
Module is **ESP32-S3 N16R8** (16MB flash + 8MB OPI PSRAM) with **native USB**
(no UART chip — so `-DARDUINO_USB_CDC_ON_BOOT=1` is required for serial, and PSRAM
is OPI → `memory_type = qio_opi` when phase 2 turns it on).

The board's BLE radio is confirmed good by the vendor's own `Example_25_BLE_scan`
demo (it uses Bluedroid; we use NimBLE, fleet-consistent). It also has an onboard
**NeoPixel on GPIO 42**, which this firmware drives as a status LED (blue=
connecting, green=connected, red=fail/stale).

**Native USB (no UART chip):** the port enumerates as USB-CDC (`/dev/ttyACM*`,
not `ttyUSB*`), the first flash may need the manual BOOT+RESET bootloader entry,
and `-DARDUINO_USB_CDC_ON_BOOT=1` is required for serial. See the README.

Pin map (authoritative from the vendor demo `#define`s + `spi_dev.h` — **not** the
stale ASCII header comments in the demos, which list wrong pins: the BLE demo's
comment, and the SD demo's `SD_CS 7 / LED 21 / DC 2` are all copy-paste junk).
Recorded so phase 2 doesn't re-derive it. Board variant: **ES3C28P / ES3N28P**,
2.8" IPS, ILI9341V. Confirmed from vendor Examples 05 (SD), 08 (touch), 14 (backlight).

| Function | GPIO | | Function | GPIO |
|---|---|---|---|---|
| LCD CS | 10 | | LCD MISO | 13 |
| LCD DC | 46 | | LCD RST | -1 (board RST) |
| LCD SCLK | 12 | | Backlight | 45 (active HIGH, PWM/ledc) |
| LCD MOSI | 11 | | Touch SDA / SCL | 16 / 15 (FT6336, I2C) |
| NeoPixel | 42 | | Touch INT / RST | 17 / 18 |
| LCD bus | SPI3/HSPI (FSPI pins) | | SPI freq (build) | 40 MHz |

**SD card is on its OWN bus** — the ESP32-S3 **SDMMC (SDIO) controller, 4-bit**, NOT
the LCD's FSPI. So SD logging and screen redraws do **not** contend (this was an
open worry; it's resolved). Vendor call: `SD_MMC.setPins(CLK,CMD,D0,D1,D2,D3)`.

| SDMMC | GPIO | | SDMMC | GPIO |
|---|---|---|---|---|
| CLK | 38 | | D0 | 39 |
| CMD | 40 | | D1 | 41 |
| | | | D2 / D3 | 48 / 47 |

Display stack the vendor ships: **TFT_eSPI** (Bodmer) + an **FT6336** touch lib,
with **LVGL** in Example_08. Board also carries an **ES8311 audio codec** and a
**battery-voltage divider** (Examples 13/16/17) — out of scope but present.

Display bring-up (milestone 2, validated 2026-08-07): TFT_eSPI is configured via
build flags in `platformio.ini [display]` (HSPI, 40 MHz), FT6336 is vendored in
`lib/FT6336`. **This IPS panel needs `-DTFT_INVERSION_ON`** — without it every colour
renders as its exact complement (white↔black, red↔cyan); the vendor's `User_Setup.h`
omits it. PSRAM stayed **OFF** (a 240×320 panel drives fine from internal SRAM;
LVGL later needs only a small partial buffer) — so the documented OPI-PSRAM bootloop
risk was sidestepped entirely, deferred to whenever smooth full-screen animation
actually needs it.

BLE-central + WiFi on one ESP32-S3 is already proven in the fleet: that's exactly
what `easytouch_bridge` does. Same single 2.4GHz radio, time-sliced, at a trivial
data rate (one BLE packet/sec, occasional HTTP).

## BLE model: hold-and-subscribe, not poll-and-release

This is the one structural difference from `easytouch_bridge`.

- **easytouch** is transactional: scan → connect → read → **disconnect** every
  cycle, deliberately releasing the link so the phone app can slip in. It needs a
  command queue and auth because it also *controls* the AC.
- **Hughes Gen 1** is a **notification stream**: connect once, subscribe to
  `ffe2`, and sit there while the device pushes a packet ~1/sec. So this firmware
  **holds** the connection and reconnects on drop.

Consequences baked into the design:

- **One client at a time.** While the bridge is connected, the Hughes phone app
  cannot connect, and vice versa. Fine for a fixed install; just know it during
  setup (kill the app to let the ESP grab the link).
- **Command path — FOUND 2026-08-07 (contra the references).** Gen 1 *does* take
  commands: plain **ASCII strings written to characteristic `0xfff5`** (RWw) on the
  `ffe0` service. Captured off the phone app with an ESP32 GATT emulator (the
  `-e emulate` build cloned the unit; the app connected and we logged its writes):
  `"POWER ON TIME"` (uptime query) and **`"RESEt"`** (bytes `52 45 53 45 74`) — which
  **resets the lifetime kWh odometer**. The public reverse-engineering concluded "V1
  writes are ignored"; they just never hit the right characteristic + string format.
  So the bridge is read-only by DESIGN, not necessity — a standalone odometer reset
  is now possible. The full GATT (dumped via `-e wdinfo`) also exposes `1003` [W] and
  `1005` [RW] and a TI OAD firmware-update service (`f000ffd0`), all unused here.

## Gen 1 (V1) protocol

Advertised name prefixes: `PMD*` / `PWS*` / `PMS*`.

| Item | UUID |
|---|---|
| Service | `0000ffe0-0000-1000-8000-00805f9b34fb` |
| TX / notify (device → us) | `0000ffe2-0000-1000-8000-00805f9b34fb` |
| **Command (us → device, ASCII)** | `0000fff5-0000-1000-8000-00805f9b34fb` — write `"RESEt"` to reset the kWh odometer |

Connect, subscribe to `ffe2`, decode. **No handshake, no init command.**

### The 40-byte data packet

Arrives as **two 20-byte notifications** buffered together (the device's ATT MTU
splits it; negotiating a larger MTU on our side doesn't change what it sends). All
multi-byte fields are **big-endian int32**.

| Bytes | Field | Decode |
|---|---|---|
| 0–2 | Header | `01 03 20` (marks a data packet) |
| 3–6 | Voltage | ÷ 10000 → V |
| 7–10 | Current | ÷ 10000 → A |
| 11–14 | Power | ÷ 10000 → W |
| 15–18 | Energy | ÷ 10000 → kWh (**lifetime odometer**) |
| 19 | Error code | byte: 0=OK, 1–9 = E1–E9, 11=F1, 12=F2 |
| 31–34 | Frequency | ÷ **100** → Hz |
| 37–39 | Line ID | `00 00 00` = L1, `01 01 01` = L2 |

50A units send a **separate packet per leg**, tagged by Line ID; a 30A unit sends
only L1. (Error-history records are a different 16-byte `Er…E` format — not
decoded here; we only need live telemetry.)

### Reassembly (header-keyed, self-healing)

```
notif arrives:
  starts with 01 03 20  ->  buf = notif;  have = len      (chunk 1 — (re)sync)
  else if have > 0      ->  buf += notif; have += len      (chunk 2)
  if have >= 40         ->  decode(buf);  have = 0
```

A mid-stream glitch just drops the partial packet; the next header resynchronizes.
`decodePacket()` reads the Line ID and routes into `g_leg[0]` (L1) or `g_leg[1]`
(L2), each carrying its own last-seen age.

## Decode on the ESP, keep the raw bytes too

`/status` emits decoded numbers **and** the raw 40-byte packet as hex per leg. The
decode is certain and trivial, so clean numbers are worth serving; but the raw
hex is a cheap safety net — if a field is mis-scaled on the actual hardware, the
collector can re-derive it from bytes without a reflash. (Same instinct as the
Govee raw-passthrough on `easytouch_bridge`, applied narrowly.)

`VERBOSE_RAW` (a `-e bridge` build flag, off by default) dumps every notification
to serial during bring-up, so a new unit validates the header, the alternating Line
IDs, and the scaling. The Watchdog has **no display** (only an error-code LCD) and
the app can't connect while we hold the link, so "validate" means internal
consistency (V×A vs W → sane power factor) + the protocol provenance below, not a
readout cross-check — see `LEDGER.md`. Drop the flag once confirmed.

## Where the billing math lives

The firmware is deliberately dumb: it reports the **raw lifetime kWh** per leg and
the instantaneous values. It does **not** compute a stay total.

The Hughes energy field is an odometer that never resets per stay (and can't be
reset over BLE on Gen 1). So `hughes-collect` on the Pi:

- **snapshots a baseline** at plug-in — `stay_kWh = (L1+L2 now) − (L1+L2 baseline)`,
  `× 0.28` for running cost,
- offers a **manual reset** (re-baseline to now) — shipping now; auto-detect of a
  fresh session is a later nicety,
- keeps a **tiered SQLite history**, copied from `pepwave-panel`'s rollup:

  | Tier | Bucket | Keeps | Fed from |
  |---|---|---|---|
  | `m1` | 60 s | 48 h | live |
  | `m15` | 15 min | 14 d | m1 |
  | `h1` | 1 h | 60 d | m15 |

  which maps onto the dashboard range buttons: 1h/6h → `m1`, 24h/7d → `m15`,
  30d/60d → `h1`. Primary graphed series is **power (W)** per leg + combined
  (the rate, like the Comms Array's throughput); **voltage** is a useful second
  series (pedestal sag under load, the flip side of the E1 undervoltage fault).

Keeping this on the Pi means the money math can be corrected and re-baselined
without touching firmware.

## Bring-up order

Phase 1 is done (see `LEDGER.md`): unit identified via `-e scan` + power-cycle,
decode validated with `-e bridge` + `VERBOSE_RAW`, flag dropped, clean `/status`.

For a **new board** now that provisioning exists:
1. Flash `-e bridge`, join the `Hughes-Setup` portal, pick WiFi + Watchdog. The
   on-device picker replaces the manual `-e scan` step for a sighted setup; `-e scan`
   stays as a bare serial diagnostic for a screenless bench board.
2. (New/uncertain unit only) re-add `-DVERBOSE_RAW`, confirm the byte layout, drop it.
3. **`hughes-collect`** on raspbpi (poll + tiered DB + baseline/reset), then the
   `a local dashboard` Shore Power section.

## Roadmap

- **Phase 1 (now):** headless bridge + collector + dashboard section, before the stay.
- **Phase 2:** local readout on the 2.8" screen (shore W / stay kWh) at the bay.
- **Later:** auto stay-detection; optional Gen-1 command support *if* someone ever
  captures the official app's write framing (not expected).

## Provenance

Gen 1 protocol reconciled from three independent sources: the original spbrogan
ESPHome `hughes_power_watchdog` component, john-k-mcdowell's `docs/protocol.md`
(HCI captures + `powerwatchdog2` app reverse-engineering), and
TechBlueprints/dbus-power-watchdog's independent Gen 1 decoder — all three agree
on the byte layout above.
