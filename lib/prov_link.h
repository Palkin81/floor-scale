#pragma once
// Передача уже проверенных учётных данных соседу через его HTTP POST /prov.

#include <string>
#include <string.h>
#include <stdio.h>
#include "esp_http_client.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "mdns.h"

namespace prov_link {

static const char *TAG_LINK = "prov_link";

inline esp_err_t collect_response(esp_http_client_event_t *evt) {
  if (evt->event_id == HTTP_EVENT_ON_DATA && evt->user_data != nullptr && evt->data_len > 0) {
    auto *response = static_cast<std::string *>(evt->user_data);
    response->append(static_cast<const char *>(evt->data), evt->data_len);
  }
  return ESP_OK;
}

inline bool get_request(const std::string &url, std::string &response, int timeout_ms = 800) {
  esp_http_client_config_t cfg = {};
  cfg.url = url.c_str();
  cfg.method = HTTP_METHOD_GET;
  cfg.timeout_ms = timeout_ms;
  cfg.event_handler = collect_response;
  cfg.user_data = &response;
  esp_http_client_handle_t client = esp_http_client_init(&cfg);
  if (client == nullptr) return false;
  esp_err_t err = esp_http_client_perform(client);
  int status = esp_http_client_get_status_code(client);
  esp_http_client_cleanup(client);
  return err == ESP_OK && status == 200;
}

inline bool post_request(const std::string &url, const std::string &body, int timeout_ms = 4000) {
  std::string response;
  esp_http_client_config_t cfg = {};
  cfg.url = url.c_str();
  cfg.method = HTTP_METHOD_POST;
  cfg.timeout_ms = timeout_ms;
  cfg.event_handler = collect_response;
  cfg.user_data = &response;

  esp_http_client_handle_t client = esp_http_client_init(&cfg);
  if (client == nullptr) return false;
  esp_http_client_set_header(client, "Content-Type", "text/plain");
  esp_http_client_set_post_field(client, body.data(), body.size());
  esp_err_t err = esp_http_client_perform(client);
  int status = esp_http_client_get_status_code(client);
  esp_http_client_cleanup(client);

  const bool accepted = err == ESP_OK && status == 200 &&
                        response.find("\"ok\":true") != std::string::npos;
  ESP_LOGI(TAG_LINK, "POST %s: err=%s http=%d accepted=%s", url.c_str(),
           esp_err_to_name(err), status, accepted ? "yes" : "no");
  return accepted;
}

inline bool post_blob(const std::string &ip, const std::string &blob) {
  return post_request("http://" + ip + "/prov", blob);
}

inline bool reset_peer_at(const std::string &host) {
  if (!host.empty() && host[0] >= '0' && host[0] <= '9') {
    return post_request("http://" + host + "/peer_reset", "reset", 5000);
  }
  std::string name = host;
  const std::string suffix = ".local";
  if (name.size() > suffix.size() &&
      name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0) {
    name.resize(name.size() - suffix.size());
  }
  esp_ip4_addr_t addr = {};
  esp_err_t err = mdns_query_a(name.c_str(), 3000, &addr);
  if (err != ESP_OK) {
    ESP_LOGW(TAG_LINK, "mDNS lookup for %s failed: %s", name.c_str(), esp_err_to_name(err));
    return false;
  }
  char ip[16];
  snprintf(ip, sizeof(ip), IPSTR, IP2STR(&addr));
  ESP_LOGI(TAG_LINK, "mDNS %s resolved to %s", name.c_str(), ip);
  return post_request("http://" + std::string(ip) + "/peer_reset", "reset", 5000);
}

// В роли точки отправляем каждому DHCP-клиенту AP. Телефоны и другие клиенты
// просто не подтвердят /prov; сосед примет блок, только если его окно открыто.
inline bool send_to_ap_clients(const std::string &blob) {
  wifi_sta_list_t stations = {};
  if (esp_wifi_ap_get_sta_list(&stations) != ESP_OK || stations.num == 0) return false;
  esp_netif_t *ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
  if (ap == nullptr) return false;

  esp_netif_pair_mac_ip_t pairs[ESP_WIFI_MAX_CONN_NUM] = {};
  const int count = stations.num < ESP_WIFI_MAX_CONN_NUM ? stations.num : ESP_WIFI_MAX_CONN_NUM;
  for (int i = 0; i < count; i++) memcpy(pairs[i].mac, stations.sta[i].mac, 6);
  if (esp_netif_dhcps_get_clients_by_mac(ap, count, pairs) != ESP_OK) return false;

  for (int i = 0; i < count; i++) {
    if (pairs[i].ip.addr == 0) continue;
    char ip[16];
    snprintf(ip, sizeof(ip), IPSTR, IP2STR(&pairs[i].ip));
    if (post_blob(ip, blob)) return true;
  }
  return false;
}

// В роли клиента сосед является точкой с фиксированным адресом.
inline bool send_to_peer_ap(const std::string &peer_ip, const std::string &blob) {
  return post_blob(peer_ip, blob);
}

// Каскадный сброс в setup-сети: когда мы AP, сосед является STA-клиентом.
inline bool reset_ap_clients() {
  wifi_sta_list_t stations = {};
  if (esp_wifi_ap_get_sta_list(&stations) != ESP_OK || stations.num == 0) return false;
  esp_netif_t *ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
  if (ap == nullptr) return false;

  esp_netif_pair_mac_ip_t pairs[ESP_WIFI_MAX_CONN_NUM] = {};
  const int count = stations.num < ESP_WIFI_MAX_CONN_NUM ? stations.num : ESP_WIFI_MAX_CONN_NUM;
  for (int i = 0; i < count; i++) memcpy(pairs[i].mac, stations.sta[i].mac, 6);
  if (esp_netif_dhcps_get_clients_by_mac(ap, count, pairs) != ESP_OK) return false;

  for (int i = 0; i < count; i++) {
    if (pairs[i].ip.addr == 0) continue;
    char ip[16];
    snprintf(ip, sizeof(ip), IPSTR, IP2STR(&pairs[i].ip));
    if (reset_peer_at(ip)) return true;
  }
  return false;
}

}  // namespace prov_link
