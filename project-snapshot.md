# Снимок состояния проекта (карусель Claude / Grok / ChatGPT)

Этот файл — канал передачи проекта между ИИ. Актуальный снимок всегда здесь, в ветке `main`. Claude обновляет его по команде «сделай снимок» и в конце значимой сессии. Правила шаблона: ничего важного не сокращать, при расхождении с кодом ориентироваться на текущие файлы репозитория и `plan.md`.

=== СОСТОЯНИЕ ПРОЕКТА ===
Цель: Два ESP-устройства с общей логикой настройки сети без Home Assistant на этом этапе. Весы (XIAO ESP32-C6, HX711, измерение и калибровка по кривой) и экран (generic ESP32 DevKit, пока заглушка со страницей настройки, позже дисплей). Настроил одно, настроились оба: пользователь вводит домашнюю сеть на странице одного устройства, оно шифрует креды AES-256-GCM (ключ `espectre_encryption_key`, тот же, что для API и OTA) и передаёт соседу по HTTP `POST /prov`. Оба переходят в домашнюю сеть. Репозиторий: https://github.com/Palkin81/floor-scale.

Текущий этап: Этапы 4–5. Передача кредов по воздуху подтверждена на железе в обе стороны, каскадный сброс подтверждён. Найдена причина «после сброса оба в своей сети по очереди, связи нет», исправление влито в `main` (PR #1, коммит `688b94b`), но ещё не собрано и не проверено на железе. Открыто: зависание проверки пароля на странице (синхронный `/connect`).

Уже сделано:
- Всё из `plan.md` (весы HX711, калибровка до 5 точек, страница `lib/ui_server.h`, поиск сетей через `WIFI_EVENT_SCAN_DONE`, проверка пароля `probe::try_connect`, AES-256-GCM `lib/prov_crypto.h`, выбор роли по MAC `lib/net_role.h`, повторный поиск роли раз в 10 с, статусы роли и связи, фоновый `peer_monitor`, очистка NVS-ключа `88491487`, каскадный сброс `/peer_reset`).
- HTTP 404 на `/prov` исправлен (коммит `94c9a3b`): `HTTPD_DEFAULT_CONFIG()` давал `max_uri_handlers = 8` при 12 маршрутах (`/`, `/status`, `/peer_identity`, `/scan`, `/connect`, `/open`, `/reset`, `/peer_reset`, `/prov`, `/zero`, `/calib`, `/curve_reset`); девятый и дальше молча отбрасывались. Теперь `cfg.max_uri_handlers = 16` и `reg_uri()` пишет `ESP_LOGE` при ошибке регистрации. Подтверждено логами 20:42–20:50: нет строк `register ... failed`, `POST http://192.168.4.11/prov` → `http=200 accepted=yes` (весы → экран), оба перезагрузились в `Xiaomi_1F6A` (весы `192.168.31.112`, экран `192.168.31.179`).
- Каскадный сброс подтверждён логами 21:00:53–55 (запущен на экране): mDNS `floor-scale` → `192.168.31.112`, `POST /peer_reset` → `http=200 accepted=yes`; у обоих `clear saved STA: preference_erase=ok nvs=ESP_OK sync=ok driver=ESP_OK`, `cleared=yes`, перезагрузка. Ключ NVS `88491487` подтверждён по исходникам ESPHome 2026.10.0 (`hash = has_sta() ? config_version_hash : 88491487UL` в `wifi_component.cpp`). После сброса на экране нет пустого SSID, точка поднимается за секунды вместо 90 с.
- Разбор логов после сброса (экран AP MAC `3C:71:BF:AA:D3:B9`, C6 AP MAC `CC:8D:A2:F3:E2:B1`): C6 в 21:01:04 пишет `role:115 STA config exists: ssid=scale_setup_net; keeping saved connection`, но ни одной строки `Connecting to` (у ESPHome сети нет). Экран находит точку C6, `role MAC compare ... -> AP`, остаётся точкой. Итог: две точки `scale_setup_net`, клиента нет. Клиенты `A0:D7:68:10:55:45` и `FE:59:61:F5:DD:54` садились то на экран, то на весы.
- Причина (подтверждена исходниками ESPHome, на железе не проверена): ESPHome после `esp_wifi_init` ставит `esp_wifi_set_storage(WIFI_STORAGE_RAM)` (`wifi_component_esp_idf.cpp`, строки 210 и 234). `probe::try_connect` возвращал `WIFI_STORAGE_FLASH`, поэтому последующие `esp_wifi_set_config` (в том числе реконнект к `scale_setup_net` в 20:49:00–04) писались в NVS драйвера. Сброс чистил только RAM; после перезагрузки драйвер подгружал старую сеть, а `netrole::saved_sta()` читал `esp_wifi_get_config(STA)` и сообщал о сохранённой сети, которой у ESPHome нет, поэтому `pick_role` пропускал выбор роли.
- Исправление влито в `main` (PR #1, merge `688b94b`, коммит `636fe29`): `lib/net_role.h` (`saved_sta()` через `esphome::wifi::global_wifi_component->has_sta()` и `get_sta().get_ssid().str()`; `clear_saved_sta()` дополнительно стирает флеш-копию через `FLASH` и возвращает `RAM`), `lib/wifi_probe.h` (`old_storage = WIFI_STORAGE_RAM`), `lib/ui_server.h` (`#include "net_role.h"`, `was_setup_client` и `setup_client` через `netrole::saved_sta()`), `screen_stub.yaml` (`api: reboot_timeout: 0s`). Содержимое в `main` проверено.
- Экран перезагружался каждые 15 минут: `[api:195] No clients; rebooting` (21:15:56, 21:30:57). У весов `api: reboot_timeout: 0s` уже было, у экрана добавлено.
- Причина зависания проверки пароля (по коду, не исправлено): `h_connect` синхронно держит HTTP-ответ до ~30 с (проба 15 с, восстановление сети до 10 с, отправка соседу до 4 с); страница весов приходит по setup-сети, а проба её на это время отключает, ответ не доходит.
- Прочее из логов: `esp_wifi_scan_get_ap_record failed: ESP_FAIL` безвреден; `esp-tls select() timeout` в `peer_monitor` — перебор клиентов точки, шум.

Ключевые решения / ограничения:
- Общая сеть настройки `prov_ssid` = `scale_setup_net`, пароль `prov_password`; экран AP `192.168.4.11`, весы AP `192.168.4.10`, подсеть `192.168.4.0/24`; страница встроена в `lib/ui_server.h`, порт управления httpd `32769`, стек httpd 12 КБ; приём кредов только при открытом окне 5 минут; блок `nonce(12)|tag(16)|ciphertext`, hex в теле `POST /prov`, до 300 байт, SSID до 32, пароль до 64; задержки только в `script`, не в `on_boot`; команды калибровки через `cmd_pending`/`run_pending()`; все `id`, `name`, `friendly_name` латиницей без пробелов.
- Роль по MAC: большая MAC (AP) становится клиентом. AP MAC: экран `3C:71:BF:AA:D3:B9`, C6 `CC:8D:A2:F3:E2:B1` (STA MAC C6 `CC:8D:A2:F3:E2:B0`). Значит, после исправления клиентом должен стать C6.
- Источник истины «есть ли сохранённая сеть» — список сетей ESPHome (`has_sta()`), а не конфиг драйвера ESP-IDF. ESPHome держит драйвер в `WIFI_STORAGE_RAM`; никогда не возвращать `FLASH`.
- `include_builtin_idf_components` на верхнем уровне `esp32` ESPHome 2026.10.0 не принимает; используется `framework: advanced: include_builtin_idf_components: [esp_http_server, esp_http_client]`. `wifi_ssid()` не существует: имя сети через `esp_wifi_sta_get_ap_info`.
- Сканирование: компонент `wifi` ESPHome забирает результаты раньше, поэтому используется обработчик `WIFI_EVENT_SCAN_DONE`; перед сканированием режим AP переключается в APSTA.
- Номера строк в логах (`ui:830`, `netrole:118` и др.) соответствуют файлам репозитория для сверки версии прошивки. После слияния PR #1 номера в `ui_server.h` и `net_role.h` сдвинулись. Метка времени компиляции в логе ненадёжна (не обновляется).
- В окружении Claude нет ESPHome: сборка и проверка на железе выполняются пользователем. Исходники ESPHome 2026.10.0 можно клонировать только для чтения (`git clone --depth 1 --branch 2026.10.0 https://github.com/esphome/esphome`).
- Порядок работы с репозиторием: изменения кода Claude кладёт в отдельную ветку и сообщает пользователю (пользователь делает merge PR); этот файл (`project-snapshot.md`) пользователь разрешил обновлять и пушить прямо в `main`. Запись в репозиторий из сессии Claude работает через клон `/home/claude/floor-scale`.
- Не решено: две точки с одним SSID при одновременном старте (после исправления выбор роли восстановится, но риск остаётся); ожидание около 90 с до точки, если сохранённая сеть недоступна; синхронный `/connect`; автонуль на весах отсутствует; один ключ открывает API, OTA и шифрование кредов; мобильная вёрстка страницы не проверена.

Текущий результат:
Ключевые фрагменты текущего кода в `main` (полный код смотреть в `lib/`).

`lib/net_role.h`:
```cpp
inline bool saved_sta(std::string &ssid) {
  ssid.clear();
  esphome::wifi::WiFiComponent *wifi = esphome::wifi::global_wifi_component;
  if (wifi == nullptr || !wifi->has_sta()) return false;
  ssid = wifi->get_sta().get_ssid().str();
  return !ssid.empty();
}
// в clear_saved_sta():
esp_wifi_disconnect();
wifi_config_t empty_cfg = {};
esp_err_t cfg_err = esp_wifi_set_storage(WIFI_STORAGE_FLASH);
if (cfg_err == ESP_OK) cfg_err = esp_wifi_set_config(WIFI_IF_STA, &empty_cfg);
esp_wifi_set_storage(WIFI_STORAGE_RAM);  // ESPHome работает с RAM
```

`lib/ui_server.h` (регистрация маршрутов):
```cpp
inline void reg_uri(httpd_handle_t srv, const httpd_uri_t &u) {
  const esp_err_t err = httpd_register_uri_handler(srv, &u);
  if (err != ESP_OK) ESP_LOGE(TAG_UI, "register %s failed: %s", u.uri, esp_err_to_name(err));
}
// в start(): cfg.max_uri_handlers = 16; каждый маршрут: reg_uri(srv, u);
```

`lib/wifi_probe.h`: `const wifi_storage_t old_storage = WIFI_STORAGE_RAM;` `screen_stub.yaml`: `api:` → `reboot_timeout: 0s`.

Ожидаемое поведение после прошивки и нового «Сбросить сеть»: у C6 в логе `role: Peer setup AP selected; saving setup network and rebooting as client`, затем `STA got IP: 192.168.4.110`; экран остаётся точкой; по перезагрузкам экрана каждые 15 мин сообщений `No clients; rebooting` быть не должно.

Что нужно сделать дальше:
1. Пересобрать и прошить оба устройства из `main` (`688b94b`), выполнить «Сбросить сеть», проверить в логах выбор роли C6 клиентом и `192.168.4.110`; прислать оба лога.
2. Если пара собралась: повторить передачу кредов в обе стороны, проверить отсутствие пустых SSID и перезагрузок экрана через 15 мин.
3. Сделать `/connect` асинхронным: POST сразу отвечает «запущено», проверка и отправка идут в отдельной задаче, страница опрашивает `/status` до итогового сообщения. Параллельно проверить гипотезу о конфликте `probe::try_connect` с реконнектом ESPHome (после `esp_wifi_disconnect()` ESPHome пишет `Connection lost; reconnecting`).
4. Решить по задержке около 90 с до точки (fallback AP, `ap_timeout`) и по гонке двух точек с одним SSID.
5. Проверить отказ при закрытом окне и неверном пароле, калибровку, сценарии «весы первыми» и смена роутера, мобильную вёрстку.
6. Решить по автонулю, затем передача веса на экран и реальный дисплей.

Открытые вопросы: автонуль (вернуть или нет); модель дисплея; таймаут связи весы → экран 30 или 60 с; способ открыть страницу клиентского устройства без знания IP; делать ли `/connect` асинхронным прямо сейчас.

Последний ИИ: Claude
Дата: 2026-10-23
