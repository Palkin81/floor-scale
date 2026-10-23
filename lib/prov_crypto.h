#pragma once
// Расшифровка кредов по AES-256-GCM и проверка сети. Черновик, не проверен на железе.
//
// Формат блока (hex):  nonce(12) | tag(16) | ciphertext
// Открытый текст:      "<ssid>\n<password>"
// Ключ: base64 от 32 байт, тот же, что espectre_encryption_key.

#include <string>
#include <vector>
#include <string.h>
#include <stdlib.h>
#include "mbedtls/gcm.h"
#include "mbedtls/base64.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_random.h"

namespace prov {

static const char *TAG_PROV = "prov";

// true, если устройство подключено к сети, отличной от сети настройки.
// Если не подключено вообще, возвращает false.
inline bool is_home_network(const char *setup_ssid) {
  wifi_ap_record_t ap;
  if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) return false;
  return strncmp((const char *) ap.ssid, setup_ssid, 32) != 0;
}

inline bool hex_to_bytes(const std::string &h, std::vector<uint8_t> &out) {
  if (h.size() % 2 != 0 || h.empty()) return false;
  out.clear();
  out.reserve(h.size() / 2);
  for (size_t i = 0; i < h.size(); i += 2) {
    char buf[3] = {h[i], h[i + 1], 0};
    char *end = nullptr;
    long v = strtol(buf, &end, 16);
    if (end == nullptr || *end != 0) return false;
    out.push_back((uint8_t) v);
  }
  return true;
}

// true, если блок расшифрован и тег совпал.
inline bool decrypt(const char *key_b64, const std::string &blob_hex,
                    std::string &ssid, std::string &pass) {
  std::vector<uint8_t> blob;
  if (!hex_to_bytes(blob_hex, blob)) return false;
  if (blob.size() < 12 + 16 + 1) return false;

  uint8_t key[32];
  size_t klen = 0;
  if (mbedtls_base64_decode(key, sizeof(key), &klen,
                            (const unsigned char *) key_b64, strlen(key_b64)) != 0 ||
      klen != 32) {
    ESP_LOGE(TAG_PROV, "Bad prov key (need base64 of 32 bytes)");
    return false;
  }

  const uint8_t *iv = blob.data();
  const uint8_t *tag = blob.data() + 12;
  const uint8_t *ct = blob.data() + 28;
  const size_t clen = blob.size() - 28;

  std::vector<uint8_t> plain(clen + 1, 0);
  mbedtls_gcm_context ctx;
  mbedtls_gcm_init(&ctx);
  int rc = mbedtls_gcm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, key, 256);
  if (rc == 0) {
    rc = mbedtls_gcm_auth_decrypt(&ctx, clen, iv, 12, nullptr, 0, tag, 16, ct, plain.data());
  }
  mbedtls_gcm_free(&ctx);
  memset(key, 0, sizeof(key));
  if (rc != 0) {
    ESP_LOGW(TAG_PROV, "GCM auth failed (wrong key or tampered data)");
    return false;
  }

  std::string s((const char *) plain.data(), clen);
  size_t p1 = s.find('\n');
  if (p1 == std::string::npos) return false;

  ssid = s.substr(0, p1);
  pass = s.substr(p1 + 1);

  if (ssid.empty() || ssid.size() > 32 || pass.size() > 64) return false;
  return true;
}

// Шифрует сетевые данные для передачи соседу в том же формате, который
// принимает POST /prov: hex(nonce | tag | ciphertext).
inline bool encrypt(const char *key_b64, const std::string &ssid,
                    const std::string &pass, std::string &blob_hex) {
  if (ssid.empty() || ssid.size() > 32 || pass.size() > 64) return false;

  uint8_t key[32];
  size_t klen = 0;
  if (mbedtls_base64_decode(key, sizeof(key), &klen,
                            (const unsigned char *) key_b64, strlen(key_b64)) != 0 ||
      klen != 32) {
    ESP_LOGE(TAG_PROV, "Bad prov key (need base64 of 32 bytes)");
    return false;
  }

  const std::string plain = ssid + "\n" + pass;
  uint8_t nonce[12];
  uint8_t tag[16];
  std::vector<uint8_t> cipher(plain.size());
  esp_fill_random(nonce, sizeof(nonce));

  mbedtls_gcm_context ctx;
  mbedtls_gcm_init(&ctx);
  int rc = mbedtls_gcm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, key, 256);
  if (rc == 0) {
    rc = mbedtls_gcm_crypt_and_tag(&ctx, MBEDTLS_GCM_ENCRYPT, plain.size(),
                                   nonce, sizeof(nonce), nullptr, 0,
                                   (const uint8_t *) plain.data(), cipher.data(),
                                   sizeof(tag), tag);
  }
  mbedtls_gcm_free(&ctx);
  memset(key, 0, sizeof(key));
  if (rc != 0) return false;

  static const char HEX[] = "0123456789abcdef";
  blob_hex.clear();
  blob_hex.reserve((sizeof(nonce) + sizeof(tag) + cipher.size()) * 2);
  auto append_hex = [&blob_hex](const uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; i++) {
      blob_hex += HEX[data[i] >> 4];
      blob_hex += HEX[data[i] & 0x0f];
    }
  };
  append_hex(nonce, sizeof(nonce));
  append_hex(tag, sizeof(tag));
  append_hex(cipher.data(), cipher.size());
  return true;
}

}  // namespace prov
