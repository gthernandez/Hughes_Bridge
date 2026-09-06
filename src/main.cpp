/*
 * hughes_bridge  --  Hughes Power Watchdog (Gen 1 / "V1", 50A)  BLE -> WiFi/HTTP bridge
 * Target: Hosyond ESP32-S3 2.8" module. pioarduino core 3.x + NimBLE-Arduino 2.x.
 *
 * READ-ONLY. Holds a BLE connection to the Watchdog, subscribes to its notify
 * stream, decodes BOTH legs of the 50A shore feed, and re-serves them as JSON at
 * http://<ip>/status so raspbpi's hughes-collect can sample, store, and graph them.
 *
 * WHY stream-not-poll (the one real difference from easytouch_bridge):
 *   Gen 1 pushes a 40-byte data packet ~1/sec as BLE notifications on ffe2. We
 *   connect once and STAY subscribed -- easytouch instead scans/connects/reads/
 *   disconnects each cycle so the phone app can share. The trade: while we hold
 *   the link, the Hughes app cannot connect (it allows one client at a time).
 *   Gen 1 ignores BLE writes anyway, so there is no command path here -- read only.
 *
 * TWO BUILD ENVS (platformio.ini):
 *   -e scan   (-DSCAN_ONLY): list advertising Watchdogs to serial -- a bare
 *             diagnostic for a screenless new board. Normal setup instead uses the
 *             on-device Watchdog picker in provisioning mode (below).
 *   -e bridge: the bridge + first-run provisioning. WiFi creds and the target
 *             Watchdog are stored in NVS at RUNTIME -- a captive portal comes up on
 *             first boot (or whenever WiFi fails). include/config.h is now only an
 *             OPTIONAL compile-time seed, not a requirement. See LEDGER.md phase 2.
 *
 * Protocol: docs/ARCHITECTURE.md, from john-k-mcdowell's protocol.md + the
 * spbrogan ESPHome component, cross-checked against TechBlueprints/dbus-power-watchdog.
 */

#include <Arduino.h>

// NimBLE + the shared Watchdog-scan helpers aren't needed by the display or audio
// tests (they link neither NimBLE nor WiFi), so keep them out of those builds entirely.
#if !defined(DISPLAY_TEST) && !defined(AUDIO_TEST) && !defined(SCREEN_SCRUB)
#include <NimBLEDevice.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

// --- Hughes Gen 1 (V1) GATT ---
static NimBLEUUID SVC   ("0000ffe0-0000-1000-8000-00805f9b34fb");
static NimBLEUUID CH_TX ("0000ffe2-0000-1000-8000-00805f9b34fb"); // device -> us (notify: data)
static NimBLEUUID CMD   ("0000fff5-0000-1000-8000-00805f9b34fb"); // us -> device (write ASCII cmds; "RESEt" resets kWh)

// Gen 1 advertised-name prefixes (EPO models)
static const char* NAME_PREFIXES[] = { "PMD", "PWS", "PMS" };
static bool nameMatches(const String& nm) {
  for (auto p : NAME_PREFIXES) if (nm.length() && nm.startsWith(p)) return true;
  return false;
}

// BLE address type -> label. 'public' is stable (safe to pin by MAC); 'random' may
// rotate (pin by name instead). Shared by scan mode and the provisioning picker.
static const char* addrTypeStr(uint8_t t) {
  switch (t) {
    case 0: return "public";
    case 1: return "random";
    case 2: return "public-id";
    case 3: return "random-id";
    default: return "?";
  }
}
#endif  // !DISPLAY_TEST

// ======================================================================
#ifdef SCAN_ONLY
// SCAN MODE -- bare diagnostic: identify a unit by RSSI + power-cycle. The normal
// path is the on-device picker in provisioning mode, which reuses this same filter.
// ======================================================================

void setup() {
  Serial.begin(115200);
  delay(400);
  Serial.println("\n\n=== hughes_bridge  SCAN MODE ===");
  Serial.println("Candidates = advertised name PMD*/PWS*/PMS*, or service 0xffe0.");
  Serial.println("Yours is the STRONGEST rssi; power-cycle it to confirm which MAC drops.");
  Serial.println("Note the addr type: 'public' is safe to pin by MAC; 'random' -> pin by name.\n");
  NimBLEDevice::init("");
}

void loop() {
  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->setActiveScan(true);
  NimBLEScanResults res = scan->getResults(5000, false);

  int hits = 0;
  Serial.printf("--- scan pass (%d devices seen) ---\n", res.getCount());
  for (int i = 0; i < res.getCount(); i++) {
    const NimBLEAdvertisedDevice* d = res.getDevice(i);
    String nm = d->haveName() ? String(d->getName().c_str()) : String();
    bool svc = d->isAdvertisingService(SVC);
    if (!nameMatches(nm) && !svc) continue;
    hits++;
    Serial.printf("  %-17s  addr=%-9s  rssi=%4d  name=\"%s\"%s\n",
                  d->getAddress().toString().c_str(),
                  addrTypeStr(d->getAddress().getType()),
                  d->getRSSI(),
                  nm.c_str(),
                  svc ? "  [ffe0]" : "");
  }
  if (!hits) Serial.println("  (no Watchdog candidates this pass)");
  Serial.println();
  scan->clearResults();
}

// ======================================================================
#elif defined(DISPLAY_TEST)
// DISPLAY TEST -- phase 2, milestone 2 bring-up. Prove the ILI9341V panel, the
// FT6336 touch, and the backlight all work, PSRAM OFF. Standalone: the bridge
// firmware is untouched. Config is in platformio.ini [display].
//   Colour bars  -> RGB order + inversion sanity (IPS panels sometimes need
//                   -DTFT_INVERSION_ON; add it to [display] if red/blue or
//                   black/white look wrong).
//   Touch strip  -> live coordinates; if the axes feel swapped/mirrored, adjust
//                   the ts.setRotation() below.
// ======================================================================

#include <TFT_eSPI.h>
#include <FT6336.h>

static TFT_eSPI tft;
static FT6336   ts(16, 15, 17, 18, 240, 320);   // SDA,SCL,INT,RST + native portrait W,H

void setup() {
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);            // S3 USB-CDC: never block a print when no host is attached.
                                       // Without this the loop stalls in Serial.print and touch feels dead.
  delay(300);
  Serial.println("\n=== hughes_bridge  DISPLAY TEST ===");

  tft.init();                          // also drives TFT_BL on (TFT_BACKLIGHT_ON=HIGH)
  tft.setRotation(1);                  // landscape 320x240
  tft.fillScreen(TFT_BLACK);

  // 1) colour bars across the top -- verify colours are right-side-up.
  const uint16_t bars[] = { TFT_RED, TFT_GREEN, TFT_BLUE, TFT_YELLOW,
                            TFT_CYAN, TFT_MAGENTA, TFT_WHITE, TFT_DARKGREY };
  int bw = tft.width() / 8;
  for (int i = 0; i < 8; i++) tft.fillRect(i * bw, 0, bw, 60, bars[i]);

  // 2) text in a couple of fonts.
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("hughes_bridge display test", 6, 74, 2);
  tft.drawString("ILI9341V + FT6336 touch", 6, 98, 2);
  tft.setTextColor(TFT_GREENYELLOW, TFT_BLACK);
  tft.drawString("Touch anywhere ->", 6, 132, 4);

  // 3) touch.
  ts.begin();                          // Wire.begin + FT6336 reset/ID handshake
  ts.setRotation(ROTATION_RIGHT);      // match tft rotation 1 (vendor mapping)
  Serial.println("setup done -- touch to draw dots + read coords.");
}

void loop() {
  ts.read();
  if (ts.isTouched) {
    int x = ts.points[0].x, y = ts.points[0].y;
    tft.fillCircle(x, y, 3, TFT_ORANGE);
    tft.fillRect(0, tft.height() - 24, tft.width(), 24, TFT_NAVY);
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(TFT_WHITE, TFT_NAVY);
    char b[40]; snprintf(b, sizeof(b), "touch:  x=%d  y=%d", x, y);
    tft.drawString(b, 6, tft.height() - 20, 2);
    Serial.printf("touch %d,%d\n", x, y);
  }
  delay(15);
}

// ======================================================================
#elif defined(SCREEN_SCRUB)
// SCREEN SCRUB -- LCD image-retention remover. The ILI9341V is an IPS *LCD*, so a ghosted
// store-demo image is image RETENTION (a DC bias built up in the liquid crystal by a long
// static image), NOT the permanent burn-in an OLED gets. High-contrast full-field cycling +
// polarity inversion drives the opposite bias into every pixel and lifts retention far
// faster than leaving the panel powered off. Flash `-e scrub`, let it run 10-30 min (longer
// for a strong ghost), then reflash `-e bridge`. Not part of any product build.
// ======================================================================
#include <TFT_eSPI.h>
#include <FT6336.h>
static TFT_eSPI tft;
static FT6336   ts(16, 15, 17, 18, 240, 320);   // SDA,SCL,INT,RST -- tap to pause/resume
static bool g_wasTouch = false;
#define C565(r,g,b) ((uint16_t)((((r)&0xF8)<<8)|(((g)&0xFC)<<3)|((b)>>3)))
static const uint16_t REACTOR_BG = C565(0x03,0x07,0x10);   // same dark grey as the reactor page background

void setup() {
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);            // S3 USB-CDC: never block a print with no host attached
  delay(300);
  Serial.println("\n=== hughes_bridge  SCREEN SCRUB (LCD de-ghost) ===");
  Serial.println("Cycling to lift image retention. TAP to pause on dark grey (inspect bleed-through), tap to resume.");
  tft.init();                          // also drives the backlight on (TFT_BACKLIGHT_ON=HIGH)
  tft.setRotation(0);
  ts.begin(); ts.setRotation(ROTATION_NORMAL);
}

// rising-edge tap detector (isTouched high after being low)
static bool tapEdge() {
  ts.read();
  bool now = ts.isTouched, edge = now && !g_wasTouch;
  g_wasTouch = now;
  return edge;
}
// responsive wait during a fill; returns true if the screen was tapped (=> pause requested)
static bool waitTap(int ms) {
  uint32_t t = millis();
  while (millis() - t < (uint32_t)ms) { if (tapEdge()) return true; delay(4); }
  return false;
}

void loop() {
  static uint32_t pass = 0;
  static bool paused = false;

  // PAUSED: hold on dark grey (backlight stays lit) so residual ghosting is visible; tap resumes.
  if (paused) {
    tft.invertDisplay(true);                             // this IPS panel needs inversion ON for TRUE colours (matches the bridge)
    tft.fillScreen(REACTOR_BG);                          // reactor-page dark grey -> inspect bleed-through as it'll look in the UI
    while (true) { if (tapEdge()) break; delay(10); }    // wait for the next tap
    paused = false;
    return;
  }

  // one scrub pass; any tap during a step pauses and bails to the grey screen next loop.
  #define STEP(fill, ms) do { fill; if (waitTap(ms)) { paused = true; return; } } while (0)
  for (int k = 0; k < 12; k++) {                         // white<->black alternation + polarity flip
    tft.invertDisplay(k & 1);
    STEP(tft.fillScreen(TFT_WHITE), 180);
    STEP(tft.fillScreen(TFT_BLACK), 180);
  }
  tft.invertDisplay(true);                               // true colours for the subpixel exercise (panel needs inversion ON)
  STEP(tft.fillScreen(TFT_RED),   350);                  // subpixel exercise
  STEP(tft.fillScreen(TFT_GREEN), 350);
  STEP(tft.fillScreen(TFT_BLUE),  350);
  STEP(tft.fillScreen(TFT_WHITE), 350);
  for (int x = 0; x < tft.width(); x += 8) {             // sweeping bright bar
    tft.fillScreen(TFT_BLACK);
    tft.fillRect(x, 0, 8, tft.height(), TFT_WHITE);
    if (waitTap(10)) { paused = true; return; }
  }
  #undef STEP
  Serial.printf("scrub pass %lu -- tap to pause & inspect\n", (unsigned long)++pass);
}

// ======================================================================
#elif defined(AUDIO_TEST)
// AUDIO BRING-UP (phase 2, audible alarm). Standalone ES8311 + I2S "klaxon" so we can
// prove the speaker path AND that the FT6336 touch survives on the SHARED I2C bus
// (SDA16/SCL15) -- the one real hazard -- BEFORE the alarm goes into the bridge. Needs a
// speaker on the board's SPK JST to hear anything; without one it just exercises the path.
//
// Pins (vendor Example_16_music, cross-checked): I2S MCK4/BCK5/WS7/DOUT8; ES8311 @0x18 on
// the shared Wire bus; amp-enable GPIO1. CRITICAL clock fact (from reading the sources):
// ESP_I2S emits MCLK at 256*fs (I2S_MCLK_MULTIPLE_256), so the codec is configured for
// 256*fs here -- NOT the vendor es8311_codec_init()'s 384*fs (which matched their Audio lib).
#include <Wire.h>
#include <ESP_I2S.h>
#include "FT6336.h"
#include "es8311.h"

#define I2S_MCK   4
#define I2S_BCK   5
#define I2S_WS    7
#define I2S_DOUT  8
#define PIN_SDA   16
#define PIN_SCL   15
#define AMP_EN     1
#define AMP_ON    LOW          // vendor demo drives GPIO1 LOW during playback -> LOW enables. Flip if silent.
#define SAMPLE_RATE 16000

static I2SClass      i2s;
static FT6336        ts(PIN_SDA, PIN_SCL, 17, 18, 240, 320);
static es8311_handle_t g_es = nullptr;

static bool codec_setup() {
  g_es = es8311_create(I2C_NUM_0, ES8311_ADDRESS_0);   // port arg unused (driver patched to Wire); addr 0x18
  if (!g_es) return false;
  es8311_clock_config_t clk = {};
  clk.mclk_inverted = false; clk.sclk_inverted = false; clk.mclk_from_mclk_pin = true;
  clk.mclk_frequency = SAMPLE_RATE * 256; clk.sample_frequency = SAMPLE_RATE;   // 4.096MHz -> in coeff table
  if (es8311_init(g_es, &clk, ES8311_RESOLUTION_16, ES8311_RESOLUTION_16) != ESP_OK) return false;
  if (es8311_sample_frequency_config(g_es, SAMPLE_RATE * 256, SAMPLE_RATE) != ESP_OK) return false;
  es8311_voice_volume_set(g_es, 80, nullptr);
  return true;
}

// Push a sine burst (mono 16-bit) of `hz` for `ms` at amplitude `amp` (0..1) out I2S.
static void tone_i2s(float hz, uint32_t ms, float amp) {
  const int N = 256; int16_t buf[N];
  uint32_t total = (uint32_t)((uint64_t)SAMPLE_RATE * ms / 1000);
  float ph = 0.0f, step = 2.0f * PI * hz / SAMPLE_RATE;
  for (uint32_t done = 0; done < total; ) {
    int n = (total - done) < (uint32_t)N ? (int)(total - done) : N;
    for (int i = 0; i < n; i++) { buf[i] = (int16_t)(amp * 28000.0f * sinf(ph)); ph += step; if (ph > 2*PI) ph -= 2*PI; }
    i2s.write((uint8_t*)buf, n * sizeof(int16_t));
    done += n;
  }
}

// Rising two-tone "reactor alarm" -- the sound the bridge will make near a 50A leg trip.
static void klaxon() {
  digitalWrite(AMP_EN, AMP_ON);                          // enable the amp only while sounding
  for (int i = 0; i < 3; i++) { tone_i2s(880, 130, 0.8f); tone_i2s(1320, 130, 0.8f); }
  digitalWrite(AMP_EN, !AMP_ON);                         // and mute it between alarms
}

void setup() {
  Serial.begin(115200); Serial.setTxTimeoutMs(0); delay(400);
  Serial.println("\n=== hughes_bridge  AUDIO TEST ===");

  Wire.begin(PIN_SDA, PIN_SCL); Wire.setClock(400000);   // shared bus: ES8311 + FT6336 both live here
  ts.begin();                                            // touch on the SAME Wire -> coexistence check
  pinMode(AMP_EN, OUTPUT); digitalWrite(AMP_EN, !AMP_ON);// amp off until we sound (avoid boot pop)

  Serial.println(codec_setup() ? "ES8311 init OK" : "ES8311 init FAILED (check I2C 0x18 / shared bus)");

  i2s.setPins(I2S_BCK, I2S_WS, I2S_DOUT, -1, I2S_MCK);   // bclk, ws, dout, din(none), mclk
  if (!i2s.begin(I2S_MODE_STD, SAMPLE_RATE, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO))
    Serial.println("I2S begin FAILED");
  else
    Serial.println("I2S OK -- klaxon every 4s; tap the screen to confirm touch still reads");
}

void loop() {
  static uint32_t lastBeep = 0;
  ts.read();
  if (ts.isTouched) Serial.printf("touch %d,%d\n", ts.points[0].x, ts.points[0].y);
  if (millis() - lastBeep > 4000) { lastBeep = millis(); Serial.println("KLAXON"); klaxon(); }
  delay(15);
}

// ======================================================================
#elif defined(WATCHDOG_INFO)
// GATT RECON -- connect to the real Watchdog and dump its COMPLETE GATT table
// (services / characteristics / properties / descriptors) to serial. Tells us what
// the emulator must replicate, and may reveal the writable characteristic the phone
// app's "reset energy" targets. Read-only; safe.
// ======================================================================
#if __has_include("config.h")
  #include "config.h"                 // target YOUR unit's MAC (HUGHES_MAC), not a neighbour's
#endif

static const char* propStr(NimBLERemoteCharacteristic* c, char* buf) {
  int n = 0;
  if (c->canRead())            buf[n++] = 'R';
  if (c->canWrite())           buf[n++] = 'W';
  if (c->canWriteNoResponse()) buf[n++] = 'w';
  if (c->canNotify())          buf[n++] = 'N';
  if (c->canIndicate())        buf[n++] = 'I';
  buf[n] = 0; return buf;
}

static void dumpGatt(NimBLEClient* c) {
  Serial.println("\n===== GATT TABLE =====");
  auto svcs = c->getServices(true);
  for (auto s : svcs) {
    Serial.printf("SERVICE %s\n", s->getUUID().toString().c_str());
    auto chrs = s->getCharacteristics(true);
    for (auto ch : chrs) {
      char pb[8];
      Serial.printf("  CHAR %s  [%s]\n", ch->getUUID().toString().c_str(), propStr(ch, pb));
      auto dscs = ch->getDescriptors(true);
      for (auto d : dscs) Serial.printf("      DESC %s\n", d->getUUID().toString().c_str());
    }
  }
  Serial.println("===== END =====\n");
}

void setup() {
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);
  delay(400);
  Serial.println("\n=== hughes_bridge  GATT RECON ===");
  NimBLEDevice::init("");
  NimBLEDevice::setMTU(517);
}

void loop() {
  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->setActiveScan(true);
  NimBLEScanResults res = scan->getResults(6000, false);
  String wantMac;
#ifdef HUGHES_MAC
  wantMac = String(HUGHES_MAC); wantMac.toLowerCase();
#endif
  const NimBLEAdvertisedDevice* tgt = nullptr;
  for (int i = 0; i < res.getCount(); i++) {
    const NimBLEAdvertisedDevice* d = res.getDevice(i);
    String nm = d->haveName() ? String(d->getName().c_str()) : String();
    if (!nameMatches(nm) && !d->isAdvertisingService(SVC)) continue;
    String a = String(d->getAddress().toString().c_str()); a.toLowerCase();
    Serial.printf("  candidate %s  name=\"%s\"  rssi=%d\n", a.c_str(), nm.c_str(), d->getRSSI());
    if (wantMac.length()) { if (a == wantMac) tgt = d; }     // pin to YOUR unit
    else if (!tgt) tgt = d;
  }
  if (!tgt) { Serial.printf("target %s not found; retrying...\n", wantMac.length()?wantMac.c_str():"(any)"); scan->clearResults(); return; }

  Serial.printf("found %s  name=\"%s\"  -- connecting...\n",
                tgt->getAddress().toString().c_str(), tgt->haveName() ? tgt->getName().c_str() : "?");
  NimBLEClient* cli = NimBLEDevice::createClient();
  if (!cli->connect(tgt)) { Serial.println("connect failed; retrying"); NimBLEDevice::deleteClient(cli); scan->clearResults(); return; }
  dumpGatt(cli);
  cli->disconnect();
  NimBLEDevice::deleteClient(cli);
  scan->clearResults();
  Serial.println("(re-dumping in ~12s; reflash -e bridge to restore the panel)");
  delay(12000);                                    // loop and re-dump, so a monitor can attach anytime
}

// ======================================================================
#elif defined(EMULATE)
// WATCHDOG EMULATOR -- impersonate the user's Watchdog (name + GATT), stream canned
// telemetry on ffe2, and LOG every byte the phone app writes to any characteristic.
// Goal: capture the app's "reset energy" command. Power the REAL unit OFF so the app
// connects to this fake. The captured write (char UUID + bytes) is the reset command.
// ======================================================================

// Canned 40-byte telemetry packet (valid header + plausible values) so the app sees a
// "live" unit and exposes its controls. Exact values don't matter for capture.
static uint8_t g_pkt[40] = {
  0x01,0x03,0x20,             // header
  0x00,0x12,0x4F,0x80,        // volts 120.0  (*10000)
  0x00,0x01,0x86,0xA0,        // amps  10.0
  0x00,0xB7,0x1B,0x00,        // watts 1200
  0x00,0x02,0xDB,0x40,        // kWh   18.72
  0x00,                       // err
  0,0,0,0,0,0,0,0,0,0,0,      // bytes 20-30 (unknown)
  0x00,0x00,0x17,0x70,        // hz 60.00 (*100)  bytes 31-34
  0,0,                        // 35-36
  0x00,0x00,0x00              // 37-39 line ID = L1
};

class WriteLog : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo& info) override {
    std::string v = c->getValue();
    Serial.printf(">>> WRITE  char %s  (%u bytes): ", c->getUUID().toString().c_str(), (unsigned)v.size());
    for (size_t i = 0; i < v.size(); i++) Serial.printf("%02x ", (uint8_t)v[i]);
    Serial.println("  <<< (this is a command from the app)");
  }
  void onSubscribe(NimBLECharacteristic* c, NimBLEConnInfo& info, uint16_t sub) override {
    Serial.printf("    app subscribed to %s (val %u)\n", c->getUUID().toString().c_str(), sub);
  }
};
static WriteLog g_wl;

class SrvCB : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* s, NimBLEConnInfo& info) override {
    Serial.println(">>> APP CONNECTED -- open the dashboard and tap Reset Energy");
  }
  void onDisconnect(NimBLEServer* s, NimBLEConnInfo& info, int reason) override {
    Serial.printf(">>> app disconnected (reason %d) -- re-advertising\n", reason);
    NimBLEDevice::startAdvertising();
  }
};
static SrvCB g_scb;
static NimBLECharacteristic* g_ffe2 = nullptr;

static NimBLECharacteristic* mkChar(NimBLEService* s, const char* uuid, uint32_t props, bool logWrites) {
  NimBLECharacteristic* c = s->createCharacteristic(uuid, props);
  if (logWrites) c->setCallbacks(&g_wl);
  return c;
}

void setup() {
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);
  delay(400);
  Serial.println("\n=== hughes_bridge  WATCHDOG EMULATOR ===");
  Serial.println("Power the REAL Watchdog OFF, then open the Hughes app -- it should connect here.");
  Serial.println("Then tap 'Reset Energy' and watch for a >>> WRITE line below.\n");

  NimBLEDevice::init("PMD      0C010FE309");
  NimBLEDevice::setMTU(517);
  NimBLEServer* srv = NimBLEDevice::createServer();
  srv->setCallbacks(&g_scb);

  // Device Info service (0x180a) with plausible values, in case the app checks it
  NimBLEService* dis = srv->createService("180a");
  dis->createCharacteristic("2a24", NIMBLE_PROPERTY::READ)->setValue("PMD");
  dis->createCharacteristic("2a25", NIMBLE_PROPERTY::READ)->setValue("0C010FE309");
  dis->createCharacteristic("2a29", NIMBLE_PROPERTY::READ)->setValue("Hughes Autoformers");
  dis->start();

  // The Hughes service (0xffe0) -- replicate the real GATT and log every write
  NimBLEService* svc = srv->createService("ffe0");
  g_ffe2   = mkChar(svc, "ffe2", NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY, true);
             mkChar(svc, "fff5", NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR, true);
             mkChar(svc, "1003", NIMBLE_PROPERTY::WRITE, true);
             mkChar(svc, "1004", NIMBLE_PROPERTY::READ, false);
             mkChar(svc, "1005", NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE, true);
  svc->start();

  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  adv->addServiceUUID("ffe0");
  adv->setName("PMD      0C010FE309");
  adv->enableScanResponse(true);
  NimBLEDevice::startAdvertising();
  Serial.println("advertising as the Watchdog -- waiting for the app to connect...");
}

void loop() {
  static uint32_t t = 0;
  if (millis() - t > 1000) {                         // stream canned telemetry ~1Hz
    t = millis();
    if (g_ffe2) { g_ffe2->setValue(g_pkt, 40); g_ffe2->notify(); }
  }
  delay(20);
}

// ======================================================================
#else
// BRIDGE MODE (+ first-run PROVISIONING)
// ======================================================================

#include <WiFi.h>
#include <WebServer.h>
#include "esp_sntp.h"                       // sntp_get_sync_status() -> only claim CLK_NTP on a real SNTP answer
#include <DNSServer.h>
#include <Preferences.h>
#include <Adafruit_NeoPixel.h>
#include <TFT_eSPI.h>
#include <FT6336.h>
#include <SD_MMC.h>
#include <math.h>
#include <ESP_I2S.h>                    // audible trip alarm: I2S out to the ES8311 codec
#include "dmesg.h"                      // persistent crash-log ring in RTC memory (survives panic/WDT/brownout)
#include <WiFiClientSecure.h>           // outbound TLS -- CA-validated HTTPS for OTA (manifest/bin) + ntfy
#include <HTTPClient.h>                 // outbound HTTPS client (ntfy POST; OTA download later)
#include <esp_wifi.h>                   // esp_wifi_get_config -- reconcile driver-stored STA creds into our config
#include <time.h>                       // SNTP wall-clock: TLS cert validity needs a real time
#include "certs.h"                      // pinned root CA (ISRG Root X1 / Let's Encrypt) for firmware.flensor.com + ntfy.sh
#include "ota.h"                        // OTA engine (A/B slot write + sha256 gate) + crash-boot guard

// Firmware version -- shown on the boot splash + INFO page + /status ("fw" key) + serial `version`.
// Bump per release; the OTA manifest compares this string (see OTA_DMESG_NTFY_PLAN.md). Start at 1.0.0
// now that the firmware is a shipped product (first units going out).
#ifndef OTA_SELFTEST_PANIC
#define FW_VERSION "2.1.0"           // <- the real version FIRST so publish.sh's `head -1` grep reads it
#else
#define FW_VERSION "9.9.9-badtest"   // bad-image build: panics on boot to exercise otaBootGuard's 3-strike revert
#endif
#include "es8311.h"                     // vendored in lib/ES8311 (I2C primitives on Wire; see -e audiotest)
#include <esp_random.h>                 // hardware RNG -> the API key for destructive HTTP endpoints
#include "esp_adc/adc_oneshot.h"        // battery-voltage ADC on GPIO9 (ADC1_CH8); see docs/board/power-battery-charging.md
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include <esp_sleep.h>                  // deep-sleep power-off + tap-to-wake (FT6336 INT GPIO17)
#include <esp_mac.h>                    // efuse MAC -> the -<last4> SSID suffix (valid before WiFi is up)
#include <qrcode.h>                     // ricmoo/QRCode -> scan-to-join Wi-Fi QR on the AP page
#if __has_include("config.h") && !defined(FACTORY_IMAGE)
  #include "config.h"                 // OPTIONAL dev seed: WIFI_SSID/PASS, HUGHES_MAC/NAME.
                                       // FACTORY_IMAGE (-e factory) skips it -> no creds compiled in.
#endif
#ifndef HTTP_PORT
  #define HTTP_PORT 80
#endif
#ifndef LED_PIN
  #define LED_PIN 42                  // onboard WS2812 (lcdwiki 2.8" S3). Set -1 in config.h to disable.
#endif
#define STALE_MS 15000                // no notification for this long -> force reconnect
#define OUTAGE_MS       20000         // had a live link, then no data this long -> outage banner
#define OUTAGE_QUIET_MS 600000        // auto-quiet the outage banner after 10 min (don't nag all trip)
#define AP_SSID  "Hughes-Setup"       // open SoftAP for the first-run captive portal (SSID gets a -<last4 MAC> suffix)
static char g_setupSsid[24] = AP_SSID;              // filled with "Hughes-Setup-XXXX" in startProvisioning
#define IDLE_MS  30000                // no touch for this long -> sleep (backlight off) + back to reactor
#define NOWD_SLEEP_MS 60000           // NO Watchdog selected + idle this long -> deep-sleep power-off (save battery)
#define SETTINGS_SLEEP_MS 300000      // on any settings page: longer grace (5 min) so config/reading isn't cut short

// Status LED (blue=connecting, green=connected/fresh, red=fail/stale, magenta=setup
// portal). Written only from setup()/bleTask()/provisioning -- single writer paths.
static Adafruit_NeoPixel g_led(1, LED_PIN < 0 ? 0 : LED_PIN, NEO_GRB + NEO_KHZ800);
static void led(uint8_t r, uint8_t g, uint8_t b) {
  if (LED_PIN < 0) return;
  g_led.setPixelColor(0, g_led.Color(r, g, b)); g_led.show();
}

// ---------------------------------------------------------------- display palette
// Milestone 3a: a STATIC reactor rendered from the live leg data -- direct-draw, no
// sprite/animation yet (that's 3b). TFT is on HSPI; BLE owns the radio on core 0, so
// ALL drawing happens from loop() on core 1, never from bleTask. Palette + helpers
// ported from aitrip_panel's Arduino_GFX UI into TFT_eSPI (see LEDGER.md reactor design).
#define C565(r,g,b) ((uint16_t)((((r)&0xF8)<<8)|(((g)&0xFC)<<3)|((b)>>3)))
static const uint16_t
  C_BG   = C565(0x03,0x07,0x10), C_CYAN = C565(0x39,0xe0,0xff),
  C_AMBER= C565(0xff,0xb4,0x54), C_GREEN= C565(0x38,0xff,0x9d),
  C_RED  = C565(0xff,0x50,0x69), C_DIM  = C565(0x4d,0x72,0x88),
  C_TEXT = C565(0xcf,0xe9,0xf5), C_LINE = C565(0x1b,0x3a,0x4d);
static uint16_t scale565(uint16_t c, uint8_t f) {
  uint16_t r=((c>>11)&0x1F)*f/255, g=((c>>5)&0x3F)*f/255, b=(c&0x1F)*f/255;
  return (r<<11)|(g<<5)|b;
}
static void polar(int cx, int cy, float rad, float aDeg, int& x, int& y) {
  float a = aDeg*DEG_TO_RAD; x = cx+(int)lroundf(rad*cosf(a)); y = cy-(int)lroundf(rad*sinf(a));
}
static TFT_eSPI tft;
static FT6336   ts(16, 15, 17, 18, 240, 320);       // capacitive touch (lib/FT6336; SDA16/SCL15/INT17/RST18)

// screen state machine (touch nav)
enum Screen { SCR_REACTOR, SCR_GRAPHS, SCR_SETTINGS };
static Screen g_scr        = SCR_REACTOR;
static bool   g_scrDirty   = true;                  // current screen needs a full repaint
static bool   g_wasTouched = false;                 // for tap (rising-edge) detection
static uint32_t g_lastTouchMs = 0;                  // last touch; drives the idle sleep timeout
static uint32_t g_lastWebMs   = 0;                  // last web request; defers the no-Watchdog auto power-off
static bool     g_asleep      = false;              // display slept (backlight off) after IDLE_MS
static bool     g_noSleep     = false;              // serial 'nosleep': skip the no-Watchdog auto deep-sleep until reboot (bench convenience)
static bool     g_capHold     = false;              // serial 'show <page>': pin that page for screenshots -- suppress the outage-yank + idle-sleep (dev/capture only; 'show r' releases)

// ---- in-RAM log ring: last LOG_LINES breadcrumbs, queryable over Wi-Fi at /log (pairs with the serial
// console -- status = "what is now", /log = "what happened"). Written from BOTH cores (bleTask core0 +
// loop core1) so head/count are guarded by a portMUX spinlock; each line is also echoed to Serial. ----
#define LOG_LINES 48
#define LOG_LEN   100
static char g_log[LOG_LINES][LOG_LEN];
static volatile int g_logHead = 0, g_logCount = 0;
static portMUX_TYPE g_logMux = portMUX_INITIALIZER_UNLOCKED;
static void logf(const char* fmt, ...) {
  char line[LOG_LEN]; uint32_t s = millis() / 1000;
  int n = snprintf(line, sizeof(line), "%lu:%02lu:%02lu ", s/3600, (s/60)%60, s%60);
  if (n < 0 || n >= (int)sizeof(line)) n = 0;
  va_list ap; va_start(ap, fmt); vsnprintf(line + n, sizeof(line) - n, fmt, ap); va_end(ap);
  portENTER_CRITICAL(&g_logMux);
  strncpy(g_log[g_logHead], line, LOG_LEN-1); g_log[g_logHead][LOG_LEN-1] = 0;
  g_logHead = (g_logHead + 1) % LOG_LINES; if (g_logCount < LOG_LINES) g_logCount++;
  dmesgAppend(line);                                 // also tee into the RTC crash-log ring (serialized by g_logMux)
  portEXIT_CRITICAL(&g_logMux);
  Serial.println(line);                              // echo (drops harmlessly when headless, setTxTimeoutMs(0))
}

// ---- Event log (v2.1 0.1): transition types + evtLog() fwd decl ----------------------------------
// Foundation for storm report / continuity cert / gap accounting / audit trail / link report /
// support bundle. Types are stable on-disk (do NOT renumber -- the SD ring stores the enum value);
// append new kinds at the end. The full SD-ring module lives after the power-history code; declared
// here because handlers/callbacks earlier in the file emit events.
enum EvtType : uint8_t {
  EV_BOOT = 1, EV_SHORE_LOST, EV_SHORE_BACK, EV_ON_BATT, EV_ON_LINE,
  EV_BLE_UP, EV_BLE_DOWN, EV_NET_MODE, EV_NTP_SYNC, EV_CLOCK_SET,
  EV_RATE_CHG, EV_RESET_STAY, EV_RESET_ODO
};
#define EVF_CLK_UNSURE 0x01                            // clock not confident when logged (epoch < 2023 -> use mono for order)
static void evtLog(uint8_t type, int16_t arg);        // defined with the SD-ring module below
static void evtSave();                                // flush the ring to SD (also called before an AP-mode reboot)
static int  stayOpenIdx();                            // index of the open stay, or -1 (defined with the Stay-Card module; fwd for the R6 reset guards above it)
static void swapBillingReset();                       // on a Watchdog change: close a real-metered open stay + zero the running bill/baselines (fwd for handleSave/reprovision above the def)

static bool     g_rssiBars     = true;              // reactor header: true=signal bars, false=dBm number (tap swaps)
struct Rect { int x, y, w, h; };
static bool hit(const Rect& r, int x, int y) { return x>=r.x && x<r.x+r.w && y>=r.y && y<r.y+r.h; }

// ---------------------------------------------------------------- config (NVS)
// WiFi creds + the target Watchdog live in flash (Preferences), set at runtime by
// the captive portal. Compile-time config.h, if present, only SEEDS an empty NVS on
// first boot -- so a board carrying the old build keeps working without re-setup.
// Time-of-day rate tier: [startH, endH) local hours + cents/kWh. endH < startH = a midnight-wrapping
// window (e.g. 22->6). A valid set tiles all 24 hours exactly once (no gaps, no overlaps).
#define MAX_TIERS 8
struct RateTier { uint8_t startH, endH; uint16_t cents; };

struct Config {
  String wifiSsid, wifiPass, hughesMac, hughesName;
  String nick;                       // owner-set nickname for this bay/Watchdog (web-settable, Tier 3)
  String apiKey;                     // gate for destructive HTTP writes; auto-generated, shown on INFO
  float  kwhBase = 0.0f;              // stay-kWh baseline; SETTINGS "reset" re-baselines it
  int    rateC   = 28;                // FLAT electricity rate, cents/kWh (also the fallback when tiers are off/time unknown)
  RateTier tiers[MAX_TIERS];         // time-of-day rate schedule; tierCount 0 = flat mode (rateC applies all day)
  uint8_t  tierCount = 0;
  int    bright  = 80;                // backlight %, adjustable in SETTINGS
  int    svcAmps = 50;                // shore service rating per leg (50 or 30 A) -- scales reactor/alarm/header
  int    tzIndex = 0;                 // US timezone: index into TZ_TABLE (0 = Not set -> UTC / local-time unknown)
  int    netMode = 0;                 // 0=HOME (join Wi-Fi), 1=AP (host own network), 2=OFF (no radio)
  bool   alarmOn = false;             // audible trip alarm (SETTINGS->AUDIO). Off until a speaker is fitted.
  bool   ntfyOn   = false;            // ntfy push alerts master switch (default OFF; owner enables + subscribes via QR/link)
  String ntfyTopic;                   // ntfy topic (auto-minted "hughes-<8hex>" on first enable) -- the only secret
  String ntfyServer = "ntfy.sh";      // ntfy server host (default ntfy.sh; overridable for a self-hosted server)
  int    ntfyMask = 0x7F;             // per-alert enable bits (NtfyAlert enum) -- default 7 core alerts on; heartbeat (bit 7) opt-in
  int    hbHour   = -1;               // daily heartbeat local hour 0-23, or -1 = off (v2.1 1.4; needs clock + ntfy on)
  String otaUrl = "https://firmware.flensor.com/hughes.json";   // OTA manifest URL (per-unit override -> channel)
  // Provisioned = the unit can run the product: host-AP mode, OR we have Wi-Fi creds. The Watchdog
  // is OPTIONAL at boot -- a unit with Wi-Fi but no Watchdog boots into the bridge showing NO WATCHDOG
  // and you pick one via DEVICES (matchTarget idles until then). Requiring a Watchdog here caused a
  // reboot loop: enter Wi-Fi, skip the scan -> not provisioned -> back to the portal, forever.
  // Brand-new units (netMode 0, no creds) still -> the setup portal.
  bool provisioned() const { return netMode == 1 || wifiSsid.length(); }
};
static Config      g_cfg;
static Preferences g_prefs;
static bool        g_forceProv = false;   // set by reprovision/factory-reset (NVS "forceprov"): on the next boot go
                                          // straight to the setup portal and do NOT re-adopt driver-cached STA creds

// ---- time-of-day rate tiers: pure helpers (no clock/tz dependency) ----
static bool tierContains(const RateTier& t, int h) {   // endH<startH => midnight-wrapping window
  return (t.startH < t.endH) ? (h >= t.startH && h < t.endH) : (h >= t.startH || h < t.endH);
}
// True iff tiers[0..n) tile all 24 hours exactly once (no gaps/overlaps). *badHour = first offending hour.
static bool tiersCover24(const RateTier* t, int n, int* badHour) {
  if (n < 1 || n > MAX_TIERS) { if (badHour) *badHour = -1; return false; }
  for (int h = 0; h < 24; h++) {
    int c = 0; for (int i = 0; i < n; i++) if (tierContains(t[i], h)) c++;
    if (c != 1) { if (badHour) *badHour = h; return false; }
  }
  return true;
}
// Parse "start-end-cents,..." into out[MAX_TIERS]; returns count, or -1 on malformed token / bad range / too many.
static int tiersParse(const String& in, RateTier* out) {
  int n = 0, i = 0, L = in.length();
  while (i < L && n < MAX_TIERS) {
    int comma = in.indexOf(',', i); if (comma < 0) comma = L;
    int d1 = in.indexOf('-', i), d2 = (d1 < 0) ? -1 : in.indexOf('-', d1 + 1);
    if (d1 < 0 || d2 < 0 || d2 >= comma) return -1;
    int s = in.substring(i, d1).toInt(), e = in.substring(d1 + 1, d2).toInt(), c = in.substring(d2 + 1, comma).toInt();
    if (s < 0 || s > 23 || e < 1 || e > 24 || s == e || c < 0 || c > 999) return -1;   // s==e = zero-length
    out[n].startH = (uint8_t)s; out[n].endH = (uint8_t)(e % 24); out[n].cents = (uint16_t)c; n++;
    i = comma + 1;
  }
  if (i < L) return -1;                                  // slots exhausted but text remains -> too many tiers
  return n;
}

// --- multi-unit foundation: key per-Watchdog state by the TARGET mac ---
// So a 2nd unit (demo roster) keeps its OWN power history + STAY baseline. Harmless with
// one unit (everything just lives under the single mac). Keyed by g_cfg.hughesMac (the
// configured target, known at boot before BLE connects), not the live peer mac.
static void macKey(char* out, size_t n) {          // "f0:f8:..:0f" -> "f0f8f2ec010f" (<=12 chars, NVS-key safe)
  size_t j = 0;
  for (size_t i = 0; i < g_cfg.hughesMac.length() && j + 1 < n; i++) {
    char c = g_cfg.hughesMac[i];
    if (c == ':') continue;
    out[j++] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;   // lower-case
  }
  out[j] = 0;
}
// Per-MAC STAY baseline. A unit we've never tracked carries a -1 sentinel (written by
// adoptWatchdog) meaning "start fresh" -> capture its baseline at the first live reading,
// instead of inheriting the previous unit's baseline (which wrongly clamped a new unit's
// STAY to 0 until its odometer passed the stale value).
static bool g_baseNeedsInit = false;                // sentinel seen -> capture baseline at first packet
static void baselineLoad() {
  char mac[16]; macKey(mac, sizeof(mac));
  if (!mac[0]) return;
  Preferences p; p.begin("hkwh", true);
  bool  has = p.isKey(mac);
  float v   = p.getFloat(mac, g_cfg.kwhBase);       // default = migrated global (original single-unit path)
  p.end();
  if (has && v < 0.0f) { g_baseNeedsInit = true; g_cfg.kwhBase = 0.0f; return; }  // fresh unit -> capture at first reading
  g_cfg.kwhBase = v;
  g_baseNeedsInit = false;
}
static double g_stayCostC = 0.0;      // accrued this-stay cost in cents (tier-aware; the accurate running bill)
// R4: PER-LEG last-billed odometer (was a single combined g_lastCk). L1/L2 arrive as separate BLE packets, so a
// combined baseline is deflated whenever one leg hasn't decoded yet -- after a cold boot the persisted (both-leg)
// baseline would re-seat DOWN on an L1-only tick, then the next both-leg tick booked L2's ENTIRE lifetime odometer
// into today's ledger + g_stayCostC. Comparing each leg to its OWN last value makes a missing leg contribute 0
// (and correctly handles 30A single-leg service, where L2 never decodes). -1 = that leg not yet baselined.
static float  g_lastCkL[2] = { -1.0f, -1.0f };
static void stayCostPersist() {       // saved periodically + on stay/odometer reset (own handle; own cadence)
  g_prefs.begin("hughes", false); g_prefs.putDouble("staycost", g_stayCostC);
  g_prefs.putFloat("lastck0", g_lastCkL[0]); g_prefs.putFloat("lastck1", g_lastCkL[1]); g_prefs.end();
}
static void stayCostReset() {         // zero the running bill + re-baseline both legs on the next reading (no accrual gap)
  g_stayCostC = 0.0; g_lastCkL[0] = -1.0f; g_lastCkL[1] = -1.0f; stayCostPersist();
}

static void baselinePersist() {                     // write the current unit's baseline to its per-MAC key
  char mac[16]; macKey(mac, sizeof(mac));
  if (!mac[0]) return;
  Preferences p; p.begin("hkwh", false); p.putFloat(mac, g_cfg.kwhBase); p.end();
}
// If we've never tracked the CURRENT g_cfg.hughesMac, write the -1 sentinel so baselineLoad captures kwhBase at
// the first packet instead of inheriting the OUTGOING unit's kwhBase (which configPersist just wrote to kwh_base).
// Idempotent: a unit we've seen before keeps its key. Used by adopt AND the portal save (2nd-pass finding#1).
static void seedFreshBaselineIfNew() {
  char mk[16]; macKey(mk, sizeof(mk));
  if (!mk[0]) return;
  Preferences p; p.begin("hkwh", false);
  if (!p.isKey(mk)) p.putFloat(mk, -1.0f);
  p.end();
}

static void configPersist() {
  g_prefs.begin("hughes", false);
  g_prefs.putString("ssid", g_cfg.wifiSsid);
  g_prefs.putString("pass", g_cfg.wifiPass);
  g_prefs.putString("mac",  g_cfg.hughesMac);
  g_prefs.putString("name", g_cfg.hughesName);
  g_prefs.putString("nick", g_cfg.nick);
  g_prefs.putFloat("kwh_base", g_cfg.kwhBase);
  g_prefs.putInt("rate_c",  g_cfg.rateC);
  g_prefs.putInt("bright",  g_cfg.bright);
  g_prefs.putInt("svc",     g_cfg.svcAmps);
  g_prefs.putInt("tz",      g_cfg.tzIndex);
  if (g_cfg.tierCount > 0) {                          // time-of-day rate schedule: compact blob [ver][count][s,e,cents-lo,cents-hi]*
    uint8_t buf[2 + MAX_TIERS*4]; buf[0] = 1; buf[1] = g_cfg.tierCount; int p = 2;
    for (int i = 0; i < g_cfg.tierCount; i++) {
      buf[p++] = g_cfg.tiers[i].startH; buf[p++] = g_cfg.tiers[i].endH;
      buf[p++] = g_cfg.tiers[i].cents & 0xFF; buf[p++] = g_cfg.tiers[i].cents >> 8;
    }
    g_prefs.putBytes("tiers", buf, p);
  } else g_prefs.remove("tiers");
  g_prefs.putInt("net",     g_cfg.netMode);
  g_prefs.putString("apikey", g_cfg.apiKey);
  g_prefs.putBool("alarm",  g_cfg.alarmOn);
  g_prefs.putBool("ntfyon",     g_cfg.ntfyOn);
  g_prefs.putString("ntfytopic", g_cfg.ntfyTopic);
  g_prefs.putString("ntfysrv",   g_cfg.ntfyServer);
  g_prefs.putInt("ntfymask",    g_cfg.ntfyMask);
  g_prefs.putInt("hbhour",      g_cfg.hbHour);
  g_prefs.putString("otaurl",   g_cfg.otaUrl);
  g_prefs.end();
}

static void configLoad() {
  bool seeded;
  g_prefs.begin("hughes", true);                 // read-only
  g_cfg.wifiSsid   = g_prefs.getString("ssid", "");
  g_cfg.wifiPass   = g_prefs.getString("pass", "");
  g_cfg.hughesMac  = g_prefs.getString("mac",  "");
  g_cfg.hughesName = g_prefs.getString("name", "");
  g_cfg.nick       = g_prefs.getString("nick", "");
  seeded           = g_prefs.getBool("seeded", false);
  g_cfg.kwhBase    = g_prefs.getFloat("kwh_base", 0.0f);
  g_cfg.rateC      = g_prefs.getInt("rate_c", 28);
  g_cfg.bright     = g_prefs.getInt("bright", 80);
  g_cfg.svcAmps    = g_prefs.getInt("svc", 50);
  g_cfg.tzIndex    = g_prefs.getInt("tz", 0);
  g_cfg.tierCount  = 0;                               // time-of-day rate schedule: load + validate the blob (bad -> flat)
  { uint8_t buf[2 + MAX_TIERS*4]; size_t got = g_prefs.getBytes("tiers", buf, sizeof(buf));
    if (got >= 2 && buf[0] == 1) { int n = buf[1];
      if (n >= 1 && n <= MAX_TIERS && got == (size_t)(2 + n*4)) {
        RateTier tmp[MAX_TIERS]; int p = 2;
        for (int i = 0; i < n; i++) { tmp[i].startH = buf[p++]; tmp[i].endH = buf[p++]; tmp[i].cents = buf[p] | (buf[p+1] << 8); p += 2; }
        int bad; if (tiersCover24(tmp, n, &bad)) { for (int i = 0; i < n; i++) g_cfg.tiers[i] = tmp[i]; g_cfg.tierCount = n; }
      }
    } }
  g_stayCostC = g_prefs.getDouble("staycost", 0.0);
  g_lastCkL[0] = g_prefs.getFloat("lastck0", -1.0f);      // R4: per-leg (old combined "lastck" is intentionally ignored -> one-time sub-sample gap on the 2.1.0 OTA, never a double-count)
  g_lastCkL[1] = g_prefs.getFloat("lastck1", -1.0f);
  g_cfg.netMode    = g_prefs.getInt("net", 0);
  g_cfg.apiKey     = g_prefs.getString("apikey", "");
  g_cfg.alarmOn    = g_prefs.getBool("alarm", false);
  g_cfg.ntfyOn     = g_prefs.getBool("ntfyon", false);
  g_cfg.ntfyTopic  = g_prefs.getString("ntfytopic", "");
  g_cfg.ntfyServer = g_prefs.getString("ntfysrv", "ntfy.sh");
  g_cfg.ntfyMask   = g_prefs.getInt("ntfymask", 0x7F);
  g_cfg.hbHour     = g_prefs.getInt("hbhour", -1);
  g_cfg.otaUrl     = g_prefs.getString("otaurl", "https://firmware.flensor.com/hughes.json");
  g_forceProv      = g_prefs.getBool("forceprov", false);   // set by reprovision/factory-reset -> force portal, skip driver-cred adopt
  g_prefs.end();
  baselineLoad();                                // override kwhBase with this unit's per-MAC baseline (if any)

  if (g_cfg.apiKey.length() < 8) {               // mint/upgrade to an 8-hex key (also the AP's WPA2 password, needs >=8)
    char k[10]; snprintf(k, sizeof(k), "%08x", (unsigned)esp_random());
    g_cfg.apiKey = k;
    g_prefs.begin("hughes", false); g_prefs.putString("apikey", g_cfg.apiKey); g_prefs.end();
    Serial.printf("api: generated key %s (shown on the INFO screen)\n", g_cfg.apiKey.c_str());
  }

#if defined(WIFI_SSID)
  // One-time migration: seed NVS from compile-time config.h so a board flashed with
  // the pre-provisioning build (or a dev seed) keeps its WiFi + Watchdog. The
  // "seeded" sentinel makes it fire exactly ONCE -- without it, seeding re-fills NVS
  // on the boot right after a /reprovision wipe, and the setup portal is unreachable.
  if (!seeded && !g_cfg.wifiSsid.length()) {
    g_cfg.wifiSsid = WIFI_SSID;
  #ifdef WIFI_PASS
    g_cfg.wifiPass = WIFI_PASS;
  #endif
  #ifdef HUGHES_MAC
    g_cfg.hughesMac = HUGHES_MAC;
  #endif
  #ifdef HUGHES_NAME
    g_cfg.hughesName = HUGHES_NAME;
  #endif
    configPersist();
    g_prefs.begin("hughes", false); g_prefs.putBool("seeded", true); g_prefs.end();
    Serial.println("config: seeded NVS from compile-time config.h (one-time)");
  }
#endif
}

// ---- US timezone table (POSIX TZ strings carry each zone's DST rules) ------------------------------
// Index 0 = "Not set" -> local time unknown (UTC fallback). The web + on-device pickers select an index;
// applyTz() pushes it into the C library so localtime_r() yields local wall-clock time. SNTP still sets
// the absolute UTC epoch (TLS cert validity is unaffected by TZ).
struct TzEntry { const char* name; const char* posix; };
static const TzEntry TZ_TABLE[] = {
  { "Not set",  ""                         },   // 0 -> UTC / local unknown
  { "Eastern",  "EST5EDT,M3.2.0,M11.1.0"   },   // 1
  { "Central",  "CST6CDT,M3.2.0,M11.1.0"   },   // 2
  { "Mountain", "MST7MDT,M3.2.0,M11.1.0"   },   // 3
  { "Arizona",  "MST7"                     },   // 4  (no DST)
  { "Pacific",  "PST8PDT,M3.2.0,M11.1.0"   },   // 5
  { "Alaska",   "AKST9AKDT,M3.2.0,M11.1.0" },   // 6
  { "Hawaii",   "HST10"                    },   // 7  (no DST)
};
static const int TZ_COUNT = sizeof(TZ_TABLE) / sizeof(TZ_TABLE[0]);
static int  tzClamp(int i)   { return (i >= 0 && i < TZ_COUNT) ? i : 0; }
static const char* tzName()  { return TZ_TABLE[tzClamp(g_cfg.tzIndex)].name; }
static const char* tzPosix() { return TZ_TABLE[tzClamp(g_cfg.tzIndex)].posix; }
static bool tzConfigured()   { return tzClamp(g_cfg.tzIndex) != 0; }
// Clock provenance (RUNTIME only -- there is no battery RTC, so the wall clock is LOST on every reboot until
// SNTP re-syncs or someone sets it by hand). Drives the "is this time trustworthy for billing?" nudges.
enum ClockProv { CLK_LOST = 0, CLK_MANUAL = 1, CLK_NTP = 2 };
static uint8_t g_clkProv = CLK_LOST;
// Is the DATE (not just the hour) trustworthy for the billing ledger? True for SNTP and the phone's real-epoch
// /settime; FALSE for the device's H:M-only editor (which fabricates the calendar date) and at boot. The ledger
// stays "undated" unless this is true, so a hand-typed clock can drive tiers (real hour) without poisoning dates.
static bool g_dateReal = false;
// M5: track "a REAL epoch was set" (NTP / phone) SEPARATELY from "a zone is known". The date is only trustworthy
// when BOTH hold. Keeping them apart means picking the time zone LATER (epoch already real, tz was 0) upgrades the
// date -- previously nothing at runtime re-dated the ledger except NTP or another /settime-with-epoch.
static bool g_epochReal = false;
static void recomputeDateReal() { g_dateReal = g_epochReal && tzConfigured(); }
static const char* clkProvWord() { return g_clkProv == CLK_NTP ? "from the internet" : g_clkProv == CLK_MANUAL ? "set by hand" : "not set"; }
static void applyTz() {                                  // push the selected zone into the C library (setenv+tzset)
  const char* p = tzPosix();
  if (*p) setenv("TZ", p, 1); else unsetenv("TZ");
  tzset();
  recomputeDateReal();   // M5 (review finding#1): EVERY tz change flows through here (web Save, device picker, serial, /settime) -> picking the zone later now dates the ledger, and clearing it can't leave g_dateReal stale
}
// Current LOCAL time as "YYYY-MM-DD HH:MM", or "--" when the clock isn't synced yet (epoch < 2023).
static void localTimeStr(char* out, size_t n) {
  time_t now = time(nullptr);
  if (now < 1700000000) { snprintf(out, n, "--"); return; }
  struct tm tmv; localtime_r(&now, &tmv);
  strftime(out, n, "%Y-%m-%d %H:%M", &tmv);
}

// ---- time-of-day rate resolver (needs the clock + timezone) ----
static int localHourNow() {                            // -1 = local time unknown (no zone, or clock not synced)
  if (!tzConfigured()) return -1;
  time_t now = time(nullptr); if (now < 1700000000) return -1;
  struct tm t; localtime_r(&now, &t); return t.tm_hour;
}
static int effectiveRateC() {                          // current cents/kWh: the active tier, or the flat rate
  if (g_cfg.tierCount == 0) return g_cfg.rateC;
  int h = localHourNow(); if (h < 0) return g_cfg.rateC;        // clock/zone unknown -> flat fallback
  for (int i = 0; i < g_cfg.tierCount; i++) if (tierContains(g_cfg.tiers[i], h)) return g_cfg.tiers[i].cents;
  return g_cfg.rateC;                                           // unreachable while the coverage invariant holds
}
static String tiersToStr() {                           // "start-end-cents,..." ("" = flat) for /status + the web editor
  String s; for (int i = 0; i < g_cfg.tierCount; i++) {
    if (i) s += ",";
    uint8_t e = g_cfg.tiers[i].endH; if (e == 0) e = 24;        // show a midnight end as 24, not 0
    s += String(g_cfg.tiers[i].startH) + "-" + String(e) + "-" + String(g_cfg.tiers[i].cents);
  }
  return s;
}

// ---------------------------------------------------------------- decode
static void toHex(const uint8_t* p, size_t n, char* out) {
  static const char* H = "0123456789abcdef";
  for (size_t i = 0; i < n; i++) { out[i*2] = H[p[i] >> 4]; out[i*2+1] = H[p[i] & 0xF]; }
  out[n*2] = 0;
}
static int32_t be32(const uint8_t* p) {
  return (int32_t)(((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
                   ((uint32_t)p[2] << 8)  |  (uint32_t)p[3]);
}

// One decoded leg of the 50A feed. bleTask/notify writes; the web server reads.
struct Leg {
  bool     valid;
  float    volts, amps, watts, kwh, hz;
  uint8_t  err;
  uint32_t seenMs;
  char     raw[81];                 // last 40-byte packet as hex, for the collector to re-derive
};
static Leg g_leg[2];                // [0] = line 1, [1] = line 2

WebServer server(HTTP_PORT);
DNSServer g_dns;                    // captive-portal catch-all, provisioning mode only
enum Mode { MODE_BRIDGE, MODE_PROV };
static Mode              g_mode         = MODE_BRIDGE;
static bool              g_apServing    = false;   // hosting our own AP (Hughes-Bridge-XXXX)
static void netApply();                            // fwd decl: apply g_cfg.netMode live (SETTINGS toggle / HTTP)
static bool wifiCaptureCreds();                     // fwd decl: adopt driver-stored STA creds into g_cfg if ours are empty
static bool syncTime(uint32_t timeoutMs = 8000);    // fwd decl: SNTP (proactive on STA connect, before its definition)
void handleNtfy();                                  // fwd decl: POST /ntfy handler (defined with the ntfy engine, below startBridge)
static bool alertsDark();                           // fwd decl: v2.1 1.4 dead-man's switch state (defined with the ntfy engine)
void handleSupportBundle();                         // fwd decl: POST /support_bundle (on-demand support bundle; defined near the ntfy engine)
static void ntfyEnsureTopic();                      // fwd decl: mint the ntfy topic if empty (used by the on-device QR page)
static void ntfyEnqueue(const char* title, const char* body, int prio, const char* tags);   // fwd decl: SEND TEST ALERT
void handleOta();                                   // fwd decl: POST /ota (check+update); defined with the OTA engine below
void handleOtaUrl();                                // fwd decl: POST /ota_url (set manifest URL)
static void enterSleep();                          // fwd decl: graceful deep-sleep power-off (tap to wake)
static bool firmwareTouch(int x, int y);           // fwd decl: FIRMWARE page taps (defined after the OTA engine)
static void wipeHistory();                         // fwd decl: delete all SD /hist_*.bin (factory reset); defined w/ the SD code
static void apSsid(char* out, size_t n);           // fwd decl: our own AP SSID (Hughes-Bridge-XXXX)
static void macStr(char* out, size_t n) {          // device MAC "AA:BB:..:FF" from efuse (valid in ANY mode, incl. AP/off)
  uint8_t m[6] = {0}; esp_read_mac(m, ESP_MAC_WIFI_STA);
  snprintf(out, n, "%02X:%02X:%02X:%02X:%02X:%02X", m[0],m[1],m[2],m[3],m[4],m[5]);
}
static volatile bool     g_reboot       = false;   // set by a handler; acted on in loop()
SemaphoreHandle_t g_mtx;            // guards g_leg + the status fields below
static volatile uint32_t g_lastNotifyMs = 0;
static volatile bool     g_hadLink      = false;  // ever received real data -> a later silence is an outage, not first-connect
static volatile bool     g_connected    = false;
static String            g_err          = "boot";
static int               g_rssi         = 0;
static char              g_peerMac[20]  = {0};   // Watchdog address + advertised name, captured at connect
static char              g_peerName[28] = {0};   // (fixed buffers -> safe to read from the display core)

static NimBLEClient*  g_cli           = nullptr;
static volatile bool  g_wantReconnect = true;
static volatile bool  g_wantReset     = false;   // set by /reset_odometer or SETTINGS -> bleTask writes "RESEt" to fff5
static volatile bool  g_released      = false;   // user hand-off: drop the link so the phone app can connect (one client at a time)

// Battery/power state — defined here (not in the battery section below) because handleStatus() /
// statusState() sit above that section and read them. Values are set by battRead(). See the ADC
// setup + trend-based inference near battSetup()/battRead().
static int  g_battMv    = 0;                      // battery millivolts (0 = not read yet)
static int  g_battPct   = 0;                      // rough state-of-charge %
static bool g_onBattery = false;                  // inferred: running on the cell (USB likely lost)
// statusState(): single source of truth for the status word; defined below drawStatusWord().
static const char* statusState(char* word, size_t wlen, uint16_t* color);

// --- DEVICES picker (SETTINGS->DEVICES): change which Watchdog we track, on-device ---
// A release-then-scan owned by bleTask (single radio owner), so the display core never
// touches BLE. Adopt = persist the new MAC + reboot -> the boot path loads that unit's
// per-MAC baseline + graph history. Roaming (a saved roster, live switch) is a future add.
struct Found { char mac[20]; char name[28]; int rssi; uint8_t atype; uint32_t lastSeen; float ema; };
static const int      FOUND_MAX = 8;
#define SCAN_AGE_MS   6000               // drop a device from the live list if unseen this long (~4 scan cycles)
static Found          g_found[FOUND_MAX];
static SemaphoreHandle_t g_foundMtx;             // guards g_found[] (bleTask scan-merge on core 0 <-> UI on core 1)
static volatile int   g_foundN   = 0;            // bleTask -> display: live candidate count
static volatile bool  g_wantScan = false;        // display -> bleTask: LIVE scan mode (keep scanning while true)
static volatile bool  g_scanBusy = false;        // bleTask -> display: a scan cycle is running (first-pass "scanning..." state)
static volatile bool  g_scanDone = false;        // bleTask -> display: table updated -> repaint (edge)
static bool           g_scanLive = false;        // bleTask-internal: currently in the continuous-scan loop

// 40-byte packet arrives as two 20-byte notifications; reassemble keyed on header.
static uint8_t g_buf[64];
static size_t  g_have = 0;

// Decode a complete 40-byte packet (header already verified) into its leg.
static void decodePacket(const uint8_t* d) {
  int line = (d[37] == 0x01) ? 2 : 1;          // Line ID bytes 37-39: 01 01 01 = L2, 00 00 00 = L1
  Leg& L = g_leg[line - 1];
  xSemaphoreTake(g_mtx, portMAX_DELAY);
  L.volts  = be32(d + 3)  / 10000.0f;
  L.amps   = be32(d + 7)  / 10000.0f;
  L.watts  = be32(d + 11) / 10000.0f;
  L.kwh    = be32(d + 15) / 10000.0f;          // lifetime odometer; the stay baseline lives in hughes-collect
  L.err    = d[19];
  L.hz     = be32(d + 31) / 100.0f;
  L.seenMs = millis();
  L.valid  = true;
  g_hadLink = true;                            // we've had a live link -> a later silence is an outage
  toHex(d, 40, L.raw);
  xSemaphoreGive(g_mtx);
}

// Notify callback (runs in the NimBLE host task). Reassemble + decode.
static void onNotify(NimBLERemoteCharacteristic* c, uint8_t* data, size_t len, bool isNotify) {
  g_lastNotifyMs = millis();
#ifdef VERBOSE_RAW
  { char hx[131]; size_t n = len < 64 ? len : 64; toHex(data, n, hx);
    Serial.printf("notif %2u: %s\n", (unsigned)len, hx); }
#endif
  if (len >= 3 && data[0] == 0x01 && data[1] == 0x03 && data[2] == 0x20) {
    g_have = (len <= sizeof(g_buf)) ? len : 0;             // chunk 1: (re)sync on header
    if (g_have) memcpy(g_buf, data, g_have);
  } else if (g_have > 0) {
    if (g_have + len <= sizeof(g_buf)) { memcpy(g_buf + g_have, data, len); g_have += len; } // chunk 2
    else g_have = 0;                                       // overflow -> drop, resync on next header
  }
  if (g_have >= 40) { decodePacket(g_buf); g_have = 0; }
}

class CliCB : public NimBLEClientCallbacks {
  void onDisconnect(NimBLEClient* c, int reason) override {
    g_connected = false; g_wantReconnect = true;
    logf("ble: disconnected (reason %d)", reason);
  }
};
static CliCB g_cliCB;

static bool matchTarget(const NimBLEAdvertisedDevice* d) {
  if (!g_cfg.hughesMac.length() && !g_cfg.hughesName.length()) return false;  // no Watchdog chosen -> idle; pick one in DEVICES (don't grab a neighbour)
  if (g_cfg.hughesMac.length()) {                          // pinned by MAC (preferred)
    String mac = g_cfg.hughesMac; mac.toLowerCase();
    String a = String(d->getAddress().toString().c_str()); a.toLowerCase();
    return a == mac;
  }
  String nm = d->haveName() ? String(d->getName().c_str()) : String();
  if (g_cfg.hughesName.length()) return nm == g_cfg.hughesName;   // pinned by name (random addr)
  return nameMatches(nm) || d->isAdvertisingService(SVC);  // fallback: any Watchdog in range
}

static bool connectTarget() {
  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->setActiveScan(true);
  NimBLEScanResults res = scan->getResults(6000, false);

  const NimBLEAdvertisedDevice* tgt = nullptr;
  for (int i = 0; i < res.getCount(); i++) {
    const NimBLEAdvertisedDevice* d = res.getDevice(i);
    if (matchTarget(d)) { tgt = d; g_rssi = d->getRSSI(); break; }
  }
  if (!tgt) { g_err = "watchdog not advertising (out of range / app connected)"; scan->clearResults(); return false; }

  strncpy(g_peerMac, tgt->getAddress().toString().c_str(), sizeof(g_peerMac)-1); g_peerMac[sizeof(g_peerMac)-1]=0;
  g_peerName[0] = 0;
  if (tgt->haveName()) { strncpy(g_peerName, tgt->getName().c_str(), sizeof(g_peerName)-1); g_peerName[sizeof(g_peerName)-1]=0; }

  if (g_cli) { NimBLEDevice::deleteClient(g_cli); g_cli = nullptr; }
  g_cli = NimBLEDevice::createClient();
  g_cli->setClientCallbacks(&g_cliCB, false);
  if (!g_cli->connect(tgt)) {
    g_err = "connect failed"; NimBLEDevice::deleteClient(g_cli); g_cli = nullptr; scan->clearResults(); return false;
  }

  NimBLERemoteService* svc = g_cli->getService(SVC);
  NimBLERemoteCharacteristic* tx = svc ? svc->getCharacteristic(CH_TX) : nullptr;
  if (!tx || !tx->canNotify()) { g_err = "ffe2 notify characteristic missing"; g_cli->disconnect(); scan->clearResults(); return false; }
  if (!tx->subscribe(true, onNotify)) { g_err = "subscribe failed"; g_cli->disconnect(); scan->clearResults(); return false; }

  scan->clearResults();
  g_connected = true; g_wantReconnect = false; g_err = ""; g_lastNotifyMs = millis();
  logf("ble: connected + subscribed to ffe2");
  return true;
}

// Owns the radio: (re)connect and hold the subscription; force a reconnect if the
// link goes silent. The web server runs on the other core so HTTP never blocks here.
void bleTask(void* pv) {
  for (;;) {
    if (g_wantScan) {                                // DEVICES LIVE scan: keep scanning, merge into g_found, age-out
      if (!g_scanLive) { g_scanLive = true; led(28, 0, 28);                 // enter: magenta + drop the held link
                         if (g_cli && g_cli->isConnected()) g_cli->disconnect(); g_connected = false; }
      NimBLEScan* scan = NimBLEDevice::getScan();
      scan->setActiveScan(true);
      NimBLEScanResults res = scan->getResults(1400, false);                // one short cycle (~1.4s -> live-ish)
      uint32_t now = millis();
      xSemaphoreTake(g_foundMtx, portMAX_DELAY);
      for (int i = 0; i < res.getCount(); i++) {
        const NimBLEAdvertisedDevice* d = res.getDevice(i);
        String nm = d->haveName() ? String(d->getName().c_str()) : String();
        if (!nameMatches(nm) && !d->isAdvertisingService(SVC)) continue;    // Watchdogs only
        char mac[20]; strncpy(mac, d->getAddress().toString().c_str(), sizeof(mac)-1); mac[sizeof(mac)-1] = 0;
        int rssi = d->getRSSI(), idx = -1;
        for (int j = 0; j < g_foundN; j++) if (!strcmp(g_found[j].mac, mac)) { idx = j; break; }
        if (idx < 0 && g_foundN < FOUND_MAX) { idx = g_foundN++; strcpy(g_found[idx].mac, mac);
          g_found[idx].name[0] = 0; g_found[idx].ema = rssi; g_found[idx].atype = d->getAddress().getType(); }
        if (idx >= 0) {
          g_found[idx].ema = g_found[idx].ema * 0.6f + rssi * 0.4f;         // EMA-smooth the noisy RSSI
          g_found[idx].rssi = (int)lroundf(g_found[idx].ema);
          g_found[idx].lastSeen = now;
          if (nm.length()) { strncpy(g_found[idx].name, nm.c_str(), sizeof(g_found[idx].name)-1); g_found[idx].name[sizeof(g_found[idx].name)-1] = 0; }
        }
      }
      int w = 0;                                                            // age-out unseen devices, compact in place
      for (int r = 0; r < g_foundN; r++) if (now - g_found[r].lastSeen <= SCAN_AGE_MS) { if (w != r) g_found[w] = g_found[r]; w++; }
      g_foundN = w;
      xSemaphoreGive(g_foundMtx);
      scan->clearResults();
      g_scanBusy = false; g_scanDone = true;                                // table updated -> repaint the rows
      continue;                                                            // keep scanning while g_wantScan stays set
    }
    if (g_scanLive) { g_scanLive = false; g_wantReconnect = true; }         // left the scan page -> reconnect the tracked unit
    if (g_released) {                                // user handed the link back to the phone app
      if (g_cli && g_cli->isConnected()) g_cli->disconnect();
      g_connected = false; g_wantReconnect = true;   // primed to reconnect the moment we un-release
      led(24, 0, 28);                                // dim magenta: released, idle (app may connect)
      vTaskDelay(pdMS_TO_TICKS(400));
      continue;
    }
    if (g_wantReconnect || !g_cli || !g_cli->isConnected()) {
      led(0, 0, 40);                                 // blue: (re)connecting
      if (connectTarget()) led(0, 40, 0);            // green: connected + subscribed
      else { led(40, 0, 0); vTaskDelay(pdMS_TO_TICKS(3000)); }  // red: not found / failed
    } else if (millis() - g_lastNotifyMs > STALE_MS) {
      led(40, 0, 0);                                 // red: link went silent
      Serial.println("no notifications -> forcing reconnect");
      g_cli->disconnect(); g_wantReconnect = true;
    } else {
      // refresh the live link RSSI (~2Hz max) so the on-screen health readout tracks
      // the real signal, not the stale snapshot taken at connect time.
      static uint32_t lastRssiMs = 0;
      if (millis() - lastRssiMs > 2000) {
        lastRssiMs = millis();
        int r = g_cli->getRssi();                     // 0 on HCI error; a real RSSI is negative
        if (r != 0) g_rssi = r;
      }
      if (g_wantReset) {                             // send the reverse-engineered reset command
        NimBLERemoteService* svc = g_cli->getService(SVC);
        NimBLERemoteCharacteristic* cmd = svc ? svc->getCharacteristic(CMD) : nullptr;
        if (cmd && (cmd->canWrite() || cmd->canWriteNoResponse())) {
          bool ok = cmd->writeValue((const uint8_t*)"RESEt", 5, true);   // 52 45 53 45 74
          Serial.printf("odometer RESEt -> fff5: %s\n", ok ? "OK" : "write FAILED");
        } else Serial.println("odometer reset: fff5 not found/writable");
        g_wantReset = false;
      }
      vTaskDelay(pdMS_TO_TICKS(250));
    }
  }
}

// ---------------------------------------------------------------- bridge HTTP
// 60s rolling aggregate of a leg (see docs/status-spec.md). Scans the last 60 entries of
// the GRAPHS 1Hz rings, counting ONLY seconds where a real packet arrived (freshness mask),
// so gaps shrink the window instead of averaging in stale zeros.
struct Agg {
  int   window;                    // real samples in the last 60s (0..60); 0 => aggregates are null
  long  wAvg, wPeak;               // watts
  float aAvg, aPeak;               // amps
};
static Agg computeAgg(int line);   // defined after the GRAPHS rings

static String legJson(const Leg& L, int line, const Agg& g) {
  if (!L.valid) return "null";
  long age = (long)((millis() - L.seenMs) / 1000);
  char b[480]; int n = 0;
  n += snprintf(b+n, sizeof(b)-n,
    "{\"line\":%d,\"volts\":%.1f,\"amps\":%.1f,\"watts\":%.0f,\"kwh\":%.3f,"
    "\"hz\":%.1f,\"err\":%u,\"age_s\":%ld,",
    line, L.volts, L.amps, L.watts, L.kwh, L.hz, L.err, age);
  if (g.window > 0)
    n += snprintf(b+n, sizeof(b)-n,
      "\"watts_avg\":%ld,\"watts_peak\":%ld,\"amps_avg\":%.1f,\"amps_peak\":%.1f,\"window_s\":%d,",
      g.wAvg, g.wPeak, g.aAvg, g.aPeak, g.window);
  else
    n += snprintf(b+n, sizeof(b)-n,   // empty window: null, NOT 0 -- a real 0W avg must differ from "no data"
      "\"watts_avg\":null,\"watts_peak\":null,\"amps_avg\":null,\"amps_peak\":null,\"window_s\":0,");
  snprintf(b+n, sizeof(b)-n, "\"raw\":\"%s\"}", L.raw);
  return String(b);
}

void handleStatus() {
  g_lastWebMs = millis();              // a viewing/polling phone keeps a no-Watchdog unit awake
  xSemaphoreTake(g_mtx, portMAX_DELAY);
  Leg l1 = g_leg[0], l2 = g_leg[1];
  bool conn = g_connected; String err = g_err; int rssi = g_rssi;
  xSemaphoreGive(g_mtx);

  uint32_t now = millis();
  bool f1 = l1.valid && (now - l1.seenMs) < 5000;
  bool f2 = l2.valid && (now - l2.seenMs) < 5000;
  float cw = (f1 ? l1.watts : 0.0f) + (f2 ? l2.watts : 0.0f);
  float ck = (l1.valid ? l1.kwh : 0.0f) + (l2.valid ? l2.kwh : 0.0f);

  Agg g1 = computeAgg(1), g2 = computeAgg(2);        // 60s rolling avg/peak per leg
  bool a1 = l1.valid && g1.window > 0, a2 = l2.valid && g2.window > 0;
  long cwAvg = (a1 ? g1.wAvg : 0) + (a2 ? g2.wAvg : 0);   // combined avg = sum of per-leg avgs

  String body = "{";
  body += "\"bridge\":\"hughes-esp32\",";
  body += "\"connected\":" + String(conn ? "true" : "false") + ",";
  body += "\"released\":" + String(g_released ? "true" : "false") + ",";
  body += "\"rssi\":" + String(rssi) + ",";
  body += "\"error\":\"" + err + "\",";
  body += "\"legs\":[" + legJson(l1, 1, g1) + "," + legJson(l2, 2, g2) + "],";
  body += "\"combined_watts\":" + String(cw, 0) + ",";
  body += "\"combined_watts_avg\":" + ((a1 || a2) ? String(cwAvg) : String("null")) + ",";
  body += "\"combined_kwh\":" + String(ck, 3) + ",";
  // --- additive fields (2026-08-11, for the web dashboard). ALL new keys; every key above is
  //     byte-identical so hughes-collect is unaffected. Safe here: g_mtx was released above. ---
  char sw[22]; const char* st = statusState(sw, sizeof(sw), nullptr);
  bool hasWd = g_cfg.hughesMac.length() || g_cfg.hughesName.length();
  float stay = ck - g_cfg.kwhBase; if (stay < 0) stay = 0;   // this-stay energy (device baseline; local counter)
  body += "\"stay_kwh\":"     + String(stay, 3) + ",";
  body += "\"rate_c\":"       + String(effectiveRateC()) + ",";   // CURRENT effective $/kWh cents (tier-aware; Pi math auto-tiers, key unchanged)
  body += "\"rate_flat_c\":"  + String(g_cfg.rateC) + ",";        // flat/fallback rate (settings-form prefill)
  body += "\"rate_tiers\":\"" + tiersToStr() + "\",";             // time-of-day schedule ("" = flat; "s-e-c,...")
  body += "\"rate_hour\":"    + String(localHourNow()) + ",";     // resolved local hour, or -1 when the clock/zone is unknown
  body += "\"stay_cost_c\":"  + String((long)(g_stayCostC + 0.5)) + ",";   // accrued this-stay cost in cents (tier-accurate accumulator)
  body += "\"bright\":"       + String(g_cfg.bright) + ",";
  { char lt[24]; localTimeStr(lt, sizeof(lt));                // timezone + local wall-clock (additive; feeds the web picker + rate tiers)
    body += "\"tz\":" + String(g_cfg.tzIndex) + ",\"tz_name\":\"" + String(tzName()) + "\",\"local_time\":\"" + String(lt) + "\","; }
  body += "\"clk_prov\":" + String((int)g_clkProv) + ",";    // 0=lost 1=hand-set 2=SNTP -- how trustworthy the clock is for billing
  body += "\"fw\":\"" FW_VERSION "\",";                       // firmware version (additive; free fleet-version audit for the Pi)
  body += "\"ntfy_on\":"    + String(g_cfg.ntfyOn ? "true" : "false") + ",";   // ntfy push alerts (web Settings)
  body += "\"ntfy_topic\":\"" + g_cfg.ntfyTopic + "\",";     // subscribe topic (hughes-<hex>); shown + QR'd on the page
  body += "\"ntfy_mask\":"  + String(g_cfg.ntfyMask) + ",";  // per-alert enable bitmask
  body += "\"hb_hour\":"    + String(g_cfg.hbHour) + ",";    // daily heartbeat local hour (-1 off) -- v2.1 1.4
  body += "\"alerts_dark\":" + String(alertsDark() ? "true" : "false") + ",";  // push ON but notify server unreachable
  { String u = g_cfg.otaUrl; u.replace("\\","\\\\"); u.replace("\"","\\\""); body += "\"ota_url\":\"" + u + "\","; }  // OTA manifest URL (web form)
  body += "\"nick\":\""       + g_cfg.nick + "\",";          // sanitized on input (/config) -> JSON-safe
  { String s = g_cfg.wifiSsid; s.replace("\\","\\\\"); s.replace("\"","\\\"");   // JSON-escape the SSID
    body += "\"wifi_ssid\":\"" + s + "\","; }                // current home SSID (prefills the web Wi-Fi form)
  body += "\"svc_amps\":"     + String(g_cfg.svcAmps) + ",";
  body += "\"has_watchdog\":" + String(hasWd ? "true" : "false") + ",";
  body += "\"on_battery\":"   + String(g_onBattery ? "true" : "false") + ",";
  body += "\"batt_pct\":"     + String(g_battPct) + ",";
  body += "\"batt_mv\":"      + String(g_battMv) + ",";
  body += "\"state\":\""      + String(st) + "\",";
  body += "\"status_word\":\"" + String(sw) + "\"";
  body += "}";
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", body);
}

void handleRoot() {
  server.send(200, "text/plain",
    "Hughes Power Watchdog (Gen 1) BLE->WiFi bridge.\n"
    "GET /status                    -- live JSON\n"
    "POST /reprovision?key=KEY      -- clear WiFi/Watchdog config, reboot into setup portal\n"
    "POST /reset_odometer?key=KEY   -- zero the Watchdog lifetime kWh odometer\n"
    "  KEY = the API KEY on the device INFO screen (SETTINGS -> INFO).\n");
}

// Destructive endpoints require ?key=<the API KEY shown on the INFO screen>. Blocks a
// casual/accidental LAN curl from wiping config or zeroing the odometer.
static bool apiAuthed() {
  return g_cfg.apiKey.length() && server.hasArg("key") && server.arg("key") == g_cfg.apiKey;
}

// Live dashboard (Tier 1 -- auto-refreshes from /status). Served at "/" in HOME and AP modes so a
// phone on our own Hughes-Bridge-XXXX network sees data with no home Wi-Fi. Themed to match the fleet
// console family (a local dashboard palette). Self-contained: system-mono + numeric entities only, no CDN.
// %HOST% is injected for the captive-portal "open in a real browser" affordance (iOS mini-browser).
void handleDash() {
  static const char DASH[] PROGMEM = R"HTML(<!doctype html>
<meta name=viewport content="width=device-width,initial-scale=1"><meta charset=utf-8>
<title>Shore Power</title><style>
:root{--bg:#03060d;--bg2:#050b16;--panel:rgba(8,18,30,.62);--cyan:#39e0ff;--amber:#ffb454;--green:#38ff9d;
--red:#ff5069;--violet:#9d7bff;--dim:#4d7288;--line:rgba(57,224,255,.22);--grid:rgba(57,224,255,.05);
--ink:#cfe9f5;--ink-bright:#eafcff;--glow:0 0 8px currentColor;
--mono:'SFMono-Regular',ui-monospace,'Roboto Mono','DejaVu Sans Mono',Consolas,monospace}
*{box-sizing:border-box}html,body{margin:0;padding:0;min-height:100%}
body{background:radial-gradient(120% 90% at 50% -10%,#0a1830 0%,var(--bg2) 45%,var(--bg) 100%);
background-attachment:fixed;color:var(--ink);font-family:var(--mono);font-size:14px;
-webkit-font-smoothing:antialiased;overflow-x:hidden;padding-bottom:24px}
.fx{position:fixed;inset:0;z-index:2;pointer-events:none}
.fx.scan{z-index:6;background:repeating-linear-gradient(to bottom,rgba(0,0,0,.11) 0 1px,transparent 1px 4px)}
.fx.vig{background:radial-gradient(120% 100% at 50% 42%,transparent 55%,rgba(0,0,0,.55) 100%)}
.fx.flick{background:rgba(57,224,255,.02);animation:flick 7s steps(60) infinite}
@keyframes flick{0%,96%,100%{opacity:.5}97%{opacity:.1}98%{opacity:.8}}
@keyframes pulse{0%,100%{opacity:1}50%{opacity:.4}}@keyframes blink{50%{opacity:.28}}
.wrap{position:relative;z-index:3;max-width:520px;margin:0 auto;padding:18px 16px 26px}
header{display:flex;align-items:center;gap:12px;flex-wrap:wrap;padding:12px 14px;margin-bottom:12px;
border:1px solid var(--line);border-radius:4px;background:linear-gradient(180deg,rgba(57,224,255,.06),transparent)}
.brand{font-weight:700;font-size:16px;color:var(--ink-bright);text-shadow:0 0 14px rgba(57,224,255,.5);line-height:1.15}
.brand b{color:var(--cyan)}.brand .sub{display:block;font-size:9.5px;letter-spacing:.34em;color:var(--dim);font-weight:400;margin-top:4px}
.spacer{flex:1}
.chip{display:flex;align-items:center;gap:8px;font-size:11px;letter-spacing:.14em;padding:6px 11px;
border:1px solid var(--line);border-radius:3px;background:rgba(56,255,157,.05);color:var(--green);white-space:nowrap}
.chip .dot{width:9px;height:9px;border-radius:50%;background:currentColor;box-shadow:0 0 9px currentColor;animation:pulse 1.7s ease-in-out infinite}
.chip.ok{color:var(--green);background:rgba(56,255,157,.06);border-color:rgba(56,255,157,.3)}
.chip.warn{color:var(--amber);background:rgba(255,180,84,.06);border-color:rgba(255,180,84,.32)}
.chip.crit{color:var(--red);background:rgba(255,80,105,.07);border-color:rgba(255,80,105,.36)}
.chip.crit .dot{animation:blink .7s steps(1) infinite}
.chip.idle{color:var(--dim);background:rgba(77,114,136,.1);border-color:var(--line)}.chip.idle .dot{animation:none}
.chip.link{color:var(--violet);background:rgba(157,123,255,.08);border-color:rgba(157,123,255,.34)}
.sig{display:flex;align-items:flex-end;gap:2px;height:15px}
.sig i{width:3px;background:var(--dim);border-radius:1px;opacity:.35}
.sig i:nth-child(1){height:5px}.sig i:nth-child(2){height:8px}.sig i:nth-child(3){height:11px}.sig i:nth-child(4){height:15px}
.sig i.on{background:var(--green);opacity:1;box-shadow:0 0 6px rgba(56,255,157,.5)}
.sig.warn i.on{background:var(--amber);box-shadow:0 0 6px rgba(255,180,84,.5)}
.sig.crit i.on{background:var(--red);box-shadow:0 0 6px rgba(255,80,105,.5)}
.ob{font-size:10.5px;color:var(--dim);letter-spacing:.03em;margin:-4px 2px 12px;text-align:center}
.ob a{color:var(--cyan);opacity:.85;text-decoration:none}
.panel{position:relative;background:var(--panel);border:1px solid var(--line);border-radius:4px;
padding:14px 15px;margin-bottom:11px;backdrop-filter:blur(2px);overflow:hidden}
.panel::before{content:'';position:absolute;inset:0;background:linear-gradient(var(--grid) 1px,transparent 1px) 0 0/100% 26px;opacity:.5;pointer-events:none}
.phead{display:flex;align-items:center;gap:8px;font-size:10.5px;letter-spacing:.24em;color:var(--dim);
text-transform:uppercase;margin-bottom:11px;border-bottom:1px solid var(--line);padding-bottom:8px}
.phead .id{margin-left:auto;color:var(--cyan);opacity:.5;font-size:9.5px;letter-spacing:.1em}
.led{width:8px;height:8px;border-radius:50%;background:var(--green);box-shadow:0 0 8px var(--green);flex:none}
.led.off{background:var(--dim);box-shadow:none}.led.amber{background:var(--amber);box-shadow:0 0 8px var(--amber)}
.led.red{background:var(--red);box-shadow:0 0 8px var(--red);animation:blink .7s steps(1) infinite}
.banner{position:relative;z-index:1;display:flex;align-items:center;gap:15px;padding:18px 17px;margin-bottom:11px;
border:1px solid;border-radius:5px;overflow:hidden}
.banner .glyph{font-size:30px;line-height:1;flex:none;filter:drop-shadow(0 0 10px currentColor)}
.banner .btxt{flex:1;min-width:0}
.banner .bstate{font-size:23px;font-weight:700;letter-spacing:.04em;line-height:1.05;text-shadow:0 0 16px currentColor;color:currentColor}
.banner .bsub{font-size:12px;color:var(--ink-bright);opacity:.82;margin-top:5px;letter-spacing:.02em}
.banner.ok{color:var(--green);border-color:rgba(56,255,157,.4);background:linear-gradient(110deg,rgba(56,255,157,.12),rgba(56,255,157,.02))}
.banner.crit{color:var(--red);border-color:rgba(255,80,105,.45);background:linear-gradient(110deg,rgba(255,80,105,.14),rgba(255,80,105,.02))}
.banner.warn{color:var(--amber);border-color:rgba(255,180,84,.42);background:linear-gradient(110deg,rgba(255,180,84,.13),rgba(255,180,84,.02))}
.banner.idle{color:var(--dim);border-color:var(--line);background:linear-gradient(110deg,rgba(77,114,136,.14),transparent)}
.banner.link{color:var(--violet);border-color:rgba(157,123,255,.42);background:linear-gradient(110deg,rgba(157,123,255,.13),rgba(157,123,255,.02))}
.banner.crit .glyph{animation:blink .7s steps(1) infinite}
body.stale .legs,body.stale #c-kw{opacity:.4;transition:opacity .3s}  /* U14: dim readings while the bridge is unreachable */
.legs{display:grid;grid-template-columns:1fr 1fr;gap:11px}
@media (max-width:430px){.legs{grid-template-columns:1fr}}
.leg .amp{display:flex;align-items:baseline;gap:6px}
.leg .amp .n{font-size:38px;font-weight:700;line-height:1;color:var(--ink-bright);text-shadow:0 0 14px rgba(57,224,255,.35);font-variant-numeric:tabular-nums}
.leg .amp .u{font-size:13px;color:var(--dim)}
.leg .of{font-size:11px;color:var(--dim);letter-spacing:.08em;margin:3px 0 9px}
.leg.warm .amp .n{color:var(--amber)}.leg.hot .amp .n{color:var(--red)}
.gauge{position:relative;height:13px;border:1px solid var(--line);border-radius:3px;background:rgba(0,0,0,.4);overflow:hidden}
.gauge .zone{position:absolute;top:0;bottom:0;right:0;width:10%;background:repeating-linear-gradient(45deg,rgba(255,80,105,.22) 0 4px,transparent 4px 8px)}
.gauge .fill{position:absolute;top:0;bottom:0;left:0;border-radius:2px;background:linear-gradient(90deg,rgba(56,255,157,.55),var(--green));transition:width .5s ease,background .3s}
.gauge .fill.warm{background:linear-gradient(90deg,rgba(255,180,84,.5),var(--amber))}
.gauge .fill.hot{background:linear-gradient(90deg,rgba(255,80,105,.5),var(--red))}
.vrow{display:flex;align-items:center;justify-content:space-between;margin-top:12px;gap:8px}
.vrow .v{display:flex;align-items:baseline;gap:5px}
.vrow .v .vn{font-size:19px;font-weight:700;color:var(--ink-bright);font-variant-numeric:tabular-nums}
.vrow .v .vu{font-size:11px;color:var(--dim)}
.tag{font-size:10px;letter-spacing:.12em;text-transform:uppercase;padding:3px 8px;border-radius:3px;border:1px solid currentColor;font-weight:600}
.tag.ok{color:var(--green);background:rgba(56,255,157,.08)}
.tag.warn{color:var(--amber);background:rgba(255,180,84,.09)}
.tag.crit{color:var(--red);background:rgba(255,80,105,.09)}
.kw{margin-top:10px;font-size:11px;color:var(--dim);letter-spacing:.06em;display:flex;justify-content:space-between}
.kw b{color:var(--ink);font-weight:600}
.nodata{color:var(--dim);letter-spacing:.16em;text-transform:uppercase;font-size:12px;text-align:center;padding:22px 0}
.comb{display:flex;align-items:center;justify-content:space-between;gap:12px}
.comb .cn{display:flex;align-items:baseline;gap:7px}
.comb .cn .n{font-size:30px;font-weight:700;color:var(--ink-bright);text-shadow:0 0 14px rgba(57,224,255,.35);font-variant-numeric:tabular-nums}
.comb .cn .u{font-size:12px;color:var(--dim)}
.comb .defer{text-align:right;font-size:10.5px;color:var(--dim);letter-spacing:.04em;line-height:1.5}.comb .defer b{color:var(--cyan);opacity:.75}
.comb .stay{text-align:right;line-height:1.15}
.comb .stay .sk{font-size:18px;font-weight:700;color:var(--ink-bright);font-variant-numeric:tabular-nums}
.comb .stay .sc{font-size:15px;font-weight:700;color:var(--cyan);text-shadow:0 0 10px rgba(57,224,255,.4);margin-top:2px}
.comb .stay .sl{font-size:9px;letter-spacing:.18em;color:var(--dim);margin-top:3px}
.chips{display:flex;gap:6px;flex-wrap:wrap;align-items:center;margin-bottom:10px}
.chips button{font:11px var(--mono);letter-spacing:.05em;color:var(--dim);background:transparent;border:1px solid var(--line);border-radius:4px;padding:5px 10px;cursor:pointer}
.chips button[aria-pressed="true"]{color:var(--cyan);border-color:var(--cyan);box-shadow:var(--glow)}
.chips button:focus-visible{outline:2px solid var(--cyan);outline-offset:2px}
.chips .csp{flex:1}
.chart{width:100%;height:132px;display:block}
.chart .gl{stroke:var(--grid);stroke-width:1}
.chart polyline{fill:none;vector-effect:non-scaling-stroke}
.chart .ctot{stroke:var(--cyan);stroke-width:2}
.chart .cl1{stroke:var(--green);stroke-width:1;opacity:.7}
.chart .cl2{stroke:var(--amber);stroke-width:1;opacity:.7}
.chfoot{display:flex;justify-content:space-between;gap:8px;margin-top:7px;font-size:10px;color:var(--dim);letter-spacing:.06em}
.chfoot b{color:var(--ink);font-weight:600}
.fl{display:block;font-size:10px;letter-spacing:.14em;text-transform:uppercase;color:var(--dim);margin:13px 0 5px}
.fl .mut2{color:var(--cyan);opacity:.8;letter-spacing:0;float:right}
.in{width:100%;font:14px var(--mono);color:var(--ink-bright);background:rgba(0,0,0,.35);border:1px solid var(--line);border-radius:4px;padding:9px 10px;outline:none}
.in:focus{border-color:var(--cyan);box-shadow:var(--glow)}
.seg{display:flex;gap:6px}
.seg button{flex:1;font:12px var(--mono);color:var(--dim);background:transparent;border:1px solid var(--line);border-radius:4px;padding:9px;cursor:pointer}
.seg button[aria-pressed="true"]{color:var(--cyan);border-color:var(--cyan);box-shadow:var(--glow)}
.rng{width:100%;accent-color:var(--cyan);margin:2px 0}
.trow{display:grid;grid-template-columns:1fr auto 1fr 1.1fr auto;gap:6px;align-items:center;margin-bottom:6px}
.trow.terr .in,.trow.terr select{border-color:var(--red)}
.trow .arr{color:var(--dim)}
.trow .ts{padding:8px 4px}
.tdel{width:30px;padding:8px 0;color:var(--red);background:transparent;border:1px solid rgba(255,80,105,.4);border-radius:4px;font:12px var(--mono);cursor:pointer}
.tadd{width:100%;margin-top:2px;font:11px var(--mono);letter-spacing:.06em;color:var(--amber);background:transparent;border:1px dashed rgba(255,180,84,.5);border-radius:4px;padding:8px;cursor:pointer}
.tcov{height:10px;margin-top:9px;border:1px solid var(--line);border-radius:2px;background:var(--line)}
.tfl{display:flex;justify-content:space-between;font-size:9px;color:var(--dim);margin-top:2px}
.btns2{display:flex;gap:8px;margin-top:15px}
.btns2 button{flex:1;font:12px var(--mono);letter-spacing:.05em;border-radius:5px;padding:11px;cursor:pointer;border:1px solid}
.btns2 button:disabled{opacity:.35;cursor:default}
.btns2 .save{color:#03060d;background:var(--cyan);border-color:var(--cyan);font-weight:700}
.btns2 .warn2{color:var(--amber);background:transparent;border-color:rgba(255,180,84,.5)}
.seg button:focus-visible,.btns2 button:focus-visible{outline:2px solid var(--cyan);outline-offset:2px}
.fmsg{margin-top:9px;font-size:11px;color:var(--green);min-height:14px;letter-spacing:.04em}
.wsec{border-top:1px solid var(--line);margin-top:16px;padding-top:14px}
.pwrap{position:relative}.pwrap .in{padding-right:66px}
.eye2{position:absolute;right:6px;top:6px;bottom:6px;width:auto;padding:0 12px;background:#1a2432;border:1px solid var(--line);border-radius:5px;color:var(--dim);font:11px var(--mono);cursor:pointer}
.wifi{width:100%;margin-top:13px;font:12px var(--mono);letter-spacing:.05em;border-radius:5px;padding:11px;cursor:pointer;color:var(--amber);background:transparent;border:1px solid rgba(255,180,84,.5)}
.wifi:focus-visible,.eye2:focus-visible{outline:2px solid var(--cyan);outline-offset:2px}
summary{cursor:pointer;list-style:none}summary::-webkit-details-marker{display:none}summary::marker{content:""}
.cfgp>summary{margin-bottom:0}
.cfgp>summary:focus-visible,.sub>summary:focus-visible{outline:2px solid var(--cyan);outline-offset:-2px}
.chev{margin-left:auto;color:var(--dim);font-size:11px;transition:transform .2s}
details[open]>summary .chev{transform:rotate(180deg)}
.cfgbody{padding-top:12px}
.sub{border:1px solid var(--line);border-radius:4px;margin-top:10px;background:rgba(0,0,0,.18)}
.cfgbody>.sub:first-child{margin-top:0}
.shead{display:flex;align-items:center;gap:8px;font-size:11px;letter-spacing:.16em;text-transform:uppercase;color:var(--ink);padding:12px 13px}
.sbody{padding:2px 13px 14px}
.factory{width:100%;font:12px var(--mono);letter-spacing:.05em;border-radius:5px;padding:11px;cursor:pointer;color:var(--red);background:transparent;border:1px solid rgba(255,80,105,.5)}
.factory:focus-visible{outline:2px solid var(--cyan);outline-offset:2px}
.hint{font-size:10.5px;color:var(--dim);margin-top:8px;line-height:1.4}
footer{margin-top:14px;border-top:1px solid var(--line);padding-top:10px;font-size:10px;color:var(--dim);
letter-spacing:.1em;display:flex;gap:14px;flex-wrap:wrap;text-transform:uppercase}
footer .ok{color:var(--green)}footer a{color:var(--dim);text-decoration:none}
@media (prefers-reduced-motion:reduce){.fx.flick,.chip .dot,.led.red,.banner.crit .glyph{animation:none}}
nav{display:flex;flex-wrap:wrap;gap:2px;border-bottom:1px solid var(--line);margin:0 0 13px}
nav button{background:none;border:none;color:var(--dim);font:11px var(--mono);letter-spacing:.13em;text-transform:uppercase;padding:9px 13px;cursor:pointer;border-bottom:2px solid transparent}
nav button[aria-selected=true]{color:var(--cyan);border-bottom-color:var(--cyan);text-shadow:0 0 8px rgba(57,224,255,.4)}
nav button:focus-visible{outline:2px solid var(--cyan);outline-offset:-2px}
.tab{display:none}.tab.on{display:block}
.ugwrap{overflow-x:auto;margin-top:8px}
.ugt{width:100%;border-collapse:collapse;font-size:12px}
.ugt th,.ugt td{text-align:left;padding:5px 6px;border-bottom:1px solid var(--line);white-space:nowrap}
.ugt th{color:var(--dim);font-weight:600}.ugt td:nth-child(3),.ugt td:nth-child(4){text-align:right;font-variant-numeric:tabular-nums}
.ugt .ugr{color:var(--dim);font-size:10.5px}.ugt tr.ugund td{color:var(--dim);font-style:italic}
.ugtot{margin-top:10px;font-size:13px;color:var(--ink-bright)}.ugtot b{color:var(--green)}
#ug-invoice{display:none}
@media print{
  body{background:#fff!important}
  body>*{display:none!important}
  #ug-invoice{display:block!important;position:absolute;left:0;top:0;width:100%;padding:24px;
    color:#000;background:#fff;font:13px/1.5 -apple-system,Segoe UI,Roboto,sans-serif}
  #ug-invoice h2{margin:0 0 2px;font-size:20px}#ug-invoice .sub{color:#555;margin-bottom:16px}
  #ug-invoice table{width:100%;border-collapse:collapse;margin-top:8px}
  #ug-invoice th,#ug-invoice td{border-bottom:1px solid #ccc;padding:6px 8px;text-align:left}
  #ug-invoice td:nth-child(2),#ug-invoice td:nth-child(3){text-align:right}
  #ug-invoice tfoot td{font-weight:700;border-top:2px solid #000;border-bottom:none}
  #ug-invoice .foot{margin-top:20px;color:#666;font-size:11px}
}
</style>
<div class="fx vig"></div><div class="fx scan"></div><div class="fx flick"></div>
<div class="wrap">
<header>
<div class="brand">EXCELSIOR <b>SHORE POWER</b><span class="sub" id="brand-sub">HUGHES WATCHDOG &middot; WHOLE-PEDESTAL</span></div>
<div class="spacer"></div>
<div class="sig" id="sig" title="link signal"><i></i><i></i><i></i><i></i></div>
<div class="chip idle" id="chip"><span class="dot"></span><span id="chip-txt">&hellip;</span></div>
</header>
<div class="ob" id="ob">Full browser: <a id="ob-a" href="/"><span id="ob-h">this address</span></a> &middot; iOS: tap Cancel &rarr; Use Without Internet</div>
<nav id="nav" role="tablist">
<button role="tab" data-tab="t-status" aria-selected="true">Status</button>
<button role="tab" data-tab="t-hist" aria-selected="false">History</button>
<button role="tab" data-tab="t-usage" aria-selected="false">Usage</button>
<button role="tab" data-tab="t-stays" aria-selected="false">Stays</button>
<button role="tab" data-tab="t-alerts" aria-selected="false">Alerts</button>
<button role="tab" data-tab="t-net" aria-selected="false">Network</button>
<button role="tab" data-tab="t-sys" aria-selected="false">System</button>
</nav>
<div class="tab on" id="t-status">
<div class="banner idle" id="banner"><div class="glyph" id="ban-glyph">&#9723;</div>
<div class="btxt"><div class="bstate" id="ban-state">CONNECTING</div><div class="bsub" id="ban-sub">Reading the bridge&hellip;</div></div></div>
<div class="legs">
<section class="panel leg" id="leg1"><div class="phead"><span class="led off" id="l1-led"></span>Line 1<span class="id" id="l1-svc2">50 A LEG</span></div>
<div id="l1-body"><div class="amp"><span class="n" id="l1-a">--</span><span class="u">A</span></div>
<div class="of">of <span id="l1-svc">50</span> A &middot; <span id="l1-head">--</span></div>
<div class="gauge"><span class="zone"></span><span class="fill" id="l1-fill" style="width:0%"></span></div>
<div class="vrow"><div class="v"><span class="vn" id="l1-v">--</span><span class="vu">V</span></div><span class="tag idle" id="l1-tag">--</span></div>
<div class="kw"><span>POWER</span><b><span id="l1-kw">--</span> kW</b></div></div>
<div class="nodata" id="l1-nd" style="display:none">no data</div></section>
<section class="panel leg" id="leg2"><div class="phead"><span class="led off" id="l2-led"></span>Line 2<span class="id" id="l2-svc2">50 A LEG</span></div>
<div id="l2-body"><div class="amp"><span class="n" id="l2-a">--</span><span class="u">A</span></div>
<div class="of">of <span id="l2-svc">50</span> A &middot; <span id="l2-head">--</span></div>
<div class="gauge"><span class="zone"></span><span class="fill" id="l2-fill" style="width:0%"></span></div>
<div class="vrow"><div class="v"><span class="vn" id="l2-v">--</span><span class="vu">V</span></div><span class="tag idle" id="l2-tag">--</span></div>
<div class="kw"><span>POWER</span><b><span id="l2-kw">--</span> kW</b></div></div>
<div class="nodata" id="l2-nd" style="display:none">no data</div></section>
</div>
<section class="panel"><div class="phead"><span class="led off" id="c-led"></span>Combined draw<span class="id">BOTH LEGS</span></div>
<div class="comb"><div class="cn"><span class="n" id="c-kw">--</span><span class="u">kW now</span></div>
<div class="stay"><div class="sk"><span id="stay-kwh">--</span> kWh</div><div class="sc" id="stay-cost">$--</div><div class="sl">THIS STAY</div></div></div></section>
</div>
<div class="tab" id="t-hist">
<section class="panel" id="hist"><div class="phead"><span class="led off" id="h-led"></span>History<span class="id" id="ch-max">--</span></div>
<div class="chips"><button data-r="5">5m</button><button data-r="10">10m</button><button data-r="30" aria-pressed="true">30m</button><span class="csp"></span><button data-m="w" aria-pressed="true">W</button><button data-m="a">A</button></div>
<svg class="chart" viewBox="0 0 300 120" preserveAspectRatio="none"><line class="gl" x1="0" y1="30" x2="300" y2="30"/><line class="gl" x1="0" y1="60" x2="300" y2="60"/><line class="gl" x1="0" y1="90" x2="300" y2="90"/><polyline id="ch-l1" class="cl1" points=""/><polyline id="ch-l2" class="cl2" points=""/><polyline id="ch-tot" class="ctot" points=""/></svg>
<div class="chfoot"><span id="ch-x0">-30m</span><span><b id="ch-pk">PK --</b> &middot; <span id="ch-avg">AVG --</span></span><span>now</span></div></section>
</div>
<div class="tab" id="t-usage">
<section class="panel"><div class="phead"><span class="led off"></span>Daily usage &amp; billing<span class="id" id="ug-clk">--</span></div>
<div class="hint">Per-day energy from the box's 30-day ledger. Tick days to total them, then Print for an invoice.</div>
<div class="ugwrap"><table class="ugt"><thead><tr><th></th><th>Date</th><th>kWh</th><th>Cost</th><th>Rates (&cent;/kWh)</th></tr></thead><tbody id="ug-body"></tbody></table></div>
<div class="ugtot" id="ug-tot">Loading&hellip;</div>
<label class="fl">Bill to (shown on the invoice)</label>
<input id="ug-label" class="in" type="text" maxlength="40" placeholder="e.g. Site 42 - Jane Doe">
<div class="btns2"><button type="button" class="save" id="ug-all">Select all dated</button><button type="button" class="save" id="ug-print">Print invoice</button></div>
</section></div>
<div class="tab" id="t-stays">
<section class="panel"><div class="phead"><span class="led off"></span>Guest stays<span class="id" id="st-cur">--</span></div>
<div class="hint">Bookend a guest session &mdash; the receipt uses the meter reading at check-in and check-out. One open stay at a time.</div>
<label class="fl">Guest name</label><input id="st-guest" class="in" type="text" maxlength="23" placeholder="e.g. Jane Doe">
<label class="fl">Site / bay</label><input id="st-site" class="in" type="text" maxlength="15" placeholder="e.g. Site 42">
<div class="btns2"><button type="button" class="save" id="st-open">Check in</button><button type="button" class="warn2" id="st-close">Check out</button></div>
<div class="fmsg" id="st-msg"></div>
<div class="ugwrap"><table class="ugt"><thead><tr><th>#</th><th>Guest</th><th>kWh</th><th>Receipt</th></tr></thead><tbody id="st-body"></tbody></table></div>
<div class="hint"><a href="/stays.csv" style="color:var(--cyan)">Download all stays (CSV)</a> for your bookkeeping.</div>
</section></div>
<div class="tab" id="t-alerts">
<section class="panel"><div class="phead"><span class="led off"></span>Notifications<span class="id">NTFY</span></div>
<label class="fl"><input type="checkbox" id="n-on"> Enable phone alerts</label>
<div class="hint">Free push notifications via the ntfy app. Off by default.</div>
<label class="fl">Your alert channel</label>
<div class="pwrap"><input id="n-topic" class="in" type="text" readonly><button type="button" class="eye2" id="n-regen">new</button></div>
<a id="n-link" class="in" href="#" target="_blank" rel="noopener" style="display:block;text-align:center">open in the ntfy app</a>
<div class="hint">Install the free <b>ntfy</b> app, then tap the link above (or scan the QR on the device screen) to subscribe.</div>
<label class="fl">Alert me about</label>
<label class="fl"><input type="checkbox" class="n-a" data-b="0"> Shore power lost</label>
<label class="fl"><input type="checkbox" class="n-a" data-b="1"> Shore power restored</label>
<label class="fl"><input type="checkbox" class="n-a" data-b="2"> Running on battery</label>
<label class="fl"><input type="checkbox" class="n-a" data-b="3"> Voltage fault (E1/E2)</label>
<label class="fl"><input type="checkbox" class="n-a" data-b="4"> High current (near trip)</label>
<label class="fl"><input type="checkbox" class="n-a" data-b="5"> Watchdog link lost</label>
<label class="fl"><input type="checkbox" class="n-a" data-b="6"> Watchdog reconnected</label>
<label class="fl">Daily &ldquo;all good&rdquo; heartbeat</label>
<select id="n-hb" class="in"></select>
<div class="hint">Optional once-a-day push at this hour, so silence means a real problem, not a dead channel. Needs the clock set.</div>
<div class="btns2"><button type="button" class="save" id="n-save">Save alerts</button><button type="button" class="warn2" id="n-test">Send test</button></div>
<div class="hint">Tap <b>Send test</b>, then check your phone buzzed &mdash; that proves the whole chain works.</div>
<div class="fmsg" id="n-dark" style="color:var(--amber)"></div>
<div class="fmsg" id="n-msg"></div>
</section></div>
<div class="tab" id="t-net">
<section class="panel"><div class="phead"><span class="led off"></span>Change Wi-Fi<span class="id">NETWORK</span></div>
<label class="fl">Wi-Fi network</label>
<input id="f-ssid" class="in" type="text" autocomplete="off" placeholder="network name (SSID)">
<label class="fl">Wi-Fi password</label>
<div class="pwrap"><input id="f-wpass" class="in" type="password" autocomplete="off" placeholder="blank = open network"><button type="button" class="eye2" id="f-weye" onclick="wpw()">show</button></div>
<button type="button" class="wifi" id="f-wifi">Update Wi-Fi &amp; reconnect</button>
<div class="fmsg" id="w-msg" style="color:var(--amber)"></div>
</section></div>
<div class="tab" id="t-sys">
<section class="panel"><div class="phead"><span class="led off"></span>Device &amp; pricing</div>
<label class="fl">Name this bay / Watchdog</label>
<input id="f-nick" class="in" type="text" maxlength="24" placeholder="e.g. Site 42 &middot; Our Coach">
<label class="fl">Electricity rate</label>
<div class="seg" id="r-mode"><button type="button" data-m="0" aria-pressed="true">Flat</button><button type="button" data-m="1">Time of day</button></div>
<div id="r-flat"><label class="fl">$ / kWh</label><input id="f-rate" class="in" type="number" step="0.01" min="0" inputmode="decimal" placeholder="0.28"></div>
<div id="r-tod" style="display:none">
<div id="tiers"></div>
<button type="button" class="tadd" id="t-add">+ Add tier</button>
<div class="tcov" id="t-cov"></div>
<div class="tfl"><span>12A</span><span>6A</span><span>12P</span><span>6P</span><span>12A</span></div>
<div class="fmsg" id="t-msg" style="min-height:14px"></div></div>
<label class="fl">Service rating</label>
<div class="seg" id="f-svc"><button type="button" data-s="30">30 A / leg</button><button type="button" data-s="50">50 A / leg</button></div>
<label class="fl">Screen brightness <span class="mut2" id="f-brv">80%</span></label>
<input id="f-bright" class="rng" type="range" min="10" max="100" step="5" value="80">
<label class="fl">Time zone <span class="mut2" id="f-tznow"></span></label>
<select id="f-tz" class="in"><option value="0">Not set</option><option value="1">Eastern</option><option value="2">Central</option><option value="3">Mountain</option><option value="4">Arizona (no DST)</option><option value="5">Pacific</option><option value="6">Alaska</option><option value="7">Hawaii</option></select>
<button type="button" class="save" id="f-setclock">Set clock from this phone</button>
<div class="mut2">No internet at the box? This sets its clock from your phone (using the zone above).</div>
<div class="btns2"><button type="button" class="save" id="f-save">Save settings</button><button type="button" class="warn2" id="f-reset">Reset running total</button></div>
<div class="fmsg" id="f-msg"></div>
</section>
<section class="panel"><div class="phead"><span class="led off"></span>Firmware<span class="id" id="o-fw">--</span></div>
<label class="fl">Update channel (manifest URL)</label>
<input id="o-url" class="in" type="text" maxlength="158" autocomplete="off" placeholder="https://firmware.flensor.com/hughes.json">
<div class="btns2"><button type="button" class="save" id="o-check">Check &amp; update</button><button type="button" class="warn2" id="o-saveurl">Save URL</button></div>
<div class="hint">Checks the manifest; if a newer version is published it downloads, verifies, and reboots. Watch progress on the device screen.</div>
<div class="fmsg" id="o-msg"></div>
</section>
<section class="panel"><div class="phead"><span class="led off"></span>Support</div>
<button type="button" class="save" id="s-diag">Send diagnostics to support</button>
<div class="hint">Sends this bridge's recent logs, status &amp; event history to the maker so they can help. Only send this if you've asked for support.</div>
<div class="fmsg" id="s-msg"></div>
</section>
<section class="panel"><div class="phead"><span class="led off"></span>System</div>
<button type="button" class="factory" id="f-factory">Factory reset &mdash; erase all settings</button>
<div class="hint">Wipes Wi-Fi, Watchdog, and every setting back to first-boot, then reboots into the setup portal.</div>
<div class="fmsg" id="x-msg" style="color:var(--red)"></div>
</section></div>
<footer><span>BRIDGE <span class="ok" id="f-h">--</span></span><span>hughes-esp32</span><span>auto-refresh 2s</span><span><a href="/status">/status</a></span></footer>
</div>
<section id="ug-invoice"></section>
<script>
function el(i){return document.getElementById(i);}
try{el('f-h').textContent=el('ob-h').textContent=location.host;el('ob-a').href=location.origin+'/';el('ob').style.display=(location.hostname==='192.168.4.1')?'':'none';}catch(e){}
document.getElementById('nav').addEventListener('click',function(e){var b=e.target.closest('button');if(!b)return;document.querySelectorAll('#nav button').forEach(function(x){x.setAttribute('aria-selected',x===b);});document.querySelectorAll('.tab').forEach(function(s){s.classList.toggle('on',s.id===b.dataset.tab);});if(b.dataset.tab==='t-usage')loadUsage();if(b.dataset.tab==='t-stays')loadStays();});
/* ---- Guest stays tab (v2.1 2.1) ---- */
function smsg(m,ok){var e=el('st-msg');e.style.color=(ok===false)?'var(--red)':'var(--green)';e.textContent=m;if(ok!==false)setTimeout(function(){if(e.textContent===m)e.textContent='';},2600);}/* U9: errors persist until the next action */
function loadStays(){fetch('/stays',{cache:'no-store'}).then(function(r){return r.json();}).then(function(d){
  var L=d.stays||[],open=null;for(var i=0;i<L.length;i++){if(L[i].open){open=L[i];break;}}
  el('st-cur').textContent=open?('open: '+(open.guest||('#'+open.id))):'none open';
  el('st-open').disabled=!!open;el('st-close').disabled=!open;
  var b=el('st-body');b.innerHTML='';
  L.forEach(function(s){var tr=document.createElement('tr');
    var nm=(s.guest||'')+(s.site?(' · '+s.site):'')||('#'+s.id);
    tr.innerHTML='<td>'+s.id+'</td><td>'+nm+(s.open?' <b style="color:var(--amber)">OPEN</b>':(s.auto?' <span style="color:var(--dim)">(auto)</span>':''))+'</td>'+
      '<td>'+s.kwh.toFixed(3)+'</td><td>'+(s.open?'&mdash;':'<a href="/receipt?id='+s.id+'" target="_blank" style="color:var(--cyan)">view</a>')+'</td>';
    b.appendChild(tr);});
  if(!L.length)b.innerHTML='<tr><td colspan="4" style="color:var(--dim)">No stays yet. Enter a guest and Check in.</td></tr>';
}).catch(function(){el('st-cur').textContent='(load failed)';});}
el('st-open').addEventListener('click',function(){
  var g=encodeURIComponent(el('st-guest').value),s=encodeURIComponent(el('st-site').value);
  fetch('/stay/open?guest='+g+'&site='+s,{method:'POST'}).then(function(r){return r.text();}).then(function(t){var ok=/opened/.test(t);smsg(t.trim(),ok);if(ok){el('st-guest').value='';el('st-site').value='';}loadStays();}).catch(function(){smsg('Check-in failed',false);});});/* U9: only clear inputs on success */
el('st-close').addEventListener('click',function(){
  if(!confirm('Check out the current guest? This closes the stay and fixes its meter reading.'))return;
  fetch('/stay/close',{method:'POST'}).then(function(r){return r.text();}).then(function(t){smsg(t.trim(),/closed/.test(t));loadStays();}).catch(function(){smsg('Check-out failed',false);});});
/* ---- Usage & billing tab (v2.1 1.3): per-day table off /usage + printable invoice ---- */
var UG=null;
function ugDate(n){if(!n)return 'undated';var s=''+n;return s.slice(0,4)+'-'+s.slice(4,6)+'-'+s.slice(6,8);}
function loadUsage(){fetch('/usage',{cache:'no-store'}).then(function(r){return r.json();}).then(function(d){UG=d;ugRender();}).catch(function(){el('ug-tot').textContent='(could not load usage)';});}
function ugRender(){
  var body=el('ug-body');body.innerHTML='';
  el('ug-clk').textContent=(UG.clk_prov==2?'clock synced':(UG.date_real?'clock set':'clock/zone not set'));
  var rows=(UG.days||[]).slice().reverse();  // newest first
  rows.forEach(function(day,idx){
    var rates=(day.rates||[]).map(function(r){return r.c+'¢ '+r.kwh.toFixed(2);}).join(' · ');
    var dated=!!day.date;
    var tr=document.createElement('tr');if(!dated)tr.className='ugund';
    tr.innerHTML='<td><input type="checkbox" class="ugck"'+(dated?'':' disabled')+'></td>'+
      '<td>'+ugDate(day.date)+'</td><td>'+day.kwh.toFixed(3)+'</td><td>$'+(day.cost_c/100).toFixed(2)+'</td>'+
      '<td class="ugr">'+(rates||'-')+'</td>';
    tr.dataset.kwh=day.kwh;tr.dataset.cost=day.cost_c;tr.dataset.date=day.date;
    body.appendChild(tr);
  });
  body.querySelectorAll('.ugck').forEach(function(c){c.addEventListener('change',ugTotal);});
  ugTotal();
}
function ugChecked(){return [].slice.call(el('ug-body').querySelectorAll('tr')).filter(function(tr){var c=tr.querySelector('.ugck');return c&&c.checked;});}
function ugTotal(){
  var sel=ugChecked(),k=0,c=0;sel.forEach(function(tr){k+=parseFloat(tr.dataset.kwh);c+=parseFloat(tr.dataset.cost);});
  el('ug-tot').innerHTML=sel.length?('Selected '+sel.length+' day'+(sel.length>1?'s':'')+': <b>'+k.toFixed(3)+' kWh &middot; $'+(c/100).toFixed(2)+'</b>'):'Tick days to total them.';
}
el('ug-all').addEventListener('click',function(){var cks=el('ug-body').querySelectorAll('.ugck:not([disabled])');var any=[].some.call(cks,function(c){return !c.checked;});cks.forEach(function(c){c.checked=any;});ugTotal();});
el('ug-print').addEventListener('click',function(){
  var sel=ugChecked();if(!sel.length){el('ug-tot').innerHTML='Tick at least one day first.';return;}
  var k=0,c=0,rowsHtml='';sel.forEach(function(tr){k+=parseFloat(tr.dataset.kwh);c+=parseFloat(tr.dataset.cost);
    rowsHtml+='<tr><td>'+ugDate(tr.dataset.date)+'</td><td>'+parseFloat(tr.dataset.kwh).toFixed(3)+'</td><td>$'+(parseFloat(tr.dataset.cost)/100).toFixed(2)+'</td></tr>';});
  var name=(el('f-nick')&&el('f-nick').value)||document.title;var lbl=el('ug-label').value.trim();
  var now=new Date().toLocaleString();
  el('ug-invoice').innerHTML='<h2>'+name+' &mdash; usage statement</h2><div class="sub">'+(lbl?('<b>'+lbl+'</b> &middot; '):'')+'generated '+now+'</div>'+
    '<table><thead><tr><th>Date</th><th>kWh</th><th>Cost</th></tr></thead><tbody>'+rowsHtml+'</tbody>'+
    '<tfoot><tr><td>Total ('+sel.length+' day'+(sel.length===1?'':'s')+')</td><td>'+k.toFixed(3)+'</td><td>$'+(c/100).toFixed(2)+'</td></tr></tfoot></table>'+
    '<div class="foot">Metered by an Excelsior Shore Power Bridge. Energy figures are the device meter; rates are per the configured plan at time of use.</div>';
  window.print();
});
function vBand(v){return (v<104||v>132)?'crit':(v<108||v>128)?'warn':'ok';}
function vTag(v){var b=vBand(v);return b==='ok'?['ok','OK']:b==='warn'?['warn',v<108?'LOW':'HIGH']:['crit',v<104?'UNDER':'OVER'];}
function aBand(a,svc){var f=a/svc;return f>0.9?'hot':f>0.7?'warm':'';}
var GLY={shore_ok:'&#10003;',l1_fault:'&#9888;',l2_fault:'&#9888;',no_signal:'&#9723;',no_watchdog:'&#9881;',on_battery:'&#9211;',app_linked:'&#128241;',alerts_dark:'&#128246;',link_weak:'&#128246;',clock_unset:'&#9200;',tz_unset:'&#9200;'};
var CLS={shore_ok:'ok',l1_fault:'crit',l2_fault:'crit',no_signal:'idle',no_watchdog:'idle',on_battery:'warn',app_linked:'link',alerts_dark:'warn',link_weak:'warn',clock_unset:'warn',tz_unset:'warn'};
function sub(s){var st=s.state;
if(st==='alerts_dark')return 'Phone alerts are ON but the bridge can&rsquo;t reach the alert server &mdash; check its internet.';
if(st==='link_weak')return 'Watchdog is connected but the Bluetooth signal is weak &mdash; try Mounting Mode / move it closer.';
if(st==='clock_unset')return 'Set the clock (from a phone, or wait for Wi-Fi) so daily usage &amp; billing can be dated.';
if(st==='tz_unset')return 'Clock is set &mdash; now pick the time zone (here in Settings, or the device TIME screen) so daily usage &amp; billing can be dated.';
if(st==='shore_ok')return 'Both legs nominal &middot; monitoring the pedestal';
if(st==='app_linked')return 'Link released so the Hughes phone app can connect.';
if(st==='no_watchdog')return 'No Watchdog selected &mdash; set one on the device (SETTINGS &rarr; DEVICES).';
if(st==='no_signal')return 'Can&rsquo;t see the pedestal &mdash; the Bluetooth link to the Watchdog is down.';
if(st==='on_battery')return 'Shore power lost &mdash; the bridge is running on its internal backup.';
if(st==='l1_fault'||st==='l2_fault'){var i=(st==='l1_fault')?0:1,n=i+1,L=s.legs&&s.legs[i];
if(L&&typeof L.volts==='number'&&(L.volts<104||L.volts>132))return 'Line '+n+' at '+L.volts.toFixed(0)+' V &mdash; outside the 104&ndash;132 V range. The Watchdog cut power to protect the rig.';
return 'Line '+n+' is reporting a fault. The Watchdog cut power to protect the rig.';}
return '';}
function sig(s){if(!s.connected)return [0,'crit'];var r=s.rssi,b=r>=-70?4:r>=-78?3:r>=-85?2:r>=-92?1:0,c=r>=-75?'':r>=-85?'warn':'crit';return [b,c];}
function leg(n,L,svc){var body=el('l'+n+'-body'),nd=el('l'+n+'-nd'),led=el('l'+n+'-led'),card=el('leg'+n);
var fresh=L&&typeof L.age_s==='number'&&L.age_s<5;
if(!fresh){body.style.display='none';nd.style.display='block';led.className='led off';card.className='panel leg';return;}
body.style.display='';nd.style.display='none';
var a=L.amps,v=L.volts,w=L.watts,err=L.err||0,ab=aBand(a,svc),vb=vBand(v);
card.className='panel leg'+(ab?(' '+ab):'');
el('l'+n+'-a').textContent=a.toFixed(1);el('l'+n+'-v').textContent=v.toFixed(1);el('l'+n+'-kw').textContent=(w/1000).toFixed(2);
el('l'+n+'-svc').textContent=svc;el('l'+n+'-svc2').textContent=svc+' A LEG';
el('l'+n+'-head').textContent=err?'tripped':(Math.max(0,svc-a).toFixed(0)+' A headroom');
var fill=el('l'+n+'-fill');fill.style.width=Math.min(100,a/svc*100)+'%';fill.className='fill'+(ab?(' '+ab):'');
var t=vTag(v),tag=el('l'+n+'-tag');tag.className='tag '+t[0];tag.textContent=t[1];
led.className='led'+((err||vb==='crit')?' red':((ab==='hot'||vb==='warn')?' amber':''));}
var uFail=0;
function u(){fetch('/status',{cache:'no-store'}).then(function(r){return r.json();}).then(function(s){
uFail=0;document.body.classList.remove('stale');   /* U14: back in contact */
var nd=el('n-dark');if(nd)nd.textContent=s.alerts_dark?'⚠ Can’t reach the alert server right now — pushes may not arrive.':'';
var st=s.state||'shore_ok',svc=s.svc_amps||50;
var chip=el('chip');chip.className='chip '+(CLS[st]||'idle');el('chip-txt').textContent=s.status_word||'--';
var sg=sig(s),ss=el('sig');ss.className='sig'+(sg[1]?(' '+sg[1]):'');
var bars=ss.children;for(var i=0;i<bars.length;i++)bars[i].className=(i<sg[0])?'on':'';
var ban=el('banner');ban.className='banner '+(CLS[st]||'idle');
el('ban-glyph').innerHTML=GLY[st]||'&#9723;';el('ban-state').textContent=s.status_word||'--';el('ban-sub').innerHTML=sub(s);
var L1=s.legs&&s.legs[0],L2=s.legs&&s.legs[1];leg(1,L1,svc);leg(2,L2,svc);
var f1=L1&&typeof L1.age_s==='number'&&L1.age_s<5,f2=L2&&typeof L2.age_s==='number'&&L2.age_s<5;
var cw=((f1?L1.watts:0)+(f2?L2.watts:0))/1000;
el('c-kw').textContent=(f1||f2)?cw.toFixed(2):'--';el('c-led').className='led'+((f1||f2)?'':' off');
if(typeof s.stay_kwh==='number'){el('stay-kwh').textContent=s.stay_kwh.toFixed(1);var sc=s.rate_tiers?((s.stay_cost_c||0)/100):(s.stay_kwh*(s.rate_c||0)/100);el('stay-cost').textContent='$'+sc.toFixed(2);}
el('brand-sub').textContent=(s.nick&&s.nick.length)?s.nick:'HUGHES WATCHDOG · WHOLE-PEDESTAL';
if(el('f-tznow'))el('f-tznow').textContent=(s.local_time&&s.local_time!=='--')?s.local_time:'(no clock yet)';
if(!cfgInit){cfgInit=true;if(s.nick)el('f-nick').value=s.nick;if(typeof s.rate_c==='number')el('f-rate').value=(s.rate_c/100).toFixed(2);if(typeof s.bright==='number'){el('f-bright').value=s.bright;el('f-brv').textContent=s.bright+'%';}setSeg(s.svc_amps||50);if(typeof s.tz==='number')el('f-tz').value=s.tz;if(s.wifi_ssid)el('f-ssid').value=s.wifi_ssid;if(s.fw)el('o-fw').textContent='v'+s.fw;if(s.ota_url)el('o-url').value=s.ota_url;if(typeof s.rate_tiers==='string'){if(s.rate_tiers)s.rate_tiers.split(',').forEach(function(t){t=t.split('-');addRow(+t[0],+t[1]%24,+t[2]);});setMode(s.rate_tiers?1:0);}}
if(!ncfg&&typeof s.ntfy_on!=='undefined'){ncfg=true;el('n-on').checked=!!s.ntfy_on;var tp=s.ntfy_topic||'';el('n-topic').value=tp;el('n-link').href='https://ntfy.sh/'+tp;el('n-link').textContent=tp?('open '+tp+' in ntfy'):'open in the ntfy app';var nm=(typeof s.ntfy_mask==='number')?s.ntfy_mask:127,cs=document.querySelectorAll('.n-a');for(var k=0;k<cs.length;k++)cs[k].checked=!!(nm&(1<<(+cs[k].dataset.b)));var hbs=el('n-hb');if(hbs&&!hbs.options.length){var ho='<option value="-1">Off</option>';for(var hh=0;hh<24;hh++)ho+='<option value="'+hh+'">'+((hh%12||12)+(hh<12?' AM':' PM'))+'</option>';hbs.innerHTML=ho;}if(hbs)hbs.value=(typeof s.hb_hour==='number'?s.hb_hour:-1);}
}).catch(function(e){   /* U14: don't silently keep stale numbers -- after ~3 misses, say so */
if(++uFail>=3){document.body.classList.add('stale');var ban=el('banner');if(ban){ban.className='banner warn';el('ban-glyph').innerHTML='&#8635;';el('ban-state').textContent='RECONNECTING…';el('ban-sub').textContent='Lost contact with the bridge — retrying. Readings below may be stale.';}}});}
u();setInterval(u,2000);
var HR=30,HM='w';
function fmtV(v,mode){return (mode==='a')?(v.toFixed(1)+' A'):(v>=1000?(v/1000).toFixed(2)+' kW':Math.round(v)+' W');}
function drawChart(h){var n=h.n,l1=h.l1,l2=h.l2,W=300,H=120,pad=3,tot=[],mx=0,i;
for(i=0;i<n;i++){var a=l1[i],b=l2[i];if(a==null&&b==null){tot.push(null);continue;}var t=(a||0)+(b||0);tot.push(t);if(t>mx)mx=t;}
if(mx<=0)mx=1;
function pl(arr){var s='';for(var j=0;j<n;j++){if(arr[j]==null)continue;var x=pad+(W-2*pad)*(n>1?j/(n-1):0);var y=H-pad-(H-2*pad)*(arr[j]/mx);s+=(s?' ':'')+x.toFixed(1)+','+y.toFixed(1);}return s;}
el('ch-tot').setAttribute('points',pl(tot));el('ch-l1').setAttribute('points',pl(l1));el('ch-l2').setAttribute('points',pl(l2));
var sum=0,c=0,pk=0;for(i=0;i<n;i++){if(tot[i]==null)continue;sum+=tot[i];c++;if(tot[i]>pk)pk=tot[i];}
el('ch-max').textContent=fmtV(mx,h.mode);el('ch-pk').textContent='PK '+fmtV(pk,h.mode);el('ch-avg').textContent='AVG '+fmtV(c?sum/c:0,h.mode);
el('ch-x0').textContent='-'+h.range_min+'m';el('h-led').className='led'+(c?'':' off');}
function hist(){fetch('/history?range='+HR+'&mode='+HM,{cache:'no-store'}).then(function(r){return r.json();}).then(drawChart).catch(function(e){});}
(function(){var box=document.querySelector('.chips');box.addEventListener('click',function(e){var b=e.target.closest('button');if(!b)return;
if(b.dataset.r){HR=+b.dataset.r;[].forEach.call(box.querySelectorAll('[data-r]'),function(x){x.setAttribute('aria-pressed',x===b);});}
else if(b.dataset.m){HM=b.dataset.m;[].forEach.call(box.querySelectorAll('[data-m]'),function(x){x.setAttribute('aria-pressed',x===b);});}
hist();});})();
hist();setInterval(hist,10000);
var cfgInit=false,ncfg=false,FSVC=50;
function setSeg(v){FSVC=v;var bs=document.querySelectorAll('#f-svc button');for(var i=0;i<bs.length;i++)bs[i].setAttribute('aria-pressed',+bs[i].dataset.s===v);}
function fmsg(m,ok){var e=el('f-msg');e.style.color=(ok===false)?'var(--red)':'var(--green)';e.textContent=m;if(ok!==false)setTimeout(function(){if(e.textContent===m)e.textContent='';},2600);}/* U9: errors persist until the next action */
el('f-svc').addEventListener('click',function(e){var b=e.target.closest('button');if(b)setSeg(+b.dataset.s);});
el('f-bright').addEventListener('input',function(){el('f-brv').textContent=this.value+'%';});
el('f-save').addEventListener('click',function(){
var nick=encodeURIComponent(el('f-nick').value.trim());
var d=parseFloat(el('f-rate').value),rc=isNaN(d)?null:Math.round(d*100);
var q='/config?nick='+nick+'&svc='+FSVC+'&bright='+el('f-bright').value+'&tz='+el('f-tz').value;
if(RMODE===1){if(!tval()){fmsg('Fix the rate schedule first',false);return;}q+='&tiers='+trows().map(function(r){return r.s+'-'+(r.e===0?24:r.e)+'-'+r.c;}).join(',');}
else{q+='&tiers=flat';if(rc!=null)q+='&rate_c='+rc;}
fetch(q,{method:'POST'}).then(function(r){return r.text();}).then(function(){fmsg('Saved ✓');u();}).catch(function(){fmsg('Save failed',false);});});
el('f-setclock').addEventListener('click',function(){
var ep=Math.floor(Date.now()/1000);
fetch('/settime?epoch='+ep+'&tz='+el('f-tz').value,{method:'POST'}).then(function(r){return r.text();}).then(function(t){var warn=/zone/i.test(t);fmsg(t.trim()||'Clock set ✓',warn?false:undefined);u();}).catch(function(){fmsg('Set clock failed',false);});});/* U11: show the server's own reply; if it warns the zone is unset, keep it up (don't flash green + auto-clear) */
el('f-reset').addEventListener('click',function(){
if(!confirm('Reset the running kWh total to zero for a fresh site? This does NOT delete any guest stays or receipts.'))return;/* U15: reassure */
fetch('/reset_stay',{method:'POST'}).then(function(r){return r.text();}).then(function(){fmsg('Running total reset ✓');u();}).catch(function(){fmsg('Reset failed',false);});});
el('s-diag').addEventListener('click',function(){
var e=el('s-msg');e.style.color='var(--dim)';e.textContent='sending...';
fetch('/support_bundle',{method:'POST'}).then(function(r){return r.text();}).then(function(t){var ok=/sending/i.test(t);e.style.color=ok?'var(--green)':'var(--amber)';e.textContent=ok?'Diagnostics sent to support ✓':t.trim();setTimeout(function(){e.textContent='';},5000);}).catch(function(){e.style.color='var(--red)';e.textContent='Send failed';});});
/* ---- time-of-day rate tier editor (end-hour 0 = midnight; validated against 24h coverage) ---- */
var RMODE=0,tiersInit=false,HL=[];for(var _h=0;_h<24;_h++)HL.push((_h%12||12)+(_h<12?'A':'P'));
function hsel(v){var s='<select class="in ts">';for(var h=0;h<24;h++)s+='<option value="'+h+'"'+(h===v?' selected':'')+'>'+HL[h]+'</option>';return s+'</select>';}
function addRow(s,e,c){var d=document.createElement('div');d.className='trow';d.innerHTML=hsel(s)+'<span class="arr">&#8594;</span>'+hsel(e)+'<input class="in tr8" type="number" step="0.01" min="0" inputmode="decimal" value="'+(c!=null?(c/100).toFixed(2):'')+'"><button type="button" class="tdel">&#215;</button>';el('tiers').appendChild(d);}
function trows(){return [].map.call(document.querySelectorAll('#tiers .trow'),function(r){var s=r.querySelectorAll('.ts');return{el:r,s:+s[0].value,e:+s[1].value,c:Math.round(parseFloat(r.querySelector('.tr8').value)*100)};});}
function trng(hs){var s=hs[0];if(hs.length<24)while(hs.indexOf((s+23)%24)>=0)s=(s+23)%24;var e=s;while(hs.indexOf(e)>=0&&hs.length<24)e=(e+1)%24;return HL[s]+' &#8594; '+HL[e%24];}
function tval(){var R=trows(),cnt=[],i,h,ok=true,msg='';for(h=0;h<24;h++)cnt[h]=0;
for(i=0;i<R.length;i++){var r=R[i];r.el.classList.remove('terr');
if(r.s===r.e){r.el.classList.add('terr');ok=false;msg='zero-length tier &#8212; use Flat for one all-day rate';continue;}
if(isNaN(r.c)||r.c<0||r.c>999){r.el.classList.add('terr');ok=false;msg='enter a $/kWh for each tier';continue;}
for(h=r.s;h!==r.e;h=(h+1)%24)cnt[h]++;}
var g=[],o=[],seg=[];for(h=0;h<24;h++){if(!cnt[h])g.push(h);if(cnt[h]>1)o.push(h);var col=cnt[h]>1?'#ff5069':!cnt[h]?'rgba(255,80,105,.18)':'rgba(57,224,255,.55)';seg.push(col+' '+(h/24*100).toFixed(2)+'% '+((h+1)/24*100).toFixed(2)+'%');}
el('t-cov').style.background='linear-gradient(90deg,'+seg.join(',')+')';
if(ok&&o.length){ok=false;msg='OVERLAP: '+trng(o)+' twice';for(i=0;i<R.length;i++)for(h=R[i].s;h!==R[i].e;h=(h+1)%24)if(cnt[h]>1){R[i].el.classList.add('terr');break;}}
if(ok&&g.length){ok=false;msg='GAP: '+trng(g)+' uncovered';}
var m=el('t-msg');m.style.color=ok?'var(--green)':'var(--red)';m.innerHTML=(ok&&R.length)?'covers all 24h &#10003;':msg;return ok&&R.length>0;}
function setMode(m){RMODE=m;[].forEach.call(el('r-mode').children,function(x){x.setAttribute('aria-pressed',+x.dataset.m===m);});el('r-flat').style.display=m?'none':'';el('r-tod').style.display=m?'':'none';if(m)tval();}
el('r-mode').addEventListener('click',function(e){var b=e.target.closest('button');if(b)setMode(+b.dataset.m);});
el('t-add').addEventListener('click',function(){var R=trows(),n=R.length;addRow(n?R[n-1].e:0,n?R[0].s:0,null);tval();});
el('tiers').addEventListener('click',function(e){if(e.target.className==='tdel'){e.target.parentNode.remove();tval();}});
el('tiers').addEventListener('change',tval);el('tiers').addEventListener('input',tval);
function wpw(){var p=el('f-wpass'),b=el('f-weye');if(p.type=='password'){p.type='text';b.textContent='hide'}else{p.type='password';b.textContent='show'}}
function wmsg(m){el('w-msg').textContent=m;}
el('f-wifi').addEventListener('click',function(){
var ss=el('f-ssid').value.trim();
if(!ss){wmsg('Enter a network name.');return;}
if(!confirm('Switch Wi-Fi to \"'+ss+'\" and reconnect? The device reboots — rejoin that network to see it again.'))return;
wmsg('Saving — reconnecting to '+ss+'…');
var done=function(){wmsg('Saved. Reconnecting to '+ss+' — rejoin that Wi-Fi. Wrong password? it hosts Hughes-Bridge-XXXX so you can retry.');};
fetch('/wifi?ssid='+encodeURIComponent(ss)+'&pass='+encodeURIComponent(el('f-wpass').value),{method:'POST'}).then(done).catch(done);});
el('f-factory').addEventListener('click',function(){
if(!confirm('FACTORY RESET — erase Wi-Fi, Watchdog, and ALL settings? This cannot be undone.'))return;
if(!confirm('Really erase everything? The device reboots into first-time setup.'))return;
var d=function(){el('x-msg').textContent='Factory reset — rejoin Hughes-Setup-XXXX to set it up again.';};
el('x-msg').textContent='Erasing — rebooting…';
fetch('/factory_reset',{method:'POST'}).then(d).catch(d);});
function nmsg(m,ok){var e=el('n-msg');e.style.color=(ok===false)?'var(--red)':'var(--green)';e.textContent=m;setTimeout(function(){if(e.textContent===m)e.textContent='';},2600);}
function nmask(){var m=0,cs=document.querySelectorAll('.n-a');for(var i=0;i<cs.length;i++)if(cs[i].checked)m|=(1<<(+cs[i].dataset.b));return m;}
el('n-save').addEventListener('click',function(){
fetch('/ntfy?on='+(el('n-on').checked?1:0)+'&mask='+nmask()+'&hb='+el('n-hb').value,{method:'POST'}).then(function(r){return r.text();}).then(function(){nmsg('Saved ✓');ncfg=false;u();}).catch(function(){nmsg('Save failed',false);});});
el('n-test').addEventListener('click',function(){
nmsg('Sending…');fetch('/ntfy?test=1',{method:'POST'}).then(function(r){return r.text();}).then(function(){nmsg('Test sent — check your phone ✓');}).catch(function(){nmsg('Test failed',false);});});
el('n-regen').addEventListener('click',function(){
if(!confirm('Generate a NEW alert channel? You will need to re-subscribe on your phone.'))return;
fetch('/ntfy?regen=1',{method:'POST'}).then(function(r){return r.text();}).then(function(){nmsg('New channel ✓');ncfg=false;u();}).catch(function(){nmsg('Failed',false);});});
function omsg(m,ok){var e=el('o-msg');e.style.color=(ok===false)?'var(--red)':(ok==='w')?'var(--cyan)':'var(--green)';e.textContent=m;}
el('o-check').addEventListener('click',function(){
if(!confirm('Check for a firmware update? If a newer version is published, the device downloads it and reboots.'))return;
omsg('Checking\\u2026','w');
fetch('/ota?key=hughes',{method:'POST'}).then(function(r){return r.text();}).then(function(){omsg('Checking on the device \\u2014 watch its screen. If already current, nothing happens.');}).catch(function(){omsg('Request failed',false);});});
el('o-saveurl').addEventListener('click',function(){var uu=el('o-url').value.trim();if(!uu)return;
fetch('/ota_url?key=hughes&url='+encodeURIComponent(uu),{method:'POST'}).then(function(r){return r.text();}).then(function(){omsg('Saved \\u2713');}).catch(function(){omsg('Save failed',false);});});
</script>
)HTML";
  server.sendHeader("Cache-Control", "no-store");
  server.send_P(200, "text/html", DASH);           // stream from flash -- no ~27KB String (was OOMing in AP mode -> white page)
}

// Not-found: in AP mode, redirect everything to the dashboard so the phone's connectivity
// check trips the "sign in to network" captive sheet. In HOME/STA mode, a plain 404.
void handleNotFound() {
  if (g_apServing) {
    server.sendHeader("Location", "http://" + WiFi.softAPIP().toString() + "/", true);
    server.send(302, "text/plain", "");
  } else {
    server.send(404, "text/plain", "not found\n");
  }
}

// Switch network mode live: POST /netmode?key=KEY&mode=home|ap|off
void handleNetMode() {
  if (!apiAuthed()) { server.send(401, "text/plain", "unauthorized: add ?key=<API KEY on INFO>\n"); return; }
  String m = server.arg("mode");
  int prevMode = g_cfg.netMode;
  g_cfg.netMode = (m == "ap") ? 1 : (m == "off") ? 2 : 0;
  configPersist();
  if (g_cfg.netMode != prevMode) evtLog(EV_NET_MODE, (int16_t)g_cfg.netMode);   // 0=home 1=ap 2=off; only on an actual change (form re-POSTs shouldn't churn the log)
  server.send(200, "text/plain", String("netmode -> ") + (g_cfg.netMode==1?"ap (hosting own AP -- rebooting)":g_cfg.netMode==2?"off":"home") + "\n");
  if (g_cfg.netMode == 1) { evtSave(); g_reboot = true; }   // AP: persist the event, then reboot for a clean radio init (the 30s flush is skipped once g_reboot is set)
  else netApply();
}

// Deep-sleep power-off (tap the screen to wake). POST /sleep?key=KEY
void handleSleep() {
  if (!apiAuthed()) { server.send(401, "text/plain", "unauthorized: add ?key=<API KEY on INFO>\n"); return; }
  server.send(200, "text/plain", "sleeping -- tap the screen to wake\n");
  delay(200);
  enterSleep();
}

// Robustly wipe the esp_wifi driver's OWN stored STA creds (nvs.net80211). The old
// WiFi.disconnect(true,true) returned ESP_FAIL when we weren't connected (e.g. while hosting our own
// AP) and left prior/vendor creds behind -- the boot reconcile then re-adopted them and never reached
// the portal (hit on a fresh board that shipped with a vendor '..._AP' cached). esp_wifi_restore()
// resets the persistent Wi-Fi settings -> clears the stored SSID/pass regardless of connection state.
static void wifiEraseDriverCreds() {
  WiFi.mode(WIFI_STA);        // ensure the STA iface exists + the driver is inited before the restore
  esp_wifi_restore();         // wipe the driver's stored STA config from NVS
}

// Wipe the saved config and drop back into the captive portal. The escape hatch if
// the Watchdog was mis-picked (wrong WiFi self-heals: a failed STA join re-enters
// provisioning on its own).
// Clear WiFi + Watchdog config and reboot into the setup portal. Keeps the "seeded"
// sentinel set so config.h can't re-seed past the wipe. Shared by /reprovision + SETTINGS.
static void doReprovision() {
  g_prefs.begin("hughes", false);
  g_prefs.remove("ssid"); g_prefs.remove("pass");
  g_prefs.remove("mac");  g_prefs.remove("name");
  g_prefs.putInt("net", 0);            // reset to HOME so we land in the setup portal, not our own AP.
                                       // (Without this, reprovisioning a netMode=1/skip-WiFi unit just
                                       //  bounced back to AP mode -> stuck, unable to re-enter Wi-Fi.)
  g_prefs.putBool("seeded", true);
  g_prefs.putBool("forceprov", true);   // next boot: force the portal, don't re-adopt any driver-cached creds
  g_prefs.end();
  stayCostReset();                      // FINDING#1: a full wipe must also clear the GLOBAL staycost/lastck NVS keys, else a newly-adopted unit inherits the old baseline
  wifiEraseDriverCreds();               // also erase the esp_wifi driver's OWN stored STA creds, else the boot-time
                                        // reconcile (setup) would re-adopt the old network and never reach the portal
  logf("reprovision: wiped Wi-Fi/Watchdog -> setup portal");
  g_reboot = true;
}

void handleReprovision() {
  if (!apiAuthed()) { server.send(401, "text/plain", "unauthorized: add ?key=<API KEY on the device INFO screen>\n"); return; }
  doReprovision();
  server.send(200, "text/plain", "config cleared -- rebooting into setup portal\n");
}

// Factory reset: wipe ALL persisted config back to first-boot defaults, then reboot to the setup
// portal. More than reprovision (which keeps rate/brightness/svc/nick/API-key/baselines) -- this
// clears the whole "hughes" NVS namespace + the per-MAC STAY baselines ("hkwh"). Keeps the "seeded"
// sentinel so config.h can't re-seed past the wipe (a re-flash re-applies the dev seed if wanted).
static void doFactoryReset() {
  { Preferences p; p.begin("hughes", false); p.clear(); p.putBool("seeded", true); p.putBool("forceprov", true); p.end(); }  // all config + anti-reseed + force portal next boot
  { Preferences p; p.begin("hkwh",   false); p.clear(); p.end(); }                              // per-MAC STAY baselines
  wipeHistory();                                                                                // + SD graph history (all units)
  wifiEraseDriverCreds();               // robustly erase the driver's stored STA creds (belt-and-suspenders with the NVS clear + forceprov)
  logf("FACTORY RESET: wiped all config + SD history -> setup portal");
  g_reboot = true;
}

// POST /factory_reset -- wipe everything from the dashboard. Unlocked like the other web writes.
void handleFactoryReset() {
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "text/plain", "factory reset -- rebooting to setup portal\n");
  delay(400);
  doFactoryReset(); ESP.restart();
}

// GET /log[?n=N] -- the in-RAM breadcrumb ring as plain text, oldest first. Diagnostic pull over Wi-Fi
// (no cable), companion to the serial console. Content read without the lock -> at worst a torn newest line.
void handleLog() {
  int head, cnt;
  portENTER_CRITICAL(&g_logMux); head = g_logHead; cnt = g_logCount; portEXIT_CRITICAL(&g_logMux);
  int want = server.hasArg("n") ? server.arg("n").toInt() : cnt;
  if (want < 1 || want > cnt) want = cnt;
  String out; out.reserve(want * 48);
  int start = (head - want + LOG_LINES) % LOG_LINES;
  for (int i = 0; i < want; i++) out += String(g_log[(start + i) % LOG_LINES]) + "\n";
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "text/plain", cnt ? out : "(log empty)\n");
}

// Persistent crash-log ring (RTC memory) -- survives panic/WDT/brownout/deep-sleep reboots, unlike /log.
// After a crash, the last line before the newest "==== BOOT ... ====" banner is where it died.
void handleDmesg() {
  String out; dmesgToString(out);
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "text/plain", out.length() ? out : "(dmesg empty)\n");
}

// Queue the reverse-engineered odometer reset (bleTask does the actual BLE write).
void handleResetOdometer() {
  if (!apiAuthed()) { server.send(401, "text/plain", "unauthorized: add ?key=<API KEY on the device INFO screen>\n"); return; }
  if (!g_connected) { server.send(409, "text/plain", "not connected to the Watchdog\n"); return; }
  if (stayOpenIdx() >= 0) { server.send(409, "text/plain", "a stay is open -- close it first; zeroing the odometer now would wipe the guest's bill\n"); return; }   // R6
  g_wantReset = true;
  evtLog(EV_RESET_ODO, 0);                           // audit: lifetime meter zeroed (web)
  server.send(200, "text/plain", "queued: writing \"RESEt\" to fff5 (zeroes the lifetime kWh odometer)\n");
}

// ---------------------------------------------------------------- provisioning HTTP
// Redirect every stray request to the portal root -- this is what pops the phone's
// "sign in to network" sheet.
void handleCaptive() {
  server.sendHeader("Location", String("http://") + WiFi.softAPIP().toString() + "/", true);
  server.send(302, "text/plain", "");
}

// Live BLE scan for the Watchdog picker -- same candidate filter as scan mode.
void handleScanBle() {
  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->setActiveScan(true);
  NimBLEScanResults res = scan->getResults(5000, false);

  String j = "["; bool first = true;
  for (int i = 0; i < res.getCount(); i++) {
    const NimBLEAdvertisedDevice* d = res.getDevice(i);
    String nm = d->haveName() ? String(d->getName().c_str()) : String();
    if (!nameMatches(nm) && !d->isAdvertisingService(SVC)) continue;
    if (!first) j += ","; first = false;
    j += "{\"mac\":\"" + String(d->getAddress().toString().c_str()) + "\",";
    j += "\"name\":\"" + nm + "\",";
    j += "\"rssi\":" + String(d->getRSSI()) + ",";
    j += "\"type\":\"" + String(addrTypeStr(d->getAddress().getType())) + "\"}";
  }
  j += "]";
  scan->clearResults();
  server.send(200, "application/json", j);
}

// The setup page: WiFi (datalist from a live scan) + a Watchdog picker fed by /scan_ble.
void handlePortalRoot() {
  int n = WiFi.scanNetworks();
  String opts;
  for (int i = 0; i < n && i < 24; i++)
    opts += "<option value=\"" + WiFi.SSID(i) + "\">" + WiFi.SSID(i) + " (" + String(WiFi.RSSI(i)) + ")</option>";
  WiFi.scanDelete();

  String page =
    "<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'><meta charset=utf-8>"
    "<title>Hughes Bridge setup</title><style>"
    ":root{--bg:#03060d;--panel:rgba(8,18,30,.62);--cyan:#39e0ff;--dim:#4d7288;--line:rgba(57,224,255,.22);"
    "--ink:#cfe9f5;--ink-b:#eafcff;--mono:'SFMono-Regular',ui-monospace,'Roboto Mono',Consolas,monospace}"
    "*{box-sizing:border-box}body{margin:0;min-height:100vh;color:var(--ink);font-family:var(--mono);font-size:14px;"
    "background:radial-gradient(120% 90% at 50% -10%,#0a1830,#050b16 45%,var(--bg))}"
    ".w{max-width:480px;margin:0 auto;padding:20px 16px 28px}"
    "header{display:flex;align-items:center;gap:10px;padding:12px 14px;margin-bottom:12px;border:1px solid var(--line);"
    "border-radius:4px;background:linear-gradient(180deg,rgba(57,224,255,.06),transparent)}"
    ".brand{font-weight:700;font-size:15px;color:var(--ink-b)}.brand b{color:var(--cyan)}"
    ".dot{width:9px;height:9px;border-radius:50%;background:#c264ff;box-shadow:0 0 9px #c264ff;flex:none}"
    "nav{display:flex;flex-wrap:wrap;gap:2px;border-bottom:1px solid var(--line);margin:0 0 13px}"
    "nav button{background:none;border:none;color:var(--dim);font:11px var(--mono);letter-spacing:.13em;"
    "text-transform:uppercase;padding:9px 13px;cursor:pointer;border-bottom:2px solid transparent}"
    "nav button[aria-selected=true]{color:var(--cyan);border-bottom-color:var(--cyan)}"
    ".tab{display:none}.tab.on{display:block}"
    ".panel{background:var(--panel);border:1px solid var(--line);border-radius:4px;padding:14px 15px}"
    "label{display:block;margin:12px 0 4px;font-size:10px;letter-spacing:.14em;text-transform:uppercase;color:var(--dim)}"
    "input{width:100%;font:14px var(--mono);color:var(--ink-b);background:rgba(0,0,0,.35);border:1px solid var(--line);"
    "border-radius:4px;padding:10px;outline:none}input:focus{border-color:var(--cyan)}"
    ".bt{width:100%;margin-top:13px;font:12px var(--mono);letter-spacing:.05em;border-radius:5px;padding:12px;cursor:pointer;"
    "border:1px solid var(--cyan);background:var(--cyan);color:#03060d;font-weight:700}"
    ".bt.sec{background:transparent;color:var(--cyan)}"
    ".cand{display:flex;gap:10px;align-items:center;padding:10px;border:1px solid var(--line);border-radius:5px;margin-top:8px}"
    ".cand input{width:auto}.mut{color:var(--dim);font-size:11px}"
    ".pw{position:relative}.pw input{padding-right:66px}"
    ".eye{position:absolute;right:6px;top:6px;bottom:6px;width:auto;margin:0;padding:0 12px;background:#1a2432;"
    "border:1px solid var(--line);border-radius:5px;font:11px var(--mono);color:var(--dim);cursor:pointer}"
    ".hint{font-size:10.5px;color:var(--dim);margin-top:10px;line-height:1.5}</style>"
    "<div class=w>"
    "<header><span class=dot></span><div class=brand>EXCELSIOR <b>SHORE POWER</b> &middot; SETUP</div></header>"
    "<nav id=nav role=tablist>"
    "<button type=button data-tab=t-wifi aria-selected=true>Wi-Fi</button>"
    "<button type=button data-tab=t-wd aria-selected=false>Watchdog</button>"
    "<button type=button data-tab=t-opt aria-selected=false>Options</button></nav>"
    "<form id=f>"
    "<input type=hidden name=netmode id=netmode value=home>"
    "<div class='tab on' id=t-wifi><section class=panel>"
    "<label>Wi-Fi network</label>"
    "<input name=ssid id=ssid list=nets autocomplete=off value='" + g_cfg.wifiSsid + "'>"
    "<datalist id=nets>" + opts + "</datalist>"
    "<label>Wi-Fi password</label>"
    "<div class=pw><input name=pass id=pass type=password autocomplete=off>"
    "<button type=button id=pweye class=eye onclick=togglePw()>show</button></div>"
    "<button type=button class=bt onclick=save()>Save &amp; connect</button>"
    "</section></div>"
    "<div class=tab id=t-wd><section class=panel>"
    "<label>Watchdog (optional)</label>"
    "<button type=button class='bt sec' onclick=scan()>Scan for Watchdogs</button>"
    "<div id=ble class=mut style='margin-top:8px'>No scan yet.</div>"
    "<input type=hidden name=mac  id=mac  value='" + g_cfg.hughesMac  + "'>"
    "<input type=hidden name=name id=name value='" + g_cfg.hughesName + "'>"
    "<div class=hint>Optional now &mdash; you can pick the Watchdog later on the device (SETTINGS &rarr; DEVICES) or the dashboard.</div>"
    "</section></div>"
    "<div class=tab id=t-opt><section class=panel>"
    "<label>No Wi-Fi here?</label>"
    "<button type=button class='bt sec' onclick=skip()>Skip &mdash; host its own network</button>"
    "<div class=hint>The bridge hosts its own <b>Hughes-Bridge-XXXX</b> Wi-Fi; connect your phone to it to see the dashboard. You can add Wi-Fi later.</div>"
    "</section></div>"
    "</form><p id=msg class=mut></p></div><script>"
    "function E(i){return document.getElementById(i)}"
    "E('nav').addEventListener('click',function(e){var b=e.target.closest('button');if(!b)return;"
    "document.querySelectorAll('#nav button').forEach(function(x){x.setAttribute('aria-selected',x===b)});"
    "document.querySelectorAll('.tab').forEach(function(s){s.classList.toggle('on',s.id===b.dataset.tab)})});"
    "function togglePw(){var p=E('pass'),b=E('pweye');if(p.type=='password'){p.type='text';b.textContent='hide'}else{p.type='password';b.textContent='show'}}"
    "async function scan(){let b=E('ble');b.textContent='Scanning\\u2026';"
    "try{let j=await(await fetch('/scan_ble')).json();"
    "if(!j.length){b.textContent='No Watchdogs found. Power on the unit and retry.';return}"
    "b.innerHTML='';j.forEach(d=>{let l=document.createElement('label');l.className='cand';"
    "l.innerHTML=`<input type=radio name=pick><span>${d.name||'(unnamed)'}<br>"
    "<span class=mut>${d.mac} \\u00b7 ${d.rssi} dBm \\u00b7 ${d.type}</span></span>`;"
    "l.querySelector('input').onchange=()=>{E('mac').value=d.mac;E('name').value=d.name};"
    "b.appendChild(l)})}catch(e){b.textContent='Scan failed: '+e}}"
    "async function post(){await fetch('/save',{method:'POST',body:new URLSearchParams(new FormData(E('f')))});}"
    "async function save(){if(!E('ssid').value){E('msg').textContent='Enter a Wi-Fi network, or use the Options tab to skip.';return}"
    "E('netmode').value='home';E('msg').textContent='Saving\\u2026';await post();"
    "E('msg').textContent='Saved. Rebooting \\u2014 reconnect to your normal Wi-Fi.'}"
    "async function skip(){E('netmode').value='ap';E('msg').textContent='Saving\\u2026';await post();"
    "E('msg').textContent='Saved. Rebooting \\u2014 it hosts its own Hughes-Bridge network (no Wi-Fi).'}"
    "</script>";
  server.send(200, "text/html", page);
}

void handleSave() {
  if (!server.arg("mac").equalsIgnoreCase(g_cfg.hughesMac)) swapBillingReset();   // FINDING#1/F6: the portal can point at a different Watchdog -> same R5/M10 hygiene as adopt (case-insensitive, only on a real change)
  g_cfg.netMode    = (server.arg("netmode") == "ap") ? 1 : 0;   // HOME (join) or AP (host own)
  g_cfg.wifiSsid   = server.arg("ssid");
  g_cfg.wifiPass   = server.arg("pass");
  g_cfg.hughesMac  = server.arg("mac");
  g_cfg.hughesName = server.arg("name");
  configPersist();
  seedFreshBaselineIfNew();                            // finding#1: parity with adopt -> a newly-picked unit's kwhBase is captured fresh, not inherited from the old unit
  Serial.printf("config saved: net=%d ssid='%s' mac='%s' name='%s'\n",
                g_cfg.netMode, g_cfg.wifiSsid.c_str(), g_cfg.hughesMac.c_str(), g_cfg.hughesName.c_str());
  server.send(200, "text/plain", "saved");
  g_reboot = true;
}

// ---------------------------------------------------------------- reactor UI (3b: animated)
// Portrait 240x320. Two stacked cells, one per leg. The CORES animate at ~20fps from a
// reusable off-screen sprite (flicker-free); the NUMBERS are a separate 1Hz text layer.
static const int CELL1_Y = 28, CELL2_Y = 138;   // 108px cells; frees a labelled TOTAL block below

static uint16_t blend565(uint16_t a, uint16_t b, uint8_t t) {   // t: 0->a, 255->b
  int ar=(a>>11)&0x1F, ag=(a>>5)&0x3F, ab=a&0x1F;
  int br=(b>>11)&0x1F, bg=(b>>5)&0x3F, bb=b&0x1F;
  return (uint16_t)((((ar+(br-ar)*t/255)&0x1F)<<11) | (((ag+(bg-ag)*t/255)&0x3F)<<5) | ((ab+(bb-ab)*t/255)&0x1F));
}

// "Heating up" core colour (option B): cold blue when idle -> cyan -> white-hot as the
// leg loads, flipping to alarm red only near its trip. Ramp is relative to the unit's
// per-leg service rating (svcAmps, 50 or 30 A). Each leg reads as a temperature.
static uint16_t heatColor(float amps) {
  float f = amps/(float)g_cfg.svcAmps; if (f < 0) f = 0; if (f > 1) f = 1;
  if (f >= 0.9f) return C_RED;                       // near the breaker -> danger red
  float t = f/0.9f;                                  // 0..1 across the heat ramp
  uint16_t cold = C565(0x22,0x55,0xff);              // deep blue (idle)
  uint16_t warm = C565(0x39,0xe0,0xff);              // cyan
  uint16_t hot  = C565(0xff,0xff,0xff);              // white-hot
  return (t < 0.5f) ? blend565(cold, warm, (uint8_t)(t*510))
                    : blend565(warm, hot,  (uint8_t)((t-0.5f)*510));
}

// rounded-rect button with a centred label
static void btn(const Rect& r, const char* label, uint16_t border, uint16_t tc, uint8_t font, int dy = 0) {
  tft.fillRect(r.x+1, r.y+1, r.w-2, r.h-2, C_BG);     // clear interior so an in-place relabel doesn't overlap
  tft.drawRoundRect(r.x, r.y, r.w, r.h, 5, border);
  tft.setTextDatum(MC_DATUM); tft.setTextColor(tc, C_BG);
  tft.drawString(label, r.x + r.w/2, r.y + r.h/2 + dy, font);   // dy nudges the label (font metrics can sit a hair high)
  tft.setTextDatum(TL_DATUM);
}
// Filled (solid) variant: reads as a status pill / the CURRENT selection rather than a tappable outline.
static void btnFill(const Rect& r, const char* label, uint16_t fill, uint16_t tc, uint8_t font) {
  tft.fillRoundRect(r.x, r.y, r.w, r.h, 5, fill);
  tft.setTextDatum(MC_DATUM); tft.setTextColor(tc, fill);
  tft.drawString(label, r.x + r.w/2, r.y + r.h/2, font);
  tft.setTextDatum(TL_DATUM);
}

// bottom nav bar, on every screen; the current screen is highlighted
static const Rect R_NAV_R={0,292,80,28}, R_NAV_G={80,292,80,28}, R_NAV_S={160,292,80,28};  // taller touch targets (easier to tap in the case)
static const Rect R_HEALTH={136,0,104,26};          // reactor header cluster: tap swaps bars <-> dBm number
static const Rect R_REACTOR_STATE={0,0,132,26};     // U6: reactor header LEFT (status word) -> tap jumps to the relevant fix page
static void drawNav() {
  tft.drawFastHLine(0, 297, 240, C_LINE);
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(g_scr==SCR_REACTOR  ? C_CYAN : C_DIM, C_BG); tft.drawString("REACTOR",  40, 303, 2);
  tft.setTextColor(g_scr==SCR_GRAPHS   ? C_CYAN : C_DIM, C_BG); tft.drawString("GRAPHS",  120, 303, 2);
  tft.setTextColor(g_scr==SCR_SETTINGS ? C_CYAN : C_DIM, C_BG); tft.drawString("SETTINGS",202, 303, 2);
  tft.setTextDatum(TL_DATUM);
}

// One reusable ~88x88 core sprite, composed off-screen and pushed to each leg cell.
// A load-reactive plasma core: gauge ring (load meter, breathing fill) + a 3-layer
// breathing gradient glow + orbiting particles. Idle = dim/slow; loaded = bright/fast;
// a leg past ~90% of its 50A trip throbs faster as an alarm.
static TFT_eSprite g_coreSpr = TFT_eSprite(&tft);
static const int   SPR = 88, SC = 44;               // sprite size + local centre
static float g_pulsePh[2]  = {0,0};                 // per-leg breathing phase (radians, accumulated)
static float g_orbitAng[2] = {0,0};                 // per-leg orbit angle (degrees, accumulated)

static void drawCoreSprite(int cellY, const Leg& L, bool fresh, int idx) {
  TFT_eSprite& s = g_coreSpr;
  s.fillSprite(C_BG);
  float amps = (L.valid && fresh) ? L.amps : 0.0f;
  float frac = amps/(float)g_cfg.svcAmps; if (frac < 0) frac = 0; if (frac > 1) frac = 1;
  uint16_t band = (L.valid && fresh) ? heatColor(amps) : C_DIM;
  float rate  = 0.05f + 0.20f*frac + (frac > 0.9f ? 0.20f : 0.0f);   // faster near trip
  // ACCUMULATE the phase per frame -- never frame*rate, or a load (rate) change would
  // jump the whole phase and the animation would snap back to the start.
  g_pulsePh[idx] = fmodf(g_pulsePh[idx] + rate, 6.2831853f);
  float pulse = 0.5f + 0.5f*sinf(g_pulsePh[idx]);

  // load-meter gauge ring: fills to % of the service rating; the filled arc breathes
  float aFill = 210.0f - 240.0f*frac;
  for (float a = 210; a >= -30.001f; a -= 8) {
    int x1,y1,x2,y2; polar(SC,SC,31,a,x1,y1); polar(SC,SC,40,a,x2,y2);
    bool on = a >= aFill-0.001f;
    s.drawLine(x1,y1,x2,y2, on ? scale565(band,(uint8_t)(150+pulse*100)) : scale565(C_DIM,55));
  }
  s.drawCircle(SC,SC,28, scale565(band,110));                        // containment ring

  // plasma core: 3-layer radial gradient (edge band -> hot centre), whole thing breathing
  uint8_t core = (uint8_t)((70.0f + 150.0f*frac) * (0.55f + 0.45f*pulse));
  s.fillCircle(SC,SC,24, scale565(band, (uint8_t)(core*0.45f)));
  s.fillCircle(SC,SC,16, scale565(band, (uint8_t)(core*0.75f)));
  s.fillCircle(SC,SC, 8, scale565(blend565(band,0xFFFF,150), core)); // hot centre

  // orbiting plasma particles -- angular speed rises with load (accumulated angle)
  if (L.valid && fresh) {
    g_orbitAng[idx] = fmodf(g_orbitAng[idx] + 2.0f + 11.0f*frac, 360.0f);
    for (int i = 0; i < 3; i++) {
      int px,py; polar(SC,SC,34, g_orbitAng[idx] + i*120.0f, px,py);
      s.fillCircle(px,py,2, scale565(band,(uint8_t)(150+pulse*105)));
    }
  }
  s.pushSprite(6, cellY+18);
}

// Numbers layer (1Hz). Padded draws overwrite each field's fixed-width box in one pass
// (no separate fillRect blank -> no flicker). Never touches the core sprite region (x<96).
static void drawNumbers(int cellY, const Leg& L, bool fresh) {
  tft.setTextDatum(TL_DATUM);
  char s[24];
  if (L.valid && fresh) {
    tft.setTextColor(heatColor(L.amps), C_BG);
    tft.setTextPadding(150); int w = tft.drawFloat(L.amps, 1, 104, cellY+18, 6);  // amps headline
    tft.setTextPadding(0);
    tft.setTextColor(C_DIM, C_BG);  tft.drawString("A", 104+w+4, cellY+42, 4);
    tft.setTextColor(C_TEXT, C_BG);
    snprintf(s, sizeof(s), "%.2f kW", L.watts/1000.0f); tft.setTextPadding(150); tft.drawString(s, 104, cellY+66, 4);
    tft.setTextColor(C_DIM, C_BG);
    snprintf(s, sizeof(s), "%.1f V", L.volts);          tft.setTextPadding(120); tft.drawString(s, 104, cellY+92, 2);
    tft.setTextPadding(0);
  } else {
    tft.fillRect(96, cellY+18, 144, 90, C_BG);          // no-data is rare; a plain clear is fine
    tft.setTextColor(C_DIM, C_BG);
    tft.drawString(L.valid ? "stale" : "-- no data --", 104, cellY+50, 2);
  }
}

// ---- battery / power state (GPIO9 = ADC1_CH8, on-board 200k/200k divider). This board has
// NO charge-status or USB-present line to a GPIO (docs/board/power-battery-charging.md), so
// "on battery" is inferred from the cell voltage: a USB-charged cell floats >=~4.05V. ----
static adc_oneshot_unit_handle_t g_adc    = nullptr;
static adc_cali_handle_t         g_adcCal = nullptr;
static bool g_battCalOk = false;
// g_battMv / g_battPct / g_onBattery are declared up top (near g_connected) so handleStatus() can read them.

static void battSetup() {
  adc_oneshot_unit_init_cfg_t ic = {}; ic.unit_id = ADC_UNIT_1;
  if (adc_oneshot_new_unit(&ic, &g_adc) != ESP_OK) { g_adc = nullptr; return; }
  adc_oneshot_chan_cfg_t cc = {}; cc.atten = ADC_ATTEN_DB_12; cc.bitwidth = ADC_BITWIDTH_12;
  adc_oneshot_config_channel(g_adc, ADC_CHANNEL_8, &cc);          // GPIO9
  adc_cali_curve_fitting_config_t cal = {}; cal.unit_id = ADC_UNIT_1; cal.atten = ADC_ATTEN_DB_12; cal.bitwidth = ADC_BITWIDTH_12;
  g_battCalOk = (adc_cali_create_scheme_curve_fitting(&cal, &g_adcCal) == ESP_OK);
}
static void battRead() {
  if (!g_adc) return;
  int raw = 0, mv = 0;
  if (adc_oneshot_read(g_adc, ADC_CHANNEL_8, &raw) != ESP_OK) return;
  if (g_battCalOk) { if (adc_cali_raw_to_voltage(g_adcCal, raw, &mv) != ESP_OK) return; }
  else             mv = raw * 3100 / 4095;                        // uncalibrated fallback (~12dB full-scale)
  g_battMv  = mv * 2;                                             // on-board /2 divider
  int p = (g_battMv - 3300) * 100 / (4200 - 3300);               // rough 3.30V=0% .. 4.20V=100%
  g_battPct = p < 0 ? 0 : (p > 100 ? 100 : p);
  // On-battery is INFERRED (no CHRG/VBUS line reaches a GPIO). A charging or float cell never
  // FALLS -- only a discharging one does -- so trend beats a fixed threshold (a charging cell sits
  // below 4.2V for a while and would false-read "on battery"). Compare to the reading ~60s ago.
  static uint16_t hist[12] = {0}; static uint8_t hpos = 0; static bool hfull = false;
  uint16_t old = hfull ? hist[hpos] : (hpos ? hist[0] : g_battMv);   // hist[hpos] = oldest (~60s ago) when full
  hist[hpos] = g_battMv; hpos = (uint8_t)((hpos + 1) % 12); if (hpos == 0) hfull = true;
  if      (g_battMv < 1000)      g_onBattery = false;           // no cell / bad read
  else if (g_battMv < old - 12)  g_onBattery = true;            // falling under load -> discharging (on battery)
  else if (g_battMv > old + 12)  g_onBattery = false;           // rising -> charging on USB
  else if (g_battMv >= 4100)     g_onBattery = false;           // flat & full -> USB float
  else                           g_onBattery = true;            // flat & not full -> on battery (a USB cell here would be rising)
}

// Single source of truth for the status word, shared by the on-screen header AND the web
// dashboard/`/status` so the screen and the phone can never disagree. Fills `word` (the human
// label) + `color`, returns a stable state code. Priority: app has the link > a leg fault >
// no watchdog chosen > no data/outage > on battery > OK. Takes g_mtx itself -- callers must NOT
// hold it (handleStatus builds its JSON only after releasing the lock).
static const char* statusState(char* word, size_t wlen, uint16_t* color) {
  Leg l1, l2; bool conn; uint32_t lastN, now = millis();
  xSemaphoreTake(g_mtx, portMAX_DELAY); l1=g_leg[0]; l2=g_leg[1]; conn=g_connected; lastN=g_lastNotifyMs; xSemaphoreGive(g_mtx);
  bool f1 = l1.valid && (now-l1.seenMs)<5000, f2 = l2.valid && (now-l2.seenMs)<5000;
  bool silent = conn && (long)(now-lastN) > (long)(STALE_MS/2);
  uint16_t c; const char* st;
  if      (g_released)       { snprintf(word,wlen,"APP LINKED");          c=C_AMBER; st="app_linked"; }
  // Decode known fault codes: E1/E2 = per-leg voltage out of the 104-132 V window (community-confirmed).
  // Show "L{n} VOLTAGE" when the leg's volts are out of range; else the raw "E{err}" (unknown code).
  else if (f1 && l1.err)     { if (l1.volts < 104.0f || l1.volts > 132.0f) snprintf(word,wlen,"L1 VOLTAGE");
                               else snprintf(word,wlen,"L1 FAULT E%u",l1.err); c=C_RED; st="l1_fault"; }
  else if (f2 && l2.err)     { if (l2.volts < 104.0f || l2.volts > 132.0f) snprintf(word,wlen,"L2 VOLTAGE");
                               else snprintf(word,wlen,"L2 FAULT E%u",l2.err); c=C_RED; st="l2_fault"; }
  else if (!g_cfg.hughesMac.length() && !g_cfg.hughesName.length())
                             { snprintf(word,wlen,"NO WATCHDOG");         c=C_AMBER; st="no_watchdog"; }  // none chosen -> setup prompt, not an alarm
  else if (!conn || silent)  { snprintf(word,wlen,"NO SIGNAL");           c=C_RED;   st="no_signal";   }
  else if (g_onBattery)      { snprintf(word,wlen,"ON BATT %d%%",g_battPct); c = g_battPct<25 ? C_RED : C_AMBER; st="on_battery"; }
  // v2.1 1.6 -- low-severity health advisories: shown only when nothing louder is wrong, worst-first, all amber.
  else if (alertsDark())     { snprintf(word,wlen,"ALERTS OFFLINE");     c=C_AMBER; st="alerts_dark"; }   // push ON but no internet
  else if (conn && g_rssi && g_rssi < -85) { snprintf(word,wlen,"WEAK SIGNAL"); c=C_AMBER; st="link_weak"; } // U8: "LINK WEAK" -> "WEAK SIGNAL" (Watchdog barely in range)
  else if (!g_dateReal) {                                                          // U1: split by CAUSE so the fix is actionable (the H:M editor can't set a real date)
    if (g_epochReal) { snprintf(word,wlen,"PICK TIME ZONE"); st="tz_unset"; }      //   clock is real (NTP/phone) but no zone -> just pick the zone
    else             { snprintf(word,wlen,"SET THE CLOCK");   st="clock_unset"; }  //   truly unset -> set the clock from a phone / wait for NTP
    c=C_AMBER;
  }
  else                       { snprintf(word,wlen,"SHORE OK");            c=C_GREEN; st="shore_ok";    }
  if (color) *color = c;
  return st;
}

// Header LEFT status word: normally quiet ("SHORE OK"), loud when something needs attention.
static void drawStatusWord() {
  char w[22]; uint16_t c;
  statusState(w, sizeof(w), &c);
  tft.setTextDatum(TL_DATUM); tft.setTextColor(c, C_BG); tft.setTextPadding(126);
  tft.drawString(w, 6, 6, 2); tft.setTextPadding(0);
}

// static chrome -- drawn once on entry; reactorUpdate() never clears these regions.
static void reactorStatic() {
  tft.fillScreen(C_BG);
  tft.setTextDatum(TL_DATUM);
  // header LEFT (status word) is drawn live by drawStatusWord() in reactorNumbers()
  tft.drawFastHLine(0, 26, 240, C_LINE);
  tft.setTextColor(C_DIM, C_BG);
  tft.drawString("L1", 8, CELL1_Y+2, 2);
  tft.drawString("L2", 8, CELL2_Y+2, 2);
  tft.drawFastHLine(0, 248, 240, C_LINE);           // separates L2 from the TOTAL block
  tft.setTextColor(C_DIM, C_BG);                    // TOTAL-block labels (values redraw at 1Hz)
  tft.drawString("COMBINED", 6, 250, 1);
  tft.drawString("STAY", 132, 250, 1);
  drawNav();                                        // nav bar (highlights the current screen)
}

// ---- outage banner: shore power / Watchdog signal lost. "Notify once, then get out of the way"
// -- a planned unplug for travel must not nag all trip, so: alert once + DISMISS + auto-quiet +
// a POWER OFF shortcut. Reconnect is automatic (plug in at the next site -> it clears itself). ----
static bool     g_outageShow  = false;              // banner visible?
static uint32_t g_outageStart = 0;                  // millis the outage was declared (elapsed + auto-quiet)
static const Rect R_OUT_DISMISS = {14, 228, 100, 44}, R_OUT_OFF = {126, 228, 100, 44};
static void outageStatic() {
  tft.fillScreen(C_BG);
  tft.drawRect(0,0,240,298,C_RED); tft.drawRect(1,1,238,296,C_RED);      // red frame above the nav bar
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(C_RED, C_BG); tft.drawString("SHORE POWER", 120, 46, 4); tft.drawString("LOST", 120, 84, 4);
  tft.setTextColor(C_DIM, C_BG); tft.drawString("no Watchdog data", 120, 128, 2);
  tft.setTextDatum(TL_DATUM);
  btn(R_OUT_DISMISS, "DISMISS",   C_LINE,  C_TEXT,  2);
  btn(R_OUT_OFF,     "POWER OFF", C_AMBER, C_AMBER, 2);
  drawNav();
}
static void outageUpdate() {
  uint32_t s = (millis() - g_lastNotifyMs) / 1000;                       // time since the last real packet
  char v[40];
  if (s < 60) snprintf(v, sizeof(v), "for %lus", (unsigned long)s);
  else        snprintf(v, sizeof(v), "for %lum %02lus", (unsigned long)(s/60), (unsigned long)(s%60));
  tft.setTextDatum(TC_DATUM); tft.setTextColor(C_AMBER, C_BG); tft.setTextPadding(224);
  tft.drawString(v, 120, 150, 4);
  if (g_onBattery) { snprintf(v, sizeof(v), "ON BATTERY %d%%", g_battPct); tft.setTextColor(g_battPct<25?C_RED:C_AMBER, C_BG); }
  else             { snprintf(v, sizeof(v), "check pedestal / breaker");   tft.setTextColor(C_DIM, C_BG); }
  tft.drawString(v, 120, 194, 2);
  tft.setTextPadding(0); tft.setTextDatum(TL_DATUM);
}

// No-Watchdog reactor body: a first-run prompt instead of two dead "-- no data --" cores.
static const Rect R_NOWD_PICK = {30, 176, 180, 52};
static void reactorNoWd() {
  tft.fillScreen(C_BG);
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(C_AMBER, C_BG); tft.drawString("NO WATCHDOG", 120, 66, 4);
  tft.setTextColor(C_DIM, C_BG);   tft.drawString("no unit selected to monitor", 120, 116, 2);
  tft.setTextDatum(TL_DATUM);
  btn(R_NOWD_PICK, "SELECT WATCHDOG", C_CYAN, C_CYAN, 2);
  tft.setTextColor(C_DIM, C_BG); tft.setTextDatum(TC_DATUM);
  tft.drawString("scan + pick a Hughes unit", 120, 244, 1);
  tft.setTextDatum(TL_DATUM);
  drawNav();
}

// Animated cores layer (~20fps): render + push both core sprites.
static void reactorCores() {
  Leg l1, l2; uint32_t now = millis();
  xSemaphoreTake(g_mtx, portMAX_DELAY);
  l1 = g_leg[0]; l2 = g_leg[1];
  xSemaphoreGive(g_mtx);
  drawCoreSprite(CELL1_Y, l1, l1.valid && (now - l1.seenMs) < 5000, 0);
  drawCoreSprite(CELL2_Y, l2, l2.valid && (now - l2.seenMs) < 5000, 1);
}

// Text layer (~1Hz): link dots, per-leg numbers, and the TOTAL block.
static void reactorNumbers() {
  drawStatusWord();                                  // header LEFT: live status word (replaces the old "SHORE" label)
  Leg l1, l2; bool conn; uint32_t lastNotify; int rssi; uint32_t now = millis();
  xSemaphoreTake(g_mtx, portMAX_DELAY);
  l1 = g_leg[0]; l2 = g_leg[1]; conn = g_connected;
  lastNotify = g_lastNotifyMs; rssi = g_rssi;
  xSemaphoreGive(g_mtx);
  bool f1 = l1.valid && (now - l1.seenMs) < 5000;
  bool f2 = l2.valid && (now - l2.seenMs) < 5000;

  // --- link health cluster (top-right): a cell-signal glyph + RSSI/silence number + WiFi dot ---
  // The 4-bar glyph reads as "signal strength" at a glance (so you don't need to know what
  // "-85" means); the number still gives the exact dBm. When the BLE link goes quiet the
  // number folds into the silence age and the bars drop to zero -- one always-visible health
  // cue, no need to open /status. The glyph replaces the old BLE dot (bars = BLE state).
  long silentMs = (long)(now - lastNotify);
  bool stale    = conn && silentMs > (long)(STALE_MS / 2);

  char hb[12]; uint16_t hc; int bars;
  if (!conn)      { strcpy(hb, "--"); hc = C_RED; bars = 0; }               // down
  else if (stale) { snprintf(hb, sizeof(hb), "%lds", silentMs / 1000);      // silent: show age, no bars
                    hc = silentMs > (long)STALE_MS ? C_RED : C_AMBER; bars = 0; }
  else            { snprintf(hb, sizeof(hb), "%d", rssi);                   // healthy: RSSI dBm + bars
                    hc   = (rssi == 0) ? C_DIM : (rssi >= -75 ? C_GREEN : (rssi >= -85 ? C_AMBER : C_RED));
                    bars = rssi>=-70?4 : rssi>=-78?3 : rssi>=-85?2 : rssi>=-92?1 : 0; }

  // small Bluetooth rune (always shown): anchors the cluster + labels the readout as the BLE link
  { uint16_t btc = conn ? C_CYAN : C_DIM;
    const int cx=192, d=3, yt=5, yb=18, y1=8, y2=15;
    tft.drawLine(cx,   yt, cx,   yb, btc);                                 // spine
    tft.drawLine(cx,   yt, cx+d, y1, btc);                                 // top -> upper-right wing
    tft.drawLine(cx+d, y1, cx-d, y2, btc);                                 // upper-right -> lower-left (cross)
    tft.drawLine(cx,   yb, cx+d, y2, btc);                                 // bottom -> lower-right wing
    tft.drawLine(cx+d, y2, cx-d, y1, btc); }                               // lower-right -> upper-left (cross)

  // Tap swaps the readout: bars OR number occupy the SAME slot (left of the rune, right edge x=184).
  // Both are flicker-free here: bars are fixed rects redrawn in place; the number self-clears via
  // padding. The slot is wiped once on the swap tap (in onTap), so no stale pixels leak between modes.
  if (g_rssiBars) {
    static const int BX[4] = {166, 171, 176, 181}, BH[4] = {4, 7, 10, 13}; // 4 rising bars, right-aligned to 184
    for (int b = 0; b < 4; b++)
      tft.fillRect(BX[b], 18 - BH[b], 3, BH[b], b < bars ? hc : scale565(C_DIM, 55));
  } else {
    tft.setTextDatum(TR_DATUM); tft.setTextColor(hc, C_BG); tft.setTextPadding(44);
    tft.drawString(hb, 184, 4, 2);                                         // right-aligned dBm / silence age
    tft.setTextPadding(0); tft.setTextDatum(TL_DATUM);
  }

  { uint16_t wc = g_apServing ? C_CYAN                                       // own AP up
                : g_cfg.netMode == 2 ? C_DIM                                 // radio off
                : (WiFi.status()==WL_CONNECTED ? C_GREEN : C_RED);           // joined / down
    tft.fillCircle(228, 12, 4, wc); }                                        // WiFi/AP status dot

  drawNumbers(CELL1_Y, l1, f1);
  drawNumbers(CELL2_Y, l2, f2);

  // TOTAL block (its own labelled zone, 250-298, between two divider lines)
  float cw   = (f1?l1.watts:0.0f) + (f2?l2.watts:0.0f);
  float ck   = (l1.valid?l1.kwh:0.0f) + (l2.valid?l2.kwh:0.0f);
  float stay = ck - g_cfg.kwhBase; if (stay < 0) stay = 0;
  float cost = (g_cfg.tierCount > 0) ? (float)(g_stayCostC / 100.0) : stay * g_cfg.rateC / 100.0f;   // tiers -> accurate accumulator
  char s[40]; tft.setTextDatum(TL_DATUM);           // labels are static; padded values -> no flash
  tft.setTextColor(C_CYAN, C_BG); snprintf(s,sizeof(s),"%.2f kW", cw/1000.0f); tft.setTextPadding(120); tft.drawString(s, 6, 261, 4);
  tft.setTextColor(C_TEXT, C_BG); snprintf(s,sizeof(s),"%.1f kWh", stay);      tft.setTextPadding(108); tft.drawString(s, 132, 261, 2);
  tft.setTextColor(C_AMBER,C_BG); snprintf(s,sizeof(s),"$%.2f", cost);         tft.setTextPadding(108); tft.drawString(s, 132, 279, 2);
  tft.setTextPadding(0);
}

static const Rect R_PROV_QR   = {16, 244, 208, 30};   // secondary: the portal URL QR (captive-portal fallback if the page doesn't auto-open)
static const Rect R_PROV_SKIP = {16, 282, 208, 30};   // on-screen "set up without Wi-Fi" (phone-free standalone setup)
static bool g_provQr = false;                         // provisioning: showing the portal URL QR instead of the join screen?
static void provisioningScreen() {
  g_provQr = false;
  tft.fillScreen(C_BG);
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(C_CYAN, C_BG);  tft.drawString("HUGHES BRIDGE", 120, 6, 4);
  tft.setTextColor(C_AMBER, C_BG); tft.drawString("FIRST-TIME SETUP", 120, 38, 2);
  tft.drawFastHLine(0, 58, 240, C_LINE);
  // Hero: a Wi-Fi JOIN QR. The setup AP is open, so a T:nopass QR makes the phone auto-join --
  // no hunting for the network by name -- and most phones then pop the setup page on their own.
  char wifi[64]; snprintf(wifi, sizeof(wifi), "WIFI:S:%s;T:nopass;;", g_setupSsid);
  QRCode qr; static uint8_t qrbuf[200];
  qrcode_initText(&qr, qrbuf, 4, ECC_LOW, wifi);
  const int scale = 4, oy = 66, q = 7;
  int qw = qr.size * scale, ox = (240 - qw) / 2;
  tft.fillRect(ox - q, oy - q, qw + 2*q, qw + 2*q, TFT_WHITE);
  for (int y = 0; y < qr.size; y++) for (int x = 0; x < qr.size; x++)
    if (qrcode_getModule(&qr, x, y)) tft.fillRect(ox + x*scale, oy + y*scale, scale, scale, TFT_BLACK);
  tft.setTextColor(C_TEXT, C_BG);  tft.drawString("scan to join -- or pick this network:", 120, oy + qw + q + 4, 1);
  tft.setTextColor(C_GREEN, C_BG); tft.drawString(g_setupSsid, 120, oy + qw + q + 18, 2);
  tft.setTextDatum(TL_DATUM);
  btn(R_PROV_QR,   "SETUP PAGE DIDN'T OPEN?", C_LINE, C_CYAN, 2);
  btn(R_PROV_SKIP, "SET UP WITHOUT WI-FI",    C_LINE, C_AMBER, 2);
}

// ---------------------------------------------------------------- settings + graphs + touch
static const Rect R_BACK   = {0,0,90,28},   R_RESET  = {14,116,212,30};   // ENERGY page: RESET STAY (below the two-line THIS STAY block)
static const Rect R_BRT_DN = {14,46,44,32}, R_BRT_UP = {182,46,44,32};    // BRIGHTNESS -- now at the TOP of the SYSTEM page (moved off SETTINGS)
static const Rect R_RATE_DN= {14,184,48,40}, R_RATE_UP= {178,184,48,40};  // ENERGY page (FLAT mode only): -/+ rate steppers, flanking the centered value
static const Rect R_INFO_NET = {14,204,212,34};              // NETWORK launcher, moved onto the INFO page (replaced the RAW hex)
static const Rect R_WEBQR     = {14,170,212,28};             // "WEB PORTAL (QR)" -> webQrDraw (where RECONFIG used to be)
static const Rect R_RESET_ODO = {14,248,212,30};             // ENERGY page: RESET ODOMETER (last element, below the RATE section rule)
static const Rect R_ODO_FIRE = {22,196,88,36}, R_ODO_CANCEL = {130,196,88,36};
static uint32_t g_btnFlash = 0;                     // RESET-button confirmation flash timer
static uint32_t g_resetArm = 0;                     // RESET STAY kWh two-tap gate: 0=idle, else 1st-tap time
static bool     g_confirmOdo = false;               // odometer-reset confirm overlay open?

static void applyBright() { ledcWrite(45, (uint32_t)map(g_cfg.bright, 0, 100, 0, 255)); }

static float stayKwh() {
  Leg l1, l2; xSemaphoreTake(g_mtx, portMAX_DELAY); l1=g_leg[0]; l2=g_leg[1]; xSemaphoreGive(g_mtx);
  float s = (l1.valid?l1.kwh:0.0f) + (l2.valid?l2.kwh:0.0f) - g_cfg.kwhBase; return s<0?0:s;
}
static void drawStayVal() {                          // THIS STAY: kWh + cost on two centered, full-precision lines (no overflow, no rounding)
  char s[24]; float st = stayKwh();
  float cost = (g_cfg.tierCount > 0) ? (float)(g_stayCostC / 100.0) : st*g_cfg.rateC/100.0f;   // tiers -> accurate accumulator
  tft.setTextDatum(TC_DATUM); tft.setTextPadding(236);
  snprintf(s, sizeof(s), "%.2f kWh", st);  tft.setTextColor(C_TEXT,  C_BG); tft.drawString(s, 120, 44, 4);
  snprintf(s, sizeof(s), "$%.2f", cost);   tft.setTextColor(C_GREEN, C_BG); tft.drawString(s, 120, 72, 4);
  tft.setTextPadding(0); tft.setTextDatum(TL_DATUM);
}
static void drawBrightVal() {                                 // centered value between the -/+ at the top of the SYSTEM page
  char s[8]; snprintf(s, sizeof(s), "%d%%", g_cfg.bright);
  tft.setTextDatum(MC_DATUM); tft.setTextColor(C_TEXT, C_BG); tft.setTextPadding(90);
  tft.drawString(s, 120, 62, 4); tft.setTextPadding(0); tft.setTextDatum(TL_DATUM);
}
static void drawRateVal() {                          // the rate VALUE only (state-aware color/pos); the 1Hz refresh calls this
  char s[12]; uint16_t col; int yv;
  if (g_cfg.tierCount > 0) {                          // TIME-OF-DAY plan
    bool known = localHourNow() >= 0;
    snprintf(s, sizeof(s), "$%.2f", (known ? effectiveRateC() : g_cfg.rateC) / 100.0f);
    col = known ? C_GREEN : C_AMBER; yv = 196; tft.setTextPadding(150);   // no -/+ buttons here -> wide erase
  } else {                                            // FLAT: value sits between the -/+ steppers (x62..x178)
    snprintf(s, sizeof(s), "$%.2f", g_cfg.rateC / 100.0f);
    col = C_TEXT; yv = 204; tft.setTextPadding(112);                      // narrow erase clears the buttons
  }
  tft.setTextDatum(MC_DATUM); tft.setTextColor(col, C_BG);
  tft.drawString(s, 120, yv, 4); tft.setTextPadding(0); tft.setTextDatum(TL_DATUM);
}
// The whole RATE section (header + mode pill + either the -/+ stepper or the read-only plan view).
// Redrawn on page entry and whenever the plan's clock-known state flips.
static void drawRateSection() {
  tft.fillRect(0, 153, 240, 88, C_BG);               // clear the rate band (below the y152 rule, above the y242 rule)
  tft.setTextDatum(TL_DATUM); tft.setTextColor(C_TEXT, C_BG); tft.drawString("RATE", 14, 158, 2);
  if (g_cfg.tierCount > 0) {                          // TIME-OF-DAY plan (edited on the web; device is read-only)
    bool known = localHourNow() >= 0;
    Rect pill = {104, 155, 122, 22}; btnFill(pill, "TIME-OF-DAY", known ? C_GREEN : C_AMBER, C_BG, 2);
    drawRateVal();
    tft.setTextDatum(TC_DATUM);
    if (known) { tft.setTextColor(C_DIM,  C_BG); tft.drawString("right now -- changes by the hour", 120, 216, 1);
                 tft.setTextColor(C_CYAN, C_BG); tft.drawString("edit the plan on the web portal",  120, 228, 1); }
    else       { tft.setTextColor(C_AMBER,C_BG); tft.drawString("clock not set -- using flat price", 120, 216, 1);
                 tft.drawString("set it on the TIME screen", 120, 228, 1); }
    tft.setTextDatum(TL_DATUM);
  } else {                                            // FLAT: the -/+ stepper
    Rect pill = {168, 155, 58, 22}; btnFill(pill, "FLAT", C_CYAN, C_BG, 2);
    btn(R_RATE_DN, "-", C_LINE, C_TEXT, 4); btn(R_RATE_UP, "+", C_LINE, C_TEXT, 4);
    drawRateVal();
    tft.setTextDatum(TC_DATUM); tft.setTextColor(C_DIM, C_BG); tft.drawString("price per kilowatt-hour", 120, 228, 1);
    tft.setTextDatum(TL_DATUM);
  }
}
// ---- SETTINGS menu (the root list; single-tap the SETTINGS nav tab to reach it) ----
// Replaces the old hidden re-tap cycle: every page is now one visible, labelled tap away,
// and each sub-page's BACK returns here.
struct MenuEntry { const char* label; int page; };
static const MenuEntry MENU[] = {
  {"STATUS",        1},     // bridge + Watchdog diagnostics
  {"USAGE",        14},     // on-device usage report (v2.1 1.1) -- yesterday / this week / 7-day bars
  {"ENERGY & RATE", 13},    // stay kWh, $/kWh, reset odometer
  {"ALERTS",        8},     // phone push alerts
  {"DEVICES",       3},     // pick / change the Watchdog
  {"WEB PORTAL",   10},     // open the web config page (QR)
  {"TIME",          9},     // clock + timezone
  {"SYSTEM",        4},     // brightness, scrub, power off, factory reset -- + FIRMWARE launcher (folded off the menu, v2.1 1.1)
  {"HELP",         15},     // U2: the manual QR -- was only reachable via the tiny "?" chip; now a first-class row too
};
static const int MENU_N = (int)(sizeof(MENU) / sizeof(MENU[0]));
static Rect menuRow(int i) { return Rect{6, 34 + i*28, 228, 26}; }   // U2: 28px pitch fits 9 rows above the nav bar (y=292)
static const Rect R_MENU_HELP = {0, 0, 52, 28};     // U2: enlarged "?" hit rect (was 30x24) -- easier to tap in the case
static void menuDraw() {
  tft.fillScreen(C_BG); tft.setTextDatum(TR_DATUM);
  tft.setTextColor(C_TEXT, C_BG); tft.drawString("SETTINGS", 234, 6, 2);
  tft.drawRoundRect(4, 3, 24, 21, 4, C_CYAN);        // HELP affordance (menu root has no "< BACK", so the top-left is free)
  tft.setTextDatum(MC_DATUM); tft.setTextColor(C_CYAN, C_BG); tft.drawString("?", 16, 14, 2);
  tft.setTextDatum(TL_DATUM); tft.drawFastHLine(0, 28, 240, C_LINE);
  for (int i = 0; i < MENU_N; i++) {
    Rect r = menuRow(i);
    tft.setTextColor(C_TEXT, C_BG); tft.drawString(MENU[i].label, 14, r.y + 7, 2);
    tft.setTextDatum(TR_DATUM); tft.setTextColor(C_DIM, C_BG); tft.drawString(">", 228, r.y + 7, 2);
    tft.setTextDatum(TL_DATUM); tft.drawFastHLine(6, r.y + r.h, 228, C_LINE);
  }
  drawNav();
}
static void settingsDraw() {                         // ENERGY & RATE page (g_setPage 13, opened from the menu)
  g_resetArm = 0;                                    // fresh page -> the RESET STAY two-tap gate starts disarmed
  tft.fillScreen(C_BG); tft.setTextDatum(TL_DATUM);
  tft.setTextColor(C_CYAN, C_BG); tft.drawString("< BACK", 6, 6, 2);
  tft.setTextDatum(TR_DATUM); tft.setTextColor(C_TEXT, C_BG); tft.drawString("ENERGY & RATE", 234, 6, 2);
  tft.setTextDatum(TL_DATUM); tft.drawFastHLine(0, 28, 240, C_LINE);
  tft.setTextColor(C_DIM, C_BG); tft.drawString("THIS STAY", 14, 34, 1);
  drawStayVal();
  tft.setTextDatum(TC_DATUM); tft.setTextColor(C_DIM, C_BG); tft.drawString("counting since your last reset", 120, 102, 1);
  tft.setTextDatum(TL_DATUM);
  btn(R_RESET, "RESET STAY", C_LINE, C_AMBER, 2);
  tft.drawFastHLine(0, 152, 240, C_LINE);
  drawRateSection();
  tft.drawFastHLine(0, 242, 240, C_LINE);
  btn(R_RESET_ODO, "RESET ODOMETER", C_RED, C_RED, 2);
  tft.setTextDatum(TC_DATUM); tft.setTextColor(C_DIM, C_BG); tft.drawString("lifetime total -- rarely needed", 120, 280, 1);
  tft.setTextDatum(TL_DATUM);
  drawNav();
}

// ---- INFO sub-page (re-tap the SETTINGS tab): bridge + Watchdog diagnostics ----
static int g_setPage = 0;                            // SETTINGS sub-page: 0=settings, 1=INFO, 2=AUDIO (tab re-tap cycles)

// Link control (hand the BLE link to the phone app -- one client at a time). Lives on DEVICES,
// drawn only when a Watchdog is paired.
static const Rect R_LINK = {14, 172, 212, 32};
static void drawLinkBtn() {
  uint16_t c = g_released ? C_GREEN : C_AMBER;
  btn(R_LINK, g_released ? "RECONNECT" : "LET PHONE APP IN", c, c, 2);
}

// Live value fields (padded -> flicker-free), redrawn on entry + at 1Hz.
static void infoUpdate() {
  Leg l1, l2; bool conn; int rssi; uint32_t lastN, now = millis();
  xSemaphoreTake(g_mtx, portMAX_DELAY);
  l1=g_leg[0]; l2=g_leg[1]; conn=g_connected; rssi=g_rssi; lastN=g_lastNotifyMs;
  xSemaphoreGive(g_mtx);
  char v[48]; const int VX = 62;
  tft.setTextDatum(TL_DATUM);
  auto row=[&](int y, const char* s, uint16_t c){ tft.setTextColor(c, C_BG); tft.setTextPadding(178); tft.drawString(s, VX, y, 1); };

  // BRIDGE (mode-aware: HOME shows the DHCP lease; AP shows our own network; MAC is the real device MAC)
  { char mac[20]; macStr(mac, sizeof(mac)); row(58, mac, C_TEXT); }        // real device MAC (efuse; not 00:00 in AP/off)
  if (g_apServing) {                                                       // hosting our own AP
    char ap[24]; apSsid(ap, sizeof(ap));
    row(44, WiFi.softAPIP().toString().c_str(), C_CYAN);
    row(72, ap,           C_CYAN);
    row(86, "hosting AP", C_CYAN);
  } else if (g_cfg.netMode == 2) {                                         // radio off
    row(44, "--", C_DIM); row(72, "radio off", C_DIM); row(86, "--", C_DIM);
  } else {                                                                 // HOME (STA)
    bool up = WiFi.status() == WL_CONNECTED;
    row(44, up ? WiFi.localIP().toString().c_str() : "0.0.0.0", up ? C_TEXT : C_RED);
    row(72, up ? WiFi.SSID().c_str() : "not joined", up ? C_TEXT : C_AMBER);
    { int r=(int)WiFi.RSSI(); const char* q=r>=-60?"strong":r>=-72?"good":r>=-82?"weak":"poor";
      snprintf(v,sizeof(v),"%d dBm (%s)", r, q); } row(86, up ? v : "--", up ? C_TEXT : C_DIM);
  }
  { uint32_t s=now/1000; snprintf(v,sizeof(v),"%luh %02lum %02lus", s/3600,(s%3600)/60,s%60); } row(100, v, C_TEXT);

  // WATCHDOG (no Watchdog tracked -> empty states, not a phantom "down for Xs" link)
  bool haveWd = g_cfg.hughesMac.length() || g_cfg.hughesName.length();
  row(136, g_peerMac[0] ? g_peerMac : (g_cfg.hughesMac.length()?g_cfg.hughesMac.c_str():"--"), C_TEXT);
  row(150, g_peerName[0] ? g_peerName : "--", C_TEXT);
  if (!haveWd)         { row(164, "no Watchdog selected", C_AMBER); }
  else if (g_released) { row(164, "paused - phone app in use", C_AMBER); }
  else { snprintf(v,sizeof(v),"%s  %ddBm  %lds", conn?"connected":"not connected", rssi, (long)((now-lastN)/1000));
         row(164, v, conn ? C_GREEN : C_RED); }
  if (haveWd) {
    snprintf(v,sizeof(v),"err%u  %.1fHz  %.2fkWh", l1.err, l1.hz, l1.kwh); row(178, v, C_TEXT);
    snprintf(v,sizeof(v),"err%u  %.1fHz  %.2fkWh", l2.err, l2.hz, l2.kwh); row(192, v, C_TEXT);
  } else { row(178, "--", C_DIM); row(192, "--", C_DIM); }

  // (RAW last-packet hex removed from INFO -- still in /status "raw" + serial; the NET launcher took this space)

  // firmware version + boot diagnostics (boot count + last reset reason, from the RTC dmesg ring)
  snprintf(v, sizeof(v), "FW %s   boot#%lu %s", FW_VERSION, (unsigned long)dmesgBootCount(), dmesgLastReset());
  tft.setTextColor(C_DIM, C_BG); tft.setTextPadding(220); tft.drawString(v, 14, 252, 1); tft.setTextPadding(0);

  // bottom: battery + API key
  if (g_battMv > 0) { const char* st = g_onBattery ? "batt" : (g_battMv >= 4100 ? "full" : "chg");
                      snprintf(v,sizeof(v),"BAT %d.%02dV %d%% %s   PASS %s",   // PASS = AP Wi-Fi password + HTTP ?key=
                               g_battMv/1000, (g_battMv%1000)/10, g_battPct, st, g_cfg.apiKey.c_str()); }
  else              snprintf(v,sizeof(v),"BAT --   PASS %s", g_cfg.apiKey.c_str());
  tft.setTextColor(C_DIM, C_BG); tft.setTextPadding(220); tft.drawString(v, 14, 280, 1);
  tft.setTextPadding(0);
}

static void infoDraw() {
  tft.fillScreen(C_BG); tft.setTextDatum(TL_DATUM);
  tft.setTextColor(C_CYAN, C_BG); tft.drawString("< BACK", 6, 6, 2);
  tft.setTextDatum(TR_DATUM); tft.setTextColor(C_TEXT, C_BG); tft.drawString("STATUS", 234, 6, 2);
  tft.setTextDatum(TL_DATUM); tft.drawFastHLine(0, 28, 240, C_LINE);
  tft.setTextColor(C_CYAN, C_BG); tft.drawString("BRIDGE", 6, 31, 1);
  tft.setTextColor(C_DIM, C_BG);
  tft.drawString("IP",   14, 44, 1); tft.drawString("MAC",  14, 58, 1); tft.drawString("WIFI", 14, 72, 1);
  tft.drawString("SIG",  14, 86, 1); tft.drawString("UP",   14, 100, 1);
  tft.setTextColor(C_CYAN, C_BG); tft.drawString("WATCHDOG", 6, 118, 1);
  tft.setTextColor(C_DIM, C_BG);
  tft.drawString("BLE",  14, 136, 1); tft.drawString("NAME", 14, 150, 1); tft.drawString("LINK", 14, 164, 1);
  tft.drawString("L1",   14, 178, 1); tft.drawString("L2",   14, 192, 1);
  { const char* nm = g_cfg.netMode==1 ? "NETWORK: HOTSPOT" : g_cfg.netMode==2 ? "NETWORK: OFF" : "NETWORK: HOME";   // U8: "OWN AP" -> "HOTSPOT"
    btn(R_INFO_NET, nm, C_LINE, C_CYAN, 2); }                                // NET launcher (moved here from SETTINGS; replaced the RAW hex)
  infoUpdate();
  drawNav();
}

// ---- TIME page (g_setPage 9, a peer settings tab): live local clock + timezone picker + SNTP blurb ----
static const Rect R_TIME_SET = {14, 234, 212, 30};       // -> manual set-time page (g_setPage 11)
static Rect tzCell(int i) { int r = i / 2, c = i % 2; return Rect{ 14 + c*110, 66 + r*34, 102, 30 }; }
static void tzClockUpdate() {                            // repaint just the clock line (1 Hz), 12-hour + amber if untrustworthy
  char lt[28]; time_t now = time(nullptr);
  bool synced = now >= 1700000000, known = synced && tzConfigured();
  if (!synced) snprintf(lt, sizeof(lt), "--:--");
  else { struct tm t; localtime_r(&now, &t); strftime(lt, sizeof(lt), "%a %b %e  %I:%M %p", &t); }
  tft.setTextDatum(TC_DATUM); tft.setTextColor(known ? C_GREEN : C_AMBER, C_BG); tft.setTextPadding(232);
  tft.drawString(lt, 120, 46, 2); tft.setTextPadding(0); tft.setTextDatum(TL_DATUM);
}
static void timeDraw() {
  tft.fillScreen(C_BG); tft.setTextDatum(TL_DATUM);
  tft.setTextColor(C_CYAN, C_BG); tft.drawString("< BACK", 6, 6, 2);
  tft.setTextDatum(TR_DATUM); tft.setTextColor(C_TEXT, C_BG); tft.drawString("TIME", 234, 6, 2);
  tft.setTextDatum(TL_DATUM); tft.drawFastHLine(0, 28, 240, C_LINE);
  bool zoneSet = tzConfigured();
  tft.setTextDatum(TC_DATUM); tft.setTextColor(zoneSet ? C_DIM : C_AMBER, C_BG);
  tft.drawString(zoneSet ? "LOCAL TIME" : "PICK YOUR TIME ZONE BELOW", 120, 32, 1);
  tft.setTextDatum(TL_DATUM);
  tzClockUpdate();
  for (int i = 0; i < TZ_COUNT; i++) {                    // timezone picker (2x4 grid)
    Rect r = tzCell(i);
    bool sel = tzClamp(g_cfg.tzIndex) == i;
    if (sel && i == 0) btnFill(r, TZ_TABLE[i].name, C_AMBER, C_BG, 2);   // "Not set" selected -> amber, not "good" green
    else if (sel)      btnFill(r, TZ_TABLE[i].name, C_GREEN, C_BG, 2);
    else               btn(r, TZ_TABLE[i].name, C_LINE, C_TEXT, 2);
  }
  bool sta = (g_cfg.netMode == 0 && WiFi.status() == WL_CONNECTED);
  tft.setTextDatum(TC_DATUM);
  if (g_clkProv == CLK_NTP)         { tft.setTextColor(C_DIM,   C_BG); tft.drawString("clock is set from the internet", 120, 205, 1); }
  else if (g_clkProv == CLK_MANUAL) { tft.setTextColor(C_DIM,   C_BG); tft.drawString("clock was set by hand", 120, 205, 1); }
  else if (sta)                     { tft.setTextColor(C_AMBER, C_BG); tft.drawString("syncing from the internet...", 120, 205, 1); }
  else                              { tft.setTextColor(C_AMBER, C_BG); tft.drawString("no internet -- join home Wi-Fi to", 120, 199, 1);
                                      tft.drawString("auto-sync, or SET TIME below", 120, 211, 1); }
  tft.setTextDatum(TL_DATUM);
  btn(R_TIME_SET, "SET TIME MANUALLY", C_LINE, C_CYAN, 2);
  tft.setTextDatum(TC_DATUM); tft.setTextColor(C_DIM, C_BG);
  tft.drawString("resets if the battery dies", 120, R_TIME_SET.y + R_TIME_SET.h + 4, 1);
  tft.setTextDatum(TL_DATUM);
  drawNav();
}

// ---- Manual set-time page (g_setPage 11, from the TIME page): hour/min +/- -> settimeofday (local) ----
static int g_setH = 12, g_setM = 0;                      // working local hour/minute
static const Rect R_TS_HDN={14,86,44,40},  R_TS_HUP={80,86,44,40};
static const Rect R_TS_MDN={128,86,44,40}, R_TS_MUP={194,86,44,40};
static const Rect R_TS_OK={14,196,100,42}, R_TS_CX={126,196,100,42};
static void timeSetValDraw() {
  char s[8]; snprintf(s, sizeof(s), "%02d:%02d", g_setH, g_setM);
  tft.setTextDatum(TC_DATUM); tft.setTextColor(C_GREEN, C_BG); tft.setTextPadding(160);
  tft.drawString(s, 120, 130, 6); tft.setTextPadding(0);                 // font 6 is ~48px tall -> spans y 130..178
  int h12 = g_setH % 12; if (h12 == 0) h12 = 12;         // 12-hour AM/PM confirmation for a US audience
  char hint[16]; snprintf(hint, sizeof(hint), "= %d:%02d %s", h12, g_setM, g_setH < 12 ? "AM" : "PM");
  tft.setTextColor(C_DIM, C_BG); tft.setTextPadding(160); tft.drawString(hint, 120, 180, 2); tft.setTextPadding(0);  // just below the big time, above the SET/CANCEL row at y196
  tft.setTextDatum(TL_DATUM);
}
static void timeSetDraw() {
  tft.fillScreen(C_BG); tft.setTextDatum(TL_DATUM);
  tft.setTextColor(C_CYAN, C_BG); tft.drawString("< BACK", 6, 6, 2);
  tft.setTextDatum(TR_DATUM); tft.setTextColor(C_TEXT, C_BG); tft.drawString("SET TIME", 234, 6, 2);
  tft.setTextDatum(TL_DATUM); tft.drawFastHLine(0, 28, 240, C_LINE);
  tft.setTextDatum(TC_DATUM); tft.setTextColor(C_DIM, C_BG);
  tft.drawString("HOUR", 47, 68, 1); tft.drawString("MIN", 161, 68, 1);
  tft.setTextDatum(TL_DATUM);
  btn(R_TS_HDN, "-", C_LINE, C_TEXT, 4); btn(R_TS_HUP, "+", C_LINE, C_TEXT, 4);
  btn(R_TS_MDN, "-5", C_LINE, C_TEXT, 2); btn(R_TS_MUP, "+5", C_LINE, C_TEXT, 2);
  timeSetValDraw();
  tft.setTextDatum(TC_DATUM); tft.setTextColor(C_DIM, C_BG);
  tft.drawString("local; lost on reboot -- Wi-Fi", 120, 246, 1);
  tft.drawString("(SNTP) is the reliable clock", 120, 258, 1);
  tft.setTextDatum(TL_DATUM);
  btn(R_TS_OK, "SET", C_GREEN, C_GREEN, 2); btn(R_TS_CX, "CANCEL", C_LINE, C_DIM, 2);
  drawNav();
}
static void timeSetApply() {                             // combine today's date + the edited local H:M -> settimeofday
  time_t now = time(nullptr);
  bool hadRealDate = (now >= 1700000000) && g_dateReal;  // an H:M edit preserves a real date but can't invent one
  if (now < 1700000000) now = 1735689600;                // fallback base date if never synced (FABRICATED -> not billing-real)
  struct tm t; localtime_r(&now, &t);
  t.tm_hour = g_setH; t.tm_min = g_setM; t.tm_sec = 0;
  time_t e = mktime(&t);                                 // mktime reads the TZ env (applyTz) -> UTC epoch
  struct timeval tv = { e, 0 }; settimeofday(&tv, nullptr);
  g_clkProv = CLK_MANUAL;                                 // hand-set: usable for tiers (real hour), flagged vs SNTP
  (void)hadRealDate;                                      // M5: an H:M edit never establishes a real epoch -> g_epochReal is left untouched; recompute derives the date trust
  recomputeDateReal();
  logf("time: set manually to %02d:%02d local (date_real=%d)", g_setH, g_setM, (int)g_dateReal);
  evtLog(EV_CLOCK_SET, (int16_t)g_cfg.tzIndex);           // hand-set on-device (audit)
}

// Full-screen "open the web portal" QR: encodes this unit's dashboard/portal URL so a phone on the same
// network (home Wi-Fi, or joined to our AP) can scan to open it -- the robust path when Android's captive
// portal doesn't auto-pop. Mode-aware: STA -> http://<ip>/, otherwise the AP portal at 192.168.4.1.
static void webQrDraw(bool showNav) {
  bool sta = (g_cfg.netMode == 0 && WiFi.status() == WL_CONNECTED);
  char url[40]; snprintf(url, sizeof(url), "http://%s/", sta ? WiFi.localIP().toString().c_str() : "192.168.4.1");
  tft.fillScreen(C_BG); tft.setTextDatum(TL_DATUM);
  tft.setTextColor(C_CYAN, C_BG); tft.drawString("< BACK", 6, 6, 2);
  tft.setTextDatum(TC_DATUM); tft.setTextColor(C_TEXT, C_BG); tft.drawString("WEB PORTAL", 120, 34, 2);
  QRCode qr; static uint8_t qrbuf[200];
  qrcode_initText(&qr, qrbuf, 4, ECC_LOW, url);
  const int scale = 5, oy = 62, q = 10;
  int qw = qr.size * scale, ox = (240 - qw) / 2;
  tft.fillRect(ox - q, oy - q, qw + 2*q, qw + 2*q, TFT_WHITE);
  for (int y = 0; y < qr.size; y++) for (int x = 0; x < qr.size; x++)
    if (qrcode_getModule(&qr, x, y)) tft.fillRect(ox + x*scale, oy + y*scale, scale, scale, TFT_BLACK);
  tft.setTextColor(C_TEXT, C_BG); tft.drawString(url, 120, oy + qw + q + 6, 2);
  tft.setTextColor(C_DIM, C_BG);
  if (sta) tft.drawString("scan on a phone on the same Wi-Fi", 120, oy + qw + q + 28, 1);
  else { tft.drawString("join this unit's Wi-Fi first,", 120, oy + qw + q + 24, 1);
         tft.drawString("then scan to open the setup page", 120, oy + qw + q + 36, 1); }
  tft.setTextDatum(TL_DATUM);
  if (showNav) drawNav();
}

// Fire the odometer reset: queue the BLE write + zero the stay baseline (so STAY stays coherent).
static void resetOdometerFire() {
  if (stayOpenIdx() >= 0) { logf("odo: reset refused -- a stay is open"); return; }   // R6 defense-in-depth (the arm site also blocks this)
  g_wantReset = true;                                // bleTask writes "RESEt" to fff5
  g_cfg.kwhBase = 0.0f; configPersist(); baselinePersist(); stayCostReset();
  evtLog(EV_RESET_ODO, 0);                           // audit: lifetime meter zeroed
}

// Destructive-action confirm overlay drawn over SETTINGS.
static void drawOdoConfirm() {
  tft.fillRect(12, 116, 216, 120, C_BG);
  tft.drawRoundRect(12, 116, 216, 120, 6, C_RED);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(C_RED, C_BG);  tft.drawString("ZERO THE WATCHDOG?", 120, 136, 2);
  tft.setTextColor(C_DIM, C_BG);  tft.drawString("permanently zeroes its", 120, 158, 1);
  tft.drawString("lifetime meter (your STAY", 120, 170, 1);
  tft.drawString("total is separate)", 120, 182, 1);
  tft.setTextDatum(TL_DATUM);
  btn(R_ODO_FIRE, "ZERO IT", C_RED, C_RED, 2);
  btn(R_ODO_CANCEL, "CANCEL", C_LINE, C_DIM, 2);
}

// Laser "discharge" reset animation on the reactor: charge -> fire beams -> collapse.
static void resetAnimation() {
  int c1x = 52, c1y = CELL1_Y + 62, c2x = 52, c2y = CELL2_Y + 62;
  auto beam = [&](int x0,int y0,int x1,int y1,uint16_t c){
    tft.drawLine(x0,y0,x1,y1,c); tft.drawLine(x0-1,y0,x1,y1,c); tft.drawLine(x0+1,y0,x1,y1,c);
  };
  for (int f = 0; f < 34; f++) {
    tft.fillScreen(C_BG);
    if (f < 10) {                                          // CHARGE: cores swell + whiten
      uint16_t col = blend565(C_CYAN, 0xFFFF, (uint8_t)(f*25));
      tft.fillCircle(c1x, c1y, 20+f, col);
      tft.fillCircle(c2x, c2y, 20+f, col);
    } else if (f < 25) {                                   // FIRE: beams out + link beam + flash
      tft.fillCircle(c1x, c1y, 30, 0xFFFF);
      tft.fillCircle(c2x, c2y, 30, 0xFFFF);
      for (int i = 0; i < 7; i++) {
        float a = (f*28 + i*52) * DEG_TO_RAD;
        beam(c1x, c1y, c1x + (int)(cosf(a)*270),  c1y + (int)(sinf(a)*270),  (i&1)?C_CYAN:0xFFFF);
        beam(c2x, c2y, c2x + (int)(cosf(-a)*270), c2y + (int)(sinf(-a)*270), (i&1)?0xFFFF:C_CYAN);
      }
      beam(c1x, c1y, c2x, c2y, 0xFFFF);                    // beam linking the two cores
      if (f & 1) tft.drawRect(0, 0, 240, 320, 0xFFFF);     // edge flash
      tft.setTextDatum(MC_DATUM); tft.setTextColor(0xFFFF, C_BG);
      tft.drawString("RESET", 120, 160, 4); tft.setTextDatum(TL_DATUM);
    } else {                                               // COLLAPSE: implode to cold idle
      int r = 30 - (f-25)*4; if (r < 1) r = 1;
      uint8_t b = (uint8_t)(255 - (f-25)*28);
      tft.fillCircle(c1x, c1y, r, scale565(C565(0x22,0x55,0xff), b));
      tft.fillCircle(c2x, c2y, r, scale565(C565(0x22,0x55,0xff), b));
    }
    delay(42);
  }
}
// ---- power history: 30-min RAM ring @ 1Hz, mirrored to SD so it survives power loss ----
#define GRAPH_CAP 1800                              // 30 min * 60 s
static uint16_t g_gL1[GRAPH_CAP], g_gL2[GRAPH_CAP]; // per-leg watts; combined = their sum
static uint16_t g_gA1[GRAPH_CAP], g_gA2[GRAPH_CAP]; // per-leg DECIAMPS (amps*10); its own graph page
static int  g_gHead = 0, g_gCount = 0;              // one head/count -> W and A share a timebase
static uint64_t g_freshL1 = 0, g_freshL2 = 0;       // per-leg "was this 1Hz tick a real packet?" -- last 64 ticks
                                                    // (bit0 = most recent). Feeds the 60s /status aggregates.
static int  g_graphMin  = 10;                       // selected range: 5 / 10 / 30 min
static int  g_graphMode = 0;                        // 0 = POWER (W), 1 = CURRENT (A); GRAPHS re-tap toggles
static bool g_sdOk = false;
static const Rect R_G5={8,30,68,24}, R_G10={86,30,68,24}, R_G30={164,30,68,24};
static const int GX0=34, GY0=64, GX1=236, GY1=240;  // chart box (top-left .. bottom-right)
#define HIST_PATH "/reactor_hist.bin"
// flicker-free chart: render into an 8-bit palette sprite and push it whole
static TFT_eSprite g_chartSpr = TFT_eSprite(&tft);
static bool g_chartOk = false;
enum { PAL_BG=0, PAL_LINE, PAL_AMBER, PAL_CYAN, PAL_TOTAL };

// ---- 30-day per-rate usage ledger (v2.1 0.3) --------------------------------------------------
// Daily kWh + cost split into per-rate buckets (bucket i <-> g_cfg.tiers[i]; bucket 0 in flat mode), accrued from
// the SAME 1 Hz odometer delta as the stay-cost accumulator (graphSample below). SD-persisted + versioned. Feeds
// the on-device USAGE page + the web billing report. Trustworthy dates need the clock set (see clock-confidence).
#define USAGE_DAYS 31                                  // 30 past days + today's in-progress record
#define USAGE_MAGIC 0x55534732u                        // "USG2" -- per-bucket cost (robust to mid-day rate edits)
struct DayRec {
  uint32_t date;                                       // yyyymmdd local; 0 = "undated" (clock not confident yet)
  float    kwh[MAX_TIERS];                             // kWh accrued in each rate bucket
  double   bcostC[MAX_TIERS];                          // per-bucket cost, cents; the bucket's rate = bcostC/kwh (derived,
                                                       //   so a mid-day rate/tier edit can't retro-misprice the bucket)
  float    totalKwh;
  double   costC;                                      // day total cents
};
static DayRec   g_day[USAGE_DAYS];
static int      g_dayHead = 0, g_dayCount = 0;         // ring; g_day[g_dayHead] = current day
static uint32_t g_dayCur = 0;                          // the date g_day[g_dayHead] represents (0 = undated)
static bool     g_usageDirty = false;                  // M9: ledger changed since the last write -> the 30s periodic save is a no-op otherwise (SD-wear + a needless rename window on an idle unit)

static uint32_t localDateNow() {                       // yyyymmdd local, or 0 if the DATE isn't trustworthy for billing
  time_t now = time(nullptr);
  if (!g_dateReal || now < 1700000000 || !tzConfigured()) return 0;   // g_dateReal: NTP or phone-set only (not a fabricated H:M date)
  struct tm t; localtime_r(&now, &t);
  return (uint32_t)(t.tm_year + 1900) * 10000u + (uint32_t)(t.tm_mon + 1) * 100u + (uint32_t)t.tm_mday;
}
static int usageTierIdx() {                            // active tier index, or -1 => flat/clock-unknown (bucket 0 @ rateC)
  if (g_cfg.tierCount == 0) return -1;
  int h = localHourNow(); if (h < 0) return -1;
  for (int i = 0; i < g_cfg.tierCount; i++) if (tierContains(g_cfg.tiers[i], h)) return i;
  return -1;
}
static void usagePath(char* out, size_t n) {
  char mac[16]; macKey(mac, sizeof(mac));
  if (mac[0]) snprintf(out, n, "/usage_%s.bin", mac);
  else        snprintf(out, n, "/usage.bin");
}
static void usageSave(bool force = false) {           // write to .tmp then swap in -> a power cut can't shred a month of billing
  if (!g_sdOk) return;
  if (!force && !g_usageDirty) return;                 // M9: dirty-gate -> no 30s rewrite (or rename window) on a unit that isn't accruing
  char p[40], tmp[48]; usagePath(p, sizeof(p)); snprintf(tmp, sizeof(tmp), "%s.tmp", p);
  File f = SD_MMC.open(tmp, "w"); if (!f) return;
  uint32_t magic = USAGE_MAGIC;
  size_t n = 0;
  n += f.write((uint8_t*)&magic, 4);
  n += f.write((uint8_t*)&g_dayHead, 4); n += f.write((uint8_t*)&g_dayCount, 4); n += f.write((uint8_t*)&g_dayCur, 4);
  n += f.write((uint8_t*)g_day, sizeof(g_day));
  f.close();
  if (n != 16 + sizeof(g_day)) { SD_MMC.remove(tmp); return; }   // short write -> keep the old good file
  SD_MMC.remove(p); SD_MMC.rename(tmp, p);                       // swap (narrow window vs a multi-KB in-place truncate+write)
  g_usageDirty = false;
  stayCostPersist();                                  // M1: persist lastck IN LOCKSTEP with the ledger -> on reboot the NVS odometer baseline can't be older than the SD ledger (which caused the [lastck,ledger] overlap to re-accrue)
}
static void usageLoad() {
  if (!g_sdOk) return;
  char p[40]; usagePath(p, sizeof(p));
  File f = SD_MMC.open(p, "r"); if (!f) return;
  uint32_t magic = 0;
  if ((int)f.size() >= (int)(16 + sizeof(g_day))) {
    f.read((uint8_t*)&magic, 4);
    if (magic == USAGE_MAGIC) {
      f.read((uint8_t*)&g_dayHead, 4); f.read((uint8_t*)&g_dayCount, 4); f.read((uint8_t*)&g_dayCur, 4);
      f.read((uint8_t*)g_day, sizeof(g_day));
      if (g_dayHead < 0 || g_dayHead >= USAGE_DAYS) g_dayHead = 0;
      if (g_dayCount < 0 || g_dayCount > USAGE_DAYS) g_dayCount = 0;
      // R8: the evtLoad/stayLoad head/count invariant, adapted to THIS ring's convention -- g_dayHead points AT
      // the newest/current record, so an un-wrapped ring has head == count-1 (NOT head == count as in stays). A
      // corrupt-but-in-bounds pair would otherwise load the billing rows misaligned. Drop to empty on violation.
      if (g_dayCount == 0) { g_dayHead = 0; g_dayCur = 0; }
      else if (g_dayCount < USAGE_DAYS && g_dayHead != g_dayCount - 1) {
        g_dayHead = 0; g_dayCount = 0; g_dayCur = 0; memset(g_day, 0, sizeof(g_day));
        Serial.println("usage: ring invariant violated -> dropped to empty");
      }
      g_dayCur = (g_dayCount > 0) ? g_day[g_dayHead].date : 0;   // FINDING#8: tie g_dayCur to the current record's date -- a corrupt in-range g_dayCur would otherwise wedge the rollover forever
      Serial.printf("usage: restored %d day(s) from SD\n", g_dayCount);
    }
  }
  f.close();
}
static void usageAccrue(double dKwh, int rateC) {      // 1 Hz from graphSample, same delta+rate as the cost accumulator
  if (dKwh <= 0) return;
  uint32_t date = localDateNow();
  if (g_dayCount == 0) { memset(&g_day[0], 0, sizeof(g_day[0])); g_day[0].date = date; g_dayHead = 0; g_dayCount = 1; g_dayCur = date; }
  else if (date != g_dayCur) {
    bool rollFwd     = (date != 0) && (g_dayCur == 0 || date > g_dayCur);   // a genuine new day (or undated -> first real date)
    bool rollUndated = (date == 0) && (g_dayCur != 0);                      // clock lost since this record was dated (e.g. offline reboot)
    if (rollFwd || rollUndated) {                       // open a fresh record; a BACKWARD clock (date<g_dayCur) keeps the current one
      g_dayHead = (g_dayHead + 1) % USAGE_DAYS;
      memset(&g_day[g_dayHead], 0, sizeof(g_day[g_dayHead]));
      g_day[g_dayHead].date = date;
      if (g_dayCount < USAGE_DAYS) g_dayCount++;
      g_dayCur = date; g_usageDirty = true; usageSave(true);   // persist the day boundary immediately (force past the dirty-gate)
    }
  }
  DayRec& d = g_day[g_dayHead];
  int ti = usageTierIdx(); int b = (ti < 0) ? 0 : ti;
  d.kwh[b] += (float)dKwh; d.bcostC[b] += dKwh * rateC;  // per-bucket cost (rate derived -> a mid-day rate edit can't retro-misprice)
  d.totalKwh += (float)dKwh; d.costC += dKwh * rateC;
  g_usageDirty = true;                                  // M9: mark the ledger for the next save
}

static void graphSample() {
  if (g_reboot) return;   // FINDING#4: a reboot is pending (e.g. mid-adopt, right after swapBillingReset) -> don't accrue/re-persist the OLD unit's baseline in the final loop iteration before restart
  Leg l1, l2; uint32_t now = millis();
  xSemaphoreTake(g_mtx, portMAX_DELAY); l1=g_leg[0]; l2=g_leg[1]; xSemaphoreGive(g_mtx);
  bool f1 = l1.valid && (now-l1.seenMs)<5000, f2 = l2.valid && (now-l2.seenMs)<5000;
  // ---- tier-aware stay-cost accumulator: accrue delta(kWh) x the rate active at this hour, PER LEG (R4) ----
  { int rc = effectiveRateC();
    const Leg* lg[2] = { &l1, &l2 };
    for (int i = 0; i < 2; i++) {
      const Leg& L = *lg[i];
      if (!L.valid || L.kwh <= 0.0f) continue;                        // leg not decoded this session -> contributes nothing (30A: L2 never valid)
      if (g_lastCkL[i] < 0.0f) { g_lastCkL[i] = L.kwh; continue; }    // first reading for THIS leg -> baseline, no accrual (never books its lifetime)
      if (L.kwh > g_lastCkL[i]) { double dk = (double)(L.kwh - g_lastCkL[i]);
                                  g_stayCostC += dk * rc; usageAccrue(dk, rc); g_lastCkL[i] = L.kwh; }   // stay cost + daily per-rate ledger
      else if (L.kwh < g_lastCkL[i]) g_lastCkL[i] = L.kwh;            // this leg's odometer went backwards (Watchdog reset/swap) -> re-baseline
    }
    static uint32_t lastCostSave = 0;
    if (millis() - lastCostSave > 300000) { lastCostSave = millis(); stayCostPersist(); }   // persist ~5 min (bound flash wear)
  }
  g_gL1[g_gHead] = f1 ? (uint16_t)l1.watts : 0;
  g_gL2[g_gHead] = f2 ? (uint16_t)l2.watts : 0;
  g_gA1[g_gHead] = f1 ? (uint16_t)(l1.amps*10.0f + 0.5f) : 0;   // deciamps (0.1A resolution, 0..500)
  g_gA2[g_gHead] = f2 ? (uint16_t)(l2.amps*10.0f + 0.5f) : 0;
  g_freshL1 = (g_freshL1 << 1) | (f1 ? 1ULL : 0ULL);           // mark whether this tick was a real packet
  g_freshL2 = (g_freshL2 << 1) | (f2 ? 1ULL : 0ULL);
  g_gHead = (g_gHead+1) % GRAPH_CAP;
  if (g_gCount < GRAPH_CAP) g_gCount++;
}

// 60s rolling aggregate for one leg (docs/status-spec.md). Only ticks flagged fresh count,
// so gaps shrink window_s instead of averaging in the stale zeros the ring stores during them.
static Agg computeAgg(int line) {
  const uint16_t* wR = (line == 1) ? g_gL1 : g_gL2;
  const uint16_t* aR = (line == 1) ? g_gA1 : g_gA2;
  uint64_t fresh     = (line == 1) ? g_freshL1 : g_freshL2;
  int avail = g_gCount < 60 ? g_gCount : 60;                    // <=64 so it fits the freshness mask
  long wSum = 0, wPeak = 0, aSum = 0; int aPeak = 0, n = 0;     // amps kept in deciamps until the end
  for (int k = 0; k < avail; k++) {
    if (!((fresh >> k) & 1ULL)) continue;                       // skip seconds with no real packet
    int i = ((g_gHead - 1 - k) % GRAPH_CAP + GRAPH_CAP) % GRAPH_CAP;
    long w = wR[i]; int a = aR[i];
    wSum += w; if (w > wPeak) wPeak = w;
    aSum += a; if (a > aPeak) aPeak = a;
    n++;
  }
  Agg r; r.window = n;
  if (n > 0) { r.wAvg = (wSum + n/2) / n; r.wPeak = wPeak; r.aAvg = (aSum/(float)n)/10.0f; r.aPeak = aPeak/10.0f; }
  else       { r.wAvg = r.wPeak = 0; r.aAvg = r.aPeak = 0; }
  return r;
}

// Snapshot the whole ring to SD (~7KB, cheap). Restored on boot so the graph isn't
// blank after a power cut. Whole-ring overwrite -> bounded file, no append/rotate.
// per-MAC history file: /hist_<mac12>.bin so each Watchdog keeps its OWN ring (multi-unit).
static void histPath(char* out, size_t n) {
  char mac[16]; macKey(mac, sizeof(mac));
  if (mac[0]) snprintf(out, n, "/hist_%s.bin", mac);
  else        snprintf(out, n, "%s", HIST_PATH);
}
static void graphSave() {
  if (!g_sdOk) return;
  char path[40]; histPath(path, sizeof(path));
  File f = SD_MMC.open(path, "w");
  if (!f) return;
  f.write((uint8_t*)&g_gHead, 4); f.write((uint8_t*)&g_gCount, 4);
  f.write((uint8_t*)g_gL1, sizeof(g_gL1)); f.write((uint8_t*)g_gL2, sizeof(g_gL2));
  f.write((uint8_t*)g_gA1, sizeof(g_gA1)); f.write((uint8_t*)g_gA2, sizeof(g_gA2)); // amps ring (appended)
  f.close();
}
static void graphLoad() {
  if (!g_sdOk) return;
  const int need = (int)(8 + sizeof(g_gL1) + sizeof(g_gL2));
  char path[40]; histPath(path, sizeof(path));
  File f = SD_MMC.open(path, "r");
  if ((!f || f.size() < need) && strcmp(path, HIST_PATH) != 0) {   // one-time migration from the pre-multiunit single file
    if (f) f.close();
    f = SD_MMC.open(HIST_PATH, "r");
  }
  if (!f) return;
  if (f.size() >= need) {
    f.read((uint8_t*)&g_gHead, 4); f.read((uint8_t*)&g_gCount, 4);
    f.read((uint8_t*)g_gL1, sizeof(g_gL1)); f.read((uint8_t*)g_gL2, sizeof(g_gL2));
    // amps arrays are appended; older files omit them -> the amps graph just refills live.
    if (f.size() >= (int)(8 + sizeof(g_gL1) + sizeof(g_gL2) + sizeof(g_gA1) + sizeof(g_gA2))) {
      f.read((uint8_t*)g_gA1, sizeof(g_gA1)); f.read((uint8_t*)g_gA2, sizeof(g_gA2));
    }
    if (g_gHead < 0 || g_gHead >= GRAPH_CAP)  g_gHead = 0;
    if (g_gCount < 0 || g_gCount > GRAPH_CAP) g_gCount = 0;
    Serial.printf("history: restored %d samples from SD\n", g_gCount);
  }
  f.close();
}

// ---- Event log on SD (v2.1 0.1) ---------------------------------------------------------------
// Append+rotate ring of fixed-size records, SD-persisted with a versioned header (the power-history
// ring has none). Records the transitions the higher phases mine: outage on/off, on-battery,
// BLE up/down, netmode, NTP sync, boot+reason, and audit actions (clock/rate/reset). Each record
// carries BOTH a wall-clock epoch (0 when the clock isn't confident) and a monotonic secs stamp, so
// events stay orderable across a clock jump. Appends are buffered in RAM (guarded by a spinlock, may
// be written from the web-server / SNTP-callback contexts) and flushed to SD on the 30 s graphSave
// cadence; critical events also tee to the RTC dmesg ring via logf() so a crash before the flush
// doesn't lose them. Reuses the usageSave() atomic .tmp+rename so a power cut can't shred the log.
#define EVT_MAGIC 0x45565431u                          // "EVT1"
#define EVT_CAP   256                                  // records in the ring (256 * 12 B = 3 KB on SD); wrap oldest
struct EvtRec {                                        // 12 bytes, fixed on-disk
  uint32_t epoch;                                      // wall-clock unix secs, or 0 if the clock wasn't confident
  uint32_t mono;                                       // millis()/1000 monotonic -> ordering survives a clock jump
  uint8_t  type;                                       // EvtType
  uint8_t  flags;                                      // EVF_* bits
  int16_t  arg;                                        // type-specific (reset reason / netmode / rate cents / ...)
};
static EvtRec        g_evt[EVT_CAP];
static int           g_evtHead = 0, g_evtCount = 0;    // ring: head = next write slot
static bool          g_evtDirty = false;               // unflushed records pending
static portMUX_TYPE  g_evtMux = portMUX_INITIALIZER_UNLOCKED;

static const char* evtTypeStr(uint8_t t) {
  switch (t) {
    case EV_BOOT:       return "boot";
    case EV_SHORE_LOST: return "shore_lost";
    case EV_SHORE_BACK: return "shore_back";
    case EV_ON_BATT:    return "on_batt";
    case EV_ON_LINE:    return "on_line";
    case EV_BLE_UP:     return "ble_up";
    case EV_BLE_DOWN:   return "ble_down";
    case EV_NET_MODE:   return "net_mode";
    case EV_NTP_SYNC:   return "ntp_sync";
    case EV_CLOCK_SET:  return "clock_set";
    case EV_RATE_CHG:   return "rate_chg";
    case EV_RESET_STAY: return "reset_stay";
    case EV_RESET_ODO:  return "reset_odo";
    default:            return "?";
  }
}
// Append one record. Cheap; safe from either core. The RRAM update is under a brief spinlock; the
// slower logf() mirror (Serial + RTC dmesg) runs AFTER the lock is released.
static void evtLog(uint8_t type, int16_t arg) {
  time_t now = time(nullptr);
  // Trust the wall-clock epoch ONLY when the DATE is real (NTP sync or phone-set-with-tz -> g_dateReal),
  // NEVER just because the epoch "looks recent" -- the RTC can carry a stale-but-plausible value across a
  // soft reset while provenance is LOST. Consistent with localDateNow()/the clock-confidence work. When
  // unsure, store epoch 0 and flag it; ordering then relies on the monotonic `mono` stamp.
  bool unsure = !g_dateReal || now < 1700000000;
  portENTER_CRITICAL(&g_evtMux);
  EvtRec& r = g_evt[g_evtHead];
  r.epoch = unsure ? 0 : (uint32_t)now;
  r.mono  = millis() / 1000;
  r.type  = type;
  r.flags = unsure ? EVF_CLK_UNSURE : 0;
  r.arg   = arg;
  g_evtHead = (g_evtHead + 1) % EVT_CAP;
  if (g_evtCount < EVT_CAP) g_evtCount++;
  g_evtDirty = true;
  portEXIT_CRITICAL(&g_evtMux);
  logf("evt %s arg=%d", evtTypeStr(type), (int)arg);   // breadcrumb + RTC dmesg mirror (survives a crash pre-flush)
}
static void evtReset() {                               // factory wipe: drop the in-RAM ring so it can't re-save
  portENTER_CRITICAL(&g_evtMux);
  g_evtHead = 0; g_evtCount = 0; g_evtDirty = false; memset(g_evt, 0, sizeof(g_evt));
  portEXIT_CRITICAL(&g_evtMux);
}
static void evtPath(char* out, size_t n) {
  char mac[16]; macKey(mac, sizeof(mac));
  if (mac[0]) snprintf(out, n, "/evt_%s.bin", mac);
  else        snprintf(out, n, "/evt.bin");
}
static void evtSave() {                                // .tmp then swap -> a power cut can't shred the log
  if (!g_sdOk || !g_evtDirty) return;
  char p[40], tmp[48]; evtPath(p, sizeof(p)); snprintf(tmp, sizeof(tmp), "%s.tmp", p);
  int head, count;
  portENTER_CRITICAL(&g_evtMux); head = g_evtHead; count = g_evtCount; g_evtDirty = false; portEXIT_CRITICAL(&g_evtMux);
  File f = SD_MMC.open(tmp, "w"); if (!f) { g_evtDirty = true; return; }   // reopen failed -> keep dirty, retry next cadence
  uint32_t magic = EVT_MAGIC;
  size_t n = 0;
  n += f.write((uint8_t*)&magic, 4);
  n += f.write((uint8_t*)&head, 4); n += f.write((uint8_t*)&count, 4);
  n += f.write((uint8_t*)g_evt, sizeof(g_evt));         // whole ring (a concurrent append = one torn record, acceptable for a log)
  f.close();
  if (n != 12 + sizeof(g_evt)) { SD_MMC.remove(tmp); g_evtDirty = true; return; }   // short write -> keep the old good file
  SD_MMC.remove(p); SD_MMC.rename(tmp, p);
}
static void evtLoad() {
  if (!g_sdOk) return;
  char p[40]; evtPath(p, sizeof(p));
  File f = SD_MMC.open(p, "r"); if (!f) return;
  uint32_t magic = 0;
  if ((int)f.size() >= (int)(12 + sizeof(g_evt))) {
    f.read((uint8_t*)&magic, 4);
    if (magic == EVT_MAGIC) {
      f.read((uint8_t*)&g_evtHead, 4); f.read((uint8_t*)&g_evtCount, 4);
      f.read((uint8_t*)g_evt, sizeof(g_evt));
      if (g_evtHead < 0 || g_evtHead >= EVT_CAP)  g_evtHead = 0;
      if (g_evtCount < 0 || g_evtCount > EVT_CAP) g_evtCount = 0;
      if (g_evtCount < EVT_CAP && g_evtHead != g_evtCount) { g_evtHead = 0; g_evtCount = 0; }  // un-wrapped ring must have head==count; inconsistent -> drop to empty
      Serial.printf("events: restored %d record(s) from SD\n", g_evtCount);
    }
  }
  f.close();
}
// GET /events?n=<count> -- the event ring as JSON (oldest first). Unlocked read, streamed like /usage.
void handleEvents() {
  int head, count;
  portENTER_CRITICAL(&g_evtMux); head = g_evtHead; count = g_evtCount; portEXIT_CRITICAL(&g_evtMux);
  int want = server.hasArg("n") ? server.arg("n").toInt() : count;
  if (want < 1 || want > count) want = count;
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", "");
  server.sendContent("{\"boot\":" + String((unsigned long)dmesgBootCount()) + ",\"count\":" + String(count) + ",\"events\":[");
  int start = (head - want + EVT_CAP) % EVT_CAP;
  for (int i = 0; i < want; i++) {
    EvtRec r = g_evt[(start + i) % EVT_CAP];             // copy out (a torn record during a concurrent append is acceptable here)
    String s = (i ? "," : "");
    s += "{\"t\":\"" + String(evtTypeStr(r.type)) + "\",\"epoch\":" + String((unsigned long)r.epoch) +
         ",\"mono\":" + String((unsigned long)r.mono) + ",\"arg\":" + String((int)r.arg) +
         ",\"clk\":" + String((r.flags & EVF_CLK_UNSURE) ? 0 : 1) + "}";
    server.sendContent(s);
  }
  server.sendContent("]}");
  g_lastWebMs = millis();
}

// ---- Stay Cards (v2.1 2.1): named guest sessions on SD -- keystone for receipts/continuity/CSV/audit ----
// A stay bookends a guest session with the combined lifetime meter reading + time at check-in and check-out.
// Text entry is web-only (POST /stay/open|close, apiAuthed); GET /stays lists them. At most one stay is OPEN
// (outEpoch==0) at a time. Forgot-to-check-out: /stay/open refuses while one's open (web prompts to close),
// and a stay auto-closes after ~24h of ~zero load with the Watchdog still reachable (guest left).
#define STAY_CAP   30
#define STAY_MAGIC 0x53544132u                          // "STA2" (bumped: closed-flag + rate snapshot replaced the outages field)
#define STAY_IDLE_CLOSE_MS (48u*60u*60u*1000u)          // 48h of ~zero load (Watchdog up) -> forgot-checkout auto-close (owner: 48h)
#define STAY_F_AUTO   1                                 // auto-closed
#define STAY_F_CLOSED 2                                 // checked out (NOT outEpoch==0 -- the clock may be unconfident at close, see H1 fix)
#define STAY_F_TIER   4                                 // time-of-day plan was active at close (rateSnapC is then unused)
struct StayRec {
  uint32_t id;
  char     guest[24];
  char     site[16];
  uint32_t inEpoch, outEpoch;                           // wall-clock epochs; 0 = clock wasn't trusted at the event (OPEN is flags&CLOSED, not outEpoch)
  float    kwhIn, kwhOut;                                // combined lifetime odometer (both legs) at open/close
  uint16_t rateSnapC;                                    // flat rate (cents/kWh) snapshotted at close -> receipts don't retro-reprice (M3)
  uint16_t flags;                                        // STAY_F_* bits
};
static StayRec  g_stay[STAY_CAP];
static int      g_stayHead = 0, g_stayCount = 0;         // ring; newest at (g_stayHead-1)
static uint32_t g_stayNextId = 1;
static inline bool stayIsClosed(const StayRec& s) { return (s.flags & STAY_F_CLOSED) != 0; }

// printable, no quote/backslash/angle-bracket -> safe in JSON, HTML (receipt/portal), and CSV (M1 XSS). Also drops
// leading spreadsheet-formula triggers (=+-@) so a guest name can't inject a formula into the CSV export (L6).
static void sanTxt(const char* in, char* out, size_t n) {
  size_t j = 0;
  if (in) {
    while (*in == '=' || *in == '+' || *in == '-' || *in == '@' || *in == ' ') in++;   // strip leading formula/space
    for (size_t i = 0; in[i] && j + 1 < n; i++) { char c = in[i];
      if (c >= 32 && c != '"' && c != '\\' && c != '<' && c != '>' && c != 127) out[j++] = c; }
  }
  out[j] = 0;
}
static float meterNow() {                                // combined lifetime kWh odometer, 0 if no live data
  Leg a, b; xSemaphoreTake(g_mtx, portMAX_DELAY); a = g_leg[0]; b = g_leg[1]; xSemaphoreGive(g_mtx);
  return (a.valid ? a.kwh : 0.0f) + (b.valid ? b.kwh : 0.0f);
}
static void stayPath(char* out, size_t n) {
  char mac[16]; macKey(mac, sizeof(mac));
  if (mac[0]) snprintf(out, n, "/stays_%s.bin", mac); else snprintf(out, n, "/stays.bin");
}
static void staySave() {
  if (!g_sdOk) return;
  char p[40], tmp[48]; stayPath(p, sizeof(p)); snprintf(tmp, sizeof(tmp), "%s.tmp", p);
  File f = SD_MMC.open(tmp, "w"); if (!f) return;
  uint32_t magic = STAY_MAGIC; size_t n = 0;
  n += f.write((uint8_t*)&magic, 4);
  n += f.write((uint8_t*)&g_stayHead, 4); n += f.write((uint8_t*)&g_stayCount, 4); n += f.write((uint8_t*)&g_stayNextId, 4);
  n += f.write((uint8_t*)g_stay, sizeof(g_stay));
  f.close();
  if (n != 16 + sizeof(g_stay)) { SD_MMC.remove(tmp); return; }
  SD_MMC.remove(p); SD_MMC.rename(tmp, p);
}
static void stayLoad() {
  if (!g_sdOk) return;
  char p[40]; stayPath(p, sizeof(p)); File f = SD_MMC.open(p, "r"); if (!f) return;
  uint32_t magic = 0;
  if ((int)f.size() >= (int)(16 + sizeof(g_stay))) {
    f.read((uint8_t*)&magic, 4);
    if (magic == STAY_MAGIC) {
      f.read((uint8_t*)&g_stayHead, 4); f.read((uint8_t*)&g_stayCount, 4); f.read((uint8_t*)&g_stayNextId, 4);
      f.read((uint8_t*)g_stay, sizeof(g_stay));
      if (g_stayHead < 0 || g_stayHead >= STAY_CAP)  g_stayHead = 0;
      if (g_stayCount < 0 || g_stayCount > STAY_CAP) g_stayCount = 0;
      if (g_stayCount < STAY_CAP && g_stayHead != g_stayCount) { g_stayHead = 0; g_stayCount = 0; }  // un-wrapped invariant
      if (g_stayNextId < 1) g_stayNextId = 1;
      Serial.printf("stays: restored %d\n", g_stayCount);
    }
  }
  f.close();
}
static void stayReset() { g_stayHead = 0; g_stayCount = 0; g_stayNextId = 1; memset(g_stay, 0, sizeof(g_stay)); }
// R4 (+ review findings): "meter trustworthy enough to bill." A stay's kwhIn/kwhOut is meterNow() = sum of
// currently-valid legs. Two ways this over-bills: (a) a 50A (two-leg) service read on ONE leg at check-in, then
// both at check-out, adds L2's whole lifetime; (b) a STALE sticky-valid leg (legs stay .valid for the whole uptime)
// gives a days-old baseline during a long Watchdog outage, billing the gap to the next guest. So: require FRESH
// readings (<5 s), and on a configured 50A service require BOTH legs fresh. (This is stronger + simpler than the
// old time-based settle window, which a slow-second-leg 50A unit could still slip through.)
static bool meterValid() {
  Leg a, b; xSemaphoreTake(g_mtx, portMAX_DELAY); a = g_leg[0]; b = g_leg[1]; xSemaphoreGive(g_mtx);
  uint32_t now = millis();
  bool f1 = a.valid && (now - a.seenMs) < 5000;
  bool f2 = b.valid && (now - b.seenMs) < 5000;
  if (g_cfg.svcAmps == 50) return f1 && f2;                        // two-leg service -> never bill off a single leg
  return f1 || f2;                                                 // 30A single-leg service
}
static int stayOpenIdx() {                               // index of the currently-open stay, or -1
  if (g_stayCount == 0) return -1;
  int idx = (g_stayHead - 1 + STAY_CAP) % STAY_CAP;
  return stayIsClosed(g_stay[idx]) ? -1 : idx;            // OPEN = the CLOSED flag isn't set (H1: not outEpoch==0)
}
static int stayOpen(const char* guest, const char* site) {   // new id, or 0 if one's already open
  if (stayOpenIdx() >= 0) return 0;
  StayRec& s = g_stay[g_stayHead]; memset(&s, 0, sizeof(s));
  s.id = g_stayNextId++; sanTxt(guest, s.guest, sizeof(s.guest)); sanTxt(site, s.site, sizeof(s.site));
  time_t now = time(nullptr); s.inEpoch = (g_dateReal && now >= 1700000000) ? (uint32_t)now : 0;
  s.kwhIn = meterNow();
  g_stayHead = (g_stayHead + 1) % STAY_CAP; if (g_stayCount < STAY_CAP) g_stayCount++;
  staySave(); logf("stay: OPEN #%lu '%s' @ %.3f kWh", (unsigned long)s.id, s.guest, s.kwhIn);
  return (int)s.id;
}
static int stayClose(bool autoClose) {                   // closed id, or 0 if none open
  int idx = stayOpenIdx(); if (idx < 0) return 0;
  StayRec& s = g_stay[idx];
  time_t now = time(nullptr); s.outEpoch = (g_dateReal && now >= 1700000000) ? (uint32_t)now : 0;
  s.kwhOut = meterNow();
  s.flags |= STAY_F_CLOSED; if (autoClose) s.flags |= STAY_F_AUTO;                         // H1: mark closed via the flag, never outEpoch==0
  if (g_cfg.tierCount > 0) s.flags |= STAY_F_TIER; else s.rateSnapC = (uint16_t)g_cfg.rateC; // M3: snapshot the rate at close
  staySave(); logf("stay: CLOSE #%lu %.3f kWh%s", (unsigned long)s.id, s.kwhOut - s.kwhIn, autoClose ? " (auto)" : "");
  return (int)s.id;
}
static void stayTick() {                                 // forgot-to-check-out: auto-close on sustained ~zero load (48h)
  if (g_reboot) return;                                  // finding#2: a swap/reboot is pending -> don't let this pass write the old stay into the NEW MAC's file
  static uint32_t lastLoadMs = 0; uint32_t now = millis();
  if (stayOpenIdx() < 0) { lastLoadMs = now; return; }
  Leg a, b; bool conn; xSemaphoreTake(g_mtx, portMAX_DELAY); a = g_leg[0]; b = g_leg[1]; conn = g_connected; xSemaphoreGive(g_mtx);
  float amps = (a.valid ? a.amps : 0.0f) + (b.valid ? b.amps : 0.0f);
  // M11: "guest left" = zero DRAW while power is AVAILABLE. During a real outage (shore cut, Watchdog on battery,
  // amps read ~0, or data stale) the guest may still be plugged in -- don't count that as idle. Require a fresh
  // shore reading with nominal volts. (Tradeoff: if the Watchdog is dead we never auto-close -- safer than closing
  // a guest's stay mid-outage.)
  bool  fa = a.valid && (now - a.seenMs) < 5000, fb = b.valid && (now - b.seenMs) < 5000;
  float volts = 0.0f; if (fa) volts = a.volts; if (fb && b.volts > volts) volts = b.volts;   // finding#4: only trust volts from FRESH legs (a stale sticky reading could fake shore-present mid-outage)
  bool  shorePresent = (fa || fb) && volts > 90.0f;
  if (!conn || !shorePresent || amps > 0.3f) { lastLoadMs = now; return; }   // load present, link down, or no shore -> not idle; re-arm
  if (!lastLoadMs) { lastLoadMs = now; return; }            // just became idle (e.g. booted into an idle open stay) -> arm, don't fire yet
  if (now - lastLoadMs > STAY_IDLE_CLOSE_MS) {
    if (!meterValid()) { lastLoadMs = now; return; }        // F1: don't auto-close on a partial/stale meter (would write kwhOut < kwhIn -> clamp the bill to 0); defer, same as manual close's H2 guard
    stayClose(true);                                        // 48h of ~0 A with shore present + a full meter reading -> guest left
  }
}
void handleStayOpen() {   // unlocked, like /config + /reset_stay (own-AP WPA / home-Wi-Fi is the gate; not destructive)
  if (stayOpenIdx() >= 0) { server.send(409, "text/plain", "a stay is already open -- close it first\n"); return; }
  if (!meterValid())       { server.send(409, "text/plain", "no meter reading yet -- wait for the Watchdog to connect before checking in\n"); return; }   // H2: never a 0.0 baseline
  int id = stayOpen(server.arg("guest").c_str(), server.arg("site").c_str());
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "text/plain", String("opened stay #") + id + "\n");
}
void handleStayClose() {
  if (stayOpenIdx() >= 0 && !meterValid()) { server.send(409, "text/plain", "no meter reading -- reconnect the Watchdog to check out cleanly\n"); return; }   // H2: don't overwrite kwhOut with 0
  int id = stayClose(false);
  server.sendHeader("Cache-Control", "no-store");
  server.send(id ? 200 : 409, "text/plain", id ? (String("closed stay #") + id + "\n") : "no open stay\n");
}
static void stayOutageWindow(uint32_t inE, uint32_t outE, bool closed, uint32_t* count, uint32_t* secs);   // fwd (defined below)
void handleStays() {                                     // GET /stays -- JSON, newest first. Unlocked read.
  server.setContentLength(CONTENT_LENGTH_UNKNOWN); server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", "");
  server.sendContent("{\"stays\":[");
  for (int k = 0; k < g_stayCount; k++) {
    StayRec& s = g_stay[(g_stayHead - 1 - k + STAY_CAP) % STAY_CAP];
    bool closed = stayIsClosed(s);
    float kwh = (closed ? s.kwhOut : meterNow()) - s.kwhIn; if (kwh < 0) kwh = 0;
    uint32_t oc, osec; stayOutageWindow(s.inEpoch, s.outEpoch, closed, &oc, &osec);   // L1: one consistent count (live for open stays too)
    String j = (k ? "," : "");
    j += "{\"id\":" + String(s.id) + ",\"guest\":\"" + s.guest + "\",\"site\":\"" + s.site + "\",";
    j += "\"in\":" + String(s.inEpoch) + ",\"out\":" + String(s.outEpoch) + ",\"kwh\":" + String(kwh, 3) + ",";
    j += "\"open\":" + String(closed ? 0 : 1) + ",\"outages\":" + String(oc) + ",\"auto\":" + String((s.flags & STAY_F_AUTO) ? 1 : 0) + "}";
    server.sendContent(j);
  }
  server.sendContent("]}");
  g_lastWebMs = millis();
}

// v2.1 2.4: replay SHORE_LOST/SHORE_BACK (whole 0.1 event history), clamp each outage to the stay window
// [inE,hi], and report the count + total seconds of outages that OVERLAP the stay. Replaying (vs only scanning
// in-window events) means an outage that began before check-in, or is still ongoing at check-out, is counted (L2).
// R7: a stay CLOSED while the clock was unconfident has outE==0 but is NOT open -- treat its outage window as
//     empty (else `hi=now` would grow its outage count/seconds forever off a stale RTC). Receipts already dodge
//     this via the dur==0 branch; this makes /stays + CSV agree.
// R3: EV_BOOT fences any OPEN outage. If a SHORE_LOST had no matching BACK before a reboot (OTA/crash/power-cut/
//     net-mode switch) the interval used to run to `hi` -> receipts printed "0-12%" for perfect-power stays, and a
//     second LOST was swallowed (`if(!out)`) so one interval could span days. We can't trust duration across a
//     restart (and the boot record's epoch is usually 0 anyway), so we DROP the partial interval at the boot and
//     let post-boot LOST/BACK edges rebuild the picture. (Boot-DURING-outage with a dead Watchdog is unobservable
//     -- no link, no data -- so it can still read ~100%; documented, not fabricated.)
static void stayOutageWindow(uint32_t inE, uint32_t outE, bool closed, uint32_t* count, uint32_t* secs) {
  *count = 0; *secs = 0; if (!inE) return;
  if (closed && !outE) return;                           // R7: closed-but-undated -> no reliable window
  uint32_t hi = outE ? outE : (uint32_t)time(nullptr);
  bool out = false; uint32_t outStart = 0;
  portENTER_CRITICAL(&g_evtMux);
  for (int k = 0; k < g_evtCount; k++) {                 // oldest-first
    EvtRec& r = g_evt[(g_evtHead - g_evtCount + k + 2 * EVT_CAP) % EVT_CAP];
    if (r.type == EV_BOOT) { out = false; continue; }    // R3: a reboot fences the open interval (handled before the epoch skip -- boot epoch is often 0)
    if (!r.epoch) continue;                              // clock-unconfident non-boot event -> can't place it
    if      (r.type == EV_SHORE_LOST) { if (!out) { out = true; outStart = r.epoch; } }
    else if (r.type == EV_SHORE_BACK && out) { out = false;
      uint32_t a = outStart > inE ? outStart : inE, b = r.epoch < hi ? r.epoch : hi;   // clamp to [inE,hi]
      if (b > a) { *secs += (b - a); (*count)++; }
    }
  }
  if (out) { uint32_t a = outStart > inE ? outStart : inE; if (hi > a) { *secs += (hi - a); (*count)++; } }   // still out at stay end
  portEXIT_CRITICAL(&g_evtMux);
}
static bool stayRateChanged(uint32_t inE, uint32_t outE, bool closed) {   // v2.1 2.7: did the rate/plan change mid-stay?
  if (!inE) return false;
  if (closed && !outE) return false;   // finding#3: closed-but-undated -> hi=now would sweep in rate changes made AFTER checkout (mirrors the R7 guard in stayOutageWindow)
  uint32_t hi = outE ? outE : (uint32_t)time(nullptr); bool chg = false;
  portENTER_CRITICAL(&g_evtMux);
  for (int k = 0; k < g_evtCount; k++) { EvtRec& r = g_evt[(g_evtHead - 1 - k + EVT_CAP) % EVT_CAP];
    if (r.type == EV_RATE_CHG && r.epoch && r.epoch >= inE && r.epoch <= hi) { chg = true; break; } }   // M8: a clock-unconfident (epoch 0) rate change can't be placed in the stay window -> don't claim a mid-stay change
  portEXIT_CRITICAL(&g_evtMux);
  return chg;
}
// GET /receipt?id=N -- a self-contained, printable statement for one stay. Unlocked read. Energy = the meter
// delta (exact); flat-rate stays get a $ total, time-of-day plans point to the Usage tab (no policy engine, 2.3
// skipped). Includes a power-continuity summary (2.4) + a mid-stay rate-change note (2.7).
void handleReceipt() {
  int id = server.hasArg("id") ? server.arg("id").toInt() : 0;
  int idx = -1;
  for (int k = 0; k < g_stayCount; k++) { int i = (g_stayHead - 1 - k + STAY_CAP) % STAY_CAP; if ((int)g_stay[i].id == id) { idx = i; break; } }
  if (idx < 0) { server.send(404, "text/html", "<!doctype html><meta charset=utf-8><h2>No such stay</h2>"); return; }
  StayRec s = g_stay[idx];
  bool closed = stayIsClosed(s);
  float kwh = (closed ? s.kwhOut : meterNow()) - s.kwhIn; if (kwh < 0) kwh = 0;
  bool flat = closed ? !(s.flags & STAY_F_TIER) : (g_cfg.tierCount == 0);
  int  rateC = closed ? (int)s.rateSnapC : g_cfg.rateC;            // M3: closed stays bill the rate snapshotted at close, not the current one
  long costC = flat ? (long)(kwh * rateC + 0.5) : -1;
  uint32_t oc, osec; stayOutageWindow(s.inEpoch, s.outEpoch, closed, &oc, &osec);
  uint32_t dur = (s.inEpoch && s.outEpoch) ? (s.outEpoch - s.inEpoch) : 0;
  float avail = (dur > 0) ? (100.0f * (float)(dur > osec ? dur - osec : 0) / (float)dur) : 100.0f;
  char tin[24], tout[24];                                          // L5: label undated events "undated", not "open"
  if (s.inEpoch)  { time_t t = s.inEpoch;  struct tm m; localtime_r(&t, &m); strftime(tin,  sizeof(tin),  "%Y-%m-%d %H:%M", &m); } else strcpy(tin, "undated");
  if (s.outEpoch) { time_t t = s.outEpoch; struct tm m; localtime_r(&t, &m); strftime(tout, sizeof(tout), "%Y-%m-%d %H:%M", &m); } else strcpy(tout, closed ? "undated" : "open");
  String h = "<!doctype html><meta charset=utf-8><meta name=viewport content=\"width=device-width,initial-scale=1\">";
  h += "<title>Statement #" + String(s.id) + "</title><style>";
  h += "body{font:14px/1.55 -apple-system,Segoe UI,Roboto,sans-serif;color:#111;background:#fff;max-width:560px;margin:24px auto;padding:0 18px}";
  h += "h1{font-size:20px;margin:0 0 2px}.sub{color:#666;margin-bottom:18px}table{width:100%;border-collapse:collapse;margin:10px 0}";
  h += "td,th{padding:7px 4px;border-bottom:1px solid #ddd;text-align:left}td:last-child,th:last-child{text-align:right}";
  h += ".tot td{font-weight:700;border-top:2px solid #000;border-bottom:none}.note{color:#555;font-size:12px;margin-top:6px}";
  h += ".btn{display:inline-block;margin:16px 0;padding:9px 16px;border:1px solid #888;border-radius:6px;background:#f4f4f4;cursor:pointer;font-size:14px}";
  h += "@media print{.btn{display:none}body{margin:0}}</style>";
  h += "<h1>" + (g_cfg.nick.length() ? g_cfg.nick : String("Shore Power")) + " &mdash; usage statement</h1>";
  h += "<div class=sub>Stay #" + String(s.id);
  if (s.guest[0]) h += " &middot; " + String(s.guest);
  if (s.site[0])  h += " &middot; " + String(s.site);
  h += "</div><table>";
  h += "<tr><td>Checked in</td><td>" + String(tin) + "</td></tr>";
  h += "<tr><td>Checked out</td><td>" + String(tout) + (s.flags & STAY_F_AUTO ? " (auto)" : "") + "</td></tr>";
  h += "<tr><td>Meter at check-in</td><td>" + String(s.kwhIn, 3) + " kWh</td></tr>";
  h += "<tr><td>Meter at check-out</td><td>" + (closed ? String(s.kwhOut, 3) + " kWh" : String("&mdash;")) + "</td></tr>";
  h += "<tr><td>Energy used</td><td>" + String(kwh, 3) + " kWh</td></tr>";
  if (flat) {
    h += "<tr><td>Rate</td><td>$" + String(rateC / 100.0, 2) + " / kWh</td></tr>";
    h += "<tr class=tot><td>Total</td><td>$" + String(costC / 100.0, 2) + "</td></tr></table>";
  } else {
    h += "<tr><td>Rate</td><td>time-of-day plan</td></tr></table>";
    // M7: DON'T point the guest at the owner's Usage tab -- those daily rows include other guests' energy and aren't a per-stay bill.
    h += "<div class=note>Billed on a time-of-day plan &mdash; your host will total this stay's " + String(kwh, 3) + " kWh at the applicable rates.</div>";
  }
  // M8: this statement is the authority for the stay (exact meter delta). The same energy also lands in the daily
  // usage ledger, so say plainly which to invoice from -- otherwise a host could bill the stay twice.
  h += "<div class=note>This statement is the authoritative bill for the stay (measured directly at the meter). Its energy is already reflected in daily usage &mdash; invoice from this statement <b>or</b> daily usage, not both.</div>";
  // power continuity (2.4)
  if (dur == 0) h += "<div class=note>Power continuity: not available (stay still open or undated).</div>";
  else if (oc == 0) h += "<div class=note><b>Power available 100%</b> &mdash; no interruptions recorded.</div>";   // finding#6: an outage spanning a bridge reboot is unobservable -> attest what we RECORDED, don't assert an absolute
  else h += "<div class=note><b>Power available " + String(avail, 1) + "%</b> &mdash; " + String(oc) + " interruption" + (oc > 1 ? "s" : "") + " totalling " + String(osec / 60) + " min. Your Watchdog kept guard.</div>";
  if (stayRateChanged(s.inEpoch, s.outEpoch, closed)) h += "<div class=note>Note: the electricity rate changed during this stay.</div>";
  h += "<div class=note>Metered by an Excelsior Shore Power Bridge.</div>";
  h += "<button class=btn onclick=\"window.print()\">Print / Save PDF</button>";
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "text/html", h);
  g_lastWebMs = millis();
}
// GET /stays.csv -- bookkeeping export (v2.1 2.6). The header row is a VERSIONED CONTRACT (host spreadsheets
// depend on column order/names -- append new columns at the end, never reorder). guest/site are pre-sanitized
// (no quotes) so plain quoting is CSV-safe. Unlocked read.
void handleStaysCsv() {
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.sendHeader("Content-Disposition", "attachment; filename=stays.csv");
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "text/csv", "");
  server.sendContent("id,guest,site,checkin_epoch,checkout_epoch,kwh,rate_c,cost_usd,outages,outage_min,auto_closed\n");
  for (int k = 0; k < g_stayCount; k++) {
    StayRec& s = g_stay[(g_stayHead - 1 - k + STAY_CAP) % STAY_CAP];
    bool closed = stayIsClosed(s);
    float kwh = (closed ? s.kwhOut : meterNow()) - s.kwhIn; if (kwh < 0) kwh = 0;
    uint32_t oc, osec; stayOutageWindow(s.inEpoch, s.outEpoch, closed, &oc, &osec);
    bool flat = closed ? !(s.flags & STAY_F_TIER) : (g_cfg.tierCount == 0);
    int  rc = closed ? (int)s.rateSnapC : g_cfg.rateC;   // M3: rate snapshotted at close
    String r = String(s.id) + ",\"" + s.guest + "\",\"" + s.site + "\"," + String(s.inEpoch) + "," + String(s.outEpoch) + ","
             + String(kwh, 3) + "," + String(flat ? rc : 0) + "," + (flat ? String(kwh * rc / 100.0, 2) : String("")) + ","
             + String(oc) + "," + String(osec / 60) + "," + String((s.flags & STAY_F_AUTO) ? 1 : 0) + "\n";
    server.sendContent(r);
  }
  g_lastWebMs = millis();
}

// Delete ALL power-history files from SD -- every unit's /hist_<mac>.bin + the legacy /reactor_hist.bin.
// Called by the factory reset so a wiped/re-shipped unit doesn't carry the previous owner's graph history.
// Collects names first (don't remove while iterating the dir), then deletes. Cap 16 = plenty of units.
static void wipeHistory() {
  if (!g_sdOk) return;
  File root = SD_MMC.open("/");
  if (!root) return;
  String del[64]; int nd = 0;                          // L4: 4 file families (hist/usage/evt/stays) x .bin/.tmp x several units
  for (File f = root.openNextFile(); f && nd < 64; f = root.openNextFile()) {
    bool dir = f.isDirectory(); String n = f.name(); f.close();
    if (dir) continue;
    int slash = n.lastIndexOf('/'); String base = (slash >= 0) ? n.substring(slash + 1) : n;
    if (base.startsWith("hist_") || base == "reactor_hist.bin" ||
        base.startsWith("usage_") || base == "usage.bin" || base == "usage.bin.tmp" ||   // + the 30-day billing ledger
        base.startsWith("evt_")   || base == "evt.bin"   || base == "evt.bin.tmp" ||     // + the event log (per-MAC .tmp caught by the prefix)
        base.startsWith("stays_") || base == "stays.bin" || base == "stays.bin.tmp")     // + Stay Cards
      del[nd++] = "/" + base;
  }
  root.close();
  for (int i = 0; i < nd; i++) { SD_MMC.remove(del[i]); logf("factory: removed SD %s", del[i].c_str()); }
  g_dayCount = 0; g_dayHead = 0; g_dayCur = 0; memset(g_day, 0, sizeof(g_day));   // drop the in-RAM ledger too, so it can't re-save
  evtReset();                                                                     // and the in-RAM event ring
  stayReset();                                                                    // and the Stay Cards ring
}

// GET /history?range=5|10|30&mode=w|a -- downsample the RAM ring to N buckets over the last
// `range` minutes for the web chart. Bucket 0 = oldest edge of the window, N-1 = now; a bucket
// with no samples yet (early after boot) is emitted null so the line starts where data begins.
// Core-1 only (runs in loop(), like graphSample/handleStatus) -> no mutex. Additive; the
// on-device GRAPHS page + its own g_graphMin/g_graphMode are untouched.
void handleHistory() {
  int range = server.hasArg("range") ? server.arg("range").toInt() : 30;
  if (range != 5 && range != 10 && range != 30) range = 30;
  bool amps = server.hasArg("mode") && server.arg("mode") == "a";
  const uint16_t* d1 = amps ? g_gA1 : g_gL1;
  const uint16_t* d2 = amps ? g_gA2 : g_gL2;
  static const int N = 90;
  const int win = range * 60;
  int avail = g_gCount < win ? g_gCount : win;
  static long s1[N], s2[N]; static int cnt[N];        // static (single-threaded) -> off the stack
  for (int b = 0; b < N; b++) { s1[b] = 0; s2[b] = 0; cnt[b] = 0; }
  for (int k = 0; k < avail; k++) {                    // k = seconds ago (0 = newest)
    int b = N - 1 - (k * N) / win; if (b < 0) b = 0; if (b >= N) b = N - 1;
    int i = ((g_gHead - 1 - k) % GRAPH_CAP + GRAPH_CAP) % GRAPH_CAP;
    s1[b] += d1[i]; s2[b] += d2[i]; cnt[b]++;
  }
  String l1 = "", l2 = "";
  for (int b = 0; b < N; b++) {
    if (b) { l1 += ","; l2 += ","; }
    if (!cnt[b])   { l1 += "null"; l2 += "null"; }
    else if (amps) { l1 += String((s1[b] / (float)cnt[b]) / 10.0f, 1); l2 += String((s2[b] / (float)cnt[b]) / 10.0f, 1); }
    else           { l1 += String((long)(s1[b] / cnt[b]));            l2 += String((long)(s2[b] / cnt[b])); }
  }
  String body = "{\"range_min\":" + String(range) + ",\"mode\":\"" + (amps ? "a" : "w") +
                "\",\"n\":" + String(N) + ",\"l1\":[" + l1 + "],\"l2\":[" + l2 + "]}";
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", body);
}

// POST /config -- set any of nick / rate_c (cents/kWh) / svc (30|50) / bright (10..100). Query params
// like /netmode; any subset applied+persisted. UNLOCKED by design (2026-08-11): the own-AP already needs
// the key (it's the WPA2 password) and home-Wi-Fi implies physical access. Optional PIN = a future add.
// GET /screenshot -- read the TFT framebuffer back over SPI and stream it as a 24-bit BMP. This is the test
// of whether THIS ILI9341V panel supports read-back (MISO wired + SPI_READ_FREQUENCY sane): a real image ->
// yes; all-black/garbage -> no (write-only panel). Runs in loop() on core 1, same as the UI (no TFT contention).
void handleScreenshot() {
  const int W = 240, H = 320;
  const uint32_t dataSize = (uint32_t)W * 3 * H, fileSize = 54 + dataSize;
  uint8_t hdr[54]; memset(hdr, 0, sizeof(hdr));
  hdr[0]='B'; hdr[1]='M';
  hdr[2]=fileSize; hdr[3]=fileSize>>8; hdr[4]=fileSize>>16; hdr[5]=fileSize>>24;
  hdr[10]=54; hdr[14]=40;
  hdr[18]=W & 0xFF; hdr[19]=(W>>8)&0xFF;
  hdr[22]=H & 0xFF; hdr[23]=(H>>8)&0xFF;             // positive height -> rows stored bottom-up
  hdr[26]=1; hdr[28]=24;
  hdr[34]=dataSize; hdr[35]=dataSize>>8; hdr[36]=dataSize>>16; hdr[37]=dataSize>>24;
  server.setContentLength(fileSize);
  server.send(200, "image/bmp", "");
  server.sendContent((const char*)hdr, 54);
  static uint16_t line[W];
  uint8_t rgb[W*3];
  for (int y = H - 1; y >= 0; y--) {
    tft.readRect(0, y, W, 1, line);
    for (int x = 0; x < W; x++) {
      uint16_t p = line[x]; uint16_t c = (uint16_t)((p >> 8) | (p << 8));        // readRect returns byte-swapped RGB565 -> swap back
      uint8_t r=(c>>11)&0x1F, g=(c>>5)&0x3F, b=c&0x1F;
      rgb[x*3+0]=(uint8_t)((b<<3)|(b>>2)); rgb[x*3+1]=(uint8_t)((g<<2)|(g>>4)); rgb[x*3+2]=(uint8_t)((r<<3)|(r>>2));   // BGR
    }
    server.sendContent((const char*)rgb, W*3);
  }
}

// Does NOT redraw the TFT (could be on another page) -- the reactor's 1Hz loop repaints stay/$ on its own.
void handleConfig() {
  bool any = false, brightChanged = false;
  int      oldRateC = g_cfg.rateC; uint8_t oldTierCount = g_cfg.tierCount;   // snapshot to detect an ACTUAL rate change (vs a form re-POST)
  RateTier oldTiers[MAX_TIERS]; memcpy(oldTiers, g_cfg.tiers, sizeof(oldTiers));
  if (server.hasArg("nick")) {
    String n = server.arg("nick"), c = "";                 // sanitize -> printable, no quote/backslash, <=24
    for (size_t i = 0; i < n.length() && c.length() < 24; i++) {
      char ch = n[i]; if (ch >= 32 && ch != '"' && ch != '\\' && ch != 127) c += ch;
    }
    c.trim(); g_cfg.nick = c; any = true;
  }
  if (server.hasArg("rate_c")) { int r = server.arg("rate_c").toInt(); g_cfg.rateC = r < 0 ? 0 : (r > 999 ? 999 : r); any = true; }
  if (server.hasArg("svc"))    { g_cfg.svcAmps = (server.arg("svc").toInt() == 30) ? 30 : 50; any = true; }
  if (server.hasArg("bright")) { int b = server.arg("bright").toInt(); g_cfg.bright = b < 10 ? 10 : (b > 100 ? 100 : b); brightChanged = true; any = true; }
  if (server.hasArg("tz"))     { int t = tzClamp(server.arg("tz").toInt()); if (t != 0 || !tzConfigured()) { g_cfg.tzIndex = t; applyTz(); } any = true; }   // US timezone picker (finding#2: a default "Not set"=0 must not clobber a configured zone)
  if (server.hasArg("tiers")) {                        // time-of-day rate schedule: "flat" or "s-e-c,s-e-c,..."
    String tv = server.arg("tiers");
    if (tv == "flat") { g_cfg.tierCount = 0; any = true; }
    else {
      RateTier tmp[MAX_TIERS]; int n = tiersParse(tv, tmp);
      if (n < 1) { server.send(400, "text/plain", "bad tiers (use start-end-cents,... or 'flat')\n"); return; }
      int bad; if (!tiersCover24(tmp, n, &bad)) { char m[52]; snprintf(m, sizeof(m), "tiers must cover all 24h (hour %d)\n", bad); server.send(400, "text/plain", m); return; }
      for (int i = 0; i < n; i++) g_cfg.tiers[i] = tmp[i]; g_cfg.tierCount = (uint8_t)n; any = true;
    }
  }
  if (!any) { server.send(400, "text/plain", "no settings in request\n"); return; }
  configPersist();
  bool rateChanged = (g_cfg.rateC != oldRateC) || (g_cfg.tierCount != oldTierCount) ||   // log only a REAL rate/schedule change
                     (g_cfg.tierCount > 0 && memcmp(g_cfg.tiers, oldTiers, g_cfg.tierCount * sizeof(RateTier)) != 0);
  if (rateChanged) evtLog(EV_RATE_CHG, g_cfg.tierCount ? (int16_t)(-g_cfg.tierCount) : (int16_t)g_cfg.rateC);   // arg: flat -> cents (>=0); tiers -> -count
  if (brightChanged) applyBright();
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "text/plain", "ok\n");
}

// POST /settime?epoch=<unix>&tz=<index> -- set the wall clock (and optionally zone) from the phone. This is the
// reliable clock path when the box is on a Wi-Fi with no internet, so SNTP can never sync (own-AP, dead-WAN campground
// router). Marks the clock MANUAL; SNTP still upgrades it to NTP later if real internet returns. Unlocked (like /config).
void handleSetTime() {
  bool any = false, clockSet = false;
  if (server.hasArg("tz")) {                              // M5: don't let a default/"Not set" (index 0) select CLOBBER a zone that's already configured
    int t = tzClamp(server.arg("tz").toInt());
    if (t != 0 || !tzConfigured()) { g_cfg.tzIndex = t; applyTz(); any = true; }
  }
  if (server.hasArg("epoch")) {
    time_t e = (time_t) strtoul(server.arg("epoch").c_str(), nullptr, 10);
    if (e <= 1700000000 || e >= 4000000000) { server.send(400, "text/plain", "epoch out of range (unix seconds, 2023..2096)\n"); return; }
    struct timeval tv = { e, 0 }; settimeofday(&tv, nullptr);
    g_clkProv = CLK_MANUAL; g_epochReal = true;           // phone sends a real epoch
    any = true; clockSet = true;
  }
  if (!any) { server.send(400, "text/plain", "need epoch (unix seconds) and/or tz index\n"); return; }
  recomputeDateReal();                                    // M5: real DATE = real epoch AND a zone -> setting EITHER can complete it (dates a previously-undated ledger when the zone is picked afterwards)
  configPersist();
  if (clockSet) evtLog(EV_CLOCK_SET, (int16_t)g_cfg.tzIndex);   // phone-set clock (audit: gates billing dates)
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "text/plain", (clockSet && !tzConfigured()) ? "clock set -- pick a time zone so daily billing can be dated\n" : "clock set\n");
}

// GET /usage -- the 30-day per-rate ledger as JSON (oldest day first). Unlocked read. Feeds the web billing view.
void handleUsage() {
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", "");
  server.sendContent("{\"clk_prov\":" + String((int)g_clkProv) + ",\"date_real\":" + String((int)g_dateReal) + ",\"days\":[");
  for (int k = 0; k < g_dayCount; k++) {
    int idx = ((g_dayHead - (g_dayCount - 1) + k) % USAGE_DAYS + USAGE_DAYS) % USAGE_DAYS;
    DayRec& d = g_day[idx];
    String s = (k ? "," : "");
    s += "{\"date\":" + String(d.date) + ",\"kwh\":" + String(d.totalKwh, 3) +
         ",\"cost_c\":" + String((long)(d.costC + 0.5)) + ",\"rates\":[";
    bool first = true;
    for (int i = 0; i < MAX_TIERS; i++) if (d.kwh[i] > 0.0f) {
      if (!first) s += ","; first = false;
      int rc = (int)(d.bcostC[i] / d.kwh[i] + 0.5);                     // derived cents/kWh for this bucket (exact cost kept separately)
      s += "{\"c\":" + String(rc) + ",\"kwh\":" + String(d.kwh[i], 3) + ",\"cost_c\":" + String((long)(d.bcostC[i] + 0.5)) + "}";
    }
    s += "]}"; server.sendContent(s);
  }
  server.sendContent("]}");
  g_lastWebMs = millis();
}

// POST /reset_stay -- re-baseline the STAY kWh tally to now (fresh site). Unlocked (a local counter;
// the Watchdog odometer + the Pi's authoritative baseline are untouched). Same math as SETTINGS reset.
void handleResetStay() {
  g_cfg.kwhBase += stayKwh(); configPersist(); baselinePersist(); stayCostReset();
  evtLog(EV_RESET_STAY, 0);                           // audit: stay tally re-baselined (web)
  server.send(200, "text/plain", "stay reset\n");
}

// POST /wifi?ssid=...&pass=... -- change home Wi-Fi from the dashboard (no setup-portal round trip).
// Saves creds, switches to HOME, and REBOOTS to join. Reboot (vs live re-join) is deliberate: on a
// wrong password, netBringUp falls back to hosting Hughes-Bridge-XXXX, so a fat-fingered password is
// still recoverable instead of stranding the unit off-network. Unlocked, like the other web writes.
void handleWifi() {
  if (!server.hasArg("ssid") || !server.arg("ssid").length()) {
    server.send(400, "text/plain", "ssid required\n"); return;
  }
  g_cfg.wifiSsid = server.arg("ssid");
  g_cfg.wifiPass = server.arg("pass");           // may be empty (open network)
  g_cfg.netMode  = 0;                            // HOME
  configPersist();
  logf("wifi: set '%s' via web -> reboot to join", g_cfg.wifiSsid.c_str());
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "text/plain", "ok -- rebooting to join\n");
  delay(400);                                    // let the response flush before the radio drops
  ESP.restart();
}

// replot the chart (~1Hz). TOTAL=white, L1=amber, L2=cyan. Rendered into an off-screen
// sprite and pushed whole -> no flash (falls back to direct-draw if the sprite failed).
// PEAK + AVG combined-watts for the selected window, in the free band under the chart.
// Padded draws overwrite each field's own box (no fillRect blank -> no flash), like the
// reactor numbers. Recomputed at 1Hz and on range change, so it re-reads when you tap 5/10/30.
static void drawGraphStats(long peak, long avg, bool amps) {
  char v[20]; tft.setTextDatum(TL_DATUM);
  tft.setTextColor(C_DIM, C_BG);   tft.drawString("PK", 6, 277, 1);
  tft.setTextColor(C_AMBER, C_BG);
  if (amps) snprintf(v,sizeof(v),"%.1f A", peak/10.0f); else snprintf(v,sizeof(v),"%.2f kW", peak/1000.0f);
  tft.setTextPadding(84); tft.drawString(v, 24, 274, 2);
  tft.setTextColor(C_DIM, C_BG);   tft.drawString("AVG", 126, 277, 1);
  tft.setTextColor(C_TEXT, C_BG);
  if (amps) snprintf(v,sizeof(v),"%.1f A", avg/10.0f); else snprintf(v,sizeof(v),"%.2f kW", avg/1000.0f);
  tft.setTextPadding(84); tft.drawString(v, 152, 274, 2);
  tft.setTextPadding(0);
}

// Y-axis tick label: watts print raw ("2000"); amps print whole amps (deciamps/10 -> "30").
static void graphYLabel(char* ss, size_t n, long v, bool amps) {
  if (amps) snprintf(ss, n, "%ld", v/10); else snprintf(ss, n, "%ld", v);
}

static void graphPlot() {
  bool amps = (g_graphMode == 1);                    // which ring/units this page graphs
  const uint16_t* d1 = amps ? g_gA1 : g_gL1;
  const uint16_t* d2 = amps ? g_gA2 : g_gL2;
  long step = amps ? 100 : 1000;                     // Y rounds up to 10A (deciamps) or 1kW
  int win = g_graphMin*60, avail = g_gCount<win ? g_gCount : win;
  long yMax = step, peak = 0, sum = 0;               // one pass: Y-scale + PEAK + AVG all fall out
  for (int k=0;k<avail;k++){ int i=((g_gHead-1-k)%GRAPH_CAP+GRAPH_CAP)%GRAPH_CAP; long c=(long)d1[i]+d2[i];
    if(c>yMax)yMax=c; if(c>peak)peak=c; sum+=c; }
  long avg = avail ? sum/avail : 0;
  yMax = ((yMax+step-1)/step)*step;                 // round up to a whole tick
  int W=GX1-GX0, H=GY1-GY0;
  auto gidx=[&](int p){ return ((g_gHead-avail+p)%GRAPH_CAP+GRAPH_CAP)%GRAPH_CAP; };

  if (g_chartOk) {
    TFT_eSprite& s = g_chartSpr;
    s.fillSprite(PAL_BG);
    for (int g=0; g<=2; g++){ int y=(H-1)-(int)((long)(yMax*g/2)*(H-1)/yMax); s.drawFastHLine(0,y,W,PAL_LINE); }
    s.drawFastVLine(W/2, 0, H, PAL_LINE);
    if (avail >= 2) {
      auto gx=[&](int p){ return (W-1)-(int)((long)(avail-1-p)*(W-1)/(win-1)); };  // right-align "now"
      auto gy=[&](long v){ return (H-1)-(int)(v*(H-1)/yMax); };
      for (int p=1;p<avail;p++){ int i0=gidx(p-1),i1=gidx(p),x0=gx(p-1),x1=gx(p);
        s.drawLine(x0,gy(d1[i0]),               x1,gy(d1[i1]),                PAL_AMBER);
        s.drawLine(x0,gy(d2[i0]),               x1,gy(d2[i1]),                PAL_CYAN);
        s.drawLine(x0,gy((long)d1[i0]+d2[i0]),  x1,gy((long)d1[i1]+d2[i1]),   PAL_TOTAL);
      }
    }
    s.pushSprite(GX0, GY0);
    tft.setTextDatum(MR_DATUM); tft.setTextColor(C_DIM, C_BG); tft.setTextPadding(30);  // Y labels (padded)
    for (int g=0; g<=2; g++){ long v=yMax*g/2; int y=GY1-(int)(v*H/yMax); char ss[8]; graphYLabel(ss,sizeof(ss),v,amps); tft.drawString(ss, GX0-3, y, 1); }
    tft.setTextPadding(0); tft.setTextDatum(TL_DATUM);
    drawGraphStats(peak, avg, amps);
    return;
  }

  // fallback: direct draw (flashes)
  tft.fillRect(GX0-1, GY0-2, W+3, H+5, C_BG);
  tft.setTextDatum(MR_DATUM); tft.setTextColor(C_DIM, C_BG);
  for (int g=0; g<=2; g++){ long v=yMax*g/2; int y=GY1-(int)(v*H/yMax); tft.drawFastHLine(GX0,y,W,C_LINE); char s[8]; graphYLabel(s,sizeof(s),v,amps); tft.drawString(s,GX0-3,y,1); }
  tft.drawFastVLine((GX0+GX1)/2, GY0, H, C_LINE);
  tft.setTextDatum(TL_DATUM);
  if (avail < 2) { drawGraphStats(peak, avg, amps); return; }
  auto gx=[&](int p){ return GX1-(int)((long)(avail-1-p)*W/(win-1)); };
  auto gy=[&](long v){ return GY1-(int)(v*H/yMax); };
  for (int p=1;p<avail;p++){ int i0=gidx(p-1),i1=gidx(p),x0=gx(p-1),x1=gx(p);
    tft.drawLine(x0,gy(d1[i0]),               x1,gy(d1[i1]),                C_AMBER);
    tft.drawLine(x0,gy(d2[i0]),               x1,gy(d2[i1]),                C_CYAN);
    tft.drawLine(x0,gy((long)d1[i0]+d2[i0]),  x1,gy((long)d1[i1]+d2[i1]),   C_TEXT);
  }
  drawGraphStats(peak, avg, amps);
}

static void graphsDraw() {
  tft.fillScreen(C_BG); tft.setTextDatum(TL_DATUM);
  tft.setTextColor(C_CYAN, C_BG); tft.drawString(g_graphMode ? "CURRENT  A" : "POWER  W", 6, 6, 2);
  if (!g_sdOk) { tft.setTextColor(C_DIM, C_BG); tft.setTextDatum(TR_DATUM); tft.drawString("no SD", 234, 8, 1); tft.setTextDatum(TL_DATUM); }
  btn(R_G5,  "5m",  g_graphMin==5 ?C_CYAN:C_LINE, g_graphMin==5 ?C_CYAN:C_DIM, 2);
  btn(R_G10, "10m", g_graphMin==10?C_CYAN:C_LINE, g_graphMin==10?C_CYAN:C_DIM, 2);
  btn(R_G30, "30m", g_graphMin==30?C_CYAN:C_LINE, g_graphMin==30?C_CYAN:C_DIM, 2);
  graphPlot();
  tft.setTextColor(C_DIM, C_BG); char xl[8];         // X-axis: time-ago labels
  tft.setTextDatum(TL_DATUM); snprintf(xl,sizeof(xl),"-%dm", g_graphMin);   tft.drawString(xl, GX0, 244, 1);
  tft.setTextDatum(TC_DATUM); snprintf(xl,sizeof(xl),"-%dm", g_graphMin/2); tft.drawString(xl, (GX0+GX1)/2, 244, 1);
  tft.setTextDatum(TR_DATUM); tft.drawString("now", GX1, 244, 1);
  tft.setTextDatum(TL_DATUM);
  int ly = 258;                                      // legend
  tft.fillRect(8,ly,12,6,C_TEXT);   tft.setTextColor(C_DIM,C_BG); tft.drawString("TOTAL", 24, ly-2, 1);
  tft.fillRect(88,ly,12,6,C_AMBER); tft.drawString("L1", 104, ly-2, 1);
  tft.fillRect(140,ly,12,6,C_CYAN); tft.drawString("L2", 156, ly-2, 1);
  drawNav();
}

// ---------------------------------------------------------------- audible trip alarm (ES8311 + I2S)
// Integrated from the validated -e audiotest. Codec on the SHARED Wire bus (with touch),
// I2S out on 4/5/7/8, amp-enable GPIO1 active-low, MCLK 256*fs to match ESP_I2S. Default
// OFF (no speaker in the enclosure yet); SETTINGS->AUDIO turns it on + has a TEST button.
// Ship-gate for the audible-alarm feature. FALSE = the AUDIO settings page is NOT
// shipped (dropped from the SETTINGS tab cycle) and the codec/alarm never init --
// because no speaker fits the current enclosure yet, and a toggle that can't make
// sound is a false sense of protection. Flip to true once a speaker is fitted.
static constexpr bool ALARM_PAGE = false;
#define ALARM_FRAC   0.9f              // alarm at this fraction of the per-unit service rating (svcAmps)
#define AMP_EN_PIN   1
#define AMP_ON_LVL   LOW
static I2SClass        g_i2s;
static es8311_handle_t g_es      = nullptr;
static bool            g_audioOk = false;

static bool audioSetup() {
  g_es = es8311_create(I2C_NUM_0, ES8311_ADDRESS_0);            // Wire is already up (after ts.begin)
  if (!g_es) return false;
  es8311_clock_config_t clk = {};
  clk.mclk_from_mclk_pin = true; clk.mclk_frequency = 16000 * 256; clk.sample_frequency = 16000;
  if (es8311_init(g_es, &clk, ES8311_RESOLUTION_16, ES8311_RESOLUTION_16) != ESP_OK) return false;
  es8311_voice_volume_set(g_es, 80, nullptr);
  pinMode(AMP_EN_PIN, OUTPUT); digitalWrite(AMP_EN_PIN, !AMP_ON_LVL);        // amp off until sounding
  g_i2s.setPins(5, 7, 8, -1, 4);                                            // bclk, ws, dout, din(none), mclk
  return g_i2s.begin(I2S_MODE_STD, 16000, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO);
}

static void audioTone(float hz, uint32_t ms, float amp) {
  const int N = 256; int16_t buf[N];
  uint32_t total = (uint32_t)((uint64_t)16000 * ms / 1000);
  float ph = 0.0f, step = 2.0f * PI * hz / 16000.0f;
  for (uint32_t done = 0; done < total; ) {
    int n = (total - done) < (uint32_t)N ? (int)(total - done) : N;
    for (int i = 0; i < n; i++) { buf[i] = (int16_t)(amp * 28000.0f * sinf(ph)); ph += step; if (ph > 2*PI) ph -= 2*PI; }
    g_i2s.write((uint8_t*)buf, n * sizeof(int16_t));
    done += n;
  }
}

// Rising two-tone reactor klaxon. Blocks ~0.8s -- fine for an infrequent alert / test tap.
static void playKlaxon() {
  if (!g_audioOk) return;
  digitalWrite(AMP_EN_PIN, AMP_ON_LVL);
  for (int i = 0; i < 3; i++) { audioTone(880, 130, 0.8f); audioTone(1320, 130, 0.8f); }
  digitalWrite(AMP_EN_PIN, !AMP_ON_LVL);
}

// AUDIO settings sub-page (3rd SETTINGS-tab tap): alarm on/off toggle + a TEST button.
static const Rect R_ALARM = {14, 60, 212, 44}, R_TEST = {14, 176, 212, 44};
static void audioDraw() {
  tft.fillScreen(C_BG); tft.setTextDatum(TL_DATUM);
  tft.setTextColor(C_CYAN, C_BG); tft.drawString("< BACK", 6, 6, 2);
  tft.setTextDatum(TR_DATUM); tft.setTextColor(C_TEXT, C_BG); tft.drawString("AUDIO", 234, 6, 2);
  tft.setTextDatum(TL_DATUM); tft.drawFastHLine(0, 28, 240, C_LINE);
  tft.setTextColor(C_DIM, C_BG); tft.drawString("TRIP ALARM", 14, 40, 1);
  btn(R_ALARM, g_cfg.alarmOn ? "ALARM: ON" : "ALARM: OFF",
      g_cfg.alarmOn ? C_GREEN : C_LINE, g_cfg.alarmOn ? C_GREEN : C_DIM, 4);
  char s[40]; tft.setTextColor(C_DIM, C_BG);
  snprintf(s, sizeof(s), "sounds when a leg >= %d A", (int)(ALARM_FRAC*g_cfg.svcAmps)); tft.drawString(s, 14, 116, 2);
  tft.drawString("(approaching the 50A breaker)", 14, 138, 1);
  btn(R_TEST, "TEST ALARM", C_AMBER, C_AMBER, 4);
  tft.setTextColor(g_audioOk ? C_DIM : C_RED, C_BG);
  tft.drawString(g_audioOk ? "speaker on the board SPK JST" : "codec init failed", 14, 228, 1);
  drawNav();
}

// ---- DEVICES sub-page (4th SETTINGS-tab tap): change which Watchdog we track ----
// Two views: the main "TRACKING" card + a CHANGE button, and a scan-results list.
// Adopt = persist the picked MAC and reboot; the boot path loads that unit's per-MAC
// baseline + graph history (each Watchdog keeps its own /hist_<mac>.bin on SD).
static bool g_devScan    = false;                   // false = main view, true = scan-results view
static int  g_devConfirm = -1;                      // >=0 = adopt-confirm overlay open (index used only for the draw)
static char g_confirmMac[20] = "", g_confirmName[28] = "";   // snapshot at row-tap -> a live reorder can't adopt the wrong unit
static uint32_t g_changeArm = 0;                    // CHANGE WATCHDOG two-tap gate (armed only when a Watchdog is already tracked)
static const Rect R_CHANGE   = {14, 250, 212, 40};
static const Rect R_SVC      = {14, 126, 212, 30};   // 50/30A service-rating toggle
static const Rect R_RESCAN   = {14, 252, 100, 34}, R_DEVCANCEL = {126, 252, 100, 34};
static const Rect R_DEV_FIRE = {22, 196, 88, 36},  R_DEV_CX    = {130, 196, 88, 36};
static const int  DEV_ROWS   = 6;                   // found units shown on screen (of up to FOUND_MAX)
static Rect devRow(int i) { return Rect{8, 36 + i*34, 224, 32}; }

// Repaint just the live LINK line (padded -> flicker-free), like infoUpdate does.
static void devLiveUpdate() {
  if (!g_cfg.hughesMac.length() && !g_cfg.hughesName.length()) return;   // nothing tracked -> no link line
  bool conn; int rssi;
  xSemaphoreTake(g_mtx, portMAX_DELAY); conn=g_connected; rssi=g_rssi; xSemaphoreGive(g_mtx);
  char v[48]; uint16_t c;
  if      (g_released) { snprintf(v, sizeof(v), "paused - phone app in use"); c = C_AMBER; }
  else if (conn)       { snprintf(v, sizeof(v), "connected   %d dBm", rssi);  c = C_GREEN; }
  else                 { snprintf(v, sizeof(v), "not connected   %d dBm", rssi); c = C_RED; }
  tft.setTextDatum(TL_DATUM); tft.setTextColor(c, C_BG);
  tft.setTextPadding(200); tft.drawString(v, 14, 104, 2); tft.setTextPadding(0);
}

static void devicesDraw() {
  tft.fillScreen(C_BG); tft.setTextDatum(TL_DATUM);
  tft.setTextColor(C_CYAN, C_BG); tft.drawString("< BACK", 6, 6, 2);
  tft.setTextDatum(TR_DATUM); tft.setTextColor(C_TEXT, C_BG); tft.drawString("DEVICES", 234, 6, 2);
  tft.setTextDatum(TL_DATUM); tft.drawFastHLine(0, 28, 240, C_LINE);
  tft.setTextColor(C_CYAN, C_BG); tft.drawString("TRACKING", 6, 34, 1);
  bool haveWd = g_cfg.hughesMac.length() || g_cfg.hughesName.length();
  if (haveWd) {
    const char* nm  = g_peerName[0] ? g_peerName : (g_cfg.hughesName.length() ? g_cfg.hughesName.c_str() : "Watchdog");
    const char* mac = g_peerMac[0]  ? g_peerMac  : (g_cfg.hughesMac.length()  ? g_cfg.hughesMac.c_str()  : "--");
    tft.setTextColor(C_TEXT, C_BG); tft.drawString(nm, 14, 52, 4);
    tft.setTextColor(C_DIM,  C_BG); tft.drawString(mac, 14, 84, 2);
    devLiveUpdate();
  } else {
    tft.setTextColor(C_AMBER, C_BG); tft.drawString("No Watchdog", 14, 52, 4);
    tft.setTextColor(C_DIM,  C_BG); tft.drawString("none selected -- SELECT below", 14, 84, 1);
  }
  char sb[24]; snprintf(sb, sizeof(sb), "SERVICE: %d AMP", g_cfg.svcAmps);
  btn(R_SVC, sb, C_LINE, C_TEXT, 2);
  tft.setTextColor(C_DIM, C_BG); tft.drawString("match your RV's plug (50A / 30A)", 14, 160, 1);
  if (haveWd) {                                     // release the held BLE link so the phone app can pair
    drawLinkBtn();
    tft.setTextColor(C_DIM, C_BG); tft.drawString("hand the link to the phone app", 14, 210, 1);
  }
  g_changeArm = 0;                                 // fresh page -> the CHANGE two-tap gate starts disarmed
  if (haveWd) { tft.setTextColor(C_AMBER, C_BG); tft.setTextDatum(TL_DATUM);
                tft.drawString("scanning disconnects this Watchdog", 14, R_CHANGE.y - 14, 1); }   // only when one is tracked
  btn(R_CHANGE, haveWd ? "CHANGE WATCHDOG" : "SELECT WATCHDOG", C_CYAN, C_CYAN, 2);
  drawNav();
}

static int rssiBars(int r) { return r >= -55 ? 4 : r >= -65 ? 3 : r >= -72 ? 2 : r >= -82 ? 1 : 0; }
// Flicker-free live rows: only a NEW/changed device redraws its border+name (clearing just that row); the
// RSSI (padded overwrite) + signal bars refresh in place every cycle. Vacated rows are cleared per-row.
static char g_prevMac[DEV_ROWS][20];                                   // last-rendered MAC per row (change detection)
static int  g_prevRows = -1;                                          // last-rendered row count (-1 = force full)
static int  g_emptyState = -2;                                        // last empty message: -2 none, 0 no-devices, 1 scanning
static void devScanRows() {
  Found list[FOUND_MAX]; int n;
  xSemaphoreTake(g_foundMtx, portMAX_DELAY); n = g_foundN; memcpy(list, g_found, sizeof(list)); xSemaphoreGive(g_foundMtx);
  int rows = n < DEV_ROWS ? n : DEV_ROWS;
  tft.setTextDatum(TL_DATUM);
  if (rows == 0) {                                                    // empty state -- redraw the message only when it changes
    int es = g_scanBusy ? 1 : 0;
    if (g_prevRows != 0 || g_emptyState != es) {
      tft.fillRect(0, 32, 240, 208, C_BG);
      if (es == 1) { tft.setTextColor(C_AMBER, C_BG); tft.drawString("scanning for Watchdogs...", 14, 42, 2); }
      else { tft.setTextColor(C_DIM, C_BG); tft.drawString("No Watchdogs in range.", 14, 42, 2);
             tft.drawString("Plug in / power the Watchdog.", 14, 64, 1); }
      g_emptyState = es;
    }
    g_prevRows = 0; return;
  }
  if (g_prevRows <= 0) tft.fillRect(0, 32, 240, 208, C_BG);           // coming from empty/first -> clear once
  g_emptyState = -2;
  for (int i = 0; i < rows; i++) {
    Rect r = devRow(i);
    bool newRow = (i >= g_prevRows) || strcmp(g_prevMac[i], list[i].mac) != 0;
    if (newRow) {                                                    // device at this row changed -> repaint the static parts
      tft.fillRect(r.x, r.y, r.w, r.h, C_BG);
      tft.drawRoundRect(r.x, r.y, r.w, r.h, 5, C_LINE);
      tft.setTextColor(C_TEXT, C_BG); tft.drawString(list[i].name[0] ? list[i].name : "(unnamed)", r.x+6, r.y+3, 2);
      strncpy(g_prevMac[i], list[i].mac, sizeof(g_prevMac[i])-1); g_prevMac[i][sizeof(g_prevMac[i])-1] = 0;
    }
    int q = rssiBars(list[i].rssi);
    uint16_t rc = q >= 3 ? C_GREEN : q == 2 ? C_CYAN : C_AMBER;       // greener = closer/stronger
    char v[48]; snprintf(v, sizeof(v), "%d dBm   %s", list[i].rssi, list[i].mac);
    tft.setTextColor(rc, C_BG); tft.setTextDatum(TL_DATUM); tft.setTextPadding(172);
    tft.drawString(v, r.x+6, r.y+18, 1); tft.setTextPadding(0);       // padded -> overwrites the old dBm in place, no clear
    for (int b = 0; b < 4; b++) {                                     // signal bars: overwrite in place (fixed rects, colour changes)
      int bh = 5 + b*5, bx = r.x + r.w - 6 - (4-b)*7, by = r.y + r.h - 4 - bh;
      tft.fillRect(bx, by, 5, bh, b < q ? rc : C_LINE);
    }
  }
  for (int i = rows; i < g_prevRows && i < DEV_ROWS; i++) { Rect r = devRow(i); tft.fillRect(r.x, r.y, r.w, r.h, C_BG); g_prevMac[i][0] = 0; }
  g_prevRows = rows;
}
static void devScanDraw() {
  tft.fillScreen(C_BG); tft.setTextDatum(TL_DATUM);
  tft.setTextColor(C_CYAN, C_BG); tft.drawString("< BACK", 6, 6, 2);
  tft.setTextDatum(TR_DATUM); tft.setTextColor(C_TEXT, C_BG); tft.drawString("SCAN (LIVE)", 234, 6, 2);
  tft.setTextDatum(TL_DATUM); tft.drawFastHLine(0, 28, 240, C_LINE);
  g_prevRows = -1; g_emptyState = -2;                                  // full frame just cleared -> force a full row repaint
  devScanRows();
  btn(R_RESCAN, "CLEAR", C_LINE, C_TEXT, 2);
  btn(R_DEVCANCEL, "CANCEL", C_LINE, C_DIM, 2);
  drawNav();
}

static void devConfirmDraw() {
  tft.fillRect(12, 108, 216, 128, C_BG);
  tft.drawRoundRect(12, 108, 216, 128, 6, C_CYAN);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(C_CYAN, C_BG); tft.drawString("SWITCH TO THIS?", 120, 126, 2);
  tft.setTextColor(C_TEXT, C_BG); tft.drawString(g_confirmName[0] ? g_confirmName : "(unnamed)", 120, 148, 2);
  tft.setTextColor(C_DIM,  C_BG); tft.drawString(g_confirmMac, 120, 166, 1);
  tft.drawString("the screen will restart", 120, 180, 1);
  tft.setTextDatum(TL_DATUM);
  btn(R_DEV_FIRE, "SWITCH", C_CYAN, C_CYAN, 2);
  btn(R_DEV_CX,   "CANCEL", C_LINE, C_DIM, 2);
}

// Shared "the tracked Watchdog is changing" hygiene (adopt / portal save). R5: staycost/lastck are GLOBAL NVS keys
// (not per-MAC) -- without a reset the new unit's first reading books (newOdo - oldLastCk), thousands of kWh, into
// the new unit's ledger + bill. M10: an open stay lives in the OUTGOING MAC's stays_*.bin and would be invisible +
// un-closeable after the swap -> close it (flagged auto) against the old unit's file. FINDING#2: only snapshot a
// close when the meter is actually readable -- a dead-meter close would write kwhOut=0 and silently wipe the bill;
// leave it open (preserved in its file) instead. Callers guard on a REAL mac change (FINDING#5: same-unit re-adopt
// must not disturb the open stay/bill).
static void swapBillingReset() {
  if (stayOpenIdx() >= 0) {
    if (meterValid()) { int id = stayClose(true); logf("swap: auto-closed open stay #%d", id); }
    else              logf("swap: open stay left intact (no valid meter to close it cleanly)");
  }
  stayCostReset();
}
// Adopt a Watchdog: flush the CURRENT unit's ring to its own file first (histPath keys
// off the still-current MAC), then persist the new target and reboot into it.
static void adoptWatchdog(const char* mac, const char* name) {
  graphSave();
  usageSave(true);                                     // flush the CURRENT unit's ledger to ITS file before the MAC (and thus the path) changes
  evtSave();                                           // and its event log (same per-MAC path)
  if (!String(mac).equalsIgnoreCase(g_cfg.hughesMac)) swapBillingReset();   // FINDING#5/F6: only on a real unit change (case-insensitive -> a case-only diff can't spuriously zero the bill)
  g_cfg.hughesMac  = mac;
  g_cfg.hughesName = name ? name : "";
  configPersist();
  seedFreshBaselineIfNew();                            // start a never-tracked unit's STAY baseline fresh (not the outgoing unit's)
  logf("adopt: switching to %s (%s) -> reboot", mac, name ? name : "");
  g_reboot = true;
}

// ---- SYSTEM sub-page (5th SETTINGS-tab tap): FIRMWARE launcher + power off + factory reset ----
static const Rect R_SYS_FW      = {14, 100, 212, 36};   // FIRMWARE UPDATES button (v2.1 1.2; took the old SCREEN SCRUB slot)
static const Rect R_SYS_PWROFF  = {14, 158, 212, 36};
static const Rect R_SYS_FACTORY = {14, 216, 212, 36};
// NB: screen-scrub (runScrub() + `-e scrub`) is intentionally kept in the code but no longer surfaced in the UI
// (owner call 2026-09-01: "don't get rid of the scrub code, just don't reference it for now").
static uint32_t   g_factoryArm  = 0;                   // FACTORY RESET two-tap gate (0=idle, else 1st-tap time)
// ---- ALERTS sub-page (peer tab, g_setPage 8): enable/disable phone alerts + the subscribe QR ----
static const Rect R_ALRT_QR  = {14, 74,  212, 36};   // step 1: subscribe QR
static const Rect R_ALRT_EN  = {14, 138, 212, 36};   // step 2: the single ON/OFF toggle
static const Rect R_ALRT_DIS = {14, 198, 212, 32};   // SEND TEST ALERT
static uint32_t   g_alrtEnArm = 0, g_alrtDisArm = 0;   // ENABLE / DISABLE two-tap gates (0=idle, else 1st-tap time)
static const Rect R_SCRUB_END  = {24, 250, 88, 40};    // shown only while the scrub is paused
static const Rect R_SCRUB_RES  = {128, 250, 88, 40};

// In-firmware LCD de-ghost (same idea as the standalone -e scrub): high-contrast full-field
// cycling + polarity flip. Blocking like the reset animation -- bleTask keeps the BLE link alive
// on core 0; HTTP just pauses until this returns. Tap to pause on the reactor-grey field, where
// END / RESUME buttons appear. Auto-stops after ~20 min.
static void __attribute__((unused)) runScrub() {   // v2.1 1.2: kept but no longer surfaced in the UI (owner call) -> silence -Wunused
  const uint16_t cols[] = { TFT_RED, TFT_GREEN, TFT_BLUE, TFT_WHITE };
  uint32_t start = millis();
  bool paused = false, wt = true, ended = false;       // wt=true: swallow the launch tap's release first
  auto tap = [&](int* px, int* py) -> bool {
    ts.read(); bool n = ts.isTouched, e = n && !wt; wt = n;
    if (e && px) { *px = ts.points[0].x; *py = ts.points[0].y; }
    return e;
  };
  auto sweep = [&](int ms) -> bool {                   // wait ms, return true on any tap
    uint32_t t = millis();
    while (millis() - t < (uint32_t)ms) { if (tap(nullptr, nullptr)) return true; delay(4); }
    return false;
  };
  while (!ended && millis() - start < 20UL * 60 * 1000) {
    if (paused) {
      tft.invertDisplay(true);                         // TRUE colours (this IPS panel needs inversion ON)
      tft.fillScreen(C_BG);                            // reactor-grey field to inspect bleed-through
      btn(R_SCRUB_END, "END", C_RED, C_RED, 2);
      btn(R_SCRUB_RES, "RESUME", C_LINE, C_CYAN, 2);
      while (paused && !ended) {
        int x, y;
        if (tap(&x, &y)) {
          if (hit(R_SCRUB_END, x, y))      ended  = true;
          else if (hit(R_SCRUB_RES, x, y)) paused = false;
        }
        delay(10);
      }
      continue;
    }
    for (int k = 0; k < 12 && !paused; k++) {          // white<->black alternation + polarity flip
      tft.invertDisplay(k & 1);
      tft.fillScreen(TFT_WHITE); if (sweep(180)) { paused = true; break; }
      tft.fillScreen(TFT_BLACK); if (sweep(180)) { paused = true; break; }
    }
    if (paused) continue;
    tft.invertDisplay(true);
    for (uint16_t c : cols) { tft.fillScreen(c); if (sweep(340)) { paused = true; break; } }   // subpixel exercise
    if (paused) continue;
    for (int x = 0; x < tft.width() && !paused; x += 8) {                                       // sweeping bar
      tft.fillScreen(TFT_BLACK); tft.fillRect(x, 0, 8, tft.height(), TFT_WHITE);
      if (sweep(10)) paused = true;
    }
  }
  tft.invertDisplay(true);                             // leave the panel in its correct (bridge) state
  g_lastTouchMs = millis();                            // don't instantly idle-sleep on return
}

// ---- USAGE lite page (device, g_setPage 14, v2.1 1.1) -- read-only report off the 0.3 ledger ----
static const Rect R_USG_PORTAL = {0, 264, 240, 28};   // "see WEB PORTAL" jump-line (just above the nav bar)
static int g_portalReturn = 0;                         // U7: page WEB PORTAL (10) BACK returns to (0=menu, 14=USAGE) -> honours the one-level-BACK promise
static const DayRec* usageFind(uint32_t date) {       // AGGREGATE of every ledger record for a yyyymmdd local date, or null
  // M6: an offline reboot opens an undated record; the next NTP sync (same calendar day) opens a SECOND record for
  // that date -> returning only the newest match undercounts the device USAGE page (the web JSON already sums the
  // whole ring, so it was right). Sum all same-date records into a scratch aggregate; callers read it immediately.
  if (!date) return nullptr;
  static DayRec agg; memset(&agg, 0, sizeof(agg)); agg.date = date;
  bool found = false;
  for (int k = 0; k < g_dayCount; k++) {
    int idx = ((g_dayHead - k) % USAGE_DAYS + USAGE_DAYS) % USAGE_DAYS;   // current day back through history
    const DayRec& r = g_day[idx];
    if (r.date != date) continue;
    found = true;
    agg.totalKwh += r.totalKwh; agg.costC += r.costC;
    for (int b = 0; b < MAX_TIERS; b++) { agg.kwh[b] += r.kwh[b]; agg.bcostC[b] += r.bcostC[b]; }
  }
  return found ? &agg : nullptr;
}
static uint32_t ymdBack(time_t now, int daysBack, int* wday) {   // yyyymmdd (+ weekday) `daysBack` local days before today
  struct tm t; localtime_r(&now, &t);                            // anchor at LOCAL NOON then step whole days -> DST-safe (no
  t.tm_hour = 12; t.tm_min = 0; t.tm_sec = 0; t.tm_mday -= daysBack;   // 86400-drift landing on the wrong calendar day near midnight)
  time_t d = mktime(&t); struct tm r; localtime_r(&d, &r);       // mktime normalizes the date
  if (wday) *wday = r.tm_wday;
  return (uint32_t)(r.tm_year + 1900) * 10000u + (uint32_t)(r.tm_mon + 1) * 100u + (uint32_t)r.tm_mday;
}
static void usageDraw() {
  tft.fillScreen(C_BG); tft.setTextDatum(TL_DATUM);
  tft.setTextColor(C_CYAN, C_BG); tft.drawString("< BACK", 6, 6, 2);
  tft.setTextDatum(TR_DATUM); tft.setTextColor(C_TEXT, C_BG); tft.drawString("USAGE", 234, 6, 2);
  tft.setTextDatum(TL_DATUM); tft.drawFastHLine(0, 28, 240, C_LINE);

  time_t now = time(nullptr);
  bool clockReal = g_epochReal && now >= 1700000000;  // U1/M5: a real epoch (NTP/phone) -- NOT the H:M editor, which fabricates the date
  bool tzKnown   = tzConfigured();
  if (!clockReal || !tzKnown) {                        // the dated daily view needs BOTH a real clock AND a time zone
    tft.setTextDatum(MC_DATUM); tft.setTextColor(C_AMBER, C_BG);
    if (!clockReal) {                                  // time itself unknown
      tft.drawString("Clock not set", 120, 128, 4);
      tft.setTextColor(C_DIM, C_BG); tft.drawString("daily usage needs the real date --", 120, 166, 2);
      tft.drawString("wait for Wi-Fi or set from a phone", 120, 186, 2);   // U1: the on-device H:M editor can't date it; only NTP / phone-set can
    } else {                                           // time known (NTP) but no zone -> can't place a LOCAL calendar day
      tft.drawString("No time zone", 120, 128, 4);
      tft.setTextColor(C_DIM, C_BG); tft.drawString("pick your zone on the TIME", 120, 166, 2);
      tft.drawString("screen to date daily usage", 120, 186, 2);
    }
    tft.setTextDatum(TL_DATUM); drawNav(); return;
  }
  bool noRate = (g_cfg.tierCount == 0 && g_cfg.rateC == 0);

  // last 7 days: i=0 -> 6 days ago .. i=6 -> today
  static const char* WD = "SMTWTFS";                  // index by tm_wday (Sun=0..Sat=6)
  float kwh7[7]; char wl[7]; float mx = 0.0f;
  for (int i = 0; i < 7; i++) {
    int wd; const DayRec* r = usageFind(ymdBack(now, 6 - i, &wd));
    kwh7[i] = r ? r->totalKwh : 0.0f; wl[i] = WD[wd];
    if (kwh7[i] > mx) mx = kwh7[i];
  }
  const DayRec* yr = usageFind(ymdBack(now, 1, nullptr));    // yesterday
  float yKwh = yr ? yr->totalKwh : 0.0f; double yCost = yr ? yr->costC : 0.0;

  struct tm tnow; localtime_r(&now, &tnow);           // this week: Monday..today
  int sinceMon = (tnow.tm_wday + 6) % 7;
  float wKwh = 0.0f; double wCost = 0.0;
  for (int d = sinceMon; d >= 0; d--) { const DayRec* r = usageFind(ymdBack(now, d, nullptr)); if (r) { wKwh += r->totalKwh; wCost += r->costC; } }

  char buf[24];
  tft.setTextColor(C_DIM, C_BG); tft.drawString("YESTERDAY", 14, 34, 1);
  snprintf(buf, sizeof(buf), "%.1f kWh", yKwh);
  tft.setTextColor(C_TEXT, C_BG); tft.drawString(buf, 14, 50, 4);
  if (noRate) { tft.setTextColor(C_DIM, C_BG); tft.drawString("set rate in", 14, 80, 1);
                tft.setTextColor(C_CYAN, C_BG); tft.drawString("ENERGY & RATE", 68, 80, 1); }
  else { snprintf(buf, sizeof(buf), "$%.2f", yCost / 100.0);
         tft.setTextDatum(TR_DATUM); tft.setTextColor(C_GREEN, C_BG); tft.drawString(buf, 226, 50, 4); tft.setTextDatum(TL_DATUM); }
  tft.drawFastHLine(0, 92, 240, C_LINE);

  tft.setTextColor(C_DIM, C_BG); tft.drawString("THIS WEEK (MON-SUN)", 14, 100, 1);
  snprintf(buf, sizeof(buf), "%.1f kWh", wKwh);
  tft.setTextColor(C_TEXT, C_BG); tft.drawString(buf, 14, 114, 2);
  if (!noRate) { snprintf(buf, sizeof(buf), "$%.2f", wCost / 100.0);
                 tft.setTextDatum(TR_DATUM); tft.setTextColor(C_GREEN, C_BG); tft.drawString(buf, 226, 114, 2); tft.setTextDatum(TL_DATUM); }
  tft.drawFastHLine(0, 142, 240, C_LINE);

  tft.setTextColor(C_DIM, C_BG); tft.drawString("LAST 7 DAYS", 14, 150, 1);
  tft.setTextDatum(TR_DATUM); tft.drawString("kWh", 226, 150, 1); tft.setTextDatum(TL_DATUM);
  const int BY = 246, BH = 80, BW = 22, BGAP = 8, BX = 14;
  for (int i = 0; i < 7; i++) {
    int x = BX + i * (BW + BGAP);
    int h = (mx > 0.0f) ? (int)(BH * kwh7[i] / mx + 0.5f) : 0; if (h < 2) h = 2;   // 2px stub so a zero day still shows
    tft.fillRect(x, BY - h, BW, h, (i == 6) ? C_AMBER : C_CYAN);                    // today amber = still counting
    char l[2] = { wl[i], 0 };
    tft.setTextDatum(TC_DATUM); tft.setTextColor(C_DIM, C_BG); tft.drawString(l, x + BW / 2, 252, 1); tft.setTextDatum(TL_DATUM);
  }

  tft.setTextDatum(TC_DATUM); tft.setTextColor(C_DIM, C_BG); tft.drawString("Full daily report on the web", 120, 268, 1);
  tft.setTextColor(C_CYAN, C_BG); tft.drawString("portal -- see WEB PORTAL", 120, 278, 1);
  tft.setTextDatum(TL_DATUM); drawNav();
}
static void systemDraw() {
  g_factoryArm = 0;                                    // fresh page -> factory-reset gate disarmed
  tft.fillScreen(C_BG); tft.setTextDatum(TL_DATUM);
  tft.setTextColor(C_CYAN, C_BG); tft.drawString("< BACK", 6, 6, 2);
  tft.setTextDatum(TR_DATUM); tft.setTextColor(C_TEXT, C_BG); tft.drawString("SYSTEM", 234, 6, 2);
  tft.setTextDatum(TL_DATUM); tft.drawFastHLine(0, 28, 240, C_LINE);
  tft.setTextColor(C_DIM, C_BG); tft.drawString("BRIGHTNESS", 14, 32, 1);
  btn(R_BRT_DN, "-", C_LINE, C_TEXT, 4); btn(R_BRT_UP, "+", C_LINE, C_TEXT, 4); drawBrightVal();
  tft.setTextColor(C_DIM, C_BG); tft.drawString("FIRMWARE", 14, 86, 1);
  btn(R_SYS_FW, "FIRMWARE UPDATES", C_LINE, C_CYAN, 2, 2);
  tft.setTextColor(C_DIM, C_BG); tft.drawString("check for & apply updates", 14, R_SYS_FW.y + R_SYS_FW.h + 2, 1);
  btn(R_SYS_PWROFF, "POWER OFF", C_LINE, C_AMBER, 4, 2);
  tft.setTextColor(C_DIM, C_BG); tft.drawString("screen & radios off -- tap to wake", 14, R_SYS_PWROFF.y + R_SYS_PWROFF.h + 2, 1);   // U8: plain-language "deep sleep"
  btn(R_SYS_FACTORY, "FACTORY RESET", C_LINE, C_RED, 4, 2);
  tft.setTextColor(C_DIM, C_BG); tft.drawString("erase ALL settings -> setup portal", 14, R_SYS_FACTORY.y + R_SYS_FACTORY.h + 2, 1);
  drawNav();
}
// ---- HELP page (device, g_setPage 15, v2.1 1.2) -- QR to the online owner's manual ----
static void helpDraw() {
  tft.fillScreen(C_BG); tft.setTextDatum(TL_DATUM);
  tft.setTextColor(C_CYAN, C_BG); tft.drawString("< BACK", 6, 6, 2);
  tft.setTextDatum(TR_DATUM); tft.setTextColor(C_TEXT, C_BG); tft.drawString("HELP", 234, 6, 2);
  tft.setTextDatum(TL_DATUM); tft.drawFastHLine(0, 28, 240, C_LINE);
  char url[72]; snprintf(url, sizeof(url), "https://firmware.flensor.com/manual-%s.html", FW_VERSION);  // versioned -> each firmware's QR opens ITS own manual (older manuals stay archived)
  QRCode qr; static uint8_t qrbuf[300];                          // v6 (41x41) holds the ~46-char URL comfortably
  qrcode_initText(&qr, qrbuf, 6, ECC_LOW, url);
  int scale = 3, dim = qr.size * scale, ox = (240 - dim) / 2, oy = 46;
  int qz = 4 * scale;                                            // U8: QR spec wants a 4-module quiet zone (was 2) -> more reliable scans
  tft.fillRect(ox - qz, oy - qz, dim + 2*qz, dim + 2*qz, TFT_WHITE);
  for (int y = 0; y < qr.size; y++) for (int x = 0; x < qr.size; x++)
    if (qrcode_getModule(&qr, x, y)) tft.fillRect(ox + x*scale, oy + y*scale, scale, scale, TFT_BLACK);
  int ty = oy + dim + 14;
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(C_TEXT, C_BG); tft.drawString("Scan for the full guide", 120, ty, 2);
  tft.setTextColor(C_DIM,  C_BG); tft.drawString(url + 8, 120, ty + 20, 1);   // caption = the URL minus "https://" (matches the versioned QR)
  tft.drawString("setup - alerts - rates - usage", 120, ty + 34, 1);
  char foot[52];                                                 // v2.1 1.5: show the bridge's name if the owner set one
  if (g_cfg.nick.length()) snprintf(foot, sizeof(foot), "%s  -  v" FW_VERSION, g_cfg.nick.c_str());
  else                     snprintf(foot, sizeof(foot), "Shore Power Bridge   v" FW_VERSION);
  tft.setTextColor(C_DIM, C_BG); tft.drawString(foot, 120, 280, 1);
  tft.setTextDatum(TL_DATUM); drawNav();
}

// ---- ALERTS sub-page (peer tab): enable/disable phone alerts on-device + reach the subscribe QR ----
// Both toggles are two-tap gated (1st tap arms "ENABLE?/DISABLE?", 2nd applies; any other tap or 2.5s disarms).
static void alertsDraw() {
  g_alrtEnArm = 0; g_alrtDisArm = 0;                   // fresh page -> both gates disarmed
  tft.fillScreen(C_BG); tft.setTextDatum(TL_DATUM);
  tft.setTextColor(C_CYAN, C_BG); tft.drawString("< BACK", 6, 6, 2);
  tft.setTextDatum(TR_DATUM); tft.setTextColor(C_TEXT, C_BG); tft.drawString("ALERTS", 234, 6, 2);
  tft.setTextDatum(TL_DATUM); tft.drawFastHLine(0, 28, 240, C_LINE);
  tft.setTextColor(C_DIM, C_BG); tft.drawString("buzz your phone if shore power fails", 14, 33, 1);
  tft.setTextColor(g_cfg.ntfyOn ? C_GREEN : C_DIM, C_BG);       // plain status line (not a button)
  tft.drawString(g_cfg.ntfyOn ? "Phone alerts: ON" : "Phone alerts: OFF", 14, 50, 2);
  btn(R_ALRT_QR, "1. SET UP MY PHONE", C_CYAN, C_CYAN, 2);       // step 1: subscribe the phone
  tft.setTextColor(C_DIM, C_BG); tft.drawString("scan the QR with your phone", 14, R_ALRT_QR.y + R_ALRT_QR.h + 2, 1);
  if (g_cfg.ntfyOn) btn(R_ALRT_EN, "2. TURN OFF ALERTS", C_LINE,  C_AMBER, 2);   // step 2: single ON/OFF action
  else              btn(R_ALRT_EN, "2. TURN ON ALERTS",  C_GREEN, C_GREEN, 2);
  btn(R_ALRT_DIS, "SEND TEST ALERT", C_LINE, C_TEXT, 2);
  bool online = (g_cfg.netMode == 0 && WiFi.status() == WL_CONNECTED);
  if (!online) { tft.setTextColor(C_AMBER, C_BG); tft.drawString("home Wi-Fi to send: STATUS>NETWORK", 14, R_ALRT_DIS.y + R_ALRT_DIS.h + 4, 1); }
  drawNav();
}

// ---- NETWORK sub-page (opened by the SETTINGS "NET:" button): pick + explain HOME/AP/OFF ----
static const Rect R_NET_HOME = {14, 36, 212, 28};
static const Rect R_NET_AP   = {14, 84, 212, 28};
static const Rect R_NET_OFF  = {14, 132, 212, 28};
static const Rect R_NET_QR   = {14, 180, 212, 30};  // "SHOW QR CODE" button (only when OWN AP is selected)
static const Rect R_NET_OK   = {14, 222, 100, 40}, R_NET_CX = {126, 222, 100, 40};
static int g_netPending = 0;                        // NETWORK page: selected-but-not-yet-applied mode
static bool g_netHomePrompt = false;                // confirming HOME -> "which Wi-Fi?" (use saved vs set up new)
static const Rect R_HP_SAVED = {14, 96, 212, 44}, R_HP_NEW = {14, 152, 212, 44}, R_HP_CX = {14, 210, 212, 34};
static void networkDraw() {
  tft.fillScreen(C_BG); tft.setTextDatum(TL_DATUM);
  tft.setTextColor(C_CYAN, C_BG); tft.drawString("< BACK", 6, 6, 2);
  tft.setTextDatum(TR_DATUM); tft.setTextColor(C_TEXT, C_BG); tft.drawString("NETWORK", 234, 6, 2);
  tft.setTextDatum(TL_DATUM); tft.drawFastHLine(0, 28, 240, C_LINE);
  auto opt = [&](const Rect& r, int mode, const char* name, const char* desc) {
    bool sel = g_netPending == mode;                                     // green = the pending selection
    btn(r, name, sel ? C_GREEN : C_LINE, sel ? C_GREEN : C_TEXT, 2);
    tft.setTextColor(C_DIM, C_BG); tft.setTextDatum(TL_DATUM);
    tft.drawString(desc, 14, r.y + r.h + 2, 1);
  };
  opt(R_NET_HOME, 0, "HOME - join Wi-Fi",  "connect to your Wi-Fi router");
  opt(R_NET_AP,   1, "HOTSPOT - no Wi-Fi", "host its own Hughes-Bridge network");   // U8: "OWN AP" -> "HOTSPOT"
  opt(R_NET_OFF,  2, "OFF - screen only",  "radio off; lowest power, no phone access");   // U8: "no /status" -> plain language
  if (g_netPending == 1) btn(R_NET_QR, "SHOW QR CODE", C_CYAN, C_CYAN, 2);   // -> full-screen Wi-Fi QR + SSID/pass
  btn(R_NET_OK, "CONFIRM", C_GREEN, C_GREEN, 2);
  btn(R_NET_CX, "CANCEL",  C_LINE,  C_DIM,   2);
  drawNav();
}

// Confirming HOME asks which Wi-Fi to use: reconnect to the saved network, or set up a new one
// (drop into the portal) -- so arriving at a new park doesn't silently retry the old creds.
static void netHomePromptDraw() {
  tft.fillScreen(C_BG); tft.setTextDatum(TL_DATUM);
  tft.setTextColor(C_CYAN, C_BG); tft.drawString("< BACK", 6, 6, 2);
  tft.setTextDatum(TR_DATUM); tft.setTextColor(C_TEXT, C_BG); tft.drawString("HOME WI-FI", 234, 6, 2);
  tft.setTextDatum(TL_DATUM); tft.drawFastHLine(0, 28, 240, C_LINE);
  tft.setTextColor(C_DIM, C_BG); tft.drawString("Join with which Wi-Fi?", 14, 62, 2);
  btn(R_HP_SAVED, "USE SAVED", C_GREEN, C_GREEN, 2);
  tft.setTextColor(C_DIM, C_BG); tft.drawString("reconnect to the last network", 14, R_HP_SAVED.y + R_HP_SAVED.h + 2, 1);
  btn(R_HP_NEW, "SET UP NEW", C_CYAN, C_CYAN, 2);
  tft.setTextColor(C_DIM, C_BG); tft.drawString("enter a new network (setup portal)", 14, R_HP_NEW.y + R_HP_NEW.h + 2, 1);
  btn(R_HP_CX, "CANCEL", C_LINE, C_DIM, 2);
  drawNav();
}

// Full-screen Wi-Fi QR: scan with a phone camera to join the AP (WPA2 password = the API key).
static void qrDraw() {
  tft.fillScreen(C_BG); tft.setTextDatum(TL_DATUM);
  tft.setTextColor(C_CYAN, C_BG); tft.drawString("< BACK", 6, 6, 2);
  char ap[24]; apSsid(ap, sizeof(ap));
  tft.setTextDatum(TC_DATUM); tft.setTextColor(C_TEXT, C_BG); tft.drawString(ap, 120, 34, 2);
  char wifi[96]; snprintf(wifi, sizeof(wifi), "WIFI:S:%s;T:WPA;P:%s;;", ap, g_cfg.apiKey.c_str());  // canonical field order (S;T;P) -- iOS is picky
  QRCode qr; static uint8_t qrbuf[200];                                 // v4 (33x33) needs 137 bytes
  qrcode_initText(&qr, qrbuf, 4, ECC_LOW, wifi);
  const int scale = 5, oy = 62, q = 10;                                 // module px, top offset, quiet-zone px
  int qw = qr.size * scale, ox = (240 - qw) / 2;
  tft.fillRect(ox - q, oy - q, qw + 2*q, qw + 2*q, TFT_WHITE);          // white panel + quiet zone (dark-on-light scans best)
  for (int y = 0; y < qr.size; y++)
    for (int x = 0; x < qr.size; x++)
      if (qrcode_getModule(&qr, x, y)) tft.fillRect(ox + x*scale, oy + y*scale, scale, scale, TFT_BLACK);
  char p[48]; snprintf(p, sizeof(p), "pass: %s", g_cfg.apiKey.c_str());
  tft.setTextColor(C_TEXT, C_BG); tft.drawString(p, 120, oy + qw + q + 6, 2);
  tft.setTextColor(C_DIM, C_BG);  tft.drawString("point your phone camera here", 120, oy + qw + q + 28, 1);
  tft.setTextDatum(TL_DATUM);
  drawNav();
}

// Full-screen ntfy subscribe QR. Encodes the ntfy:// deep-link scheme so an ANDROID scan opens the ntfy app
// straight to the topic + subscribes (https://... only opens the website -- Android app-links for it are
// unreliable, and iOS has no deep-link at all). iPhone users open the ntfy app and add the topic by name,
// which is why the topic is shown large below the code.
static void ntfyQrDraw() {
  tft.fillScreen(C_BG); tft.setTextDatum(TL_DATUM);
  tft.setTextColor(C_CYAN, C_BG); tft.drawString("< BACK", 6, 6, 2);
  tft.setTextDatum(TC_DATUM); tft.setTextColor(C_TEXT, C_BG); tft.drawString("SUBSCRIBE", 120, 34, 2);
  ntfyEnsureTopic();
  char url[96]; snprintf(url, sizeof(url), "ntfy://%s/%s", g_cfg.ntfyServer.c_str(), g_cfg.ntfyTopic.c_str());  // Android app deep-link
  QRCode qr; static uint8_t qrbuf[200];                                 // v4 (33x33) holds the ~30-char URL comfortably
  qrcode_initText(&qr, qrbuf, 4, ECC_LOW, url);
  const int scale = 5, oy = 62, q = 10;                                 // module px, top offset, quiet-zone px
  int qw = qr.size * scale, ox = (240 - qw) / 2;
  tft.fillRect(ox - q, oy - q, qw + 2*q, qw + 2*q, TFT_WHITE);          // white panel + quiet zone (dark-on-light scans best)
  for (int y = 0; y < qr.size; y++)
    for (int x = 0; x < qr.size; x++)
      if (qrcode_getModule(&qr, x, y)) tft.fillRect(ox + x*scale, oy + y*scale, scale, scale, TFT_BLACK);
  tft.setTextColor(C_TEXT, C_BG); tft.drawString(g_cfg.ntfyTopic.c_str(), 120, oy + qw + q + 4, 2);   // topic name (large -> manual add)
  tft.setTextColor(C_DIM, C_BG);
  tft.drawString("install the free 'ntfy' app first", 120, oy + qw + q + 24, 1);
  tft.drawString("Android: scan   -   iPhone: add topic", 120, oy + qw + q + 36, 1);
  tft.setTextDatum(TL_DATUM);
  drawNav();
}

static void onTap(int x, int y) {
  Serial.printf("tap %d,%d (scr %d)\n", x, y, (int)g_scr);
  if (g_confirmOdo) {                                   // modal: the odometer-reset confirm swallows all other taps
    if (hit(R_ODO_FIRE, x, y))   { g_confirmOdo=false;
      if (stayOpenIdx() >= 0) {                            // FINDING#6: a stay opened (via web) between arm and confirm -> refuse WITHOUT the fake "meter reset" animation
        settingsDraw();
        tft.fillRect(0, 244, 240, 46, C_BG);              // UI 2nd-pass nit: tell the owner WHY (same message as the arm-site guard), don't bounce silently
        tft.setTextDatum(TC_DATUM); tft.setTextColor(C_RED, C_BG); tft.drawString("CLOSE THE OPEN STAY FIRST", 120, 254, 2);
        tft.setTextColor(C_DIM, C_BG); tft.drawString("this would zero the guest's bill", 120, 278, 1);
        tft.setTextDatum(TL_DATUM); return;
      }
      g_scr=SCR_REACTOR; resetAnimation(); resetOdometerFire(); g_scrDirty=true; return; }
    if (hit(R_ODO_CANCEL, x, y)) { g_confirmOdo=false; settingsDraw(); return; }
    return;
  }
  if (hit(R_NAV_S, x, y)) {                            // SETTINGS: single tap -> the menu (root list); every page is one tap from here
    g_scr = SCR_SETTINGS; g_setPage = 0; g_scrDirty = true; return;
  }
  if (hit(R_NAV_R, x, y)) { if (g_scr != SCR_REACTOR)  { g_scr=SCR_REACTOR;  g_scrDirty=true; } return; }
  if (hit(R_NAV_G, x, y)) {                            // GRAPHS: first tap enters; re-tap toggles W <-> A
    if (g_scr != SCR_GRAPHS) g_scr = SCR_GRAPHS; else g_graphMode ^= 1;
    g_scrDirty = true; return; }
  if (g_scr == SCR_REACTOR && g_outageShow) {          // outage banner: DISMISS / POWER OFF (nav handled above)
    if (hit(R_OUT_DISMISS, x, y)) { g_outageShow = false; g_scrDirty = true; return; }
    if (hit(R_OUT_OFF, x, y))     { enterSleep(); return; }
    return;
  }
  if (g_scr == SCR_REACTOR && !g_cfg.hughesMac.length() && !g_cfg.hughesName.length()) {   // no-Watchdog prompt -> jump to DEVICES
    if (hit(R_NOWD_PICK, x, y)) { g_scr=SCR_SETTINGS; g_setPage=3; g_devScan=false; g_devConfirm=-1; g_scrDirty=true; return; }
    return;
  }
  if (g_scr == SCR_REACTOR && hit(R_REACTOR_STATE, x, y)) {   // U6: tap the status word -> jump straight to the fix (helps the boondock owner act on ALERTS OFFLINE / SET THE CLOCK)
    char w[22]; const char* st = statusState(w, sizeof(w), nullptr);
    int page = 0;
    if      (!strcmp(st, "clock_unset") || !strcmp(st, "tz_unset"))                                 page = 9;   // TIME
    else if (!strcmp(st, "alerts_dark"))                                                            page = 8;   // ALERTS
    else if (!strcmp(st, "no_signal") || !strcmp(st, "link_weak") || !strcmp(st, "l1_fault") || !strcmp(st, "l2_fault")) page = 1;   // STATUS
    if (page) { g_scr = SCR_SETTINGS; g_setPage = page; g_scrDirty = true; }   // otherwise (SHORE OK / on-batt / app-linked) there's nothing to act on -> ignore
    return;
  }
  if (g_scr == SCR_REACTOR && hit(R_HEALTH, x, y)) {   // header cluster: swap signal bars <-> dBm number
    g_rssiBars = !g_rssiBars;
    tft.fillRect(138, 4, 48, 16, C_BG);                // wipe the shared slot before drawing the new one
    reactorNumbers(); return; }
  if (g_scr == SCR_SETTINGS) {
    if (g_setPage == 0) {                             // MENU (root list): each row opens a sub-page
      if (hit(R_MENU_HELP, x, y)) { g_setPage = 15; g_scrDirty = true; return; }   // "?" -> HELP
      for (int i = 0; i < MENU_N; i++) if (hit(menuRow(i), x, y)) {
        int p = MENU[i].page;
        if (p == 3) { g_devScan = false; g_devConfirm = -1; }              // land on the DEVICES main view
        if (p == 10) g_portalReturn = 0;                                   // U7: opened from the menu -> BACK returns to the menu
        g_setPage = p; g_scrDirty = true; return;
      }
      return;                                          // ignore taps in the gaps
    }
    if (g_setPage == 3) {                             // DEVICES sub-page: change which Watchdog we track
      if (g_devConfirm >= 0) {                        // modal: adopt confirm swallows other taps
        if (hit(R_DEV_FIRE, x, y)) { g_devConfirm=-1; g_wantScan=false; adoptWatchdog(g_confirmMac, g_confirmName); return; }  // adopt the snapshot MAC
        if (hit(R_DEV_CX,   x, y)) { g_devConfirm=-1; devScanDraw(); return; }
        return;
      }
      if (g_devScan) {                                // LIVE scan view
        if (hit(R_BACK, x, y) || hit(R_DEVCANCEL, x, y)) { g_devScan=false; g_wantScan=false; g_scrDirty=true; return; }   // stop scanning -> reconnect
        if (hit(R_RESCAN, x, y)) { xSemaphoreTake(g_foundMtx, portMAX_DELAY); g_foundN=0; xSemaphoreGive(g_foundMtx); g_scanBusy=true; devScanRows(); return; }  // CLEAR the list, keep scanning
        for (int i = 0; i < g_foundN && i < DEV_ROWS; i++)
          if (hit(devRow(i), x, y)) {
            bool ok = false; xSemaphoreTake(g_foundMtx, portMAX_DELAY);
            if (i < g_foundN) { strcpy(g_confirmMac, g_found[i].mac); strcpy(g_confirmName, g_found[i].name); ok = true; }
            xSemaphoreGive(g_foundMtx);
            if (ok) { g_devConfirm = i; devConfirmDraw(); } return;
          }
        return;
      }
      if (hit(R_BACK, x, y))   { g_setPage=0; g_scrDirty=true; return; }   // -> menu
      if (hit(R_SVC, x, y))    { g_cfg.svcAmps = (g_cfg.svcAmps == 50) ? 30 : 50; configPersist();
                                 char sb[24]; snprintf(sb, sizeof(sb), "SERVICE: %d AMP", g_cfg.svcAmps);
                                 btn(R_SVC, sb, C_LINE, C_TEXT, 2); return; }   // rescales reactor/alarm/header on next REACTOR draw
      if ((g_cfg.hughesMac.length() || g_cfg.hughesName.length()) && hit(R_LINK, x, y)) { g_released = !g_released; drawLinkBtn(); return; }
      if (g_changeArm && !hit(R_CHANGE, x, y)) { g_changeArm = 0; btn(R_CHANGE, "CHANGE WATCHDOG", C_CYAN, C_CYAN, 2); }   // any other tap disarms
      if (hit(R_CHANGE, x, y)) {
        bool haveWd = g_cfg.hughesMac.length() || g_cfg.hughesName.length();
        if (haveWd && !g_changeArm) { g_changeArm = millis(); btn(R_CHANGE, "TAP AGAIN TO SCAN", C_RED, C_RED, 2); return; }   // 1st tap arms (only when tracking one)
        g_changeArm = 0; g_devScan=true; xSemaphoreTake(g_foundMtx, portMAX_DELAY); g_foundN=0; xSemaphoreGive(g_foundMtx);
        g_scanBusy=true; g_scanDone=false; g_wantScan=true; g_scrDirty=true; return;   // start the LIVE scan (2nd tap, or nothing tracked)
      }
      return;
    }
    if (g_setPage == 5) {                             // NETWORK sub-page (opened via the NET button)
      if (g_netHomePrompt) {                          // HOME confirm -> "which Wi-Fi?" prompt
        if (hit(R_HP_SAVED, x, y)) {                    // reconnect with the saved creds (ours, or adopt the driver's)
          g_netHomePrompt=false;
          if (!g_cfg.wifiSsid.length()) wifiCaptureCreds();          // pull driver-stored creds into g_cfg so provisioned() stays true
          if (!g_cfg.wifiSsid.length()) { doReprovision(); return; } // nothing saved anywhere -> fall through to setup portal
          g_cfg.netMode=0; configPersist(); evtLog(EV_NET_MODE, 0); netApply(); g_setPage=1; g_scrDirty=true; return;   // -> STATUS
        }
        if (hit(R_HP_NEW,   x, y)) { g_netHomePrompt=false; doReprovision(); return; }                        // wipe -> setup portal (reboot)
        if (hit(R_HP_CX, x, y) || hit(R_BACK, x, y)) { g_netHomePrompt=false; g_scrDirty=true; return; }      // back to the NETWORK page
        return;
      }
      if (hit(R_BACK, x, y) || hit(R_NET_CX, x, y)) { g_setPage = 1; g_scrDirty = true; return; }   // cancel -> STATUS, no change
      if (hit(R_NET_HOME, x, y)) { g_netPending = 0; networkDraw(); return; }   // select (pending, not applied)
      if (hit(R_NET_AP,   x, y)) { g_netPending = 1; networkDraw(); return; }
      if (hit(R_NET_OFF,  x, y)) { g_netPending = 2; networkDraw(); return; }
      if (g_netPending == 1 && hit(R_NET_QR, x, y)) { g_setPage = 6; g_scrDirty = true; return; }   // show the join QR
      if (hit(R_NET_OK,   x, y)) {
        if (g_netPending == 0) { g_netHomePrompt = true; netHomePromptDraw(); return; }             // HOME -> ask which Wi-Fi first
        int prevMode = g_cfg.netMode;
        g_cfg.netMode = g_netPending; configPersist();
        if (g_cfg.netMode != prevMode) evtLog(EV_NET_MODE, (int16_t)g_cfg.netMode);   // only on an actual change
        if (g_netPending == 1) {   // AP: REBOOT so apStart() runs from a clean boot state -- a live STA->AP switch leaves
          evtSave();               // persist the event before the reboot (the 30s flush is skipped once g_reboot is set)
          tft.fillScreen(C_BG); tft.setTextDatum(MC_DATUM); tft.setTextColor(C_CYAN, C_BG);   // softAP with the wrong/blank WPA2 PSK ("wrong password" on join)
          tft.drawString("RESTARTING...", 120, 160, 4); tft.setTextDatum(TL_DATUM); g_reboot = true; return;
        }
        netApply(); g_setPage = 1; g_scrDirty = true; return;   // HOME/OFF apply live -> STATUS
      }
      return;
    }
    if (g_setPage == 6) {                             // QR join view -> back to NETWORK
      if (hit(R_BACK, x, y)) { g_setPage = 5; g_scrDirty = true; return; }
      return;
    }
    if (g_setPage == 7) {                             // ntfy subscribe QR -> back to ALERTS
      if (hit(R_BACK, x, y)) { g_setPage = 8; g_scrDirty = true; return; }
      return;
    }
    if (g_setPage == 9) {                             // TIME page: zone picker + SET TIME; BACK -> menu
      if (hit(R_BACK, x, y)) { g_setPage=0; g_scrDirty=true; return; }
      if (hit(R_TIME_SET, x, y)) {                    // -> manual set-time, seeded from the current local time
        time_t now=time(nullptr); if(now<1700000000) now=1735689600; struct tm t; localtime_r(&now,&t);
        g_setH=t.tm_hour; g_setM=t.tm_min; g_setPage=11; g_scrDirty=true; return;
      }
      for (int i = 0; i < TZ_COUNT; i++) if (hit(tzCell(i), x, y)) {
        g_cfg.tzIndex = i; applyTz(); configPersist(); logf("tz: set %s (on-device)", tzName()); g_scrDirty = true; return;
      }
      return;
    }
    if (g_setPage == 11) {                            // manual set-time -> back to TIME
      if (hit(R_BACK, x, y) || hit(R_TS_CX, x, y)) { g_setPage=9; g_scrDirty=true; return; }
      if (hit(R_TS_HDN, x, y)) { g_setH=(g_setH+23)%24; timeSetValDraw(); return; }
      if (hit(R_TS_HUP, x, y)) { g_setH=(g_setH+1)%24;  timeSetValDraw(); return; }
      if (hit(R_TS_MDN, x, y)) { g_setM=(g_setM+55)%60; timeSetValDraw(); return; }   // -5 min
      if (hit(R_TS_MUP, x, y)) { g_setM=(g_setM+5)%60;  timeSetValDraw(); return; }   // +5 min
      if (hit(R_TS_OK,  x, y)) { timeSetApply(); g_setPage=9; g_scrDirty=true; return; }
      return;
    }
    if (g_setPage == 10) {                            // WEB PORTAL QR -> back to wherever we came from (U7)
      if (hit(R_BACK, x, y)) { g_setPage = g_portalReturn; g_portalReturn = 0; g_scrDirty = true; return; }
      return;
    }
    if (g_setPage == 14) {                            // USAGE report -> BACK to menu; portal jump-line
      if (hit(R_BACK, x, y)) { g_setPage = 0; g_scrDirty = true; return; }
      if (hit(R_USG_PORTAL, x, y)) { g_portalReturn = 14; g_setPage = 10; g_scrDirty = true; return; }   // U7: remember USAGE as the return page
      return;
    }
    if (g_setPage == 12 && hit(R_BACK, x, y)) { g_setPage = 4; g_scrDirty = true; return; }   // FIRMWARE -> SYSTEM (its only entry point, v2.1 1.2 one-level BACK)
    if (hit(R_BACK, x, y))    { g_setPage=0; g_scrDirty=true; return; }   // STATUS/SYSTEM/ALERTS/ENERGY -> menu
    if (g_setPage == 4) {                             // SYSTEM sub-page: scrub / power off / factory reset
      if (g_factoryArm && !hit(R_SYS_FACTORY, x, y)) { g_factoryArm = 0; btn(R_SYS_FACTORY, "FACTORY RESET", C_LINE, C_RED, 4, 2); }  // any other tap disarms
      if (hit(R_BRT_DN, x, y))  { if (g_cfg.bright > 10)  g_cfg.bright -= 10; applyBright(); configPersist(); drawBrightVal(); return; }   // BRIGHTNESS (moved here from SETTINGS)
      if (hit(R_BRT_UP, x, y))  { if (g_cfg.bright < 100) g_cfg.bright += 10; applyBright(); configPersist(); drawBrightVal(); return; }
      if (hit(R_SYS_FW, x, y)) { g_setPage = 12; g_scrDirty = true; return; }                     // FIRMWARE UPDATES (BACK from there -> SYSTEM)
      if (hit(R_SYS_PWROFF, x, y)) { enterSleep(); return; }
      if (hit(R_SYS_FACTORY, x, y)) {
        if (g_factoryArm) { g_factoryArm = 0; doFactoryReset(); return; }                        // 2nd tap -> wipe + reboot to portal
        g_factoryArm = millis(); btn(R_SYS_FACTORY, "TAP AGAIN TO ERASE", C_RED, C_RED, 2); return;      // U4: name the consequence (font 2 to fit) -- was a bare "TAP AGAIN"
      }
      return;
    }
    if (g_setPage == 12) { firmwareTouch(x, y); return; }   // FIRMWARE page: check / update / rollback
    if (g_setPage == 8) {                             // ALERTS sub-page: enable/disable phone alerts + subscribe QR
      if (g_alrtDisArm && !hit(R_ALRT_EN, x, y)) { g_alrtDisArm = 0; btn(R_ALRT_EN, "2. TURN OFF ALERTS", C_LINE, C_AMBER, 2); }  // any other tap disarms the OFF gate
      if (hit(R_ALRT_QR, x, y)) { g_setPage = 7; g_scrDirty = true; return; }                    // step 1: subscribe QR
      if (hit(R_ALRT_EN, x, y)) {                                                                 // step 2: ON = single tap, OFF = two-tap gated
        if (!g_cfg.ntfyOn) { g_cfg.ntfyOn = true; ntfyEnsureTopic(); configPersist(); logf("alerts: ON (device)"); g_scrDirty = true; return; }
        if (g_alrtDisArm)  { g_alrtDisArm = 0; g_cfg.ntfyOn = false; configPersist(); logf("alerts: OFF (device)"); g_scrDirty = true; return; }
        g_alrtDisArm = millis(); btn(R_ALRT_EN, "TAP AGAIN TO TURN OFF", C_RED, C_RED, 2); return;
      }
      if (hit(R_ALRT_DIS, x, y)) {                                                                // SEND TEST ALERT
        ntfyEnsureTopic(); ntfyEnqueue("Test alert", "Hughes bridge test -- alerts are working.", 3, "wave");
        btn(R_ALRT_DIS, "SENT -- CHECK PHONE", C_GREEN, C_GREEN, 2); logf("alerts: test queued (device)"); return;
      }
      return;
    }
    if (g_setPage == 1) {                             // INFO sub-page: read-only, except the NETWORK + TIME ZONE launchers
      if (hit(R_INFO_NET, x, y)) { g_setPage = 5; g_netPending = g_cfg.netMode; g_netHomePrompt = false; g_scrDirty = true; return; }   // open NETWORK
      return;
    }
    if (ALARM_PAGE && g_setPage == 2) {              // AUDIO sub-page: alarm toggle + TEST button
      if (hit(R_ALARM, x, y)) { g_cfg.alarmOn = !g_cfg.alarmOn; configPersist(); audioDraw(); return; }
      if (hit(R_TEST,  x, y)) { btn(R_TEST, "SOUNDING...", C_AMBER, C_AMBER, 4); playKlaxon(); audioDraw(); return; }
      return;
    }
    if (g_setPage != 13) return;                     // only the ENERGY & RATE page has controls past here
    if (g_resetArm && !hit(R_RESET, x, y)) { g_resetArm = 0; btn(R_RESET, "RESET STAY", C_LINE, C_AMBER, 2); }  // any other tap disarms
    if (hit(R_RESET, x, y)) {                          // RESET STAY: two-tap gate
      if (g_resetArm) {                                // 2nd tap -> fire the re-baseline
        g_resetArm = 0;
        g_cfg.kwhBase += stayKwh(); configPersist(); baselinePersist(); stayCostReset(); evtLog(EV_RESET_STAY, 0); drawStayVal();
        btn(R_RESET, "STAY RESET", C_GREEN, C_GREEN, 2); g_btnFlash = millis();
      } else {                                         // 1st tap -> arm (filled amber pill = unmissable)
        g_resetArm = millis();
        btnFill(R_RESET, "TAP AGAIN TO CONFIRM", C_AMBER, C_BG, 2);
      }
      return;
    }
    if (g_cfg.tierCount == 0 && hit(R_RATE_DN, x, y)) { if (g_cfg.rateC > 1)   g_cfg.rateC -= 1; configPersist(); drawRateVal(); return; }   // flat mode only
    if (g_cfg.tierCount == 0 && hit(R_RATE_UP, x, y)) { if (g_cfg.rateC < 999) g_cfg.rateC += 1; configPersist(); drawRateVal(); return; }   // clamp 999 to match web
    if (hit(R_RESET_ODO, x, y)){
      if (stayOpenIdx() >= 0) {                        // R6: don't even arm the reset while a stay is open -- it would zero the guest's bill
        tft.fillRect(0, 244, 240, 46, C_BG);           // finding#2(UI): stop at y290 so the nav bar (y292) survives -- was h76, which wiped the nav with no repaint
        tft.setTextDatum(TC_DATUM); tft.setTextColor(C_RED, C_BG); tft.drawString("CLOSE THE OPEN STAY FIRST", 120, 254, 2);
        tft.setTextColor(C_DIM, C_BG); tft.drawString("this would zero the guest's bill", 120, 278, 1);
        tft.setTextDatum(TL_DATUM); return;
      }
      g_confirmOdo = true; drawOdoConfirm(); return;
    }
  } else if (g_scr == SCR_GRAPHS) {
    if (hit(R_G5,  x, y)) { g_graphMin=5;  g_scrDirty=true; return; }
    if (hit(R_G10, x, y)) { g_graphMin=10; g_scrDirty=true; return; }
    if (hit(R_G30, x, y)) { g_graphMin=30; g_scrDirty=true; return; }
  }
}
static void handleTouch() {
  ts.read();
  bool now = ts.isTouched;
  if (now && !g_wasTouched) {                        // rising edge = a tap
    g_lastTouchMs = millis();
    if (g_asleep) { g_asleep = false; applyBright(); }   // wake only -- consume this tap
    else onTap(ts.points[0].x, ts.points[0].y);
  }
  g_wasTouched = now;
}

// ---------------------------------------------------------------- boot screen
// Terminal-style subsystem boot log, matching the fleet's Comms Array panel so the
// consoles feel like one system. Each line is a REAL step, drawn as it actually happens
// -- a boot screen that always succeeds is just an animation.
static int g_bootY = 0;
static void bootBegin() {
  tft.fillScreen(C_BG);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(C_CYAN, C_BG); tft.drawString("EXCELSIOR", 8, 8, 4);
  tft.setTextDatum(TR_DATUM); tft.setTextColor(C_DIM, C_BG); tft.drawString("v" FW_VERSION, 232, 12, 1);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(C_DIM, C_BG);  tft.drawString("SHORE POWER - HUGHES GEN1 50A", 8, 38, 1);
  tft.drawFastHLine(0, 52, 240, C_LINE);
  g_bootY = 64;
}
static void bootStep(const char* label) {           // draw "LABEL ......." (status appended after)
  tft.setTextDatum(TL_DATUM); tft.setTextColor(C_CYAN, C_BG);
  tft.drawString(label, 8, g_bootY, 1);
  delay(180);
}
static void bootResult(bool ok, const char* okTxt, const char* warnTxt) {
  tft.setTextDatum(TR_DATUM);
  tft.setTextColor(ok ? C_GREEN : C_AMBER, C_BG);
  tft.drawString(ok ? okTxt : warnTxt, 232, g_bootY, 1);
  tft.setTextDatum(TL_DATUM);
  g_bootY += 16; delay(120);
}
static void bootDone() {
  tft.drawFastHLine(0, g_bootY + 4, 240, C_LINE);
  if (g_cfg.nick.length()) {                                     // v2.1 1.5: greet the named bridge on boot
    char g[40]; snprintf(g, sizeof(g), ">> %s ONLINE", g_cfg.nick.c_str());
    tft.setTextColor(C_CYAN, C_BG); tft.drawString(g, 8, g_bootY + 12, 2);
  } else {
    tft.setTextColor(C_CYAN, C_BG); tft.drawString(">> REACTOR ONLINE", 8, g_bootY + 12, 2);
  }
  delay(750);
}

// ---------------------------------------------------------------- modes
// ---------------------------------------------------------------- network bring-up
// The product (BLE + touchscreen) always runs; Wi-Fi is layered on per g_cfg.netMode:
// HOME = join home Wi-Fi (fall back to our own AP if it won't connect), AP = host our own
// Hughes-Bridge-XXXX network, OFF = radio off (screen-only, lowest power).
static void macLast4(char* out, size_t n) {         // last 2 MAC bytes as 4 hex, from efuse (works before WiFi is up)
  uint8_t m[6] = {0}; esp_read_mac(m, ESP_MAC_WIFI_STA);
  snprintf(out, n, "%02X%02X", m[4], m[5]);
}
static void apSsid(char* out, size_t n) {           // "Hughes-Bridge-XXXX"
  char l4[8]; macLast4(l4, sizeof(l4));
  snprintf(out, n, "Hughes-Bridge-%s", l4);
}
static void apStart() {                             // host our own network, secured by the API key
  WiFi.mode(WIFI_AP);
  char ssid[24]; apSsid(ssid, sizeof(ssid));
  // Force WPA2-PSK + CCMP (AES) -- the default mixed TKIP+CCMP cipher makes modern phones (esp. iOS)
  // flaky ("sometimes connects"). ch1, not hidden, 4 clients, no FTM.
  if (g_cfg.apiKey.length() >= 8) WiFi.softAP(ssid, g_cfg.apiKey.c_str(), 1, 0, 4, false, WIFI_AUTH_WPA2_PSK, WIFI_CIPHER_TYPE_CCMP);
  else                            WiFi.softAP(ssid);                          // open (key too short for WPA2)
  IPAddress ip = WiFi.softAPIP();
  g_dns.start(53, "*", ip);                         // captive DNS -> the dashboard pops on join
  g_apServing = true;
  logf("ap: '%s' up at %s", ssid, ip.toString().c_str());
}
// Reconcile our config with the Wi-Fi driver's OWN persisted STA creds. Some connect paths get the board
// onto Wi-Fi via the esp_wifi stack's stored config without populating g_cfg.wifiSsid -- e.g. WiFi.begin("","")
// falls back to the driver's saved network, and the NETWORK-page "USE SAVED" tap used to leave ssid empty.
// provisioned() keys off g_cfg.wifiSsid, so an empty value drops the unit to the setup portal on the next
// boot even though Wi-Fi works. Adopt the real creds into g_cfg (our source of truth) + persist, so a
// provisioned unit STAYS provisioned across a power cycle. Returns true if it captured/kept real creds.
static bool wifiCaptureCreds() {
  if (g_cfg.wifiSsid.length()) return true;                    // already ours -- nothing to do
  wifi_config_t conf; if (esp_wifi_get_config(WIFI_IF_STA, &conf) != ESP_OK) return false;
  if (conf.sta.ssid[0] == 0) return false;                     // driver has no stored network either
  char ss[33]; memcpy(ss, conf.sta.ssid, 32); ss[32] = 0;      // fields aren't guaranteed NUL-terminated at max len
  char pw[65]; memcpy(pw, conf.sta.password, 64); pw[64] = 0;
  g_cfg.wifiSsid = ss; g_cfg.wifiPass = pw; g_cfg.netMode = 0;
  configPersist();
  logf("wifi: adopted driver creds '%s' into config (was empty) -> provisioned", g_cfg.wifiSsid.c_str());
  return true;
}

static void netBringUp() {                          // boot-time: set up Wi-Fi per netMode (with boot-screen steps)
  if (g_cfg.netMode == 2) { WiFi.mode(WIFI_OFF); g_apServing = false;
                            bootStep("NETWORK ..............."); bootResult(false, "", "OFF"); return; }
  if (g_cfg.netMode == 1) { apStart();
                            bootStep("HOST OWN AP .........."); bootResult(true, "AP", ""); return; }
  WiFi.mode(WIFI_STA);                              // HOME: join, fall back to our own AP on failure
  WiFi.begin(g_cfg.wifiSsid.c_str(), g_cfg.wifiPass.c_str());
  bootStep("JOIN WIFI .............");
  Serial.printf("WiFi: joining %s", g_cfg.wifiSsid.c_str());
  for (int i = 0; i < 40 && WiFi.status() != WL_CONNECTED; i++) { delay(500); Serial.print("."); }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    g_apServing = false; bootResult(true, "OK", "");
    wifiCaptureCreds();                              // if we joined via the driver's cache (empty g_cfg), adopt it -> stays provisioned
    logf("wifi: joined '%s' IP %s", g_cfg.wifiSsid.c_str(), WiFi.localIP().toString().c_str());
    syncTime();                                      // proactive SNTP on STA connect -> local clock is set early (not only for TLS/OTA)
  } else {
    bootResult(false, "OK", "AP MODE"); delay(300);
    logf("wifi: join '%s' FAILED -> hosting own AP", g_cfg.wifiSsid.c_str());
    apStart();
  }
}
static void netApply() {                            // runtime: apply a netMode change live (SETTINGS toggle / HTTP)
  g_dns.stop();
  if (g_cfg.netMode == 2)      { WiFi.mode(WIFI_OFF); g_apServing = false; Serial.println("net -> OFF"); }
  else if (g_cfg.netMode == 1) { apStart(); }
  else { g_apServing = false; WiFi.mode(WIFI_STA);
         WiFi.begin(g_cfg.wifiSsid.c_str(), g_cfg.wifiPass.c_str()); Serial.println("net -> HOME (STA)"); }
}

// Graceful deep-sleep power-off. Flushes history, blanks the screen, arms tap-to-wake on the
// FT6336 touch INT (GPIO17, active-low). Deep sleep ~10-20uA; waking is a fresh boot (~5s).
// NOTE: BLE/Wi-Fi are off while asleep -> the Pi feed stops until it wakes. Validate tap-to-wake
// on hardware (does the FT6336 INT hold low long enough to trigger ext1 while asleep?).
static void enterSleep() {
  graphSave();                                     // don't lose the last <=30s of history
  usageSave(true);                                  // flush the usage ledger before deep sleep
  evtSave();                                        // flush the event log before deep sleep
  tft.fillScreen(C_BG);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(C_DIM, C_BG); tft.drawString("sleeping", 120, 148, 4);
  tft.drawString("tap to wake", 120, 178, 2);
  tft.setTextDatum(TL_DATUM);
  led(0, 0, 0);
  logf("sleep: deep sleep (tap to wake = fresh boot)");
  delay(1200);
  ledcWrite(45, 0);                                // backlight off
  delay(50);
  esp_sleep_enable_ext1_wakeup(1ULL << 17, ESP_EXT1_WAKEUP_ANY_LOW);   // touch INT active-low
  esp_deep_sleep_start();                          // never returns; wake = reset -> full boot
}

static void startProvisioning() {
  g_mode = MODE_PROV;
  led(40, 0, 40);                                  // magenta: setup portal
  WiFi.mode(WIFI_AP_STA);                          // AP for the portal; STA so we can scan networks
  { char l4[8]; macLast4(l4, sizeof(l4)); snprintf(g_setupSsid, sizeof(g_setupSsid), "%s-%s", AP_SSID, l4); }   // Hughes-Setup-XXXX
  WiFi.softAP(g_setupSsid);                         // open network -- setup only
  IPAddress ip = WiFi.softAPIP();
  Serial.printf("\nPROVISIONING: join Wi-Fi '%s', open http://%s\n", g_setupSsid, ip.toString().c_str());

  NimBLEDevice::init("");                           // for the Watchdog picker scan
  g_dns.start(53, "*", ip);                         // captive-portal catch-all

  server.on("/", handlePortalRoot);
  server.on("/scan_ble", handleScanBle);
  server.on("/save", HTTP_POST, handleSave);
  server.onNotFound(handleCaptive);
  server.begin();
  ts.begin(); ts.setRotation(ROTATION_NORMAL);     // touch for the on-screen SKIP button
  provisioningScreen();                            // TFT shows how to join the setup AP
}

static void startBridge() {
  g_mode = MODE_BRIDGE;
  NimBLEDevice::init("");
  NimBLEDevice::setMTU(517);
  bootStep("START BLE LINK ......."); bootResult(true, "OK", "");

  server.on("/", handleDash);                      // live dashboard (works on home Wi-Fi or our own AP)
  server.on("/help", handleRoot);
  server.on("/status", handleStatus);
  server.on("/log", handleLog);                    // in-RAM breadcrumb ring (diagnostics over Wi-Fi)
  server.on("/dmesg", handleDmesg);                // persistent RTC crash-log ring (survives reboots)
  server.on("/history", handleHistory);            // downsampled RAM-ring history for the web chart
  server.on("/screenshot", handleScreenshot);      // TFT framebuffer -> BMP (device screen capture, if the panel reads back)
  server.on("/config", HTTP_POST, handleConfig);   // web settings (nick/rate/svc/bright) -- unlocked
  server.on("/settime", HTTP_POST, handleSetTime); // set the wall clock from the phone (no-internet path) -- unlocked
  server.on("/usage", handleUsage);                // 30-day per-rate usage ledger (JSON) -- unlocked read
  server.on("/events", handleEvents);              // event log ring (JSON) -- unlocked read
  server.on("/stays", handleStays);                // Stay Cards list (JSON) -- unlocked read
  server.on("/stay/open",  HTTP_POST, handleStayOpen);   // open a guest stay (unlocked, like /config; refuses if one's open or no meter yet)
  server.on("/stay/close", HTTP_POST, handleStayClose);  // close the open stay (unlocked; refuses if no live meter reading)
  server.on("/receipt", handleReceipt);            // printable per-stay statement (2.2 + continuity 2.4)
  server.on("/stays.csv", handleStaysCsv);         // bookkeeping CSV export (2.6)
  server.on("/support_bundle", HTTP_POST, handleSupportBundle);   // on-demand: send diagnostics to the maker (owner-initiated)
  server.on("/ntfy", HTTP_POST, handleNtfy);       // ntfy push alerts: enable/mask/regen/test -- unlocked
  server.on("/ota", HTTP_POST, handleOta);         // OTA: check + auto-update if newer -- gated on ?key=hughes
  server.on("/ota_url", HTTP_POST, handleOtaUrl);  // OTA: set the manifest URL -- gated on ?key=hughes
  server.on("/reset_stay", HTTP_POST, handleResetStay);
  server.on("/wifi", HTTP_POST, handleWifi);       // change home Wi-Fi from the dashboard -- unlocked
  server.on("/factory_reset", HTTP_POST, handleFactoryReset);   // wipe all config -- unlocked
  server.on("/reprovision", HTTP_POST, handleReprovision);
  server.on("/reset_odometer", HTTP_POST, handleResetOdometer);
  server.on("/netmode", HTTP_POST, handleNetMode);
  server.on("/sleep", HTTP_POST, handleSleep);
  server.onNotFound(handleNotFound);               // AP mode: captive redirect -> pops the dashboard
  server.begin();
  Serial.printf("HTTP server on :%d  (/status, /)\n", HTTP_PORT);

  xTaskCreatePinnedToCore(bleTask, "ble", 12288, NULL, 1, NULL, 0);

  if (!g_coreSpr.createSprite(SPR, SPR))            // ~15KB reusable core sprite (internal SRAM)
    Serial.println("WARN: core sprite alloc failed -- cores will not draw");
  ts.begin(); ts.setRotation(ROTATION_NORMAL);     // capacitive touch (portrait)

  if (ALARM_PAGE) {                                // audible alarm only shipped when a speaker is fitted (see ALARM_PAGE)
    g_audioOk = audioSetup();                      // ES8311 + I2S trip alarm (shares Wire w/ touch)
    Serial.printf("audio: %s\n", g_audioOk ? "ready" : "codec/I2S init failed");
    bootStep("ARM AUDIO ALARM ....."); bootResult(g_audioOk, "OK", "NONE");
  }

  battSetup();                                     // battery-voltage ADC (GPIO9); charger is autonomous, no control
  bootStep("READ BATTERY ........."); bootResult(g_adc != nullptr, "OK", "N/A");

  g_chartSpr.setColorDepth(4);                     // 4-bit palette sprite (~18KB), flicker-free graph
  if (g_chartSpr.createSprite(GX1-GX0, GY1-GY0)) {
    static uint16_t pal[16] = { C_BG, C_LINE, C_AMBER, C_CYAN, C_TEXT };  // idx 0..4 used
    g_chartSpr.createPalette(pal, 16); g_chartOk = true;
  } else Serial.println("WARN: chart sprite alloc failed -- graph falls back to direct-draw");

  SD_MMC.setPins(38, 40, 39, 41, 48, 47);          // dedicated SDMMC bus (not the LCD SPI)
  g_sdOk = SD_MMC.begin("/sdcard", false);         // 4-bit (vendor-proven); history survives power loss
  Serial.printf("SD: %s\n", g_sdOk ? "mounted" : "not present (RAM-only history)");
  bootStep("MOUNT SD CARD ........"); bootResult(g_sdOk, "OK", "NO CARD");
  graphLoad();                                     // restore the power-history ring from SD
  usageLoad();                                      // restore the 30-day per-rate usage ledger from SD
  evtLoad();                                        // restore the event log ring from SD
  stayLoad();                                       // restore Stay Cards from SD
  evtLog(EV_BOOT, (int16_t)esp_reset_reason());     // stamp this boot + its reset reason into the event log
  bootStep("RESTORE HISTORY ......"); bootResult(g_gCount > 0, "OK", "EMPTY");

  g_lastTouchMs = millis();                         // start the idle-sleep countdown from boot
  bootDone();
  g_scr = SCR_REACTOR; g_scrDirty = true;          // loop() paints the reactor on first pass
}

// ---------------------------------------------------------------- outbound TLS foundation (Step 1a)
// Shared by the coming OTA (manifest/bin over HTTPS) + ntfy alerts. Two hard-won constraints on this
// PSRAM-off S3, both from the intercom firmware's experience:
//   1. TLS cert validation needs a correct wall-clock time -- an unsynced clock (epoch 0) fails every
//      handshake on the cert's notBefore/notAfter. So we lazy-sync SNTP (UTC) on first use.
//   2. mbedtls on the 8 KB loop()/Arduino task stack PANICs -- so the handshake runs on a dedicated
//      16 KB task, never inline. A WiFiClientSecure context also wants ~40-45 KB of heap; STA mode only
//      (own-AP runs the captive DNS + is the documented low-heap regime).
static volatile bool g_timeSynced = false;
// Fires on a REAL SNTP sync (esp_sntp notification) -- the ONLY thing that marks the clock trustworthy. Async-safe:
// it lands whenever the sync actually completes (even after syncTime's wait window), so a hand-set clock is never
// mistaken for NTP, and a late/background sync still upgrades provenance + the ledger date on its own.
static void onSntpSync(struct timeval*) { bool was = g_timeSynced; g_clkProv = CLK_NTP; g_epochReal = true; recomputeDateReal(); g_timeSynced = true; if (!was) evtLog(EV_NTP_SYNC, 0); }   // M5: NTP gives a real epoch; the date needs a zone too (recompute)
static bool syncTime(uint32_t timeoutMs) {                       // safe from loop() (SNTP is light, no mbedtls); default in the fwd decl
  if (g_timeSynced) return true;
  if (WiFi.status() != WL_CONNECTED) return false;               // needs STA/WAN; own-AP has no internet
  sntp_set_time_sync_notification_cb(onSntpSync);                // (idempotent) fire onSntpSync on the real answer
  configTzTime(tzPosix(), "pool.ntp.org", "time.nist.gov");      // starts SNTP + applies the selected zone (epoch stays UTC for TLS)
  uint32_t t0 = millis();
  while (!g_timeSynced && millis() - t0 < timeoutMs) delay(200); // g_timeSynced is set by onSntpSync, not by a plausible epoch
  logf(g_timeSynced ? "time: SNTP ok (epoch %lu)" : "time: SNTP pending (async, epoch %lu)", (unsigned long)time(nullptr));
  return g_timeSynced;
}

// One-shot TLS handshake probe to <host>:443 on a 16 KB task -- validates the SNTP + CA-pinning path end
// to end before OTA/ntfy ride on it, and measures the real heap cost on hardware. Logs to /log + Serial.
static char g_tlsHost[64];
static void tlsTask(void*) {
  const char* host = g_tlsHost;
  size_t heap0 = ESP.getFreeHeap();
  if (!syncTime()) { logf("tls: no time -> cannot validate cert"); vTaskDelete(nullptr); return; }
  WiFiClientSecure client;
  client.setCACert(ISRG_ROOT_X1);
  client.setHandshakeTimeout(12);                                // seconds
  uint32_t t0 = millis();
  bool ok = client.connect(host, 443);
  uint32_t dt = millis() - t0;
  if (ok) {
    logf("tls: %s:443 OK in %lums (heap %u->%u)", host, (unsigned long)dt, (unsigned)heap0, (unsigned)ESP.getFreeHeap());
    client.stop();
  } else {
    char e[96] = ""; client.lastError(e, sizeof(e));
    logf("tls: %s FAILED (%s) heap0=%u", host, e[0] ? e : "connect", (unsigned)heap0);
  }
  vTaskDelete(nullptr);
}
static void tlsProbe(const char* host) {
  if (WiFi.status() != WL_CONNECTED) { logf("tls: not on STA Wi-Fi -> skip"); return; }
  strlcpy(g_tlsHost, host, sizeof(g_tlsHost));
  logf("tls: probing %s ...", g_tlsHost);
  xTaskCreatePinnedToCore(tlsTask, "tlsprobe", 16384, nullptr, 1, nullptr, 1);  // 16 KB stack, core 1
}

// ---------------------------------------------------------------- ntfy push alerts (Step 1)
// Pushes alerts to the owner's phone via ntfy.sh over HTTPS (reuses the TLS foundation above). Default
// OFF; the owner enables it + subscribes by scanning the topic (QR/link on the web Settings page), where
// each alert type can be toggled. Detection is centralized in ntfyPoll() -- edge-triggered off the SAME
// globals the screen/outage banner use -- so there are no notify() calls scattered through the UI code.
enum NtfyAlert { NA_SHORE_LOST=0, NA_SHORE_OK, NA_ON_BATT, NA_VOLT, NA_OVERCURRENT, NA_WD_LOST, NA_WD_OK, NA_HEARTBEAT, NA_COUNT };
#define NTFY_COOLDOWN_MS 300000       // per-alert min gap -> dedup a flapping condition
#define NTFY_MAX_TRIES   3            // v2.1 R2: total send attempts before an edge-alert is dropped (a router/TLS blip mustn't lose "Shore power LOST")
// ---- alert-trust (v2.1 1.4): dead-man's switch (reachability) + opt-in daily heartbeat ----
#define ALERT_DARK_MS   (20u*60u*1000u)   // ntfy host unreachable this long while alerts ON -> "alerts are dark"
static volatile bool g_ntfyReach  = true;     // last reachability-probe result (true until first probe)
static uint32_t      g_ntfyReachMs = 0;       // millis() of the last SUCCESSFUL reach
static uint32_t      g_alertsDarkSince = 0;   // millis() alerts went dark (0 = not dark); for the catch-up window
static int           g_hbLastDay   = -1;      // yyyymmdd of the last heartbeat sent (fire once/day)
static volatile bool g_reachBusy   = false;
static char          g_reachHostSnap[48] = "";   // finding#5: snapshot the probe host so reachTask never reads a live g_cfg.ntfyServer String (symmetry with the other M3 snapshots; future-proofs a custom-server setter)
// "alerts are dark" = push is ON but we can't reach the notify server (no internet / server down). Surfaced by the
// 1.6 health banner so a dead alert channel is visible on-device instead of silently failing.
static bool alertsDark() { return g_cfg.ntfyOn && g_alertsDarkSince != 0; }

struct NtfyMsg { char title[48]; char body[100]; char tags[24]; uint8_t prio; uint8_t tries; };  // tries: R2 bounded-retry counter (0 on first enqueue)
static NtfyMsg          g_nq[4];                       // small send ring (alerts are rare); drops oldest if full
static volatile uint8_t g_nqHead=0, g_nqCount=0;
static volatile bool    g_ntfySending=false;
static portMUX_TYPE     g_nqMux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t         g_ntfyLast[NA_COUNT] = {0};
static char             g_ntfySrvSnap[48] = "", g_ntfyTopSnap[48] = "";   // M3: server/topic snapshot -> ntfyTask (runs seconds during TLS) never dereferences a g_cfg String that handleNtfy's regen may free underneath it

static void ntfyEnsureTopic() {
  if (g_cfg.ntfyTopic.length()) return;
  char t[20]; snprintf(t, sizeof(t), "hughes-%08x", (unsigned)esp_random());
  g_cfg.ntfyTopic = t; configPersist(); logf("ntfy: minted topic %s", t);
}

static bool ntfyPost(const NtfyMsg& m) {              // on the ntfy task (16 KB stack); one TLS ctx at a time
  if (WiFi.status() != WL_CONNECTED) return false;
  WiFiClientSecure client; client.setCACert(ISRG_ROOT_X1); client.setHandshakeTimeout(12);
  HTTPClient http; String url = String("https://") + g_ntfySrvSnap + "/" + g_ntfyTopSnap;   // M3: use the snapshot, not the live g_cfg Strings
  if (!http.begin(client, url)) { logf("ntfy: begin failed"); return false; }
  http.addHeader("Title", m.title);
  char p[2] = { (char)('0' + (m.prio < 1 ? 1 : (m.prio > 5 ? 5 : m.prio))), 0 }; http.addHeader("Priority", p);
  if (m.tags[0]) http.addHeader("Tags", m.tags);
  int code = http.POST((uint8_t*)m.body, strlen(m.body));
  http.end();
  logf("ntfy: '%s' -> %d", m.title, code);
  return code > 0 && code < 400;
}

static void ntfyTask(void*) {
  for (;;) {
    NtfyMsg m; bool have=false;
    portENTER_CRITICAL(&g_nqMux);
    if (g_nqCount) { m = g_nq[g_nqHead]; g_nqHead=(g_nqHead+1)%4; g_nqCount--; have=true; }
    else           { g_ntfySending=false; }           // mark idle INSIDE the lock so a racing enqueue re-spawns
    portEXIT_CRITICAL(&g_nqMux);
    if (!have) break;
    bool ok = ntfyPost(m);
    if (!ok && ++m.tries < NTFY_MAX_TRIES) {             // R2: transient failure (TLS/router blip) -> re-queue with backoff instead of dropping the edge alert
      portENTER_CRITICAL(&g_nqMux);
      uint8_t idx = (g_nqHead + g_nqCount) % 4;
      if (g_nqCount >= 4) g_nqHead = (g_nqHead + 1) % 4; else g_nqCount++;   // full -> drop oldest (same policy as ntfyEnqueue)
      g_nq[idx] = m;
      portEXIT_CRITICAL(&g_nqMux);
      logf("ntfy: '%s' retry %u/%u", m.title, m.tries, NTFY_MAX_TRIES - 1);
      delay(4000);                                        // short backoff before the retry (BLE stays released only inside ntfyPost)
    } else {
      delay(300);
    }
  }
  vTaskDelete(nullptr);
}

static void ntfyEnqueue(const char* title, const char* body, int prio, const char* tags) {
  bool spawn;
  portENTER_CRITICAL(&g_nqMux);
  uint8_t idx = (g_nqHead + g_nqCount) % 4;
  if (g_nqCount >= 4) g_nqHead = (g_nqHead + 1) % 4; else g_nqCount++;     // full -> drop the oldest
  NtfyMsg& m = g_nq[idx];
  if (g_cfg.nick.length()) snprintf(m.title, sizeof(m.title), "%s: %s", g_cfg.nick.c_str(), title);  // v2.1 1.5: name the bridge in the alert ("Pearl: Shore power LOST")
  else                     strlcpy(m.title, title, sizeof(m.title));
  strlcpy(m.body, body, sizeof(m.body));
  strlcpy(m.tags, tags ? tags : "", sizeof(m.tags)); m.prio = (uint8_t)prio; m.tries = 0;
  spawn = !g_ntfySending; if (spawn) g_ntfySending = true;
  portEXIT_CRITICAL(&g_nqMux);
  if (spawn) {   // M3 (finding#4): snapshot ONLY when no sender task exists yet -> the buffers are never written while ntfyTask is mid-ntfyPost reading them (no torn URL). Queued-while-sending messages reuse the running task's snapshot.
    strlcpy(g_ntfySrvSnap, g_cfg.ntfyServer.c_str(), sizeof(g_ntfySrvSnap));
    strlcpy(g_ntfyTopSnap, g_cfg.ntfyTopic.c_str(),  sizeof(g_ntfyTopSnap));
  }
  logf("ntfy: queued '%s'", title);
  if (spawn && xTaskCreatePinnedToCore(ntfyTask, "ntfy", 16384, nullptr, 1, nullptr, 1) != pdPASS) {
    g_ntfySending = false;                               // R1: spawn failed -> clear the busy flag so the NEXT alert can respawn the sender (msg stays queued)
    logf("ntfy: task spawn FAILED (will retry on next alert)");
  }
}

// ---- Phase 4.1: on-demand support bundle (OWNER-INITIATED, not telemetry) ---------------------
// A person explicitly asks the bridge to send its recent diagnostics to the maker for help. There is
// NO periodic reporting, NO heartbeat, NO passive monitoring -- this fires only on a deliberate tap /
// serial cmd / portal button (see [[no-passive-user-monitoring]]). Bundles status + dmesg (which already
// carries the /log + event-log breadcrumbs) + the last events, POSTed once over TLS on a dedicated 16 KB
// task (mbedtls won't fit the loop stack), to the maker's VPS. Host is support.example.com (the Caddy/LE
// box) -- NOT firmware.flensor.com (DreamHost static); the pinned ISRG Root X1 validates both.
#define SUPPORT_HOST  "support.example.com"
// R9: the support-bundle auth token is a SECRET -- it must ship in the .bin but must NOT live in tracked source
// (this repo is slated for GitHub, and the old inline value is in git history -> ROTATE it before publishing:
// change both this header AND fleet-monitor/app/server.py, then redeploy the VPS). Real value lives in the
// gitignored src/support_token.h (copy src/support_token.example.h); a secret-less checkout still builds.
#if __has_include("support_token.h")
  #include "support_token.h"
#endif
#ifndef SUPPORT_TOKEN
  #define SUPPORT_TOKEN "SUPPORT_TOKEN_UNSET"   // no secret on disk -> bundles won't authenticate at the VPS, but the build succeeds
#endif
static volatile bool g_supportBusy = false;
static uint32_t      g_supportLast = 0;                // cooldown so a button masher can't spam the maker
static char          g_supportNickSnap[48] = "";       // M3: nick snapshot -> supportTask (runs seconds) never reads a g_cfg.nick String a web handler may reassign
static volatile bool g_otaBusy = false;                // declared here (ahead of the OTA section) so alertTrustPoll's M2 TLS-contention guard can see it

static void buildSupportBundle(String& b) {
  char mac[20]; macStr(mac, sizeof(mac));
  b.reserve(6144);
  b += "==== HUGHES SUPPORT BUNDLE ====\n";
  b += "fw=" FW_VERSION "  mac="; b += mac;
  b += "  boot="; b += String((unsigned long)dmesgBootCount());
  b += "  reset="; b += dmesgLastReset();
  b += "  uptime_s="; b += String((unsigned long)(millis() / 1000));
  b += "\nnick="; b += g_supportNickSnap;             // M3: snapshot, not the live String
  b += "  netmode="; b += String(g_cfg.netMode);
  b += "  ip="; b += (g_cfg.netMode == 0 && WiFi.status() == WL_CONNECTED) ? WiFi.localIP().toString() : WiFi.softAPIP().toString();
  b += "  heap="; b += String((unsigned long)ESP.getFreeHeap());
  b += "\n==== STATUS ====\n";
  { char w[40]; statusState(w, sizeof(w), nullptr); b += "state="; b += w; }
  b += "  batt="; b += String(g_battPct); b += "%  on_batt="; b += String((int)g_onBattery);
  b += "  wd_connected="; b += String((int)g_connected);
  b += "  clk_prov="; b += String((int)g_clkProv); b += "  stay_kwh="; b += String(stayKwh(), 3);
  b += "\n==== DMESG ====\n"; { String d; dmesgToString(d); b += d; }
  int head, count;
  portENTER_CRITICAL(&g_evtMux); head = g_evtHead; count = g_evtCount; portEXIT_CRITICAL(&g_evtMux);
  int want = count < 40 ? count : 40;                  // last 40 structured events (dmesg already has the text breadcrumbs)
  b += "==== EVENTS ("; b += String(want); b += " of "; b += String(count); b += ") ====\n";
  int start = (head - want + EVT_CAP) % EVT_CAP;
  for (int i = 0; i < want; i++) {
    EvtRec r = g_evt[(start + i) % EVT_CAP];
    b += evtTypeStr(r.type); b += " epoch="; b += String((unsigned long)r.epoch);
    b += " mono="; b += String((unsigned long)r.mono); b += " arg="; b += String((int)r.arg);
    b += (r.flags & EVF_CLK_UNSURE) ? " clk?\n" : "\n";
  }
}
static void supportTask(void*) {
  String body; buildSupportBundle(body);
  char mac[20]; macStr(mac, sizeof(mac));
  if (WiFi.status() == WL_CONNECTED) {
    WiFiClientSecure client; client.setCACert(ISRG_ROOT_X1); client.setHandshakeTimeout(12);
    HTTPClient http;
    String url = "https://" SUPPORT_HOST "/support?tok=" SUPPORT_TOKEN "&mac="; url += mac;
    if (http.begin(client, url)) {
      http.addHeader("Content-Type", "text/plain");
      int code = http.POST((uint8_t*)body.c_str(), body.length());
      http.end();
      logf("support: bundle %u B -> %d", (unsigned)body.length(), code);
    } else logf("support: begin failed");
  } else logf("support: no wifi");
  g_supportBusy = false;
  vTaskDelete(nullptr);
}
static bool supportFire() {                            // owner-initiated only; returns false if busy/cooldown/offline
  if (g_supportBusy) return false;
  if (WiFi.status() != WL_CONNECTED) { logf("support: no wifi -- need home Wi-Fi"); return false; }
  uint32_t now = millis();
  if (g_supportLast && now - g_supportLast < 30000) { logf("support: cooldown (wait 30s)"); return false; }
  g_supportLast = now; g_supportBusy = true;
  strlcpy(g_supportNickSnap, g_cfg.nick.c_str(), sizeof(g_supportNickSnap));   // M3: snapshot nick on this (web) task before the support task reads it
  if (xTaskCreatePinnedToCore(supportTask, "support", 16384, nullptr, 1, nullptr, 1) != pdPASS) {
    g_supportBusy = false; g_supportLast = 0;            // R1: spawn failed -> clear busy + cooldown so "Send diagnostics" doesn't 429 forever
    logf("support: task spawn FAILED"); return false;
  }
  return true;
}
void handleSupportBundle() {                           // POST /support_bundle -- portal "Send diagnostics" button
  bool ok = supportFire();
  server.sendHeader("Cache-Control", "no-store");
  server.send(ok ? 200 : 429, "text/plain", ok ? "sending diagnostics to support...\n" : "busy, cooling down, or no home Wi-Fi\n");
}

// Fire an alert if the master switch is on, this alert bit is enabled, and it's past its cooldown.
static void ntfyAlert(int a, const char* title, const char* body, int prio, const char* tags) {
  if (!g_cfg.ntfyOn || !(g_cfg.ntfyMask & (1 << a))) return;
  uint32_t now = millis();
  if (g_ntfyLast[a] && now - g_ntfyLast[a] < NTFY_COOLDOWN_MS) return;
  g_ntfyLast[a] = now ? now : 1;
  ntfyEnqueue(title, body, prio, tags);
}

// Edge-triggered alert detection, called ~every 4s from loop() (bridge mode). Compares live state to the
// previous poll and fires on transitions -- reusing the same signals the screen + outage banner use.
static void ntfyPoll() {
  if (!g_cfg.ntfyOn) return;
  ntfyEnsureTopic();
  if (WiFi.status() != WL_CONNECTED) return;
  // R1 follow-up (finding#7): if a prior xTaskCreate failed and left messages queued with no sender, retry the
  // spawn here (~4 s cadence, Wi-Fi confirmed) so a "Shore power LOST" queued at the OOM moment isn't stranded
  // until the next edge fires.
  { bool respawn = false;
    portENTER_CRITICAL(&g_nqMux);
    if (g_nqCount > 0 && !g_ntfySending) { g_ntfySending = true; respawn = true; }
    portEXIT_CRITICAL(&g_nqMux);
    if (respawn && xTaskCreatePinnedToCore(ntfyTask, "ntfy", 16384, nullptr, 1, nullptr, 1) != pdPASS) {
      g_ntfySending = false;
    }
  }
  uint32_t now = millis();
  Leg a1, a2; bool conn;
  xSemaphoreTake(g_mtx, portMAX_DELAY); a1=g_leg[0]; a2=g_leg[1]; conn=g_connected; xSemaphoreGive(g_mtx);
  bool haveWd = g_cfg.hughesMac.length() || g_cfg.hughesName.length();

  // shore power (outage) -- same formula as the on-screen banner
  bool outage = g_hadLink && !g_released && (now - g_lastNotifyMs) > OUTAGE_MS;
  static int8_t pOut = -1;
  if (pOut >= 0 && (bool)pOut != outage) {
    if (outage) ntfyAlert(NA_SHORE_LOST, "Shore power LOST", "No data from the Watchdog - check the pedestal", 5, "rotating_light");
    else        ntfyAlert(NA_SHORE_OK,   "Shore power restored", "The Watchdog is reporting again", 3, "white_check_mark");
  }
  pOut = outage;

  // on battery (onset only; the shore-restored alert covers the recovery side)
  static int8_t pBatt = -1;
  if (pBatt >= 0 && g_onBattery && !pBatt) ntfyAlert(NA_ON_BATT, "Running on battery", "The bridge lost USB/shore power", 4, "battery");
  pBatt = g_onBattery ? 1 : 0;

  // per-leg voltage fault (E1/E2 -> err nonzero)
  static uint8_t pE1 = 0, pE2 = 0;
  if (a1.valid && a1.err && a1.err != pE1) { char b[96]; snprintf(b, sizeof(b), "L1 error E%u at %.0f V (safe band 104-132 V)", a1.err, a1.volts); ntfyAlert(NA_VOLT, "L1 voltage fault", b, 5, "warning"); }
  if (a2.valid && a2.err && a2.err != pE2) { char b[96]; snprintf(b, sizeof(b), "L2 error E%u at %.0f V (safe band 104-132 V)", a2.err, a2.volts); ntfyAlert(NA_VOLT, "L2 voltage fault", b, 5, "warning"); }
  if (a1.valid) pE1 = a1.err;
  if (a2.valid) pE2 = a2.err;

  // over-current near the breaker (rising edge)
  bool hot = (a1.valid && (now - a1.seenMs) < 5000 && a1.amps >= ALARM_FRAC * g_cfg.svcAmps) ||
             (a2.valid && (now - a2.seenMs) < 5000 && a2.amps >= ALARM_FRAC * g_cfg.svcAmps);
  static int8_t pHot = -1;
  if (pHot >= 0 && hot && !pHot) { char b[80]; snprintf(b, sizeof(b), "A leg is near the %d A breaker trip", g_cfg.svcAmps); ntfyAlert(NA_OVERCURRENT, "High current draw", b, 4, "zap"); }
  pHot = hot ? 1 : 0;

  // Watchdog BLE link lost / reacquired -- debounced >=20s against the RSSI flaps the LEDGER notes
  static int8_t pConn = -1; static uint32_t lostSince = 0; static bool lostAlerted = false;
  if (haveWd) {
    if (!conn) {
      if (pConn == 1) lostSince = now;
      if (!lostAlerted && lostSince && now - lostSince > 20000) { ntfyAlert(NA_WD_LOST, "Watchdog link lost", "BLE disconnected for >20s", 4, "electric_plug"); lostAlerted = true; }
    } else {
      if (lostAlerted) { ntfyAlert(NA_WD_OK, "Watchdog reconnected", "BLE link restored", 3, "electric_plug"); lostAlerted = false; }
      lostSince = 0;
    }
    pConn = conn ? 1 : 0;
  } else { pConn = -1; lostSince = 0; lostAlerted = false; }
}

// Sensor-edge -> event log. A sibling of ntfyPoll() run on the same ~4 s loop cadence, but WITHOUT
// ntfyPoll's `ntfyOn`/Wi-Fi early-outs: the audit log must record whether or not push alerts are on
// or the box is online. Keeps its own edge state (independent of ntfyPoll's) and logs the debounced
// BLE lost/reacquired the same way (>=20 s) so RSSI flaps don't churn the ring.
static void evtPoll() {
  uint32_t now = millis();
  // shore power: while the link is intentionally released (phone hand-off / OTA) the outage formula lies
  // (data stops, g_lastNotifyMs goes stale), so suspend edge detection and re-arm -- otherwise the audit
  // record gets a phantom LOST/BACK pair on every hand-off. Only log BACK if we actually logged a LOST.
  static int8_t pOut = -1; static bool shoreLostLogged = false; static bool pReleased = false;
  if (g_released) {
    // R3: entering a blind window (OTA / phone hand-off) with an outage still open -> close it here. A release
    // logs no BACK on its own, so an unmatched LOST would otherwise run to stay-end in the replay (no reboot to
    // fence it). If shore is still out when the link resumes, a fresh LOST re-opens it -- so we only lose the
    // (short) blind window itself, not the whole tail of the stay.
    if (!pReleased && shoreLostLogged) { evtLog(EV_SHORE_BACK, 0); shoreLostLogged = false; }
    pOut = -1;
  }
  else {
    bool outage = g_hadLink && (now - g_lastNotifyMs) > OUTAGE_MS;
    if (pOut >= 0 && (bool)pOut != outage) {
      if (outage)                 { evtLog(EV_SHORE_LOST, 0); shoreLostLogged = true; }
      else if (shoreLostLogged)   { evtLog(EV_SHORE_BACK, 0); shoreLostLogged = false; }   // suppress a BACK with no preceding LOST (stale-data resume)
    }
    pOut = outage ? 1 : 0;
  }
  pReleased = g_released;

  static int8_t pBatt = -1;
  if (pBatt >= 0 && (g_onBattery ? 1 : 0) != pBatt) evtLog(g_onBattery ? EV_ON_BATT : EV_ON_LINE, 0);
  pBatt = g_onBattery ? 1 : 0;

  bool conn; bool haveWd = g_cfg.hughesMac.length() || g_cfg.hughesName.length();
  xSemaphoreTake(g_mtx, portMAX_DELAY); conn = g_connected; xSemaphoreGive(g_mtx);
  static int8_t pConn = -1; static uint32_t lostSince = 0; static bool lostLogged = false;
  if (haveWd && !g_released) {                       // a deliberate release isn't a link loss -> don't log it
    if (!conn) {
      if (pConn == 1) lostSince = now;
      if (!lostLogged && lostSince && now - lostSince > 20000) { evtLog(EV_BLE_DOWN, 0); lostLogged = true; }
    } else {
      if (lostLogged) { evtLog(EV_BLE_UP, 0); lostLogged = false; }
      lostSince = 0;
    }
    pConn = conn ? 1 : 0;
  } else { pConn = -1; lostSince = 0; lostLogged = false; }   // released or no Watchdog -> re-arm on the next real evaluation
}

// Reachability probe (dead-man's switch): a quick TLS connect to the ntfy host on a 16 KB task (TLS won't fit
// the loop stack). Success stamps g_ntfyReachMs; failure leaves it stale so alertTrustPoll can declare darkness.
static void reachTask(void*) {
  bool ok = false;
  if (WiFi.status() == WL_CONNECTED) {
    WiFiClientSecure c; c.setCACert(ISRG_ROOT_X1); c.setHandshakeTimeout(8);
    ok = c.connect(g_reachHostSnap, 443); c.stop();       // finding#5: snapshot, not the live g_cfg String
  }
  g_ntfyReach = ok; if (ok) g_ntfyReachMs = millis();
  g_reachBusy = false; vTaskDelete(nullptr);
}
// v2.1 1.4: alert-channel trust -- runs on the 4s cadence, NOT gated on Wi-Fi (a dead channel must be noticed).
// Probes the ntfy host ~every 15 min; declares "alerts dark" when unreachable for ALERT_DARK_MS and pushes a
// catch-up when it recovers; fires the opt-in daily heartbeat at the configured local hour.
static void alertTrustPoll() {
  uint32_t now = millis();
  if (!g_cfg.ntfyOn) { g_alertsDarkSince = 0; g_ntfyReachMs = 0; g_ntfyReach = true; return; }   // M4: also clear the last-reach stamp so re-enabling after >20 min doesn't instantly declare DARK + fire a bogus "back online"
  if (g_ntfyReachMs == 0) g_ntfyReachMs = now;             // seed on first poll after boot/enable -> a full grace window before "dark" (no boot false-positive)
  static uint32_t lastProbe = 0;
  uint32_t probeEvery = g_ntfyReach ? (15u*60u*1000u) : (3u*60u*1000u);   // healthy: every 15 min; after a miss: retry every 3 min (real retries before "dark")
  // M2: don't spawn a 4th ~40 KB TLS context while OTA/ntfy/support already hold one (OOM regime: BLE up, PSRAM
  // off) -- and a probe that failed *because* OTA was running would wrongly count toward "alerts dark." These ops
  // are short; skipping defers the probe (lastProbe unchanged) so it runs the moment they finish.
  if (!g_reachBusy && !g_otaBusy && !g_ntfySending && !g_supportBusy &&
      WiFi.status() == WL_CONNECTED && (lastProbe == 0 || now - lastProbe > probeEvery)) {
    lastProbe = now; g_reachBusy = true;
    strlcpy(g_reachHostSnap, g_cfg.ntfyServer.c_str(), sizeof(g_reachHostSnap));   // finding#5: snapshot on this task before reachTask reads it
    if (xTaskCreatePinnedToCore(reachTask, "reach", 16384, nullptr, 1, nullptr, 1) != pdPASS) g_reachBusy = false;   // spawn failed -> don't wedge the switch
  }
  bool reachable = (now - g_ntfyReachMs < ALERT_DARK_MS);   // dark only after a SUSTAINED gap (>=~6 retries at 3-min spacing over 20 min)
  if (!reachable) {
    if (g_alertsDarkSince == 0) { g_alertsDarkSince = now ? now : 1; logf("alerts: DARK (notify server unreachable)"); }
  } else if (g_alertsDarkSince && !g_reachBusy) {        // recovered -> one catch-up push (bypasses the alert mask). finding#3/F2: defer while a reach probe is IN FLIGHT (g_reachBusy, not just spawned-this-pass), so the two ~40KB TLS contexts don't overlap
    uint32_t mins = (now - g_alertsDarkSince) / 60000;
    char b[100]; snprintf(b, sizeof(b), "Phone alerts were offline for ~%lu min; the channel is working again.", (unsigned long)mins);
    ntfyEnqueue("Alerts back online", b, 3, "signal_strength");
    logf("alerts: recovered after %lu min", (unsigned long)mins); g_alertsDarkSince = 0;
  }
  if (g_cfg.hbHour >= 0 && !g_reachBusy) {                  // opt-in daily heartbeat (needs a trusted clock). F2: defer while a reach probe is in flight (8s handshake can outlast the 4s cadence) so the heartbeat's ntfy TLS doesn't overlap it
    // g_hbLastDay is RAM-only: a reboot within the heartbeat hour can re-send once (accepted -- a stray "All good"
    // is harmless, not worth NVS churn). A send that fails isn't retried that day (silence = problem, by design).
    uint32_t today = localDateNow();
    if (today && localHourNow() == g_cfg.hbHour && (int)today != g_hbLastDay) {
      g_hbLastDay = (int)today;
      ntfyEnqueue("All good", "Daily check-in: the bridge is online and watching your shore power.", 2, "white_check_mark");
      logf("alerts: heartbeat sent");
    }
  }
}

// POST /ntfy?on=0|1&mask=N&regen=1&test=1&hb=H -- web Settings: master toggle, per-alert bitmask, mint a new
// topic, send a test push, or set the daily heartbeat hour (hb=-1 off, 0-23). Unlocked like the other web writes.
void handleNtfy() {
  bool any = false;
  if (server.hasArg("on"))   { g_cfg.ntfyOn = server.arg("on").toInt() != 0; if (g_cfg.ntfyOn) ntfyEnsureTopic(); any = true; }
  if (server.hasArg("mask")) { g_cfg.ntfyMask = server.arg("mask").toInt() & 0x7F; any = true; }
  if (server.hasArg("hb") && server.arg("hb").length()) { int h = server.arg("hb").toInt(); g_cfg.hbHour = (h >= 0 && h <= 23) ? h : -1; any = true; }   // daily heartbeat hour (-1 off); empty = no change (guards an unpopulated select)
  if (server.hasArg("regen")){ g_cfg.ntfyTopic = ""; ntfyEnsureTopic(); any = true; }
  if (any) configPersist();
  if (server.hasArg("test")) {
    ntfyEnsureTopic();
    ntfyEnqueue("Test alert", "Hughes bridge notifications are working.", 3, "wave");
  }
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "text/plain", g_cfg.ntfyOn ? "ok\n" : "off\n");
}

// ---------------------------------------------------------------- OTA firmware update (Step 2)
// Owner-triggered: fetch the JSON manifest (version/url/sha256/size) over HTTPS, compare version to
// FW_VERSION, and if it differs download+verify+flash the idle slot then reboot. Runs on a dedicated 16 KB
// task (mbedtls + the ~40 KB TLS heap PSRAM frees). Releases the BLE link first (heap + avoids a mid-flash
// supervision timeout). String compare (not semver) -> repoint the manifest at an older bin to downgrade.
static volatile bool g_otaCheckOnly = false;           // true = check the manifest + report, DON'T download
static char g_otaStatus[64] = "idle";
static char g_otaAvail[24]  = "";                      // newer version found by a check ("" = none / up to date)
static char g_otaUrlSnap[160] = "";                    // M3: manifest-URL snapshot -> otaTask (runs seconds) never reads a g_cfg.otaUrl String a web/serial handler may reassign (finding#8: 160 > the 128-byte serial URL buffer, so no silent truncation)

static void otaProgressDraw(int pct) {
  static int last = -1; if (pct == last) return;
  if (last < 0 || pct < last) {                        // first call, or a new download -> paint the static frame ONCE
    tft.fillScreen(C_BG); tft.setTextDatum(MC_DATUM);
    tft.setTextColor(C_CYAN, C_BG); tft.drawString("UPDATING", 120, 116, 4); tft.drawString("FIRMWARE", 120, 148, 4);
    tft.drawRect(20, 224, 200, 14, C_LINE);
    tft.setTextColor(C_DIM, C_BG); tft.drawString("do not power off", 120, 262, 2);
    tft.setTextDatum(TL_DATUM);
  }
  last = pct;
  char b[8]; snprintf(b, sizeof(b), "%d%%", pct);      // % updates in place (padded overwrite -> no flicker)
  tft.setTextDatum(MC_DATUM); tft.setTextColor(C_TEXT, C_BG); tft.setTextPadding(100);
  tft.drawString(b, 120, 190, 4); tft.setTextPadding(0); tft.setTextDatum(TL_DATUM);
  tft.fillRect(21, 225, 198*pct/100, 12, C_CYAN);      // bar fill grows inside the (once-drawn) border
}

// Tiny JSON scrapers -- the manifest is small + fixed-shape, so strstr beats pulling in a JSON lib.
static String otaJsonStr(const String& j, const char* key) {
  String k = String("\"") + key + "\""; int i = j.indexOf(k); if (i < 0) return "";
  i = j.indexOf(':', i); if (i < 0) return "";
  int q1 = j.indexOf('"', i + 1); if (q1 < 0) return ""; int q2 = j.indexOf('"', q1 + 1); if (q2 < 0) return "";
  return j.substring(q1 + 1, q2);
}
static long otaJsonNum(const String& j, const char* key) {
  String k = String("\"") + key + "\""; int i = j.indexOf(k); if (i < 0) return -1;
  i = j.indexOf(':', i); if (i < 0) return -1;
  return atol(j.c_str() + i + 1);
}

static void otaTask(void*) {
  snprintf(g_otaStatus, sizeof(g_otaStatus), "checking"); logf("ota: checking %s", g_otaUrlSnap);
  if (WiFi.status() != WL_CONNECTED) { snprintf(g_otaStatus, sizeof(g_otaStatus), "no wifi"); g_otaBusy = false; vTaskDelete(nullptr); return; }
  g_released = true; delay(500);                        // drop the BLE link UP FRONT -> free NimBLE heap so the TLS handshake fits
  logf("ota: released BLE, heap=%u", (unsigned)ESP.getFreeHeap());   // the manifest fetch OOM'd with BLE up + a fragmented heap
  syncTime();                                          // TLS cert validity needs a real clock
  int code = 0; String body;
  { WiFiClientSecure c; c.setCACert(ISRG_ROOT_X1); c.setHandshakeTimeout(12);
    HTTPClient h; h.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS); h.setTimeout(12000);
    if (h.begin(c, g_otaUrlSnap)) { code = h.GET(); if (code == 200) body = h.getString(); h.end(); } else code = -2; }   // M3: snapshot, not the live g_cfg String
  if (code != 200) { snprintf(g_otaStatus, sizeof(g_otaStatus), "manifest http %d", code); logf("ota: manifest http %d", code); g_released = false; g_otaBusy = false; vTaskDelete(nullptr); return; }
  String ver = otaJsonStr(body, "version"), url = otaJsonStr(body, "url"), sha = otaJsonStr(body, "sha256");
  long size = otaJsonNum(body, "size");
  logf("ota: manifest v=%s size=%ld (running %s)", ver.c_str(), size, FW_VERSION);
  if (!ver.length() || ver == FW_VERSION) { snprintf(g_otaStatus, sizeof(g_otaStatus), "up to date (%s)", FW_VERSION); g_otaAvail[0]=0; logf("ota: up to date"); g_released = false; g_otaBusy = false; g_scrDirty = true; vTaskDelete(nullptr); return; }
  if (!url.length() || size <= 0 || sha.length() != 64) { snprintf(g_otaStatus, sizeof(g_otaStatus), "bad manifest"); g_otaAvail[0]=0; logf("ota: bad manifest"); g_released = false; g_otaBusy = false; g_scrDirty = true; vTaskDelete(nullptr); return; }
  snprintf(g_otaAvail, sizeof(g_otaAvail), "%s", ver.c_str());          // a newer version exists
  if (g_otaCheckOnly) { snprintf(g_otaStatus, sizeof(g_otaStatus), "update ready: %s", ver.c_str()); logf("ota: %s available", ver.c_str());
                        g_released = false; g_otaBusy = false; g_scrDirty = true; vTaskDelete(nullptr); return; }   // check-only: report, don't download
  snprintf(g_otaStatus, sizeof(g_otaStatus), "updating -> %s", ver.c_str());
  logf("ota: %s -> %s, downloading", FW_VERSION, ver.c_str());
  bool ok = otaFromUrl(url.c_str(), size, sha.c_str(), otaProgressDraw);   // BLE already released above
  if (ok) { logf("ota: flashed %s -> rebooting", ver.c_str()); evtSave(); delay(300); ESP.restart(); }   // flush the event ring before this clean, foreseeable reboot
  snprintf(g_otaStatus, sizeof(g_otaStatus), "FAIL: %s", g_otaErr);
  logf("ota: FAILED (%s)", g_otaErr);
  g_released = false; g_scrDirty = true;               // reconnect BLE; repaint (progress screen clobbered it)
  g_otaBusy = false; vTaskDelete(nullptr);
}
static void otaCheckStart(bool checkOnly = false) {    // checkOnly=false also downloads+reboots if newer; true just reports
  if (g_otaBusy) return;
  g_otaCheckOnly = checkOnly; g_otaBusy = true;
  strlcpy(g_otaUrlSnap, g_cfg.otaUrl.c_str(), sizeof(g_otaUrlSnap));   // M3: snapshot the manifest URL on the caller task before otaTask reads it
  if (xTaskCreatePinnedToCore(otaTask, "ota", 16384, nullptr, 1, nullptr, 1) != pdPASS) {
    g_otaBusy = false;                                   // R1: spawn failed -> clear busy so loop() doesn't freeze the whole UI (touch/render/polls skipped while g_otaBusy)
    snprintf(g_otaStatus, sizeof(g_otaStatus), "busy - try again"); g_scrDirty = true;
    logf("ota: task spawn FAILED");
  }
}

// ---- FIRMWARE page (g_setPage 12, a peer tab before SYSTEM): version, check/update, safe rollback ----
static const Rect R_FW_CHECK  = {14, 118, 212, 34};
static const Rect R_FW_UPDATE = {14, 158, 212, 34};
static const Rect R_FW_ROLL   = {14, 214, 212, 30};
static uint32_t   g_fwRollArm = 0;                       // ROLLBACK two-tap gate

static void firmwareStatusLine() {                       // OTA status -> friendly, human-readable line (padded overwrite)
  const char* s = g_otaStatus; uint16_t sc = C_DIM; char disp[48];
  if      (g_otaBusy)                              { sc = C_AMBER; snprintf(disp, sizeof(disp), "%s", s); }
  else if (g_otaAvail[0])                          { sc = C_GREEN; snprintf(disp, sizeof(disp), "%s", s); }        // "update ready: vX"
  else if (!strcasecmp(s, "idle"))                { sc = C_DIM;   snprintf(disp, sizeof(disp), "tap CHECK FOR UPDATE"); }
  else if (strstr(s, "wifi"))                     { sc = C_RED;   snprintf(disp, sizeof(disp), "home Wi-Fi: STATUS > NETWORK"); }
  else if (strstr(s, "http") || strstr(s, "bad")) { sc = C_RED;   snprintf(disp, sizeof(disp), "couldn't reach the update server"); }
  else if (!strncasecmp(s, "fail", 4))            { sc = C_RED;   snprintf(disp, sizeof(disp), "update failed -- device unchanged"); }
  else                                            { sc = C_DIM;   snprintf(disp, sizeof(disp), "%s", s); }        // "up to date (vX)"
  tft.setTextDatum(TL_DATUM); tft.setTextColor(sc, C_BG); tft.setTextPadding(226);
  tft.drawString(disp, 14, 98, 2); tft.setTextPadding(0);
}
static void firmwareDraw() {
  g_fwRollArm = 0;
  tft.fillScreen(C_BG); tft.setTextDatum(TL_DATUM);
  tft.setTextColor(C_CYAN, C_BG); tft.drawString("< BACK", 6, 6, 2);
  tft.setTextDatum(TR_DATUM); tft.setTextColor(C_TEXT, C_BG); tft.drawString("FIRMWARE", 234, 6, 2);
  tft.setTextDatum(TL_DATUM); tft.drawFastHLine(0, 28, 240, C_LINE);
  tft.setTextColor(C_DIM,  C_BG); tft.drawString("YOUR VERSION", 14, 40, 1);
  tft.setTextColor(C_CYAN, C_BG); tft.drawString("v" FW_VERSION, 14, 54, 4);   // boot/slot diagnostics dropped -> they live on STATUS
  bool online = (g_cfg.netMode == 0 && WiFi.status() == WL_CONNECTED);       // OTA needs home Wi-Fi + WAN; AP/OFF can't reach the server
  if (online) {
    firmwareStatusLine();
    btn(R_FW_CHECK, "CHECK FOR UPDATE", C_CYAN, C_CYAN, 2);
    if (g_otaAvail[0]) btn(R_FW_UPDATE, "UPDATE NOW", C_GREEN, C_GREEN, 2);
    else               btn(R_FW_UPDATE, "UPDATE NOW", C_LINE, C_DIM, 2);     // dim when nothing new is staged
  } else {                                                                   // offline: grey both out, explain why
    tft.setTextColor(C_RED, C_BG); tft.setTextPadding(226);
    tft.drawString("home Wi-Fi: STATUS > NETWORK", 14, 98, 2); tft.setTextPadding(0);
    btn(R_FW_CHECK,  "CHECK FOR UPDATE", C_LINE, C_DIM, 2);
    btn(R_FW_UPDATE, "UPDATE NOW",       C_LINE, C_DIM, 2);
  }
  tft.setTextColor(C_DIM, C_BG); tft.drawString("updates are safety-checked; a bad", 14, 196, 1);
  tft.drawString("one undoes itself automatically", 14, 208, 1);
  btn(R_FW_ROLL, "UNDO LAST UPDATE", C_LINE, C_AMBER, 2);
  tft.setTextColor(C_DIM, C_BG); tft.drawString("if the device acts up after updating", 14, R_FW_ROLL.y + R_FW_ROLL.h + 2, 1);
  drawNav();
}
static void firmwareTick(uint32_t now) {                 // time out the rollback arm
  if (g_fwRollArm && now - g_fwRollArm > 4000) { g_fwRollArm = 0; btn(R_FW_ROLL, "UNDO LAST UPDATE", C_LINE, C_AMBER, 2); }
}
static bool firmwareTouch(int x, int y) {
  if (g_fwRollArm && !hit(R_FW_ROLL, x, y)) { g_fwRollArm = 0; btn(R_FW_ROLL, "UNDO LAST UPDATE", C_LINE, C_AMBER, 2); }   // any other tap disarms
  bool online = (g_cfg.netMode == 0 && WiFi.status() == WL_CONNECTED);       // no WAN in AP/OFF -> CHECK/UPDATE are dead here
  if (hit(R_FW_CHECK, x, y)) {
    if (!online) { snprintf(g_otaStatus, sizeof(g_otaStatus), "no wifi"); firmwareStatusLine(); return true; }   // dead-tap feedback (maps to "needs home Wi-Fi")
    if (!g_otaBusy) { snprintf(g_otaStatus, sizeof(g_otaStatus), "checking..."); firmwareStatusLine(); otaCheckStart(true); }   // show it before the UI freezes
    return true;
  }
  if (hit(R_FW_UPDATE, x, y)) {
    if (g_otaBusy) return true;
    if (!online) { snprintf(g_otaStatus, sizeof(g_otaStatus), "no wifi"); firmwareStatusLine(); return true; }   // dead-tap feedback (maps to "needs home Wi-Fi")
    if (g_otaAvail[0]) { snprintf(g_otaStatus, sizeof(g_otaStatus), "updating..."); firmwareStatusLine(); otaCheckStart(false); }   // download + verify + reboot
    else { snprintf(g_otaStatus, sizeof(g_otaStatus), "tap CHECK first"); firmwareStatusLine(); }   // dead-tap feedback
    return true;
  }
  if (hit(R_FW_ROLL, x, y)) {                             // two-tap: revert to the other A/B slot
    if (g_fwRollArm) { g_fwRollArm = 0;
      if (otaRollback()) { tft.fillScreen(C_BG); tft.setTextDatum(MC_DATUM); tft.setTextColor(C_CYAN, C_BG); tft.drawString("RESTARTING...", 120, 160, 4); tft.setTextDatum(TL_DATUM); logf("fw: manual undo -> other slot, reboot"); delay(400); ESP.restart(); }
      else { snprintf(g_otaStatus, sizeof(g_otaStatus), "nothing to undo"); firmwareStatusLine(); btn(R_FW_ROLL, "UNDO LAST UPDATE", C_LINE, C_AMBER, 2); }
    }
    else { g_fwRollArm = millis(); btn(R_FW_ROLL, "TAP AGAIN: UNDO", C_RED, C_RED, 2); }
    return true;
  }
  return false;
}

// Web writes for OTA are gated on the simple word "hughes" (owner decision -- a foot-gun guard, not real
// auth; the own-AP already needs the API key, home Wi-Fi implies physical access). See OTA_DMESG_NTFY_PLAN.
static bool otaWebGate() { return server.hasArg("key") && server.arg("key") == "hughes"; }

void handleOta() {                                      // POST /ota?key=hughes -- check + auto-update if newer
  if (!otaWebGate()) { server.send(401, "text/plain", "unauthorized: add ?key=hughes\n"); return; }
  otaCheckStart();
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "text/plain", "checking for update (watch /log or the status)\n");
}
void handleOtaUrl() {                                   // POST /ota_url?key=hughes&url=... -- set the manifest URL
  if (!otaWebGate()) { server.send(401, "text/plain", "unauthorized: add ?key=hughes\n"); return; }
  if (server.hasArg("url") && server.arg("url").startsWith("http")) {
    if (server.arg("url").length() >= 159) { server.send(400, "text/plain", "url too long (max 158)\n"); return; }   // F3: reject rather than silently truncate at the 160-byte otaTask snapshot
    g_cfg.otaUrl = server.arg("url"); configPersist();
  }
  server.send(200, "text/plain", g_cfg.otaUrl + "\n");
}

// --- Minimal serial console (USB-CDC). NOT a REPL -- a few line-based commands for field
// debugging, now that the battery means a USB unplug no longer power-cycles the unit. Type a
// word + Enter; case-insensitive. Works in both BRIDGE and setup-PORTAL modes.
static void handleSerialCmd(const char* c) {
  if (!c[0]) return;
  if (!strcasecmp(c, "reset") || !strcasecmp(c, "reboot")) {
    Serial.println("-> rebooting"); delay(150); ESP.restart();
  } else if (!strcasecmp(c, "reprovision") || !strcasecmp(c, "prov")) {
    Serial.println("-> wiping Wi-Fi/Watchdog config, rebooting into the setup portal");
    delay(150); doReprovision(); ESP.restart();
  } else if (!strcasecmp(c, "factory")) {
    Serial.println("-> FACTORY RESET: wiping ALL config, rebooting into the setup portal");
    delay(150); doFactoryReset(); ESP.restart();
  } else if (!strcasecmp(c, "status") || !strcasecmp(c, "s")) {
    char w[22]; statusState(w, sizeof(w), nullptr);
    IPAddress ip = g_apServing ? WiFi.softAPIP() : WiFi.localIP();
    Serial.printf("mode=%s  netMode=%d  provisioned=%d  apServing=%d\n",
      g_mode == MODE_PROV ? "PROV" : "BRIDGE", g_cfg.netMode, (int)g_cfg.provisioned(), (int)g_apServing);
    Serial.printf("wifi ssid='%s'  ip=%s  rssi=%d\n", g_cfg.wifiSsid.c_str(), ip.toString().c_str(), (int)WiFi.RSSI());
    Serial.printf("watchdog mac='%s'  name='%s'  nick='%s'\n", g_cfg.hughesMac.c_str(), g_cfg.hughesName.c_str(), g_cfg.nick.c_str());
    Serial.printf("ble connected=%d  status='%s'\n", (int)g_connected, w);
    Serial.printf("fw=%s  boot#%lu reset=%s  heap=%u min=%u\n", FW_VERSION,
      (unsigned long)dmesgBootCount(), dmesgLastReset(), (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap());
  } else if (!strcasecmp(c, "version") || !strcasecmp(c, "ver")) {
    Serial.printf("fw=%s  boot#%lu  reset=%s\n", FW_VERSION, (unsigned long)dmesgBootCount(), dmesgLastReset());
  } else if (!strcasecmp(c, "heap") || !strcasecmp(c, "mem")) {
    Serial.printf("heap free=%u  min-free=%u  max-alloc=%u\n",
      (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
    Serial.printf("psram size=%u  free=%u\n", (unsigned)ESP.getPsramSize(), (unsigned)ESP.getFreePsram());
  } else if (!strcasecmp(c, "ip")) {
    IPAddress ip = g_apServing ? WiFi.softAPIP() : WiFi.localIP();
    Serial.printf("ip=%s  %s\n", ip.toString().c_str(), g_apServing ? "(AP)" : "(STA)");
  } else if (!strcasecmp(c, "wifi")) {
    Serial.printf("ssid='%s'  rssi=%d  status=%d  apServing=%d\n",
      WiFi.SSID().c_str(), (int)WiFi.RSSI(), (int)WiFi.status(), (int)g_apServing);
  } else if (!strcasecmp(c, "mac")) {
    char mac[20]; macStr(mac, sizeof(mac)); Serial.printf("mac=%s\n", mac);
  } else if (!strcasecmp(c, "dmesg")) {
    dmesgDump(Serial); Serial.println();
  } else if (!strcasecmp(c, "nosleep")) {
    g_noSleep = true; g_lastTouchMs = millis();      // also bump the timer so it won't sleep before the flag takes hold
    Serial.println("-> no-Watchdog auto deep-sleep DISABLED until reboot");
  } else if (!strcasecmp(c, "time")) {
    bool ok = syncTime(); time_t now = time(nullptr); struct tm ut; gmtime_r(&now, &ut);
    char uts[32]; strftime(uts, sizeof(uts), "%Y-%m-%d %H:%M:%SZ", &ut);
    char lt[24]; localTimeStr(lt, sizeof(lt));
    Serial.printf("time: %s  local=%s (%s)  epoch=%ld  synced=%d\n", uts, lt, tzName(), (long)now, (int)ok);
  } else if (!strncasecmp(c, "show", 4) && c[4] == ' ') {   // dev/screenshot nav: "show <setpage>" | "show r" | "show g"
    const char* a = c + 5;
    g_capHold = strcasecmp(a, "r") != 0;               // pin the shown page for screenshots (suppress outage-yank/idle-sleep); "show r" releases the hold
    if      (!strcasecmp(a, "r")) g_scr = SCR_REACTOR;
    else if (!strcasecmp(a, "g")) g_scr = SCR_GRAPHS;
    else if (!strcasecmp(a, "prov") || !strcasecmp(a, "provqr")) {    // preview the first-run setup screen (screenshot only)
      char l4[8]; macLast4(l4, sizeof(l4)); snprintf(g_setupSsid, sizeof(g_setupSsid), "%s-%s", AP_SSID, l4);
      g_scr = SCR_SETTINGS; g_setPage = !strcasecmp(a, "provqr") ? 91 : 90;
    }
    else { g_scr = SCR_SETTINGS; g_setPage = atoi(a); if (g_setPage == 3) { g_devScan = false; g_devConfirm = -1; } }
    g_scrDirty = true; g_lastTouchMs = millis();
    Serial.printf("show: scr=%d page=%d hold=%d\n", (int)g_scr, g_setPage, (int)g_capHold);
  } else if (!strncasecmp(c, "tap", 3) && c[3] == ' ') {   // dev: inject a tap through the REAL touch path (onTap) -- drives buttons for stress tests
    int tx = 0, ty = 0;
    if (sscanf(c + 4, "%d %d", &tx, &ty) == 2) { Serial.printf("tap-> %d,%d\n", tx, ty); onTap(tx, ty); }
    else Serial.println("usage: tap <x> <y>");
  } else if (!strncasecmp(c, "tz", 2)) {             // "tz" (list + show) or "tz <n>" (select a US timezone)
    if (c[2] == ' ' && c[3]) { g_cfg.tzIndex = tzClamp(atoi(c + 3)); applyTz(); configPersist(); }
    Serial.printf("tz = %d (%s)\n", g_cfg.tzIndex, tzName());
    for (int i = 0; i < TZ_COUNT; i++) Serial.printf("  %d = %s\n", i, TZ_TABLE[i].name);
  } else if (!strncasecmp(c, "tls", 3)) {            // "tls" (default firmware.flensor.com) or "tls <host>"
    tlsProbe((c[3] == ' ' && c[4]) ? c + 4 : "firmware.flensor.com");
  } else if (!strcasecmp(c, "ntfytest")) {           // fire a test push (bypasses the master switch; still needs Wi-Fi)
    ntfyEnsureTopic();
    ntfyEnqueue("Test alert", "Hughes bridge serial test.", 3, "wave");
    Serial.printf("-> ntfy test queued (server %s, topic %s, on=%d)\n", g_cfg.ntfyServer.c_str(), g_cfg.ntfyTopic.c_str(), (int)g_cfg.ntfyOn);
  } else if (!strcasecmp(c, "support")) {            // on-demand: bundle diagnostics -> the maker's VPS (owner-initiated)
    Serial.printf("-> support bundle %s\n", supportFire() ? "queued (watch /log)" : "NOT sent (busy / cooldown / no Wi-Fi)");
  } else if (!strcasecmp(c, "otacheck")) {           // fetch the manifest -> auto-update if newer (reboots on success)
    Serial.printf("-> OTA check %s\n", g_cfg.otaUrl.c_str()); otaCheckStart();
  } else if (!strncasecmp(c, "otaurl", 6)) {         // "otaurl" (print) or "otaurl <url>" (set the manifest URL)
    if (c[6] == ' ' && c[7]) { g_cfg.otaUrl = c + 7; configPersist(); }
    Serial.printf("ota url=%s\n", g_cfg.otaUrl.c_str());
  } else if (!strcasecmp(c, "otainfo")) {
    Serial.printf("fw=%s  running='%s'  next='%s'  status='%s'\n", FW_VERSION, otaRunningLabel(), otaNextLabel(), g_otaStatus);
  } else if (!strcasecmp(c, "rollback")) {           // flip to the other A/B slot + reboot (recover a bad image)
    if (otaRollback()) { Serial.println("-> rolling back to the other slot, rebooting"); delay(200); ESP.restart(); }
    else Serial.println("rollback failed (no other slot)");
  } else if (!strcasecmp(c, "help") || !strcasecmp(c, "?")) {
    Serial.println("serial commands: reset | reprovision | factory | status | version | heap | ip | wifi | mac | dmesg | nosleep | time | tz [n] | tls [host] | ntfytest | support | otacheck | otaurl [url] | otainfo | rollback | help");
  } else {
    Serial.printf("? '%s'  (try: help)\n", c);
  }
}
static void serialPoll() {
  static char buf[128]; static uint8_t n = 0;     // room for a full manifest URL arg, e.g. "otaurl https://firmware.flensor.com/hughes-badtest.json" (48 was too short -> truncated the URL)
  while (Serial.available()) {
    char ch = (char)Serial.read();
    if (ch == '\r' || ch == '\n') { if (n) { buf[n] = 0; handleSerialCmd(buf); n = 0; } }
    else if (n < sizeof(buf) - 1)  { buf[n++] = ch; }
  }
}

void setup() {
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);            // S3 USB-CDC: don't block prints when running headless (no monitor attached)
  delay(300);
  dmesgInit();                         // sanitize the RTC crash-log ring + stamp the boot banner (reset reason) BEFORE any logf()
  otaBootGuard();                      // crash-boot counter -> revert to the other A/B slot after 3 strikes (rollback is off in this core)
#ifdef OTA_SELFTEST_PANIC
  // Deliberate boot panic to VALIDATE the 3-strike A/B revert above. Placed AFTER otaBootGuard() so each
  // bad boot still runs the guard and increments the NVS strike counter; at strike 3 the guard reverts to
  // the good slot before we ever panic again. delay() lets the non-resetting serial monitor catch the
  // strike prints each cycle. Only compiled with -DOTA_SELFTEST_PANIC (never in a shipped build).
  { delay(1500); Serial.println("[selftest] OTA_SELFTEST_PANIC -> deliberate panic (boot-guard revert test)");
    Serial.flush(); volatile int* p = nullptr; *p = 0xDEAD; }
#endif
  Serial.println("\n=== hughes_bridge  BRIDGE MODE  v" FW_VERSION " ===");

  for (int i = 0; i < 2; i++) { g_leg[i].valid = false; g_leg[i].raw[0] = 0; }
  g_mtx = xSemaphoreCreateMutex();
  g_foundMtx = xSemaphoreCreateMutex();

  g_led.begin(); g_led.setBrightness(40); led(0, 0, 40);   // blue: booting

  tft.init();                                              // ILI9341V on HSPI
  tft.setRotation(0);                                      // portrait 240x320
  ledcAttach(45, 5000, 8); ledcWrite(45, 255);            // backlight PWM (GPIO45), full during boot
  bootBegin();
  bootStep("MOUNT DISPLAY ........."); bootResult(true, "OK", "");

  configLoad();
  applyTz();                                               // apply the saved US timezone so localtime_r() is correct once SNTP syncs
  applyBright();                                           // apply the saved brightness
  bootStep("LOAD CONFIG ..........."); bootResult(true, "OK", "");
  if (g_forceProv) {                                       // an explicit reprovision/factory-reset just ran: go straight to the
    g_prefs.begin("hughes", false); g_prefs.remove("forceprov"); g_prefs.end();   // portal and do NOT resurrect any driver-cached
    logf("boot: forceProv (post-wipe) -> setup portal, skipping driver-cred adopt");   // creds (one-shot; clear it now)
  } else if (!g_cfg.wifiSsid.length() && g_cfg.netMode == 0) {   // our config is empty but the driver may hold a stored network
    WiFi.mode(WIFI_STA);                                   // init the driver so esp_wifi_get_config can read its saved config
    if (wifiCaptureCreds()) { bootStep("RECOVER WIFI ........."); bootResult(true, "OK", ""); }   // adopt it -> provisioned, boot to bridge
  }
  if (!g_cfg.provisioned()) {
    logf("boot: not provisioned -> setup portal");
    startProvisioning();
    return;
  }
  logf("boot: bridge, netMode=%d, watchdog=%s", g_cfg.netMode,
       (g_cfg.hughesMac.length() || g_cfg.hughesName.length()) ? "set" : "none");
  netBringUp();     // HOME (join, fall back to our own AP) / AP (host) / OFF -- the product runs regardless
  startBridge();
  otaMarkValid();   // we booted far enough to run the product -> cancel any pending OTA rollback
}

void loop() {
  serialPoll();                        // check for a serial command (reset / status / help)
  server.handleClient();
  if (g_mode == MODE_PROV) {
    g_dns.processNextRequest();
    ts.read();                                       // on-screen SKIP / portal-QR
    static bool provWas = false; bool nowT = ts.isTouched;
    if (nowT && !provWas) {
      int tx = ts.points[0].x, ty = ts.points[0].y;
      if (g_provQr) {                                // showing the portal QR -> BACK returns to the join screen
        if (hit(R_BACK, tx, ty)) provisioningScreen();
      } else if (hit(R_PROV_QR, tx, ty)) {           // scan-to-open-setup (Android captive-portal workaround)
        g_provQr = true; webQrDraw(false);
      } else if (hit(R_PROV_SKIP, tx, ty)) {
        g_cfg.netMode = 1; g_cfg.wifiSsid = ""; g_cfg.wifiPass = ""; configPersist();
        Serial.println("prov: SKIP -> host-AP standalone (reboot)"); g_reboot = true;
      }
    }
    provWas = nowT;
    if (g_reboot) { delay(300); ESP.restart(); }
    delay(2); return;
  }

  otaBootHealthyTick();                                 // clear the crash-strike counter once we've run healthy a while
  if (g_otaBusy) { delay(20); return; }                 // OTA in progress -> the OTA task owns the screen; skip all render/UI

  if (g_cfg.netMode == 0 && !g_apServing && WiFi.status() != WL_CONNECTED) {   // HOME only: reconnect quietly
    static uint32_t lastWifiTry = 0;
    if (millis() - lastWifiTry > 5000) { lastWifiTry = millis(); WiFi.reconnect(); }
  } else if (g_cfg.netMode == 0 && !g_cfg.wifiSsid.length() && WiFi.status() == WL_CONNECTED) {
    wifiCaptureCreds();          // on Wi-Fi via the driver's cached creds but our config is empty -> adopt so provisioned() stays true
  }
  if (g_apServing) g_dns.processNextRequest();          // captive DNS while hosting our own AP
  handleTouch();
  uint32_t nowMs = millis();
  static uint32_t lastFrame = 0, lastNum = 0, lastSample = 0, lastSave = 0;
  if (nowMs - lastSample >= 1000)  { lastSample = nowMs; graphSample(); }           // history ring (all screens)
  { static uint32_t lastBatt = 0; if (nowMs - lastBatt >= 5000) { lastBatt = nowMs; battRead(); } }  // battery voltage (slow-moving)

  if (g_baseNeedsInit) {                             // newly-adopted unit: capture its STAY baseline from the first live reading
    Leg b1, b2; xSemaphoreTake(g_mtx, portMAX_DELAY); b1=g_leg[0]; b2=g_leg[1]; xSemaphoreGive(g_mtx);
    bool f1 = b1.valid && (nowMs-b1.seenMs)<5000, f2 = b2.valid && (nowMs-b2.seenMs)<5000;
    static uint32_t firstFresh = 0;
    if (f1 || f2) {
      g_cfg.kwhBase = (b1.valid?b1.kwh:0.0f) + (b2.valid?b2.kwh:0.0f);   // track combined -> STAY stays ~0 while settling
      if (!firstFresh) firstFresh = nowMs;
      if ((f1 && f2) || nowMs - firstFresh > 4000) {                     // both legs in, or a 30A unit's L1 alone after 4s
        baselinePersist(); g_baseNeedsInit = false; firstFresh = 0;
        Serial.printf("STAY baseline captured for new unit: %.3f kWh\n", g_cfg.kwhBase);
      }
    }
  }
  if (!g_reboot && nowMs - lastSave >= 30000) { lastSave = nowMs; graphSave(); usageSave(); evtSave(); }  // persist ring + usage ledger + event log to SD (skip once a reboot is pending, e.g. mid-adopt, so we don't write the old unit's data to the new MAC's file)

  if (g_cfg.alarmOn && g_audioOk) {                  // trip alarm: sound near a leg's 50A breaker (rate-limited)
    static uint32_t lastAlarm = 0;
    Leg a1, a2; xSemaphoreTake(g_mtx, portMAX_DELAY); a1 = g_leg[0]; a2 = g_leg[1]; xSemaphoreGive(g_mtx);
    bool hot = (a1.valid && (nowMs - a1.seenMs) < 5000 && a1.amps >= ALARM_FRAC*g_cfg.svcAmps) ||
               (a2.valid && (nowMs - a2.seenMs) < 5000 && a2.amps >= ALARM_FRAC*g_cfg.svcAmps);
    if (hot && nowMs - lastAlarm > 30000) { lastAlarm = nowMs; playKlaxon(); }
  }
  // outage detection: had a live link, not a deliberate release, no data for OUTAGE_MS.
  { bool active = g_hadLink && !g_released && (nowMs - g_lastNotifyMs) > OUTAGE_MS;
    static bool prevActive = false;
    if (active && !prevActive && !g_capHold) {          // onset -> notify once + wake the screen (skipped while a screenshot hold is pinned)
      g_outageStart = nowMs; g_outageShow = true;
      if (g_asleep) { g_asleep = false; applyBright(); }
      g_lastTouchMs = nowMs;                            // give a wake window (even on battery) before it can re-sleep
      g_scr = SCR_REACTOR; g_scrDirty = true;
    } else if (!active && prevActive && g_outageShow) { // cleared (data back / released) -> normal reactor
      g_outageShow = false; if (g_scr == SCR_REACTOR) g_scrDirty = true;
    }
    if (g_outageShow && nowMs - g_outageStart > OUTAGE_QUIET_MS) {   // auto-quiet: stop nagging
      g_outageShow = false; if (g_scr == SCR_REACTOR) g_scrDirty = true;
    }
    prevActive = active;
  }

  { static uint32_t lastNtfy = 0; if (nowMs - lastNtfy >= 4000) { lastNtfy = nowMs; ntfyPoll(); evtPoll(); alertTrustPoll(); stayTick(); } }  // ntfy alerts + event log + alert-channel trust + forgot-checkout

  // Context-aware sleep grace: NEVER on the DEVICES scan view (walk-up RSSI hunt -- you're watching, not
  // touching), 5 min on any other settings page (config/reading), the usual short timeout elsewhere.
  bool onSettings = (g_scr == SCR_SETTINGS);
  bool scanning   = onSettings && g_setPage == 3 && g_devScan;                            // DEVICES scan-results view
  uint32_t idleMs = scanning ? UINT32_MAX : (onSettings ? SETTINGS_SLEEP_MS : IDLE_MS);
  uint32_t nowdMs = scanning ? UINT32_MAX : (onSettings ? SETTINGS_SLEEP_MS : NOWD_SLEEP_MS);
  if (!g_asleep && !g_capHold && nowMs - g_lastTouchMs > idleMs && !(g_outageShow && !g_onBattery)) {   // idle -> sleep (stay lit during an on-USB outage; never while a screenshot hold is pinned)
    g_asleep = true; ledcWrite(45, 0);
    if (g_scr != SCR_REACTOR) { g_scr = SCR_REACTOR; g_scrDirty = true; }
  }
  // Auto power-off: a unit with NO Watchdog selected has nothing to monitor -> deep-sleep after
  // `nowdMs` of inactivity (touch OR web resets it) to spare the battery. Never fires once a
  // Watchdog is chosen, so the production collector is unaffected. Tap to wake (= fresh boot).
  if (!g_noSleep && !g_cfg.hughesMac.length() && !g_cfg.hughesName.length()) {            // 'nosleep' serial cmd keeps a bench unit awake
    uint32_t lastAct = g_lastWebMs > g_lastTouchMs ? g_lastWebMs : g_lastTouchMs;
    if (nowMs - lastAct > nowdMs) enterSleep();                                           // never returns (deep sleep)
  }
  switch (g_scr) {
    case SCR_REACTOR:
      if (g_outageShow) {                                                              // outage banner
        if (g_scrDirty) { outageStatic(); g_scrDirty = false; lastNum = nowMs; }
        if (nowMs - lastNum >= 1000) { lastNum = nowMs; outageUpdate(); }              // elapsed ticks
      } else if (!g_cfg.hughesMac.length() && !g_cfg.hughesName.length()) {            // no Watchdog -> pick-one prompt
        if (g_scrDirty) { reactorNoWd(); g_scrDirty = false; lastNum = nowMs; }
      } else {
        if (g_scrDirty) { reactorStatic(); reactorNumbers(); g_scrDirty = false; lastNum = nowMs; }
        if (nowMs - lastFrame >= 50)   { lastFrame = nowMs; reactorCores(); }          // ~20fps cores
        if (nowMs - lastNum   >= 1000) { lastNum   = nowMs; reactorNumbers(); }         // 1Hz text
      }
      break;
    case SCR_GRAPHS:
      if (g_scrDirty) { graphsDraw(); g_scrDirty = false; lastNum = nowMs; }
      if (nowMs - lastNum >= 1000) { lastNum = nowMs; graphPlot(); }                  // live trace refresh
      break;
    case SCR_SETTINGS:
      if (g_setPage == 1) {                                                            // INFO sub-page (re-tap)
        if (g_scrDirty) { infoDraw(); g_scrDirty = false; lastNum = nowMs; }
        if (nowMs - lastNum >= 1000) { lastNum = nowMs; infoUpdate(); }                // live diagnostics
      } else if (ALARM_PAGE && g_setPage == 2) {                                       // AUDIO sub-page (static)
        if (g_scrDirty) { audioDraw(); g_scrDirty = false; lastNum = nowMs; }
      } else if (g_setPage == 3) {                                                     // DEVICES sub-page
        if (g_scrDirty) { g_devScan ? devScanDraw() : devicesDraw(); g_scrDirty = false; lastNum = nowMs; }
        if (g_devScan) {
          if (g_scanDone) { g_scanDone = false; if (g_devConfirm < 0) devScanRows(); } // live cycle -> repaint just the rows
        } else {                                                                       // main view: keep LINK line live + time out the CHANGE gate
          if (g_changeArm && nowMs - g_changeArm > 2500) { g_changeArm = 0; btn(R_CHANGE, "CHANGE WATCHDOG", C_CYAN, C_CYAN, 2); }
          if (nowMs - lastNum >= 1000) { lastNum = nowMs; devLiveUpdate(); }
        }
      } else if (g_setPage == 4) {                                                     // SYSTEM sub-page (static)
        if (g_scrDirty) { systemDraw(); g_scrDirty = false; lastNum = nowMs; }
        if (g_factoryArm && nowMs - g_factoryArm > 2500) { g_factoryArm = 0; btn(R_SYS_FACTORY, "FACTORY RESET", C_LINE, C_RED, 4, 2); }  // arm times out
      } else if (g_setPage == 5) {                                                     // NETWORK sub-page (static)
        if (g_scrDirty) { g_netHomePrompt ? netHomePromptDraw() : networkDraw(); g_scrDirty = false; lastNum = nowMs; }
      } else if (g_setPage == 6) {                                                     // QR join view (static)
        if (g_scrDirty) { qrDraw(); g_scrDirty = false; lastNum = nowMs; }
      } else if (g_setPage == 7) {                                                     // ntfy subscribe QR (static)
        if (g_scrDirty) { ntfyQrDraw(); g_scrDirty = false; lastNum = nowMs; }
      } else if (g_setPage == 8) {                                                     // ALERTS sub-page (static)
        if (g_scrDirty) { alertsDraw(); g_scrDirty = false; lastNum = nowMs; }
        if (g_alrtDisArm && nowMs - g_alrtDisArm > 4000) { g_alrtDisArm = 0; btn(R_ALRT_EN, "2. TURN OFF ALERTS", C_LINE, C_AMBER, 2); }  // OFF gate times out (4s)
      } else if (g_setPage == 9) {                                                     // TIME page (live local clock)
        if (g_scrDirty) { timeDraw(); g_scrDirty = false; lastNum = nowMs; }
        if (nowMs - lastNum >= 1000) { lastNum = nowMs; tzClockUpdate(); }
      } else if (g_setPage == 11) {                                                    // manual set-time (static)
        if (g_scrDirty) { timeSetDraw(); g_scrDirty = false; lastNum = nowMs; }
      } else if (g_setPage == 10) {                                                    // WEB PORTAL QR (static)
        if (g_scrDirty) { webQrDraw(true); g_scrDirty = false; lastNum = nowMs; }
      } else if (g_setPage == 14) {                                                    // USAGE report (static)
        if (g_scrDirty) { usageDraw(); g_scrDirty = false; lastNum = nowMs; }
      } else if (g_setPage == 15) {                                                    // HELP page (manual QR, static)
        if (g_scrDirty) { helpDraw(); g_scrDirty = false; lastNum = nowMs; }
      } else if (g_setPage == 90) {                                                    // first-run setup preview (screenshot only, via `show prov`)
        if (g_scrDirty) { provisioningScreen(); g_scrDirty = false; lastNum = nowMs; }
      } else if (g_setPage == 91) {                                                    // first-run portal-URL QR preview (`show provqr`)
        if (g_scrDirty) { webQrDraw(false); g_scrDirty = false; lastNum = nowMs; }
      } else if (g_setPage == 12) {                                                    // FIRMWARE page
        if (g_scrDirty) { firmwareDraw(); g_scrDirty = false; lastNum = nowMs; }
        firmwareTick(nowMs);                                                           // rollback-arm timeout
      } else if (g_setPage == 13) {                                                    // ENERGY & RATE page
        static int g_rateKnownWas = -1;
        if (g_scrDirty) { settingsDraw(); g_scrDirty = false; lastNum = nowMs; g_rateKnownWas = (g_cfg.tierCount > 0) ? (localHourNow() >= 0) : -1; }
        if (!g_confirmOdo && nowMs - lastNum >= 1000) { lastNum = nowMs;         // live refresh (paused while the odo confirm modal is up)
          drawStayVal();
          int kn = (g_cfg.tierCount > 0) ? (localHourNow() >= 0) : -1;
          if (kn != g_rateKnownWas) { g_rateKnownWas = kn; if (!g_resetArm) drawRateSection(); }   // plan clock-known flipped -> redraw the whole RATE section
          else drawRateVal();                                                    // otherwise just refresh the live value
        }
        if (g_btnFlash && nowMs - g_btnFlash > 800)  { g_btnFlash = 0; btn(R_RESET, "RESET STAY", C_LINE, C_AMBER, 2); }
        if (g_resetArm && nowMs - g_resetArm > 2500) { g_resetArm = 0; btn(R_RESET, "RESET STAY", C_LINE, C_AMBER, 2); }  // arm times out
      } else {                                                                         // page 0 (or any stray): the SETTINGS menu
        if (g_scrDirty) { menuDraw(); g_scrDirty = false; lastNum = nowMs; }
      }
      break;
  }
  if (g_reboot) { delay(300); ESP.restart(); }
  delay(2);
}

#endif // SCAN_ONLY
