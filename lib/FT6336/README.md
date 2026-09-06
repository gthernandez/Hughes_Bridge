# FT6336 (vendored)

Capacitive-touch driver for the FT6336G on the 2.8" ES3C28P/ES3N28P board, copied
**verbatim** from the vendor SDK (`1-Demo/Arduino/Install libraries/FT6336-arduino`,
the lcdwiki/QDtech demo bundle for this display). Kept unmodified so it can be
diffed against the vendor source if the upstream ever updates.

- I2C addr `0x38`; `begin()` calls `Wire.begin(sda,scl)` and runs the reset + ID
  handshake itself — no separate `Wire.begin` needed.
- Pins on this board: SDA 16 · SCL 15 · INT 17 · RST 18 (see `docs/ARCHITECTURE.md`).
- Construct with the **native portrait** panel size: `FT6336(16,15,17,18,240,320)`,
  then `setRotation()` to match the TFT rotation.

Only compiled into envs that `#include <FT6336.h>` (currently `-e disptest`).
