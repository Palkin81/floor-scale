#pragma once
// Поиск сетей и проверка пароля без отключения точки доступа.
// Черновик, не проверен на железе: вызовы ESP-IDF в обход менеджера wifi ESPHome.

#include <string>
#include <string.h>
#include <stdio.h>
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace probe {

static const char *TAG_PR = "probe";

// Список найденных сетей, по одной на строку: "имя (сигнал dBm)"
inline std::string scan_list() {
  wifi_scan_config_t cfg = {};
  if (esp_wifi_scan_start(&cfg, true) != ESP_OK) return "Ошибка сканирования";

  uint16_t n = 16;
  wifi_ap_record_t recs[16];
  if (esp_wifi_scan_get_ap_records(&n, recs) != ESP_OK) return "Ошибка чтения списка";

  std::string out;
  for (int i = 0; i < n; i++) {
    char name[33] = {0};
    memcpy(name, recs[i].ssid, 32);
    if (name[0] == 0) continue;
    char line[80];
    snprintf(line, sizeof(line), "%s (%d dBm)\n", name, (int) recs[i].rssi);
    out += line;
  }
  return out.empty() ? "Сети не найдены" : out;
}

// Пробует подключиться к сети и ждёт IP. true — подключение успешно.
// ВНИМАНИЕ: блокирует вызывающий поток на timeout_ms.
inline bool try_connect(const std::string &ssid, const std::string &pass, int timeout_ms,
                        bool *previous_network_restored = nullptr) {
  wifi_config_t old_cfg = {};
  const bool had_old_cfg = esp_wifi_get_config(WIFI_IF_STA, &old_cfg) == ESP_OK;
  // У ESP-IDF есть setter, но нет getter. ESPHome после esp_wifi_init сам
  // переключает драйвер в WIFI_STORAGE_RAM (wifi_component_esp_idf.cpp), поэтому
  // именно RAM и надо возвращать. Возврат FLASH заставлял драйвер записывать в
  // NVS каждую последующую сеть, и после сброса там оставалась устаревшая запись.
  const wifi_storage_t old_storage = WIFI_STORAGE_RAM;

  wifi_config_t cfg = {};
  memcpy(cfg.sta.ssid, ssid.c_str(), ssid.size() < 32 ? ssid.size() : 32);
  memcpy(cfg.sta.password, pass.c_str(), pass.size() < 64 ? pass.size() : 64);

  esp_wifi_disconnect();
  vTaskDelay(pdMS_TO_TICKS(250));
  // Не записываем проверяемую сеть во флеш. Восстановим исходную конфигурацию
  // после проверки, чтобы отправить данные соседу по сети настройки.
  if (esp_wifi_set_storage(WIFI_STORAGE_RAM) != ESP_OK) {
    if (had_old_cfg && old_cfg.sta.ssid[0] != 0) esp_wifi_connect();
    ESP_LOGE(TAG_PR, "cannot switch Wi-Fi storage to RAM");
    return false;
  }
  if (esp_wifi_set_config(WIFI_IF_STA, &cfg) != ESP_OK) {
    if (had_old_cfg && old_cfg.sta.ssid[0] != 0) esp_wifi_connect();
    esp_wifi_set_storage(old_storage);
    ESP_LOGE(TAG_PR, "set_config failed");
    return false;
  }
  esp_wifi_connect();

  esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  bool connected = false;
  for (int t = 0; t < timeout_ms; t += 250) {
    esp_netif_ip_info_t ip = {};
    if (netif != nullptr && esp_netif_get_ip_info(netif, &ip) == ESP_OK && ip.ip.addr != 0) {
      connected = true;
      break;
    }
    vTaskDelay(pdMS_TO_TICKS(250));
  }

  esp_wifi_disconnect();
  if (had_old_cfg) esp_wifi_set_config(WIFI_IF_STA, &old_cfg);
  esp_wifi_set_storage(old_storage);
  bool restored = !had_old_cfg || old_cfg.sta.ssid[0] == 0;
  if (had_old_cfg && old_cfg.sta.ssid[0] != 0) {
    esp_wifi_connect();
    // Дожидаемся не только ассоциации, но и DHCP IP до отправки соседу.
    for (int t = 0; t < 10000; t += 250) {
      wifi_ap_record_t ap = {};
      esp_netif_ip_info_t ip = {};
      const bool associated = esp_wifi_sta_get_ap_info(&ap) == ESP_OK &&
          memcmp(ap.ssid, old_cfg.sta.ssid, sizeof(old_cfg.sta.ssid)) == 0;
      const bool has_ip = netif != nullptr &&
          esp_netif_get_ip_info(netif, &ip) == ESP_OK && ip.ip.addr != 0;
      if (associated && has_ip) {
        restored = true;
        break;
      }
      vTaskDelay(pdMS_TO_TICKS(250));
    }
    if (!restored) ESP_LOGW(TAG_PR, "previous network not restored with IP; skipping peer send");
  }
  if (previous_network_restored != nullptr) *previous_network_restored = restored;
  return connected;
}

}  // namespace probe
