#pragma once
// Выбор роли при старте: клиент сохранённой сети или точка доступа.
// Черновик, не проверен на железе.

#include <string>
#include <string.h>
#include <stdio.h>
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs.h"
#include "esphome/core/preferences.h"
#include "esphome/components/wifi/wifi_component.h"

namespace netrole {

static const char *TAG_NR = "netrole";
inline bool logging_started = false;

// ESPHome's clear_sta() clears only the runtime list. With no static STA
// networks in YAML, runtime credentials use fallback preference key
// 88491487 in the NVS namespace "esphome". Erase only that key to preserve
// calibration and other ESPHome preferences.
inline bool clear_saved_sta(esphome::wifi::WiFiComponent *wifi) {
  if (wifi == nullptr || esphome::global_preferences == nullptr) return false;
  wifi->clear_sta();

  // Commit any already queued ESPHome preferences before deleting this key.
  const bool prefs_synced = esphome::global_preferences->sync();
  nvs_handle_t nvs = 0;
  esp_err_t nvs_err = nvs_open("esphome", NVS_READWRITE, &nvs);
  bool pref_erased = false;
  if (nvs_err == ESP_OK) {
    char key[12];
    snprintf(key, sizeof(key), "%lu", 88491487UL);
    nvs_err = nvs_erase_key(nvs, key);
    if (nvs_err == ESP_OK) {
      nvs_err = nvs_commit(nvs);
      pref_erased = nvs_err == ESP_OK;
    } else if (nvs_err == ESP_ERR_NVS_NOT_FOUND) {
      pref_erased = true;
      nvs_err = ESP_OK;
    }
    nvs_close(nvs);
  }

  esp_wifi_disconnect();
  wifi_config_t empty_cfg = {};
  const esp_err_t cfg_err = esp_wifi_set_config(WIFI_IF_STA, &empty_cfg);
  ESP_LOGI(TAG_NR, "clear saved STA: preference_erase=%s nvs=%s sync=%s driver=%s",
           pref_erased ? "ok" : "failed", esp_err_to_name(nvs_err),
           prefs_synced ? "ok" : "failed", esp_err_to_name(cfg_err));
  return pref_erased && prefs_synced && cfg_err == ESP_OK;
}

inline void mac_text(const uint8_t mac[6], char out[18]) {
  snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

inline void wifi_event_log(void *, esp_event_base_t base, int32_t id, void *data) {
  if (base == WIFI_EVENT) {
    if (id == WIFI_EVENT_STA_START) {
      ESP_LOGI(TAG_NR, "STA started");
    } else if (id == WIFI_EVENT_AP_START) {
      wifi_config_t cfg = {};
      uint8_t mac[6] = {0};
      char mac_s[18];
      char ssid[33] = {0};
      esp_wifi_get_config(WIFI_IF_AP, &cfg);
      esp_wifi_get_mac(WIFI_IF_AP, mac);
      memcpy(ssid, cfg.ap.ssid, 32);
      mac_text(mac, mac_s);
      ESP_LOGI(TAG_NR, "AP started: ssid=%s mac=%s channel=%u",
               ssid, mac_s, (unsigned) cfg.ap.channel);
    } else if (id == WIFI_EVENT_STA_CONNECTED && data != nullptr) {
      auto *ev = static_cast<wifi_event_sta_connected_t *>(data);
      char ssid[33] = {0};
      char mac[18];
      memcpy(ssid, ev->ssid, 32);
      mac_text(ev->bssid, mac);
      ESP_LOGI(TAG_NR, "STA connected: ssid=%s bssid=%s channel=%u",
               ssid, mac, (unsigned) ev->channel);
    } else if (id == WIFI_EVENT_STA_DISCONNECTED && data != nullptr) {
      auto *ev = static_cast<wifi_event_sta_disconnected_t *>(data);
      char ssid[33] = {0};
      char mac[18];
      memcpy(ssid, ev->ssid, 32);
      mac_text(ev->bssid, mac);
      ESP_LOGW(TAG_NR, "STA disconnected: ssid=%s bssid=%s reason=%u",
               ssid, mac, (unsigned) ev->reason);
    } else if (id == WIFI_EVENT_AP_STACONNECTED && data != nullptr) {
      auto *ev = static_cast<wifi_event_ap_staconnected_t *>(data);
      char mac[18];
      mac_text(ev->mac, mac);
      ESP_LOGI(TAG_NR, "AP client connected: mac=%s aid=%u", mac, (unsigned) ev->aid);
    } else if (id == WIFI_EVENT_AP_STADISCONNECTED && data != nullptr) {
      auto *ev = static_cast<wifi_event_ap_stadisconnected_t *>(data);
      char mac[18];
      mac_text(ev->mac, mac);
      ESP_LOGW(TAG_NR, "AP client disconnected: mac=%s aid=%u", mac, (unsigned) ev->aid);
    }
  } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP && data != nullptr) {
    auto *ev = static_cast<ip_event_got_ip_t *>(data);
    ESP_LOGI(TAG_NR, "STA got IP: " IPSTR " gateway=" IPSTR,
             IP2STR(&ev->ip_info.ip), IP2STR(&ev->ip_info.gw));
  }
}

inline void start_logging() {
  if (logging_started) return;
  esp_err_t wifi_err = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                   &wifi_event_log, nullptr);
  esp_err_t ip_err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                 &wifi_event_log, nullptr);
  logging_started = wifi_err == ESP_OK && ip_err == ESP_OK;
  ESP_LOGI(TAG_NR, "Wi-Fi event logging: wifi=%s ip=%s",
           esp_err_to_name(wifi_err), esp_err_to_name(ip_err));
}

// Есть ли сохранённая клиентская сеть (домашняя или сеть настройки)
inline bool saved_sta(std::string &ssid) {
  wifi_config_t cfg = {};
  esp_err_t err = esp_wifi_get_config(WIFI_IF_STA, &cfg);
  if (err != ESP_OK) {
    ESP_LOGW(TAG_NR, "cannot read saved STA config: %s", esp_err_to_name(err));
    return false;
  }
  char s[33] = {0};
  memcpy(s, cfg.sta.ssid, 32);
  ssid = s;
  return !ssid.empty();
}

// Ищет точку с нужным именем. Возвращает её BSSID.
inline bool find_bssid(const std::string &target, uint8_t out[6]) {
  // В режиме только AP сканирование возвращает ESP_FAIL. Временно включаем
  // STA-интерфейс, сохраняя свою точку доступа.
  wifi_mode_t mode = WIFI_MODE_NULL;
  esp_err_t mode_err = esp_wifi_get_mode(&mode);
  if (mode_err != ESP_OK) {
    ESP_LOGW(TAG_NR, "cannot read Wi-Fi mode: %s", esp_err_to_name(mode_err));
    return false;
  }
  ESP_LOGI(TAG_NR, "role scan: mode=%d target=%s", (int) mode, target.c_str());
  if (mode == WIFI_MODE_AP) {
    esp_err_t e = esp_wifi_set_mode(WIFI_MODE_APSTA);
    ESP_LOGI(TAG_NR, "mode AP -> APSTA for role scan: %s", esp_err_to_name(e));
    if (e != ESP_OK) return false;
  }

  uint8_t own_ap[6] = {0};
  esp_wifi_get_mac(WIFI_IF_AP, own_ap);
  char own_s[18];
  mac_text(own_ap, own_s);
  ESP_LOGI(TAG_NR, "own AP BSSID=%s", own_s);
  wifi_scan_config_t cfg = {};
  esp_err_t scan_err = esp_wifi_scan_start(&cfg, true);
  if (scan_err != ESP_OK) {
    ESP_LOGW(TAG_NR, "role scan failed: %s", esp_err_to_name(scan_err));
    return false;
  }
  uint16_t n = 20;
  wifi_ap_record_t recs[20];
  if (esp_wifi_scan_get_ap_records(&n, recs) != ESP_OK) {
    ESP_LOGW(TAG_NR, "cannot read role scan results");
    return false;
  }
  ESP_LOGI(TAG_NR, "role scan found %u access points", (unsigned) n);
  for (int i = 0; i < n; i++) {
    char s[33] = {0};
    memcpy(s, recs[i].ssid, 32);
    if (s[0] != 0) {
      ESP_LOGI(TAG_NR, "scan AP: ssid=%s rssi=%d channel=%u",
               s, (int) recs[i].rssi, (unsigned) recs[i].primary);
    }
    if (target == s) {
      // Оба устройства используют общий prov_ssid. Не выбираем собственный
      // beacon как точку соседа.
      char bssid_s[18];
      mac_text(recs[i].bssid, bssid_s);
      if (memcmp(recs[i].bssid, own_ap, sizeof(own_ap)) == 0) {
        ESP_LOGI(TAG_NR, "ignoring own setup AP: bssid=%s", bssid_s);
        continue;
      }
      memcpy(out, recs[i].bssid, 6);
      ESP_LOGI(TAG_NR, "found peer setup AP: ssid=%s bssid=%s rssi=%d channel=%u",
               s, bssid_s, (int) recs[i].rssi, (unsigned) recs[i].primary);
      return true;
    }
  }
  ESP_LOGI(TAG_NR, "peer setup AP not found");
  return false;
}

// Роль по MAC: большая MAC становится клиентом, меньшая остаётся точкой.
// Так два устройства, увидевшие друг друга одновременно, не окажутся оба клиентами.
inline bool should_join_as_client(const uint8_t found[6]) {
  uint8_t own[6] = {0};
  esp_wifi_get_mac(WIFI_IF_AP, own);
  char own_s[18], peer_s[18];
  mac_text(own, own_s);
  mac_text(found, peer_s);
  const bool join = memcmp(own, found, 6) > 0;
  ESP_LOGI(TAG_NR, "role MAC compare: own=%s peer=%s -> %s",
           own_s, peer_s, join ? "STA client" : "AP");
  return join;
}

}  // namespace netrole
