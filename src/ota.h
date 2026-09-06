// ota.h -- firmware OTA engine + boot-guard for hughes_bridge.
//
// Writes a new image into the idle A/B slot (partitions_ota.csv), verifies size + sha256, sets it as the
// next boot partition; the caller reboots. Because arduino-esp32 ships bootloader app-rollback OFF (a bad
// OTA that boots-then-crashes would loop forever), otaBootGuard() counts crash-boots in NVS and reverts to
// the other slot after 3 strikes. Self-contained: logs to Serial + sets g_otaErr; main.cpp adds /log
// breadcrumbs at the call sites. TLS is CA-pinned (ISRG Root X1 via certs.h) and needs a synced clock
// (SNTP) + the ~40 KB handshake heap that PSRAM frees -- run the check/download on a >=16 KB task.
#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Update.h>
#include <Preferences.h>
#include <esp_ota_ops.h>
#include "mbedtls/sha256.h"
#include "certs.h"

static char g_otaErr[80] = "";                       // last failure reason (shown by the UI)

// ---- Boot guard: 3 consecutive crash-boots (PANIC/WDT) -> fall back to the other slot. A clean reset,
// or OTA_BOOT_HEALTHY_MS of uptime, clears the counter, so normal dev flashing never trips it. ----
#define OTA_BOOT_HEALTHY_MS 25000
static bool s_otaHealthyArmed = false;
static void otaBootGuard() {
  esp_reset_reason_t r = esp_reset_reason();
  bool crash = (r == ESP_RST_PANIC || r == ESP_RST_INT_WDT || r == ESP_RST_TASK_WDT || r == ESP_RST_WDT);
  Preferences p; p.begin("ota", false);
  uint8_t strikes = p.getUChar("strikes", 0);
  if (crash) {
    strikes++;
    Serial.printf("[ota] crash boot (reason %d) -> strike %u/3\n", (int)r, strikes);
    if (strikes >= 3) {
      const esp_partition_t* other = esp_ota_get_next_update_partition(nullptr);
      if (other && esp_ota_set_boot_partition(other) == ESP_OK) {
        Serial.printf("[ota] 3 strikes -> reverting to '%s' + reboot\n", other->label);
        p.putUChar("strikes", 0); p.end(); delay(200); ESP.restart();
      }
      strikes = 0;                                    // couldn't revert -> reset so the guard doesn't loop
    }
    p.putUChar("strikes", strikes);
  } else if (strikes) {
    p.putUChar("strikes", 0);                         // clean boot -> clear the counter
  }
  p.end();
  s_otaHealthyArmed = true;
}
// Call from loop(): once we've run healthy for OTA_BOOT_HEALTHY_MS, clear any lingering strike.
static void otaBootHealthyTick() {
  if (!s_otaHealthyArmed || millis() < OTA_BOOT_HEALTHY_MS) return;
  s_otaHealthyArmed = false;
  Preferences p; p.begin("ota", false); if (p.getUChar("strikes", 0)) p.putUChar("strikes", 0); p.end();
}

// Cancel a pending rollback if the bootloader has it armed (harmless no-op on this core -- see the header).
static void otaMarkValid() {
  const esp_partition_t* run = esp_ota_get_running_partition();
  esp_ota_img_states_t st;
  if (esp_ota_get_state_partition(run, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY)
    esp_ota_mark_app_valid_cancel_rollback();
}

// Manually flip the boot partition to the other slot (serial/web `rollback`). Caller reboots.
static bool otaRollback() {
  const esp_partition_t* other = esp_ota_get_next_update_partition(nullptr);
  return other && esp_ota_set_boot_partition(other) == ESP_OK;
}

static const char* otaRunningLabel() { const esp_partition_t* r = esp_ota_get_running_partition(); return r ? r->label : "?"; }
static const char* otaNextLabel()    { const esp_partition_t* n = esp_ota_get_next_update_partition(nullptr); return n ? n->label : "?"; }

static bool otaFail(const char* m){ snprintf(g_otaErr, sizeof(g_otaErr), "%s", m); return false; }
static bool otaFailAbort(const char* m){ Update.abort(); return otaFail(m); }

// Stream `total` bytes from `in` into the idle slot; if sha256hex is set the whole image must match.
// progress(pct) fires ~each chunk. Returns true with the new slot armed as boot (caller reboots).
static bool otaApply(Stream& in, int total, const char* sha256hex, void(*progress)(int)) {
  g_otaErr[0] = 0;
  if (total <= 0) return otaFail("no image size");
  if (!Update.begin((size_t)total)) return otaFail(Update.errorString());   // picks + erases the idle slot
  mbedtls_sha256_context sha; mbedtls_sha256_init(&sha); mbedtls_sha256_starts(&sha, 0);
  uint8_t buf[1460]; int done = 0, lastPct = -1; uint32_t last = millis();
  while (done < total) {
    int want = (total - done) < (int)sizeof(buf) ? (total - done) : (int)sizeof(buf);
    int n = in.readBytes(buf, want);
    if (n <= 0) { if (millis() - last > 15000) { mbedtls_sha256_free(&sha); return otaFailAbort("download stalled"); } continue; }
    if (Update.write(buf, n) != (size_t)n) { mbedtls_sha256_free(&sha); return otaFailAbort(Update.errorString()); }
    mbedtls_sha256_update(&sha, buf, n); done += n; last = millis();
    int pct = (int)((int64_t)done * 100 / total); if (progress && pct != lastPct) { lastPct = pct; progress(pct); }
  }
  uint8_t dig[32]; mbedtls_sha256_finish(&sha, dig); mbedtls_sha256_free(&sha);
  if (sha256hex && *sha256hex) {                       // integrity gate
    char hex[65]; for (int i = 0; i < 32; i++) snprintf(hex + i*2, 3, "%02x", dig[i]);
    if (strcasecmp(hex, sha256hex) != 0) return otaFailAbort("sha256 mismatch");
  }
  if (!Update.end(true)) return otaFail(Update.errorString());   // finalize + set the boot partition
  return true;
}

// HTTPS GET <url> -> apply exactly `size` bytes with the sha check. Hardened for Dreamhost/Apache: CA-pinned,
// follows redirects (http->https, trailing-slash), requires Content-Length == size (rejects chunked/gzip),
// asks for identity encoding. Runs on the caller's task -- use a >=16 KB one (mbedtls).
static bool otaFromUrl(const char* url, long size, const char* sha256hex, void(*progress)(int)) {
  g_otaErr[0] = 0;
  if (WiFi.status() != WL_CONNECTED) return otaFail("no wifi");
  WiFiClientSecure client; client.setCACert(ISRG_ROOT_X1); client.setHandshakeTimeout(12);
  HTTPClient http; http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS); http.setTimeout(15000);
  if (!http.begin(client, url)) return otaFail("http begin failed");
  http.addHeader("Accept-Encoding", "identity");
  int code = http.GET();
  if (code != 200) { char m[40]; snprintf(m, sizeof(m), "http %d", code); http.end(); return otaFail(m); }
  int len = http.getSize();
  if (len < 0) { http.end(); return otaFail("no content-length (chunked?)"); }
  if (size > 0 && len != size) { char m[52]; snprintf(m, sizeof(m), "len %d != manifest %ld", len, size); http.end(); return otaFail(m); }
  bool ok = otaApply(*http.getStreamPtr(), len, sha256hex, progress);
  http.end();
  return ok;
}
