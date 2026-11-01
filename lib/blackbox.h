#pragma once
// "Chorny yashchik": compact ASCII-only event log kept in RAM and mirrored to NVS.
// Survives reboots, dumped as a text file by GET /bb (and the neighbour's by /bb_peer).
//
// Layout: ring of kSlots chunks (kChunk bytes each) in NVS namespace "bb", keys "c0".."c23".
// The newest chunk lives in RAM (cur) and is written to its slot by flush().
// Every boot starts a fresh chunk, so partial chunks of earlier boots stay intact.
// Line format: "t<uptime_s> <TAG> <text>\n". Boot marker: "#BOOT n=<boots> rst=<reason> ...".

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace bb {

inline constexpr int kSlots = 24;
inline constexpr size_t kChunk = 2048;
inline constexpr uint32_t kLazyFlushMs = 120000;  // periodic flush of a dirty chunk
inline constexpr uint32_t kKeyFlushMs = 5000;     // minimum gap between flushes forced by key()

inline char cur[kChunk];
inline size_t cur_len = 0;
inline uint32_t seq = 0;      // number of the chunk being filled
inline uint32_t boots = 0;
inline uint32_t last_flush_ms = 0;
inline uint32_t dropped = 0;  // lines lost because the lock was busy
inline bool dirty = false;
inline bool ready = false;
inline nvs_handle_t h = 0;
inline SemaphoreHandle_t lock = nullptr;
inline std::string kind = "dev";
inline std::string build = "";

inline uint32_t ms() { return (uint32_t) (esp_timer_get_time() / 1000); }

inline const char *reason_name(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON: return "power";
    case ESP_RST_EXT: return "ext";
    case ESP_RST_SW: return "sw";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT: return "int_wdt";
    case ESP_RST_TASK_WDT: return "task_wdt";
    case ESP_RST_WDT: return "wdt";
    case ESP_RST_DEEPSLEEP: return "deepsleep";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_USB: return "usb";
    default: return "other";
  }
}

// Caller holds the lock.
inline void flush_locked() {
  if (!ready || !dirty) return;
  char key[8];
  snprintf(key, sizeof(key), "c%u", (unsigned) (seq % kSlots));
  esp_err_t e = nvs_set_blob(h, key, cur, cur_len);
  if (e == ESP_OK) e = nvs_set_u32(h, "q", seq);
  if (e == ESP_OK) e = nvs_commit(h);
  if (e == ESP_OK) dirty = false;
  last_flush_ms = ms();
}

inline void flush() {
  if (lock == nullptr) return;
  if (xSemaphoreTake(lock, pdMS_TO_TICKS(500)) != pdTRUE) return;
  flush_locked();
  xSemaphoreGive(lock);
}

inline void shutdown_flush() {
  // Runs inside esp_restart(): no waiting, take the lock only if free.
  if (lock != nullptr && xSemaphoreTake(lock, 0) == pdTRUE) {
    flush_locked();
    xSemaphoreGive(lock);
  }
}

inline void append_locked(const char *s, size_t n) {
  if (cur_len + n > kChunk) {
    flush_locked();
    seq++;
    cur_len = 0;
  }
  if (n > kChunk) n = kChunk;
  memcpy(cur + cur_len, s, n);
  cur_len += n;
  dirty = true;
}

inline void vline(bool force_flush, const char *tag, const char *fmt, va_list ap) {
  if (lock == nullptr) return;
  char body[176];
  vsnprintf(body, sizeof(body), fmt, ap);
  // ASCII only: anything else becomes '?'
  for (char *p = body; *p; p++) {
    if ((unsigned char) *p < 0x20 || (unsigned char) *p > 0x7e) *p = '?';
  }
  char line[220];
  const int n = snprintf(line, sizeof(line), "t%lu %s %s\n",
                         (unsigned long) (esp_timer_get_time() / 1000000), tag, body);
  if (n <= 0) return;
  const size_t len = (size_t) n < sizeof(line) ? (size_t) n : sizeof(line) - 1;
  if (xSemaphoreTake(lock, pdMS_TO_TICKS(200)) != pdTRUE) {
    dropped++;
    return;
  }
  append_locked(line, len);
  if (force_flush && ms() - last_flush_ms >= kKeyFlushMs) flush_locked();
  xSemaphoreGive(lock);
}

// Ordinary line: flushed lazily (every ~2 min or when the chunk fills up).
inline void line(const char *tag, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
inline void line(const char *tag, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vline(false, tag, fmt, ap);
  va_end(ap);
}

// Important line: also flushed to NVS right away (rate limited).
inline void key(const char *tag, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
inline void key(const char *tag, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vline(true, tag, fmt, ap);
  va_end(ap);
}

// Periodic call (every ~30 s): flush a dirty chunk if it has waited long enough.
inline void tick() {
  if (lock == nullptr || !ready) return;
  if (dirty && ms() - last_flush_ms >= kLazyFlushMs) flush();
}

inline void begin() {
  if (lock != nullptr) return;
  lock = xSemaphoreCreateMutex();
  if (lock == nullptr) return;
  if (nvs_open("bb", NVS_READWRITE, &h) != ESP_OK) {
    ready = false;
  } else {
    uint32_t q = 0, n = 0;
    const bool had = nvs_get_u32(h, "q", &q) == ESP_OK;
    nvs_get_u32(h, "n", &n);
    boots = n + 1;
    seq = had ? q + 1 : 0;
    nvs_set_u32(h, "n", boots);
    nvs_set_u32(h, "q", seq);
    nvs_commit(h);
    ready = true;
    esp_register_shutdown_handler(shutdown_flush);
  }
  char l[100];
  snprintf(l, sizeof(l), "#BOOT n=%lu rst=%s heap=%lu nvs=%s",
           (unsigned long) boots, reason_name(esp_reset_reason()),
           (unsigned long) esp_get_free_heap_size(), ready ? "ok" : "FAIL");
  line("BOOT", "%s", l + 6);
}

inline void clear() {
  if (lock == nullptr || !ready) return;
  if (xSemaphoreTake(lock, pdMS_TO_TICKS(500)) != pdTRUE) return;
  char k[8];
  for (int i = 0; i < kSlots; i++) {
    snprintf(k, sizeof(k), "c%d", i);
    nvs_erase_key(h, k);
  }
  seq = 0;
  nvs_set_u32(h, "q", 0);
  nvs_commit(h);
  cur_len = 0;
  dirty = false;
  xSemaphoreGive(lock);
}

// Streams the whole log (oldest chunk first) as text through chunked HTTP.
inline esp_err_t dump(httpd_req_t *req, const char *mac_text) {
  httpd_resp_set_type(req, "text/plain; charset=us-ascii");
  char disp[96];
  snprintf(disp, sizeof(disp), "attachment; filename=\"bb-%s-%s.txt\"", kind.c_str(), mac_text + 12);
  httpd_resp_set_hdr(req, "Content-Disposition", disp);

  char head[220];
  snprintf(head, sizeof(head), "#FLOORSCALE-BB v1 kind=%s mac=%s build=%s boots=%lu seq=%lu dropped=%lu heap=%lu\n",
           kind.c_str(), mac_text, build.c_str(), (unsigned long) boots, (unsigned long) seq,
           (unsigned long) dropped, (unsigned long) esp_get_free_heap_size());
  httpd_resp_send_chunk(req, head, HTTPD_RESP_USE_STRLEN);

  char *buf = (char *) malloc(kChunk);
  if (buf == nullptr) {
    httpd_resp_send_chunk(req, "#ERR no memory\n", HTTPD_RESP_USE_STRLEN);
    return httpd_resp_send_chunk(req, nullptr, 0);
  }
  flush();
  const uint32_t first = seq >= (uint32_t) (kSlots - 1) ? seq - (kSlots - 1) : 0;
  for (uint32_t s = first; s < seq; s++) {
    char k[8];
    snprintf(k, sizeof(k), "c%u", (unsigned) (s % kSlots));
    size_t len = kChunk;
    if (ready && nvs_get_blob(h, k, buf, &len) == ESP_OK && len > 0) {
      httpd_resp_send_chunk(req, buf, len);
    }
  }
  size_t n = 0;
  if (lock != nullptr && xSemaphoreTake(lock, pdMS_TO_TICKS(500)) == pdTRUE) {
    n = cur_len;
    memcpy(buf, cur, n);
    xSemaphoreGive(lock);
  }
  if (n > 0) httpd_resp_send_chunk(req, buf, n);
  free(buf);
  return httpd_resp_send_chunk(req, nullptr, 0);
}

}  // namespace bb
