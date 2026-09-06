// dmesg.h -- persistent crash-log ring in RTC slow memory (hughes_bridge, ESP32-S3).
//
// The board's native USB-CDC re-enumerates on every reset, so a panic / watchdog /
// brownout reboot drops the serial log at exactly the moment you need it. This keeps a
// ring in RTC "slow" memory: RTC_NOINIT survives a *software* reset -- panic, abort,
// task/int watchdog, deep sleep -- and only loses data on a true power cycle. Each boot
// stamps a banner with the decoded esp_reset_reason(): after a crash you let it reboot,
// attach a monitor (or hit GET /dmesg), and the last line before the next BOOT banner is
// where it died.
//
// Companion to the existing RAM /log breadcrumb ring: /log = queryable over Wi-Fi ("what
// happened"), dmesg = survives the crash. To avoid two parallel log APIs, main.cpp's
// logf() feeds BOTH -- it calls dmesgAppend() from inside its g_logMux critical section
// (see the note on dmesgAppend). Do NOT call dmesgAppend() cross-core without that lock.
//
// Usage: dmesgInit() at the very top of setup() (right after Serial.begin, BEFORE any
// logf()/task start -- it sanitizes the ring on a cold boot); GET /dmesg + serial `dmesg`
// call dmesgToString()/dmesgDump().
#pragma once
#include <Arduino.h>
#include <esp_system.h>

#define DMESG_CAP 3072                 // ring bytes in RTC slow RAM (8 KB total on the S3) -- a few boots' worth

RTC_NOINIT_ATTR static char     s_dmBuf[DMESG_CAP];
RTC_NOINIT_ATTR static uint32_t s_dmHead;     // next write index, [0, DMESG_CAP)
RTC_NOINIT_ATTR static uint32_t s_dmLen;      // valid bytes, <= DMESG_CAP
RTC_NOINIT_ATTR static uint32_t s_dmMagic;    // validity sentinel (garbage on a cold boot)
RTC_NOINIT_ATTR static uint32_t s_dmBoot;     // boot counter (survives warm resets)

static const uint32_t DMESG_MAGIC = 0x484D5347;   // 'HMSG'
static esp_reset_reason_t s_dmReset = ESP_RST_UNKNOWN;

static inline void dmesgPutc(char c) {
  s_dmBuf[s_dmHead] = c;
  s_dmHead = (s_dmHead + 1) % DMESG_CAP;
  if (s_dmLen < DMESG_CAP) s_dmLen++;
}

// Append a line (a trailing newline is added; CRs stripped). NOT internally locked -- the
// caller must serialize calls. hughes calls this from logf() under g_logMux, and from
// dmesgInit() before the BLE task starts. Cross-core use without a lock corrupts the indices.
static void dmesgAppend(const char* s) {
  for (const char* p = s; *p; p++) if (*p != '\r' && *p != '\n') dmesgPutc(*p);
  dmesgPutc('\n');
}

static const char* dmesgResetStr(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON:   return "POWERON";
    case ESP_RST_EXT:       return "EXT";
    case ESP_RST_SW:        return "SW";
    case ESP_RST_PANIC:     return "PANIC";
    case ESP_RST_INT_WDT:   return "INT_WDT";
    case ESP_RST_TASK_WDT:  return "TASK_WDT";
    case ESP_RST_WDT:       return "WDT";
    case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
    case ESP_RST_BROWNOUT:  return "BROWNOUT";
    case ESP_RST_SDIO:      return "SDIO";
    case ESP_RST_USB:       return "USB";
    case ESP_RST_JTAG:      return "JTAG";
    default:                return "UNKNOWN";
  }
}

// Call once at the very top of setup(), BEFORE any dmesgAppend/logf and before tasks start.
// Sanitizes the ring on a cold boot (RTC RAM is garbage when power was lost), bumps the boot
// counter (survives warm resets), and stamps a banner with the decoded reset reason.
static void dmesgInit() {
  s_dmReset = esp_reset_reason();
  if (s_dmMagic != DMESG_MAGIC) {        // cold boot / first run -> the ring is garbage
    s_dmMagic = DMESG_MAGIC;
    s_dmHead = 0; s_dmLen = 0; s_dmBoot = 0;
  }
  if (s_dmHead >= DMESG_CAP) s_dmHead = 0;          // defensive against a corrupt index
  if (s_dmLen  >  DMESG_CAP) s_dmLen  = DMESG_CAP;
  s_dmBoot++;
  char banner[80];
  snprintf(banner, sizeof(banner), "==== BOOT #%lu  reset=%s ====",
           (unsigned long)s_dmBoot, dmesgResetStr(s_dmReset));
  dmesgAppend(banner);
  Serial.println(banner);
}

static uint32_t    dmesgBootCount() { return s_dmBoot; }
static const char* dmesgLastReset() { return dmesgResetStr(s_dmReset); }

// Copy the ring, oldest-first, into `out`. Lock-free (diagnostic; a rare torn byte during a
// concurrent write is acceptable). The ring wraps, so start = head - len.
static void dmesgToString(String& out) {
  uint32_t len = s_dmLen; if (len > DMESG_CAP) len = DMESG_CAP;
  uint32_t start = (s_dmHead + DMESG_CAP - len) % DMESG_CAP;
  out.reserve(len + 1);
  for (uint32_t i = 0; i < len; i++) out += s_dmBuf[(start + i) % DMESG_CAP];
}

static void dmesgDump(Print& p) { String s; dmesgToString(s); p.print(s); }
