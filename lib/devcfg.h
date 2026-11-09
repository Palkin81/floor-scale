#pragma once
// Persistent device settings (NVS namespace "devcfg") with a safety net:
//  * trial apply: a changed setting is applied at once but rolls back after kTrialS seconds unless confirmed
//    (and at once if the device reboots during the trial);
//  * 3 consecutive short power-on boots reset all settings and the saved Wi-Fi network (the user can always
//    get back to a known state without a computer).
// Fields: mode (0 = with remote screen, 1 = simple), hx (weight sensor on/off/default), manual neighbour record.

#include <string.h>
#include <string>
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs.h"

namespace devcfg {

inline constexpr uint32_t kTrialS = 180;      // time to confirm a changed setting
inline constexpr uint32_t kQuickBootS = 15;   // a boot shorter than this counts as "quick"
inline constexpr uint8_t kQuickBootsToReset = 3;

struct Values {
  uint8_t mode = 0;            // 0 = with remote screen, 1 = simple
  uint8_t hx = 2;              // weight sensor HX711: 0 = off (screen), 1 = on (scale), 2 = board default
  char peer_ip[16] = "";       // manual neighbour address (empty = automatic)
  char peer_mac[18] = "";      // informational
  char peer_role[8] = "";      // "", "scale", "screen"
};

inline Values cur, prev;
inline bool trial = false;
inline uint32_t trial_start_ms = 0;
inline bool ready = false;
inline nvs_handle_t h = 0;
inline bool quick_pending = false;  // quick-boot counter is non-zero and still has to be cleared

inline uint32_t ms() { return (uint32_t) (esp_timer_get_time() / 1000); }

// Simple mode (mode 1): the device works alone. No neighbour search, no role election, no polling.
inline bool simple() { return cur.mode == 1; }

// HX711 weight sensor: the board default comes from the YAML (set at boot), the user can override it.
// Role "scale" = sensor on, role "screen" = sensor off; it takes effect after a reboot.
inline uint8_t def_hx = 0;
inline bool hx_eff(const Values &v) { return v.hx == 2 ? def_hx != 0 : v.hx == 1; }
inline bool hx_on() { return hx_eff(cur); }

inline void save_all() {
  if (!ready) return;
  nvs_set_blob(h, "v", &cur, sizeof(cur));
  nvs_set_blob(h, "p", &prev, sizeof(prev));
  nvs_set_u8(h, "tr", trial ? 1 : 0);
  nvs_commit(h);
}

inline void load_blob(const char *key, Values &out) {
  Values tmp;
  size_t len = sizeof(tmp);
  if (nvs_get_blob(h, key, &tmp, &len) == ESP_OK && len == sizeof(tmp)) {
    tmp.peer_ip[sizeof(tmp.peer_ip) - 1] = 0;
    tmp.peer_mac[sizeof(tmp.peer_mac) - 1] = 0;
    tmp.peer_role[sizeof(tmp.peer_role) - 1] = 0;
    out = tmp;
  }
}

// Returns true if three quick power-on boots in a row were detected.
inline bool begin() {
  if (nvs_open("devcfg", NVS_READWRITE, &h) != ESP_OK) return false;
  ready = true;
  load_blob("v", cur);
  load_blob("p", prev);
  uint8_t tr = 0;
  nvs_get_u8(h, "tr", &tr);
  if (tr) {
    // The device rebooted while a changed setting was still unconfirmed: go back.
    cur = prev;
    trial = false;
    save_all();
  }
  const esp_reset_reason_t r = esp_reset_reason();
  const bool manual = r == ESP_RST_POWERON || r == ESP_RST_EXT || r == ESP_RST_USB;
  uint8_t q = 0;
  nvs_get_u8(h, "qb", &q);
  q = manual ? (uint8_t) (q + 1) : 0;
  bool reset_due = false;
  if (q >= kQuickBootsToReset) {
    reset_due = true;
    q = 0;
  }
  nvs_set_u8(h, "qb", q);
  nvs_commit(h);
  quick_pending = q != 0;
  return reset_due;
}

inline void reset_all() {
  if (!ready) return;
  cur = Values();
  prev = Values();
  trial = false;
  nvs_erase_all(h);
  nvs_commit(h);
}

inline uint32_t trial_left_s() {
  if (!trial) return 0;
  const uint32_t el = (ms() - trial_start_ms) / 1000;
  return el >= kTrialS ? 0 : kTrialS - el;
}

inline void confirm() {
  trial = false;
  save_all();
}

inline void revert() {
  if (!trial) return;
  cur = prev;
  trial = false;
  save_all();
}

// New values are applied immediately but stay "on trial"; the previous ones are kept for the rollback.
inline void apply_trial(const Values &nv) {
  if (!trial) prev = cur;  // an earlier unconfirmed change keeps its original rollback point
  cur = nv;
  trial = true;
  trial_start_ms = ms();
  save_all();
}

// Called every ~10 s. Returns true when a trial has just expired and was rolled back.
inline bool tick(uint32_t up_s) {
  if (ready && quick_pending && up_s >= kQuickBootS) {
    nvs_set_u8(h, "qb", 0);
    nvs_commit(h);
    quick_pending = false;
  }
  if (trial && trial_left_s() == 0) {
    revert();
    return true;
  }
  return false;
}

}  // namespace devcfg
