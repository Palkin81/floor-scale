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
inline bool try_connect(const std::string &ssid, const std::string &pass, int timeout_ms) {
  wifi_config_t cfg = {};
  memcpy(cfg.sta.ssid, ssid.c_str(), ssid.size() < 32 ? ssid.size() : 32);
  memcpy(cfg.sta.password, pass.c_str(), pass.size() < 64 ? pass.size() : 64);

  esp_wifi_disconnect();
  if (esp_wifi_set_config(WIFI_IF_STA, &cfg) != ESP_OK) {
    ESP_LOGE(TAG_PR, "set_config failed");
    return false;
  }
  esp_wifi_connect();

  esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  for (int t = 0; t < timeout_ms; t += 250) {
    esp_netif_ip_info_t ip = {};
    if (netif != nullptr && esp_netif_get_ip_info(netif, &ip) == ESP_OK && ip.ip.addr != 0) {
      return true;
    }
    vTaskDelay(pdMS_TO_TICKS(250));
  }
  return false;
}

}  // namespace probe
