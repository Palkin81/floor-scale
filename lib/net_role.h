#pragma once
// Выбор роли при старте: клиент сохранённой сети или точка доступа.
// Черновик, не проверен на железе.

#include <string>
#include <string.h>
#include "esp_wifi.h"
#include "esp_log.h"

namespace netrole {

static const char *TAG_NR = "netrole";

// Есть ли сохранённая клиентская сеть (домашняя или сеть настройки)
inline bool saved_sta(std::string &ssid) {
  wifi_config_t cfg = {};
  if (esp_wifi_get_config(WIFI_IF_STA, &cfg) != ESP_OK) return false;
  char s[33] = {0};
  memcpy(s, cfg.sta.ssid, 32);
  ssid = s;
  return !ssid.empty();
}

// Ищет точку с нужным именем. Возвращает её BSSID.
inline bool find_bssid(const std::string &target, uint8_t out[6]) {
  wifi_scan_config_t cfg = {};
  if (esp_wifi_scan_start(&cfg, true) != ESP_OK) return false;
  uint16_t n = 20;
  wifi_ap_record_t recs[20];
  if (esp_wifi_scan_get_ap_records(&n, recs) != ESP_OK) return false;
  for (int i = 0; i < n; i++) {
    char s[33] = {0};
    memcpy(s, recs[i].ssid, 32);
    if (target == s) {
      memcpy(out, recs[i].bssid, 6);
      return true;
    }
  }
  return false;
}

// Роль по MAC: большая MAC становится клиентом, меньшая остаётся точкой.
// Так два устройства, увидевшие друг друга одновременно, не окажутся оба клиентами.
inline bool should_join_as_client(const uint8_t found[6]) {
  uint8_t own[6] = {0};
  esp_wifi_get_mac(WIFI_IF_AP, own);
  return memcmp(own, found, 6) > 0;
}

}  // namespace netrole
