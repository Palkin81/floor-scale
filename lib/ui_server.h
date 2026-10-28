#pragma once
// Веб-страница экрана на порту 80: статус, поиск сетей, проверка пароля,
// окно настройки, приём зашифрованных кредов, сброс сети.
// Заменяет встроенный web_server ESPHome. Черновик, не проверен на железе.

#include <string>
#include <vector>
#include <functional>
#include <cmath>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <time.h>
#include "esp_http_server.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esphome/components/wifi/wifi_component.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "wifi_probe.h"
#include "prov_crypto.h"
#include "prov_link.h"
#include "net_role.h"

#include "ui_icons.h"

namespace ui {

static const char *TAG_UI = "ui";

// ---- состояние ----
inline uint32_t window_until = 0;           // до какого момента (мс) открыто окно
// Фиксированный буфер: статус пишется из цикла ESPHome и читается из задачи httpd
inline char status_buf[160] = "Ожидание";
inline std::string prov_key;                // base64 ключ, задаётся из YAML
inline std::string setup_ssid;
inline std::string peer_ap_ip;
inline std::string peer_host;
inline std::function<void(const std::string &, const std::string &)> on_creds;
inline std::function<void()> on_reset;
// Перезагрузка по кнопке на странице. Задаётся из YAML: App.reboot()
inline std::function<void()> on_reboot;
// Время: нет часов с батареей и сети, поэтому страница передаёт его (Unix-секунды, UTC)
inline volatile long long clock_set_at = 0;  // 0 = время не задано
// Смещение часового пояса страницы в минутах (как getTimezoneOffset: UTC минус местное)
inline volatile int clock_tz_min = 0;

// Метка времени для журнала: внутренние часы, если их задала страница, иначе время с запуска
inline std::string clock_stamp() {
  char b[32];
  if (clock_set_at) {
    const time_t local = (time_t) time(nullptr) - (time_t) clock_tz_min * 60;
    struct tm tm = {};
    gmtime_r(&local, &tm);
    snprintf(b, sizeof(b), "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
  } else {
    snprintf(b, sizeof(b), "с запуска %lu с", (unsigned long) (esp_timer_get_time() / 1000000));
  }
  return b;
}
inline esphome::wifi::WiFiComponent *wifi_comp_ptr = nullptr;  // задаётся из YAML: id(wifi_comp)
inline SemaphoreHandle_t peer_status_lock = nullptr;
inline bool peer_check_done = false;
inline bool peer_check_applicable = false;
inline bool peer_link_state = false;
inline char peer_role_buf[64] = "";
struct PeerCandidateCache {
  uint8_t mac[6] = {0};
  uint32_t checked_at = 0;
  bool valid = false;
};
inline PeerCandidateCache peer_candidate_cache[ESP_WIFI_MAX_CONN_NUM] = {};
// Адрес соседа, найденный проверкой связи (пишет только peer_monitor_task, читает под peer_status_lock)
inline char peer_ip_buf[16] = "";
inline std::string home_peer_ip;           // последний рабочий адрес соседа в домашней сети
inline std::string peer_data;              // последний ответ /status соседа (только экран)
inline uint32_t peer_data_at = 0;          // когда получен, мс
inline int peer_misses = 0;                // подряд пропущенных проверок связи

// ---- устройство ----
inline std::string device_kind = "screen";        // "screen" или "scale"
inline std::string device_title = "Экран";        // заголовок страницы
// Фрагмент JSON без ведущей запятой, например "\"live\":1.2". Задаётся из YAML.
inline std::function<std::string()> extra_status;
// Действия калибровки, задаются из YAML, возвращают текст для пользователя
inline std::function<std::string()> on_zero;
inline std::function<std::string(float)> on_calib;
inline std::function<std::string()> on_curve_reset;
// Калибровка для экспорта/импорта: текст "ноль,кг1,отсч1,…,кг5,отсч5". Задаются только на весах.
inline std::function<std::string()> export_cal;
inline std::function<std::string(const std::string &)> import_cal;

inline uint32_t now_ms() { return (uint32_t) (esp_timer_get_time() / 1000); }

inline void open_window(uint32_t ms) { window_until = now_ms() + ms; }
inline void close_window() { window_until = 0; }
inline bool window_open() {
  return window_until != 0 && (int32_t) (now_ms() - window_until) < 0;
}
// ---- Журнал событий для раздела «События» на странице ----
// Кольцо из 24 записей; страница забирает новые по GET /events?after=<id>.
// Сюда попадают смена статуса, результаты тестов, ошибки и смена связи с соседом.
struct UiEvent {
  uint32_t id;
  long long ts;     // Unix-время, если часы заданы страницей, иначе 0
  uint32_t up_s;    // секунд с запуска
  char lvl;         // 'i' информация, 'w' предупреждение, 'e' ошибка
  char text[140];
};
inline constexpr int kEvCount = 24;
inline UiEvent ev_ring[kEvCount] = {};
inline uint32_t ev_last_id = 0;
inline SemaphoreHandle_t ev_lock = nullptr;

inline void add_event(char lvl, const char *text) {
  if (ev_lock == nullptr) ev_lock = xSemaphoreCreateMutex();
  if (ev_lock == nullptr || xSemaphoreTake(ev_lock, pdMS_TO_TICKS(50)) != pdTRUE) return;
  const UiEvent &prev = ev_ring[(ev_last_id + kEvCount - 1) % kEvCount];
  if (!(ev_last_id > 0 && prev.lvl == lvl && strncmp(prev.text, text, sizeof(prev.text) - 1) == 0)) {
    UiEvent &e = ev_ring[ev_last_id % kEvCount];
    e.id = ++ev_last_id;
    e.ts = clock_set_at ? (long long) time(nullptr) : 0;
    e.up_s = (uint32_t) (esp_timer_get_time() / 1000000);
    e.lvl = lvl;
    snprintf(e.text, sizeof(e.text), "%s", text);
  }
  xSemaphoreGive(ev_lock);
}

// Пишет и в журнал устройства, и в «События». Формат как у printf; префикс "[время] " в событие не попадает.
inline void evlog(char lvl, const char *fmt, ...) {
  char buf[200];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  if (lvl == 'e') ESP_LOGE(TAG_UI, "%s", buf);
  else if (lvl == 'w') ESP_LOGW(TAG_UI, "%s", buf);
  else ESP_LOGI(TAG_UI, "%s", buf);
  const char *t = buf;
  if (buf[0] == '[') {
    const char *c = strstr(buf, "] ");
    if (c != nullptr) t = c + 2;
  }
  add_event(lvl, t);
}

inline void set_status(const std::string &s) {
  if (s != status_buf && s != "Ожидание") add_event('i', s.c_str());
  snprintf(status_buf, sizeof(status_buf), "%s", s.c_str());
}

// Команды калибровки: httpd только ставит в очередь, выполняет цикл ESPHome (run_pending)
inline volatile bool cmd_pending = false;
inline std::string cmd_name;
inline float cmd_known = 0.0f;
inline std::string cmd_text;

inline void run_test(const std::string &name);  // определена ниже, рядом с тестами страницы

inline void run_pending() {
  if (!cmd_pending) return;
  cmd_pending = false;
  if (cmd_name.rfind("test_", 0) == 0) {
    run_test(cmd_name);
    return;
  }
  std::string r;
  if (cmd_name == "zero" && on_zero) r = on_zero();
  else if (cmd_name == "calib" && on_calib) r = on_calib(cmd_known);
  else if (cmd_name == "curve_reset" && on_curve_reset) r = on_curve_reset();
  else if (cmd_name == "import_cal" && import_cal) r = import_cal(cmd_text);
  if (!r.empty()) set_status(r);
}

static const char PAGE[] = R"HTML(
<!doctype html>
<html lang="ru"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<meta name="color-scheme" content="light dark">
<link rel="icon" href="/icon-192.png" type="image/png">
<link rel="apple-touch-icon" href="/icon-180.png">
<link rel="manifest" href="/manifest.webmanifest">
<meta name="apple-mobile-web-app-capable" content="yes">
<meta name="mobile-web-app-capable" content="yes">
<meta name="apple-mobile-web-app-title" content="{{TITLE}}">
<meta name="apple-mobile-web-app-status-bar-style" content="default">
<meta name="theme-color" content="#f2f4f8" media="(prefers-color-scheme: light)">
<meta name="theme-color" content="#0b0d12" media="(prefers-color-scheme: dark)">
<title>{{TITLE}}</title>
<style>
:root{
--text:#16181d;--text2:rgba(22,24,29,.66);--text3:rgba(22,24,29,.4);
--glass:rgba(255,255,255,.46);--glass2:rgba(255,255,255,.8);--field:rgba(255,255,255,.6);
--stroke:rgba(255,255,255,.8);--stroke-in:rgba(255,255,255,.4);--sheen:.7;
--tabs:rgba(255,255,255,.66);
--sep:rgba(60,60,67,.13);--shadow:0 10px 32px rgba(40,60,130,.14);
--blue:#0a7aff;--green:#1fa84a;--red:#e5382d;--orange:#e08400;
--base:#eaf0fb;--b1:#b9d4ff;--b2:#e4c8ff;--b3:#ffd8bb;
}
@media (prefers-color-scheme:dark){:root{
--text:#f4f6fa;--text2:rgba(244,246,250,.68);--text3:rgba(244,246,250,.42);
--glass:rgba(36,40,58,.5);--glass2:rgba(30,34,50,.86);--field:rgba(255,255,255,.09);
--stroke:rgba(255,255,255,.17);--stroke-in:rgba(255,255,255,.06);--sheen:.22;
--tabs:rgba(26,30,46,.7);
--sep:rgba(255,255,255,.1);--shadow:0 12px 36px rgba(0,0,0,.42);
--blue:#4da3ff;--green:#3ddc6a;--red:#ff6a60;--orange:#ffb340;
--base:#090c17;--b1:#1c3d8c;--b2:#552d8e;--b3:#0e5a64;
}}
*{box-sizing:border-box;-webkit-tap-highlight-color:transparent}
html{-webkit-text-size-adjust:100%}
@media (hover:hover) and (pointer:fine){html{overflow-y:scroll}}
::-webkit-scrollbar{width:0;height:0;background:transparent}
html{scrollbar-width:none}
body{margin:0;min-height:100vh;color:var(--text);-webkit-font-smoothing:antialiased;
font:16px/1.4 -apple-system,BlinkMacSystemFont,"SF Pro Text","SF Pro Display","Helvetica Neue",system-ui,"Segoe UI",Roboto,sans-serif;
background:var(--base)}
body::before{content:"";position:fixed;inset:0;z-index:-1;pointer-events:none;background:radial-gradient(70vmax 70vmax at 0% 0%,var(--b1),transparent 62%),radial-gradient(60vmax 60vmax at 100% 28%,var(--b2),transparent 62%),radial-gradient(70vmax 70vmax at 20% 100%,var(--b3),transparent 62%),var(--base);
}
main{max-width:640px;margin:0 auto;padding:calc(20px + env(safe-area-inset-top)) 16px calc(120px + env(safe-area-inset-bottom))}
.glass{position:relative;background:var(--glass);border:1px solid var(--stroke);border-radius:26px;
box-shadow:var(--shadow),inset 0 1px 0 var(--stroke),inset 0 0 0 1px var(--stroke-in);
-webkit-backdrop-filter:blur(26px) saturate(170%);backdrop-filter:blur(26px) saturate(170%)}
.glass::before{content:"";position:absolute;inset:0;border-radius:inherit;pointer-events:none;opacity:var(--sheen);
background:linear-gradient(145deg,rgba(255,255,255,.4),rgba(255,255,255,0) 40%)}
.glass>*{position:relative}
@media (max-width:700px){.glass{-webkit-backdrop-filter:blur(14px);backdrop-filter:blur(14px)}.glass::before{display:none}}
@supports not ((backdrop-filter:blur(1px)) or (-webkit-backdrop-filter:blur(1px))){.glass{background:var(--glass2)}}
.top{display:flex;align-items:flex-end;justify-content:space-between;gap:12px;margin:4px 4px 20px}
h1{margin:0;font-size:34px;line-height:1.1;font-weight:700;letter-spacing:-.02em}
.sub{margin:5px 0 0;color:var(--text2);font-size:15px}
h2{margin:24px 8px 10px;font-size:13px;font-weight:600;letter-spacing:.06em;text-transform:uppercase;color:var(--text2)}
.chip{display:inline-flex;align-items:center;gap:8px;padding:8px 14px;border-radius:99px;font-size:13px;font-weight:600;white-space:nowrap;flex:none}
.dot{width:8px;height:8px;border-radius:50%;background:var(--text3)}
.chip.ok .dot{background:var(--green);box-shadow:0 0 10px var(--green)}
.chip.bad .dot{background:var(--red);box-shadow:0 0 10px var(--red)}
.chip.wait .dot{background:var(--orange);animation:pulse 1.2s ease-in-out infinite}
@keyframes pulse{50%{opacity:.3}}
.page{display:none}
.page.on{display:block}
.card{padding:18px;margin-bottom:14px}
.hero{padding:28px 22px;text-align:center;margin-bottom:14px}
.lab{font-size:12px;font-weight:600;letter-spacing:.07em;text-transform:uppercase;color:var(--text2)}
.big{font-size:80px;font-weight:700;letter-spacing:-.045em;line-height:1.05;margin:8px 0 4px;font-variant-numeric:tabular-nums}
.big small{font-size:26px;font-weight:600;color:var(--text2);margin-left:8px;letter-spacing:0}
.mid{font-size:30px;font-weight:700;letter-spacing:-.02em;margin:10px 0 6px}
.hint{color:var(--text2);font-size:15px}
.grid{display:grid;grid-template-columns:1fr 1fr;gap:12px;margin-bottom:14px}
.tile{padding:16px 18px;min-width:0}
.tile .v{margin-top:7px;font-size:18px;font-weight:600;overflow-wrap:anywhere;display:flex;align-items:center;gap:10px}
.wide{grid-column:1/-1}
.list{padding:2px 18px;margin-bottom:14px}
.row{display:flex;align-items:center;justify-content:space-between;gap:16px;padding:14px 0;border-bottom:1px solid var(--sep)}
.row:last-child{border-bottom:0}
.row .k{color:var(--text2);flex:none}
.row .v{font-weight:600;text-align:right;overflow-wrap:anywhere;display:flex;align-items:center;gap:10px;justify-content:flex-end}
.btn{appearance:none;font:inherit;font-weight:600;font-size:17px;min-height:52px;padding:0 20px;width:100%;cursor:pointer;
display:flex;align-items:center;justify-content:center;gap:9px;color:var(--text);
background:var(--field);border:1px solid var(--stroke);border-radius:17px;
transition:transform .12s,opacity .2s}
.btn:active{transform:scale(.975)}
.btn:disabled{opacity:.45;pointer-events:none}
.btn.pri{color:#fff;background:linear-gradient(180deg,#4aa3ff,#0a7aff);border-color:rgba(255,255,255,.38);box-shadow:0 8px 20px rgba(10,122,255,.34),inset 0 1px 0 rgba(255,255,255,.4)}
.btn.dng{color:var(--red)}
.btn svg{width:20px;height:20px}
.stack{display:grid;gap:10px}
.inp{width:100%;min-height:52px;padding:0 16px;font:inherit;font-size:17px;color:var(--text);background:var(--field);border:1px solid var(--stroke);border-radius:17px;outline:0}
.inp:focus{border-color:var(--blue);box-shadow:0 0 0 4px rgba(10,122,255,.22)}
.inp::placeholder{color:var(--text3)}
.steps{margin:0 0 4px;padding:0;list-style:none;counter-reset:s}
.steps li{counter-increment:s;position:relative;padding:0 0 14px 40px;color:var(--text2);font-size:15px}
.steps li::before{content:counter(s);position:absolute;left:0;top:-1px;width:28px;height:28px;border-radius:50%;background:var(--field);border:1px solid var(--stroke);color:var(--text);font-weight:700;font-size:14px;display:flex;align-items:center;justify-content:center}
.pill{display:inline-block;padding:7px 14px;margin:0 8px 8px 0;border-radius:99px;background:var(--field);border:1px solid var(--stroke);font-size:14px;font-weight:600}
.net{display:flex;align-items:center;gap:12px;width:100%;padding:15px 18px;border:0;border-bottom:1px solid var(--sep);background:none;color:inherit;font:inherit;text-align:left;cursor:pointer}
.net:last-child{border-bottom:0}
.net b{flex:1;font-weight:600;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.net svg{width:18px;height:18px;flex:none;color:var(--text2);fill:none;stroke:currentColor;stroke-width:2;stroke-linecap:round;stroke-linejoin:round}
.net svg.cur{color:var(--green)}
.net:disabled{opacity:.5}
.bars{display:inline-flex;align-items:flex-end;gap:2px;height:16px;flex:none}
.bars i{width:4px;border-radius:2px;background:var(--text);opacity:.2}
.bars i:nth-child(1){height:5px}.bars i:nth-child(2){height:8px}.bars i:nth-child(3){height:12px}.bars i:nth-child(4){height:16px}
.bars i.on{opacity:.85}
.empty{padding:22px 18px;text-align:center;color:var(--text2)}
.ev{display:flex;gap:14px;padding:11px 0;border-bottom:1px solid var(--sep);font-size:14px}
.ev:last-child{border-bottom:0}
.ev.e span{color:var(--red)}
.ev.w span{color:var(--orange)}
.ev time{color:var(--text3);font-variant-numeric:tabular-nums;flex:none}
.back{appearance:none;border:0;background:none;color:var(--blue);font:inherit;font-weight:600;font-size:17px;padding:6px 8px;margin:-6px 0 8px -8px;cursor:pointer}
.net .v{color:var(--text2);font-size:15px;flex:none}
.ta{width:100%;min-height:104px;padding:12px 14px;font:12px/1.4 ui-monospace,SFMono-Regular,Menlo,monospace;color:var(--text);background:var(--field);border:1px solid var(--stroke);border-radius:17px;outline:0;resize:vertical;word-break:break-all}
.ta:focus{border-color:var(--blue);box-shadow:0 0 0 4px rgba(10,122,255,.22)}
.note{margin:0 0 12px;padding:12px 14px;border-radius:14px;background:var(--field);border:1px solid var(--stroke);font-size:14px;color:var(--text2)}
.foot{text-align:center;color:var(--text3);font-size:13px;margin:22px 0 0}
.tabs{position:fixed;left:50%;transform:translateX(-50%);bottom:calc(14px + env(safe-area-inset-bottom));z-index:20;
display:flex;gap:4px;padding:6px;border-radius:32px;width:min(calc(100% - 28px),470px);background:var(--tabs)}
.tab{flex:1;display:flex;flex-direction:column;align-items:center;gap:3px;padding:9px 2px 8px;border:0;border-radius:26px;background:none;color:var(--text2);font:inherit;font-size:11px;font-weight:600;cursor:pointer;transition:background .2s,color .2s}
.tab svg{width:25px;height:25px;fill:none;stroke:currentColor;stroke-width:1.8;stroke-linecap:round;stroke-linejoin:round}
.tab[aria-selected=true]{color:var(--blue);background:rgba(10,122,255,.15)}
.toast{position:fixed;left:50%;top:calc(12px + env(safe-area-inset-top));z-index:60;width:min(calc(100% - 28px),520px);padding:14px 18px;border-radius:22px;
background:var(--glass2);font-weight:500;line-height:1.35;transform:translate(-50%,-160%);opacity:0;pointer-events:none;transition:transform .38s cubic-bezier(.2,.9,.3,1.1),opacity .25s}
.toast.on{transform:translate(-50%,0);opacity:1;pointer-events:auto}
.scrim{position:fixed;inset:0;z-index:50;display:none;align-items:flex-end;justify-content:center;background:rgba(10,14,28,.36);-webkit-backdrop-filter:blur(6px);backdrop-filter:blur(6px)}
.scrim.on{display:flex;animation:fade .2s}
@keyframes fade{from{opacity:0}}
.panel{width:min(100% - 16px,520px);margin:0 8px calc(8px + env(safe-area-inset-bottom));padding:24px 20px 20px;border-radius:32px;background:var(--glass2);animation:up .34s cubic-bezier(.2,.9,.3,1)}
@keyframes up{from{transform:translateY(46px);opacity:0}}
.panel h3{margin:0 0 6px;font-size:21px;letter-spacing:-.01em}
.panel p{margin:0 0 18px;color:var(--text2);font-size:15px}
.spin{width:36px;height:36px;border-radius:50%;border:3.5px solid var(--sep);border-top-color:var(--blue);animation:rot .8s linear infinite;margin:4px 0 16px}
@keyframes rot{to{transform:rotate(360deg)}}
.two{display:grid;grid-template-columns:1fr 1fr;gap:10px}
.eye{position:absolute;right:6px;top:6px;width:auto;min-height:40px;padding:0 12px;font-size:14px;border-radius:13px;color:var(--blue)}
.fld{position:relative;margin-bottom:12px}
.fld .inp{padding-right:100px}
@media (prefers-reduced-motion:reduce){*{animation:none!important;transition:none!important}}
</style></head><body>
<svg width="0" height="0" style="position:absolute" aria-hidden="true"><defs>
<symbol id="i-home" viewBox="0 0 24 24"><path d="M3.5 11.2 12 4l8.5 7.2M5.8 9.6V19a1 1 0 0 0 1 1h10.4a1 1 0 0 0 1-1V9.6"/><path d="M10 20v-5.2h4V20"/></symbol>
<symbol id="i-wifi" viewBox="0 0 24 24"><path d="M2.6 9.2a14 14 0 0 1 18.8 0M5.7 12.6a9.6 9.6 0 0 1 12.6 0M8.8 16a5.2 5.2 0 0 1 6.4 0"/><circle cx="12" cy="19.3" r="1" fill="currentColor"/></symbol>
<symbol id="i-cal" viewBox="0 0 24 24"><rect x="3.5" y="4" width="17" height="16" rx="4.5"/><path d="M7.8 11.2a4.5 4.5 0 0 1 8.4 0M12 11.4l2-2.6"/></symbol>
<symbol id="i-sys" viewBox="0 0 24 24"><path d="M4 6.5h8M17 6.5h3M4 12h3M12 12h8M4 17.5h9M18 17.5h2"/><circle cx="14.5" cy="6.5" r="2.3"/><circle cx="9.5" cy="12" r="2.3"/><circle cx="15.5" cy="17.5" r="2.3"/></symbol>
<symbol id="i-chev" viewBox="0 0 24 24"><path d="m9 6 6 6-6 6"/></symbol>
<symbol id="i-lock" viewBox="0 0 24 24"><rect x="5" y="10.5" width="14" height="9.5" rx="2.6"/><path d="M8.5 10.5V8a3.5 3.5 0 0 1 7 0v2.5"/></symbol>
<symbol id="i-check" viewBox="0 0 24 24"><path d="m5 12.5 4.5 4.5L19 7.5"/></symbol>
<symbol id="i-search" viewBox="0 0 24 24"><path d="M20 12a8 8 0 1 1-2.4-5.7M20 4v4.6h-4.6"/></symbol>
</defs></svg>
<main>
<header class="top">
<div><h1 id="title">Floor Scale</h1><p class="sub" id="sub">Подключение…</p></div>
<span class="chip glass" id="chip"><i class="dot"></i><span id="chiptxt">Загрузка</span></span>
</header>
<section class="page on" id="p-home">
<div class="glass hero" id="hero-w" hidden>
<div class="lab">Вес сейчас</div>
<div class="big"><span id="live">—</span><small>кг</small></div>
<div class="hint">Последний результат: <b id="res">нет данных</b></div>
</div>
<div class="glass hero" id="hero-off" hidden>
<div class="lab">Весы</div>
<div class="mid" id="ho-main">Проверяем…</div>
<div class="hint" id="ho-sub"></div>
</div>
<div class="grid">
<div class="glass tile"><div class="lab" id="h-peer-lab">Связь</div><div class="v" id="h-peer">—</div></div>
<div class="glass tile"><div class="lab">Калибровка</div><div class="v" id="h-cal">—</div></div>
</div>
</section>
<section class="page" id="p-net">
<div class="glass list">
<div class="row"><span class="k">Сеть</span><span class="v" id="n-net">—</span></div>
<div class="row"><span class="k">IP-адрес</span><span class="v" id="n-ip">—</span></div>
<div class="row"><span class="k">Сигнал</span><span class="v"><span id="n-bars"></span><span id="n-sig">—</span></span></div>
<div class="row"><span class="k">Режим пары</span><span class="v" id="n-role">—</span></div>
<div class="row"><span class="k">Второе устройство</span><span class="v" id="n-peer">—</span></div>
<div class="row"><span class="k">Статус</span><span class="v" id="n-st">—</span></div>
</div>
<h2>Окно настройки</h2>
<div class="glass card">
<p class="hint" style="margin:0 0 14px" id="n-win">Окно закрыто. Пока оно закрыто, устройство не принимает новые настройки сети от соседа.</p>
<button class="btn" id="b-open"><span>Открыть на 5 минут</span></button>
</div>
<h2>Wi-Fi сети</h2>
<button class="btn pri" id="b-scan"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><use href="#i-search"/></svg><span>Искать сети</span></button>
<div class="glass list" id="list" style="margin-top:14px;padding:0" hidden></div>
</section>
<section class="page" id="p-sys">
<div class="glass list">
<div class="row"><span class="k">Устройство</span><span class="v" id="s-dev">—</span></div>
<div class="row"><span class="k">Время устройства</span><span class="v" id="s-clock">—</span></div>
</div>
<h2>Разделы</h2>
<div class="glass list" style="padding:0">
<button class="net" type="button" data-go="cal"><b>Калибровка</b><span class="v" id="l-cal"></span><svg viewBox="0 0 24 24"><use href="#i-chev"/></svg></button>
<button class="net" type="button" data-go="cfg"><b>Настройки: экспорт и импорт</b><svg viewBox="0 0 24 24"><use href="#i-chev"/></svg></button>
<button class="net" type="button" data-go="tests"><b>Тесты</b><svg viewBox="0 0 24 24"><use href="#i-chev"/></svg></button>
</div>
<div id="install">
<h2>На экран телефона</h2>
<div class="glass card">
<p class="hint" style="margin:0 0 10px" id="inst-text"></p>
<p class="hint" style="margin:0">Адрес этого устройства в сети настройки и в домашней сети разный, поэтому ярлык нужно добавить отдельно в каждой, когда вы туда попадёте.</p>
</div>
</div>
<h2>Сброс сети</h2>
<div class="glass card">
<p class="hint" style="margin:0 0 14px">Сбрасывает Wi-Fi на обоих устройствах, если второе отвечает. Оба перезагрузятся и заново найдут друг друга.</p>
<button class="btn dng" id="b-reset">Сбросить сеть</button>
</div>
<h2>Перезагрузка</h2>
<div class="glass card">
<p class="hint" style="margin:0 0 14px">Перезагружает только это устройство. Сосед не затрагивается, сеть сохраняется.</p>
<button class="btn" id="b-reboot">Перезагрузить</button>
</div>
<h2>События</h2>
<div class="glass list" id="events"></div>
<p class="foot">Floor Scale · веб-интерфейс устройства</p>
</section>
<section class="page" id="p-cal">
<button class="back" type="button" data-go="sys">‹ Система</button>
<div class="note" id="cal-note" hidden>Весы не на связи: калибровка недоступна.</div>
<div class="glass hero">
<div class="lab">Вес сейчас</div>
<div class="big"><span id="c-live">—</span><small>кг</small></div>
</div>
<div class="glass card">
<ol class="steps">
<li>Уберите всё с платформы и нажмите «Обнулить».</li>
<li>Введите массу известного груза и нажмите «Калибровать», затем положите груз на платформу.</li>
<li>Повторите с другими грузами: можно сохранить до пяти точек, промежуточные значения считаются линейно.</li>
</ol>
<div class="stack">
<button class="btn" id="b-zero">Обнулить</button>
<input class="inp" id="known" type="number" inputmode="decimal" step="0.1" min="0.1" placeholder="Известный вес, кг" aria-label="Известный вес, кг">
<button class="btn pri" id="b-calib">Калибровать по грузу</button>
</div>
</div>
<h2>Точки калибровки</h2>
<div class="glass card">
<div id="curve"></div>
<div class="hint" id="curve-note" style="margin-bottom:14px"></div>
<button class="btn dng" id="b-curve">Сбросить точки</button>
</div>
</section>
<section class="page" id="p-cfg">
<button class="back" type="button" data-go="sys">‹ Система</button>
<h2 style="margin-top:6px">Экспорт</h2>
<div class="glass card">
<p class="hint" style="margin:0 0 14px">Файл содержит сохранённую домашнюю сеть и калибровку весов. Пароль Wi-Fi в нём зашифрован ключом устройств: открыть файл можно только на устройствах этой пары.</p>
<button class="btn pri" id="b-export">Скачать файл настроек</button>
<div id="x-box" hidden style="margin-top:14px">
<p class="hint" id="x-info" style="margin:0 0 8px"></p>
<textarea class="ta" id="x-out" readonly aria-label="Содержимое файла настроек"></textarea>
<button class="btn" id="b-copy" style="margin-top:10px">Скопировать текст</button>
</div>
</div>
<h2>Импорт</h2>
<div class="glass card">
<p class="hint" style="margin:0 0 14px">Выберите файл настроек или вставьте его содержимое. Настройки применятся к этому и ко второму устройству. Если сеть изменится, устройства перезагрузятся.</p>
<input id="f-in" type="file" accept=".json,.txt,application/json,text/plain" hidden>
<div class="stack">
<button class="btn" id="b-file">Выбрать файл…</button>
<textarea class="ta" id="i-in" placeholder="Или вставьте содержимое файла сюда" aria-label="Содержимое файла настроек для импорта"></textarea>
<button class="btn dng" id="b-import">Импортировать</button>
</div>
</div>
</section>
<section class="page" id="p-tests">
<button class="back" type="button" data-go="sys">‹ Система</button>
<p class="hint" style="margin:0 0 14px">Каждый тест пишет начало и конец в журнал устройства. Время в строках — внутреннее время устройства, если оно синхронизировано со страницей, иначе — время с запуска.</p>
<div class="glass card">
<h3 style="margin:0 0 6px">Маркер в журнал</h3>
<p class="hint" style="margin:0 0 12px">Записывает отметку в журнал. Нажмите перед нужным действием, чтобы найти момент в логе.</p>
<button class="btn" data-test="marker">Записать маркер</button>
</div>
<div class="glass card">
<h3 style="margin:0 0 6px">Состояние роли</h3>
<p class="hint" style="margin:0 0 12px">Пишет роль, своя точка активна или нет, сохранённую сеть и связь с соседом. Без сканирования.</p>
<button class="btn" data-test="role">Проверить роль</button>
</div>
<div class="glass card">
<h3 style="margin:0 0 6px">Скан эфира</h3>
<p class="hint" style="margin:0 0 12px">Один скан сетей: пишет все найденные точки и результат поиска точки настройки. Связь на несколько секунд прерывается.</p>
<button class="btn" data-test="scan">Сканировать</button>
</div>
<div class="glass card">
<h3 style="margin:0 0 6px">Тай-брейк: сброс обоих</h3>
<p class="hint" style="margin:0 0 12px">Сбрасывает сеть на обоих устройствах: они перезагружаются почти одновременно. Через ~70 с после загрузки каждое пишет итог роли в журнал. Лог обоих устройств нужен для разбора.</p>
<button class="btn dng" data-test="tiebreak">Запустить тай-брейк</button>
</div>
<div class="glass card">
<h3 style="margin:0 0 6px">Проверка соседа</h3>
<p class="hint" style="margin:0 0 12px">Запрос к соседу по адресу точки и по имени. Пишет ответ, время ответа, запомненный адрес соседа и число пропущенных проверок связи подряд.</p>
<button class="btn" data-test="peer">Проверить соседа</button>
</div>
<div class="glass card" id="tc-pd" hidden>
<h3 style="margin:0 0 6px">Данные весов на экране</h3>
<p class="hint" style="margin:0 0 12px">Пишет, получены ли от весов вес и калибровка, размер ответа и его возраст. Данные считаются свежими, если им меньше 10 с.</p>
<button class="btn" data-test="peer_data">Проверить данные весов</button>
</div>
<div class="glass card">
<h3 style="margin:0 0 6px">Экспорт настроек</h3>
<p class="hint" style="margin:0 0 12px">Собирает файл настроек в памяти, шифрует и расшифровывает его и пишет в журнал, что в нём есть (сеть, калибровка) и размер. Пароли в журнал не пишутся.</p>
<button class="btn" data-test="export">Проверить экспорт</button>
</div>
</section>
</main>
<nav class="tabs glass" role="tablist" aria-label="Разделы">
<button class="tab" role="tab" id="t-home" data-p="home" aria-selected="true"><svg viewBox="0 0 24 24"><use href="#i-home"/></svg>Главная</button>
<button class="tab" role="tab" id="t-net" data-p="net" aria-selected="false"><svg viewBox="0 0 24 24"><use href="#i-wifi"/></svg>Сеть</button>
<button class="tab" role="tab" id="t-sys" data-p="sys" aria-selected="false"><svg viewBox="0 0 24 24"><use href="#i-sys"/></svg>Система</button>
</nav>
<div class="toast glass" id="toast" role="status" aria-live="polite"></div>
<div class="scrim" id="scrim">
<div class="panel glass" id="s-pass" hidden>
<h3 id="ps-ssid">Сеть</h3>
<p>Введите пароль Wi-Fi. Мы проверим подключение и передадим сеть второму устройству.</p>
<div class="fld">
<input class="inp" id="ps-pass" type="password" placeholder="Пароль" maxlength="64" autocomplete="off" autocapitalize="off" spellcheck="false" aria-label="Пароль Wi-Fi">
<button class="btn eye" id="ps-eye" type="button">Показать</button>
</div>
<div class="two"><button class="btn" id="ps-cancel">Отмена</button><button class="btn pri" id="ps-go">Подключить</button></div>
</div>
<div class="panel glass" id="s-prog" hidden>
<div class="spin"></div>
<h3 id="pg-t">Подождите</h3>
<p id="pg-p" style="margin:0"></p>
</div>
<div class="panel glass" id="s-ask" hidden>
<h3 id="ask-t"></h3>
<p id="ask-p"></p>
<div class="two"><button class="btn" id="ask-no">Отмена</button><button class="btn" id="ask-ok">ОК</button></div>
</div>
</div>
<script>
/* Отладка по адресу #debug: показывает, перезагружается ли страница при переключении вкладок */
(function(){
var ss=window.sessionStorage,on=false;
try{on=/debug/.test(location.hash)||ss.getItem('dbg')==='1';if(on)ss.setItem('dbg','1')}catch(e){}
if(!on)return;
var now=function(){return Math.round(performance.now())};
var loads=0,hc=0,reqs={},inflight=0,logs=[];
try{loads=(+ss.getItem('dbg_loads')||0)+1;ss.setItem('dbg_loads',loads);logs=JSON.parse(ss.getItem('dbg_log')||'[]')}catch(e){loads=1}
function log(t){logs.push(new Date().toLocaleTimeString('ru-RU')+' +'+now()+'мс '+t);if(logs.length>40)logs.shift();try{ss.setItem('dbg_log',JSON.stringify(logs))}catch(e){}}
log('ЗАГРУЗКА страницы №'+loads);
addEventListener('beforeunload',function(){log('beforeunload: страница выгружается')});
addEventListener('pagehide',function(e){log('pagehide persisted='+e.persisted)});
addEventListener('pageshow',function(e){log('pageshow persisted='+e.persisted)});
addEventListener('hashchange',function(){hc++;log('hashchange '+location.hash)});
document.addEventListener('visibilitychange',function(){log('visibility '+document.visibilityState)});
document.addEventListener('click',function(e){var t=e.target.closest&&e.target.closest('.tab');if(t)log('нажата вкладка '+t.dataset.p)},true);
var of=window.fetch;
window.fetch=function(u){var k=String(u).split('?')[0],t=now();inflight++;reqs[k]=(reqs[k]||0)+1;
return of.apply(this,arguments).then(function(r){inflight--;var d=now()-t;if(d>800||k!=='/status')log('fetch '+k+' '+d+'мс');return r},function(e){inflight--;log('fetch '+k+' ОШИБКА');throw e})};
function init(){
var box=document.createElement('div');
box.style.cssText='position:fixed;left:6px;right:6px;top:6px;z-index:2147483000;padding:8px 10px;border-radius:10px;background:rgba(0,0,0,.82);color:#fff;font:11px/1.35 ui-monospace,Menlo,monospace;white-space:pre-wrap;word-break:break-all;max-height:46vh;overflow:auto';
var pre=document.createElement('div'),x=document.createElement('button');
x.textContent='Закрыть отладку';x.style.cssText='display:block;margin:6px 0 0;padding:6px 10px;font:inherit';
x.onclick=function(){try{ss.removeItem('dbg');ss.removeItem('dbg_loads');ss.removeItem('dbg_log')}catch(e){}box.remove()};
box.append(pre,x);document.body.append(box);
function paint(){
var nav=(performance.getEntriesByType&&performance.getEntriesByType('navigation')[0])||{};
var res=performance.getEntriesByType?performance.getEntriesByType('resource'):[];
pre.textContent='ОТЛАДКА\nзагрузок страницы за сессию: '+loads+'\nвозраст страницы: '+(performance.now()/1000).toFixed(1)+' с\nтип загрузки: '+(nav.type||'?')+'\nсмен адреса #: '+hc+
'\nзапросов: '+Object.keys(reqs).map(function(k){return k+'×'+reqs[k]}).join(', ')+' (в полёте '+inflight+')'+
'\nресурсов страницы: '+res.length+(res.length?' (последний: '+res[res.length-1].name.replace(location.origin,'')+')':'')+
'\nготовность: '+document.readyState+'\n--- журнал ---\n'+logs.slice(-12).join('\n');
}
paint();setInterval(paint,700);
}
if(document.body)init();else document.addEventListener('DOMContentLoaded',init);
})();
</script>
<script>
const $=id=>document.getElementById(id);
const el=(t,c,x)=>{const e=document.createElement(t);if(c)e.className=c;if(x!=null)e.textContent=x;return e};
const clear=e=>{e.textContent=''};
const num=v=>v==null?'—':String(v).replace('.',',');
(function(){
const standalone=window.navigator.standalone===true||(window.matchMedia&&matchMedia('(display-mode: standalone)').matches);
const ua=navigator.userAgent||'';
const ios=/iPhone|iPad|iPod/.test(ua)||(/Macintosh/.test(ua)&&navigator.maxTouchPoints>1);
const box=document.getElementById('install');
if(standalone){box.hidden=true;return;}
document.getElementById('inst-text').textContent=ios
?'Откройте страницу в Safari, нажмите «Поделиться» и выберите «На экран Домой». Ярлык откроется без адресной строки, со своей иконкой.'
:'В Chrome откройте меню ⋮ и выберите «Добавить на главный экран» (или «Установить приложение»). Ярлык получит свою иконку; адресная строка может остаться, потому что устройство отдаёт страницу по HTTP, без HTTPS.';
})();
let S={},W=null,offline=0,isScale=false,busy=false,pend='',askRes=null,cur=null,tt,ready=false,lastSt='';
const events=[];
/* ---------- сообщения ---------- */
function logEv(t,l,at){
if(!t||(!at&&events[0]&&events[0].t===t&&events[0].l===(l||'i')))return;
events.unshift({t:t,l:l||'i',at:at||new Date()});if(events.length>60)events.pop();renderEv();
}
let lastEv=0;
async function pullEvents(){
const j=await api('/events?after='+lastEv);
if(j.last<lastEv)lastEv=0;            // устройство перезагрузилось: нумерация началась заново
(j.ev||[]).forEach(e=>{
if(e.i<=lastEv)return;
lastEv=e.i;
const at=e.t?new Date(e.t*1000):new Date();
logEv(e.m,e.l,at);
});
}
function renderEv(){
const b=$('events');clear(b);
if(!events.length){b.append(el('div','empty','Пока нет событий'));return}
events.forEach(e=>{const r=el('div','ev'+(e.l==='e'?' e':e.l==='w'?' w':''));r.append(el('time',null,e.at.toLocaleTimeString('ru-RU')),el('span',null,e.t));b.append(r)});
}
function say(t,keep){
const s=$('toast');clearTimeout(tt);
if(!t){s.classList.remove('on');return}
s.textContent=t;s.classList.add('on');
if(!/^Команда принята/.test(t))logEv(t,/Нет связи|Ошибка|Не удалось|не ответ|не найден|не на связи|повреждена|не подходит|слишком большой/i.test(t)?'e':'i');
tt=setTimeout(()=>s.classList.remove('on'),keep?6500:2800);
}
$('toast').onclick=()=>say('');
/* ---------- запросы ---------- */
async function api(u,o,ms){
const c=new AbortController(),id=setTimeout(()=>c.abort(),ms||8000);
try{const r=await fetch(u,Object.assign({signal:c.signal,cache:'no-store'},o));return await r.json()}
finally{clearTimeout(id)}
}
const post=(u,b,ms)=>api(u,b?{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:new URLSearchParams(b)}:{method:'POST'},ms);
function setBusy(b){busy=b;document.querySelectorAll('.btn,.net').forEach(x=>x.disabled=b)}
/* ---------- разделы ---------- */
const pages=['home','net','sys','cal','cfg','tests'];
const tabOf={cal:'sys',cfg:'sys',tests:'sys'};
let curPage='';
function go(p){
if(!pages.includes(p))p='home';
if(p===curPage)return;
curPage=p;
const tab=tabOf[p]||p;
pages.forEach(x=>$('p-'+x).classList.toggle('on',x===p));
['home','net','sys'].forEach(x=>$('t-'+x).setAttribute('aria-selected',x===tab));
scrollTo(0,0);
}
const nav=p=>{if(location.hash.slice(1)===p)go(p);else location.hash=p};
document.querySelectorAll('.tab').forEach(t=>t.onclick=()=>nav(t.dataset.p));
document.querySelectorAll('[data-go]').forEach(t=>t.onclick=()=>nav(t.dataset.go));
addEventListener('hashchange',()=>go(location.hash.slice(1)));
/* ---------- отрисовка статуса ---------- */
function bars(r){
const n=isNaN(r)?0:r>=-55?4:r>=-67?3:r>=-75?2:r>=-85?1:0,b=el('span','bars');
b.setAttribute('aria-hidden','true');
for(let i=1;i<=4;i++){const x=document.createElement('i');if(i<=n)x.className='on';b.append(x)}
return b;
}
const sig={};
function put(id,node,key){if(sig[id]===key)return;sig[id]=key;const e=$(id);clear(e);if(node)e.append(node)}
const txt=(id,v)=>{const e=$(id);if(e.textContent!==v)e.textContent=v};
const vis=(id,show)=>{const e=$(id);if(e.hidden===show)e.hidden=!show};
function chip(c,t){$('chip').className='chip glass '+c;$('chiptxt').textContent=t}
function renderCurve(s){
if(sig.curve===s)return;sig.curve=s;
const box=$('curve'),note=$('curve-note');clear(box);note.textContent='';
if(!s||s.indexOf('нет точек')===0){
note.textContent=s?'Точек пока нет ('+s.replace('нет точек, ','')+').':'Точек пока нет.';return;
}
s.split(' | ').forEach((p,i)=>box.append(el('span','pill',i===0?p.replace('ноль','Ноль:'):p.replace('.',','))));
}
function render(j){
isScale=j.kind==='scale';
W=isScale?j:(j.scale||null);
if(!ready){ready=true;go(location.hash.slice(1))}
const title=j.title||'Floor Scale';txt('title',title);if(document.title!==title)document.title=title;
txt('sub',j.role||'Роль не определена');
const peerName=isScale?'Экран':'Весы';
let c='',t='Ожидание сети',peer='Ожидание сети',main='Ожидание сети';
if(j.peer_applicable){
if(!j.peer_checked){c='wait';t='Проверка связи';peer='Проверка…';main='Проверяем связь…'}
else if(j.peer_link){c='ok';t='Связь есть';peer='Подключено — '+(j.peer_role||'сосед');main='на связи'}
else{c='bad';t='Нет связи';peer='Нет связи';main='не найдены'}
}
chip(c,t);
// главная: вес (свой или от весов через экран) и кратко связь с калибровкой
vis('hero-w',!!W);vis('hero-off',!W);
if(W){txt('live',num(W.live));txt('c-live',num(W.live));txt('res',W.result==null?'нет данных':num(W.result)+' кг')}
else{
txt('c-live','—');
txt('ho-main',j.peer_applicable&&j.peer_checked&&j.peer_link?'Ждём данные весов…':'Весы не на связи');
txt('ho-sub',j.peer_applicable&&j.peer_checked&&j.peer_link?'Весы нашлись, данные ещё не пришли.':'Вес и калибровка появятся, когда экран найдёт весы.');
}
txt('h-peer-lab','Связь: '+peerName.toLowerCase());
txt('h-peer',j.peer_applicable?(j.peer_checked?(j.peer_link?'Есть':'Нет'):'Проверка…'):'—');
const cs=W&&W.curve;
const npts=cs&&cs.indexOf('нет точек')!==0?cs.split(' | ').length-1:0;
txt('h-cal',!W?'—':npts?npts+' '+(npts===1?'точка':npts<5?'точки':'точек'):'линейно');
txt('l-cal',!W?'нет данных':npts?npts+' т.':'линейно');
// сеть
const net=j.network&&j.network!=='нет'?j.network:'Не подключено';
const ip=j.ip&&j.ip!=='нет'?j.ip:'—';
const rssi=parseInt(j.signal,10),sg=isNaN(rssi)?'—':rssi+' dBm';
txt('n-net',net);txt('n-ip',ip);put('n-bars',bars(rssi),rssi);txt('n-sig',sg);
txt('n-role',j.role||'не определён');txt('n-peer',peer);txt('n-st',j.status||'—');
txt('n-win',j.window?'Окно открыто: устройство принимает настройки сети от соседа.':'Окно закрыто. Пока оно закрыто, устройство не принимает новые настройки сети от соседа.');
// система
txt('s-dev',title);
txt('s-clock',j.clock_ts?new Date(j.clock_ts*1000).toLocaleString('ru-RU'):'не задано');
vis('tc-pd',!isScale);
// калибровка
vis('cal-note',!W);
if(W)renderCurve(W.curve);else renderCurve('');
}
async function refresh(){
if(document.hidden)return;
try{S=await api('/status');offline=0;render(S);syncClock(S);pullEvents().catch(()=>{})}
catch(e){if(++offline>=2){chip('bad','Нет связи со страницей')}}
}
/* ---------- время ---------- */
let lastClockTry=0;
// У устройства нет часов с батареей и сети: время берём у телефона/ПК, на котором открыта страница
function syncClock(j){
const now=Math.floor(Date.now()/1000);
if(Math.abs((j.clock_ts||0)-now)<=5||Date.now()-lastClockTry<10000)return;
lastClockTry=Date.now();
post('/time',{ts:now,tz:new Date().getTimezoneOffset()}).catch(()=>{});
}
/* ---------- панели ---------- */
function show(id){
cur=id;document.querySelectorAll('.panel').forEach(p=>{p.hidden=p.id!==id});$('scrim').classList.add('on');
}
function closeSheet(v){
$('scrim').classList.remove('on');cur=null;
if(askRes){const r=askRes;askRes=null;r(v===true)}
}
$('scrim').onclick=e=>{if(e.target===e.currentTarget&&cur!=='s-prog')closeSheet(false)};
addEventListener('keydown',e=>{if(e.key==='Escape'&&cur&&cur!=='s-prog')closeSheet(false)});
function prog(t,p){$('pg-t').textContent=t;$('pg-p').textContent=p;show('s-prog')}
function ask(t,p,ok,danger){
return new Promise(res=>{
$('ask-t').textContent=t;$('ask-p').textContent=p;
const b=$('ask-ok');b.textContent=ok;b.className='btn '+(danger?'dng':'pri');
askRes=res;show('s-ask');
});
}
$('ask-ok').onclick=()=>closeSheet(true);
$('ask-no').onclick=()=>closeSheet(false);
/* ---------- действия ---------- */
async function openWin(){
if(busy)return;
try{await post('/open');say('Окно настройки открыто на 5 минут')}catch(e){say('Нет связи с устройством')}
refresh();
}
async function resetNet(){
if(busy)return;
if(!await ask('Сбросить сеть?','Wi-Fi будет сброшен на обоих устройствах. Они перезагрузятся и заново найдут друг друга.','Сбросить',true))return;
setBusy(true);prog('Сброс сети','Передаём команду второму устройству…');
try{const j=await post('/reset',null,20000);say(j.msg||'Сброс сети, перезагрузка…',true)}
catch(e){say('Устройство перезагружается…',true)}
setBusy(false);closeSheet();
}
async function scan(){
if(busy)return;
const box=$('list');box.hidden=false;clear(box);box.append(el('div','empty','Ищем сети…'));
setBusy(true);
try{
const n=await api('/scan',null,20000);
clear(box);
if(n.error){box.hidden=true;say('Ошибка поиска: '+n.error,true)}
else{
const best={};
n.forEach(x=>{if(x.ssid&&(!best[x.ssid]||x.rssi>best[x.ssid].rssi))best[x.ssid]=x});
const arr=Object.keys(best).map(k=>best[k]).sort((a,b)=>b.rssi-a.rssi);
if(!arr.length)box.append(el('div','empty','Сети не найдены'));
arr.forEach(addNet);
}
}catch(e){box.hidden=true;say('Нет ответа от устройства при поиске',true)}
setBusy(false);
}
function addNet(n){
const b=el('button','net');b.type='button';
b.append(el('b',null,n.ssid),bars(n.rssi));
const ic=document.createElementNS('http://www.w3.org/2000/svg','svg');ic.setAttribute('viewBox','0 0 24 24');
const u=document.createElementNS('http://www.w3.org/2000/svg','use');
if(S.network===n.ssid){u.setAttribute('href','#i-check');ic.setAttribute('class','cur')}
else if(!n.open)u.setAttribute('href','#i-lock');
if(u.getAttribute('href')){ic.append(u);b.append(ic)}
b.onclick=()=>{if(n.open)connect(n.ssid,'');else openPass(n.ssid)};
$('list').append(b);
}
function openPass(ssid){
pend=ssid;$('ps-ssid').textContent=ssid;
const i=$('ps-pass');i.value='';i.type='password';$('ps-eye').textContent='Показать';
show('s-pass');setTimeout(()=>i.focus(),260);
}
$('ps-eye').onclick=()=>{const i=$('ps-pass'),h=i.type==='password';i.type=h?'text':'password';$('ps-eye').textContent=h?'Скрыть':'Показать'};
$('ps-cancel').onclick=()=>closeSheet(false);
$('ps-go').onclick=()=>{const p=$('ps-pass').value;if(!p){$('ps-pass').focus();return}closeSheet();connect(pend,p)};
$('ps-pass').onkeydown=e=>{if(e.key==='Enter')$('ps-go').click()};
async function connect(ssid,pass){
setBusy(true);
prog('Проверяем «'+ssid+'»','Подключаемся к сети и проверяем пароль. Страница может ненадолго потерять связь — это нормально.');
try{const j=await post('/connect',{ssid:ssid,pass:pass},60000);say(j.msg,!j.ok)}
catch(e){say('Нет связи с устройством. Возможно, точка доступа перезапустилась: подождите минуту и обновите страницу.',true)}
setBusy(false);closeSheet();refresh();
}
const calApi=(cmd,extra)=>isScale?post(cmd==='calib'?'/calib':cmd==='zero'?'/zero':'/curve_reset',extra):post('/peer_cmd',Object.assign({cmd:cmd},extra||{}));
async function zeroIt(){
if(busy)return;if(!W){say('Весы не на связи');return}
try{const j=await calApi('zero');say(j.msg,j.ok===false)}catch(e){say('Нет связи с устройством')}
setTimeout(refresh,1500);
}
async function calibIt(){
if(busy)return;if(!W){say('Весы не на связи');return}
const v=$('known').value;
if(!v){say('Укажите известный вес');$('known').focus();return}
try{const j=await calApi('calib',{known:v});say(j.msg,j.ok===false)}catch(e){say('Нет связи с устройством')}
setTimeout(refresh,1500);
}
async function rebootIt(){
if(busy)return;
if(!await ask('Перезагрузить?','Это устройство перезагрузится. Сосед не затрагивается.','Перезагрузить',true))return;
setBusy(true);prog('Перезагрузка','Устройство перезагружается…');
try{const j=await post('/reboot',null,5000);say(j.msg||'Перезагрузка',true)}
catch(e){say('Устройство перезагружается…',true)}
setBusy(false);closeSheet();
}
async function runTest(n){
if(busy)return;
try{const j=await post('/test',{name:n});say(j.msg||'Тест принят',true)}
catch(e){say('Нет связи с устройством')}
setTimeout(refresh,1500);
}
async function curveReset(){
if(busy)return;if(!W){say('Весы не на связи');return}
if(!await ask('Сбросить точки?','Все точки калибровки будут удалены.','Сбросить',true))return;
try{const j=await calApi('curve_reset');say(j.msg,j.ok===false)}catch(e){say('Нет связи с устройством')}
setTimeout(refresh,1500);
}
/* ---------- экспорт и импорт настроек ---------- */
async function exportCfg(){
if(busy)return;
setBusy(true);
try{
const j=await api('/export',null,15000);
if(!j.ok){say(j.msg||'Не удалось создать файл',true);return}
const text=JSON.stringify({app:j.app,v:j.v,kind:j.kind,blob:j.blob});
$('x-out').value=text;$('x-box').hidden=false;
$('x-info').textContent='В файле: сеть — '+(j.net?'да':'нет')+', калибровка — '+(j.cal?'да':'нет')+(!j.cal&&j.kind==='screen'?' (весы не ответили)':'');
const d=new Date(),ymd=d.getFullYear()+String(d.getMonth()+1).padStart(2,'0')+String(d.getDate()).padStart(2,'0');
const a=document.createElement('a');
a.href=URL.createObjectURL(new Blob([text],{type:'application/json'}));
a.download='floor-scale-'+j.kind+'-'+ymd+'.json';
document.body.append(a);a.click();a.remove();
setTimeout(()=>URL.revokeObjectURL(a.href),5000);
say('Файл настроек создан');
}catch(e){say('Нет связи с устройством')}
finally{setBusy(false)}
}
function copyCfg(){
const t=$('x-out');t.focus();t.select();t.setSelectionRange(0,t.value.length);
let ok=false;try{ok=document.execCommand('copy')}catch(e){}
say(ok?'Текст скопирован':'Выделите текст и скопируйте вручную');
}
function parseCfg(t){
t=(t||'').trim();if(!t)return null;
try{const o=JSON.parse(t);if(o&&typeof o.blob==='string')t=o.blob}catch(e){}
return /^[0-9a-fA-F]{60,}$/.test(t)?t:null;
}
async function importCfg(){
if(busy)return;
const blob=parseCfg($('i-in').value);
if(!blob){say('Выберите файл настроек или вставьте его содержимое',true);return}
if(!await ask('Импортировать настройки?','Сеть и калибровка из файла заменят текущие. Если сеть изменится, устройства перезагрузятся.','Импортировать',true))return;
setBusy(true);prog('Импорт настроек','Применяем настройки на этом и втором устройстве…');
try{const j=await post('/import',{blob:blob,forward:'1'},25000);say(j.msg||'Готово',j.ok===false);if(j.ok)$('i-in').value=''}
catch(e){say('Устройство перезагружается или не отвечает. Подождите минуту и обновите страницу.',true)}
setBusy(false);closeSheet();refresh();
}
$('b-export').onclick=exportCfg;$('b-copy').onclick=copyCfg;$('b-import').onclick=importCfg;
$('b-file').onclick=()=>$('f-in').click();
$('f-in').onchange=async e=>{
const f=e.target.files&&e.target.files[0];if(!f)return;
if(f.size>20000){say('Файл слишком большой для файла настроек',true);return}
try{$('i-in').value=await f.text();say('Файл прочитан: '+f.name)}catch(err){say('Не удалось прочитать файл',true)}
e.target.value='';
};
$('b-open').onclick=openWin;$('b-reset').onclick=resetNet;$('b-scan').onclick=scan;$('b-reboot').onclick=rebootIt;
document.querySelectorAll('[data-test]').forEach(b=>b.onclick=()=>runTest(b.dataset.test));
$('b-zero').onclick=zeroIt;$('b-calib').onclick=calibIt;$('b-curve').onclick=curveReset;
go(location.hash.slice(1));renderEv();refresh();
setInterval(refresh,5000);
document.addEventListener('visibilitychange',()=>{if(!document.hidden)refresh()});
</script></body></html>
)HTML";

inline std::string json_escape(const char *s) {
  std::string o;
  for (const char *p = s; *p; p++) {
    unsigned char c = (unsigned char) *p;
    if (c == '"' || c == '\\') { o += '\\'; o += (char) c; }
    else if (c < 0x20) { o += ' '; }
    else { o += (char) c; }
  }
  return o;
}

inline std::string url_decode(const std::string &s) {
  std::string o;
  for (size_t i = 0; i < s.size(); i++) {
    if (s[i] == '+') {
      o += ' ';
    } else if (s[i] == '%' && i + 2 < s.size()) {
      std::string hex = s.substr(i + 1, 2);
      o += (char) strtol(hex.c_str(), nullptr, 16);
      i += 2;
    } else {
      o += s[i];
    }
  }
  return o;
}

inline std::string form_value(const std::string &body, const char *key) {
  std::string k = std::string(key) + "=";
  size_t p;
  if (body.compare(0, k.size(), k) == 0) {
    p = 0;
  } else {
    p = body.find("&" + k);
    if (p == std::string::npos) return "";
    p += 1;
  }
  p += k.size();
  size_t e = body.find('&', p);
  return url_decode(body.substr(p, e == std::string::npos ? std::string::npos : e - p));
}

// Читает тело запроса целиком. Возвращает -1, если тело больше max_len или ошибка.
inline int recv_body(httpd_req_t *req, std::string &out, size_t max_len) {
  size_t total = req->content_len;
  if (total == 0 || total > max_len) return -1;
  out.assign(total, '\0');
  size_t got = 0;
  while (got < total) {
    int r = httpd_req_recv(req, &out[got], total - got);
    if (r <= 0) return -1;
    got += (size_t) r;
  }
  return (int) total;
}

// Число для JSON: null, если значения нет
inline std::string num_json(float v) {
  if (std::isnan(v)) return "null";
  char b[24];
  snprintf(b, sizeof(b), "%.1f", v);
  return b;
}

inline std::string local_role(std::string *connected_ssid = nullptr) {
  wifi_ap_record_t ap = {};
  if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
    char name[33] = {0};
    memcpy(name, ap.ssid, 32);
    if (connected_ssid != nullptr) *connected_ssid = name;
    return setup_ssid == name ? "Клиент (слейв)" : "Домашняя сеть";
  }
  wifi_mode_t mode = WIFI_MODE_NULL;
  if (esp_wifi_get_mode(&mode) == ESP_OK && (mode == WIFI_MODE_AP || mode == WIFI_MODE_APSTA))
    return "Точка доступа (мастер)";
  return "Ожидание сети";
}

inline bool read_identity(const std::string &ip, const std::string &expected_kind,
                          std::string &role) {
  std::string response;
  if (!prov_link::get_request("http://" + ip + "/peer_identity", response, 1500)) return false;
  const std::string kind_field = "\"kind\":\"" + expected_kind + "\"";
  if (response.find(kind_field) == std::string::npos) return false;
  const std::string key = "\"role\":\"";
  size_t pos = response.find(key);
  if (pos != std::string::npos) {
    pos += key.size();
    size_t end = response.find('"', pos);
    if (end != std::string::npos) role = response.substr(pos, end - pos);
  }
  return true;
}

inline bool resolve_peer_host(std::string &ip) {
  if (peer_host.empty()) return false;
  std::string name = peer_host;
  const std::string suffix = ".local";
  if (name.size() > suffix.size() &&
      name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0)
    name.resize(name.size() - suffix.size());
  esp_ip4_addr_t addr = {};
  if (mdns_query_a(name.c_str(), 500, &addr) != ESP_OK) return false;
  char value[16];
  snprintf(value, sizeof(value), IPSTR, IP2STR(&addr));
  ip = value;
  return true;
}

// Клиент точки, который не оказался соседом (например, телефон), не опрашиваем чаще
// раза в 10 минут. Раньше опрос был каждые 15 с и давал select() timeout в логе.
inline constexpr uint32_t kNonPeerRecheckMs = 600000;

inline std::string probe_ip;  // адрес, по которому последний раз ответил сосед

inline bool probe_peer(std::string &peer_role) {
  const std::string expected = device_kind == "scale" ? "screen" : "scale";
  const std::string role = local_role();
  if (role == "Клиент (слейв)") {
    if (peer_ap_ip.empty() || !read_identity(peer_ap_ip, expected, peer_role)) return false;
    probe_ip = peer_ap_ip;
    return true;
  }
  if (role == "Домашняя сеть") {
    // Сначала последний рабочий адрес: mDNS-запрос теряется заметно чаще, чем HTTP.
    if (!home_peer_ip.empty() && read_identity(home_peer_ip, expected, peer_role)) {
      probe_ip = home_peer_ip;
      return true;
    }
    std::string ip;
    if (resolve_peer_host(ip) && read_identity(ip, expected, peer_role)) {
      home_peer_ip = ip;
      probe_ip = ip;
      return true;
    }
    home_peer_ip.clear();
    return false;
  }
  if (role != "Точка доступа (мастер)") return false;

  // В режиме мастера ищем именно веб-морду второго устройства среди клиентов
  // DHCP, чтобы телефон, подключённый к той же AP, не считался соседом.
  wifi_sta_list_t stations = {};
  if (esp_wifi_ap_get_sta_list(&stations) != ESP_OK || stations.num == 0) return false;
  esp_netif_t *ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
  if (ap_netif == nullptr) return false;
  esp_netif_pair_mac_ip_t pairs[ESP_WIFI_MAX_CONN_NUM] = {};
  const int count = stations.num < ESP_WIFI_MAX_CONN_NUM ? stations.num : ESP_WIFI_MAX_CONN_NUM;
  for (int i = 0; i < count; i++) memcpy(pairs[i].mac, stations.sta[i].mac, 6);
  if (esp_netif_dhcps_get_clients_by_mac(ap_netif, count, pairs) != ESP_OK) return false;
  const uint32_t now = now_ms();
  for (int i = 0; i < count; i++) {
    if (pairs[i].ip.addr == 0) continue;
    int cache_slot = -1;
    for (int j = 0; j < ESP_WIFI_MAX_CONN_NUM; j++) {
      if (peer_candidate_cache[j].valid &&
          memcmp(peer_candidate_cache[j].mac, pairs[i].mac, 6) == 0) {
        cache_slot = j;
        if ((uint32_t) (now - peer_candidate_cache[j].checked_at) < kNonPeerRecheckMs) break;
        cache_slot = j;
        break;
      }
      if (!peer_candidate_cache[j].valid && cache_slot < 0) cache_slot = j;
    }
    if (cache_slot >= 0 && peer_candidate_cache[cache_slot].valid &&
        memcmp(peer_candidate_cache[cache_slot].mac, pairs[i].mac, 6) == 0 &&
        (uint32_t) (now - peer_candidate_cache[cache_slot].checked_at) < kNonPeerRecheckMs &&
        peer_candidate_cache[cache_slot].checked_at >= netrole::ap_join_ms)
      continue;
    char ip[16];
    snprintf(ip, sizeof(ip), IPSTR, IP2STR(&pairs[i].ip));
    if (read_identity(ip, expected, peer_role)) {
      if (cache_slot >= 0) peer_candidate_cache[cache_slot].valid = false;
      probe_ip = ip;
      return true;
    }
    if (cache_slot < 0) cache_slot = i % ESP_WIFI_MAX_CONN_NUM;
    memcpy(peer_candidate_cache[cache_slot].mac, pairs[i].mac, 6);
    peer_candidate_cache[cache_slot].checked_at = now;
    peer_candidate_cache[cache_slot].valid = true;
  }
  return false;
}

// Проверяем соседа вне HTTP-сервера: иначе встречные запросы /status с двух
// страниц могут занять оба HTTP server task и взаимно ждать ответа.
inline void peer_monitor_task(void *) {
  while (true) {
    std::string role;
    const std::string own_role = local_role();
    const bool applicable = own_role == "Клиент (слейв)" ||
                            own_role == "Точка доступа (мастер)" ||
                            own_role == "Домашняя сеть";
    const bool linked = applicable && probe_peer(role);
    // Одиночный пропуск (потерянный пакет, занятый сосед) не должен сбрасывать связь:
    // раньше из-за этого одно устройство видело соседа, а другое нет. Состояние
    // «нет связи» ставим после трёх подряд неудачных проверок.
    bool keep_state = false;
    if (linked) {
      peer_misses = 0;
    } else if (applicable && peer_link_state && peer_misses < 2) {
      peer_misses++;
      keep_state = true;
    }
    if (!keep_state && peer_status_lock != nullptr &&
        xSemaphoreTake(peer_status_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
      peer_link_state = linked;
      peer_check_applicable = applicable;
      peer_check_done = true;
      snprintf(peer_role_buf, sizeof(peer_role_buf), "%s", linked ? role.c_str() : "");
      snprintf(peer_ip_buf, sizeof(peer_ip_buf), "%s", linked ? probe_ip.c_str() : "");
      xSemaphoreGive(peer_status_lock);
    }
    if (!keep_state && applicable) {
      static bool prev_known = false, prev_linked = false;
      if (!prev_known || linked != prev_linked)
        evlog(linked ? 'i' : 'w', "Связь с соседом: %s", linked ? "есть" : "потеряна");
      prev_known = true;
      prev_linked = linked;
    }
    uint32_t wait = 5000;
    if (own_role == "Домашняя сеть") wait = (linked && peer_misses == 0) ? 30000 : 8000;
    vTaskDelay(pdMS_TO_TICKS(wait));
  }
}

inline bool peer_ip_now(std::string &ip) {
  ip.clear();
  if (peer_status_lock != nullptr && xSemaphoreTake(peer_status_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
    if (peer_link_state && peer_ip_buf[0] != '\0') ip = peer_ip_buf;
    xSemaphoreGive(peer_status_lock);
  }
  return !ip.empty();
}

// Экран забирает у весов их /status раз в 2 с и показывает на своей странице
// вес и калибровку. Запросы идут из отдельной задачи, а не из HTTP-обработчика.
inline void peer_data_task(void *) {
  while (true) {
    std::string ip;
    if (peer_ip_now(ip)) {
      std::string r;
      if (prov_link::get_request("http://" + ip + "/status", r, 1500) && r.size() > 2 &&
          r.front() == '{' && r.back() == '}' && r.find("\"kind\":\"scale\"") != std::string::npos &&
          peer_status_lock != nullptr && xSemaphoreTake(peer_status_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
        peer_data = r;
        peer_data_at = now_ms();
        xSemaphoreGive(peer_status_lock);
      }
    }
    vTaskDelay(pdMS_TO_TICKS(2000));
  }
}

inline std::string identity_json() {
  return "{\"kind\":\"" + device_kind + "\",\"role\":\"" +
         json_escape(local_role().c_str()) + "\"}";
}

// Состояние сети и отдельная проверка доступности именно второго устройства.
inline std::string status_json() {
  std::string network = "нет", signal = "нет", ip = "нет", connected_ssid;
  wifi_ap_record_t ap = {};
  if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
    char name[33] = {0};
    memcpy(name, ap.ssid, 32);
    network = name;
    signal = std::to_string((int) ap.rssi) + " dBm";
    connected_ssid = name;
  }
  const char *ifkey = connected_ssid.empty() ? "WIFI_AP_DEF" : "WIFI_STA_DEF";
  esp_netif_t *netif = esp_netif_get_handle_from_ifkey(ifkey);
  esp_netif_ip_info_t ipi = {};
  if (netif != nullptr && esp_netif_get_ip_info(netif, &ipi) == ESP_OK && ipi.ip.addr != 0) {
    char b[20];
    snprintf(b, sizeof(b), IPSTR, IP2STR(&ipi.ip));
    ip = b;
  }
  std::string out = "{";
  out += "\"status\":\"" + json_escape(status_buf) + "\",";
  out += "\"role\":\"" + json_escape(local_role().c_str()) + "\",";
  out += "\"network\":\"" + json_escape(network.c_str()) + "\",";
  out += "\"ip\":\"" + ip + "\",";
  out += "\"signal\":\"" + signal + "\",";
  out += std::string("\"window\":") + (window_open() ? "true" : "false");
  // Текущее время устройства (Unix, UTC) или 0, если время ещё не задано страницей
  const long long clock_now = clock_set_at ? (long long) time(nullptr) : 0;
  out += ",\"clock_ts\":" + std::to_string(clock_now);
  out += ",\"kind\":\"" + device_kind + "\"";
  out += ",\"title\":\"" + json_escape(device_title.c_str()) + "\"";
  bool checked = false, applicable = false, linked = false;
  char peer_role[sizeof(peer_role_buf)] = "";
  if (peer_status_lock != nullptr && xSemaphoreTake(peer_status_lock, pdMS_TO_TICKS(20)) == pdTRUE) {
    checked = peer_check_done;
    applicable = peer_check_applicable;
    linked = peer_link_state;
    memcpy(peer_role, peer_role_buf, sizeof(peer_role));
    peer_role[sizeof(peer_role) - 1] = '\0';
    xSemaphoreGive(peer_status_lock);
  }
  out += std::string(",\"peer_checked\":") + (checked ? "true" : "false");
  out += std::string(",\"peer_applicable\":") + (applicable ? "true" : "false");
  out += std::string(",\"peer_link\":") + (linked ? "true" : "false");
  if (linked) out += ",\"peer_role\":\"" + json_escape(peer_role) + "\"";
  if (extra_status) out += "," + extra_status();
  if (device_kind == "screen" && peer_status_lock != nullptr &&
      xSemaphoreTake(peer_status_lock, pdMS_TO_TICKS(20)) == pdTRUE) {
    if (!peer_data.empty() && (uint32_t) (now_ms() - peer_data_at) < 10000) out += ",\"scale\":" + peer_data;
    xSemaphoreGive(peer_status_lock);
  }
  out += "}";
  return out;
}

// Страница отдаётся кусками: маркер {{TITLE}} заменяется именем устройства,
// чтобы ярлык на экране телефона назывался «Весы» или «Экран».
static esp_err_t h_root(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html; charset=utf-8");
  static const char MARK[] = "{{TITLE}}";
  const char *p = PAGE;
  const char *hit;
  while ((hit = strstr(p, MARK)) != nullptr) {
    if (hit > p) httpd_resp_send_chunk(req, p, hit - p);  // нулевая длина закрыла бы ответ
    httpd_resp_send_chunk(req, device_title.c_str(), HTTPD_RESP_USE_STRLEN);
    p = hit + sizeof(MARK) - 1;
  }
  httpd_resp_send_chunk(req, p, HTTPD_RESP_USE_STRLEN);
  return httpd_resp_send_chunk(req, nullptr, 0);
}

static esp_err_t h_manifest(httpd_req_t *req) {
  const std::string t = json_escape(device_title.c_str());
  const std::string theme = device_kind == "scale" ? "#2846c8" : "#107870";
  std::string m = "{\"name\":\"Floor Scale · " + t + "\",\"short_name\":\"" + t +
      "\",\"start_url\":\"/\",\"scope\":\"/\",\"display\":\"standalone\"," +
      "\"background_color\":\"" + theme + "\",\"theme_color\":\"" + theme + "\"," +
      "\"icons\":[{\"src\":\"/icon-192.png\",\"sizes\":\"192x192\",\"type\":\"image/png\",\"purpose\":\"any maskable\"}," +
      "{\"src\":\"/icon-512.png\",\"sizes\":\"512x512\",\"type\":\"image/png\",\"purpose\":\"any maskable\"}]}";
  httpd_resp_set_type(req, "application/manifest+json");
  httpd_resp_set_hdr(req, "Cache-Control", "max-age=3600");
  return httpd_resp_send(req, m.c_str(), m.size());
}

static esp_err_t send_icon(httpd_req_t *req, const uint8_t *scale, size_t scale_len,
                           const uint8_t *screen, size_t screen_len) {
  const bool is_scale = device_kind == "scale";
  httpd_resp_set_type(req, "image/png");
  httpd_resp_set_hdr(req, "Cache-Control", "max-age=86400");
  return httpd_resp_send(req, (const char *) (is_scale ? scale : screen), is_scale ? scale_len : screen_len);
}
static esp_err_t h_icon180(httpd_req_t *req) {
  return send_icon(req, ICON_SCALE_180, ICON_SCALE_180_LEN, ICON_SCREEN_180, ICON_SCREEN_180_LEN);
}
static esp_err_t h_icon192(httpd_req_t *req) {
  return send_icon(req, ICON_SCALE_192, ICON_SCALE_192_LEN, ICON_SCREEN_192, ICON_SCREEN_192_LEN);
}
static esp_err_t h_icon512(httpd_req_t *req) {
  return send_icon(req, ICON_SCALE_512, ICON_SCALE_512_LEN, ICON_SCREEN_512, ICON_SCREEN_512_LEN);
}

static esp_err_t h_status(httpd_req_t *req) {
  std::string s = status_json();
  httpd_resp_set_type(req, "application/json; charset=utf-8");
  return httpd_resp_send(req, s.c_str(), s.size());
}

static esp_err_t h_peer_identity(httpd_req_t *req) {
  const std::string s = identity_json();
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_send(req, s.c_str(), s.size());
}

static esp_err_t send_scan_error(httpd_req_t *req, const char *where, esp_err_t e) {
  evlog('e', "%s failed: %s", where, esp_err_to_name(e));
  char err[160];
  snprintf(err, sizeof(err), "{\"error\":\"%s: %s\"}", where, esp_err_to_name(e));
  httpd_resp_set_type(req, "application/json; charset=utf-8");
  return httpd_resp_sendstr(req, err);
}

// ---- поиск сетей ----
// Результаты забираем в своём обработчике события SCAN_DONE: он срабатывает
// сразу после обработчика ESPHome, раньше его основного цикла, который
// тоже вычитывает список из драйвера (после чтения список пустеет).
inline SemaphoreHandle_t scan_sem = nullptr;
inline wifi_ap_record_t scan_recs[20];
inline uint16_t scan_cnt = 0;
inline uint32_t scan_evt_status = 0;
inline uint16_t scan_evt_num = 0;
inline volatile bool scan_wanted = false;
inline bool scan_hooked = false;

static void on_scan_done(void *, esp_event_base_t, int32_t, void *data) {
  if (!scan_wanted) return;  // чужие сканы (ESPHome) не трогаем
  auto *ev = (wifi_event_sta_scan_done_t *) data;
  scan_evt_status = ev->status;
  scan_evt_num = ev->number;
  scan_cnt = 20;
  if (esp_wifi_scan_get_ap_records(&scan_cnt, scan_recs) != ESP_OK) scan_cnt = 0;
  scan_wanted = false;
  if (scan_sem) xSemaphoreGive(scan_sem);
}

static esp_err_t h_scan(httpd_req_t *req) {
  if (!scan_hooked) {
    scan_sem = xSemaphoreCreateBinary();
    esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_SCAN_DONE, &on_scan_done, nullptr);
    scan_hooked = true;
  }

  // Сканирование не работает, когда клиентский интерфейс выключен (только точка доступа).
  // Включаем клиент без подключения: сохранённой сети нет, переподключений не будет.
  wifi_mode_t mode = WIFI_MODE_NULL;
  if (esp_wifi_get_mode(&mode) == ESP_OK && mode == WIFI_MODE_AP) {
    esp_err_t me = esp_wifi_set_mode(WIFI_MODE_APSTA);
    ESP_LOGI(TAG_UI, "mode AP -> APSTA for scan: %s", esp_err_to_name(me));
  }

  xSemaphoreTake(scan_sem, 0);  // сбросить старый сигнал
  scan_cnt = 0;
  scan_wanted = true;

  wifi_scan_config_t cfg = {};
  cfg.show_hidden = false;
  cfg.scan_type = WIFI_SCAN_TYPE_ACTIVE;
  cfg.scan_time.active.min = 100;
  cfg.scan_time.active.max = 300;
  esp_err_t e = esp_wifi_scan_start(&cfg, false);  // не блокирующий
  if (e != ESP_OK) {
    scan_wanted = false;
    return send_scan_error(req, "scan_start", e);
  }
  if (xSemaphoreTake(scan_sem, pdMS_TO_TICKS(10000)) != pdTRUE) {
    scan_wanted = false;
    return send_scan_error(req, "scan_timeout", ESP_ERR_TIMEOUT);
  }
  ESP_LOGI(TAG_UI, "scan event: status=%u number=%u fetched=%u",
           (unsigned) scan_evt_status, (unsigned) scan_evt_num, (unsigned) scan_cnt);

  std::string out = "[";
  bool first = true;
  for (int i = 0; i < scan_cnt; i++) {
    char name[33] = {0};
    memcpy(name, scan_recs[i].ssid, 32);
    if (name[0] == 0) continue;
    char item[256];
    snprintf(item, sizeof(item), "%s{\"ssid\":\"%s\",\"rssi\":%d,\"open\":%s}",
             first ? "" : ",", json_escape(name).c_str(), (int) scan_recs[i].rssi,
             scan_recs[i].authmode == WIFI_AUTH_OPEN ? "true" : "false");
    out += item;
    first = false;
  }
  out += "]";
  httpd_resp_set_type(req, "application/json; charset=utf-8");
  return httpd_resp_send(req, out.c_str(), out.size());
}

// Ответ для действий калибровки
static esp_err_t reply_msg(httpd_req_t *req, const std::string &m) {
  httpd_resp_set_type(req, "application/json; charset=utf-8");
  std::string r = "{\"ok\":true,\"msg\":\"" + json_escape(m.c_str()) + "\"}";
  return httpd_resp_send(req, r.c_str(), r.size());
}

// Ставит команду в очередь; выполнит цикл ESPHome, результат придёт в статус
static esp_err_t queue_cmd(httpd_req_t *req, const char *name, float known, bool supported) {
  if (!supported) return reply_msg(req, "Не поддерживается");
  if (cmd_pending) return reply_msg(req, "Предыдущая команда ещё выполняется");
  cmd_name = name;
  cmd_known = known;
  cmd_pending = true;
  return reply_msg(req, "Команда принята");
}

static esp_err_t h_zero(httpd_req_t *req) {
  return queue_cmd(req, "zero", 0.0f, (bool) on_zero);
}

static esp_err_t h_calib(httpd_req_t *req) {
  std::string b;
  if (recv_body(req, b, 200) < 0) return reply_msg(req, "Ошибка запроса");
  std::string v = form_value(b, "known");
  float known = (float) strtod(v.c_str(), nullptr);
  return queue_cmd(req, "calib", known, (bool) on_calib);
}

static esp_err_t h_curve_reset(httpd_req_t *req) {
  return queue_cmd(req, "curve_reset", 0.0f, (bool) on_curve_reset);
}

static esp_err_t reply_err(httpd_req_t *req, const std::string &m) {
  httpd_resp_set_type(req, "application/json; charset=utf-8");
  std::string r = "{\"ok\":false,\"msg\":\"" + json_escape(m.c_str()) + "\"}";
  return httpd_resp_send(req, r.c_str(), r.size());
}

// Экран пересылает команды калибровки весам. Адрес берём у проверки связи.
// Новые события для страницы: GET /events?after=<последний полученный id>
static esp_err_t h_events(httpd_req_t *req) {
  uint32_t after = 0;
  char q[48];
  if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
    char v[16];
    if (httpd_query_key_value(q, "after", v, sizeof(v)) == ESP_OK) after = (uint32_t) strtoul(v, nullptr, 10);
  }
  std::string out = "{\"ev\":[";
  uint32_t last = 0;
  if (ev_lock != nullptr && xSemaphoreTake(ev_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
    last = ev_last_id;
    uint32_t first = after + 1;
    if (last >= (uint32_t) kEvCount && first < last - kEvCount + 1) first = last - kEvCount + 1;
    bool comma = false;
    for (uint32_t id = first; id <= last; id++) {
      const UiEvent &e = ev_ring[(id - 1) % kEvCount];
      if (e.id != id) continue;
      char head[96];
      snprintf(head, sizeof(head), "%s{\"i\":%lu,\"t\":%lld,\"u\":%lu,\"l\":\"%c\",\"m\":\"", comma ? "," : "",
               (unsigned long) e.id, e.ts, (unsigned long) e.up_s, e.lvl);
      out += head;
      out += json_escape(e.text);
      out += "\"}";
      comma = true;
    }
    xSemaphoreGive(ev_lock);
  }
  out += "],\"last\":" + std::to_string(last) + "}";
  httpd_resp_set_type(req, "application/json; charset=utf-8");
  return httpd_resp_send(req, out.c_str(), out.size());
}

static esp_err_t h_peer_cmd(httpd_req_t *req) {
  std::string b;
  if (recv_body(req, b, 200) < 0) return reply_err(req, "Ошибка запроса");
  const std::string cmd = form_value(b, "cmd");
  std::string path, body;
  if (cmd == "zero") path = "/zero";
  else if (cmd == "curve_reset") path = "/curve_reset";
  else if (cmd == "calib") { path = "/calib"; body = "known=" + form_value(b, "known"); }
  else return reply_err(req, "Неизвестная команда");
  std::string ip;
  if (!peer_ip_now(ip)) return reply_err(req, "Весы не на связи");
  std::string resp;
  if (!prov_link::post_text("http://" + ip + path, body, resp) || resp.empty())
    return reply_err(req, "Весы не ответили");
  httpd_resp_set_type(req, "application/json; charset=utf-8");
  return httpd_resp_send(req, resp.c_str(), resp.size());
}

// ---- Экспорт / импорт настроек ----
// Файл содержит один зашифрованный блок (тот же AES-256-GCM и ключ, что у передачи сети),
// поэтому пароль Wi-Fi не лежит в нём открытым текстом и файл читается только устройствами пары.
// Открытый текст: строки "FS1", "kind=…", "net=<hex ssid>,<hex пароль>", "cal=<числа>".
inline std::string to_hex(const std::string &v) {
  static const char H[] = "0123456789abcdef";
  std::string o;
  for (unsigned char c : v) { o += H[c >> 4]; o += H[c & 15]; }
  return o;
}
inline bool from_hex(const std::string &h, std::string &out) {
  std::vector<uint8_t> v;
  out.clear();
  if (h.empty()) return true;
  if (!prov::hex_to_bytes(h, v)) return false;
  out.assign((const char *) v.data(), v.size());
  return true;
}
inline std::string line_value(const std::string &text, const char *key) {
  const std::string k = std::string("\n") + key + "=";
  size_t pos = text.find(k);
  if (pos == std::string::npos) return "";
  pos += k.size();
  size_t end = text.find('\n', pos);
  return text.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
}

inline std::string settings_plain(bool *has_net = nullptr, bool *has_cal = nullptr) {
  std::string out = "FS1\nkind=" + device_kind + "\n";
  std::string ssid;
  bool net = false, cal = false;
  if (wifi_comp_ptr != nullptr && netrole::saved_sta(ssid) && ssid != setup_ssid) {
    out += "net=" + to_hex(ssid) + "," + to_hex(wifi_comp_ptr->get_sta().get_password().str()) + "\n";
    net = true;
  }
  std::string cal_text;
  if (export_cal) {
    cal_text = export_cal();
  } else if (device_kind == "screen") {
    // Калибровка хранится на весах: просим у них такой же файл и берём из него строку cal
    std::string ip, r;
    if (peer_ip_now(ip) && prov_link::get_request("http://" + ip + "/export", r, 2500)) {
      const std::string key = "\"blob\":\"";
      size_t pos = r.find(key);
      size_t end = pos == std::string::npos ? pos : r.find('"', pos + key.size());
      std::string plain;
      if (end != std::string::npos &&
          prov::decrypt_text(prov_key.c_str(), r.substr(pos + key.size(), end - pos - key.size()), plain))
        cal_text = line_value(plain, "cal");
    }
  }
  if (!cal_text.empty()) { out += "cal=" + cal_text + "\n"; cal = true; }
  if (has_net) *has_net = net;
  if (has_cal) *has_cal = cal;
  return out;
}

static esp_err_t h_export(httpd_req_t *req) {
  bool net = false, cal = false;
  const std::string plain = settings_plain(&net, &cal);
  std::string blob;
  httpd_resp_set_type(req, "application/json; charset=utf-8");
  if (!prov::encrypt_text(prov_key.c_str(), plain, blob))
    return httpd_resp_sendstr(req, "{\"ok\":false,\"msg\":\"Не удалось зашифровать настройки\"}");
  std::string r = std::string("{\"ok\":true,\"app\":\"floor-scale\",\"v\":1,\"kind\":\"") + device_kind +
                  "\",\"net\":" + (net ? "true" : "false") + ",\"cal\":" + (cal ? "true" : "false") +
                  ",\"blob\":\"" + blob + "\"}";
  return httpd_resp_send(req, r.c_str(), r.size());
}

static esp_err_t h_import(httpd_req_t *req) {
  std::string b;
  if (recv_body(req, b, 3000) < 0) return reply_err(req, "Ошибка запроса или файл слишком большой");
  const std::string blob = form_value(b, "blob");
  const bool forward = form_value(b, "forward") == "1";
  std::string plain;
  if (!prov::decrypt_text(prov_key.c_str(), blob, plain) || plain.compare(0, 4, "FS1\n") != 0)
    return reply_err(req, "Файл не подходит: другой ключ или повреждён");
  plain = "\n" + plain;

  std::string ssid, pass;
  bool has_net = false;
  const std::string net = line_value(plain, "net");
  if (!net.empty()) {
    const size_t comma = net.find(',');
    std::string h1 = net.substr(0, comma), h2 = comma == std::string::npos ? "" : net.substr(comma + 1);
    if (comma == std::string::npos || !from_hex(h1, ssid) || !from_hex(h2, pass) || ssid.empty() ||
        ssid.size() > 32 || pass.size() > 64)
      return reply_err(req, "В файле неверные данные сети");
    has_net = true;
  }
  const std::string cal = line_value(plain, "cal");
  std::string msg;

  // Сначала сосед: после смены сети это устройство перезагрузится
  bool forwarded = false;
  if (forward) {
    std::string ip, resp;
    forwarded = peer_ip_now(ip) &&
        prov_link::post_text("http://" + ip + "/import", "forward=0&blob=" + blob, resp, "application/x-www-form-urlencoded", 4000) &&
        resp.find("\"ok\":true") != std::string::npos;
  }

  bool cal_applied = false;
  if (!cal.empty() && import_cal) {
    if (cmd_pending) return reply_err(req, "Предыдущая команда ещё выполняется");
    cmd_text = cal;
    cmd_name = "import_cal";
    cmd_pending = true;
    for (int i = 0; i < 30 && cmd_pending; i++) vTaskDelay(pdMS_TO_TICKS(100));
    cal_applied = !cmd_pending;
  }

  bool net_changed = false;
  if (has_net) {
    std::string cur;
    const bool have = netrole::saved_sta(cur);
    const std::string cur_pass = (have && wifi_comp_ptr) ? wifi_comp_ptr->get_sta().get_password().str() : "";
    net_changed = !have || cur != ssid || cur_pass != pass;
  }

  if (cal_applied) msg += "Калибровка применена. ";
  if (!cal.empty() && !import_cal) msg += forwarded ? "Калибровка передана весам. " : "Калибровка не передана: весы не на связи. ";
  if (has_net) msg += net_changed ? "Сеть «" + ssid + "» сохранена, устройство перезагружается. " : "Сеть уже такая же. ";
  if (forward && !forwarded) msg += "Второе устройство не ответило. ";
  if (msg.empty()) msg = "В файле нет настроек для этого устройства";
  while (!msg.empty() && msg.back() == ' ') msg.pop_back();
  set_status(msg);

  std::string r = std::string("{\"ok\":true,\"reboot\":") + (net_changed && on_creds ? "true" : "false") +
                  ",\"msg\":\"" + json_escape(msg.c_str()) + "\"}";
  httpd_resp_set_type(req, "application/json; charset=utf-8");
  httpd_resp_send(req, r.c_str(), r.size());
  if (net_changed && on_creds) on_creds(ssid, pass);
  return ESP_OK;
}

// Проверка сети и передача зашифрованных учётных данных соседу.
static esp_err_t h_connect(httpd_req_t *req) {
  std::string b;
  httpd_resp_set_type(req, "application/json; charset=utf-8");
  if (recv_body(req, b, 300) < 0) {
    return httpd_resp_sendstr(req, "{\"ok\":false,\"msg\":\"Ошибка запроса\"}");
  }
  std::string ssid = form_value(b, "ssid");
  std::string pass = form_value(b, "pass");
  if (ssid.empty()) {
    return httpd_resp_sendstr(req, "{\"ok\":false,\"msg\":\"Выберите сеть\"}");
  }
  // Клиент ли мы setup-сети, определяем по списку сетей ESPHome, а не по
  // конфигурации драйвера: во флеше драйвера может лежать устаревшая запись.
  std::string sta_ssid;
  const bool was_setup_client = netrole::saved_sta(sta_ssid) && setup_ssid.size() > 0 &&
      sta_ssid == setup_ssid;

  bool previous_network_restored = false;
  if (probe::try_connect(ssid, pass, 15000, &previous_network_restored)) {
    if (!previous_network_restored) {
      set_status("Сеть проверена, но связь с соседом не восстановилась");
      return httpd_resp_sendstr(req,
          "{\"ok\":false,\"msg\":\"Сеть подходит, но устройство не вернулось в setup-сеть. Проверьте связь с соседом и повторите\"}");
    }
    std::string blob;
    if (!prov::encrypt(prov_key.c_str(), ssid, pass, blob)) {
      set_status("Ошибка шифрования учётных данных");
      return httpd_resp_sendstr(req,
          "{\"ok\":false,\"msg\":\"Не удалось зашифровать учётные данные\"}");
    }

    const bool sent = was_setup_client
        ? prov_link::send_to_peer_ap(peer_ap_ip, blob)
        : prov_link::send_to_ap_clients(blob);
    if (!sent) {
      set_status("Сеть проверена, сосед не принял настройки");
      return httpd_resp_sendstr(req,
          "{\"ok\":false,\"msg\":\"Сеть работает, но сосед не найден или его окно настройки закрыто. Откройте окно на втором устройстве и повторите\"}");
    }

    set_status("Сосед принял настройки, подключение к сети");
    httpd_resp_sendstr(req,
        "{\"ok\":true,\"msg\":\"Сосед принял настройки. Устройство подключается к сети\"}");
    if (on_creds) on_creds(ssid, pass);
    return ESP_OK;
  }
  set_status("Не удалось подключиться, проверьте пароль");
  return httpd_resp_sendstr(req,
      "{\"ok\":false,\"msg\":\"Не удалось подключиться, проверьте пароль\"}");
}

static esp_err_t h_open(httpd_req_t *req) {
  open_window(300000);
  set_status("Окно настройки открыто на 5 минут");
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_sendstr(req, "{\"ok\":true}");
}

// Сброс соседа: адрес зависит от сети (домашняя / setup-клиент / мастер с клиентами)
inline bool reset_peer_now() {
  if (prov::is_home_network(setup_ssid.c_str())) return prov_link::reset_peer_at(peer_host);
  std::string sta_ssid;
  const bool setup_client = netrole::saved_sta(sta_ssid) && setup_ssid.size() > 0 &&
      sta_ssid == setup_ssid;
  return setup_client ? prov_link::reset_peer_at(peer_ap_ip) : prov_link::reset_ap_clients();
}

// Метка «после сброса»: переживает перезагрузку (NVS), чтобы после загрузки записать итог роли
inline void mark_reset_pending() {
  nvs_handle_t h = 0;
  if (nvs_open("ui_test", NVS_READWRITE, &h) != ESP_OK) return;
  nvs_set_u8(h, "tb_pending", 1);
  nvs_commit(h);
  nvs_close(h);
}
// Метка читается при каждой загрузке и снимается только после записи итога:
// если устройство перезагрузилось ещё раз (тай-брейк), итог будет после последней загрузки.
inline bool reset_pending() {
  nvs_handle_t h = 0;
  if (nvs_open("ui_test", NVS_READWRITE, &h) != ESP_OK) return false;
  uint8_t v = 0;
  const bool had = nvs_get_u8(h, "tb_pending", &v) == ESP_OK && v == 1;
  nvs_close(h);
  return had;
}
inline void clear_reset_pending() {
  nvs_handle_t h = 0;
  if (nvs_open("ui_test", NVS_READWRITE, &h) != ESP_OK) return;
  nvs_erase_key(h, "tb_pending");
  nvs_commit(h);
  nvs_close(h);
}

static esp_err_t h_reset(httpd_req_t *req) {
  const bool peer_reset = reset_peer_now();
  mark_reset_pending();

  const char *msg = peer_reset
      ? "Сброс отправлен обоим устройствам, они перезагружаются"
      : "Сосед не ответил; сбрасывается только это устройство";
  set_status(msg);
  httpd_resp_set_type(req, "application/json; charset=utf-8");
  std::string body = std::string("{\"ok\":true,\"peer_reset\":") +
                     (peer_reset ? "true" : "false") +
                     ",\"msg\":\"" + msg + "\"}";
  httpd_resp_send(req, body.c_str(), body.size());
  vTaskDelay(pdMS_TO_TICKS(250));
  if (on_reset) on_reset();
  return ESP_OK;
}

// ---- Тесты (страница «Тесты») ----
// Каждый тест выполняется в отдельной задаче: сканирование и HTTP не должны
// блокировать цикл ESPHome. Начало и конец пишутся в журнал со временем устройства.
inline volatile bool test_busy = false;

static const char *test_label(const std::string &n) {
  if (n == "test_marker") return "маркер в журнал";
  if (n == "test_role") return "состояние роли";
  if (n == "test_scan") return "скан эфира";
  if (n == "test_peer") return "проверка соседа";
  if (n == "test_peer_data") return "данные весов на экране";
  if (n == "test_export") return "экспорт настроек";
  if (n == "test_tiebreak") return "тай-брейк: сброс обоих";
  return nullptr;
}

static void test_task(void *arg) {
  char *name = static_cast<char *>(arg);
  const char *label = test_label(name);
  const std::string self = name;
  evlog('i', "[%s] ТЕСТ НАЧАТ: %s", clock_stamp().c_str(), label ? label : self.c_str());
  if (self == "test_marker") {
    evlog('i', "[%s] МАРКЕР: отметка для поиска момента в журнале", clock_stamp().c_str());
    set_status("Маркер записан в журнал");
  } else if (self == "test_role") {
    std::string saved;
    const bool has_saved = netrole::saved_sta(saved);
    evlog('i', "[%s] роль: %s", clock_stamp().c_str(), local_role().c_str());
    evlog('i', "[%s] своя точка активна: %s; сохранённая сеть: %s; связь с соседом: %s",
             clock_stamp().c_str(), netrole::own_ap_active() ? "да" : "нет",
             has_saved ? saved.c_str() : "нет", peer_link_state ? "да" : "нет");
    set_status("Состояние роли записано в журнал");
  } else if (self == "test_scan") {
    uint8_t bssid[6] = {0};
    const bool found = netrole::find_bssid(setup_ssid, bssid);
    evlog('i', "[%s] скан завершён: точка настройки %s", clock_stamp().c_str(),
             found ? "найдена" : "не найдена");
    set_status(found ? "Скан: точка настройки найдена" : "Скан: точка настройки не найдена");
  } else if (self == "test_peer") {
    const std::string hosts[2] = {peer_ap_ip, peer_host};
    for (const std::string &host : hosts) {
      if (host.empty()) continue;
      std::string resp;
      const std::string url = "http://" + host + "/peer_identity";
      const uint32_t t0 = now_ms();
      const bool ok = prov_link::get_request(url, resp, 2000);
      evlog('i', "[%s] сосед %s: %s, %u байт, %lu мс", clock_stamp().c_str(), host.c_str(),
               ok ? "ответил" : "не ответил", (unsigned) resp.size(),
               (unsigned long) (now_ms() - t0));
    }
    std::string known_ip;
    evlog('i', "[%s] адрес соседа из проверки связи: %s; кэш домашней сети: %s; пропусков подряд: %d",
             clock_stamp().c_str(), peer_ip_now(known_ip) ? known_ip.c_str() : "нет",
             home_peer_ip.empty() ? "пуст" : home_peer_ip.c_str(), peer_misses);
    set_status("Проверка соседа записана в журнал");
  } else if (self == "test_peer_data") {
    if (device_kind != "screen") {
      evlog('i', "[%s] тест нужен только на экране", clock_stamp().c_str());
      set_status("Тест выполняется на экране");
    } else {
      uint32_t age = 0;
      size_t size = 0;
      bool have = false;
      if (peer_status_lock != nullptr && xSemaphoreTake(peer_status_lock, pdMS_TO_TICKS(200)) == pdTRUE) {
        have = !peer_data.empty();
        size = peer_data.size();
        age = now_ms() - peer_data_at;
        xSemaphoreGive(peer_status_lock);
      }
      evlog('i', "[%s] данные весов: %s, %u байт, возраст %lu мс", clock_stamp().c_str(),
               have ? "есть" : "нет", (unsigned) size, (unsigned long) age);
      set_status(have && age < 10000 ? "Данные весов свежие" : "Данных весов нет или они устарели");
    }
  } else if (self == "test_export") {
    bool net = false, cal = false;
    const std::string plain = settings_plain(&net, &cal);
    std::string blob, back;
    const bool enc = prov::encrypt_text(prov_key.c_str(), plain, blob);
    const bool dec = enc && prov::decrypt_text(prov_key.c_str(), blob, back) && back == plain;
    evlog('i', "[%s] экспорт: сеть=%s, калибровка=%s, блок %u байт, шифрование и расшифровка %s",
             clock_stamp().c_str(), net ? "да" : "нет", cal ? "да" : "нет", (unsigned) blob.size(),
             dec ? "успешны" : "НЕ ПРОШЛИ");
    set_status(dec ? "Экспорт: шифрование проверено" : "Экспорт: ошибка шифрования");
  } else if (self == "test_tiebreak") {
    evlog('i', "[%s] сброс обоих: команда соседу и перезагрузка этого устройства", clock_stamp().c_str());
    const bool peer_ok = reset_peer_now();
    evlog('i', "[%s] сосед %s", clock_stamp().c_str(),
             peer_ok ? "принял сброс" : "не ответил: сбрасывается только это устройство");
    mark_reset_pending();
    set_status("Сброс: перезагрузка для теста тай-брейка");
    vTaskDelay(pdMS_TO_TICKS(1000));
    if (on_reset) on_reset();  // перезагрузка; итог роли запишется после загрузки
  }
  evlog('i', "[%s] ТЕСТ ЗАВЕРШЁН: %s", clock_stamp().c_str(), label ? label : self.c_str());
  free(name);
  test_busy = false;
  vTaskDelete(nullptr);
}

// После загрузки, если перед ней был сброс: через 70 с (роль и 45-секундная проверка) пишем итог
inline void tiebreak_check_task(void *) {
  vTaskDelay(pdMS_TO_TICKS(70000));
  std::string saved;
  const bool has_saved = netrole::saved_sta(saved);
  evlog('i', "[%s] ТЕСТ ТАЙ-БРЕЙК, итог после сброса: роль=%s; своя точка активна=%s; сохранённая сеть=%s; связь с соседом=%s",
           clock_stamp().c_str(), local_role().c_str(), netrole::own_ap_active() ? "да" : "нет",
           has_saved ? saved.c_str() : "нет", peer_link_state ? "да" : "нет");
  clear_reset_pending();
  vTaskDelete(nullptr);
}

inline void run_test(const std::string &name) {
  if (!test_label(name)) return;
  if (test_busy) {
    set_status("Тест уже выполняется");
    return;
  }
  test_busy = true;
  char *copy = strdup(name.c_str());
  if (copy == nullptr || xTaskCreate(test_task, "ui_test", 8192, copy, 2, nullptr) != pdPASS) {
    free(copy);
    test_busy = false;
    set_status("Не удалось запустить тест");
  }
}

static esp_err_t h_test(httpd_req_t *req) {
  std::string b;
  if (recv_body(req, b, 100) < 0) return reply_msg(req, "Ошибка запроса");
  const std::string name = "test_" + form_value(b, "name");
  if (!test_label(name)) return reply_msg(req, "Неизвестный тест");
  return queue_cmd(req, name.c_str(), 0.0f, true);
}

// Перезагрузка по кнопке. Ответ уходит до перезагрузки; запись в журнал устройства.
static esp_err_t h_reboot(httpd_req_t *req) {
  httpd_resp_set_type(req, "application/json; charset=utf-8");
  if (!on_reboot) return reply_msg(req, "Перезагрузка не поддерживается");
  evlog('w', "reboot requested from web page");
  set_status("Перезагрузка по команде со страницы");
  httpd_resp_sendstr(req, "{\"ok\":true,\"msg\":\"Перезагрузка\"}");
  vTaskDelay(pdMS_TO_TICKS(250));
  on_reboot();
  return ESP_OK;
}

// Установка времени со страницы (поле ts: Unix-секунды, UTC). Принимаем только разумные значения.
static esp_err_t h_time(httpd_req_t *req) {
  std::string b;
  if (recv_body(req, b, 100) < 0) return reply_msg(req, "Ошибка запроса");
  const long long ts = strtoll(form_value(b, "ts").c_str(), nullptr, 10);
  if (ts < 1704067200LL || ts > 4102444800LL) return reply_msg(req, "Неверное время");
  struct timeval tv = {};
  tv.tv_sec = (time_t) ts;
  tv.tv_usec = 0;
  if (settimeofday(&tv, nullptr) != 0) return reply_msg(req, "Не удалось установить время");
  const std::string tz = form_value(b, "tz");
  if (!tz.empty()) clock_tz_min = (int) strtol(tz.c_str(), nullptr, 10);
  clock_set_at = ts;
  evlog('i', "[%s] время задано со страницы", clock_stamp().c_str());
  return reply_msg(req, "Время установлено");
}

// Команда от соседа. Отвечает до сброса и не пересылает команду обратно.
static esp_err_t h_peer_reset(httpd_req_t *req) {
  mark_reset_pending();
  set_status("Сосед запросил сброс сети");
  httpd_resp_set_type(req, "application/json; charset=utf-8");
  httpd_resp_sendstr(req, "{\"ok\":true,\"msg\":\"Сброс принят\"}");
  vTaskDelay(pdMS_TO_TICKS(500));
  if (on_reset) on_reset();
  return ESP_OK;
}

// Приём зашифрованного блока кредов (hex в теле запроса)
static esp_err_t h_prov(httpd_req_t *req) {
  httpd_resp_set_type(req, "application/json; charset=utf-8");
  if (!window_open()) {
    set_status("Окно настройки закрыто. Откройте его кнопкой на странице");
    return httpd_resp_sendstr(req, "{\"ok\":false,\"msg\":\"Окно настройки закрыто\"}");
  }
  std::string blob;
  if (recv_body(req, blob, 300) < 0) {
    return httpd_resp_sendstr(req, "{\"ok\":false,\"msg\":\"Ошибка запроса\"}");
  }
  std::string ssid, pass;
  if (!prov::decrypt(prov_key.c_str(), blob, ssid, pass)) {
    set_status("Ошибка: неверный ключ или данные");
    return httpd_resp_sendstr(req, "{\"ok\":false,\"msg\":\"Неверный ключ или данные\"}");
  }
  set_status("Креды приняты, перезагрузка");
  close_window();
  httpd_resp_sendstr(req, "{\"ok\":true,\"msg\":\"Креды приняты\"}");
  if (on_creds) on_creds(ssid, pass);
  return ESP_OK;
}

// Регистрация маршрута с проверкой результата: при переполнении таблицы
// обработчиков (ESP_ERR_HTTPD_HANDLERS_FULL) маршрут иначе пропадает молча,
// и клиент получает HTTP 404.
inline void reg_uri(httpd_handle_t srv, const httpd_uri_t &u) {
  const esp_err_t err = httpd_register_uri_handler(srv, &u);
  if (err != ESP_OK) {
    evlog('e', "register %s failed: %s", u.uri, esp_err_to_name(err));
  }
}

inline void start(int port) {
  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  cfg.server_port = port;
  // По умолчанию 8 обработчиков; у нас 12 маршрутов, лишние тихо отбрасывались (404)
  cfg.max_uri_handlers = 28;
  // web_server ESPHome больше не используется, но порт управления оставляем отдельным
  cfg.ctrl_port = 32769;
  // Стек по умолчанию (4 КБ) переполняется в обработчике сканирования
  cfg.stack_size = 12288;
  httpd_handle_t srv = nullptr;
  if (httpd_start(&srv, &cfg) != ESP_OK) {
    evlog('e', "httpd start failed on port %d", port);
    return;
  }

  httpd_uri_t u = {};
  u.uri = "/"; u.method = HTTP_GET; u.handler = h_root;
  reg_uri(srv, u);

  u = {};
  u.uri = "/status"; u.method = HTTP_GET; u.handler = h_status;
  reg_uri(srv, u);

  u = {};
  u.uri = "/peer_identity"; u.method = HTTP_GET; u.handler = h_peer_identity;
  reg_uri(srv, u);

  if (peer_status_lock == nullptr) peer_status_lock = xSemaphoreCreateMutex();
  if (peer_status_lock != nullptr) {
    xTaskCreate(peer_monitor_task, "peer_monitor", 8192, nullptr, 1, nullptr);
    if (device_kind == "screen") xTaskCreate(peer_data_task, "peer_data", 6144, nullptr, 1, nullptr);
  } else {
    evlog('e', "cannot create peer status mutex");
  }

  u = {};
  u.uri = "/scan"; u.method = HTTP_GET; u.handler = h_scan;
  reg_uri(srv, u);

  u = {};
  u.uri = "/connect"; u.method = HTTP_POST; u.handler = h_connect;
  reg_uri(srv, u);

  u = {};
  u.uri = "/open"; u.method = HTTP_POST; u.handler = h_open;
  reg_uri(srv, u);

  u = {};
  u.uri = "/reset"; u.method = HTTP_POST; u.handler = h_reset;
  reg_uri(srv, u);

  u = {};
  u.uri = "/peer_reset"; u.method = HTTP_POST; u.handler = h_peer_reset;
  reg_uri(srv, u);

  u = {};
  u.uri = "/prov"; u.method = HTTP_POST; u.handler = h_prov;
  reg_uri(srv, u);

  u = {};
  u.uri = "/zero"; u.method = HTTP_POST; u.handler = h_zero;
  reg_uri(srv, u);

  u = {};
  u.uri = "/calib"; u.method = HTTP_POST; u.handler = h_calib;
  reg_uri(srv, u);

  u = {};
  u.uri = "/curve_reset"; u.method = HTTP_POST; u.handler = h_curve_reset;
  reg_uri(srv, u);
  u = {};
  u.uri = "/reboot"; u.method = HTTP_POST; u.handler = h_reboot;
  reg_uri(srv, u);
  u = {};
  u.uri = "/time"; u.method = HTTP_POST; u.handler = h_time;
  reg_uri(srv, u);
  u = {};
  u.uri = "/test"; u.method = HTTP_POST; u.handler = h_test;
  reg_uri(srv, u);

  u = {};
  u.uri = "/events"; u.method = HTTP_GET; u.handler = h_events;
  reg_uri(srv, u);
  u = {};
  u.uri = "/peer_cmd"; u.method = HTTP_POST; u.handler = h_peer_cmd;
  reg_uri(srv, u);
  u = {};
  u.uri = "/export"; u.method = HTTP_GET; u.handler = h_export;
  reg_uri(srv, u);
  u = {};
  u.uri = "/import"; u.method = HTTP_POST; u.handler = h_import;
  reg_uri(srv, u);
  u = {};
  u.uri = "/manifest.webmanifest"; u.method = HTTP_GET; u.handler = h_manifest;
  reg_uri(srv, u);
  u = {};
  u.uri = "/icon-180.png"; u.method = HTTP_GET; u.handler = h_icon180;
  reg_uri(srv, u);
  u = {};
  u.uri = "/icon-192.png"; u.method = HTTP_GET; u.handler = h_icon192;
  reg_uri(srv, u);
  u = {};
  u.uri = "/icon-512.png"; u.method = HTTP_GET; u.handler = h_icon512;
  reg_uri(srv, u);

  if (reset_pending()) {
    xTaskCreate(tiebreak_check_task, "tb_check", 8192, nullptr, 1, nullptr);
  }
  ESP_LOGI(TAG_UI, "UI started on port %d", port);
}

}  // namespace ui
