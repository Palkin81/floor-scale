#!/usr/bin/env python3
"""
Тест приёма кредов на экране без веб-интерфейса.

Использование:
  python3 make_blob.py <ssid> <password> <key_b64> <screen_ip>

Пример:
  python3 make_blob.py "MyHome" "MyPass123" "BASE64KEY==" 192.168.4.11

Экран должен быть в окне настройки (после старта без домашней сети
или после нажатия «Открыть окно настройки» на странице).

Нужны пакеты: cryptography, requests.
"""
import base64
import os
import sys

import requests
from cryptography.hazmat.primitives.ciphers.aead import AESGCM


def make_blob(ssid: str, password: str, key_b64: str) -> str:
    key = base64.b64decode(key_b64)
    if len(key) != 32:
        raise SystemExit(f"Ключ должен декодироваться в 32 байта, а получилось {len(key)}")
    nonce = os.urandom(12)
    plain = f"{ssid}\n{password}".encode("utf-8")
    ct_and_tag = AESGCM(key).encrypt(nonce, plain, None)
    cipher, tag = ct_and_tag[:-16], ct_and_tag[-16:]
    # Формат на экране: nonce | tag | ciphertext
    return (nonce + tag + cipher).hex()


def main():
    if len(sys.argv) != 5:
        print(__doc__)
        sys.exit(1)
    ssid, password, key_b64, screen_ip = sys.argv[1:]
    blob = make_blob(ssid, password, key_b64)
    print(f"Blob length: {len(blob)} hex chars")

    url = f"http://{screen_ip}/prov"
    r = requests.post(url, data=blob, timeout=10)
    print("POST", url, "->", "HTTP", r.status_code, r.text[:200])


if __name__ == "__main__":
    main()
