/*
 * config.example.h  --  copy to config.h (gitignored) and fill in.
 *
 *   cp include/config.example.h include/config.h
 *
 * Only the BRIDGE build (`pio run -e bridge`) reads this. The SCAN build
 * (`-e scan`) ignores it entirely -- you can run the scan straight after clone
 * with no secrets on disk.
 */
#pragma once

#define WIFI_SSID   "your-ssid"
#define WIFI_PASS   "your-pass"

// Target Watchdog. PREFER the MAC -- get it from the scan build, and only if the
// scan reported its address type as 'public' (a 'random' address may rotate; pin
// by leaving this empty + relying on the name-prefix fallback instead).
// Leaving this empty matches ANY PMD*/PWS*/PMS* / 0xffe0 advertiser: fine if
// yours is the only Watchdog in range, risky at a crowded park.
#define HUGHES_MAC  ""              // e.g. "0c:61:cf:32:4b:81"

#define HTTP_PORT   80

// Onboard status LED is GPIO 42 (WS2812). Uncomment to move it, or set -1 to disable.
// #define LED_PIN   42
