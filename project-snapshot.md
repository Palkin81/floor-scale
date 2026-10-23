# СОСТОЯНИЕ ПРОЕКТА

**Цель:** Два ESP-устройства с общей логикой настройки сети без Home Assistant на этом этапе. Весы (XIAO ESP32-C6, HX711, измерение и калибровка по кривой) и экран (generic ESP32, пока заглушка со страницей настройки, позже дисплей). Настроил одно, настроились оба: пользователь вводит домашнюю сеть на странице одного устройства, оно шифрует креды AES-256-GCM и передаёт соседу по HTTP. Оба переходят в домашнюю сеть. Шифрование ключом `espectre_encryption_key`, тем же, что для API и OTA.

**Текущий этап:** Этап 4–5. Настройка через страницу и поиск сетей реализованы на экране, на весах в коде. Отправка кредов соседу не реализована ни на одном устройстве. Работает автономно, без Home Assistant.

## Уже сделано

- **Весы:** чтение HX711 (D10/D9, 10 Гц, RATE на GND), медиана и EMA, машина состояний IDLE → WAIT_STABLE → RESULT, адаптивный допуск стабильности, таймер выключения дисплея 10 с, калибровка по кривой до 5 точек, ноль при загрузке с проверкой, кнопки «Обнулить», «Калибровать», «Сбросить точки», OTA с шифрованием.
- **Экран:** страница на порту 80, поиск сетей через событие ESP-IDF, проверка пароля, сброс сети, окно настройки 5 минут, приём `POST /prov`. По журналу разработки проверялись на железе поиск (fetched=10–15), неверный и верный пароли, сброс сети, приём блока с ноутбука и отказ при закрытом окне. `make_blob.py` формирует тестовый блок и отправляет его на `/prov`; отправка учётных данных соседнему устройству не реализована.
- **Весы:** та же страница и сетевая логика, плюс блоки веса и калибровки. Код написан и статически проверен, на железе не запускался.
- Все `id`, `name`, `friendly_name` латиницей без пробелов (проверено скриптом). Русский только в сообщениях и на странице.
- `lib/ui_server.h`: команды калибровки ставятся в очередь (`cmd_pending`) и выполняются в цикле датчика (`run_pending()`). Статус хранится в фиксированном буфере `status_buf`. Сделано после обнаружения риска падения из-за доступа к состоянию из двух задач.
- Архив `esphome-project.zip` в структуре `/config/esphome`.

## Ключевые решения и ограничения

- Общая сеть настройки `prov_ssid`, пароль `prov_password`. Ненастроенное устройство сначала проверяет сохранённую сеть, затем ищет `prov_ssid` без подключения. Если нашло, подключается клиентом, только когда его MAC меньше найденного. Иначе поднимает точку. Перед решением случайная задержка 0–10 с, в `script`, не в `on_boot`.
- Точки доступа: экран `192.168.4.11`, весы `192.168.4.10`, маска 255.255.255.0. Без `captive_portal`, без `ap_timeout`. ESPHome поднимает точку примерно за 80–90 с.
- Страница встроена в `lib/ui_server.h`. `web_server` ESPHome отключён, css и js не нужны. Порт управления httpd `32769`, стек httpd 12 КБ.
- Сканирование: компонент `wifi` ESPHome забирает результаты раньше нас, поэтому используется обработчик `WIFI_EVENT_SCAN_DONE`. Перед сканированием режим AP переключается в APSTA без подключения, иначе `ESP_FAIL`.
- Приём кредов только при открытом окне. Счётчика версий нет, защита от повтора обеспечивается окном.
- Блок кредов: `nonce(12) | tag(16) | ciphertext`, открытый текст `ssid\npassword`, hex в теле `POST /prov`, до 300 байт. SSID до 32 байт, пароль до 64.
- `include_builtin_idf_components` на верхнем уровне `esp32` ESPHome 2026.10.0 не принимает. Используется `framework: advanced: include_builtin_idf_components: [esp_http_server]`.
- Задержки в `on_boot` блокируют цикл дольше 5 с и срабатывают watchdog. Задержки только в `script`.
- Имя текущей сети берётся через `esp_wifi_sta_get_ap_info`. `wifi_ssid()` в 2026.10.0 не существует.
- Не подтверждено для ESPHome 2026.10.0: `save_wifi_sta`, `clear_sta`, `on_connect` в `wifi`, вызовы ESP-IDF для сканирования и проверки.
- **Отложенный риск:** `probe::try_connect` вызывает `esp_wifi_set_config`, который по умолчанию пишет во флеш. Проверочная сеть остаётся сохранённой и после перезагрузки подключается без окна. Предлагаемое исправление: `esp_wifi_set_storage(WIFI_STORAGE_RAM)` перед проверкой. Касается обоих устройств.
- Автонуль в покое на весах в текущей версии отсутствует. Решение не принято.
- Гонка за роль точки при одновременном старте не решена полностью.
- Один ключ открывает API, OTA и шифрование кредов.
- Клиентское устройство доступно только по IP в сети другого устройства.
- Проверка пароля через AP+STA: на экране страница не пропадала, но риск остаётся.
- Мобильная вёрстка страницы и список сетей на телефоне не проверены.

## Текущий результат

В текущем репозитории присутствуют `floor-scale.yaml`, `screen_stub.yaml`, `make_blob.py`, `plan.md` и все четыре заголовка в `lib/`. Локальные `secrets.yaml` и архивы сборки в репозиторий включать не следует.

### Блок wifi (весы; у экрана адрес `.11`)

```yaml
wifi:
  id: wifi_comp
  ap:
    ssid: ${prov_ssid}
    password: ${prov_password}
    manual_ip:
      static_ip: 192.168.4.10   # у экрана 192.168.4.11
      gateway: 192.168.4.10
      subnet: 255.255.255.0
  on_connect:
    - lambda: |-
        if (prov::is_home_network("${prov_ssid}")) {
          ui::close_window();
          ui::set_status("Подключено к домашней сети");
        }
  reboot_timeout: 15min
```

### Выбор роли (script)

```yaml
script:
  - id: pick_role
    then:
      - delay: !lambda 'return esp_random() % 10000;'
      - lambda: |-
          std::string saved;
          uint8_t found[6] = {0};
          if (netrole::saved_sta(saved)) {
            ESP_LOGI("role", "Saved network %s, keeping it", saved.c_str());
          } else if (netrole::find_bssid("${prov_ssid}", found) &&
                     netrole::should_join_as_client(found)) {
            ESP_LOGI("role", "Setup network found, joining as client");
            id(wifi_comp)->save_wifi_sta("${prov_ssid}", "${prov_password}");
            App.reboot();
          } else {
            ESP_LOGI("role", "No setup network to join, staying as access point");
          }
```

### Инициализация страницы (on_boot, весы)

```yaml
esphome:
  on_boot:
    priority: -100
    then:
      - lambda: |-
          ui::prov_key = "${prov_key_b64}";
          ui::device_kind = "scale";
          ui::device_title = "Весы";
          ui::on_reset = []() { id(wifi_comp)->clear_sta(); App.reboot(); };
          ui::on_creds = [](const std::string &ssid, const std::string &pass) {
            id(wifi_comp)->save_wifi_sta(ssid, pass); delay(1000); App.reboot(); };
          ui::wifi_comp_ptr = id(wifi_comp);
          ui::open_window(${window_ms});
          ui::set_status("Окно настройки открыто на 5 минут");
          ui::start(80);
      - script.execute: pick_role
```

Экран то же без `device_kind`, `extra_status` и колбэков калибровки.

### Очередь команд (lib/ui_server.h)

```cpp
inline volatile bool cmd_pending = false;
inline std::string cmd_name;
inline float cmd_known = 0.0f;

inline void run_pending() {
  if (!cmd_pending) return;
  cmd_pending = false;
  std::string r;
  if (cmd_name == "zero" && on_zero) r = on_zero();
  else if (cmd_name == "calib" && on_calib) r = on_calib(cmd_known);
  else if (cmd_name == "curve_reset" && on_curve_reset) r = on_curve_reset();
  if (!r.empty()) set_status(r);
}
```

Вызов в начале `on_value` датчика HX711 весов: `ui::run_pending();`.

### Статус в фиксированном буфере

```cpp
inline char status_buf[160] = "Ожидание";
inline void set_status(const std::string &s) { snprintf(status_buf, sizeof(status_buf), "%s", s.c_str()); }
```

### Приём кредов (POST /prov)

```cpp
static esp_err_t h_prov(httpd_req_t *req) {
  if (!window_open()) { set_status("Окно настройки закрыто. Откройте его кнопкой на странице"); /* ответ */ }
  std::string blob;  // recv_body(req, blob, 300)
  std::string ssid, pass;
  if (!prov::decrypt(prov_key.c_str(), blob, ssid, pass)) { set_status("Ошибка: неверный ключ или данные"); /* ответ */ }
  set_status("Креды приняты, перезагрузка");
  close_window();
  // ответ, затем on_creds(ssid, pass)
}
```

### Формат блока (lib/prov_crypto.h)

- hex, `nonce(12) | tag(16) | ciphertext`
- AES-256-GCM (mbedtls), ключ — base64 от 32 байт из `espectre_encryption_key`
- открытый текст: `ssid\npassword`

### Поиск сетей (/scan)

- Обработчик `WIFI_EVENT_SCAN_DONE` (`on_scan_done`) забирает записи сразу после события.
- Перед сканированием `esp_wifi_set_mode(WIFI_MODE_APSTA)`, если режим `WIFI_MODE_AP`.
- `esp_wifi_scan_start(&cfg, false)`, ожидание семафора до 10 с.

### Тест приёма с ноутбука

```
python3 make_blob.py "ssid" "password" "BASE64-КЛЮЧ" 192.168.4.11
```

Эндпоинт `POST http://<ip>/prov`, тело — hex-блок. Ожидаемо `HTTP 200`, повтор при закрытом окне отклоняется.

### Платы

- Весы: `board: seeed_xiao_esp32c6`, `variant: esp32c6`
- Экран: `board: esp32dev`
- Оба: `framework: type: esp-idf` с `advanced.include_builtin_idf_components: [esp_http_server]`

## Что нужно сделать дальше

1. Собрать и прошить весы из архива. Проверить страницу на телефоне: поиск, проверка пароля, калибровка (`zero`, `calib`, `curve_reset`), вес на странице.
2. Проверить, что `prov_crypto.h` и `wifi_probe.h` в `/config/esphome/lib/` подключаются и совпадают с ожидаемыми.
3. Решить по автонулю на весах: вернуть или оставить без него.
4. Реализовать отправку кредов соседу после проверки пароля на обоих устройствах: шифрование, `POST /prov` на `<neighbor>.local` или по IP клиента, подтверждение и перезагрузка.
5. Внедрить `esp_wifi_set_storage(WIFI_STORAGE_RAM)` перед проверкой пароля (отложенный риск).
6. Проверить сценарии «весы первые», «экран первым», смену роутера и перезагрузку под нагрузкой.
7. Затем передача веса на экран (этап 6) и дисплей (этап 10).

## Открытые вопросы

- Вернуть автонуль на весах или оставить без него?
- Экран с реальным дисплеем: какой именно?
- Таймаут связи весы → экран: 30 или 60 с?
- Нужен ли способ открыть страницу клиентского устройства без знания IP?

---
