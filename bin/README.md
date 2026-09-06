# Prebuilt firmware — v2.1.0

Flash-ready images so you don't need the PlatformIO toolchain. Built from the
sanitized public source: **no credentials and no backend host baked in** — see the
"Send diagnostics" note in the top-level README (this build sends nothing to anyone).

| File | What it is | Flash offset |
|---|---|---|
| `hughes_bridge-2.1.0.factory.bin` | **Full image** (bootloader + partitions + app) for a **blank board** | `0x0` |
| `hughes_bridge-2.1.0.bin` | App-only image, for **OTA** if you host your own manifest | `0x10000` |

## Flash a blank board

With [esptool](https://github.com/espressif/esptool):

```bash
esptool.py --chip esp32s3 --port <YOUR_PORT> write_flash 0x0 hughes_bridge-2.1.0.factory.bin
```

Or use the browser flasher at https://espressif.github.io/esptool-js/ — connect the
board, add `hughes_bridge-2.1.0.factory.bin` at offset `0x0`, and Program. Then
follow **Quickstart** in the top-level README to provision it over the setup portal.

Verify your download against `SHA256SUMS` (`sha256sum -c SHA256SUMS`).
