# `/status` field spec — 60s rolling aggregates

> **Status: implemented in firmware 2026-08-08** (`legJson`/`computeAgg`, per-leg 64-tick
> freshness masks `g_freshL1`/`g_freshL2`). Validated on hardware: `window_s` climbs 0→60 over
> the first minute and caps; `combined_watts_avg` = sum of per-leg avgs; empty window → `null`.

**For the firmware session.** `hughes-collect` (raspbpi) will poll `/status` once
per ~60s and store it into a tiered history (m1=60s / m15 / h1), then feed the
`a local dashboard` powerplant dashboard. A once-a-minute poll of the *instantaneous*
reading is a coin-flip snapshot of that minute. This spec adds a 60s rolling
**average** and **peak** so the minute value is representative — computed on the
ESP because the ESP is the only place the per-second samples exist (the Pi can't
average what it doesn't poll, and we deliberately don't poll 1/s).

Cheap to implement: the firmware already keeps **1 Hz ring buffers** for the
GRAPHS page (per-leg watts, per-leg deci-amps). These aggregates are a scan of the
**last 60 entries** of those same rings — no new sampling, no new timer.

## Principles

- **Additive and non-breaking.** Every existing key stays exactly as-is with the
  same name, type, and meaning. New keys are added alongside. A missing new key
  (during rollout) must be tolerated by the collector, so order/rollout is free.
- **Division of labor:** the **ESP owns sub-minute aggregation** (it sees it); the
  **Pi owns minute→15min→hour rollup, retention, and billing**.
- **Never aggregate `kwh`.** It is a cumulative lifetime odometer; the delta
  between two reads is already exact. Averaging it is meaningless. Leave it raw.

## New per-leg fields

Added inside each object in `legs[]`, next to the existing `watts`/`amps`:

| Field | Type | Definition |
|---|---|---|
| `watts_avg` | number (int, `%.0f`) | mean of the leg's watts over the last 60s of received samples |
| `watts_peak` | number (int, `%.0f`) | max of the leg's watts over the same window |
| `amps_avg` | number (`%.1f`) | mean of the leg's amps over the window |
| `amps_peak` | number (`%.1f`) | max of the leg's amps over the window |
| `window_s` | integer | how many real samples the window actually covers, 0–60 |

`volts`/`watts`/`amps`/`kwh`/`hz`/`err`/`age_s`/`raw`/`line` are **unchanged**
(still the latest instantaneous packet, `kwh` still the raw odometer).

## New top-level field

| Field | Type | Definition |
|---|---|---|
| `combined_watts_avg` | number (int, `%.0f`) | `legs[0].watts_avg + legs[1].watts_avg` |

`combined_watts` (instantaneous) and `combined_kwh` (raw odometer sum) are
**unchanged**. No combined *peak* — the two legs' peaks rarely coincide, so a sum
would overstate; the collector can sum the per-leg peaks itself if it ever wants
one, with that caveat understood.

## Window semantics (the part that matters)

- **Rolling, ending "now"** — "the last 60s," not a tumbling clock-minute. The Pi
  polls ~every 60s and takes a fresh trailing window each time; no boundary
  coordination between the two boxes.
- **Only real samples count.** The window is over samples actually received in the
  last 60s, not the last 60 ring *slots*. During a gap (link down, `released`, or
  a power-cycle) the ring stops being fed, so the window must shrink, not average
  in stale data. Track it however is simplest — e.g. count samples whose age ≤ 60s,
  or a per-1Hz-tick counter that only advances when a packet arrived.
- **`window_s` is the honesty signal.** It tells the Pi how much data backs the
  average: 60 = a full clean minute; a small number = partial (just booted,
  reconnected, or recovering from a gap). The Pi still uses the value but knows
  the confidence.
- **Empty window → `null`.** If `window_s == 0` (no fresh samples — link down long
  enough that the window emptied), emit `watts_avg`/`watts_peak`/`amps_avg`/
  `amps_peak` as JSON `null` (not `0` — a real 0 W average and "no data" must be
  distinguishable; a literal 0 would corrupt the Pi's stored average). `window_s`
  stays `0`. The collector records this minute as a **gap**, not a zero.

## Edge cases

- **`connected:false` / `released:true`** — the ring isn't being fed, so `window_s`
  decays toward 0 and the aggregates go `null` as above. The Pi already keys
  freshness off `connected`/`released`/`age_s`; this just makes the averages agree.
- **30A single-leg unit** — `legs[1]` is `null` today; leave it `null` (no avg/peak
  object). `combined_watts_avg` = `legs[0].watts_avg` in that case.
- **Boot** — `window_s` climbs 0→60 over the first minute; aggregates are valid but
  partial meanwhile. Fine.
- **`/reset_odometer`** — resets `kwh` only. The W/A aggregates are unaffected
  (they're power/current, not energy). No special handling.

## Example

Before (today):
```json
{"line":1,"volts":118.5,"amps":15.4,"watts":1384,"kwh":5.030,"hz":60.0,"err":0,"age_s":0,"raw":"0103..."}
```

After:
```json
{"line":1,"volts":118.5,"amps":15.4,"watts":1384,"kwh":5.030,"hz":60.0,"err":0,"age_s":0,
 "watts_avg":1291,"watts_peak":1710,"amps_avg":14.6,"amps_peak":18.9,"window_s":60,"raw":"0103..."}
```

Top level gains `"combined_watts_avg":1972` alongside the existing
`"combined_watts"` / `"combined_kwh"`.

Gap example (link just dropped, window emptied):
```json
{"line":1,"volts":118.5,"amps":15.4,"watts":1384,"kwh":5.030,"hz":60.0,"err":0,"age_s":47,
 "watts_avg":null,"watts_peak":null,"amps_avg":null,"amps_peak":null,"window_s":0,"raw":"0103..."}
```

## What the collector will do with it (context, not a requirement)

Store `watts_avg` as the m1 (60s) bucket value; carry `watts_peak` as a companion
"peak" series so the dashboard can show a brief spike toward the 50A trip that a
minute-average would smooth away; `window_s == 0` → gap row (no interpolation).
`kwh` delta between polls is the billed energy, independent of all the above.
